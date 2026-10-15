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
 * Filename: microeefs_nand.c
 *
 * Purpose: Bootloader library for the EEFS NAND backend (see microeefs_nand.h).
 *
 * Design Notes:
 *   Read only: the only HAL call made is ReadPage.  The only page-sized buffer is the caller's PageBuffer; everything
 *   else is on the stack (one decoded entry, one decoded metadata page, the OOB of one page, the directory candidates
 *   of one mirror: about 1.5 KB with the default configuration, arm-rtems7-gcc -O2).  No malloc, no globals except the slot hook.
 *
 *   Finding the directory (docs/nand-format.md section 5, steps 1-4):
 *     for each mirror, page 0 of every ring block is read; valid CHECKPOINT pages are the candidates.  They are walked
 *     in descending Seq order; the first whose checkpoint record is valid is the mirror's current block, and the chain
 *     walk (checkpoint, then TXNs with consecutive Seq) gives the mirror's LastSeq.  The mirror with the greater
 *     LastSeq wins, mirror 0 on a tie.  The walk validates every page of every record, so the replay pass that
 *     follows (section 7.2) only re-reads the pages it has already validated.
 *   Replay (section 7.2): one candidate entry is tracked by index.  A checkpoint clears it; an ENTRY item with the
 *     candidate's index replaces it (same name, in use) or clears it (removed or renamed away); any other in-use ENTRY
 *     with the name becomes the candidate.  VOLUME items are checked against the geometry and the build.
 *   Scan fallback (section 6 for one name), only when neither mirror has a valid checkpoint.
 *   Slot choice and loading (sections 7.3, 7.4).
 *
 *   Loading reads full data pages straight into the destination; only a final partial page goes through PageBuffer,
 *   and the blocks after it (which hold no data) have their CRCs checked before that, so page 0 is never re-read.
 *
 * References:
 *   docs/nand-format.md sections 3.3, 3.4, 4, 5, 6, 7.  docs/nand-implementation.md section 9.
 */

/*
 * Includes
 */

#include <string.h>
#include "common_types.h"
#include "eefs_fileapi.h"
#include "eefs_nand_format.h"
#include "microeefs_nand.h"

/*
 * Macro Definitions
 */

#define MICROEEFS_NAND_MAX_CANDIDATES   (EEFS_NAND_MAX_DIR_BLOCKS / 2)  /* ring blocks per mirror */
#define MICROEEFS_NAND_NO_INDEX         0xFFFFFFFFUL

/*
 * Type Definitions
 */

/* The chain of a mirror's current block */
typedef struct
{
    uint32                              Block;              /* ring block holding the checkpoint */
    uint32                              LastSeq;            /* Seq of the last valid record */
    uint32                              EndPage;            /* first page after the last valid record */
    uint8                               Valid;              /* the block has a valid checkpoint */
} MicroEEFS_NandChain_t;

/*
 * Local Data
 */

static MicroEEFS_NandSlotHook_t MicroEEFS_NandSlotHook = NULL;
static void                    *MicroEEFS_NandSlotHookCtx = NULL;

/*
 * Local Function Prototypes
 */

static int32    MicroEEFS_NandRead(const EEFS_NandDevice_t *Dev, uint32 Block, uint32 Page, void *Data, uint8 *Oob);
static boolean  MicroEEFS_NandReadPage0Good(const EEFS_NandDevice_t *Dev, uint8 *Buf, uint32 Block);
static boolean  MicroEEFS_NandReadRecord(const EEFS_NandDevice_t *Dev, uint8 *Buf, uint32 Block, uint32 Page,
                                         EEFS_NandRecHdr_t *Hdr);
static void     MicroEEFS_NandWalkChain(const EEFS_NandDevice_t *Dev, uint8 *Buf, uint32 Block,
                                        MicroEEFS_NandChain_t *Chain);
static void     MicroEEFS_NandScanMirror(const EEFS_NandDevice_t *Dev, uint8 *Buf, uint32 Mirror,
                                         MicroEEFS_NandChain_t *Chain);
static int32    MicroEEFS_NandReplay(const EEFS_NandDevice_t *Dev, uint8 *Buf, const MicroEEFS_NandChain_t *Chain,
                                     const char *Path, EEFS_NandEntry_t *Entry, boolean *Found);
static boolean  MicroEEFS_NandBlocksInRange(const EEFS_NandGeometry_t *Geo, const uint8 *Page, uint32 BlockCount);
static void     MicroEEFS_NandScan(const EEFS_NandDevice_t *Dev, uint8 *Buf, const char *Path,
                                   EEFS_NandEntry_t *Entry, boolean *Found, boolean *AnyPage);
