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
 * Filename: test_fileops.c
 *
 * Purpose: Suite "fileops": the file operations of the NAND backend (docs/nand-implementation.md section 6, phase 3):
 *   whole-file writes with copy-on-write, the allocator and its reserve classes, Remove, the first-block rewrite of
 *   Rename and SetFileAttributes, Open/Read/LSeek, Stat, ReadDir, VerifyFile, GetFileInfo, GetVolumeInfo, grown bad
 *   blocks during writes, the driver path rules and mount reconcile.
 *
 * Design Notes:
 *   Counts (erases, programs, reads) are asserted exactly where the spec pins them.  The space cases run on a small
 *   device (EEFS_NAND_RESERVE_BLOCKS + 5 data blocks) so that the reserve levels are reached with a few files.
 *   open_checks_page0_only_by_default and open_verifies_data_when_enabled are compiled for the build's
 *   EEFS_NAND_VERIFY_ON_OPEN setting (the second runs in eefs_nand_test_verifyopen).
 *
 */

/*
 * Includes
 */

#include <stdlib.h>
#include "nand_vol.h"
#include "eefs_filesys.h"

/*
 * Macro Definitions
 */

#define T_MAXB              1200

/*
 * Local Data
 */

static uint16       OldB[T_MAXB];
static uint32       OldC[T_MAXB];
static uint16       NewB[T_MAXB];
static uint32       NewC[T_MAXB];

extern int32        EEFS_SplitPath(char *InputPath, EEFS_SplitPath_t *SplitPath);

/*
 * Helpers
 */

static V_File_t T_File(const char *Path, uint32 Size, uint32 Seed)
{
    V_File_t    F;

    memset(&F, 0, sizeof(F));
    F.Path = Path;
    F.Size = Size;
    F.Seed = Seed;
    return(F);
}

static V_File_t T_Mod(V_File_t F, uint32 Offset)
{
    if (F.ModCount < V_MAX_MODS) {
        F.Mod[F.ModCount++] = Offset;
    }
    return(F);
}

static uint32 T_Dir(void)
{
    return(NandTest_Geo.DirRegionBlocks);
}

static uint32 T_PPB(void)
{
    return(NandTest_Geo.PagesPerBlock);
}

static uint32 T_ProgrammedPages(uint32 Block)
{
    uint32  p;
    uint32  n = 0;

    for (p = 0; p < NandTest_Geo.PagesPerBlock; p++) {
        if (NandSim_PageIsProgrammed(V_Sim, Block, p)) n++;
    }
    return(n);
}

static uint32 T_DirProgrammedPages(void)
{
    uint32  b;
    uint32  n = 0;

    for (b = 0; b < V_Dev.Geometry.DirRegionBlocks; b++) {
        n += T_ProgrammedPages(b);
    }
    return(n);
}

static uint32 T_DirErases(void)
{
    uint32  b;
    uint32  n = 0;

    for (b = 0; b < V_Dev.Geometry.DirRegionBlocks; b++) {
        n += NandSim_BlockErases(V_Sim, b);
    }
    return(n);
}

static uint32 T_CountMap(uint8 Value)
{
    uint32  b;
    uint32  n = 0;

    for (b = V_Dev.Geometry.DirRegionBlocks; b < V_Dev.Geometry.BlockCount; b++) {
        if (V_Vol.Working.Map[b] == Value) n++;
    }
    return(n);
}

static uint32 T_FirstBad(void)
{
    uint32  b;

    for (b = V_Dev.Geometry.DirRegionBlocks; b < V_Dev.Geometry.BlockCount; b++) {
        if (V_Vol.Working.Map[b] == EEFS_NAND_MAP_BAD) return(b);
    }
    return(0);
}

/* Pages a file of Size bytes programs: page 0 plus every data page */
static uint32 T_PagesForSize(uint32 Size)
{
    uint32  Need = EEFS_NandBlocksNeeded(&V_Vol, Size);
    uint32  k;
    uint32  Len;
    uint32  n = 1;

    for (k = 0; k < Need; k++) {
        Len = EEFS_NandBlockDataRange(NandTest_Geo.PageSize, NandTest_Geo.PagesPerBlock, Size, k, NULL);
        n += (Len + NandTest_Geo.PageSize - 1) / NandTest_Geo.PageSize;
    }
    return(n);
}

static int32 T_Commit(void)
{
    return(EEFS_LibCommit(&V_Vol));
}

static int T_InList(uint16 Block, const uint16 *List, uint32 Count)
{
    uint32  i;

    for (i = 0; i < Count; i++) {
        if (List[i] == Block) return(1);
    }
    return(0);
}

/* Small device: EEFS_NAND_RESERVE_BLOCKS + 5 data blocks; "/f" (3 blocks) committed, so FreeCount = RESERVE + 2 */
static V_File_t T_SpaceStart(void)
{
    V_File_t    F = T_File("/f", V_SizeForBlocks(3), 11);

    V_NewVolumeBlocks(T_Dir() + EEFS_NAND_RESERVE_BLOCKS + 5);
    CHECK_RC(V_Write(&F), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(EEFS_NandFreeCount(&V_Vol), EEFS_NAND_RESERVE_BLOCKS + 2);
    return(F);
}

/* From T_SpaceStart: a filler file grown with modify-class allocations until FreeCount = BADBLOCK_RESERVE + 1 */
static V_File_t T_SpaceFill(void)
{
    V_File_t    Fill = T_File("/fill", V_SizeForBlocks(1), 12);
    uint32      Free;
    uint32      N;

    CHECK_RC(V_Write(&Fill), EEFS_SUCCESS);                 /* new class: leaves RESERVE + 1 */
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    Free = EEFS_NandFreeCount(&V_Vol);
    N = Free + 1 - (EEFS_NAND_BADBLOCK_RESERVE + 1);
    Fill = T_File("/fill", V_SizeForBlocks(N), 13);         /* every position new: N blocks, modify class */
    CHECK_RC(V_Write(&Fill), EEFS_SUCCESS);
    CHECK_EQ(EEFS_NandFreeCount(&V_Vol), EEFS_NAND_BADBLOCK_RESERVE);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);                     /* the old first block of the filler is free again */
    CHECK_EQ(EEFS_NandFreeCount(&V_Vol), EEFS_NAND_BADBLOCK_RESERVE + 1);
    return(Fill);
}

static void T_CheckType(uint8 FileType, const EEFS_NandTypeInfo_t *A, const EEFS_NandTypeInfo_t *B)
{
    switch (FileType) {
        case EEFS_FILE_TYPE_BOOT_FIT:
            CHECK(A->Fit.DestAddress == B->Fit.DestAddress);
            break;
        case EEFS_FILE_TYPE_BOOT_ELF:
            CHECK(A->Elf.DestAddress == B->Elf.DestAddress);
            CHECK(A->Elf.EntryPoint == B->Elf.EntryPoint);
            CHECK_EQ(A->Elf.Flags, B->Elf.Flags);
            break;
        case EEFS_FILE_TYPE_BOOT_DTB:
            CHECK(A->Dtb.DestAddress == B->Dtb.DestAddress);
            break;
        case EEFS_FILE_TYPE_CFS_APP:
            CHECK(strcmp(A->App.AppName, B->App.AppName) == 0);
            CHECK(strcmp(A->App.EntrySymbol, B->App.EntrySymbol) == 0);
            CHECK_EQ(A->App.Priority, B->App.Priority);
            CHECK_EQ(A->App.ExceptionAction, B->App.ExceptionAction);
            CHECK_EQ(A->App.StackSize, B->App.StackSize);
            CHECK(A->App.LoadAddress == B->App.LoadAddress);
            break;
        case EEFS_FILE_TYPE_CFS_LIB:
            CHECK(strcmp(A->Lib.LibName, B->Lib.LibName) == 0);
            CHECK(strcmp(A->Lib.EntrySymbol, B->Lib.EntrySymbol) == 0);
            CHECK(A->Lib.LoadAddress == B->Lib.LoadAddress);
            break;
        default:
            break;
    }
}

/*
 * Writes and reads
 */

static void write_read_sizes(void)
{
    static char     Paths[7][16];
    V_File_t        F[7];
    uint32          Sizes[7];
    uint32          i;
    uint32          k;
    uint32          n;
    uint32          Len;
    uint32          Programs;
    uint32          Erases;
    const EEFS_NandSlot_t *S;

    V_NewVolume();
    Sizes[0] = 0;
    Sizes[1] = 1;
    Sizes[2] = NandTest_Geo.PageSize - 1;
    Sizes[3] = NandTest_Geo.PageSize;
    Sizes[4] = V_D0();
    Sizes[5] = V_D0() + 1;
    Sizes[6] = 3 * V_BlockBytes() + 17;

    for (i = 0; i < 7; i++) {
        sprintf(Paths[i], "/sizes/%lu", (unsigned long)i);
        F[i] = T_File(Paths[i], Sizes[i], i + 1);
        Programs = NandSim_Programs(V_Sim);
        Erases = NandSim_Erases(V_Sim);
        CHECK_RC(V_Write(&F[i]), EEFS_SUCCESS);
        CHECK_EQ(NandSim_Programs(V_Sim) - Programs, T_PagesForSize(Sizes[i]));
        CHECK_EQ(NandSim_Erases(V_Sim) - Erases, EEFS_NandBlocksNeeded(&V_Vol, Sizes[i]));

        /* exactly page 0 and the data pages are programmed, in order; pages after the data stay erased */
        S = V_SlotA(&V_Vol, Paths[i]);
        CHECK_EQ(S->AllocBlocks, EEFS_NandBlocksNeeded(&V_Vol, Sizes[i]));
        n = V_RawBlocks(S->FirstBlock, NewB, NewC);
        CHECK_EQ(n, S->AllocBlocks);
        for (k = 0; k < n; k++) {
            Len = EEFS_NandBlockDataRange(NandTest_Geo.PageSize, NandTest_Geo.PagesPerBlock, Sizes[i], k, NULL);
            CHECK_EQ(T_ProgrammedPages(NewB[k]), ((k == 0) ? 1 : 0) + (Len + NandTest_Geo.PageSize - 1) / NandTest_Geo.PageSize);
        }
        V_CheckFile(&V_Vol, &F[i]);
    }
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    V_Remount();
    V_CheckSlotsValid(&V_Vol);
    for (i = 0; i < 7; i++) {
        V_CheckFile(&V_Vol, &F[i]);
    }
    CHECK_EQ(NandSim_Violations(V_Sim), 0);
    V_Destroy();
}

