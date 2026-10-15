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
 * Filename: test_sim.c
 *
 * Purpose: Suite "sim": the RAM NAND simulator (tests/nand/nand_sim.h) that every later suite relies on: basic
 *   program/read/erase, lazy allocation, factory-bad blocks, fault injection, power cuts in every torn mode, and the
 *   NAND programming-rule checks.
 *
 */

/*
 * Includes
 */

#include <stdlib.h>
#include "nand_test.h"

/*
 * Local Data
 */

static uint8    Data[EEFS_NAND_MAX_PAGE_SIZE];
static uint8    Back[EEFS_NAND_MAX_PAGE_SIZE];
static uint8    Oob[EEFS_NAND_MAX_OOB_SIZE];
static uint8    OobBack[EEFS_NAND_MAX_OOB_SIZE];

/*
 * Helpers
 */

static void S_Fill(uint8 *P, uint32 Length, uint32 Seed)
{
    uint32  i;

    for (i = 0; i < Length; i++) {
        P[i] = (uint8)((i * 7) + Seed + (i >> 8));
    }
}

static int S_AllBytes(const uint8 *P, uint32 Length, uint8 Value)
{
    uint32  i;

    for (i = 0; i < Length; i++) {
        if (P[i] != Value) return(0);
    }
    return(1);
}

/* Programs Block/Page through the HAL with the pattern Seed (data and OOB) */
static int32 S_Program(const EEFS_NandDevice_t *Dev, uint32 Block, uint32 Page, uint32 Seed)
{
    S_Fill(Data, Dev->Geometry.PageSize, Seed);
    S_Fill(Oob, Dev->Geometry.OobSize, Seed + 1);
    return(Dev->Ops->ProgramPage(Dev->Ctx, Block, Page, Data, Oob));
}

/* TRUE if Block/Page holds the pattern Seed (read through the HAL) */
static int S_Holds(const EEFS_NandDevice_t *Dev, uint32 Block, uint32 Page, uint32 Seed)
{
    S_Fill(Data, Dev->Geometry.PageSize, Seed);
    if (Dev->Ops->ReadPage(Dev->Ctx, Block, Page, Back, NULL) != 0) {
        return(0);
    }
    return(memcmp(Data, Back, Dev->Geometry.PageSize) == 0);
}

/* TRUE if Block/Page reads erased through the HAL */
static int S_ReadsErased(const EEFS_NandDevice_t *Dev, uint32 Block, uint32 Page)
{
    if (Dev->Ops->ReadPage(Dev->Ctx, Block, Page, Back, OobBack) != 0) {
        return(0);
    }
    return(S_AllBytes(Back, Dev->Geometry.PageSize, 0xFF) && S_AllBytes(OobBack, Dev->Geometry.OobSize, 0xFF));
}

/*
 * Cases
 */

