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
 * Filename: test_rebuild.c
 *
 * Purpose: Suite "rebuild": rebuilding the directory from the metadata pages when neither mirror holds a valid
 *   checkpoint (docs/nand-implementation.md sections 5.2 step 5, 8.1-8.3, 14 phase 4; docs/nand-format.md section 6).
 *
 * Design Notes:
 *   The directory is destroyed by erasing (or corrupting) the whole directory region in the simulator; the mount that
 *   follows must succeed without programming or erasing anything, set DirectoryRebuilt, and produce the committed
 *   state.  Entry indices of a rebuilt directory follow the block order of the metadata pages, so rebuilt states are
 *   compared with the committed one by path (R_SameByPath).
 *
 *   The sweeps follow the pattern of test_powercut.c: one clone of an image per cut point and torn mode, the operation
 *   cut after that many programs and erases, a reboot, then (here) the directory region erased and a rebuilding mount.
 *   The rebuilt state must be a consistent one (exactly the state before or after the operation where that is
 *   decidable, or for the mixed batch a per-file choice between the versions the batch could leave), every slot must
 *   verify (no torn or superseded copy selected), and RepairDirectory must persist it: the remount after it reads the
 *   checkpoint and gives the same state, with both mirrors in agreement.
 *
 */

/*
 * Includes
 */

#include "nand_vol.h"

/*
 * Macro Definitions
 */

#define R_SWEEP_BLOCKS      112     /* data blocks of the sweep device (the sweeps rebuild after every cut) */
#define R_MAX_NAMES         (EEFS_MAX_FILES + 1)

/*
 * Type Definitions
 */

typedef int32 (*R_Run_t)(void);
typedef int   (*R_Check_t)(int Done);       /* checks the mounted state; returns its phase (non-decreasing in the cut) */

/*
 * Local Data
 */

static EEFS_InodeTable_t    R_Expect;       /* a committed (or rebuilt) state to compare with */
static EEFS_InodeTable_t    R_Save;
static NandSim_t           *R_Image;
static V_Snap_t             R_SnapA;
static V_Snap_t             R_SnapB;
static uint8                R_Raw[EEFS_NAND_MAX_PAGE_SIZE];
static uint8                R_Oob[EEFS_NAND_MAX_OOB_SIZE];
static uint16               R_Blocks[1200];
static uint32               R_Crcs[1200];
static char                 R_Names[R_MAX_NAMES][24];

static const char          *R_ModeName[3] = { "NONE", "PARTIAL", "GARBAGE" };

/* Scenario files */
static V_File_t             R_X, R_Y, R_M, R_M2, R_Old, R_New;
static V_File_t             R_R1, R_R2, R_N, R_N2, R_K, R_Bm, R_Bm2, R_NewF[5];

/*
 * Helpers
 */

static V_File_t R_File(const char *Path, uint32 Size, uint32 Seed)
{
    V_File_t    F;

    memset(&F, 0, sizeof(F));
    F.Path = Path;
    F.Size = Size;
    F.Seed = Seed;
    return(F);
}

static V_File_t R_Mod(V_File_t F, uint32 Offset)
{
    if (F.ModCount < V_MAX_MODS) {
        F.Mod[F.ModCount++] = Offset;
    }
    return(F);
}

/* Destroys the directory: both mirrors erased (a factory-bad block keeps its marker) */
static void R_EraseDir(void)
{
    uint32  b;

    for (b = 0; b < NandTest_Geo.DirRegionBlocks; b++) {
        NandSim_RawRead(V_Sim, b, 0, NULL, R_Oob);
        if (R_Oob[NandTest_Geo.BadMarkerOffset] != 0xFF) continue;
        NandSim_RawErase(V_Sim, b);
    }
}

/* Destroys the directory without erasing it: every checkpoint is damaged (page 1 when it has one, so the header of
 * page 0 still decodes and its Seq is seen by the scan) */
static void R_CorruptDir(void)
{
    EEFS_NandRecHdr_t   H;
    uint32              b;

    for (b = 0; b < NandTest_Geo.DirRegionBlocks; b++) {
        NandSim_RawRead(V_Sim, b, 0, R_Raw, NULL);
        if (EEFS_NandDecodeRecordPage(R_Raw, NandTest_Geo.PageSize, &H) != EEFS_SUCCESS) continue;
        if (H.RecType != EEFS_NAND_REC_CHECKPOINT) continue;
        NandSim_CorruptBytes(V_Sim, b, (H.PageCount > 1) ? 1 : 0, 40, 1);
    }
}

/* A mount that must rebuild: no programs or erases, DirectoryRebuilt */
static int32 R_MountRebuilt(void)
{
    uint32  Ops = V_Ops();
    int32   Status;

    Status = V_Mount(&V_Vol);
    CHECK_EQ(V_Ops(), Ops);
    if (Status == EEFS_SUCCESS) {
        CHECK_EQ(V_Vol.DirectoryRebuilt, 1);
        CHECK_EQ(V_Vol.Base.WriteSeqHigh, V_Vol.Working.WriteSeqHigh);
        CHECK(!V_Vol.Mirror[0].HasState && !V_Vol.Mirror[1].HasState);
        CHECK(V_Vol.Mirror[0].NeedsResync && V_Vol.Mirror[1].NeedsResync);
    }
    return(Status);
}

static uint32 R_EntriesInUse(const EEFS_InodeTable_t *Vol)
{
    uint32  i;
    uint32  n = 0;

    for (i = 0; i < EEFS_MAX_FILES; i++) {
        if (Vol->Working.Entry[i].InUse) n++;
    }
    return(n);
}

static int32 R_FindPath(const EEFS_InodeTable_t *Vol, const char *Path)
{
    uint32  i;

    for (i = 0; i < EEFS_MAX_FILES; i++) {
        if (Vol->Working.Entry[i].InUse && (strcmp(Vol->Working.Entry[i].Path, Path) == 0)) return((int32)i);
    }
    return(-1);
}

/* The two states hold the same files with the same entry fields and slot records, and the same data-region map, with
 * owners matched by path (entry indices may differ).  Prints the first difference. */
static int R_SameByPath(const EEFS_InodeTable_t *A, const EEFS_InodeTable_t *B)
{
    const EEFS_NandEntry_t *Ea;
    const EEFS_NandEntry_t *Eb;
    int32                   Map[EEFS_MAX_FILES];
    uint32                  i;
    uint32                  b;
    uint8                   va;
    uint8                   vb;
    uint8                   Expected;

    if (R_EntriesInUse(A) != R_EntriesInUse(B)) {
        printf("    %lu entries in use, expected %lu\n", (unsigned long)R_EntriesInUse(B), (unsigned long)R_EntriesInUse(A));
        return(0);
    }
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        Map[i] = -1;
        Ea = &A->Working.Entry[i];
        if (!Ea->InUse) continue;
        Map[i] = R_FindPath(B, Ea->Path);
        if (Map[i] < 0) {
            printf("    %s missing\n", Ea->Path);
            return(0);
        }
        Eb = &B->Working.Entry[Map[i]];
        if ((Ea->IsAB != Eb->IsAB) || (Ea->ActiveSlot != Eb->ActiveSlot) || (Ea->FallbackCompatible != Eb->FallbackCompatible) ||
            (Ea->ActiveFailed != Eb->ActiveFailed) || (Ea->FileType != Eb->FileType) ||
            (Ea->Attributes != Eb->Attributes) || (Ea->CreationDate != Eb->CreationDate) ||
            (memcmp(Ea->Slot, Eb->Slot, sizeof(Ea->Slot)) != 0)) {
            printf("    %s: entry differs\n", Ea->Path);
            return(0);
        }
    }
    for (b = NandTest_Geo.DirRegionBlocks; b < NandTest_Geo.BlockCount; b++) {
        va = A->Working.Map[b];
        vb = B->Working.Map[b];
        if (va == EEFS_NAND_MAP_FREEING) va = EEFS_NAND_MAP_FREE;
        Expected = va;
        if (EEFS_NAND_MAP_IS_OWNER(va)) {
            Expected = (Map[EEFS_NAND_MAP_OWNER_ENTRY(va)] < 0) ? 0xEE :
                       EEFS_NAND_MAP_OWNER(Map[EEFS_NAND_MAP_OWNER_ENTRY(va)], EEFS_NAND_MAP_OWNER_SLOT(va));
        }
        if (vb != Expected) {
            printf("    block %lu: map 0x%02x, expected 0x%02x\n", (unsigned long)b, (unsigned)vb, (unsigned)Expected);
            return(0);
        }
    }
    return(1);
}

