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
 * Filename: eefs_nand_health.c
 *
 * Purpose: Resumable health check of a mounted NAND volume (docs/nand-implementation.md section 10).
 *
 * Design Notes:
 *   The check is meant for a low-priority background task after every boot: it reports A/B copies that are corrupt or
 *   disagree, files whose metadata page or data fails its CRCs, a directory that does not match the metadata pages or
 *   the block map, damaged or lagging directory mirrors, interrupted commits and cleanups, orphaned metadata pages,
 *   unrecorded bad blocks, blocks that need scrubbing and low free space.
 *
 *   It never programs or erases.  It runs in steps: EEFS_LibHealthCheckStep holds EEFS_LIB_LOCK only for the step and
 *   stops after MaxPageReads page reads.  Every cursor and intermediate result lives in the caller's report, and
 *   every unit of work reads at most one page into the volume's scratch buffer and finishes with it, so the result
 *   does not depend on where the steps end (MaxPageReads = 1 gives the same report as one big step).  Any change of
 *   the volume between steps (VolumeChangeCounter, bumped by every mutating call and by every program and erase)
 *   restarts the check from phase 1.
 *
 *   Phases: 1 directory (the mirror chains and their tails, the pending cleanup), 2 files (every slot of the Working
 *   directory: metadata page, block claims against the map, every data page against the block and file CRCs), 3 A/B
 *   (no reads), 4 scan of the data area (bad-block markers and orphaned metadata pages), then the free-space check.
 *   A slot that fails in phase 2 is flagged exactly as a failed EEFS_NandValidateSlot would (SLOTFLAG_CORRUPT, and
 *   ActiveFailed for the active copy of an A/B entry, which the next commit persists) without restarting the check.
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

#define H_PHASE_DIR         1
#define H_PHASE_FILES       2
#define H_PHASE_AB          3
#define H_PHASE_SCAN        4
#define H_PHASE_DONE        5

/* phase 1 */
#define H_DIR_SCAN          0       /* every directory block: bad-block markers, checkpoint candidates */
#define H_DIR_PICK          1       /* the next candidate of the mirror, or the mirror's findings */
#define H_DIR_WALK          2       /* the chain of one candidate, page by page */
#define H_DIR_MIRRORS       3       /* mirror missing / mismatch, rebuilt directory */
#define H_DIR_CLEANUP       4       /* the pending cleanup loaded from the media */

/* phase 2 */
#define H_FILE_SLOT         0       /* the next slot in use */
#define H_FILE_PAGE0        1       /* its metadata page */
#define H_FILE_BLOCK        2       /* metadata page again, for Blocks[K] */
#define H_FILE_DATA         3       /* one data page */
#define H_FILE_MAP          4       /* the map against the claims */

/* SlotResult */
#define H_SLOT_OK           1
#define H_SLOT_CORRUPT      2
#define H_SLOT_IO_ERROR     3
#define H_SLOT_UNUSED       4

/* CandState */
#define H_CAND_NONE         0
#define H_CAND_PENDING      1
#define H_CAND_WALKED       2
#define H_CAND_TORN         3

#define H_NA8               0xFF
#define H_NA16              0xFFFF
#define H_MIXED             0xFE    /* FirstActive: AB_LOAD_MIXED already reported */

#define H_CLAIMED(R, b)     (((R)->Claimed[(b) / 8] & (uint8)(1u << ((b) % 8))) != 0)
#define H_CLAIM(R, b)       ((R)->Claimed[(b) / 8] |= (uint8)(1u << ((b) % 8)))

/*
 * Local Function Prototypes
 */

static void     H_Begin(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol);
static void     H_Unit(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol);
static void     H_Finish(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol);
static int32    H_Read(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol, uint32 Block, uint32 Page, uint8 *Data, uint8 *Oob,
                       uint32 *Corrected);
static boolean  H_MarkerPage(EEFS_InodeTable_t *Vol, uint32 Index, uint32 *Page);
static int32    H_MarkerUnit(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol, uint32 Block);
static int32    H_DecodePage0(EEFS_InodeTable_t *Vol, uint32 Block, EEFS_NandPage0_t *P);
static void     H_Add(EEFS_HealthReport_t *R, uint16 Code, uint8 Slot, uint16 EntryIndex, uint32 Block, uint32 Aux0,
                      uint32 Aux1, const char *Path);
static boolean  H_OnCleanupList(EEFS_InodeTable_t *Vol, uint32 Block);

static void     H_DirScan(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol);
static void     H_DirPick(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol);
static void     H_DirWalk(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol);
static void     H_DirWalkEnd(EEFS_HealthReport_t *R);
static void     H_DirMirrors(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol);
static void     H_DirCleanup(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol);

static void     H_FileSlot(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol);
static void     H_FilePage0(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol);
static void     H_FileBlock(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol);
static void     H_FileData(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol);
static void     H_FileMap(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol);
static void     H_SlotEnd(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol, uint8 Result);
static void     H_SlotFinding(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol, uint16 Code, uint32 Block, uint32 Aux0);

static void     H_AB(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol);
static void     H_Scan(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol);

/*
 * Function Definitions
 */

