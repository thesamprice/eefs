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
 * Filename: eefs_nand_journal.c
 *
 * Purpose: Directory journal of the NAND backend: format, directory scan and replay, commit, abort, resync, pending
 *   cleanup and the mirror rings (docs/nand-implementation.md sections 3.2 and 5).
 *
 * Design Notes:
 *   The directory is a log of records (docs/nand-format.md section 4) written to two mirrors.  Each mirror is a ring
 *   of good blocks; the first record of a block is always a CHECKPOINT (the whole state), later records in the same
 *   block are TXNs (changed map ranges and entries).  A commit writes the same Seq to mirror 0, then mirror 1, then
 *   erases the blocks on the cleanup list.  A mirror that lags (cut between the mirrors, failed write) is brought up
 *   to date with a CHECKPOINT of the committed state before the next record; a mirror whose current block ends in a
 *   torn page writes its next record into a freshly erased block.
 *
 *   Records are serialized one page at a time in Vol->PageBuf by a packer that runs twice: a dry run that counts the
 *   pages (so every page header carries the exact PageCount) and the programming run.  Every programmed page is read
 *   back into Vol->CacheBuf and compared.
 *
 *   Reading the directory (mount) is two passes per mirror: EEFS_NandScanMirror validates the chains without applying
 *   anything, EEFS_NandApplyChain re-reads the chosen chain and applies its items.
 *
 */

/*
 * Includes
 */

#include "common_types.h"
#include "eefs_fileapi.h"
#include "eefs_macros.h"
#include "eefs_nand_internal.h"
#include <string.h>

/*
 * Macro Definitions
 */

#define EEFS_NAND_MAP_MERGE_GAP         16      /* TXN map ranges merge across fewer than this many unchanged bytes */
#define EEFS_NAND_RING_SIZE             (EEFS_NAND_MAX_DIR_BLOCKS / 2)

/*
 * Type Definitions
 */

/* Record serializer state (one page at a time in Vol->PageBuf) */
typedef struct
{
    EEFS_InodeTable_t  *Vol;
    boolean             Program;        /* FALSE: dry run, count pages only */
    uint8               M;
    uint8               RecType;
    uint32              Seq;
    uint32              PageCount;      /* program run: the page count announced in every header */
    uint32              PageIndex;
    uint32              Used;           /* payload bytes used in the current page */
    uint32              Block;          /* program run: destination block and first page */
    uint32              FirstPage;
} EEFS_NandPacker_t;

/*
 * Local Function Prototypes
 */

static void     EEFS_NandPackBegin(EEFS_NandPacker_t *P);
static int32    EEFS_NandPackFlush(EEFS_NandPacker_t *P);
static int32    EEFS_NandPackNextPage(EEFS_NandPacker_t *P);
static int32    EEFS_NandPackRoom(EEFS_NandPacker_t *P, uint32 Size);
static int32    EEFS_NandPackMap(EEFS_NandPacker_t *P, const EEFS_NandDirState_t *State, uint32 Start, uint32 End);
static int32    EEFS_NandPackRecord(EEFS_NandPacker_t *P, const EEFS_NandDirState_t *State,
                                    const EEFS_NandDirState_t *Base, const uint8 *EntryMask,
                                    const uint16 *Cleanup, uint16 CleanupCount);
static int32    EEFS_NandWriteToMirror(EEFS_InodeTable_t *Vol, uint8 M, uint32 Seq, boolean ForceCheckpoint,
                                       const EEFS_NandDirState_t *State, const EEFS_NandDirState_t *Base,
                                       const uint8 *EntryMask, const uint16 *Cleanup, uint16 CleanupCount);
static boolean  EEFS_NandWalkChain(EEFS_InodeTable_t *Vol, uint32 Block, uint32 *CheckpointSeq, uint32 *LastSeq,
                                   uint32 *NextPage, uint32 *MaxSeqSeen);
static void     EEFS_NandUpdateResyncFlags(EEFS_InodeTable_t *Vol);
static boolean  EEFS_NandCleanupErase(EEFS_InodeTable_t *Vol, uint16 Block);
static void     EEFS_NandReconcile(EEFS_InodeTable_t *Vol, uint32 *MaxPageWriteSeq);

/*
 * Function Definitions
 */

/* Geometry and ops of Vol->Dev (section 5.1 step 1); fills the cached sizes */
int32 EEFS_NandCheckGeometry(EEFS_InodeTable_t *Vol)
{
    const EEFS_NandDevice_t    *Dev = Vol->Dev;
    const EEFS_NandGeometry_t  *Geo;
    uint32                      Ckpt;

    if ((Dev == NULL) || (Dev->Ops == NULL) || (Dev->Ops->ReadPage == NULL) ||
        (Dev->Ops->ProgramPage == NULL) || (Dev->Ops->EraseBlock == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    Geo = &Dev->Geometry;

    if ((Geo->PageSize < EEFS_NAND_MIN_PAGE_SIZE) || (Geo->PageSize > EEFS_NAND_MAX_PAGE_SIZE) ||
        ((Geo->PageSize & (Geo->PageSize - 1)) != 0) ||
        (Geo->OobSize < 1) || (Geo->OobSize > EEFS_NAND_MAX_OOB_SIZE) ||
        ((Geo->DirRegionBlocks % 2) != 0) || (Geo->DirRegionBlocks < 4) ||
        (Geo->DirRegionBlocks > EEFS_NAND_MAX_DIR_BLOCKS) ||
        (Geo->BlockCount < (Geo->DirRegionBlocks + 1)) || (Geo->BlockCount > EEFS_NAND_MAX_BLOCKS) ||
        (Geo->BadMarkerOffset >= Geo->OobSize) ||
        (Geo->BadMarkerPages == 0) ||
        ((Geo->BadMarkerPages & ~(uint32)(EEFS_NAND_BADMARKER_PAGE0 | EEFS_NAND_BADMARKER_PAGE1 | EEFS_NAND_BADMARKER_LAST)) != 0) ||
        (Geo->PagesPerBlock < 4) || (Geo->PagesPerBlock > 0xFFFF) ||
        (Geo->PagesPerBlock > (0xFFFFFFFFUL / Geo->PageSize))) {
        return(EEFS_INVALID_ARGUMENT);
    }

    Vol->PageSize = (uint16)Geo->PageSize;
    Vol->PagesPerBlock = (uint16)Geo->PagesPerBlock;
    Vol->BlockBytes = Geo->PageSize * Geo->PagesPerBlock;
    Vol->MaxBlocksPerFile = EEFS_NandMaxBlocksPerFile(Geo->PageSize);

    Ckpt = EEFS_NandMaxCheckpointPages(Vol);
    if ((Ckpt == 0) || (Geo->PagesPerBlock < (2 * (Ckpt + 1)))) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_SUCCESS);

} /* End of EEFS_NandCheckGeometry() */

/* Pages of the largest CHECKPOINT this configuration can produce (every entry in use, the full map) */
uint32 EEFS_NandMaxCheckpointPages(EEFS_InodeTable_t *Vol)
{
    uint32  i;
    uint32  Pages;

    memset(&Vol->Base, 0, sizeof(Vol->Base));
    memset(Vol->Base.Map, EEFS_NAND_MAP_FREE, sizeof(Vol->Base.Map));
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        Vol->Base.Entry[i].InUse = 1;
        Vol->Base.Entry[i].Path[0] = '/';
    }
    Pages = EEFS_NandRecordPages(Vol, EEFS_NAND_REC_CHECKPOINT, &Vol->Base, NULL, NULL, 0);
    memset(&Vol->Base, 0, sizeof(Vol->Base));
    return(Pages);

} /* End of EEFS_NandMaxCheckpointPages() */

