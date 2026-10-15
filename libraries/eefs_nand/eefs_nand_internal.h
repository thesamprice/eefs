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
 * Filename: eefs_nand_internal.h
 *
 * Purpose: Internal interfaces shared by the NAND backend's source files (and included by the host tests).  Not part
 *   of the public API.  Names and semantics are fixed by docs/nand-implementation.md section 3; the phase that
 *   implements each group is noted below.  Until then the functions are declared only.
 *
 */

#ifndef _eefs_nand_internal_
#define _eefs_nand_internal_

/*
 * Includes
 */

#include "common_types.h"
#include "eefs_fileapi.h"
#include "eefs_nand.h"
#include "eefs_nand_format.h"

/*
 * Exported Data
 */

/* The descriptor table shared by all volumes (eefs_nand_fileapi.c); Abort checks it for slots of the batch */
extern EEFS_FileDescriptor_t    EEFS_FileDescriptorTable[EEFS_MAX_OPEN_FILES];

/*
 * Exported Functions
 */

/* I/O (eefs_nand_io.c, phase 2), section 3.1 */

/* Uncached read; Data/Oob may be NULL.  EEFS_SUCCESS or EEFS_IO_ERROR (UNCORRECTABLE and IO_ERROR both map here;
 * *Corrected = 0xFFFFFFFF for uncorrectable). */
int32           EEFS_NandReadPage(EEFS_InodeTable_t *Vol, uint32 Block, uint32 Page, uint8 *Data, uint8 *Oob, uint32 *Corrected);

/* Returns Vol->CacheBuf (valid until the next NAND operation) or NULL with *Status = EEFS_IO_ERROR */
const uint8    *EEFS_NandReadCached(EEFS_InodeTable_t *Vol, uint32 Block, uint32 Page, int32 *Status);
void            EEFS_NandInvalidateCache(EEFS_InodeTable_t *Vol);

/* Programs Data (Oob NULL); with EEFS_NAND_VERIFY_WRITES reads back into Vol->PageBuf and compares.  EEFS_SUCCESS or
 * EEFS_IO_ERROR.  Invalidates the cache. */
int32           EEFS_NandProgramPage(EEFS_InodeTable_t *Vol, uint32 Block, uint32 Page, const uint8 *Data);

/* As EEFS_NandProgramPage, but the read-back goes into Scratch (PageSize bytes) */
int32           EEFS_NandProgramPageVerified(EEFS_InodeTable_t *Vol, uint32 Block, uint32 Page, const uint8 *Data, uint8 *Scratch);

/* EEFS_SUCCESS or EEFS_IO_ERROR; invalidates the cache */
int32           EEFS_NandEraseBlock(EEFS_InodeTable_t *Vol, uint32 Block);

/* Working.Map[Block] = BAD; best-effort OOB marker on the lowest BadMarkerPages page; removes Block from any mirror
 * ring (sets Disabled when empty); VolumeChangeCounter++ */
void            EEFS_NandMarkBad(EEFS_InodeTable_t *Vol, uint32 Block);

/* Reads the OOB of every BadMarkerPages page (Data NULL); TRUE if any byte at BadMarkerOffset != 0xFF or a read fails */
boolean         EEFS_NandHasBadMarker(EEFS_InodeTable_t *Vol, uint32 Block);

/* TRUE if the data area of (Block, Page) reads as all 0xFF (uncached, into Vol->PageBuf); a read error counts as not
 * erased */
boolean         EEFS_NandPageIsErased(EEFS_InodeTable_t *Vol, uint32 Block, uint32 Page);

/* Cached read + EEFS_NandDecodePage0(ExpectBlock = Block) + bounds check of every Blocks[i] (>= DirRegionBlocks,
 * < BlockCount).  EEFS_SUCCESS / EEFS_CORRUPT_FILE / EEFS_IO_ERROR.  *Raw = cache pointer for Blocks[] access. */
int32           EEFS_NandReadPage0(EEFS_InodeTable_t *Vol, uint32 Block, EEFS_NandPage0_t *P, const uint8 **Raw);

