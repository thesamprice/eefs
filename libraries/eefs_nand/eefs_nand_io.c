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
 * Filename: eefs_nand_io.c
 *
 * Purpose: HAL wrappers of the NAND backend: cached page read, program with read-back verify, erase, bad-block marking and
 *   the factory bad-marker test (docs/nand-implementation.md section 3.1).
 *
 * Design Notes:
 *   Phase 2 implements the functions declared under "I/O" in eefs_nand_internal.h here.
 *
 */

/*
 * Includes
 */

#include "common_types.h"
#include "eefs_fileapi.h"
#include "eefs_macros.h"
#include "eefs_nand_internal.h"

/************************/
/*  End of File Comment */
/************************/