/* Section 5.1 steps 2-6 */
int32 EEFS_NandFormat(EEFS_InodeTable_t *Vol)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      b;
    uint8                       m;
    uint32                      Written = 0;

    /* map, and the bad-block markers of every block before anything is erased */
    for (b = 0; b < Geo->BlockCount; b++) {
        Vol->Working.Map[b] = (b < Geo->DirRegionBlocks) ? EEFS_NAND_MAP_DIR : EEFS_NAND_MAP_FREE;
    }
    for (b = 0; b < Geo->BlockCount; b++) {
        if (EEFS_NandHasBadMarker(Vol, b) == TRUE) {
            Vol->Working.Map[b] = EEFS_NAND_MAP_BAD;
        }
    }

    /* erase every good block: no stale directory record or metadata page survives */
    for (b = 0; b < Geo->BlockCount; b++) {
        if (Vol->Working.Map[b] != EEFS_NAND_MAP_BAD) {
            if (EEFS_NandEraseBlock(Vol, b) != EEFS_SUCCESS) {
                EEFS_NandMarkBad(Vol, b);
            }
        }
    }

    EEFS_NandBuildRings(Vol);
    if ((Vol->Mirror[0].Count < 2) || (Vol->Mirror[1].Count < 2)) {
        return(EEFS_IO_ERROR);
    }

    /* the empty state */
    memset(Vol->Working.Entry, 0, sizeof(Vol->Working.Entry));
    Vol->Working.WriteSeqHigh = 0;
    Vol->FormatTime = (uint32)EEFS_LIB_TIME;
    Vol->LastSeq = 1;
    Vol->NextWriteSeq = 1;
    Vol->PendingCleanupCount = 0;
    Vol->PendingCleanupBase = 0;
    memcpy(&Vol->Base, &Vol->Working, sizeof(Vol->Base));
    for (m = 0; m < 2; m++) {
        Vol->Mirror[m].CurIndex = 0;
        Vol->Mirror[m].NextPage = 0;
        Vol->Mirror[m].HasState = FALSE;
        Vol->Mirror[m].EnterNewBlock = FALSE;
        Vol->Mirror[m].NeedsResync = FALSE;
    }

    /* CHECKPOINT Seq 1 into the first block of each ring (already erased) */
    for (m = 0; m < 2; m++) {
        if (EEFS_NandWriteRecordToMirror(Vol, m, 1, TRUE, NULL, NULL, 0) == EEFS_SUCCESS) {
            Written++;
        }
    }
    return((Written > 0) ? EEFS_SUCCESS : EEFS_IO_ERROR);

} /* End of EEFS_NandFormat() */

/* Section 5.2 steps 2-9 */
int32 EEFS_NandMount(EEFS_InodeTable_t *Vol)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    EEFS_NandChain_t            Chain[2];
    EEFS_NandMirror_t          *Mir;
    uint8                       DirBad[EEFS_NAND_MAX_DIR_BLOCKS];
    uint32                      b;
    uint8                       m;
    uint8                       Best;
    uint16                      CleanupCount = 0;
    uint32                      MaxFiles = 0;
    uint32                      MaxPageWriteSeq = 0;
    uint32                      High;
    int32                       Status;

    /* step 2: map skeleton, directory bad blocks, rings */
    memset(Vol->Working.Map, EEFS_NAND_MAP_FREE, sizeof(Vol->Working.Map));
    for (b = 0; b < Geo->DirRegionBlocks; b++) {
        DirBad[b] = (uint8)EEFS_NandHasBadMarker(Vol, b);
        Vol->Working.Map[b] = DirBad[b] ? EEFS_NAND_MAP_BAD : EEFS_NAND_MAP_DIR;
    }
    EEFS_NandBuildRings(Vol);

    /* step 3: scan both mirrors, take the longer chain (tie: mirror 0) */
    EEFS_NandScanMirror(Vol, 0, &Chain[0]);
    EEFS_NandScanMirror(Vol, 1, &Chain[1]);
    if ((Chain[0].Valid == FALSE) && (Chain[1].Valid == FALSE)) {
        /* step 5 (no directory): the rebuild from the metadata pages is phase 4 */
        return(EEFS_NOT_FORMATTED);
    }
    Best = ((Chain[1].Valid == TRUE) && ((Chain[0].Valid == FALSE) || (Chain[1].LastSeq > Chain[0].LastSeq))) ? 1 : 0;

    /* step 4: replay */
    Status = EEFS_NandApplyChain(Vol, Best, &Chain[Best], &Vol->Working, Vol->PendingCleanup, &CleanupCount, &MaxFiles);
    if (Status != EEFS_SUCCESS) {
        return(Status);
    }
    for (b = 0; b < Geo->DirRegionBlocks; b++) {
        if (DirBad[b]) {
            Vol->Working.Map[b] = EEFS_NAND_MAP_BAD;
        }
    }
    Vol->LastSeq = Chain[Best].LastSeq;
    Vol->PendingCleanupCount = CleanupCount;
    Vol->PendingCleanupBase = CleanupCount;
    for (m = 0; m < 2; m++) {
        Mir = &Vol->Mirror[m];
        Mir->HasState = Chain[m].Valid;
        if (Chain[m].Valid) {
            Mir->CurIndex = Chain[m].BlockIdx;
            Mir->NextPage = Chain[m].NextPage;
            Mir->CheckpointSeq = Chain[m].CheckpointSeq;
            Mir->LastSeq = Chain[m].LastSeq;
        }
        else {
            Mir->CurIndex = 0;
            Mir->NextPage = 0;
            Mir->CheckpointSeq = 0;
            Mir->LastSeq = 0;
        }
        Mir->NeedsResync = (uint8)((Chain[m].Valid == FALSE) || (Chain[m].LastSeq < Vol->LastSeq));
        Mir->EnterNewBlock = (uint8)(Chain[m].Torn || Mir->NeedsResync);
    }

    /* step 6: reconcile the slots with their metadata pages */
    EEFS_NandReconcile(Vol, &MaxPageWriteSeq);

    /* step 7 */
    memcpy(&Vol->Base, &Vol->Working, sizeof(Vol->Base));
    Vol->AllocCursor = Geo->DirRegionBlocks;    /* round-robin cursor (6.9): after the highest owned block */
    for (b = Geo->BlockCount; b > Geo->DirRegionBlocks; b--) {
        if (EEFS_NAND_MAP_IS_OWNER(Vol->Working.Map[b - 1])) {
            Vol->AllocCursor = (b < Geo->BlockCount) ? b : Geo->DirRegionBlocks;
            break;
        }
    }

    /* step 8 */
    High = Vol->Working.WriteSeqHigh;
    if (MaxPageWriteSeq > High) High = MaxPageWriteSeq;
    Vol->NextWriteSeq = High + 1 + EEFS_NAND_WRITESEQ_MOUNT_GAP;

    /* step 9 */
    Vol->Mounted = TRUE;
    Vol->VolumeChangeCounter = 1;
    return(EEFS_SUCCESS);

} /* End of EEFS_NandMount() */

/* Mirror rings: the good directory blocks of each half, ascending */
int32 EEFS_NandBuildRings(EEFS_InodeTable_t *Vol)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      Half = Geo->DirRegionBlocks / 2;
    EEFS_NandMirror_t          *Mir;
    uint32                      m;
    uint32                      b;

    for (m = 0; m < 2; m++) {
        Mir = &Vol->Mirror[m];
        Mir->Count = 0;
        for (b = m * Half; b < (m + 1) * Half; b++) {
            if (Vol->Working.Map[b] != EEFS_NAND_MAP_BAD) {
                Mir->Blocks[Mir->Count++] = (uint16)b;
            }
        }
        Mir->Disabled = (uint8)(Mir->Count == 0);
        if (Mir->CurIndex >= Mir->Count) {
            Mir->CurIndex = 0;
        }
    }
    return(EEFS_SUCCESS);

} /* End of EEFS_NandBuildRings() */

