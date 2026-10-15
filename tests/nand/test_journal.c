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
 * Filename: test_journal.c
 *
 * Purpose: Suite "journal": the journaled directory of the NAND backend (docs/nand-implementation.md sections 5.1-5.6,
 *   phase 2): format, mount, commit, abort, the mirror rings, resync, torn records, bad directory blocks, the pending
 *   cleanup list and the power-cut sweeps.
 *
 * Design Notes:
 *   The file operations are phase 3, so the tests simulate them the way the spec allows: they set entries and map
 *   bytes in Working directly (through eefs_nand_internal.h) and write a fake but valid metadata page with the
 *   encoders where a slot needs one.
 *
 *   A power-cut sweep runs the operation under test once per cut point (every program and erase it issues) and per
 *   torn mode, each time on a fresh clone of the same image: cut, reboot, remount (which must not program or erase),
 *   then the directory must be exactly the state before or exactly the state after the operation (and never go back
 *   to "before" once a smaller cut gave "after"), every slot must validate, a follow-up commit must succeed with both
 *   mirrors agreeing, and the blocks that were due for cleanup must be erased iff they are free.
 *
 */

/*
 * Includes
 */

#include <stdlib.h>
#include "nand_test.h"
#include "eefs_nand_internal.h"

/*
 * Macro Definitions
 */

#define J_FOLLOW_ENTRY      (EEFS_MAX_FILES - 1)    /* entry the follow-up commit of a sweep creates */
#define J_MAX_CLEANUP       8

/*
 * Type Definitions
 */

/* Directory state compared by the tests: entries, data-region map, committed Seq */
typedef struct
{
    EEFS_NandEntry_t    Entry[EEFS_MAX_FILES];
    uint8               Map[EEFS_NAND_MAX_BLOCKS];
    uint32              LastSeq;
} J_Snap_t;

typedef void  (*J_Batch_t)(void);
typedef int32 (*J_Run_t)(void);

/*
 * Local Data
 */

static EEFS_InodeTable_t    J_Vol;          /* the volume under test */
static EEFS_InodeTable_t    J_Aux;          /* format scratch, second mount */
static EEFS_NandDevice_t    J_Dev;
static NandSim_t           *J_Sim;
static EEFS_NandDirState_t  J_StateA;
static EEFS_NandDirState_t  J_StateB;
static J_Snap_t             J_Pre;
static J_Snap_t             J_Post;
static J_Snap_t             J_Cur;
static J_Snap_t             J_Tmp;
static uint8                J_Page[EEFS_NAND_MAX_PAGE_SIZE];
static uint16               J_Blocks[1200];
static uint32               J_Crcs[1200];

/* Per-sweep parameters set by the scenario before J_Sweep */
static uint16               J_Cleanup[J_MAX_CLEANUP];      /* blocks whose page 0 must be erased once they are free */
static uint32               J_CleanupCount;
static uint32               J_File[8][3];                   /* scenario files: First, Count, WriteSeq */

/*
 * Helpers
 */

static uint32 J_DataStart(void)
{
    return(NandTest_Geo.DirRegionBlocks + 4);
}

static void J_Attach(NandSim_t *Sim)
{
    J_Sim = Sim;
    NandSim_FillDevice(Sim, &J_Dev);
}

static int32 J_Format(void)
{
    memset(&J_Aux, 0, sizeof(J_Aux));
    return(EEFS_LibFormat(&J_Aux, (cpuaddr)&J_Dev));
}

static int32 J_Mount(EEFS_InodeTable_t *Vol)
{
    memset(Vol, 0, sizeof(*Vol));
    return(EEFS_LibInitFS(Vol, (cpuaddr)&J_Dev));
}

/* New simulator of the run geometry, formatted and mounted on J_Vol */
static void J_NewVolume(void)
{
    J_Attach(NandTest_NewSim());
    CHECK_RC(J_Format(), EEFS_SUCCESS);
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
}

static void J_Destroy(void)
{
    memset(&J_Vol, 0, sizeof(J_Vol));
    memset(&J_Aux, 0, sizeof(J_Aux));
    if (J_Sim != NULL) {
        NandSim_Destroy(J_Sim);
        J_Sim = NULL;
    }
}

/* Entry i in use in Working, no slots */
static void J_SetEntry(EEFS_InodeTable_t *Vol, uint32 i, const char *Path)
{
    EEFS_NandEntry_t   *E = &Vol->Working.Entry[i];

    memset(E, 0, sizeof(*E));
    strncpy(E->Path, Path, EEFS_NAND_MAX_PATH_SIZE - 1);
    E->InUse = 1;
    E->FileType = EEFS_FILE_TYPE_OTHER;
    E->CreationDate = 100 + i;
}

/* A valid metadata page for (Path, Count blocks from First) at page 0 of First, through the HAL */
static void J_WritePage0(const char *Path, uint32 First, uint32 Count, uint32 WriteSeq, uint32 Tag)
{
    EEFS_NandPage0_t    P;
    uint32              k;

    memset(&P, 0, sizeof(P));
    P.WriteSeq = WriteSeq;
    strncpy(P.Path, Path, EEFS_NAND_MAX_PATH_SIZE - 1);
    P.FileType = EEFS_FILE_TYPE_OTHER;
    P.Slot = EEFS_NAND_SLOT_NONE;
    P.Version = 7 + Tag;
    P.FileSize = Count * NandTest_Geo.PageSize * NandTest_Geo.PagesPerBlock - NandTest_Geo.PageSize;
    P.FileCrc32 = 0x1000 + Tag;
    P.CreationDate = 100;
    P.ModificationDate = 200 + Tag;
    P.BlockCount = Count;
    for (k = 0; k < Count; k++) {
        J_Blocks[k] = (uint16)(First + k);
        J_Crcs[k] = 0;
    }
    CHECK_RC(EEFS_NandEncodePage0(J_Page, NandTest_Geo.PageSize, &P, J_Blocks, J_Crcs), EEFS_SUCCESS);
    CHECK_RC(J_Dev.Ops->EraseBlock(J_Dev.Ctx, First), 0);
    CHECK_RC(J_Dev.Ops->ProgramPage(J_Dev.Ctx, First, 0, J_Page, NULL), 0);
}

/* Slot 0 of entry i owns Count blocks from First, with the copied fields of J_WritePage0 */
static void J_ClaimSlot(EEFS_InodeTable_t *Vol, uint32 i, uint32 First, uint32 Count, uint32 WriteSeq, uint32 Tag)
{
    EEFS_NandSlot_t    *S = &Vol->Working.Entry[i].Slot[0];
    uint32              k;

    memset(S, 0, sizeof(*S));
    S->InUse = 1;
    S->FirstBlock = (uint16)First;
    S->AllocBlocks = (uint16)Count;
    S->FileSize = Count * NandTest_Geo.PageSize * NandTest_Geo.PagesPerBlock - NandTest_Geo.PageSize;
    S->Version = 7 + Tag;
    S->WriteSeq = WriteSeq;
    S->FileCrc32 = 0x1000 + Tag;
    S->ModificationDate = 200 + Tag;
    for (k = 0; k < Count; k++) {
        Vol->Working.Map[First + k] = EEFS_NAND_MAP_OWNER(i, 0);
    }
}

/* A complete synthetic file: entry i, path, metadata page and map */
static void J_AddFile(EEFS_InodeTable_t *Vol, uint32 i, const char *Path, uint32 First, uint32 Count)
{
    uint32  Seq = Vol->NextWriteSeq++;

    J_SetEntry(Vol, i, Path);
    J_WritePage0(Path, First, Count, Seq, i);
    J_ClaimSlot(Vol, i, First, Count, Seq, i);
}

/* Removes entry i in the batch (as EEFS_LibRemove will) */
static void J_RemoveEntry(EEFS_InodeTable_t *Vol, uint32 i)
{
    uint8   s;

    for (s = 0; s < 2; s++) {
        if (Vol->Working.Entry[i].Slot[s].InUse) {
            EEFS_NandFreeSlot(Vol, i, s);
        }
    }
    memset(&Vol->Working.Entry[i], 0, sizeof(EEFS_NandEntry_t));
}

static void J_Take(const EEFS_InodeTable_t *Vol, J_Snap_t *S)
{
    uint32  b;

    memset(S, 0, sizeof(*S));
    memcpy(S->Entry, Vol->Working.Entry, sizeof(S->Entry));
    for (b = NandTest_Geo.DirRegionBlocks; b < NandTest_Geo.BlockCount; b++) {
        S->Map[b] = (Vol->Working.Map[b] == EEFS_NAND_MAP_FREEING) ? EEFS_NAND_MAP_FREE : Vol->Working.Map[b];
    }
    S->LastSeq = Vol->LastSeq;
}

static int J_Same(const J_Snap_t *A, const J_Snap_t *B)
{
    return(memcmp(A, B, sizeof(*A)) == 0);
}

static uint32 J_EntriesInUse(const EEFS_InodeTable_t *Vol)
{
    uint32  i;
    uint32  n = 0;

    for (i = 0; i < EEFS_MAX_FILES; i++) {
        if (Vol->Working.Entry[i].InUse) n++;
    }
    return(n);
}

/* Both mirrors hold a valid chain ending at the volume's Seq, and replaying either gives the committed state */
static void J_CheckMirrorsAgree(EEFS_InodeTable_t *Vol)
{
    EEFS_NandChain_t    C[2];
    static uint16       Cl[EEFS_NAND_MAX_CLEANUP_BLOCKS];
    uint16              N;
    uint32              MaxFiles;
    uint32              Dir = NandTest_Geo.DirRegionBlocks;
    uint32              Data = NandTest_Geo.BlockCount - Dir;

    CHECK_RC(EEFS_NandScanMirror(Vol, 0, &C[0]), EEFS_SUCCESS);
    CHECK_RC(EEFS_NandScanMirror(Vol, 1, &C[1]), EEFS_SUCCESS);
    CHECK(C[0].Valid && C[1].Valid);
    if (!(C[0].Valid && C[1].Valid)) return;
    CHECK_EQ(C[0].LastSeq, Vol->LastSeq);
    CHECK_EQ(C[1].LastSeq, Vol->LastSeq);
    CHECK_RC(EEFS_NandApplyChain(Vol, 0, &C[0], &J_StateA, Cl, &N, &MaxFiles), EEFS_SUCCESS);
    CHECK_RC(EEFS_NandApplyChain(Vol, 1, &C[1], &J_StateB, Cl, &N, &MaxFiles), EEFS_SUCCESS);
    CHECK(memcmp(J_StateA.Entry, J_StateB.Entry, sizeof(J_StateA.Entry)) == 0);
    CHECK(memcmp(&J_StateA.Map[Dir], &J_StateB.Map[Dir], Data) == 0);
    CHECK(memcmp(J_StateA.Entry, Vol->Base.Entry, sizeof(J_StateA.Entry)) == 0);
    CHECK(memcmp(&J_StateA.Map[Dir], &Vol->Base.Map[Dir], Data) == 0);
    CHECK_EQ(J_StateA.WriteSeqHigh, Vol->Base.WriteSeqHigh);
}

/* A second mount of the same device agrees with the RAM state of Vol */
static void J_CheckRemountAgrees(EEFS_InodeTable_t *Vol)
{
    J_Take(Vol, &J_Cur);
    CHECK_RC(J_Mount(&J_Aux), EEFS_SUCCESS);
    J_Take(&J_Aux, &J_Tmp);
    CHECK(J_Same(&J_Cur, &J_Tmp));
    memset(&J_Aux, 0, sizeof(J_Aux));
}

/* Commits a one-entry change (entry i's CreationDate) */
static int32 J_TouchCommit(EEFS_InodeTable_t *Vol, uint32 i)
{
    if (Vol->Working.Entry[i].InUse == 0) {
        J_SetEntry(Vol, i, "/touched");
    }
    Vol->Working.Entry[i].CreationDate++;
    return(EEFS_LibCommit(Vol));
}