static void write_all_types_roundtrip_via_getfileinfo(void)
{
    static char             Paths[6][16];
    EEFS_NandFileInfo_t     Meta[6];
    EEFS_NandFileInfo_t     Info;
    uint32                  t;
    uint32                  Pass;

    V_NewVolume();
    for (t = 0; t < 6; t++) {
        sprintf(Paths[t], "/types/t%lu", (unsigned long)t);
        V_Meta(&Meta[t], (uint8)t, 100 + t);
        Meta[t].Attributes = (t == 5) ? EEFS_ATTRIBUTE_READONLY : EEFS_ATTRIBUTE_NONE;
        switch (t) {
            case EEFS_FILE_TYPE_BOOT_FIT:
                Meta[t].Type.Fit.DestAddress = 0x80000000UL + t;
                break;
            case EEFS_FILE_TYPE_BOOT_ELF:
                Meta[t].Type.Elf.DestAddress = 0x40000000UL;
                Meta[t].Type.Elf.EntryPoint = 0x40001000UL;
                Meta[t].Type.Elf.Flags = 1;
                break;
            case EEFS_FILE_TYPE_BOOT_DTB:
                Meta[t].Type.Dtb.DestAddress = 0x1F000000UL;
                break;
            case EEFS_FILE_TYPE_CFS_APP:
                strcpy(Meta[t].Type.App.AppName, "SAMPLE_APP");
                strcpy(Meta[t].Type.App.EntrySymbol, "SAMPLE_AppMain");
                Meta[t].Type.App.Priority = 50;
                Meta[t].Type.App.ExceptionAction = 1;
                Meta[t].Type.App.StackSize = 16384;
                Meta[t].Type.App.LoadAddress = 0x100000UL;
                break;
            case EEFS_FILE_TYPE_CFS_LIB:
                strcpy(Meta[t].Type.Lib.LibName, "SAMPLE_LIB");
                strcpy(Meta[t].Type.Lib.EntrySymbol, "SAMPLE_LibInit");
                Meta[t].Type.Lib.LoadAddress = 0x200000UL;
                break;
            default:
                break;
        }
        memset(V_Data, (int)(0x30 + t), 1000 + t);
        CHECK_RC(EEFS_LibWriteFile(&V_Vol, Paths[t], &Meta[t], V_Data, 1000 + t), EEFS_SUCCESS);
    }

    for (Pass = 0; Pass < 2; Pass++) {
        for (t = 0; t < 6; t++) {
            memset(&Info, 0xEE, sizeof(Info));
            CHECK_RC(EEFS_LibGetFileInfo(&V_Vol, Paths[t], &Info), EEFS_SUCCESS);
            CHECK(strcmp(Info.Path, Paths[t]) == 0);
            CHECK_EQ(Info.FileType, t);
            CHECK_EQ(Info.Version, 100 + t);
            CHECK_EQ(Info.Attributes, Meta[t].Attributes);
            CHECK_EQ(Info.FileSize, 1000 + t);
            memset(V_Data, (int)(0x30 + t), 1000 + t);
            CHECK_EQ(Info.FileCrc32, EEFS_Crc32(V_Data, 1000 + t));
            CHECK_EQ(Info.Slot, EEFS_NAND_SLOT_A);
            CHECK_EQ(Info.IsAB, 0);
            CHECK_EQ(Info.SlotInfo[0].InUse, 1);
            CHECK_EQ(Info.SlotInfo[1].InUse, 0);
            CHECK_EQ(Info.SlotInfo[0].Corrupt, 0);
            T_CheckType((uint8)t, &Info.Type, &Meta[t].Type);
        }
        CHECK_RC(T_Commit(), EEFS_SUCCESS);
        V_Remount();
    }
    V_Destroy();
}

static void path_255_ok_256_rejected(void)
{
    static char Path255[260];
    static char Path256[260];
    static char Name257[262];
    EEFS_Stat_t St;
    V_File_t    F;

    V_NewVolume();
    Path255[0] = '/';
    memset(&Path255[1], 'a', 254);
    Path255[255] = '\0';
    Path256[0] = '/';
    memset(&Path256[1], 'b', 255);
    Path256[256] = '\0';
    strcpy(Name257, Path255);
    strcat(Name257, "@A");

    F = T_File(Path255, 100, 1);
    CHECK_RC(V_Write(&F), EEFS_SUCCESS);
    V_CheckFile(&V_Vol, &F);
    CHECK_RC(EEFS_LibStat(&V_Vol, Name257, &St), EEFS_SUCCESS);     /* 255 + "@A": the suffix is not stored */
    CHECK_EQ(strlen(St.Filename), 255);

    F = T_File(Path256, 100, 2);
    CHECK_RC(V_Write(&F), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibStat(&V_Vol, Path256, &St), EEFS_INVALID_ARGUMENT);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    V_Remount();
    F = T_File(Path255, 100, 1);
    V_CheckFile(&V_Vol, &F);
    V_Destroy();
}

static void name_rules(void)
{
    static const char  *Bad[] = { "noslash", "", "@A", "/a@b@A", "/x@C", "/x@", "/x@a", "/a@b", "x@A" };
    char                Path[EEFS_NAND_MAX_PATH_SIZE];
    uint8               Spec;
    uint32              i;
    V_File_t            F;
    EEFS_Stat_t         St;

    V_NewVolume();
    for (i = 0; i < sizeof(Bad) / sizeof(Bad[0]); i++) {
        CHECK_RC(EEFS_NandParseName(Bad[i], Path, &Spec), EEFS_INVALID_ARGUMENT);
        F = T_File(Bad[i], 10, 1);
        CHECK_RC(V_Write(&F), EEFS_INVALID_ARGUMENT);
    }
    CHECK_RC(EEFS_NandParseName("/x@A", Path, &Spec), EEFS_SUCCESS);
    CHECK(strcmp(Path, "/x") == 0);
    CHECK_EQ(Spec, EEFS_NAND_SLOT_A);
    CHECK_RC(EEFS_NandParseName("/cf/apps/x.so@B", Path, &Spec), EEFS_SUCCESS);
    CHECK(strcmp(Path, "/cf/apps/x.so") == 0);
    CHECK_EQ(Spec, EEFS_NAND_SLOT_B);
    CHECK_RC(EEFS_NandParseName("/cf/apps/x.so", Path, &Spec), EEFS_SUCCESS);
    CHECK_EQ(Spec, EEFS_NAND_SLOT_NONE);

    /* an explicit slot works (A/B behaviour beyond that is phase 5) */
    F = T_File("/ok", 10, 3);
    CHECK_RC(V_WriteName("/ok@B", &F), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibStat(&V_Vol, "/ok@B", &St), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibStat(&V_Vol, "/ok@A", &St), EEFS_FILE_NOT_FOUND);
    CHECK_RC(EEFS_LibStat(&V_Vol, "/ok", &St), EEFS_SUCCESS);       /* a B-only file: B is its active slot */
    CHECK_EQ(V_Vol.Working.Entry[V_Entry(&V_Vol, "/ok")].IsAB, 1);
    CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/ok@B"), EEFS_SUCCESS);
    V_Destroy();
}