/* Finds the current chain of one mirror (section 5.3).  Reads only. */
int32 EEFS_NandScanMirror(EEFS_InodeTable_t *Vol, uint8 M, EEFS_NandChain_t *C)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    EEFS_NandMirror_t          *Mir = &Vol->Mirror[M];
    EEFS_NandRecHdr_t           H;
    uint16                      CandIdx[EEFS_NAND_RING_SIZE];
    uint32                      CandSeq[EEFS_NAND_RING_SIZE];
    uint32                      Cands = 0;
    uint32                      i;
    uint32                      j;
    uint16                      TmpIdx;
    uint32                      TmpSeq;
    uint32                      Ckpt;
    uint32                      Last;
    uint32                      Next;
    boolean                     Found = FALSE;
    boolean                     TornCandidate = FALSE;

    memset(C, 0, sizeof(*C));

    /* candidates: ring blocks whose page 0 is the first page of a CHECKPOINT */
    for (i = 0; i < Mir->Count; i++) {
        if (EEFS_NandReadPage(Vol, Mir->Blocks[i], 0, Vol->PageBuf, NULL, NULL) != EEFS_SUCCESS) continue;
        if (EEFS_NandDecodeRecordPage(Vol->PageBuf, Geo->PageSize, &H) != EEFS_SUCCESS) continue;
        if (H.Seq > C->MaxSeqSeen) C->MaxSeqSeen = H.Seq;
        if ((H.RecType != EEFS_NAND_REC_CHECKPOINT) || (H.PageIndex != 0)) continue;
        CandIdx[Cands] = (uint16)i;
        CandSeq[Cands] = H.Seq;
        Cands++;
    }

    /* newest first */
    for (i = 1; i < Cands; i++) {
        TmpIdx = CandIdx[i];
        TmpSeq = CandSeq[i];
        for (j = i; (j > 0) && (CandSeq[j - 1] < TmpSeq); j--) {
            CandIdx[j] = CandIdx[j - 1];
            CandSeq[j] = CandSeq[j - 1];
        }
        CandIdx[j] = TmpIdx;
        CandSeq[j] = TmpSeq;
    }

    /* the first candidate with a valid checkpoint is the current block.  Candidates with the same checkpoint Seq are
     * all walked and the longest chain wins (two blocks can carry the same checkpoint Seq only after a resync). */
    for (i = 0; i < Cands; i++) {
        if ((Found == TRUE) && (CandSeq[i] < C->CheckpointSeq)) {
            break;
        }
        if (EEFS_NandWalkChain(Vol, Mir->Blocks[CandIdx[i]], &Ckpt, &Last, &Next, &C->MaxSeqSeen) == FALSE) {
            TornCandidate = TRUE;       /* interrupted while entering this block */
            continue;
        }
        if ((Found == FALSE) || (Last > C->LastSeq)) {
            Found = TRUE;
            C->BlockIdx = CandIdx[i];
            C->CheckpointSeq = Ckpt;
            C->LastSeq = Last;
            C->NextPage = (uint16)Next;
        }
    }
    if (Found == FALSE) {
        C->Valid = FALSE;
        return(EEFS_SUCCESS);
    }

    C->Valid = TRUE;
    C->Torn = (uint8)TornCandidate;
    if ((C->NextPage < Geo->PagesPerBlock) &&
        (EEFS_NandPageIsErased(Vol, Mir->Blocks[C->BlockIdx], C->NextPage) == FALSE)) {
        C->Torn = TRUE;
    }
    return(EEFS_SUCCESS);

} /* End of EEFS_NandScanMirror() */

/* Second pass: re-reads the chain and applies its items (FMT 5 step 5) */
int32 EEFS_NandApplyChain(EEFS_InodeTable_t *Vol, uint8 M, const EEFS_NandChain_t *C, EEFS_NandDirState_t *Dst,
                          uint16 *Cleanup, uint16 *CleanupCount, uint32 *MaxFilesSeen)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      Block = Vol->Mirror[M].Blocks[C->BlockIdx];
    EEFS_NandRecHdr_t           H;
    EEFS_NandVolumeItem_t       V;
    EEFS_NandEntry_t            E;
    uint32                      p = 0;
    uint32                      q;
    uint32                      Count;
    uint32                      Cursor;
    uint8                       Type;
    const uint8                *Body;
    uint16                      Len;
    uint16                      Index;
    uint32                      Start;
    uint32                      N;
    uint32                      k;
    int32                       Status;

    *CleanupCount = 0;
    *MaxFilesSeen = 0;

    while (p < C->NextPage) {
        if ((EEFS_NandReadPage(Vol, Block, p, Vol->PageBuf, NULL, NULL) != EEFS_SUCCESS) ||
            (EEFS_NandDecodeRecordPage(Vol->PageBuf, Geo->PageSize, &H) != EEFS_SUCCESS)) {
            return(EEFS_IO_ERROR);
        }
        Count = H.PageCount;
        if (H.RecType == EEFS_NAND_REC_CHECKPOINT) {
            memset(Dst->Entry, 0, sizeof(Dst->Entry));
            memset(Dst->Map, EEFS_NAND_MAP_FREE, sizeof(Dst->Map));
            Dst->WriteSeqHigh = 0;
        }
        *CleanupCount = 0;      /* the list of the newest record replaces the earlier ones */

        for (q = 0; q < Count; q++) {
            if (q > 0) {
                if ((EEFS_NandReadPage(Vol, Block, p + q, Vol->PageBuf, NULL, NULL) != EEFS_SUCCESS) ||
                    (EEFS_NandDecodeRecordPage(Vol->PageBuf, Geo->PageSize, &H) != EEFS_SUCCESS)) {
                    return(EEFS_IO_ERROR);
                }
            }
            Cursor = 0;
            while ((Status = EEFS_NandNextItem(&Vol->PageBuf[EEFS_NAND_REC_HDR_SIZE], H.PayloadLen, &Cursor,
                                               &Type, &Body, &Len)) == 1) {
                switch (Type) {
                    case EEFS_NAND_ITEM_VOLUME:
                        if (EEFS_NandDecodeVolumeItem(Body, Len, &V) != EEFS_SUCCESS) {
                            return(EEFS_IO_ERROR);
                        }
                        if ((V.PageSize != Geo->PageSize) || (V.PagesPerBlock != Geo->PagesPerBlock) ||
                            (V.BlockCount != Geo->BlockCount) || (V.DirRegionBlocks != Geo->DirRegionBlocks) ||
                            (V.PathSize != EEFS_NAND_MAX_PATH_SIZE) || (V.CfsNameSize != EEFS_NAND_CFS_NAME_SIZE) ||
                            (V.CfsSymbolSize != EEFS_NAND_CFS_SYMBOL_SIZE) || (V.MaxFiles > EEFS_MAX_FILES)) {
                            return(EEFS_GEOMETRY_MISMATCH);
                        }
                        Dst->WriteSeqHigh = V.WriteSeqHigh;
                        Vol->FormatTime = V.FormatTime;
                        if (V.MaxFiles > *MaxFilesSeen) *MaxFilesSeen = V.MaxFiles;
                        break;
                    case EEFS_NAND_ITEM_MAPCHUNK:
                        Start = EEFS_NandGet32(&Body[EEFS_NAND_MAP_OFF_START]);
                        N = EEFS_NandGet32(&Body[EEFS_NAND_MAP_OFF_COUNT]);
                        for (k = 0; (k < N) && (Start < Geo->BlockCount) && (k < (Geo->BlockCount - Start)); k++) {
                            Dst->Map[Start + k] = Body[EEFS_NAND_MAP_OFF_BYTES + k];
                        }
                        break;
                    case EEFS_NAND_ITEM_ENTRY:
                        if (EEFS_NandDecodeEntryItem(Body, Len, &Index, &E) != EEFS_SUCCESS) {
                            return(EEFS_IO_ERROR);
                        }
                        if (Index >= EEFS_MAX_FILES) {
                            return(EEFS_GEOMETRY_MISMATCH);
                        }
                        memcpy(&Dst->Entry[Index], &E, sizeof(E));
                        break;
                    case EEFS_NAND_ITEM_CLEANUP:
                        N = EEFS_NandGet16(&Body[EEFS_NAND_CLN_OFF_COUNT]);
                        *CleanupCount = 0;
                        for (k = 0; (k < N) && (k < EEFS_NAND_MAX_CLEANUP_BLOCKS); k++) {
                            Cleanup[k] = EEFS_NandGet16(&Body[EEFS_NAND_CLN_OFF_BLOCKS + (2 * k)]);
                        }
                        *CleanupCount = (uint16)k;
                        break;
                    default:
                        break;
                }
            }
            if (Status != 0) {
                return(EEFS_IO_ERROR);
            }
        }
        p += Count;
    }
    return(EEFS_SUCCESS);

} /* End of EEFS_NandApplyChain() */

