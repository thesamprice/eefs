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
 * Filename: test_health.c
 *
 * Purpose: Suite "health": the resumable health check and EEFS_LibReclaimOrphans (docs/nand-implementation.md
 *   sections 10 and 8.4, phase 6).
 *
 * Design Notes:
 *   Every case builds its fault deliberately and compares the complete finding list with the expected one (code,
 *   slot, entry, block, Aux where defined, path), so a missing finding and an extra one both fail.  Every check run
 *   (H_Check) also proves the protocol: each step reads at most MaxPageReads pages, PagesRead equals the HAL reads, the
 *   check never programs or erases, the volume's summary equals the report, and the summary bits follow from the
 *   finding codes.
 *
 *   The step-size case runs the same volume RAM image (memcpy of the volume; the check never writes NAND) with
 *   MaxPageReads 1, 13 and unlimited and requires identical reports apart from Steps.  The power-cut sweep runs the
 *   batch of test_powercut.c (5 new files, 2 removes, a rename and a modify, one commit) cut at every program and
 *   erase in every torn mode, then checks the report after the remount against expectations derived from the raw
 *   media (orphans, cleanup still pending, unrecorded bad markers left by torn erases, lagging or torn mirrors) and
 *   nothing else; after ReclaimOrphans and a follow-up commit only the bad markers of torn erases may remain.
 *
 */

/*
 * Includes
 */

#include <stdlib.h>
#include "nand_vol.h"
#include "microeefs_nand.h"

/*
 * Macro Definitions
 */

#define H_ANY               0xFFFFFFFFUL
#define H_MAX_EXP           64
#define H_DATA_BLOCKS       112             /* data blocks of the small test devices */
#define H_MAX_BLOCKS        512

/*
 * Type Definitions
 */

/* An expected finding; H_ANY (or a NULL Path) matches anything */
typedef struct
{
    uint32      Code, Slot, Entry, Block, Aux0, Aux1;
    int         AnyPath;
    char        Path[EEFS_NAND_HEALTH_PATH_SIZE];
} H_Exp_t;

typedef int32 (*H_Run_t)(void);

/*
 * Local Data
 */

static EEFS_HealthReport_t  H_Rep;
static EEFS_HealthSummary_t H_Sum;
static EEFS_HealthSummary_t H_Sum2;
static EEFS_InodeTable_t    H_Saved;
static H_Exp_t              H_Exp[H_MAX_EXP];
static uint32               H_ExpCount;
static uint8                H_PageBuf[EEFS_NAND_MAX_PAGE_SIZE];
static uint8                H_Oob[EEFS_NAND_MAX_OOB_SIZE];
static uint16               H_Blk[H_MAX_BLOCKS];
static uint16               H_Blk2[H_MAX_BLOCKS];
static uint32               H_Crc[H_MAX_BLOCKS];
static NandSim_t           *H_Image;
static V_Snap_t             H_SnapPre;
static V_Snap_t             H_SnapPost;
static V_Snap_t             H_SnapCur;

static const char          *H_ModeName[3] = { "NONE", "PARTIAL", "GARBAGE" };

/*
 * Generic helpers
 */

static V_File_t H_File(const char *Path, uint32 Size, uint32 Seed)
{
    V_File_t    F;

    memset(&F, 0, sizeof(F));
    F.Path = Path;
    F.Size = Size;
    F.Seed = Seed;
    return(F);
}

static V_File_t H_Mod(V_File_t F, uint32 Offset)
{
    if (F.ModCount < V_MAX_MODS) {
        F.Mod[F.ModCount++] = Offset;
    }
    return(F);
}

static void H_NewVolume(void)
{
    V_NewVolumeBlocks(NandTest_Geo.DirRegionBlocks + H_DATA_BLOCKS);
}

static uint32 H_Markers(void)
{
    uint32  m = NandTest_Geo.BadMarkerPages;

    return(((m & 1) != 0) + ((m & 2) != 0) + ((m & 4) != 0));
}

static EEFS_NandEntry_t *H_E(const char *Path)
{
    static EEFS_NandEntry_t None;
    int32                   i = EEFS_NandFindEntry(&V_Vol, Path);

    if (i < 0) {
        memset(&None, 0, sizeof(None));
        return(&None);
    }
    return(&V_Vol.Working.Entry[i]);
}

static uint32 H_Idx(const char *Path)
{
    int32   i = EEFS_NandFindEntry(&V_Vol, Path);

    CHECK(i >= 0);
    return((i < 0) ? 0 : (uint32)i);
}

static uint32 H_First(const char *Path, uint8 Slot)
{
    return(H_E(Path)->Slot[Slot].FirstBlock);
}

/* Blocks[] of a slot, read raw from its metadata page; returns the count */
static uint32 H_Blocks(const char *Path, uint8 Slot, uint16 *Blocks)
{
    const EEFS_NandEntry_t *E = H_E(Path);

    if (E->Slot[Slot].InUse == 0) return(0);
    return(V_RawBlocks(E->Slot[Slot].FirstBlock, Blocks, H_Crc));
}

/* WriteSeq of the metadata page at Block (raw) */
static uint32 H_PageSeq(uint32 Block)
{
    EEFS_NandPage0_t    P;

    NandSim_RawRead(V_Sim, Block, 0, H_PageBuf, NULL);
    if (EEFS_NandDecodePage0(H_PageBuf, NandTest_Geo.PageSize, NandTest_Geo.PagesPerBlock, Block, &P) != EEFS_SUCCESS) {
        return(0);
    }
    return(P.WriteSeq);
}

/* A data block that is free in the map, counted from the top of the device */
static uint32 H_FreeBlock(uint32 Nth)
{
    uint32  b;

    for (b = V_Dev.Geometry.BlockCount; b > V_Dev.Geometry.DirRegionBlocks; b--) {
        if (V_Vol.Working.Map[b - 1] == EEFS_NAND_MAP_FREE) {
            if (Nth == 0) return(b - 1);
            Nth--;
        }
    }
    CHECK(0);
    return(V_Dev.Geometry.DirRegionBlocks);
}

/* Reboot and remount (the RAM state of the session is gone) */
static void H_Reboot(void)
{
    NandSim_Reboot(V_Sim);
    EEFS_LibInit();
    V_Remount();
}

/* The same, but the allocator continues where it was (the mount restarts it after the highest owned block, which
 * would hand out the blocks of the uncommitted copies first and erase the orphans the case wants to keep) */
static void H_RebootKeepCursor(void)
{
    uint32  Cursor = V_Vol.AllocCursor;

    H_Reboot();
    V_Vol.AllocCursor = Cursor;
}

/*
 * A/B helpers (from test_ab.c)
 */

static uint8        A_PageBuf[EEFS_NAND_MAX_PAGE_SIZE];
static V_File_t     A_F1, A_F2, A_F1a, A_F2b;

static V_File_t A_As(V_File_t F, const char *Name)
{
    F.Path = Name;
    return(F);
}

/* WriteFile of F's content under Name with Meta.Slot = MetaSlot and the version */
static int32 A_Put(const char *Name, const V_File_t *F, uint8 MetaSlot, uint32 Version)
{
    EEFS_NandFileInfo_t Meta;

    V_Meta(&Meta, EEFS_FILE_TYPE_OTHER, Version);
    Meta.Slot = MetaSlot;
    Meta.Attributes = F->Attributes;
    return(EEFS_LibWriteFile(&V_Vol, (char *)Name, &Meta, V_Content(F), F->Size));
}

