/*
**      Copyright (c) 2010-2014, United States government as represented by the
**      administrator of the National Aeronautics Space Administration.
**      All rights reserved. This software was created at NASAs Goddard
**      Space Flight Center pursuant to government contracts.
**
**      This is governed by the NASA Open Source Agreement and may be used,
**      distributed and modified only pursuant to the terms of that agreement.
*/

/*
 * Filename: eefs_nand_fileapi.c
 *
 * Purpose: EEFS_Lib* API of the NAND backend: mount, file operations, A/B, descriptors, directory listing and stat.
 *
 * Design Notes:
 *   Format, mount, commit, abort and RepairDirectory call the directory algorithms of eefs_nand_journal.c.  The file
 *   operations (docs/nand-implementation.md section 6) live here: name parsing, the block allocator with its reserve
 *   classes and round-robin cursor (6.9), whole-file writes with copy-on-write (6.2, 6.3), Remove (6.5), the
 *   first-block rewrite of Rename and SetFileAttributes (6.6), Open/Read/LSeek/Close (6.4, 6.7), Stat, ReadDir,
 *   VerifyFile, GetFileInfo, GetVolumeInfo and ChkDsk, and the A/B operations of section 7 (the setters, CopySlot and
 *   the compatible fallback of Open).
 *
 *   Every change goes into Working and becomes durable at the next EEFS_LibCommit.  A slot's blocks are never erased
 *   or programmed while the slot is live: a rewrite writes a new first block plus fresh blocks for the changed data
 *   positions and keeps the unchanged ones (same length, same CRC and a byte compare against the old block).  New
 *   blocks come only from blocks that are FREE in both Working and Base, so nothing the committed directory
 *   references is ever touched before the commit.  Data blocks are programmed first (positions 1..n-1), the first
 *   block last; within a block pages go in ascending order, so page 0 of the first block precedes its own data pages.
 *
 *   All writers (WriteFile, Rename, SetFileAttributes and CopySlot) go through one write job
 *   (EEFS_NandRunWriteJob) whose page source is either the caller's RAM image or the blocks of an existing copy.  A
 *   failed erase, program or read-back verify of a destination block marks the block bad, takes a replacement with
 *   EEFS_NAND_ALLOC_REPLACE and restarts the job, at most EEFS_NAND_WRITE_RETRIES times.
 *
 *   The write job keeps its block lists in static scratch arrays sized for the largest file the build supports
 *   (EEFS_NAND_MAX_FILE_BLOCKS), instead of on the stack.  Like the global descriptor table, they are shared by all
 *   volumes and protected by EEFS_LIB_LOCK, which must therefore be a single library-wide lock (as it is for the
 *   EEPROM backend).
 *
 *   A/B: names accept "@A"/"@B", which address one copy and never fall back.  Without a suffix an A/B entry resolves
 *   to its active slot; Open alone falls back to the inactive copy when the active one fails validation and the entry
 *   is FallbackCompatible (the same rule as the bootloader, FMT 7.3), and the failed validation sets ActiveFailed.
 *   The setters change Working only (a commit makes them durable); switching the active slot, rewriting the active
 *   copy or copying into it clears ActiveFailed.  CopySlot copies every block of one slot into newly allocated blocks
 *   of the other, so A and B never share a block; the replaced copy is freed in the batch and erased after the commit.
 *
 */

/*
 * Includes
 */

#include "common_types.h"
#include "eefs_fileapi.h"
#include "eefs_macros.h"
#include "eefs_nand.h"
#include "eefs_nand_internal.h"
#include <string.h>

/*
 * Macro Definitions
 */

/* Largest Blocks[] array of this build (EEFS_NandMaxBlocksPerFile of the largest page) */
#define EEFS_NAND_MAX_FILE_BLOCKS   ((EEFS_NAND_MAX_PAGE_SIZE - EEFS_NAND_P0_FIXED_SIZE) / EEFS_NAND_P0_BLOCK_ENTRY_SIZE)
#define EEFS_NAND_KEPT_BYTES        ((EEFS_NAND_MAX_FILE_BLOCKS + 7) / 8)
#define EEFS_NAND_ANY_SLOT          0xFF
#define EEFS_NAND_NO_K              0xFFFFFFFFUL

/*
 * Type Definitions
 */

/* One write of a copy: a new metadata page plus the data positions that are not kept */
typedef struct
{
    EEFS_NandPage0_t       *P0;         /* the new metadata page (WriteSeq is assigned per attempt) */
    uint16                 *Blocks;     /* destination Blocks[]; a grown bad block is replaced in place */
    uint32                 *Crcs;       /* CRC of each position */
    const uint8            *Kept;       /* bit k: position k is kept (not written); position 0 never is */
    const uint8            *Data;       /* RAM source (FromRam) */
    const uint16           *Src;        /* NAND source: the blocks of the copy being duplicated (!FromRam) */
    boolean                 FromRam;
    uint8                   Owner;      /* map value of the destination slot */
} EEFS_NandWriteJob_t;

/*
 * Local Data
 */

/* Note: the file descriptors are shared across all file systems */
uint32                          EEFS_FileDescriptorsInUse;
uint32                          EEFS_FileDescriptorsHighWaterMark;
EEFS_FileDescriptor_t           EEFS_FileDescriptorTable[EEFS_MAX_OPEN_FILES];

/* Note: there is only one directory descriptor, so only one process can use it at a time */
EEFS_DirectoryDescriptor_t      EEFS_DirectoryDescriptor;
EEFS_DirectoryEntry_t           EEFS_DirectoryEntry;

/* Write job scratch (under EEFS_LIB_LOCK) */
static uint16                   EEFS_NandJobBlocks[EEFS_NAND_MAX_FILE_BLOCKS];
static uint32                   EEFS_NandJobCrcs[EEFS_NAND_MAX_FILE_BLOCKS];
static uint16                   EEFS_NandJobSrc[EEFS_NAND_MAX_FILE_BLOCKS];
static uint8                    EEFS_NandJobKept[EEFS_NAND_KEPT_BYTES];
static EEFS_NandPage0_t         EEFS_NandJobP0;
static EEFS_NandPage0_t         EEFS_NandJobOldP0;

/*
 * Local Function Prototypes
 */

static uint8                    EEFS_LibIsValidFileDescriptor(int32 FileDescriptor);
static int32                    EEFS_NandGetFileDescriptor(void);
static void                     EEFS_NandFreeFileDescriptor(int32 FileDescriptor);
static boolean                  EEFS_NandIsOpen(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 Slot);
static boolean                  EEFS_NandIsMounted(EEFS_InodeTable_t *Vol);
static boolean                  EEFS_NandBlockBytesEqual(EEFS_InodeTable_t *Vol, uint16 OldBlock, uint32 K, const uint8 *Slice,
                                                         uint32 Len);
static int32                    EEFS_NandResolve(EEFS_InodeTable_t *Vol, char *Name, uint32 *Entry, uint8 *Slot,
                                                 uint8 *SlotSpec);
static int32                    EEFS_NandValidateSlotP0(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 Slot, boolean Full,
                                                        EEFS_NandPage0_t *P);
static void                     EEFS_NandSlotFailed(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 Slot);
static int32                    EEFS_NandReadSlotPage0(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 Slot,
                                                       EEFS_NandPage0_t *P, const uint8 **Raw);
static int32                    EEFS_NandWriteAttempt(EEFS_InodeTable_t *Vol, EEFS_NandWriteJob_t *Job, uint32 *BadK);
static int32                    EEFS_NandRunWriteJob(EEFS_InodeTable_t *Vol, EEFS_NandWriteJob_t *Job);
static void                     EEFS_NandDiscardJob(EEFS_InodeTable_t *Vol, EEFS_NandWriteJob_t *Job);
static int32                    EEFS_NandWriteFileLocked(EEFS_InodeTable_t *Vol, char *Name, const EEFS_NandFileInfo_t *Meta,
                                                         const uint8 *Data, uint32 Size);
static int32                    EEFS_NandRewriteFirstBlocks(EEFS_InodeTable_t *Vol, uint32 Entry, const char *NewPath,
                                                            boolean SetAttr, uint32 Attributes);
static int32                    EEFS_NandAfterChange(EEFS_InodeTable_t *Vol);
static int32                    EEFS_NandCheckWritable(EEFS_InodeTable_t *Vol);
static int32                    EEFS_NandFindABEntry(EEFS_InodeTable_t *Vol, char *Name, int32 *Entry);
static int32                    EEFS_NandCopySlotLocked(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 From, uint8 To);

#define EEFS_NAND_KEPT_TEST(k)  ((EEFS_NandJobKept[(k) / 8] & (uint8)(1u << ((k) % 8))) != 0)
#define EEFS_NAND_KEPT_SET(k)   (EEFS_NandJobKept[(k) / 8] |= (uint8)(1u << ((k) % 8)))

/*
 * Function Definitions
 */

/* Initialize global data shared by all file systems */
void EEFS_LibInit(void)
{
    EEFS_FileDescriptorsInUse = 0;
    EEFS_FileDescriptorsHighWaterMark = 0;
    memset(EEFS_FileDescriptorTable, 0, sizeof(EEFS_FileDescriptorTable));
    memset(&EEFS_DirectoryDescriptor, 0, sizeof(EEFS_DirectoryDescriptor_t));
    memset(&EEFS_DirectoryEntry, 0, sizeof(EEFS_DirectoryEntry_t));

} /* End of EEFS_LibInit() */