/* Quiet content check: the file exists with exactly this size, attributes and content */
static int R_FileIs(const V_File_t *F)
{
    EEFS_Stat_t     St;
    int32           Fd;
    int32           n = 0;
    uint32          Total = 0;
    int             Same;

    if (EEFS_LibStat(&V_Vol, (char *)F->Path, &St) != EEFS_SUCCESS) return(0);
    if ((St.FileSize != F->Size) || (St.Attributes != F->Attributes)) return(0);
    if ((Fd = EEFS_LibOpen(&V_Vol, (char *)F->Path, O_RDONLY, 0)) < 0) return(0);
    for (;;) {
        n = EEFS_LibRead(Fd, &V_Read[Total], 100003UL);
        if (n <= 0) break;
        Total += (uint32)n;
        if (Total > F->Size) break;
    }
    EEFS_LibClose(Fd);
    Same = ((n == 0) && (Total == F->Size) && (memcmp(V_Read, V_Content(F), F->Size) == 0));
    return(Same);
}

static int R_Absent(const char *Path)
{
    EEFS_Stat_t     St;

    return(EEFS_LibStat(&V_Vol, (char *)Path, &St) == EEFS_FILE_NOT_FOUND);
}

/* Exactly these files, each with exactly this content */
static int R_StateIs(const V_File_t *Files, uint32 Count)
{
    uint32  i;

    if (R_EntriesInUse(&V_Vol) != Count) return(0);
    for (i = 0; i < Count; i++) {
        if (!R_FileIs(&Files[i])) return(0);
    }
    return(1);
}

/* Every slot in use passes the full check (page 0, every block CRC, the file CRC), and none is flagged */
static void R_VerifyAll(void)
{
    char    Name[EEFS_NAND_MAX_PATH_SIZE + 2];
    uint32  i;
    uint32  s;

    for (i = 0; i < EEFS_MAX_FILES; i++) {
        if (!V_Vol.Working.Entry[i].InUse) continue;
        for (s = 0; s < 2; s++) {
            if (!V_Vol.Working.Entry[i].Slot[s].InUse) continue;
            CHECK_EQ(V_Vol.SlotFlags[i][s], 0);
            strcpy(Name, V_Vol.Working.Entry[i].Path);
            if (V_Vol.Working.Entry[i].IsAB) strcat(Name, (s == 0) ? "@A" : "@B");
            CHECK_RC(EEFS_LibVerifyFile(&V_Vol, Name), EEFS_SUCCESS);
        }
    }
}

/* NextWriteSeq is above every metadata page on the device */
static void R_CheckNextWriteSeq(void)
{
    EEFS_NandPage0_t    P;
    uint32              b;

    for (b = NandTest_Geo.DirRegionBlocks; b < NandTest_Geo.BlockCount; b++) {
        if (!NandSim_PageIsProgrammed(V_Sim, b, 0)) continue;
        NandSim_RawRead(V_Sim, b, 0, R_Raw, NULL);
        if (EEFS_NandDecodePage0(R_Raw, NandTest_Geo.PageSize, NandTest_Geo.PagesPerBlock, b, &P) != EEFS_SUCCESS) continue;
        CHECK(V_Vol.NextWriteSeq > P.WriteSeq);
    }
}

/* Commits with the cleanup erases cut (the record is written to both mirrors, the superseded first blocks keep their
 * metadata pages), then remounts: the committed state with the cleanup pending */
static void R_CommitSkipCleanup(void)
{
    NandSim_t  *Orig = V_Sim;
    uint32      Pending = V_Vol.PendingCleanupCount;
    uint32      Before;
    uint32      Ops;

    CHECK(Pending > 0);
    memcpy(&R_Save, &V_Vol, sizeof(R_Save));
    V_Attach(NandSim_Clone(Orig));
    Before = V_Ops();
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    Ops = V_Ops() - Before;
    NandSim_Destroy(V_Sim);

    V_Attach(Orig);
    memcpy(&V_Vol, &R_Save, sizeof(V_Vol));
    NandSim_SetPowerBudget(Orig, Ops - Pending, NANDSIM_TORN_NONE);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK(NandSim_PowerCutHappened(Orig));
    NandSim_Reboot(Orig);
    EEFS_LibInit();
    V_Remount();
    CHECK_EQ(V_Vol.PendingCleanupBase, Pending);
}

/* Programs an OOB bad-block marker on page 0 of Block (as EEFS_NandMarkBad does), leaving the data as it is */
static void R_SetGrownBadMarker(uint32 Block)
{
    NandSim_RawRead(V_Sim, Block, 0, R_Raw, R_Oob);
    R_Oob[NandTest_Geo.BadMarkerOffset] = 0x00;
    NandSim_RawWrite(V_Sim, Block, 0, NULL, R_Oob);
}

static void R_ImageEnd(void)
{
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    R_Image = V_Sim;
    V_Sim = NULL;
    memset(&V_Vol, 0, sizeof(V_Vol));
}

static void R_ImageFree(void)
{
    if (R_Image != NULL) {
        NandSim_Destroy(R_Image);
        R_Image = NULL;
    }
}

/* The sweep: for every cut point and torn mode, run the operation on a clone of R_Image, reboot, erase the directory
 * region, rebuild, check the state, persist it with RepairDirectory and check the remount.  Returns the number of cut
 * points of the uninterrupted run. */
static uint32 R_Sweep(const char *Name, R_Run_t Run, R_Check_t Check)
{
    uint32      Cut;
    uint32      Cuts = 0;
    int         Mode;
    int         Done;
    int         Phase;
    int         Last;
    int32       Status;

    for (Mode = NANDSIM_TORN_NONE; Mode <= NANDSIM_TORN_GARBAGE; Mode++) {
        Last = 0;
        for (Cut = 0; ; Cut++) {
            V_Attach(NandSim_Clone(R_Image));
            CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);
            NandSim_SetPowerBudget(V_Sim, Cut, Mode);
            Status = Run();
            Done = !NandSim_PowerCutHappened(V_Sim);
            if (Done) CHECK_RC(Status, EEFS_SUCCESS);
            NandSim_Reboot(V_Sim);
            EEFS_LibInit();

            R_EraseDir();
            Status = R_MountRebuilt();
            CHECK_RC(Status, EEFS_SUCCESS);
            if (Status == EEFS_SUCCESS) {
                Phase = Check(Done);
                CHECK(Phase >= Last);           /* never back to an earlier state after a later one */
                Last = Phase;
                R_VerifyAll();
                R_CheckNextWriteSeq();

                /* persisted by RepairDirectory: the remount reads the checkpoint and gives the same state */
                V_Take(&V_Vol, &R_SnapA);
                CHECK_RC(EEFS_LibRepairDirectory(&V_Vol), EEFS_SUCCESS);
                CHECK_EQ(V_Vol.DirectoryRebuilt, 0);
                V_Remount();
                CHECK_EQ(V_Vol.DirectoryRebuilt, 0);
                V_Take(&V_Vol, &R_SnapB);
                R_SnapB.LastSeq = R_SnapA.LastSeq;
                CHECK(V_Same(&R_SnapA, &R_SnapB));
                V_CheckMirrorsAgree(&V_Vol);
                CHECK_EQ(Check(Done), Phase);
            }
            CHECK_EQ(NandSim_Violations(V_Sim), 0);

            if (NandTest_CaseFailed()) {
                printf("    sweep %s: failure at cut %lu, torn mode %s\n", Name, (unsigned long)Cut, R_ModeName[Mode]);
                V_Destroy();
                return(Cut);
            }
            V_Destroy();
            if (Done) {
                Cuts = Cut;
                break;
            }
        }
    }
    printf("    sweep %s: %lu cut points x 3 torn modes, rebuilt after each\n", Name, (unsigned long)Cuts);
    return(Cuts);
}