/* "/f": A v1 (two blocks) and B v2 (one block), B active, FallbackCompatible as given, committed */
static void A_MakeF(uint8 Compat)
{
    A_F1 = H_File("/f", V_D0() + NandTest_Geo.PageSize + 33, 701);
    A_F2 = H_File("/f", 5 * NandTest_Geo.PageSize + 9, 702);
    A_F1a = A_As(A_F1, "/f@A");
    A_F2b = A_As(A_F2, "/f@B");
    CHECK_RC(A_Put("/f", &A_F1, EEFS_NAND_SLOT_A, 1), EEFS_SUCCESS);
    CHECK_RC(A_Put("/f", &A_F2, EEFS_NAND_SLOT_B, 2), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibSetActiveSlot(&V_Vol, "/f", EEFS_NAND_SLOT_B), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibSetFallbackCompatible(&V_Vol, "/f", Compat), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
}

/* An A/B file with A and B of the same version, compatible, the given slot active (a clean A/B entry) */
static void A_MakeClean(const char *Path, uint32 Seed, uint8 Active)
{
    V_File_t    F = H_File(Path, 3 * NandTest_Geo.PageSize + 11, Seed);

    CHECK_RC(A_Put(Path, &F, EEFS_NAND_SLOT_A, 5), EEFS_SUCCESS);
    CHECK_RC(A_Put(Path, &F, EEFS_NAND_SLOT_B, 5), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibSetActiveSlot(&V_Vol, (char *)Path, Active), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibSetFallbackCompatible(&V_Vol, (char *)Path, 1), EEFS_SUCCESS);
}

static void A_CorruptPage0(const char *Path, uint8 Slot)
{
    NandSim_CorruptBytes(V_Sim, H_First(Path, Slot), 0, 20, 4);       /* inside Path: header CRC fails */
}

static void A_CorruptData(const char *Path, uint8 Slot)
{
    NandSim_CorruptBytes(V_Sim, H_First(Path, Slot), 1, 3, 1);        /* first data page */
}

static void A_InjectEcc(const char *Path, uint8 Slot, uint32 Page)
{
    NandSim_InjectRead(V_Sim, H_First(Path, Slot), Page, EEFS_NAND_READ_UNCORRECTABLE, -1);
}

/* The bootloader library loads Name: expected return code, slot, content and ActiveFailed; it never writes */
static void A_CheckBoot(const char *Name, int32 Rc, uint8 Slot, const V_File_t *F, uint8 Failed)
{
    MicroEEFS_NandInfo_t    Info;
    uint32                  Programs = NandSim_Programs(V_Sim);
    uint32                  Erases = NandSim_Erases(V_Sim);
    int32                   Status;

    memset(&Info, 0, sizeof(Info));
    Status = MicroEEFS_NandLoadFile(&V_Dev, A_PageBuf, Name, V_Read, V_BUF_SIZE, &Info);
    CHECK_RC(Status, Rc);
    if ((Status == EEFS_SUCCESS) && (Rc == EEFS_SUCCESS)) {
        CHECK_EQ(Info.LoadedSlot, Slot);
        CHECK_EQ(Info.Info.FileSize, F->Size);
        CHECK(memcmp(V_Read, V_Content(F), F->Size) == 0);
    }
    if (Status != EEFS_FILE_NOT_FOUND) {
        CHECK_EQ(Info.Info.ActiveFailed, Failed);
    }
    CHECK_EQ(NandSim_Programs(V_Sim), Programs);
    CHECK_EQ(NandSim_Erases(V_Sim), Erases);
}

/*
 * Running a check and matching its findings
 */

static void H_Print(const EEFS_HealthSummary_t *S)
{
    uint32  i;

    printf("    report: summary 0x%lx, %lu finding(s), overflow %lu, files %lu, slots %lu, pages %lu, restarts %lu\n",
           (unsigned long)S->Summary, (unsigned long)S->FindingCount, (unsigned long)S->Overflow,
           (unsigned long)S->FilesChecked, (unsigned long)S->SlotsChecked, (unsigned long)S->PagesRead,
           (unsigned long)S->Restarts);
    for (i = 0; i < S->FindingCount; i++) {
        printf("      code %u slot 0x%x entry 0x%x block %u aux0 0x%lx aux1 0x%lx path '%s'\n",
               (unsigned)S->Finding[i].Code, (unsigned)S->Finding[i].Slot, (unsigned)S->Finding[i].EntryIndex,
               (unsigned)S->Finding[i].Block, (unsigned long)S->Finding[i].Aux0, (unsigned long)S->Finding[i].Aux1,
               S->Finding[i].Path);
    }
}

/* The summary bits implied by the finding codes (section 10.1) */
static uint32 H_Bits(const EEFS_HealthSummary_t *S)
{
    uint32  Bits = 0;
    uint32  i;
    uint32  c;

    for (i = 0; i < S->FindingCount; i++) {
        c = S->Finding[i].Code;
        if ((c >= 10) && (c <= 12)) Bits |= EEFS_HEALTH_FILE_ISSUE | EEFS_HEALTH_DEGRADED;
        else if (((c >= 20) && (c <= 23)) || (c == 26)) Bits |= EEFS_HEALTH_AB_ISSUE | EEFS_HEALTH_DEGRADED;
        else if ((c == 24) || (c == 25) || (c == 27)) Bits |= EEFS_HEALTH_INFO;
        else if (c == 40) {
            Bits |= EEFS_HEALTH_SPACE_ISSUE |
                    ((S->Finding[i].Aux0 < EEFS_NAND_BADBLOCK_RESERVE) ? EEFS_HEALTH_DEGRADED : EEFS_HEALTH_INFO);
        }
        else Bits |= EEFS_HEALTH_DIR_ISSUE | EEFS_HEALTH_DEGRADED;
    }
    if (S->Overflow > 0) Bits |= EEFS_HEALTH_OVERFLOW;
    return(Bits);
}

/* Runs a whole check of V_Vol in steps of Max page reads and returns its report (H_Sum).  Checks the protocol (see
 * the file header).  MaxSteps limits the run (0 = none). */
static const EEFS_HealthSummary_t *H_Check(uint32 Max)
{
    uint32  Programs = NandSim_Programs(V_Sim);
    uint32  Erases = NandSim_Erases(V_Sim);
    uint32  Reads = NandSim_Reads(V_Sim);
    uint32  Before;
    uint32  Steps = 0;
    uint32  Limit = (Max == 0) ? 1 : Max;
    int32   Status;

    memset(&H_Sum, 0, sizeof(H_Sum));
    CHECK_RC(EEFS_LibHealthCheckStart(&V_Vol, &H_Rep), EEFS_SUCCESS);
    for (;;) {
        Before = NandSim_Reads(V_Sim);
        Status = EEFS_LibHealthCheckStep(&V_Vol, Max);
        Steps++;
        CHECK(NandSim_Reads(V_Sim) - Before <= Limit);
        if (Status != 1) break;
        CHECK(H_Rep.S.Summary & EEFS_HEALTH_INCOMPLETE);
        if (Steps > 50000000UL) {
            CHECK(0);
            break;
        }
    }
    CHECK_RC(Status, EEFS_SUCCESS);
    CHECK(V_Vol.ActiveReport == NULL);
    CHECK_EQ(H_Rep.S.Steps, Steps);
    CHECK_EQ(H_Rep.S.Complete, 1);
    CHECK_EQ(H_Rep.S.Restarts, 0);
    CHECK_EQ(H_Rep.S.PagesRead, NandSim_Reads(V_Sim) - Reads);
    CHECK_EQ(NandSim_Programs(V_Sim), Programs);
    CHECK_EQ(NandSim_Erases(V_Sim), Erases);
    CHECK_RC(EEFS_LibHealthCheckSummary(&V_Vol, &H_Sum), EEFS_SUCCESS);
    CHECK(memcmp(&H_Sum, &H_Rep.S, sizeof(H_Sum)) == 0);
    CHECK_EQ(H_Sum.DirSeq, V_Vol.LastSeq);
    CHECK_EQ(H_Sum.Summary, H_Bits(&H_Sum));
    CHECK(H_Sum.FindingCount <= EEFS_NAND_MAX_HEALTH_FINDINGS);
    return(&H_Sum);
}

static void H_ExpReset(void)
{
    H_ExpCount = 0;
}

static void H_Expect(uint32 Code, uint32 Slot, uint32 Entry, uint32 Block, uint32 Aux0, uint32 Aux1, const char *Path)
{
    H_Exp_t    *E;

    if (H_ExpCount >= H_MAX_EXP) {
        CHECK(0);
        return;
    }
    E = &H_Exp[H_ExpCount++];
    E->Code = Code;
    E->Slot = Slot;
    E->Entry = Entry;
    E->Block = Block;
    E->Aux0 = Aux0;
    E->Aux1 = Aux1;
    E->AnyPath = (Path == NULL);
    memset(E->Path, 0, sizeof(E->Path));
    if (Path != NULL) strncpy(E->Path, Path, sizeof(E->Path) - 1);     /* as the finding truncates it */
}

static int H_Match(const H_Exp_t *E, const EEFS_HealthFinding_t *F)
{
    if (E->Code != F->Code) return(0);
    if ((E->Slot != H_ANY) && (E->Slot != F->Slot)) return(0);
    if ((E->Entry != H_ANY) && (E->Entry != F->EntryIndex)) return(0);
    if ((E->Block != H_ANY) && (E->Block != F->Block)) return(0);
    if ((E->Aux0 != H_ANY) && (E->Aux0 != F->Aux0)) return(0);
    if ((E->Aux1 != H_ANY) && (E->Aux1 != F->Aux1)) return(0);
    if (!E->AnyPath && (strcmp(E->Path, F->Path) != 0)) return(0);
    return(1);
}

/* The report holds exactly the expected findings (one to one, in any order) */
static void H_ExpectExactly(const EEFS_HealthSummary_t *S)
{
    uint8   Used[EEFS_NAND_MAX_HEALTH_FINDINGS];
    uint32  e;
    uint32  f;
    int     Ok = (S->FindingCount == H_ExpCount) && (S->Overflow == 0);

    memset(Used, 0, sizeof(Used));
    for (e = 0; Ok && (e < H_ExpCount); e++) {
        for (f = 0; f < S->FindingCount; f++) {
            if (!Used[f] && H_Match(&H_Exp[e], &S->Finding[f])) break;
        }
        if (f >= S->FindingCount) {
            printf("    expected finding %lu (code %lu block %lu) not found\n", (unsigned long)e,
                   (unsigned long)H_Exp[e].Code, (unsigned long)H_Exp[e].Block);
            Ok = 0;
            break;
        }
        Used[f] = 1;
    }
    if (!Ok) {
        printf("    expected %lu finding(s)\n", (unsigned long)H_ExpCount);
        H_Print(S);
    }
    CHECK(Ok);
}

/* Count of findings with Code */
static uint32 H_Count(const EEFS_HealthSummary_t *S, uint32 Code)
{
    uint32  i;
    uint32  n = 0;

    for (i = 0; i < S->FindingCount; i++) {
        if (S->Finding[i].Code == Code) n++;
    }
    return(n);
}

/* The two reports are identical apart from Steps (and Restarts when IgnoreRestarts) */
static int H_SameReport(const EEFS_HealthSummary_t *A, const EEFS_HealthSummary_t *B, int IgnoreRestarts)
{
    EEFS_HealthSummary_t    X;
    EEFS_HealthSummary_t    Y;

    memcpy(&X, A, sizeof(X));
    memcpy(&Y, B, sizeof(Y));
    X.Steps = Y.Steps = 0;
    if (IgnoreRestarts) X.Restarts = Y.Restarts = 0;
    return(memcmp(&X, &Y, sizeof(X)) == 0);
}

/* The page reads a check of the current volume must take (section 10.3, as implemented): per directory block not
 * recorded bad, its marker pages and page 0; per mirror, the chain pages plus the page after it; the cleanup pages
 * from the media; per slot, page 0, page 0 again per further block, and every data page; per data block not recorded
 * bad, its marker pages and, unless a slot claims it or it is on the cleanup list, page 0.  Assumes no torn or extra
 * checkpoint candidates. */
static uint32 H_ExpectedReads(void)
{
    const EEFS_NandGeometry_t  *Geo = &V_Dev.Geometry;
    static uint8                Claimed[EEFS_NAND_MAX_BLOCKS];
    uint32                      Reads = 0;
    uint32                      Markers = H_Markers();
    uint32                      b;
    uint32                      i;
    uint32                      s;
    uint32                      k;
    uint32                      n;
    uint32                      Len;
    const EEFS_NandEntry_t     *E;
    EEFS_NandPage0_t            P;

    memset(Claimed, 0, sizeof(Claimed));
    for (b = 0; b < Geo->DirRegionBlocks; b++) {
        if (V_Vol.Working.Map[b] != EEFS_NAND_MAP_BAD) Reads += Markers + 1;
    }
    for (i = 0; i < 2; i++) {
        Reads += V_Vol.Mirror[i].NextPage + ((V_Vol.Mirror[i].NextPage < Geo->PagesPerBlock) ? 1 : 0);
    }
    Reads += V_Vol.PendingCleanupBase;
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        E = &V_Vol.Working.Entry[i];
        if (E->InUse == 0) continue;
        for (s = 0; s < 2; s++) {
            if (E->Slot[s].InUse == 0) continue;
            n = V_RawBlocks(E->Slot[s].FirstBlock, H_Blk, H_Crc);
            NandSim_RawRead(V_Sim, E->Slot[s].FirstBlock, 0, H_PageBuf, NULL);
            CHECK_RC(EEFS_NandDecodePage0(H_PageBuf, Geo->PageSize, Geo->PagesPerBlock, E->Slot[s].FirstBlock, &P),
                     EEFS_SUCCESS);
            Reads += n;     /* page 0, and again for each further block */
            for (k = 0; k < n; k++) {
                Claimed[H_Blk[k]] = 1;
                Len = EEFS_NandBlockDataRange(Geo->PageSize, Geo->PagesPerBlock, P.FileSize, k, NULL);
                Reads += (Len + Geo->PageSize - 1) / Geo->PageSize;
            }
        }
    }
    for (b = Geo->DirRegionBlocks; b < Geo->BlockCount; b++) {
        if (V_Vol.Working.Map[b] == EEFS_NAND_MAP_BAD) continue;
        Reads += Markers;
        if (Claimed[b]) continue;
        for (i = 0; i < V_Vol.PendingCleanupCount; i++) {
            if (V_Vol.PendingCleanup[i] == b) break;
        }
        if (i >= V_Vol.PendingCleanupCount) Reads++;
    }
    return(Reads);
}

/* Cuts the next commit right after its directory records, before the first cleanup erase (TORN_NONE): the count of
 * operations is taken from the same commit on a clone of the device and of the volume RAM.  Then reboot + remount. */
static void H_CommitCutCleanup(uint32 CleanupErases)
{
    NandSim_t  *Clone = NandSim_Clone(V_Sim);
    uint32      Ops;

    memcpy(&H_Saved, &V_Vol, sizeof(H_Saved));
    NandSim_FillDevice(Clone, &V_Dev);
    Ops = NandSim_Programs(Clone) + NandSim_Erases(Clone);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    Ops = NandSim_Programs(Clone) + NandSim_Erases(Clone) - Ops;
    NandSim_Destroy(Clone);
    NandSim_FillDevice(V_Sim, &V_Dev);
    memcpy(&V_Vol, &H_Saved, sizeof(V_Vol));

    CHECK(Ops > CleanupErases);
    NandSim_SetPowerBudget(V_Sim, Ops - CleanupErases, NANDSIM_TORN_NONE);
    (void)EEFS_LibCommit(&V_Vol);
    CHECK(NandSim_PowerCutHappened(V_Sim));
    H_Reboot();
    CHECK(V_Vol.PendingCleanupBase > 0);
}

/* A record page header (payload empty) written raw at (Block, Page) */
static void H_RawRecord(uint32 Block, uint32 Page, uint8 RecType, uint32 Seq, uint16 PageIndex, uint16 PageCount)
{
    EEFS_NandRecHdr_t   H;

    memset(&H, 0, sizeof(H));
    H.RecType = RecType;
    H.Seq = Seq;
    H.PageIndex = PageIndex;
    H.PageCount = PageCount;
    EEFS_NandBeginRecordPage(H_PageBuf, NandTest_Geo.PageSize, &H);
    EEFS_NandSealRecordPage(H_PageBuf, NandTest_Geo.PageSize, 0);
    NandSim_RawWrite(V_Sim, Block, Page, H_PageBuf, NULL);
}

/* Current ring block of mirror M (from the volume RAM) */
static uint32 H_MirrorBlock(uint8 M)
{
    return(V_Vol.Mirror[M].Blocks[V_Vol.Mirror[M].CurIndex]);
}

/*
 * Clean volume
 */

static V_File_t H_CA, H_CB;

static void H_InitClean(void)
{
    H_CA = H_File("/a", V_SizeForBlocks(3) - 100, 11);
    H_CB = H_File("/b", 1000, 12);
}

static void H_MakeClean(void)
{
    H_InitClean();
    CHECK_RC(V_Write(&H_CA), EEFS_SUCCESS);
    CHECK_RC(V_Write(&H_CB), EEFS_SUCCESS);
    A_MakeClean("/c", 13, EEFS_NAND_SLOT_A);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
}

static void health_clean_volume_ok(void)
{
    const EEFS_HealthSummary_t *S;
    uint32                      Expected;

    V_NewVolume();          /* the full-size device of the run */
    H_MakeClean();
    Expected = H_ExpectedReads();
    S = H_Check(0xFFFFFFFFUL);
    CHECK_EQ(S->Summary, EEFS_HEALTH_OK);
    CHECK_EQ(S->FindingCount, 0);
    CHECK_EQ(S->Overflow, 0);
    CHECK_EQ(S->FilesChecked, 3);
    CHECK_EQ(S->SlotsChecked, 4);
    CHECK_EQ(S->Steps, 1);
    CHECK_EQ(S->CorrectedBits, 0);
    CHECK_EQ(S->PagesRead, Expected);
    if (S->FindingCount != 0) H_Print(S);

    /* small steps: the same report */
    memcpy(&H_Sum2, S, sizeof(H_Sum2));
    S = H_Check(64);
    CHECK(H_SameReport(S, &H_Sum2, 0));
    CHECK(S->Steps > 1);
    V_Destroy();

    /* the small device too, and after a remount */
    H_NewVolume();
    H_MakeClean();
    V_Remount();
    Expected = H_ExpectedReads();
    S = H_Check(1);
    CHECK_EQ(S->Summary, EEFS_HEALTH_OK);
    CHECK_EQ(S->FindingCount, 0);
    CHECK_EQ(S->PagesRead, Expected);
    CHECK_EQ(S->Steps, Expected + 1);       /* one read per step, then the step that finds nothing more to read */
    V_Destroy();
}

/*
 * Directory findings
 */