/* Mounts the volume behind BaseAddress (an EEFS_NandDevice_t *), section 5.2.  Reads only. */
int32 EEFS_LibInitFS(EEFS_InodeTable_t *InodeTable, cpuaddr BaseAddress)
{
    int32   ReturnCode;

    if ((InodeTable == NULL) || (BaseAddress == 0)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if (InodeTable->Mounted != 0) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else {
        memset(InodeTable, 0, sizeof(EEFS_InodeTable_t));
        InodeTable->Dev = (EEFS_NandDevice_t *)BaseAddress;
        if ((ReturnCode = EEFS_NandCheckGeometry(InodeTable)) == EEFS_SUCCESS) {
            ReturnCode = EEFS_NandMount(InodeTable);
        }
        if (ReturnCode != EEFS_SUCCESS) {
            memset(InodeTable, 0, sizeof(EEFS_InodeTable_t));
        }
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibInitFS() */

/* Unmounts the volume.  The batch must have been committed or aborted. */
int32 EEFS_LibFreeFS(EEFS_InodeTable_t *InodeTable)
{
    int32   ReturnCode;

    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if (InodeTable->Mounted == 0) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else if ((EEFS_LibHasOpenFiles(InodeTable) == TRUE) || (EEFS_LibHasOpenDir(InodeTable) == TRUE) ||
             (InodeTable->ActiveReport != NULL)) {
        ReturnCode = EEFS_DEVICE_IS_BUSY;
    }
    else if (EEFS_NandHasUncommitted(InodeTable, FALSE) == TRUE) {
        ReturnCode = EEFS_TRANSACTION_ERROR;
    }
    else {
        memset(InodeTable, 0, sizeof(EEFS_InodeTable_t));
        ReturnCode = EEFS_SUCCESS;
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibFreeFS() */

/* Opens a file for reading (sections 6.4, 7.3).  Only Flags == O_RDONLY is supported. */
int32 EEFS_LibOpen(EEFS_InodeTable_t *InodeTable, char *Filename, uint32 Flags, uint32 Attributes)
{
    EEFS_FileDescriptor_t  *Fd;
    const EEFS_NandEntry_t *E;
    EEFS_NandPage0_t        P;
    uint32                  Entry;
    uint8                   Slot;
    uint8                   Spec;
    uint8                   Fallback = 0;
    int32                   FileDescriptor;
    int32                   ReturnCode;

    (void)Attributes;

    if ((InodeTable == NULL) || (Filename == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    if (Flags != (uint32)O_RDONLY) {
        return(EEFS_UNSUPPORTED_OPTION);
    }

    EEFS_LIB_LOCK;
    if (EEFS_NandIsMounted(InodeTable) == FALSE) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else if ((ReturnCode = EEFS_NandResolve(InodeTable, Filename, &Entry, &Slot, &Spec)) == EEFS_SUCCESS) {
        ReturnCode = EEFS_NandValidateSlotP0(InodeTable, Entry, Slot, (boolean)(EEFS_NAND_VERIFY_ON_OPEN != 0), &P);
        E = &InodeTable->Working.Entry[Entry];
        if ((ReturnCode != EEFS_SUCCESS) && (Spec == EEFS_NAND_SLOT_NONE) && (E->IsAB != 0) &&
            (E->FallbackCompatible != 0) && (E->Slot[1 - Slot].InUse != 0)) {
            /* compatible fallback (section 7.3): the failed validation has set ActiveFailed; the inactive copy is
             * opened if it validates, else its error is returned */
            Slot = (uint8)(1 - Slot);
            Fallback = 1;
            ReturnCode = EEFS_NandValidateSlotP0(InodeTable, Entry, Slot, (boolean)(EEFS_NAND_VERIFY_ON_OPEN != 0), &P);
        }
        if (ReturnCode == EEFS_SUCCESS) {
            if ((FileDescriptor = EEFS_NandGetFileDescriptor()) >= 0) {
                Fd = &EEFS_FileDescriptorTable[FileDescriptor];
                Fd->Mode = EEFS_FREAD;
                Fd->InodeTable = InodeTable;
                Fd->InodeIndex = Entry;
                Fd->Slot = Slot;
                Fd->Fallback = Fallback;
                Fd->FirstBlock = InodeTable->Working.Entry[Entry].Slot[Slot].FirstBlock;
                Fd->FileSize = P.FileSize;
                Fd->ByteOffset = 0;
                Fd->CurK = EEFS_NAND_NO_K;
                Fd->CurBlock = 0;
                ReturnCode = FileDescriptor;
            }
            else {
                ReturnCode = EEFS_NO_FREE_FILE_DESCRIPTOR;
            }
        }
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibOpen() */

/* Not supported by the NAND backend: files are written whole with EEFS_LibWriteFile */
int32 EEFS_LibCreat(EEFS_InodeTable_t *InodeTable, char *Filename, uint32 Attributes)
{
    (void)Attributes;

    if ((InodeTable == NULL) || (Filename == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibCreat() */

/* Closes a file */
int32 EEFS_LibClose(int32 FileDescriptor)
{
    int32   ReturnCode;

    EEFS_LIB_LOCK;
    if (EEFS_LibIsValidFileDescriptor(FileDescriptor) == FALSE) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else {
        EEFS_NandFreeFileDescriptor(FileDescriptor);
        ReturnCode = EEFS_SUCCESS;
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibClose() */

/* Reads from a file (section 6.7).  Returns the bytes read, 0 at the end of the file. */
int32 EEFS_LibRead(int32 FileDescriptor, void *Buffer, uint32 Length)
{
    EEFS_FileDescriptor_t  *Fd;
    EEFS_InodeTable_t      *Vol;
    EEFS_NandPage0_t        P;
    const uint8            *Raw;
    const uint8            *Page;
    uint8                  *Dst = (uint8 *)Buffer;
    uint32                  Offset;
    uint32                  Remaining;
    uint32                  Done = 0;
    uint32                  D0;
    uint32                  K;
    uint32                  PageNo;
    uint32                  InPage;
    uint32                  Rel;
    uint32                  n;
    uint32                  Crc;
    int32                   Status;
    int32                   ReturnCode = EEFS_SUCCESS;

    EEFS_LIB_LOCK;
    if ((EEFS_LibIsValidFileDescriptor(FileDescriptor) == FALSE) || (Buffer == NULL)) {
        EEFS_LIB_UNLOCK;
        return(EEFS_INVALID_ARGUMENT);
    }
    Fd = &EEFS_FileDescriptorTable[FileDescriptor];
    Vol = Fd->InodeTable;
    if (EEFS_NandIsMounted(Vol) == FALSE) {
        EEFS_LIB_UNLOCK;
        return(EEFS_INVALID_ARGUMENT);
    }

    Offset = Fd->ByteOffset;
    Remaining = (Fd->ByteOffset < Fd->FileSize) ? (Fd->FileSize - Fd->ByteOffset) : 0;
    if (Length < Remaining) Remaining = Length;
    if (Remaining > 0x7FFFFFFFUL) Remaining = 0x7FFFFFFFUL;    /* the count is returned as an int32 */
    D0 = Vol->BlockBytes - Vol->PageSize;

    while (Remaining > 0) {
        if (Offset < D0) {
            K = 0;
            PageNo = 1 + (Offset / Vol->PageSize);
            InPage = Offset % Vol->PageSize;
        }
        else {
            Rel = Offset - D0;
            K = 1 + (Rel / Vol->BlockBytes);
            PageNo = (Rel % Vol->BlockBytes) / Vol->PageSize;
            InPage = Rel % Vol->PageSize;
        }
        if (K != Fd->CurK) {
            Status = EEFS_NandReadPage0(Vol, Fd->FirstBlock, &P, &Raw);
            if (Status != EEFS_SUCCESS) {
                ReturnCode = Status;
                break;
            }
            if (K >= P.BlockCount) {
                ReturnCode = EEFS_CORRUPT_FILE;
                break;
            }
            EEFS_NandPage0Block(Raw, K, &Fd->CurBlock, &Crc);
            Fd->CurK = K;
        }
        if ((Page = EEFS_NandReadCached(Vol, Fd->CurBlock, PageNo, &Status)) == NULL) {
            ReturnCode = EEFS_IO_ERROR;
            break;
        }
        n = Vol->PageSize - InPage;
        if (n > Remaining) n = Remaining;
        memcpy(&Dst[Done], &Page[InPage], n);
        Done += n;
        Offset += n;
        Remaining -= n;
    }

    if (ReturnCode == EEFS_SUCCESS) {
        Fd->ByteOffset = Offset;
        ReturnCode = (int32)Done;
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibRead() */

/* Not supported by the NAND backend: files are written whole with EEFS_LibWriteFile */
int32 EEFS_LibWrite(int32 FileDescriptor, void *Buffer, uint32 Length)
{
    (void)Length;

    if ((EEFS_LibIsValidFileDescriptor(FileDescriptor) == FALSE) || (Buffer == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibWrite() */

/* Sets the file pointer (as the EEPROM backend: SEEK_SET/CUR/END, clamped to the end of the file) */
int32 EEFS_LibLSeek(int32 FileDescriptor, int32 ByteOffset, uint16 Origin)
{
    EEFS_FileDescriptor_t  *Fd;
    int32                   Base;
    int32                   Target;
    int32                   ReturnCode;

    EEFS_LIB_LOCK;
    if (EEFS_LibIsValidFileDescriptor(FileDescriptor) == FALSE) {
        EEFS_LIB_UNLOCK;
        return(EEFS_INVALID_ARGUMENT);
    }
    Fd = &EEFS_FileDescriptorTable[FileDescriptor];

    if (Origin == SEEK_SET) {
        Base = 0;
    }
    else if (Origin == SEEK_CUR) {
        Base = (int32)Fd->ByteOffset;
    }
    else if (Origin == SEEK_END) {
        Base = (int32)Fd->FileSize;
    }
    else {
        EEFS_LIB_UNLOCK;
        return(EEFS_INVALID_ARGUMENT);
    }

    Target = Base + ByteOffset;
    if (Target < 0) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else {
        if ((uint32)Target > Fd->FileSize) {
            Target = (int32)Fd->FileSize;
        }
        Fd->ByteOffset = (uint32)Target;
        ReturnCode = Target;
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibLSeek() */

/* Removes a file, or one slot of it (section 6.5) */
int32 EEFS_LibRemove(EEFS_InodeTable_t *InodeTable, char *Filename)
{
    char                Path[EEFS_NAND_MAX_PATH_SIZE];
    EEFS_NandEntry_t   *E;
    uint8               Spec;
    uint8               s;
    uint8               Other;
    uint32              InUse;
    int32               i;
    int32               ReturnCode;

    if ((InodeTable == NULL) || (Filename == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if (EEFS_NandIsMounted(InodeTable) == FALSE) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else if ((ReturnCode = EEFS_NandParseName(Filename, Path, &Spec)) != EEFS_SUCCESS) {
        /* invalid name */
    }
    else if (EEFS_LIB_IS_WRITE_PROTECTED != FALSE) {
        ReturnCode = EEFS_READ_ONLY_FILE_SYSTEM;
    }
    else {
        if (InodeTable->PendingCleanupBase > 0) {
            EEFS_NandRunPendingCleanup(InodeTable);
        }
        if ((i = EEFS_NandFindEntry(InodeTable, Path)) < 0) {
            ReturnCode = EEFS_FILE_NOT_FOUND;
        }
        else {
            E = &InodeTable->Working.Entry[i];
            InUse = (uint32)(E->Slot[0].InUse != 0) + (uint32)(E->Slot[1].InUse != 0);
            if ((E->Attributes & EEFS_ATTRIBUTE_READONLY) != 0) {
                ReturnCode = EEFS_PERMISSION_DENIED;
            }
            else if (Spec == EEFS_NAND_SLOT_NONE) {
                if (EEFS_NandIsOpen(InodeTable, (uint32)i, EEFS_NAND_ANY_SLOT) == TRUE) {
                    ReturnCode = EEFS_PERMISSION_DENIED;
                }
                else if (EEFS_NandCleanupHasRoom(InodeTable, InUse) == FALSE) {
                    ReturnCode = EEFS_TRANSACTION_ERROR;
                }
                else {
                    for (s = 0; s < 2; s++) {
                        if (E->Slot[s].InUse != 0) {
                            EEFS_NandFreeSlot(InodeTable, (uint32)i, s);
                        }
                        InodeTable->SlotFlags[i][s] = 0;
                    }
                    memset(E, 0, sizeof(EEFS_NandEntry_t));
                    ReturnCode = EEFS_SUCCESS;
                }
            }
            else {
                s = Spec;
                Other = (uint8)(1 - s);
                if (E->Slot[s].InUse == 0) {
                    ReturnCode = EEFS_FILE_NOT_FOUND;
                }
                else if (EEFS_NandIsOpen(InodeTable, (uint32)i, s) == TRUE) {
                    ReturnCode = EEFS_PERMISSION_DENIED;
                }
                else if ((E->IsAB != 0) && (E->ActiveSlot == s) && (E->Slot[Other].InUse != 0)) {
                    ReturnCode = EEFS_PERMISSION_DENIED;      /* switch the active slot first */
                }
                else if ((E->IsAB == 0) && (E->Slot[Other].InUse != 0)) {
                    ReturnCode = EEFS_PERMISSION_DENIED;
                }
                else if (EEFS_NandCleanupHasRoom(InodeTable, 1) == FALSE) {
                    ReturnCode = EEFS_TRANSACTION_ERROR;
                }
                else {
                    EEFS_NandFreeSlot(InodeTable, (uint32)i, s);
                    InodeTable->SlotFlags[i][s] = 0;
                    if (E->Slot[Other].InUse == 0) {
                        memset(E, 0, sizeof(EEFS_NandEntry_t));
                    }
                    else if (E->ActiveSlot == s) {
                        E->ActiveSlot = Other;
                        E->ActiveFailed = 0;
                    }
                    ReturnCode = EEFS_SUCCESS;
                }
            }
            if (ReturnCode == EEFS_SUCCESS) {
                ReturnCode = EEFS_NandAfterChange(InodeTable);
            }
        }
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibRemove() */

/* Renames a file by rewriting the first block of each slot (section 6.6) */
int32 EEFS_LibRename(EEFS_InodeTable_t *InodeTable, char *OldFilename, char *NewFilename)
{
    char                OldPath[EEFS_NAND_MAX_PATH_SIZE];
    char                NewPath[EEFS_NAND_MAX_PATH_SIZE];
    uint8               OldSpec;
    uint8               NewSpec;
    int32               i;
    int32               ReturnCode;

    if ((InodeTable == NULL) || (OldFilename == NULL) || (NewFilename == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if (EEFS_NandIsMounted(InodeTable) == FALSE) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else if ((EEFS_NandParseName(OldFilename, OldPath, &OldSpec) != EEFS_SUCCESS) ||
             (EEFS_NandParseName(NewFilename, NewPath, &NewSpec) != EEFS_SUCCESS) ||
             (OldSpec != EEFS_NAND_SLOT_NONE) || (NewSpec != EEFS_NAND_SLOT_NONE)) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else if (EEFS_LIB_IS_WRITE_PROTECTED != FALSE) {
        ReturnCode = EEFS_READ_ONLY_FILE_SYSTEM;
    }
    else {
        if (InodeTable->PendingCleanupBase > 0) {
            EEFS_NandRunPendingCleanup(InodeTable);
        }
        if (EEFS_NandFindEntry(InodeTable, NewPath) >= 0) {
            ReturnCode = EEFS_PERMISSION_DENIED;
        }
        else if ((i = EEFS_NandFindEntry(InodeTable, OldPath)) < 0) {
            ReturnCode = EEFS_FILE_NOT_FOUND;
        }
        else if ((InodeTable->Working.Entry[i].Attributes & EEFS_ATTRIBUTE_READONLY) != 0) {
            ReturnCode = EEFS_PERMISSION_DENIED;
        }
        else if (EEFS_NandIsOpen(InodeTable, (uint32)i, EEFS_NAND_ANY_SLOT) == TRUE) {
            ReturnCode = EEFS_PERMISSION_DENIED;
        }
        else if ((ReturnCode = EEFS_NandRewriteFirstBlocks(InodeTable, (uint32)i, NewPath, FALSE, 0)) == EEFS_SUCCESS) {
            ReturnCode = EEFS_NandAfterChange(InodeTable);
        }
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibRename() */

/* Returns file information from the RAM directory (no NAND read).  An A/B name without suffix: the active slot. */
int32 EEFS_LibStat(EEFS_InodeTable_t *InodeTable, char *Filename, EEFS_Stat_t *StatBuffer)
{
    const EEFS_NandEntry_t *E;
    uint32                  Entry;
    uint8                   Slot;
    int32                   ReturnCode;

    if ((InodeTable == NULL) || (Filename == NULL) || (StatBuffer == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if (EEFS_NandIsMounted(InodeTable) == FALSE) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else if ((ReturnCode = EEFS_NandResolve(InodeTable, Filename, &Entry, &Slot, NULL)) == EEFS_SUCCESS) {
        E = &InodeTable->Working.Entry[Entry];
        memset(StatBuffer, 0, sizeof(EEFS_Stat_t));
        StatBuffer->InodeIndex = Entry;
        StatBuffer->Crc = E->Slot[Slot].FileCrc32;
        StatBuffer->Attributes = E->Attributes;
        StatBuffer->FileSize = E->Slot[Slot].FileSize;
        StatBuffer->ModificationDate = (time_t)E->Slot[Slot].ModificationDate;
        StatBuffer->CreationDate = (time_t)E->CreationDate;
        strncpy(StatBuffer->Filename, E->Path, EEFS_MAX_FILENAME_SIZE - 1);
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibStat() */

/* Returns file information for an open file (the slot it was opened on) */
int32 EEFS_LibFstat(int32 FileDescriptor, EEFS_Stat_t *StatBuffer)
{
    const EEFS_FileDescriptor_t    *Fd;
    const EEFS_NandEntry_t         *E;
    int32                           ReturnCode;

    EEFS_LIB_LOCK;
    if ((EEFS_LibIsValidFileDescriptor(FileDescriptor) == FALSE) || (StatBuffer == NULL)) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else {
        Fd = &EEFS_FileDescriptorTable[FileDescriptor];
        E = &Fd->InodeTable->Working.Entry[Fd->InodeIndex];
        memset(StatBuffer, 0, sizeof(EEFS_Stat_t));
        StatBuffer->InodeIndex = Fd->InodeIndex;
        StatBuffer->Crc = E->Slot[Fd->Slot].FileCrc32;
        StatBuffer->Attributes = E->Attributes;
        StatBuffer->FileSize = Fd->FileSize;
        StatBuffer->ModificationDate = (time_t)E->Slot[Fd->Slot].ModificationDate;
        StatBuffer->CreationDate = (time_t)E->CreationDate;
        strncpy(StatBuffer->Filename, E->Path, EEFS_MAX_FILENAME_SIZE - 1);
        ReturnCode = EEFS_SUCCESS;
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibFstat() */

/* Sets the attributes of a file by rewriting the first block of each slot (section 6.6).  Setting the value the file
 * already has changes nothing. */
int32 EEFS_LibSetFileAttributes(EEFS_InodeTable_t *InodeTable, char *Filename, uint32 Attributes)
{
    char                Path[EEFS_NAND_MAX_PATH_SIZE];
    uint8               Spec;
    int32               i;
    int32               ReturnCode;

    if ((InodeTable == NULL) || (Filename == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if (EEFS_NandIsMounted(InodeTable) == FALSE) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else if ((Attributes != EEFS_ATTRIBUTE_NONE) && (Attributes != EEFS_ATTRIBUTE_READONLY)) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else if ((EEFS_NandParseName(Filename, Path, &Spec) != EEFS_SUCCESS) || (Spec != EEFS_NAND_SLOT_NONE)) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else if (EEFS_LIB_IS_WRITE_PROTECTED != FALSE) {
        ReturnCode = EEFS_READ_ONLY_FILE_SYSTEM;
    }
    else {
        if (InodeTable->PendingCleanupBase > 0) {
            EEFS_NandRunPendingCleanup(InodeTable);
        }
        if ((i = EEFS_NandFindEntry(InodeTable, Path)) < 0) {
            ReturnCode = EEFS_FILE_NOT_FOUND;
        }
        else if (EEFS_NandIsOpen(InodeTable, (uint32)i, EEFS_NAND_ANY_SLOT) == TRUE) {
            ReturnCode = EEFS_PERMISSION_DENIED;
        }
        else if (InodeTable->Working.Entry[i].Attributes == Attributes) {
            ReturnCode = EEFS_SUCCESS;
        }
        else if ((ReturnCode = EEFS_NandRewriteFirstBlocks(InodeTable, (uint32)i, NULL, TRUE, Attributes)) == EEFS_SUCCESS) {
            ReturnCode = EEFS_NandAfterChange(InodeTable);
        }
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibSetFileAttributes() */

/* Opens the directory of a volume */
EEFS_DirectoryDescriptor_t *EEFS_LibOpenDir(EEFS_InodeTable_t *InodeTable)
{
    EEFS_DirectoryDescriptor_t *DirectoryDescriptor = NULL;

    EEFS_LIB_LOCK;
    if ((EEFS_NandIsMounted(InodeTable) == TRUE) && (EEFS_DirectoryDescriptor.InUse == FALSE)) {
        EEFS_DirectoryDescriptor.InUse = TRUE;
        EEFS_DirectoryDescriptor.InodeIndex = 0;
        EEFS_DirectoryDescriptor.InodeTable = InodeTable;
        DirectoryDescriptor = &EEFS_DirectoryDescriptor;
    }
    EEFS_LIB_UNLOCK;
    return(DirectoryDescriptor);

} /* End of EEFS_LibOpenDir() */

/* Reads the next directory entry (every entry of Working, in use or not); NULL after the last one */
EEFS_DirectoryEntry_t *EEFS_LibReadDir(EEFS_DirectoryDescriptor_t *DirectoryDescriptor)
{
    EEFS_DirectoryEntry_t  *DirectoryEntry = NULL;
    EEFS_InodeTable_t      *Vol;
    const EEFS_NandEntry_t *E;
    const EEFS_NandSlot_t  *S;
    uint32                  i;

    EEFS_LIB_LOCK;
    if ((DirectoryDescriptor != NULL) && (DirectoryDescriptor->InUse == TRUE) &&
        (EEFS_NandIsMounted(DirectoryDescriptor->InodeTable) == TRUE) &&
        (DirectoryDescriptor->InodeIndex < EEFS_MAX_FILES)) {
        Vol = DirectoryDescriptor->InodeTable;
        i = DirectoryDescriptor->InodeIndex;
        E = &Vol->Working.Entry[i];
        memset(&EEFS_DirectoryEntry, 0, sizeof(EEFS_DirectoryEntry));
        EEFS_DirectoryEntry.InodeIndex = i;
        EEFS_DirectoryEntry.InUse = (E->InUse != 0) ? TRUE : FALSE;
        EEFS_DirectoryEntry.FileHeaderPointer = NULL;
        if (E->InUse != 0) {
            strncpy(EEFS_DirectoryEntry.Filename, E->Path, EEFS_MAX_FILENAME_SIZE - 1);
            S = &E->Slot[(E->IsAB != 0) ? E->ActiveSlot : EEFS_NAND_SLOT_A];
            if (S->InUse != 0) {
                EEFS_DirectoryEntry.MaxFileSize = ((uint32)S->AllocBlocks * Vol->BlockBytes) - Vol->PageSize;
            }
        }
        DirectoryDescriptor->InodeIndex++;
        DirectoryEntry = &EEFS_DirectoryEntry;
    }
    EEFS_LIB_UNLOCK;
    return(DirectoryEntry);

} /* End of EEFS_LibReadDir() */

/* Closes the directory */
int32 EEFS_LibCloseDir(EEFS_DirectoryDescriptor_t *DirectoryDescriptor)
{
    int32   ReturnCode;

    EEFS_LIB_LOCK;
    if ((DirectoryDescriptor == NULL) || (DirectoryDescriptor->InUse != TRUE)) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else {
        memset(DirectoryDescriptor, 0, sizeof(EEFS_DirectoryDescriptor_t));
        memset(&EEFS_DirectoryEntry, 0, sizeof(EEFS_DirectoryEntry_t));
        ReturnCode = EEFS_SUCCESS;
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibCloseDir() */

/* Returns TRUE if any files in the file system are open. */
uint8 EEFS_LibHasOpenFiles(EEFS_InodeTable_t *InodeTable)
{
    uint32  i;

    for (i = 0; i < EEFS_MAX_OPEN_FILES; i++) {
        if ((EEFS_FileDescriptorTable[i].InUse == TRUE) &&
            (EEFS_FileDescriptorTable[i].InodeTable == InodeTable)) {
            return(TRUE);
        }
    }
    return(FALSE);

} /* End of EEFS_LibHasOpenFiles() */

/* Returns TRUE if a directory is open. */
uint8 EEFS_LibHasOpenDir(EEFS_InodeTable_t *InodeTable)
{
    if ((EEFS_DirectoryDescriptor.InUse == TRUE) &&
        (EEFS_DirectoryDescriptor.InodeTable == InodeTable)) {
        return(TRUE);
    }
    return(FALSE);

} /* End of EEFS_LibHasOpenDir() */

/* Returns a pointer to the specified File Descriptor, or NULL if the specified File Descriptor is not valid */
EEFS_FileDescriptor_t *EEFS_LibFileDescriptor2Pointer(int32 FileDescriptor)
{
    if (EEFS_LibIsValidFileDescriptor(FileDescriptor)) {
        return(&EEFS_FileDescriptorTable[FileDescriptor]);
    }
    return(NULL);

} /* End of EEFS_LibFileDescriptor2Pointer() */

/* Prints the Working directory and verifies every slot in use.  EEFS_SUCCESS or the first verification error. */
int32 EEFS_LibChkDsk(EEFS_InodeTable_t *InodeTable, uint32 Flags)
{
    EEFS_NandVolumeInfo_t   Info;
    EEFS_NandPage0_t        P;
    const EEFS_NandEntry_t *E;
    const EEFS_NandSlot_t  *S;
    uint32                  i;
    uint8                   s;
    int32                   Status;
    int32                   ReturnCode = EEFS_SUCCESS;

    (void)Flags;

    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }
    if ((Status = EEFS_LibGetVolumeInfo(InodeTable, &Info)) != EEFS_SUCCESS) {
        return(Status);
    }

    EEFS_LIB_LOCK;
    printf("NAND volume: %lu blocks (%lu directory), %lu free, %lu freeing, %lu used, %lu bad, Seq %lu\n",
           (unsigned long)Info.BlockCount, (unsigned long)Info.DirRegionBlocks, (unsigned long)Info.FreeBlocks,
           (unsigned long)Info.FreeingBlocks, (unsigned long)Info.UsedBlocks, (unsigned long)Info.BadBlocks,
           (unsigned long)Info.LastSeq);
    printf("  mirrors: %s %s, pending cleanup %lu, uncommitted %s\n",
           Info.MirrorValid[0] ? "ok" : "MISSING", Info.MirrorValid[1] ? "ok" : "MISSING",
           (unsigned long)Info.PendingCleanup, Info.UncommittedChanges ? "yes" : "no");
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        E = &InodeTable->Working.Entry[i];
        if (E->InUse == 0) continue;
        printf("  %2lu %s type %u attr %lu%s\n", (unsigned long)i, E->Path, (unsigned int)E->FileType,
               (unsigned long)E->Attributes, (E->IsAB != 0) ? " A/B" : "");
        for (s = 0; s < 2; s++) {
            S = &E->Slot[s];
            if (S->InUse == 0) continue;
            Status = EEFS_NandValidateSlotP0(InodeTable, i, s, TRUE, &P);
            printf("     %c%s first %u blocks %u size %lu crc %08lx seq %lu flags %02x: %s\n", (s == 0) ? 'A' : 'B',
                   ((E->IsAB != 0) && (E->ActiveSlot == s)) ? "*" : " ", (unsigned int)S->FirstBlock,
                   (unsigned int)S->AllocBlocks, (unsigned long)S->FileSize, (unsigned long)S->FileCrc32,
                   (unsigned long)S->WriteSeq, (unsigned int)InodeTable->SlotFlags[i][s],
                   (Status == EEFS_SUCCESS) ? "ok" : ((Status == EEFS_IO_ERROR) ? "IO ERROR" : "CORRUPT"));
            if ((Status != EEFS_SUCCESS) && (ReturnCode == EEFS_SUCCESS)) {
                ReturnCode = Status;
            }
        }
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibChkDsk() */

/* Returns the number of file descriptors currently in use */
uint32 EEFS_LibGetFileDescriptorsInUse(void)
{
    return(EEFS_FileDescriptorsInUse);

} /* End of EEFS_LibGetFileDescriptorsInUse() */

/* Returns the file descriptors high water mark */
uint32 EEFS_LibGetFileDescriptorsHighWaterMark(void)
{
    return(EEFS_FileDescriptorsHighWaterMark);

} /* End of EEFS_LibGetFileDescriptorsHighWaterMark() */

/* Returns the max number of files the file system can support */
uint32 EEFS_LibGetMaxFiles(void)
{
    return(EEFS_MAX_FILES);

} /* End of EEFS_LibGetMaxFiles() */

/* Returns the max number of file descriptors */
uint32 EEFS_LibGetMaxOpenFiles(void)
{
    return(EEFS_MAX_OPEN_FILES);

} /* End of EEFS_LibGetMaxOpenFiles() */

/* Prints the paths of all open files for debugging */
void EEFS_LibPrintOpenFiles(void)
{
    EEFS_FileDescriptor_t  *Fd;
    uint32                  i;

    for (i = 0; i < EEFS_MAX_OPEN_FILES; i++) {
        Fd = &EEFS_FileDescriptorTable[i];
        if ((Fd->InUse == TRUE) && (Fd->InodeTable != NULL) && (Fd->InodeIndex < EEFS_MAX_FILES)) {
            printf("%s\n", Fd->InodeTable->Working.Entry[Fd->InodeIndex].Path);
        }
    }

} /* End of EEFS_LibPrintOpenFiles() */

/*
 * NAND API (eefs_nand.h)
 */

/* Formats the device (section 5.1).  Scratch is working RAM and is left zeroed (unmounted). */
int32 EEFS_LibFormat(EEFS_InodeTable_t *Scratch, cpuaddr BaseAddress)
{
    int32   ReturnCode;

    if ((Scratch == NULL) || (BaseAddress == 0)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if (Scratch->Mounted != 0) {
        ReturnCode = EEFS_DEVICE_IS_BUSY;
    }
    else {
        memset(Scratch, 0, sizeof(EEFS_InodeTable_t));
        Scratch->Dev = (EEFS_NandDevice_t *)BaseAddress;
        if ((ReturnCode = EEFS_NandCheckGeometry(Scratch)) == EEFS_SUCCESS) {
            ReturnCode = EEFS_NandFormat(Scratch);
        }
        memset(Scratch, 0, sizeof(EEFS_InodeTable_t));
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibFormat() */

/* Writes a whole file into the batch (sections 6.2, 6.3) */
int32 EEFS_LibWriteFile(EEFS_InodeTable_t *InodeTable, char *Name, const EEFS_NandFileInfo_t *Meta,
                        const void *Data, uint32 Size)
{
    int32   ReturnCode;

    if ((InodeTable == NULL) || (Name == NULL) || (Meta == NULL) || ((Data == NULL) && (Size != 0))) {
        return(EEFS_INVALID_ARGUMENT);
    }
    if ((Meta->FileType > EEFS_FILE_TYPE_MAX) ||
        ((Meta->Slot != EEFS_NAND_SLOT_A) && (Meta->Slot != EEFS_NAND_SLOT_B) && (Meta->Slot != EEFS_NAND_SLOT_NONE)) ||
        ((Meta->Attributes & ~(uint32)EEFS_ATTRIBUTE_READONLY) != 0)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if (EEFS_NandIsMounted(InodeTable) == FALSE) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else if ((ReturnCode = EEFS_NandWriteFileLocked(InodeTable, Name, Meta, (const uint8 *)Data, Size)) == EEFS_SUCCESS) {
        ReturnCode = EEFS_NandAfterChange(InodeTable);
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibWriteFile() */

/* Commits the batch (section 5.4) */
int32 EEFS_LibCommit(EEFS_InodeTable_t *InodeTable)
{
    int32   ReturnCode;

    if ((InodeTable == NULL) || (InodeTable->Mounted == 0)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    EEFS_LIB_LOCK;
    ReturnCode = EEFS_NandCommit(InodeTable, FALSE);
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibCommit() */

/* Discards the batch (section 5.5) */
int32 EEFS_LibAbort(EEFS_InodeTable_t *InodeTable)
{
    int32   ReturnCode;

    if ((InodeTable == NULL) || (InodeTable->Mounted == 0)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    EEFS_LIB_LOCK;
    ReturnCode = EEFS_NandAbort(InodeTable);
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibAbort() */

/* Checks page 0 and every CRC of the named copy (section 6.8).  Never falls back. */
int32 EEFS_LibVerifyFile(EEFS_InodeTable_t *InodeTable, char *Name)
{
    EEFS_NandPage0_t    P;
    uint32              Entry;
    uint8               Slot;
    int32               ReturnCode;

    if ((InodeTable == NULL) || (Name == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if (EEFS_NandIsMounted(InodeTable) == FALSE) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else if ((ReturnCode = EEFS_NandResolve(InodeTable, Name, &Entry, &Slot, NULL)) == EEFS_SUCCESS) {
        ReturnCode = EEFS_NandValidateSlotP0(InodeTable, Entry, Slot, TRUE, &P);
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibVerifyFile() */

/* Returns the directory entry and the type info of the resolved slot's page 0 (section 6.8) */
int32 EEFS_LibGetFileInfo(EEFS_InodeTable_t *InodeTable, char *Name, EEFS_NandFileInfo_t *Info)
{
    const EEFS_NandEntry_t *E;
    const EEFS_NandSlot_t  *S;
    EEFS_NandPage0_t        P;
    uint32                  Entry;
    uint8                   Slot;
    uint8                   s;
    int32                   ReturnCode;

    if ((InodeTable == NULL) || (Name == NULL) || (Info == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if (EEFS_NandIsMounted(InodeTable) == FALSE) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else if ((ReturnCode = EEFS_NandResolve(InodeTable, Name, &Entry, &Slot, NULL)) == EEFS_SUCCESS) {
        E = &InodeTable->Working.Entry[Entry];
        memset(Info, 0, sizeof(EEFS_NandFileInfo_t));
        strncpy(Info->Path, E->Path, EEFS_NAND_MAX_PATH_SIZE - 1);
        Info->FileType = E->FileType;
        Info->Slot = Slot;
        Info->IsAB = E->IsAB;
        Info->ActiveSlot = E->ActiveSlot;
        Info->FallbackCompatible = E->FallbackCompatible;
        Info->ActiveFailed = E->ActiveFailed;
        Info->Attributes = E->Attributes;
        Info->CreationDate = E->CreationDate;
        Info->Version = E->Slot[Slot].Version;
        Info->FileSize = E->Slot[Slot].FileSize;
        Info->FileCrc32 = E->Slot[Slot].FileCrc32;
        for (s = 0; s < 2; s++) {
            S = &E->Slot[s];
            Info->SlotInfo[s].InUse = S->InUse;
            Info->SlotInfo[s].Corrupt = (uint8)((InodeTable->SlotFlags[Entry][s] & EEFS_NAND_SLOTFLAG_CORRUPT) != 0);
            Info->SlotInfo[s].FirstBlock = S->FirstBlock;
            Info->SlotInfo[s].AllocBlocks = S->AllocBlocks;
            Info->SlotInfo[s].FileSize = S->FileSize;
            Info->SlotInfo[s].Version = S->Version;
            Info->SlotInfo[s].WriteSeq = S->WriteSeq;
            Info->SlotInfo[s].FileCrc32 = S->FileCrc32;
            Info->SlotInfo[s].ModificationDate = S->ModificationDate;
        }

        /* page 0 wins for the content fields; the type info exists only there */
        ReturnCode = EEFS_NandReadSlotPage0(InodeTable, Entry, Slot, &P, NULL);
        if (ReturnCode == EEFS_SUCCESS) {
            Info->FileType = P.FileType;
            Info->Version = P.Version;
            Info->FileSize = P.FileSize;
            Info->FileCrc32 = P.FileCrc32;
            memcpy(&Info->Type, &P.Type, sizeof(Info->Type));
        }
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibGetFileInfo() */

/* Space and state of the volume (section 6.9).  No NAND reads. */
int32 EEFS_LibGetVolumeInfo(EEFS_InodeTable_t *InodeTable, EEFS_NandVolumeInfo_t *Info)
{
    const EEFS_NandGeometry_t  *Geo;
    const EEFS_NandEntry_t     *E;
    uint32                      b;
    uint32                      i;
    uint8                       v;
    uint8                       m;

    if ((InodeTable == NULL) || (Info == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if (EEFS_NandIsMounted(InodeTable) == FALSE) {
        EEFS_LIB_UNLOCK;
        return(EEFS_INVALID_ARGUMENT);
    }
    Geo = &InodeTable->Dev->Geometry;
    memset(Info, 0, sizeof(EEFS_NandVolumeInfo_t));
    Info->BlockCount = Geo->BlockCount;
    Info->DirRegionBlocks = Geo->DirRegionBlocks;
    Info->DataBlocks = Geo->BlockCount - Geo->DirRegionBlocks;
    for (b = 0; b < Geo->BlockCount; b++) {
        v = InodeTable->Working.Map[b];
        if (v == EEFS_NAND_MAP_BAD) {
            Info->BadBlocks++;
        }
        else if (b < Geo->DirRegionBlocks) {
            continue;
        }
        else if (v == EEFS_NAND_MAP_FREE) {
            Info->FreeBlocks++;
        }
        else if (v == EEFS_NAND_MAP_FREEING) {
            Info->FreeingBlocks++;
        }
        else if (EEFS_NAND_MAP_IS_OWNER(v)) {
            Info->UsedBlocks++;
        }
    }
    Info->ReserveBlocks = EEFS_NAND_RESERVE_BLOCKS;
    Info->BadBlockReserve = EEFS_NAND_BADBLOCK_RESERVE;
    Info->AvailableForNew = (Info->FreeBlocks > Info->ReserveBlocks) ? (Info->FreeBlocks - Info->ReserveBlocks) : 0;
    Info->AvailableForModify = (Info->FreeBlocks > Info->BadBlockReserve) ? (Info->FreeBlocks - Info->BadBlockReserve) : 0;
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        E = &InodeTable->Working.Entry[i];
        if (E->InUse == 0) continue;
        Info->FilesInUse++;
        Info->SlotsInUse += (uint32)(E->Slot[0].InUse != 0) + (uint32)(E->Slot[1].InUse != 0);
    }
    Info->LastSeq = InodeTable->LastSeq;
    Info->NextWriteSeq = InodeTable->NextWriteSeq;
    Info->PendingCleanup = InodeTable->PendingCleanupCount;
    Info->DirectoryRebuilt = InodeTable->DirectoryRebuilt;
    Info->UncommittedChanges = (uint8)EEFS_NandHasUncommitted(InodeTable, TRUE);
    for (m = 0; m < 2; m++) {
        Info->MirrorValid[m] = (uint8)((InodeTable->Mirror[m].HasState == TRUE) && (InodeTable->Mirror[m].Disabled == FALSE));
    }
    EEFS_LIB_UNLOCK;
    return(EEFS_SUCCESS);

} /* End of EEFS_LibGetVolumeInfo() */

/* Selects the active slot of an A/B file (section 7.2).  Switching clears ActiveFailed. */
int32 EEFS_LibSetActiveSlot(EEFS_InodeTable_t *InodeTable, char *Name, uint8 Slot)
{
    EEFS_NandEntry_t   *E;
    int32               i;
    int32               ReturnCode;

    if ((InodeTable == NULL) || (Name == NULL) || (Slot > EEFS_NAND_SLOT_B)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if ((ReturnCode = EEFS_NandFindABEntry(InodeTable, Name, &i)) == EEFS_SUCCESS) {
        E = &InodeTable->Working.Entry[i];
        if (E->Slot[Slot].InUse == 0) {
            ReturnCode = EEFS_FILE_NOT_FOUND;
        }
        else {
            E->ActiveSlot = Slot;
            E->ActiveFailed = 0;
            ReturnCode = EEFS_NandAfterChange(InodeTable);
        }
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibSetActiveSlot() */

/* Selects slot Slot for every A/B file that has it.  Returns the number of entries set. */
int32 EEFS_LibSetActiveSlotAll(EEFS_InodeTable_t *InodeTable, uint8 Slot)
{
    EEFS_NandEntry_t   *E;
    uint32              i;
    int32               Count = 0;
    int32               ReturnCode;

    if ((InodeTable == NULL) || (Slot > EEFS_NAND_SLOT_B)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if ((ReturnCode = EEFS_NandCheckWritable(InodeTable)) == EEFS_SUCCESS) {
        for (i = 0; i < EEFS_MAX_FILES; i++) {
            E = &InodeTable->Working.Entry[i];
            if ((E->InUse != 0) && (E->IsAB != 0) && (E->Slot[Slot].InUse != 0)) {
                E->ActiveSlot = Slot;
                E->ActiveFailed = 0;
                Count++;
            }
        }
        ReturnCode = (Count > 0) ? EEFS_NandAfterChange(InodeTable) : EEFS_SUCCESS;
        if (ReturnCode == EEFS_SUCCESS) {
            ReturnCode = Count;
        }
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibSetActiveSlotAll() */

/* Sets the compatible-fallback flag of an A/B file (any non-zero Flag is stored as 1) */
int32 EEFS_LibSetFallbackCompatible(EEFS_InodeTable_t *InodeTable, char *Name, uint8 Flag)
{
    int32   i;
    int32   ReturnCode;

    if ((InodeTable == NULL) || (Name == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if ((ReturnCode = EEFS_NandFindABEntry(InodeTable, Name, &i)) == EEFS_SUCCESS) {
        InodeTable->Working.Entry[i].FallbackCompatible = (uint8)((Flag != 0) ? 1 : 0);
        ReturnCode = EEFS_NandAfterChange(InodeTable);
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibSetFallbackCompatible() */

/* Sets the compatible-fallback flag of every A/B file.  Returns the number of entries set. */
int32 EEFS_LibSetFallbackCompatibleAll(EEFS_InodeTable_t *InodeTable, uint8 Flag)
{
    EEFS_NandEntry_t   *E;
    uint32              i;
    int32               Count = 0;
    int32               ReturnCode;

    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if ((ReturnCode = EEFS_NandCheckWritable(InodeTable)) == EEFS_SUCCESS) {
        for (i = 0; i < EEFS_MAX_FILES; i++) {
            E = &InodeTable->Working.Entry[i];
            if ((E->InUse != 0) && (E->IsAB != 0)) {
                E->FallbackCompatible = (uint8)((Flag != 0) ? 1 : 0);
                Count++;
            }
        }
        ReturnCode = (Count > 0) ? EEFS_NandAfterChange(InodeTable) : EEFS_SUCCESS;
        if (ReturnCode == EEFS_SUCCESS) {
            ReturnCode = Count;
        }
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibSetFallbackCompatibleAll() */

/* Records that the active copy of an A/B file failed (what the application calls after the bootloader reported
 * ActiveFailed) */
int32 EEFS_LibSetFailed(EEFS_InodeTable_t *InodeTable, char *Name)
{
    int32   i;
    int32   ReturnCode;

    if ((InodeTable == NULL) || (Name == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if ((ReturnCode = EEFS_NandFindABEntry(InodeTable, Name, &i)) == EEFS_SUCCESS) {
        InodeTable->Working.Entry[i].ActiveFailed = 1;
        ReturnCode = EEFS_NandAfterChange(InodeTable);
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibSetFailed() */

/* Clears the failed flag of an A/B file */
int32 EEFS_LibClearFailed(EEFS_InodeTable_t *InodeTable, char *Name)
{
    int32   i;
    int32   ReturnCode;

    if ((InodeTable == NULL) || (Name == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if ((ReturnCode = EEFS_NandFindABEntry(InodeTable, Name, &i)) == EEFS_SUCCESS) {
        InodeTable->Working.Entry[i].ActiveFailed = 0;
        ReturnCode = EEFS_NandAfterChange(InodeTable);
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibClearFailed() */

/* Copies one slot of a file into the other slot's own blocks (section 7.4) */
int32 EEFS_LibCopySlot(EEFS_InodeTable_t *InodeTable, char *Name, uint8 From, uint8 To)
{
    char                Path[EEFS_NAND_MAX_PATH_SIZE];
    uint8               Spec;
    int32               i;
    int32               ReturnCode;

    if ((InodeTable == NULL) || (Name == NULL) || (From > EEFS_NAND_SLOT_B) || (To > EEFS_NAND_SLOT_B) || (From == To)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if ((ReturnCode = EEFS_NandCheckWritable(InodeTable)) != EEFS_SUCCESS) {
        /* not mounted or write protected */
    }
    else if ((EEFS_NandParseName(Name, Path, &Spec) != EEFS_SUCCESS) || (Spec != EEFS_NAND_SLOT_NONE)) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else if ((i = EEFS_NandFindEntry(InodeTable, Path)) < 0) {
        ReturnCode = EEFS_FILE_NOT_FOUND;
    }
    else if ((ReturnCode = EEFS_NandCopySlotLocked(InodeTable, (uint32)i, From, To)) == EEFS_SUCCESS) {
        ReturnCode = EEFS_NandAfterChange(InodeTable);
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibCopySlot() */

/* CopySlot for every A/B file with slot From in use (entry order).  Stops at the first error and returns it (the copies
 * already made stay in Working); otherwise returns the number of copies. */
int32 EEFS_LibCopySlotAll(EEFS_InodeTable_t *InodeTable, uint8 From, uint8 To)
{
    EEFS_NandEntry_t   *E;
    uint32              i;
    int32               Count = 0;
    int32               ReturnCode;

    if ((InodeTable == NULL) || (From > EEFS_NAND_SLOT_B) || (To > EEFS_NAND_SLOT_B) || (From == To)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if ((ReturnCode = EEFS_NandCheckWritable(InodeTable)) == EEFS_SUCCESS) {
        for (i = 0; i < EEFS_MAX_FILES; i++) {
            E = &InodeTable->Working.Entry[i];
            if ((E->InUse == 0) || (E->IsAB == 0) || (E->Slot[From].InUse == 0)) {
                continue;
            }
            if ((ReturnCode = EEFS_NandCopySlotLocked(InodeTable, i, From, To)) != EEFS_SUCCESS) {
                break;
            }
            Count++;
        }
        if (Count > 0) {
            EEFS_NandTouch(InodeTable);
        }
#if EEFS_NAND_AUTO_COMMIT
        if ((ReturnCode == EEFS_SUCCESS) && (Count > 0)) {
            ReturnCode = EEFS_NandCommit(InodeTable, FALSE);
        }
#endif
        if (ReturnCode == EEFS_SUCCESS) {
            ReturnCode = Count;
        }
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibCopySlotAll() */

/* Commits pending changes and writes a fresh CHECKPOINT into a new block of both mirrors (section 8.2) */
int32 EEFS_LibRepairDirectory(EEFS_InodeTable_t *InodeTable)
{
    int32   ReturnCode;

    if ((InodeTable == NULL) || (InodeTable->Mounted == 0)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    EEFS_LIB_LOCK;
    if (InodeTable->PendingCleanupBase > 0) {
        EEFS_NandRunPendingCleanup(InodeTable);
    }
    ReturnCode = EEFS_NandCommit(InodeTable, TRUE);
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibRepairDirectory() */

/*
 * Names, entries and the allocator (eefs_nand_internal.h, section 3.3)
 */

/* Splits Name into the stored path and an optional "@A"/"@B" slot suffix (section 6.1).  '@' is reserved: a path
 * that still contains one after the suffix is removed (a second '@', or an unknown suffix such as "@C") is invalid. */
int32 EEFS_NandParseName(const char *Name, char *Path, uint8 *SlotSpec)
{
    uint32  Len = 0;
    uint32  i;

    if ((Name == NULL) || (Path == NULL) || (SlotSpec == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    while ((Len < (EEFS_NAND_MAX_PATH_SIZE + 2)) && (Name[Len] != '\0')) {
        Len++;
    }
    if (Len >= (EEFS_NAND_MAX_PATH_SIZE + 2)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    *SlotSpec = EEFS_NAND_SLOT_NONE;
    if ((Len >= 2) && (Name[Len - 2] == '@') && ((Name[Len - 1] == 'A') || (Name[Len - 1] == 'B'))) {
        *SlotSpec = (Name[Len - 1] == 'A') ? EEFS_NAND_SLOT_A : EEFS_NAND_SLOT_B;
        Len -= 2;
    }
    if ((Len == 0) || (Len >= EEFS_NAND_MAX_PATH_SIZE) || (Name[0] != '/')) {
        return(EEFS_INVALID_ARGUMENT);
    }
    for (i = 0; i < Len; i++) {
        if (Name[i] == '@') {
            return(EEFS_INVALID_ARGUMENT);
        }
    }
    memset(Path, 0, EEFS_NAND_MAX_PATH_SIZE);
    memcpy(Path, Name, Len);
    return(EEFS_SUCCESS);

} /* End of EEFS_NandParseName() */

/* Index of the Working entry with this path, or EEFS_FILE_NOT_FOUND */
int32 EEFS_NandFindEntry(EEFS_InodeTable_t *Vol, const char *Path)
{
    uint32  i;

    for (i = 0; i < EEFS_MAX_FILES; i++) {
        if ((Vol->Working.Entry[i].InUse != 0) &&
            (strncmp(Vol->Working.Entry[i].Path, Path, EEFS_NAND_MAX_PATH_SIZE) == 0)) {
            return((int32)i);
        }
    }
    return(EEFS_FILE_NOT_FOUND);

} /* End of EEFS_NandFindEntry() */

/* Lowest unused Working entry, or EEFS_NO_SPACE_LEFT_ON_DEVICE */
int32 EEFS_NandAllocEntry(EEFS_InodeTable_t *Vol)
{
    uint32  i;

    for (i = 0; i < EEFS_MAX_FILES; i++) {
        if (Vol->Working.Entry[i].InUse == 0) {
            return((int32)i);
        }
    }
    return(EEFS_NO_SPACE_LEFT_ON_DEVICE);

} /* End of EEFS_NandAllocEntry() */

/* Number of FREE data blocks in Working */
uint32 EEFS_NandFreeCount(EEFS_InodeTable_t *Vol)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      b;
    uint32                      n = 0;

    for (b = Geo->DirRegionBlocks; b < Geo->BlockCount; b++) {
        if (Vol->Working.Map[b] == EEFS_NAND_MAP_FREE) n++;
    }
    return(n);

} /* End of EEFS_NandFreeCount() */

/* Takes Count FREE blocks round-robin from AllocCursor for Owner (section 6.9).  The class's reserve must remain
 * free afterwards, else EEFS_NO_SPACE_LEFT_ON_DEVICE with nothing changed.  A block is only taken when it is FREE in
 * Base as well, so a block the committed directory references is never handed out. */
int32 EEFS_NandAllocBlocks(EEFS_InodeTable_t *Vol, uint32 Count, uint16 *Out, uint8 Owner, uint8 Class)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      Reserve;
    uint32                      Free;
    uint32                      Data = Geo->BlockCount - Geo->DirRegionBlocks;
    uint32                      b;
    uint32                      n;
    uint32                      Taken = 0;

    if (Class == EEFS_NAND_ALLOC_NEW) {
        Reserve = EEFS_NAND_RESERVE_BLOCKS;
    }
    else if (Class == EEFS_NAND_ALLOC_MODIFY) {
        Reserve = EEFS_NAND_BADBLOCK_RESERVE;
    }
    else {
        Reserve = 0;
    }
    Free = EEFS_NandFreeCount(Vol);
    if ((Count > Free) || ((Free - Count) < Reserve)) {
        return(EEFS_NO_SPACE_LEFT_ON_DEVICE);
    }
    if (Count == 0) {
        return(EEFS_SUCCESS);
    }

    /* a pre-check: blocks that are FREE in Working but not in Base cannot be taken */
    n = 0;
    for (b = Geo->DirRegionBlocks; b < Geo->BlockCount; b++) {
        if ((Vol->Working.Map[b] == EEFS_NAND_MAP_FREE) && (Vol->Base.Map[b] == EEFS_NAND_MAP_FREE)) n++;
    }
    if (n < Count) {
        return(EEFS_NO_SPACE_LEFT_ON_DEVICE);
    }

    b = Vol->AllocCursor;
    if ((b < Geo->DirRegionBlocks) || (b >= Geo->BlockCount)) {
        b = Geo->DirRegionBlocks;
    }
    for (n = 0; (n < Data) && (Taken < Count); n++) {
        if ((Vol->Working.Map[b] == EEFS_NAND_MAP_FREE) && (Vol->Base.Map[b] == EEFS_NAND_MAP_FREE)) {
            Vol->Working.Map[b] = Owner;
            EEFS_NandForgetCleanup(Vol, (uint16)b);
            Out[Taken++] = (uint16)b;
        }
        b++;
        if (b >= Geo->BlockCount) {
            b = Geo->DirRegionBlocks;
        }
    }
    Vol->AllocCursor = b;
    return(EEFS_SUCCESS);

} /* End of EEFS_NandAllocBlocks() */

/* Back to FREE; a BAD block stays BAD */
void EEFS_NandReleaseBlocks(EEFS_InodeTable_t *Vol, const uint16 *Blocks, uint32 Count)
{
    uint32  i;

    for (i = 0; i < Count; i++) {
        if ((Blocks[i] < Vol->Dev->Geometry.BlockCount) && (Vol->Working.Map[Blocks[i]] != EEFS_NAND_MAP_BAD)) {
            Vol->Working.Map[Blocks[i]] = EEFS_NAND_MAP_FREE;
        }
    }

} /* End of EEFS_NandReleaseBlocks() */

/* Frees a whole slot in the batch: its blocks become FREEING, its first block goes on the cleanup list (the caller has
 * checked that the list has room) */
void EEFS_NandFreeSlot(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 Slot)
{
    EEFS_NandSlot_t    *S = &Vol->Working.Entry[Entry].Slot[Slot];
    uint8               Owner = EEFS_NAND_MAP_OWNER(Entry, Slot);
    uint32              b;

    for (b = Vol->Dev->Geometry.DirRegionBlocks; b < Vol->Dev->Geometry.BlockCount; b++) {
        if (Vol->Working.Map[b] == Owner) {
            Vol->Working.Map[b] = EEFS_NAND_MAP_FREEING;
        }
    }
    if (S->InUse != 0) {
        (void)EEFS_NandAddCleanup(Vol, S->FirstBlock);
    }
    memset(S, 0, sizeof(EEFS_NandSlot_t));

} /* End of EEFS_NandFreeSlot() */

/* Copy-on-write: every block of the slot except Keep[] becomes FREEING */
void EEFS_NandFreeBlocksExcept(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 Slot, const uint16 *Keep, uint32 KeepCount)
{
    uint8   Owner = EEFS_NAND_MAP_OWNER(Entry, Slot);
    uint32  b;
    uint32  k;

    for (b = Vol->Dev->Geometry.DirRegionBlocks; b < Vol->Dev->Geometry.BlockCount; b++) {
        if (Vol->Working.Map[b] == Owner) {
            Vol->Working.Map[b] = EEFS_NAND_MAP_FREEING;
        }
    }
    for (k = 0; k < KeepCount; k++) {
        if ((Keep[k] < Vol->Dev->Geometry.BlockCount) && (Vol->Working.Map[Keep[k]] == EEFS_NAND_MAP_FREEING)) {
            Vol->Working.Map[Keep[k]] = Owner;
        }
    }

} /* End of EEFS_NandFreeBlocksExcept() */

/* Blocks a file of Size bytes occupies: max(1, ceil((PageSize + Size) / BlockBytes)) */
uint32 EEFS_NandBlocksNeeded(EEFS_InodeTable_t *Vol, uint32 Size)
{
    uint64  Bytes = (uint64)Vol->PageSize + Size;
    uint64  n = (Bytes + Vol->BlockBytes - 1) / Vol->BlockBytes;

    if (n < 1) n = 1;
    return((n > 0xFFFFFFFFUL) ? 0xFFFFFFFFUL : (uint32)n);

} /* End of EEFS_NandBlocksNeeded() */

/* Copy-on-write "unchanged" test (section 6.3 step 5): same length, CRC match, then a byte compare of the old
 * block's data pages (uncached reads into PageBuf).  Any read error means "changed". */
boolean EEFS_NandBlockUnchanged(EEFS_InodeTable_t *Vol, uint16 OldBlock, uint32 K, uint32 OldCrc, uint32 OldLen,
                                const uint8 *NewSlice, uint32 NewLen)
{
    if ((OldLen != NewLen) || (NewLen == 0) || (NewSlice == NULL)) {
        return(FALSE);
    }
    if (EEFS_Crc32(NewSlice, NewLen) != OldCrc) {
        return(FALSE);
    }
    return(EEFS_NandBlockBytesEqual(Vol, OldBlock, K, NewSlice, NewLen));

} /* End of EEFS_NandBlockUnchanged() */

/* Checks a slot's page 0 (and, when Full, every block CRC and the file CRC); sets or clears SLOTFLAG_CORRUPT */
int32 EEFS_NandValidateSlot(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 Slot, boolean Full)
{
    EEFS_NandPage0_t    P;

    return(EEFS_NandValidateSlotP0(Vol, Entry, Slot, Full, &P));

} /* End of EEFS_NandValidateSlot() */

/* Copies the content of (SrcEntry, SrcSlot) into DstBlocks with NewP0 as the new metadata page (section 7.5): data
 * positions 1..n-1 first, the first block last, every source block checked against its CRC.  DstBlocks[] must be
 * allocated (owned by the destination) and is updated in place when a grown bad block is replaced.  NewP0->BlockCount
 * and FileSize must equal the source's; NewP0->WriteSeq is assigned here.  EEFS_SUCCESS, EEFS_CORRUPT_FILE (source),
 * EEFS_IO_ERROR.  On failure the destination blocks are released (except those that went bad). */
int32 EEFS_NandWriteCopy(EEFS_InodeTable_t *Vol, uint32 SrcEntry, uint8 SrcSlot, EEFS_NandPage0_t *NewP0, uint16 *DstBlocks)
{
    EEFS_NandWriteJob_t     Job;
    EEFS_NandPage0_t        SrcP;
    const uint8            *Raw;
    uint32                  k;
    uint32                  Crc;
    int32                   Status;
    uint8                   Owner;

    Owner = Vol->Working.Map[DstBlocks[0]];
    if ((NewP0->BlockCount > EEFS_NAND_MAX_FILE_BLOCKS) || (EEFS_NAND_MAP_IS_OWNER(Owner) == FALSE)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    Status = EEFS_NandReadSlotPage0(Vol, SrcEntry, SrcSlot, &SrcP, &Raw);
    if ((Status == EEFS_SUCCESS) &&
        ((SrcP.BlockCount != NewP0->BlockCount) || (SrcP.FileSize != NewP0->FileSize))) {
        Status = EEFS_INVALID_ARGUMENT;
    }
    if (Status != EEFS_SUCCESS) {
        for (k = 0; k < NewP0->BlockCount; k++) {      /* nothing was written: give the destination blocks back */
            if (Vol->Working.Map[DstBlocks[k]] == Owner) {
                EEFS_NandReleaseBlocks(Vol, &DstBlocks[k], 1);
            }
        }
        return(Status);
    }
    for (k = 0; k < SrcP.BlockCount; k++) {
        EEFS_NandPage0Block(Raw, k, &EEFS_NandJobSrc[k], &Crc);
        EEFS_NandJobCrcs[k] = Crc;
    }
    memset(EEFS_NandJobKept, 0, sizeof(EEFS_NandJobKept));

    memset(&Job, 0, sizeof(Job));
    Job.P0 = NewP0;
    Job.Blocks = DstBlocks;
    Job.Crcs = EEFS_NandJobCrcs;
    Job.Kept = EEFS_NandJobKept;
    Job.Src = EEFS_NandJobSrc;
    Job.FromRam = FALSE;
    Job.Owner = Owner;
    Status = EEFS_NandRunWriteJob(Vol, &Job);
    if (Status == EEFS_CORRUPT_FILE) {
        Vol->SlotFlags[SrcEntry][SrcSlot] |= EEFS_NAND_SLOTFLAG_CORRUPT;
    }
    if (Status != EEFS_SUCCESS) {
        EEFS_NandDiscardJob(Vol, &Job);
        if (Status == EEFS_NO_SPACE_LEFT_ON_DEVICE) Status = EEFS_IO_ERROR;
    }
    return(Status);

} /* End of EEFS_NandWriteCopy() */

/* Records a change of the volume (restarts a running health check) */
void EEFS_NandTouch(EEFS_InodeTable_t *Vol)
{
    Vol->VolumeChangeCounter++;

} /* End of EEFS_NandTouch() */

/*
 * Local Functions
 */

/* Returns TRUE if the specified File Descriptor is valid */
static uint8 EEFS_LibIsValidFileDescriptor(int32 FileDescriptor)
{
    if ((FileDescriptor >= 0) &&
        (FileDescriptor < EEFS_MAX_OPEN_FILES) &&
        (EEFS_FileDescriptorTable[FileDescriptor].InUse == TRUE)) {
        return(TRUE);
    }
    return(FALSE);

} /* End of EEFS_LibIsValidFileDescriptor() */

/* Allocates a descriptor (zeroed, InUse) or returns EEFS_NO_FREE_FILE_DESCRIPTOR */
static int32 EEFS_NandGetFileDescriptor(void)
{
    uint32  i;

    for (i = 0; i < EEFS_MAX_OPEN_FILES; i++) {
        if (EEFS_FileDescriptorTable[i].InUse == FALSE) {
            memset(&EEFS_FileDescriptorTable[i], 0, sizeof(EEFS_FileDescriptor_t));
            EEFS_FileDescriptorTable[i].InUse = TRUE;
            EEFS_FileDescriptorsInUse++;
            if (EEFS_FileDescriptorsInUse > EEFS_FileDescriptorsHighWaterMark) {
                EEFS_FileDescriptorsHighWaterMark = EEFS_FileDescriptorsInUse;
            }
            return((int32)i);
        }
    }
    return(EEFS_NO_FREE_FILE_DESCRIPTOR);

} /* End of EEFS_NandGetFileDescriptor() */

static void EEFS_NandFreeFileDescriptor(int32 FileDescriptor)
{
    memset(&EEFS_FileDescriptorTable[FileDescriptor], 0, sizeof(EEFS_FileDescriptor_t));
    EEFS_FileDescriptorsInUse--;

} /* End of EEFS_NandFreeFileDescriptor() */

/* TRUE if a descriptor is open on (Entry, Slot) of Vol; Slot EEFS_NAND_ANY_SLOT matches both slots */
static boolean EEFS_NandIsOpen(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 Slot)
{
    const EEFS_FileDescriptor_t    *Fd;
    uint32                          i;

    for (i = 0; i < EEFS_MAX_OPEN_FILES; i++) {
        Fd = &EEFS_FileDescriptorTable[i];
        if ((Fd->InUse == TRUE) && (Fd->InodeTable == Vol) && (Fd->InodeIndex == Entry) &&
            ((Slot == EEFS_NAND_ANY_SLOT) || (Fd->Slot == Slot))) {
            return(TRUE);
        }
    }
    return(FALSE);

} /* End of EEFS_NandIsOpen() */

static boolean EEFS_NandIsMounted(EEFS_InodeTable_t *Vol)
{
    return((boolean)((Vol != NULL) && (Vol->Mounted != 0) && (Vol->Dev != NULL)));

} /* End of EEFS_NandIsMounted() */

/* The byte compare of EEFS_NandBlockUnchanged: the data pages of OldBlock for position K (uncached reads into PageBuf)
 * equal Slice.  A read error means "not equal". */
static boolean EEFS_NandBlockBytesEqual(EEFS_InodeTable_t *Vol, uint16 OldBlock, uint32 K, const uint8 *Slice, uint32 Len)
{
    uint32  FirstPage = (K == 0) ? 1 : 0;
    uint32  Done = 0;
    uint32  n;
    uint32  p;

    for (p = FirstPage; Done < Len; p++) {
        if (p >= Vol->PagesPerBlock) {
            return(FALSE);
        }
        if (EEFS_NandReadPage(Vol, OldBlock, p, Vol->PageBuf, NULL, NULL) != EEFS_SUCCESS) {
            return(FALSE);
        }
        n = Len - Done;
        if (n > Vol->PageSize) n = Vol->PageSize;
        if (memcmp(Vol->PageBuf, &Slice[Done], n) != 0) {
            return(FALSE);
        }
        Done += n;
    }
    return(TRUE);

} /* End of EEFS_NandBlockBytesEqual() */

/* Name -> (entry, slot) for Open/Stat/GetFileInfo/VerifyFile (section 6.1): an explicit slot, else the active slot of an
 * A/B entry, else A.  The slot must be in use.  No fallback (Open applies it, section 7.3).  *SlotSpec (may be NULL)
 * receives the name's suffix. */
static int32 EEFS_NandResolve(EEFS_InodeTable_t *Vol, char *Name, uint32 *Entry, uint8 *Slot, uint8 *SlotSpec)
{
    char                Path[EEFS_NAND_MAX_PATH_SIZE];
    const EEFS_NandEntry_t *E;
    uint8               Spec;
    int32               i;

    if (EEFS_NandParseName(Name, Path, &Spec) != EEFS_SUCCESS) {
        return(EEFS_INVALID_ARGUMENT);
    }
    if ((i = EEFS_NandFindEntry(Vol, Path)) < 0) {
        return(EEFS_FILE_NOT_FOUND);
    }
    E = &Vol->Working.Entry[i];
    if (SlotSpec != NULL) {
        *SlotSpec = Spec;
    }
    if (Spec != EEFS_NAND_SLOT_NONE) {
        *Slot = Spec;
    }
    else {
        *Slot = (E->IsAB != 0) ? E->ActiveSlot : EEFS_NAND_SLOT_A;
    }
    if ((*Slot > EEFS_NAND_SLOT_B) || (E->Slot[*Slot].InUse == 0)) {
        return(EEFS_FILE_NOT_FOUND);
    }
    *Entry = (uint32)i;
    return(EEFS_SUCCESS);

} /* End of EEFS_NandResolve() */

/* Page 0 of a slot, validated against the entry: path, and an explicit page Slot equal to the slot */
static int32 EEFS_NandReadSlotPage0(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 Slot, EEFS_NandPage0_t *P,
                                    const uint8 **Raw)
{
    const EEFS_NandEntry_t *E = &Vol->Working.Entry[Entry];
    int32                   Status;

    Status = EEFS_NandReadPage0(Vol, E->Slot[Slot].FirstBlock, P, Raw);
    if ((Status == EEFS_SUCCESS) &&
        ((strncmp(P->Path, E->Path, EEFS_NAND_MAX_PATH_SIZE) != 0) ||
         ((P->Slot != EEFS_NAND_SLOT_NONE) && (P->Slot != Slot)))) {
        Status = EEFS_CORRUPT_FILE;
    }
    return(Status);

} /* End of EEFS_NandReadSlotPage0() */

/* A slot failed validation: SLOTFLAG_CORRUPT, and ActiveFailed for the active copy of an A/B entry (section 6.4) */
static void EEFS_NandSlotFailed(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 Slot)
{
    EEFS_NandEntry_t   *W = &Vol->Working.Entry[Entry];
    EEFS_NandEntry_t   *B = &Vol->Base.Entry[Entry];
    boolean             Changed = FALSE;

    if ((Vol->SlotFlags[Entry][Slot] & EEFS_NAND_SLOTFLAG_CORRUPT) == 0) {
        Vol->SlotFlags[Entry][Slot] |= EEFS_NAND_SLOTFLAG_CORRUPT;
        Changed = TRUE;
    }
    if ((W->IsAB != 0) && (W->ActiveSlot == Slot) && (W->ActiveFailed == 0)) {
        W->ActiveFailed = 1;
        /* a detected media fact: kept by Abort, persisted by the next commit (section 4) */
        if ((B->InUse != 0) && (B->IsAB != 0) && (B->ActiveSlot == Slot) &&
            (memcmp(&B->Slot[Slot], &W->Slot[Slot], sizeof(EEFS_NandSlot_t)) == 0)) {
            B->ActiveFailed = 1;
        }
        EEFS_NAND_MASK_SET(Vol->ForceDirty, Entry);
        Changed = TRUE;
    }
    if (Changed == TRUE) {
        EEFS_NandTouch(Vol);
    }

} /* End of EEFS_NandSlotFailed() */

/* EEFS_NandValidateSlot, returning the decoded page 0 */
static int32 EEFS_NandValidateSlotP0(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 Slot, boolean Full,
                                     EEFS_NandPage0_t *P)
{
    const uint8    *Raw;
    uint32          k;
    uint32          Block;
    uint32          Len;
    uint32          Off;
    uint32          FirstPage;
    uint32          Done;
    uint32          n;
    uint32          p;
    uint32          Crc;
    uint32          BlockCrc;
    uint32          FileCrc = EEFS_CRC32_INIT;
    uint16          B;
    int32           Status;

    Status = EEFS_NandReadSlotPage0(Vol, Entry, Slot, P, &Raw);
    for (k = 0; (Status == EEFS_SUCCESS) && (Full == TRUE) && (k < P->BlockCount); k++) {
        /* Raw stays valid: data pages are read uncached into PageBuf */
        EEFS_NandPage0Block(Raw, k, &B, &BlockCrc);
        Block = B;
        Len = EEFS_NandBlockDataRange(Vol->PageSize, Vol->PagesPerBlock, P->FileSize, k, &Off);
        FirstPage = (k == 0) ? 1 : 0;
        Crc = EEFS_CRC32_INIT;
        Done = 0;
        for (p = FirstPage; Done < Len; p++) {
            if (EEFS_NandReadPage(Vol, Block, p, Vol->PageBuf, NULL, NULL) != EEFS_SUCCESS) {
                Status = EEFS_IO_ERROR;
                break;
            }
            n = Len - Done;
            if (n > Vol->PageSize) n = Vol->PageSize;
            EEFS_Crc32Update2(&Crc, &FileCrc, Vol->PageBuf, n);
            Done += n;
        }
        if ((Status == EEFS_SUCCESS) && (EEFS_Crc32Final(Crc) != BlockCrc)) {
            Status = EEFS_CORRUPT_FILE;
        }
    }
    if ((Status == EEFS_SUCCESS) && (Full == TRUE) && (EEFS_Crc32Final(FileCrc) != P->FileCrc32)) {
        Status = EEFS_CORRUPT_FILE;
    }

    if (Status == EEFS_SUCCESS) {
        Vol->SlotFlags[Entry][Slot] &= (uint8)~EEFS_NAND_SLOTFLAG_CORRUPT;
    }
    else {
        EEFS_NandSlotFailed(Vol, Entry, Slot);
    }
    return(Status);

} /* End of EEFS_NandValidateSlotP0() */

/* One pass of a write job: the positions that are not kept, 1..n-1 ascending and then 0 (page 0 first, then the
 * first block's data pages).  EEFS_SUCCESS; EEFS_ERROR with *BadK = the position whose destination block failed to
 * erase, program or verify; EEFS_IO_ERROR (source read) or EEFS_CORRUPT_FILE (source CRC) for a copy job. */
static int32 EEFS_NandWriteAttempt(EEFS_InodeTable_t *Vol, EEFS_NandWriteJob_t *Job, uint32 *BadK)
{
    const EEFS_NandPage0_t *P0 = Job->P0;
    const uint8            *Page;
    uint32                  Count = P0->BlockCount;
    uint32                  i;
    uint32                  k;
    uint32                  Block;
    uint32                  Len;
    uint32                  Off;
    uint32                  FirstPage;
    uint32                  Done;
    uint32                  n;
    uint32                  p;
    uint32                  Crc;

    for (i = 1; i <= Count; i++) {
        k = (i < Count) ? i : 0;                /* positions 1..Count-1, then 0 */
        if ((k != 0) && ((Job->Kept[k / 8] & (uint8)(1u << (k % 8))) != 0)) {
            continue;
        }
        Block = Job->Blocks[k];
        *BadK = k;
        if (EEFS_NandEraseBlock(Vol, Block) != EEFS_SUCCESS) {
            return(EEFS_ERROR);
        }
        if (k == 0) {
            if (EEFS_NandEncodePage0(Vol->PageBuf, Vol->PageSize, P0, Job->Blocks, Job->Crcs) != EEFS_SUCCESS) {
                *BadK = EEFS_NAND_NO_K;
                return(EEFS_INVALID_ARGUMENT);
            }
            if (EEFS_NandProgramPageVerified(Vol, Block, 0, Vol->PageBuf, Vol->CacheBuf) != EEFS_SUCCESS) {
                return(EEFS_ERROR);
            }
        }

        Len = EEFS_NandBlockDataRange(Vol->PageSize, Vol->PagesPerBlock, P0->FileSize, k, &Off);
        FirstPage = (k == 0) ? 1 : 0;
        Crc = EEFS_CRC32_INIT;
        Done = 0;
        for (p = FirstPage; Done < Len; p++) {
            n = Len - Done;
            if (n > Vol->PageSize) n = Vol->PageSize;
            if (Job->FromRam == TRUE) {
                if (n == Vol->PageSize) {
                    Page = &Job->Data[Off + Done];      /* a full page is programmed straight from the caller's image */
                }
                else {
                    memcpy(Vol->PageBuf, &Job->Data[Off + Done], n);
                    memset(&Vol->PageBuf[n], 0xFF, Vol->PageSize - n);
                    Page = Vol->PageBuf;
                }
            }
            else {
                if (EEFS_NandReadPage(Vol, Job->Src[k], p, Vol->PageBuf, NULL, NULL) != EEFS_SUCCESS) {
                    *BadK = EEFS_NAND_NO_K;
                    return(EEFS_IO_ERROR);
                }
                if (n < Vol->PageSize) {
                    memset(&Vol->PageBuf[n], 0xFF, Vol->PageSize - n);
                }
                Crc = EEFS_Crc32Update(Crc, Vol->PageBuf, n);
                Page = Vol->PageBuf;
            }
            if (EEFS_NandProgramPageVerified(Vol, Block, p, Page, Vol->CacheBuf) != EEFS_SUCCESS) {
                return(EEFS_ERROR);
            }
            Done += n;
        }
        if ((Job->FromRam == FALSE) && (EEFS_Crc32Final(Crc) != Job->Crcs[k])) {
            *BadK = EEFS_NAND_NO_K;
            return(EEFS_CORRUPT_FILE);
        }
    }
    return(EEFS_SUCCESS);

} /* End of EEFS_NandWriteAttempt() */

/* Runs a write job, replacing grown bad destination blocks and restarting up to EEFS_NAND_WRITE_RETRIES times.  Each
 * attempt assigns a new WriteSeq.  EEFS_SUCCESS, EEFS_IO_ERROR, EEFS_NO_SPACE_LEFT_ON_DEVICE (no replacement block),
 * EEFS_CORRUPT_FILE (copy source).  The caller discards the job on failure. */
static int32 EEFS_NandRunWriteJob(EEFS_InodeTable_t *Vol, EEFS_NandWriteJob_t *Job)
{
    uint32  Restarts = 0;
    uint32  BadK;
    uint16  Replacement;
    int32   Status;

    for (;;) {
        Job->P0->WriteSeq = Vol->NextWriteSeq++;
        BadK = EEFS_NAND_NO_K;
        Status = EEFS_NandWriteAttempt(Vol, Job, &BadK);
        if (Status == EEFS_SUCCESS) {
            return(EEFS_SUCCESS);
        }
        if ((Status != EEFS_ERROR) || (BadK == EEFS_NAND_NO_K)) {
            return(Status);
        }

        /* grown bad block: mark it, take a replacement for that position, start over */
        EEFS_NandMarkBad(Vol, Job->Blocks[BadK]);
        if (++Restarts > EEFS_NAND_WRITE_RETRIES) {
            return(EEFS_IO_ERROR);
        }
        if (EEFS_NandAllocBlocks(Vol, 1, &Replacement, Job->Owner, EEFS_NAND_ALLOC_REPLACE) != EEFS_SUCCESS) {
            return(EEFS_NO_SPACE_LEFT_ON_DEVICE);
        }
        Job->Blocks[BadK] = Replacement;
    }

} /* End of EEFS_NandRunWriteJob() */

/* After a failed job: erase the new first block if it got a metadata page (no orphan is left for a rebuild) and give
 * every new block back.  Kept positions belong to the old copy and are not touched. */
static void EEFS_NandDiscardJob(EEFS_InodeTable_t *Vol, EEFS_NandWriteJob_t *Job)
{
    uint32  k;
    uint16  First = Job->Blocks[0];

    if ((Vol->Working.Map[First] == Job->Owner) && (EEFS_NandPageIsErased(Vol, First, 0) == FALSE)) {
        if (EEFS_NandEraseBlock(Vol, First) != EEFS_SUCCESS) {
            EEFS_NandMarkBad(Vol, First);
        }
    }
    for (k = 0; k < Job->P0->BlockCount; k++) {
        if ((k == 0) || ((Job->Kept[k / 8] & (uint8)(1u << (k % 8))) == 0)) {
            if (Vol->Working.Map[Job->Blocks[k]] == Job->Owner) {
                EEFS_NandReleaseBlocks(Vol, &Job->Blocks[k], 1);
            }
        }
    }

} /* End of EEFS_NandDiscardJob() */

/* WriteFile under the lock (section 6.3 steps 1-7) */
static int32 EEFS_NandWriteFileLocked(EEFS_InodeTable_t *Vol, char *Name, const EEFS_NandFileInfo_t *Meta,
                                      const uint8 *Data, uint32 Size)
{
    char                Path[EEFS_NAND_MAX_PATH_SIZE];
    EEFS_NandWriteJob_t Job;
    EEFS_NandEntry_t   *E;
    EEFS_NandSlot_t    *S;
    EEFS_NandPage0_t   *P = &EEFS_NandJobP0;
    EEFS_NandPage0_t   *OldP = &EEFS_NandJobOldP0;
    const uint8        *Raw;
    uint8               Spec;
    uint8               s;
    uint8               Owner;
    boolean             NewEntry;
    boolean             Modify;
    boolean             IsAB;
    uint32              Need;
    uint32              k;
    uint32              j;
    uint32              Len;
    uint32              OldLen;
    uint32              Off;
    uint32              OldCount;
    uint32              Crc;
    uint32              FileCrc;
    uint32              KeptCount = 0;
    uint32              NewCount;
    uint32              Now;
    uint16              OldFirst = 0;
    int32               i;
    int32               Status;

    if (EEFS_NandParseName(Name, Path, &Spec) != EEFS_SUCCESS) {
        return(EEFS_INVALID_ARGUMENT);
    }
    if (EEFS_LIB_IS_WRITE_PROTECTED != FALSE) {
        return(EEFS_READ_ONLY_FILE_SYSTEM);
    }
    Need = EEFS_NandBlocksNeeded(Vol, Size);
    if ((Need > Vol->MaxBlocksPerFile) || (Need > EEFS_NAND_MAX_FILE_BLOCKS)) {
        return(EEFS_INVALID_ARGUMENT);      /* larger than the largest file the geometry can describe */
    }
    if (Vol->PendingCleanupBase > 0) {
        EEFS_NandRunPendingCleanup(Vol);
    }

    /* step 3: entry and slot */
    if ((i = EEFS_NandFindEntry(Vol, Path)) >= 0) {
        E = &Vol->Working.Entry[i];
        if ((E->Attributes & EEFS_ATTRIBUTE_READONLY) != 0) {
            return(EEFS_PERMISSION_DENIED);
        }
        if (Spec != EEFS_NAND_SLOT_NONE) {
            s = Spec;
        }
        else if (Meta->Slot != EEFS_NAND_SLOT_NONE) {
            s = Meta->Slot;
        }
        else {
            s = (E->IsAB != 0) ? E->ActiveSlot : EEFS_NAND_SLOT_A;
        }
        NewEntry = FALSE;
        IsAB = (boolean)((E->IsAB != 0) || (s != EEFS_NAND_SLOT_A) ||
                         (Spec != EEFS_NAND_SLOT_NONE) || (Meta->Slot != EEFS_NAND_SLOT_NONE));
    }
    else {
        if ((i = EEFS_NandAllocEntry(Vol)) < 0) {
            return(EEFS_NO_SPACE_LEFT_ON_DEVICE);
        }
        E = &Vol->Working.Entry[i];
        s = (Spec != EEFS_NAND_SLOT_NONE) ? Spec : ((Meta->Slot != EEFS_NAND_SLOT_NONE) ? Meta->Slot : EEFS_NAND_SLOT_A);
        NewEntry = TRUE;
        IsAB = (boolean)((Spec != EEFS_NAND_SLOT_NONE) || (Meta->Slot != EEFS_NAND_SLOT_NONE));
    }
    S = &E->Slot[s];
    Owner = EEFS_NAND_MAP_OWNER(i, s);
    if (EEFS_NandIsOpen(Vol, (uint32)i, s) == TRUE) {
        return(EEFS_PERMISSION_DENIED);
    }

    /* step 4 */
    Modify = (boolean)(S->InUse != 0);
    if ((Modify == TRUE) && (EEFS_NandCleanupHasRoom(Vol, 1) == FALSE)) {
        return(EEFS_TRANSACTION_ERROR);
    }

    /* the new metadata page, and the CRC of every position of the new image */
    Now = (uint32)EEFS_LIB_TIME;
    memset(P, 0, sizeof(*P));
    memcpy(P->Path, Path, EEFS_NAND_MAX_PATH_SIZE);
    P->FileType = Meta->FileType;
    P->Slot = (IsAB == TRUE) ? s : EEFS_NAND_SLOT_NONE;
    P->Attributes = Meta->Attributes;
    P->Version = Meta->Version;
    P->FileSize = Size;
    P->CreationDate = (NewEntry == TRUE) ? Now : E->CreationDate;
    P->ModificationDate = Now;
    memcpy(&P->Type, &Meta->Type, sizeof(P->Type));
    P->BlockCount = Need;
    FileCrc = EEFS_CRC32_INIT;
    for (k = 0; k < Need; k++) {
        Len = EEFS_NandBlockDataRange(Vol->PageSize, Vol->PagesPerBlock, Size, k, &Off);
        Crc = EEFS_CRC32_INIT;
        if (Len > 0) {
            EEFS_Crc32Update2(&Crc, &FileCrc, &Data[Off], Len);
        }
        EEFS_NandJobCrcs[k] = EEFS_Crc32Final(Crc);
    }
    P->FileCrc32 = EEFS_Crc32Final(FileCrc);

    /* step 5: which positions can be kept (copy-on-write of an existing slot) */
    memset(EEFS_NandJobKept, 0, sizeof(EEFS_NandJobKept));
    if (Modify == TRUE) {
        OldFirst = S->FirstBlock;
        Status = EEFS_NandReadSlotPage0(Vol, (uint32)i, s, OldP, &Raw);
        if (Status == EEFS_SUCCESS) {
            OldCount = OldP->BlockCount;
            /* candidates: same length and same CRC, a block this slot owns, listed once */
            for (k = 1; (k < Need) && (k < OldCount); k++) {
                EEFS_NandPage0Block(Raw, k, &EEFS_NandJobSrc[k], &Crc);
                Len = EEFS_NandBlockDataRange(Vol->PageSize, Vol->PagesPerBlock, Size, k, &Off);
                OldLen = EEFS_NandBlockDataRange(Vol->PageSize, Vol->PagesPerBlock, OldP->FileSize, k, NULL);
                if ((Len != OldLen) || (Crc != EEFS_NandJobCrcs[k]) || (EEFS_NandJobSrc[k] == OldFirst) ||
                    (Vol->Working.Map[EEFS_NandJobSrc[k]] != Owner)) {
                    continue;
                }
                for (j = 1; j < k; j++) {
                    if (EEFS_NAND_KEPT_TEST(j) && (EEFS_NandJobSrc[j] == EEFS_NandJobSrc[k])) break;
                }
                if (j < k) continue;
                EEFS_NAND_KEPT_SET(k);
            }
            /* the byte compare (a CRC match alone is not proof); the length and CRC were matched above, so this is
             * EEFS_NandBlockUnchanged without recomputing the CRC */
            for (k = 1; (k < Need) && (k < OldCount); k++) {
                if (!EEFS_NAND_KEPT_TEST(k)) continue;
                Len = EEFS_NandBlockDataRange(Vol->PageSize, Vol->PagesPerBlock, Size, k, &Off);
                if (EEFS_NandBlockBytesEqual(Vol, EEFS_NandJobSrc[k], k, &Data[Off], Len) == TRUE) {
                    EEFS_NandJobBlocks[k] = EEFS_NandJobSrc[k];
                    KeptCount++;
                }
                else {
                    EEFS_NandJobKept[k / 8] &= (uint8)~(1u << (k % 8));
                }
            }
        }
        else {
            EEFS_NandSlotFailed(Vol, (uint32)i, s);     /* nothing of a corrupt copy is kept */
        }
    }

    /* the new blocks, atomically against the class's reserve */
    NewCount = Need - KeptCount;
    Status = EEFS_NandAllocBlocks(Vol, NewCount, EEFS_NandJobSrc, Owner,
                                  (Modify == TRUE) ? EEFS_NAND_ALLOC_MODIFY : EEFS_NAND_ALLOC_NEW);
    if (Status != EEFS_SUCCESS) {
        return(Status);         /* nothing changed; an entry found free in step 3 is simply left unused */
    }
    for (k = 0, j = 0; k < Need; k++) {
        if ((k == 0) || !EEFS_NAND_KEPT_TEST(k)) {
            EEFS_NandJobBlocks[k] = EEFS_NandJobSrc[j++];
        }
    }

    /* step 6: write */
    memset(&Job, 0, sizeof(Job));
    Job.P0 = P;
    Job.Blocks = EEFS_NandJobBlocks;
    Job.Crcs = EEFS_NandJobCrcs;
    Job.Kept = EEFS_NandJobKept;
    Job.Data = Data;
    Job.FromRam = TRUE;
    Job.Owner = Owner;
    Status = EEFS_NandRunWriteJob(Vol, &Job);
    if (Status != EEFS_SUCCESS) {
        EEFS_NandDiscardJob(Vol, &Job);
        return(Status);         /* the slot is unchanged: the old copy was never written to */
    }

    /* step 7: the new copy becomes the slot's content in Working */
    if (Modify == TRUE) {
        EEFS_NandFreeBlocksExcept(Vol, (uint32)i, s, EEFS_NandJobBlocks, Need);
        (void)EEFS_NandAddCleanup(Vol, OldFirst);   /* room was checked in step 4 */
    }
    if (NewEntry == TRUE) {
        memset(E, 0, sizeof(*E));
        memcpy(E->Path, Path, EEFS_NAND_MAX_PATH_SIZE);
        E->CreationDate = P->CreationDate;
        E->ActiveSlot = s;
    }
    E->InUse = 1;
    E->FileType = Meta->FileType;
    E->Attributes = Meta->Attributes;
    E->IsAB = (uint8)IsAB;
    if (E->IsAB == 0) {
        E->ActiveSlot = EEFS_NAND_SLOT_A;
    }
    S->InUse = 1;
    S->Reserved = 0;
    S->FirstBlock = EEFS_NandJobBlocks[0];
    S->AllocBlocks = (uint16)Need;
    S->Reserved2 = 0;
    S->FileSize = Size;
    S->Version = Meta->Version;
    S->WriteSeq = P->WriteSeq;
    S->FileCrc32 = P->FileCrc32;
    S->ModificationDate = P->ModificationDate;
    if ((E->ActiveSlot == s) && (E->ActiveFailed != 0)) {
        E->ActiveFailed = 0;    /* a rewrite of the active copy repairs it */
    }
    Vol->SlotFlags[i][s] = 0;
    return(EEFS_SUCCESS);

} /* End of EEFS_NandWriteFileLocked() */

/* Rename / SetFileAttributes (section 6.6): every slot in use gets a new first block (new page 0 plus a verbatim copy of
 * the old first block's data pages); all other blocks stay.  NewPath != NULL renames; SetAttr sets Attributes.  Nothing
 * changes in Working unless every slot was rewritten. */
static int32 EEFS_NandRewriteFirstBlocks(EEFS_InodeTable_t *Vol, uint32 Entry, const char *NewPath,
                                         boolean SetAttr, uint32 Attributes)
{
    EEFS_NandEntry_t   *E = &Vol->Working.Entry[Entry];
    EEFS_NandSlot_t    *S;
    EEFS_NandPage0_t   *P = &EEFS_NandJobP0;
    EEFS_NandWriteJob_t Job;
    const uint8        *Raw;
    uint16              NewFirst[2] = { 0, 0 };
    uint32              NewSeq[2] = { 0, 0 };
    uint32              NewMDate[2] = { 0, 0 };
    boolean             Done[2] = { FALSE, FALSE };
    uint32              InUse = 0;
    uint32              k;
    uint32              Crc;
    uint8               s;
    uint8               t;
    uint8               Owner;
    int32               Status = EEFS_SUCCESS;

    for (s = 0; s < 2; s++) {
        if (E->Slot[s].InUse != 0) InUse++;
    }
    if (EEFS_NandCleanupHasRoom(Vol, InUse) == FALSE) {
        return(EEFS_TRANSACTION_ERROR);
    }

    for (s = 0; (s < 2) && (Status == EEFS_SUCCESS); s++) {
        S = &E->Slot[s];
        if (S->InUse == 0) continue;
        Owner = EEFS_NAND_MAP_OWNER(Entry, s);

        Status = EEFS_NandReadSlotPage0(Vol, Entry, s, P, &Raw);
        if (Status != EEFS_SUCCESS) {
            if (Status == EEFS_CORRUPT_FILE) EEFS_NandSlotFailed(Vol, Entry, s);
            break;
        }
        if (P->BlockCount > EEFS_NAND_MAX_FILE_BLOCKS) {
            Status = EEFS_CORRUPT_FILE;
            break;
        }
        for (k = 0; k < P->BlockCount; k++) {
            EEFS_NandPage0Block(Raw, k, &EEFS_NandJobSrc[k], &Crc);
            EEFS_NandJobBlocks[k] = EEFS_NandJobSrc[k];
            EEFS_NandJobCrcs[k] = Crc;
        }
        memset(EEFS_NandJobKept, 0xFF, sizeof(EEFS_NandJobKept));     /* every data position is kept */
        if ((Status = EEFS_NandAllocBlocks(Vol, 1, &EEFS_NandJobBlocks[0], Owner, EEFS_NAND_ALLOC_MODIFY)) != EEFS_SUCCESS) {
            break;
        }
        if (NewPath != NULL) {
            memset(P->Path, 0, sizeof(P->Path));
            strncpy(P->Path, NewPath, EEFS_NAND_MAX_PATH_SIZE - 1);
        }
        if (SetAttr == TRUE) {
            P->Attributes = Attributes;
            P->ModificationDate = (uint32)EEFS_LIB_TIME;
        }

        memset(&Job, 0, sizeof(Job));
        Job.P0 = P;
        Job.Blocks = EEFS_NandJobBlocks;
        Job.Crcs = EEFS_NandJobCrcs;
        Job.Kept = EEFS_NandJobKept;
        Job.Src = EEFS_NandJobSrc;
        Job.FromRam = FALSE;
        Job.Owner = Owner;
        Status = EEFS_NandRunWriteJob(Vol, &Job);
        if (Status != EEFS_SUCCESS) {
            if (Status == EEFS_CORRUPT_FILE) EEFS_NandSlotFailed(Vol, Entry, s);
            if (Status == EEFS_NO_SPACE_LEFT_ON_DEVICE) Status = EEFS_IO_ERROR;
            EEFS_NandDiscardJob(Vol, &Job);
            break;
        }
        NewFirst[s] = EEFS_NandJobBlocks[0];
        NewSeq[s] = P->WriteSeq;
        NewMDate[s] = P->ModificationDate;
        Done[s] = TRUE;
    }

    if (Status != EEFS_SUCCESS) {
        /* undo the slots already rewritten: their new first block holds a valid page 0 */
        for (t = 0; t < 2; t++) {
            if (Done[t] == FALSE) continue;
            if (EEFS_NandPageIsErased(Vol, NewFirst[t], 0) == FALSE) {
                if (EEFS_NandEraseBlock(Vol, NewFirst[t]) != EEFS_SUCCESS) {
                    EEFS_NandMarkBad(Vol, NewFirst[t]);
                }
            }
            EEFS_NandReleaseBlocks(Vol, &NewFirst[t], 1);
        }
        return(Status);
    }

    for (s = 0; s < 2; s++) {
        if (Done[s] == FALSE) continue;
        S = &E->Slot[s];
        Vol->Working.Map[S->FirstBlock] = EEFS_NAND_MAP_FREEING;
        (void)EEFS_NandAddCleanup(Vol, S->FirstBlock);   /* room was checked above */
        S->FirstBlock = NewFirst[s];
        S->WriteSeq = NewSeq[s];
        S->ModificationDate = NewMDate[s];
    }
    if (NewPath != NULL) {
        memset(E->Path, 0, sizeof(E->Path));
        strncpy(E->Path, NewPath, EEFS_NAND_MAX_PATH_SIZE - 1);
    }
    if (SetAttr == TRUE) {
        E->Attributes = Attributes;
    }
    return(EEFS_SUCCESS);

} /* End of EEFS_NandRewriteFirstBlocks() */

/* Mounted and not write protected: EEFS_SUCCESS, EEFS_INVALID_ARGUMENT or EEFS_READ_ONLY_FILE_SYSTEM */
static int32 EEFS_NandCheckWritable(EEFS_InodeTable_t *Vol)
{
    if (EEFS_NandIsMounted(Vol) == FALSE) {
        return(EEFS_INVALID_ARGUMENT);
    }
    if (EEFS_LIB_IS_WRITE_PROTECTED != FALSE) {
        return(EEFS_READ_ONLY_FILE_SYSTEM);
    }
    return(EEFS_SUCCESS);

} /* End of EEFS_NandCheckWritable() */

/* The entry of an A/B setter (section 7.2): a name without suffix (else EEFS_INVALID_ARGUMENT), an entry in use (else
 * EEFS_FILE_NOT_FOUND) that is IsAB (else EEFS_INVALID_ARGUMENT); the volume mounted and writable */
static int32 EEFS_NandFindABEntry(EEFS_InodeTable_t *Vol, char *Name, int32 *Entry)
{
    char    Path[EEFS_NAND_MAX_PATH_SIZE];
    uint8   Spec;
    int32   Status;

    if ((Status = EEFS_NandCheckWritable(Vol)) != EEFS_SUCCESS) {
        return(Status);
    }
    if ((EEFS_NandParseName(Name, Path, &Spec) != EEFS_SUCCESS) || (Spec != EEFS_NAND_SLOT_NONE)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    if ((*Entry = EEFS_NandFindEntry(Vol, Path)) < 0) {
        return(EEFS_FILE_NOT_FOUND);
    }
    if (Vol->Working.Entry[*Entry].IsAB == 0) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_SUCCESS);

} /* End of EEFS_NandFindABEntry() */

/* CopySlot of entry Entry under the lock (section 7.4), without the final Touch/commit.  The destination gets its own
 * freshly allocated blocks (EEFS_NAND_ALLOC_NEW), so A and B never share a block; a destination copy that was in use
 * becomes FREEING (its first block goes on the cleanup list) and is erased only after the commit. */
static int32 EEFS_NandCopySlotLocked(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 From, uint8 To)
{
    EEFS_NandEntry_t   *E = &Vol->Working.Entry[Entry];
    EEFS_NandSlot_t    *S = &E->Slot[To];
    EEFS_NandPage0_t   *P = &EEFS_NandJobP0;
    boolean             Replace;
    uint16              OldFirst;
    int32               Status;

    if ((E->IsAB == 0) && ((From != EEFS_NAND_SLOT_A) || (E->Slot[To].InUse != 0))) {
        return(EEFS_INVALID_ARGUMENT);      /* a single-copy file can only opt in by copying A into B */
    }
    if (E->Slot[From].InUse == 0) {
        return(EEFS_FILE_NOT_FOUND);
    }
    if (((E->Attributes & EEFS_ATTRIBUTE_READONLY) != 0) || (EEFS_NandIsOpen(Vol, Entry, To) == TRUE)) {
        return(EEFS_PERMISSION_DENIED);
    }
    if (Vol->PendingCleanupBase > 0) {
        EEFS_NandRunPendingCleanup(Vol);
    }
    Replace = (boolean)(S->InUse != 0);
    if ((Replace == TRUE) && (EEFS_NandCleanupHasRoom(Vol, 1) == FALSE)) {
        return(EEFS_TRANSACTION_ERROR);
    }

    /* the source's metadata page, re-addressed to the destination slot */
    Status = EEFS_NandReadSlotPage0(Vol, Entry, From, P, NULL);
    if ((Status == EEFS_SUCCESS) && (P->BlockCount > EEFS_NAND_MAX_FILE_BLOCKS)) {
        Status = EEFS_CORRUPT_FILE;
    }
    if (Status != EEFS_SUCCESS) {
        EEFS_NandSlotFailed(Vol, Entry, From);
        return(Status);
    }
    P->Slot = To;

    /* the destination's own blocks, then the copy (blocks 1..n-1, then the first block) */
    Status = EEFS_NandAllocBlocks(Vol, P->BlockCount, EEFS_NandJobBlocks, EEFS_NAND_MAP_OWNER(Entry, To),
                                  EEFS_NAND_ALLOC_NEW);
    if (Status != EEFS_SUCCESS) {
        return(Status);
    }
    Status = EEFS_NandWriteCopy(Vol, Entry, From, P, EEFS_NandJobBlocks);
    if (Status != EEFS_SUCCESS) {
        if (Status == EEFS_CORRUPT_FILE) {
            EEFS_NandSlotFailed(Vol, Entry, From);
        }
        return(Status);         /* WriteCopy released the new blocks; the destination is unchanged */
    }

    /* the old destination copy (if any) is freed in the batch; the new one becomes the slot */
    if (Replace == TRUE) {
        OldFirst = S->FirstBlock;
        EEFS_NandFreeBlocksExcept(Vol, Entry, To, EEFS_NandJobBlocks, P->BlockCount);
        (void)EEFS_NandAddCleanup(Vol, OldFirst);   /* room was checked above */
    }
    S->InUse = 1;
    S->Reserved = 0;
    S->FirstBlock = EEFS_NandJobBlocks[0];
    S->AllocBlocks = (uint16)P->BlockCount;
    S->Reserved2 = 0;
    S->FileSize = P->FileSize;
    S->Version = P->Version;
    S->WriteSeq = P->WriteSeq;
    S->FileCrc32 = P->FileCrc32;
    S->ModificationDate = P->ModificationDate;
    if (E->IsAB == 0) {
        E->IsAB = 1;            /* opted in; A stays the active slot */
        E->ActiveSlot = EEFS_NAND_SLOT_A;
    }
    if (E->ActiveSlot == To) {
        E->ActiveFailed = 0;    /* the active copy was repaired */
    }
    Vol->SlotFlags[Entry][To] = 0;
    return(EEFS_SUCCESS);

} /* End of EEFS_NandCopySlotLocked() */

/* Common tail of every mutating call: Touch, and the commit when EEFS_NAND_AUTO_COMMIT is set */
static int32 EEFS_NandAfterChange(EEFS_InodeTable_t *Vol)
{
    EEFS_NandTouch(Vol);
#if EEFS_NAND_AUTO_COMMIT
    return(EEFS_NandCommit(Vol, FALSE));
#else
    return(EEFS_SUCCESS);
#endif

} /* End of EEFS_NandAfterChange() */

/************************/
/*  End of File Comment */
/************************/