static void sim_erase_program_read(void)
{
    NandSim_t          *Sim = NandTest_NewSim();
    EEFS_NandDevice_t   Dev;
    uint32              PageSize = NandTest_Geo.PageSize;
    uint32              OobSize = NandTest_Geo.OobSize;
    uint32              Last = NandTest_Geo.BlockCount - 1;

    NandSim_FillDevice(Sim, &Dev);
    CHECK(memcmp(&Dev.Geometry, &NandTest_Geo, sizeof(NandTest_Geo)) == 0);
    CHECK(Dev.Ops == NandSim_Ops());
    CHECK(Dev.Ctx == (void *)Sim);

    /* a fresh device reads erased, status 0 */
    memset(Back, 0, PageSize);
    memset(OobBack, 0, OobSize);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 3, 5, Back, OobBack), 0);
    CHECK(S_AllBytes(Back, PageSize, 0xFF));
    CHECK(S_AllBytes(OobBack, OobSize, 0xFF));
    CHECK(NandSim_PageIsErased(Sim, 3, 5));

    /* program data + OOB, read back each area separately */
    CHECK_RC(S_Program(&Dev, 3, 0, 11), 0);
    CHECK(S_Holds(&Dev, 3, 0, 11));
    memset(OobBack, 0, OobSize);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 3, 0, NULL, OobBack), 0);
    S_Fill(Oob, OobSize, 12);
    CHECK(memcmp(Oob, OobBack, OobSize) == 0);
    CHECK(NandSim_PageIsProgrammed(Sim, 3, 0));
    CHECK(NandSim_PageIsErased(Sim, 3, 0) == 0);

    /* data only: the OOB stays erased */
    S_Fill(Data, PageSize, 21);
    CHECK_RC(Dev.Ops->ProgramPage(Dev.Ctx, 3, 1, Data, NULL), 0);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 3, 1, Back, OobBack), 0);
    CHECK(memcmp(Data, Back, PageSize) == 0);
    CHECK(S_AllBytes(OobBack, OobSize, 0xFF));

    /* the last page of the last block works too */
    CHECK_RC(S_Program(&Dev, Last, 0, 1), 0);
    NandSim_SetStrict(Sim, 0);
    CHECK_RC(S_Program(&Dev, Last, NandTest_Geo.PagesPerBlock - 1, 31), 0);
    NandSim_SetStrict(Sim, 1);
    CHECK(S_Holds(&Dev, Last, NandTest_Geo.PagesPerBlock - 1, 31));

    /* other pages and blocks are untouched */
    CHECK(S_ReadsErased(&Dev, 3, 2));
    CHECK(S_ReadsErased(&Dev, 4, 0));

    /* erase */
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 3), 0);
    CHECK(S_ReadsErased(&Dev, 3, 0));
    CHECK(S_ReadsErased(&Dev, 3, 1));
    CHECK(NandSim_PageIsProgrammed(Sim, 3, 0) == 0);
    CHECK_RC(S_Program(&Dev, 3, 0, 41), 0);                            /* programmable again after the erase */
    CHECK(S_Holds(&Dev, 3, 0, 41));

    /* counters count calls */
    CHECK_EQ(NandSim_Programs(Sim), 5);
    CHECK_EQ(NandSim_Erases(Sim), 1);
    CHECK_EQ(NandSim_BlockErases(Sim, 3), 1);
    CHECK_EQ(NandSim_BlockErases(Sim, 4), 0);
    CHECK(NandSim_Reads(Sim) >= 10);
    CHECK_EQ(NandSim_Violations(Sim), 0);
    NandSim_ResetCounters(Sim);
    CHECK_EQ(NandSim_Reads(Sim), 0);
    CHECK_EQ(NandSim_Programs(Sim), 0);
    CHECK_EQ(NandSim_Erases(Sim), 0);
    CHECK_EQ(NandSim_BlockErases(Sim, 3), 0);

    /* out of range fails (and is a violation in strict mode) */
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, NandTest_Geo.BlockCount, 0, Back, NULL), EEFS_NAND_READ_IO_ERROR);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 0, NandTest_Geo.PagesPerBlock, Back, NULL), EEFS_NAND_READ_IO_ERROR);
    CHECK_RC(S_Program(&Dev, NandTest_Geo.BlockCount, 0, 1), -1);
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, NandTest_Geo.BlockCount), -1);
    CHECK_EQ(NandSim_Violations(Sim), 4);

    /* raw access */
    S_Fill(Data, PageSize, 51);
    NandSim_RawWrite(Sim, 9, 4, Data, NULL);
    CHECK(S_Holds(&Dev, 9, 4, 51));
    NandSim_RawRead(Sim, 9, 4, Back, OobBack);
    CHECK(memcmp(Data, Back, PageSize) == 0);
    CHECK(S_AllBytes(OobBack, OobSize, 0xFF));
    NandSim_CorruptBytes(Sim, 9, 4, 10, 3);
    NandSim_RawRead(Sim, 9, 4, Back, NULL);
    CHECK_EQ(Back[9], Data[9]);
    CHECK_EQ(Back[10], Data[10] ^ 0x5A);
    CHECK_EQ(Back[12], Data[12] ^ 0x5A);
    CHECK_EQ(Back[13], Data[13]);
    NandSim_CorruptBytes(Sim, 9, 4, PageSize, 1);                       /* reaches the OOB */
    NandSim_RawRead(Sim, 9, 4, NULL, OobBack);
    CHECK_EQ(OobBack[0], 0xFF ^ 0x5A);
    NandSim_RawErase(Sim, 9);
    CHECK(S_ReadsErased(&Dev, 9, 4));

    NandSim_Destroy(Sim);
}