static void health_dir_mirror_missing(void)
{
    const EEFS_HealthSummary_t *S;
    uint32                      Half = NandTest_Geo.DirRegionBlocks / 2;
    uint32                      b;
    uint8                       m;

    for (m = 0; m < 2; m++) {
        H_NewVolume();
        H_MakeClean();
        for (b = m * Half; b < (m + 1) * Half; b++) {
            NandSim_RawErase(V_Sim, b);
        }
        V_Remount();
        S = H_Check(5);
        H_ExpReset();
        H_Expect(EEFS_HEALTH_DIR_MIRROR_MISSING, m, 0xFFFF, 0xFFFF, H_ANY, H_ANY, "");
        H_ExpectExactly(S);
        CHECK_EQ(S->Summary, EEFS_HEALTH_DEGRADED | EEFS_HEALTH_DIR_ISSUE);

        /* the next commit resyncs the mirror */
        CHECK_RC(EEFS_LibRepairDirectory(&V_Vol), EEFS_SUCCESS);
        S = H_Check(5);
        CHECK_EQ(S->FindingCount, 0);
        V_Destroy();
    }
}

static void health_dir_mirror_mismatch(void)
{
    const EEFS_HealthSummary_t *S;
    V_File_t                    D = H_File("/d", 777, 21);
    uint32                      Seq;

    H_NewVolume();
    H_MakeClean();
    CHECK_RC(V_Write(&D), EEFS_SUCCESS);
    V_Vol.Mirror[1].Disabled = TRUE;            /* this commit reaches mirror 0 only */
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    Seq = V_Vol.LastSeq;
    V_Remount();
    CHECK_EQ(V_Vol.LastSeq, Seq);
    S = H_Check(3);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_DIR_MIRROR_MISMATCH, 1, 0xFFFF, 0xFFFF, Seq - 1, Seq, "");
    H_ExpectExactly(S);
    CHECK_EQ(S->FilesChecked, 4);
    V_Destroy();
}

static void health_dir_record_corrupt(void)
{
    const EEFS_HealthSummary_t *S;
    uint32                      Block;
    uint32                      Page;

    H_NewVolume();
    H_MakeClean();
    Block = H_MirrorBlock(0);
    Page = V_Vol.Mirror[0].NextPage;
    memset(H_PageBuf, 0x00, sizeof(H_PageBuf));
    NandSim_RawWrite(V_Sim, Block, Page, H_PageBuf, NULL);       /* junk after mirror 0's chain */
    V_Remount();
    S = H_Check(2);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_DIR_RECORD_CORRUPT, 0, 0xFFFF, Block, Page, 0, "");
    H_ExpectExactly(S);

    /* the next commit starts a new block: the junk is no longer after the chain */
    CHECK_RC(V_Write(&H_CB), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    S = H_Check(2);
    CHECK_EQ(S->FindingCount, 0);
    V_Destroy();
}

static void health_dir_uncommitted_txn(void)
{
    const EEFS_HealthSummary_t *S;
    static char                 Path[32][16];
    V_File_t                    F;
    uint32                      Block;
    uint32                      Page;
    uint32                      Seq;
    uint32                      Free;
    uint32                      Files;
    uint32                      i;

    /* a real torn transaction: a commit of more entries than one record page holds, cut after its first page */
    H_NewVolume();
    H_MakeClean();
    Block = H_MirrorBlock(0);
    Page = V_Vol.Mirror[0].NextPage;
    Seq = V_Vol.LastSeq;
    Files = (NandTest_Geo.PageSize - EEFS_NAND_REC_HDR_SIZE) / (EEFS_NAND_ENTRY_REC_SIZE + 8) + 1;
    for (i = 0; i < Files; i++) {
        sprintf(Path[i], "/t%02lu", (unsigned long)i);
        F = H_File(Path[i], 0, 200 + i);
        CHECK_RC(V_Write(&F), EEFS_SUCCESS);
    }
    NandSim_SetPowerBudget(V_Sim, 1, NANDSIM_TORN_NONE);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_IO_ERROR);
    H_Reboot();
    CHECK_EQ(V_Vol.LastSeq, Seq);
    CHECK(NandSim_PageIsProgrammed(V_Sim, Block, Page));
    S = H_Check(4);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_DIR_UNCOMMITTED_TXN, 0, 0xFFFF, Block, Page, Seq + 1, "");
    for (i = 0; i < Files; i++) {      /* the files were never committed */
        H_Expect(EEFS_HEALTH_ORPHAN_FILE, EEFS_NAND_SLOT_NONE, 0xFFFF, H_ANY, H_ANY, 0, Path[i]);
    }
    H_ExpectExactly(S);
    V_Destroy();

    /* a one-page record whose header survived but not its CRC (readable header: uncommitted); a two-page TXN whose
     * second page never came; and a torn checkpoint above the chain in mirror 1 */
    H_NewVolume();
    H_MakeClean();
    Block = H_MirrorBlock(0);
    Page = V_Vol.Mirror[0].NextPage;
    Seq = V_Vol.LastSeq;
    H_RawRecord(H_MirrorBlock(1), V_Vol.Mirror[1].NextPage, EEFS_NAND_REC_TXN, Seq + 1, 0, 1);
    NandSim_CorruptBytes(V_Sim, H_MirrorBlock(1), V_Vol.Mirror[1].NextPage, NandTest_Geo.PageSize - 9, 1);
    H_RawRecord(Block, Page, EEFS_NAND_REC_TXN, Seq + 1, 0, 2);
    Free = V_Vol.Mirror[1].Blocks[(V_Vol.Mirror[1].CurIndex + 3) % V_Vol.Mirror[1].Count];
    H_RawRecord(Free, 0, EEFS_NAND_REC_CHECKPOINT, Seq + 7, 0, 2);
    i = V_Vol.Mirror[1].NextPage;
    Files = H_MirrorBlock(1);
    V_Remount();
    S = H_Check(1);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_DIR_UNCOMMITTED_TXN, 0, 0xFFFF, Block, Page, Seq + 1, "");
    H_Expect(EEFS_HEALTH_DIR_UNCOMMITTED_TXN, 1, 0xFFFF, Files, i, Seq + 1, "");
    H_Expect(EEFS_HEALTH_DIR_UNCOMMITTED_TXN, 1, 0xFFFF, Free, 0, Seq + 7, "");
    H_ExpectExactly(S);

    /* the same page with a damaged header is just corrupt */
    NandSim_CorruptBytes(V_Sim, Files, i, 0, 1);
    S = H_Check(1);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_DIR_UNCOMMITTED_TXN, 0, 0xFFFF, Block, Page, Seq + 1, "");
    H_Expect(EEFS_HEALTH_DIR_RECORD_CORRUPT, 1, 0xFFFF, Files, i, 0, "");
    H_Expect(EEFS_HEALTH_DIR_UNCOMMITTED_TXN, 1, 0xFFFF, Free, 0, Seq + 7, "");
    H_ExpectExactly(S);
    V_Destroy();
}

/* Superseded first blocks: a copy-on-write modify, a rename and a CopySlot that replaces a copy, committed with the
 * cleanup erase cut.  The old metadata pages list blocks the new copies still use (modify, rename) or that are free
 * now (CopySlot): DIR_CLEANUP_PENDING only, never an orphan or a cross-link.  An open batch gives no false finding. */
static void health_cow_superseded_page0_not_orphan_or_crosslink(void)
{
    const EEFS_HealthSummary_t *S;
    V_File_t                    M = H_File("/m", V_SizeForBlocks(4), 31);
    V_File_t                    M2 = H_Mod(M, V_FileOffset(2, 100));
    V_File_t                    N = H_File("/n", V_SizeForBlocks(2) + 5, 32);
    V_File_t                    New1 = H_File("/new1", 4000, 33);
    uint32                      OldM;
    uint32                      OldN;
    uint32                      OldA;

    H_NewVolume();
    CHECK_RC(V_Write(&M), EEFS_SUCCESS);
    CHECK_RC(V_Write(&N), EEFS_SUCCESS);
    A_MakeF(1);
    OldM = H_First("/m", 0);
    OldN = H_First("/n", 0);
    OldA = H_First("/f", 0);

    /* the batch, before its commit */
    CHECK_RC(V_Write(&M2), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibRename(&V_Vol, "/n", "/n2"), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/f", EEFS_NAND_SLOT_B, EEFS_NAND_SLOT_A), EEFS_SUCCESS);
    CHECK_RC(V_Write(&New1), EEFS_SUCCESS);
    S = H_Check(7);
    H_ExpReset();
    H_ExpectExactly(S);                 /* nothing: Working is consistent, the old copies are on the batch's list */
    CHECK_EQ(S->FilesChecked, 4);

    /* committed, the cleanup cut */
    H_CommitCutCleanup(3);
    S = H_Check(7);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_DIR_CLEANUP_PENDING, 0xFF, 0xFFFF, OldM, 0, 0, "");
    H_Expect(EEFS_HEALTH_DIR_CLEANUP_PENDING, 0xFF, 0xFFFF, OldN, 0, 0, "");
    H_Expect(EEFS_HEALTH_DIR_CLEANUP_PENDING, 0xFF, 0xFFFF, OldA, 0, 0, "");
    H_ExpectExactly(S);
    CHECK(!NandSim_PageIsErased(V_Sim, OldA, 0));

    /* the pending cleanup run: a clean report */
    CHECK_RC(EEFS_NandRunPendingCleanup(&V_Vol), EEFS_SUCCESS);
    S = H_Check(7);
    H_ExpReset();
    H_ExpectExactly(S);
    CHECK_EQ(S->Summary, EEFS_HEALTH_OK);
    V_Destroy();
}

static void health_dir_cleanup_pending(void)
{
    const EEFS_HealthSummary_t *S;
    uint32                      Old;

    /* a removed file whose cleanup erase was cut */
    H_NewVolume();
    H_MakeClean();
    Old = H_First("/a", 0);
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/a"), EEFS_SUCCESS);
    H_CommitCutCleanup(1);
    S = H_Check(1);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_DIR_CLEANUP_PENDING, 0xFF, 0xFFFF, Old, 0, 0, "");
    H_ExpectExactly(S);
    CHECK_EQ(S->FilesChecked, 2);

    /* the next commit finishes it */
    CHECK_RC(EEFS_LibRepairDirectory(&V_Vol), EEFS_SUCCESS);
    S = H_Check(1);
    CHECK_EQ(S->FindingCount, 0);
    V_Destroy();
}

static void health_dir_rebuilt_not_persisted(void)
{
    const EEFS_HealthSummary_t *S;
    uint32                      b;

    H_NewVolume();
    H_InitClean();
    CHECK_RC(V_Write(&H_CA), EEFS_SUCCESS);
    CHECK_RC(V_Write(&H_CB), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    for (b = 0; b < NandTest_Geo.DirRegionBlocks; b++) {
        NandSim_RawErase(V_Sim, b);
    }
    V_Remount();
    CHECK(V_Vol.DirectoryRebuilt);
    S = H_Check(9);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_DIR_MIRROR_MISSING, 0, 0xFFFF, 0xFFFF, 0, 0, "");
    H_Expect(EEFS_HEALTH_DIR_MIRROR_MISSING, 1, 0xFFFF, 0xFFFF, 0, 0, "");
    H_Expect(EEFS_HEALTH_DIR_REBUILT_NOT_PERSISTED, 0xFF, 0xFFFF, 0xFFFF, 0, 0, "");
    H_ExpectExactly(S);
    CHECK_EQ(S->FilesChecked, 2);

    /* a commit persists the rebuilt directory */
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    S = H_Check(9);
    CHECK_EQ(S->FindingCount, 0);
    V_Destroy();
}

/*
 * File findings
 */

static V_File_t H_X3;

/* "/x": three blocks, committed, after H_MakeClean */
static void H_MakeX3(void)
{
    H_NewVolume();
    H_MakeClean();
    H_X3 = H_File("/x", V_SizeForBlocks(3) - 7, 41);
    CHECK_RC(V_Write(&H_X3), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
}

static void health_file_corrupt_page0(void)
{
    const EEFS_HealthSummary_t *S;
    uint32                      i;
    uint32                      n;
    uint32                      k;

    H_MakeX3();
    i = H_Idx("/x");
    n = H_Blocks("/x", 0, H_Blk);
    CHECK_EQ(n, 3);
    NandSim_CorruptBytes(V_Sim, H_Blk[0], 0, 20, 4);
    S = H_Check(3);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_FILE_CORRUPT, 0, i, H_Blk[0], 0, 0, "/x");
    for (k = 0; k < n; k++) {       /* no valid page 0 claims its blocks */
        H_Expect(EEFS_HEALTH_MAP_MISMATCH, 0xFF, 0xFFFF, H_Blk[k], EEFS_NAND_MAP_OWNER(i, 0), 0xFFFF, "");
    }
    H_ExpectExactly(S);
    CHECK(V_Vol.SlotFlags[i][0] & EEFS_NAND_SLOTFLAG_CORRUPT);
    CHECK_EQ(S->Summary, EEFS_HEALTH_DEGRADED | EEFS_HEALTH_DIR_ISSUE | EEFS_HEALTH_FILE_ISSUE);

    /* a directory whose FirstBlock is not a data block: corrupt, and no HAL read outside the data area */
    V_Vol.Working.Entry[i].Slot[0].FirstBlock = (uint16)(NandTest_Geo.DirRegionBlocks - 1);
    S = H_Check(3);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_FILE_CORRUPT, 0, i, NandTest_Geo.DirRegionBlocks - 1, 0, 0, "/x");
    for (k = 0; k < n; k++) {
        H_Expect(EEFS_HEALTH_MAP_MISMATCH, 0xFF, 0xFFFF, H_Blk[k], EEFS_NAND_MAP_OWNER(i, 0), 0xFFFF, "");
    }
    H_ExpectExactly(S);
    CHECK_EQ(NandSim_Violations(V_Sim), 0);
    V_Destroy();
}

