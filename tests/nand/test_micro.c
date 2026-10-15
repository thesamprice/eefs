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
 * Filename: test_micro.c
 *
 * Purpose: Suite "micro": the bootloader library (libraries/microeefs_nand) against volumes built with the format
 *   encoders (nand_fixture.c), docs/nand-format.md sections 5-7.
 *
 * Design Notes:
 *   Every FindFile/LoadFile call goes through T_Find/T_Load, which assert that the call neither programmed nor erased
 *   anything, never wrote past PageSize in the page buffer and never wrote past MaxSize in the destination.
 *   The expected values come from the fixture's own directory state (F.Entry[]).  micro_matches_library_state
 *   (phase 3) instead builds the volume with the real library (EEFS_LibWriteFile, Commit, Rename, Remove,
 *   SetFileAttributes, copy-on-write modifies) and checks the loader against the library's GetFileInfo and the
 *   written bytes.
 *
 */

/*
 * Includes
 */

#include "nand_test.h"
#include "nand_fixture.h"
#include "microeefs_nand.h"
#include "nand_vol.h"

/*
 * Macro Definitions
 */

#define T_GUARD             64
#define T_DEST_SIZE         (2UL * 1024UL * 1024UL)
#define T_POISON            0xA5

/*
 * Local Data
 */

static Fx_t     F;
static uint8    PageBuf[EEFS_NAND_MAX_PAGE_SIZE + T_GUARD];
static uint8    Dest[T_DEST_SIZE + T_GUARD];
static uint8    Data[T_DEST_SIZE];
static uint8    Data2[T_DEST_SIZE];
static uint8    Raw[EEFS_NAND_MAX_PAGE_SIZE];

static uint32   HookCalls;
static uint8    HookDirSlot;
static uint8    HookReturn;
static char     HookName[EEFS_NAND_MAX_PATH_SIZE];

/*
 * Helpers
 */

static uint32 T_BlockBytes(void)
{
    return(NandTest_Geo.PageSize * NandTest_Geo.PagesPerBlock);
}

/* Data capacity of a file's first block */
static uint32 T_D0(void)
{
    return(T_BlockBytes() - NandTest_Geo.PageSize);
}

static void T_PrepBuffers(void)
{
    memset(PageBuf, T_POISON, sizeof(PageBuf));
}

static void T_CheckNoWrites(uint32 Programs, uint32 Erases)
{
    uint32  i;

    CHECK_EQ(NandSim_Programs(F.Sim), Programs);
    CHECK_EQ(NandSim_Erases(F.Sim), Erases);
    for (i = 0; i < T_GUARD; i++) {
        if (PageBuf[NandTest_Geo.PageSize + i] != T_POISON) {
            CHECK(PageBuf[NandTest_Geo.PageSize + i] == T_POISON);
            break;
        }
    }
}

static int32 T_Find(const char *Name, MicroEEFS_NandInfo_t *Info)
{
    uint32  Programs = NandSim_Programs(F.Sim);
    uint32  Erases = NandSim_Erases(F.Sim);
    int32   Status;

    T_PrepBuffers();
    Status = MicroEEFS_NandFindFile(&F.Dev, PageBuf, Name, Info);
    T_CheckNoWrites(Programs, Erases);
    return(Status);
}

static int32 T_Load(const char *Name, uint32 MaxSize, MicroEEFS_NandInfo_t *Info)
{
    uint32  Programs = NandSim_Programs(F.Sim);
    uint32  Erases = NandSim_Erases(F.Sim);
    uint32  i;
    int32   Status;

    T_PrepBuffers();
    memset(Dest, T_POISON, sizeof(Dest));
    Status = MicroEEFS_NandLoadFile(&F.Dev, PageBuf, Name, Dest, MaxSize, Info);
    T_CheckNoWrites(Programs, Erases);
    for (i = MaxSize; i < (MaxSize + T_GUARD); i++) {
        if (Dest[i] != T_POISON) {
            CHECK(Dest[i] == T_POISON);     /* nothing written past MaxSize */
            break;
        }
    }
    return(Status);
}

/* Loaded content equals Expected */
static void T_CheckContent(const uint8 *Expected, uint32 Size)
{
    CHECK(memcmp(Dest, Expected, Size) == 0);
}

/* Info matches the fixture's entry, with Slot loaded/validated */
static void T_CheckEntry(const MicroEEFS_NandInfo_t *Info, uint32 Index, uint8 Slot)
{
    const EEFS_NandEntry_t *E = &F.Entry[Index];
    uint32                  s;

    CHECK(strcmp(Info->Info.Path, E->Path) == 0);
    CHECK_EQ(Info->Info.IsAB, E->IsAB);
    CHECK_EQ(Info->Info.ActiveSlot, E->ActiveSlot);
    CHECK_EQ(Info->Info.FallbackCompatible, E->FallbackCompatible);
    CHECK_EQ(Info->Info.Attributes, E->Attributes);
    CHECK_EQ(Info->Info.CreationDate, E->CreationDate);
    CHECK_EQ(Info->Info.FileType, E->FileType);
    CHECK_EQ(Info->LoadedSlot, Slot);
    CHECK_EQ(Info->Info.Slot, Slot);
    CHECK_EQ(Info->Info.FileSize, E->Slot[Slot].FileSize);
    CHECK_EQ(Info->Info.FileCrc32, E->Slot[Slot].FileCrc32);
    CHECK_EQ(Info->Info.Version, E->Slot[Slot].Version);
    for (s = 0; s < 2; s++) {
        CHECK_EQ(Info->Info.SlotInfo[s].InUse, E->Slot[s].InUse);
        CHECK_EQ(Info->Info.SlotInfo[s].FirstBlock, E->Slot[s].FirstBlock);
        CHECK_EQ(Info->Info.SlotInfo[s].AllocBlocks, E->Slot[s].AllocBlocks);
        CHECK_EQ(Info->Info.SlotInfo[s].FileSize, E->Slot[s].FileSize);
        CHECK_EQ(Info->Info.SlotInfo[s].Version, E->Slot[s].Version);
        CHECK_EQ(Info->Info.SlotInfo[s].WriteSeq, E->Slot[s].WriteSeq);
        CHECK_EQ(Info->Info.SlotInfo[s].FileCrc32, E->Slot[s].FileCrc32);
        CHECK_EQ(Info->Info.SlotInfo[s].ModificationDate, E->Slot[s].ModificationDate);
    }
    CHECK_EQ(Info->UsedScan, 0);
    CHECK_EQ(Info->DirectoryValid, 1);
    CHECK_EQ(Info->DirSeq, F.Seq);
}

