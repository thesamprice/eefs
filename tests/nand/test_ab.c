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
 * Filename: test_ab.c
 *
 * Purpose: Suite "ab": A/B software loads (docs/nand-implementation.md section 7, phase 5): the upload, activate,
 *   test, promote and rollback workflow; name resolution with and without "@A"/"@B"; the compatible fallback of Open
 *   on a failed active copy (corrupt metadata page, data CRC, uncorrectable ECC) and the error without it;
 *   ActiveFailed (set by a failed validation or SetFailed, persisted by a commit, cleared by a rewrite or a copy into
 *   the active slot or a slot switch); single-slot Remove; CopySlot(+All) into the other slot's own blocks with the
 *   NEW reserve class; the bootloader library (microeefs_nand) resolving every state the same way as the library;
 *   and power-cut sweeps of the upload/activate commit, the CopySlotAll promote and the rollback.
 *
 * Design Notes:
 *   A copy is described by a V_File_t whose Path is the name it is read under ("/f@A", "/f@B" or "/f"), so the
 *   nand_vol helpers check it (Stat, Open/Read byte for byte, VerifyFile).  The data-corruption cases are compiled
 *   for the build's EEFS_NAND_VERIFY_ON_OPEN: with it on (eefs_nand_test_verifyopen, nand_verify_on_open_ab_4096)
 *   Open falls back like the bootloader; with the default (off) Open checks page 0 only, opens the active copy, and
 *   VerifyFile is what detects the corruption and sets ActiveFailed.
 *
 *   The sweeps follow test_powercut.c: one uninterrupted run gives the "after" state, then one run per cut point
 *   (every program and erase, the commit included) and torn mode on a clone of the same image.  After each cut:
 *   remount without programs or erases; exactly the "before" or the "after" directory (monotonic in the cut point);
 *   every copy of that state reads back exactly under its "@A"/"@B" name; the active load of every file boots
 *   through microeefs_nand (the expected slot and content, no fallback, ActiveFailed 0); no metadata page lists a bad
 *   data block; then a follow-up write and commit, the cleanup, both mirrors and the copies again.
 *
 */

/*
 * Includes
 */

#include "nand_vol.h"
#include "microeefs_nand.h"

/*
 * Macro Definitions
 */

#define A_MAX_COPIES        8
#define A_MAX_BOOT          4
#define A_MAX_CLEANUP       16
#define A_MAX_BLOCKS        512

/*
 * Type Definitions
 */

typedef int32 (*A_Run_t)(void);

/* One expected directory state: every copy under its "@A"/"@B" name, and the load each file boots */
typedef struct
{
    V_File_t    Copy[A_MAX_COPIES];
    uint32      CopyCount;
    V_File_t    Boot[A_MAX_BOOT];       /* Path = the name without suffix */
    uint8       BootSlot[A_MAX_BOOT];
    uint32      BootCount;
} A_State_t;

/*
 * Local Data
 */

static uint8        A_PageBuf[EEFS_NAND_MAX_PAGE_SIZE];
static uint16       A_BlocksA[A_MAX_BLOCKS];
static uint16       A_BlocksB[A_MAX_BLOCKS];
static uint32       A_Crcs[A_MAX_BLOCKS];

static A_State_t    A_Pre;
static A_State_t    A_Post;
static V_Snap_t     A_SnapPre;
static V_Snap_t     A_SnapPost;
static V_Snap_t     A_SnapCur;
static uint16       A_Cleanup[A_MAX_CLEANUP];
static uint32       A_CleanupCount;
static NandSim_t   *A_Image;

static const char  *A_ModeName[3] = { "NONE", "PARTIAL", "GARBAGE" };

/* The software load of the workflow and the sweeps: three files, v1 and v2 */
static const char  *A_Names[3] = { "/boot/kernel.fit", "/cf/apps/app.so", "/cf/lib.so" };
static const char  *A_NamesA[3] = { "/boot/kernel.fit@A", "/cf/apps/app.so@A", "/cf/lib.so@A" };
static const char  *A_NamesB[3] = { "/boot/kernel.fit@B", "/cf/apps/app.so@B", "/cf/lib.so@B" };
static V_File_t     A_V1[3];
static V_File_t     A_V2[3];

/*
 * Helpers
 */

static V_File_t A_File(const char *Path, uint32 Size, uint32 Seed)
{
    V_File_t    F;

    memset(&F, 0, sizeof(F));
    F.Path = Path;
    F.Size = Size;
    F.Seed = Seed;
    return(F);
}

/* The same content under another name */
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

static EEFS_NandEntry_t *A_E(const char *Path)
{
    static EEFS_NandEntry_t None;
    int32                   i = EEFS_NandFindEntry(&V_Vol, Path);

    if (i < 0) {
        memset(&None, 0, sizeof(None));
        return(&None);
    }
    return(&V_Vol.Working.Entry[i]);
}

/* The directory entry: VerA / VerB = 0 means that slot is not in use */
static void A_CheckEntry(const char *Path, uint8 IsAB, uint8 Active, uint8 Compat, uint8 Failed, uint32 VerA, uint32 VerB)
{
    const EEFS_NandEntry_t *E = A_E(Path);

    CHECK_EQ(E->InUse, 1);
    CHECK_EQ(E->IsAB, IsAB);
    CHECK_EQ(E->ActiveSlot, Active);
    CHECK_EQ(E->FallbackCompatible, Compat);
    CHECK_EQ(E->ActiveFailed, Failed);
    CHECK_EQ(E->Slot[0].InUse, (VerA != 0) ? 1 : 0);
    CHECK_EQ(E->Slot[1].InUse, (VerB != 0) ? 1 : 0);
    if (VerA != 0) CHECK_EQ(E->Slot[0].Version, VerA);
    if (VerB != 0) CHECK_EQ(E->Slot[1].Version, VerB);
}

/* Opens Name, checks the slot opened, the fallback flag and the exact content */
static void A_CheckOpen(const char *Name, uint8 Slot, uint8 Fallback, const V_File_t *F)
{
    EEFS_FileDescriptor_t  *Fd;
    int32                   d;
    int32                   n;
    uint32                  Total = 0;

    d = EEFS_LibOpen(&V_Vol, (char *)Name, O_RDONLY, 0);
    CHECK(d >= 0);
    if (d < 0) {
        printf("    open %s: %ld\n", Name, (long)d);
        return;
    }
    Fd = EEFS_LibFileDescriptor2Pointer(d);
    CHECK(Fd != NULL);
    if (Fd != NULL) {
        CHECK_EQ(Fd->Slot, Slot);
        CHECK_EQ(Fd->Fallback, Fallback);
    }
    while ((n = EEFS_LibRead(d, &V_Read[Total], 65537)) > 0) {
        Total += (uint32)n;
    }
    CHECK(n == 0);
    CHECK_EQ(Total, F->Size);
    if (Total == F->Size) {
        CHECK(memcmp(V_Read, V_Content(F), F->Size) == 0);
    }
    CHECK_RC(EEFS_LibClose(d), EEFS_SUCCESS);
}

/* The bootloader library loads Name: expected return code, slot, content (F, when Rc is success) and ActiveFailed.
 * It never programs or erases. */
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
        CHECK_EQ(Info.UsedScan, 0);
    }
    CHECK_EQ(NandSim_Programs(V_Sim), Programs);
    CHECK_EQ(NandSim_Erases(V_Sim), Erases);
}

/* Blocks[] of a slot, read raw from its metadata page; returns the count */
static uint32 A_RawBlocks(const char *Path, uint8 Slot, uint16 *Blocks)
{
    const EEFS_NandEntry_t *E = A_E(Path);

    if (E->Slot[Slot].InUse == 0) return(0);
    return(V_RawBlocks(E->Slot[Slot].FirstBlock, Blocks, A_Crcs));
}

/* A and B of Path share no block, and each slot owns exactly its own blocks in the map */
static void A_CheckDisjoint(const char *Path)
{
    int32   i = EEFS_NandFindEntry(&V_Vol, Path);
    uint32  na = A_RawBlocks(Path, 0, A_BlocksA);
    uint32  nb = A_RawBlocks(Path, 1, A_BlocksB);
    uint32  j;
    uint32  k;

    CHECK(i >= 0);
    if (i < 0) return;
    CHECK(na > 0);
    CHECK(nb > 0);
    for (j = 0; j < na; j++) {
        CHECK_EQ(V_Vol.Working.Map[A_BlocksA[j]], EEFS_NAND_MAP_OWNER(i, 0));
        for (k = 0; k < nb; k++) {
            CHECK(A_BlocksA[j] != A_BlocksB[k]);
        }
    }
    for (k = 0; k < nb; k++) {
        CHECK_EQ(V_Vol.Working.Map[A_BlocksB[k]], EEFS_NAND_MAP_OWNER(i, 1));
    }
}

static uint32 A_Free(void)
{
    EEFS_NandVolumeInfo_t   Info;

    CHECK_RC(EEFS_LibGetVolumeInfo(&V_Vol, &Info), EEFS_SUCCESS);
    return(Info.FreeBlocks);
}

/* Media faults on a copy (the volume is remounted afterwards by the caller, so no cached page hides them) */
static void A_CorruptPage0(const char *Path, uint8 Slot)
{
    NandSim_CorruptBytes(V_Sim, A_E(Path)->Slot[Slot].FirstBlock, 0, 20, 4);       /* inside Path: header CRC fails */
}

static void A_CorruptData(const char *Path, uint8 Slot)
{
    NandSim_CorruptBytes(V_Sim, A_E(Path)->Slot[Slot].FirstBlock, 1, 3, 1);        /* first data page */
}

static void A_InjectEcc(const char *Path, uint8 Slot, uint32 Page)
{
    NandSim_InjectRead(V_Sim, A_E(Path)->Slot[Slot].FirstBlock, Page, EEFS_NAND_READ_UNCORRECTABLE, -1);
}