static void health_file_corrupt_block_crc(void)
{
    const EEFS_HealthSummary_t *S;
    uint32                      i;

    H_MakeX3();
    i = H_Idx("/x");
    CHECK_EQ(H_Blocks("/x", 0, H_Blk), 3);
    NandSim_CorruptBytes(V_Sim, H_Blk[1], 2, 77, 1);       /* position 1, its third page */
    S = H_Check(2);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_FILE_CORRUPT, 0, i, H_Blk[1], 1, 0, "/x");
    H_ExpectExactly(S);
    CHECK(V_Vol.SlotFlags[i][0] & EEFS_NAND_SLOTFLAG_CORRUPT);
    CHECK_EQ(S->SlotsChecked, 5);

    /* a rewrite repairs it */
    CHECK_RC(V_Write(&H_X3), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    S = H_Check(2);
    CHECK_EQ(S->FindingCount, 0);
    V_Destroy();
}

static void health_file_corrupt_file_crc(void)
{
    const EEFS_HealthSummary_t *S;
    EEFS_NandPage0_t            P;
    uint32                      i;
    uint32                      n;
    uint32                      k;

    /* page 0 rewritten with a wrong file CRC (every block CRC right); the remount's reconcile copies it into the
     * directory, so only the file CRC check can see it */
    H_MakeX3();
    i = H_Idx("/x");
    n = H_Blocks("/x", 0, H_Blk);
    NandSim_RawRead(V_Sim, H_Blk[0], 0, H_PageBuf, NULL);
    CHECK_RC(EEFS_NandDecodePage0(H_PageBuf, NandTest_Geo.PageSize, NandTest_Geo.PagesPerBlock, H_Blk[0], &P),
             EEFS_SUCCESS);
    for (k = 0; k < n; k++) EEFS_NandPage0Block(H_PageBuf, k, &H_Blk2[k], &H_Crc[k]);
    P.FileCrc32 ^= 0x10203040UL;
    CHECK_RC(EEFS_NandEncodePage0(H_PageBuf, NandTest_Geo.PageSize, &P, H_Blk2, H_Crc), EEFS_SUCCESS);
    NandSim_RawWrite(V_Sim, H_Blk[0], 0, H_PageBuf, NULL);
    V_Remount();
    CHECK_EQ(V_Vol.Working.Entry[i].Slot[0].FileCrc32, P.FileCrc32);
    S = H_Check(11);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_FILE_CORRUPT, 0, i, H_Blk[0], 2, 0, "/x");
    H_ExpectExactly(S);
    V_Destroy();
}

static void health_file_io_error(void)
{
    const EEFS_HealthSummary_t *S;
    uint32                      i;
    uint32                      k;
    uint32                      n;

    /* a data page */
    H_MakeX3();
    i = H_Idx("/x");
    n = H_Blocks("/x", 0, H_Blk);
    NandSim_InjectRead(V_Sim, H_Blk[2], 1, EEFS_NAND_READ_UNCORRECTABLE, -1);
    S = H_Check(3);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_FILE_IO_ERROR, 0, i, H_Blk[2], 1, 0, "/x");
    H_ExpectExactly(S);
    CHECK(V_Vol.SlotFlags[i][0] & EEFS_NAND_SLOTFLAG_CORRUPT);
    V_Destroy();

    /* the metadata page */
    H_MakeX3();
    i = H_Idx("/x");
    n = H_Blocks("/x", 0, H_Blk);
    NandSim_InjectRead(V_Sim, H_Blk[0], 0, EEFS_NAND_READ_UNCORRECTABLE, -1);
    S = H_Check(3);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_FILE_IO_ERROR, 0, i, H_Blk[0], 0, 0, "/x");
    for (k = 0; k < n; k++) {
        H_Expect(EEFS_HEALTH_MAP_MISMATCH, 0xFF, 0xFFFF, H_Blk[k], EEFS_NAND_MAP_OWNER(i, 0), 0xFFFF, "");
    }
    H_ExpectExactly(S);
    V_Destroy();
}

static void health_block_needs_scrub(void)
{
    const EEFS_HealthSummary_t *S;
    uint32                      i;

    H_MakeX3();
    i = H_Idx("/x");
    CHECK_EQ(H_Blocks("/x", 0, H_Blk), 3);
    V_Dev.Geometry.EccScrubThreshold = 4;
    NandSim_InjectRead(V_Sim, H_Blk[1], 2, 5, -1);      /* 5 corrected bits: at the threshold */
    NandSim_InjectRead(V_Sim, H_Blk[2], 0, 3, -1);      /* 3: below it */
    S = H_Check(5);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_BLOCK_NEEDS_SCRUB, 0, i, H_Blk[1], 5, 0, "/x");
    H_ExpectExactly(S);
    CHECK_EQ(S->CorrectedBits, 8);
    CHECK_EQ(S->Summary, EEFS_HEALTH_DEGRADED | EEFS_HEALTH_FILE_ISSUE);
    CHECK_EQ(V_Vol.SlotFlags[i][0], 0);     /* the data is fine */

    /* threshold 0: never */
    V_Dev.Geometry.EccScrubThreshold = 0;
    S = H_Check(5);
    CHECK_EQ(S->FindingCount, 0);
    CHECK_EQ(S->CorrectedBits, 8);
    V_Destroy();
}

/*
 * A/B findings
 */

static void health_ab_a_corrupt(void)
{
    const EEFS_HealthSummary_t *S;
    uint32                      i;

    H_NewVolume();
    A_MakeF(1);
    i = H_Idx("/f");
    A_CorruptData("/f", EEFS_NAND_SLOT_A);
    S = H_Check(4);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_FILE_CORRUPT, 0, i, H_First("/f", 0), 1, 0, "/f");
    H_Expect(EEFS_HEALTH_AB_A_CORRUPT, 0, i, H_First("/f", 0), 0, 0, "/f");
    H_Expect(EEFS_HEALTH_AB_VERSION_MISMATCH, 0xFF, i, 0xFFFF, 1, 2, "/f");
    H_ExpectExactly(S);
    CHECK_EQ(H_E("/f")->ActiveFailed, 0);       /* A is not the active copy */
    CHECK_EQ(S->Summary, EEFS_HEALTH_DEGRADED | EEFS_HEALTH_AB_ISSUE | EEFS_HEALTH_FILE_ISSUE | EEFS_HEALTH_INFO);
    V_Destroy();
}

/* B active and corrupt (Fault 0: page 0, 1: data CRC, 2: uncorrectable ECC): AB_B_CORRUPT and AB_ACTIVE_CORRUPT, and
 * the check sets ActiveFailed (Open checks page 0 only by default, so for the data faults the check is what finds
 * it).  The next commit persists the flag; the next check reports it as already set; the bootloader falls back. */
static void H_ActiveCorrupt(uint32 Fault)
{
    const EEFS_HealthSummary_t *S;
    uint32                      i;
    uint32                      FirstB;
    uint32                      Pass;
    int32                       Fd;

    H_NewVolume();
    A_MakeF(1);
    i = H_Idx("/f");
    FirstB = H_First("/f", 1);
    if (Fault == 0) A_CorruptPage0("/f", EEFS_NAND_SLOT_B);
    if (Fault == 1) A_CorruptData("/f", EEFS_NAND_SLOT_B);
    if (Fault == 2) A_InjectEcc("/f", EEFS_NAND_SLOT_B, 1);
#if !EEFS_NAND_VERIFY_ON_OPEN
    if (Fault != 0) {           /* Open checks page 0 only: it opens the corrupt active copy and flags nothing */
        Fd = EEFS_LibOpen(&V_Vol, "/f", O_RDONLY, 0);
        CHECK(Fd >= 0);
        if (Fd >= 0) EEFS_LibClose(Fd);
        CHECK_EQ(H_E("/f")->ActiveFailed, 0);
    }
#else
    (void)Fd;
#endif
    for (Pass = 0; Pass < 2; Pass++) {
        S = H_Check(3);
        H_ExpReset();
        if (Fault == 0) {
            H_Expect(EEFS_HEALTH_FILE_CORRUPT, 1, i, FirstB, 0, 0, "/f");
            H_Expect(EEFS_HEALTH_MAP_MISMATCH, 0xFF, 0xFFFF, FirstB, EEFS_NAND_MAP_OWNER(i, 1), 0xFFFF, "");
        }
        if (Fault == 1) H_Expect(EEFS_HEALTH_FILE_CORRUPT, 1, i, FirstB, 1, 0, "/f");
        if (Fault == 2) H_Expect(EEFS_HEALTH_FILE_IO_ERROR, 1, i, FirstB, 1, 0, "/f");
        H_Expect(EEFS_HEALTH_AB_B_CORRUPT, 1, i, FirstB, 0, 0, "/f");
        H_Expect(EEFS_HEALTH_AB_ACTIVE_CORRUPT, 1, i, FirstB, 0, 0, "/f");
        H_Expect(EEFS_HEALTH_AB_VERSION_MISMATCH, 0xFF, i, 0xFFFF, 1, 2, "/f");
        if (Pass == 1) H_Expect(EEFS_HEALTH_AB_FAILED_FLAG_SET, 1, i, 0xFFFF, 0, 0, "/f");
        H_ExpectExactly(S);
        CHECK_EQ(S->Restarts, 0);       /* setting the flag does not restart the check */

        /* the flag: in Working and Base, to be persisted */
        CHECK_EQ(V_Vol.Working.Entry[i].ActiveFailed, 1);
        CHECK_EQ(V_Vol.Base.Entry[i].ActiveFailed, 1);
        CHECK(V_Vol.SlotFlags[i][1] & EEFS_NAND_SLOTFLAG_CORRUPT);
        if (Pass == 0) {
            CHECK(EEFS_NAND_MASK_TEST(V_Vol.ForceDirty, i));
            CHECK_RC(EEFS_LibAbort(&V_Vol), EEFS_SUCCESS);          /* a detected fact survives Abort */
            CHECK_EQ(V_Vol.Working.Entry[i].ActiveFailed, 1);
            CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
            V_Remount();
            CHECK_EQ(V_Vol.Working.Entry[i].ActiveFailed, 1);
        }
    }
    A_CheckBoot("/f", EEFS_SUCCESS, EEFS_NAND_SLOT_A, &A_F1, 1);
    V_Destroy();
}

static void health_ab_b_corrupt_and_active_corrupt_sets_flag(void)
{
    uint32  Fault;

    for (Fault = 0; (Fault < 3) && !NandTest_CaseFailed(); Fault++) {
        H_ActiveCorrupt(Fault);
        if (NandTest_CaseFailed()) printf("    fault %lu\n", (unsigned long)Fault);
    }
}

static void health_ab_active_missing(void)
{
    const EEFS_HealthSummary_t *S;
    V_File_t                    G = H_File("/g", 2000, 51);
    uint32                      i;

    H_NewVolume();
    CHECK_RC(A_Put("/g", &G, EEFS_NAND_SLOT_A, 3), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    i = H_Idx("/g");
    V_Vol.Working.Entry[i].ActiveSlot = EEFS_NAND_SLOT_B;      /* a directory naming a copy that does not exist */
    S = H_Check(6);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_AB_ACTIVE_MISSING, 1, i, 0xFFFF, 0, 0, "/g");
    H_Expect(EEFS_HEALTH_AB_FALLBACK_UNAVAILABLE, 0, i, 0xFFFF, 0, 0, "/g");
    H_ExpectExactly(S);
    CHECK_EQ(S->Summary, EEFS_HEALTH_DEGRADED | EEFS_HEALTH_AB_ISSUE | EEFS_HEALTH_INFO);
    V_Destroy();
}

static void health_ab_version_mismatch(void)
{
    const EEFS_HealthSummary_t *S;

    H_NewVolume();
    A_MakeF(1);
    S = H_Check(6);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_AB_VERSION_MISMATCH, 0xFF, H_Idx("/f"), 0xFFFF, 1, 2, "/f");
    H_ExpectExactly(S);
    CHECK_EQ(S->Summary, EEFS_HEALTH_INFO);         /* B under test: informational */

    /* after a promote the versions agree */
    CHECK_RC(EEFS_LibCopySlotAll(&V_Vol, EEFS_NAND_SLOT_B, EEFS_NAND_SLOT_A), 1);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    S = H_Check(6);
    CHECK_EQ(S->FindingCount, 0);
    V_Destroy();
}

static void health_ab_fallback_unavailable(void)
{
    const EEFS_HealthSummary_t *S;
    V_File_t                    G = H_File("/g", 2000, 52);

    H_NewVolume();
    A_MakeF(0);                                         /* not compatible */
    CHECK_RC(A_Put("/g", &G, EEFS_NAND_SLOT_B, 3), EEFS_SUCCESS);   /* only one copy */
    CHECK_RC(EEFS_LibSetFallbackCompatible(&V_Vol, "/g", 1), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    S = H_Check(6);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_AB_VERSION_MISMATCH, 0xFF, H_Idx("/f"), 0xFFFF, 1, 2, "/f");
    H_Expect(EEFS_HEALTH_AB_FALLBACK_UNAVAILABLE, 0, H_Idx("/f"), 0xFFFF, 0, 0, "/f");
    H_Expect(EEFS_HEALTH_AB_FALLBACK_UNAVAILABLE, 0, H_Idx("/g"), 0xFFFF, 0, 0, "/g");
    H_ExpectExactly(S);
    CHECK_EQ(S->Summary, EEFS_HEALTH_INFO);
    V_Destroy();
}