/* Storage is allocated per programmed block and released by erase */
static void sim_lazy_alloc(void)
{
    NandSim_t          *Sim = NandTest_NewSim();
    EEFS_NandDevice_t   Dev;
    size_t              Base;
    uint32              b;
    uint32              p;
    size_t              BlockBytes = ((size_t)NandTest_Geo.PageSize + NandTest_Geo.OobSize + 1) * NandTest_Geo.PagesPerBlock;

    NandSim_FillDevice(Sim, &Dev);
    Base = NandSim_AllocatedBytes(Sim);
    CHECK(Base < (size_t)(256 * 1024));

    /* erasing every block allocates nothing */
    for (b = 0; b < NandTest_Geo.BlockCount; b++) {
        CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, b), 0);
    }
    CHECK_EQ(NandSim_AllocatedBytes(Sim), Base);

    /* write 3 whole blocks */
    for (b = 100; b < 103; b++) {
        for (p = 0; p < NandTest_Geo.PagesPerBlock; p++) {
            CHECK_RC(S_Program(&Dev, b, p, b + p), 0);
        }
    }
    CHECK_EQ(NandSim_AllocatedBytes(Sim), Base + (3 * BlockBytes));
    CHECK(NandSim_AllocatedBytes(Sim) < (size_t)(4UL * 1024 * 1024));
    CHECK(S_Holds(&Dev, 101, NandTest_Geo.PagesPerBlock - 1, 101 + NandTest_Geo.PagesPerBlock - 1));

    /* reading erased blocks does not allocate; erasing written ones releases them */
    CHECK(S_ReadsErased(&Dev, 200, 0));
    CHECK_EQ(NandSim_AllocatedBytes(Sim), Base + (3 * BlockBytes));
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 101), 0);
    CHECK_EQ(NandSim_AllocatedBytes(Sim), Base + (2 * BlockBytes));

    /* a clone copies the content, the counters, and costs the same */
    {
        NandSim_t          *Copy = NandSim_Clone(Sim);
        EEFS_NandDevice_t   CopyDev;

        CHECK(Copy != NULL);
        if (Copy != NULL) {
            NandSim_FillDevice(Copy, &CopyDev);
            CHECK_EQ(NandSim_AllocatedBytes(Copy), NandSim_AllocatedBytes(Sim));
            CHECK_EQ(NandSim_Programs(Copy), NandSim_Programs(Sim));
            CHECK_EQ(NandSim_Erases(Copy), NandSim_Erases(Sim));
            CHECK(S_Holds(&CopyDev, 102, 3, 105));
            CHECK(S_ReadsErased(&CopyDev, 101, 0));
            /* independent: erasing in the clone leaves the original intact */
            CHECK_RC(CopyDev.Ops->EraseBlock(CopyDev.Ctx, 102), 0);
            CHECK(S_ReadsErased(&CopyDev, 102, 3));
            CHECK(S_Holds(&Dev, 102, 3, 105));
            NandSim_Destroy(Copy);
        }
    }

    NandSim_Destroy(Sim);
}