/*
 * Rebuild of a committed state
 */

static void rebuild_after_both_mirrors_erased_matches_committed_set(void)
{
    static const char  *Paths[10] = { "/f/0", "/f/1", "/f/2", "/f/3", "/f/4", "/f/5", "/f/6", "/f/7", "/f/8", "/f/9" };
    V_File_t            F[10];
    V_File_t            Ren;
    V_File_t            AbA;
    V_File_t            AbB;
    V_File_t            Bonly;
    V_File_t            Re3;
    EEFS_NandVolumeInfo_t Info;
    uint32              Sizes[10];
    uint32              i;
    int32               e;

    Sizes[0] = 100;
    Sizes[1] = NandTest_Geo.PageSize + 1;
    Sizes[2] = V_D0();
    Sizes[3] = V_D0() + 1;
    Sizes[4] = V_SizeForBlocks(3) + 17;
    Sizes[5] = 0;
    Sizes[6] = 5000;
    Sizes[7] = V_SizeForBlocks(2);
    Sizes[8] = 3000;
    Sizes[9] = 777;

    V_NewVolume();
    for (i = 0; i < 10; i++) {
        F[i] = R_File(Paths[i], Sizes[i], 100 + i);
        CHECK_RC(V_Write(&F[i]), EEFS_SUCCESS);
    }
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);

    /* modifies (copy-on-write, kept blocks shared with nothing after the cleanup), a rename, removes, an attribute */
    F[1] = R_Mod(F[1], 5);
    CHECK_RC(V_Write(&F[1]), EEFS_SUCCESS);
    F[4] = R_Mod(F[4], V_FileOffset(2, 33));
    CHECK_RC(V_Write(&F[4]), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibRename(&V_Vol, "/f/2", "/r/renamed"), EEFS_SUCCESS);
    Ren = F[2];
    Ren.Path = "/r/renamed";
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/f/3"), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/f/5"), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibSetFileAttributes(&V_Vol, "/f/6", EEFS_ATTRIBUTE_READONLY), EEFS_SUCCESS);
    F[6].Attributes = EEFS_ATTRIBUTE_READONLY;
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);

    /* A/B copies, and a removed name written again */
    AbA = R_File("/ab/both@A", 4000, 201);
    AbB = R_File("/ab/both@B", V_D0() + 300, 202);
    Bonly = R_File("/ab/bonly@B", 1500, 203);
    CHECK_RC(V_Write(&AbA), EEFS_SUCCESS);
    CHECK_RC(V_Write(&AbB), EEFS_SUCCESS);
    CHECK_RC(V_Write(&Bonly), EEFS_SUCCESS);
    Re3 = R_File("/f/3", 2222, 204);
    CHECK_RC(V_Write(&Re3), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    F[7] = R_Mod(F[7], V_FileOffset(1, 0));
    CHECK_RC(V_Write(&F[7]), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.PendingCleanupCount, 0);
    memcpy(&R_Expect, &V_Vol, sizeof(R_Expect));

    /* both mirrors lost */
    R_EraseDir();
    CHECK_RC(R_MountRebuilt(), EEFS_SUCCESS);
    CHECK(R_SameByPath(&R_Expect, &V_Vol));
    CHECK_EQ(R_EntriesInUse(&V_Vol), 11);
    for (i = 0; i < 10; i++) {
        if ((i == 2) || (i == 3) || (i == 5)) continue;
        V_CheckFile(&V_Vol, &F[i]);
    }
    V_CheckFile(&V_Vol, &Ren);
    V_CheckFile(&V_Vol, &Re3);
    V_CheckFile(&V_Vol, &AbA);
    V_CheckFile(&V_Vol, &AbB);
    V_CheckFile(&V_Vol, &Bonly);
    V_CheckAbsent(&V_Vol, "/f/2");
    V_CheckAbsent(&V_Vol, "/f/5");
    e = R_FindPath(&V_Vol, "/ab/both");
    CHECK(e >= 0);
    if (e >= 0) {
        CHECK_EQ(V_Vol.Working.Entry[e].IsAB, 1);
        CHECK_EQ(V_Vol.Working.Entry[e].ActiveSlot, EEFS_NAND_SLOT_A);
    }
    e = R_FindPath(&V_Vol, "/ab/bonly");
    CHECK(e >= 0);
    if (e >= 0) {
        CHECK_EQ(V_Vol.Working.Entry[e].ActiveSlot, EEFS_NAND_SLOT_B);
    }
    R_VerifyAll();
    R_CheckNextWriteSeq();
    CHECK_RC(EEFS_LibGetVolumeInfo(&V_Vol, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.DirectoryRebuilt, 1);
    CHECK_EQ(Info.UncommittedChanges, 0);
    CHECK_EQ(Info.FilesInUse, 11);

    /* the next commit persists it */
    F[0] = R_Mod(F[0], 1);
    CHECK_RC(V_Write(&F[0]), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.DirectoryRebuilt, 0);
    memcpy(&R_Expect, &V_Vol, sizeof(R_Expect));
    V_Remount();
    CHECK_EQ(V_Vol.DirectoryRebuilt, 0);
    CHECK(R_SameByPath(&R_Expect, &V_Vol));
    V_CheckMirrorsAgree(&V_Vol);
    V_CheckFile(&V_Vol, &F[0]);
    V_CheckFile(&V_Vol, &AbB);
    CHECK_EQ(NandSim_Violations(V_Sim), 0);
    V_Destroy();
}

/* Two metadata pages for one path: the higher WriteSeq wins; on equal WriteSeq the lower block wins */
static void rebuild_highest_writeseq_wins(void)
{
    EEFS_NandPage0_t    P;
    V_File_t            Pad;
    V_File_t            V1;
    V_File_t            V2;
    uint32              PadBlock;
    uint32              First1;
    uint32              First2;
    uint32              n;
    uint32              k;
    uint32              p;

    V_NewVolume();
    Pad = R_File("/pad", 100, 300);
    CHECK_RC(V_Write(&Pad), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    PadBlock = V_SlotA(&V_Vol, "/pad")->FirstBlock;
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/pad"), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);     /* an erased free block below the others */
    CHECK(NandSim_PageIsErased(V_Sim, PadBlock, 0));

    V1 = R_File("/h", V_SizeForBlocks(2), 301);
    V2 = R_File("/h", V_SizeForBlocks(2), 302);         /* every position differs: no block shared */
    CHECK_RC(V_Write(&V1), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    First1 = V_SlotA(&V_Vol, "/h")->FirstBlock;
    CHECK_RC(V_Write(&V2), EEFS_SUCCESS);
    R_CommitSkipCleanup();
    First2 = V_SlotA(&V_Vol, "/h")->FirstBlock;
    CHECK(First1 != First2);
    CHECK(PadBlock < First2);
    CHECK_EQ(V_RawBlocks(First1, R_Blocks, R_Crcs), 2);     /* the old copy is still on the media */

    R_EraseDir();
    CHECK_RC(R_MountRebuilt(), EEFS_SUCCESS);
    CHECK_EQ(R_EntriesInUse(&V_Vol), 1);
    CHECK_EQ(V_SlotA(&V_Vol, "/h")->FirstBlock, First2);
    V_CheckFile(&V_Vol, &V2);
    for (k = 0; k < 2; k++) {
        CHECK_EQ(V_Vol.Working.Map[R_Blocks[k]], EEFS_NAND_MAP_FREE);      /* the loser's blocks are free */
    }

    /* the same WriteSeq at a lower block: a copy of the winner whose first block is PadBlock */
    n = V_RawBlocks(First2, R_Blocks, R_Crcs);
    NandSim_RawRead(V_Sim, First2, 0, R_Raw, NULL);
    CHECK_RC(EEFS_NandDecodePage0(R_Raw, NandTest_Geo.PageSize, NandTest_Geo.PagesPerBlock, First2, &P), EEFS_SUCCESS);
    for (p = 1; p < NandTest_Geo.PagesPerBlock; p++) {
        if (!NandSim_PageIsProgrammed(V_Sim, First2, p)) break;
        NandSim_RawRead(V_Sim, First2, p, R_Raw, NULL);
        NandSim_RawWrite(V_Sim, PadBlock, p, R_Raw, NULL);
    }
    R_Blocks[0] = (uint16)PadBlock;
    CHECK_RC(EEFS_NandEncodePage0(R_Raw, NandTest_Geo.PageSize, &P, R_Blocks, R_Crcs), EEFS_SUCCESS);
    NandSim_RawWrite(V_Sim, PadBlock, 0, R_Raw, NULL);
    CHECK_EQ(n, 2);

    CHECK_RC(R_MountRebuilt(), EEFS_SUCCESS);
    CHECK_EQ(V_SlotA(&V_Vol, "/h")->FirstBlock, PadBlock);
    CHECK_EQ(V_SlotA(&V_Vol, "/h")->WriteSeq, P.WriteSeq);
    CHECK_EQ(V_Vol.Working.Map[First2], EEFS_NAND_MAP_FREE);
    V_CheckFile(&V_Vol, &V2);
    V_Destroy();
}

/* A committed rename whose cleanup was cut: the old-name page 0 shares Blocks[1..] with the new one and loses to its
 * higher WriteSeq (overlap rule); only the new name is rebuilt */
static void rebuild_overlap_rule_after_cut_rename(void)
{
    uint16      NewBlocks[8];
    uint32      NewCrcs[8];
    uint32      OldFirst;
    uint32      NewFirst;
    uint32      k;

    R_Old = R_File("/old", V_SizeForBlocks(3), 311);
    R_New = R_Old;
    R_New.Path = "/new";

    V_NewVolume();
    CHECK_RC(V_Write(&R_Old), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    OldFirst = V_SlotA(&V_Vol, "/old")->FirstBlock;
    CHECK_RC(EEFS_LibRename(&V_Vol, "/old", "/new"), EEFS_SUCCESS);
    R_CommitSkipCleanup();
    NewFirst = V_SlotA(&V_Vol, "/new")->FirstBlock;
    CHECK_EQ(V_RawBlocks(OldFirst, R_Blocks, R_Crcs), 3);
    CHECK_EQ(V_RawBlocks(NewFirst, NewBlocks, NewCrcs), 3);
    for (k = 1; k < 3; k++) {
        CHECK_EQ(R_Blocks[k], NewBlocks[k]);            /* the two pages share the data blocks */
    }

    R_EraseDir();
    CHECK_RC(R_MountRebuilt(), EEFS_SUCCESS);
    CHECK_EQ(R_EntriesInUse(&V_Vol), 1);
    V_CheckAbsent(&V_Vol, "/old");
    V_CheckFile(&V_Vol, &R_New);
    CHECK_EQ(V_SlotA(&V_Vol, "/new")->FirstBlock, NewFirst);
    CHECK_EQ(V_Vol.Working.Map[OldFirst], EEFS_NAND_MAP_FREE);
    for (k = 1; k < 3; k++) {
        CHECK(EEFS_NAND_MAP_IS_OWNER(V_Vol.Working.Map[NewBlocks[k]]));
    }
    V_CheckSlotsValid(&V_Vol);
    V_Destroy();
}

/* The documented caveats (FMT 6): a file of a batch that was never committed, and a removed file whose cleanup erase
 * was cut, both come back */
static void rebuild_includes_uncommitted_and_uncleaned_copies(void)
{
    V_File_t    Keep = R_File("/keep", 3000, 321);
    V_File_t    Gone = R_File("/gone", V_D0() + 10, 322);
    V_File_t    Unc = R_File("/uncommitted", 4500, 323);

    V_NewVolume();
    CHECK_RC(V_Write(&Keep), EEFS_SUCCESS);
    CHECK_RC(V_Write(&Gone), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);

    /* a write that is never committed (reboot), then a removal whose cleanup erase is cut */
    CHECK_RC(V_Write(&Unc), EEFS_SUCCESS);
    EEFS_LibInit();
    V_Remount();
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/gone"), EEFS_SUCCESS);
    R_CommitSkipCleanup();

    /* the directory has neither */
    V_CheckAbsent(&V_Vol, "/gone");
    V_CheckAbsent(&V_Vol, "/uncommitted");
    CHECK_EQ(R_EntriesInUse(&V_Vol), 1);

    R_EraseDir();
    CHECK_RC(R_MountRebuilt(), EEFS_SUCCESS);
    CHECK_EQ(R_EntriesInUse(&V_Vol), 3);
    V_CheckFile(&V_Vol, &Keep);
    V_CheckFile(&V_Vol, &Gone);         /* caveat: uncleaned copy */
    V_CheckFile(&V_Vol, &Unc);          /* caveat: uncommitted file */
    R_CheckNextWriteSeq();
    V_Destroy();
}

/* EEFS_NOT_FORMATTED only when there is neither a checkpoint nor a valid metadata page */
static void rebuild_not_formatted_when_no_page0(void)
{
    V_File_t    T = R_File("/t", (2 * NandTest_Geo.PageSize) + 5, 331);
    uint32      First;

    /* blank device */
    V_Attach(NandTest_NewSim());
    CHECK_RC(V_Mount(&V_Vol), EEFS_NOT_FORMATTED);
    CHECK_EQ(V_Vol.Mounted, 0);
    CHECK_EQ(V_Ops(), 0);
    V_Destroy();

    /* an empty volume whose directory is lost */
    V_NewVolume();
    R_EraseDir();
    CHECK_RC(V_Mount(&V_Vol), EEFS_NOT_FORMATTED);
    CHECK_EQ(V_Vol.Mounted, 0);

    /* data that is not a metadata page */
    memset(R_Raw, 0x45, NandTest_Geo.PageSize);
    NandSim_RawWrite(V_Sim, NandTest_Geo.DirRegionBlocks + 7, 0, R_Raw, NULL);
    CHECK_RC(V_Mount(&V_Vol), EEFS_NOT_FORMATTED);
    V_Destroy();

    /* a valid metadata page whose first block fails its CRC is a page 0 all the same: the volume mounts, empty */
    V_NewVolume();
    CHECK_RC(V_Write(&T), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    First = V_SlotA(&V_Vol, "/t")->FirstBlock;
    NandSim_CorruptBytes(V_Sim, First, 2, 0, 1);
    R_EraseDir();
    CHECK_RC(R_MountRebuilt(), EEFS_SUCCESS);
    CHECK_EQ(R_EntriesInUse(&V_Vol), 0);
    CHECK_EQ(V_Vol.Working.Map[First], EEFS_NAND_MAP_FREE);
    CHECK(V_Vol.NextWriteSeq > V_Vol.Working.WriteSeqHigh);
    CHECK_RC(EEFS_LibRepairDirectory(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    CHECK_EQ(V_Vol.DirectoryRebuilt, 0);
    CHECK_EQ(R_EntriesInUse(&V_Vol), 0);
    V_Destroy();
}

/* Bad blocks come back from the OOB markers: factory-bad blocks of both regions, a grown bad block the directory
 * recorded, and a marked block the directory never knew about */
static void rebuild_bad_blocks_from_oob(void)
{
    V_File_t    A = R_File("/a", V_SizeForBlocks(2), 341);
    V_File_t    B = R_File("/b", 6000, 342);
    uint32      Dir = NandTest_Geo.DirRegionBlocks;
    uint32      Factory[2];
    uint32      Grown = 0;
    uint32      Unrecorded = 0;
    uint32      b;
    uint32      n;

    Factory[0] = Dir + 5;
    Factory[1] = Dir + 40;
    V_Attach(NandTest_NewSim());
    NandSim_SetFactoryBad(V_Sim, Factory[0]);
    NandSim_SetFactoryBad(V_Sim, Factory[1]);
    NandSim_SetFactoryBad(V_Sim, 3);                    /* a directory block of mirror 0 */
    memset(&V_Aux, 0, sizeof(V_Aux));
    CHECK_RC(EEFS_LibFormat(&V_Aux, (cpuaddr)&V_Dev), EEFS_SUCCESS);
    CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);

    NandSim_InjectProgramFail(V_Sim, NANDSIM_ANY, 2, 1);   /* a grown bad block during the write of /a */
    CHECK_RC(V_Write(&A), EEFS_SUCCESS);
    CHECK_RC(V_Write(&B), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    for (b = Dir; b < NandTest_Geo.BlockCount; b++) {
        if ((V_Vol.Working.Map[b] == EEFS_NAND_MAP_BAD) && (b != Factory[0]) && (b != Factory[1])) Grown = b;
        if ((Unrecorded == 0) && (b > Dir + 50) && (V_Vol.Working.Map[b] == EEFS_NAND_MAP_FREE)) Unrecorded = b;
    }
    CHECK(Grown != 0);
    CHECK(Unrecorded != 0);
    R_SetGrownBadMarker(Unrecorded);
    memcpy(&R_Expect, &V_Vol, sizeof(R_Expect));
    R_Expect.Working.Map[Unrecorded] = EEFS_NAND_MAP_BAD;

    R_EraseDir();
    CHECK_RC(R_MountRebuilt(), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.Working.Map[3], EEFS_NAND_MAP_BAD);
    for (n = 0, b = Dir; b < NandTest_Geo.BlockCount; b++) {
        if (V_Vol.Working.Map[b] == EEFS_NAND_MAP_BAD) n++;
    }
    CHECK_EQ(n, 4);
    CHECK_EQ(V_Vol.Working.Map[Grown], EEFS_NAND_MAP_BAD);
    CHECK_EQ(V_Vol.Working.Map[Unrecorded], EEFS_NAND_MAP_BAD);
    CHECK(R_SameByPath(&R_Expect, &V_Vol));
    V_CheckFile(&V_Vol, &A);
    V_CheckFile(&V_Vol, &B);

    /* persisted */
    CHECK_RC(EEFS_LibRepairDirectory(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    CHECK_EQ(V_Vol.DirectoryRebuilt, 0);
    CHECK(R_SameByPath(&R_Expect, &V_Vol));
    CHECK_EQ(V_Vol.Working.Map[3], EEFS_NAND_MAP_BAD);
    V_CheckMirrorsAgree(&V_Vol);
    V_Destroy();
}

/* A rebuilt slot listing a block that carries a bad-block marker keeps the block BAD and is flagged CROSSLINK */
static void rebuild_crosslink_flagged(void)
{
    V_File_t    X = R_File("/x", V_SizeForBlocks(3), 351);
    uint32      First;
    int32       e;

    V_NewVolume();
    CHECK_RC(V_Write(&X), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    First = V_SlotA(&V_Vol, "/x")->FirstBlock;
    CHECK_EQ(V_RawBlocks(First, R_Blocks, R_Crcs), 3);
    R_SetGrownBadMarker(R_Blocks[1]);

    R_EraseDir();
    CHECK_RC(R_MountRebuilt(), EEFS_SUCCESS);
    e = R_FindPath(&V_Vol, "/x");
    CHECK(e >= 0);
    if (e >= 0) {
        CHECK((V_Vol.SlotFlags[e][0] & EEFS_NAND_SLOTFLAG_CROSSLINK) != 0);
        CHECK_EQ(V_Vol.Working.Map[R_Blocks[0]], EEFS_NAND_MAP_OWNER(e, 0));
        CHECK_EQ(V_Vol.Working.Map[R_Blocks[2]], EEFS_NAND_MAP_OWNER(e, 0));
    }
    CHECK_EQ(V_Vol.Working.Map[R_Blocks[1]], EEFS_NAND_MAP_BAD);
    V_CheckFileEx(&V_Vol, &X, FALSE);   /* the data still reads */
    V_Destroy();
}

/* RepairDirectory persists a rebuilt directory with a checkpoint above every Seq seen, and clears the flag; a plain
 * Commit with nothing to write persists it as well */
static void repair_directory_persists_and_clears_flag(void)
{
    static char         Paths[14][8];
    V_File_t            F[14];
    EEFS_NandChain_t    C[2];
    uint32              MaxSeq;
    uint32              i;
    uint8               m;

    V_NewVolume();
    for (i = 0; i < 14; i++) {
        sprintf(Paths[i], "/p/%lu", (unsigned long)i);
        F[i] = R_File(Paths[i], 500 + (i * 300), 400 + i);
        CHECK_RC(V_Write(&F[i]), EEFS_SUCCESS);
        if ((i % 3) == 2) CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    }
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    memcpy(&R_Expect, &V_Vol, sizeof(R_Expect));

    R_CorruptDir();
    CHECK_RC(R_MountRebuilt(), EEFS_SUCCESS);
    CHECK(R_SameByPath(&R_Expect, &V_Vol));
    CHECK_RC(EEFS_NandScanMirror(&V_Vol, 0, &C[0]), EEFS_SUCCESS);
    CHECK_RC(EEFS_NandScanMirror(&V_Vol, 1, &C[1]), EEFS_SUCCESS);
    CHECK(!C[0].Valid && !C[1].Valid);
    MaxSeq = (C[0].MaxSeqSeen > C[1].MaxSeqSeen) ? C[0].MaxSeqSeen : C[1].MaxSeqSeen;
    CHECK_EQ(V_Vol.LastSeq, MaxSeq + 1);

    CHECK_RC(EEFS_LibRepairDirectory(&V_Vol), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.DirectoryRebuilt, 0);
    for (m = 0; m < 2; m++) {
        CHECK_RC(EEFS_NandScanMirror(&V_Vol, m, &C[m]), EEFS_SUCCESS);
        CHECK(C[m].Valid);
        CHECK(C[m].CheckpointSeq > MaxSeq);
        CHECK_EQ(C[m].CheckpointSeq, C[m].LastSeq);
        CHECK_EQ(C[m].LastSeq, V_Vol.LastSeq);
    }
    V_CheckMirrorsAgree(&V_Vol);
    V_Remount();
    CHECK_EQ(V_Vol.DirectoryRebuilt, 0);
    CHECK(V_Vol.Mirror[0].HasState && V_Vol.Mirror[1].HasState);
    CHECK(R_SameByPath(&R_Expect, &V_Vol));
    for (i = 0; i < 14; i++) {
        V_CheckFile(&V_Vol, &F[i]);
    }

    /* a Commit with no change persists a rebuilt state too (the resync checkpoint) */
    R_EraseDir();
    CHECK_RC(R_MountRebuilt(), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.DirectoryRebuilt, 0);
    V_Remount();
    CHECK_EQ(V_Vol.DirectoryRebuilt, 0);
    CHECK(R_SameByPath(&R_Expect, &V_Vol));
    V_CheckMirrorsAgree(&V_Vol);
    V_Destroy();
}

/* EEFS_LibRebuildDirectory: only on an idle volume; rebuilds in RAM, the next commit persists it */
static void rebuild_directory_api_requires_idle(void)
{
    V_File_t                    A = R_File("/a", 7000, 361);
    V_File_t                    B = R_File("/b", V_D0() + 1, 362);
    V_File_t                    C = R_File("/c", 10, 363);
    EEFS_DirectoryDescriptor_t *Dir;
    EEFS_InodeTable_t           Unmounted;
    uint32                      LastSeq;
    int32                       Fd;

    V_NewVolume();
    CHECK_RC(V_Write(&A), EEFS_SUCCESS);
    CHECK_RC(V_Write(&B), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    memcpy(&R_Expect, &V_Vol, sizeof(R_Expect));

    CHECK_RC(EEFS_LibRebuildDirectory(NULL), EEFS_INVALID_ARGUMENT);
    memset(&Unmounted, 0, sizeof(Unmounted));
    CHECK_RC(EEFS_LibRebuildDirectory(&Unmounted), EEFS_INVALID_ARGUMENT);
    Fd = EEFS_LibOpen(&V_Vol, "/a", O_RDONLY, 0);
    CHECK(Fd >= 0);
    CHECK_RC(EEFS_LibRebuildDirectory(&V_Vol), EEFS_DEVICE_IS_BUSY);
    CHECK_RC(EEFS_LibClose(Fd), EEFS_SUCCESS);
    Dir = EEFS_LibOpenDir(&V_Vol);
    CHECK(Dir != NULL);
    CHECK_RC(EEFS_LibRebuildDirectory(&V_Vol), EEFS_DEVICE_IS_BUSY);
    CHECK_RC(EEFS_LibCloseDir(Dir), EEFS_SUCCESS);
    V_Vol.ActiveReport = (struct EEFS_HealthReport *)&Unmounted;     /* a health check in progress */
    CHECK_RC(EEFS_LibRebuildDirectory(&V_Vol), EEFS_DEVICE_IS_BUSY);
    V_Vol.ActiveReport = NULL;
    CHECK_RC(V_Write(&C), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibRebuildDirectory(&V_Vol), EEFS_TRANSACTION_ERROR);
    CHECK_RC(EEFS_LibAbort(&V_Vol), EEFS_SUCCESS);

    /* idle: the RAM directory is rebuilt, nothing is written */
    LastSeq = V_Vol.LastSeq;
    NandSim_ResetCounters(V_Sim);
    CHECK_RC(EEFS_LibRebuildDirectory(&V_Vol), EEFS_SUCCESS);
    CHECK_EQ(V_Ops(), 0);
    CHECK_EQ(V_Vol.DirectoryRebuilt, 1);
    CHECK_EQ(V_Vol.Mounted, 1);
    CHECK(V_Vol.LastSeq > LastSeq);
    CHECK(R_SameByPath(&R_Expect, &V_Vol));
    V_CheckFile(&V_Vol, &A);
    V_CheckFile(&V_Vol, &B);
    V_CheckAbsent(&V_Vol, "/c");
    R_CheckNextWriteSeq();

    /* persisted by the next commit, in fresh ring blocks */
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.DirectoryRebuilt, 0);
    V_Remount();
    CHECK(R_SameByPath(&R_Expect, &V_Vol));
    V_CheckMirrorsAgree(&V_Vol);

    /* a pending cleanup loaded from the media runs first: a committed removal does not come back */
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/b"), EEFS_SUCCESS);
    R_CommitSkipCleanup();
    CHECK_RC(EEFS_LibRebuildDirectory(&V_Vol), EEFS_SUCCESS);
    V_CheckAbsent(&V_Vol, "/b");
    CHECK_EQ(R_EntriesInUse(&V_Vol), 1);
    V_Destroy();

    /* nothing to rebuild from: EEFS_NOT_FORMATTED, and the volume stays mounted with its directory */
    V_NewVolume();
    CHECK_RC(EEFS_LibRebuildDirectory(&V_Vol), EEFS_NOT_FORMATTED);
    CHECK_EQ(V_Vol.Mounted, 1);
    CHECK_EQ(V_Vol.DirectoryRebuilt, 0);
    CHECK_RC(V_Write(&C), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    V_CheckFile(&V_Vol, &C);
    V_Destroy();
}

/* A/B defaults: IsAB from explicit slots, A active if present else B, FallbackCompatible 0, ActiveFailed 0 */
static void rebuild_ab_defaults(void)
{
    V_File_t    Single = R_File("/single", 1000, 371);
    V_File_t    BOnly = R_File("/bonly@B", 1100, 372);
    V_File_t    BothA = R_File("/both@A", 1200, 373);
    V_File_t    BothB = R_File("/both@B", 1300, 374);
    V_File_t    MixA = R_File("/mix", 1400, 375);
    V_File_t    MixB = R_File("/mix@B", 1500, 376);
    V_File_t    MixA2 = R_File("/mix@A", 1400, 375);
    int32       e;

    V_NewVolume();
    CHECK_RC(V_Write(&Single), EEFS_SUCCESS);
    CHECK_RC(V_Write(&BOnly), EEFS_SUCCESS);
    CHECK_RC(V_Write(&BothB), EEFS_SUCCESS);       /* B first: the directory makes B active ... */
    CHECK_RC(V_Write(&BothA), EEFS_SUCCESS);
    CHECK_RC(V_Write(&MixA), EEFS_SUCCESS);        /* written without a slot (page Slot NONE), then B explicitly */
    CHECK_RC(V_Write(&MixB), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);

    R_EraseDir();
    CHECK_RC(R_MountRebuilt(), EEFS_SUCCESS);
    CHECK_EQ(R_EntriesInUse(&V_Vol), 4);
    e = R_FindPath(&V_Vol, "/single");
    CHECK(e >= 0);
    if (e >= 0) {
        CHECK_EQ(V_Vol.Working.Entry[e].IsAB, 0);
        CHECK_EQ(V_Vol.Working.Entry[e].ActiveSlot, EEFS_NAND_SLOT_A);
        CHECK_EQ(V_Vol.Working.Entry[e].Slot[1].InUse, 0);
    }
    e = R_FindPath(&V_Vol, "/bonly");
    CHECK(e >= 0);
    if (e >= 0) {
        CHECK_EQ(V_Vol.Working.Entry[e].IsAB, 1);
        CHECK_EQ(V_Vol.Working.Entry[e].ActiveSlot, EEFS_NAND_SLOT_B);
        CHECK_EQ(V_Vol.Working.Entry[e].Slot[0].InUse, 0);
        CHECK_EQ(V_Vol.Working.Entry[e].Slot[1].Version, 1);
    }
    e = R_FindPath(&V_Vol, "/both");
    CHECK(e >= 0);
    if (e >= 0) {
        CHECK_EQ(V_Vol.Working.Entry[e].IsAB, 1);
        CHECK_EQ(V_Vol.Working.Entry[e].ActiveSlot, EEFS_NAND_SLOT_A);   /* ... a rebuild makes A active */
        CHECK_EQ(V_Vol.Working.Entry[e].FallbackCompatible, 0);
        CHECK_EQ(V_Vol.Working.Entry[e].ActiveFailed, 0);
    }
    e = R_FindPath(&V_Vol, "/mix");
    CHECK(e >= 0);
    if (e >= 0) {
        CHECK_EQ(V_Vol.Working.Entry[e].IsAB, 1);
        CHECK_EQ(V_Vol.Working.Entry[e].ActiveSlot, EEFS_NAND_SLOT_A);
    }
    V_CheckFile(&V_Vol, &Single);
    V_CheckFile(&V_Vol, &BOnly);
    V_CheckFile(&V_Vol, &BothA);
    V_CheckFile(&V_Vol, &BothB);
    V_CheckFile(&V_Vol, &MixA2);
    V_CheckFile(&V_Vol, &MixB);
    R_VerifyAll();
    V_Destroy();
}

/* More paths on the media than EEFS_MAX_FILES: the path whose first metadata page sits at the highest block finds the
 * table full and stays an orphan (its blocks free); every other path is rebuilt */
static void rebuild_max_files_overflow(void)
{
    V_File_t    F[R_MAX_NAMES];
    uint32      First[R_MAX_NAMES];
    uint32      Drop = 0;
    uint32      i;

    V_NewVolume();
    for (i = 0; i < R_MAX_NAMES; i++) {
        if (i < EEFS_MAX_FILES) sprintf(R_Names[i], "/m/%lu", (unsigned long)i);
        else sprintf(R_Names[i], "/extra%lu", (unsigned long)i);
        F[i] = R_File(R_Names[i], 100 + i, 500 + i);
    }
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        CHECK_RC(V_Write(&F[i]), EEFS_SUCCESS);
    }
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK_RC(V_Write(&F[EEFS_MAX_FILES]), EEFS_NO_SPACE_LEFT_ON_DEVICE);   /* the table is full */
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        First[i] = V_SlotA(&V_Vol, F[i].Path)->FirstBlock;
    }

    /* one batch frees an entry and uses it for a new path; the cleanup erase is cut, so the removed file's page 0 stays
     * and the media holds EEFS_MAX_FILES + 1 paths */
    CHECK_RC(EEFS_LibRemove(&V_Vol, (char *)F[0].Path), EEFS_SUCCESS);
    CHECK_RC(V_Write(&F[EEFS_MAX_FILES]), EEFS_SUCCESS);
    R_CommitSkipCleanup();
    First[EEFS_MAX_FILES] = V_SlotA(&V_Vol, F[EEFS_MAX_FILES].Path)->FirstBlock;
    for (i = 1; i < R_MAX_NAMES; i++) {
        if (First[i] > First[Drop]) Drop = i;
    }

    R_EraseDir();
    CHECK_RC(R_MountRebuilt(), EEFS_SUCCESS);
    CHECK_EQ(R_EntriesInUse(&V_Vol), EEFS_MAX_FILES);
    for (i = 0; i < R_MAX_NAMES; i++) {
        if (i == Drop) {
            V_CheckAbsent(&V_Vol, F[i].Path);
            CHECK_EQ(V_Vol.Working.Map[First[i]], EEFS_NAND_MAP_FREE);
        }
        else {
            V_CheckFileEx(&V_Vol, &F[i], (i % 8) == 0);
        }
    }
    V_Destroy();
}

/*
 * Sweeps: the operation cut at every point, then the directory lost
 */

static int32 R_RunRename(void)
{
    int32   Status;

    if ((Status = EEFS_LibRename(&V_Vol, "/old", "/new")) != EEFS_SUCCESS) return(Status);
    return(EEFS_LibCommit(&V_Vol));
}

static int R_CheckRename(int Done)
{
    V_File_t    Pre[2];
    V_File_t    Post[2];

    Pre[0] = R_X;
    Pre[1] = R_Old;
    Post[0] = R_X;
    Post[1] = R_New;
    if (R_StateIs(Post, 2)) return(1);
    CHECK(!Done);
    CHECK(R_StateIs(Pre, 2));               /* exactly one of the names (overlap rule) */
    return(0);
}

static void rebuild_sweep_cut_rename(void)
{
    R_X = R_File("/x", 3000, 601);
    R_Old = R_File("/old", V_SizeForBlocks(3) - 50, 602);
    R_New = R_Old;
    R_New.Path = "/new";
    V_NewVolumeBlocks(NandTest_Geo.DirRegionBlocks + R_SWEEP_BLOCKS);
    CHECK_RC(V_Write(&R_X), EEFS_SUCCESS);
    CHECK_RC(V_Write(&R_Old), EEFS_SUCCESS);
    R_ImageEnd();
    CHECK(R_Sweep("rename", R_RunRename, R_CheckRename) > 0);
    R_ImageFree();
}

static int32 R_RunModify(void)
{
    int32   Status;

    if ((Status = V_Write(&R_M2)) != EEFS_SUCCESS) return(Status);
    return(EEFS_LibCommit(&V_Vol));
}

static int R_CheckModify(int Done)
{
    if (R_StateIs(&R_M2, 1)) return(1);
    CHECK(!Done);
    CHECK(R_StateIs(&R_M, 1));              /* the old copy, never a torn or mixed one */
    return(0);
}

static void rebuild_sweep_cut_modify(void)
{
    R_M = R_File("/boot/image.fit", V_SizeForBlocks(4), 611);
    R_M2 = R_Mod(R_Mod(R_Mod(R_M, 7), V_FileOffset(2, 1000)), V_FileOffset(3, 5));   /* first block and two others */
    V_NewVolumeBlocks(NandTest_Geo.DirRegionBlocks + R_SWEEP_BLOCKS);
    CHECK_RC(V_Write(&R_M), EEFS_SUCCESS);
    R_ImageEnd();
    CHECK(R_Sweep("modify", R_RunModify, R_CheckModify) > 0);
    R_ImageFree();
}

static int32 R_RunWriteNew(void)
{
    int32   Status;

    if ((Status = V_Write(&R_Y)) != EEFS_SUCCESS) return(Status);
    return(EEFS_LibCommit(&V_Vol));
}

static int R_CheckWriteNew(int Done)
{
    V_File_t    Post[2];

    Post[0] = R_X;
    Post[1] = R_Y;
    if (R_StateIs(Post, 2)) return(1);
    CHECK(!Done);
    CHECK(R_StateIs(&R_X, 1));
    return(0);
}

static void rebuild_sweep_cut_new_write(void)
{
    R_X = R_File("/x", 3000, 621);
    R_Y = R_File("/y", V_SizeForBlocks(3) - 100, 622);
    V_NewVolumeBlocks(NandTest_Geo.DirRegionBlocks + R_SWEEP_BLOCKS);
    CHECK_RC(V_Write(&R_X), EEFS_SUCCESS);
    R_ImageEnd();
    CHECK(R_Sweep("write_new", R_RunWriteNew, R_CheckWriteNew) > 0);
    R_ImageFree();
}

/* The mixed batch of test_powercut.c (5 new files, 2 removes, a rename, a modify, one commit and its cleanup) cut at
 * every point, then rebuilt: each name holds one of the versions the batch can leave (an uncommitted new file or an
 * uncleaned removed file may appear, per the caveats), never a torn copy, and the renamed file appears under exactly one
 * of its names */
static int32 R_RunBatch(void)
{
    int32   Status;
    uint32  i;

    for (i = 0; i < 5; i++) {
        if ((Status = V_Write(&R_NewF[i])) != EEFS_SUCCESS) return(Status);
        if (i == 1) {
            if ((Status = EEFS_LibRemove(&V_Vol, "/r1")) != EEFS_SUCCESS) return(Status);
        }
        if (i == 2) {
            if ((Status = EEFS_LibRename(&V_Vol, "/n", "/n2")) != EEFS_SUCCESS) return(Status);
            if ((Status = EEFS_LibRemove(&V_Vol, "/r2")) != EEFS_SUCCESS) return(Status);
        }
        if (i == 3) {
            if ((Status = V_Write(&R_Bm2)) != EEFS_SUCCESS) return(Status);
        }
    }
    return(EEFS_LibCommit(&V_Vol));
}

static int R_CheckBatch(int Done)
{
    uint32  Expected = 0;
    uint32  i;
    int     HasN;
    int     HasN2;

    /* /k is untouched; /m is the old or the new content */
    CHECK(R_FileIs(&R_K));
    CHECK(R_FileIs(&R_Bm) || R_FileIs(&R_Bm2));
    Expected += 2;
    /* the renamed file under exactly one name */
    HasN = !R_Absent("/n");
    HasN2 = !R_Absent("/n2");
    CHECK(HasN != HasN2);
    CHECK(HasN ? R_FileIs(&R_N) : R_FileIs(&R_N2));
    Expected += 1;
    /* removed files: gone, or (caveat) back with their content */
    if (!R_Absent("/r1")) {
        CHECK(R_FileIs(&R_R1));
        Expected++;
    }
    if (!R_Absent("/r2")) {
        CHECK(R_FileIs(&R_R2));
        Expected++;
    }
    /* new files: absent or complete */
    for (i = 0; i < 5; i++) {
        if (!R_Absent(R_NewF[i].Path)) {
            CHECK(R_FileIs(&R_NewF[i]));
            Expected++;
        }
    }
    CHECK_EQ(R_EntriesInUse(&V_Vol), Expected);

    if (Done) {
        /* the whole batch and its cleanup completed */
        CHECK(HasN2);
        CHECK(R_FileIs(&R_Bm2));
        CHECK(R_Absent("/r1") && R_Absent("/r2"));
        CHECK_EQ(Expected, 8);
    }
    return(0);
}

static void rebuild_sweep_batch_then_rebuild(void)
{
    static const char  *Paths[5] = { "/new/0", "/new/1", "/new/2", "/new/3", "/new/4" };
    uint32              Sizes[5];
    uint32              i;

    Sizes[0] = 100;
    Sizes[1] = NandTest_Geo.PageSize + 1;
    Sizes[2] = V_D0() + 5;
    Sizes[3] = 300;
    Sizes[4] = 0;
    R_R1 = R_File("/r1", V_SizeForBlocks(2), 631);
    R_R2 = R_File("/r2", 5000, 632);
    R_N = R_File("/n", V_SizeForBlocks(2) + 10, 633);
    R_N2 = R_N;
    R_N2.Path = "/n2";
    R_Bm = R_File("/m", V_SizeForBlocks(4), 634);
    R_Bm2 = R_Mod(R_Mod(R_Bm, 3), V_FileOffset(1, 3));
    R_K = R_File("/k", 2000, 635);
    for (i = 0; i < 5; i++) {
        R_NewF[i] = R_File(Paths[i], Sizes[i], 640 + i);
    }

    V_NewVolumeBlocks(NandTest_Geo.DirRegionBlocks + R_SWEEP_BLOCKS);
    CHECK_RC(V_Write(&R_R1), EEFS_SUCCESS);
    CHECK_RC(V_Write(&R_R2), EEFS_SUCCESS);
    CHECK_RC(V_Write(&R_N), EEFS_SUCCESS);
    CHECK_RC(V_Write(&R_Bm), EEFS_SUCCESS);
    CHECK_RC(V_Write(&R_K), EEFS_SUCCESS);
    R_ImageEnd();
    CHECK(R_Sweep("batch", R_RunBatch, R_CheckBatch) > 0);
    R_ImageFree();
}

/* RepairDirectory of a rebuilt volume cut at every point: the remount gives the rebuilt state, either rebuilt again
 * or from the new checkpoint (and once from the checkpoint, never rebuilt again at a later cut) */
static void rebuild_sweep_repair_directory(void)
{
    V_File_t    Files[4];
    V_File_t    Follow = R_File("/follow", 2500, 659);
    uint32      Cut;
    uint32      Cuts = 0;
    uint32      Ops;
    uint32      i;
    int         Mode;
    int         Done;
    int         Persisted;
    int32       Status;

    Files[0] = R_File("/a", V_SizeForBlocks(2) + 3, 651);
    Files[1] = R_File("/b", 700, 652);
    Files[2] = R_File("/c@B", V_D0() - 1, 653);
    Files[3] = R_File("/d", 0, 654);
    V_NewVolumeBlocks(NandTest_Geo.DirRegionBlocks + R_SWEEP_BLOCKS);
    for (i = 0; i < 4; i++) {
        CHECK_RC(V_Write(&Files[i]), EEFS_SUCCESS);
    }
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    R_CorruptDir();                             /* damaged, not erased: the old blocks stay in the rings */
    CHECK_RC(R_MountRebuilt(), EEFS_SUCCESS);
    memcpy(&R_Expect, &V_Vol, sizeof(R_Expect));
    R_Image = V_Sim;
    V_Sim = NULL;
    memset(&V_Vol, 0, sizeof(V_Vol));

    for (Mode = NANDSIM_TORN_NONE; Mode <= NANDSIM_TORN_GARBAGE; Mode++) {
        Persisted = 0;
        for (Cut = 0; ; Cut++) {
            V_Attach(NandSim_Clone(R_Image));
            CHECK_RC(R_MountRebuilt(), EEFS_SUCCESS);
            NandSim_SetPowerBudget(V_Sim, Cut, Mode);
            Status = EEFS_LibRepairDirectory(&V_Vol);
            Done = !NandSim_PowerCutHappened(V_Sim);
            if (Done) CHECK_RC(Status, EEFS_SUCCESS);
            NandSim_Reboot(V_Sim);
            EEFS_LibInit();

            Ops = V_Ops();
            Status = V_Mount(&V_Vol);
            CHECK_RC(Status, EEFS_SUCCESS);
            CHECK_EQ(V_Ops(), Ops);
            if (Status == EEFS_SUCCESS) {
                CHECK(R_SameByPath(&R_Expect, &V_Vol));
                if (!V_Vol.DirectoryRebuilt) Persisted = 1;
                else CHECK(!Persisted);
                if (Done) CHECK_EQ(V_Vol.DirectoryRebuilt, 0);
                for (i = 0; i < 4; i++) {
                    CHECK(R_FileIs(&Files[i]));
                }
                CHECK_RC(V_Write(&Follow), EEFS_SUCCESS);
                CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
                CHECK_EQ(V_Vol.DirectoryRebuilt, 0);
                V_CheckMirrorsAgree(&V_Vol);
                V_Remount();
                CHECK(R_FileIs(&Follow));
                R_VerifyAll();
            }
            CHECK_EQ(NandSim_Violations(V_Sim), 0);
            if (NandTest_CaseFailed()) {
                printf("    sweep repair: failure at cut %lu, torn mode %s\n", (unsigned long)Cut, R_ModeName[Mode]);
                V_Destroy();
                R_ImageFree();
                return;
            }
            V_Destroy();
            if (Done) {
                Cuts = Cut;
                break;
            }
        }
    }
    printf("    sweep repair_directory: %lu cut points x 3 torn modes\n", (unsigned long)Cuts);
    CHECK(Cuts > 0);
    R_ImageFree();
}

/*
 * Case table
 */

const NandTest_Case_t NandTest_RebuildCases[] = {
    { "rebuild_after_both_mirrors_erased_matches_committed_set",    rebuild_after_both_mirrors_erased_matches_committed_set },
    { "rebuild_highest_writeseq_wins",                              rebuild_highest_writeseq_wins },
    { "rebuild_overlap_rule_after_cut_rename",                      rebuild_overlap_rule_after_cut_rename },
    { "rebuild_includes_uncommitted_and_uncleaned_copies",          rebuild_includes_uncommitted_and_uncleaned_copies },
    { "rebuild_not_formatted_when_no_page0",                        rebuild_not_formatted_when_no_page0 },
    { "rebuild_bad_blocks_from_oob",                                rebuild_bad_blocks_from_oob },
    { "rebuild_crosslink_flagged",                                  rebuild_crosslink_flagged },
    { "repair_directory_persists_and_clears_flag",                  repair_directory_persists_and_clears_flag },
    { "rebuild_directory_api_requires_idle",                        rebuild_directory_api_requires_idle },
    { "rebuild_ab_defaults",                                        rebuild_ab_defaults },
    { "rebuild_max_files_overflow",                                 rebuild_max_files_overflow },
    { "rebuild_sweep_cut_rename",                                   rebuild_sweep_cut_rename },
    { "rebuild_sweep_cut_modify",                                   rebuild_sweep_cut_modify },
    { "rebuild_sweep_cut_new_write",                                rebuild_sweep_cut_new_write },
    { "rebuild_sweep_batch_then_rebuild",                           rebuild_sweep_batch_then_rebuild },
    { "rebuild_sweep_repair_directory",                             rebuild_sweep_repair_directory },
    { NULL, NULL }
};

/************************/
/*  End of File Comment */
/************************/