static void health_ab_failed_flag_set(void)
{
    const EEFS_HealthSummary_t *S;

    H_NewVolume();
    A_MakeF(1);
    CHECK_RC(EEFS_LibSetFailed(&V_Vol, "/f"), EEFS_SUCCESS);       /* the bootloader reported a failed boot */
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    S = H_Check(6);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_AB_VERSION_MISMATCH, 0xFF, H_Idx("/f"), 0xFFFF, 1, 2, "/f");
    H_Expect(EEFS_HEALTH_AB_FAILED_FLAG_SET, 1, H_Idx("/f"), 0xFFFF, 0, 0, "/f");
    H_ExpectExactly(S);
    CHECK_EQ(S->Summary, EEFS_HEALTH_DEGRADED | EEFS_HEALTH_AB_ISSUE | EEFS_HEALTH_INFO);

    /* ClearFailed */
    CHECK_RC(EEFS_LibClearFailed(&V_Vol, "/f"), EEFS_SUCCESS);
    S = H_Check(6);
    CHECK_EQ(H_Count(S, EEFS_HEALTH_AB_FAILED_FLAG_SET), 0);
    V_Destroy();
}

static void health_ab_load_mixed(void)
{
    const EEFS_HealthSummary_t *S;
    V_File_t                    Single = H_File("/single", 1234, 65);

    H_NewVolume();
    A_MakeClean("/l1", 61, EEFS_NAND_SLOT_A);
    A_MakeClean("/l2", 62, EEFS_NAND_SLOT_B);
    A_MakeClean("/l3", 63, EEFS_NAND_SLOT_A);
    A_MakeClean("/l4", 64, EEFS_NAND_SLOT_B);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    S = H_Check(6);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_AB_LOAD_MIXED, 1, H_Idx("/l2"), 0xFFFF, 0, 1, "/l2");      /* one finding */
    H_ExpectExactly(S);
    CHECK_EQ(S->Summary, EEFS_HEALTH_INFO);

    /* a single-copy file does not count */
    CHECK_RC(EEFS_LibSetActiveSlotAll(&V_Vol, EEFS_NAND_SLOT_B), 4);
    CHECK_RC(V_Write(&Single), EEFS_SUCCESS);
    S = H_Check(6);
    CHECK_EQ(S->FindingCount, 0);
    V_Destroy();
}

/*
 * Directory against the metadata pages and the map
 */

static void health_dir_page0_mismatch(void)
{
    const EEFS_HealthSummary_t *S;
    EEFS_NandEntry_t           *E;
    V_File_t                    G = H_File("/g", 3000, 71);
    uint32                      Reason;
    uint32                      i;
    uint32                      b;

    for (Reason = 1; (Reason <= 4) && !NandTest_CaseFailed(); Reason++) {
        H_MakeX3();
        if (Reason == 2) {
            /* "/g" exists only as B; the directory says it is A */
            CHECK_RC(A_Put("/g", &G, EEFS_NAND_SLOT_B, 3), EEFS_SUCCESS);
            CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
            i = H_Idx("/g");
            E = &V_Vol.Working.Entry[i];
            E->Slot[0] = E->Slot[1];
            memset(&E->Slot[1], 0, sizeof(E->Slot[1]));
            E->ActiveSlot = EEFS_NAND_SLOT_A;
            for (b = 0; b < NandTest_Geo.BlockCount; b++) {
                if (V_Vol.Working.Map[b] == EEFS_NAND_MAP_OWNER(i, 1)) V_Vol.Working.Map[b] = EEFS_NAND_MAP_OWNER(i, 0);
            }
            S = H_Check(4);
            H_ExpReset();
            H_Expect(EEFS_HEALTH_DIR_PAGE0_MISMATCH, 0, i, E->Slot[0].FirstBlock, 2, 0, "/g");
            H_Expect(EEFS_HEALTH_AB_A_CORRUPT, 0, i, E->Slot[0].FirstBlock, 0, 0, "/g");
            H_Expect(EEFS_HEALTH_AB_ACTIVE_CORRUPT, 0, i, E->Slot[0].FirstBlock, 0, 0, "/g");
            H_Expect(EEFS_HEALTH_AB_FALLBACK_UNAVAILABLE, 1, i, 0xFFFF, 0, 0, "/g");
            H_ExpectExactly(S);
            CHECK_EQ(E->ActiveFailed, 1);   /* the page is not this copy: it fails like a failed validation */
            V_Destroy();
            continue;
        }
        i = H_Idx("/x");
        E = &V_Vol.Working.Entry[i];
        if (Reason == 1) strcpy(E->Path, "/x-renamed");
        if (Reason == 3) E->Slot[0].AllocBlocks++;
        if (Reason == 4) E->Slot[0].Version ^= 1;
        S = H_Check(4);
        H_ExpReset();
        H_Expect(EEFS_HEALTH_DIR_PAGE0_MISMATCH, 0, i, E->Slot[0].FirstBlock, Reason, 0, E->Path);
        H_ExpectExactly(S);
        /* reason 1: the page belongs to another name (the slot fails); 3 and 4: the content is still checked and
         * good */
        CHECK_EQ((V_Vol.SlotFlags[i][0] & EEFS_NAND_SLOTFLAG_CORRUPT) != 0, (Reason == 1) ? 1 : 0);
        V_Destroy();
        if (NandTest_CaseFailed()) printf("    reason %lu\n", (unsigned long)Reason);
    }
}

static void health_map_mismatch(void)
{
    const EEFS_HealthSummary_t *S;
    uint32                      i;
    uint32                      b;

    /* a free block given to an owner */
    H_MakeX3();
    i = H_Idx("/x");
    b = H_FreeBlock(5);
    V_Vol.Working.Map[b] = EEFS_NAND_MAP_OWNER(i, 0);
    S = H_Check(8);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_MAP_MISMATCH, 0xFF, 0xFFFF, b, EEFS_NAND_MAP_OWNER(i, 0), 0xFFFF, "");
    H_ExpectExactly(S);
    V_Destroy();

    /* a listed block that the map calls free, and a DIR value in the data area */
    H_MakeX3();
    i = H_Idx("/x");
    CHECK_EQ(H_Blocks("/x", 0, H_Blk), 3);
    V_Vol.Working.Map[H_Blk[2]] = EEFS_NAND_MAP_FREE;
    b = H_FreeBlock(2);
    V_Vol.Working.Map[b] = EEFS_NAND_MAP_DIR;
    S = H_Check(8);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_MAP_MISMATCH, 0xFF, 0xFFFF, H_Blk[2], EEFS_NAND_MAP_FREE, EEFS_NAND_MAP_OWNER(i, 0), "");
    H_Expect(EEFS_HEALTH_MAP_MISMATCH, 0xFF, 0xFFFF, b, EEFS_NAND_MAP_DIR, 0xFFFF, "");
    H_ExpectExactly(S);
    CHECK_EQ(V_Vol.SlotFlags[i][0], 0);     /* the file itself is fine */
    V_Destroy();
}

static void health_block_crosslinked(void)
{
    const EEFS_HealthSummary_t *S;
    EEFS_NandPage0_t            P;
    V_File_t                    X = H_File("/cx", V_SizeForBlocks(2), 81);
    V_File_t                    Y = H_File("/cy", V_SizeForBlocks(2), 81);     /* the same content */
    uint32                      ix;
    uint32                      iy;
    uint32                      k;

    H_NewVolume();
    CHECK_RC(V_Write(&X), EEFS_SUCCESS);
    CHECK_RC(V_Write(&Y), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    ix = H_Idx("/cx");
    iy = H_Idx("/cy");
    CHECK(ix < iy);
    CHECK_EQ(H_Blocks("/cx", 0, H_Blk), 2);
    CHECK_EQ(H_Blocks("/cy", 0, H_Blk2), 2);

    /* /cy's metadata page lists /cx's second block (same data, so every CRC still matches) */
    NandSim_RawRead(V_Sim, H_Blk2[0], 0, H_PageBuf, NULL);
    CHECK_RC(EEFS_NandDecodePage0(H_PageBuf, NandTest_Geo.PageSize, NandTest_Geo.PagesPerBlock, H_Blk2[0], &P),
             EEFS_SUCCESS);
    {
        uint16  B[2];
        uint32  C[2];

        for (k = 0; k < 2; k++) EEFS_NandPage0Block(H_PageBuf, k, &B[k], &C[k]);
        B[1] = H_Blk[1];
        CHECK_RC(EEFS_NandEncodePage0(H_PageBuf, NandTest_Geo.PageSize, &P, B, C), EEFS_SUCCESS);
    }
    NandSim_RawWrite(V_Sim, H_Blk2[0], 0, H_PageBuf, NULL);
    S = H_Check(3);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_BLOCK_CROSSLINKED, 0, iy, H_Blk[1], EEFS_NAND_MAP_OWNER(ix, 0), 0, "/cy");
    H_Expect(EEFS_HEALTH_MAP_MISMATCH, 0xFF, 0xFFFF, H_Blk2[1], EEFS_NAND_MAP_OWNER(iy, 0), 0xFFFF, "");
    H_ExpectExactly(S);
    V_Destroy();
}

static void health_badblock_unrecorded(void)
{
    const EEFS_HealthSummary_t *S;
    uint32                      Data;
    uint32                      Dir;
    uint32                      Known;

    H_NewVolume();
    H_MakeClean();
    Data = H_FreeBlock(0);
    Known = H_FreeBlock(1);
    Dir = V_Vol.Mirror[1].Blocks[(V_Vol.Mirror[1].CurIndex + 2) % V_Vol.Mirror[1].Count];
    NandSim_SetFactoryBad(V_Sim, Data);
    NandSim_SetFactoryBad(V_Sim, Dir);
    NandSim_SetFactoryBad(V_Sim, Known);
    V_Vol.Working.Map[Known] = EEFS_NAND_MAP_BAD;          /* recorded: not reported */
    S = H_Check(2);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_BADBLOCK_UNRECORDED, 0xFF, 0xFFFF, Data, 0, 0, "");
    H_Expect(EEFS_HEALTH_BADBLOCK_UNRECORDED, 0xFF, 0xFFFF, Dir, 0, 0, "");
    H_ExpectExactly(S);
    V_Destroy();
}

/*
 * Orphans and ReclaimOrphans
 */

static void health_orphan_file(void)
{
    const EEFS_HealthSummary_t *S;
    V_File_t                    O = H_File("/o", V_SizeForBlocks(2), 91);
    V_File_t                    Q = H_File("/q", 500, 92);
    uint32                      Fo;
    uint32                      Fq;

    /* an uncommitted batch, then a reboot */
    H_NewVolume();
    H_MakeClean();
    CHECK_RC(V_Write(&O), EEFS_SUCCESS);
    Fo = H_First("/o", 0);
    H_RebootKeepCursor();
    S = H_Check(5);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_ORPHAN_FILE, EEFS_NAND_SLOT_NONE, 0xFFFF, Fo, H_PageSeq(Fo), 0, "/o");
    H_ExpectExactly(S);

    /* an aborted batch whose abort erase was cut */
    CHECK_RC(A_Put("/q", &Q, EEFS_NAND_SLOT_B, 9), EEFS_SUCCESS);
    Fq = H_First("/q", 1);
    NandSim_SetPowerBudget(V_Sim, 0, NANDSIM_TORN_NONE);
    (void)EEFS_LibAbort(&V_Vol);
    CHECK(NandSim_PowerCutHappened(V_Sim));
    H_Reboot();
    S = H_Check(5);
    H_ExpReset();
    H_Expect(EEFS_HEALTH_ORPHAN_FILE, EEFS_NAND_SLOT_NONE, 0xFFFF, Fo, H_PageSeq(Fo), 0, "/o");
    H_Expect(EEFS_HEALTH_ORPHAN_FILE, EEFS_NAND_SLOT_B, 0xFFFF, Fq, H_PageSeq(Fq), 0, "/q");
    H_ExpectExactly(S);
    CHECK_EQ(S->Summary, EEFS_HEALTH_DEGRADED | EEFS_HEALTH_DIR_ISSUE);

    /* reclaimed */
    CHECK_RC(EEFS_LibReclaimOrphans(&V_Vol), 2);
    CHECK(NandSim_PageIsErased(V_Sim, Fo, 0));
    CHECK(NandSim_PageIsErased(V_Sim, Fq, 0));
    S = H_Check(5);
    CHECK_EQ(S->FindingCount, 0);
    CHECK_RC(EEFS_LibReclaimOrphans(&V_Vol), 0);
    V_CheckFile(&V_Vol, &H_CA);
    V_CheckFile(&V_Vol, &H_CB);
    V_Destroy();
}

/* An open batch (new files, a copy-on-write rewrite, a rename, a remove, a CopySlot) is consistent: no orphan, no
 * cross-link, no map finding */
