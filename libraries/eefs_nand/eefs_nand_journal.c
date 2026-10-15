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
 * Filename: eefs_nand_journal.c
 *
 * Purpose: Directory journal of the NAND backend: format, directory scan and replay, commit, abort, resync, pending
 *   cleanup and the mirror rings (docs/nand-implementation.md sections 3.2 and 5).
 *
 * Design Notes:
 *   Phase 2 implements the functions declared under "Journal" in eefs_nand_internal.h here.
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
