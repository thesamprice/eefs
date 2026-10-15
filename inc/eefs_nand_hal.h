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
 * Filename: eefs_nand_hal.h
 *
 * Purpose: Hardware abstraction of the EEFS NAND backend: the device geometry, the three page/block operations a BSP
 *   provides, and the file type / slot / type-info definitions shared by the library, the bootloader library and the
 *   applications.
 *
 * Design Notes:
 *
 * HAL contract:
 *   ReadPage(Ctx, Block, Page, Data, Oob)
 *     Data receives PageSize bytes and Oob receives OobSize bytes; either may be NULL to skip that area.  Returns
 *     >= 0 on success (the number of bit errors the ECC corrected, 0 = clean), EEFS_NAND_READ_UNCORRECTABLE when the
 *     ECC could not correct the page, or EEFS_NAND_READ_IO_ERROR on a controller or bus error.  An erased page must
 *     read as all 0xFF with a status >= 0 (controllers that flag erased pages as ECC errors must special-case them).
 *     A Data == NULL read of a factory-bad block must return the raw OOB bytes; if it returns an error, the library
 *     treats the block as bad.
 *   ProgramPage(Ctx, Block, Page, Data, Oob)
 *     Data or Oob may be NULL to leave that area untouched.  Programming only the OOB is used for the bad-block marker
 *     and is best effort (the return value is ignored there).  The library programs the pages of a block in ascending
 *     order and never programs a page twice between erases (except the best-effort OOB-only bad-block marker).
 *     Returns 0 or -1.
 *   EraseBlock(Ctx, Block)
 *     Returns 0 or -1.  The library marks a block bad after a failed erase.
 *   All calls are made under EEFS_LIB_LOCK.  Blocks are numbered 0..BlockCount-1 across all chips; chip select is the
 *   HAL's business.
 *
 * References:
 *   docs/nand-format.md (on-media format), docs/nand-implementation.md section 2.3.
 */

#ifndef _eefs_nand_hal_
#define _eefs_nand_hal_

/*
 * Includes
 */

#include "common_types.h"
#include "eefs_config.h"

/*
 * Macro Definitions
 */

/* ReadPage status codes.  ReadPage >= 0: success, value = corrected bit count (0 = clean) */
#define EEFS_NAND_READ_UNCORRECTABLE    (-1)    /* ECC could not correct the page */
#define EEFS_NAND_READ_IO_ERROR         (-2)    /* controller/bus error */

/* Slots */
#define EEFS_NAND_SLOT_A                0
#define EEFS_NAND_SLOT_B                1
#define EEFS_NAND_SLOT_NONE             0xFF    /* "unspecified" in API calls; "single copy" on media */

/* File types (metadata page FileType) */
#define EEFS_FILE_TYPE_OTHER            0
#define EEFS_FILE_TYPE_BOOT_FIT         1
#define EEFS_FILE_TYPE_BOOT_ELF         2
#define EEFS_FILE_TYPE_BOOT_DTB         3
#define EEFS_FILE_TYPE_CFS_APP          4
#define EEFS_FILE_TYPE_CFS_LIB          5
#define EEFS_FILE_TYPE_MAX              EEFS_FILE_TYPE_CFS_LIB

/* BOOT_ELF Flags */
#define EEFS_NAND_ELF_FLAG_ENTRY_VALID  0x1     /* EntryPoint is valid and overrides the ELF header */

/* Geometry.BadMarkerPages: pages that carry the factory bad-block marker */
#define EEFS_NAND_BADMARKER_PAGE0       0x1
#define EEFS_NAND_BADMARKER_PAGE1       0x2
#define EEFS_NAND_BADMARKER_LAST        0x4

/*
 * Type Definitions
 */