/* "/f": A v1 and B v2, B active, FallbackCompatible as given, committed */
static V_File_t A_F1, A_F2, A_F1a, A_F2b;

static void A_MakeF(uint8 Compat)
{
    A_F1 = A_File("/f", V_D0() + NandTest_Geo.PageSize + 33, 701);      /* two blocks */
    A_F2 = A_File("/f", 5 * NandTest_Geo.PageSize + 9, 702);
    A_F1a = A_As(A_F1, "/f@A");
    A_F2b = A_As(A_F2, "/f@B");
    CHECK_RC(A_Put("/f", &A_F1, EEFS_NAND_SLOT_A, 1), EEFS_SUCCESS);
    CHECK_RC(A_Put("/f", &A_F2, EEFS_NAND_SLOT_B, 2), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibSetActiveSlot(&V_Vol, "/f", EEFS_NAND_SLOT_B), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibSetFallbackCompatible(&V_Vol, "/f", Compat), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    A_CheckEntry("/f", 1, EEFS_NAND_SLOT_B, Compat, 0, 1, 2);
}

/* Uploads v2 of the load into B, activates it and makes v1 the compatible fallback (no commit) */
static int32 A_UploadActivate(void)
{
    int32   Status;
    uint32  i;

    for (i = 0; i < 3; i++) {
        if ((Status = A_Put(A_Names[i], &A_V2[i], EEFS_NAND_SLOT_B, 2)) != EEFS_SUCCESS) return(Status);
    }
    if ((Status = EEFS_LibSetActiveSlotAll(&V_Vol, EEFS_NAND_SLOT_B)) != 3) return((Status < 0) ? Status : EEFS_ERROR);
    if ((Status = EEFS_LibSetFallbackCompatibleAll(&V_Vol, 1)) != 3) return((Status < 0) ? Status : EEFS_ERROR);
    return(EEFS_SUCCESS);
}

/* The load's v1 on a fresh volume, committed: two files written without a slot (single copy), one into "@A" */
static void A_LoadV1(void)
{
    uint32  i;

    A_V1[0] = A_File(A_Names[0], V_D0() + NandTest_Geo.PageSize + 5, 801);   /* two blocks, two pages in block 1 */
    A_V1[1] = A_File(A_Names[1], 3 * NandTest_Geo.PageSize + 1, 802);
    A_V1[2] = A_File(A_Names[2], 200, 803);
    A_V2[0] = A_File(A_Names[0], V_D0() + NandTest_Geo.PageSize + 77, 811);
    A_V2[1] = A_File(A_Names[1], 2 * NandTest_Geo.PageSize + 50, 812);
    A_V2[2] = A_File(A_Names[2], 999, 813);
    for (i = 0; i < 3; i++) {
        CHECK_RC(A_Put((i == 2) ? A_NamesA[i] : A_Names[i], &A_V1[i], EEFS_NAND_SLOT_NONE, 1), EEFS_SUCCESS);
    }
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
}

/*
 * The workflow
 */

/* Upload v2 into B, activate B with v1 as the compatible fallback, one commit; GetFileInfo shows both slots */
static void A_StepUpload(void)
{
    EEFS_NandFileInfo_t Info;
    uint32              Seq;
    uint32              DirPrograms;
    uint32              i;

    A_LoadV1();
    A_CheckEntry(A_Names[0], 0, EEFS_NAND_SLOT_A, 0, 0, 1, 0);
    A_CheckEntry(A_Names[1], 0, EEFS_NAND_SLOT_A, 0, 0, 1, 0);
    A_CheckEntry(A_Names[2], 1, EEFS_NAND_SLOT_A, 0, 0, 1, 0);     /* written with an explicit "@A" */

    Seq = V_Vol.LastSeq;
    CHECK_RC(A_UploadActivate(), EEFS_SUCCESS);
    for (i = 0; i < 3; i++) {
        A_CheckEntry(A_Names[i], 1, EEFS_NAND_SLOT_B, 1, 0, 1, 2);
        A_CheckBoot(A_Names[i], EEFS_SUCCESS, EEFS_NAND_SLOT_A, &A_V1[i], 0);  /* nothing committed yet */
        A_CheckBoot(A_NamesB[i], EEFS_FILE_NOT_FOUND, 0, NULL, 0);
    }
    CHECK_EQ(V_Vol.LastSeq, Seq);
    DirPrograms = NandSim_Programs(V_Sim);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.LastSeq, Seq + 1);                       /* one record ... */
    DirPrograms = NandSim_Programs(V_Sim) - DirPrograms;
    CHECK((DirPrograms % 2) == 0);                          /* ... of the same size in each mirror */
    V_CheckMirrorsAgree(&V_Vol);

    V_Remount();
    for (i = 0; i < 3; i++) {
        A_CheckEntry(A_Names[i], 1, EEFS_NAND_SLOT_B, 1, 0, 1, 2);
        A_CheckDisjoint(A_Names[i]);
        CHECK_RC(EEFS_LibGetFileInfo(&V_Vol, (char *)A_Names[i], &Info), EEFS_SUCCESS);
        CHECK_EQ(Info.Slot, EEFS_NAND_SLOT_B);
        CHECK_EQ(Info.IsAB, 1);
        CHECK_EQ(Info.ActiveSlot, EEFS_NAND_SLOT_B);
        CHECK_EQ(Info.FallbackCompatible, 1);
        CHECK_EQ(Info.Version, 2);
        CHECK_EQ(Info.FileSize, A_V2[i].Size);
        CHECK_EQ(Info.SlotInfo[0].InUse, 1);
        CHECK_EQ(Info.SlotInfo[0].Version, 1);
        CHECK_EQ(Info.SlotInfo[0].FileSize, A_V1[i].Size);
        CHECK_EQ(Info.SlotInfo[1].InUse, 1);
        CHECK_EQ(Info.SlotInfo[1].Version, 2);
        CHECK_EQ(Info.SlotInfo[1].FileSize, A_V2[i].Size);
        A_CheckOpen(A_Names[i], EEFS_NAND_SLOT_B, 0, &A_V2[i]);
        A_CheckBoot(A_Names[i], EEFS_SUCCESS, EEFS_NAND_SLOT_B, &A_V2[i], 0);
        A_CheckBoot(A_NamesA[i], EEFS_SUCCESS, EEFS_NAND_SLOT_A, &A_V1[i], 0);
    }
}

static void ab_upload_activate_compatible_one_commit(void)
{
    V_NewVolume();
    A_StepUpload();
    V_Destroy();
}

/* The whole workflow: upload, activate, test B, roll back, activate again, promote B into A, with the directory checked
 * after every step */
static void ab_workflow_upload_activate_promote_rollback(void)
{
    uint16      OldFirstA[3];
    uint32      i;
    V_File_t    F;

    V_NewVolume();
    A_StepUpload();

    /* test B: every CRC of the active load */
    for (i = 0; i < 3; i++) {
        CHECK_RC(EEFS_LibVerifyFile(&V_Vol, (char *)A_Names[i]), EEFS_SUCCESS);
    }

    /* roll back: A active again, B kept */
    CHECK_RC(EEFS_LibSetActiveSlotAll(&V_Vol, EEFS_NAND_SLOT_A), 3);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    for (i = 0; i < 3; i++) {
        A_CheckEntry(A_Names[i], 1, EEFS_NAND_SLOT_A, 1, 0, 1, 2);
        A_CheckOpen(A_Names[i], EEFS_NAND_SLOT_A, 0, &A_V1[i]);
        A_CheckBoot(A_Names[i], EEFS_SUCCESS, EEFS_NAND_SLOT_A, &A_V1[i], 0);
        A_CheckBoot(A_NamesB[i], EEFS_SUCCESS, EEFS_NAND_SLOT_B, &A_V2[i], 0);
    }

    /* activate B again, then promote: B copied into A, A active */
    CHECK_RC(EEFS_LibSetActiveSlotAll(&V_Vol, EEFS_NAND_SLOT_B), 3);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    for (i = 0; i < 3; i++) {
        OldFirstA[i] = A_E(A_Names[i])->Slot[0].FirstBlock;
    }
    CHECK_RC(EEFS_LibCopySlotAll(&V_Vol, EEFS_NAND_SLOT_B, EEFS_NAND_SLOT_A), 3);
    CHECK_RC(EEFS_LibSetActiveSlotAll(&V_Vol, EEFS_NAND_SLOT_A), 3);
    for (i = 0; i < 3; i++) {
        CHECK_EQ(V_Vol.Working.Map[OldFirstA[i]], EEFS_NAND_MAP_FREEING);
        CHECK(!NandSim_PageIsErased(V_Sim, OldFirstA[i], 0));      /* erased only after the commit */
        A_CheckBoot(A_Names[i], EEFS_SUCCESS, EEFS_NAND_SLOT_B, &A_V2[i], 0);
    }
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    for (i = 0; i < 3; i++) {
        A_CheckEntry(A_Names[i], 1, EEFS_NAND_SLOT_A, 1, 0, 2, 2);
        CHECK_EQ(A_E(A_Names[i])->Slot[0].FileSize, A_V2[i].Size);
        CHECK_EQ(A_E(A_Names[i])->Slot[0].FileCrc32, A_E(A_Names[i])->Slot[1].FileCrc32);
        CHECK(A_E(A_Names[i])->Slot[0].WriteSeq > A_E(A_Names[i])->Slot[1].WriteSeq);
        CHECK(NandSim_PageIsErased(V_Sim, OldFirstA[i], 0));
        A_CheckDisjoint(A_Names[i]);
        A_CheckOpen(A_Names[i], EEFS_NAND_SLOT_A, 0, &A_V2[i]);
        F = A_As(A_V2[i], A_NamesA[i]);
        V_CheckFile(&V_Vol, &F);
        F = A_As(A_V2[i], A_NamesB[i]);
        V_CheckFile(&V_Vol, &F);
        A_CheckBoot(A_Names[i], EEFS_SUCCESS, EEFS_NAND_SLOT_A, &A_V2[i], 0);
    }
    V_CheckMirrorsAgree(&V_Vol);
    V_CheckSlotsValid(&V_Vol);
    CHECK_EQ(NandSim_Violations(V_Sim), 0);
    V_Destroy();
}