/* Ops (programs + erases) of the simulator so far */
static uint32 J_Ops(void)
{
    return(NandSim_Programs(J_Sim) + NandSim_Erases(J_Sim));
}

/* Reads page p of directory block b and decodes its record header */
static int32 J_RecHdr(uint32 b, uint32 p, EEFS_NandRecHdr_t *H)
{
    NandSim_RawRead(J_Sim, b, p, J_Page, NULL);
    return(EEFS_NandDecodeRecordPage(J_Page, NandTest_Geo.PageSize, H));
}

/* Lists the items of the record starting at (b, p): bit i of *Entries for ENTRY i (first 64), map coverage counted,
 * cleanup blocks copied.  Returns the page count, 0 if invalid. */
static uint32 J_RecItems(uint32 b, uint32 p, uint8 *EntryMask, uint32 *MapBytes, uint32 *VolumeItems,
                         uint16 *Cleanup, uint32 *CleanupCount, uint8 *MapOut)
{
    EEFS_NandRecHdr_t   H;
    uint32              q;
    uint32              Count;
    uint32              Cursor;
    uint8               Type;
    const uint8        *Body;
    uint16              Len;
    uint16              Index;
    uint32              Start;
    uint32              N;
    uint32              k;

    if (J_RecHdr(b, p, &H) != EEFS_SUCCESS) return(0);
    Count = H.PageCount;
    memset(EntryMask, 0, EEFS_NAND_MASK_BYTES);
    *MapBytes = 0;
    *VolumeItems = 0;
    *CleanupCount = 0;
    for (q = 0; q < Count; q++) {
        if (J_RecHdr(b, p + q, &H) != EEFS_SUCCESS) return(0);
        Cursor = 0;
        while (EEFS_NandNextItem(&J_Page[EEFS_NAND_REC_HDR_SIZE], H.PayloadLen, &Cursor, &Type, &Body, &Len) == 1) {
            if (Type == EEFS_NAND_ITEM_VOLUME) {
                (*VolumeItems)++;
            }
            else if (Type == EEFS_NAND_ITEM_ENTRY) {
                Index = EEFS_NandGet16(Body);
                if (Index < EEFS_MAX_FILES) EEFS_NAND_MASK_SET(EntryMask, Index);
            }
            else if (Type == EEFS_NAND_ITEM_MAPCHUNK) {
                Start = EEFS_NandGet32(&Body[EEFS_NAND_MAP_OFF_START]);
                N = EEFS_NandGet32(&Body[EEFS_NAND_MAP_OFF_COUNT]);
                *MapBytes += N;
                for (k = 0; (MapOut != NULL) && (k < N) && ((Start + k) < EEFS_NAND_MAX_BLOCKS); k++) {
                    MapOut[Start + k] = Body[EEFS_NAND_MAP_OFF_BYTES + k];
                }
            }
            else if (Type == EEFS_NAND_ITEM_CLEANUP) {
                N = EEFS_NandGet16(Body);
                for (k = 0; (k < N) && (k < J_MAX_CLEANUP); k++) {
                    Cleanup[k] = EEFS_NandGet16(&Body[EEFS_NAND_CLN_OFF_BLOCKS + 2 * k]);
                }
                *CleanupCount = N;
            }
        }
    }
    return(Count);
}

/* Pages of the TXN the next commit of Vol would write */
static uint32 J_TxnPages(EEFS_InodeTable_t *Vol)
{
    uint8   Mask[EEFS_NAND_MASK_BYTES];
    uint32  MapChanged;

    EEFS_NandDiff(Vol, Mask, &MapChanged);
    return(EEFS_NandRecordPages(Vol, EEFS_NAND_REC_TXN, &Vol->Working, &Vol->Base, Mask, Vol->PendingCleanupCount));
}

/*
 * Power-cut sweep
 */

static const char *J_ModeName[3] = { "NONE", "PARTIAL", "GARBAGE" };

/* After a follow-up commit: blocks due for cleanup are erased iff they are free, and hold a page 0 iff owned */
static void J_CheckCleanupDone(EEFS_InodeTable_t *Vol)
{
    uint32  i;
    uint16  b;

    for (i = 0; i < J_CleanupCount; i++) {
        b = J_Cleanup[i];
        if (Vol->Working.Map[b] == EEFS_NAND_MAP_FREE) {
            CHECK(NandSim_PageIsErased(J_Sim, b, 0));
        }
        else {
            CHECK(EEFS_NAND_MAP_IS_OWNER(Vol->Working.Map[b]));
            CHECK(!NandSim_PageIsErased(J_Sim, b, 0));
        }
    }
}

/* Every in-use slot validated at mount */
static void J_CheckSlotsValid(EEFS_InodeTable_t *Vol)
{
    uint32  i;
    uint32  s;

    for (i = 0; i < EEFS_MAX_FILES; i++) {
        for (s = 0; s < 2; s++) {
            if (Vol->Working.Entry[i].InUse && Vol->Working.Entry[i].Slot[s].InUse) {
                CHECK_EQ(Vol->SlotFlags[i][s], 0);
            }
        }
    }
}

/* Runs Run on a volume mounted from Image with Batch applied, once per cut point and torn mode.  J_Pre and J_Post
 * must hold the expected states.  Prints and returns the number of cut points of the uninterrupted run (per mode). */
static uint32 J_Sweep(const char *Name, const NandSim_t *Image, J_Batch_t Batch, J_Run_t Run)
{
    NandSim_t  *Sim;
    uint32      Cut;
    uint32      Cuts = 0;
    uint32      Ops;
    int         Mode;
    int         Done;
    int         SeenPost;
    int         IsPre;
    int         IsPost;
    int32       Status;

    for (Mode = NANDSIM_TORN_NONE; Mode <= NANDSIM_TORN_GARBAGE; Mode++) {
        SeenPost = 0;
        for (Cut = 0; ; Cut++) {
            Sim = NandSim_Clone(Image);
            CHECK(Sim != NULL);
            if (Sim == NULL) return(0);
            J_Attach(Sim);
            CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
            if (Batch != NULL) Batch();

            NandSim_SetPowerBudget(Sim, Cut, Mode);
            Status = Run();
            Done = !NandSim_PowerCutHappened(Sim);
            if (Done) {
                CHECK_RC(Status, EEFS_SUCCESS);
                J_Take(&J_Vol, &J_Cur);
                CHECK(J_Same(&J_Cur, &J_Post));
            }
            NandSim_Reboot(Sim);

            /* remount: reads only */
            Ops = J_Ops();
            Status = J_Mount(&J_Vol);
            CHECK_RC(Status, EEFS_SUCCESS);
            CHECK_EQ(J_Ops(), Ops);
            if (Status == EEFS_SUCCESS) {
                J_Take(&J_Vol, &J_Cur);
                IsPre = J_Same(&J_Cur, &J_Pre);
                IsPost = J_Same(&J_Cur, &J_Post);
                CHECK(IsPre || IsPost);
                if (Done) CHECK(IsPost);
                if (IsPost && !IsPre) SeenPost = 1;
                else if (SeenPost) CHECK(!IsPre);      /* never back to "before" after an "after" */
                J_CheckSlotsValid(&J_Vol);

                /* the volume keeps working: a follow-up commit, both mirrors agree, cleanup finished */
                CHECK_RC(J_TouchCommit(&J_Vol, J_FOLLOW_ENTRY), EEFS_SUCCESS);
                CHECK_EQ(J_Vol.PendingCleanupCount, 0);
                J_CheckCleanupDone(&J_Vol);
                J_CheckMirrorsAgree(&J_Vol);
            }
            CHECK_EQ(NandSim_Violations(Sim), 0);

            if (NandTest_CaseFailed()) {
                printf("    sweep %s: failure at cut %lu, torn mode %s\n", Name, (unsigned long)Cut, J_ModeName[Mode]);
                J_Destroy();
                return(Cut);
            }
            J_Destroy();
            if (Done) {
                Cuts = Cut;
                break;
            }
        }
    }
    printf("    sweep %s: %lu cut points x 3 torn modes\n", Name, (unsigned long)Cuts);
    return(Cuts);
}

/* Double fault: for every cut point c1 of Run, reboot, then cut the recovery commit (a one-entry change) at every
 * point c2 as well.  After the second reboot the state must be exactly the state found after the first one, or that
 * state plus the change; then a third commit must work and the mirrors agree.  Returns the number of (c1, c2) pairs
 * per torn mode. */
static uint32 J_SweepTwice(const char *Name, const NandSim_t *Image, J_Batch_t Batch, J_Run_t Run)
{
    static J_Snap_t Mid;
    static J_Snap_t MidPost;
    NandSim_t      *First;
    NandSim_t      *Sim;
    uint32          C1;
    uint32          C2;
    uint32          Pairs = 0;
    uint32          Total;
    int             Mode;
    int             Done1;
    int             Done2;
    int32           Status;

    for (Mode = NANDSIM_TORN_NONE; Mode <= NANDSIM_TORN_GARBAGE; Mode++) {
        Total = 0;
        for (C1 = 0; ; C1++) {
            First = NandSim_Clone(Image);
            J_Attach(First);
            CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
            if (Batch != NULL) Batch();
            NandSim_SetPowerBudget(First, C1, Mode);
            (void)Run();
            Done1 = !NandSim_PowerCutHappened(First);
            NandSim_Reboot(First);
            memset(&J_Vol, 0, sizeof(J_Vol));
            if (Done1) {
                NandSim_Destroy(First);
                J_Sim = NULL;
                break;
            }

            for (C2 = 0; ; C2++) {
                Sim = NandSim_Clone(First);
                J_Attach(Sim);
                CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
                J_Take(&J_Vol, &Mid);
                if (J_Vol.Working.Entry[J_FOLLOW_ENTRY].InUse == 0) J_SetEntry(&J_Vol, J_FOLLOW_ENTRY, "/touched");
                J_Vol.Working.Entry[J_FOLLOW_ENTRY].CreationDate++;
                J_Take(&J_Vol, &MidPost);
                MidPost.LastSeq = Mid.LastSeq + 1;
                NandSim_SetPowerBudget(Sim, C2, Mode);
                Status = EEFS_LibCommit(&J_Vol);
                Done2 = !NandSim_PowerCutHappened(Sim);
                if (Done2) CHECK_RC(Status, EEFS_SUCCESS);
                NandSim_Reboot(Sim);

                CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
                J_Take(&J_Vol, &J_Cur);
                CHECK(J_Same(&J_Cur, &Mid) || J_Same(&J_Cur, &MidPost));
                if (Done2) CHECK(J_Same(&J_Cur, &MidPost));
                J_CheckSlotsValid(&J_Vol);
                CHECK_RC(J_TouchCommit(&J_Vol, J_FOLLOW_ENTRY - 1), EEFS_SUCCESS);
                CHECK_EQ(J_Vol.PendingCleanupCount, 0);
                J_CheckCleanupDone(&J_Vol);
                J_CheckMirrorsAgree(&J_Vol);
                CHECK_EQ(NandSim_Violations(Sim), 0);

                memset(&J_Vol, 0, sizeof(J_Vol));
                NandSim_Destroy(Sim);
                J_Sim = NULL;
                Total++;
                if (NandTest_CaseFailed()) {
                    printf("    double sweep %s: failure at cuts %lu then %lu, torn mode %s\n", Name,
                           (unsigned long)C1, (unsigned long)C2, J_ModeName[Mode]);
                    NandSim_Destroy(First);
                    return(Total);
                }
                if (Done2) break;
            }
            NandSim_Destroy(First);
            J_Sim = NULL;
        }
        Pairs = Total;
    }
    printf("    double sweep %s: %lu cut pairs x 3 torn modes\n", Name, (unsigned long)Pairs);
    return(Pairs);
}