/* Factory-bad blocks: marker on the selected pages, uncorrectable data, no program/erase */
static void sim_factory_bad(void)
{
    NandSim_t          *Sim = NandTest_NewSim();
    EEFS_NandDevice_t   Dev;
    EEFS_NandGeometry_t Geo = NandTest_Geo;
    uint32              Off = NandTest_Geo.BadMarkerOffset;
    uint32              LastPage = NandTest_Geo.PagesPerBlock - 1;

    NandSim_FillDevice(Sim, &Dev);
    NandSim_SetFactoryBad(Sim, 20);

    /* Data != NULL: uncorrectable */
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 20, 0, Back, OobBack), EEFS_NAND_READ_UNCORRECTABLE);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 20, 5, Back, NULL), EEFS_NAND_READ_UNCORRECTABLE);

    /* Data == NULL: raw OOB with the marker on page 0 and page 1 (the run's BadMarkerPages) */
    memset(OobBack, 0xFF, sizeof(OobBack));
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 20, 0, NULL, OobBack), 0);
    CHECK_EQ(OobBack[Off], 0x00);
    memset(OobBack, 0xFF, sizeof(OobBack));
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 20, 1, NULL, OobBack), 0);
    CHECK_EQ(OobBack[Off], 0x00);
    CHECK(NandSim_PageIsErased(Sim, 20, 3) == 0);                       /* random content, not erased */

    /* program and erase fail and change nothing */
    CHECK_RC(S_Program(&Dev, 20, 0, 1), -1);
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 20), -1);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 20, 0, NULL, OobBack), 0);
    CHECK_EQ(OobBack[Off], 0x00);

    /* neighbours are good */
    CHECK(S_ReadsErased(&Dev, 19, 0));
    CHECK(S_ReadsErased(&Dev, 21, 0));
    CHECK_RC(S_Program(&Dev, 21, 0, 3), 0);

    /* RawErase turns it back into a good block */
    NandSim_RawErase(Sim, 20);
    CHECK(S_ReadsErased(&Dev, 20, 0));
    CHECK_RC(S_Program(&Dev, 20, 0, 1), 0);
    NandSim_Destroy(Sim);

    /* another marker position: last page only, OOB byte 5 */
    Geo.BadMarkerOffset = 5;
    Geo.BadMarkerPages = EEFS_NAND_BADMARKER_LAST;
    Sim = NandSim_Create(&Geo);
    CHECK(Sim != NULL);
    if (Sim == NULL) return;
    NandSim_FillDevice(Sim, &Dev);
    NandSim_SetFactoryBad(Sim, 7);
    memset(OobBack, 0xFF, sizeof(OobBack));
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 7, LastPage, NULL, OobBack), 0);
    CHECK_EQ(OobBack[5], 0x00);
    NandSim_Destroy(Sim);
}

/* Read injections: corrected-bit status, uncorrectable and I/O errors, wildcards, Times */
static void sim_inject_read(void)
{
    NandSim_t          *Sim = NandTest_NewSim();
    EEFS_NandDevice_t   Dev;
    uint32              PageSize = NandTest_Geo.PageSize;

    NandSim_FillDevice(Sim, &Dev);
    CHECK_RC(S_Program(&Dev, 30, 0, 5), 0);
    CHECK_RC(S_Program(&Dev, 30, 1, 6), 0);

    /* corrected bits: the status is reported once, the data is correct */
    NandSim_InjectRead(Sim, 30, 0, 3, 1);
    S_Fill(Data, PageSize, 5);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 30, 0, Back, NULL), 3);
    CHECK(memcmp(Data, Back, PageSize) == 0);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 30, 0, Back, NULL), 0);

    /* uncorrectable twice: wrong data, then clean */
    NandSim_InjectRead(Sim, 30, 1, EEFS_NAND_READ_UNCORRECTABLE, 2);
    S_Fill(Data, PageSize, 6);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 30, 1, Back, NULL), EEFS_NAND_READ_UNCORRECTABLE);
    CHECK(memcmp(Data, Back, PageSize) != 0);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 30, 0, Back, NULL), 0);         /* other page unaffected */
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 30, 1, Back, NULL), EEFS_NAND_READ_UNCORRECTABLE);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 30, 1, Back, NULL), 0);
    CHECK(memcmp(Data, Back, PageSize) == 0);

    /* I/O error forever on any page of block 31, including OOB-only reads and erased pages */
    NandSim_InjectRead(Sim, 31, NANDSIM_ANY, EEFS_NAND_READ_IO_ERROR, -1);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 31, 0, Back, NULL), EEFS_NAND_READ_IO_ERROR);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 31, 7, NULL, OobBack), EEFS_NAND_READ_IO_ERROR);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 31, 7, Back, NULL), EEFS_NAND_READ_IO_ERROR);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 32, 7, Back, NULL), 0);

    /* any block, page 2 */
    NandSim_InjectRead(Sim, NANDSIM_ANY, 2, 1, -1);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 50, 2, Back, NULL), 1);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 51, 2, Back, NULL), 1);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 51, 3, Back, NULL), 0);

    /* injections do not affect programs and erases */
    CHECK_RC(S_Program(&Dev, 31, 0, 9), 0);
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 31), 0);

    /* ClearInjections */
    NandSim_ClearInjections(Sim);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 31, 0, Back, NULL), 0);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 50, 2, Back, NULL), 0);

    /* Reboot also clears them */
    NandSim_InjectRead(Sim, 30, 0, EEFS_NAND_READ_IO_ERROR, -1);
    NandSim_Reboot(Sim);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 30, 0, Back, NULL), 0);

    NandSim_Destroy(Sim);
}

