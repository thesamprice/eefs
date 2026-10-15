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
 * Filename: eefs_nand_rebuild.c
 *
 * Purpose: Rebuilding the NAND directory from the file metadata pages (docs/nand-format.md section 6), and the
 *   directory maintenance calls built on it: EEFS_LibRebuildDirectory and EEFS_LibReclaimOrphans
 *   (docs/nand-implementation.md section 8).
 *
 * Design Notes:
 *   EEFS_NandRebuild (8.1) reads page 0 of every data block and never writes NAND.
 *
 *   Pass 1 keeps, for each (Path, slot), the candidate with the highest WriteSeq (the lower block on a tie).  A
 *   candidate is a valid metadata page (FMT 3.4) whose first block's own data matches Blocks[0].Crc32: writers program
 *   page 0 of the first block before that block's data pages, so a page 0 left by an interrupted write can be complete
 *   while its data is not (every other listed block was complete before page 0 was written).  The winners are kept in a
 *   scratch table built inside Base.Entry[] (Path, Slot[s].InUse/FirstBlock/WriteSeq), which is unused during the
 *   rebuild; a path that finds the table full is ignored.
 *
 *   Pass 2 takes the winners in descending WriteSeq order (the lower block first on a tie) and discards a winner that
 *   lists a block already claimed by an earlier, newer winner (the overlap rule): that is the superseded page 0 of a
 *   copy-on-write rewrite, Rename or SetFileAttributes whose cleanup erase was cut, or an older copy that an
 *   uncommitted rewrite superseded.  Accepted winners claim their blocks in Working; a block with a bad-block marker
 *   stays BAD and the mount's reconcile step flags the slot CROSSLINK.
 *
 *   EEFS_NandRebuild only builds the entries and the data-region map; the caller (mount step 5, or
 *   EEFS_LibRebuildDirectory) sets DirectoryRebuilt, LastSeq and the mirror state, then runs the rest of the mount
 *   (reconcile, Base, the allocation cursor and NextWriteSeq).
 *
 *   EEFS_LibReclaimOrphans is phase 6 and still returns EEFS_UNSUPPORTED_OPTION.
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
 * Local Function Prototypes
 */

static boolean  EEFS_NandFirstBlockGood(EEFS_InodeTable_t *Vol, uint32 Block, uint32 FileSize, uint32 Crc);
static int32    EEFS_NandFindRow(EEFS_InodeTable_t *Vol, const char *Path);
static boolean  EEFS_NandNextWinner(EEFS_InodeTable_t *Vol, uint32 *Row, uint8 *Slot);
static boolean  EEFS_NandClaimWinner(EEFS_InodeTable_t *Vol, uint32 Row, uint8 Slot);
static int32    EEFS_NandRebuildMounted(EEFS_InodeTable_t *Vol);

/*
 * Function Definitions
 */

/* Rebuilds the entries and the data-region map of Working from the metadata pages (section 8.1).  Reads only.
 * EEFS_SUCCESS (also when every page found was rejected: the directory is then empty), or EEFS_NOT_FORMATTED when the
 * data region holds no valid metadata page at all. */