/* Builds the expected post state by running Batch + Run once on a clone of Image (J_Pre from the mount) */
static void J_Expect(const NandSim_t *Image, J_Batch_t Batch, J_Run_t Run)
{
    J_Attach(NandSim_Clone(Image));
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    J_Take(&J_Vol, &J_Pre);
    if (Batch != NULL) Batch();
    CHECK_RC(Run(), EEFS_SUCCESS);
    J_Take(&J_Vol, &J_Post);
    CHECK(!J_Same(&J_Pre, &J_Post));
    J_Destroy();
}

static int32 J_RunCommit(void)
{
    return(EEFS_LibCommit(&J_Vol));
}

static int32 J_RunRepair(void)
{
    return(EEFS_LibRepairDirectory(&J_Vol));
}

/* The standard batch: modify entry 0, remove entry 1 (cleanup of its first block), add entry 2 (its metadata page was
 * written into the image beforehand) */
static void J_Batch3(void)
{
    J_Vol.Working.Entry[0].Attributes ^= EEFS_ATTRIBUTE_READONLY;
    J_RemoveEntry(&J_Vol, 1);
    J_SetEntry(&J_Vol, 2, "/cf/apps/new.so");
    J_ClaimSlot(&J_Vol, 2, J_File[2][0], J_File[2][1], J_File[2][2], 2);
}

/* Image for J_Batch3: entries 0 and 1 committed, the page 0 of entry 2 written into a free block */
static NandSim_t *J_MakeImage3(void)
{
    uint32  D = J_DataStart();

    J_NewVolume();
    J_AddFile(&J_Vol, 0, "/cf/apps/a.so", D, 2);
    J_AddFile(&J_Vol, 1, "/cf/apps/b.so", D + 2, 3);
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    J_File[2][0] = D + 10;
    J_File[2][1] = 2;
    J_File[2][2] = J_Vol.NextWriteSeq++;
    J_WritePage0("/cf/apps/new.so", J_File[2][0], J_File[2][1], J_File[2][2], 2);
    J_Cleanup[0] = (uint16)(D + 2);
    J_CleanupCount = 1;
    return(J_Sim);
}

/*
 * Format
 */

static void format_rejects_512_page(void)
{
    static const uint32 Sizes[] = { 512, 1024, 3000, 2 * EEFS_NAND_MAX_PAGE_SIZE };
    uint32              i;

    J_Attach(NandTest_NewSim());
    for (i = 0; i < sizeof(Sizes) / sizeof(Sizes[0]); i++) {
        J_Dev.Geometry.PageSize = Sizes[i];
        CHECK_RC(J_Format(), EEFS_INVALID_ARGUMENT);
        CHECK_RC(J_Mount(&J_Vol), EEFS_INVALID_ARGUMENT);
        CHECK_EQ(J_Vol.Mounted, 0);
    }
    CHECK_EQ(J_Ops(), 0);
    CHECK_EQ(NandSim_Reads(J_Sim), 0);
    J_Destroy();
}

static void format_rejects_bad_geometry(void)
{
    EEFS_NandGeometry_t Good;
    uint32              Ckpt;
    uint32              Expect;

    J_Attach(NandTest_NewSim());
    Good = J_Dev.Geometry;

    /* the library's checkpoint packer gives the FMT 4.6 page counts (4096 blocks, 64 files) */
    memset(&J_Aux, 0, sizeof(J_Aux));
    J_Aux.Dev = &J_Dev;
    CHECK_RC(EEFS_NandCheckGeometry(&J_Aux), EEFS_SUCCESS);
    Ckpt = EEFS_NandMaxCheckpointPages(&J_Aux);
    Expect = (Good.PageSize == 2048) ? 13 : (Good.PageSize == 4096) ? 7 : 4;
    if ((Good.BlockCount == 4096) && (EEFS_MAX_FILES == 64)) {
        CHECK_EQ(Ckpt, Expect);
    }

    /* PagesPerBlock: 2 x (checkpoint pages + 1) is the minimum */
    J_Dev.Geometry.PagesPerBlock = 2 * (Ckpt + 1);
    memset(&J_Aux, 0, sizeof(J_Aux));
    J_Aux.Dev = &J_Dev;
    CHECK_RC(EEFS_NandCheckGeometry(&J_Aux), EEFS_SUCCESS);
    J_Dev.Geometry.PagesPerBlock = 2 * (Ckpt + 1) - 1;
    CHECK_RC(J_Format(), EEFS_INVALID_ARGUMENT);

    J_Dev.Geometry = Good;
    J_Dev.Geometry.DirRegionBlocks = 15;
    CHECK_RC(J_Format(), EEFS_INVALID_ARGUMENT);
    J_Dev.Geometry.DirRegionBlocks = 2;
    CHECK_RC(J_Format(), EEFS_INVALID_ARGUMENT);
    J_Dev.Geometry.DirRegionBlocks = EEFS_NAND_MAX_DIR_BLOCKS + 2;
    CHECK_RC(J_Format(), EEFS_INVALID_ARGUMENT);

    J_Dev.Geometry = Good;
    J_Dev.Geometry.BadMarkerPages = 0;
    CHECK_RC(J_Format(), EEFS_INVALID_ARGUMENT);
    J_Dev.Geometry = Good;
    J_Dev.Geometry.BadMarkerOffset = Good.OobSize;
    CHECK_RC(J_Format(), EEFS_INVALID_ARGUMENT);
    J_Dev.Geometry = Good;
    J_Dev.Geometry.OobSize = 0;
    CHECK_RC(J_Format(), EEFS_INVALID_ARGUMENT);
    J_Dev.Geometry = Good;
    J_Dev.Geometry.BlockCount = Good.DirRegionBlocks;
    CHECK_RC(J_Format(), EEFS_INVALID_ARGUMENT);
    J_Dev.Geometry = Good;
    J_Dev.Geometry.BlockCount = EEFS_NAND_MAX_BLOCKS + 1;
    CHECK_RC(J_Format(), EEFS_INVALID_ARGUMENT);
    J_Dev.Geometry = Good;
    J_Dev.Ops = NULL;
    CHECK_RC(J_Format(), EEFS_INVALID_ARGUMENT);

    /* nothing was touched, and a mounted scratch volume is refused */
    CHECK_EQ(J_Ops(), 0);
    J_Dev.Ops = NandSim_Ops();
    J_Dev.Geometry = Good;
    CHECK_RC(J_Format(), EEFS_SUCCESS);
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibFormat(&J_Vol, (cpuaddr)&J_Dev), EEFS_DEVICE_IS_BUSY);
    CHECK_RC(EEFS_LibFormat(NULL, (cpuaddr)&J_Dev), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibFormat(&J_Aux, 0), EEFS_INVALID_ARGUMENT);
    J_Destroy();
}

static void format_scans_markers_before_erase(void)
{
    uint32  Bad[4];
    uint32  i;
    uint32  b;

    Bad[0] = 5;
    Bad[1] = NandTest_Geo.DirRegionBlocks / 2 + 4;
    Bad[2] = NandTest_Geo.DirRegionBlocks + 100;
    Bad[3] = NandTest_Geo.BlockCount - 1;
    J_Attach(NandTest_NewSim());
    for (i = 0; i < 4; i++) {
        NandSim_SetFactoryBad(J_Sim, Bad[i]);
    }
    CHECK_RC(J_Format(), EEFS_SUCCESS);
    for (i = 0; i < 4; i++) {
        CHECK_EQ(NandSim_BlockErases(J_Sim, Bad[i]), 0);
    }
    CHECK_EQ(NandSim_Erases(J_Sim), NandTest_Geo.BlockCount - 4);
    for (b = 0; b < NandTest_Geo.BlockCount; b++) {
        if ((b != Bad[0]) && (b != Bad[1]) && (b != Bad[2]) && (b != Bad[3])) {
            if (NandSim_BlockErases(J_Sim, b) != 1) {
                CHECK_EQ(NandSim_BlockErases(J_Sim, b), 1);
                break;
            }
        }
    }

    /* the map records them, the rings skip them */
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    for (i = 0; i < 4; i++) {
        CHECK_EQ(J_Vol.Working.Map[Bad[i]], EEFS_NAND_MAP_BAD);
    }
    CHECK_EQ(J_Vol.Working.Map[0], EEFS_NAND_MAP_DIR);
    CHECK_EQ(J_Vol.Mirror[0].Count, NandTest_Geo.DirRegionBlocks / 2 - 1);
    CHECK_EQ(J_Vol.Mirror[1].Count, NandTest_Geo.DirRegionBlocks / 2 - 1);
    CHECK_EQ(NandSim_Violations(J_Sim), 0);
    J_Destroy();
}

static void format_erases_all_good_blocks(void)
{
    uint32  Junk[4];
    uint32  i;
    uint32  p;

    Junk[0] = 2;
    Junk[1] = NandTest_Geo.DirRegionBlocks / 2 + 1;
    Junk[2] = NandTest_Geo.DirRegionBlocks + 4;
    Junk[3] = NandTest_Geo.BlockCount - 2;
    J_Attach(NandTest_NewSim());
    for (i = 0; i < 4; i++) {
        for (p = 0; p < 3; p++) {
            memset(J_Page, (int)(0x11 * (i + 1) + p), NandTest_Geo.PageSize);
            NandSim_RawWrite(J_Sim, Junk[i], p, J_Page, NULL);
        }
    }
    /* and a valid metadata page from an "earlier life" of the device */
    J_WritePage0("/old/file", NandTest_Geo.DirRegionBlocks + 7, 1, 99, 0);

    CHECK_RC(J_Format(), EEFS_SUCCESS);
    for (i = 0; i < 4; i++) {
        for (p = 0; p < 3; p++) {
            if ((Junk[i] == 0) || (Junk[i] == NandTest_Geo.DirRegionBlocks / 2)) continue;
            CHECK(NandSim_PageIsErased(J_Sim, Junk[i], p));
        }
    }
    CHECK(NandSim_PageIsErased(J_Sim, NandTest_Geo.DirRegionBlocks + 7, 0));
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_EntriesInUse(&J_Vol), 0);
    J_Destroy();
}