static void signature_block_present_and_excluded(void)
{
    V_File_t                F = T_File("/sig", 5000, 4);
    const EEFS_NandSlot_t  *S;
    uint32                  i;
    uint32                  NotFF = 0;
    int32                   Fd;

    V_NewVolume();
    CHECK_RC(V_Write(&F), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    S = V_SlotA(&V_Vol, "/sig");
    NandSim_RawRead(V_Sim, S->FirstBlock, 0, V_Page, NULL);
    CHECK_EQ(EEFS_NandGet16(&V_Page[EEFS_NAND_P0_OFF_SIGTYPE]), 0);
    CHECK_EQ(EEFS_NandGet32(&V_Page[EEFS_NAND_P0_OFF_SIGLEN]), 0);
    for (i = EEFS_NAND_P0_OFF_SIG; i < EEFS_NAND_P0_SIG_END; i++) {
        if (V_Page[i] != 0xFF) NotFF++;
    }
    CHECK_EQ(NotFF, 0);

    /* the signature bytes are outside HeaderCrc32: changing them keeps the page valid */
    NandSim_CorruptBytes(V_Sim, S->FirstBlock, 0, EEFS_NAND_P0_OFF_SIG + 100, 16);
    EEFS_NandInvalidateCache(&V_Vol);
    CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/sig"), EEFS_SUCCESS);
    Fd = EEFS_LibOpen(&V_Vol, "/sig", O_RDONLY, 0);
    CHECK(Fd >= 0);
    if (Fd >= 0) CHECK_RC(EEFS_LibClose(Fd), EEFS_SUCCESS);

    /* any other header byte is covered */
    NandSim_CorruptBytes(V_Sim, S->FirstBlock, 0, EEFS_NAND_P0_OFF_MDATE, 1);
    EEFS_NandInvalidateCache(&V_Vol);
    CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/sig"), EEFS_CORRUPT_FILE);
    CHECK_RC(EEFS_LibOpen(&V_Vol, "/sig", O_RDONLY, 0), EEFS_CORRUPT_FILE);
    V_Destroy();
}

/*
 * Copy-on-write
 */

/* A committed 6-block file "/cow" and its blocks in OldB/OldC */
static V_File_t T_Cow6(void)
{
    V_File_t    F = T_File("/cow", V_SizeForBlocks(6), 7);

    V_NewVolume();
    CHECK_RC(V_Write(&F), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/cow")->FirstBlock, OldB, OldC), 6);
    return(F);
}

static void cow_modify_keeps_unchanged_blocks(void)
{
    V_File_t    F = T_Cow6();
    V_File_t    F2 = T_Mod(F, V_FileOffset(3, 100));
    uint32      Programs = NandSim_Programs(V_Sim);
    uint32      Erases = NandSim_Erases(V_Sim);
    uint32      DirPages = T_DirProgrammedPages();
    uint32      DirErases = T_DirErases();
    uint32      k;
    uint16      NewFirst;

    CHECK_EQ(V_Vol.PendingCleanupCount, 0);
    CHECK_RC(V_Write(&F2), EEFS_SUCCESS);

    /* exactly two blocks erased and programmed: the new first block and position 3's replacement */
    CHECK_EQ(NandSim_Erases(V_Sim) - Erases, 2);
    CHECK_EQ(NandSim_Programs(V_Sim) - Programs, 2 * T_PPB());
    NewFirst = V_SlotA(&V_Vol, "/cow")->FirstBlock;
    CHECK(NewFirst != OldB[0]);
    CHECK_EQ(V_RawBlocks(NewFirst, NewB, NewC), 6);
    for (k = 1; k < 6; k++) {
        if (k == 3) {
            CHECK(NewB[k] != OldB[k]);
            CHECK(NewC[k] != OldC[k]);
        }
        else {
            CHECK_EQ(NewB[k], OldB[k]);
            CHECK_EQ(NewC[k], OldC[k]);
        }
    }
    CHECK_EQ(NewC[0], OldC[0]);
    CHECK_EQ(V_Vol.Working.Map[OldB[0]], EEFS_NAND_MAP_FREEING);
    CHECK_EQ(V_Vol.Working.Map[OldB[3]], EEFS_NAND_MAP_FREEING);
    CHECK_EQ(T_CountMap(EEFS_NAND_MAP_FREEING), 2);
    CHECK_EQ(V_Vol.PendingCleanupCount, 1);
    CHECK_EQ(V_Vol.PendingCleanup[0], OldB[0]);
    CHECK_EQ(T_DirProgrammedPages(), DirPages);         /* the directory is not touched before the commit */
    CHECK_EQ(T_DirErases(), DirErases);
    V_CheckFile(&V_Vol, &F2);

    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK(NandSim_PageIsErased(V_Sim, OldB[0], 0));     /* the old first block went through the cleanup */
    CHECK_EQ(V_Vol.Working.Map[OldB[0]], EEFS_NAND_MAP_FREE);
    CHECK_EQ(V_Vol.Working.Map[OldB[3]], EEFS_NAND_MAP_FREE);
    CHECK(NandSim_PageIsProgrammed(V_Sim, OldB[3], 0)); /* data blocks are not erased until reused */
    V_CheckFile(&V_Vol, &F2);
    V_Remount();
    V_CheckSlotsValid(&V_Vol);
    V_CheckFile(&V_Vol, &F2);
    CHECK_EQ(NandSim_Violations(V_Sim), 0);
    V_Destroy();
}

/* Reads of a rewrite: the old page 0 (at most 1), the byte compare of the kept candidates, the verify read-backs */
static void T_RewriteReads(uint32 *Reads, uint32 *Programs, V_File_t *F2)
{
    uint32  R = NandSim_Reads(V_Sim);
    uint32  P = NandSim_Programs(V_Sim);

    CHECK_RC(V_Write(F2), EEFS_SUCCESS);
    *Reads = NandSim_Reads(V_Sim) - R;
    *Programs = NandSim_Programs(V_Sim) - P;
#if EEFS_NAND_VERIFY_WRITES
    *Reads -= *Programs;        /* every programmed page is read back once */
#endif
}

static void cow_modify_byte_compare_reads_kept_blocks(void)
{
    V_File_t    F = T_Cow6();
    V_File_t    F2 = T_Mod(F, V_FileOffset(3, 100));
    uint32      Reads;
    uint32      Programs;

    /* positions 1, 2, 4 and 5 have the same CRC: every one of their data pages is read and compared */
    T_RewriteReads(&Reads, &Programs, &F2);
    CHECK(Reads >= 4 * T_PPB());
    CHECK_EQ(Programs, 2 * T_PPB());
    V_CheckFile(&V_Vol, &F2);
    V_Destroy();
}

static void cow_modify_changed_crc_skips_compare(void)
{
    V_File_t    F = T_Cow6();
    V_File_t    F2 = T_Mod(T_Mod(F, V_FileOffset(3, 100)), V_FileOffset(4, 7));
    uint32      Reads;
    uint32      Programs;

    /* positions 3 and 4 changed their CRC: they are not read; only 1, 2 and 5 are (plus page 0 at most once) */
    T_RewriteReads(&Reads, &Programs, &F2);
    CHECK(Reads >= 3 * T_PPB());
    CHECK(Reads <= 3 * T_PPB() + 1);
    CHECK_EQ(Programs, 3 * T_PPB());
    V_CheckFile(&V_Vol, &F2);
    V_Destroy();
}

/* The CRC of a decayed block still matches the intended content, but its bytes do not: the byte compare must catch
 * it, or the "unchanged" block would be kept and the new copy would carry the damage */
static void cow_modify_rewrites_decayed_block(void)
{
    V_File_t    F = T_Cow6();
    uint32      Erases;
    uint32      k;

    NandSim_CorruptBytes(V_Sim, OldB[2], 5, 10, 4);
    EEFS_NandInvalidateCache(&V_Vol);
    CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/cow"), EEFS_CORRUPT_FILE);

    Erases = NandSim_Erases(V_Sim);
    CHECK_RC(V_Write(&F), EEFS_SUCCESS);                /* the same, intended content */
    CHECK_EQ(NandSim_Erases(V_Sim) - Erases, 2);        /* new first block + position 2 */
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/cow")->FirstBlock, NewB, NewC), 6);
    for (k = 1; k < 6; k++) {
        CHECK_EQ(NewC[k], OldC[k]);
        if (k == 2) CHECK(NewB[k] != OldB[k]);
        else CHECK_EQ(NewB[k], OldB[k]);
    }
    V_CheckFile(&V_Vol, &F);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    V_Remount();
    V_CheckFile(&V_Vol, &F);
    V_Destroy();
}

static void cow_modify_grow_and_shrink(void)
{
    V_File_t    F4 = T_File("/gs", V_SizeForBlocks(4), 9);
    V_File_t    G = T_File("/gs", V_SizeForBlocks(6), 9);           /* same bytes, two more blocks */
    V_File_t    S = T_File("/gs", V_FileOffset(3, 100), 9);         /* 4 blocks, position 3 shortened */
    uint32      Erases;
    uint32      k;

    V_NewVolume();
    CHECK_RC(V_Write(&F4), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/gs")->FirstBlock, OldB, OldC), 4);

    /* grow: positions 1..3 kept, 4 and 5 appended */
    Erases = NandSim_Erases(V_Sim);
    CHECK_RC(V_Write(&G), EEFS_SUCCESS);
    CHECK_EQ(NandSim_Erases(V_Sim) - Erases, 3);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/gs")->FirstBlock, NewB, NewC), 6);
    for (k = 1; k < 4; k++) {
        CHECK_EQ(NewB[k], OldB[k]);
    }
    CHECK(!T_InList(NewB[4], OldB, 4) && !T_InList(NewB[5], OldB, 4));
    CHECK_EQ(V_Vol.Working.Map[OldB[0]], EEFS_NAND_MAP_FREEING);
    CHECK_EQ(T_CountMap(EEFS_NAND_MAP_FREEING), 1);
    V_CheckFile(&V_Vol, &G);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    memcpy(OldB, NewB, sizeof(NewB));

    /* shrink: 1 and 2 kept, 3 rewritten (its length differs), 4 and 5 dropped */
    Erases = NandSim_Erases(V_Sim);
    CHECK_RC(V_Write(&S), EEFS_SUCCESS);
    CHECK_EQ(NandSim_Erases(V_Sim) - Erases, 2);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/gs")->FirstBlock, NewB, NewC), 4);
    CHECK_EQ(NewB[1], OldB[1]);
    CHECK_EQ(NewB[2], OldB[2]);
    CHECK(NewB[3] != OldB[3]);
    CHECK_EQ(V_Vol.Working.Map[OldB[0]], EEFS_NAND_MAP_FREEING);
    CHECK_EQ(V_Vol.Working.Map[OldB[3]], EEFS_NAND_MAP_FREEING);
    CHECK_EQ(V_Vol.Working.Map[OldB[4]], EEFS_NAND_MAP_FREEING);
    CHECK_EQ(V_Vol.Working.Map[OldB[5]], EEFS_NAND_MAP_FREEING);
    CHECK_EQ(T_CountMap(EEFS_NAND_MAP_FREEING), 4);
    CHECK_EQ(V_Vol.Working.Entry[V_Entry(&V_Vol, "/gs")].Slot[0].AllocBlocks, 4);
    V_CheckFile(&V_Vol, &S);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.Working.Map[OldB[4]], EEFS_NAND_MAP_FREE);
    V_Remount();
    V_CheckSlotsValid(&V_Vol);
    V_CheckFile(&V_Vol, &S);
    V_Destroy();
}

static void cow_modify_of_corrupt_slot_rewrites_all(void)
{
    V_File_t    F = T_File("/bad", V_SizeForBlocks(4), 21);
    uint32      Erases;
    uint32      k;
    int32       i;

    V_NewVolume();
    CHECK_RC(V_Write(&F), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/bad")->FirstBlock, OldB, OldC), 4);
    NandSim_CorruptBytes(V_Sim, OldB[0], 0, EEFS_NAND_P0_OFF_PATH + 3, 2);
    V_Remount();
    i = V_Entry(&V_Vol, "/bad");
    CHECK(i >= 0);
    if (i < 0) { V_Destroy(); return; }
    CHECK((V_Vol.SlotFlags[i][0] & EEFS_NAND_SLOTFLAG_CORRUPT) != 0);

    Erases = NandSim_Erases(V_Sim);
    CHECK_RC(V_Write(&F), EEFS_SUCCESS);
    CHECK_EQ(NandSim_Erases(V_Sim) - Erases, 4);        /* nothing of the corrupt copy is kept */
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/bad")->FirstBlock, NewB, NewC), 4);
    for (k = 0; k < 4; k++) {
        CHECK(!T_InList(NewB[k], OldB, 4));
        CHECK_EQ(V_Vol.Working.Map[OldB[k]], EEFS_NAND_MAP_FREEING);
    }
    CHECK_EQ(V_Vol.SlotFlags[i][0], 0);
    V_CheckFile(&V_Vol, &F);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    V_Remount();
    V_CheckSlotsValid(&V_Vol);
    V_CheckFile(&V_Vol, &F);
    V_Destroy();
}

/*
 * Space: reserve classes
 */

static void cow_modify_no_space_no_change(void)
{
    static EEFS_NandEntry_t Entries[EEFS_MAX_FILES];
    static uint8            Map[EEFS_NAND_MAX_BLOCKS];
    V_File_t                F = T_SpaceStart();
    V_File_t                F2;
    V_File_t                F3;
    uint32                  Ops;
    uint16                  Cleanup;

    (void)T_SpaceFill();
    CHECK_EQ(EEFS_NandFreeCount(&V_Vol), EEFS_NAND_BADBLOCK_RESERVE + 1);

    /* needs 2 new blocks (first + position 1): would leave BADBLOCK_RESERVE - 1 */
    F2 = T_Mod(T_Mod(F, V_FileOffset(0, 10)), V_FileOffset(1, 10));
    memcpy(Entries, V_Vol.Working.Entry, sizeof(Entries));
    memcpy(Map, V_Vol.Working.Map, sizeof(Map));
    Ops = V_Ops();
    Cleanup = V_Vol.PendingCleanupCount;
    CHECK_RC(V_Write(&F2), EEFS_NO_SPACE_LEFT_ON_DEVICE);
    CHECK_EQ(V_Ops(), Ops);
    CHECK(memcmp(Entries, V_Vol.Working.Entry, sizeof(Entries)) == 0);
    CHECK(memcmp(Map, V_Vol.Working.Map, sizeof(Map)) == 0);
    CHECK_EQ(V_Vol.PendingCleanupCount, Cleanup);
    V_CheckFile(&V_Vol, &F);

    /* needs 1 (only the first block's data changed): allowed down to BADBLOCK_RESERVE */
    F3 = T_Mod(F, V_FileOffset(0, 10));
    CHECK_RC(V_Write(&F3), EEFS_SUCCESS);
    CHECK_EQ(EEFS_NandFreeCount(&V_Vol), EEFS_NAND_BADBLOCK_RESERVE);
    V_CheckFile(&V_Vol, &F3);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    V_Remount();
    V_CheckFile(&V_Vol, &F3);
    V_Destroy();
}

static void reserve_new_file_vs_modify(void)
{
    V_File_t    F = T_SpaceStart();
    V_File_t    N = T_File("/new", V_SizeForBlocks(3), 31);
    V_File_t    F2 = T_Mod(T_Mod(T_Mod(F, V_FileOffset(0, 1)), V_FileOffset(1, 1)), V_FileOffset(2, 1));
    V_File_t    Fill;
    V_File_t    Fill2;
    uint32      Ops;

    /* FreeBlocks == RESERVE + 2: a 3-block new file would leave RESERVE - 1 */
    Ops = V_Ops();
    CHECK_RC(V_Write(&N), EEFS_NO_SPACE_LEFT_ON_DEVICE);
    CHECK_EQ(V_Ops(), Ops);
    V_CheckAbsent(&V_Vol, "/new");
    CHECK_EQ(V_Entry(&V_Vol, "/new"), EEFS_FILE_NOT_FOUND);

    /* a 3-block modify (every position changed) may use the reserve */
    CHECK_RC(V_Write(&F2), EEFS_SUCCESS);
    CHECK_EQ(EEFS_NandFreeCount(&V_Vol), EEFS_NAND_RESERVE_BLOCKS - 1);
    V_CheckFile(&V_Vol, &F2);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(EEFS_NandFreeCount(&V_Vol), EEFS_NAND_RESERVE_BLOCKS + 2);

    /* down to BADBLOCK_RESERVE: a 1-block modify fails */
    Fill = T_SpaceFill();
    F = T_Mod(F2, V_FileOffset(0, 50));
    CHECK_RC(V_Write(&F), EEFS_SUCCESS);
    CHECK_EQ(EEFS_NandFreeCount(&V_Vol), EEFS_NAND_BADBLOCK_RESERVE);
    Fill2 = T_Mod(Fill, V_FileOffset(0, 50));
    Ops = V_Ops();
    CHECK_RC(V_Write(&Fill2), EEFS_NO_SPACE_LEFT_ON_DEVICE);
    CHECK_EQ(V_Ops(), Ops);
    V_CheckFile(&V_Vol, &Fill);
    V_CheckFile(&V_Vol, &F);
    V_Destroy();
}

static void reserve_badblock_replacement_may_use_everything(void)
{
    V_File_t    F = T_SpaceStart();
    V_File_t    F2;
    uint32      Bad;
    uint8       Oob[EEFS_NAND_MAX_OOB_SIZE];

    (void)T_SpaceFill();
    CHECK_EQ(EEFS_NandFreeCount(&V_Vol), EEFS_NAND_BADBLOCK_RESERVE + 1);

    /* a 1-block modify is allowed; its new block fails to program and the replacement takes a reserve block */
    F2 = T_Mod(F, V_FileOffset(0, 77));
    NandSim_InjectProgramFail(V_Sim, NANDSIM_ANY, 0, 1);
    CHECK_RC(V_Write(&F2), EEFS_SUCCESS);
    CHECK_EQ(T_CountMap(EEFS_NAND_MAP_BAD), 1);
    CHECK_EQ(EEFS_NandFreeCount(&V_Vol), EEFS_NAND_BADBLOCK_RESERVE - 1);
    Bad = T_FirstBad();
    NandSim_RawRead(V_Sim, Bad, 0, NULL, Oob);
    CHECK_EQ(Oob[NandTest_Geo.BadMarkerOffset], 0x00);
    V_CheckFile(&V_Vol, &F2);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    V_Remount();
    CHECK_EQ(V_Vol.Working.Map[Bad], EEFS_NAND_MAP_BAD);
    V_CheckFile(&V_Vol, &F2);
    V_Destroy();
}

static void freeing_not_counted_as_free_until_commit(void)
{
    V_File_t    F = T_SpaceStart();
    V_File_t    H = T_File("/h", V_SizeForBlocks(2), 41);
    V_File_t    X = T_File("/x", V_SizeForBlocks(3), 42);

    (void)F;
    CHECK_RC(V_Write(&H), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(EEFS_NandFreeCount(&V_Vol), EEFS_NAND_RESERVE_BLOCKS);

    CHECK_RC(EEFS_LibRemove(&V_Vol, "/f"), EEFS_SUCCESS);
    CHECK_EQ(EEFS_NandFreeCount(&V_Vol), EEFS_NAND_RESERVE_BLOCKS);
    CHECK_EQ(T_CountMap(EEFS_NAND_MAP_FREEING), 3);
    CHECK_RC(V_Write(&X), EEFS_NO_SPACE_LEFT_ON_DEVICE);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(EEFS_NandFreeCount(&V_Vol), EEFS_NAND_RESERVE_BLOCKS + 3);
    CHECK_RC(V_Write(&X), EEFS_SUCCESS);
    V_CheckFile(&V_Vol, &X);
    V_Destroy();
}

/*
 * Allocator and volume info
 */

static void T_CheckInfoMatchesMap(void)
{
    EEFS_NandVolumeInfo_t   Info;
    uint32                  b;
    uint32                  Owned = 0;
    uint32                  Bad = 0;

    CHECK_RC(EEFS_LibGetVolumeInfo(&V_Vol, &Info), EEFS_SUCCESS);
    for (b = 0; b < V_Dev.Geometry.BlockCount; b++) {
        if (V_Vol.Working.Map[b] == EEFS_NAND_MAP_BAD) Bad++;
        else if ((b >= V_Dev.Geometry.DirRegionBlocks) && EEFS_NAND_MAP_IS_OWNER(V_Vol.Working.Map[b])) Owned++;
    }
    CHECK_EQ(Info.FreeBlocks, T_CountMap(EEFS_NAND_MAP_FREE));
    CHECK_EQ(Info.FreeingBlocks, T_CountMap(EEFS_NAND_MAP_FREEING));
    CHECK_EQ(Info.UsedBlocks, Owned);
    CHECK_EQ(Info.BadBlocks, Bad);
    CHECK_EQ(Info.FreeBlocks + Info.FreeingBlocks + Info.UsedBlocks + T_CountMap(EEFS_NAND_MAP_BAD), Info.DataBlocks);
}

static void allocator_round_robin(void)
{
    V_File_t    A = T_File("/a", V_SizeForBlocks(1), 51);
    V_File_t    B = T_File("/b", V_SizeForBlocks(1), 52);
    V_File_t    C = T_File("/c", V_SizeForBlocks(2), 53);
    V_File_t    E = T_File("/e", V_SizeForBlocks(1), 54);
    V_File_t    G = T_File("/g", V_SizeForBlocks(1), 55);
    V_File_t    H = T_File("/h", V_SizeForBlocks(2), 56);
    uint32      D = T_Dir();
    uint32      Last = NandTest_Geo.BlockCount - 1;

    V_NewVolume();
    CHECK_EQ(V_Vol.AllocCursor, D);
    CHECK_RC(V_Write(&A), EEFS_SUCCESS);
    CHECK_RC(V_Write(&B), EEFS_SUCCESS);
    CHECK_RC(V_Write(&C), EEFS_SUCCESS);
    CHECK_EQ(V_SlotA(&V_Vol, "/a")->FirstBlock, D);
    CHECK_EQ(V_SlotA(&V_Vol, "/b")->FirstBlock, D + 1);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/c")->FirstBlock, NewB, NewC), 2);
    CHECK_EQ(NewB[0], D + 2);
    CHECK_EQ(NewB[1], D + 3);
    CHECK_EQ(V_Vol.AllocCursor, D + 4);
    T_CheckInfoMatchesMap();
    CHECK_RC(T_Commit(), EEFS_SUCCESS);

    /* a block freed and committed is not reused before the cursor wraps */
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/b"), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.Working.Map[D + 1], EEFS_NAND_MAP_FREE);
    CHECK_RC(V_Write(&E), EEFS_SUCCESS);
    CHECK_EQ(V_SlotA(&V_Vol, "/e")->FirstBlock, D + 4);
    T_CheckInfoMatchesMap();
    CHECK_RC(T_Commit(), EEFS_SUCCESS);

    /* after a remount the cursor starts after the highest owned block */
    V_Remount();
    CHECK_EQ(V_Vol.AllocCursor, D + 5);
    CHECK_RC(V_Write(&G), EEFS_SUCCESS);
    CHECK_EQ(V_SlotA(&V_Vol, "/g")->FirstBlock, D + 5);

    /* wrap-around from the last block to the start of the data area */
    V_Vol.AllocCursor = Last;
    CHECK_RC(V_Write(&H), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/h")->FirstBlock, NewB, NewC), 2);
    CHECK_EQ(NewB[0], Last);
    CHECK_EQ(NewB[1], D + 1);
    CHECK_EQ(V_Vol.AllocCursor, D + 2);
    T_CheckInfoMatchesMap();
    CHECK_RC(T_Commit(), EEFS_SUCCESS);

    /* the highest owned block is the last one: the cursor wraps to the start at mount */
    V_Remount();
    CHECK_EQ(V_Vol.AllocCursor, D);
    V_CheckFile(&V_Vol, &A);
    V_CheckFile(&V_Vol, &C);
    V_CheckFile(&V_Vol, &E);
    V_CheckFile(&V_Vol, &G);
    V_CheckFile(&V_Vol, &H);
    V_Destroy();
}

static void get_volume_info_counts(void)
{
    EEFS_NandVolumeInfo_t   Info;
    V_File_t                A = T_File("/a", V_SizeForBlocks(2), 61);
    V_File_t                B = T_File("/b", V_SizeForBlocks(3), 62);
    uint32                  Data;

    V_Attach(NandTest_NewSim());
    NandSim_SetFactoryBad(V_Sim, T_Dir() + 7);
    memset(&V_Aux, 0, sizeof(V_Aux));
    CHECK_RC(EEFS_LibFormat(&V_Aux, (cpuaddr)&V_Dev), EEFS_SUCCESS);
    CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);
    Data = NandTest_Geo.BlockCount - T_Dir();

    CHECK_RC(EEFS_LibGetVolumeInfo(&V_Vol, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.BlockCount, NandTest_Geo.BlockCount);
    CHECK_EQ(Info.DirRegionBlocks, T_Dir());
    CHECK_EQ(Info.DataBlocks, Data);
    CHECK_EQ(Info.FreeBlocks, Data - 1);
    CHECK_EQ(Info.BadBlocks, 1);
    CHECK_EQ(Info.UsedBlocks, 0);
    CHECK_EQ(Info.FreeingBlocks, 0);
    CHECK_EQ(Info.ReserveBlocks, EEFS_NAND_RESERVE_BLOCKS);
    CHECK_EQ(Info.BadBlockReserve, EEFS_NAND_BADBLOCK_RESERVE);
    CHECK_EQ(Info.AvailableForNew, Data - 1 - EEFS_NAND_RESERVE_BLOCKS);
    CHECK_EQ(Info.AvailableForModify, Data - 1 - EEFS_NAND_BADBLOCK_RESERVE);
    CHECK_EQ(Info.FilesInUse, 0);
    CHECK_EQ(Info.SlotsInUse, 0);
    CHECK_EQ(Info.LastSeq, 1);
    CHECK_EQ(Info.UncommittedChanges, 0);
    CHECK_EQ(Info.MirrorValid[0], 1);
    CHECK_EQ(Info.MirrorValid[1], 1);
    CHECK_EQ(Info.PendingCleanup, 0);

    CHECK_RC(V_Write(&A), EEFS_SUCCESS);
    CHECK_RC(V_Write(&B), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibGetVolumeInfo(&V_Vol, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.FreeBlocks, Data - 6);
    CHECK_EQ(Info.UsedBlocks, 5);
    CHECK_EQ(Info.FilesInUse, 2);
    CHECK_EQ(Info.SlotsInUse, 2);
    CHECK_EQ(Info.UncommittedChanges, 1);
    CHECK_EQ(Info.NextWriteSeq, V_Vol.NextWriteSeq);

    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibGetVolumeInfo(&V_Vol, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.UncommittedChanges, 0);
    CHECK_EQ(Info.LastSeq, 2);

    CHECK_RC(EEFS_LibRemove(&V_Vol, "/a"), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibGetVolumeInfo(&V_Vol, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.FreeBlocks, Data - 6);
    CHECK_EQ(Info.FreeingBlocks, 2);
    CHECK_EQ(Info.UsedBlocks, 3);
    CHECK_EQ(Info.FilesInUse, 1);
    CHECK_EQ(Info.PendingCleanup, 1);
    CHECK_EQ(Info.UncommittedChanges, 1);

    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibGetVolumeInfo(&V_Vol, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.FreeBlocks, Data - 4);
    CHECK_EQ(Info.FreeingBlocks, 0);
    CHECK_EQ(Info.PendingCleanup, 0);
    CHECK_EQ(Info.UncommittedChanges, 0);
    CHECK_EQ(Info.LastSeq, 3);
    T_CheckInfoMatchesMap();
    V_Destroy();
}

/*
 * Remove, Rename, SetFileAttributes
 */

static void remove_then_commit_erases_page0(void)
{
    V_File_t    A = T_File("/a", V_SizeForBlocks(2), 71);

    V_NewVolume();
    CHECK_RC(V_Write(&A), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/a")->FirstBlock, OldB, OldC), 2);

    CHECK_RC(EEFS_LibRemove(&V_Vol, "/missing"), EEFS_FILE_NOT_FOUND);
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/a"), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.Working.Map[OldB[0]], EEFS_NAND_MAP_FREEING);
    CHECK_EQ(V_Vol.Working.Map[OldB[1]], EEFS_NAND_MAP_FREEING);
    CHECK_EQ(V_Vol.PendingCleanupCount, 1);
    V_CheckAbsent(&V_Vol, "/a");
    CHECK(!NandSim_PageIsErased(V_Sim, OldB[0], 0));    /* nothing is erased before the commit */

    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK(NandSim_PageIsErased(V_Sim, OldB[0], 0));
    CHECK(!NandSim_PageIsErased(V_Sim, OldB[1], 0));    /* only the first block is erased */
    CHECK_EQ(V_Vol.Working.Map[OldB[0]], EEFS_NAND_MAP_FREE);
    CHECK_EQ(V_Vol.Working.Map[OldB[1]], EEFS_NAND_MAP_FREE);
    V_Remount();
    V_CheckAbsent(&V_Vol, "/a");
    /* the newest record still lists the block (FMT 4.3.4); re-running that cleanup is a no-op */
    CHECK_EQ(V_Vol.PendingCleanupBase, 1);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.PendingCleanupCount, 0);
    CHECK_EQ(V_Vol.LastSeq, 3);                         /* nothing else to write: no new record */
    V_Destroy();
}

static void rename_rewrites_first_block_only(void)
{
    V_File_t                F = T_File("/old", V_SizeForBlocks(4), 81);
    V_File_t                O = T_File("/other", 100, 82);
    EEFS_NandPage0_t        P;
    uint32                  Programs;
    uint32                  Erases;
    uint32                  k;
    uint16                  NewFirst;
    int32                   Fd;

    V_NewVolume();
    CHECK_RC(V_Write(&F), EEFS_SUCCESS);
    CHECK_RC(V_Write(&O), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/old")->FirstBlock, OldB, OldC), 4);

    Programs = NandSim_Programs(V_Sim);
    Erases = NandSim_Erases(V_Sim);
    CHECK_RC(EEFS_LibRename(&V_Vol, "/old", "/new"), EEFS_SUCCESS);
    CHECK_EQ(NandSim_Erases(V_Sim) - Erases, 1);
    CHECK_EQ(NandSim_Programs(V_Sim) - Programs, T_PPB());     /* page 0 + the first block's data pages */
    NewFirst = V_SlotA(&V_Vol, "/new")->FirstBlock;
    CHECK(NewFirst != OldB[0]);
    CHECK_EQ(V_RawBlocks(NewFirst, NewB, NewC), 4);
    for (k = 0; k < 4; k++) {
        if (k > 0) CHECK_EQ(NewB[k], OldB[k]);
        CHECK_EQ(NewC[k], OldC[k]);
    }
    NandSim_RawRead(V_Sim, NewFirst, 0, V_Page, NULL);
    CHECK_RC(EEFS_NandDecodePage0(V_Page, NandTest_Geo.PageSize, NandTest_Geo.PagesPerBlock, NewFirst, &P), EEFS_SUCCESS);
    CHECK(strcmp(P.Path, "/new") == 0);
    CHECK_EQ(V_Vol.Working.Map[OldB[0]], EEFS_NAND_MAP_FREEING);
    CHECK_EQ(V_Vol.PendingCleanupCount, 1);
    V_CheckAbsent(&V_Vol, "/old");
    F.Path = "/new";
    V_CheckFile(&V_Vol, &F);

    /* errors */
    CHECK_RC(EEFS_LibRename(&V_Vol, "/new", "/other"), EEFS_PERMISSION_DENIED);
    CHECK_RC(EEFS_LibRename(&V_Vol, "/missing", "/x"), EEFS_FILE_NOT_FOUND);
    CHECK_RC(EEFS_LibRename(&V_Vol, "/new@A", "/x"), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibRename(&V_Vol, "/new", "x"), EEFS_INVALID_ARGUMENT);
    Fd = EEFS_LibOpen(&V_Vol, "/new", O_RDONLY, 0);
    CHECK(Fd >= 0);
    CHECK_RC(EEFS_LibRename(&V_Vol, "/new", "/x"), EEFS_PERMISSION_DENIED);
    if (Fd >= 0) CHECK_RC(EEFS_LibClose(Fd), EEFS_SUCCESS);

    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK(NandSim_PageIsErased(V_Sim, OldB[0], 0));
    V_Remount();
    V_CheckSlotsValid(&V_Vol);
    V_CheckAbsent(&V_Vol, "/old");
    V_CheckFile(&V_Vol, &F);
    V_CheckFile(&V_Vol, &O);
    V_Destroy();
}

static void setattr_rewrites_first_block_and_readonly_rules(void)
{
    V_File_t                F = T_File("/attr", V_SizeForBlocks(3), 91);
    V_File_t                F2;
    EEFS_NandPage0_t        P;
    uint32                  Programs;
    uint32                  Erases;
    uint16                  First;

    V_NewVolume();
    CHECK_RC(V_Write(&F), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/attr")->FirstBlock, OldB, OldC), 3);

    Programs = NandSim_Programs(V_Sim);
    Erases = NandSim_Erases(V_Sim);
    CHECK_RC(EEFS_LibSetFileAttributes(&V_Vol, "/attr", EEFS_ATTRIBUTE_READONLY), EEFS_SUCCESS);
    CHECK_EQ(NandSim_Erases(V_Sim) - Erases, 1);
    CHECK_EQ(NandSim_Programs(V_Sim) - Programs, T_PPB());
    First = V_SlotA(&V_Vol, "/attr")->FirstBlock;
    CHECK(First != OldB[0]);
    CHECK_EQ(V_RawBlocks(First, NewB, NewC), 3);
    CHECK_EQ(NewB[1], OldB[1]);
    CHECK_EQ(NewB[2], OldB[2]);
    NandSim_RawRead(V_Sim, First, 0, V_Page, NULL);
    CHECK_RC(EEFS_NandDecodePage0(V_Page, NandTest_Geo.PageSize, NandTest_Geo.PagesPerBlock, First, &P), EEFS_SUCCESS);
    CHECK_EQ(P.Attributes, EEFS_ATTRIBUTE_READONLY);
    F.Attributes = EEFS_ATTRIBUTE_READONLY;
    V_CheckFile(&V_Vol, &F);

    /* READONLY: no write, remove or rename; the same value again changes nothing */
    F2 = T_Mod(F, 5);
    F2.Attributes = EEFS_ATTRIBUTE_NONE;
    CHECK_RC(V_Write(&F2), EEFS_PERMISSION_DENIED);
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/attr"), EEFS_PERMISSION_DENIED);
    CHECK_RC(EEFS_LibRename(&V_Vol, "/attr", "/other"), EEFS_PERMISSION_DENIED);
    Programs = V_Ops();
    CHECK_RC(EEFS_LibSetFileAttributes(&V_Vol, "/attr", EEFS_ATTRIBUTE_READONLY), EEFS_SUCCESS);
    CHECK_EQ(V_Ops(), Programs);
    CHECK_RC(EEFS_LibSetFileAttributes(&V_Vol, "/attr", 2), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibSetFileAttributes(&V_Vol, "/attr@A", EEFS_ATTRIBUTE_NONE), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibSetFileAttributes(&V_Vol, "/missing", EEFS_ATTRIBUTE_NONE), EEFS_FILE_NOT_FOUND);

    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK(NandSim_PageIsErased(V_Sim, OldB[0], 0));
    V_Remount();
    V_CheckFile(&V_Vol, &F);

    /* READONLY files may have their attributes changed: that is how READONLY is cleared */
    CHECK_RC(EEFS_LibSetFileAttributes(&V_Vol, "/attr", EEFS_ATTRIBUTE_NONE), EEFS_SUCCESS);
    CHECK_RC(V_Write(&F2), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    V_Remount();
    V_CheckFile(&V_Vol, &F2);
    V_Destroy();
}

/*
 * Open, Read, Stat, ReadDir
 */

static void open_write_flags_unsupported(void)
{
    V_File_t    F = T_File("/f", 3000, 101);
    int32       Fd;

    V_NewVolume();
    CHECK_RC(V_Write(&F), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibOpen(&V_Vol, "/f", O_WRONLY, 0), EEFS_UNSUPPORTED_OPTION);
    CHECK_RC(EEFS_LibOpen(&V_Vol, "/f", O_RDWR, 0), EEFS_UNSUPPORTED_OPTION);
    CHECK_RC(EEFS_LibOpen(&V_Vol, "/f", O_RDONLY | O_CREAT, 0), EEFS_UNSUPPORTED_OPTION);
    CHECK_RC(EEFS_LibOpen(&V_Vol, "/new", O_CREAT, 0), EEFS_UNSUPPORTED_OPTION);
    CHECK_RC(EEFS_LibCreat(&V_Vol, "/f", 0), EEFS_UNSUPPORTED_OPTION);
    CHECK_RC(EEFS_LibOpen(&V_Vol, "/missing", O_RDONLY, 0), EEFS_FILE_NOT_FOUND);
    Fd = EEFS_LibOpen(&V_Vol, "/f", O_RDONLY, 0);
    CHECK(Fd >= 0);
    if (Fd >= 0) {
        CHECK_RC(EEFS_LibWrite(Fd, V_Data, 10), EEFS_UNSUPPORTED_OPTION);

        /* an open slot cannot be rewritten or removed */
        CHECK_RC(V_Write(&F), EEFS_PERMISSION_DENIED);
        CHECK_RC(EEFS_LibRemove(&V_Vol, "/f"), EEFS_PERMISSION_DENIED);
        CHECK_RC(EEFS_LibSetFileAttributes(&V_Vol, "/f", EEFS_ATTRIBUTE_READONLY), EEFS_PERMISSION_DENIED);
        CHECK_RC(EEFS_LibFreeFS(&V_Vol), EEFS_DEVICE_IS_BUSY);
        CHECK_RC(EEFS_LibClose(Fd), EEFS_SUCCESS);
        CHECK_RC(EEFS_LibClose(Fd), EEFS_INVALID_ARGUMENT);
    }
    CHECK_EQ(EEFS_LibGetFileDescriptorsInUse(), 0);
    V_Destroy();
}

static void read_lseek_across_blocks(void)
{
    V_File_t    F = T_File("/rl", 3 * V_BlockBytes() + 17, 111);
    EEFS_Stat_t St;
    uint32      Total = 0;
    uint32      Off;
    int32       Fd;
    int32       n;

    V_NewVolume();
    CHECK_RC(V_Write(&F), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    V_Fill(V_Data2, &F);
    Fd = EEFS_LibOpen(&V_Vol, "/rl", O_RDONLY, 0);
    CHECK(Fd >= 0);
    if (Fd < 0) { V_Destroy(); return; }

    while ((n = EEFS_LibRead(Fd, &V_Read[Total], 1000)) > 0) {
        Total += (uint32)n;
    }
    CHECK_RC(n, 0);
    CHECK_EQ(Total, F.Size);
    CHECK(memcmp(V_Read, V_Data2, F.Size) == 0);
    CHECK_RC(EEFS_LibRead(Fd, V_Read, 10), 0);

    /* a read across the first block boundary */
    Off = V_D0() - 5;
    CHECK_RC(EEFS_LibLSeek(Fd, (int32)Off, SEEK_SET), (int32)Off);
    CHECK_RC(EEFS_LibRead(Fd, V_Read, 10), 10);
    CHECK(memcmp(V_Read, &V_Data2[Off], 10) == 0);
    /* across a later block boundary, from SEEK_CUR */
    CHECK_RC(EEFS_LibLSeek(Fd, (int32)V_BlockBytes() - 10, SEEK_CUR), (int32)(Off + V_BlockBytes()));
    CHECK_RC(EEFS_LibRead(Fd, V_Read, 20), 20);
    CHECK(memcmp(V_Read, &V_Data2[Off + V_BlockBytes()], 20) == 0);
    /* the end */
    CHECK_RC(EEFS_LibLSeek(Fd, -3, SEEK_END), (int32)(F.Size - 3));
    CHECK_RC(EEFS_LibRead(Fd, V_Read, 100), 3);
    CHECK(memcmp(V_Read, &V_Data2[F.Size - 3], 3) == 0);
    CHECK_RC(EEFS_LibLSeek(Fd, (int32)F.Size + 100, SEEK_SET), (int32)F.Size);
    CHECK_RC(EEFS_LibLSeek(Fd, -1, SEEK_SET), EEFS_INVALID_ARGUMENT);
    CHECK_RC(EEFS_LibLSeek(Fd, 0, 7), EEFS_INVALID_ARGUMENT);

    CHECK_RC(EEFS_LibFstat(Fd, &St), EEFS_SUCCESS);
    CHECK_EQ(St.FileSize, F.Size);
    CHECK(strcmp(St.Filename, "/rl") == 0);
    CHECK_RC(EEFS_LibClose(Fd), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibRead(Fd, V_Read, 10), EEFS_INVALID_ARGUMENT);
    V_Destroy();
}

static void stat_readdir_full_paths(void)
{
    V_File_t                    X = T_File("/cf/apps/x.so", V_SizeForBlocks(2), 121);
    V_File_t                    W = T_File("/boot/fw.bin", 1000, 122);
    EEFS_Stat_t                 St;
    EEFS_DirectoryDescriptor_t *Dd;
    EEFS_DirectoryEntry_t      *De;
    uint32                      Entries = 0;
    uint32                      InUse = 0;

    V_NewVolume();
    CHECK_RC(V_Write(&X), EEFS_SUCCESS);
    CHECK_RC(V_Write(&W), EEFS_SUCCESS);
    V_Fill(V_Data2, &X);
    CHECK_RC(EEFS_LibStat(&V_Vol, "/cf/apps/x.so", &St), EEFS_SUCCESS);
    CHECK(strcmp(St.Filename, "/cf/apps/x.so") == 0);
    CHECK_EQ(St.InodeIndex, 0);
    CHECK_EQ(St.FileSize, X.Size);
    CHECK_EQ(St.Crc, EEFS_Crc32(V_Data2, X.Size));
    CHECK_RC(EEFS_LibStat(&V_Vol, "/cf/apps", &St), EEFS_FILE_NOT_FOUND);  /* flat namespace */

    Dd = EEFS_LibOpenDir(&V_Vol);
    CHECK(Dd != NULL);
    CHECK(EEFS_LibOpenDir(&V_Vol) == NULL);             /* one directory descriptor */
    while ((Dd != NULL) && ((De = EEFS_LibReadDir(Dd)) != NULL)) {
        CHECK_EQ(De->InodeIndex, Entries);
        CHECK(De->FileHeaderPointer == NULL);
        if (De->InUse) {
            InUse++;
            if (De->InodeIndex == 0) {
                CHECK(strcmp(De->Filename, "/cf/apps/x.so") == 0);
                CHECK_EQ(De->MaxFileSize, 2 * V_BlockBytes() - NandTest_Geo.PageSize);
            }
            else {
                CHECK(strcmp(De->Filename, "/boot/fw.bin") == 0);
                CHECK_EQ(De->MaxFileSize, V_D0());
            }
        }
        else {
            CHECK_EQ(De->MaxFileSize, 0);
        }
        Entries++;
    }
    CHECK_EQ(Entries, EEFS_MAX_FILES);
    CHECK_EQ(InUse, 2);
    CHECK_RC(EEFS_LibFreeFS(&V_Vol), EEFS_DEVICE_IS_BUSY);
    CHECK_RC(EEFS_LibCloseDir(Dd), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCloseDir(Dd), EEFS_INVALID_ARGUMENT);
    V_Destroy();
}

/*
 * Batches
 */

static void batch_20_files_one_commit_one_record(void)
{
    static char     Paths[20][16];
    V_File_t        F[20];
    uint8           Mask[EEFS_NAND_MASK_BYTES];
    uint32          MapChanged;
    uint32          TxnPages;
    uint32          DirPages;
    uint32          Programs;
    uint32          Erases;
    uint32          i;

    V_NewVolume();
    DirPages = T_DirProgrammedPages();
    for (i = 0; i < 20; i++) {
        sprintf(Paths[i], "/batch/%02lu", (unsigned long)i);
        F[i] = T_File(Paths[i], 100 + i, 200 + i);
        CHECK_RC(V_Write(&F[i]), EEFS_SUCCESS);
    }
    CHECK_EQ(T_DirProgrammedPages(), DirPages);         /* nothing reaches the directory before the commit */

    EEFS_NandDiff(&V_Vol, Mask, &MapChanged);
    TxnPages = EEFS_NandRecordPages(&V_Vol, EEFS_NAND_REC_TXN, &V_Vol.Working, &V_Vol.Base, Mask,
                                    V_Vol.PendingCleanupCount);
    CHECK(TxnPages >= 1);
    Programs = NandSim_Programs(V_Sim);
    Erases = NandSim_Erases(V_Sim);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(NandSim_Programs(V_Sim) - Programs, 2 * TxnPages);    /* one record per mirror */
    CHECK_EQ(NandSim_Erases(V_Sim) - Erases, 0);
    CHECK_EQ(T_DirProgrammedPages(), DirPages + 2 * TxnPages);
    CHECK_EQ(V_Vol.LastSeq, 2);
    V_CheckMirrorsAgree(&V_Vol);

    V_Remount();
    for (i = 0; i < 20; i++) {
        V_CheckFile(&V_Vol, &F[i]);
    }
    V_Destroy();
}

static void reboot_before_commit_rolls_back(void)
{
    V_File_t    A = T_File("/a", V_SizeForBlocks(3), 131);
    V_File_t    A2 = T_Mod(T_Mod(A, V_FileOffset(1, 3)), 0);
    V_File_t    B = T_File("/b", V_SizeForBlocks(2), 132);
    static V_Snap_t Snap[2];
    V_Snap_t   *Pre = &Snap[0];
    V_Snap_t   *Cur = &Snap[1];

    V_NewVolume();
    CHECK_RC(V_Write(&A), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    V_Take(&V_Vol, Pre);
    CHECK_RC(V_Write(&A2), EEFS_SUCCESS);
    CHECK_RC(V_Write(&B), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/a"), EEFS_SUCCESS);
    V_Remount();                                        /* power off without a commit */
    V_Take(&V_Vol, Cur);
    CHECK(V_Same(Pre, Cur));
    V_CheckSlotsValid(&V_Vol);
    V_CheckFile(&V_Vol, &A);
    V_CheckAbsent(&V_Vol, "/b");
    V_CheckOrphansComplete(NULL);
    V_Destroy();
}

static void abort_frees_space_and_reusable(void)
{
    V_File_t    A = T_File("/a", V_SizeForBlocks(6), 141);
    V_File_t    A2 = T_Mod(A, V_FileOffset(3, 9));
    V_File_t    B = T_File("/b", V_SizeForBlocks(2), 142);
    V_File_t    C = T_File("/c", V_SizeForBlocks(1), 143);
    V_File_t    C2 = T_Mod(C, 1);
    V_File_t    D = T_File("/d", 100, 144);
    uint32      KeptErases[6];
    uint32      Free;
    uint32      k;
    uint16      NA;
    uint16      NB;
    uint16      NC1;
    uint16      NC2;
    int32       Fd;

    V_NewVolume();
    CHECK_RC(V_Write(&A), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/a")->FirstBlock, OldB, OldC), 6);
    Free = EEFS_NandFreeCount(&V_Vol);
    for (k = 0; k < 6; k++) KeptErases[k] = NandSim_BlockErases(V_Sim, OldB[k]);

    CHECK_RC(V_Write(&A2), EEFS_SUCCESS);
    CHECK_RC(V_Write(&B), EEFS_SUCCESS);
    CHECK_RC(V_Write(&C), EEFS_SUCCESS);
    NC1 = V_SlotA(&V_Vol, "/c")->FirstBlock;
    CHECK_RC(V_Write(&C2), EEFS_SUCCESS);                /* superseded inside the batch */
    NA = V_SlotA(&V_Vol, "/a")->FirstBlock;
    NB = V_SlotA(&V_Vol, "/b")->FirstBlock;
    NC2 = V_SlotA(&V_Vol, "/c")->FirstBlock;

    /* a descriptor on a slot the batch created blocks Abort */
    CHECK_RC(V_Write(&D), EEFS_SUCCESS);
    Fd = EEFS_LibOpen(&V_Vol, "/d", O_RDONLY, 0);
    CHECK(Fd >= 0);
    CHECK_RC(EEFS_LibAbort(&V_Vol), EEFS_DEVICE_IS_BUSY);
    if (Fd >= 0) CHECK_RC(EEFS_LibClose(Fd), EEFS_SUCCESS);

    CHECK_RC(EEFS_LibAbort(&V_Vol), EEFS_SUCCESS);
    CHECK(NandSim_PageIsErased(V_Sim, NA, 0));
    CHECK(NandSim_PageIsErased(V_Sim, NB, 0));
    CHECK(NandSim_PageIsErased(V_Sim, NC1, 0));
    CHECK(NandSim_PageIsErased(V_Sim, NC2, 0));
    for (k = 0; k < 6; k++) {
        CHECK_EQ(NandSim_BlockErases(V_Sim, OldB[k]), KeptErases[k]);   /* the committed copy is never touched */
    }
    CHECK_EQ(EEFS_NandFreeCount(&V_Vol), Free);
    CHECK_EQ(T_CountMap(EEFS_NAND_MAP_FREEING), 0);
    CHECK(EEFS_NandHasUncommitted(&V_Vol, TRUE) == FALSE);
    V_CheckFile(&V_Vol, &A);
    V_CheckAbsent(&V_Vol, "/b");
    V_CheckAbsent(&V_Vol, "/c");
    V_CheckAbsent(&V_Vol, "/d");
    V_CheckOrphansComplete(NULL);

    /* the space is usable again */
    CHECK_RC(V_Write(&B), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    V_Remount();
    V_CheckFile(&V_Vol, &A);
    V_CheckFile(&V_Vol, &B);
    V_Destroy();
}

static void freed_blocks_not_reused_before_commit(void)
{
    V_File_t    A = T_File("/a", V_SizeForBlocks(2), 151);
    V_File_t    B = T_File("/b", V_SizeForBlocks(2), 152);

    V_NewVolume();
    CHECK_RC(V_Write(&A), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/a")->FirstBlock, OldB, OldC), 2);
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/a"), EEFS_SUCCESS);
    V_Vol.AllocCursor = T_Dir();                        /* even with the cursor on them */
    CHECK_RC(V_Write(&B), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/b")->FirstBlock, NewB, NewC), 2);
    CHECK(!T_InList(NewB[0], OldB, 2));
    CHECK(!T_InList(NewB[1], OldB, 2));
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    V_Remount();
    V_CheckFile(&V_Vol, &B);
    V_Destroy();
}

/*
 * Faults
 */

static void uncorrectable_read_gives_io_error(void)
{
    V_File_t    A = T_File("/a", V_SizeForBlocks(2), 161);
    int32       Fd;

    V_NewVolume();
    CHECK_RC(V_Write(&A), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/a")->FirstBlock, OldB, OldC), 2);
    Fd = EEFS_LibOpen(&V_Vol, "/a", O_RDONLY, 0);
    CHECK(Fd >= 0);
    if (Fd < 0) { V_Destroy(); return; }
    NandSim_InjectRead(V_Sim, OldB[1], 3, EEFS_NAND_READ_UNCORRECTABLE, -1);
    CHECK_RC(EEFS_LibRead(Fd, V_Read, A.Size), EEFS_IO_ERROR);
    CHECK_RC(EEFS_LibLSeek(Fd, 0, SEEK_CUR), 0);        /* the offset did not move */
    CHECK_RC(EEFS_LibRead(Fd, V_Read, 100), 100);       /* other pages still read */
    CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/a"), EEFS_IO_ERROR);
    NandSim_ClearInjections(V_Sim);
    CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/a"), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibClose(Fd), EEFS_SUCCESS);
    V_Destroy();
}

#if !EEFS_NAND_VERIFY_ON_OPEN
static void open_checks_page0_only_by_default(void)
{
    V_File_t    A = T_File("/a", V_SizeForBlocks(2), 171);
    uint32      Reads;
    int32       Fd;
    int32       i;

    V_NewVolume();
    CHECK_RC(V_Write(&A), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/a")->FirstBlock, OldB, OldC), 2);
    NandSim_CorruptBytes(V_Sim, OldB[1], 2, 7, 1);
    EEFS_NandInvalidateCache(&V_Vol);
    Reads = NandSim_Reads(V_Sim);
    Fd = EEFS_LibOpen(&V_Vol, "/a", O_RDONLY, 0);
    CHECK(Fd >= 0);
    CHECK_EQ(NandSim_Reads(V_Sim) - Reads, 1);
    if (Fd >= 0) CHECK_RC(EEFS_LibClose(Fd), EEFS_SUCCESS);
    i = V_Entry(&V_Vol, "/a");
    CHECK_EQ(V_Vol.SlotFlags[i][0], 0);
    CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/a"), EEFS_CORRUPT_FILE);
    CHECK((V_Vol.SlotFlags[i][0] & EEFS_NAND_SLOTFLAG_CORRUPT) != 0);
    V_Destroy();
}
#else
static void open_verifies_data_when_enabled(void)
{
    V_File_t    A = T_File("/a", V_SizeForBlocks(2), 171);
    int32       i;

    V_NewVolume();
    CHECK_RC(V_Write(&A), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    V_CheckFile(&V_Vol, &A);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/a")->FirstBlock, OldB, OldC), 2);
    NandSim_CorruptBytes(V_Sim, OldB[1], 2, 7, 1);
    EEFS_NandInvalidateCache(&V_Vol);
    CHECK_RC(EEFS_LibOpen(&V_Vol, "/a", O_RDONLY, 0), EEFS_CORRUPT_FILE);
    i = V_Entry(&V_Vol, "/a");
    CHECK((V_Vol.SlotFlags[i][0] & EEFS_NAND_SLOTFLAG_CORRUPT) != 0);
    NandSim_CorruptBytes(V_Sim, OldB[1], 2, 7, 1);      /* repaired: Open validates again and clears the flag */
    CHECK(EEFS_LibOpen(&V_Vol, "/a", O_RDONLY, 0) >= 0);
    CHECK_EQ(V_Vol.SlotFlags[i][0], 0);
    V_Destroy();
}
#endif

static void grown_bad_block_recovered(void)
{
    V_File_t    F = T_File("/new", V_SizeForBlocks(3), 181);
    V_File_t    G = T_File("/mod", V_SizeForBlocks(6), 182);
    V_File_t    G2 = T_Mod(G, V_FileOffset(3, 33));
    uint32      KeptErases[6];
    uint32      Bad;
    uint32      Bad2 = 0;
    uint32      b;
    uint32      k;
    uint8       Oob[EEFS_NAND_MAX_OOB_SIZE];

    /* NEW: a data page of position 1 fails to program */
    V_NewVolume();
    NandSim_InjectProgramFail(V_Sim, NANDSIM_ANY, 5, 1);
    CHECK_RC(V_Write(&F), EEFS_SUCCESS);
    CHECK_EQ(T_CountMap(EEFS_NAND_MAP_BAD), 1);
    Bad = T_FirstBad();
    NandSim_RawRead(V_Sim, Bad, 0, NULL, Oob);
    CHECK_EQ(Oob[NandTest_Geo.BadMarkerOffset], 0x00);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/new")->FirstBlock, NewB, NewC), 3);
    CHECK(!T_InList((uint16)Bad, NewB, 3));
    V_CheckFile(&V_Vol, &F);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    V_Remount();
    CHECK_EQ(V_Vol.Working.Map[Bad], EEFS_NAND_MAP_BAD);
    V_CheckFile(&V_Vol, &F);

    /* MODIFY: the erase of position 3's new block fails; the kept blocks are never erased */
    CHECK_RC(V_Write(&G), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/mod")->FirstBlock, OldB, OldC), 6);
    for (k = 0; k < 6; k++) KeptErases[k] = NandSim_BlockErases(V_Sim, OldB[k]);
    NandSim_InjectEraseFail(V_Sim, NANDSIM_ANY, 1);
    CHECK_RC(V_Write(&G2), EEFS_SUCCESS);
    CHECK_EQ(T_CountMap(EEFS_NAND_MAP_BAD), 2);
    for (b = T_Dir(); b < NandTest_Geo.BlockCount; b++) {
        if ((V_Vol.Working.Map[b] == EEFS_NAND_MAP_BAD) && (b != Bad)) Bad2 = b;
    }
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/mod")->FirstBlock, NewB, NewC), 6);
    CHECK(!T_InList((uint16)Bad2, NewB, 6));
    for (k = 0; k < 6; k++) {
        CHECK_EQ(NandSim_BlockErases(V_Sim, OldB[k]), KeptErases[k]);
        if ((k != 0) && (k != 3)) CHECK_EQ(NewB[k], OldB[k]);
    }
    V_CheckFile(&V_Vol, &G2);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    V_Remount();
    V_CheckFile(&V_Vol, &G2);
    V_CheckFile(&V_Vol, &F);
    V_Destroy();
}

static void write_retries_exhausted_io_error(void)
{
    V_File_t                F = T_File("/new", V_SizeForBlocks(3), 191);
    V_File_t                G = T_File("/mod", V_SizeForBlocks(4), 192);
    V_File_t                G2 = T_Mod(G, V_FileOffset(2, 1));
    EEFS_NandSlot_t         Before;
    uint32                  Free;

    /* NEW */
    V_NewVolume();
    Free = EEFS_NandFreeCount(&V_Vol);
    NandSim_InjectProgramFail(V_Sim, NANDSIM_ANY, NANDSIM_ANY, -1);
    CHECK_RC(V_Write(&F), EEFS_IO_ERROR);
    NandSim_ClearInjections(V_Sim);
    V_CheckAbsent(&V_Vol, "/new");
    CHECK_EQ(V_Entry(&V_Vol, "/new"), EEFS_FILE_NOT_FOUND);
    CHECK_EQ(T_CountMap(EEFS_NAND_MAP_BAD), EEFS_NAND_WRITE_RETRIES + 1);
    CHECK_EQ(EEFS_NandFreeCount(&V_Vol), Free - (EEFS_NAND_WRITE_RETRIES + 1));
    CHECK_EQ(T_CountMap(EEFS_NAND_MAP_FREEING), 0);
    V_CheckOrphansComplete(NULL);

    /* MODIFY: the old copy is intact and still the slot's content */
    CHECK_RC(V_Write(&G), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    memcpy(&Before, V_SlotA(&V_Vol, "/mod"), sizeof(Before));
    NandSim_InjectProgramFail(V_Sim, NANDSIM_ANY, NANDSIM_ANY, -1);
    CHECK_RC(V_Write(&G2), EEFS_IO_ERROR);
    NandSim_ClearInjections(V_Sim);
    CHECK(memcmp(&Before, V_SlotA(&V_Vol, "/mod"), sizeof(Before)) == 0);
    CHECK_EQ(V_Vol.PendingCleanupCount, 0);
    V_CheckFile(&V_Vol, &G);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);                 /* persists the bad-block marks */
    V_Remount();
    V_CheckFile(&V_Vol, &G);
    CHECK_EQ(T_CountMap(EEFS_NAND_MAP_BAD), 2 * (EEFS_NAND_WRITE_RETRIES + 1));
    V_Destroy();
}

static void verify_file_detects_corruption(void)
{
    V_File_t                A = T_File("/a", V_SizeForBlocks(3) - 1000, 201);
    EEFS_NandFileInfo_t     Info;
    uint32                  LastPage;
    uint32                  Len;
    int32                   i;

    V_NewVolume();
    CHECK_RC(V_Write(&A), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/a")->FirstBlock, OldB, OldC), 3);
    i = V_Entry(&V_Vol, "/a");
    CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/a"), EEFS_SUCCESS);

    /* a data byte */
    NandSim_CorruptBytes(V_Sim, OldB[1], 0, 10, 1);
    CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/a"), EEFS_CORRUPT_FILE);
    CHECK_RC(EEFS_LibGetFileInfo(&V_Vol, "/a", &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.SlotInfo[0].Corrupt, 1);
    NandSim_CorruptBytes(V_Sim, OldB[1], 0, 10, 1);     /* XOR back */
    CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/a"), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.SlotFlags[i][0], 0);

    /* the last data byte of the file */
    Len = EEFS_NandBlockDataRange(NandTest_Geo.PageSize, NandTest_Geo.PagesPerBlock, A.Size, 2, NULL);
    LastPage = (Len - 1) / NandTest_Geo.PageSize;
    NandSim_CorruptBytes(V_Sim, OldB[2], LastPage, (Len - 1) % NandTest_Geo.PageSize, 1);
    CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/a"), EEFS_CORRUPT_FILE);
    NandSim_CorruptBytes(V_Sim, OldB[2], LastPage, (Len - 1) % NandTest_Geo.PageSize, 1);

    /* page 0 */
    NandSim_CorruptBytes(V_Sim, OldB[0], 0, EEFS_NAND_P0_OFF_FILESIZE, 1);
    EEFS_NandInvalidateCache(&V_Vol);
    CHECK_RC(EEFS_LibVerifyFile(&V_Vol, "/a"), EEFS_CORRUPT_FILE);
    CHECK_RC(EEFS_LibOpen(&V_Vol, "/a", O_RDONLY, 0), EEFS_CORRUPT_FILE);
    CHECK_RC(EEFS_LibGetFileInfo(&V_Vol, "/a", &Info), EEFS_CORRUPT_FILE);
    CHECK(strcmp(Info.Path, "/a") == 0);                /* filled from the directory */
    CHECK_EQ(Info.FileSize, A.Size);
    CHECK_RC(EEFS_LibChkDsk(&V_Vol, 0), EEFS_CORRUPT_FILE);
    V_Destroy();
}

/*
 * Limits, driver, reconcile
 */

static void max_file_size_rejected(void)
{
    uint32  Ops;
    uint32  Max;

    V_NewVolume();
    Max = V_Vol.MaxBlocksPerFile;
    CHECK_EQ(EEFS_NandBlocksNeeded(&V_Vol, V_SizeForBlocks(Max)), Max);
    CHECK_EQ(EEFS_NandBlocksNeeded(&V_Vol, V_SizeForBlocks(Max) + 1), Max + 1);
    Ops = V_Ops();
    /* the data is never touched: the size alone is rejected */
    {
        EEFS_NandFileInfo_t Meta;
        V_Meta(&Meta, EEFS_FILE_TYPE_OTHER, 1);
        CHECK_RC(EEFS_LibWriteFile(&V_Vol, "/big", &Meta, V_Data, V_SizeForBlocks(Max) + 1), EEFS_INVALID_ARGUMENT);
        CHECK_RC(EEFS_LibWriteFile(&V_Vol, "/big", &Meta, V_Data, 0xFFFFFFFFUL), EEFS_INVALID_ARGUMENT);
        CHECK_RC(EEFS_LibWriteFile(&V_Vol, "/big", &Meta, NULL, 10), EEFS_INVALID_ARGUMENT);
        Meta.FileType = EEFS_FILE_TYPE_MAX + 1;
        CHECK_RC(EEFS_LibWriteFile(&V_Vol, "/big", &Meta, V_Data, 10), EEFS_INVALID_ARGUMENT);
        Meta.FileType = EEFS_FILE_TYPE_OTHER;
        Meta.Slot = 2;
        CHECK_RC(EEFS_LibWriteFile(&V_Vol, "/big", &Meta, V_Data, 10), EEFS_INVALID_ARGUMENT);
        Meta.Slot = EEFS_NAND_SLOT_NONE;
        Meta.Attributes = 4;
        CHECK_RC(EEFS_LibWriteFile(&V_Vol, "/big", &Meta, V_Data, 10), EEFS_INVALID_ARGUMENT);
    }
    CHECK_EQ(V_Ops(), Ops);
    CHECK_EQ(V_Entry(&V_Vol, "/big"), EEFS_FILE_NOT_FOUND);
    V_Destroy();
}

static void split_path_driver(void)
{
    static char             Long[EEFS_MAX_PATH_SIZE + 8];
    EEFS_SplitPath_t        Sp;
    EEFS_NandFileInfo_t     Meta;
    EEFS_NandFileInfo_t     Info;
    EEFS_NandVolumeInfo_t   VInfo;
    EEFS_Stat_t             St;
    V_File_t                F = T_File("/cf/apps/x.so", V_SizeForBlocks(2), 211);

    CHECK_RC(EEFS_SplitPath("/NAND0/cf/apps/x.so", &Sp), EEFS_SUCCESS);
    CHECK(strcmp(Sp.MountPoint, "/NAND0") == 0);
    CHECK(strcmp(Sp.Filename, "/cf/apps/x.so") == 0);
    Long[0] = '/';
    memset(&Long[1], 'p', EEFS_MAX_PATH_SIZE);
    Long[EEFS_MAX_PATH_SIZE] = '\0';
    CHECK(EEFS_SplitPath(Long, &Sp) != EEFS_SUCCESS);

    V_Attach(NandTest_NewSim());
    CHECK_RC(EEFS_Format("/NANDFOPS", (cpuaddr)&V_Dev), EEFS_SUCCESS);
    CHECK_RC(EEFS_Mount("/NANDFOPS", "/FO"), EEFS_SUCCESS);
    V_Meta(&Meta, EEFS_FILE_TYPE_CFS_APP, 3);
    strcpy(Meta.Type.App.AppName, "X");
    V_Fill(V_Data, &F);
    CHECK_RC(EEFS_WriteFile("/FO/cf/apps/x.so", &Meta, V_Data, F.Size), EEFS_SUCCESS);
    CHECK_RC(EEFS_WriteFile("/NOPE/cf/apps/x.so", &Meta, V_Data, F.Size), EEFS_ERROR);
    CHECK_RC(EEFS_Stat("/FO/cf/apps/x.so", &St), EEFS_SUCCESS);
    CHECK(strcmp(St.Filename, "/cf/apps/x.so") == 0);
    CHECK_EQ(St.FileSize, F.Size);
    CHECK_RC(EEFS_GetVolumeInfo("/FO", &VInfo), EEFS_SUCCESS);
    CHECK_EQ(VInfo.FilesInUse, 1);
    CHECK_EQ(VInfo.UncommittedChanges, 1);
    CHECK_RC(EEFS_GetFileInfo("/FO/cf/apps/x.so", &Info), EEFS_SUCCESS);
    CHECK(strcmp(Info.Type.App.AppName, "X") == 0);
    CHECK_RC(EEFS_VerifyFile("/FO/cf/apps/x.so"), EEFS_SUCCESS);
    CHECK_RC(EEFS_VerifyFile("/FO/cf/apps/y.so"), EEFS_FILE_NOT_FOUND);

    /* unmount commits the batch */
    CHECK_RC(EEFS_UnMount("/FO"), EEFS_SUCCESS);
    CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);
    CHECK_EQ(V_Vol.LastSeq, 2);
    V_CheckFile(&V_Vol, &F);
    V_Destroy();
}

static void reconcile_page0_wins_block_ownership(void)
{
    V_File_t    F = T_File("/r", V_SizeForBlocks(6), 221);
    V_File_t    F2 = T_Mod(F, V_FileOffset(3, 1));
    uint32      k;
    int32       i;

    V_NewVolume();
    CHECK_RC(V_Write(&F), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/r")->FirstBlock, OldB, OldC), 6);
    CHECK_RC(V_Write(&F2), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/r")->FirstBlock, NewB, NewC), 6);

    V_Remount();                                        /* power cut before the commit */
    i = V_Entry(&V_Vol, "/r");
    CHECK(i >= 0);
    if (i < 0) { V_Destroy(); return; }
    CHECK_EQ(V_Vol.Working.Entry[i].Slot[0].FirstBlock, OldB[0]);
    CHECK_EQ(V_Vol.Working.Map[NewB[0]], EEFS_NAND_MAP_FREE);
    CHECK_EQ(V_Vol.Working.Map[NewB[3]], EEFS_NAND_MAP_FREE);
    for (k = 0; k < 6; k++) {
        CHECK_EQ(V_Vol.Working.Map[OldB[k]], EEFS_NAND_MAP_OWNER(i, 0));
    }
    V_CheckSlotsValid(&V_Vol);
    V_CheckFile(&V_Vol, &F);
    V_CheckOrphansComplete(NULL);
    V_Destroy();
}

static void pending_cleanup_block_forgotten_on_alloc(void)
{
    V_File_t    A = T_File("/a", V_SizeForBlocks(2), 231);
    uint8       Mask[EEFS_NAND_MASK_BYTES];
    uint32      MapChanged;
    uint32      TxnPages;
    uint32      Erases;
    uint16      Got = 0;

    V_NewVolume();
    CHECK_RC(V_Write(&A), EEFS_SUCCESS);
    CHECK_RC(T_Commit(), EEFS_SUCCESS);
    CHECK_EQ(V_RawBlocks(V_SlotA(&V_Vol, "/a")->FirstBlock, OldB, OldC), 2);
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/a"), EEFS_SUCCESS);

    /* cut the commit right at its cleanup erase */
    EEFS_NandDiff(&V_Vol, Mask, &MapChanged);
    TxnPages = EEFS_NandRecordPages(&V_Vol, EEFS_NAND_REC_TXN, &V_Vol.Working, &V_Vol.Base, Mask,
                                    V_Vol.PendingCleanupCount);
    NandSim_SetPowerBudget(V_Sim, 2 * TxnPages, NANDSIM_TORN_NONE);
    (void)T_Commit();
    CHECK(NandSim_PowerCutHappened(V_Sim));
    NandSim_Reboot(V_Sim);
    V_Remount();
    CHECK_EQ(V_Vol.PendingCleanupBase, 1);
    CHECK_EQ(V_Vol.PendingCleanup[0], OldB[0]);
    CHECK_EQ(V_Vol.Working.Map[OldB[0]], EEFS_NAND_MAP_FREE);
    CHECK(!NandSim_PageIsErased(V_Sim, OldB[0], 0));

    /* the allocator hands the block out and forgets its pending erase */
    V_Vol.AllocCursor = OldB[0];
    CHECK_RC(EEFS_NandAllocBlocks(&V_Vol, 1, &Got, EEFS_NAND_MAP_OWNER(5, 0), EEFS_NAND_ALLOC_NEW), EEFS_SUCCESS);
    CHECK_EQ(Got, OldB[0]);
    CHECK_EQ(V_Vol.PendingCleanupCount, 0);
    CHECK_EQ(V_Vol.PendingCleanupBase, 0);
    EEFS_NandReleaseBlocks(&V_Vol, &Got, 1);

    /* normal path: the first mutating call runs the pending cleanup */
    V_Remount();
    CHECK_EQ(V_Vol.PendingCleanupBase, 1);
    Erases = NandSim_BlockErases(V_Sim, OldB[0]);
    CHECK_RC(EEFS_LibRepairDirectory(&V_Vol), EEFS_SUCCESS);
    CHECK(NandSim_PageIsErased(V_Sim, OldB[0], 0));
    CHECK_EQ(NandSim_BlockErases(V_Sim, OldB[0]), Erases + 1);
    CHECK_EQ(V_Vol.PendingCleanupBase, 0);
    V_Destroy();
}

/*
 * Case table
 */

const NandTest_Case_t NandTest_FileopsCases[] = {
    { "write_read_sizes",                               write_read_sizes },
    { "write_all_types_roundtrip_via_getfileinfo",      write_all_types_roundtrip_via_getfileinfo },
    { "path_255_ok_256_rejected",                       path_255_ok_256_rejected },
    { "name_rules",                                     name_rules },
    { "signature_block_present_and_excluded",           signature_block_present_and_excluded },
    { "cow_modify_keeps_unchanged_blocks",              cow_modify_keeps_unchanged_blocks },
    { "cow_modify_byte_compare_reads_kept_blocks",      cow_modify_byte_compare_reads_kept_blocks },
    { "cow_modify_changed_crc_skips_compare",           cow_modify_changed_crc_skips_compare },
    { "cow_modify_rewrites_decayed_block",              cow_modify_rewrites_decayed_block },
    { "cow_modify_grow_and_shrink",                     cow_modify_grow_and_shrink },
    { "cow_modify_of_corrupt_slot_rewrites_all",        cow_modify_of_corrupt_slot_rewrites_all },
    { "cow_modify_no_space_no_change",                  cow_modify_no_space_no_change },
    { "reserve_new_file_vs_modify",                     reserve_new_file_vs_modify },
    { "reserve_badblock_replacement_may_use_everything", reserve_badblock_replacement_may_use_everything },
    { "freeing_not_counted_as_free_until_commit",       freeing_not_counted_as_free_until_commit },
    { "allocator_round_robin",                          allocator_round_robin },
    { "get_volume_info_counts",                         get_volume_info_counts },
    { "remove_then_commit_erases_page0",                remove_then_commit_erases_page0 },
    { "rename_rewrites_first_block_only",               rename_rewrites_first_block_only },
    { "setattr_rewrites_first_block_and_readonly_rules", setattr_rewrites_first_block_and_readonly_rules },
    { "open_write_flags_unsupported",                   open_write_flags_unsupported },
    { "read_lseek_across_blocks",                       read_lseek_across_blocks },
    { "stat_readdir_full_paths",                        stat_readdir_full_paths },
    { "batch_20_files_one_commit_one_record",           batch_20_files_one_commit_one_record },
    { "reboot_before_commit_rolls_back",                reboot_before_commit_rolls_back },
    { "abort_frees_space_and_reusable",                 abort_frees_space_and_reusable },
    { "freed_blocks_not_reused_before_commit",          freed_blocks_not_reused_before_commit },
    { "uncorrectable_read_gives_io_error",              uncorrectable_read_gives_io_error },
#if !EEFS_NAND_VERIFY_ON_OPEN
    { "open_checks_page0_only_by_default",              open_checks_page0_only_by_default },
#else
    { "open_verifies_data_when_enabled",                open_verifies_data_when_enabled },
#endif
    { "grown_bad_block_recovered",                      grown_bad_block_recovered },
    { "write_retries_exhausted_io_error",               write_retries_exhausted_io_error },
    { "verify_file_detects_corruption",                 verify_file_detects_corruption },
    { "max_file_size_rejected",                         max_file_size_rejected },
    { "split_path_driver",                              split_path_driver },
    { "reconcile_page0_wins_block_ownership",           reconcile_page0_wins_block_ownership },
    { "pending_cleanup_block_forgotten_on_alloc",       pending_cleanup_block_forgotten_on_alloc },
    { NULL, NULL }
};

/************************/
/*  End of File Comment */
/************************/