/* Journal (eefs_nand_journal.c, phase 2), section 3.2 */

/* Bytes of an entry bit mask (EntryMask, ForceDirty) */
#define EEFS_NAND_MASK_BYTES            ((EEFS_MAX_FILES + 7) / 8)
#define EEFS_NAND_MASK_TEST(m, i)       (((m)[(i) / 8] & (uint8)(1u << ((i) % 8))) != 0)
#define EEFS_NAND_MASK_SET(m, i)        ((m)[(i) / 8] |= (uint8)(1u << ((i) % 8)))

/* Section 5.1 step 1: checks Vol->Dev (ops and geometry) and fills the cached sizes of Vol.  Uses Vol->Base as
 * scratch (for MaxCheckpointPages), so it is called on a volume that is about to be (re)initialized.
 * EEFS_SUCCESS or EEFS_INVALID_ARGUMENT. */
int32           EEFS_NandCheckGeometry(EEFS_InodeTable_t *Vol);

/* Pages of a CHECKPOINT with every entry in use and the full map (uses Vol->Base as scratch) */
uint32          EEFS_NandMaxCheckpointPages(EEFS_InodeTable_t *Vol);

/* Section 5.1 steps 2-6 on a volume prepared by EEFS_NandCheckGeometry */
int32           EEFS_NandFormat(EEFS_InodeTable_t *Vol);

/* Section 5.2 steps 2-9 on a volume prepared by EEFS_NandCheckGeometry.  EEFS_SUCCESS, EEFS_GEOMETRY_MISMATCH,
 * EEFS_NOT_FORMATTED, EEFS_IO_ERROR. */
int32           EEFS_NandMount(EEFS_InodeTable_t *Vol);

/* TRUE if Working differs from Base (entries or map), or, with IncludeForceDirty, a ForceDirty bit is set */
boolean         EEFS_NandHasUncommitted(EEFS_InodeTable_t *Vol, boolean IncludeForceDirty);

int32           EEFS_NandBuildRings(EEFS_InodeTable_t *Vol);
int32           EEFS_NandScanMirror(EEFS_InodeTable_t *Vol, uint8 M, EEFS_NandChain_t *C);
int32           EEFS_NandApplyChain(EEFS_InodeTable_t *Vol, uint8 M, const EEFS_NandChain_t *C, EEFS_NandDirState_t *Dst,
                                    uint16 *Cleanup, uint16 *CleanupCount, uint32 *MaxFilesSeen);
int32           EEFS_NandWriteRecord(EEFS_InodeTable_t *Vol, uint8 M, uint8 RecType, uint32 Seq, const EEFS_NandDirState_t *State,
                                     const EEFS_NandDirState_t *Base, const uint8 *EntryMask,
                                     const uint16 *Cleanup, uint16 CleanupCount);
uint32          EEFS_NandRecordPages(EEFS_InodeTable_t *Vol, uint8 RecType, const EEFS_NandDirState_t *State,
                                     const EEFS_NandDirState_t *Base, const uint8 *EntryMask, uint16 CleanupCount);
int32           EEFS_NandEnterBlock(EEFS_InodeTable_t *Vol, uint8 M);
int32           EEFS_NandWriteRecordToMirror(EEFS_InodeTable_t *Vol, uint8 M, uint32 Seq, boolean ForceCheckpoint,
                                             const uint8 *EntryMask, const uint16 *Cleanup, uint16 CleanupCount);
int32           EEFS_NandResync(EEFS_InodeTable_t *Vol);
int32           EEFS_NandCommit(EEFS_InodeTable_t *Vol, boolean ForceCheckpointBoth);
int32           EEFS_NandAbort(EEFS_InodeTable_t *Vol);
int32           EEFS_NandRunPendingCleanup(EEFS_InodeTable_t *Vol);
int32           EEFS_NandAddCleanup(EEFS_InodeTable_t *Vol, uint16 Block);
boolean         EEFS_NandCleanupHasRoom(EEFS_InodeTable_t *Vol, uint32 Needed);
void            EEFS_NandForgetCleanup(EEFS_InodeTable_t *Vol, uint16 Block);
void            EEFS_NandDiff(EEFS_InodeTable_t *Vol, uint8 *EntryMask, uint32 *MapRanges);