static void format_checkpoint_seq1_both_mirrors(void)
{
    static uint8        Map[EEFS_NAND_MAX_BLOCKS];
    EEFS_NandRecHdr_t   H;
    EEFS_NandVolumeItem_t V;
    uint8               Mask[EEFS_NAND_MASK_BYTES];
    uint32              MapBytes;
    uint32              Vols;
    uint16              Cl[J_MAX_CLEANUP];
    uint32              ClCount;
    uint32              m;
    uint32              b;
    uint32              i;
    uint32              Cursor;
    uint8               Type;
    const uint8        *Body;
    uint16              Len;

    J_NewVolume();
    for (m = 0; m < 2; m++) {
        b = m * (NandTest_Geo.DirRegionBlocks / 2);
        CHECK_RC(J_RecHdr(b, 0, &H), EEFS_SUCCESS);
        CHECK_EQ(H.RecType, EEFS_NAND_REC_CHECKPOINT);
        CHECK_EQ(H.Seq, 1);
        CHECK_EQ(H.PageIndex, 0);
        CHECK_EQ(H.Mirror, m);

        /* first item VOLUME with the geometry */
        Cursor = 0;
        CHECK_RC(EEFS_NandNextItem(&J_Page[EEFS_NAND_REC_HDR_SIZE], H.PayloadLen, &Cursor, &Type, &Body, &Len), 1);
        CHECK_EQ(Type, EEFS_NAND_ITEM_VOLUME);
        CHECK_RC(EEFS_NandDecodeVolumeItem(Body, Len, &V), EEFS_SUCCESS);
        CHECK_EQ(V.PageSize, NandTest_Geo.PageSize);
        CHECK_EQ(V.BlockCount, NandTest_Geo.BlockCount);
        CHECK_EQ(V.DirRegionBlocks, NandTest_Geo.DirRegionBlocks);
        CHECK_EQ(V.MaxFiles, EEFS_MAX_FILES);
        CHECK_EQ(V.WriteSeqHigh, 0);
        CHECK_EQ(V.PathSize, EEFS_NAND_MAX_PATH_SIZE);

        memset(Map, 0x00, sizeof(Map));
        CHECK_EQ(J_RecItems(b, 0, Mask, &MapBytes, &Vols, Cl, &ClCount, Map), H.PageCount);
        CHECK_EQ(Vols, 1);
        CHECK_EQ(MapBytes, NandTest_Geo.BlockCount);
        CHECK_EQ(ClCount, 0);
        for (i = 0; i < EEFS_NAND_MASK_BYTES; i++) CHECK_EQ(Mask[i], 0);
        for (i = 0; i < NandTest_Geo.BlockCount; i++) {
            if (Map[i] != ((i < NandTest_Geo.DirRegionBlocks) ? EEFS_NAND_MAP_DIR : EEFS_NAND_MAP_FREE)) {
                CHECK_EQ(Map[i], (i < NandTest_Geo.DirRegionBlocks) ? EEFS_NAND_MAP_DIR : EEFS_NAND_MAP_FREE);
                break;
            }
        }
        /* nothing after the checkpoint */
        CHECK(NandSim_PageIsErased(J_Sim, b, H.PageCount));
        CHECK_EQ(J_Vol.Mirror[m].NextPage, H.PageCount);
    }
    CHECK_EQ(J_Vol.LastSeq, 1);
    CHECK_EQ(NandSim_Violations(J_Sim), 0);
    J_Destroy();
}

static void format_fails_with_one_good_block_in_mirror(void)
{
    uint32  Half = NandTest_Geo.DirRegionBlocks / 2;
    uint32  b;
    uint32  m;

    for (m = 0; m < 2; m++) {
        J_Attach(NandTest_NewSim());
        for (b = m * Half + 1; b < (m + 1) * Half; b++) {
            NandSim_SetFactoryBad(J_Sim, b);
        }
        CHECK_RC(J_Format(), EEFS_IO_ERROR);
        CHECK_RC(J_Mount(&J_Vol), EEFS_NOT_FORMATTED);
        J_Destroy();
    }

    /* two good blocks are enough */
    J_Attach(NandTest_NewSim());
    for (b = 2; b < Half; b++) {
        NandSim_SetFactoryBad(J_Sim, b);
    }
    CHECK_RC(J_Format(), EEFS_SUCCESS);
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.Mirror[0].Count, 2);
    J_Destroy();
}

/* Format of a device that holds a volume, cut at every program and erase (on a 64-block device to keep it short):
 * the device mounts as the old volume, as NOT_FORMATTED, or as the new empty volume, in that order, and can always be
 * formatted again. */
static void powercut_sweep_format(void)
{
    EEFS_NandGeometry_t Geo = NandTest_Geo;
    NandSim_t          *Image;
    NandSim_t          *Sim;
    uint32              Cut;
    uint32              Cuts = 0;
    int                 Mode;
    int                 Phase;
    int                 Last;
    int                 Done;
    int32               Status;

    Geo.BlockCount = Geo.DirRegionBlocks + 48;
    Image = NandSim_Create(&Geo);
    CHECK(Image != NULL);
    if (Image == NULL) return;
    J_Attach(Image);
    CHECK_RC(J_Format(), EEFS_SUCCESS);
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    J_AddFile(&J_Vol, 0, "/old/a", Geo.DirRegionBlocks + 3, 2);
    J_AddFile(&J_Vol, 1, "/old/b", Geo.DirRegionBlocks + 30, 1);
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    J_Take(&J_Vol, &J_Pre);
    memset(&J_Vol, 0, sizeof(J_Vol));

    for (Mode = NANDSIM_TORN_NONE; Mode <= NANDSIM_TORN_GARBAGE; Mode++) {
        Last = 0;
        for (Cut = 0; ; Cut++) {
            Sim = NandSim_Clone(Image);
            J_Attach(Sim);
            NandSim_SetPowerBudget(Sim, Cut, Mode);
            Status = J_Format();
            Done = !NandSim_PowerCutHappened(Sim);
            if (Done) CHECK_RC(Status, EEFS_SUCCESS);
            NandSim_Reboot(Sim);

            Status = J_Mount(&J_Vol);
            if (Status == EEFS_NOT_FORMATTED) {
                Phase = 1;
            }
            else {
                CHECK_RC(Status, EEFS_SUCCESS);
                J_Take(&J_Vol, &J_Cur);
                if (J_Same(&J_Cur, &J_Pre)) {
                    Phase = 0;
                }
                else {
                    Phase = 2;
                    CHECK_EQ(J_EntriesInUse(&J_Vol), 0);
                    CHECK_EQ(J_Vol.LastSeq, 1);
                }
            }
            CHECK(Phase >= Last);
            if (Done) CHECK_EQ(Phase, 2);
            Last = Phase;
            if (Phase == 2) {
                CHECK_RC(J_TouchCommit(&J_Vol, J_FOLLOW_ENTRY), EEFS_SUCCESS);
                J_CheckMirrorsAgree(&J_Vol);
            }

            /* formatting again always works */
            memset(&J_Vol, 0, sizeof(J_Vol));
            CHECK_RC(J_Format(), EEFS_SUCCESS);
            CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
            CHECK_EQ(J_EntriesInUse(&J_Vol), 0);
            CHECK_EQ(NandSim_Violations(Sim), 0);

            memset(&J_Vol, 0, sizeof(J_Vol));
            NandSim_Destroy(Sim);
            J_Sim = NULL;
            if (NandTest_CaseFailed()) {
                printf("    sweep format: failure at cut %lu, torn mode %s\n", (unsigned long)Cut, J_ModeName[Mode]);
                NandSim_Destroy(Image);
                return;
            }
            if (Done) {
                Cuts = Cut;
                break;
            }
        }
    }
    printf("    sweep format (%lu blocks): %lu cut points x 3 torn modes\n", (unsigned long)Geo.BlockCount,
           (unsigned long)Cuts);
    NandSim_Destroy(Image);
}

/*
 * Mount
 */

static void mount_empty_volume_reads_only(void)
{
    uint32  b;
    uint32  Markers = 2;    /* NANDTEST_BADMARKER_PAGES: page 0 and page 1 */

    J_Attach(NandTest_NewSim());
    CHECK_RC(J_Format(), EEFS_SUCCESS);
    NandSim_ResetCounters(J_Sim);
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Ops(), 0);
    CHECK(NandSim_Reads(J_Sim) <= NandTest_Geo.DirRegionBlocks * (1 + Markers) + 4 * NandTest_Geo.PagesPerBlock +
                                  2 * EEFS_MAX_FILES);
    CHECK_EQ(J_Vol.Mounted, TRUE);
    CHECK_EQ(J_Vol.LastSeq, 1);
    CHECK_EQ(J_EntriesInUse(&J_Vol), 0);
    for (b = 0; b < NandTest_Geo.BlockCount; b++) {
        if (J_Vol.Working.Map[b] != ((b < NandTest_Geo.DirRegionBlocks) ? EEFS_NAND_MAP_DIR : EEFS_NAND_MAP_FREE)) {
            CHECK(0);
            break;
        }
    }
    CHECK(J_Vol.Mirror[0].HasState && J_Vol.Mirror[1].HasState);
    CHECK(!J_Vol.Mirror[0].NeedsResync && !J_Vol.Mirror[1].NeedsResync);
    CHECK(!J_Vol.Mirror[0].EnterNewBlock && !J_Vol.Mirror[1].EnterNewBlock);
    CHECK_EQ(J_Vol.PendingCleanupCount, 0);
    CHECK_EQ(J_Vol.VolumeChangeCounter, 1);
    CHECK_EQ(J_Vol.PageSize, NandTest_Geo.PageSize);
    CHECK_EQ(J_Vol.BlockBytes, NandTest_Geo.PageSize * NandTest_Geo.PagesPerBlock);
    CHECK_EQ(J_Vol.MaxBlocksPerFile, EEFS_NandMaxBlocksPerFile(NandTest_Geo.PageSize));
    CHECK(memcmp(&J_Vol.Working, &J_Vol.Base, sizeof(J_Vol.Base)) == 0);

    /* a mounted volume can't be mounted again, and unmounts cleanly */
    CHECK_RC(EEFS_LibInitFS(&J_Vol, (cpuaddr)&J_Dev), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibFreeFS(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.Mounted, 0);
    CHECK_RC(EEFS_LibFreeFS(&J_Vol), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibAbort(&J_Vol), EEFS_INVALID_ARGUMENT);
    J_Destroy();
}

static void mount_unformatted_returns_not_formatted(void)
{
    J_Attach(NandTest_NewSim());
    CHECK_RC(J_Mount(&J_Vol), EEFS_NOT_FORMATTED);
    CHECK_EQ(J_Vol.Mounted, 0);
    CHECK_EQ(J_Ops(), 0);

    /* junk in the directory region is not a directory */
    memset(J_Page, 0x3C, NandTest_Geo.PageSize);
    NandSim_RawWrite(J_Sim, 0, 0, J_Page, NULL);
    NandSim_RawWrite(J_Sim, NandTest_Geo.DirRegionBlocks / 2, 0, J_Page, NULL);
    CHECK_RC(J_Mount(&J_Vol), EEFS_NOT_FORMATTED);
    J_Destroy();
}

static void mount_geometry_mismatch(void)
{
    EEFS_NandGeometry_t Good;
    EEFS_NandRecHdr_t   H;
    uint32              m;
    uint32              b;

    J_NewVolume();
    memset(&J_Vol, 0, sizeof(J_Vol));
    Good = J_Dev.Geometry;

    J_Dev.Geometry.BlockCount = Good.BlockCount - 1;
    CHECK_RC(J_Mount(&J_Vol), EEFS_GEOMETRY_MISMATCH);
    CHECK_EQ(J_Vol.Mounted, 0);
    J_Dev.Geometry = Good;
    J_Dev.Geometry.DirRegionBlocks = Good.DirRegionBlocks - 4;
    CHECK_RC(J_Mount(&J_Vol), EEFS_GEOMETRY_MISMATCH);
    J_Dev.Geometry = Good;
    J_Dev.Geometry.PagesPerBlock = Good.PagesPerBlock / 2;
    CHECK_RC(J_Mount(&J_Vol), EEFS_GEOMETRY_MISMATCH);
    J_Dev.Geometry = Good;
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    memset(&J_Vol, 0, sizeof(J_Vol));

    /* a writer with more entries than this build */
    for (m = 0; m < 2; m++) {
        b = m * (Good.DirRegionBlocks / 2);
        CHECK_RC(J_RecHdr(b, 0, &H), EEFS_SUCCESS);
        EEFS_NandPut32(&J_Page[EEFS_NAND_REC_HDR_SIZE + EEFS_NAND_ITEM_HDR_SIZE + EEFS_NAND_VOL_OFF_MAXFILES],
                       EEFS_MAX_FILES + 1);
        EEFS_NandSealRecordPage(J_Page, Good.PageSize, H.PayloadLen);
        NandSim_RawWrite(J_Sim, b, 0, J_Page, NULL);
    }
    CHECK_RC(J_Mount(&J_Vol), EEFS_GEOMETRY_MISMATCH);
    J_Destroy();
}