/* Promote: A's content and version become B's, the old A first block is erased by the commit's cleanup, A active */
static void ab_promote_copyslot_b_to_a(void)
{
    uint16  OldA;
    uint32  Cleanup;

    V_NewVolume();
    A_MakeF(1);
    OldA = A_E("/f")->Slot[0].FirstBlock;
    Cleanup = V_Vol.PendingCleanupCount;
    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/f", EEFS_NAND_SLOT_B, EEFS_NAND_SLOT_A), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.PendingCleanupCount, Cleanup + 1);
    A_CheckEntry("/f", 1, EEFS_NAND_SLOT_B, 1, 0, 2, 2);
    CHECK_RC(EEFS_LibSetActiveSlot(&V_Vol, "/f", EEFS_NAND_SLOT_A), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK(NandSim_PageIsErased(V_Sim, OldA, 0));
    V_Remount();
    A_CheckEntry("/f", 1, EEFS_NAND_SLOT_A, 1, 0, 2, 2);
    A_CheckDisjoint("/f");
    A_CheckOpen("/f", EEFS_NAND_SLOT_A, 0, &A_F2);
    A_CheckBoot("/f", EEFS_SUCCESS, EEFS_NAND_SLOT_A, &A_F2, 0);
    /* the copy's metadata page names slot A */
    NandSim_RawRead(V_Sim, A_E("/f")->Slot[0].FirstBlock, 0, V_Page, NULL);
    CHECK_EQ(V_Page[273], EEFS_NAND_SLOT_A);
    V_Destroy();
}

/* Rollback: SetActiveSlotAll(A) and a commit, nothing else written */
static void ab_rollback_setactive_all_a(void)
{
    uint32  Programs;
    uint32  Erases;

    V_NewVolume();
    A_MakeF(1);
    Programs = NandSim_Programs(V_Sim);
    Erases = NandSim_Erases(V_Sim);
    CHECK_RC(EEFS_LibSetActiveSlotAll(&V_Vol, EEFS_NAND_SLOT_A), 1);
    CHECK_EQ(NandSim_Programs(V_Sim), Programs);        /* Working only until the commit */
    CHECK_EQ(NandSim_Erases(V_Sim), Erases);
    A_CheckBoot("/f", EEFS_SUCCESS, EEFS_NAND_SLOT_B, &A_F2, 0);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    A_CheckEntry("/f", 1, EEFS_NAND_SLOT_A, 1, 0, 1, 2);
    A_CheckOpen("/f", EEFS_NAND_SLOT_A, 0, &A_F1);
    A_CheckBoot("/f", EEFS_SUCCESS, EEFS_NAND_SLOT_A, &A_F1, 0);
    V_CheckFile(&V_Vol, &A_F2b);
    V_Destroy();
}

/*
 * Resolution
 */

static void ab_explicit_suffix_open_and_stat(void)
{
    EEFS_DirectoryDescriptor_t *Dir;
    EEFS_DirectoryEntry_t      *De;
    EEFS_NandFileInfo_t         Info;
    EEFS_Stat_t                 St;
    V_File_t                    Y = A_File("/y", 3000, 721);
    V_File_t                    Yb = A_As(Y, "/y@B");
    V_File_t                    Z = A_File("/z", 4000, 722);
    V_File_t                    Za = A_As(Z, "/z@A");
    int32                       d;

    V_NewVolume();
    A_MakeF(0);
    CHECK_RC(A_Put("/y@B", &Y, EEFS_NAND_SLOT_NONE, 5), EEFS_SUCCESS);       /* a file present only in B */
    CHECK_RC(A_Put("/z", &Z, EEFS_NAND_SLOT_NONE, 6), EEFS_SUCCESS);         /* a single-copy file */
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();

    /* "/f": the name is the active slot B; the suffixes address each copy */
    CHECK_RC(EEFS_LibStat(&V_Vol, "/f", &St), EEFS_SUCCESS);
    CHECK_EQ(St.FileSize, A_F2.Size);
    CHECK_RC(EEFS_LibStat(&V_Vol, "/f@A", &St), EEFS_SUCCESS);
    CHECK_EQ(St.FileSize, A_F1.Size);
    CHECK_RC(EEFS_LibStat(&V_Vol, "/f@B", &St), EEFS_SUCCESS);
    CHECK_EQ(St.FileSize, A_F2.Size);
    A_CheckOpen("/f", EEFS_NAND_SLOT_B, 0, &A_F2);
    A_CheckOpen("/f@A", EEFS_NAND_SLOT_A, 0, &A_F1);
    A_CheckOpen("/f@B", EEFS_NAND_SLOT_B, 0, &A_F2);
    CHECK_RC(EEFS_LibGetFileInfo(&V_Vol, "/f@A", &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.Slot, EEFS_NAND_SLOT_A);
    CHECK_EQ(Info.Version, 1);
    CHECK_RC(EEFS_LibGetFileInfo(&V_Vol, "/f", &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.Slot, EEFS_NAND_SLOT_B);
    CHECK_EQ(Info.Version, 2);
    V_CheckFile(&V_Vol, &A_F1a);
    V_CheckFile(&V_Vol, &A_F2b);
    A_CheckBoot("/f", EEFS_SUCCESS, EEFS_NAND_SLOT_B, &A_F2, 0);
    A_CheckBoot("/f@A", EEFS_SUCCESS, EEFS_NAND_SLOT_A, &A_F1, 0);
    A_CheckBoot("/f@B", EEFS_SUCCESS, EEFS_NAND_SLOT_B, &A_F2, 0);

    /* "/y" only in B: B is its active slot; "@A" does not exist */
    A_CheckEntry("/y", 1, EEFS_NAND_SLOT_B, 0, 0, 0, 5);
    CHECK_RC(EEFS_LibStat(&V_Vol, "/y", &St), EEFS_SUCCESS);
    CHECK_EQ(St.FileSize, Y.Size);
    A_CheckOpen("/y", EEFS_NAND_SLOT_B, 0, &Y);
    V_CheckFile(&V_Vol, &Yb);
    CHECK_RC(EEFS_LibStat(&V_Vol, "/y@A", &St), EEFS_FILE_NOT_FOUND);
    CHECK_RC(EEFS_LibOpen(&V_Vol, "/y@A", O_RDONLY, 0), EEFS_FILE_NOT_FOUND);
    CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/y@A"), EEFS_FILE_NOT_FOUND);
    CHECK_RC(EEFS_LibGetFileInfo(&V_Vol, "/y@A", &Info), EEFS_FILE_NOT_FOUND);
    A_CheckBoot("/y", EEFS_SUCCESS, EEFS_NAND_SLOT_B, &Y, 0);
    A_CheckBoot("/y@A", EEFS_FILE_NOT_FOUND, 0, NULL, 0);

    /* "/z" single copy: A; "@B" does not exist */
    A_CheckEntry("/z", 0, EEFS_NAND_SLOT_A, 0, 0, 6, 0);
    A_CheckOpen("/z", EEFS_NAND_SLOT_A, 0, &Z);
    V_CheckFile(&V_Vol, &Za);
    CHECK_RC(EEFS_LibOpen(&V_Vol, "/z@B", O_RDONLY, 0), EEFS_FILE_NOT_FOUND);
    CHECK_RC(EEFS_LibStat(&V_Vol, "/z@B", &St), EEFS_FILE_NOT_FOUND);
    A_CheckBoot("/z", EEFS_SUCCESS, EEFS_NAND_SLOT_A, &Z, 0);
    A_CheckBoot("/z@B", EEFS_FILE_NOT_FOUND, 0, NULL, 0);

    /* Fstat follows the opened copy; ReadDir reports the active slot's size */
    d = EEFS_LibOpen(&V_Vol, "/f@A", O_RDONLY, 0);
    CHECK(d >= 0);
    CHECK_RC(EEFS_LibFstat(d, &St), EEFS_SUCCESS);
    CHECK_EQ(St.FileSize, A_F1.Size);
    CHECK_RC(EEFS_LibClose(d), EEFS_SUCCESS);
    Dir = EEFS_LibOpenDir(&V_Vol);
    CHECK(Dir != NULL);
    while ((Dir != NULL) && ((De = EEFS_LibReadDir(Dir)) != NULL)) {
        if (De->InUse && (strcmp(De->Filename, "/f") == 0)) {
            CHECK_EQ(De->MaxFileSize, ((uint32)A_E("/f")->Slot[1].AllocBlocks * V_BlockBytes()) - NandTest_Geo.PageSize);
        }
    }
    if (Dir != NULL) CHECK_RC(EEFS_LibCloseDir(Dir), EEFS_SUCCESS);

    /* names the A/B calls reject */
    CHECK_RC(EEFS_LibSetActiveSlot(&V_Vol, "/f@A", EEFS_NAND_SLOT_A), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibSetActiveSlot(&V_Vol, "/f", 2), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibSetActiveSlot(&V_Vol, "/nope", EEFS_NAND_SLOT_A), EEFS_FILE_NOT_FOUND);
    CHECK_RC(EEFS_LibSetActiveSlot(&V_Vol, "/y", EEFS_NAND_SLOT_A), EEFS_FILE_NOT_FOUND);
    CHECK_RC(EEFS_LibSetActiveSlot(&V_Vol, "/z", EEFS_NAND_SLOT_A), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibSetActiveSlotAll(&V_Vol, 2), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibSetFallbackCompatible(&V_Vol, "/f@B", 1), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibSetFallbackCompatible(&V_Vol, "/z", 1), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibSetFailed(&V_Vol, "/f@B"), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibClearFailed(&V_Vol, "/nope"), EEFS_FILE_NOT_FOUND);
    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/f@A", EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_B), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/f", EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_A), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/f", EEFS_NAND_SLOT_A, 2), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/y", EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_B), EEFS_FILE_NOT_FOUND);
    CHECK_RC(EEFS_LibCopySlotAll(&V_Vol, EEFS_NAND_SLOT_B, EEFS_NAND_SLOT_B), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibSetFallbackCompatibleAll(&V_Vol, 0), 2);          /* /f and /y */
    CHECK_RC(EEFS_LibSetActiveSlotAll(&V_Vol, EEFS_NAND_SLOT_A), 1);   /* /y has no A */
    A_CheckEntry("/y", 1, EEFS_NAND_SLOT_B, 0, 0, 0, 5);
    V_Destroy();
}

