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
 * Filename: microeefs_nand.c
 *
 * Purpose: Bootloader library for the EEFS NAND backend (see microeefs_nand.h).
 *
 * Design Notes:
 *   Phase 1: the lookup functions validate their arguments and return EEFS_UNSUPPORTED_OPTION.  Phase 7 implements
 *   them (docs/nand-implementation.md section 9).
 *
 */

/*
 * Includes
 */

#include "common_types.h"
#include "eefs_fileapi.h"
#include "eefs_nand_format.h"
#include "microeefs_nand.h"

/*
 * Local Data
 */

static MicroEEFS_NandSlotHook_t MicroEEFS_NandSlotHook = NULL;
static void                    *MicroEEFS_NandSlotHookCtx = NULL;

/*
 * Function Definitions
 */

/* Sets the BSP slot hook; NULL restores the default */
void MicroEEFS_NandSetSlotHook(MicroEEFS_NandSlotHook_t Hook, void *HookCtx)
{
    MicroEEFS_NandSlotHook = Hook;
    MicroEEFS_NandSlotHookCtx = (Hook != NULL) ? HookCtx : NULL;

} /* End of MicroEEFS_NandSetSlotHook() */

/* Finds a file.  Not implemented yet (phase 7). */
int32 MicroEEFS_NandFindFile(const EEFS_NandDevice_t *Dev, uint8 *PageBuffer, const char *Name, MicroEEFS_NandInfo_t *Info)
{
    if ((Dev == NULL) || (Dev->Ops == NULL) || (PageBuffer == NULL) || (Name == NULL) || (Info == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of MicroEEFS_NandFindFile() */

/* Finds and loads a file.  Not implemented yet (phase 7). */
int32 MicroEEFS_NandLoadFile(const EEFS_NandDevice_t *Dev, uint8 *PageBuffer, const char *Name, void *Dest, uint32 MaxSize,
                             MicroEEFS_NandInfo_t *Info)
{
    (void)MaxSize;

    if ((Dev == NULL) || (Dev->Ops == NULL) || (PageBuffer == NULL) || (Name == NULL) || (Dest == NULL) ||
        (Info == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    return(EEFS_UNSUPPORTED_OPTION);

} /* End of MicroEEFS_NandLoadFile() */

/************************/
/*  End of File Comment */
/************************/