static void writeseq_gap_after_mount(void)
{
    uint32  High;

    J_NewVolume();
    CHECK_EQ(J_Vol.Base.WriteSeqHigh, 0);
    CHECK_EQ(J_Vol.NextWriteSeq, 1 + EEFS_NAND_WRITESEQ_MOUNT_GAP);

    /* a commit records NextWriteSeq - 1 as WriteSeqHigh; the next mount adds the gap again */
    J_Vol.NextWriteSeq += 5;
    High = J_Vol.NextWriteSeq - 1;
    CHECK_RC(J_TouchCommit(&J_Vol, 3), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.Base.WriteSeqHigh, High);
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.Working.WriteSeqHigh, High);
    CHECK_EQ(J_Vol.NextWriteSeq, High + 1 + EEFS_NAND_WRITESEQ_MOUNT_GAP);

    /* a committed metadata page with a higher WriteSeq than WriteSeqHigh wins */
    J_SetEntry(&J_Vol, 4, "/seq");
    J_WritePage0("/seq", J_DataStart(), 1, 1000000, 4);
    J_ClaimSlot(&J_Vol, 4, J_DataStart(), 1, 1000000, 4);
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.NextWriteSeq, 1000000 + 1 + EEFS_NAND_WRITESEQ_MOUNT_GAP);
    CHECK_EQ(J_Vol.SlotFlags[4][0], 0);
    J_Destroy();
}

/* Mount step 6 with synthetic slots: page 0 wins for ownership and copied fields; a bad page 0 flags the slot */
static void mount_reconcile_follows_page0(void)
{
    uint32  D = J_DataStart();

    J_NewVolume();
    J_AddFile(&J_Vol, 0, "/r/a", D, 3);
    J_AddFile(&J_Vol, 1, "/r/b", D + 5, 2);
    /* the directory claims one block too many for entry 0 and has a stale size */
    J_Vol.Working.Map[D + 3] = EEFS_NAND_MAP_OWNER(0, 0);
    J_Vol.Working.Entry[0].Slot[0].FileSize -= 1;
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    /* entry 1's metadata page is destroyed */
    NandSim_CorruptBytes(J_Sim, D + 5, 0, 20, 2);

    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.SlotFlags[0][0], 0);
    CHECK_EQ(J_Vol.Working.Map[D + 3], EEFS_NAND_MAP_FREE);
    CHECK_EQ(J_Vol.Working.Map[D + 2], EEFS_NAND_MAP_OWNER(0, 0));
    CHECK_EQ(J_Vol.Working.Entry[0].Slot[0].FileSize,
             3 * NandTest_Geo.PageSize * NandTest_Geo.PagesPerBlock - NandTest_Geo.PageSize);
    CHECK(EEFS_NAND_MASK_TEST(J_Vol.ForceDirty, 0));
    CHECK_EQ(J_Vol.SlotFlags[1][0], EEFS_NAND_SLOTFLAG_CORRUPT);
    CHECK_EQ(J_Vol.Working.Map[D + 5], EEFS_NAND_MAP_OWNER(1, 0));     /* the map stays as the directory says */
    CHECK(memcmp(&J_Vol.Working, &J_Vol.Base, sizeof(J_Vol.Base)) == 0);
    CHECK(EEFS_NandHasUncommitted(&J_Vol, TRUE));
    CHECK(!EEFS_NandHasUncommitted(&J_Vol, FALSE));

    /* the next commit persists the repair (ForceDirty), so a remount needs none */
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK(!EEFS_NAND_MASK_TEST(J_Vol.ForceDirty, 0));
    CHECK_EQ(J_Vol.Working.Map[D + 3], EEFS_NAND_MAP_FREE);
    J_Destroy();
}

/*
 * Commit and abort
 */

static void commit_nothing_dirty_writes_nothing(void)
{
    uint32  Ops;

    J_NewVolume();
    Ops = J_Ops();
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Ops(), Ops);
    CHECK_EQ(J_Vol.LastSeq, 1);

    /* a change undone by hand, and a change aborted, write nothing either */
    J_SetEntry(&J_Vol, 3, "/x");
    memset(&J_Vol.Working.Entry[3], 0, sizeof(EEFS_NandEntry_t));
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    J_SetEntry(&J_Vol, 3, "/x");
    CHECK_RC(EEFS_LibAbort(&J_Vol), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Ops(), Ops);
    CHECK_EQ(J_Vol.LastSeq, 1);
    J_Destroy();
}

static void commit_txn_visible_after_remount(void)
{
    uint8   Mask[EEFS_NAND_MASK_BYTES];
    uint32  MapBytes;
    uint32  Vols;
    uint16  Cl[J_MAX_CLEANUP];
    uint32  ClCount;
    uint32  Pages;
    uint32  Before[2];
    uint32  m;
    uint32  b;
    uint32  D;

    J_MakeImage3();
    D = J_DataStart();
    CHECK_EQ(J_Vol.LastSeq, 2);
    J_Batch3();
    CHECK_EQ(J_Vol.PendingCleanupCount, 1);
    Pages = J_TxnPages(&J_Vol);
    CHECK(Pages >= 1);
    Before[0] = J_Vol.Mirror[0].NextPage;
    Before[1] = J_Vol.Mirror[1].NextPage;
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.LastSeq, 3);
    CHECK_EQ(J_Vol.PendingCleanupCount, 0);
    CHECK(NandSim_PageIsErased(J_Sim, D + 2, 0));

    /* one TXN of the expected page count in each mirror, with exactly the changed entries, map ranges and cleanup */
    for (m = 0; m < 2; m++) {
        CHECK_EQ(J_Vol.Mirror[m].NextPage, Before[m] + Pages);
        b = J_Vol.Mirror[m].Blocks[J_Vol.Mirror[m].CurIndex];
        CHECK_EQ(J_RecItems(b, Before[m], Mask, &MapBytes, &Vols, Cl, &ClCount, NULL), Pages);
        CHECK_EQ(Vols, 1);
        CHECK_EQ(Mask[0], 0x07);
        CHECK(MapBytes >= 5);           /* entry 1's 3 blocks freed, entry 2's 2 blocks claimed */
        CHECK(MapBytes < 32);
        CHECK_EQ(ClCount, 1);
        CHECK_EQ(Cl[0], D + 2);
    }

    J_CheckRemountAgrees(&J_Vol);
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.LastSeq, 3);
    CHECK_EQ(J_Vol.PendingCleanupCount, 1);     /* the newest record's list; the erase already happened */
    CHECK_EQ(J_Vol.PendingCleanupBase, 1);
    CHECK_EQ(J_Vol.Working.Map[D + 2], EEFS_NAND_MAP_FREE);
    CHECK_EQ(J_Vol.Working.Map[D + 10], EEFS_NAND_MAP_OWNER(2, 0));
    CHECK_EQ(J_Vol.Working.Entry[0].Attributes, EEFS_ATTRIBUTE_READONLY);
    CHECK_EQ(J_Vol.Working.Entry[1].InUse, 0);
    CHECK(strcmp(J_Vol.Working.Entry[2].Path, "/cf/apps/new.so") == 0);
    J_CheckSlotsValid(&J_Vol);
    J_CheckMirrorsAgree(&J_Vol);
    CHECK_EQ(NandSim_Violations(J_Sim), 0);
    J_Destroy();
}

static void commit_entry_only_in_use_in_checkpoint(void)
{
    uint8   Mask[EEFS_NAND_MASK_BYTES];
    uint32  MapBytes;
    uint32  Vols;
    uint16  Cl[J_MAX_CLEANUP];
    uint32  ClCount;
    uint32  m;
    uint32  b;
    uint32  D = J_DataStart();

    J_NewVolume();
    J_AddFile(&J_Vol, 0, "/c/0", D, 1);
    J_AddFile(&J_Vol, 1, "/c/1", D + 1, 1);
    J_AddFile(&J_Vol, 2, "/c/2", D + 2, 1);
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    J_RemoveEntry(&J_Vol, 1);
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);

    CHECK_RC(EEFS_LibRepairDirectory(&J_Vol), EEFS_SUCCESS);
    for (m = 0; m < 2; m++) {
        CHECK_EQ(J_Vol.Mirror[m].CurIndex, 1);
        CHECK_EQ(J_Vol.Mirror[m].CheckpointSeq, J_Vol.LastSeq);
        b = J_Vol.Mirror[m].Blocks[1];
        CHECK(J_RecItems(b, 0, Mask, &MapBytes, &Vols, Cl, &ClCount, NULL) > 0);
        CHECK_EQ(Mask[0], 0x05);
        CHECK_EQ(MapBytes, NandTest_Geo.BlockCount);
    }
    J_CheckRemountAgrees(&J_Vol);
    J_CheckMirrorsAgree(&J_Vol);
    J_Destroy();
}

static void abort_restores_working_and_keeps_bad_marks(void)
{
    uint32  Bad = J_DataStart() + 40;
    uint32  b;

    J_MakeImage3();
    J_Take(&J_Vol, &J_Pre);
    J_Batch3();
    EEFS_NandMarkBad(&J_Vol, Bad);
    CHECK_EQ(J_Vol.PendingCleanupCount, 1);
    CHECK_RC(EEFS_LibAbort(&J_Vol), EEFS_SUCCESS);

    CHECK(memcmp(J_Vol.Working.Entry, J_Vol.Base.Entry, sizeof(J_Vol.Base.Entry)) == 0);
    for (b = 0; b < NandTest_Geo.BlockCount; b++) {
        if (b == Bad) continue;
        if (J_Vol.Working.Map[b] != J_Vol.Base.Map[b]) {
            CHECK_EQ(J_Vol.Working.Map[b], J_Vol.Base.Map[b]);
            break;
        }
    }
    CHECK_EQ(J_Vol.Working.Map[Bad], EEFS_NAND_MAP_BAD);
    CHECK_EQ(J_Vol.PendingCleanupCount, 0);
    CHECK(!NandSim_PageIsErased(J_Sim, J_DataStart() + 2, 0));     /* entry 1's page 0 untouched */

    /* the bad mark is persisted by the next commit */
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.Working.Map[Bad], EEFS_NAND_MAP_BAD);
    J_Take(&J_Vol, &J_Cur);
    J_Cur.Map[Bad] = EEFS_NAND_MAP_FREE;
    J_Cur.LastSeq = J_Pre.LastSeq;
    CHECK(J_Same(&J_Cur, &J_Pre));
    J_Destroy();
}

static void abort_erases_batch_page0s(void)
{
    uint32  D = J_DataStart();
    uint32  Seq;

    J_NewVolume();
    J_AddFile(&J_Vol, 0, "/a/live", D, 2);
    J_AddFile(&J_Vol, 3, "/a/other", D + 20, 1);
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);

    /* new entry 1 at D+5; entry 0 rewritten (copy-on-write) into a new first block D+6 that keeps D+1 */
    J_AddFile(&J_Vol, 1, "/a/new", D + 5, 1);
    Seq = J_Vol.NextWriteSeq++;
    J_WritePage0("/a/live", D + 6, 1, Seq, 0);
    EEFS_NandFreeSlot(&J_Vol, 0, 0);
    J_SetEntry(&J_Vol, 0, "/a/live");
    J_ClaimSlot(&J_Vol, 0, D + 6, 1, Seq, 0);
    CHECK_EQ(J_Vol.Working.Map[D], EEFS_NAND_MAP_FREEING);
    CHECK_EQ(J_Vol.PendingCleanupCount, 1);
    /* a bogus batch slot pointing at a committed block of another entry must not get it erased */
    J_SetEntry(&J_Vol, 2, "/a/bogus");
    J_Vol.Working.Entry[2].Slot[0].InUse = 1;
    J_Vol.Working.Entry[2].Slot[0].FirstBlock = (uint16)(D + 20);

    CHECK_RC(EEFS_LibAbort(&J_Vol), EEFS_SUCCESS);
    CHECK(NandSim_PageIsErased(J_Sim, D + 5, 0));
    CHECK(NandSim_PageIsErased(J_Sim, D + 6, 0));
    CHECK(!NandSim_PageIsErased(J_Sim, D, 0));
    CHECK(!NandSim_PageIsErased(J_Sim, D + 20, 0));
    CHECK_EQ(J_Vol.Working.Entry[0].Slot[0].FirstBlock, D);
    CHECK_EQ(J_Vol.Working.Map[D], EEFS_NAND_MAP_OWNER(0, 0));
    CHECK_EQ(J_Vol.Working.Map[D + 5], EEFS_NAND_MAP_FREE);
    CHECK_EQ(J_Vol.Working.Map[D + 6], EEFS_NAND_MAP_FREE);
    CHECK_EQ(J_Vol.PendingCleanupCount, 0);
    CHECK(!EEFS_NandHasUncommitted(&J_Vol, TRUE));
    CHECK_RC(EEFS_LibFreeFS(&J_Vol), EEFS_SUCCESS);
    J_Destroy();
}