static void health_open_batch_no_false_orphans(void)
{
    const EEFS_HealthSummary_t *S;
    V_File_t                    N1 = H_File("/n1", V_SizeForBlocks(2), 101);
    V_File_t                    A2;

    H_NewVolume();
    H_MakeClean();
    A2 = H_Mod(H_CA, V_FileOffset(1, 5));
    A_MakeF(1);
    CHECK_RC(V_Write(&N1), EEFS_SUCCESS);
    CHECK_RC(V_Write(&A2), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibRename(&V_Vol, "/b", "/b2"), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/c"), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/f", EEFS_NAND_SLOT_B, EEFS_NAND_SLOT_A), EEFS_SUCCESS);
    S = H_Check(10);
    H_ExpReset();
    H_ExpectExactly(S);
    CHECK_EQ(S->FilesChecked, 4);
    CHECK_EQ(S->SlotsChecked, 5);

    /* ReclaimOrphans leaves the batch alone, then the batch commits and verifies */
    CHECK_RC(EEFS_LibReclaimOrphans(&V_Vol), 0);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_CheckFile(&V_Vol, &A2);
    V_CheckFile(&V_Vol, &N1);
    S = H_Check(10);
    CHECK_EQ(S->FindingCount, 0);
    V_Destroy();
}

/* ReclaimOrphans erases exactly the orphans: never an owned block (also of the open batch), a FREEING block or a block
 * on the cleanup list */
static void reclaim_orphans_erases_only_unreferenced(void)
{
    V_File_t                    O1 = H_File("/o1", V_SizeForBlocks(2), 111);
    V_File_t                    O2 = H_File("/o2", 300, 112);
    V_File_t                    N1 = H_File("/n1", V_SizeForBlocks(2), 113);
    V_File_t                    A2;
    static uint32               Before[EEFS_NAND_MAX_BLOCKS];
    uint32                      Orphan[2];
    uint32                      Old;
    uint32                      Extra;
    uint32                      b;
    uint32                      n = 0;
    uint32                      i;
    int                         IsOrphan;

    H_NewVolume();
    H_MakeClean();
    A2 = H_Mod(H_CA, V_FileOffset(2, 9));
    CHECK_RC(V_Write(&O1), EEFS_SUCCESS);
    CHECK_RC(V_Write(&O2), EEFS_SUCCESS);
    Orphan[0] = H_First("/o1", 0);
    Orphan[1] = H_First("/o2", 0);
    H_RebootKeepCursor();

    /* an open batch: a new file, a copy-on-write rewrite (FREEING blocks, the old first block on the list) */
    CHECK_RC(V_Write(&N1), EEFS_SUCCESS);
    CHECK_RC(V_Write(&A2), EEFS_SUCCESS);
    CHECK(V_Vol.PendingCleanupCount == 1);
    for (b = 0; b < NandTest_Geo.BlockCount; b++) Before[b] = NandSim_BlockErases(V_Sim, b);
    CHECK_RC(EEFS_LibReclaimOrphans(&V_Vol), 2);
    for (b = 0; b < V_Dev.Geometry.BlockCount; b++) {
        IsOrphan = (b == Orphan[0]) || (b == Orphan[1]);
        CHECK_EQ(NandSim_BlockErases(V_Sim, b) - Before[b], IsOrphan ? 1 : 0);
        if (IsOrphan) {
            n++;
            CHECK(NandSim_PageIsErased(V_Sim, b, 0));
        }
    }
    CHECK_EQ(n, 2);
    for (i = 0; i < V_Vol.PendingCleanupCount; i++) {
        CHECK(!NandSim_PageIsErased(V_Sim, V_Vol.PendingCleanup[i], 0));
    }

    /* each protection on its own (RAM states a consistent volume never has; restored afterwards): a FREEING page 0
     * missing from the cleanup list, and a listed block that is free in the map */
    Old = V_Vol.PendingCleanup[0];
    CHECK_EQ(V_Vol.Working.Map[Old], EEFS_NAND_MAP_FREEING);
    memcpy(&H_Saved, &V_Vol, sizeof(H_Saved));
    EEFS_NandForgetCleanup(&V_Vol, (uint16)Old);
    CHECK_RC(EEFS_LibReclaimOrphans(&V_Vol), 0);
    CHECK(!NandSim_PageIsErased(V_Sim, Old, 0));
    memcpy(&V_Vol, &H_Saved, sizeof(V_Vol));
    CHECK_RC(V_Write(&O2), EEFS_SUCCESS);           /* a valid page 0 in a block that will be free again */
    Extra = H_First("/o2", 0);
    memcpy(&H_Saved, &V_Vol, sizeof(H_Saved));
    V_Vol.Working.Map[Extra] = EEFS_NAND_MAP_FREE;
    V_Vol.Base.Map[Extra] = EEFS_NAND_MAP_FREE;
    CHECK_RC(EEFS_NandAddCleanup(&V_Vol, (uint16)Extra), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibReclaimOrphans(&V_Vol), 0);
    CHECK(!NandSim_PageIsErased(V_Sim, Extra, 0));
    memcpy(&V_Vol, &H_Saved, sizeof(V_Vol));

    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_CheckFile(&V_Vol, &A2);
    V_CheckFile(&V_Vol, &N1);
    V_CheckFile(&V_Vol, &H_CB);
    V_CheckFile(&V_Vol, &O2);
    CHECK_EQ(H_Check(16)->FindingCount, 0);
    V_Destroy();
}

/*
 * Space
 */

static void health_free_below_reserve(void)
{
    const EEFS_HealthSummary_t *S;
    uint32                      Target[3];
    uint32                      t;
    uint32                      Free;

    Target[0] = EEFS_NAND_RESERVE_BLOCKS;               /* at the reserve: nothing */
    Target[1] = EEFS_NAND_RESERVE_BLOCKS - 1;           /* below it: informational */
    Target[2] = EEFS_NAND_BADBLOCK_RESERVE - 1;         /* below the bad-block reserve: degraded */
    for (t = 0; (t < 3) && !NandTest_CaseFailed(); t++) {
        H_NewVolume();
        H_MakeClean();
        while ((Free = EEFS_NandFreeCount(&V_Vol)) > Target[t]) {
            V_Vol.Working.Map[H_FreeBlock(0)] = EEFS_NAND_MAP_BAD;
        }
        S = H_Check(32);
        H_ExpReset();
        if (t > 0) {
            H_Expect(EEFS_HEALTH_FREE_BELOW_RESERVE, 0xFF, 0xFFFF, 0xFFFF, Target[t], EEFS_NAND_RESERVE_BLOCKS, "");
        }
        H_ExpectExactly(S);
        if (t == 0) CHECK_EQ(S->Summary, EEFS_HEALTH_OK);
        if (t == 1) CHECK_EQ(S->Summary, EEFS_HEALTH_SPACE_ISSUE | EEFS_HEALTH_INFO);
        if (t == 2) CHECK_EQ(S->Summary, EEFS_HEALTH_SPACE_ISSUE | EEFS_HEALTH_DEGRADED);
        V_Destroy();
    }
}

/*
 * Protocol: step size, restarts, cancel, overflow
 */

/* Fault mixes for the step-size case */
static void H_Mix(uint32 Mix)
{
    V_File_t    O = H_File("/o", V_SizeForBlocks(2) + 3, 121);
    V_File_t    D = H_File("/d", 999, 122);
    uint32      i;

    H_NewVolume();
    H_MakeClean();
    if (Mix == 0) return;
    if (Mix == 1) {
        /* A/B active copy corrupt, the inactive page 0 corrupt, an orphan, a map fault, a block to scrub */
        A_MakeF(1);
        H_X3 = H_File("/x", V_SizeForBlocks(3) - 7, 41);
        CHECK_RC(V_Write(&H_X3), EEFS_SUCCESS);
        CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
        CHECK_RC(V_Write(&O), EEFS_SUCCESS);
        H_Reboot();
        A_CorruptData("/f", EEFS_NAND_SLOT_B);
        A_CorruptPage0("/f", EEFS_NAND_SLOT_A);
        i = H_Idx("/x");
        V_Vol.Working.Map[H_FreeBlock(3)] = EEFS_NAND_MAP_OWNER(i, 0);
        CHECK_EQ(H_Blocks("/x", 0, H_Blk), 3);
        V_Dev.Geometry.EccScrubThreshold = 2;
        NandSim_InjectRead(V_Sim, H_Blk[1], 3, 2, -1);
        return;
    }
    /* directory faults: a lagging mirror with junk after mirror 0's chain, a torn checkpoint in mirror 1, a pending
     * cleanup, an unrecorded bad block, an uncorrectable data page */
    CHECK_RC(V_Write(&D), EEFS_SUCCESS);
    V_Vol.Mirror[1].Disabled = TRUE;            /* mirror 1 misses the next commits */
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/b"), EEFS_SUCCESS);
    H_CommitCutCleanup(1);                      /* last mutation: a later write would run the cleanup */
    memset(H_PageBuf, 0x33, sizeof(H_PageBuf));
    NandSim_RawWrite(V_Sim, H_MirrorBlock(0), V_Vol.Mirror[0].NextPage, H_PageBuf, NULL);
    H_RawRecord(V_Vol.Mirror[1].Blocks[(V_Vol.Mirror[1].CurIndex + 2) % V_Vol.Mirror[1].Count], 0,
                EEFS_NAND_REC_CHECKPOINT, V_Vol.LastSeq + 3, 0, 3);
    NandSim_SetFactoryBad(V_Sim, H_FreeBlock(7));
    CHECK_EQ(H_Blocks("/a", 0, H_Blk), 3);
    NandSim_InjectRead(V_Sim, H_Blk[1], 4, EEFS_NAND_READ_UNCORRECTABLE, -1);
    V_Remount();
}

static void health_maxpagereads_1_equals_one_big_step(void)
{
    static const uint32         Max[3] = { 1, 13, 0xFFFFFFFFUL };
    static EEFS_HealthSummary_t First;
    const EEFS_HealthSummary_t *S;
    uint32                      Mix;
    uint32                      m;
    uint32                      MinFindings[3] = { 0, 8, 6 };

    for (Mix = 0; (Mix < 3) && !NandTest_CaseFailed(); Mix++) {
        H_Mix(Mix);
        memcpy(&H_Saved, &V_Vol, sizeof(H_Saved));
        for (m = 0; m < 3; m++) {
            memcpy(&V_Vol, &H_Saved, sizeof(V_Vol));        /* the same volume RAM for every run */
            S = H_Check(Max[m]);
            if (m == 0) {
                memcpy(&First, S, sizeof(First));
                CHECK(S->FindingCount >= MinFindings[Mix]);
                CHECK_EQ(S->Steps, S->PagesRead + 1);
            }
            else if (!H_SameReport(S, &First, 0)) {
                printf("    mix %lu: MaxPageReads %lu differs from MaxPageReads 1\n", (unsigned long)Mix,
                       (unsigned long)Max[m]);
                H_Print(&First);
                H_Print(S);
                CHECK(0);
            }
        }
        if (Mix == 1) {
            CHECK_EQ(H_Count(&First, EEFS_HEALTH_AB_ACTIVE_CORRUPT), 1);
            CHECK_EQ(H_Count(&First, EEFS_HEALTH_ORPHAN_FILE), 1);
            CHECK_EQ(H_Count(&First, EEFS_HEALTH_BLOCK_NEEDS_SCRUB), 1);
        }
        if (Mix == 2) {
            CHECK_EQ(H_Count(&First, EEFS_HEALTH_DIR_MIRROR_MISMATCH), 1);
            CHECK_EQ(H_Count(&First, EEFS_HEALTH_DIR_RECORD_CORRUPT), 1);
            CHECK_EQ(H_Count(&First, EEFS_HEALTH_DIR_UNCOMMITTED_TXN), 1);
            CHECK_EQ(H_Count(&First, EEFS_HEALTH_DIR_CLEANUP_PENDING), 1);
            CHECK_EQ(H_Count(&First, EEFS_HEALTH_BADBLOCK_UNRECORDED), 1);
            CHECK_EQ(H_Count(&First, EEFS_HEALTH_FILE_IO_ERROR), 1);
        }
        V_Destroy();
        if (NandTest_CaseFailed()) printf("    mix %lu\n", (unsigned long)Mix);
    }
}

/* Steps until the check is in phase Phase (or done); returns the last step's code */
static int32 H_StepTo(uint8 Phase, uint32 Max)
{
    int32   Status = 1;

    while ((Status == 1) && (H_Rep.Phase < Phase)) {
        Status = EEFS_LibHealthCheckStep(&V_Vol, Max);
    }
    return(Status);
}