/* Serializes and programs one record into mirror M at Mirror[M].NextPage of its current block */
int32 EEFS_NandWriteRecord(EEFS_InodeTable_t *Vol, uint8 M, uint8 RecType, uint32 Seq, const EEFS_NandDirState_t *State,
                           const EEFS_NandDirState_t *Base, const uint8 *EntryMask,
                           const uint16 *Cleanup, uint16 CleanupCount)
{
    EEFS_NandMirror_t  *Mir = &Vol->Mirror[M];
    EEFS_NandPacker_t   P;
    uint32              Pages;

    if ((Mir->Disabled == TRUE) || (Mir->Count == 0) || (Mir->CurIndex >= Mir->Count) ||
        ((RecType == EEFS_NAND_REC_TXN) && (Base == NULL)) || ((CleanupCount > 0) && (Cleanup == NULL))) {
        return(EEFS_INVALID_ARGUMENT);
    }
    Pages = EEFS_NandRecordPages(Vol, RecType, State, Base, EntryMask, CleanupCount);
    if ((Pages == 0) || ((Mir->NextPage + Pages) > Vol->PagesPerBlock)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    memset(&P, 0, sizeof(P));
    P.Vol = Vol;
    P.Program = TRUE;
    P.M = M;
    P.RecType = RecType;
    P.Seq = Seq;
    P.PageCount = Pages;
    P.Block = Mir->Blocks[Mir->CurIndex];
    P.FirstPage = Mir->NextPage;
    return(EEFS_NandPackRecord(&P, State, Base, EntryMask, Cleanup, CleanupCount));

} /* End of EEFS_NandWriteRecord() */

/* Dry run of the serializer: the number of pages of the record, 0 if it cannot be built */
uint32 EEFS_NandRecordPages(EEFS_InodeTable_t *Vol, uint8 RecType, const EEFS_NandDirState_t *State,
                            const EEFS_NandDirState_t *Base, const uint8 *EntryMask, uint16 CleanupCount)
{
    EEFS_NandPacker_t   P;

    if ((RecType == EEFS_NAND_REC_TXN) && (Base == NULL)) {
        return(0);
    }
    memset(&P, 0, sizeof(P));
    P.Vol = Vol;
    P.Program = FALSE;
    P.RecType = RecType;
    if (EEFS_NandPackRecord(&P, State, Base, EntryMask, NULL, CleanupCount) != EEFS_SUCCESS) {
        return(0);
    }
    return(P.PageIndex);

} /* End of EEFS_NandRecordPages() */

/* Advances mirror M to the next ring block and erases it; failed blocks are marked bad and skipped */
int32 EEFS_NandEnterBlock(EEFS_InodeTable_t *Vol, uint8 M)
{
    EEFS_NandMirror_t  *Mir = &Vol->Mirror[M];
    uint32              Idx;

    if ((Mir->Disabled == TRUE) || (Mir->Count == 0)) {
        Mir->Disabled = TRUE;
        return(EEFS_IO_ERROR);
    }
    Idx = (Mir->HasState == TRUE) ? ((uint32)Mir->CurIndex + 1) % Mir->Count : 0;

    for (;;) {
        Mir->CurIndex = (uint16)Idx;
        Mir->NextPage = 0;
        if (EEFS_NandEraseBlock(Vol, Mir->Blocks[Idx]) == EEFS_SUCCESS) {
            return(EEFS_SUCCESS);
        }
        EEFS_NandMarkBad(Vol, Mir->Blocks[Idx]);    /* takes it out of the ring; CurIndex now precedes its follower */
        if ((Mir->Disabled == TRUE) || (Mir->Count == 0)) {
            return(EEFS_IO_ERROR);
        }
        Idx = ((uint32)Mir->CurIndex + 1) % Mir->Count;
    }

} /* End of EEFS_NandEnterBlock() */

/* Writes the Working record for Seq into mirror M (TXN or CHECKPOINT, entering a block when needed, retrying) */
int32 EEFS_NandWriteRecordToMirror(EEFS_InodeTable_t *Vol, uint8 M, uint32 Seq, boolean ForceCheckpoint,
                                   const uint8 *EntryMask, const uint16 *Cleanup, uint16 CleanupCount)
{
    return(EEFS_NandWriteToMirror(Vol, M, Seq, ForceCheckpoint, &Vol->Working, &Vol->Base, EntryMask,
                                  Cleanup, CleanupCount));

} /* End of EEFS_NandWriteRecordToMirror() */

/* Brings lagging or empty mirrors up to date: CHECKPOINT of Base at Seq LastSeq in a fresh block */
int32 EEFS_NandResync(EEFS_InodeTable_t *Vol)
{
    uint8   m;

    for (m = 0; m < 2; m++) {
        if ((Vol->Mirror[m].NeedsResync == TRUE) && (Vol->Mirror[m].Disabled == FALSE)) {
            (void)EEFS_NandWriteToMirror(Vol, m, Vol->LastSeq, TRUE, &Vol->Base, &Vol->Base, NULL,
                                         Vol->PendingCleanup, Vol->PendingCleanupBase);
        }
    }
    return(EEFS_SUCCESS);

} /* End of EEFS_NandResync() */

/* Commit (section 5.4) */
int32 EEFS_NandCommit(EEFS_InodeTable_t *Vol, boolean ForceCheckpointBoth)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint8                       EntryMask[EEFS_NAND_MASK_BYTES];
    uint32                      MapChanged;
    uint32                      Written = 0;
    uint32                      Seq;
    uint32                      SavedHigh;
    uint32                      i;
    uint32                      b;
    uint8                       m;
    boolean                     Dirty;

    if (Vol->PendingCleanupBase > 0) {
        EEFS_NandRunPendingCleanup(Vol);
    }
    EEFS_NandResync(Vol);

    EEFS_NandDiff(Vol, EntryMask, &MapChanged);
    Dirty = (boolean)((MapChanged != 0) || (Vol->PendingCleanupCount != 0));
    for (i = 0; i < EEFS_NAND_MASK_BYTES; i++) {
        if (EntryMask[i] != 0) Dirty = TRUE;
    }
    if ((ForceCheckpointBoth == FALSE) && (Dirty == FALSE)) {
        return(EEFS_SUCCESS);
    }

    Seq = Vol->LastSeq + 1;
    SavedHigh = Vol->Working.WriteSeqHigh;
    if ((Vol->NextWriteSeq > 0) && ((Vol->NextWriteSeq - 1) > Vol->Working.WriteSeqHigh)) {
        Vol->Working.WriteSeqHigh = Vol->NextWriteSeq - 1;
    }

    for (m = 0; m < 2; m++) {
        if (Vol->Mirror[m].Disabled == TRUE) continue;
        if (EEFS_NandWriteRecordToMirror(Vol, m, Seq, ForceCheckpointBoth, EntryMask,
                                         Vol->PendingCleanup, Vol->PendingCleanupCount) == EEFS_SUCCESS) {
            Written++;
        }
    }
    if (Written == 0) {
        /* nothing committed: Working stays as it was (bad-block marks made on the way stay too) */
        Vol->Working.WriteSeqHigh = SavedHigh;
        EEFS_NandUpdateResyncFlags(Vol);
        EEFS_NandTouch(Vol);
        return(EEFS_IO_ERROR);
    }

    /* committed: the batch becomes the base */
    for (b = 0; b < Geo->BlockCount; b++) {
        if (Vol->Working.Map[b] == EEFS_NAND_MAP_FREEING) {
            Vol->Working.Map[b] = EEFS_NAND_MAP_FREE;
        }
    }
    memcpy(&Vol->Base, &Vol->Working, sizeof(Vol->Base));
    memset(Vol->ForceDirty, 0, sizeof(Vol->ForceDirty));
    Vol->LastSeq = Seq;
    Vol->DirectoryRebuilt = FALSE;
    EEFS_NandUpdateResyncFlags(Vol);

    /* the cleanup of this commit: erase the superseded first blocks */
    for (i = 0; i < Vol->PendingCleanupCount; i++) {
        (void)EEFS_NandCleanupErase(Vol, Vol->PendingCleanup[i]);
    }
    Vol->PendingCleanupCount = 0;
    Vol->PendingCleanupBase = 0;

    EEFS_NandTouch(Vol);
    return(EEFS_SUCCESS);

} /* End of EEFS_NandCommit() */