/* FreeFS refuses an open batch; an open descriptor on a batch slot blocks Abort */
static void freefs_and_abort_busy_rules(void)
{
    J_NewVolume();
    J_SetEntry(&J_Vol, 2, "/b");
    CHECK_RC(EEFS_LibFreeFS(&J_Vol), EEFS_TRANSACTION_ERROR);
    J_ClaimSlot(&J_Vol, 2, J_DataStart(), 1, 5, 2);
    EEFS_FileDescriptorTable[0].InUse = TRUE;
    EEFS_FileDescriptorTable[0].InodeTable = &J_Vol;
    EEFS_FileDescriptorTable[0].InodeIndex = 2;
    EEFS_FileDescriptorTable[0].Slot = 0;
    CHECK_RC(EEFS_LibAbort(&J_Vol), EEFS_DEVICE_IS_BUSY);
    CHECK_RC(EEFS_LibFreeFS(&J_Vol), EEFS_DEVICE_IS_BUSY);
    memset(&EEFS_FileDescriptorTable[0], 0, sizeof(EEFS_FileDescriptorTable[0]));
    CHECK_RC(EEFS_LibAbort(&J_Vol), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibFreeFS(&J_Vol), EEFS_SUCCESS);
    J_Destroy();
}

static void freeing_is_map_state_only(void)
{
    static uint8    Map[EEFS_NAND_MAX_BLOCKS];
    uint8           Mask[EEFS_NAND_MASK_BYTES];
    uint32          MapBytes;
    uint32          Vols;
    uint16          Cl[J_MAX_CLEANUP];
    uint32          ClCount;
    uint32          Per;
    uint32          Files = 5;
    uint32          D = J_DataStart();
    uint32          MapChanged;
    uint32          Freeing = 0;
    uint32          i;
    uint32          b;
    char            Path[32];

    Per = 60;
    if (Per > EEFS_NandMaxBlocksPerFile(NandTest_Geo.PageSize)) Per = EEFS_NandMaxBlocksPerFile(NandTest_Geo.PageSize);
    J_NewVolume();
    for (i = 0; i < Files; i++) {
        sprintf(Path, "/big/%lu", (unsigned long)i);
        J_AddFile(&J_Vol, i, Path, D + i * Per, Per);
    }
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);

    for (i = 0; i < Files; i++) {
        J_RemoveEntry(&J_Vol, i);
    }
    for (b = 0; b < NandTest_Geo.BlockCount; b++) {
        if (J_Vol.Working.Map[b] == EEFS_NAND_MAP_FREEING) Freeing++;
    }
    CHECK_EQ(Freeing, Files * Per);
    CHECK_EQ(J_Vol.PendingCleanupCount, Files);    /* one cleanup entry per slot, not per block */
    EEFS_NandDiff(&J_Vol, Mask, &MapChanged);
    CHECK_EQ(MapChanged, Files * Per);

    b = J_Vol.Mirror[0].NextPage;
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    memset(Map, 0x00, sizeof(Map));
    CHECK(J_RecItems(J_Vol.Mirror[0].Blocks[J_Vol.Mirror[0].CurIndex], b, Mask, &MapBytes, &Vols, Cl, &ClCount, Map) > 0);
    CHECK_EQ(MapBytes, Files * Per);                /* one merged range, serialized as FREE */
    for (i = D; i < D + Files * Per; i++) {
        if (Map[i] != EEFS_NAND_MAP_FREE) {
            CHECK_EQ(Map[i], EEFS_NAND_MAP_FREE);
            break;
        }
    }
    CHECK_EQ(ClCount, Files);

    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_EntriesInUse(&J_Vol), 0);
    for (b = D; b < D + Files * Per; b++) {
        if (J_Vol.Working.Map[b] != EEFS_NAND_MAP_FREE) {
            CHECK_EQ(J_Vol.Working.Map[b], EEFS_NAND_MAP_FREE);
            break;
        }
    }
    for (i = 0; i < Files; i++) {
        CHECK(NandSim_PageIsErased(J_Sim, D + i * Per, 0));
        CHECK_EQ(NandSim_BlockErases(J_Sim, D + i * Per + 1), 1);     /* data blocks: only the format erase, no cleanup */
    }
    J_CheckMirrorsAgree(&J_Vol);
    J_Destroy();
}

/*
 * Rings and mirrors
 */

/* 64 entries change in every commit, so the records are large and the rings wrap several times */
static void J_RingRun(uint32 Commits, const uint32 *FactoryBad, uint32 BadCount)
{
    uint32  n;
    uint32  i;
    uint32  m;
    uint32  b;
    uint32  Pages;
    uint32  Ckpt;
    uint32  Before[2];
    uint32  Index[2];
    uint32  CkptSeq[2];
    uint32  Entered = 0;
    char    Path[32];

    J_Attach(NandTest_NewSim());
    for (i = 0; i < BadCount; i++) {
        NandSim_SetFactoryBad(J_Sim, FactoryBad[i]);
    }
    CHECK_RC(J_Format(), EEFS_SUCCESS);
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        sprintf(Path, "/ring/%lu", (unsigned long)i);
        J_SetEntry(&J_Vol, i, Path);
    }
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);

    for (n = 1; n <= Commits; n++) {
        for (i = 0; i < EEFS_MAX_FILES; i++) {
            J_Vol.Working.Entry[i].CreationDate = n * 1000 + i;
        }
        b = J_DataStart() + (n % 50);                                  /* and a map byte */
        J_Vol.Working.Map[b] = (J_Vol.Working.Map[b] == EEFS_NAND_MAP_FREE) ? EEFS_NAND_MAP_OWNER(0, 0) : EEFS_NAND_MAP_FREE;
        Pages = J_TxnPages(&J_Vol);
        Ckpt = EEFS_NandRecordPages(&J_Vol, EEFS_NAND_REC_CHECKPOINT, &J_Vol.Working, NULL, NULL, 0);
        for (m = 0; m < 2; m++) {
            Before[m] = J_Vol.Mirror[m].NextPage;
            Index[m] = J_Vol.Mirror[m].CurIndex;
            CkptSeq[m] = J_Vol.Mirror[m].CheckpointSeq;
        }
        CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);

        /* a CHECKPOINT in the next ring block exactly when the TXN would not fit */
        for (m = 0; m < 2; m++) {
            if ((Before[m] + Pages) > NandTest_Geo.PagesPerBlock) {
                CHECK_EQ(J_Vol.Mirror[m].CurIndex, (Index[m] + 1) % J_Vol.Mirror[m].Count);
                CHECK_EQ(J_Vol.Mirror[m].CheckpointSeq, J_Vol.LastSeq);
                CHECK_EQ(J_Vol.Mirror[m].NextPage, Ckpt);
                if (m == 0) Entered++;
            }
            else {
                CHECK_EQ(J_Vol.Mirror[m].CurIndex, Index[m]);
                CHECK_EQ(J_Vol.Mirror[m].CheckpointSeq, CkptSeq[m]);
                CHECK_EQ(J_Vol.Mirror[m].NextPage, Before[m] + Pages);
            }
        }
        if ((n % 50) == 0) {
            J_CheckRemountAgrees(&J_Vol);
            J_CheckMirrorsAgree(&J_Vol);
        }
        if (NandTest_CaseFailed()) {
            printf("    ring: failure at commit %lu\n", (unsigned long)n);
            break;
        }
    }

    /* every block of each ring was entered (erased again after the format) */
    CHECK(Entered > J_Vol.Mirror[0].Count);
    for (b = 0; b < NandTest_Geo.DirRegionBlocks; b++) {
        for (i = 0; i < BadCount; i++) {
            if (FactoryBad[i] == b) break;
        }
        if (i < BadCount) {
            CHECK_EQ(NandSim_BlockErases(J_Sim, b), 0);
        }
        else if (NandSim_BlockErases(J_Sim, b) < 2) {
            printf("    ring: directory block %lu was never entered\n", (unsigned long)b);
            CHECK(0);
        }
    }
    CHECK_EQ(J_Vol.Mirror[0].Count + J_Vol.Mirror[1].Count, NandTest_Geo.DirRegionBlocks - BadCount);
    CHECK_EQ(J_Vol.LastSeq, Commits + 2);
    J_CheckRemountAgrees(&J_Vol);
    CHECK_EQ(NandSim_Violations(J_Sim), 0);
    printf("    ring: %lu commits, mirror 0 entered %lu blocks (ring of %lu)\n", (unsigned long)Commits,
           (unsigned long)Entered, (unsigned long)J_Vol.Mirror[0].Count);
    J_Destroy();
}

static void ring_rotation_300_commits(void)
{
    J_RingRun(300, NULL, 0);
}

static void ring_wraps_around_factory_bad_dir_blocks(void)
{
    uint32  Bad[3];

    Bad[0] = 3;
    Bad[1] = NandTest_Geo.DirRegionBlocks / 2;          /* the first block of mirror 1 */
    Bad[2] = NandTest_Geo.DirRegionBlocks - 1;          /* the last block of mirror 1 */
    J_RingRun(200, Bad, 3);
}

static void mirror0_erased_mounts_from_mirror1_and_resyncs(void)
{
    EEFS_NandRecHdr_t   H;
    uint32              b;
    uint32              Seq;
    uint32              Ckpt;

    J_MakeImage3();
    J_Batch3();
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    J_Take(&J_Vol, &J_Post);
    Seq = J_Vol.LastSeq;

    for (b = 0; b < NandTest_Geo.DirRegionBlocks / 2; b++) {
        NandSim_RawErase(J_Sim, b);
    }
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    J_Take(&J_Vol, &J_Cur);
    CHECK(J_Same(&J_Cur, &J_Post));
    CHECK(!J_Vol.Mirror[0].HasState);
    CHECK(J_Vol.Mirror[0].NeedsResync);

    /* the next commit puts a CHECKPOINT at Seq into mirror 0 first, then the TXN */
    Ckpt = EEFS_NandRecordPages(&J_Vol, EEFS_NAND_REC_CHECKPOINT, &J_Vol.Base, NULL, NULL, 1);
    CHECK_RC(J_TouchCommit(&J_Vol, 5), EEFS_SUCCESS);
    CHECK_RC(J_RecHdr(0, 0, &H), EEFS_SUCCESS);
    CHECK_EQ(H.RecType, EEFS_NAND_REC_CHECKPOINT);
    CHECK_EQ(H.Seq, Seq);
    CHECK_RC(J_RecHdr(0, H.PageCount, &H), EEFS_SUCCESS);
    CHECK_EQ(H.RecType, EEFS_NAND_REC_TXN);
    CHECK_EQ(H.Seq, Seq + 1);
    CHECK(Ckpt >= 1);
    CHECK(!J_Vol.Mirror[0].NeedsResync);
    J_CheckMirrorsAgree(&J_Vol);
    J_CheckRemountAgrees(&J_Vol);
    J_Destroy();
}