static void health_restart_on_change_during_check(void)
{
    const EEFS_HealthSummary_t *S;
    V_File_t                    D = H_File("/d", V_SizeForBlocks(2), 131);
    V_File_t                    A2;
    uint32                      Programs;
    int32                       Status;

    H_NewVolume();
    H_MakeClean();
    A2 = H_Mod(H_CA, V_FileOffset(2, 3));
    A_MakeF(1);
    A_CorruptData("/f", EEFS_NAND_SLOT_B);          /* the check sets ActiveFailed: that must not restart it */

    CHECK_RC(EEFS_LibHealthCheckStart(&V_Vol, &H_Rep), EEFS_SUCCESS);
    CHECK_RC(H_StepTo(2, 3), 1);
    CHECK_RC(EEFS_LibHealthCheckStep(&V_Vol, 5), 1);
    CHECK_EQ(H_Rep.S.Restarts, 0);

    /* a write between steps (nothing committed): restart */
    CHECK_RC(V_Write(&D), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibHealthCheckStep(&V_Vol, 5), 1);
    CHECK_EQ(H_Rep.S.Restarts, 1);
    CHECK_EQ(H_Rep.Phase, 1);
    CHECK_RC(H_StepTo(3, 2), 1);                    /* into the A/B phase: ActiveFailed now set by the check */
    CHECK_EQ(V_Vol.Working.Entry[H_Idx("/f")].ActiveFailed, 1);
    CHECK_RC(EEFS_LibHealthCheckStep(&V_Vol, 1), 1);
    CHECK_EQ(H_Rep.S.Restarts, 1);

    /* a commit: restart */
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibHealthCheckStep(&V_Vol, 5), 1);
    CHECK_EQ(H_Rep.S.Restarts, 2);

    /* a commit with nothing to write changes nothing: no restart */
    Programs = NandSim_Programs(V_Sim);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK_EQ(NandSim_Programs(V_Sim), Programs);
    CHECK_RC(EEFS_LibHealthCheckStep(&V_Vol, 5), 1);
    CHECK_EQ(H_Rep.S.Restarts, 2);

    /* the file being checked is rewritten and committed (its old first block erased) in the middle of its data */
    CHECK_RC(H_StepTo(2, 1), 1);
    while ((H_Rep.Phase == 2) && (H_Rep.EntryIndex < (uint16)H_Idx("/a"))) {
        CHECK_RC(EEFS_LibHealthCheckStep(&V_Vol, 1), 1);
    }
    CHECK_RC(EEFS_LibHealthCheckStep(&V_Vol, 4), 1);
    CHECK_EQ(H_Rep.EntryIndex, H_Idx("/a"));
    CHECK_RC(V_Write(&A2), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    do {
        Status = EEFS_LibHealthCheckStep(&V_Vol, 6);
    } while (Status == 1);
    CHECK_RC(Status, EEFS_SUCCESS);
    CHECK_EQ(H_Rep.S.Restarts, 3);
    memcpy(&H_Sum2, &H_Rep.S, sizeof(H_Sum2));

    /* the final report: correct for the final state (the flag set before a restart is not "already set") */
    H_ExpReset();
    H_Expect(EEFS_HEALTH_FILE_CORRUPT, 1, H_Idx("/f"), H_First("/f", 1), 1, 0, "/f");
    H_Expect(EEFS_HEALTH_AB_B_CORRUPT, 1, H_Idx("/f"), H_First("/f", 1), 0, 0, "/f");
    H_Expect(EEFS_HEALTH_AB_ACTIVE_CORRUPT, 1, H_Idx("/f"), H_First("/f", 1), 0, 0, "/f");
    H_Expect(EEFS_HEALTH_AB_VERSION_MISMATCH, 0xFF, H_Idx("/f"), 0xFFFF, 1, 2, "/f");
    H_Expect(EEFS_HEALTH_AB_LOAD_MIXED, 1, H_Idx("/f"), 0xFFFF, 0, 1, "/f");    /* /c runs A, /f runs B */
    H_ExpectExactly(&H_Sum2);
    CHECK_EQ(H_Sum2.FilesChecked, 5);
    CHECK_EQ(H_Sum2.DirSeq, V_Vol.LastSeq);
    V_CheckFile(&V_Vol, &A2);

    /* an uninterrupted check of the same state: the same reads, and now the flag counts as set before the check */
    S = H_Check(1000);
    CHECK_EQ(H_Count(S, EEFS_HEALTH_AB_FAILED_FLAG_SET), 1);
    CHECK_EQ(S->FindingCount, H_Sum2.FindingCount + 1);
    CHECK_EQ(S->FilesChecked, H_Sum2.FilesChecked);
    CHECK_EQ(S->SlotsChecked, H_Sum2.SlotsChecked);
    CHECK_EQ(S->PagesRead, H_Sum2.PagesRead);
    V_Destroy();
}

static void health_cancel_and_summary(void)
{
    EEFS_HealthReport_t         Other;
    EEFS_InodeTable_t          *Unmounted = &V_Aux;
    const EEFS_HealthSummary_t *S;

    H_NewVolume();
    H_MakeClean();
    CHECK_RC(EEFS_LibHealthCheckSummary(&V_Vol, &H_Sum), EEFS_ERROR);      /* none completed yet */
    CHECK_RC(EEFS_LibHealthCheckStep(&V_Vol, 10), EEFS_INVALID_ARGUMENT);  /* none started */
    CHECK_RC(EEFS_LibHealthCheckStart(NULL, &H_Rep), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibHealthCheckStart(&V_Vol, NULL), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibHealthCheckSummary(&V_Vol, NULL), EEFS_INVALID_ARGUMENT);
    memset(Unmounted, 0, sizeof(*Unmounted));
    CHECK_RC(EEFS_LibHealthCheckStart(Unmounted, &H_Rep), EEFS_INVALID_ARGUMENT);

    CHECK_RC(EEFS_LibHealthCheckStart(&V_Vol, &H_Rep), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibHealthCheckStart(&V_Vol, &Other), EEFS_DEVICE_IS_BUSY);
    CHECK_RC(EEFS_LibHealthCheckStep(&V_Vol, 0), 1);                       /* 0 counts as 1 */
    CHECK_EQ(H_Rep.S.PagesRead, 1);
    CHECK_EQ(H_Rep.S.Complete, 0);
    CHECK(H_Rep.S.Summary & EEFS_HEALTH_INCOMPLETE);
    CHECK_RC(EEFS_LibFreeFS(&V_Vol), EEFS_DEVICE_IS_BUSY);
    CHECK_RC(EEFS_LibRebuildDirectory(&V_Vol), EEFS_DEVICE_IS_BUSY);
    CHECK_RC(EEFS_LibHealthCheckCancel(&V_Vol), EEFS_SUCCESS);
    CHECK(V_Vol.ActiveReport == NULL);
    CHECK_RC(EEFS_LibHealthCheckStep(&V_Vol, 10), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibHealthCheckSummary(&V_Vol, &H_Sum), EEFS_ERROR);      /* a cancelled check leaves no report */
    CHECK_RC(EEFS_LibHealthCheckCancel(&V_Vol), EEFS_SUCCESS);             /* nothing attached: fine */

    S = H_Check(100);
    CHECK_EQ(S->FindingCount, 0);
    CHECK_RC(EEFS_LibHealthCheckStep(&V_Vol, 10), EEFS_INVALID_ARGUMENT);  /* done: detached */

    /* a later cancelled check keeps the last completed report */
    CHECK_RC(V_Write(&H_CB), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibHealthCheckStart(&V_Vol, &Other), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibHealthCheckStep(&V_Vol, 5), 1);
    CHECK_RC(EEFS_LibHealthCheckCancel(&V_Vol), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibHealthCheckSummary(&V_Vol, &H_Sum2), EEFS_SUCCESS);
    CHECK(H_SameReport(&H_Sum2, S, 0));
    CHECK_EQ(H_Sum2.Steps, S->Steps);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibFreeFS(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    CHECK_RC(EEFS_LibHealthCheckSummary(&V_Vol, &H_Sum2), EEFS_ERROR);     /* a new mount has no report yet */
    V_Destroy();
}

static void health_findings_overflow(void)
{
    static const char  *Names = "/orphan-%02lu";
    static char         Path[40][16];
    const EEFS_HealthSummary_t *S;
    V_File_t            F;
    uint32              i;

    H_NewVolume();
    for (i = 0; i < 40; i++) {
        sprintf(Path[i], Names, (unsigned long)i);
        F = H_File(Path[i], 100 + i, 140 + i);
        CHECK_RC(V_Write(&F), EEFS_SUCCESS);
    }
    H_Reboot();
    S = H_Check(50);
    CHECK_EQ(S->FindingCount, EEFS_NAND_MAX_HEALTH_FINDINGS);
    CHECK_EQ(S->Overflow, 40 - EEFS_NAND_MAX_HEALTH_FINDINGS);
    CHECK_EQ(H_Count(S, EEFS_HEALTH_ORPHAN_FILE), EEFS_NAND_MAX_HEALTH_FINDINGS);
    CHECK_EQ(S->Summary, EEFS_HEALTH_DEGRADED | EEFS_HEALTH_DIR_ISSUE | EEFS_HEALTH_OVERFLOW);
    CHECK_RC(EEFS_LibReclaimOrphans(&V_Vol), 40);
    S = H_Check(50);
    CHECK_EQ(S->FindingCount, 0);
    CHECK_EQ(S->Overflow, 0);
    CHECK_EQ(S->Summary, EEFS_HEALTH_OK);
    V_Destroy();
}

/*
 * After a power cut
 */

/* The batch of test_powercut.c */
static V_File_t     H_R1, H_R2, H_N, H_M, H_M2, H_K, H_New[5];

static int32 H_RunBatch(void)
{
    int32   Status;
    uint32  i;

    for (i = 0; i < 5; i++) {
        if ((Status = V_Write(&H_New[i])) != EEFS_SUCCESS) return(Status);
        if (i == 1) {
            if ((Status = EEFS_LibRemove(&V_Vol, "/r1")) != EEFS_SUCCESS) return(Status);
        }
        if (i == 2) {
            if ((Status = EEFS_LibRename(&V_Vol, "/n", "/n2")) != EEFS_SUCCESS) return(Status);
            if ((Status = EEFS_LibRemove(&V_Vol, "/r2")) != EEFS_SUCCESS) return(Status);
        }
        if (i == 3) {
            if ((Status = V_Write(&H_M2)) != EEFS_SUCCESS) return(Status);
        }
    }
    return(EEFS_LibCommit(&V_Vol));
}

static void H_BatchImage(void)
{
    static const char  *Paths[5] = { "/new/0", "/new/1", "/new/2", "/new/3", "/new/4" };
    uint32              Sizes[5];
    uint32              i;

    Sizes[0] = 100;
    Sizes[1] = NandTest_Geo.PageSize + 1;
    Sizes[2] = V_D0() + 5;
    Sizes[3] = 300;
    Sizes[4] = 0;
    H_R1 = H_File("/r1", V_SizeForBlocks(2), 331);
    H_R2 = H_File("/r2", 5000, 332);
    H_N = H_File("/n", V_SizeForBlocks(2) + 10, 333);
    H_M = H_File("/m", V_SizeForBlocks(4), 334);
    H_M2 = H_Mod(H_M, V_FileOffset(1, 3));
    H_K = H_File("/k", 2000, 335);
    for (i = 0; i < 5; i++) {
        H_New[i] = H_File(Paths[i], Sizes[i], 340 + i);
    }
    H_NewVolume();
    CHECK_RC(V_Write(&H_R1), EEFS_SUCCESS);
    CHECK_RC(V_Write(&H_R2), EEFS_SUCCESS);
    CHECK_RC(V_Write(&H_N), EEFS_SUCCESS);
    CHECK_RC(V_Write(&H_M), EEFS_SUCCESS);
    CHECK_RC(V_Write(&H_K), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    H_Image = V_Sim;
    V_Sim = NULL;
    memset(&V_Vol, 0, sizeof(V_Vol));
}

/* What the check must report on the volume as it is, derived from the raw media and the directory: orphans (a
 * valid metadata page that no slot claims and that is not on the cleanup list, which must be a block of the batch:
 * free in the image), the cleanup still pending from the media, unrecorded bad markers (left by torn erases), a
 * lagging mirror, and a torn tail for exactly the mirrors whose scan says so.  Nothing else.  Returns the number of
 * orphans; *Seen accumulates the codes seen (bit per code). */
static uint32 H_CheckDerived(const EEFS_HealthSummary_t *S, const V_Snap_t *Image, uint32 *Seen)
{
    const EEFS_NandGeometry_t  *Geo = &V_Dev.Geometry;
    static uint8                Claimed[EEFS_NAND_MAX_BLOCKS];
    EEFS_NandChain_t            C[2];
    EEFS_NandPage0_t            P;
    const EEFS_NandEntry_t     *E;
    uint32                      Orphans = 0;
    uint32                      Tails[2] = { 0, 0 };
    uint32                      Expected;
    uint32                      b;
    uint32                      i;
    uint32                      s;
    uint32                      k;
    uint32                      n;
    uint32                      Mp;
    uint32                      Page;
    int                         Marked;
    int                         Listed;

    H_ExpReset();
    memset(Claimed, 0, sizeof(Claimed));
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        E = &V_Vol.Working.Entry[i];
        for (s = 0; s < 2; s++) {
            if (!E->InUse || !E->Slot[s].InUse) continue;
            n = V_RawBlocks(E->Slot[s].FirstBlock, H_Blk, H_Crc);
            CHECK(n > 0);
            for (k = 0; k < n; k++) Claimed[H_Blk[k]] = 1;
        }
    }
    for (b = 0; b < Geo->BlockCount; b++) {
        if (V_Vol.Working.Map[b] == EEFS_NAND_MAP_BAD) continue;
        Marked = 0;
        for (Mp = 0; Mp < 3; Mp++) {
            if ((Geo->BadMarkerPages & (1u << Mp)) == 0) continue;
            Page = (Mp == 0) ? 0 : ((Mp == 1) ? 1 : (Geo->PagesPerBlock - 1));
            NandSim_RawRead(V_Sim, b, Page, NULL, H_Oob);
            if (H_Oob[Geo->BadMarkerOffset] != 0xFF) Marked = 1;
        }
        if (Marked) {
            H_Expect(EEFS_HEALTH_BADBLOCK_UNRECORDED, 0xFF, 0xFFFF, b, 0, 0, "");
            continue;
        }
        if ((b < Geo->DirRegionBlocks) || Claimed[b]) continue;
        Listed = 0;
        for (i = 0; i < V_Vol.PendingCleanupCount; i++) {
            if (V_Vol.PendingCleanup[i] == b) Listed = 1;
        }
        if (Listed) continue;
        NandSim_RawRead(V_Sim, b, 0, H_PageBuf, NULL);
        if (EEFS_NandDecodePage0(H_PageBuf, Geo->PageSize, Geo->PagesPerBlock, b, &P) != EEFS_SUCCESS) continue;
        for (k = 0; k < P.BlockCount; k++) {
            EEFS_NandPage0Block(H_PageBuf, k, &H_Blk[0], &H_Crc[0]);
            if ((H_Blk[0] < Geo->DirRegionBlocks) || (H_Blk[0] >= Geo->BlockCount)) break;
        }
        if (k < P.BlockCount) continue;
        H_Expect(EEFS_HEALTH_ORPHAN_FILE, P.Slot, 0xFFFF, b, P.WriteSeq, 0, P.Path);
        CHECK_EQ(Image->Map[b], EEFS_NAND_MAP_FREE);        /* only a block of the batch can be an orphan */
        Orphans++;
    }
    for (i = 0; i < V_Vol.PendingCleanupBase; i++) {
        b = V_Vol.PendingCleanup[i];
        NandSim_RawRead(V_Sim, b, 0, H_PageBuf, NULL);
        if (!EEFS_NandIsErased(H_PageBuf, Geo->PageSize)) {
            H_Expect(EEFS_HEALTH_DIR_CLEANUP_PENDING, 0xFF, 0xFFFF, b, 0, 0, "");
        }
    }
    CHECK_RC(EEFS_NandScanMirror(&V_Vol, 0, &C[0]), EEFS_SUCCESS);
    CHECK_RC(EEFS_NandScanMirror(&V_Vol, 1, &C[1]), EEFS_SUCCESS);
    CHECK(C[0].Valid && C[1].Valid);
    if (C[0].Valid && C[1].Valid && (C[0].LastSeq != C[1].LastSeq)) {
        k = (C[0].LastSeq < C[1].LastSeq) ? 0 : 1;
        H_Expect(EEFS_HEALTH_DIR_MIRROR_MISMATCH, k, 0xFFFF, 0xFFFF, C[k].LastSeq, C[1 - k].LastSeq, "");
    }

    /* the torn tails: one or more findings 3/4 for a mirror iff its scan says Torn */
    for (i = 0; i < S->FindingCount; i++) {
        if (((S->Finding[i].Code == EEFS_HEALTH_DIR_RECORD_CORRUPT) ||
             (S->Finding[i].Code == EEFS_HEALTH_DIR_UNCOMMITTED_TXN)) && (S->Finding[i].Slot < 2)) {
            Tails[S->Finding[i].Slot]++;
            H_Expect(S->Finding[i].Code, S->Finding[i].Slot, 0xFFFF, H_ANY, H_ANY, H_ANY, "");
        }
    }
    CHECK_EQ(Tails[0] > 0, C[0].Torn != 0);
    CHECK_EQ(Tails[1] > 0, C[1].Torn != 0);
    H_ExpectExactly(S);

    Expected = 0;
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        if (V_Vol.Working.Entry[i].InUse) Expected++;
    }
    CHECK_EQ(S->FilesChecked, Expected);
    for (i = 0; i < S->FindingCount; i++) {
        if (S->Finding[i].Code < 32) *Seen |= (1UL << S->Finding[i].Code);
        else if (S->Finding[i].Code == EEFS_HEALTH_ORPHAN_FILE) *Seen |= (1UL << 31);
        else if (S->Finding[i].Code == EEFS_HEALTH_BADBLOCK_UNRECORDED) *Seen |= (1UL << 30);
    }
    return(Orphans);
}

