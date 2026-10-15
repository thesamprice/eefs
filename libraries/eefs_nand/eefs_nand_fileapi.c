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
 *   Phase 1 of docs/nand-implementation.md: the API exists so that drivers and applications build against it.
 *   Every volume function validates its pointer arguments (EEFS_INVALID_ARGUMENT) and otherwise returns
 *   EEFS_UNSUPPORTED_OPTION (NULL for the directory functions) until the phase that implements it lands.  The global
 *   descriptor table and the helpers that only inspect it (EEFS_LibInit, EEFS_LibHasOpenFiles, ...) behave as in the
 *   EEPROM backend already; nothing can open a descriptor yet.  EEFS_LibCreat and EEFS_LibWrite are final: the NAND
 *   backend writes whole files with EEFS_LibWriteFile.
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
 * Local Data
 */

/* Note: the file descriptors are shared across all file systems */
uint32                          EEFS_FileDescriptorsInUse;
uint32                          EEFS_FileDescriptorsHighWaterMark;
EEFS_FileDescriptor_t           EEFS_FileDescriptorTable[EEFS_MAX_OPEN_FILES];

/* Note: there is only one directory descriptor, so only one process can use it at a time */
EEFS_DirectoryDescriptor_t      EEFS_DirectoryDescriptor;
EEFS_DirectoryEntry_t           EEFS_DirectoryEntry;

/*
 * Local Function Prototypes
 */

static uint8                    EEFS_LibIsValidFileDescriptor(int32 FileDescriptor);

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

