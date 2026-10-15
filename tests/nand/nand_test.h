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
 * Filename: nand_test.h
 *
 * Purpose: Host test harness of the EEFS NAND backend: CHECK macros, the case/suite tables and the geometry given on
 *   the command line (eefs_nand_test <suite> <page> <oob> <ppb> <blocks> [case-substring]).
 *
 */

#ifndef _nand_test_
#define _nand_test_

/*
 * Includes
 */

#include <stdio.h>
#include <string.h>
#include "common_types.h"
#include "eefs_fileapi.h"
#include "eefs_nand_hal.h"
#include "eefs_nand_format.h"
#include "nand_sim.h"

/*
 * Macro Definitions
 */

/* Defaults for the geometry fields that are not on the command line */
#define NANDTEST_DIR_BLOCKS         16
#define NANDTEST_BADMARKER_OFFSET   0
#define NANDTEST_BADMARKER_PAGES    (EEFS_NAND_BADMARKER_PAGE0 | EEFS_NAND_BADMARKER_PAGE1)

/* Records a failure and continues the case */
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            NandTest_Fail(__FILE__, __LINE__, #cond);                               \
        }                                                                           \
    } while (0)

/* Records a failure showing both values (compared as unsigned long) */
#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        unsigned long NandTest_A = (unsigned long)(a);                              \
        unsigned long NandTest_B = (unsigned long)(b);                              \
        if (NandTest_A != NandTest_B) {                                             \
            NandTest_FailEq(__FILE__, __LINE__, #a, #b, NandTest_A, NandTest_B);    \
        }                                                                           \
    } while (0)

/* Signed variant, for return codes */
#define CHECK_RC(a, b)                                                              \
    do {                                                                            \
        long NandTest_A = (long)(a);                                                \
        long NandTest_B = (long)(b);                                                \
        if (NandTest_A != NandTest_B) {                                             \
            NandTest_FailRc(__FILE__, __LINE__, #a, #b, NandTest_A, NandTest_B);    \
        }                                                                           \
    } while (0)

/*
 * Type Definitions
 */

typedef void (*NandTest_Func_t)(void);

typedef struct
{
    const char         *Name;
    NandTest_Func_t     Func;
} NandTest_Case_t;          /* tables end with { NULL, NULL } */

/*
 * Exported Data
 */

/* Geometry of this run (from the command line, plus the NANDTEST_* defaults) */
extern EEFS_NandGeometry_t  NandTest_Geo;

/* Case tables of the suites */
extern const NandTest_Case_t NandTest_LayoutCases[];
extern const NandTest_Case_t NandTest_SimCases[];
extern const NandTest_Case_t NandTest_JournalCases[];
extern const NandTest_Case_t NandTest_FileopsCases[];
extern const NandTest_Case_t NandTest_PowercutCases[];

/*
 * Exported Functions
 */

void        NandTest_Fail(const char *File, int Line, const char *Expr);
void        NandTest_FailEq(const char *File, int Line, const char *A, const char *B, unsigned long VA, unsigned long VB);
void        NandTest_FailRc(const char *File, int Line, const char *A, const char *B, long VA, long VB);
int         NandTest_CaseFailed(void);          /* TRUE once the running case has recorded a failure */

/* A simulator with the run's geometry (aborts the run when it cannot be created) */
NandSim_t  *NandTest_NewSim(void);

#endif

/************************/
/*  End of File Comment */
/************************/
