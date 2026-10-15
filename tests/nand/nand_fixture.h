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
 * Filename: nand_fixture.h
 *
 * Purpose: Test fixture builder: produces EEFS NAND volumes in the simulator with the phase 1 format encoders only
 *   (no journal writer, no file API), so readers such as the bootloader library can be tested before the library's
 *   writers exist.
 *
 * Design Notes:
 *   The fixture keeps its own directory state (entries + block map) and writes, through NandSim_RawWrite, exactly
 *   what docs/nand-format.md describes: metadata page + data pages per file (section 3), CHECKPOINT and TXN records in
 *   both mirror rings (section 4).  It does not try to be the library: no cleanup erases (superseded first blocks stay
 *   on the media and are listed in a CLEANUP item, as after an interrupted cleanup), no block reuse (the data
 *   allocator only moves forward, so stale copies are never overwritten), no bad-block handling in the data area.
 *
 *   Typical use:
 *     Fx_Format(&F);                       empty volume, Seq 1 checkpoint in both mirrors
 *     Fx_PutFile(&F, 0, A, NONE, "/x", ...);  writes a copy and attaches it to entry 0 slot A (staged)
 *     Fx_Commit(&F);                       one TXN (or a checkpoint in a fresh ring block when the block is full)
 *   Fields of F.Entry[] may be edited directly followed by Fx_Touch to stage them.  F.MirrorMask selects the mirrors
 *   the next commits write (a mirror that missed a commit gets a checkpoint in a new block, as the library does).
 *
 * References:
 *   docs/nand-format.md, docs/nand-implementation.md section 14 (phase 7).
 */

#ifndef _nand_fixture_
#define _nand_fixture_

/*
 * Includes
 */

#include "nand_test.h"

/*
 * Macro Definitions
 */

#define FX_MAX_REC_PAGES        32      /* largest record the fixture builds */
#define FX_MAX_CLEANUP          64

/*
 * Type Definitions
 */

typedef struct
{
    uint32                  Block;
    uint32                  Page;
    uint32                  Count;
} Fx_RecLoc_t;

typedef struct
{
    NandSim_t              *Sim;
    EEFS_NandDevice_t       Dev;
    EEFS_NandEntry_t        Entry[EEFS_MAX_FILES];
    uint8                   Changed[EEFS_MAX_FILES];
    uint8                   Map[EEFS_NAND_MAX_BLOCKS];
    uint8                   Used[EEFS_NAND_MAX_BLOCKS];  /* data blocks the fixture ever wrote (never reallocated) */
    uint32                  MapLo, MapHi;           /* changed map range, MapLo > MapHi when none */
    uint16                  Cleanup[FX_MAX_CLEANUP];
    uint32                  CleanupCount;
    uint32                  Seq;                    /* Seq of the last commit */
    uint32                  WriteSeq;               /* last WriteSeq assigned (the next copy gets WriteSeq + 1) */
    uint32                  NextBlock;              /* data allocator cursor (only moves forward) */
    uint32                  CurBlock[2];            /* current ring block of each mirror */
    uint32                  NextPage[2];
    uint32                  MirrorSeq[2];           /* Seq of the last record written to each mirror */
    uint8                   MirrorMask;             /* bit m: the next commits write mirror m (default 3) */
    uint8                   ForceCheckpoint;        /* the next commit enters a new ring block */
    uint8                   Reserved[2];
    uint32                  VolBlockCount;          /* BlockCount written in VOLUME items (default: the geometry's) */
    Fx_RecLoc_t             LastRec[2];             /* where the last record of each mirror went */
} Fx_t;

/*
 * Exported Functions
 */

/* New simulator with the run's geometry; empty volume (Seq 1 checkpoint at page 0 of the first block of each mirror) */
void    Fx_Format(Fx_t *F);
/* Simulator only (blank device, no directory) */
void    Fx_Blank(Fx_t *F);
/* Writes the Seq 1 checkpoints on a Fx_Blank volume (after the test marked factory-bad blocks, for example) */
void    Fx_WriteFormat(Fx_t *F);
void    Fx_Free(Fx_t *F);

/* Deterministic content */
void    Fx_Pattern(uint8 *Data, uint32 Size, uint32 Seed);

/* Writes one copy (data pages, then the metadata page) at the allocator cursor and returns its first block.  PageSlot
 * is the metadata page's Slot field (A, B or NONE).  Type may be NULL.  Out (may be NULL) receives the slot record
 * the directory would hold.  Does not touch the directory state. */
uint32  Fx_WriteCopy(Fx_t *F, const char *Path, uint8 PageSlot, uint8 FileType, uint32 Version,
                     const EEFS_NandTypeInfo_t *Type, const uint8 *Data, uint32 Size, EEFS_NandSlot_t *Out);

/* Fx_WriteCopy + attach to Entry[Index].Slot[Slot] (the previous copy's blocks are freed, its first block is listed
 * for cleanup).  Sets InUse, Path, FileType; IsAB is set when PageSlot is A or B. */
uint32  Fx_PutFile(Fx_t *F, uint32 Index, uint8 Slot, uint8 PageSlot, const char *Path, uint8 FileType,
                   uint32 Version, const EEFS_NandTypeInfo_t *Type, const uint8 *Data, uint32 Size);

/* Removes the entry (all slots) */
void    Fx_Remove(Fx_t *F, uint32 Index);

/* Renames the way the library does (docs/nand-format.md 3.5): new first block with a new metadata page and a copy of
 * the first block's data pages; the other data blocks are kept.  Applied to every slot in use. */
void    Fx_Rename(Fx_t *F, uint32 Index, const char *NewPath);

/* Stages Entry[Index] after a direct edit */
void    Fx_Touch(Fx_t *F, uint32 Index);

/* Writes the staged changes as one record with Seq = F->Seq + 1 */
void    Fx_Commit(Fx_t *F);

/* Reads back one page's raw data (no counters) */
void    Fx_RawPage(Fx_t *F, uint32 Block, uint32 Page, uint8 *Data);

#endif

/************************/
/*  End of File Comment */
/************************/