/* Allocation and lookup (eefs_nand_fileapi.c, phase 3), section 3.3 */

int32           EEFS_NandParseName(const char *Name, char *Path, uint8 *SlotSpec);
int32           EEFS_NandFindEntry(EEFS_InodeTable_t *Vol, const char *Path);
int32           EEFS_NandAllocEntry(EEFS_InodeTable_t *Vol);
#define EEFS_NAND_ALLOC_NEW             0   /* new file, CopySlot: must leave >= EEFS_NAND_RESERVE_BLOCKS free */
#define EEFS_NAND_ALLOC_MODIFY          1   /* copy-on-write modify, Rename, SetAttr: must leave >= EEFS_NAND_BADBLOCK_RESERVE free */
#define EEFS_NAND_ALLOC_REPLACE         2   /* replacement for a grown bad block: may take the last free block */
uint32          EEFS_NandFreeCount(EEFS_InodeTable_t *Vol);     /* Working.Map bytes == FREE in [DirRegionBlocks, BlockCount) */
int32           EEFS_NandAllocBlocks(EEFS_InodeTable_t *Vol, uint32 Count, uint16 *Out, uint8 Owner, uint8 Class);

/* Phase 2 implements the map/cleanup helpers below (ReleaseBlocks, FreeSlot, FreeBlocksExcept, Touch) */

/* Back to FREE (a BAD block stays BAD) */
void            EEFS_NandReleaseBlocks(EEFS_InodeTable_t *Vol, const uint16 *Blocks, uint32 Count);

/* Every Working.Map byte == OWNER(Entry, Slot) -> FREEING; adds FirstBlock to the cleanup list (the caller checked
 * room); zeroes the slot record */
void            EEFS_NandFreeSlot(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 Slot);

/* Every Working.Map byte == OWNER(Entry, Slot) whose block is not in Keep[] -> FREEING (copy-on-write).  Keep[] must
 * list blocks owned by the slot. */
void            EEFS_NandFreeBlocksExcept(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 Slot, const uint16 *Keep, uint32 KeepCount);

uint32          EEFS_NandBlocksNeeded(EEFS_InodeTable_t *Vol, uint32 Size);
boolean         EEFS_NandBlockUnchanged(EEFS_InodeTable_t *Vol, uint16 OldBlock, uint32 K, uint32 OldCrc, uint32 OldLen,
                                        const uint8 *NewSlice, uint32 NewLen);
int32           EEFS_NandValidateSlot(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 Slot, boolean Full);
/* The flags of a failed validation: SLOTFLAG_CORRUPT and, for the active copy of an A/B entry, ActiveFailed in Working
 * (and in Base when Base holds the same slot record) with ForceDirty; Touch when anything changed */
void            EEFS_NandSlotFailed(EEFS_InodeTable_t *Vol, uint32 Entry, uint8 Slot);
int32           EEFS_NandWriteCopy(EEFS_InodeTable_t *Vol, uint32 SrcEntry, uint8 SrcSlot, EEFS_NandPage0_t *NewP0, uint16 *DstBlocks);
void            EEFS_NandTouch(EEFS_InodeTable_t *Vol);

/* Rebuild (eefs_nand_rebuild.c, phase 4), section 3.4 */

int32           EEFS_NandRebuild(EEFS_InodeTable_t *Vol);
void            EEFS_NandMountFinish(EEFS_InodeTable_t *Vol);   /* mount steps 6-8 (journal.c), shared with RebuildDirectory */
int32           EEFS_NandReclaimOrphans(EEFS_InodeTable_t *Vol);

#endif

/************************/
/*  End of File Comment */
/************************/
