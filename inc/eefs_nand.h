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
 * Filename: eefs_nand.h
 *
 * Purpose: Public API of the NAND backend, in addition to the EEFS_Lib* functions declared in eefs_fileapi.h (which
 *   keep their signatures).  Build with EEFS_BACKEND_NAND defined.
 *
 * Design Notes:
 *   The volume is an EEFS_InodeTable_t; its BaseAddress is a pointer to an EEFS_NandDevice_t (eefs_nand_hal.h).
 *   Files are written whole with EEFS_LibWriteFile, and every change is collected in a batch that EEFS_LibCommit makes
 *   durable with one directory record (or EEFS_LibAbort discards).  Name arguments accept "path", "path@A" or
 *   "path@B".  All functions take EEFS_LIB_LOCK for their whole duration unless stated, and return
 *   EEFS_INVALID_ARGUMENT for NULL pointers and an unmounted volume in addition to the codes listed.
 *
 * References:
 *   docs/nand-implementation.md section 2.6, docs/nand-format.md.
 */

#ifndef _eefs_nand_
#define _eefs_nand_

/*
 * Includes
 */

#include "common_types.h"
#include "eefs_fileapi.h"
#include "eefs_nand_hal.h"

#ifndef EEFS_BACKEND_NAND
#error "eefs_nand.h needs EEFS_BACKEND_NAND (configure with -DEEFS_BACKEND=nand)"
#endif

/*
 * Exported Functions
 */

/* Formats the device behind BaseAddress (an EEFS_NandDevice_t *).  Scratch is a volume that must not be mounted; it
 * is used as working RAM and left unmounted (zeroed).  EEFS_SUCCESS, EEFS_INVALID_ARGUMENT (geometry),
 * EEFS_DEVICE_IS_BUSY (Scratch mounted), EEFS_IO_ERROR (a mirror has fewer than 2 good blocks, or the checkpoint
 * could not be written to either mirror). */
int32   EEFS_LibFormat(EEFS_InodeTable_t *Scratch, cpuaddr BaseAddress);

/* Whole-file write.  Data may be NULL only when Size == 0.  EEFS_SUCCESS, EEFS_INVALID_ARGUMENT (bad name, bad
 * type/slot, Size > max), EEFS_PERMISSION_DENIED (read-only attribute, file open), EEFS_READ_ONLY_FILE_SYSTEM
 * (write-protected), EEFS_NO_SPACE_LEFT_ON_DEVICE (entries or blocks), EEFS_TRANSACTION_ERROR (cleanup list full),
 * EEFS_IO_ERROR (retries exhausted). */
int32   EEFS_LibWriteFile(EEFS_InodeTable_t *InodeTable, char *Name, const EEFS_NandFileInfo_t *Meta,
                          const void *Data, uint32 Size);

/* Commit: EEFS_SUCCESS (also when there is nothing to write), EEFS_IO_ERROR (no mirror accepted the record).
 * Abort: EEFS_SUCCESS, EEFS_DEVICE_IS_BUSY (a file opened from a batch-created slot is still open). */
int32   EEFS_LibCommit(EEFS_InodeTable_t *InodeTable);
int32   EEFS_LibAbort(EEFS_InodeTable_t *InodeTable);

/* Reads page 0 and every data page of the named copy (explicit slot, or active slot; no fallback) and checks all
 * CRCs.  EEFS_SUCCESS, EEFS_FILE_NOT_FOUND, EEFS_CORRUPT_FILE, EEFS_IO_ERROR. */
int32   EEFS_LibVerifyFile(EEFS_InodeTable_t *InodeTable, char *Name);

/* Fills Info from the directory entry and the resolved slot's page 0.  EEFS_SUCCESS, EEFS_FILE_NOT_FOUND,
 * EEFS_CORRUPT_FILE (Info is still filled from the directory, Type zeroed), EEFS_IO_ERROR. */
int32   EEFS_LibGetFileInfo(EEFS_InodeTable_t *InodeTable, char *Name, EEFS_NandFileInfo_t *Info);

/* A/B.  Name must not carry a suffix for these. */
int32   EEFS_LibSetActiveSlot(EEFS_InodeTable_t *InodeTable, char *Name, uint8 Slot);
int32   EEFS_LibSetActiveSlotAll(EEFS_InodeTable_t *InodeTable, uint8 Slot);             /* >= 0: entries changed */
int32   EEFS_LibSetFallbackCompatible(EEFS_InodeTable_t *InodeTable, char *Name, uint8 Flag);
int32   EEFS_LibSetFallbackCompatibleAll(EEFS_InodeTable_t *InodeTable, uint8 Flag);     /* >= 0: entries changed */
int32   EEFS_LibSetFailed(EEFS_InodeTable_t *InodeTable, char *Name);
int32   EEFS_LibClearFailed(EEFS_InodeTable_t *InodeTable, char *Name);
int32   EEFS_LibCopySlot(EEFS_InodeTable_t *InodeTable, char *Name, uint8 From, uint8 To);
int32   EEFS_LibCopySlotAll(EEFS_InodeTable_t *InodeTable, uint8 From, uint8 To);         /* >= 0 copies done, or the first error */

/* Directory maintenance */
int32   EEFS_LibRepairDirectory(EEFS_InodeTable_t *InodeTable);    /* commit + fresh CHECKPOINT in both mirrors */
int32   EEFS_LibRebuildDirectory(EEFS_InodeTable_t *InodeTable);   /* rebuild the RAM directory from the metadata pages */
int32   EEFS_LibReclaimOrphans(EEFS_InodeTable_t *InodeTable);     /* >= 0 blocks erased, or EEFS_IO_ERROR */

/* Health check (resumable; the lock is held only inside Step) */
int32   EEFS_LibHealthCheckStart(EEFS_InodeTable_t *InodeTable, EEFS_HealthReport_t *Report);
int32   EEFS_LibHealthCheckStep(EEFS_InodeTable_t *InodeTable, uint32 MaxPageReads);     /* 1 = more, EEFS_SUCCESS = done */
int32   EEFS_LibHealthCheckSummary(EEFS_InodeTable_t *InodeTable, EEFS_HealthSummary_t *Out);
int32   EEFS_LibHealthCheckCancel(EEFS_InodeTable_t *InodeTable);

#endif

/************************/
/*  End of File Comment */
/************************/