/*
 * Fallback
 */

#define A_FAULT_PAGE0       0
#define A_FAULT_DATA        1
#define A_FAULT_ECC_PAGE0   2
#define A_FAULT_ECC_DATA    3

static const char  *A_FaultName[4] = { "page0", "data", "ecc_page0", "ecc_data" };

static void A_Fault(int Fault, const char *Path, uint8 Slot)
{
    switch (Fault) {
        case A_FAULT_PAGE0:     A_CorruptPage0(Path, Slot); break;
        case A_FAULT_DATA:      A_CorruptData(Path, Slot); break;
        case A_FAULT_ECC_PAGE0: A_InjectEcc(Path, Slot, 0); break;
        default:                A_InjectEcc(Path, Slot, 1); break;
    }
}

/* B (active) of "/f" fails with the fault; Compat chooses the fallback rule.  Open by name, the explicit names, the
 * persistence of ActiveFailed, and the bootloader's resolution of the same state. */
static void A_FallbackCase(int Fault, uint8 Compat)
{
    int32   Rc = ((Fault == A_FAULT_ECC_PAGE0) || (Fault == A_FAULT_ECC_DATA)) ? EEFS_IO_ERROR : EEFS_CORRUPT_FILE;
    boolean OpenSees = (boolean)((Fault == A_FAULT_PAGE0) || (Fault == A_FAULT_ECC_PAGE0) || (EEFS_NAND_VERIFY_ON_OPEN != 0));
    int32   d;
    char    Buf[64];

    printf("    fault %s, FallbackCompatible %u\n", A_FaultName[Fault], (unsigned int)Compat);
    V_NewVolume();
    A_MakeF(Compat);
    A_Fault(Fault, "/f", EEFS_NAND_SLOT_B);
    V_Remount();

    if (OpenSees == TRUE) {
        if (Compat != 0) {
            A_CheckOpen("/f", EEFS_NAND_SLOT_A, 1, &A_F1);      /* the inactive copy */
        }
        else {
            CHECK_RC(EEFS_LibOpen(&V_Vol, "/f", O_RDONLY, 0), Rc);
        }
        CHECK_EQ(A_E("/f")->ActiveFailed, 1);
        CHECK(V_Vol.SlotFlags[EEFS_NandFindEntry(&V_Vol, "/f")][1] & EEFS_NAND_SLOTFLAG_CORRUPT);
    }
    else {
        /* VERIFY_ON_OPEN off: Open checks page 0 only and opens the active copy; VerifyFile finds the fault */
        d = EEFS_LibOpen(&V_Vol, "/f", O_RDONLY, 0);
        CHECK(d >= 0);
        if (d >= 0) {
            CHECK_EQ(EEFS_LibFileDescriptor2Pointer(d)->Slot, EEFS_NAND_SLOT_B);
            if (Fault == A_FAULT_ECC_DATA) CHECK_RC(EEFS_LibRead(d, Buf, sizeof(Buf)), EEFS_IO_ERROR);
            CHECK_RC(EEFS_LibClose(d), EEFS_SUCCESS);
        }
        CHECK_EQ(A_E("/f")->ActiveFailed, 0);
        CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/f"), Rc);
        CHECK_EQ(A_E("/f")->ActiveFailed, 1);
    }

    /* explicit names never fall back */
    d = EEFS_LibOpen(&V_Vol, "/f@B", O_RDONLY, 0);
    if (OpenSees == TRUE) {
        CHECK_RC(d, Rc);
    }
    else {
        CHECK(d >= 0);
        if (d >= 0) CHECK_RC(EEFS_LibClose(d), EEFS_SUCCESS);
    }
    A_CheckOpen("/f@A", EEFS_NAND_SLOT_A, 0, &A_F1);
    CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/f@B"), Rc);

    /* the bootloader: same choice, and ActiveFailed */
    if (Compat != 0) {
        A_CheckBoot("/f", EEFS_SUCCESS, EEFS_NAND_SLOT_A, &A_F1, 1);
    }
    else {
        A_CheckBoot("/f", Rc, 0, NULL, 1);
    }
    A_CheckBoot("/f@B", Rc, 0, NULL, 1);
    A_CheckBoot("/f@A", EEFS_SUCCESS, EEFS_NAND_SLOT_A, &A_F1, 0);

    /* ActiveFailed is a detected media fact: kept by Abort, persisted by the next commit */
    CHECK_RC(EEFS_LibAbort(&V_Vol), EEFS_SUCCESS);
    CHECK_EQ(A_E("/f")->ActiveFailed, 1);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    CHECK_EQ(A_E("/f")->ActiveFailed, 1);
    if (Compat != 0) {
        A_CheckBoot("/f", EEFS_SUCCESS, EEFS_NAND_SLOT_A, &A_F1, 1);
    }

    /* repaired by a rewrite of the active copy (the fault is on the old blocks only) */
    NandSim_ClearInjections(V_Sim);
    CHECK_RC(A_Put("/f", &A_F2, EEFS_NAND_SLOT_NONE, 3), EEFS_SUCCESS);
    CHECK_EQ(A_E("/f")->ActiveFailed, 0);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    A_CheckEntry("/f", 1, EEFS_NAND_SLOT_B, Compat, 0, 1, 3);
    A_CheckOpen("/f", EEFS_NAND_SLOT_B, 0, &A_F2);
    A_CheckBoot("/f", EEFS_SUCCESS, EEFS_NAND_SLOT_B, &A_F2, 0);
    CHECK_EQ(NandSim_Violations(V_Sim), 0);
    V_Destroy();
}

static void ab_active_corrupt_compatible_falls_back(void)
{
    int     Fault;

    for (Fault = A_FAULT_PAGE0; Fault <= A_FAULT_ECC_DATA; Fault++) {
        A_FallbackCase(Fault, 1);
    }
}

static void ab_active_corrupt_not_compatible_fails(void)
{
    int     Fault;

    for (Fault = A_FAULT_PAGE0; Fault <= A_FAULT_ECC_DATA; Fault++) {
        A_FallbackCase(Fault, 0);
    }
}

/* Both copies bad: the fallback's own error; an explicit name never falls back even with a good other copy */
static void ab_explicit_name_never_falls_back(void)
{
    V_NewVolume();
    A_MakeF(1);
    A_CorruptPage0("/f", EEFS_NAND_SLOT_A);
    V_Remount();
    CHECK_RC(EEFS_LibOpen(&V_Vol, "/f@A", O_RDONLY, 0), EEFS_CORRUPT_FILE);   /* B is fine and compatible */
    CHECK_EQ(A_E("/f")->ActiveFailed, 0);                                      /* A is not the active copy */
    A_CheckOpen("/f", EEFS_NAND_SLOT_B, 0, &A_F2);
    A_CheckBoot("/f@A", EEFS_CORRUPT_FILE, 0, NULL, 0);
    A_CheckBoot("/f", EEFS_SUCCESS, EEFS_NAND_SLOT_B, &A_F2, 0);

    /* now B as well: Open by name returns the fallback's error */
    A_InjectEcc("/f", EEFS_NAND_SLOT_B, 0);
    V_Remount();
    CHECK_RC(EEFS_LibOpen(&V_Vol, "/f", O_RDONLY, 0), EEFS_CORRUPT_FILE);
    CHECK_EQ(A_E("/f")->ActiveFailed, 1);
    CHECK_RC(EEFS_LibOpen(&V_Vol, "/f@B", O_RDONLY, 0), EEFS_IO_ERROR);
    A_CheckBoot("/f", EEFS_CORRUPT_FILE, 0, NULL, 1);
    A_CheckBoot("/f@B", EEFS_IO_ERROR, 0, NULL, 1);
    V_Destroy();
}

/*
 * ActiveFailed
 */