/* Program and erase failures: the call fails, the media is unchanged, Times is honoured */
static void sim_program_erase_fail(void)
{
    NandSim_t          *Sim = NandTest_NewSim();
    EEFS_NandDevice_t   Dev;

    NandSim_FillDevice(Sim, &Dev);

    NandSim_InjectProgramFail(Sim, 40, 0, 1);
    CHECK_RC(S_Program(&Dev, 40, 0, 1), -1);
    CHECK(NandSim_PageIsErased(Sim, 40, 0));
    CHECK(NandSim_PageIsProgrammed(Sim, 40, 0) == 0);
    CHECK_RC(S_Program(&Dev, 40, 0, 1), 0);                            /* consumed: the retry works */
    CHECK(S_Holds(&Dev, 40, 0, 1));

    NandSim_InjectProgramFail(Sim, 40, NANDSIM_ANY, -1);
    CHECK_RC(S_Program(&Dev, 40, 1, 2), -1);
    CHECK_RC(S_Program(&Dev, 40, 1, 2), -1);
    CHECK_RC(S_Program(&Dev, 41, 0, 2), 0);                            /* other block fine */
    NandSim_ClearInjections(Sim);
    CHECK_RC(S_Program(&Dev, 40, 1, 2), 0);

    NandSim_InjectEraseFail(Sim, 40, 2);
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 40), -1);
    CHECK(S_Holds(&Dev, 40, 0, 1));                                     /* content intact */
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 41), 0);
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 40), -1);
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 40), 0);
    CHECK(S_ReadsErased(&Dev, 40, 0));
    CHECK_EQ(NandSim_BlockErases(Sim, 40), 3);                          /* attempts are counted */

    NandSim_InjectEraseFail(Sim, NANDSIM_ANY, -1);
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 41), -1);
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 42), -1);
    NandSim_Reboot(Sim);
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 42), 0);

    CHECK_EQ(NandSim_Violations(Sim), 0);
    NandSim_Destroy(Sim);
}