int32 EEFS_NandRebuild(EEFS_InodeTable_t *Vol)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    EEFS_NandEntry_t           *Row;
    EEFS_NandEntry_t           *E;
    EEFS_NandSlot_t            *W;
    EEFS_NandPage0_t            P;
    const uint8                *Raw;
    uint8                       Explicit[EEFS_MAX_FILES];
    uint32                      MaxPageWriteSeq = 0;
    uint32                      Crc0;
    uint32                      b;
    uint32                      r = 0;
    uint8                       s = 0;
    boolean                     AnyPage = FALSE;
    int32                       Index;

    memset(Vol->Working.Entry, 0, sizeof(Vol->Working.Entry));
    memset(Vol->Base.Entry, 0, sizeof(Vol->Base.Entry));
    memset(Explicit, 0, sizeof(Explicit));
    for (b = Geo->DirRegionBlocks; b < Geo->BlockCount; b++) {
        if (Vol->Working.Map[b] != EEFS_NAND_MAP_BAD) {
            Vol->Working.Map[b] = EEFS_NAND_MAP_FREE;
        }
    }

    /* pass 1: the best candidate per (Path, slot), in the scratch table Base.Entry[] */
    for (b = Geo->DirRegionBlocks; b < Geo->BlockCount; b++) {
        if (Vol->Working.Map[b] == EEFS_NAND_MAP_BAD) {
            continue;
        }
        if (EEFS_NandHasBadMarker(Vol, b) == TRUE) {
            Vol->Working.Map[b] = EEFS_NAND_MAP_BAD;
            continue;
        }
        if (EEFS_NandReadPage0(Vol, b, &P, &Raw) != EEFS_SUCCESS) {
            continue;
        }
        AnyPage = TRUE;
        if (P.WriteSeq > MaxPageWriteSeq) MaxPageWriteSeq = P.WriteSeq;

        s = (P.Slot == EEFS_NAND_SLOT_B) ? EEFS_NAND_SLOT_B : EEFS_NAND_SLOT_A;
        if ((Index = EEFS_NandFindRow(Vol, P.Path)) < 0) {
            continue;                   /* EEFS_MAX_FILES paths already: this one stays an orphan */
        }
        Row = &Vol->Base.Entry[Index];
        if ((Row->Slot[s].InUse != 0) && (P.WriteSeq <= Row->Slot[s].WriteSeq)) {
            continue;                   /* an older copy, or the same WriteSeq at a higher block */
        }

        /* an interrupted write: page 0 precedes the first block's own data pages */
        EEFS_NandPage0Block(Raw, 0, NULL, &Crc0);
        if (EEFS_NandFirstBlockGood(Vol, b, P.FileSize, Crc0) == FALSE) {
            continue;
        }

        if (Row->InUse == 0) {
            Row->InUse = 1;
            strncpy(Row->Path, P.Path, EEFS_NAND_MAX_PATH_SIZE);
        }
        Row->Slot[s].InUse = 1;
        Row->Slot[s].FirstBlock = (uint16)b;
        Row->Slot[s].WriteSeq = P.WriteSeq;
        Row->Slot[s].Reserved = (uint8)(P.Slot != EEFS_NAND_SLOT_NONE);   /* scratch: written with an explicit slot */
    }
    if (AnyPage == FALSE) {
        return(EEFS_NOT_FORMATTED);
    }

    /* pass 2: the winners, newest first; one that shares a block with a newer winner is discarded (overlap rule) */
    while (EEFS_NandNextWinner(Vol, &r, &s) == TRUE) {
        if (EEFS_NandClaimWinner(Vol, r, s) == TRUE) {
            if (Vol->Base.Entry[r].Slot[s].Reserved != 0) {
                Explicit[r] = 1;
            }
        }
        Vol->Base.Entry[r].Slot[s].InUse = 0;           /* processed */
    }

    /* the entry fields: A/B from the accepted slots, the rest from the active slot's metadata page */
    for (r = 0; r < EEFS_MAX_FILES; r++) {
        E = &Vol->Working.Entry[r];
        if (E->InUse == 0) continue;
        E->IsAB = Explicit[r];
        E->ActiveSlot = (E->Slot[EEFS_NAND_SLOT_A].InUse != 0) ? EEFS_NAND_SLOT_A : EEFS_NAND_SLOT_B;
        E->FallbackCompatible = 0;
        E->ActiveFailed = 0;
        W = &E->Slot[E->ActiveSlot];
        if (EEFS_NandReadPage0(Vol, W->FirstBlock, &P, &Raw) == EEFS_SUCCESS) {
            E->FileType = P.FileType;
            E->Attributes = P.Attributes;
            E->CreationDate = P.CreationDate;
        }
    }

    Vol->Working.WriteSeqHigh = MaxPageWriteSeq;
    memcpy(&Vol->Base, &Vol->Working, sizeof(Vol->Base));
    return(EEFS_SUCCESS);

} /* End of EEFS_NandRebuild() */

/* Discards the RAM directory and rebuilds it from the metadata pages (section 8.3) */
int32 EEFS_LibRebuildDirectory(EEFS_InodeTable_t *InodeTable)
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
        ReturnCode = EEFS_NandRebuildMounted(InodeTable);
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibRebuildDirectory() */