/* Abort (section 5.5) */
int32 EEFS_NandAbort(EEFS_InodeTable_t *Vol)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    EEFS_FileDescriptor_t      *Fd;
    const EEFS_NandSlot_t      *W;
    const EEFS_NandSlot_t      *B;
    uint32                      i;
    uint32                      s;
    uint32                      b;
    uint32                      First;

    /* a descriptor on a slot the batch created or rewrote would be stranded */
    for (i = 0; i < EEFS_MAX_OPEN_FILES; i++) {
        Fd = &EEFS_FileDescriptorTable[i];
        if ((Fd->InUse == TRUE) && (Fd->InodeTable == Vol) && (Fd->InodeIndex < EEFS_MAX_FILES) && (Fd->Slot < 2)) {
            if (memcmp(&Vol->Working.Entry[Fd->InodeIndex].Slot[Fd->Slot],
                       &Vol->Base.Entry[Fd->InodeIndex].Slot[Fd->Slot], sizeof(EEFS_NandSlot_t)) != 0) {
                return(EEFS_DEVICE_IS_BUSY);
            }
        }
    }

    /* best effort: erase the new first blocks of the batch, so no orphan page 0 is left for a rebuild.  Only blocks
     * that are free in the committed map are touched: with copy-on-write every other block of a rewritten slot may
     * be shared with the committed copy. */
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        if (Vol->Working.Entry[i].InUse == 0) continue;
        for (s = 0; s < 2; s++) {
            W = &Vol->Working.Entry[i].Slot[s];
            B = &Vol->Base.Entry[i].Slot[s];
            if ((W->InUse == 0) || ((B->InUse != 0) && (B->FirstBlock == W->FirstBlock))) continue;
            First = W->FirstBlock;
            if ((First < Geo->DirRegionBlocks) || (First >= Geo->BlockCount) ||
                (Vol->Base.Map[First] != EEFS_NAND_MAP_FREE) || (Vol->Working.Map[First] == EEFS_NAND_MAP_BAD)) {
                continue;
            }
            if (EEFS_NandPageIsErased(Vol, First, 0) == FALSE) {
                if (EEFS_NandEraseBlock(Vol, First) != EEFS_SUCCESS) {
                    EEFS_NandMarkBad(Vol, First);
                }
            }
        }
    }

    /* likewise the first blocks the batch created and then superseded itself (written, then rewritten, renamed or
     * removed in the same batch): they are on the batch's cleanup list and free in the committed map */
    for (i = Vol->PendingCleanupBase; i < Vol->PendingCleanupCount; i++) {
        First = Vol->PendingCleanup[i];
        if ((First < Geo->DirRegionBlocks) || (First >= Geo->BlockCount) ||
            (Vol->Base.Map[First] != EEFS_NAND_MAP_FREE) || (Vol->Working.Map[First] != EEFS_NAND_MAP_FREEING)) {
            continue;
        }
        if (EEFS_NandPageIsErased(Vol, First, 0) == FALSE) {
            if (EEFS_NandEraseBlock(Vol, First) != EEFS_SUCCESS) {
                EEFS_NandMarkBad(Vol, First);
            }
        }
    }

    /* back to the committed state; bad-block marks are sticky */
    for (b = 0; b < Geo->BlockCount; b++) {
        if (Vol->Working.Map[b] != EEFS_NAND_MAP_BAD) {
            Vol->Working.Map[b] = Vol->Base.Map[b];
        }
    }
    memcpy(Vol->Working.Entry, Vol->Base.Entry, sizeof(Vol->Working.Entry));
    Vol->Working.WriteSeqHigh = Vol->Base.WriteSeqHigh;
    Vol->PendingCleanupCount = Vol->PendingCleanupBase;

    EEFS_NandTouch(Vol);
    return(EEFS_SUCCESS);

} /* End of EEFS_NandAbort() */

/* Pending cleanup loaded from the media (section 5.6) */
int32 EEFS_NandRunPendingCleanup(EEFS_InodeTable_t *Vol)
{
    uint32  n = Vol->PendingCleanupBase;
    uint32  i;

    if (n > Vol->PendingCleanupCount) n = Vol->PendingCleanupCount;
    for (i = 0; i < n; i++) {
        (void)EEFS_NandCleanupErase(Vol, Vol->PendingCleanup[i]);
    }
    for (i = n; i < Vol->PendingCleanupCount; i++) {
        Vol->PendingCleanup[i - n] = Vol->PendingCleanup[i];
    }
    Vol->PendingCleanupCount = (uint16)(Vol->PendingCleanupCount - n);
    Vol->PendingCleanupBase = 0;
    return(EEFS_SUCCESS);

} /* End of EEFS_NandRunPendingCleanup() */

/* Appends a block to the cleanup list (once) */
int32 EEFS_NandAddCleanup(EEFS_InodeTable_t *Vol, uint16 Block)
{
    uint32  i;

    for (i = 0; i < Vol->PendingCleanupCount; i++) {
        if (Vol->PendingCleanup[i] == Block) return(EEFS_SUCCESS);
    }
    if (Vol->PendingCleanupCount >= EEFS_NAND_MAX_CLEANUP_BLOCKS) {
        return(EEFS_TRANSACTION_ERROR);
    }
    Vol->PendingCleanup[Vol->PendingCleanupCount++] = Block;
    return(EEFS_SUCCESS);

} /* End of EEFS_NandAddCleanup() */

