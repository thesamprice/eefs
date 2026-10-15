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
 * Filename: nand_test_main.c
 *
 * Purpose: Entry point of the EEFS NAND host tests.
 *
 *   eefs_nand_test <suite> <page> <oob> <ppb> <blocks> [case-substring]
 *
 *   Runs every case of the suite (or those whose name contains the substring) on the given geometry, prints PASS or
 *   FAIL per case, and exits non-zero on any failure, on an unknown suite, or when no case matched.
 *
 */

/*
 * Includes
 */

#include <stdlib.h>
#include "nand_test.h"

/*
 * Type Definitions
 */

typedef struct
{
    const char             *Name;
    const NandTest_Case_t  *Cases;
} NandTest_Suite_t;

/*
 * Local Data
 */

EEFS_NandGeometry_t         NandTest_Geo;

extern const NandTest_Case_t NandTest_MicroCases[];     /* test_micro.c */
extern const NandTest_Case_t NandTest_AbCases[];        /* test_ab.c */
extern const NandTest_Case_t NandTest_RebuildCases[];   /* test_rebuild.c */
extern const NandTest_Case_t NandTest_HealthCases[];    /* test_health.c */

static const NandTest_Suite_t NandTest_Suites[] = {
    { "layout", NandTest_LayoutCases },
    { "sim",    NandTest_SimCases },
    { "journal", NandTest_JournalCases },
    { "micro",  NandTest_MicroCases },
    { "fileops", NandTest_FileopsCases },
    { "powercut", NandTest_PowercutCases },
    { "ab",     NandTest_AbCases },
    { "rebuild", NandTest_RebuildCases },
    { "health", NandTest_HealthCases },
    { NULL,     NULL }
};

static int                  NandTest_Failures;      /* failures in the running case */

/*
 * Function Definitions
 */

void NandTest_Fail(const char *File, int Line, const char *Expr)
{
    printf("    %s:%d: CHECK(%s) failed\n", File, Line, Expr);
    NandTest_Failures++;
}

void NandTest_FailEq(const char *File, int Line, const char *A, const char *B, unsigned long VA, unsigned long VB)
{
    printf("    %s:%d: %s == %s failed: 0x%lx (%lu) != 0x%lx (%lu)\n", File, Line, A, B, VA, VA, VB, VB);
    NandTest_Failures++;
}

void NandTest_FailRc(const char *File, int Line, const char *A, const char *B, long VA, long VB)
{
    printf("    %s:%d: %s == %s failed: %ld != %ld\n", File, Line, A, B, VA, VB);
    NandTest_Failures++;
}

int NandTest_CaseFailed(void)
{
    return(NandTest_Failures != 0);
}

NandSim_t *NandTest_NewSim(void)
{
    NandSim_t *Sim = NandSim_Create(&NandTest_Geo);

    if (Sim == NULL) {
        printf("cannot create the NAND simulator\n");
        exit(2);
    }
    return(Sim);
}

static uint32 NandTest_Arg(const char *Text)
{
    return((uint32)strtoul(Text, NULL, 0));
}

int main(int argc, char *argv[])
{
    const NandTest_Suite_t *Suite;
    const NandTest_Case_t  *Case;
    const char             *Filter = NULL;
    int                     Run = 0;
    int                     Failed = 0;

    if ((argc < 6) || (argc > 7)) {
        printf("usage: %s <suite> <page> <oob> <ppb> <blocks> [case-substring]\n", argv[0]);
        return(2);
    }
    if (argc == 7) {
        Filter = argv[6];
    }

    for (Suite = NandTest_Suites; Suite->Name != NULL; Suite++) {
        if (strcmp(Suite->Name, argv[1]) == 0) break;
    }
    if (Suite->Name == NULL) {
        printf("unknown suite '%s'\n", argv[1]);
        return(2);
    }

    memset(&NandTest_Geo, 0, sizeof(NandTest_Geo));
    NandTest_Geo.PageSize = NandTest_Arg(argv[2]);
    NandTest_Geo.OobSize = NandTest_Arg(argv[3]);
    NandTest_Geo.PagesPerBlock = NandTest_Arg(argv[4]);
    NandTest_Geo.BlockCount = NandTest_Arg(argv[5]);
    NandTest_Geo.DirRegionBlocks = NANDTEST_DIR_BLOCKS;
    NandTest_Geo.BadMarkerOffset = NANDTEST_BADMARKER_OFFSET;
    NandTest_Geo.BadMarkerPages = NANDTEST_BADMARKER_PAGES;
    NandTest_Geo.EccScrubThreshold = 0;

    if ((NandTest_Geo.PageSize < EEFS_NAND_MIN_PAGE_SIZE) || (NandTest_Geo.PageSize > EEFS_NAND_MAX_PAGE_SIZE) ||
        (NandTest_Geo.OobSize < 1) || (NandTest_Geo.OobSize > EEFS_NAND_MAX_OOB_SIZE) ||
        (NandTest_Geo.PagesPerBlock < 2) ||
        (NandTest_Geo.BlockCount <= NandTest_Geo.DirRegionBlocks) || (NandTest_Geo.BlockCount > EEFS_NAND_MAX_BLOCKS)) {
        printf("geometry %lu/%lu/%lu/%lu is outside this build's limits (max page %lu, oob %lu, blocks %lu)\n",
               (unsigned long)NandTest_Geo.PageSize, (unsigned long)NandTest_Geo.OobSize,
               (unsigned long)NandTest_Geo.PagesPerBlock, (unsigned long)NandTest_Geo.BlockCount,
               (unsigned long)EEFS_NAND_MAX_PAGE_SIZE, (unsigned long)EEFS_NAND_MAX_OOB_SIZE,
               (unsigned long)EEFS_NAND_MAX_BLOCKS);
        return(2);
    }

    printf("suite %s, geometry page %lu oob %lu ppb %lu blocks %lu\n", Suite->Name,
           (unsigned long)NandTest_Geo.PageSize, (unsigned long)NandTest_Geo.OobSize,
           (unsigned long)NandTest_Geo.PagesPerBlock, (unsigned long)NandTest_Geo.BlockCount);

    for (Case = Suite->Cases; Case->Name != NULL; Case++) {
        if ((Filter != NULL) && (strstr(Case->Name, Filter) == NULL)) {
            continue;
        }
        NandTest_Failures = 0;
        EEFS_LibInit();
        Case->Func();
        Run++;
        if (NandTest_Failures == 0) {
            printf("PASS %s\n", Case->Name);
        }
        else {
            printf("FAIL %s (%d checks)\n", Case->Name, NandTest_Failures);
            Failed++;
        }
        fflush(stdout);
    }

    printf("%d case(s), %d failed\n", Run, Failed);
    if (Run == 0) {
        return(2);
    }
    return((Failed == 0) ? 0 : 1);
}

/************************/
/*  End of File Comment */
/************************/