static void ab_setfailed_persists_and_clears(void)
{
    V_NewVolume();
    A_MakeF(1);

    /* SetFailed is a pending change: lost without a commit, kept across a remount after one */
    CHECK_RC(EEFS_LibSetFailed(&V_Vol, "/f"), EEFS_SUCCESS);
    CHECK_EQ(A_E("/f")->ActiveFailed, 1);
    V_Remount();
    CHECK_EQ(A_E("/f")->ActiveFailed, 0);
    CHECK_RC(EEFS_LibSetFailed(&V_Vol, "/f"), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    A_CheckEntry("/f", 1, EEFS_NAND_SLOT_B, 1, 1, 1, 2);
    A_CheckOpen("/f", EEFS_NAND_SLOT_B, 0, &A_F2);         /* the flag alone does not move the active slot */
    A_CheckBoot("/f", EEFS_SUCCESS, EEFS_NAND_SLOT_B, &A_F2, 1);

    /* ClearFailed */
    CHECK_RC(EEFS_LibClearFailed(&V_Vol, "/f"), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    CHECK_EQ(A_E("/f")->ActiveFailed, 0);
    A_CheckBoot("/f", EEFS_SUCCESS, EEFS_NAND_SLOT_B, &A_F2, 0);

    /* a rewrite of the inactive copy does not clear it; a rewrite of the active copy does */
    CHECK_RC(EEFS_LibSetFailed(&V_Vol, "/f"), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK_RC(A_Put("/f@A", &A_F1, EEFS_NAND_SLOT_NONE, 1), EEFS_SUCCESS);
    CHECK_EQ(A_E("/f")->ActiveFailed, 1);
    CHECK_RC(A_Put("/f", &A_F2, EEFS_NAND_SLOT_NONE, 2), EEFS_SUCCESS);
    CHECK_EQ(A_E("/f")->ActiveFailed, 0);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    CHECK_EQ(A_E("/f")->ActiveFailed, 0);

    /* a copy into the inactive slot does not clear it; a copy into the active slot does */
    CHECK_RC(EEFS_LibSetFailed(&V_Vol, "/f"), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/f", EEFS_NAND_SLOT_B, EEFS_NAND_SLOT_A), EEFS_SUCCESS);
    CHECK_EQ(A_E("/f")->ActiveFailed, 1);
    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/f", EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_B), EEFS_SUCCESS);
    CHECK_EQ(A_E("/f")->ActiveFailed, 0);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    CHECK_EQ(A_E("/f")->ActiveFailed, 0);
    A_CheckOpen("/f", EEFS_NAND_SLOT_B, 0, &A_F2);

    /* switching the active slot clears it (SetActiveSlot and SetActiveSlotAll) */
    CHECK_RC(EEFS_LibSetFailed(&V_Vol, "/f"), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    CHECK_RC(EEFS_LibSetActiveSlot(&V_Vol, "/f", EEFS_NAND_SLOT_A), EEFS_SUCCESS);
    CHECK_EQ(A_E("/f")->ActiveFailed, 0);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    A_CheckEntry("/f", 1, EEFS_NAND_SLOT_A, 1, 0, 2, 2);
    CHECK_RC(EEFS_LibSetFailed(&V_Vol, "/f"), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibSetActiveSlotAll(&V_Vol, EEFS_NAND_SLOT_B), 1);
    CHECK_EQ(A_E("/f")->ActiveFailed, 0);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    A_CheckEntry("/f", 1, EEFS_NAND_SLOT_B, 1, 0, 2, 2);
    V_Destroy();
}

/*
 * Remove of one slot (section 6.5)
 */

static void ab_remove_slot_rules(void)
{
    V_File_t    Y = A_File("/y", 2500, 731);
    V_File_t    Z = A_File("/z", 2600, 732);
    uint16      FirstA;
    uint32      Cleanup;
    int32       d;

    V_NewVolume();
    A_MakeF(1);
    CHECK_RC(A_Put("/y@A", &Y, EEFS_NAND_SLOT_NONE, 1), EEFS_SUCCESS);
    CHECK_RC(A_Put("/y@B", &Y, EEFS_NAND_SLOT_NONE, 1), EEFS_SUCCESS);
    CHECK_RC(A_Put("/z@A", &Z, EEFS_NAND_SLOT_NONE, 1), EEFS_SUCCESS);
    CHECK_RC(A_Put("/z@B", &Z, EEFS_NAND_SLOT_NONE, 1), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);

    /* the active slot cannot be removed while the other exists */
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/f@B"), EEFS_PERMISSION_DENIED);
    /* the inactive one can: the entry stays with B */
    FirstA = A_E("/f")->Slot[0].FirstBlock;
    Cleanup = V_Vol.PendingCleanupCount;
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/f@A"), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.PendingCleanupCount, Cleanup + 1);
    CHECK_EQ(V_Vol.Working.Map[FirstA], EEFS_NAND_MAP_FREEING);
    A_CheckEntry("/f", 1, EEFS_NAND_SLOT_B, 1, 0, 0, 2);
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/f@A"), EEFS_FILE_NOT_FOUND);
    CHECK_RC(EEFS_LibSetActiveSlot(&V_Vol, "/f", EEFS_NAND_SLOT_A), EEFS_FILE_NOT_FOUND);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK(NandSim_PageIsErased(V_Sim, FirstA, 0));
    V_Remount();
    A_CheckEntry("/f", 1, EEFS_NAND_SLOT_B, 1, 0, 0, 2);
    A_CheckOpen("/f", EEFS_NAND_SLOT_B, 0, &A_F2);
    A_CheckBoot("/f", EEFS_SUCCESS, EEFS_NAND_SLOT_B, &A_F2, 0);
    A_CheckBoot("/f@A", EEFS_FILE_NOT_FOUND, 0, NULL, 0);
    /* the last slot: the entry goes */
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/f@B"), EEFS_SUCCESS);
    CHECK(EEFS_NandFindEntry(&V_Vol, "/f") < 0);

    /* an open copy cannot be removed; the other copy can */
    d = EEFS_LibOpen(&V_Vol, "/y@B", O_RDONLY, 0);
    CHECK(d >= 0);
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/y@B"), EEFS_PERMISSION_DENIED);
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/y"), EEFS_PERMISSION_DENIED);
    CHECK_RC(EEFS_LibClose(d), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/y@B"), EEFS_SUCCESS);
    A_CheckEntry("/y", 1, EEFS_NAND_SLOT_A, 0, 0, 1, 0);

    /* the whole entry: both slots freed, two first blocks to clean up */
    Cleanup = V_Vol.PendingCleanupCount;
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/z"), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.PendingCleanupCount, Cleanup + 2);
    CHECK(EEFS_NandFindEntry(&V_Vol, "/z") < 0);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    CHECK(EEFS_NandFindEntry(&V_Vol, "/f") < 0);
    CHECK(EEFS_NandFindEntry(&V_Vol, "/z") < 0);
    A_CheckBoot("/z", EEFS_FILE_NOT_FOUND, 0, NULL, 0);
    A_CheckBoot("/y", EEFS_SUCCESS, EEFS_NAND_SLOT_A, &Y, 0);
    V_Destroy();
}

/*
 * CopySlot
 */

/* CopySlotAll stops at the first failing entry; the copies before it stay in Working; nothing leaks */
static void ab_copyslot_all_stops_at_first_error(void)
{
    static const char  *Names[3] = { "/c0", "/c1", "/c2" };
    V_File_t            Fa[3];
    V_File_t            Fb[3];
    V_File_t            G = A_File("/g", 500, 740);
    EEFS_NandSlot_t     Before[3];
    uint32              Free;
    uint32              i;

    V_NewVolume();
    for (i = 0; i < 3; i++) {
        Fa[i] = A_File(Names[i], NandTest_Geo.PageSize * (2 + i), 741 + i);
        Fb[i] = A_File(Names[i], NandTest_Geo.PageSize * (3 + i) + 1, 751 + i);
        CHECK_RC(A_Put(Names[i], &Fa[i], EEFS_NAND_SLOT_A, 1), EEFS_SUCCESS);
        CHECK_RC(A_Put(Names[i], &Fb[i], EEFS_NAND_SLOT_B, 2), EEFS_SUCCESS);
    }
    CHECK_RC(A_Put("/g", &G, EEFS_NAND_SLOT_NONE, 1), EEFS_SUCCESS);   /* single copy: skipped */
    CHECK_RC(EEFS_LibSetActiveSlotAll(&V_Vol, EEFS_NAND_SLOT_B), 3);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    A_CorruptData("/c1", EEFS_NAND_SLOT_B);         /* found only while copying (page 0 is fine) */
    V_Remount();
    CHECK_EQ(A_E("/c1")->ActiveFailed, 0);
    for (i = 0; i < 3; i++) Before[i] = A_E(Names[i])->Slot[0];
    Free = A_Free();

    CHECK_RC(EEFS_LibCopySlotAll(&V_Vol, EEFS_NAND_SLOT_B, EEFS_NAND_SLOT_A), EEFS_CORRUPT_FILE);
    CHECK_EQ(A_E("/c1")->ActiveFailed, 1);          /* the source is c1's active copy */
    CHECK_EQ(A_E("/c0")->Slot[0].Version, 2);       /* copied */
    CHECK(A_E("/c0")->Slot[0].FirstBlock != Before[0].FirstBlock);
    CHECK(memcmp(&A_E("/c1")->Slot[0], &Before[1], sizeof(EEFS_NandSlot_t)) == 0);    /* the failing source */
    CHECK(memcmp(&A_E("/c2")->Slot[0], &Before[2], sizeof(EEFS_NandSlot_t)) == 0);    /* never reached */
    CHECK_EQ(A_Free(), Free - A_E("/c0")->Slot[0].AllocBlocks);                       /* c1's blocks were released */
    CHECK_RC(EEFS_LibAbort(&V_Vol), EEFS_SUCCESS);
    CHECK_EQ(A_Free(), Free);
    CHECK(memcmp(&A_E("/c0")->Slot[0], &Before[0], sizeof(EEFS_NandSlot_t)) == 0);
    CHECK_EQ(A_E("/c1")->ActiveFailed, 1);          /* a detected media fact survives the Abort */
    V_CheckOrphansComplete(&V_Vol);

    /* the other direction repairs c1's active copy (a copy into the active slot clears ActiveFailed) */
    CHECK_RC(EEFS_LibCopySlotAll(&V_Vol, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_B), 3);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    for (i = 0; i < 3; i++) {
        A_CheckEntry(Names[i], 1, EEFS_NAND_SLOT_B, 0, 0, 1, 1);
        A_CheckOpen(Names[i], EEFS_NAND_SLOT_B, 0, &Fa[i]);
        A_CheckDisjoint(Names[i]);
    }
    A_CheckEntry("/g", 0, EEFS_NAND_SLOT_A, 0, 0, 1, 0);

    /* a read-only entry stops it as well */
    CHECK_RC(EEFS_LibSetFileAttributes(&V_Vol, "/c0", EEFS_ATTRIBUTE_READONLY), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCopySlotAll(&V_Vol, EEFS_NAND_SLOT_B, EEFS_NAND_SLOT_A), EEFS_PERMISSION_DENIED);
    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/c0", EEFS_NAND_SLOT_B, EEFS_NAND_SLOT_A), EEFS_PERMISSION_DENIED);
    V_Destroy();
}

/* A single-copy file opts into A/B by copying A into B */
static void ab_single_copy_opt_in_via_copyslot(void)
{
    V_File_t    P = A_File("/p", V_D0() + 10, 761);
    V_File_t    Pb = A_As(P, "/p@B");
    V_File_t    Pa = A_As(P, "/p@A");

    V_NewVolume();
    CHECK_RC(A_Put("/p", &P, EEFS_NAND_SLOT_NONE, 9), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibSetActiveSlot(&V_Vol, "/p", EEFS_NAND_SLOT_A), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibSetFallbackCompatible(&V_Vol, "/p", 1), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibSetFailed(&V_Vol, "/p"), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibClearFailed(&V_Vol, "/p"), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/p", EEFS_NAND_SLOT_B, EEFS_NAND_SLOT_A), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibCopySlotAll(&V_Vol, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_B), 0);    /* not IsAB: not included */
    CHECK_RC(EEFS_LibSetActiveSlotAll(&V_Vol, EEFS_NAND_SLOT_A), 0);

    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/p", EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_B), EEFS_SUCCESS);
    A_CheckEntry("/p", 1, EEFS_NAND_SLOT_A, 0, 0, 9, 9);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    A_CheckEntry("/p", 1, EEFS_NAND_SLOT_A, 0, 0, 9, 9);
    A_CheckDisjoint("/p");
    NandSim_RawRead(V_Sim, A_E("/p")->Slot[0].FirstBlock, 0, V_Page, NULL);
    CHECK_EQ(V_Page[273], EEFS_NAND_SLOT_NONE);       /* A's metadata page is unchanged */
    NandSim_RawRead(V_Sim, A_E("/p")->Slot[1].FirstBlock, 0, V_Page, NULL);
    CHECK_EQ(V_Page[273], EEFS_NAND_SLOT_B);
    V_CheckFile(&V_Vol, &Pa);
    V_CheckFile(&V_Vol, &Pb);
    A_CheckOpen("/p", EEFS_NAND_SLOT_A, 0, &P);
    A_CheckBoot("/p@B", EEFS_SUCCESS, EEFS_NAND_SLOT_B, &P, 0);
    A_CheckBoot("/p", EEFS_SUCCESS, EEFS_NAND_SLOT_A, &P, 0);
    /* now the setters apply */
    CHECK_RC(EEFS_LibSetActiveSlot(&V_Vol, "/p", EEFS_NAND_SLOT_B), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibSetFallbackCompatible(&V_Vol, "/p", 7), EEFS_SUCCESS);
    A_CheckEntry("/p", 1, EEFS_NAND_SLOT_B, 1, 0, 9, 9);
    V_Destroy();
}

/* CopySlot takes its blocks with the NEW reserve class (a rewrite of the same slot may go deeper), copies into blocks of
 * its own and changes nothing when it cannot */
static void ab_copyslot_reserve_and_own_blocks(void)
{
    V_File_t                F = A_File("/r", V_SizeForBlocks(3), 771);
    V_File_t                F2 = A_File("/r", V_SizeForBlocks(3), 772);
    V_File_t                S = A_File("/s", 10, 773);
    V_File_t                Small;
    static char             Names[40][8];
    V_Snap_t                Snap;
    V_Snap_t                Snap2;
    uint32                  Programs;
    uint32                  Erases;
    uint32                  i = 0;

    V_NewVolumeBlocks(NandTest_Geo.DirRegionBlocks + 40);
    CHECK_RC(A_Put("/r@A", &F, EEFS_NAND_SLOT_NONE, 1), EEFS_SUCCESS);
    CHECK_RC(A_Put("/r@B", &F, EEFS_NAND_SLOT_NONE, 1), EEFS_SUCCESS);
    CHECK_RC(A_Put("/s", &S, EEFS_NAND_SLOT_NONE, 1), EEFS_SUCCESS);
    /* fill with one-block files until FreeBlocks == RESERVE + 3 - 1 */
    while ((A_Free() > EEFS_NAND_RESERVE_BLOCKS + 2) && (i < 40)) {
        sprintf(Names[i], "/s%lu", (unsigned long)i);
        Small = A_File(Names[i], 10, 780 + i);
        CHECK_RC(A_Put(Names[i], &Small, EEFS_NAND_SLOT_NONE, 1), EEFS_SUCCESS);
        i++;
    }
    CHECK_EQ(A_Free(), EEFS_NAND_RESERVE_BLOCKS + 2);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);

    /* a 3-block copy would leave less than the reserve: no change at all */
    V_Take(&V_Vol, &Snap);
    Programs = NandSim_Programs(V_Sim);
    Erases = NandSim_Erases(V_Sim);
    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/r", EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_B), EEFS_NO_SPACE_LEFT_ON_DEVICE);
    V_Take(&V_Vol, &Snap2);
    CHECK(V_Same(&Snap, &Snap2));
    CHECK_EQ(NandSim_Programs(V_Sim), Programs);
    CHECK_EQ(NandSim_Erases(V_Sim), Erases);
    CHECK_EQ(V_Vol.PendingCleanupCount, 0);

    /* a copy-on-write rewrite of the same slot uses the MODIFY class and fits */
    CHECK_RC(A_Put("/r@B", &F2, EEFS_NAND_SLOT_NONE, 2), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);

    /* one block more and the copy fits; A and B then have disjoint blocks */
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/s"), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK_EQ(A_Free(), EEFS_NAND_RESERVE_BLOCKS + 3);
    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/r", EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_B), EEFS_SUCCESS);
    CHECK_EQ(A_Free(), EEFS_NAND_RESERVE_BLOCKS);
    A_CheckDisjoint("/r");
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    A_CheckDisjoint("/r");
    A_CheckEntry("/r", 1, EEFS_NAND_SLOT_A, 0, 0, 1, 1);
    Small = A_As(F, "/r@B");
    V_CheckFile(&V_Vol, &Small);
    V_Destroy();
}

/* CopySlot into an open destination is refused; reading the source while copying is fine */
static void ab_copyslot_open_rules(void)
{
    int32   d;

    V_NewVolume();
    A_MakeF(1);
    d = EEFS_LibOpen(&V_Vol, "/f@A", O_RDONLY, 0);
    CHECK(d >= 0);
    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/f", EEFS_NAND_SLOT_B, EEFS_NAND_SLOT_A), EEFS_PERMISSION_DENIED);
    CHECK_RC(EEFS_LibCopySlot(&V_Vol, "/f", EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_B), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibClose(d), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    V_Remount();
    A_CheckEntry("/f", 1, EEFS_NAND_SLOT_B, 1, 0, 1, 1);
    A_CheckOpen("/f", EEFS_NAND_SLOT_B, 0, &A_F1);
    V_Destroy();
}

/*
 * Copy-on-write of one slot never touches the other
 */

static void ab_modify_inactive_slot_leaves_active_untouched(void)
{
    V_File_t    M1 = A_File("/m", V_SizeForBlocks(6), 791);
    V_File_t    M2;
    V_File_t    M1a;
    V_File_t    M2b;
    uint32      ErasesA[A_MAX_BLOCKS];
    uint8       Page0A[EEFS_NAND_MAX_PAGE_SIZE];
    uint16      OldB[A_MAX_BLOCKS];
    uint32      na;
    uint32      nb;
    uint32      k;

    M2 = M1;
    M2.Mod[M2.ModCount++] = V_FileOffset(3, 100);        /* only position 3 differs */
    M1a = A_As(M1, "/m@A");
    M2b = A_As(M2, "/m@B");
    V_NewVolume();
    CHECK_RC(A_Put("/m@A", &M1, EEFS_NAND_SLOT_NONE, 1), EEFS_SUCCESS);
    CHECK_RC(A_Put("/m@B", &M1, EEFS_NAND_SLOT_NONE, 1), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    A_CheckDisjoint("/m");
    na = A_RawBlocks("/m", 0, A_BlocksA);
    nb = A_RawBlocks("/m", 1, OldB);
    for (k = 0; k < na; k++) ErasesA[k] = NandSim_BlockErases(V_Sim, A_BlocksA[k]);
    NandSim_RawRead(V_Sim, A_BlocksA[0], 0, Page0A, NULL);

    CHECK_RC(A_Put("/m@B", &M2, EEFS_NAND_SLOT_NONE, 2), EEFS_SUCCESS);   /* B is inactive (A active) */
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    CHECK_EQ(A_RawBlocks("/m", 1, A_BlocksB), nb);
    for (k = 1; k < nb; k++) {
        if (k == 3) CHECK(A_BlocksB[k] != OldB[k]);
        else CHECK_EQ(A_BlocksB[k], OldB[k]);           /* B's unchanged blocks kept */
    }
    for (k = 0; k < na; k++) {
        CHECK_EQ(NandSim_BlockErases(V_Sim, A_BlocksA[k]), ErasesA[k]);
    }
    NandSim_RawRead(V_Sim, A_BlocksA[0], 0, V_Page, NULL);
    CHECK(memcmp(V_Page, Page0A, NandTest_Geo.PageSize) == 0);
    V_Remount();
    A_CheckDisjoint("/m");
    V_CheckFile(&V_Vol, &M1a);
    V_CheckFile(&V_Vol, &M2b);
    A_CheckEntry("/m", 1, EEFS_NAND_SLOT_A, 0, 0, 1, 2);
    V_Destroy();
}

/* A rewrite of the active copy in a batch, power lost before the commit: the old active copy is intact and boots */
static void ab_modify_active_slot_is_safe(void)
{
    V_File_t    F3;

    V_NewVolume();
    A_MakeF(1);
    F3 = A_F2;
    F3.Mod[F3.ModCount++] = 5;
    CHECK_RC(A_Put("/f", &F3, EEFS_NAND_SLOT_NONE, 3), EEFS_SUCCESS);       /* the active slot B */
    CHECK_EQ(A_E("/f")->Slot[1].Version, 3);
    A_CheckBoot("/f", EEFS_SUCCESS, EEFS_NAND_SLOT_B, &A_F2, 0);
    NandSim_Reboot(V_Sim);
    EEFS_LibInit();
    V_Remount();
    A_CheckEntry("/f", 1, EEFS_NAND_SLOT_B, 1, 0, 1, 2);
    A_CheckOpen("/f", EEFS_NAND_SLOT_B, 0, &A_F2);
    V_CheckFile(&V_Vol, &A_F2b);
    V_CheckFile(&V_Vol, &A_F1a);
    A_CheckBoot("/f", EEFS_SUCCESS, EEFS_NAND_SLOT_B, &A_F2, 0);
    V_CheckSlotsValid(&V_Vol);
    V_Destroy();
}

/*
 * The bootloader resolves every state as the library does
 */

/* One state: library Open by name and the bootloader load agree on the return code, the slot, the content and
 * ActiveFailed (the library's, after the open) */
static void A_Agree(const char *Name)
{
    MicroEEFS_NandInfo_t    Info;
    EEFS_FileDescriptor_t  *Fd;
    uint8                  *Lib = V_Data2;
    uint32                  Total = 0;
    uint32                  Programs = NandSim_Programs(V_Sim);
    uint32                  Erases = NandSim_Erases(V_Sim);
    int32                   d;
    int32                   n;
    int32                   Status;
    uint8                   Slot = 0xFF;
    char                    Path[EEFS_NAND_MAX_PATH_SIZE];
    uint8                   Spec;

    d = EEFS_LibOpen(&V_Vol, (char *)Name, O_RDONLY, 0);
    if (d >= 0) {
        Fd = EEFS_LibFileDescriptor2Pointer(d);
        Slot = Fd->Slot;
        while ((n = EEFS_LibRead(d, &Lib[Total], 65537)) > 0) Total += (uint32)n;
        CHECK(n == 0);
        CHECK_RC(EEFS_LibClose(d), EEFS_SUCCESS);
    }
    memset(&Info, 0, sizeof(Info));
    Status = MicroEEFS_NandLoadFile(&V_Dev, A_PageBuf, Name, V_Read, V_BUF_SIZE, &Info);
    CHECK_RC(Status, (d >= 0) ? EEFS_SUCCESS : d);
    if ((Status == EEFS_SUCCESS) && (d >= 0)) {
        CHECK_EQ(Info.LoadedSlot, Slot);
        CHECK_EQ(Info.Info.FileSize, Total);
        CHECK(memcmp(V_Read, Lib, Total) == 0);
    }
    CHECK(EEFS_NandParseName(Name, Path, &Spec) == EEFS_SUCCESS);
    if ((Status != EEFS_FILE_NOT_FOUND) && (Spec == EEFS_NAND_SLOT_NONE)) {
        CHECK_EQ(Info.Info.ActiveFailed, A_E(Path)->ActiveFailed);
    }
    CHECK_EQ(NandSim_Programs(V_Sim), Programs);
    CHECK_EQ(NandSim_Erases(V_Sim), Erases);
}

static void ab_bootloader_agrees_with_library(void)
{
    static const int    Faults[] = { -1, A_FAULT_PAGE0, A_FAULT_ECC_PAGE0, A_FAULT_DATA, A_FAULT_ECC_DATA };
    uint32              f;
    uint8               Compat;
    uint8               Failed;
    uint8               Both;
    V_File_t            Y = A_File("/y", 4000, 795);

    for (f = 0; f < sizeof(Faults) / sizeof(Faults[0]); f++) {
#if !EEFS_NAND_VERIFY_ON_OPEN
        if ((Faults[f] == A_FAULT_DATA) || (Faults[f] == A_FAULT_ECC_DATA)) continue;  /* Open checks page 0 only */
#endif
        for (Compat = 0; Compat < 2; Compat++) {
            for (Failed = 0; Failed < 2; Failed++) {
                for (Both = 0; Both < 2; Both++) {
                    if ((Both != 0) && (Faults[f] < 0)) continue;
                    V_NewVolume();
                    A_MakeF(Compat);
                    CHECK_RC(A_Put("/y@B", &Y, EEFS_NAND_SLOT_NONE, 1), EEFS_SUCCESS);
                    if (Failed != 0) CHECK_RC(EEFS_LibSetFailed(&V_Vol, "/f"), EEFS_SUCCESS);
                    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
                    if (Faults[f] >= 0) {
                        A_Fault(Faults[f], "/f", EEFS_NAND_SLOT_B);
                        if (Both != 0) A_CorruptPage0("/f", EEFS_NAND_SLOT_A);
                    }
                    V_Remount();
                    A_Agree("/f");
                    A_Agree("/f@A");
                    A_Agree("/f@B");
                    A_Agree("/y");
                    A_Agree("/y@A");
                    A_Agree("/y@B");
                    if (NandTest_CaseFailed()) {
                        printf("    fault %d, compatible %u, failed %u, both %u\n", Faults[f], (unsigned int)Compat,
                               (unsigned int)Failed, (unsigned int)Both);
                        V_Destroy();
                        return;
                    }
                    V_Destroy();
                }
            }
        }
    }
}

/*
 * Rebuild recovers locations and versions (FMT 6): the bootloader's scan now, the library once phase 4 is in
 */

static void ab_rebuild_recovers_locations_versions(void)
{
    MicroEEFS_NandInfo_t    Info;
    EEFS_NandSlot_t         Slots[2];
    uint32                  b;
    int32                   Status;

    V_NewVolume();
    A_MakeF(1);
    Slots[0] = A_E("/f")->Slot[0];
    Slots[1] = A_E("/f")->Slot[1];
    for (b = 0; b < NandTest_Geo.DirRegionBlocks; b++) {
        NandSim_RawErase(V_Sim, b);
    }

    memset(&Info, 0, sizeof(Info));
    CHECK_RC(MicroEEFS_NandLoadFile(&V_Dev, A_PageBuf, "/f", V_Read, V_BUF_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.UsedScan, 1);
    CHECK_EQ(Info.Info.IsAB, 1);
    CHECK_EQ(Info.Info.ActiveSlot, EEFS_NAND_SLOT_A);         /* A when present */
    CHECK_EQ(Info.Info.FallbackCompatible, 0);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_A);
    CHECK(memcmp(V_Read, V_Content(&A_F1), A_F1.Size) == 0);
    for (b = 0; b < 2; b++) {
        CHECK_EQ(Info.Info.SlotInfo[b].FirstBlock, Slots[b].FirstBlock);
        CHECK_EQ(Info.Info.SlotInfo[b].Version, Slots[b].Version);
        CHECK_EQ(Info.Info.SlotInfo[b].FileSize, Slots[b].FileSize);
    }
    CHECK_RC(MicroEEFS_NandLoadFile(&V_Dev, A_PageBuf, "/f@B", V_Read, V_BUF_SIZE, &Info), EEFS_SUCCESS);
    CHECK(memcmp(V_Read, V_Content(&A_F2), A_F2.Size) == 0);

    /* the library: mount rebuilds the directory from page 0s */
    Status = V_Mount(&V_Vol);
    CHECK_RC(Status, EEFS_SUCCESS);
    if (Status == EEFS_SUCCESS) {
        CHECK_EQ(V_Vol.DirectoryRebuilt, 1);
        A_CheckEntry("/f", 1, EEFS_NAND_SLOT_A, 0, 0, 1, 2);
        for (b = 0; b < 2; b++) {
            CHECK_EQ(A_E("/f")->Slot[b].FirstBlock, Slots[b].FirstBlock);
            CHECK_EQ(A_E("/f")->Slot[b].AllocBlocks, Slots[b].AllocBlocks);
            CHECK_EQ(A_E("/f")->Slot[b].WriteSeq, Slots[b].WriteSeq);
        }
        V_CheckFile(&V_Vol, &A_F1a);
        V_CheckFile(&V_Vol, &A_F2b);
    }
    V_Destroy();
}

/*
 * Power-cut sweeps
 */

static int A_InState(const char *Path, const A_State_t *S)
{
    uint32  i;

    for (i = 0; i < S->CopyCount; i++) {
        if (strcmp(S->Copy[i].Path, Path) == 0) return(1);
    }
    return(0);
}

/* Every copy of the state exactly, the copies only the other state has absent, and every file boots its load */
static void A_CheckState(const A_State_t *S, const A_State_t *Other, int Verify)
{
    uint32  i;

    for (i = 0; i < S->CopyCount; i++) {
        V_CheckFileEx(&V_Vol, &S->Copy[i], (boolean)Verify);
    }
    for (i = 0; i < Other->CopyCount; i++) {
        if (!A_InState(Other->Copy[i].Path, S)) V_CheckAbsent(&V_Vol, Other->Copy[i].Path);
    }
    for (i = 0; i < S->BootCount; i++) {
        A_CheckBoot(S->Boot[i].Path, EEFS_SUCCESS, S->BootSlot[i], &S->Boot[i], 0);
    }
}

/* The first blocks of the pre state that the post state no longer uses */
static void A_FindCleanup(const EEFS_InodeTable_t *PreVol)
{
    uint32  i;
    uint32  j;
    uint32  s;
    uint32  t;
    uint16  b;
    int     Used;

    A_CleanupCount = 0;
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        for (s = 0; s < 2; s++) {
            if (!PreVol->Base.Entry[i].InUse || !PreVol->Base.Entry[i].Slot[s].InUse) continue;
            b = PreVol->Base.Entry[i].Slot[s].FirstBlock;
            Used = 0;
            for (j = 0; j < EEFS_MAX_FILES; j++) {
                for (t = 0; t < 2; t++) {
                    if (V_Vol.Working.Entry[j].InUse && V_Vol.Working.Entry[j].Slot[t].InUse &&
                        (V_Vol.Working.Entry[j].Slot[t].FirstBlock == b)) Used = 1;
                }
            }
            if (!Used && (A_CleanupCount < A_MAX_CLEANUP)) A_Cleanup[A_CleanupCount++] = b;
        }
    }
}

static void A_CheckCleanupDone(void)
{
    uint32  i;

    for (i = 0; i < A_CleanupCount; i++) {
        if (V_Vol.Working.Map[A_Cleanup[i]] == EEFS_NAND_MAP_FREE) {
            CHECK(NandSim_PageIsErased(V_Sim, A_Cleanup[i], 0));
        }
        else {
            CHECK(EEFS_NAND_MAP_IS_OWNER(V_Vol.Working.Map[A_Cleanup[i]]));
        }
    }
}

static uint32 A_Sweep(const char *Name, A_Run_t Run)
{
    static EEFS_InodeTable_t    PreVol;
    V_File_t                    Follow = A_File("/follow", 3000, 999);
    uint32                      Cut;
    uint32                      Cuts = 0;
    uint32                      Ops;
    int                         Mode;
    int                         Done;
    int                         SeenPost;
    int                         IsPre;
    int                         IsPost;
    int32                       Status;

    /* the uninterrupted run */
    V_Attach(NandSim_Clone(A_Image));
    CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);
    V_Take(&V_Vol, &A_SnapPre);
    memcpy(&PreVol, &V_Vol, sizeof(PreVol));
    A_CheckState(&A_Pre, &A_Post, 1);
    CHECK_RC(Run(), EEFS_SUCCESS);
    V_Take(&V_Vol, &A_SnapPost);
    CHECK(!V_Same(&A_SnapPre, &A_SnapPost));
    A_FindCleanup(&PreVol);
    A_CheckState(&A_Post, &A_Pre, 1);
    V_Destroy();
    if (NandTest_CaseFailed()) {
        printf("    sweep %s: the uninterrupted run failed\n", Name);
        return(0);
    }

    for (Mode = NANDSIM_TORN_NONE; Mode <= NANDSIM_TORN_GARBAGE; Mode++) {
        SeenPost = 0;
        for (Cut = 0; ; Cut++) {
            V_Attach(NandSim_Clone(A_Image));
            CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);
            NandSim_SetPowerBudget(V_Sim, Cut, Mode);
            Status = Run();
            Done = !NandSim_PowerCutHappened(V_Sim);
            if (Done) {
                CHECK_RC(Status, EEFS_SUCCESS);
            }
            NandSim_Reboot(V_Sim);
            EEFS_LibInit();

            Ops = V_Ops();
            Status = V_Mount(&V_Vol);
            CHECK_RC(Status, EEFS_SUCCESS);
            CHECK_EQ(V_Ops(), Ops);
            if (Status == EEFS_SUCCESS) {
                V_Take(&V_Vol, &A_SnapCur);
                IsPre = V_Same(&A_SnapCur, &A_SnapPre);
                IsPost = V_Same(&A_SnapCur, &A_SnapPost);
                CHECK(IsPre || IsPost);
                if (Done) CHECK(IsPost);
                if (IsPost) SeenPost = 1;
                else if (SeenPost) CHECK(!IsPre);
                V_CheckSlotsValid(&V_Vol);
                V_CheckOrphansComplete(&V_Vol);
                A_CheckState(IsPost ? &A_Post : &A_Pre, IsPost ? &A_Pre : &A_Post, 0);

                CHECK_RC(V_Write(&Follow), EEFS_SUCCESS);
                CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
                CHECK_EQ(V_Vol.PendingCleanupCount, 0);
                A_CheckCleanupDone();
                V_CheckMirrorsAgree(&V_Vol);
                if (((Cut % 8) == 0) || Done) {
                    A_CheckState(IsPost ? &A_Post : &A_Pre, IsPost ? &A_Pre : &A_Post, 1);
                }
            }
            CHECK_EQ(NandSim_Violations(V_Sim), 0);

            if (NandTest_CaseFailed()) {
                printf("    sweep %s: failure at cut %lu, torn mode %s\n", Name, (unsigned long)Cut, A_ModeName[Mode]);
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
    printf("    sweep %s: %lu cut points x 3 torn modes\n", Name, (unsigned long)Cuts);
    return(Cuts);
}

/* States of the load: v1 in A only, both with B or A active, or v2 in both */
static void A_StateV1(A_State_t *S)
{
    uint32  i;

    memset(S, 0, sizeof(*S));
    for (i = 0; i < 3; i++) {
        S->Copy[S->CopyCount++] = A_As(A_V1[i], A_NamesA[i]);
        S->Boot[i] = A_V1[i];
        S->BootSlot[i] = EEFS_NAND_SLOT_A;
    }
    S->BootCount = 3;
}

static void A_StateBoth(A_State_t *S, uint8 Active, int Promoted)
{
    uint32  i;

    memset(S, 0, sizeof(*S));
    for (i = 0; i < 3; i++) {
        S->Copy[S->CopyCount++] = A_As(Promoted ? A_V2[i] : A_V1[i], A_NamesA[i]);
        S->Copy[S->CopyCount++] = A_As(A_V2[i], A_NamesB[i]);
        S->Boot[i] = (Active == EEFS_NAND_SLOT_B) ? A_V2[i] : (Promoted ? A_V2[i] : A_V1[i]);
        S->BootSlot[i] = Active;
    }
    S->BootCount = 3;
}

static int32 A_RunUpload(void)
{
    int32   Status;

    if ((Status = A_UploadActivate()) != EEFS_SUCCESS) return(Status);
    return(EEFS_LibCommit(&V_Vol));
}

static int32 A_RunPromote(void)
{
    int32   Status;

    if ((Status = EEFS_LibCopySlotAll(&V_Vol, EEFS_NAND_SLOT_B, EEFS_NAND_SLOT_A)) != 3) return((Status < 0) ? Status : EEFS_ERROR);
    if ((Status = EEFS_LibSetActiveSlotAll(&V_Vol, EEFS_NAND_SLOT_A)) != 3) return((Status < 0) ? Status : EEFS_ERROR);
    return(EEFS_LibCommit(&V_Vol));
}

static int32 A_RunRollback(void)
{
    int32   Status;

    if ((Status = EEFS_LibSetActiveSlotAll(&V_Vol, EEFS_NAND_SLOT_A)) != 3) return((Status < 0) ? Status : EEFS_ERROR);
    return(EEFS_LibCommit(&V_Vol));
}

/* The image: v1 committed, and with Uploaded v2 uploaded and activated as well */
static void A_MakeImage(int Uploaded)
{
    V_NewVolume();
    A_LoadV1();
    if (Uploaded) CHECK_RC(A_RunUpload(), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    A_Image = V_Sim;
    V_Sim = NULL;
    memset(&V_Vol, 0, sizeof(V_Vol));
}

static void A_ImageFree(void)
{
    if (A_Image != NULL) {
        NandSim_Destroy(A_Image);
        A_Image = NULL;
    }
}

static void ab_powercut_sweep_upload_activate(void)
{
    A_MakeImage(0);
    A_StateV1(&A_Pre);
    A_StateBoth(&A_Post, EEFS_NAND_SLOT_B, 0);
    CHECK(A_Sweep("upload_activate", A_RunUpload) > 0);
    A_ImageFree();
}

static void ab_powercut_sweep_promote_copyslot_all(void)
{
    A_MakeImage(1);
    A_StateBoth(&A_Pre, EEFS_NAND_SLOT_B, 0);
    A_StateBoth(&A_Post, EEFS_NAND_SLOT_A, 1);
    CHECK(A_Sweep("promote", A_RunPromote) > 0);
    CHECK_EQ(A_CleanupCount, 3);                /* the old A first blocks */
    A_ImageFree();
}

static void ab_powercut_sweep_rollback(void)
{
    A_MakeImage(1);
    A_StateBoth(&A_Pre, EEFS_NAND_SLOT_B, 0);
    A_StateBoth(&A_Post, EEFS_NAND_SLOT_A, 0);
    CHECK(A_Sweep("rollback", A_RunRollback) > 0);
    A_ImageFree();
}

/*
 * Case table
 */

const NandTest_Case_t NandTest_AbCases[] = {
    { "ab_upload_activate_compatible_one_commit",           ab_upload_activate_compatible_one_commit },
    { "ab_workflow_upload_activate_promote_rollback",       ab_workflow_upload_activate_promote_rollback },
    { "ab_promote_copyslot_b_to_a",                         ab_promote_copyslot_b_to_a },
    { "ab_rollback_setactive_all_a",                        ab_rollback_setactive_all_a },
    { "ab_explicit_suffix_open_and_stat",                   ab_explicit_suffix_open_and_stat },
    { "ab_active_corrupt_compatible_falls_back",            ab_active_corrupt_compatible_falls_back },
    { "ab_active_corrupt_not_compatible_fails",             ab_active_corrupt_not_compatible_fails },
    { "ab_explicit_name_never_falls_back",                  ab_explicit_name_never_falls_back },
    { "ab_setfailed_persists_and_clears",                   ab_setfailed_persists_and_clears },
    { "ab_remove_slot_rules",                               ab_remove_slot_rules },
    { "ab_copyslot_all_stops_at_first_error",               ab_copyslot_all_stops_at_first_error },
    { "ab_single_copy_opt_in_via_copyslot",                 ab_single_copy_opt_in_via_copyslot },
    { "ab_copyslot_reserve_and_own_blocks",                 ab_copyslot_reserve_and_own_blocks },
    { "ab_copyslot_open_rules",                             ab_copyslot_open_rules },
    { "ab_modify_inactive_slot_leaves_active_untouched",    ab_modify_inactive_slot_leaves_active_untouched },
    { "ab_modify_active_slot_is_safe",                      ab_modify_active_slot_is_safe },
    { "ab_bootloader_agrees_with_library",                  ab_bootloader_agrees_with_library },
    { "ab_rebuild_recovers_locations_versions",             ab_rebuild_recovers_locations_versions },
    { "ab_powercut_sweep_upload_activate",                  ab_powercut_sweep_upload_activate },
    { "ab_powercut_sweep_promote_copyslot_all",             ab_powercut_sweep_promote_copyslot_all },
    { "ab_powercut_sweep_rollback",                         ab_powercut_sweep_rollback },
    { NULL, NULL }
};

/************************/
/*  End of File Comment */
/************************/
