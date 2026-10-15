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
 * Filename: eefs_nand_rebuild.c
 *
 * Purpose: Rebuilding the NAND directory from the file metadata pages (docs/nand-format.md section 6), and the
 *   directory maintenance calls built on it: EEFS_LibRebuildDirectory and EEFS_LibReclaimOrphans
 *   (docs/nand-implementation.md section 8).
 *
 * Design Notes:
 *   Phase 1: the public functions validate their arguments and return EEFS_UNSUPPORTED_OPTION.  Phase 4 implements
 *   EEFS_NandRebuild and EEFS_LibRebuildDirectory, phase 6 EEFS_NandReclaimOrphans and EEFS_LibReclaimOrphans.
 *
 */

/*
 * Includes
 */

#include "common_types.h"
#include "eefs_fileapi.h"
#include "eefs_macros.h"
#include "eefs_nand.h"
#include "eefs_nand_internal.h"

/*
 * Function Definitions
 */

/* Discards the RAM directory and rebuilds it from the metadata pages (section 8.3).  Not implemented yet (phase 4). */
int32 EEFS_LibRebuildDirectory(EEFS_InodeTable_t *InodeTable)
{
    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibRebuildDirectory() */

/* Erases free blocks that still hold a valid metadata page (section 8.4).  Not implemented yet (phase 6). */
int32 EEFS_LibReclaimOrphans(EEFS_InodeTable_t *InodeTable)
{
    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibReclaimOrphans() */

/************************/
/*  End of File Comment */
/************************/