/* Attaches Report to the volume and starts a check (section 10.2) */
int32 EEFS_LibHealthCheckStart(EEFS_InodeTable_t *InodeTable, EEFS_HealthReport_t *Report)
{
    int32   ReturnCode;

    if ((InodeTable == NULL) || (Report == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if ((InodeTable->Mounted == 0) || (InodeTable->Dev == NULL)) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else if (InodeTable->ActiveReport != NULL) {
        ReturnCode = EEFS_DEVICE_IS_BUSY;
    }
    else {
        memset(Report, 0, sizeof(EEFS_HealthReport_t));
        H_Begin(Report, InodeTable);
        InodeTable->ActiveReport = Report;
        ReturnCode = EEFS_SUCCESS;
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibHealthCheckStart() */

/* Runs the check for at most MaxPageReads page reads (0 counts as 1).  1 = more to do, EEFS_SUCCESS = done (the report
 * is complete and copied to the volume's last report), EEFS_INVALID_ARGUMENT = no check attached. */
int32 EEFS_LibHealthCheckStep(EEFS_InodeTable_t *InodeTable, uint32 MaxPageReads)
{
    EEFS_HealthReport_t    *R;
    int32                   ReturnCode;

    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    R = InodeTable->ActiveReport;
    if ((InodeTable->Mounted == 0) || (InodeTable->Dev == NULL) || (R == NULL) || (R->Volume != InodeTable)) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else {
        R->S.Steps++;
        if (InodeTable->VolumeChangeCounter != R->ChangeCounter) {
            R->S.Restarts++;
            H_Begin(R, InodeTable);
        }
        if (MaxPageReads == 0) {
            MaxPageReads = 1;
        }
        R->StepReads = 0;
        while ((R->Phase != H_PHASE_DONE) && (R->StepReads < MaxPageReads)) {
            H_Unit(R, InodeTable);
        }
        if (R->Phase == H_PHASE_DONE) {
            H_Finish(R, InodeTable);
            ReturnCode = EEFS_SUCCESS;
        }
        else {
            ReturnCode = 1;
        }
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibHealthCheckStep() */

/* Copies the last completed report of the volume */
int32 EEFS_LibHealthCheckSummary(EEFS_InodeTable_t *InodeTable, EEFS_HealthSummary_t *Out)
{
    int32   ReturnCode;

    if ((InodeTable == NULL) || (Out == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    if ((InodeTable->Mounted == 0) || (InodeTable->Dev == NULL)) {
        ReturnCode = EEFS_INVALID_ARGUMENT;
    }
    else if (InodeTable->LastReport.Complete == 0) {
        ReturnCode = EEFS_ERROR;
    }
    else {
        memcpy(Out, &InodeTable->LastReport, sizeof(EEFS_HealthSummary_t));
        ReturnCode = EEFS_SUCCESS;
    }
    EEFS_LIB_UNLOCK;
    return(ReturnCode);

} /* End of EEFS_LibHealthCheckSummary() */

/* Detaches the active report (if any); the volume's last report is unchanged */
int32 EEFS_LibHealthCheckCancel(EEFS_InodeTable_t *InodeTable)
{
    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }

    EEFS_LIB_LOCK;
    InodeTable->ActiveReport = NULL;
    EEFS_LIB_UNLOCK;
    return(EEFS_SUCCESS);

} /* End of EEFS_LibHealthCheckCancel() */

/*
 * Local Functions
 */

/* (Re)starts the check from phase 1.  Restarts, Steps and the ActiveFailed flags this check set survive a restart. */
static void H_Begin(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol)
{
    const EEFS_NandEntry_t *E;
    uint8                   SetByCheck[EEFS_NAND_MASK_BYTES];
    uint32                  Restarts = R->S.Restarts;
    uint32                  Steps = R->S.Steps;
    uint32                  i;

    memcpy(SetByCheck, R->SetByCheck, sizeof(SetByCheck));
    memset(R, 0, sizeof(*R));
    R->S.Restarts = Restarts;
    R->S.Steps = Steps;
    R->S.Summary = EEFS_HEALTH_INCOMPLETE;
    R->Volume = Vol;
    R->ChangeCounter = Vol->VolumeChangeCounter;
    R->Phase = H_PHASE_DIR;
    R->Sub = H_DIR_SCAN;
    R->FirstActive = H_NA8;

    /* AB_FAILED_FLAG_SET is about flags that were set before this check looked at the volume */
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        E = &Vol->Working.Entry[i];
        if ((E->InUse != 0) && (E->IsAB != 0) && (E->ActiveFailed != 0)) {
            if (EEFS_NAND_MASK_TEST(SetByCheck, i)) {
                EEFS_NAND_MASK_SET(R->SetByCheck, i);
            }
            else {
                EEFS_NAND_MASK_SET(R->FailedAtStart, i);
            }
        }
    }

} /* End of H_Begin() */

/* One unit of work: at most one page read */
static void H_Unit(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol)
{
    switch (R->Phase) {
        case H_PHASE_DIR:
            switch (R->Sub) {
                case H_DIR_SCAN:    H_DirScan(R, Vol);      break;
                case H_DIR_PICK:    H_DirPick(R, Vol);      break;
                case H_DIR_WALK:    H_DirWalk(R, Vol);      break;
                case H_DIR_MIRRORS: H_DirMirrors(R, Vol);   break;
                default:            H_DirCleanup(R, Vol);   break;
            }
            break;
        case H_PHASE_FILES:
            switch (R->Sub) {
                case H_FILE_SLOT:   H_FileSlot(R, Vol);     break;
                case H_FILE_PAGE0:  H_FilePage0(R, Vol);    break;
                case H_FILE_BLOCK:  H_FileBlock(R, Vol);    break;
                case H_FILE_DATA:   H_FileData(R, Vol);     break;
                default:            H_FileMap(R, Vol);      break;
            }
            break;
        case H_PHASE_AB:
            H_AB(R, Vol);
            break;
        case H_PHASE_SCAN:
            H_Scan(R, Vol);
            break;
        default:
            R->Phase = H_PHASE_DONE;
            break;
    }

} /* End of H_Unit() */

/* The space check, the summary, and the hand-over to the volume */
static void H_Finish(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol)
{
    uint32  Free = EEFS_NandFreeCount(Vol);

    if (Free < EEFS_NAND_RESERVE_BLOCKS) {
        H_Add(R, EEFS_HEALTH_FREE_BELOW_RESERVE, H_NA8, H_NA16, H_NA16, Free, EEFS_NAND_RESERVE_BLOCKS, "");
    }
    R->S.Summary &= ~(uint32)EEFS_HEALTH_INCOMPLETE;
    R->S.Complete = 1;
    R->S.DirSeq = Vol->LastSeq;
    memcpy(&Vol->LastReport, &R->S, sizeof(EEFS_HealthSummary_t));
    Vol->ActiveReport = NULL;

} /* End of H_Finish() */

/* A counted page read (every HAL read of the check goes through here) */
static int32 H_Read(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol, uint32 Block, uint32 Page, uint8 *Data, uint8 *Oob,
                    uint32 *Corrected)
{
    uint32  Bits = 0;
    int32   Status;

    R->StepReads++;
    R->S.PagesRead++;
    Status = EEFS_NandReadPage(Vol, Block, Page, Data, Oob, &Bits);
    if ((Status == EEFS_SUCCESS) && (Bits != 0xFFFFFFFFUL)) {
        if (Data != NULL) {
            R->S.CorrectedBits += Bits;     /* data reads only (an OOB-only marker read reports no data bits) */
        }
    }
    else {
        Bits = 0;
    }
    if (Corrected != NULL) {
        *Corrected = Bits;
    }
    return(Status);

} /* End of H_Read() */

/* The Index-th page that carries the bad-block marker (BadMarkerPages); FALSE past the last one */
static boolean H_MarkerPage(EEFS_InodeTable_t *Vol, uint32 Index, uint32 *Page)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      n = 0;

    if ((Geo->BadMarkerPages & EEFS_NAND_BADMARKER_PAGE0) != 0) {
        if (n++ == Index) { *Page = 0; return(TRUE); }
    }
    if ((Geo->BadMarkerPages & EEFS_NAND_BADMARKER_PAGE1) != 0) {
        if (n++ == Index) { *Page = 1; return(TRUE); }
    }
    if ((Geo->BadMarkerPages & EEFS_NAND_BADMARKER_LAST) != 0) {
        if (n == Index) { *Page = Geo->PagesPerBlock - 1; return(TRUE); }
    }
    return(FALSE);

} /* End of H_MarkerPage() */

/* Checks the next marker page of Block (one OOB-only read, as EEFS_NandHasBadMarker does).  Returns 1 when a marker
 * page was read and is clean, 2 when the block carries a marker that the map does not record (reported), 0 when every
 * marker page has been checked.  A failed OOB read is not reported as a marker: the page 0 read reports the block. */
static int32 H_MarkerUnit(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol, uint32 Block)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      Page;

    if (H_MarkerPage(Vol, R->Marker, &Page) == FALSE) {
        return(0);
    }
    R->Marker++;
    if ((H_Read(R, Vol, Block, Page, NULL, Vol->OobBuf, NULL) == EEFS_SUCCESS) &&
        (Vol->OobBuf[Geo->BadMarkerOffset] != 0xFF)) {
        H_Add(R, EEFS_HEALTH_BADBLOCK_UNRECORDED, H_NA8, H_NA16, Block, 0, 0, "");
        return(2);
    }
    return(1);

} /* End of H_MarkerUnit() */

/* Decodes the metadata page in Vol->PageBuf read from Block (FMT 3.4, as EEFS_NandReadPage0 checks it) */
static int32 H_DecodePage0(EEFS_InodeTable_t *Vol, uint32 Block, EEFS_NandPage0_t *P)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      i;
    uint16                      B;
    uint32                      Crc;

    if ((Block < Geo->DirRegionBlocks) || (Block >= Geo->BlockCount) ||
        (EEFS_NandDecodePage0(Vol->PageBuf, Geo->PageSize, Geo->PagesPerBlock, Block, P) != EEFS_SUCCESS)) {
        return(EEFS_CORRUPT_FILE);
    }
    for (i = 0; i < P->BlockCount; i++) {
        EEFS_NandPage0Block(Vol->PageBuf, i, &B, &Crc);
        if ((B < Geo->DirRegionBlocks) || (B >= Geo->BlockCount)) {
            return(EEFS_CORRUPT_FILE);
        }
    }
    return(EEFS_SUCCESS);

} /* End of H_DecodePage0() */

/* Appends a finding (once per Code, EntryIndex, Slot, Block) and accumulates the summary bits, also for findings that
 * no longer fit (EEFS_HEALTH_OVERFLOW) */
static void H_Add(EEFS_HealthReport_t *R, uint16 Code, uint8 Slot, uint16 EntryIndex, uint32 Block, uint32 Aux0,
                  uint32 Aux1, const char *Path)
{
    EEFS_HealthFinding_t   *F;
    uint32                  Bits;
    uint32                  i;

    switch (Code) {
        case EEFS_HEALTH_FILE_CORRUPT:
        case EEFS_HEALTH_FILE_IO_ERROR:
        case EEFS_HEALTH_BLOCK_NEEDS_SCRUB:
            Bits = EEFS_HEALTH_FILE_ISSUE | EEFS_HEALTH_DEGRADED;
            break;
        case EEFS_HEALTH_AB_A_CORRUPT:
        case EEFS_HEALTH_AB_B_CORRUPT:
        case EEFS_HEALTH_AB_ACTIVE_CORRUPT:
        case EEFS_HEALTH_AB_ACTIVE_MISSING:
        case EEFS_HEALTH_AB_FAILED_FLAG_SET:
            Bits = EEFS_HEALTH_AB_ISSUE | EEFS_HEALTH_DEGRADED;
            break;
        case EEFS_HEALTH_AB_VERSION_MISMATCH:
        case EEFS_HEALTH_AB_FALLBACK_UNAVAILABLE:
        case EEFS_HEALTH_AB_LOAD_MIXED:
            Bits = EEFS_HEALTH_INFO;
            break;
        case EEFS_HEALTH_FREE_BELOW_RESERVE:
            Bits = EEFS_HEALTH_SPACE_ISSUE | ((Aux0 < EEFS_NAND_BADBLOCK_RESERVE) ? EEFS_HEALTH_DEGRADED : EEFS_HEALTH_INFO);
            break;
        default:        /* directory codes 1-6, 30, 32-35 */
            Bits = EEFS_HEALTH_DIR_ISSUE | EEFS_HEALTH_DEGRADED;
            break;
    }
    R->S.Summary |= Bits;

    for (i = 0; i < R->S.FindingCount; i++) {
        F = &R->S.Finding[i];
        if ((F->Code == Code) && (F->EntryIndex == EntryIndex) && (F->Slot == Slot) && (F->Block == (uint16)Block)) {
            return;
        }
    }
    if (R->S.FindingCount >= EEFS_NAND_MAX_HEALTH_FINDINGS) {
        R->S.Overflow++;
        R->S.Summary |= EEFS_HEALTH_OVERFLOW;
        return;
    }
    F = &R->S.Finding[R->S.FindingCount++];
    memset(F, 0, sizeof(*F));
    F->Code = Code;
    F->Slot = Slot;
    F->EntryIndex = EntryIndex;
    F->Block = (uint16)Block;
    F->Aux0 = Aux0;
    F->Aux1 = Aux1;
    strncpy(F->Path, Path, EEFS_NAND_HEALTH_PATH_SIZE - 1);
    F->Path[EEFS_NAND_HEALTH_PATH_SIZE - 1] = '\0';

} /* End of H_Add() */

/* Block is on the pending cleanup list (loaded from the media or added by the batch) */
static boolean H_OnCleanupList(EEFS_InodeTable_t *Vol, uint32 Block)
{
    uint32  i;

    for (i = 0; i < Vol->PendingCleanupCount; i++) {
        if (Vol->PendingCleanup[i] == Block) {
            return(TRUE);
        }
    }
    return(FALSE);

} /* End of H_OnCleanupList() */

/*
 * Phase 1: directory
 */

/* Each directory block that the map does not record as bad: its bad-block markers, then page 0, which makes it a
 * checkpoint candidate of its mirror when it is the first page of a CHECKPOINT (section 5.3) */
static void H_DirScan(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    EEFS_NandRecHdr_t           H;
    uint32                      b = R->BlockIdx;
    int32                       Marker;

    if (b >= Geo->DirRegionBlocks) {
        R->Sub = H_DIR_PICK;
        R->Mirror = 0;
        return;
    }
    if (Vol->Working.Map[b] == EEFS_NAND_MAP_BAD) {
        R->BlockIdx++;
        R->Marker = 0;
        return;
    }
    Marker = H_MarkerUnit(R, Vol, b);
    if (Marker == 1) {
        return;
    }
    if (Marker == 0) {
        if ((H_Read(R, Vol, b, 0, Vol->PageBuf, NULL, NULL) == EEFS_SUCCESS) &&
            (EEFS_NandDecodeRecordPage(Vol->PageBuf, Geo->PageSize, &H) == EEFS_SUCCESS) &&
            (H.RecType == EEFS_NAND_REC_CHECKPOINT) && (H.PageIndex == 0)) {
            R->CandState[b] = H_CAND_PENDING;
            R->CandSeq[b] = H.Seq;
        }
    }
    R->BlockIdx++;
    R->Marker = 0;

} /* End of H_DirScan() */

/* Picks the next candidate of mirror R->Mirror as section 5.3 orders them (highest Seq first, the lower block on a
 * tie; candidates below the chosen checkpoint are not walked).  When none is left: the mirror's chain findings. */
static void H_DirPick(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      Half = Geo->DirRegionBlocks / 2;
    uint8                       m = R->Mirror;
    EEFS_NandChain_t           *C;
    uint32                      Best = 0xFFFFFFFFUL;
    uint32                      b;

    if (m >= 2) {
        R->Sub = H_DIR_MIRRORS;
        return;
    }
    C = &R->Chain[m];
    for (b = m * Half; b < (m + 1) * Half; b++) {
        if (R->CandState[b] != H_CAND_PENDING) continue;
        if ((Best == 0xFFFFFFFFUL) || (R->CandSeq[b] > R->CandSeq[Best])) {
            Best = b;
        }
    }
    if ((Best != 0xFFFFFFFFUL) && ((C->Valid == FALSE) || (R->CandSeq[Best] >= C->CheckpointSeq))) {
        R->WalkBlock = Best;
        R->WalkPage = 0;
        R->WalkSub = 0;
        R->WalkRecSeq = 0;
        R->WalkRecCount = 0;
        R->WalkLast = 0;
        R->WalkCkpt = 0;
        R->WalkValid = FALSE;
        R->WalkTail = 0;
        R->WalkTailPage = 0;
        R->WalkTailSeq = 0;
        R->Sub = H_DIR_WALK;
        return;
    }

    /* the mirror's chain is known: torn checkpoints above it (an interrupted entry into a new block), and the page
     * after the chain (an interrupted commit) */
    if (C->Valid == TRUE) {
        for (b = m * Half; b < (m + 1) * Half; b++) {
            if ((R->CandState[b] == H_CAND_TORN) && (R->CandSeq[b] > C->CheckpointSeq)) {
                H_Add(R, EEFS_HEALTH_DIR_UNCOMMITTED_TXN, m, H_NA16, b, 0, R->CandSeq[b], "");
            }
        }
        if (R->ChainTail[m] != 0) {
            H_Add(R, R->ChainTail[m], m, H_NA16, R->ChainBlock[m], R->ChainTailPage[m],
                  (R->ChainTail[m] == EEFS_HEALTH_DIR_UNCOMMITTED_TXN) ? R->ChainTailSeq[m] : 0, "");
        }
        C->Torn = (uint8)((R->TornCand[m] != 0) || (R->ChainTail[m] != 0));
    }
    R->Mirror++;

} /* End of H_DirPick() */

/* One page of the chain walk of R->WalkBlock (EEFS_NandScanMirror's walk, section 5.3).  When the walk stops after a
 * valid chain, the page where it stopped is classified: erased (nothing), a record whose first page is readable and
 * continues the chain (DIR_UNCOMMITTED_TXN: an interrupted commit), or anything else (DIR_RECORD_CORRUPT). */
static void H_DirWalk(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    EEFS_NandRecHdr_t           H;
    uint32                      p = R->WalkPage + R->WalkSub;
    uint8                       Type = (R->WalkPage == 0) ? EEFS_NAND_REC_CHECKPOINT : EEFS_NAND_REC_TXN;
    boolean                     ReadOk;
    boolean                     Ok;
    const uint8                *Pg = Vol->PageBuf;

    if (p >= Geo->PagesPerBlock) {
        H_DirWalkEnd(R);
        return;
    }
    ReadOk = (boolean)(H_Read(R, Vol, R->WalkBlock, p, Vol->PageBuf, NULL, NULL) == EEFS_SUCCESS);
    Ok = (boolean)((ReadOk == TRUE) && (EEFS_NandDecodeRecordPage(Vol->PageBuf, Geo->PageSize, &H) == EEFS_SUCCESS));

    if (R->WalkSub == 0) {
        if ((Ok == TRUE) && ((H.PageIndex != 0) || (H.RecType != Type) ||
                             ((R->WalkPage > 0) && (H.Seq != (R->WalkLast + 1))) ||
                             ((R->WalkPage + H.PageCount) > Geo->PagesPerBlock))) {
            Ok = FALSE;
        }
        if (Ok == FALSE) {
            if (R->WalkPage > 0) {
                R->WalkTailPage = R->WalkPage;
                if (ReadOk == FALSE) {
                    R->WalkTail = EEFS_HEALTH_DIR_RECORD_CORRUPT;
                }
                else if (EEFS_NandIsErased(Pg, Geo->PageSize) == TRUE) {
                    R->WalkTail = 0;
                }
                else if ((EEFS_NandGet32(&Pg[EEFS_NAND_REC_OFF_MAGIC]) == EEFS_NAND_REC_MAGIC) &&
                         (EEFS_NandGet16(&Pg[EEFS_NAND_REC_OFF_VERSION]) == EEFS_NAND_LAYOUT_VERSION) &&
                         (EEFS_NandGet16(&Pg[EEFS_NAND_REC_OFF_PAGEINDEX]) == 0) &&
                         (EEFS_NandGet32(&Pg[EEFS_NAND_REC_OFF_SEQ]) == (R->WalkLast + 1))) {
                    R->WalkTail = EEFS_HEALTH_DIR_UNCOMMITTED_TXN;      /* the header survived, the record did not */
                    R->WalkTailSeq = R->WalkLast + 1;
                }
                else {
                    R->WalkTail = EEFS_HEALTH_DIR_RECORD_CORRUPT;
                }
            }
            H_DirWalkEnd(R);
            return;
        }
        R->WalkRecSeq = H.Seq;
        R->WalkRecCount = H.PageCount;
        R->WalkSub = 1;
    }
    else {
        if ((Ok == FALSE) || (H.Seq != R->WalkRecSeq) || (H.RecType != Type) || (H.PageIndex != R->WalkSub) ||
            (H.PageCount != R->WalkRecCount)) {
            if (R->WalkPage > 0) {
                R->WalkTail = EEFS_HEALTH_DIR_UNCOMMITTED_TXN;          /* a record cut after its first page */
                R->WalkTailPage = R->WalkPage;
                R->WalkTailSeq = R->WalkRecSeq;
            }
            H_DirWalkEnd(R);
            return;
        }
        R->WalkSub++;
    }

    if (R->WalkSub >= R->WalkRecCount) {
        R->WalkLast = R->WalkRecSeq;
        if (R->WalkPage == 0) {
            R->WalkCkpt = R->WalkRecSeq;
            R->WalkValid = TRUE;
        }
        R->WalkPage += R->WalkRecCount;
        R->WalkSub = 0;
        if (R->WalkPage >= Geo->PagesPerBlock) {
            H_DirWalkEnd(R);
        }
    }

} /* End of H_DirWalk() */

/* End of one candidate's walk: a torn checkpoint, or a chain that becomes the mirror's if it is the longest so far */
static void H_DirWalkEnd(EEFS_HealthReport_t *R)
{
    uint8               m = R->Mirror;
    EEFS_NandChain_t   *C = &R->Chain[m];
    uint32              b = R->WalkBlock;

    if (R->WalkValid == FALSE) {
        R->CandState[b] = H_CAND_TORN;
        R->TornCand[m] = TRUE;
    }
    else {
        R->CandState[b] = H_CAND_WALKED;
        if ((C->Valid == FALSE) || (R->WalkLast > C->LastSeq)) {
            C->Valid = TRUE;
            C->CheckpointSeq = R->WalkCkpt;
            C->LastSeq = R->WalkLast;
            C->NextPage = (uint16)R->WalkPage;
            R->ChainBlock[m] = b;
            R->ChainTail[m] = R->WalkTail;
            R->ChainTailPage[m] = R->WalkTailPage;
            R->ChainTailSeq[m] = R->WalkTailSeq;
        }
    }
    R->Sub = H_DIR_PICK;

} /* End of H_DirWalkEnd() */

/* No reads: a mirror without a chain, mirrors that disagree, a rebuilt directory not yet written */
static void H_DirMirrors(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol)
{
    uint8   m;
    uint8   Lag;

    for (m = 0; m < 2; m++) {
        if (R->Chain[m].Valid == FALSE) {
            H_Add(R, EEFS_HEALTH_DIR_MIRROR_MISSING, m, H_NA16, H_NA16, 0, 0, "");
        }
    }
    if ((R->Chain[0].Valid == TRUE) && (R->Chain[1].Valid == TRUE) && (R->Chain[0].LastSeq != R->Chain[1].LastSeq)) {
        Lag = (R->Chain[0].LastSeq < R->Chain[1].LastSeq) ? 0 : 1;
        H_Add(R, EEFS_HEALTH_DIR_MIRROR_MISMATCH, Lag, H_NA16, H_NA16, R->Chain[Lag].LastSeq, R->Chain[1 - Lag].LastSeq,
              "");
    }
    if (Vol->DirectoryRebuilt != 0) {
        H_Add(R, EEFS_HEALTH_DIR_REBUILT_NOT_PERSISTED, H_NA8, H_NA16, H_NA16, 0, 0, "");
    }
    R->Sub = H_DIR_CLEANUP;
    R->BlockIdx = 0;

} /* End of H_DirMirrors() */

/* The pending cleanup loaded from the media: a listed block that still holds a page 0 (an interrupted cleanup).
 * Blocks added by the current batch are not pending from the media and are not reported. */
static void H_DirCleanup(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      b;

    if ((R->BlockIdx >= Vol->PendingCleanupBase) || (R->BlockIdx >= Vol->PendingCleanupCount)) {
        R->Phase = H_PHASE_FILES;
        R->Sub = H_FILE_SLOT;
        R->EntryIndex = 0;
        R->Slot = 0;
        return;
    }
    b = Vol->PendingCleanup[R->BlockIdx];
    R->BlockIdx++;
    if ((b < Geo->DirRegionBlocks) || (b >= Geo->BlockCount)) {
        return;
    }
    if ((H_Read(R, Vol, b, 0, Vol->PageBuf, NULL, NULL) != EEFS_SUCCESS) ||
        (EEFS_NandIsErased(Vol->PageBuf, Geo->PageSize) == FALSE)) {
        H_Add(R, EEFS_HEALTH_DIR_CLEANUP_PENDING, H_NA8, H_NA16, b, 0, 0, "");
    }

} /* End of H_DirCleanup() */

/*
 * Phase 2: files
 */

/* Moves to the next in-use slot of the Working directory (no reads) */
static void H_FileSlot(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol)
{
    const EEFS_NandEntry_t *E;
    const EEFS_NandGeometry_t *Geo = &Vol->Dev->Geometry;

    for (;;) {
        if (R->EntryIndex >= EEFS_MAX_FILES) {
            R->Sub = H_FILE_MAP;
            R->Block = Geo->DirRegionBlocks;
            return;
        }
        E = &Vol->Working.Entry[R->EntryIndex];
        if (E->InUse == 0) {
            R->SlotResult[R->EntryIndex][0] = H_SLOT_UNUSED;
            R->SlotResult[R->EntryIndex][1] = H_SLOT_UNUSED;
            R->EntryIndex++;
            R->Slot = 0;
            continue;
        }
        if (R->Slot >= 2) {
            R->S.FilesChecked++;
            R->EntryIndex++;
            R->Slot = 0;
            continue;
        }
        if (E->Slot[R->Slot].InUse == 0) {
            R->SlotResult[R->EntryIndex][R->Slot] = H_SLOT_UNUSED;
            R->Slot++;
            continue;
        }
        R->FirstBlock = E->Slot[R->Slot].FirstBlock;
        R->Verdict = H_SLOT_OK;
        R->Sub = H_FILE_PAGE0;
        return;
    }

} /* End of H_FileSlot() */

/* The slot's metadata page: validity, agreement with the directory entry, and the claims of its blocks */
static void H_FilePage0(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol)
{
    const EEFS_NandEntry_t *E = &Vol->Working.Entry[R->EntryIndex];
    const EEFS_NandSlot_t  *S = &E->Slot[R->Slot];
    EEFS_NandPage0_t        P;
    uint32                  Corrected;
    uint32                  Reason = 0;
    uint32                  k;
    uint16                  B;
    uint32                  Crc;
    uint8                   Owner = EEFS_NAND_MAP_OWNER(R->EntryIndex, R->Slot);

    if ((R->FirstBlock < Vol->Dev->Geometry.DirRegionBlocks) || (R->FirstBlock >= Vol->Dev->Geometry.BlockCount)) {
        H_SlotFinding(R, Vol, EEFS_HEALTH_FILE_CORRUPT, R->FirstBlock, 0);      /* a directory naming no data block */
        H_SlotEnd(R, Vol, H_SLOT_CORRUPT);
        return;
    }
    if (H_Read(R, Vol, R->FirstBlock, 0, Vol->PageBuf, NULL, &Corrected) != EEFS_SUCCESS) {
        H_SlotFinding(R, Vol, EEFS_HEALTH_FILE_IO_ERROR, R->FirstBlock, 0);
        H_SlotEnd(R, Vol, H_SLOT_IO_ERROR);
        return;
    }
    if (H_DecodePage0(Vol, R->FirstBlock, &P) != EEFS_SUCCESS) {
        H_SlotFinding(R, Vol, EEFS_HEALTH_FILE_CORRUPT, R->FirstBlock, 0);
        H_SlotEnd(R, Vol, H_SLOT_CORRUPT);
        return;
    }

    /* the directory against the page (the page wins; reasons 1 and 2 mean the page is not this slot's, so the slot
     * fails as it would fail EEFS_NandValidateSlot, Open and the bootloader) */
    if (strncmp(P.Path, E->Path, EEFS_NAND_MAX_PATH_SIZE) != 0) {
        Reason = 1;
    }
    else if ((P.Slot != EEFS_NAND_SLOT_NONE) && (P.Slot != R->Slot)) {
        Reason = 2;
    }
    else if (P.BlockCount != S->AllocBlocks) {
        Reason = 3;
    }
    else if ((P.Version != S->Version) || (P.WriteSeq != S->WriteSeq) || (P.FileSize != S->FileSize) ||
             (P.FileCrc32 != S->FileCrc32) || (P.ModificationDate != S->ModificationDate)) {
        Reason = 4;
    }
    if (Reason != 0) {
        H_SlotFinding(R, Vol, EEFS_HEALTH_DIR_PAGE0_MISMATCH, R->FirstBlock, Reason);
        if (Reason <= 2) {
            R->Verdict = H_SLOT_CORRUPT;
        }
    }

    /* every block it lists: claimed once, and owned by this slot in the map */
    for (k = 0; k < P.BlockCount; k++) {
        EEFS_NandPage0Block(Vol->PageBuf, k, &B, &Crc);
        if (H_CLAIMED(R, B)) {
            H_SlotFinding(R, Vol, EEFS_HEALTH_BLOCK_CROSSLINKED, B, Vol->Working.Map[B]);
            continue;
        }
        H_CLAIM(R, B);
        if (Vol->Working.Map[B] != Owner) {
            H_Add(R, EEFS_HEALTH_MAP_MISMATCH, H_NA8, H_NA16, B, Vol->Working.Map[B], Owner, "");
        }
    }

    /* then the content, block by block */
    EEFS_NandPage0Block(Vol->PageBuf, 0, &B, &Crc);
    R->BlockCount = P.BlockCount;
    R->FileSize = P.FileSize;
    R->FileCrc32 = P.FileCrc32;
    R->K = 0;
    R->Block = B;
    R->ExpectBlockCrc = Crc;
    R->RunBlockCrc = EEFS_CRC32_INIT;
    R->RunFileCrc = EEFS_CRC32_INIT;
    R->MaxCorrectedInBlock = Corrected;
    R->Page = 1;
    R->DataOffset = 0;
    R->Sub = H_FILE_DATA;

} /* End of H_FilePage0() */

/* The metadata page again at a block transition, for Blocks[K] */
static void H_FileBlock(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol)
{
    EEFS_NandPage0_t    P;
    uint16              B;
    uint32              Crc;

    if (H_Read(R, Vol, R->FirstBlock, 0, Vol->PageBuf, NULL, NULL) != EEFS_SUCCESS) {
        H_SlotFinding(R, Vol, EEFS_HEALTH_FILE_IO_ERROR, R->FirstBlock, 0);
        H_SlotEnd(R, Vol, H_SLOT_IO_ERROR);
        return;
    }
    if ((H_DecodePage0(Vol, R->FirstBlock, &P) != EEFS_SUCCESS) || (P.BlockCount != R->BlockCount)) {
        H_SlotFinding(R, Vol, EEFS_HEALTH_FILE_CORRUPT, R->FirstBlock, 0);
        H_SlotEnd(R, Vol, H_SLOT_CORRUPT);
        return;
    }
    EEFS_NandPage0Block(Vol->PageBuf, R->K, &B, &Crc);
    R->Block = B;
    R->ExpectBlockCrc = Crc;
    R->RunBlockCrc = EEFS_CRC32_INIT;
    R->MaxCorrectedInBlock = 0;
    R->Page = 0;
    R->DataOffset = 0;
    R->Sub = H_FILE_DATA;

} /* End of H_FileBlock() */

/* One data page of Blocks[K], or the end of the block (its CRC, the scrub threshold) and of the file (its CRC) */
static void H_FileData(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      Len;
    uint32                      n;
    uint32                      Corrected;

    Len = EEFS_NandBlockDataRange(Geo->PageSize, Geo->PagesPerBlock, R->FileSize, R->K, NULL);
    if (R->DataOffset < Len) {
        if (H_Read(R, Vol, R->Block, R->Page, Vol->PageBuf, NULL, &Corrected) != EEFS_SUCCESS) {
            H_SlotFinding(R, Vol, EEFS_HEALTH_FILE_IO_ERROR, R->Block, R->Page);
            H_SlotEnd(R, Vol, H_SLOT_IO_ERROR);
            return;
        }
        if (Corrected > R->MaxCorrectedInBlock) {
            R->MaxCorrectedInBlock = Corrected;
        }
        n = Len - R->DataOffset;
        if (n > Geo->PageSize) n = Geo->PageSize;
        EEFS_Crc32Update2(&R->RunBlockCrc, &R->RunFileCrc, Vol->PageBuf, n);
        R->DataOffset += n;
        R->Page++;
        return;
    }

    /* end of the block */
    if ((Geo->EccScrubThreshold > 0) && (R->MaxCorrectedInBlock >= Geo->EccScrubThreshold)) {
        H_SlotFinding(R, Vol, EEFS_HEALTH_BLOCK_NEEDS_SCRUB, R->Block, R->MaxCorrectedInBlock);
    }
    if (EEFS_Crc32Final(R->RunBlockCrc) != R->ExpectBlockCrc) {
        H_SlotFinding(R, Vol, EEFS_HEALTH_FILE_CORRUPT, R->Block, 1);
        H_SlotEnd(R, Vol, H_SLOT_CORRUPT);
        return;
    }
    R->K++;
    if (R->K < R->BlockCount) {
        R->Sub = H_FILE_BLOCK;
        return;
    }

    /* end of the file */
    if (EEFS_Crc32Final(R->RunFileCrc) != R->FileCrc32) {
        H_SlotFinding(R, Vol, EEFS_HEALTH_FILE_CORRUPT, R->FirstBlock, 2);
        H_SlotEnd(R, Vol, H_SLOT_CORRUPT);
        return;
    }
    H_SlotEnd(R, Vol, R->Verdict);

} /* End of H_FileData() */

/* No reads: every data block the map gives to an owner must be claimed by a valid metadata page; DIR is only valid in
 * the directory region */
static void H_FileMap(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      b;
    uint8                       v;

    for (b = Geo->DirRegionBlocks; b < Geo->BlockCount; b++) {
        v = Vol->Working.Map[b];
        if ((EEFS_NAND_MAP_IS_OWNER(v) && !H_CLAIMED(R, b)) || (v == EEFS_NAND_MAP_DIR)) {
            H_Add(R, EEFS_HEALTH_MAP_MISMATCH, H_NA8, H_NA16, b, v, H_NA16, "");
        }
    }
    R->Phase = H_PHASE_AB;
    R->EntryIndex = 0;

} /* End of H_FileMap() */

/* A slot is done: its result, and for a failure the flags of a failed validation (EEFS_NandSlotFailed: SLOTFLAG_CORRUPT
 * and ActiveFailed for the active copy of an A/B entry, recorded in Base too with ForceDirty so the next commit
 * persists it).  The check does not count its own flagging as a volume change (it would restart itself). */
static void H_SlotEnd(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol, uint8 Result)
{
    const EEFS_NandEntry_t *E = &Vol->Working.Entry[R->EntryIndex];
    uint32                  Saved;

    R->SlotResult[R->EntryIndex][R->Slot] = Result;
    R->S.SlotsChecked++;
    if ((Result == H_SLOT_CORRUPT) || (Result == H_SLOT_IO_ERROR)) {
        if ((E->IsAB != 0) && (E->ActiveSlot == R->Slot) && (E->ActiveFailed == 0)) {
            EEFS_NAND_MASK_SET(R->SetByCheck, R->EntryIndex);
        }
        Saved = Vol->VolumeChangeCounter;
        EEFS_NandSlotFailed(Vol, R->EntryIndex, R->Slot);
        Vol->VolumeChangeCounter = Saved;
    }
    else {
        Vol->SlotFlags[R->EntryIndex][R->Slot] &= (uint8)~EEFS_NAND_SLOTFLAG_CORRUPT;
    }
    R->Slot++;
    R->Sub = H_FILE_SLOT;

} /* End of H_SlotEnd() */

/* A finding about the current slot */
static void H_SlotFinding(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol, uint16 Code, uint32 Block, uint32 Aux0)
{
    H_Add(R, Code, R->Slot, R->EntryIndex, Block, Aux0, 0, Vol->Working.Entry[R->EntryIndex].Path);

} /* End of H_SlotFinding() */

/*
 * Phase 3: A/B (no reads)
 */

static void H_AB(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol)
{
    const EEFS_NandEntry_t *E;
    uint32                  i;
    uint8                   s;
    uint8                   Act;
    uint16                  Idx;

    for (i = 0; i < EEFS_MAX_FILES; i++) {
        E = &Vol->Working.Entry[i];
        if ((E->InUse == 0) || (E->IsAB == 0)) continue;
        Idx = (uint16)i;
        Act = (uint8)((E->ActiveSlot == EEFS_NAND_SLOT_B) ? EEFS_NAND_SLOT_B : EEFS_NAND_SLOT_A);
        for (s = 0; s < 2; s++) {
            if ((R->SlotResult[i][s] == H_SLOT_CORRUPT) || (R->SlotResult[i][s] == H_SLOT_IO_ERROR)) {
                H_Add(R, (s == EEFS_NAND_SLOT_A) ? EEFS_HEALTH_AB_A_CORRUPT : EEFS_HEALTH_AB_B_CORRUPT, s, Idx,
                      E->Slot[s].FirstBlock, 0, 0, E->Path);
            }
        }
        if ((R->SlotResult[i][Act] == H_SLOT_CORRUPT) || (R->SlotResult[i][Act] == H_SLOT_IO_ERROR)) {
            /* ActiveFailed was set by the failed validation in phase 2 (H_SlotEnd) */
            H_Add(R, EEFS_HEALTH_AB_ACTIVE_CORRUPT, Act, Idx, E->Slot[Act].FirstBlock, 0, 0, E->Path);
        }
        if (E->Slot[Act].InUse == 0) {
            H_Add(R, EEFS_HEALTH_AB_ACTIVE_MISSING, Act, Idx, H_NA16, 0, 0, E->Path);
        }
        if ((E->Slot[0].InUse != 0) && (E->Slot[1].InUse != 0) && (E->Slot[0].Version != E->Slot[1].Version)) {
            H_Add(R, EEFS_HEALTH_AB_VERSION_MISMATCH, H_NA8, Idx, H_NA16, E->Slot[0].Version, E->Slot[1].Version, E->Path);
        }
        if ((E->Slot[1 - Act].InUse == 0) || (E->FallbackCompatible == 0)) {
            H_Add(R, EEFS_HEALTH_AB_FALLBACK_UNAVAILABLE, (uint8)(1 - Act), Idx, H_NA16, 0, 0, E->Path);
        }
        if (EEFS_NAND_MASK_TEST(R->FailedAtStart, i)) {
            H_Add(R, EEFS_HEALTH_AB_FAILED_FLAG_SET, Act, Idx, H_NA16, 0, 0, E->Path);
        }
        if (R->FirstActive == H_NA8) {
            R->FirstActive = E->ActiveSlot;
        }
        else if ((R->FirstActive != H_MIXED) && (E->ActiveSlot != R->FirstActive)) {
            H_Add(R, EEFS_HEALTH_AB_LOAD_MIXED, E->ActiveSlot, Idx, H_NA16, R->FirstActive, E->ActiveSlot, E->Path);
            R->FirstActive = H_MIXED;       /* one finding for the volume */
        }
    }
    R->Phase = H_PHASE_SCAN;
    R->Block = Vol->Dev->Geometry.DirRegionBlocks;
    R->Marker = 0;

} /* End of H_AB() */

/*
 * Phase 4: scan of the data area
 */

/* Each data block the map does not record as bad: its bad-block markers, then (unless a valid page 0 claims it or it
 * is on the pending cleanup list) page 0, which is an orphan when it is a valid metadata page */
static void H_Scan(EEFS_HealthReport_t *R, EEFS_InodeTable_t *Vol)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    EEFS_NandPage0_t            P;
    uint32                      b = R->Block;
    int32                       Marker;

    if (b >= Geo->BlockCount) {
        R->Phase = H_PHASE_DONE;
        return;
    }
    if (Vol->Working.Map[b] == EEFS_NAND_MAP_BAD) {
        R->Block++;
        R->Marker = 0;
        return;
    }
    Marker = H_MarkerUnit(R, Vol, b);
    if (Marker == 1) {
        return;
    }
    if ((Marker == 0) && !H_CLAIMED(R, b) && (H_OnCleanupList(Vol, b) == FALSE)) {
        if ((H_Read(R, Vol, b, 0, Vol->PageBuf, NULL, NULL) == EEFS_SUCCESS) &&
            (H_DecodePage0(Vol, b, &P) == EEFS_SUCCESS)) {
            H_Add(R, EEFS_HEALTH_ORPHAN_FILE, P.Slot, H_NA16, b, P.WriteSeq, 0, P.Path);
        }
    }
    R->Block++;
    R->Marker = 0;

} /* End of H_Scan() */

/************************/
/*  End of File Comment */
/************************/
