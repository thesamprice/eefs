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
 * Filename: nand_vol.h
 *
 * Purpose: Helpers shared by the "fileops" and "powercut" suites: a simulator-backed volume (V_Vol), deterministic
 *   file contents, write/read-back checks, directory snapshots and media invariants.
 *
 * Design Notes:
 *   File contents are described by V_File_t (path, size, pattern seed and up to V_MAX_MODS flipped bytes) so a test
 *   can state "the old or the new content" without keeping copies around: V_Fill regenerates the bytes.
 *
 */

#ifndef _nand_vol_
#define _nand_vol_

/*
 * Includes
 */

#include "nand_test.h"
#include "eefs_nand_internal.h"

/*
 * Macro Definitions
 */

#define V_BUF_SIZE          (8UL * 1024UL * 1024UL)
#define V_MAX_MODS          4
#define V_MAX_FILES         16

/*
 * Type Definitions
 */

/* A file content: Pattern(Seed) of Size bytes with the bytes at Mod[0..ModCount) XORed with 0x5A */
typedef struct
{
    const char         *Path;
    uint32              Size;
    uint32              Seed;
    uint32              ModCount;
    uint32              Mod[V_MAX_MODS];
    uint32              Attributes;
} V_File_t;

/* Directory state compared by the tests: entries (dates zeroed), data-region map (FREEING as FREE), committed Seq */
typedef struct
{
    EEFS_NandEntry_t    Entry[EEFS_MAX_FILES];
    uint8               Map[EEFS_NAND_MAX_BLOCKS];
    uint32              LastSeq;
} V_Snap_t;

/*
 * Exported Data
 */

extern EEFS_InodeTable_t    V_Vol;
extern EEFS_InodeTable_t    V_Aux;
extern EEFS_NandDevice_t    V_Dev;
extern NandSim_t           *V_Sim;
extern uint8               *V_Data;             /* V_BUF_SIZE bytes each, allocated by the first V_Attach */
extern uint8               *V_Data2;
extern uint8               *V_Read;
extern uint8                V_Page[EEFS_NAND_MAX_PAGE_SIZE];

/*
 * Exported Functions
 */

uint32      V_BlockBytes(void);
uint32      V_D0(void);                                 /* data bytes of a file's first block */
uint32      V_SizeForBlocks(uint32 Blocks);             /* largest size that needs exactly Blocks blocks */
uint32      V_FileOffset(uint32 K, uint32 Within);      /* file offset of byte Within of position K */

void        V_Attach(NandSim_t *Sim);
int32       V_Mount(EEFS_InodeTable_t *Vol);
void        V_NewVolume(void);                          /* run geometry, formatted, mounted on V_Vol */
void        V_NewVolumeBlocks(uint32 BlockCount);       /* same with a smaller device */
void        V_Remount(void);                            /* fresh mount of V_Vol on the same device (a reboot) */
void        V_Destroy(void);
uint32      V_Ops(void);                                /* programs + erases so far */

void        V_Fill(uint8 *Buf, const V_File_t *F);
const uint8 *V_Content(const V_File_t *F);              /* the content, generated once and cached */
void        V_Meta(EEFS_NandFileInfo_t *Meta, uint8 FileType, uint32 Version);
int32       V_Write(const V_File_t *F);                 /* EEFS_LibWriteFile(V_Vol, F) with type OTHER, version 1 */
int32       V_WriteName(const char *Name, const V_File_t *F);
void        V_CheckFile(EEFS_InodeTable_t *Vol, const V_File_t *F);   /* Stat, Open/Read (exact), VerifyFile */
void        V_CheckFileEx(EEFS_InodeTable_t *Vol, const V_File_t *F, boolean Verify);
void        V_CheckAbsent(EEFS_InodeTable_t *Vol, const char *Path);

/* Slot A of Path: entry index (or -1), and the slot record */
int32       V_Entry(EEFS_InodeTable_t *Vol, const char *Path);
const EEFS_NandSlot_t *V_SlotA(EEFS_InodeTable_t *Vol, const char *Path);
/* Blocks[] and CRCs of the metadata page at Block, read raw from the simulator; returns BlockCount (0 if invalid) */
uint32      V_RawBlocks(uint32 Block, uint16 *Blocks, uint32 *Crcs);

void        V_Take(const EEFS_InodeTable_t *Vol, V_Snap_t *S);
int         V_Same(const V_Snap_t *A, const V_Snap_t *B);

/* Media invariants */
void        V_CheckMirrorsAgree(EEFS_InodeTable_t *Vol);
void        V_CheckOrphansComplete(const EEFS_InodeTable_t *Live);
            /* every valid metadata page: positions 1..n-1 match their CRCs (Live != NULL: skip its slots' first blocks,
             * which VerifyFile covers) */
void        V_CheckSlotsValid(EEFS_InodeTable_t *Vol);  /* no slot flagged at mount */

#endif

/************************/
/*  End of File Comment */
/************************/