/* Erases free blocks that still hold a valid metadata page (section 8.4).  Not implemented yet (phase 6). */
int32 EEFS_LibReclaimOrphans(EEFS_InodeTable_t *InodeTable)
{
    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibReclaimOrphans() */

/*
 * Local Functions
 */

/* TRUE when the data pages of Block hold the first block's share of a FileSize-byte file with CRC Crc (any read
 * error: FALSE).  Uses Vol->PageBuf; the read cache is not touched. */
static boolean EEFS_NandFirstBlockGood(EEFS_InodeTable_t *Vol, uint32 Block, uint32 FileSize, uint32 Crc)
{
    uint32  Count;
    uint32  Done;
    uint32  Chunk;
    uint32  Page;
    uint32  State = EEFS_CRC32_INIT;

    Count = EEFS_NandBlockDataRange(Vol->PageSize, Vol->PagesPerBlock, FileSize, 0, NULL);
    for (Done = 0, Page = 1; Done < Count; Done += Chunk, Page++) {
        Chunk = Count - Done;
        if (Chunk > Vol->PageSize) Chunk = Vol->PageSize;
        if (EEFS_NandReadPage(Vol, Block, Page, Vol->PageBuf, NULL, NULL) != EEFS_SUCCESS) {
            return(FALSE);
        }
        State = EEFS_Crc32Update(State, Vol->PageBuf, Chunk);
    }
    return((boolean)(EEFS_Crc32Final(State) == Crc));

} /* End of EEFS_NandFirstBlockGood() */

/* Row of the scratch table holding Path, or the first free row; -1 when the table is full */
static int32 EEFS_NandFindRow(EEFS_InodeTable_t *Vol, const char *Path)
{
    int32   Free = -1;
    uint32  r;

    for (r = 0; r < EEFS_MAX_FILES; r++) {
        if (Vol->Base.Entry[r].InUse == 0) {
            if (Free < 0) Free = (int32)r;
        }
        else if (strncmp(Vol->Base.Entry[r].Path, Path, EEFS_NAND_MAX_PATH_SIZE) == 0) {
            return((int32)r);
        }
    }
    return(Free);

} /* End of EEFS_NandFindRow() */

/* The unprocessed winner with the highest WriteSeq (on a tie, the lower block) */
static boolean EEFS_NandNextWinner(EEFS_InodeTable_t *Vol, uint32 *Row, uint8 *Slot)
{
    const EEFS_NandSlot_t  *S;
    const EEFS_NandSlot_t  *Best = NULL;
    uint32                  r;
    uint8                   s;

    for (r = 0; r < EEFS_MAX_FILES; r++) {
        if (Vol->Base.Entry[r].InUse == 0) continue;
        for (s = 0; s < 2; s++) {
            S = &Vol->Base.Entry[r].Slot[s];
            if (S->InUse == 0) continue;
            if ((Best == NULL) || (S->WriteSeq > Best->WriteSeq) ||
                ((S->WriteSeq == Best->WriteSeq) && (S->FirstBlock < Best->FirstBlock))) {
                Best = S;
                *Row = r;
                *Slot = s;
            }
        }
    }
    return((boolean)(Best != NULL));

} /* End of EEFS_NandNextWinner() */

/* Pass 2 for one winner: discarded (FALSE) if its page 0 no longer reads or any of its blocks is claimed by a newer
 * winner; otherwise it becomes Working.Entry[Row].Slot[Slot] and claims its blocks (TRUE). */
static boolean EEFS_NandClaimWinner(EEFS_InodeTable_t *Vol, uint32 Row, uint8 Slot)
{
    EEFS_NandEntry_t   *E = &Vol->Working.Entry[Row];
    EEFS_NandSlot_t    *W = &E->Slot[Slot];
    EEFS_NandPage0_t    P;
    const uint8        *Raw;
    uint32              First = Vol->Base.Entry[Row].Slot[Slot].FirstBlock;
    uint32              k;
    uint16              Blk;
    uint8               Owner = EEFS_NAND_MAP_OWNER(Row, Slot);

    if (EEFS_NandReadPage0(Vol, First, &P, &Raw) != EEFS_SUCCESS) {
        return(FALSE);
    }
    for (k = 0; k < P.BlockCount; k++) {
        EEFS_NandPage0Block(Raw, k, &Blk, NULL);
        if (EEFS_NAND_MAP_IS_OWNER(Vol->Working.Map[Blk])) {
            return(FALSE);              /* the overlap rule: a newer copy owns this block */
        }
    }
    for (k = 0; k < P.BlockCount; k++) {
        EEFS_NandPage0Block(Raw, k, &Blk, NULL);
        if (Vol->Working.Map[Blk] == EEFS_NAND_MAP_FREE) {
            Vol->Working.Map[Blk] = Owner;
        }
    }

    E->InUse = 1;
    strncpy(E->Path, P.Path, EEFS_NAND_MAX_PATH_SIZE);
    memset(W, 0, sizeof(*W));
    W->InUse = 1;
    W->FirstBlock = (uint16)First;
    W->AllocBlocks = (uint16)P.BlockCount;
    W->FileSize = P.FileSize;
    W->Version = P.Version;
    W->WriteSeq = P.WriteSeq;
    W->FileCrc32 = P.FileCrc32;
    W->ModificationDate = P.ModificationDate;
    return(TRUE);

} /* End of EEFS_NandClaimWinner() */

/* EEFS_LibRebuildDirectory on an idle mounted volume: rebuild the RAM directory, keep the directory-region map and the
 * mirror rings, and make the next commit write a CHECKPOINT of the rebuilt state above every Seq on the media.  If the
 * rebuild finds nothing, the volume is mounted again from its directory (nothing was written). */
static int32 EEFS_NandRebuildMounted(EEFS_InodeTable_t *Vol)
{
    EEFS_NandChain_t        C[2];
    EEFS_NandDevice_t      *Dev;
    EEFS_HealthSummary_t    Report;
    uint32                  High;
    uint32                  NextWriteSeq = Vol->NextWriteSeq;
    uint8                   m;
    int32                   Status;

    if (Vol->PendingCleanupBase > 0) {
        EEFS_NandRunPendingCleanup(Vol);        /* committed removals must not come back */
    }
    EEFS_NandScanMirror(Vol, 0, &C[0]);
    EEFS_NandScanMirror(Vol, 1, &C[1]);

    memset(Vol->ForceDirty, 0, sizeof(Vol->ForceDirty));
    memset(Vol->SlotFlags, 0, sizeof(Vol->SlotFlags));
    Vol->PendingCleanupCount = 0;
    Vol->PendingCleanupBase = 0;
    EEFS_NandInvalidateCache(Vol);

    Status = EEFS_NandRebuild(Vol);
    if (Status != EEFS_SUCCESS) {
        Dev = Vol->Dev;
        memcpy(&Report, &Vol->LastReport, sizeof(Report));
        memset(Vol, 0, sizeof(*Vol));
        Vol->Dev = Dev;
        if ((EEFS_NandCheckGeometry(Vol) != EEFS_SUCCESS) || (EEFS_NandMount(Vol) != EEFS_SUCCESS)) {
            memset(Vol, 0, sizeof(*Vol));       /* left unmounted */
        }
        else {
            memcpy(&Vol->LastReport, &Report, sizeof(Report));
        }
        return(Status);
    }

    High = Vol->LastSeq;
    if (C[0].MaxSeqSeen > High) High = C[0].MaxSeqSeen;
    if (C[1].MaxSeqSeen > High) High = C[1].MaxSeqSeen;
    Vol->LastSeq = High + 1;
    Vol->DirectoryRebuilt = TRUE;
    for (m = 0; m < 2; m++) {
        if (Vol->Mirror[m].Disabled == FALSE) {
            Vol->Mirror[m].NeedsResync = TRUE;      /* a CHECKPOINT in a new block, keeping the current one */
            Vol->Mirror[m].EnterNewBlock = TRUE;
        }
    }

    EEFS_NandMountFinish(Vol);
    if (NextWriteSeq > Vol->NextWriteSeq) {
        Vol->NextWriteSeq = NextWriteSeq;
    }
    EEFS_NandTouch(Vol);
    return(EEFS_SUCCESS);

} /* End of EEFS_NandRebuildMounted() */

/************************/
/*  End of File Comment */
/************************/
