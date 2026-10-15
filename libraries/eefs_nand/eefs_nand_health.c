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
 * Filename: eefs_nand_health.c
 *
 * Purpose: Resumable health check of a mounted NAND volume (docs/nand-implementation.md section 10).
 *
 * Design Notes:
 *   Phase 1: the public functions validate their arguments and return EEFS_UNSUPPORTED_OPTION.  Phase 6 implements
 *   the check.
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

/* Attaches Report to the volume and starts a check.  Not implemented yet (phase 6). */
int32 EEFS_LibHealthCheckStart(EEFS_InodeTable_t *InodeTable, EEFS_HealthReport_t *Report)
{
    if ((InodeTable == NULL) || (Report == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibHealthCheckStart() */

/* Runs the check for at most MaxPageReads page reads.  Not implemented yet (phase 6). */
int32 EEFS_LibHealthCheckStep(EEFS_InodeTable_t *InodeTable, uint32 MaxPageReads)
{
    (void)MaxPageReads;

    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibHealthCheckStep() */

/* Copies the last completed report.  Not implemented yet (phase 6). */
int32 EEFS_LibHealthCheckSummary(EEFS_InodeTable_t *InodeTable, EEFS_HealthSummary_t *Out)
{
    if ((InodeTable == NULL) || (Out == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibHealthCheckSummary() */

/* Detaches the active report.  Not implemented yet (phase 6). */
int32 EEFS_LibHealthCheckCancel(EEFS_InodeTable_t *InodeTable)
{
    if (InodeTable == NULL) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of EEFS_LibHealthCheckCancel() */

/************************/
/*  End of File Comment */
/************************/
