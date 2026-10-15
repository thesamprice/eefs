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
 * Filename: microeefs_nand.h
 *
 * Purpose: Bootloader library for the EEFS NAND backend: finds a file through the directory journal (or, when no
 *   directory exists, by scanning the metadata pages) and loads it with every CRC checked.  Read only; needs only the
 *   HAL's ReadPage, the geometry and one caller-supplied page buffer.
 *
 * Design Notes:
 *   Links only microeefs_nand.c, eefs_nand_format.c and eefs_crc32.c.  No globals except the slot hook pointer.
 *   Algorithms: docs/nand-format.md sections 7.2-7.4.
 *
 * References:
 *   docs/nand-implementation.md section 9.
 */

#ifndef _microeefs_nand_
#define _microeefs_nand_

/*
 * Includes
 */

#include "common_types.h"
#include "eefs_fileapi.h"
#include "eefs_nand_hal.h"

/*
 * Type Definitions
 */

/* BSP slot hook: returns the slot to try first given the directory's choice (default: DirectorySlot) */
typedef uint8 (*MicroEEFS_NandSlotHook_t)(const char *Name, uint8 DirectorySlot, void *HookCtx);

typedef struct
{
    EEFS_NandFileInfo_t                 Info;               /* Path, FileType, Version, Attributes, FileSize, FileCrc32, Type, IsAB, ActiveSlot,
                                                               FallbackCompatible, ActiveFailed (directory value OR this boot's failure), SlotInfo[2] */
    uint8                               LoadedSlot;         /* slot whose data was loaded/validated */
    uint8                               UsedScan;           /* 1 if the directory was unusable and the scan fallback was used */
    uint8                               DirectoryValid;
    uint8                               Reserved;
    uint32                              DirSeq;             /* LastSeq of the chain used, 0 when scanned */
} MicroEEFS_NandInfo_t;

/*
 * Exported Functions
 */

/* Sets the BSP slot hook; NULL restores the default (returns DirectorySlot) */
void    MicroEEFS_NandSetSlotHook(MicroEEFS_NandSlotHook_t Hook, void *HookCtx);

/* Finds Name and validates the metadata page of the slot to load.  PageBuffer is Dev->Geometry.PageSize bytes.
 * EEFS_SUCCESS, EEFS_INVALID_ARGUMENT, EEFS_FILE_NOT_FOUND, EEFS_GEOMETRY_MISMATCH, EEFS_CORRUPT_FILE, EEFS_IO_ERROR,
 * EEFS_NOT_FORMATTED. */
int32   MicroEEFS_NandFindFile(const EEFS_NandDevice_t *Dev, uint8 *PageBuffer, const char *Name, MicroEEFS_NandInfo_t *Info);

/* Finds and loads Name into Dest (at most MaxSize bytes), checking every CRC.  Return codes as FindFile, plus
 * EEFS_NO_SPACE_LEFT_ON_DEVICE when FileSize > MaxSize. */
int32   MicroEEFS_NandLoadFile(const EEFS_NandDevice_t *Dev, uint8 *PageBuffer, const char *Name, void *Dest, uint32 MaxSize,
                               MicroEEFS_NandInfo_t *Info);

#endif

/************************/
/*  End of File Comment */
/************************/