/* TRUE if Needed more blocks fit on the cleanup list */
boolean EEFS_NandCleanupHasRoom(EEFS_InodeTable_t *Vol, uint32 Needed)
{
    return((boolean)(((uint32)Vol->PendingCleanupCount + Needed) <= EEFS_NAND_MAX_CLEANUP_BLOCKS));

} /* End of EEFS_NandCleanupHasRoom() */

/* Removes Block from the cleanup list (the allocator hands it out again) */
void EEFS_NandForgetCleanup(EEFS_InodeTable_t *Vol, uint16 Block)
{
    uint32  i;
    uint32  j;

    for (i = 0; i < Vol->PendingCleanupCount; i++) {
        if (Vol->PendingCleanup[i] != Block) continue;
        for (j = i + 1; j < Vol->PendingCleanupCount; j++) {
            Vol->PendingCleanup[j - 1] = Vol->PendingCleanup[j];
        }
        Vol->PendingCleanupCount--;
        if (i < Vol->PendingCleanupBase) Vol->PendingCleanupBase--;
        return;
    }

} /* End of EEFS_NandForgetCleanup() */

/* Dirty entries (memcmp + ForceDirty) and the number of changed map bytes */
void EEFS_NandDiff(EEFS_InodeTable_t *Vol, uint8 *EntryMask, uint32 *MapRanges)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      i;
    uint32                      Changed = 0;

    memset(EntryMask, 0, EEFS_NAND_MASK_BYTES);
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        if ((EEFS_NAND_MASK_TEST(Vol->ForceDirty, i)) ||
            (memcmp(&Vol->Working.Entry[i], &Vol->Base.Entry[i], sizeof(EEFS_NandEntry_t)) != 0)) {
            EEFS_NAND_MASK_SET(EntryMask, i);
        }
    }
    for (i = 0; i < Geo->BlockCount; i++) {
        if (Vol->Working.Map[i] != Vol->Base.Map[i]) Changed++;
    }
    if (MapRanges != NULL) *MapRanges = Changed;

} /* End of EEFS_NandDiff() */

/* Uncommitted changes in the batch */
boolean EEFS_NandHasUncommitted(EEFS_InodeTable_t *Vol, boolean IncludeForceDirty)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      i;

    if (memcmp(Vol->Working.Entry, Vol->Base.Entry, sizeof(Vol->Working.Entry)) != 0) return(TRUE);
    if (memcmp(Vol->Working.Map, Vol->Base.Map, Geo->BlockCount) != 0) return(TRUE);
    if (Vol->PendingCleanupCount > Vol->PendingCleanupBase) return(TRUE);
    if (IncludeForceDirty == TRUE) {
        for (i = 0; i < EEFS_NAND_MASK_BYTES; i++) {
            if (Vol->ForceDirty[i] != 0) return(TRUE);
        }
    }
    return(FALSE);

} /* End of EEFS_NandHasUncommitted() */

/*
 * Local Functions
 */

/* Writes one record for Seq into mirror M from State (TXN against Base when it fits in the current block, else a
 * CHECKPOINT in a freshly entered block).  A failed program or verify marks the block bad and retries in the next
 * block, up to EEFS_NAND_WRITE_RETRIES times.  Updates Mirror[M] on success. */
static int32 EEFS_NandWriteToMirror(EEFS_InodeTable_t *Vol, uint8 M, uint32 Seq, boolean ForceCheckpoint,
                                    const EEFS_NandDirState_t *State, const EEFS_NandDirState_t *Base,
                                    const uint8 *EntryMask, const uint16 *Cleanup, uint16 CleanupCount)
{
    EEFS_NandMirror_t  *Mir = &Vol->Mirror[M];
    uint32              Retries = 0;
    uint32              TxnPages;
    uint32              Pages;
    boolean             UseCheckpoint;
    uint8               RecType;
    int32               Status;

    for (;;) {
        if ((Mir->Disabled == TRUE) || (Mir->Count == 0)) {
            return(EEFS_IO_ERROR);
        }
        TxnPages = EEFS_NandRecordPages(Vol, EEFS_NAND_REC_TXN, State, Base, EntryMask, CleanupCount);
        UseCheckpoint = (boolean)((ForceCheckpoint == TRUE) || (Mir->HasState == FALSE) ||
                                  (Mir->EnterNewBlock == TRUE) || (Mir->NeedsResync == TRUE) || (TxnPages == 0) ||
                                  (((uint32)Mir->NextPage + TxnPages) > Vol->PagesPerBlock));
        if (UseCheckpoint == TRUE) {
            if ((Mir->HasState == TRUE) || (Mir->EnterNewBlock == TRUE) ||
                (EEFS_NandPageIsErased(Vol, Mir->Blocks[Mir->CurIndex], 0) == FALSE)) {
                if (EEFS_NandEnterBlock(Vol, M) != EEFS_SUCCESS) {
                    return(EEFS_IO_ERROR);
                }
            }
            else {
                Mir->NextPage = 0;      /* the first ring block of a fresh format, already erased */
            }
        }
        RecType = UseCheckpoint ? EEFS_NAND_REC_CHECKPOINT : EEFS_NAND_REC_TXN;
        Pages = EEFS_NandRecordPages(Vol, RecType, State, UseCheckpoint ? NULL : Base, EntryMask, CleanupCount);

        Status = EEFS_NandWriteRecord(Vol, M, RecType, Seq, State, UseCheckpoint ? NULL : Base, EntryMask,
                                      Cleanup, CleanupCount);
        if (Status == EEFS_SUCCESS) {
            Mir->HasState = TRUE;
            Mir->LastSeq = Seq;
            if (UseCheckpoint == TRUE) Mir->CheckpointSeq = Seq;
            Mir->NextPage = (uint16)(Mir->NextPage + Pages);
            Mir->EnterNewBlock = FALSE;
            Mir->NeedsResync = FALSE;
            return(EEFS_SUCCESS);
        }
        if (Status != EEFS_IO_ERROR) {
            return(EEFS_IO_ERROR);      /* not a media failure: nothing to mark */
        }

        /* program or verify failure in the current block */
        EEFS_NandMarkBad(Vol, Mir->Blocks[Mir->CurIndex]);
        Mir->EnterNewBlock = TRUE;
        Retries++;
        if (Retries > EEFS_NAND_WRITE_RETRIES) {
            return(EEFS_IO_ERROR);
        }
    }

} /* End of EEFS_NandWriteToMirror() */

/* Validates the chain of records of one block (section 5.3).  TRUE if page 0 holds a valid CHECKPOINT; the chain is
 * then CHECKPOINT(*CheckpointSeq) .. TXN(*LastSeq) and *NextPage is the page after it. */