typedef struct
{
    uint32                              PageSize;           /* 2048, 4096 or 8192 */
    uint32                              OobSize;            /* <= EEFS_NAND_MAX_OOB_SIZE */
    uint32                              PagesPerBlock;
    uint32                              BlockCount;         /* <= EEFS_NAND_MAX_BLOCKS */
    uint32                              DirRegionBlocks;    /* even, >= 4, <= EEFS_NAND_MAX_DIR_BLOCKS */
    uint32                              BadMarkerOffset;    /* OOB byte index */
    uint32                              BadMarkerPages;     /* EEFS_NAND_BADMARKER_* mask, non-zero */
    uint32                              EccScrubThreshold;  /* corrected bits per page that trigger BLOCK_NEEDS_SCRUB; 0 = never */
} EEFS_NandGeometry_t;

typedef struct
{
    int32   (*ReadPage)(void *Ctx, uint32 Block, uint32 Page, void *Data, void *Oob);
    int32   (*ProgramPage)(void *Ctx, uint32 Block, uint32 Page, const void *Data, const void *Oob);
    int32   (*EraseBlock)(void *Ctx, uint32 Block);
} EEFS_NandOps_t;

typedef struct
{
    EEFS_NandGeometry_t                 Geometry;
    const EEFS_NandOps_t               *Ops;
    void                               *Ctx;
} EEFS_NandDevice_t;

/* Type-specific metadata, selected by the file type (docs/nand-format.md section 3.2) */
typedef union
{
    struct {
        uint64                          DestAddress;        /* where the loader places the FIT image */
    }                                   Fit;
    struct {
        uint64                          DestAddress;        /* where the raw ELF file is staged */
        uint64                          EntryPoint;         /* entry override */
        uint32                          Flags;              /* EEFS_NAND_ELF_FLAG_* */
    }                                   Elf;
    struct {
        uint64                          DestAddress;
    }                                   Dtb;
    struct {
        char                            AppName[EEFS_NAND_CFS_NAME_SIZE];
        char                            EntrySymbol[EEFS_NAND_CFS_SYMBOL_SIZE];
        uint16                          Priority;
        uint8                           ExceptionAction;    /* 0 = restart the app, 1 = reset the processor */
        uint32                          StackSize;
        uint64                          LoadAddress;        /* 0 = let the loader choose */
    }                                   App;
    struct {
        char                            LibName[EEFS_NAND_CFS_NAME_SIZE];
        char                            EntrySymbol[EEFS_NAND_CFS_SYMBOL_SIZE];   /* library init function */
        uint64                          LoadAddress;
    }                                   Lib;
} EEFS_NandTypeInfo_t;

typedef struct
{
    uint8                               InUse;
    uint8                               Corrupt;            /* RAM flag: page 0 or CRC check failed since mount (output only) */
    uint16                              FirstBlock;
    uint16                              AllocBlocks;
    uint16                              Reserved;
    uint32                              FileSize;
    uint32                              Version;
    uint32                              WriteSeq;
    uint32                              FileCrc32;
    uint32                              ModificationDate;
} EEFS_NandSlotInfo_t;

typedef struct
{
    char                                Path[EEFS_NAND_MAX_PATH_SIZE];  /* output: stored path (no @A/@B) */
    uint8                               FileType;           /* input + output */
    uint8                               Slot;               /* input: slot to write when the name has no suffix (A, B or NONE); output: resolved slot */
    uint8                               IsAB;               /* output */
    uint8                               ActiveSlot;         /* output */
    uint8                               FallbackCompatible; /* output */
    uint8                               ActiveFailed;       /* output */
    uint8                               Reserved[2];
    uint32                              Attributes;         /* input + output */
    uint32                              Version;            /* input + output */
    uint32                              CreationDate;       /* output */
    uint32                              FileSize;           /* output: resolved slot */
    uint32                              FileCrc32;          /* output: resolved slot */
    EEFS_NandTypeInfo_t                 Type;               /* input + output (from the resolved slot's page 0) */
    EEFS_NandSlotInfo_t                 SlotInfo[2];        /* output */
} EEFS_NandFileInfo_t;

#endif

/************************/
/*  End of File Comment */
/************************/