static void mirror_lagging_one_record(void)
{
    uint32  Pages;
    uint32  Seq;

    J_MakeImage3();
    J_Take(&J_Vol, &J_Pre);
    J_Batch3();
    Pages = J_TxnPages(&J_Vol);
    Seq = J_Vol.LastSeq + 1;
    J_Take(&J_Vol, &J_Post);        /* before the commit: after the cut the RAM holds marks of the failed ops */
    J_Post.LastSeq = Seq;
    NandSim_SetPowerBudget(J_Sim, Pages, NANDSIM_TORN_NONE);     /* mirror 0's record completes, mirror 1's doesn't start */
    (void)EEFS_LibCommit(&J_Vol);
    CHECK(NandSim_PowerCutHappened(J_Sim));
    NandSim_Reboot(J_Sim);

    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.LastSeq, Seq);
    J_Take(&J_Vol, &J_Cur);
    CHECK(J_Same(&J_Cur, &J_Post));
    CHECK_EQ(J_Vol.Mirror[0].LastSeq, Seq);
    CHECK_EQ(J_Vol.Mirror[1].LastSeq, Seq - 1);
    CHECK(J_Vol.Mirror[1].NeedsResync);
    CHECK(!J_Vol.Mirror[0].NeedsResync);
    CHECK_EQ(J_Vol.PendingCleanupBase, 1);       /* the commit's cleanup never ran */
    CHECK(!NandSim_PageIsErased(J_Sim, J_DataStart() + 2, 0));

    /* the next commit erases the pending block, resyncs mirror 1 and writes the TXN to both */
    CHECK_RC(J_TouchCommit(&J_Vol, 6), EEFS_SUCCESS);
    CHECK(NandSim_PageIsErased(J_Sim, J_DataStart() + 2, 0));
    CHECK_EQ(J_Vol.Mirror[1].CheckpointSeq, Seq);
    CHECK_EQ(J_Vol.Mirror[1].LastSeq, Seq + 1);
    J_CheckMirrorsAgree(&J_Vol);
    J_CheckRemountAgrees(&J_Vol);
    CHECK_EQ(NandSim_Violations(J_Sim), 0);
    J_Destroy();
}

static void torn_tail_record_ignored_and_block_abandoned(void)
{
    EEFS_NandRecHdr_t   H;
    uint32              m;
    uint32              b;
    uint32              Index[2];
    uint32              Seq;

    J_MakeImage3();
    J_Take(&J_Vol, &J_Pre);
    J_Batch3();
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    Seq = J_Vol.LastSeq;

    /* corrupt the last page of the newest record in both mirrors */
    for (m = 0; m < 2; m++) {
        Index[m] = J_Vol.Mirror[m].CurIndex;
        b = J_Vol.Mirror[m].Blocks[Index[m]];
        NandSim_CorruptBytes(J_Sim, b, J_Vol.Mirror[m].NextPage - 1u, 100, 4);
    }
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.LastSeq, Seq - 1);
    J_Take(&J_Vol, &J_Cur);
    J_Pre.LastSeq = Seq - 1;
    CHECK(J_Same(&J_Cur, &J_Pre));
    for (m = 0; m < 2; m++) {
        CHECK(J_Vol.Mirror[m].EnterNewBlock);
        CHECK(!J_Vol.Mirror[m].NeedsResync);
    }

    /* the next record goes into a fresh block as a CHECKPOINT (reusing Seq) */
    CHECK_RC(J_TouchCommit(&J_Vol, 7), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.LastSeq, Seq);
    for (m = 0; m < 2; m++) {
        CHECK_EQ(J_Vol.Mirror[m].CurIndex, (Index[m] + 1) % J_Vol.Mirror[m].Count);
        CHECK_RC(J_RecHdr(J_Vol.Mirror[m].Blocks[J_Vol.Mirror[m].CurIndex], 0, &H), EEFS_SUCCESS);
        CHECK_EQ(H.RecType, EEFS_NAND_REC_CHECKPOINT);
        CHECK_EQ(H.Seq, Seq);
    }
    J_CheckMirrorsAgree(&J_Vol);
    J_CheckRemountAgrees(&J_Vol);
    CHECK_EQ(NandSim_Violations(J_Sim), 0);
    J_Destroy();
}

/* Commits one-entry changes until both mirrors' current blocks are full */
static void J_FillBlocks(void)
{
    uint32  n = 0;

    while ((J_Vol.Mirror[0].NextPage < NandTest_Geo.PagesPerBlock) && (n < 4 * NandTest_Geo.PagesPerBlock)) {
        CHECK_RC(J_TouchCommit(&J_Vol, 9), EEFS_SUCCESS);
        n++;
    }
    CHECK_EQ(J_Vol.Mirror[0].NextPage, NandTest_Geo.PagesPerBlock);
    CHECK_EQ(J_Vol.Mirror[1].NextPage, NandTest_Geo.PagesPerBlock);
}

static void torn_entry_checkpoint_falls_back_to_previous_block(void)
{
    EEFS_NandChain_t    C;
    uint32              Index;
    uint32              Torn;
    uint32              Seq;
    uint32              m;

    for (m = 0; m < 2; m++) {    /* 0: only mirror 0's new checkpoint is torn; 1: both */
        J_MakeImage3();
        J_FillBlocks();
        J_Take(&J_Vol, &J_Pre);
        Index = J_Vol.Mirror[0].CurIndex;
        J_Batch3();
        CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
        J_Take(&J_Vol, &J_Post);
        Seq = J_Vol.LastSeq;
        Torn = (Index + 1) % J_Vol.Mirror[0].Count;
        CHECK_EQ(J_Vol.Mirror[0].CurIndex, Torn);
        NandSim_CorruptBytes(J_Sim, J_Vol.Mirror[0].Blocks[Torn], J_Vol.Mirror[0].NextPage - 1u, 64, 1);
        if (m == 1) {
            NandSim_CorruptBytes(J_Sim, J_Vol.Mirror[1].Blocks[J_Vol.Mirror[1].CurIndex],
                                 J_Vol.Mirror[1].NextPage - 1u, 64, 1);
        }

        CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
        CHECK_RC(EEFS_NandScanMirror(&J_Vol, 0, &C), EEFS_SUCCESS);
        CHECK(C.Valid);
        CHECK_EQ(C.BlockIdx, Index);
        CHECK_EQ(C.LastSeq, Seq - 1);
        J_Take(&J_Vol, &J_Cur);
        if (m == 0) {
            CHECK(J_Same(&J_Cur, &J_Post));     /* mirror 1 has it */
        }
        else {
            J_Pre.LastSeq = Seq - 1;
            CHECK(J_Same(&J_Cur, &J_Pre));
        }

        /* mirror 0 re-enters the torn block */
        CHECK_RC(J_TouchCommit(&J_Vol, 8), EEFS_SUCCESS);
        CHECK_EQ(J_Vol.Mirror[0].CurIndex, Torn);
        J_CheckMirrorsAgree(&J_Vol);
        J_CheckRemountAgrees(&J_Vol);
        CHECK_EQ(NandSim_Violations(J_Sim), 0);
        J_Destroy();
    }
}

static void dir_block_program_failure_marks_bad_and_retries(void)
{
    uint32  Index;
    uint32  Block;
    uint32  Next;
    uint32  Count;

    J_MakeImage3();
    Index = J_Vol.Mirror[0].CurIndex;
    Block = J_Vol.Mirror[0].Blocks[Index];
    Count = J_Vol.Mirror[0].Count;
    NandSim_InjectProgramFail(J_Sim, Block, J_Vol.Mirror[0].NextPage, 1);
    J_Batch3();
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.Working.Map[Block], EEFS_NAND_MAP_BAD);
    CHECK_EQ(J_Vol.Mirror[0].Count, Count - 1);
    CHECK_EQ(J_Vol.Mirror[0].CheckpointSeq, J_Vol.LastSeq);
    CHECK(J_Vol.Mirror[0].Blocks[J_Vol.Mirror[0].CurIndex] != Block);
    CHECK(EEFS_NandHasBadMarker(&J_Vol, Block));

    /* an erase failure while entering the next block skips that block too */
    Next = J_Vol.Mirror[0].Blocks[(J_Vol.Mirror[0].CurIndex + 1) % J_Vol.Mirror[0].Count];
    NandSim_InjectEraseFail(J_Sim, Next, 1);
    CHECK_RC(EEFS_LibRepairDirectory(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.Working.Map[Next], EEFS_NAND_MAP_BAD);
    CHECK_EQ(J_Vol.Mirror[0].Count, Count - 2);

    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.Working.Map[Block], EEFS_NAND_MAP_BAD);
    CHECK_EQ(J_Vol.Working.Map[Next], EEFS_NAND_MAP_BAD);
    CHECK_EQ(J_Vol.Mirror[0].Count, Count - 2);
    J_CheckMirrorsAgree(&J_Vol);
    CHECK_EQ(NandSim_Violations(J_Sim), 0);

    /* both mirrors failing: EEFS_IO_ERROR, nothing committed, the batch still pending */
    NandSim_InjectProgramFail(J_Sim, NANDSIM_ANY, NANDSIM_ANY, -1);
    J_SetEntry(&J_Vol, 9, "/fail");
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_IO_ERROR);
    CHECK(J_Vol.Working.Entry[9].InUse);
    CHECK(EEFS_NandHasUncommitted(&J_Vol, FALSE));
    J_Destroy();
}

static void pending_cleanup_rerun_before_first_mutating_op(void)
{
    uint32  Pages;
    uint32  Block = J_DataStart() + 2;
    uint32  Programs;

    J_MakeImage3();
    J_RemoveEntry(&J_Vol, 1);
    Pages = J_TxnPages(&J_Vol);
    NandSim_SetPowerBudget(J_Sim, 2 * Pages, NANDSIM_TORN_NONE);     /* both records, then the cleanup erase is cut */
    (void)EEFS_LibCommit(&J_Vol);
    CHECK(NandSim_PowerCutHappened(J_Sim));
    NandSim_Reboot(J_Sim);

    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.PendingCleanupBase, 1);
    CHECK_EQ(J_Vol.PendingCleanup[0], Block);
    CHECK(!NandSim_PageIsErased(J_Sim, Block, 0));
    CHECK_EQ(J_Vol.Working.Map[Block], EEFS_NAND_MAP_FREE);

    /* a commit with nothing else to do runs the cleanup and writes no record */
    Programs = NandSim_Programs(J_Sim);
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    CHECK(NandSim_PageIsErased(J_Sim, Block, 0));
    CHECK_EQ(NandSim_Programs(J_Sim), Programs);
    CHECK_EQ(J_Vol.PendingCleanupCount, 0);
    CHECK_EQ(J_Vol.PendingCleanupBase, 0);

    /* a later remount still lists it (it is in the newest record) but there is nothing left to erase */
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.PendingCleanupBase, 1);
    CHECK_RC(EEFS_NandRunPendingCleanup(&J_Vol), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.PendingCleanupCount, 0);

    /* cleanup list helpers */
    CHECK_RC(EEFS_NandAddCleanup(&J_Vol, 100), EEFS_SUCCESS);
    CHECK_RC(EEFS_NandAddCleanup(&J_Vol, 100), EEFS_SUCCESS);
    CHECK_EQ(J_Vol.PendingCleanupCount, 1);
    CHECK(EEFS_NandCleanupHasRoom(&J_Vol, EEFS_NAND_MAX_CLEANUP_BLOCKS - 1));
    CHECK(!EEFS_NandCleanupHasRoom(&J_Vol, EEFS_NAND_MAX_CLEANUP_BLOCKS));
    EEFS_NandForgetCleanup(&J_Vol, 100);
    CHECK_EQ(J_Vol.PendingCleanupCount, 0);
    for (Pages = 0; Pages < EEFS_NAND_MAX_CLEANUP_BLOCKS; Pages++) {
        CHECK_RC(EEFS_NandAddCleanup(&J_Vol, (uint16)(200 + Pages)), EEFS_SUCCESS);
    }
    CHECK_RC(EEFS_NandAddCleanup(&J_Vol, 199), EEFS_TRANSACTION_ERROR);
    J_Vol.PendingCleanupCount = 0;
    J_Destroy();
}