/* Type-specific fields of FileType t */
static void T_CheckType(const EEFS_NandTypeInfo_t *A, const EEFS_NandTypeInfo_t *B, uint32 t)
{
    switch (t) {
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

/* Rewrites the metadata page of FirstBlock after changing a decoded field (the CRC stays valid) */
static void T_RewritePage0(uint32 FirstBlock, uint32 FileCrcXor, uint32 BlockK, uint32 BlockCrcXor)
{
    static uint16       Blocks[1200];
    static uint32       Crcs[1200];
    EEFS_NandPage0_t    P;
    uint32              k;

    Fx_RawPage(&F, FirstBlock, 0, Raw);
    CHECK_RC(EEFS_NandDecodePage0(Raw, NandTest_Geo.PageSize, NandTest_Geo.PagesPerBlock, FirstBlock, &P), EEFS_SUCCESS);
    for (k = 0; k < P.BlockCount; k++) {
        EEFS_NandPage0Block(Raw, k, &Blocks[k], &Crcs[k]);
    }
    P.FileCrc32 ^= FileCrcXor;
    Crcs[BlockK] ^= BlockCrcXor;
    CHECK_RC(EEFS_NandEncodePage0(Raw, NandTest_Geo.PageSize, &P, Blocks, Crcs), EEFS_SUCCESS);
    NandSim_RawWrite(F.Sim, FirstBlock, 0, Raw, NULL);
}

/* Destroys a mirror (erases every block of its ring) */
static void T_EraseMirror(uint32 Mirror)
{
    uint32  Half = NandTest_Geo.DirRegionBlocks / 2;
    uint32  b;

    for (b = Mirror * Half; b < (Mirror + 1) * Half; b++) {
        NandSim_RawErase(F.Sim, b);
    }
}

static uint8 T_Hook(const char *Name, uint8 DirectorySlot, void *HookCtx)
{
    HookCalls++;
    HookDirSlot = DirectorySlot;
    strncpy(HookName, Name, sizeof(HookName) - 1);
    CHECK(HookCtx == (void *)&HookCalls);
    return(HookReturn);
}

static void T_SetHook(uint8 Return)
{
    HookCalls = 0;
    HookDirSlot = 0xEE;
    HookReturn = Return;
    memset(HookName, 0, sizeof(HookName));
    MicroEEFS_NandSetSlotHook(T_Hook, &HookCalls);
}

/* A/B entry "/ab" at Index: A = Data (SizeA, v1), B = Data2 (SizeB, v2) */
static void T_MakeAB(uint32 Index, const char *Path, uint32 SizeA, uint32 SizeB, uint8 Active, uint8 Fallback)
{
    Fx_Pattern(Data, SizeA, 11);
    Fx_Pattern(Data2, SizeB, 22);
    Fx_PutFile(&F, Index, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_A, Path, EEFS_FILE_TYPE_BOOT_ELF, 1, NULL, Data, SizeA);
    Fx_PutFile(&F, Index, EEFS_NAND_SLOT_B, EEFS_NAND_SLOT_B, Path, EEFS_FILE_TYPE_BOOT_ELF, 2, NULL, Data2, SizeB);
    F.Entry[Index].ActiveSlot = Active;
    F.Entry[Index].FallbackCompatible = Fallback;
    Fx_Touch(&F, Index);
    Fx_Commit(&F);
}

/*
 * Cases
 */

/* Checkpoint + TXNs: add, rename away and back, remove, re-add at another index, new ring block, rewrite */
static void micro_find_via_journal(void)
{
    MicroEEFS_NandInfo_t    Info;
    uint32                  SizeB = T_D0() + (3 * NandTest_Geo.PageSize) + 17;
    uint32                  OldA;

    Fx_Format(&F);
    CHECK_RC(T_Find("/boot/a", &Info), EEFS_FILE_NOT_FOUND);
    CHECK_EQ(Info.DirectoryValid, 1);
    CHECK_EQ(Info.DirSeq, 1);

    Fx_Pattern(Data, 5000, 1);
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/boot/a", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 5000);
    Fx_Pattern(Data2, SizeB, 2);
    Fx_PutFile(&F, 1, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/boot/b", EEFS_FILE_TYPE_OTHER, 1, NULL, Data2, SizeB);
    Fx_PutFile(&F, 2, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/cf/x", EEFS_FILE_TYPE_OTHER, 7, NULL, Data, 100);
    Fx_Commit(&F);

    CHECK_RC(T_Find("/boot/a", &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 0, EEFS_NAND_SLOT_A);
    CHECK_RC(T_Find("/boot/b", &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 1, EEFS_NAND_SLOT_A);
    CHECK_RC(T_Load("/boot/b", SizeB, &Info), EEFS_SUCCESS);
    T_CheckContent(Data2, SizeB);
    CHECK_RC(T_Find("/cf/x", &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 2, EEFS_NAND_SLOT_A);
    CHECK_RC(T_Find("/cf", &Info), EEFS_FILE_NOT_FOUND);
    CHECK_RC(T_Find("/boot/a2", &Info), EEFS_FILE_NOT_FOUND);

    /* rename away: the old name disappears although its metadata page is still on the media */
    OldA = F.Entry[0].Slot[0].FirstBlock;
    Fx_Rename(&F, 0, "/boot/a2");
    Fx_Commit(&F);
    CHECK_RC(T_Find("/boot/a", &Info), EEFS_FILE_NOT_FOUND);
    CHECK_RC(T_Load("/boot/a2", 5000, &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 0, EEFS_NAND_SLOT_A);
    T_CheckContent(Data, 5000);
    CHECK(NandSim_PageIsProgrammed(F.Sim, OldA, 0));

    /* and back */
    Fx_Rename(&F, 0, "/boot/a");
    Fx_Commit(&F);
    CHECK_RC(T_Find("/boot/a2", &Info), EEFS_FILE_NOT_FOUND);
    CHECK_RC(T_Load("/boot/a", 5000, &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 0, EEFS_NAND_SLOT_A);
    T_CheckContent(Data, 5000);
    CHECK(Info.Info.SlotInfo[0].FirstBlock != OldA);

    /* remove */
    Fx_Remove(&F, 1);
    Fx_Commit(&F);
    CHECK_RC(T_Find("/boot/b", &Info), EEFS_FILE_NOT_FOUND);
    CHECK_RC(T_Find("/boot/a", &Info), EEFS_SUCCESS);

    /* re-add at another index */
    Fx_Pattern(Data2, 3000, 3);
    Fx_PutFile(&F, 5, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/boot/b", EEFS_FILE_TYPE_OTHER, 2, NULL, Data2, 3000);
    Fx_Commit(&F);
    CHECK_RC(T_Load("/boot/b", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 5, EEFS_NAND_SLOT_A);
    T_CheckContent(Data2, 3000);

    /* remove and re-add at another index in the same commit (either item order works) */
    Fx_Remove(&F, 5);
    Fx_Pattern(Data2, 2500, 4);
    Fx_PutFile(&F, 3, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/boot/b", EEFS_FILE_TYPE_OTHER, 3, NULL, Data2, 2500);
    Fx_Commit(&F);
    CHECK_RC(T_Load("/boot/b", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 3, EEFS_NAND_SLOT_A);
    T_CheckContent(Data2, 2500);
    Fx_Remove(&F, 3);
    Fx_PutFile(&F, 9, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/boot/b", EEFS_FILE_TYPE_OTHER, 4, NULL, Data2, 2400);
    Fx_Commit(&F);
    CHECK_RC(T_Load("/boot/b", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 9, EEFS_NAND_SLOT_A);
    T_CheckContent(Data2, 2400);

    /* attribute change carried by a checkpoint in a new ring block, then TXNs after it */
    F.Entry[2].Attributes = EEFS_ATTRIBUTE_READONLY;
    Fx_Touch(&F, 2);
    F.ForceCheckpoint = 1;
    Fx_Commit(&F);
    CHECK_EQ(F.LastRec[0].Page, 0);
    CHECK_RC(T_Find("/cf/x", &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 2, EEFS_NAND_SLOT_A);
    CHECK_EQ(Info.Info.Attributes, EEFS_ATTRIBUTE_READONLY);
    CHECK_RC(T_Find("/boot/b", &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 9, EEFS_NAND_SLOT_A);

    Fx_Pattern(Data, 7777, 5);
    Fx_PutFile(&F, 2, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/cf/x", EEFS_FILE_TYPE_OTHER, 8, NULL, Data, 7777);
    Fx_Commit(&F);
    CHECK_RC(T_Load("/cf/x", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 2, EEFS_NAND_SLOT_A);
    T_CheckContent(Data, 7777);
    CHECK_RC(T_Find("/boot/a", &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 0, EEFS_NAND_SLOT_A);

    Fx_Free(&F);
}

/* The ring wraps: every block of each mirror is reused several times */
static void micro_ring_wrap(void)
{
    MicroEEFS_NandInfo_t    Info;
    uint32                  i;
    uint32                  Rounds = (NandTest_Geo.DirRegionBlocks / 2) * 3;

    Fx_Format(&F);
    for (i = 0; i < Rounds; i++) {
        Fx_Pattern(Data, 1000 + i, 100 + i);
        Fx_PutFile(&F, i % 3, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, (i % 3 == 0) ? "/r0" : ((i % 3 == 1) ? "/r1" : "/r2"),
                   EEFS_FILE_TYPE_OTHER, i, NULL, Data, 1000 + i);
        F.ForceCheckpoint = (uint8)((i % 3) != 0);
        Fx_Commit(&F);
        CHECK_RC(T_Load((i % 3 == 0) ? "/r0" : ((i % 3 == 1) ? "/r1" : "/r2"), T_DEST_SIZE, &Info), EEFS_SUCCESS);
        T_CheckEntry(&Info, i % 3, EEFS_NAND_SLOT_A);
        T_CheckContent(Data, 1000 + i);
    }
    Fx_Free(&F);
}

/* Every file type, sizes around the page and block boundaries; content and Type fields */
static void micro_load_via_journal_all_types(void)
{
    MicroEEFS_NandInfo_t    Info;
    EEFS_NandTypeInfo_t     Type[6];
    uint32                  Size[6];
    char                    Name[32];
    uint32                  t;
    uint32                  Reads;
    uint32                  Budget;

    memset(Type, 0, sizeof(Type));
    Type[EEFS_FILE_TYPE_BOOT_FIT].Fit.DestAddress = 0x80000000UL;
    Type[EEFS_FILE_TYPE_BOOT_ELF].Elf.DestAddress = 0x81000000UL;
    Type[EEFS_FILE_TYPE_BOOT_ELF].Elf.EntryPoint = ((uint64)0x12345678UL << 32) | 0x81000100UL;
    Type[EEFS_FILE_TYPE_BOOT_ELF].Elf.Flags = EEFS_NAND_ELF_FLAG_ENTRY_VALID;
    Type[EEFS_FILE_TYPE_BOOT_DTB].Dtb.DestAddress = 0x82000000UL;
    strcpy(Type[EEFS_FILE_TYPE_CFS_APP].App.AppName, "SAMPLE_APP");
    strcpy(Type[EEFS_FILE_TYPE_CFS_APP].App.EntrySymbol, "SAMPLE_AppMain");
    Type[EEFS_FILE_TYPE_CFS_APP].App.Priority = 50;
    Type[EEFS_FILE_TYPE_CFS_APP].App.ExceptionAction = 1;
    Type[EEFS_FILE_TYPE_CFS_APP].App.StackSize = 16384;
    Type[EEFS_FILE_TYPE_CFS_APP].App.LoadAddress = 0x90000000UL;
    strcpy(Type[EEFS_FILE_TYPE_CFS_LIB].Lib.LibName, "SAMPLE_LIB");
    strcpy(Type[EEFS_FILE_TYPE_CFS_LIB].Lib.EntrySymbol, "SAMPLE_LibInit");
    Type[EEFS_FILE_TYPE_CFS_LIB].Lib.LoadAddress = 0x91000000UL;

    Size[0] = 0;                                                    /* empty file */
    Size[1] = 1;
    Size[2] = NandTest_Geo.PageSize;                                /* exactly one data page */
    Size[3] = T_D0();                                               /* fills the first block exactly */
    Size[4] = T_D0() + 1;                                           /* one byte in the second block */
    Size[5] = T_D0() + (2 * T_BlockBytes()) - 5;                    /* three blocks, partial last page */

    Fx_Format(&F);
    for (t = 0; t < 6; t++) {
        sprintf(Name, "/types/t%lu", (unsigned long)t);
        Fx_Pattern(Data, Size[t], 40 + t);
        Fx_PutFile(&F, 10 + t, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, Name, (uint8)t, 0x100 + t, &Type[t], Data, Size[t]);
    }
    Fx_Commit(&F);

    for (t = 0; t < 6; t++) {
        sprintf(Name, "/types/t%lu", (unsigned long)t);
        Fx_Pattern(Data, Size[t], 40 + t);
        NandSim_ResetCounters(F.Sim);
        CHECK_RC(T_Load(Name, Size[t], &Info), EEFS_SUCCESS);
        Reads = NandSim_Reads(F.Sim);
        T_CheckEntry(&Info, 10 + t, EEFS_NAND_SLOT_A);
        T_CheckContent(Data, Size[t]);
        CHECK_EQ(Info.Info.FileType, t);
        CHECK_EQ(Info.Info.Version, 0x100 + t);
        T_CheckType(&Info.Info.Type, &Type[t], t);
        CHECK_EQ(Info.Info.SlotInfo[0].AllocBlocks, (NandTest_Geo.PageSize + Size[t] + T_BlockBytes() - 1) / T_BlockBytes());
        /* FMT 5 / impl 9 read budget: directory + 1 per data page + page 0 */
        Budget = NandTest_Geo.DirRegionBlocks + (4 * NandTest_Geo.PagesPerBlock) + 1 +
                 (Info.Info.SlotInfo[0].AllocBlocks * NandTest_Geo.PagesPerBlock);
        CHECK(Reads <= Budget);

        CHECK_RC(T_Find(Name, &Info), EEFS_SUCCESS);
        T_CheckEntry(&Info, 10 + t, EEFS_NAND_SLOT_A);
        T_CheckType(&Info.Info.Type, &Type[t], t);
    }
    Fx_Free(&F);
}

/* Corrupt metadata page, data, per-block CRC, file CRC, path and slot mismatches: EEFS_CORRUPT_FILE, never a scan */
static void micro_rejects_corrupt_page0_and_data(void)
{
    MicroEEFS_NandInfo_t    Info;
    uint32                  Size = T_D0() + T_BlockBytes() + 100;  /* 3 blocks */
    uint32                  i;
    uint32                  First;
    uint16                  Second;
    static const char      *Names[] = { "/c/page0", "/c/data0", "/c/data1", "/c/filecrc", "/c/blockcrc", "/c/last" };

    Fx_Format(&F);
    Fx_Pattern(Data, Size, 9);
    for (i = 0; i < 6; i++) {
        Fx_PutFile(&F, i, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, Names[i], EEFS_FILE_TYPE_OTHER, 1, NULL, Data, Size);
    }
    Fx_Commit(&F);
    for (i = 0; i < 6; i++) {
        CHECK_RC(T_Load(Names[i], Size, &Info), EEFS_SUCCESS);
    }

    /* metadata page: one byte of the path */
    NandSim_CorruptBytes(F.Sim, F.Entry[0].Slot[0].FirstBlock, 0, 20, 1);
    CHECK_RC(T_Find(Names[0], &Info), EEFS_CORRUPT_FILE);
    CHECK_EQ(Info.UsedScan, 0);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_NONE);
    CHECK_EQ(Info.Info.ActiveFailed, 1);
    CHECK_EQ(Info.Info.SlotInfo[0].Corrupt, 1);
    CHECK_RC(T_Load(Names[0], Size, &Info), EEFS_CORRUPT_FILE);

    /* data page in the first block: page 0 is fine (FindFile succeeds), the per-block CRC fails */
    NandSim_CorruptBytes(F.Sim, F.Entry[1].Slot[0].FirstBlock, 2, 7, 1);
    CHECK_RC(T_Find(Names[1], &Info), EEFS_SUCCESS);
    CHECK_RC(T_Load(Names[1], Size, &Info), EEFS_CORRUPT_FILE);
    CHECK_EQ(Info.Info.ActiveFailed, 1);

    /* data page in the second block */
    Fx_RawPage(&F, F.Entry[2].Slot[0].FirstBlock, 0, Raw);
    EEFS_NandPage0Block(Raw, 1, &Second, NULL);
    NandSim_CorruptBytes(F.Sim, Second, NandTest_Geo.PagesPerBlock - 1, 0, 1);
    CHECK_RC(T_Load(Names[2], Size, &Info), EEFS_CORRUPT_FILE);

    /* FileCrc32 wrong, every block CRC right */
    T_RewritePage0(F.Entry[3].Slot[0].FirstBlock, 0x1, 0, 0);
    CHECK_RC(T_Find(Names[3], &Info), EEFS_SUCCESS);
    CHECK_RC(T_Load(Names[3], Size, &Info), EEFS_CORRUPT_FILE);

    /* one block CRC wrong, FileCrc32 right */
    T_RewritePage0(F.Entry[4].Slot[0].FirstBlock, 0, 2, 0x80000000UL);
    CHECK_RC(T_Load(Names[4], Size, &Info), EEFS_CORRUPT_FILE);

    /* the last (partial) data page */
    Fx_RawPage(&F, F.Entry[5].Slot[0].FirstBlock, 0, Raw);
    EEFS_NandPage0Block(Raw, 2, &Second, NULL);
    NandSim_CorruptBytes(F.Sim, Second, 0, 99, 1);
    CHECK_RC(T_Load(Names[5], Size, &Info), EEFS_CORRUPT_FILE);

    /* directory entry pointing at another file's metadata page (path mismatch) */
    First = F.Entry[5].Slot[0].FirstBlock;
    F.Entry[5].Slot[0].FirstBlock = F.Entry[2].Slot[0].FirstBlock;
    Fx_Touch(&F, 5);
    Fx_Commit(&F);
    CHECK_RC(T_Find(Names[5], &Info), EEFS_CORRUPT_FILE);
    F.Entry[5].Slot[0].FirstBlock = (uint16)First;

    /* directory slot pointing at a data block (not a metadata page) and outside the data area */
    F.Entry[5].Slot[0].FirstBlock = Second;
    Fx_Touch(&F, 5);
    Fx_Commit(&F);
    CHECK_RC(T_Find(Names[5], &Info), EEFS_CORRUPT_FILE);
    F.Entry[5].Slot[0].FirstBlock = 1;
    Fx_Touch(&F, 5);
    Fx_Commit(&F);
    CHECK_RC(T_Find(Names[5], &Info), EEFS_CORRUPT_FILE);

    /* metadata page Slot B attached to directory slot A */
    Fx_PutFile(&F, 7, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_B, "/c/slot", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 10);
    Fx_Commit(&F);
    CHECK_RC(T_Find("/c/slot", &Info), EEFS_CORRUPT_FILE);

    /* unsigned only: SigType != 0 is a corrupt page */
    Fx_PutFile(&F, 8, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/c/sig", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 10);
    Fx_Commit(&F);
    CHECK_RC(T_Find("/c/sig", &Info), EEFS_SUCCESS);
    Fx_RawPage(&F, F.Entry[8].Slot[0].FirstBlock, 0, Raw);
    EEFS_NandPut16(&Raw[EEFS_NAND_P0_OFF_SIGTYPE], 1);      /* outside HeaderCrc32 */
    NandSim_RawWrite(F.Sim, F.Entry[8].Slot[0].FirstBlock, 0, Raw, NULL);
    CHECK_RC(T_Find("/c/sig", &Info), EEFS_CORRUPT_FILE);

    Fx_Free(&F);
}

/* Read failures: uncorrectable ECC and controller errors are EEFS_IO_ERROR; corrected bits are fine */
static void micro_io_error(void)
{
    MicroEEFS_NandInfo_t    Info;
    uint32                  Size = T_D0() + 10;
    uint32                  First;

    Fx_Format(&F);
    Fx_Pattern(Data, Size, 3);
    First = Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/io", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, Size);
    Fx_Commit(&F);

    NandSim_InjectRead(F.Sim, First, 0, EEFS_NAND_READ_UNCORRECTABLE, -1);
    CHECK_RC(T_Find("/io", &Info), EEFS_IO_ERROR);
    CHECK_EQ(Info.Info.ActiveFailed, 1);
    CHECK_RC(T_Load("/io", Size, &Info), EEFS_IO_ERROR);
    NandSim_ClearInjections(F.Sim);

    NandSim_InjectRead(F.Sim, First, 3, EEFS_NAND_READ_UNCORRECTABLE, -1);
    CHECK_RC(T_Find("/io", &Info), EEFS_SUCCESS);
    CHECK_RC(T_Load("/io", Size, &Info), EEFS_IO_ERROR);
    NandSim_ClearInjections(F.Sim);

    NandSim_InjectRead(F.Sim, First + 1, 0, EEFS_NAND_READ_IO_ERROR, -1);
    CHECK_RC(T_Load("/io", Size, &Info), EEFS_IO_ERROR);
    NandSim_ClearInjections(F.Sim);

    /* corrected bit errors everywhere: success */
    NandSim_InjectRead(F.Sim, NANDSIM_ANY, NANDSIM_ANY, 3, -1);
    CHECK_RC(T_Load("/io", Size, &Info), EEFS_SUCCESS);
    T_CheckContent(Data, Size);
    NandSim_ClearInjections(F.Sim);

    /* an unreadable directory page: the other mirror serves */
    NandSim_InjectRead(F.Sim, F.CurBlock[0], 0, EEFS_NAND_READ_UNCORRECTABLE, -1);
    CHECK_RC(T_Load("/io", Size, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.UsedScan, 0);
    CHECK_EQ(Info.DirSeq, F.Seq);
    NandSim_InjectRead(F.Sim, F.CurBlock[1], 0, EEFS_NAND_READ_UNCORRECTABLE, -1);
    CHECK_RC(T_Load("/io", Size, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.UsedScan, 1);                 /* no readable checkpoint left in either mirror */
    NandSim_ClearInjections(F.Sim);

    Fx_Free(&F);
}

/* No directory: scan the metadata pages; highest WriteSeq per slot wins, lower block on a tie */
static void micro_scan_fallback_when_no_directory(void)
{
    MicroEEFS_NandInfo_t    Info;
    EEFS_NandSlot_t         S1;
    EEFS_NandSlot_t         S2;
    EEFS_NandSlot_t         S3;
    uint32                  Dir = NandTest_Geo.DirRegionBlocks;
    uint32                  m;

    /* blank device */
    Fx_Blank(&F);
    CHECK_RC(T_Find("/s/a", &Info), EEFS_NOT_FORMATTED);
    CHECK_EQ(Info.UsedScan, 1);
    CHECK_EQ(Info.DirectoryValid, 0);
    Fx_Free(&F);

    /* formatted then directory lost */
    Fx_Format(&F);
    Fx_Pattern(Data, 4000, 1);
    Fx_Pattern(Data2, 4000, 2);
    F.NextBlock = Dir + 100;
    Fx_WriteCopy(&F, "/s/a", EEFS_NAND_SLOT_NONE, EEFS_FILE_TYPE_BOOT_FIT, 1, NULL, Data, 4000, &S1);
    F.NextBlock = Dir + 50;                     /* the newer copy sits at a lower block */
    Fx_WriteCopy(&F, "/s/a", EEFS_NAND_SLOT_NONE, EEFS_FILE_TYPE_BOOT_FIT, 2, NULL, Data2, 4000, &S2);
    F.NextBlock = Dir + 150;                    /* and an even older copy at a higher block */
    F.WriteSeq = 10;
    Fx_WriteCopy(&F, "/s/a", EEFS_NAND_SLOT_NONE, EEFS_FILE_TYPE_BOOT_FIT, 0, NULL, Data, 4000, &S3);
    F.WriteSeq = 1000;

    /* tie on WriteSeq: the lower block wins */
    F.NextBlock = Dir + 300;
    F.WriteSeq = 499;
    Fx_WriteCopy(&F, "/s/tie", EEFS_NAND_SLOT_NONE, EEFS_FILE_TYPE_OTHER, 2, NULL, Data2, 300, NULL);
    F.NextBlock = Dir + 200;
    F.WriteSeq = 499;
    Fx_WriteCopy(&F, "/s/tie", EEFS_NAND_SLOT_NONE, EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 300, NULL);

    /* B only (explicit) and A + B */
    F.NextBlock = Dir + 400;
    F.WriteSeq = 2000;
    Fx_WriteCopy(&F, "/s/bonly", EEFS_NAND_SLOT_B, EEFS_FILE_TYPE_OTHER, 5, NULL, Data2, 777, NULL);
    Fx_WriteCopy(&F, "/s/ab", EEFS_NAND_SLOT_A, EEFS_FILE_TYPE_OTHER, 6, NULL, Data, 888, NULL);
    Fx_WriteCopy(&F, "/s/ab", EEFS_NAND_SLOT_B, EEFS_FILE_TYPE_OTHER, 7, NULL, Data2, 999, NULL);
    Fx_WriteCopy(&F, "/s/abbad", EEFS_NAND_SLOT_A, EEFS_FILE_TYPE_OTHER, 6, NULL, Data, 888, &S1);
    Fx_WriteCopy(&F, "/s/abbad", EEFS_NAND_SLOT_B, EEFS_FILE_TYPE_OTHER, 7, NULL, Data2, 999, NULL);
    NandSim_CorruptBytes(F.Sim, S1.FirstBlock, 1, 0, 1);

    /* the directory never heard of these files: it is valid, so no scan */
    CHECK_RC(T_Find("/s/a", &Info), EEFS_FILE_NOT_FOUND);
    CHECK_EQ(Info.UsedScan, 0);

    for (m = 0; m < 2; m++) {
        NandSim_CorruptBytes(F.Sim, F.CurBlock[m], 0, 100, 2);  /* both checkpoints unreadable */
    }

    NandSim_ResetCounters(F.Sim);
    CHECK_RC(T_Load("/s/a", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK(NandSim_Reads(F.Sim) <= (NandTest_Geo.BlockCount + (5 * NandTest_Geo.PagesPerBlock) + 2));
    CHECK_EQ(Info.UsedScan, 1);
    CHECK_EQ(Info.DirectoryValid, 0);
    CHECK_EQ(Info.DirSeq, 0);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_A);
    CHECK_EQ(Info.Info.SlotInfo[0].FirstBlock, S2.FirstBlock);
    CHECK_EQ(Info.Info.SlotInfo[0].WriteSeq, S2.WriteSeq);
    CHECK_EQ(Info.Info.SlotInfo[1].InUse, 0);
    CHECK_EQ(Info.Info.Version, 2);
    CHECK_EQ(Info.Info.IsAB, 0);
    CHECK_EQ(Info.Info.FallbackCompatible, 0);
    CHECK_EQ(Info.Info.FileType, EEFS_FILE_TYPE_BOOT_FIT);
    CHECK_EQ(Info.Info.CreationDate, 1000 + S2.WriteSeq);
    T_CheckContent(Data2, 4000);

    CHECK_RC(T_Load("/s/tie", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.Info.SlotInfo[0].FirstBlock, Dir + 200);
    CHECK_EQ(Info.Info.Version, 1);
    T_CheckContent(Data, 300);

    CHECK_RC(T_Load("/s/bonly", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.Info.IsAB, 1);
    CHECK_EQ(Info.Info.ActiveSlot, EEFS_NAND_SLOT_B);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_B);
    T_CheckContent(Data2, 777);

    CHECK_RC(T_Load("/s/ab", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.Info.IsAB, 1);
    CHECK_EQ(Info.Info.ActiveSlot, EEFS_NAND_SLOT_A);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_A);
    T_CheckContent(Data, 888);
    CHECK_RC(T_Load("/s/ab@B", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_B);
    T_CheckContent(Data2, 999);

    /* a scan never falls back (FallbackCompatible = 0) */
    CHECK_RC(T_Load("/s/abbad", T_DEST_SIZE, &Info), EEFS_CORRUPT_FILE);
    CHECK_EQ(Info.UsedScan, 1);
    CHECK_EQ(Info.Info.ActiveFailed, 1);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_NONE);

    CHECK_RC(T_Find("/s/none", &Info), EEFS_FILE_NOT_FOUND);
    CHECK_EQ(Info.UsedScan, 1);

    /* erased directory region gives the same result */
    T_EraseMirror(0);
    T_EraseMirror(1);
    CHECK_RC(T_Load("/s/a", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.UsedScan, 1);
    CHECK_EQ(Info.Info.SlotInfo[0].FirstBlock, S2.FirstBlock);

    Fx_Free(&F);
}

/* A valid directory whose metadata page is bad: EEFS_CORRUPT_FILE, no scan, although an older copy is valid */
static void micro_no_scan_when_directory_valid_but_page0_bad(void)
{
    MicroEEFS_NandInfo_t    Info;
    EEFS_NandPage0_t        P;
    uint32                  Old;
    uint32                  New;

    Fx_Format(&F);
    Fx_Pattern(Data, 6000, 1);
    Old = Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/n", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 6000);
    Fx_Commit(&F);
    Fx_Pattern(Data2, 6000, 2);
    New = Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/n", EEFS_FILE_TYPE_OTHER, 2, NULL, Data2, 6000);
    Fx_Commit(&F);                              /* the old metadata page stays (pending cleanup) */

    CHECK_RC(T_Load("/n", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    T_CheckContent(Data2, 6000);

    NandSim_CorruptBytes(F.Sim, New, 0, 30, 1);
    CHECK_RC(T_Find("/n", &Info), EEFS_CORRUPT_FILE);
    CHECK_EQ(Info.UsedScan, 0);
    CHECK_EQ(Info.DirectoryValid, 1);
    CHECK_RC(T_Load("/n", T_DEST_SIZE, &Info), EEFS_CORRUPT_FILE);
    CHECK_EQ(Info.UsedScan, 0);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_NONE);

    /* the older copy is still a valid metadata page on the device */
    Fx_RawPage(&F, Old, 0, Raw);
    CHECK_RC(EEFS_NandDecodePage0(Raw, NandTest_Geo.PageSize, NandTest_Geo.PagesPerBlock, Old, &P), EEFS_SUCCESS);
    CHECK(strcmp(P.Path, "/n") == 0);

    Fx_Free(&F);
}

/* The scan is used only when both mirrors lack a valid checkpoint */
static void micro_scan_only_when_both_mirrors_invalid(void)
{
    MicroEEFS_NandInfo_t    Info;

    Fx_Format(&F);
    Fx_Pattern(Data, 1234, 1);
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/m", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 1234);
    Fx_Commit(&F);

    T_EraseMirror(1);
    CHECK_RC(T_Load("/m", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.UsedScan, 0);
    CHECK_EQ(Info.DirectoryValid, 1);
    CHECK_EQ(Info.DirSeq, F.Seq);
    Fx_Free(&F);

    Fx_Format(&F);
    Fx_Pattern(Data, 1234, 1);
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/m", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 1234);
    Fx_Commit(&F);
    T_EraseMirror(0);
    CHECK_RC(T_Load("/m", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.UsedScan, 0);
    CHECK_EQ(Info.DirectoryValid, 1);
    CHECK_EQ(Info.DirSeq, F.Seq);
    T_CheckContent(Data, 1234);

    /* a mirror whose only checkpoint is damaged counts as invalid too */
    NandSim_CorruptBytes(F.Sim, F.CurBlock[1], 0, 40, 1);
    CHECK_RC(T_Load("/m", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.UsedScan, 1);
    CHECK_EQ(Info.DirectoryValid, 0);
    T_CheckContent(Data, 1234);
    Fx_Free(&F);
}

/* A/B: the active slot is loaded; "@A"/"@B" name one copy */
static void micro_ab_active_and_explicit_slot(void)
{
    MicroEEFS_NandInfo_t    Info;

    Fx_Format(&F);
    T_MakeAB(4, "/ab", 5000, 6000, EEFS_NAND_SLOT_B, 1);
    Fx_PutFile(&F, 5, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/one", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 100);
    Fx_Commit(&F);

    CHECK_RC(T_Load("/ab", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 4, EEFS_NAND_SLOT_B);
    CHECK_EQ(Info.Info.IsAB, 1);
    CHECK_EQ(Info.Info.ActiveFailed, 0);
    T_CheckContent(Data2, 6000);
    CHECK_RC(T_Load("/ab@A", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 4, EEFS_NAND_SLOT_A);
    CHECK(strcmp(Info.Info.Path, "/ab") == 0);
    T_CheckContent(Data, 5000);
    CHECK_RC(T_Load("/ab@B", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 4, EEFS_NAND_SLOT_B);
    CHECK_RC(T_Find("/ab@A", &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_A);

    F.Entry[4].ActiveSlot = EEFS_NAND_SLOT_A;
    Fx_Touch(&F, 4);
    Fx_Commit(&F);
    CHECK_RC(T_Load("/ab", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    T_CheckEntry(&Info, 4, EEFS_NAND_SLOT_A);
    T_CheckContent(Data, 5000);

    /* single-copy file: A only */
    CHECK_RC(T_Load("/one", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.Info.IsAB, 0);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_A);
    CHECK_RC(T_Load("/one@A", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_RC(T_Load("/one@B", T_DEST_SIZE, &Info), EEFS_FILE_NOT_FOUND);

    /* an explicit copy never falls back, even when FallbackCompatible is set */
    NandSim_CorruptBytes(F.Sim, F.Entry[4].Slot[0].FirstBlock, 1, 0, 1);
    CHECK_RC(T_Load("/ab@A", T_DEST_SIZE, &Info), EEFS_CORRUPT_FILE);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_NONE);
    CHECK_EQ(Info.Info.SlotInfo[0].Corrupt, 1);
    CHECK_EQ(Info.Info.SlotInfo[1].Corrupt, 0);
    CHECK_EQ(Info.Info.ActiveFailed, 1);        /* A is the active copy */
    CHECK_RC(T_Load("/ab@B", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.Info.ActiveFailed, 0);

    Fx_Free(&F);
}

/* Active copy bad, FallbackCompatible: the other copy is loaded, ActiveFailed = 1 */
static void micro_ab_fallback_compatible(void)
{
    MicroEEFS_NandInfo_t    Info;

    Fx_Format(&F);
    T_MakeAB(0, "/fb", 5000, 9000, EEFS_NAND_SLOT_B, 1);

    /* bad data in B: FindFile (page 0 only) still picks B, LoadFile falls back to A */
    NandSim_CorruptBytes(F.Sim, F.Entry[0].Slot[1].FirstBlock, 1, 5, 1);
    CHECK_RC(T_Find("/fb", &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_B);
    CHECK_EQ(Info.Info.ActiveFailed, 0);
    CHECK_RC(T_Load("/fb", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_A);
    CHECK_EQ(Info.Info.Slot, EEFS_NAND_SLOT_A);
    CHECK_EQ(Info.Info.ActiveFailed, 1);
    CHECK_EQ(Info.Info.ActiveSlot, EEFS_NAND_SLOT_B);
    CHECK_EQ(Info.Info.SlotInfo[1].Corrupt, 1);
    CHECK_EQ(Info.Info.SlotInfo[0].Corrupt, 0);
    CHECK_EQ(Info.Info.FileSize, 5000);
    CHECK_EQ(Info.Info.Version, 1);
    T_CheckContent(Data, 5000);

    /* bad metadata page of B: FindFile applies the rule at page-0 level */
    NandSim_CorruptBytes(F.Sim, F.Entry[0].Slot[1].FirstBlock, 0, 64, 1);
    CHECK_RC(T_Find("/fb", &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_A);
    CHECK_EQ(Info.Info.ActiveFailed, 1);
    CHECK_RC(T_Load("/fb", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_A);
    T_CheckContent(Data, 5000);
    Fx_Free(&F);

    /* an uncorrectable ECC read of the active copy is a failure like a CRC mismatch */
    Fx_Format(&F);
    T_MakeAB(0, "/fb", 5000, 9000, EEFS_NAND_SLOT_B, 1);
    NandSim_InjectRead(F.Sim, F.Entry[0].Slot[1].FirstBlock, 2, EEFS_NAND_READ_UNCORRECTABLE, -1);
    CHECK_RC(T_Load("/fb", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_A);
    CHECK_EQ(Info.Info.ActiveFailed, 1);
    T_CheckContent(Data, 5000);
    NandSim_ClearInjections(F.Sim);

    /* the directory's ActiveFailed passes through when the active copy loads */
    F.Entry[0].ActiveFailed = 1;
    Fx_Touch(&F, 0);
    Fx_Commit(&F);
    CHECK_RC(T_Load("/fb", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_B);
    CHECK_EQ(Info.Info.ActiveFailed, 1);
    T_CheckContent(Data2, 9000);

    /* both copies bad: the fallback's error */
    NandSim_CorruptBytes(F.Sim, F.Entry[0].Slot[1].FirstBlock, 0, 64, 1);
    NandSim_InjectRead(F.Sim, F.Entry[0].Slot[0].FirstBlock, 1, EEFS_NAND_READ_IO_ERROR, -1);
    CHECK_RC(T_Load("/fb", T_DEST_SIZE, &Info), EEFS_IO_ERROR);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_NONE);
    CHECK_EQ(Info.Info.ActiveFailed, 1);
    CHECK_EQ(Info.Info.SlotInfo[0].Corrupt, 1);
    CHECK_EQ(Info.Info.SlotInfo[1].Corrupt, 1);
    Fx_Free(&F);
}

/* Active copy bad, not FallbackCompatible: error, the other copy is not loaded */
static void micro_ab_no_fallback_when_incompatible(void)
{
    MicroEEFS_NandInfo_t    Info;
    int32                   Status;

    Fx_Format(&F);
    T_MakeAB(0, "/nf", 5000, 9000, EEFS_NAND_SLOT_B, 0);
    NandSim_CorruptBytes(F.Sim, F.Entry[0].Slot[1].FirstBlock, 1, 5, 1);

    CHECK_RC(T_Load("/nf", T_DEST_SIZE, &Info), EEFS_CORRUPT_FILE);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_NONE);
    CHECK_EQ(Info.Info.ActiveFailed, 1);
    CHECK_EQ(Info.Info.SlotInfo[1].Corrupt, 1);
    CHECK_EQ(Info.Info.SlotInfo[0].Corrupt, 0);

    /* A's metadata page is not even read: a one-shot injection on it is still pending afterwards */
    NandSim_InjectRead(F.Sim, F.Entry[0].Slot[0].FirstBlock, 0, EEFS_NAND_READ_UNCORRECTABLE, 1);
    CHECK_RC(T_Load("/nf", T_DEST_SIZE, &Info), EEFS_CORRUPT_FILE);
    Status = F.Dev.Ops->ReadPage(F.Dev.Ctx, F.Entry[0].Slot[0].FirstBlock, 0, Raw, NULL);
    CHECK_RC(Status, EEFS_NAND_READ_UNCORRECTABLE);
    NandSim_ClearInjections(F.Sim);

    /* uncorrectable read of the active page 0: IO error, no fallback */
    Fx_Free(&F);
    Fx_Format(&F);
    T_MakeAB(0, "/nf", 5000, 9000, EEFS_NAND_SLOT_B, 0);
    NandSim_InjectRead(F.Sim, F.Entry[0].Slot[1].FirstBlock, 0, EEFS_NAND_READ_UNCORRECTABLE, -1);
    CHECK_RC(T_Find("/nf", &Info), EEFS_IO_ERROR);
    CHECK_EQ(Info.Info.ActiveFailed, 1);
    CHECK_RC(T_Load("/nf", T_DEST_SIZE, &Info), EEFS_IO_ERROR);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_NONE);
    Fx_Free(&F);
}

/* The BSP hook picks the copy to try first; the compatibility rule still decides any fallback */
static void micro_hook_override_honoured_and_compatible_rule_applies(void)
{
    MicroEEFS_NandInfo_t    Info;

    Fx_Format(&F);
    T_MakeAB(0, "/h", 5000, 9000, EEFS_NAND_SLOT_A, 0);
    Fx_PutFile(&F, 1, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/single", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 50);
    Fx_Commit(&F);

    /* hook chooses B (the inactive copy) */
    T_SetHook(EEFS_NAND_SLOT_B);
    CHECK_RC(T_Load("/h", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(HookCalls, 1);
    CHECK_EQ(HookDirSlot, EEFS_NAND_SLOT_A);
    CHECK(strcmp(HookName, "/h") == 0);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_B);
    CHECK_EQ(Info.Info.ActiveSlot, EEFS_NAND_SLOT_A);
    CHECK_EQ(Info.Info.ActiveFailed, 0);
    T_CheckContent(Data2, 9000);

    /* hook returns something that is not a slot in use: the directory's choice */
    T_SetHook(7);
    CHECK_RC(T_Load("/h", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_A);
    T_SetHook(EEFS_NAND_SLOT_B);
    CHECK_RC(T_Load("/single", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(HookCalls, 1);
    CHECK_EQ(HookDirSlot, EEFS_NAND_SLOT_A);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_A);

    /* an explicit name bypasses the hook */
    T_SetHook(EEFS_NAND_SLOT_B);
    CHECK_RC(T_Load("/h@A", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(HookCalls, 0);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_A);

    /* the hook's copy is bad and the entry is not FallbackCompatible: error, A is not loaded */
    NandSim_CorruptBytes(F.Sim, F.Entry[0].Slot[1].FirstBlock, 1, 0, 1);
    CHECK_RC(T_Load("/h", T_DEST_SIZE, &Info), EEFS_CORRUPT_FILE);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_NONE);
    CHECK_EQ(Info.Info.ActiveFailed, 1);
    CHECK_EQ(Info.Info.SlotInfo[1].Corrupt, 1);
    CHECK_EQ(Info.Info.SlotInfo[0].Corrupt, 0);

    /* FallbackCompatible: the directory's copy is loaded after the hook's copy failed */
    F.Entry[0].FallbackCompatible = 1;
    Fx_Touch(&F, 0);
    Fx_Commit(&F);
    CHECK_RC(T_Load("/h", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_A);
    CHECK_EQ(Info.Info.ActiveFailed, 1);
    T_CheckContent(Data, 5000);

    /* the hook pointing back at the directory's copy (default behaviour restored) */
    MicroEEFS_NandSetSlotHook(NULL, NULL);
    CHECK_RC(T_Load("/h", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_A);
    CHECK_EQ(Info.Info.ActiveFailed, 0);
    Fx_Free(&F);
}

/* Torn records are ignored entirely: trailing TXN, a TXN torn in a later page, a torn checkpoint in a new block */
static void micro_torn_txn_ignored(void)
{
    MicroEEFS_NandInfo_t    Info;
    char                    Name[32];
    uint32                  i;
    uint32                  m;
    uint32                  GoodSeq;

    Fx_Format(&F);
    Fx_Pattern(Data, 3000, 1);
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/t", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 3000);
    Fx_Commit(&F);
    GoodSeq = F.Seq;
    Fx_Pattern(Data2, 3000, 2);
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/t", EEFS_FILE_TYPE_OTHER, 2, NULL, Data2, 3000);
    Fx_Commit(&F);
    CHECK_RC(T_Load("/t", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    T_CheckContent(Data2, 3000);

    /* the last TXN torn (garbage in its payload) in both mirrors: state of the previous Seq */
    for (m = 0; m < 2; m++) {
        NandSim_CorruptBytes(F.Sim, F.LastRec[m].Block, F.LastRec[m].Page, 40, 4);
    }
    CHECK_RC(T_Load("/t", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.DirSeq, GoodSeq);
    CHECK_EQ(Info.UsedScan, 0);
    CHECK_EQ(Info.Info.Version, 1);
    T_CheckContent(Data, 3000);
    Fx_Free(&F);

    /* a multi-page TXN whose last page is torn: none of its items count, including those on its first page */
    Fx_Format(&F);
    Fx_Pattern(Data, 100, 1);
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/keep", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 100);
    Fx_Commit(&F);
    GoodSeq = F.Seq;
    Fx_Remove(&F, 0);                           /* ENTRY 0 lands on the first page of the record */
    for (i = 1; i < 40; i++) {
        sprintf(Name, "/many/%lu", (unsigned long)i);
        Fx_PutFile(&F, i, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, Name, EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 10);
    }
    Fx_Commit(&F);
    CHECK(F.LastRec[0].Count >= 2);
    CHECK_RC(T_Find("/keep", &Info), EEFS_FILE_NOT_FOUND);
    CHECK_RC(T_Find("/many/39", &Info), EEFS_SUCCESS);
    for (m = 0; m < 2; m++) {
        NandSim_CorruptBytes(F.Sim, F.LastRec[m].Block, F.LastRec[m].Page + F.LastRec[m].Count - 1,
                             NandTest_Geo.PageSize - 1, 1);
    }
    CHECK_RC(T_Find("/keep", &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.DirSeq, GoodSeq);
    CHECK_RC(T_Find("/many/1", &Info), EEFS_FILE_NOT_FOUND);
    CHECK_RC(T_Find("/many/39", &Info), EEFS_FILE_NOT_FOUND);

    Fx_Free(&F);

    /* a record whose last page was never programmed */
    Fx_Format(&F);
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/keep", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 100);
    Fx_Commit(&F);
    GoodSeq = F.Seq;
    for (i = 1; i < 40; i++) {
        sprintf(Name, "/many/%lu", (unsigned long)i);
        Fx_PutFile(&F, i, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, Name, EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 10);
    }
    Fx_Commit(&F);
    memset(Raw, 0xFF, sizeof(Raw));
    for (m = 0; m < 2; m++) {
        NandSim_RawWrite(F.Sim, F.LastRec[m].Block, F.LastRec[m].Page + F.LastRec[m].Count - 1, Raw, NULL);
    }
    CHECK_RC(T_Find("/many/1", &Info), EEFS_FILE_NOT_FOUND);
    CHECK_RC(T_Find("/keep", &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.DirSeq, GoodSeq);
    Fx_Free(&F);

    /* a checkpoint torn while entering a new ring block: the previous block's state */
    Fx_Format(&F);
    for (i = 0; i < 40; i++) {                  /* enough entries for a multi-page checkpoint */
        sprintf(Name, "/c/%lu", (unsigned long)i);
        Fx_PutFile(&F, i, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, Name, EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 10);
    }
    Fx_Commit(&F);
    GoodSeq = F.Seq;
    Fx_Remove(&F, 39);
    F.ForceCheckpoint = 1;
    Fx_Commit(&F);
    CHECK_EQ(F.LastRec[0].Page, 0);
    CHECK(F.LastRec[0].Count >= 2);
    CHECK_RC(T_Find("/c/39", &Info), EEFS_FILE_NOT_FOUND);
    CHECK_EQ(Info.DirSeq, F.Seq);
    for (m = 0; m < 2; m++) {
        NandSim_CorruptBytes(F.Sim, F.LastRec[m].Block, F.LastRec[m].Count - 1, 77, 1);
    }
    CHECK_RC(T_Find("/c/39", &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.DirSeq, GoodSeq);
    CHECK_EQ(Info.UsedScan, 0);
    Fx_Free(&F);

    /* a TXN with a Seq gap is not part of the chain */
    Fx_Format(&F);
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/gap", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 10);
    Fx_Commit(&F);
    GoodSeq = F.Seq;
    F.Seq++;                                    /* the next record claims Seq + 2 */
    F.MirrorSeq[0] = F.Seq;
    F.MirrorSeq[1] = F.Seq;
    Fx_Remove(&F, 0);
    Fx_Commit(&F);
    CHECK_RC(T_Find("/gap", &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.DirSeq, GoodSeq);
    Fx_Free(&F);
}

/* The mirror with the longer chain wins, mirror 0 on a tie */
static void micro_picks_longest_mirror_chain(void)
{
    MicroEEFS_NandInfo_t    Info;

    /* mirror 1 one record behind (cut between the two mirror writes) */
    Fx_Format(&F);
    Fx_Pattern(Data, 2000, 1);
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/p", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 2000);
    Fx_Commit(&F);
    Fx_Pattern(Data2, 2000, 2);
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/p", EEFS_FILE_TYPE_OTHER, 2, NULL, Data2, 2000);
    F.MirrorMask = 1;
    Fx_Commit(&F);
    CHECK_RC(T_Load("/p", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.DirSeq, F.Seq);
    CHECK_EQ(Info.Info.Version, 2);
    T_CheckContent(Data2, 2000);

    /* mirror 1 ahead (mirror 0's newest record lost): mirror 1 wins */
    Fx_Pattern(Data, 2100, 3);
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/p", EEFS_FILE_TYPE_OTHER, 3, NULL, Data, 2100);
    F.MirrorMask = 2;                           /* mirror 1 catches up with a checkpoint at Seq + 1 */
    Fx_Commit(&F);
    CHECK_EQ(F.LastRec[1].Page, 0);
    CHECK_RC(T_Load("/p", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.DirSeq, F.Seq);
    CHECK_EQ(Info.Info.Version, 3);
    T_CheckContent(Data, 2100);
    Fx_Free(&F);

    /* equal LastSeq with different content (cannot happen with the library): mirror 0 */
    Fx_Format(&F);
    Fx_Pattern(Data, 2000, 1);
    Fx_Pattern(Data2, 2200, 2);
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/p", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 2000);
    F.MirrorMask = 1;
    Fx_Commit(&F);
    F.Seq--;
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/p", EEFS_FILE_TYPE_OTHER, 2, NULL, Data2, 2200);
    F.MirrorMask = 2;
    Fx_Commit(&F);
    CHECK_EQ(F.LastRec[0].Page, F.LastRec[1].Page);  /* both a TXN right after the format checkpoint */
    CHECK(F.LastRec[0].Page > 0);
    CHECK_RC(T_Load("/p", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.DirSeq, F.Seq);
    CHECK_EQ(Info.Info.Version, 1);
    T_CheckContent(Data, 2000);

    /* mirror 0 ahead by more records */
    F.MirrorMask = 1;
    F.Entry[0].Attributes = 1;
    Fx_Touch(&F, 0);
    Fx_Commit(&F);
    CHECK_RC(T_Find("/p", &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.Info.Attributes, 1);
    Fx_Free(&F);
}

/* Destination too small: EEFS_NO_SPACE_LEFT_ON_DEVICE, nothing written, no fallback, no ActiveFailed */
static void micro_maxsize_too_small(void)
{
    MicroEEFS_NandInfo_t    Info;
    uint32                  Size = T_D0() + 333;
    uint32                  i;

    Fx_Format(&F);
    Fx_Pattern(Data, Size, 1);
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/big", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, Size);
    Fx_Commit(&F);

    CHECK_RC(T_Load("/big", Size - 1, &Info), EEFS_NO_SPACE_LEFT_ON_DEVICE);
    CHECK_EQ(Info.Info.ActiveFailed, 0);
    CHECK_EQ(Info.Info.FileSize, Size);
    for (i = 0; i < Size; i++) {
        if (Dest[i] != T_POISON) break;
    }
    CHECK_EQ(i, Size);                          /* nothing loaded */
    CHECK_RC(T_Load("/big", 0, &Info), EEFS_NO_SPACE_LEFT_ON_DEVICE);
    CHECK_RC(T_Find("/big", &Info), EEFS_SUCCESS);
    CHECK_RC(T_Load("/big", Size, &Info), EEFS_SUCCESS);
    T_CheckContent(Data, Size);

    /* A/B: the active copy is too big, the compatible old one would fit; still NO_SPACE */
    T_MakeAB(1, "/abbig", 1000, 50000, EEFS_NAND_SLOT_B, 1);
    CHECK_RC(T_Load("/abbig", 2000, &Info), EEFS_NO_SPACE_LEFT_ON_DEVICE);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_NONE);
    CHECK_EQ(Info.Info.ActiveFailed, 0);
    CHECK_EQ(Info.Info.SlotInfo[1].Corrupt, 0);
    CHECK_RC(T_Load("/abbig", 50000, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_B);

    /* an empty file fits in MaxSize 0 */
    Fx_PutFile(&F, 2, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/empty", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 0);
    Fx_Commit(&F);
    CHECK_RC(T_Load("/empty", 0, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.Info.FileSize, 0);
    Fx_Free(&F);
}

/* VOLUME items that disagree with the geometry */
static void micro_geometry_mismatch(void)
{
    MicroEEFS_NandInfo_t    Info;

    Fx_Blank(&F);
    F.VolBlockCount = NandTest_Geo.BlockCount - 1;
    Fx_WriteFormat(&F);
    CHECK_RC(T_Find("/x", &Info), EEFS_GEOMETRY_MISMATCH);
    CHECK_EQ(Info.UsedScan, 0);
    Fx_Free(&F);

    /* the reader's geometry differs from the one the volume was formatted with */
    Fx_Format(&F);
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/x", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 10);
    Fx_Commit(&F);
    CHECK_RC(T_Find("/x", &Info), EEFS_SUCCESS);
    F.Dev.Geometry.BlockCount = NandTest_Geo.BlockCount - 2;
    CHECK_RC(T_Find("/x", &Info), EEFS_GEOMETRY_MISMATCH);
    F.Dev.Geometry.BlockCount = NandTest_Geo.BlockCount;
    Fx_Free(&F);
}

/* Factory-bad blocks in the directory rings are skipped */
static void micro_bad_directory_blocks(void)
{
    MicroEEFS_NandInfo_t    Info;
    uint32                  Half = NandTest_Geo.DirRegionBlocks / 2;

    Fx_Blank(&F);
    NandSim_SetFactoryBad(F.Sim, 0);
    NandSim_SetFactoryBad(F.Sim, Half + 1);
    Fx_WriteFormat(&F);
    CHECK_EQ(F.CurBlock[0], 1);
    Fx_PutFile(&F, 0, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_NONE, "/x", EEFS_FILE_TYPE_OTHER, 1, NULL, Data, 10);
    F.ForceCheckpoint = 1;
    Fx_Commit(&F);
    CHECK_EQ(F.CurBlock[1], Half + 2);
    CHECK_RC(T_Load("/x", T_DEST_SIZE, &Info), EEFS_SUCCESS);
    CHECK_EQ(Info.DirSeq, F.Seq);
    Fx_Free(&F);
}

/*
 * Cross-check with the library (phase 3)
 */

typedef struct
{
    V_File_t                F;
    uint8                   FileType;
    uint32                  Version;
    int                     Live;           /* expected to exist in the committed directory */
} M_File_t;

static M_File_t     M_Files[8];
static uint32       M_Count;

static int32 M_Write(M_File_t *M)
{
    EEFS_NandFileInfo_t Meta;

    V_Meta(&Meta, M->FileType, M->Version);
    Meta.Attributes = M->F.Attributes;
    switch (M->FileType) {
        case EEFS_FILE_TYPE_BOOT_ELF:
            Meta.Type.Elf.DestAddress = 0x40000000UL;
            Meta.Type.Elf.EntryPoint = 0x40000400UL + M->Version;
            Meta.Type.Elf.Flags = 1;
            break;
        case EEFS_FILE_TYPE_BOOT_DTB:
            Meta.Type.Dtb.DestAddress = 0x1F000000UL;
            break;
        case EEFS_FILE_TYPE_CFS_APP:
            strcpy(Meta.Type.App.AppName, "X_APP");
            strcpy(Meta.Type.App.EntrySymbol, "X_AppMain");
            Meta.Type.App.Priority = 70;
            Meta.Type.App.StackSize = 8192;
            break;
        default:
            break;
    }
    return(EEFS_LibWriteFile(&V_Vol, (char *)M->F.Path, &Meta, V_Content(&M->F), M->F.Size));
}

/* The loader, reading the device the library wrote, finds and loads every committed file byte for byte, with the
 * library's view of the entry; removed and renamed-away names are not found. */
static void M_CheckAll(void)
{
    MicroEEFS_NandInfo_t    Info;
    EEFS_NandFileInfo_t     L;
    uint32                  Programs = NandSim_Programs(V_Sim);
    uint32                  Erases = NandSim_Erases(V_Sim);
    uint32                  i;
    uint32                  s;
    int32                   Status;

    for (i = 0; i < M_Count; i++) {
        T_PrepBuffers();
        memset(&Info, 0, sizeof(Info));
        Status = MicroEEFS_NandLoadFile(&V_Dev, PageBuf, M_Files[i].F.Path, Dest, T_DEST_SIZE, &Info);
        if (!M_Files[i].Live) {
            CHECK_RC(Status, EEFS_FILE_NOT_FOUND);
            continue;
        }
        CHECK_RC(Status, EEFS_SUCCESS);
        CHECK_RC(EEFS_LibGetFileInfo(&V_Vol, (char *)M_Files[i].F.Path, &L), EEFS_SUCCESS);
        CHECK(memcmp(Dest, V_Content(&M_Files[i].F), M_Files[i].F.Size) == 0);
        CHECK(strcmp(Info.Info.Path, L.Path) == 0);
        CHECK_EQ(Info.Info.FileType, L.FileType);
        CHECK_EQ(Info.Info.Version, L.Version);
        CHECK_EQ(Info.Info.FileSize, L.FileSize);
        CHECK_EQ(Info.Info.FileSize, M_Files[i].F.Size);
        CHECK_EQ(Info.Info.FileCrc32, L.FileCrc32);
        CHECK_EQ(Info.Info.Attributes, L.Attributes);
        CHECK_EQ(Info.Info.Attributes, M_Files[i].F.Attributes);
        CHECK_EQ(Info.Info.CreationDate, L.CreationDate);
        CHECK_EQ(Info.Info.IsAB, L.IsAB);
        CHECK_EQ(Info.Info.ActiveSlot, L.ActiveSlot);
        CHECK_EQ(Info.Info.ActiveFailed, 0);
        CHECK_EQ(Info.LoadedSlot, EEFS_NAND_SLOT_A);
        CHECK_EQ(Info.UsedScan, 0);
        CHECK_EQ(Info.DirectoryValid, 1);
        CHECK_EQ(Info.DirSeq, V_Vol.LastSeq);
        for (s = 0; s < 2; s++) {
            CHECK_EQ(Info.Info.SlotInfo[s].InUse, L.SlotInfo[s].InUse);
            CHECK_EQ(Info.Info.SlotInfo[s].FirstBlock, L.SlotInfo[s].FirstBlock);
            CHECK_EQ(Info.Info.SlotInfo[s].AllocBlocks, L.SlotInfo[s].AllocBlocks);
            CHECK_EQ(Info.Info.SlotInfo[s].FileSize, L.SlotInfo[s].FileSize);
            CHECK_EQ(Info.Info.SlotInfo[s].Version, L.SlotInfo[s].Version);
            CHECK_EQ(Info.Info.SlotInfo[s].WriteSeq, L.SlotInfo[s].WriteSeq);
            CHECK_EQ(Info.Info.SlotInfo[s].FileCrc32, L.SlotInfo[s].FileCrc32);
            CHECK_EQ(Info.Info.SlotInfo[s].ModificationDate, L.SlotInfo[s].ModificationDate);
        }
        T_CheckType(&Info.Info.Type, &L.Type, L.FileType);
    }
    CHECK_EQ(NandSim_Programs(V_Sim), Programs);
    CHECK_EQ(NandSim_Erases(V_Sim), Erases);
}

static M_File_t M_Make(const char *Path, uint32 Size, uint32 Seed, uint8 FileType, uint32 Version)
{
    M_File_t    M;

    memset(&M, 0, sizeof(M));
    M.F.Path = Path;
    M.F.Size = Size;
    M.F.Seed = Seed;
    M.FileType = FileType;
    M.Version = Version;
    M.Live = 1;
    return(M);
}

static void micro_matches_library_state(void)
{
    uint32  BB = NandTest_Geo.PageSize * NandTest_Geo.PagesPerBlock;
    uint32  D0 = BB - NandTest_Geo.PageSize;
    uint32  i;

    V_NewVolume();
    M_Files[0] = M_Make("/boot/kernel.elf", 2 * BB + 1234, 501, EEFS_FILE_TYPE_BOOT_ELF, 1);
    M_Files[1] = M_Make("/cf/apps/x.so", BB + 77, 502, EEFS_FILE_TYPE_CFS_APP, 1);
    M_Files[2] = M_Make("/etc/cfg", 100, 503, EEFS_FILE_TYPE_OTHER, 1);
    M_Files[3] = M_Make("/empty", 0, 504, EEFS_FILE_TYPE_OTHER, 1);
    M_Files[4] = M_Make("/boot/board.dtb", 3000, 505, EEFS_FILE_TYPE_BOOT_DTB, 1);
    M_Count = 5;
    for (i = 0; i < M_Count; i++) {
        CHECK_RC(M_Write(&M_Files[i]), EEFS_SUCCESS);
    }
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    M_CheckAll();

    /* a copy-on-write modify, a rename, a remove and an attribute change in one batch */
    M_Files[0].F.Mod[M_Files[0].F.ModCount++] = D0 + 5;        /* position 1 changes, position 2 is kept */
    M_Files[0].Version = 2;
    CHECK_RC(M_Write(&M_Files[0]), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibRename(&V_Vol, "/cf/apps/x.so", "/cf/apps/y.so"), EEFS_SUCCESS);
    M_Files[1].Live = 0;
    M_Files[5] = M_Files[1];
    M_Files[5].F.Path = "/cf/apps/y.so";
    M_Files[5].Live = 1;
    CHECK_RC(EEFS_LibRemove(&V_Vol, "/etc/cfg"), EEFS_SUCCESS);
    M_Files[2].Live = 0;
    CHECK_RC(EEFS_LibSetFileAttributes(&V_Vol, "/boot/board.dtb", EEFS_ATTRIBUTE_READONLY), EEFS_SUCCESS);
    M_Files[4].F.Attributes = EEFS_ATTRIBUTE_READONLY;
    M_Count = 6;
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    M_CheckAll();

    /* an uncommitted rewrite is invisible to the loader until the commit */
    M_Files[0].F.Mod[M_Files[0].F.ModCount++] = 3;
    M_Files[0].Version = 3;
    CHECK_RC(M_Write(&M_Files[0]), EEFS_SUCCESS);
    T_PrepBuffers();
    {
        MicroEEFS_NandInfo_t    Info;
        V_File_t                Old = M_Files[0].F;

        Old.ModCount--;
        CHECK_RC(MicroEEFS_NandLoadFile(&V_Dev, PageBuf, "/boot/kernel.elf", Dest, T_DEST_SIZE, &Info), EEFS_SUCCESS);
        CHECK_EQ(Info.Info.Version, 2);
        CHECK(memcmp(Dest, V_Content(&Old), Old.Size) == 0);
    }
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    M_CheckAll();

    /* grow and shrink, re-add a removed name at another index, and enough commits to enter new ring blocks */
    M_Files[5].F.Size = 3 * BB;
    CHECK_RC(M_Write(&M_Files[5]), EEFS_SUCCESS);
    M_Files[0].F.Size = D0 - 10;
    CHECK_RC(M_Write(&M_Files[0]), EEFS_SUCCESS);
    M_Files[2].Live = 1;
    M_Files[2].F.Seed = 506;
    CHECK_RC(M_Write(&M_Files[2]), EEFS_SUCCESS);
    CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    M_CheckAll();
    for (i = 0; i < 2 * NandTest_Geo.PagesPerBlock; i++) {
        M_Files[2].F.Seed = 600 + i;
        M_Files[2].F.Size = 50 + i;
        CHECK_RC(M_Write(&M_Files[2]), EEFS_SUCCESS);
        CHECK_RC(EEFS_LibCommit(&V_Vol), EEFS_SUCCESS);
    }
    M_CheckAll();

    /* and after a remount of the library */
    CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);
    M_CheckAll();
    V_Destroy();
}

/* Argument and geometry validation */
static void micro_invalid_arguments(void)
{
    MicroEEFS_NandInfo_t    Info;
    EEFS_NandDevice_t       Dev;
    EEFS_NandOps_t          NoRead;
    char                    Long[EEFS_NAND_MAX_PATH_SIZE + 4];

    Fx_Format(&F);
    CHECK_RC(MicroEEFS_NandFindFile(NULL, PageBuf, "/x", &Info), EEFS_INVALID_ARGUMENT);
    CHECK_RC(MicroEEFS_NandFindFile(&F.Dev, NULL, "/x", &Info), EEFS_INVALID_ARGUMENT);
    CHECK_RC(MicroEEFS_NandFindFile(&F.Dev, PageBuf, NULL, &Info), EEFS_INVALID_ARGUMENT);
    CHECK_RC(MicroEEFS_NandFindFile(&F.Dev, PageBuf, "/x", NULL), EEFS_INVALID_ARGUMENT);
    CHECK_RC(MicroEEFS_NandLoadFile(&F.Dev, PageBuf, "/x", NULL, 10, &Info), EEFS_INVALID_ARGUMENT);
    CHECK_RC(T_Find("", &Info), EEFS_INVALID_ARGUMENT);
    CHECK_RC(T_Find("x", &Info), EEFS_INVALID_ARGUMENT);
    CHECK_RC(T_Find("@A", &Info), EEFS_INVALID_ARGUMENT);
    memset(Long, 'a', sizeof(Long));
    Long[0] = '/';
    Long[EEFS_NAND_MAX_PATH_SIZE] = '\0';       /* 256 characters: one too many */
    CHECK_RC(T_Find(Long, &Info), EEFS_INVALID_ARGUMENT);
    Long[EEFS_NAND_MAX_PATH_SIZE - 1] = '\0';   /* 255: allowed */
    CHECK_RC(T_Find(Long, &Info), EEFS_FILE_NOT_FOUND);

    Dev = F.Dev;
    Dev.Ops = NULL;
    CHECK_RC(MicroEEFS_NandFindFile(&Dev, PageBuf, "/x", &Info), EEFS_INVALID_ARGUMENT);
    NoRead = *F.Dev.Ops;
    NoRead.ReadPage = NULL;
    Dev.Ops = &NoRead;
    CHECK_RC(MicroEEFS_NandFindFile(&Dev, PageBuf, "/x", &Info), EEFS_INVALID_ARGUMENT);
    Dev = F.Dev;
    Dev.Geometry.OobSize = EEFS_NAND_MAX_OOB_SIZE + 1;
    CHECK_RC(MicroEEFS_NandFindFile(&Dev, PageBuf, "/x", &Info), EEFS_INVALID_ARGUMENT);
    Dev = F.Dev;
    Dev.Geometry.PageSize = 512;
    CHECK_RC(MicroEEFS_NandFindFile(&Dev, PageBuf, "/x", &Info), EEFS_INVALID_ARGUMENT);
    Dev = F.Dev;
    Dev.Geometry.DirRegionBlocks = 3;
    CHECK_RC(MicroEEFS_NandFindFile(&Dev, PageBuf, "/x", &Info), EEFS_INVALID_ARGUMENT);
    Dev = F.Dev;
    Dev.Geometry.DirRegionBlocks = EEFS_NAND_MAX_DIR_BLOCKS + 2;
    CHECK_RC(MicroEEFS_NandFindFile(&Dev, PageBuf, "/x", &Info), EEFS_INVALID_ARGUMENT);
    Fx_Free(&F);
}

/*
 * Exported Data
 */

const NandTest_Case_t NandTest_MicroCases[] = {
    { "micro_find_via_journal",                                     micro_find_via_journal },
    { "micro_ring_wrap",                                            micro_ring_wrap },
    { "micro_load_via_journal_all_types",                           micro_load_via_journal_all_types },
    { "micro_rejects_corrupt_page0_and_data",                       micro_rejects_corrupt_page0_and_data },
    { "micro_io_error",                                             micro_io_error },
    { "micro_scan_fallback_when_no_directory",                      micro_scan_fallback_when_no_directory },
    { "micro_no_scan_when_directory_valid_but_page0_bad",           micro_no_scan_when_directory_valid_but_page0_bad },
    { "micro_scan_only_when_both_mirrors_invalid",                  micro_scan_only_when_both_mirrors_invalid },
    { "micro_ab_active_and_explicit_slot",                          micro_ab_active_and_explicit_slot },
    { "micro_ab_fallback_compatible",                               micro_ab_fallback_compatible },
    { "micro_ab_no_fallback_when_incompatible",                     micro_ab_no_fallback_when_incompatible },
    { "micro_hook_override_honoured_and_compatible_rule_applies",   micro_hook_override_honoured_and_compatible_rule_applies },
    { "micro_torn_txn_ignored",                                     micro_torn_txn_ignored },
    { "micro_picks_longest_mirror_chain",                           micro_picks_longest_mirror_chain },
    { "micro_maxsize_too_small",                                    micro_maxsize_too_small },
    { "micro_geometry_mismatch",                                    micro_geometry_mismatch },
    { "micro_bad_directory_blocks",                                 micro_bad_directory_blocks },
    { "micro_invalid_arguments",                                    micro_invalid_arguments },
    { "micro_matches_library_state",                                micro_matches_library_state },
    { NULL,                                                         NULL }
};

/************************/
/*  End of File Comment */
/************************/