/* One torn-mode run: Budget operations succeed, the next is torn, then everything fails until Reboot */
static void S_PowerCutRun(int Mode)
{
    NandSim_t          *Sim = NandTest_NewSim();
    NandSim_t          *Saved;
    EEFS_NandDevice_t   Dev;
    uint32              PageSize = NandTest_Geo.PageSize;
    uint32              Half = NandTest_Geo.PagesPerBlock / 2;
    uint32              Last = NandTest_Geo.PagesPerBlock - 1;
    uint32              Reads;
    uint32              i;
    int                 Ok;

    NandSim_FillDevice(Sim, &Dev);
    CHECK_RC(S_Program(&Dev, 5, 0, 100), 0);
    CHECK_RC(S_Program(&Dev, 6, 0, 101), 0);
    Saved = NandSim_Clone(Sim);
    CHECK(Saved != NULL);

    NandSim_SetPowerBudget(Sim, 2, Mode);
    CHECK(NandSim_PowerCutHappened(Sim) == 0);
    Reads = NandSim_Reads(Sim);
    CHECK(S_Holds(&Dev, 5, 0, 100));                                    /* reads do not consume budget */
    CHECK(S_Holds(&Dev, 5, 0, 100));
    CHECK_EQ(NandSim_Reads(Sim), Reads + 2);
    CHECK_RC(S_Program(&Dev, 5, 1, 102), 0);                           /* 1 of 2 */
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 6), 0);                      /* 2 of 2 */
    CHECK(NandSim_PowerCutHappened(Sim) == 0);

    /* the torn program */
    CHECK_RC(S_Program(&Dev, 5, 2, 103), -1);
    CHECK(NandSim_PowerCutHappened(Sim) != 0);
    S_Fill(Data, PageSize, 103);
    NandSim_RawRead(Sim, 5, 2, Back, OobBack);
    if (Mode == NANDSIM_TORN_NONE) {
        CHECK(NandSim_PageIsErased(Sim, 5, 2));
    }
    else if (Mode == NANDSIM_TORN_PARTIAL) {
        CHECK(memcmp(Back, Data, PageSize / 2) == 0);
        CHECK(S_AllBytes(Back + (PageSize / 2), PageSize / 2, 0xFF));
        CHECK(S_AllBytes(OobBack, NandTest_Geo.OobSize, 0xFF));
        CHECK(NandSim_PageIsProgrammed(Sim, 5, 2));
    }
    else {
        Ok = 1;
        for (i = 0; i < PageSize; i++) {
            if (Back[i] != (uint8)(Data[i] ^ 0xA5)) Ok = 0;
        }
        CHECK(Ok);
        CHECK(NandSim_PageIsProgrammed(Sim, 5, 2));
    }

    /* after the cut every program/erase fails and changes nothing; reads still work */
    CHECK_RC(S_Program(&Dev, 7, 0, 104), -1);
    CHECK(NandSim_PageIsErased(Sim, 7, 0));
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 5), -1);
    CHECK(S_Holds(&Dev, 5, 0, 100));
    CHECK(S_Holds(&Dev, 5, 1, 102));
    CHECK(S_ReadsErased(&Dev, 6, 0));                                   /* the erase before the cut happened */

    /* reboot: content persists, operations work again, no budget */
    NandSim_Reboot(Sim);
    CHECK(NandSim_PowerCutHappened(Sim) == 0);
    CHECK(S_Holds(&Dev, 5, 0, 100));
    CHECK_RC(S_Program(&Dev, 7, 0, 104), 0);
    CHECK(S_Holds(&Dev, 7, 0, 104));
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 7), 0);
    for (i = 0; i < 20; i++) {
        CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 8), 0);
    }

    /* a torn erase (budget 0: the very next operation is torn) of a block with data in every page */
    for (i = 0; i <= Last; i++) {
        CHECK_RC(S_Program(&Dev, 9, i, 200 + i), 0);
    }
    NandSim_SetPowerBudget(Sim, 0, Mode);
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 9), -1);
    CHECK(NandSim_PowerCutHappened(Sim) != 0);
    if (Mode == NANDSIM_TORN_NONE) {
        CHECK(S_Holds(&Dev, 9, 0, 200));
        CHECK(S_Holds(&Dev, 9, Last, 200 + Last));
    }
    else if (Mode == NANDSIM_TORN_PARTIAL) {
        CHECK(NandSim_PageIsErased(Sim, 9, 0));
        CHECK(NandSim_PageIsErased(Sim, 9, Half - 1));
        CHECK(S_Holds(&Dev, 9, Half, 200 + Half));
        CHECK(S_Holds(&Dev, 9, Last, 200 + Last));
    }
    else {
        S_Fill(Data, PageSize, 200);
        NandSim_RawRead(Sim, 9, 0, Back, NULL);
        Ok = 1;
        for (i = 0; i < PageSize; i++) {
            if (Back[i] != (uint8)(Data[i] ^ 0xA5)) Ok = 0;
        }
        CHECK(Ok);
        CHECK(NandSim_PageIsErased(Sim, 9, Last) == 0);
    }
    NandSim_Reboot(Sim);

    /* the clone taken before the cut is unaffected (the sweep pattern restores from it) */
    if (Saved != NULL) {
        EEFS_NandDevice_t SavedDev;

        NandSim_FillDevice(Saved, &SavedDev);
        CHECK(NandSim_PowerCutHappened(Saved) == 0);
        CHECK(S_ReadsErased(&SavedDev, 5, 1));
        CHECK(S_Holds(&SavedDev, 6, 0, 101));
        NandSim_Destroy(Saved);
    }

    CHECK_EQ(NandSim_Violations(Sim), 0);
    NandSim_Destroy(Sim);
}