static void health_powercut_sweep_never_crashes_and_matches_expectation(void)
{
    V_File_t    Follow = H_File("/follow", 3000, 999);
    uint32      Cut;
    uint32      Cuts = 0;
    uint32      Orphans;
    uint32      Seen = 0;
    int         Mode;
    int         Done;
    int32       Status;
    const EEFS_HealthSummary_t *S;

    H_BatchImage();
    V_Attach(NandSim_Clone(H_Image));
    CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);
    V_Take(&V_Vol, &H_SnapPre);
    CHECK_RC(H_RunBatch(), EEFS_SUCCESS);
    V_Take(&V_Vol, &H_SnapPost);
    V_Destroy();

    for (Mode = NANDSIM_TORN_NONE; (Mode <= NANDSIM_TORN_GARBAGE) && !NandTest_CaseFailed(); Mode++) {
        for (Cut = 0; ; Cut++) {
            V_Attach(NandSim_Clone(H_Image));
            CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);
            NandSim_SetPowerBudget(V_Sim, Cut, Mode);
            Status = H_RunBatch();
            Done = !NandSim_PowerCutHappened(V_Sim);
            if (Done) CHECK_RC(Status, EEFS_SUCCESS);
            NandSim_Reboot(V_Sim);
            EEFS_LibInit();
            CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);
            V_Take(&V_Vol, &H_SnapCur);
            CHECK(V_Same(&H_SnapCur, &H_SnapPre) || V_Same(&H_SnapCur, &H_SnapPost));

            /* the check after the remount: completes, exactly the derived findings */
            S = H_Check(7);
            Orphans = H_CheckDerived(S, &H_SnapPre, &Seen);
            CHECK_EQ(H_Count(S, EEFS_HEALTH_FILE_CORRUPT) + H_Count(S, EEFS_HEALTH_BLOCK_CROSSLINKED) +
                     H_Count(S, EEFS_HEALTH_MAP_MISMATCH) + H_Count(S, EEFS_HEALTH_DIR_PAGE0_MISMATCH), 0);

            /* ReclaimOrphans takes exactly the orphans; then a follow-up commit: only torn-erase markers remain */
            CHECK_RC(EEFS_LibReclaimOrphans(&V_Vol), (int32)Orphans);
            CHECK_RC(V_Write(&Follow), EEFS_SUCCESS);
            CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
            S = H_Check(1000);
            (void)H_CheckDerived(S, &H_SnapPre, &Seen);
            CHECK_EQ(S->FindingCount, H_Count(S, EEFS_HEALTH_BADBLOCK_UNRECORDED));
            CHECK_EQ(NandSim_Violations(V_Sim), 0);

            if (NandTest_CaseFailed()) {
                printf("    failure at cut %lu, torn mode %s\n", (unsigned long)Cut, H_ModeName[Mode]);
                V_Destroy();
                break;
            }
            V_Destroy();
            if (Done) {
                Cuts = Cut;
                break;
            }
        }
    }
    printf("    sweep batch: %lu cut points x 3 torn modes, codes seen 0x%lx\n", (unsigned long)Cuts,
           (unsigned long)Seen);
    /* the sweep produced each kind of finding it is meant to produce */
    CHECK(Seen & (1UL << 31));                                  /* orphans */
    CHECK(Seen & (1UL << EEFS_HEALTH_DIR_CLEANUP_PENDING));
    CHECK(Seen & (1UL << EEFS_HEALTH_DIR_MIRROR_MISMATCH));
    CHECK(Seen & ((1UL << EEFS_HEALTH_DIR_UNCOMMITTED_TXN) | (1UL << EEFS_HEALTH_DIR_RECORD_CORRUPT)));
    NandSim_Destroy(H_Image);
    H_Image = NULL;
}

/* ReclaimOrphans cut at every erase: the directory and every file are untouched, and a second run finishes the job */
static void reclaim_orphans_powercut_sweep_no_damage(void)
{
    V_File_t    O[3];
    uint32      Cut;
    uint32      Cuts = 0;
    int         Mode;
    int         Done;
    uint32      i;
    const EEFS_HealthSummary_t *S;

    H_NewVolume();
    H_MakeClean();
    O[0] = H_File("/o0", V_SizeForBlocks(2), 151);
    O[1] = H_File("/o1", 10, 152);
    O[2] = H_File("/o2", V_D0() + 1, 153);
    for (i = 0; i < 3; i++) CHECK_RC(V_Write(&O[i]), EEFS_SUCCESS);
    H_Reboot();
    V_Take(&V_Vol, &H_SnapPre);
    CHECK_EQ(H_Count(H_Check(100), EEFS_HEALTH_ORPHAN_FILE), 3);
    H_Image = V_Sim;
    V_Sim = NULL;
    memset(&V_Vol, 0, sizeof(V_Vol));

    for (Mode = NANDSIM_TORN_NONE; (Mode <= NANDSIM_TORN_GARBAGE) && !NandTest_CaseFailed(); Mode++) {
        for (Cut = 0; ; Cut++) {
            V_Attach(NandSim_Clone(H_Image));
            CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);
            NandSim_SetPowerBudget(V_Sim, Cut, Mode);
            (void)EEFS_LibReclaimOrphans(&V_Vol);
            Done = !NandSim_PowerCutHappened(V_Sim);
            NandSim_Reboot(V_Sim);
            EEFS_LibInit();
            CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);
            V_Take(&V_Vol, &H_SnapCur);
            CHECK(V_Same(&H_SnapCur, &H_SnapPre));
            V_CheckSlotsValid(&V_Vol);
            V_CheckFile(&V_Vol, &H_CA);
            V_CheckFile(&V_Vol, &H_CB);
            S = H_Check(9);
            CHECK_EQ(S->FindingCount, H_Count(S, EEFS_HEALTH_ORPHAN_FILE) + H_Count(S, EEFS_HEALTH_BADBLOCK_UNRECORDED));
            CHECK(H_Count(S, EEFS_HEALTH_ORPHAN_FILE) <= 3);
            CHECK_RC(EEFS_LibReclaimOrphans(&V_Vol), (int32)H_Count(S, EEFS_HEALTH_ORPHAN_FILE));
            S = H_Check(9);
            CHECK_EQ(S->FindingCount, H_Count(S, EEFS_HEALTH_BADBLOCK_UNRECORDED));
            CHECK_EQ(NandSim_Violations(V_Sim), 0);
            if (NandTest_CaseFailed()) {
                printf("    failure at cut %lu, torn mode %s\n", (unsigned long)Cut, H_ModeName[Mode]);
                V_Destroy();
                break;
            }
            V_Destroy();
            if (Done) {
                Cuts = Cut;
                break;
            }
        }
    }
    printf("    sweep reclaim: %lu cut points x 3 torn modes\n", (unsigned long)Cuts);
    CHECK_EQ(Cuts, 3);
    NandSim_Destroy(H_Image);
    H_Image = NULL;
}

/*
 * Case table
 */

const NandTest_Case_t NandTest_HealthCases[] = {
    { "health_clean_volume_ok",                                 health_clean_volume_ok },
    { "health_dir_mirror_missing",                              health_dir_mirror_missing },
    { "health_dir_mirror_mismatch",                             health_dir_mirror_mismatch },
    { "health_dir_record_corrupt",                              health_dir_record_corrupt },
    { "health_dir_uncommitted_txn",                             health_dir_uncommitted_txn },
    { "health_dir_cleanup_pending",                             health_dir_cleanup_pending },
    { "health_cow_superseded_page0_not_orphan_or_crosslink",    health_cow_superseded_page0_not_orphan_or_crosslink },
    { "health_dir_rebuilt_not_persisted",                       health_dir_rebuilt_not_persisted },
    { "health_file_corrupt_page0",                              health_file_corrupt_page0 },
    { "health_file_corrupt_block_crc",                          health_file_corrupt_block_crc },
    { "health_file_corrupt_file_crc",                           health_file_corrupt_file_crc },
    { "health_file_io_error",                                   health_file_io_error },
    { "health_block_needs_scrub",                               health_block_needs_scrub },
    { "health_ab_a_corrupt",                                    health_ab_a_corrupt },
    { "health_ab_b_corrupt_and_active_corrupt_sets_flag",       health_ab_b_corrupt_and_active_corrupt_sets_flag },
    { "health_ab_active_missing",                               health_ab_active_missing },
    { "health_ab_version_mismatch",                             health_ab_version_mismatch },
    { "health_ab_fallback_unavailable",                         health_ab_fallback_unavailable },
    { "health_ab_failed_flag_set",                              health_ab_failed_flag_set },
    { "health_ab_load_mixed",                                   health_ab_load_mixed },
    { "health_dir_page0_mismatch",                              health_dir_page0_mismatch },
    { "health_map_mismatch",                                    health_map_mismatch },
    { "health_block_crosslinked",                               health_block_crosslinked },
    { "health_badblock_unrecorded",                             health_badblock_unrecorded },
    { "health_orphan_file",                                     health_orphan_file },
    { "health_open_batch_no_false_orphans",                     health_open_batch_no_false_orphans },
    { "health_free_below_reserve",                              health_free_below_reserve },
    { "health_maxpagereads_1_equals_one_big_step",              health_maxpagereads_1_equals_one_big_step },
    { "health_restart_on_change_during_check",                  health_restart_on_change_during_check },
    { "health_cancel_and_summary",                              health_cancel_and_summary },
    { "health_findings_overflow",                               health_findings_overflow },
    { "health_powercut_sweep_never_crashes_and_matches_expectation",
                                                                health_powercut_sweep_never_crashes_and_matches_expectation },
    { "reclaim_orphans_erases_only_unreferenced",               reclaim_orphans_erases_only_unreferenced },
    { "reclaim_orphans_powercut_sweep_no_damage",               reclaim_orphans_powercut_sweep_no_damage },
    { NULL, NULL }
};

/************************/
/*  End of File Comment */
/************************/