static int32    MicroEEFS_NandCheckPage0(const EEFS_NandDevice_t *Dev, uint8 *Buf, const EEFS_NandEntry_t *Entry,
                                         uint8 Slot, EEFS_NandPage0_t *P);
static int32    MicroEEFS_NandLoadData(const EEFS_NandDevice_t *Dev, uint8 *Buf, const EEFS_NandPage0_t *P,
                                       uint8 *Dest);
static int32    MicroEEFS_NandTrySlot(const EEFS_NandDevice_t *Dev, uint8 *Buf, const EEFS_NandEntry_t *Entry,
                                      uint8 Slot, uint8 *Dest, uint32 MaxSize, MicroEEFS_NandInfo_t *Info);
static int32    MicroEEFS_NandResolve(const EEFS_NandDevice_t *Dev, uint8 *PageBuffer, const char *Name,
                                      uint8 *Dest, uint32 MaxSize, MicroEEFS_NandInfo_t *Info);

/*
 * Function Definitions
 */

/* Sets the BSP slot hook; NULL restores the default */
void MicroEEFS_NandSetSlotHook(MicroEEFS_NandSlotHook_t Hook, void *HookCtx)
{
    MicroEEFS_NandSlotHook = Hook;
    MicroEEFS_NandSlotHookCtx = (Hook != NULL) ? HookCtx : NULL;

} /* End of MicroEEFS_NandSetSlotHook() */