/* A power-cut sweep over a fixed operation sequence, as the later suites write them */
static void S_PowerCutSweep(int Mode)
{
    NandSim_t          *Image = NandTest_NewSim();
    NandSim_t          *Sim;
    EEFS_NandDevice_t   Dev;
    uint32              Cut;
    uint32              Done;
    uint32              Ops = 0;
    int                 i;

    for (Cut = 0; ; Cut++) {
        Sim = NandSim_Clone(Image);
        CHECK(Sim != NULL);
        if (Sim == NULL) break;
        NandSim_FillDevice(Sim, &Dev);
        NandSim_SetPowerBudget(Sim, Cut, Mode);

        /* the "operation": erase block 12, program pages 0..3 */
        Done = 0;
        if (Dev.Ops->EraseBlock(Dev.Ctx, 12) == 0) Done++;
        for (i = 0; i < 4; i++) {
            if (S_Program(&Dev, 12, (uint32)i, 300 + (uint32)i) == 0) Done++;
        }

        if (NandSim_PowerCutHappened(Sim) == 0) {
            CHECK_EQ(Done, 5);
            Ops = Cut;
            NandSim_Destroy(Sim);
            break;
        }
        /* exactly Cut operations completed before the torn one */
        CHECK_EQ(Done, Cut);
        NandSim_Reboot(Sim);
        for (i = 0; (uint32)i + 1 < Cut; i++) {
            CHECK(S_Holds(&Dev, 12, (uint32)i, 300 + (uint32)i));
        }
        NandSim_Destroy(Sim);
    }
    CHECK_EQ(Ops, 5);       /* budget 5 is the first that does not cut the 5-operation sequence */
    NandSim_Destroy(Image);
}

static void sim_power_budget_and_reboot(void)
{
    S_PowerCutRun(NANDSIM_TORN_NONE);
    S_PowerCutRun(NANDSIM_TORN_PARTIAL);
    S_PowerCutRun(NANDSIM_TORN_GARBAGE);
    S_PowerCutSweep(NANDSIM_TORN_NONE);
    S_PowerCutSweep(NANDSIM_TORN_PARTIAL);
    S_PowerCutSweep(NANDSIM_TORN_GARBAGE);
}