static boolean EEFS_NandWalkChain(EEFS_InodeTable_t *Vol, uint32 Block, uint32 *CheckpointSeq, uint32 *LastSeq,
                                  uint32 *NextPage, uint32 *MaxSeqSeen)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    EEFS_NandRecHdr_t           H;
    EEFS_NandRecHdr_t           H2;
    uint32                      p = 0;
    uint32                      q;
    boolean                     Valid = FALSE;
    boolean                     Ok;

    *CheckpointSeq = 0;
    *LastSeq = 0;
    while (p < Geo->PagesPerBlock) {
        if ((EEFS_NandReadPage(Vol, Block, p, Vol->PageBuf, NULL, NULL) != EEFS_SUCCESS) ||
            (EEFS_NandDecodeRecordPage(Vol->PageBuf, Geo->PageSize, &H) != EEFS_SUCCESS)) {
            break;
        }
        if (H.Seq > *MaxSeqSeen) *MaxSeqSeen = H.Seq;
        if (H.PageIndex != 0) break;
        if (p == 0) {
            if (H.RecType != EEFS_NAND_REC_CHECKPOINT) break;
        }
        else if ((H.RecType != EEFS_NAND_REC_TXN) || (H.Seq != (*LastSeq + 1))) {
            break;
        }
        if ((p + H.PageCount) > Geo->PagesPerBlock) break;

        Ok = TRUE;
        for (q = 1; q < H.PageCount; q++) {
            if ((EEFS_NandReadPage(Vol, Block, p + q, Vol->PageBuf, NULL, NULL) != EEFS_SUCCESS) ||
                (EEFS_NandDecodeRecordPage(Vol->PageBuf, Geo->PageSize, &H2) != EEFS_SUCCESS) ||
                (H2.Seq != H.Seq) || (H2.RecType != H.RecType) || (H2.PageIndex != q) ||
                (H2.PageCount != H.PageCount)) {
                Ok = FALSE;
                break;
            }
        }
        if (Ok == FALSE) break;

        *LastSeq = H.Seq;
        if (p == 0) {
            *CheckpointSeq = H.Seq;
            Valid = TRUE;
        }
        p += H.PageCount;
    }
    *NextPage = p;
    return(Valid);

} /* End of EEFS_NandWalkChain() */

/* A mirror needs a resync when it has no state or ends before the committed Seq */
static void EEFS_NandUpdateResyncFlags(EEFS_InodeTable_t *Vol)
{
    EEFS_NandMirror_t  *Mir;
    uint8               m;

    for (m = 0; m < 2; m++) {
        Mir = &Vol->Mirror[m];
        if (Mir->Disabled == TRUE) continue;
        if ((Mir->HasState == FALSE) || (Mir->LastSeq < Vol->LastSeq)) {
            Mir->NeedsResync = TRUE;
            Mir->EnterNewBlock = TRUE;
        }
    }

} /* End of EEFS_NandUpdateResyncFlags() */

/* Cleanup erase of one block: only a data block that is free in Working and still holds a programmed page 0.  A
 * failed erase marks the block bad.  TRUE if an erase was issued. */
static boolean EEFS_NandCleanupErase(EEFS_InodeTable_t *Vol, uint16 Block)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;

    if ((Block < Geo->DirRegionBlocks) || (Block >= Geo->BlockCount) ||
        (Vol->Working.Map[Block] != EEFS_NAND_MAP_FREE)) {
        return(FALSE);
    }
    if (EEFS_NandPageIsErased(Vol, Block, 0) == TRUE) {
        return(FALSE);
    }
    if (EEFS_NandEraseBlock(Vol, Block) != EEFS_SUCCESS) {
        EEFS_NandMarkBad(Vol, Block);
    }
    return(TRUE);

} /* End of EEFS_NandCleanupErase() */

/* Mount step 6: every in-use slot against its metadata page */
static void EEFS_NandReconcile(EEFS_InodeTable_t *Vol, uint32 *MaxPageWriteSeq)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    EEFS_NandEntry_t           *E;
    EEFS_NandSlot_t            *S;
    EEFS_NandPage0_t            P;
    const uint8                *Raw;
    uint32                      i;
    uint8                       s;
    uint8                       Owner;
    uint8                       v;
    uint32                      b;
    uint32                      k;
    uint16                      Blk;
    uint32                      Crc;
    int32                       Status;

    for (i = 0; i < EEFS_MAX_FILES; i++) {
        E = &Vol->Working.Entry[i];
        if (E->InUse == 0) continue;
        for (s = 0; s < 2; s++) {
            S = &E->Slot[s];
            if (S->InUse == 0) continue;
            Owner = EEFS_NAND_MAP_OWNER(i, s);

            Status = EEFS_NandReadPage0(Vol, S->FirstBlock, &P, &Raw);
            if ((Status == EEFS_SUCCESS) &&
                ((strncmp(P.Path, E->Path, EEFS_NAND_MAX_PATH_SIZE) != 0) ||
                 ((P.Slot != EEFS_NAND_SLOT_NONE) && (P.Slot != s)))) {
                Status = EEFS_CORRUPT_FILE;
            }
            if (Status != EEFS_SUCCESS) {
                Vol->SlotFlags[i][s] |= EEFS_NAND_SLOTFLAG_CORRUPT;
                if ((E->IsAB != 0) && (E->ActiveSlot == s) && (E->ActiveFailed == 0)) {
                    E->ActiveFailed = 1;
                    EEFS_NAND_MASK_SET(Vol->ForceDirty, i);
                }
                continue;       /* the map stays as the directory says */
            }
            if (P.WriteSeq > *MaxPageWriteSeq) *MaxPageWriteSeq = P.WriteSeq;

            /* block ownership follows the metadata page (no FREEING exists at mount, so it marks "was ours") */
            for (b = Geo->DirRegionBlocks; b < Geo->BlockCount; b++) {
                if (Vol->Working.Map[b] == Owner) Vol->Working.Map[b] = EEFS_NAND_MAP_FREEING;
            }
            for (k = 0; k < P.BlockCount; k++) {
                EEFS_NandPage0Block(Raw, k, &Blk, &Crc);
                v = Vol->Working.Map[Blk];
                if ((v == EEFS_NAND_MAP_FREE) || (v == EEFS_NAND_MAP_FREEING) || (v == Owner)) {
                    Vol->Working.Map[Blk] = Owner;
                }
                else {
                    Vol->SlotFlags[i][s] |= EEFS_NAND_SLOTFLAG_CROSSLINK;
                }
            }
            for (b = Geo->DirRegionBlocks; b < Geo->BlockCount; b++) {
                if (Vol->Working.Map[b] == EEFS_NAND_MAP_FREEING) Vol->Working.Map[b] = EEFS_NAND_MAP_FREE;
            }

            /* the copied fields: page 0 wins */
            if ((P.BlockCount != S->AllocBlocks) || (P.FileSize != S->FileSize) || (P.Version != S->Version) ||
                (P.WriteSeq != S->WriteSeq) || (P.FileCrc32 != S->FileCrc32) ||
                (P.ModificationDate != S->ModificationDate)) {
                S->AllocBlocks = (uint16)P.BlockCount;
                S->FileSize = P.FileSize;
                S->Version = P.Version;
                S->WriteSeq = P.WriteSeq;
                S->FileCrc32 = P.FileCrc32;
                S->ModificationDate = P.ModificationDate;
                EEFS_NAND_MASK_SET(Vol->ForceDirty, i);
            }
        }
    }

} /* End of EEFS_NandReconcile() */

/*
 * Record serializer
 */

/* Starts page PageIndex of the record in PageBuf */
static void EEFS_NandPackBegin(EEFS_NandPacker_t *P)
{
    EEFS_NandRecHdr_t   H;

    H.RecType = P->RecType;
    H.Mirror = P->M;
    H.Seq = P->Seq;
    H.PageIndex = (uint16)P->PageIndex;
    H.PageCount = (uint16)P->PageCount;
    H.PayloadLen = 0;
    EEFS_NandBeginRecordPage(P->Vol->PageBuf, P->Vol->PageSize, &H);
    P->Used = 0;

} /* End of EEFS_NandPackBegin() */

/* Seals and (program run) programs the current page with read-back verify */
static int32 EEFS_NandPackFlush(EEFS_NandPacker_t *P)
{
    EEFS_InodeTable_t  *Vol = P->Vol;

    if (P->Program == FALSE) {
        return(EEFS_SUCCESS);
    }
    if (P->PageIndex >= P->PageCount) {
        return(EEFS_ERROR);     /* the dry run and the program run disagree */
    }
    EEFS_NandSealRecordPage(Vol->PageBuf, Vol->PageSize, P->Used);
    return(EEFS_NandProgramPageVerified(Vol, P->Block, P->FirstPage + P->PageIndex, Vol->PageBuf, Vol->CacheBuf));

} /* End of EEFS_NandPackFlush() */

