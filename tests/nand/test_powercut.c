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
 * Filename: test_powercut.c
 *
 * Purpose: Suite "powercut": power-cut sweeps over the file operations of the NAND backend (docs/nand-implementation.md
 *   section 14, phase 3): a new file, a copy-on-write modify, Remove, Rename and SetFileAttributes, a mixed batch with
 *   its cleanup, and grown bad blocks during a write, each followed by the commit that makes it durable.
 *
 * Design Notes:
 *   A sweep runs the operation once per cut point (every program and erase it issues, the commit included) and per
 *   torn mode (NONE, PARTIAL, GARBAGE), each time on a fresh clone of the same image.  After the cut: reboot and
 *   remount, which must not program or erase; the directory must be exactly the state before or exactly the state
 *   after the operation (and never "before" again once a smaller cut gave "after"); every file of that state must
 *   read back byte for byte and pass VerifyFile (old content or new content, never corrupt or mixed); no slot may be
 *   flagged; and no metadata page on the device, referenced or orphaned, may list a data block whose content does not
 *   match its CRC (what a directory rebuild relies on).  Then the volume must keep working: a follow-up write and
 *   commit succeed, the pending cleanup is finished (every superseded first block is erased once free), both mirrors
 *   agree, and the files still verify.  The simulator's NAND rule checks must never fire.
 *
 */

/*
 * Includes
 */

#include "nand_vol.h"

/*
 * Macro Definitions
 */

#define P_MAX_CLEANUP       16

/*
 * Type Definitions
 */

typedef int32 (*P_Run_t)(void);
typedef void  (*P_Setup_t)(void);

/*
 * Local Data
 */

static V_File_t     P_PreFiles[V_MAX_FILES];
static uint32       P_PreCount;
static V_File_t     P_PostFiles[V_MAX_FILES];
static uint32       P_PostCount;
static V_Snap_t     P_Pre;
static V_Snap_t     P_Post;
static V_Snap_t     P_Cur;
static uint16       P_Cleanup[P_MAX_CLEANUP];      /* first blocks the operation supersedes */
static uint32       P_CleanupCount;
static NandSim_t   *P_Image;

static const char  *P_ModeName[3] = { "NONE", "PARTIAL", "GARBAGE" };

/* Scenario files */
static V_File_t     P_X, P_Y, P_M, P_M2, P_A, P_A2, P_B, P_B2;
static V_File_t     P_R1, P_R2, P_N, P_N2, P_K, P_New[5];

/*
 * Helpers
 */

static V_File_t P_File(const char *Path, uint32 Size, uint32 Seed)
{
    V_File_t    F;

    memset(&F, 0, sizeof(F));
    F.Path = Path;
    F.Size = Size;
    F.Seed = Seed;
    return(F);
}

static V_File_t P_Mod(V_File_t F, uint32 Offset)
{
    if (F.ModCount < V_MAX_MODS) {
        F.Mod[F.ModCount++] = Offset;
    }
    return(F);
}

/* Starts an image: a fresh formatted volume mounted on V_Vol */
static void P_ImageBegin(void)
{
    V_NewVolume();
}

/* Ends an image: commits, keeps the simulator as P_Image and unmounts */
static void P_ImageEnd(void)
{
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    P_Image = V_Sim;
    V_Sim = NULL;
    memset(&V_Vol, 0, sizeof(V_Vol));
}

static void P_ImageFree(void)
{
    if (P_Image != NULL) {
        NandSim_Destroy(P_Image);
        P_Image = NULL;
    }
}

static int P_InFiles(const char *Path, const V_File_t *Files, uint32 Count)
{
    uint32  i;

    for (i = 0; i < Count; i++) {
        if (strcmp(Files[i].Path, Path) == 0) return(1);
    }
    return(0);
}

/* Every file of the state reads back exactly (Full) or passes VerifyFile (!Full); the files of the other state that
 * this one lacks are absent */