/*
 * Power-cut sweeps
 */

static NandSim_t   *J_Image;

/* Every program and erase of a commit of 3 entries (modify, remove with cleanup, add) */
static void powercut_sweep_commit(void)
{
    J_Image = J_MakeImage3();
    J_Sim = NULL;
    memset(&J_Vol, 0, sizeof(J_Vol));
    J_Expect(J_Image, J_Batch3, J_RunCommit);
    CHECK(J_Sweep("commit", J_Image, J_Batch3, J_RunCommit) >= 3);
    NandSim_Destroy(J_Image);
}

/* J_Batch3 plus enough new entries for a TXN of at least three pages */
static void J_BatchMany(void)
{
    uint32  PerPage = (NandTest_Geo.PageSize - EEFS_NAND_REC_HDR_SIZE) / (EEFS_NAND_ITEM_HDR_SIZE + EEFS_NAND_ENTRY_ITEM_SIZE);
    uint32  n = 2 * PerPage + 1;
    uint32  i;
    char    Path[32];

    if (n > 40) n = 40;
    J_Batch3();
    for (i = 0; i < n; i++) {
        sprintf(Path, "/many/%lu", (unsigned long)i);
        J_SetEntry(&J_Vol, 10 + i, Path);
    }
}

static void powercut_sweep_commit_multipage_txn(void)
{
    J_Image = J_MakeImage3();
    J_Sim = NULL;
    memset(&J_Vol, 0, sizeof(J_Vol));
    J_Expect(J_Image, J_BatchMany, J_RunCommit);
    CHECK(J_Sweep("commit with a multi-page TXN", J_Image, J_BatchMany, J_RunCommit) >= 5);
    NandSim_Destroy(J_Image);
}

/* The same commit when the TXN does not fit: both mirrors enter the next ring block (erase + CHECKPOINT).  Ring
 * blocks 1 of each mirror are factory bad, so the entered block is 2. */
static void powercut_sweep_commit_enters_block(void)
{
    uint32  D = J_DataStart();

    J_Attach(NandTest_NewSim());
    NandSim_SetFactoryBad(J_Sim, 1);
    NandSim_SetFactoryBad(J_Sim, NandTest_Geo.DirRegionBlocks / 2 + 1);
    CHECK_RC(J_Format(), EEFS_SUCCESS);
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    J_AddFile(&J_Vol, 0, "/cf/apps/a.so", D, 2);
    J_AddFile(&J_Vol, 1, "/cf/apps/b.so", D + 2, 3);
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    J_FillBlocks();
    J_File[2][0] = D + 10;
    J_File[2][1] = 2;
    J_File[2][2] = J_Vol.NextWriteSeq++;
    J_WritePage0("/cf/apps/new.so", J_File[2][0], J_File[2][1], J_File[2][2], 2);
    J_Cleanup[0] = (uint16)(D + 2);
    J_CleanupCount = 1;
    J_Image = J_Sim;
    J_Sim = NULL;
    memset(&J_Vol, 0, sizeof(J_Vol));

    J_Expect(J_Image, J_Batch3, J_RunCommit);
    CHECK(J_Sweep("commit entering a ring block", J_Image, J_Batch3, J_RunCommit) >= 5);
    NandSim_Destroy(J_Image);
}

/* A commit while mirror 1 lags one record (resync CHECKPOINT into a new block, then the TXN in both, then cleanup) */
static void powercut_sweep_commit_with_resync(void)
{
    uint32  Pages;

    J_MakeImage3();
    CHECK_RC(J_TouchCommit(&J_Vol, 4), EEFS_SUCCESS);
    J_Vol.Working.Entry[4].CreationDate++;
    Pages = J_TxnPages(&J_Vol);
    NandSim_SetPowerBudget(J_Sim, Pages, NANDSIM_TORN_NONE);
    (void)EEFS_LibCommit(&J_Vol);
    CHECK(NandSim_PowerCutHappened(J_Sim));
    NandSim_Reboot(J_Sim);
    J_Image = J_Sim;
    J_Sim = NULL;
    memset(&J_Vol, 0, sizeof(J_Vol));

    J_Attach(NandSim_Clone(J_Image));
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    CHECK(J_Vol.Mirror[1].NeedsResync);
    J_Destroy();

    J_Expect(J_Image, J_Batch3, J_RunCommit);
    CHECK(J_Sweep("commit with mirror 1 resync", J_Image, J_Batch3, J_RunCommit) >= 5);
    NandSim_Destroy(J_Image);
}

/* A commit that starts by finishing the cleanup a previous commit left pending */
static void J_BatchModify(void)
{
    J_Vol.Working.Entry[0].Attributes ^= EEFS_ATTRIBUTE_READONLY;
}

static void powercut_sweep_pending_cleanup_then_commit(void)
{
    uint32  Pages;

    J_MakeImage3();
    J_RemoveEntry(&J_Vol, 1);
    Pages = J_TxnPages(&J_Vol);
    NandSim_SetPowerBudget(J_Sim, 2 * Pages, NANDSIM_TORN_NONE);
    (void)EEFS_LibCommit(&J_Vol);
    CHECK(NandSim_PowerCutHappened(J_Sim));
    NandSim_Reboot(J_Sim);
    J_Image = J_Sim;
    J_Sim = NULL;
    memset(&J_Vol, 0, sizeof(J_Vol));

    J_Expect(J_Image, J_BatchModify, J_RunCommit);
    CHECK(J_Sweep("pending cleanup then commit", J_Image, J_BatchModify, J_RunCommit) >= 3);
    NandSim_Destroy(J_Image);
}

/* RepairDirectory with a small batch: both mirrors enter a new block with a CHECKPOINT */
static void powercut_sweep_repair_directory(void)
{
    J_Image = J_MakeImage3();
    J_Sim = NULL;
    memset(&J_Vol, 0, sizeof(J_Vol));
    J_Expect(J_Image, J_BatchModify, J_RunRepair);
    CHECK(J_Sweep("repair directory", J_Image, J_BatchModify, J_RunRepair) >= 4);
    NandSim_Destroy(J_Image);
}

/* Double faults on the commit, the ring entry and the resync scenarios */
static void powercut_double_sweep_commit(void)
{
    J_Image = J_MakeImage3();
    J_Sim = NULL;
    memset(&J_Vol, 0, sizeof(J_Vol));
    CHECK(J_SweepTwice("commit", J_Image, J_BatchMany, J_RunCommit) > 0);
    NandSim_Destroy(J_Image);
}

static void powercut_double_sweep_enters_block(void)
{
    uint32  D = J_DataStart();

    J_Attach(NandTest_NewSim());
    CHECK_RC(J_Format(), EEFS_SUCCESS);
    CHECK_RC(J_Mount(&J_Vol), EEFS_SUCCESS);
    J_AddFile(&J_Vol, 0, "/cf/apps/a.so", D, 2);
    J_AddFile(&J_Vol, 1, "/cf/apps/b.so", D + 2, 3);
    CHECK_RC(EEFS_LibCommit(&J_Vol), EEFS_SUCCESS);
    J_FillBlocks();
    J_File[2][0] = D + 10;
    J_File[2][1] = 2;
    J_File[2][2] = J_Vol.NextWriteSeq++;
    J_WritePage0("/cf/apps/new.so", J_File[2][0], J_File[2][1], J_File[2][2], 2);
    J_Cleanup[0] = (uint16)(D + 2);
    J_CleanupCount = 1;
    J_Image = J_Sim;
    J_Sim = NULL;
    memset(&J_Vol, 0, sizeof(J_Vol));
    CHECK(J_SweepTwice("commit entering a ring block", J_Image, J_Batch3, J_RunCommit) > 0);
    NandSim_Destroy(J_Image);
}

/*
 * Suite table
 */

const NandTest_Case_t NandTest_JournalCases[] = {
    { "format_rejects_512_page",                            format_rejects_512_page },
    { "format_rejects_bad_geometry",                        format_rejects_bad_geometry },
    { "format_scans_markers_before_erase",                  format_scans_markers_before_erase },
    { "format_erases_all_good_blocks",                      format_erases_all_good_blocks },
    { "format_checkpoint_seq1_both_mirrors",                format_checkpoint_seq1_both_mirrors },
    { "format_fails_with_one_good_block_in_mirror",         format_fails_with_one_good_block_in_mirror },
    { "mount_empty_volume_reads_only",                      mount_empty_volume_reads_only },
    { "mount_unformatted_returns_not_formatted",            mount_unformatted_returns_not_formatted },
    { "mount_geometry_mismatch",                            mount_geometry_mismatch },
    { "mount_reconcile_follows_page0",                      mount_reconcile_follows_page0 },
    { "writeseq_gap_after_mount",                           writeseq_gap_after_mount },
    { "commit_nothing_dirty_writes_nothing",                commit_nothing_dirty_writes_nothing },
    { "commit_txn_visible_after_remount",                   commit_txn_visible_after_remount },
    { "commit_entry_only_in_use_in_checkpoint",             commit_entry_only_in_use_in_checkpoint },
    { "abort_restores_working_and_keeps_bad_marks",         abort_restores_working_and_keeps_bad_marks },
    { "abort_erases_batch_page0s",                          abort_erases_batch_page0s },
    { "freefs_and_abort_busy_rules",                        freefs_and_abort_busy_rules },
    { "freeing_is_map_state_only",                          freeing_is_map_state_only },
    { "ring_rotation_300_commits",                          ring_rotation_300_commits },
    { "ring_wraps_around_factory_bad_dir_blocks",           ring_wraps_around_factory_bad_dir_blocks },
    { "mirror0_erased_mounts_from_mirror1_and_resyncs",     mirror0_erased_mounts_from_mirror1_and_resyncs },
    { "mirror_lagging_one_record",                          mirror_lagging_one_record },
    { "torn_tail_record_ignored_and_block_abandoned",       torn_tail_record_ignored_and_block_abandoned },
    { "torn_entry_checkpoint_falls_back_to_previous_block", torn_entry_checkpoint_falls_back_to_previous_block },
    { "dir_block_program_failure_marks_bad_and_retries",    dir_block_program_failure_marks_bad_and_retries },
    { "pending_cleanup_rerun_before_first_mutating_op",     pending_cleanup_rerun_before_first_mutating_op },
    { "powercut_sweep_commit",                              powercut_sweep_commit },
    { "powercut_sweep_commit_multipage_txn",                powercut_sweep_commit_multipage_txn },
    { "powercut_sweep_commit_enters_block",                 powercut_sweep_commit_enters_block },
    { "powercut_sweep_commit_with_resync",                  powercut_sweep_commit_with_resync },
    { "powercut_sweep_pending_cleanup_then_commit",         powercut_sweep_pending_cleanup_then_commit },
    { "powercut_sweep_repair_directory",                    powercut_sweep_repair_directory },
    { "powercut_sweep_format",                              powercut_sweep_format },
    { "powercut_double_sweep_commit",                       powercut_double_sweep_commit },
    { "powercut_double_sweep_enters_block",                 powercut_double_sweep_enters_block },
    { NULL,                                                 NULL }
};

/************************/
/*  End of File Comment */
/************************/