/* Finishes the current page and starts the next one */
static int32 EEFS_NandPackNextPage(EEFS_NandPacker_t *P)
{
    int32   Status;

    if ((Status = EEFS_NandPackFlush(P)) != EEFS_SUCCESS) {
        return(Status);
    }
    P->PageIndex++;
    if (P->PageIndex > P->Vol->PagesPerBlock) {
        return(EEFS_ERROR);     /* a record never exceeds a block */
    }
    EEFS_NandPackBegin(P);
    return(EEFS_SUCCESS);

} /* End of EEFS_NandPackNextPage() */

/* Makes sure an item of Size bytes fits in the current page (items never span pages) */
static int32 EEFS_NandPackRoom(EEFS_NandPacker_t *P, uint32 Size)
{
    uint32  Payload = (uint32)P->Vol->PageSize - EEFS_NAND_REC_HDR_SIZE;

    if (Size > Payload) {
        return(EEFS_ERROR);
    }
    if ((P->Used + Size) > Payload) {
        return(EEFS_NandPackNextPage(P));
    }
    return(EEFS_SUCCESS);

} /* End of EEFS_NandPackRoom() */

/* MAPCHUNK items for blocks [Start, End), greedy: each chunk fills the rest of the page */
static int32 EEFS_NandPackMap(EEFS_NandPacker_t *P, const EEFS_NandDirState_t *State, uint32 Start, uint32 End)
{
    uint32  Payload = (uint32)P->Vol->PageSize - EEFS_NAND_REC_HDR_SIZE;
    uint32  Fit;
    uint32  Count;
    int32   Status;

    while (Start < End) {
        Fit = EEFS_NandMapChunkFit(Payload - P->Used);
        if (Fit == 0) {
            if ((Status = EEFS_NandPackNextPage(P)) != EEFS_SUCCESS) return(Status);
            continue;
        }
        Count = End - Start;
        if (Count > Fit) Count = Fit;
        if (P->Program == TRUE) {
            (void)EEFS_NandEncodeMapChunk(&P->Vol->PageBuf[EEFS_NAND_REC_HDR_SIZE + P->Used], Payload - P->Used,
                                          Start, Count, &State->Map[Start]);
        }
        P->Used += EEFS_NAND_ITEM_HDR_SIZE + EEFS_NAND_MAPCHUNK_HDR_SIZE + Count;
        Start += Count;
    }
    return(EEFS_SUCCESS);

} /* End of EEFS_NandPackMap() */

/* Serializes a record (docs/nand-format.md 4.4): VOLUME, MAPCHUNKs, ENTRYs, CLEANUP.  Dry run: P->PageIndex
 * receives the page count. */
static int32 EEFS_NandPackRecord(EEFS_NandPacker_t *P, const EEFS_NandDirState_t *State,
                                 const EEFS_NandDirState_t *Base, const uint8 *EntryMask,
                                 const uint16 *Cleanup, uint16 CleanupCount)
{
    EEFS_InodeTable_t          *Vol = P->Vol;
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      Payload = (uint32)Vol->PageSize - EEFS_NAND_REC_HDR_SIZE;
    EEFS_NandVolumeItem_t       V;
    uint32                      b;
    uint32                      j;
    uint32                      Last;
    uint32                      i;
    uint32                      Size;
    boolean                     Include;
    int32                       Status;

    P->PageIndex = 0;
    EEFS_NandPackBegin(P);

    /* VOLUME */
    memset(&V, 0, sizeof(V));
    V.PageSize = Geo->PageSize;
    V.OobSize = Geo->OobSize;
    V.PagesPerBlock = Geo->PagesPerBlock;
    V.BlockCount = Geo->BlockCount;
    V.DirRegionBlocks = Geo->DirRegionBlocks;
    V.MaxFiles = EEFS_MAX_FILES;
    V.WriteSeqHigh = State->WriteSeqHigh;
    V.FormatTime = Vol->FormatTime;
    V.PathSize = EEFS_NAND_MAX_PATH_SIZE;
    V.CfsNameSize = EEFS_NAND_CFS_NAME_SIZE;
    V.CfsSymbolSize = EEFS_NAND_CFS_SYMBOL_SIZE;
    Size = EEFS_NAND_ITEM_HDR_SIZE + EEFS_NAND_VOLUME_ITEM_SIZE;
    if (P->Program == TRUE) {
        (void)EEFS_NandEncodeVolumeItem(&Vol->PageBuf[EEFS_NAND_REC_HDR_SIZE], Payload, &V);
    }
    P->Used += Size;

    /* MAPCHUNKs: the whole map, or the changed ranges (gaps of fewer than 16 unchanged bytes are merged) */
    if (P->RecType == EEFS_NAND_REC_CHECKPOINT) {
        if ((Status = EEFS_NandPackMap(P, State, 0, Geo->BlockCount)) != EEFS_SUCCESS) return(Status);
    }
    else {
        b = 0;
        while (b < Geo->BlockCount) {
            if (State->Map[b] == Base->Map[b]) {
                b++;
                continue;
            }
            Last = b;
            for (j = b + 1; (j < Geo->BlockCount) && ((j - Last) <= EEFS_NAND_MAP_MERGE_GAP); j++) {
                if (State->Map[j] != Base->Map[j]) Last = j;
            }
            if ((Status = EEFS_NandPackMap(P, State, b, Last + 1)) != EEFS_SUCCESS) return(Status);
            b = Last + 1;
        }
    }

    /* ENTRYs: every in-use entry (CHECKPOINT) or the dirty ones (TXN) */
    Size = EEFS_NAND_ITEM_HDR_SIZE + EEFS_NAND_ENTRY_ITEM_SIZE;
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        if (P->RecType == EEFS_NAND_REC_CHECKPOINT) {
            Include = (boolean)(State->Entry[i].InUse != 0);
        }
        else {
            Include = (boolean)((EntryMask != NULL) && EEFS_NAND_MASK_TEST(EntryMask, i));
        }
        if (Include == FALSE) continue;
        if ((Status = EEFS_NandPackRoom(P, Size)) != EEFS_SUCCESS) return(Status);
        if (P->Program == TRUE) {
            (void)EEFS_NandEncodeEntryItem(&Vol->PageBuf[EEFS_NAND_REC_HDR_SIZE + P->Used], Payload - P->Used,
                                           (uint16)i, &State->Entry[i]);
        }
        P->Used += Size;
    }

    /* CLEANUP (one item, so that the list of the record is complete in it) */
    if (CleanupCount > 0) {
        Size = EEFS_NAND_ITEM_HDR_SIZE + EEFS_NAND_CLEANUP_HDR_SIZE + (2 * (uint32)CleanupCount);
        if ((Status = EEFS_NandPackRoom(P, Size)) != EEFS_SUCCESS) return(Status);
        if (P->Program == TRUE) {
            (void)EEFS_NandEncodeCleanupItem(&Vol->PageBuf[EEFS_NAND_REC_HDR_SIZE + P->Used], Payload - P->Used,
                                             Cleanup, CleanupCount);
        }
        P->Used += Size;
    }

    if ((Status = EEFS_NandPackFlush(P)) != EEFS_SUCCESS) {
        return(Status);
    }
    P->PageIndex++;
    return(EEFS_SUCCESS);

} /* End of EEFS_NandPackRecord() */

/************************/
/*  End of File Comment */
/************************/