static void P_CheckFiles(int Post, int Full)
{
    const V_File_t *Files = Post ? P_PostFiles : P_PreFiles;
    uint32          Count = Post ? P_PostCount : P_PreCount;
    const V_File_t *Other = Post ? P_PreFiles : P_PostFiles;
    uint32          OtherCount = Post ? P_PreCount : P_PostCount;
    uint32          i;

    for (i = 0; i < Count; i++) {
        if (Full) V_CheckFileEx(&V_Vol, &Files[i], FALSE);     /* exact content through Open/Read */
        else CHECK_RC(EEFS_LibVerifyFile(&V_Vol, (char *)Files[i].Path), EEFS_SUCCESS);
    }
    for (i = 0; i < OtherCount; i++) {
        if (!P_InFiles(Other[i].Path, Files, Count)) V_CheckAbsent(&V_Vol, Other[i].Path);
    }
}

/* After the follow-up commit: a superseded first block is erased iff it is free */
static void P_CheckCleanupDone(void)
{
    uint32  i;
    uint16  b;

    for (i = 0; i < P_CleanupCount; i++) {
        b = P_Cleanup[i];
        if (V_Vol.Working.Map[b] == EEFS_NAND_MAP_FREE) {
            CHECK(NandSim_PageIsErased(V_Sim, b, 0));
        }
        else {
            CHECK(EEFS_NAND_MAP_IS_OWNER(V_Vol.Working.Map[b]));
        }
    }
}

/* The first blocks of the pre state that the post state no longer uses */
static void P_FindCleanup(const EEFS_InodeTable_t *PreVol, const EEFS_InodeTable_t *PostVol)
{
    uint32  i;
    uint32  j;
    uint32  s;
    uint32  t;
    uint16  b;
    int     Used;

    P_CleanupCount = 0;
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        for (s = 0; s < 2; s++) {
            if (!PreVol->Base.Entry[i].InUse || !PreVol->Base.Entry[i].Slot[s].InUse) continue;
            b = PreVol->Base.Entry[i].Slot[s].FirstBlock;
            Used = 0;
            for (j = 0; j < EEFS_MAX_FILES; j++) {
                for (t = 0; t < 2; t++) {
                    if (PostVol->Working.Entry[j].InUse && PostVol->Working.Entry[j].Slot[t].InUse &&
                        (PostVol->Working.Entry[j].Slot[t].FirstBlock == b)) Used = 1;
                }
            }
            if (!Used && (P_CleanupCount < P_MAX_CLEANUP)) P_Cleanup[P_CleanupCount++] = b;
        }
    }
}

/* The expected states: one uninterrupted run on a clone of the image */
static void P_Expect(P_Setup_t Setup, P_Run_t Run)
{
    static EEFS_InodeTable_t    PreVol;

    V_Attach(NandSim_Clone(P_Image));
    CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);
    V_Take(&V_Vol, &P_Pre);
    memcpy(&PreVol, &V_Vol, sizeof(PreVol));
    P_CheckFiles(0, 1);
    P_CheckFiles(0, 0);
    if (Setup != NULL) Setup();
    CHECK_RC(Run(), EEFS_SUCCESS);
    V_Take(&V_Vol, &P_Post);
    CHECK(!V_Same(&P_Pre, &P_Post));
    P_FindCleanup(&PreVol, &V_Vol);
    P_CheckFiles(1, 1);
    P_CheckFiles(1, 0);
    V_Destroy();
}