/* NOP = 1 and in-order programming, OOB-only marker exemption, non-strict mode */
static void sim_strict_nop_and_order(void)
{
    NandSim_t          *Sim = NandTest_NewSim();
    EEFS_NandDevice_t   Dev;
    uint32              PageSize = NandTest_Geo.PageSize;
    uint32              Off = NandTest_Geo.BadMarkerOffset;
    uint32              i;

    NandSim_FillDevice(Sim, &Dev);

    /* programming a page twice */
    CHECK_RC(S_Program(&Dev, 60, 0, 1), 0);
    CHECK_RC(S_Program(&Dev, 60, 0, 2), -1);
    CHECK_EQ(NandSim_Violations(Sim), 1);
    CHECK(S_Holds(&Dev, 60, 0, 1));

    /* programming with an all-0xFF page still counts as programmed */
    memset(Data, 0xFF, PageSize);
    CHECK_RC(Dev.Ops->ProgramPage(Dev.Ctx, 60, 1, Data, NULL), 0);
    CHECK(NandSim_PageIsErased(Sim, 60, 1));
    CHECK(NandSim_PageIsProgrammed(Sim, 60, 1));
    CHECK_RC(S_Program(&Dev, 60, 1, 3), -1);
    CHECK_EQ(NandSim_Violations(Sim), 2);

    /* out of order: page 3 while page 2 is erased */
    CHECK_RC(S_Program(&Dev, 60, 3, 4), -1);
    CHECK_EQ(NandSim_Violations(Sim), 3);
    CHECK(NandSim_PageIsErased(Sim, 60, 3));
    CHECK_RC(S_Program(&Dev, 60, 2, 5), 0);
    CHECK_RC(S_Program(&Dev, 60, 3, 4), 0);
    CHECK_RC(S_Program(&Dev, 61, 1, 4), -1);                           /* page 1 of an erased block */
    CHECK_EQ(NandSim_Violations(Sim), 4);

    /* the OOB-only bad-block marker is allowed on a programmed page and on an erased block */
    memset(Oob, 0xFF, sizeof(Oob));
    Oob[Off] = 0x00;
    CHECK_RC(Dev.Ops->ProgramPage(Dev.Ctx, 60, 0, NULL, Oob), 0);
    CHECK_RC(Dev.Ops->ProgramPage(Dev.Ctx, 62, 0, NULL, Oob), 0);
    CHECK_EQ(NandSim_Violations(Sim), 4);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 60, 0, NULL, OobBack), 0);
    CHECK_EQ(OobBack[Off], 0x00);
    CHECK(S_Holds(&Dev, 60, 0, 1));                                     /* data untouched */
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 62, 0, NULL, OobBack), 0);
    CHECK_EQ(OobBack[Off], 0x00);

    /* an erase resets the rules */
    CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 60), 0);
    CHECK_RC(S_Program(&Dev, 60, 0, 6), 0);

    /* non-strict: double programming is allowed and ANDs the bits, nothing is counted */
    NandSim_SetStrict(Sim, 0);
    memset(Data, 0xF0, PageSize);
    CHECK_RC(Dev.Ops->ProgramPage(Dev.Ctx, 63, 0, Data, NULL), 0);
    memset(Data, 0x3C, PageSize);
    CHECK_RC(Dev.Ops->ProgramPage(Dev.Ctx, 63, 0, Data, NULL), 0);
    CHECK_RC(Dev.Ops->ReadPage(Dev.Ctx, 63, 0, Back, NULL), 0);
    CHECK(S_AllBytes(Back, PageSize, 0x30));
    CHECK_RC(S_Program(&Dev, 63, 5, 7), 0);                            /* out of order allowed */
    CHECK_RC(S_Program(&Dev, NandTest_Geo.BlockCount + 1, 0, 7), -1);  /* out of range still fails */
    CHECK_EQ(NandSim_Violations(Sim), 4);

    /* strict again: out of range is a violation */
    NandSim_SetStrict(Sim, 1);
    CHECK_RC(Dev.Ops->ProgramPage(Dev.Ctx, 0, NandTest_Geo.PagesPerBlock, Data, NULL), -1);
    CHECK_EQ(NandSim_Violations(Sim), 5);

    for (i = 0; i < 2; i++) {
        CHECK_RC(Dev.Ops->EraseBlock(Dev.Ctx, 63), 0);
    }
    NandSim_Destroy(Sim);
}

/*
 * Suite table
 */

const NandTest_Case_t NandTest_SimCases[] = {
    { "sim_erase_program_read",         sim_erase_program_read },
    { "sim_lazy_alloc",                 sim_lazy_alloc },
    { "sim_factory_bad",                sim_factory_bad },
    { "sim_inject_read",                sim_inject_read },
    { "sim_program_erase_fail",         sim_program_erase_fail },
    { "sim_power_budget_and_reboot",    sim_power_budget_and_reboot },
    { "sim_strict_nop_and_order",       sim_strict_nop_and_order },
    { NULL,                             NULL }
};

/************************/
/*  End of File Comment */
/************************/