/* Mounts the volume (section 5.2).  Not implemented yet (phase 2). */
int32 EEFS_LibInitFS(EEFS_InodeTable_t *InodeTable, cpuaddr BaseAddress)
{
    if ((InodeTable == NULL) || (BaseAddress == 0)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibInitFS() */

/* Unmounts the volume.  Not implemented yet (phase 2). */
int32 EEFS_LibFreeFS(EEFS_InodeTable_t *InodeTable)
{
    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibFreeFS() */

/* Opens a file for reading (sections 6.4, 7.3).  Not implemented yet (phase 3). */
int32 EEFS_LibOpen(EEFS_InodeTable_t *InodeTable, char *Filename, uint32 Flags, uint32 Attributes)
{
    (void)Flags;
    (void)Attributes;

    if ((InodeTable == NULL) || (Filename == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

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

/* Closes a file.  Not implemented yet (phase 3). */
int32 EEFS_LibClose(int32 FileDescriptor)
{
    if (EEFS_LibIsValidFileDescriptor(FileDescriptor) == FALSE) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibClose() */

/* Reads from a file (section 6.7).  Not implemented yet (phase 3). */
int32 EEFS_LibRead(int32 FileDescriptor, void *Buffer, uint32 Length)
{
    (void)Length;

    if ((EEFS_LibIsValidFileDescriptor(FileDescriptor) == FALSE) || (Buffer == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

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

/* Sets the file pointer.  Not implemented yet (phase 3). */
int32 EEFS_LibLSeek(int32 FileDescriptor, int32 ByteOffset, uint16 Origin)
{
    (void)ByteOffset;
    (void)Origin;

    if (EEFS_LibIsValidFileDescriptor(FileDescriptor) == FALSE) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibLSeek() */

/* Removes a file or one slot of it (section 6.5).  Not implemented yet (phase 3). */
int32 EEFS_LibRemove(EEFS_InodeTable_t *InodeTable, char *Filename)
{
    if ((InodeTable == NULL) || (Filename == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibRemove() */

/* Renames a file by copying it (section 6.6).  Not implemented yet (phase 3). */
int32 EEFS_LibRename(EEFS_InodeTable_t *InodeTable, char *OldFilename, char *NewFilename)
{
    if ((InodeTable == NULL) || (OldFilename == NULL) || (NewFilename == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibRename() */

/* Returns file information from the RAM directory.  Not implemented yet (phase 3). */
int32 EEFS_LibStat(EEFS_InodeTable_t *InodeTable, char *Filename, EEFS_Stat_t *StatBuffer)
{
    if ((InodeTable == NULL) || (Filename == NULL) || (StatBuffer == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibStat() */

/* Returns file information for an open file.  Not implemented yet (phase 3). */
int32 EEFS_LibFstat(int32 FileDescriptor, EEFS_Stat_t *StatBuffer)
{
    if ((EEFS_LibIsValidFileDescriptor(FileDescriptor) == FALSE) || (StatBuffer == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibFstat() */

/* Sets the attributes of a file by copying it (section 6.6).  Not implemented yet (phase 3). */
int32 EEFS_LibSetFileAttributes(EEFS_InodeTable_t *InodeTable, char *Filename, uint32 Attributes)
{
    (void)Attributes;

    if ((InodeTable == NULL) || (Filename == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibSetFileAttributes() */

/* Opens the directory of a volume.  Not implemented yet (phase 3): returns NULL. */
EEFS_DirectoryDescriptor_t *EEFS_LibOpenDir(EEFS_InodeTable_t *InodeTable)
{
    (void)InodeTable;
    return(NULL);

} /* End of EEFS_LibOpenDir() */

/* Reads the next directory entry.  Not implemented yet (phase 3): returns NULL. */
EEFS_DirectoryEntry_t *EEFS_LibReadDir(EEFS_DirectoryDescriptor_t *DirectoryDescriptor)
{
    (void)DirectoryDescriptor;
    return(NULL);

} /* End of EEFS_LibReadDir() */

/* Closes the directory.  Not implemented yet (phase 3). */
int32 EEFS_LibCloseDir(EEFS_DirectoryDescriptor_t *DirectoryDescriptor)
{
    if (DirectoryDescriptor == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

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

/* Prints the directory and verifies every slot.  Not implemented yet (phase 3). */
int32 EEFS_LibChkDsk(EEFS_InodeTable_t *InodeTable, uint32 Flags)
{
    (void)Flags;

    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

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

/* Formats the device (section 5.1).  Not implemented yet (phase 2). */
int32 EEFS_LibFormat(EEFS_InodeTable_t *Scratch, cpuaddr BaseAddress)
{
    if ((Scratch == NULL) || (BaseAddress == 0)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibFormat() */

/* Writes a whole file (section 6.3).  Not implemented yet (phase 3). */
int32 EEFS_LibWriteFile(EEFS_InodeTable_t *InodeTable, char *Name, const EEFS_NandFileInfo_t *Meta,
                        const void *Data, uint32 Size)
{
    if ((InodeTable == NULL) || (Name == NULL) || (Meta == NULL) || ((Data == NULL) && (Size != 0))) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibWriteFile() */

/* Commits the batch (section 5.4).  Not implemented yet (phase 2). */
int32 EEFS_LibCommit(EEFS_InodeTable_t *InodeTable)
{
    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibCommit() */

/* Discards the batch (section 5.5).  Not implemented yet (phase 2). */
int32 EEFS_LibAbort(EEFS_InodeTable_t *InodeTable)
{
    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibAbort() */

/* Checks every CRC of a file (section 6.8).  Not implemented yet (phase 3). */
int32 EEFS_LibVerifyFile(EEFS_InodeTable_t *InodeTable, char *Name)
{
    if ((InodeTable == NULL) || (Name == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibVerifyFile() */

/* Returns the directory entry and type info of a file (section 6.8).  Not implemented yet (phase 3). */
int32 EEFS_LibGetFileInfo(EEFS_InodeTable_t *InodeTable, char *Name, EEFS_NandFileInfo_t *Info)
{
    if ((InodeTable == NULL) || (Name == NULL) || (Info == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibGetFileInfo() */

/* A/B setters (section 7.2).  Not implemented yet (phase 5). */
int32 EEFS_LibSetActiveSlot(EEFS_InodeTable_t *InodeTable, char *Name, uint8 Slot)
{
    (void)Slot;

    if ((InodeTable == NULL) || (Name == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibSetActiveSlot() */

int32 EEFS_LibSetActiveSlotAll(EEFS_InodeTable_t *InodeTable, uint8 Slot)
{
    (void)Slot;

    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibSetActiveSlotAll() */

int32 EEFS_LibSetFallbackCompatible(EEFS_InodeTable_t *InodeTable, char *Name, uint8 Flag)
{
    (void)Flag;

    if ((InodeTable == NULL) || (Name == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibSetFallbackCompatible() */

int32 EEFS_LibSetFallbackCompatibleAll(EEFS_InodeTable_t *InodeTable, uint8 Flag)
{
    (void)Flag;

    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibSetFallbackCompatibleAll() */

int32 EEFS_LibSetFailed(EEFS_InodeTable_t *InodeTable, char *Name)
{
    if ((InodeTable == NULL) || (Name == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibSetFailed() */

int32 EEFS_LibClearFailed(EEFS_InodeTable_t *InodeTable, char *Name)
{
    if ((InodeTable == NULL) || (Name == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibClearFailed() */

/* Copies one slot of a file into the other (section 7.4).  Not implemented yet (phase 5). */
int32 EEFS_LibCopySlot(EEFS_InodeTable_t *InodeTable, char *Name, uint8 From, uint8 To)
{
    (void)From;
    (void)To;

    if ((InodeTable == NULL) || (Name == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibCopySlot() */

int32 EEFS_LibCopySlotAll(EEFS_InodeTable_t *InodeTable, uint8 From, uint8 To)
{
    (void)From;
    (void)To;

    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibCopySlotAll() */

/* Commits and writes a fresh checkpoint to both mirrors (section 8.2).  Not implemented yet (phase 2). */
int32 EEFS_LibRepairDirectory(EEFS_InodeTable_t *InodeTable)
{
    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibRepairDirectory() */

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

/************************/
/*  End of File Comment */
/************************/