/* The sweep (see the file header).  Returns the number of cut points of the uninterrupted run. */
static uint32 P_Sweep(const char *Name, P_Setup_t Setup, P_Run_t Run)
{
    V_File_t    Follow = P_File("/follow", 3000, 999);
    uint32      Cut;
    uint32      Cuts = 0;
    uint32      Ops;
    int         Mode;
    int         Done;
    int         SeenPost;
    int         IsPre;
    int         IsPost;
    int32       Status;

    P_Expect(Setup, Run);
    if (NandTest_CaseFailed()) {
        printf("    sweep %s: the uninterrupted run failed\n", Name);
        return(0);
    }

    for (Mode = NANDSIM_TORN_NONE; Mode <= NANDSIM_TORN_GARBAGE; Mode++) {
        SeenPost = 0;
        for (Cut = 0; ; Cut++) {
            V_Attach(NandSim_Clone(P_Image));
            CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);
            if (Setup != NULL) Setup();

            NandSim_SetPowerBudget(V_Sim, Cut, Mode);
            Status = Run();
            Done = !NandSim_PowerCutHappened(V_Sim);
            if (Done) {
                CHECK_RC(Status, EEFS_SUCCESS);
                V_Take(&V_Vol, &P_Cur);
                CHECK(V_Same(&P_Cur, &P_Post));
            }
            NandSim_Reboot(V_Sim);
            EEFS_LibInit();

            /* remount: reads only */
            Ops = V_Ops();
            Status = V_Mount(&V_Vol);
            CHECK_RC(Status, EEFS_SUCCESS);
            CHECK_EQ(V_Ops(), Ops);
            if (Status == EEFS_SUCCESS) {
                V_Take(&V_Vol, &P_Cur);
                IsPre = V_Same(&P_Cur, &P_Pre);
                IsPost = V_Same(&P_Cur, &P_Post);
                CHECK(IsPre || IsPost);
                if (Done) CHECK(IsPost);
                if (IsPost) SeenPost = 1;
                else if (SeenPost) CHECK(!IsPre);       /* never back to "before" after an "after" */
                V_CheckSlotsValid(&V_Vol);
                V_CheckOrphansComplete(&V_Vol);
                P_CheckFiles(IsPost, 1);                /* old or new content, exactly */

                /* the volume keeps working; the cleanup erases must not have touched a live block */
                CHECK_RC(V_Write(&Follow), EEFS_SUCCESS);
                CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
                CHECK_EQ(V_Vol.PendingCleanupCount, 0);
                P_CheckCleanupDone();
                V_CheckMirrorsAgree(&V_Vol);
                P_CheckFiles(IsPost, 1);
                if (((Cut % 8) == 0) || Done) {
                    P_CheckFiles(IsPost, 0);            /* every CRC (the exact reads above already prove the data) */
                    CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/follow"), EEFS_SUCCESS);
                }
            }
            CHECK_EQ(NandSim_Violations(V_Sim), 0);

            if (NandTest_CaseFailed()) {
                printf("    sweep %s: failure at cut %lu, torn mode %s\n", Name, (unsigned long)Cut, P_ModeName[Mode]);
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

static uint32 P_CountBad(const V_Snap_t *S)
{
    uint32  b;
    uint32  n = 0;

    for (b = 0; b < EEFS_NAND_MAX_BLOCKS; b++) {
        if (S->Map[b] == EEFS_NAND_MAP_BAD) n++;
    }
    return(n);
}

static int32 P_Commit(void)
{
    return(EEFS_LibCommit(&V_Vol));
}

/*
 * A new file
 */

static int32 P_RunWriteNew(void)
{
    int32   Status;

    if ((Status = V_Write(&P_Y)) != EEFS_SUCCESS) return(Status);
    return(P_Commit());
}

static void powercut_sweep_write_new(void)
{
    P_X = P_File("/cf/x", V_SizeForBlocks(2), 301);
    P_Y = P_File("/cf/y", V_SizeForBlocks(3) - 100, 302);
    P_ImageBegin();
    CHECK_RC(V_Write(&P_X), EEFS_SUCCESS);
    P_ImageEnd();
    P_PreFiles[0] = P_X;
    P_PreCount = 1;
    P_PostFiles[0] = P_X;
    P_PostFiles[1] = P_Y;
    P_PostCount = 2;
    CHECK(P_Sweep("write_new", NULL, P_RunWriteNew) > 0);
    P_ImageFree();
}

/*
 * A copy-on-write modify
 */

static int32 P_RunModify(void)
{
    int32   Status;

    if ((Status = V_Write(&P_M2)) != EEFS_SUCCESS) return(Status);
    return(P_Commit());
}

static void powercut_sweep_modify(void)
{
    P_M = P_File("/boot/image.fit", V_SizeForBlocks(6), 311);
    P_M2 = P_Mod(P_Mod(P_M, V_FileOffset(2, 1000)), V_FileOffset(4, 7));   /* two positions changed */
    P_ImageBegin();
    CHECK_RC(V_Write(&P_M), EEFS_SUCCESS);
    P_ImageEnd();
    P_PreFiles[0] = P_M;
    P_PreCount = 1;
    P_PostFiles[0] = P_M2;
    P_PostCount = 1;
    CHECK(P_Sweep("modify", NULL, P_RunModify) > 0);
    P_ImageFree();
}

/*
 * Remove, Rename, SetFileAttributes
 */

static int32 P_RunRemove(void)
{
    int32   Status;

    if ((Status = EEFS_LibRemove(&V_Vol, "/a")) != EEFS_SUCCESS) return(Status);
    return(P_Commit());
}

static int32 P_RunRename(void)
{
    int32   Status;

    if ((Status = EEFS_LibRename(&V_Vol, "/b", "/b2")) != EEFS_SUCCESS) return(Status);
    return(P_Commit());
}

static int32 P_RunSetAttr(void)
{
    int32   Status;

    if ((Status = EEFS_LibSetFileAttributes(&V_Vol, "/a", EEFS_ATTRIBUTE_READONLY)) != EEFS_SUCCESS) return(Status);
    return(P_Commit());
}

static void powercut_sweep_remove_rename_setattr(void)
{
    P_A = P_File("/a", V_SizeForBlocks(3), 321);
    P_B = P_File("/b", V_SizeForBlocks(2) - 5, 322);
    P_B2 = P_B;
    P_B2.Path = "/b2";
    P_A2 = P_A;
    P_A2.Attributes = EEFS_ATTRIBUTE_READONLY;
    P_ImageBegin();
    CHECK_RC(V_Write(&P_A), EEFS_SUCCESS);
    CHECK_RC(V_Write(&P_B), EEFS_SUCCESS);
    P_ImageEnd();
    P_PreFiles[0] = P_A;
    P_PreFiles[1] = P_B;
    P_PreCount = 2;

    P_PostFiles[0] = P_B;
    P_PostCount = 1;
    CHECK(P_Sweep("remove", NULL, P_RunRemove) > 0);

    P_PostFiles[0] = P_A;
    P_PostFiles[1] = P_B2;
    P_PostCount = 2;
    if (!NandTest_CaseFailed()) CHECK(P_Sweep("rename", NULL, P_RunRename) > 0);

    P_PostFiles[0] = P_A2;
    P_PostFiles[1] = P_B;
    P_PostCount = 2;
    if (!NandTest_CaseFailed()) CHECK(P_Sweep("setattr", NULL, P_RunSetAttr) > 0);
    P_ImageFree();
}

/*
 * A mixed batch: 5 new files, 2 removes, 1 rename, 1 modify, one commit (and its cleanup)
 */

static int32 P_RunBatch(void)
{
    int32   Status;
    uint32  i;

    for (i = 0; i < 5; i++) {
        if ((Status = V_Write(&P_New[i])) != EEFS_SUCCESS) return(Status);
        if (i == 1) {
            if ((Status = EEFS_LibRemove(&V_Vol, "/r1")) != EEFS_SUCCESS) return(Status);
        }
        if (i == 2) {
            if ((Status = EEFS_LibRename(&V_Vol, "/n", "/n2")) != EEFS_SUCCESS) return(Status);
            if ((Status = EEFS_LibRemove(&V_Vol, "/r2")) != EEFS_SUCCESS) return(Status);
        }
        if (i == 3) {
            if ((Status = V_Write(&P_M2)) != EEFS_SUCCESS) return(Status);
        }
    }
    return(P_Commit());
}

static void powercut_sweep_batch_and_cleanup(void)
{
    static const char  *Paths[5] = { "/new/0", "/new/1", "/new/2", "/new/3", "/new/4" };
    uint32              Sizes[5];
    uint32              i;

    Sizes[0] = 100;
    Sizes[1] = NandTest_Geo.PageSize + 1;
    Sizes[2] = V_D0() + 5;
    Sizes[3] = 300;
    Sizes[4] = 0;
    P_R1 = P_File("/r1", V_SizeForBlocks(2), 331);
    P_R2 = P_File("/r2", 5000, 332);
    P_N = P_File("/n", V_SizeForBlocks(2) + 10, 333);
    P_N2 = P_N;
    P_N2.Path = "/n2";
    P_M = P_File("/m", V_SizeForBlocks(4), 334);
    P_M2 = P_Mod(P_M, V_FileOffset(1, 3));
    P_K = P_File("/k", 2000, 335);
    for (i = 0; i < 5; i++) {
        P_New[i] = P_File(Paths[i], Sizes[i], 340 + i);
    }

    P_ImageBegin();
    CHECK_RC(V_Write(&P_R1), EEFS_SUCCESS);
    CHECK_RC(V_Write(&P_R2), EEFS_SUCCESS);
    CHECK_RC(V_Write(&P_N), EEFS_SUCCESS);
    CHECK_RC(V_Write(&P_M), EEFS_SUCCESS);
    CHECK_RC(V_Write(&P_K), EEFS_SUCCESS);
    P_ImageEnd();
    P_PreFiles[0] = P_R1;
    P_PreFiles[1] = P_R2;
    P_PreFiles[2] = P_N;
    P_PreFiles[3] = P_M;
    P_PreFiles[4] = P_K;
    P_PreCount = 5;
    P_PostFiles[0] = P_N2;
    P_PostFiles[1] = P_M2;
    P_PostFiles[2] = P_K;
    for (i = 0; i < 5; i++) {
        P_PostFiles[3 + i] = P_New[i];
    }
    P_PostCount = 8;
    CHECK(P_Sweep("batch", NULL, P_RunBatch) > 0);
    CHECK(P_CleanupCount == 4);         /* r1, r2, n and m's old first blocks */
    P_ImageFree();
}

/*
 * Grown bad blocks during the write (program failure, erase failure), then the commit that persists the mark
 */

static void P_InjectProgramFail(void)
{
    NandSim_InjectProgramFail(V_Sim, NANDSIM_ANY, 2, 1);
}

static void P_InjectEraseFail(void)
{
    NandSim_InjectEraseFail(V_Sim, NANDSIM_ANY, 1);
}

static void powercut_sweep_grown_bad_during_write(void)
{
    P_M = P_File("/m", V_SizeForBlocks(4), 351);
    P_M2 = P_Mod(P_Mod(P_M, V_FileOffset(1, 9)), V_FileOffset(3, 9));
    P_ImageBegin();
    CHECK_RC(V_Write(&P_M), EEFS_SUCCESS);
    P_ImageEnd();
    P_PreFiles[0] = P_M;
    P_PreCount = 1;
    P_PostFiles[0] = P_M2;
    P_PostCount = 1;
    CHECK(P_Sweep("grown_bad_program", P_InjectProgramFail, P_RunModify) > 0);
    CHECK(P_CountBad(&P_Post) == 1);    /* the grown bad block is part of the committed state */
    if (!NandTest_CaseFailed()) CHECK(P_Sweep("grown_bad_erase", P_InjectEraseFail, P_RunModify) > 0);
    CHECK(P_CountBad(&P_Post) == 1);

    /* a new file as well */
    P_Y = P_File("/y", V_SizeForBlocks(3), 352);
    P_PostFiles[0] = P_M;
    P_PostFiles[1] = P_Y;
    P_PostCount = 2;
    if (!NandTest_CaseFailed()) CHECK(P_Sweep("grown_bad_new", P_InjectProgramFail, P_RunWriteNew) > 0);
    P_ImageFree();
}

/*
 * Case table
 */

const NandTest_Case_t NandTest_PowercutCases[] = {
    { "powercut_sweep_write_new",                       powercut_sweep_write_new },
    { "powercut_sweep_modify",                          powercut_sweep_modify },
    { "powercut_sweep_remove_rename_setattr",           powercut_sweep_remove_rename_setattr },
    { "powercut_sweep_batch_and_cleanup",               powercut_sweep_batch_and_cleanup },
    { "powercut_sweep_grown_bad_during_write",          powercut_sweep_grown_bad_during_write },
    { NULL, NULL }
};

/************************/
/*  End of File Comment */
/************************/