/* Finds a file and validates the metadata page of the slot that would be loaded */
int32 MicroEEFS_NandFindFile(const EEFS_NandDevice_t *Dev, uint8 *PageBuffer, const char *Name, MicroEEFS_NandInfo_t *Info)
{
    if ((Dev == NULL) || (Dev->Ops == NULL) || (Dev->Ops->ReadPage == NULL) || (PageBuffer == NULL) ||
        (Name == NULL) || (Info == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(MicroEEFS_NandResolve(Dev, PageBuffer, Name, NULL, 0, Info));

} /* End of MicroEEFS_NandFindFile() */

/* Finds and loads a file, checking every CRC */
int32 MicroEEFS_NandLoadFile(const EEFS_NandDevice_t *Dev, uint8 *PageBuffer, const char *Name, void *Dest, uint32 MaxSize,
                             MicroEEFS_NandInfo_t *Info)
{
    if ((Dev == NULL) || (Dev->Ops == NULL) || (Dev->Ops->ReadPage == NULL) || (PageBuffer == NULL) ||
        (Name == NULL) || (Dest == NULL) || (Info == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(MicroEEFS_NandResolve(Dev, PageBuffer, Name, (uint8 *)Dest, MaxSize, Info));

} /* End of MicroEEFS_NandLoadFile() */

/*
 * Local Functions
 */

/* One HAL read: EEFS_SUCCESS (clean or corrected) or EEFS_IO_ERROR (uncorrectable ECC or controller error) */
static int32 MicroEEFS_NandRead(const EEFS_NandDevice_t *Dev, uint32 Block, uint32 Page, void *Data, uint8 *Oob)
{
    if (Dev->Ops->ReadPage(Dev->Ctx, Block, Page, Data, Oob) < 0) {
        return(EEFS_IO_ERROR);
    }
    return(EEFS_SUCCESS);

} /* End of MicroEEFS_NandRead() */

/* Reads page 0 of Block (data and OOB) into Buf.  FALSE when the read failed or the page carries a bad-block marker. */
static boolean MicroEEFS_NandReadPage0Good(const EEFS_NandDevice_t *Dev, uint8 *Buf, uint32 Block)
{
    uint8   Oob[EEFS_NAND_MAX_OOB_SIZE];

    if (MicroEEFS_NandRead(Dev, Block, 0, Buf, Oob) != EEFS_SUCCESS) {
        return(FALSE);
    }
    if (((Dev->Geometry.BadMarkerPages & EEFS_NAND_BADMARKER_PAGE0) != 0) &&
        (Dev->Geometry.BadMarkerOffset < Dev->Geometry.OobSize) &&
        (Oob[Dev->Geometry.BadMarkerOffset] != 0xFF)) {
        return(FALSE);
    }
    return(TRUE);

} /* End of MicroEEFS_NandReadPage0Good() */

/* Validates the record starting at Page of Block (docs/nand-format.md 4.2): every page valid, same Seq, RecType and
 * PageCount, PageIndex in order, inside the block.  Hdr receives the header of the first page. */
static boolean MicroEEFS_NandReadRecord(const EEFS_NandDevice_t *Dev, uint8 *Buf, uint32 Block, uint32 Page,
                                        EEFS_NandRecHdr_t *Hdr)
{
    const EEFS_NandGeometry_t  *Geo = &Dev->Geometry;
    EEFS_NandRecHdr_t           H;
    uint32                      i;

    if ((MicroEEFS_NandRead(Dev, Block, Page, Buf, NULL) != EEFS_SUCCESS) ||
        (EEFS_NandDecodeRecordPage(Buf, Geo->PageSize, Hdr) != EEFS_SUCCESS) ||
        (Hdr->PageIndex != 0) ||
        ((Page + Hdr->PageCount) > Geo->PagesPerBlock)) {
        return(FALSE);
    }

    for (i = 1; i < Hdr->PageCount; i++) {
        if ((MicroEEFS_NandRead(Dev, Block, Page + i, Buf, NULL) != EEFS_SUCCESS) ||
            (EEFS_NandDecodeRecordPage(Buf, Geo->PageSize, &H) != EEFS_SUCCESS) ||
            (H.Seq != Hdr->Seq) || (H.RecType != Hdr->RecType) ||
            (H.PageCount != Hdr->PageCount) || (H.PageIndex != i)) {
            return(FALSE);
        }
    }
    return(TRUE);

} /* End of MicroEEFS_NandReadRecord() */

/* Walks the chain of a ring block (docs/nand-format.md 5 step 3).  Chain->Valid is set when the checkpoint at page 0
 * is a valid record. */
static void MicroEEFS_NandWalkChain(const EEFS_NandDevice_t *Dev, uint8 *Buf, uint32 Block,
                                    MicroEEFS_NandChain_t *Chain)
{
    EEFS_NandRecHdr_t   H;
    uint32              Page = 0;
    uint32              LastSeq = 0;

    while (Page < Dev->Geometry.PagesPerBlock) {
        if (MicroEEFS_NandReadRecord(Dev, Buf, Block, Page, &H) == FALSE) {
            break;
        }
        if (Page == 0) {
            if (H.RecType != EEFS_NAND_REC_CHECKPOINT) {
                break;
            }
        }
        else if ((H.RecType != EEFS_NAND_REC_TXN) || (H.Seq != (uint32)(LastSeq + 1))) {
            break;
        }
        LastSeq = H.Seq;
        Page += H.PageCount;
    }

    Chain->Block = Block;
    Chain->LastSeq = LastSeq;
    Chain->EndPage = Page;
    Chain->Valid = (Page > 0) ? 1 : 0;

} /* End of MicroEEFS_NandWalkChain() */

/* Finds a mirror's current block (docs/nand-format.md 5 steps 1-2) */
static void MicroEEFS_NandScanMirror(const EEFS_NandDevice_t *Dev, uint8 *Buf, uint32 Mirror,
                                     MicroEEFS_NandChain_t *Chain)
{
    uint32              Seq[MICROEEFS_NAND_MAX_CANDIDATES];
    uint16              Blk[MICROEEFS_NAND_MAX_CANDIDATES];
    uint32              Count = 0;
    uint32              Half = Dev->Geometry.DirRegionBlocks / 2;
    uint32              Block;
    uint32              i;
    EEFS_NandRecHdr_t   H;

    for (Block = Mirror * Half; Block < ((Mirror + 1) * Half); Block++) {
        if ((MicroEEFS_NandReadPage0Good(Dev, Buf, Block) == FALSE) ||
            (EEFS_NandDecodeRecordPage(Buf, Dev->Geometry.PageSize, &H) != EEFS_SUCCESS) ||
            (H.RecType != EEFS_NAND_REC_CHECKPOINT) || (H.PageIndex != 0)) {
            continue;
        }
        /* insert, keeping the list in descending Seq order */
        for (i = Count; (i > 0) && (Seq[i - 1] < H.Seq); i--) {
            Seq[i] = Seq[i - 1];
            Blk[i] = Blk[i - 1];
        }
        Seq[i] = H.Seq;
        Blk[i] = (uint16)Block;
        Count++;
    }

    Chain->Valid = 0;
    for (i = 0; i < Count; i++) {
        MicroEEFS_NandWalkChain(Dev, Buf, Blk[i], Chain);
        if (Chain->Valid != 0) {
            return;
        }
    }

} /* End of MicroEEFS_NandScanMirror() */

/* Replays a chain tracking the single entry named Path (docs/nand-format.md 7.2).  Every record of the chain was
 * validated by the walk, so a page that no longer reads or decodes is an error rather than the end of the chain. */
static int32 MicroEEFS_NandReplay(const EEFS_NandDevice_t *Dev, uint8 *Buf, const MicroEEFS_NandChain_t *Chain,
                                  const char *Path, EEFS_NandEntry_t *Entry, boolean *Found)
{
    const EEFS_NandGeometry_t  *Geo = &Dev->Geometry;
    EEFS_NandRecHdr_t           H;
    EEFS_NandVolumeItem_t       V;
    uint32                      CandIndex = MICROEEFS_NAND_NO_INDEX;
    uint32                      PathLen = (uint32)strlen(Path);
    uint32                      Page;
    uint32                      Cursor;
    uint32                      Index;
    uint16                      DecodedIndex;
    uint8                       Type;
    const uint8                *Body;
    const uint8                *Rec;
    uint16                      Len;
    int32                       Status;
    boolean                     Match;

    for (Page = 0; Page < Chain->EndPage; Page++) {
        Status = MicroEEFS_NandRead(Dev, Chain->Block, Page, Buf, NULL);
        if (Status != EEFS_SUCCESS) {
            return(Status);
        }
        if (EEFS_NandDecodeRecordPage(Buf, Geo->PageSize, &H) != EEFS_SUCCESS) {
            return(EEFS_CORRUPT_FILE);
        }
        if ((H.RecType == EEFS_NAND_REC_CHECKPOINT) && (H.PageIndex == 0)) {
            CandIndex = MICROEEFS_NAND_NO_INDEX;      /* a checkpoint starts from "no entry in use" */
        }

        Cursor = 0;
        while ((Status = EEFS_NandNextItem(&Buf[EEFS_NAND_REC_HDR_SIZE], H.PayloadLen, &Cursor,
                                           &Type, &Body, &Len)) == 1) {
            if (Type == EEFS_NAND_ITEM_VOLUME) {
                if (EEFS_NandDecodeVolumeItem(Body, Len, &V) != EEFS_SUCCESS) {
                    return(EEFS_CORRUPT_FILE);
                }
                if ((V.PageSize != Geo->PageSize) || (V.PagesPerBlock != Geo->PagesPerBlock) ||
                    (V.BlockCount != Geo->BlockCount) || (V.DirRegionBlocks != Geo->DirRegionBlocks) ||
                    (V.PathSize != EEFS_NAND_MAX_PATH_SIZE) || (V.CfsNameSize != EEFS_NAND_CFS_NAME_SIZE) ||
                    (V.CfsSymbolSize != EEFS_NAND_CFS_SYMBOL_SIZE)) {
                    return(EEFS_GEOMETRY_MISMATCH);
                }
            }
            else if (Type == EEFS_NAND_ITEM_ENTRY) {
                /* compare in place; decode only the item that becomes the candidate */
                Index = EEFS_NandGet16(&Body[EEFS_NAND_ENT_OFF_INDEX]);
                Rec = &Body[EEFS_NAND_ENT_OFF_RECORD];
                Match = ((Rec[EEFS_NAND_ER_OFF_INUSE] != 0) &&
                         (memcmp(&Rec[EEFS_NAND_ER_OFF_PATH], Path, PathLen + 1) == 0)) ? TRUE : FALSE;
                if (Match == TRUE) {
                    if (EEFS_NandDecodeEntryItem(Body, Len, &DecodedIndex, Entry) != EEFS_SUCCESS) {
                        return(EEFS_CORRUPT_FILE);
                    }
                    CandIndex = Index;
                }
                else if (Index == CandIndex) {
                    CandIndex = MICROEEFS_NAND_NO_INDEX;  /* removed, or renamed away */
                }
            }
            /* MAPCHUNK, CLEANUP and unknown items are skipped */
        }
        if (Status != 0) {
            return(EEFS_CORRUPT_FILE);
        }
    }

    *Found = (CandIndex != MICROEEFS_NAND_NO_INDEX) ? TRUE : FALSE;
    return(EEFS_SUCCESS);

} /* End of MicroEEFS_NandReplay() */

/* TRUE when every Blocks[] element of an (already decoded) metadata page is a data block */
static boolean MicroEEFS_NandBlocksInRange(const EEFS_NandGeometry_t *Geo, const uint8 *Page, uint32 BlockCount)
{
    uint32  k;
    uint16  Block;

    for (k = 0; k < BlockCount; k++) {
        EEFS_NandPage0Block(Page, k, &Block, NULL);
        if ((Block < Geo->DirRegionBlocks) || (Block >= Geo->BlockCount)) {
            return(FALSE);
        }
    }
    return(TRUE);

} /* End of MicroEEFS_NandBlocksInRange() */

/* Scan fallback (docs/nand-format.md 6, for one name): the valid metadata page with the highest WriteSeq per slot
 * (lowest block on a tie) wins.  AnyPage tells whether any valid metadata page exists at all. */
static void MicroEEFS_NandScan(const EEFS_NandDevice_t *Dev, uint8 *Buf, const char *Path,
                               EEFS_NandEntry_t *Entry, boolean *Found, boolean *AnyPage)
{
    const EEFS_NandGeometry_t  *Geo = &Dev->Geometry;
    EEFS_NandPage0_t            P;
    EEFS_NandSlot_t            *S;
    uint32                      PathLen = (uint32)strlen(Path);
    uint32                      Block;
    uint8                       Slot;
    uint8                       Explicit[2];
    uint32                      Attributes[2];
    uint32                      CreationDate[2];
    uint8                       FileType[2];

    memset(Entry, 0, sizeof(EEFS_NandEntry_t));
    memset(Explicit, 0, sizeof(Explicit));
    memset(Attributes, 0, sizeof(Attributes));
    memset(CreationDate, 0, sizeof(CreationDate));
    memset(FileType, 0, sizeof(FileType));
    *AnyPage = FALSE;
    *Found = FALSE;

    for (Block = Geo->DirRegionBlocks; Block < Geo->BlockCount; Block++) {
        if (MicroEEFS_NandReadPage0Good(Dev, Buf, Block) == FALSE) {
            continue;
        }
        /* once a metadata page has been seen, skip other names before paying for the CRC */
        if ((*AnyPage == TRUE) && (memcmp(&Buf[EEFS_NAND_P0_OFF_PATH], Path, PathLen + 1) != 0)) {
            continue;
        }
        if ((EEFS_NandDecodePage0(Buf, Geo->PageSize, Geo->PagesPerBlock, Block, &P) != EEFS_SUCCESS) ||
            (MicroEEFS_NandBlocksInRange(Geo, Buf, P.BlockCount) == FALSE)) {
            continue;
        }
        *AnyPage = TRUE;
        if (strcmp(P.Path, Path) != 0) {
            continue;
        }

        Slot = (P.Slot == EEFS_NAND_SLOT_B) ? EEFS_NAND_SLOT_B : EEFS_NAND_SLOT_A;
        S = &Entry->Slot[Slot];
        if ((S->InUse != 0) && (P.WriteSeq <= S->WriteSeq)) {
            continue;
        }
        S->InUse = 1;
        S->FirstBlock = (uint16)Block;
        S->AllocBlocks = (uint16)P.BlockCount;
        S->FileSize = P.FileSize;
        S->Version = P.Version;
        S->WriteSeq = P.WriteSeq;
        S->FileCrc32 = P.FileCrc32;
        S->ModificationDate = P.ModificationDate;
        Explicit[Slot] = (P.Slot != EEFS_NAND_SLOT_NONE) ? 1 : 0;
        Attributes[Slot] = P.Attributes;
        CreationDate[Slot] = P.CreationDate;
        FileType[Slot] = P.FileType;
    }

    if ((Entry->Slot[EEFS_NAND_SLOT_A].InUse == 0) && (Entry->Slot[EEFS_NAND_SLOT_B].InUse == 0)) {
        return;
    }

    strcpy(Entry->Path, Path);
    Entry->InUse = 1;
    Entry->IsAB = ((Explicit[0] != 0) || (Explicit[1] != 0)) ? 1 : 0;
    Entry->ActiveSlot = (Entry->Slot[EEFS_NAND_SLOT_A].InUse != 0) ? EEFS_NAND_SLOT_A : EEFS_NAND_SLOT_B;
    Entry->FallbackCompatible = 0;
    Entry->ActiveFailed = 0;
    Entry->FileType = FileType[Entry->ActiveSlot];
    Entry->Attributes = Attributes[Entry->ActiveSlot];
    Entry->CreationDate = CreationDate[Entry->ActiveSlot];
    *Found = TRUE;

} /* End of MicroEEFS_NandScan() */

/* Reads and validates the metadata page of one slot (docs/nand-format.md 7.4 step 1).  On success Buf holds the page
 * and P its decoded fields. */
static int32 MicroEEFS_NandCheckPage0(const EEFS_NandDevice_t *Dev, uint8 *Buf, const EEFS_NandEntry_t *Entry,
                                      uint8 Slot, EEFS_NandPage0_t *P)
{
    const EEFS_NandGeometry_t  *Geo = &Dev->Geometry;
    uint32                      Block = Entry->Slot[Slot].FirstBlock;

    if ((Entry->Slot[Slot].InUse == 0) || (Block < Geo->DirRegionBlocks) || (Block >= Geo->BlockCount)) {
        return(EEFS_CORRUPT_FILE);
    }
    if (MicroEEFS_NandRead(Dev, Block, 0, Buf, NULL) != EEFS_SUCCESS) {
        return(EEFS_IO_ERROR);
    }
    if ((EEFS_NandDecodePage0(Buf, Geo->PageSize, Geo->PagesPerBlock, Block, P) != EEFS_SUCCESS) ||
        (MicroEEFS_NandBlocksInRange(Geo, Buf, P->BlockCount) == FALSE) ||
        (strcmp(P->Path, Entry->Path) != 0) ||
        ((P->Slot != EEFS_NAND_SLOT_NONE) && (P->Slot != Slot))) {
        return(EEFS_CORRUPT_FILE);
    }
    return(EEFS_SUCCESS);

} /* End of MicroEEFS_NandCheckPage0() */

/* Loads the data of a validated metadata page held in Buf (docs/nand-format.md 7.4 steps 3-4) */
static int32 MicroEEFS_NandLoadData(const EEFS_NandDevice_t *Dev, uint8 *Buf, const EEFS_NandPage0_t *P,
                                    uint8 *Dest)
{
    const EEFS_NandGeometry_t  *Geo = &Dev->Geometry;
    uint32                      PageSize = Geo->PageSize;
    uint32                      k;
    uint32                      j;
    uint32                      Count;
    uint32                      Offset;
    uint32                      Done;
    uint32                      Chunk;
    uint32                      Page;
    uint32                      State;
    uint32                      Crc;
    uint16                      Block;
    int32                       Status;
    boolean                     Last = FALSE;

    for (k = 0; (k < P->BlockCount) && (Last == FALSE); k++) {
        EEFS_NandPage0Block(Buf, k, &Block, &Crc);
        Count = EEFS_NandBlockDataRange(PageSize, Geo->PagesPerBlock, P->FileSize, k, &Offset);
        if (Count == 0) {
            if (Crc != 0) {                             /* the CRC of zero bytes */
                return(EEFS_CORRUPT_FILE);
            }
            continue;
        }

        if ((Count % PageSize) != 0) {
            /* This block holds the file's last, partial page, which is read through Buf.  Check the remaining
             * Blocks[] entries (no data, CRC 0) now, while Buf still holds page 0. */
            for (j = k + 1; j < P->BlockCount; j++) {
                EEFS_NandPage0Block(Buf, j, NULL, &Crc);
                if (Crc != 0) {
                    return(EEFS_CORRUPT_FILE);
                }
            }
            EEFS_NandPage0Block(Buf, k, NULL, &Crc);
            Last = TRUE;                                /* Buf no longer holds page 0 after this block */
        }

        State = EEFS_CRC32_INIT;
        Page = (k == 0) ? 1 : 0;
        for (Done = 0; Done < Count; Done += Chunk, Page++) {
            Chunk = Count - Done;
            if (Chunk >= PageSize) {
                Chunk = PageSize;
                Status = MicroEEFS_NandRead(Dev, Block, Page, &Dest[Offset + Done], NULL);
            }
            else {
                Status = MicroEEFS_NandRead(Dev, Block, Page, Buf, NULL);
                memcpy(&Dest[Offset + Done], Buf, Chunk);
            }
            if (Status != EEFS_SUCCESS) {
                return(Status);
            }
            State = EEFS_Crc32Update(State, &Dest[Offset + Done], Chunk);
        }
        if (EEFS_Crc32Final(State) != Crc) {
            return(EEFS_CORRUPT_FILE);
        }
    }

    if (EEFS_Crc32(Dest, P->FileSize) != P->FileCrc32) {
        return(EEFS_CORRUPT_FILE);
    }
    return(EEFS_SUCCESS);

} /* End of MicroEEFS_NandLoadData() */

/* Validates (and, with Dest, loads) one slot.  On success fills the slot-dependent fields of Info. */
static int32 MicroEEFS_NandTrySlot(const EEFS_NandDevice_t *Dev, uint8 *Buf, const EEFS_NandEntry_t *Entry,
                                   uint8 Slot, uint8 *Dest, uint32 MaxSize, MicroEEFS_NandInfo_t *Info)
{
    EEFS_NandPage0_t    P;
    int32               Status;

    Status = MicroEEFS_NandCheckPage0(Dev, Buf, Entry, Slot, &P);
    if ((Status == EEFS_SUCCESS) && (Dest != NULL)) {
        if (P.FileSize > MaxSize) {
            return(EEFS_NO_SPACE_LEFT_ON_DEVICE);
        }
        Status = MicroEEFS_NandLoadData(Dev, Buf, &P, Dest);
    }
    if (Status != EEFS_SUCCESS) {
        return(Status);
    }

    Info->LoadedSlot = Slot;
    Info->Info.Slot = Slot;
    Info->Info.FileType = P.FileType;
    Info->Info.Version = P.Version;
    Info->Info.FileSize = P.FileSize;
    Info->Info.FileCrc32 = P.FileCrc32;
    Info->Info.Type = P.Type;
    return(EEFS_SUCCESS);

} /* End of MicroEEFS_NandTrySlot() */

/* FindFile (Dest == NULL) and LoadFile */
static int32 MicroEEFS_NandResolve(const EEFS_NandDevice_t *Dev, uint8 *PageBuffer, const char *Name,
                                   uint8 *Dest, uint32 MaxSize, MicroEEFS_NandInfo_t *Info)
{
    const EEFS_NandGeometry_t  *Geo = &Dev->Geometry;
    MicroEEFS_NandChain_t       Chain[2];
    const MicroEEFS_NandChain_t *Use;
    EEFS_NandEntry_t            Entry;
    EEFS_NandSlotInfo_t        *SI;
    uint32                      Len;
    uint32                      s;
    uint8                       SlotSpec = EEFS_NAND_SLOT_NONE;
    uint8                       DirSlot;
    uint8                       Try;
    uint8                       Other;
    boolean                     Found = FALSE;
    boolean                     AnyPage = FALSE;
    boolean                     AllowFallback;
    int32                       Status;
    int32                       Status2;

    memset(Info, 0, sizeof(MicroEEFS_NandInfo_t));
    Info->LoadedSlot = EEFS_NAND_SLOT_NONE;
    Info->Info.Slot = EEFS_NAND_SLOT_NONE;

    /* Geometry this reader can handle */
    if ((Geo->PageSize < EEFS_NAND_MIN_PAGE_SIZE) || (EEFS_NandMaxBlocksPerFile(Geo->PageSize) < 1) ||
        (Geo->OobSize < 1) || (Geo->OobSize > EEFS_NAND_MAX_OOB_SIZE) || (Geo->PagesPerBlock < 2) ||
        (Geo->DirRegionBlocks < 4) || ((Geo->DirRegionBlocks % 2) != 0) ||
        (Geo->DirRegionBlocks > EEFS_NAND_MAX_DIR_BLOCKS) || (Geo->BlockCount <= Geo->DirRegionBlocks) ||
        (Geo->BlockCount > EEFS_NAND_MAX_BLOCKS)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    /* Name: "path", "path@A" or "path@B" */
    for (Len = 0; (Len < (EEFS_NAND_MAX_PATH_SIZE + 2)) && (Name[Len] != '\0'); Len++) {
    }
    if ((Len >= 2) && (Name[Len - 2] == '@') && ((Name[Len - 1] == 'A') || (Name[Len - 1] == 'B'))) {
        SlotSpec = (Name[Len - 1] == 'A') ? EEFS_NAND_SLOT_A : EEFS_NAND_SLOT_B;
        Len -= 2;
    }
    if ((Len == 0) || (Len >= EEFS_NAND_MAX_PATH_SIZE) || (Name[0] != '/')) {
        return(EEFS_INVALID_ARGUMENT);
    }
    memcpy(Info->Info.Path, Name, Len);
    Info->Info.Path[Len] = '\0';

    /* Directory (sections 5 and 7.2), or the scan when neither mirror has a valid checkpoint (7.2 step 3) */
    MicroEEFS_NandScanMirror(Dev, PageBuffer, 0, &Chain[0]);
    MicroEEFS_NandScanMirror(Dev, PageBuffer, 1, &Chain[1]);
    if ((Chain[0].Valid != 0) || (Chain[1].Valid != 0)) {
        if (Chain[0].Valid == 0) {
            Use = &Chain[1];
        }
        else if (Chain[1].Valid == 0) {
            Use = &Chain[0];
        }
        else {
            Use = (Chain[1].LastSeq > Chain[0].LastSeq) ? &Chain[1] : &Chain[0];
        }
        Info->DirectoryValid = 1;
        Info->DirSeq = Use->LastSeq;
        Status = MicroEEFS_NandReplay(Dev, PageBuffer, Use, Info->Info.Path, &Entry, &Found);
        if (Status != EEFS_SUCCESS) {
            return(Status);
        }
    }
    else {
        Info->UsedScan = 1;
        MicroEEFS_NandScan(Dev, PageBuffer, Info->Info.Path, &Entry, &Found, &AnyPage);
        if (AnyPage == FALSE) {
            return(EEFS_NOT_FORMATTED);
        }
    }
    if (Found == FALSE) {
        return(EEFS_FILE_NOT_FOUND);
    }

    /* Directory view of the file */
    Info->Info.IsAB = Entry.IsAB;
    Info->Info.ActiveSlot = Entry.ActiveSlot;
    Info->Info.FallbackCompatible = Entry.FallbackCompatible;
    Info->Info.ActiveFailed = Entry.ActiveFailed;
    Info->Info.FileType = Entry.FileType;
    Info->Info.Attributes = Entry.Attributes;
    Info->Info.CreationDate = Entry.CreationDate;
    for (s = 0; s < 2; s++) {
        SI = &Info->Info.SlotInfo[s];
        SI->InUse = Entry.Slot[s].InUse;
        SI->FirstBlock = Entry.Slot[s].FirstBlock;
        SI->AllocBlocks = Entry.Slot[s].AllocBlocks;
        SI->FileSize = Entry.Slot[s].FileSize;
        SI->Version = Entry.Slot[s].Version;
        SI->WriteSeq = Entry.Slot[s].WriteSeq;
        SI->FileCrc32 = Entry.Slot[s].FileCrc32;
        SI->ModificationDate = Entry.Slot[s].ModificationDate;
    }

    /* Slot choice (section 7.3).  An explicit "@A"/"@B" names that copy only: no hook and no fallback. */
    DirSlot = ((Entry.IsAB != 0) && (Entry.ActiveSlot == EEFS_NAND_SLOT_B)) ? EEFS_NAND_SLOT_B : EEFS_NAND_SLOT_A;
    if (SlotSpec != EEFS_NAND_SLOT_NONE) {
        if (Entry.Slot[SlotSpec].InUse == 0) {
            return(EEFS_FILE_NOT_FOUND);
        }
        Try = SlotSpec;
        AllowFallback = FALSE;
    }
    else {
        Try = DirSlot;
        if (MicroEEFS_NandSlotHook != NULL) {
            Try = MicroEEFS_NandSlotHook(Info->Info.Path, DirSlot, MicroEEFS_NandSlotHookCtx);
        }
        if ((Try > EEFS_NAND_SLOT_B) || (Entry.Slot[Try].InUse == 0)) {
            Try = DirSlot;
        }
        AllowFallback = TRUE;
    }
    Info->Info.FileSize = Entry.Slot[Try].FileSize;
    Info->Info.FileCrc32 = Entry.Slot[Try].FileCrc32;
    Info->Info.Version = Entry.Slot[Try].Version;

    Status = MicroEEFS_NandTrySlot(Dev, PageBuffer, &Entry, Try, Dest, MaxSize, Info);
    if ((Status == EEFS_SUCCESS) || (Status == EEFS_NO_SPACE_LEFT_ON_DEVICE)) {
        return(Status);                                 /* a too small destination is not a failed copy */
    }

    Info->Info.SlotInfo[Try].Corrupt = 1;
    if ((AllowFallback == TRUE) || (Try == DirSlot)) {
        Info->Info.ActiveFailed = 1;
    }
    Other = (uint8)(1 - Try);
    if ((AllowFallback == TRUE) && (Entry.Slot[Other].InUse != 0) && (Entry.FallbackCompatible != 0)) {
        Status2 = MicroEEFS_NandTrySlot(Dev, PageBuffer, &Entry, Other, Dest, MaxSize, Info);
        if ((Status2 != EEFS_SUCCESS) && (Status2 != EEFS_NO_SPACE_LEFT_ON_DEVICE)) {
            Info->Info.SlotInfo[Other].Corrupt = 1;
        }
        return(Status2);
    }
    return(Status);

} /* End of MicroEEFS_NandResolve() */

/************************/
/*  End of File Comment */
/************************/
