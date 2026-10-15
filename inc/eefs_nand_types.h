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
 * Filename: eefs_nand_types.h
 *
 * Purpose: RAM types of the NAND backend: the volume (EEFS_InodeTable_t), the file descriptor, the directory mirror
 *   and chain state, and the health check report types (the volume embeds a health summary, so they live here).
 *   Included by eefs_fileapi.h when EEFS_BACKEND_NAND is defined; do not include it directly.
 *
 * Design Notes:
 *   All storage is static: the volume holds two copies of the directory state (Base = committed, Working = pending)
 *   and two page buffers sized by EEFS_NAND_MAX_PAGE_SIZE.  See docs/nand-implementation.md section 2.5 for the RAM
 *   budget (about 62 KB per volume at the defaults).
 *
 * References:
 *   docs/nand-implementation.md sections 2.5 and 10.1.
 */

#ifndef _eefs_nand_types_
#define _eefs_nand_types_

/*
 * Includes
 */

#include "common_types.h"
#include "eefs_config.h"
#include "eefs_nand_hal.h"
#include "eefs_nand_format.h"

/*
 * Macro Definitions
 */

/* SlotFlags bits */
#define EEFS_NAND_SLOTFLAG_CORRUPT              0x01    /* page 0 or CRC failed since mount */
#define EEFS_NAND_SLOTFLAG_CROSSLINK            0x02    /* reconcile found another slot claiming one of its blocks */

/* Health check finding codes */
#define EEFS_HEALTH_DIR_MIRROR_MISSING          1   /* Slot = mirror; no valid chain */
#define EEFS_HEALTH_DIR_MIRROR_MISMATCH         2   /* Slot = lagging mirror; Aux0 = its LastSeq, Aux1 = the other's */
#define EEFS_HEALTH_DIR_RECORD_CORRUPT          3   /* Slot = mirror, Block, Aux0 = page: a non-erased page that fails validation inside/after the chain */
#define EEFS_HEALTH_DIR_UNCOMMITTED_TXN         4   /* Slot = mirror, Block, Aux0 = page, Aux1 = Seq: a torn record with a readable header after the chain */
#define EEFS_HEALTH_DIR_CLEANUP_PENDING         5   /* Block: listed in the newest record's CLEANUP and still holds a non-erased page 0 */
#define EEFS_HEALTH_DIR_REBUILT_NOT_PERSISTED   6
#define EEFS_HEALTH_FILE_CORRUPT                10  /* Path, Slot, Block = first block whose CRC failed (or FirstBlock for a bad page 0), Aux0 = 0 page0 / 1 block crc / 2 file crc */
#define EEFS_HEALTH_FILE_IO_ERROR               11  /* Path, Slot, Block, Aux0 = page */
#define EEFS_HEALTH_BLOCK_NEEDS_SCRUB           12  /* Path, Slot, Block, Aux0 = max corrected bits seen in the block */
#define EEFS_HEALTH_AB_A_CORRUPT                20
#define EEFS_HEALTH_AB_B_CORRUPT                21
#define EEFS_HEALTH_AB_ACTIVE_CORRUPT           22
#define EEFS_HEALTH_AB_ACTIVE_MISSING           23
#define EEFS_HEALTH_AB_VERSION_MISMATCH         24  /* Aux0 = Version A, Aux1 = Version B (informational) */
#define EEFS_HEALTH_AB_FALLBACK_UNAVAILABLE     25  /* informational */
#define EEFS_HEALTH_AB_FAILED_FLAG_SET          26
#define EEFS_HEALTH_AB_LOAD_MIXED               27  /* one finding; Path = first entry whose ActiveSlot differs from entry 0's (informational) */
#define EEFS_HEALTH_DIR_PAGE0_MISMATCH          30  /* Path, Slot, Block, Aux0 = reason (1 Path, 2 Slot, 3 BlockCount, 4 copied fields): page 0 disagrees with the directory entry */
/* 31 was DIR_PAGE0_STALE (removed with in-place modify; copied fields can no longer be legitimately stale).  Do not reuse. */
#define EEFS_HEALTH_MAP_MISMATCH                32  /* Block, Aux0 = map value, Aux1 = OWNER expected or 0xFFFF */
#define EEFS_HEALTH_BLOCK_CROSSLINKED           33  /* Path, Slot (second claimant), Block, Aux0 = OWNER of the first claimant */
#define EEFS_HEALTH_BADBLOCK_UNRECORDED         34  /* Block */
#define EEFS_HEALTH_ORPHAN_FILE                 35  /* Path (from page 0), Slot (page 0 Slot), Block, Aux0 = WriteSeq */
#define EEFS_HEALTH_FREE_BELOW_RESERVE          40  /* Aux0 = free blocks, Aux1 = EEFS_NAND_RESERVE_BLOCKS; informational unless free < EEFS_NAND_BADBLOCK_RESERVE */

/* Health summary bits */
#define EEFS_HEALTH_OK                          0x00
#define EEFS_HEALTH_DEGRADED                    0x01    /* any non-informational finding */
#define EEFS_HEALTH_AB_ISSUE                    0x02    /* codes 20-23, 26 */
#define EEFS_HEALTH_DIR_ISSUE                   0x04    /* codes 1-6, 30, 32-35 */
#define EEFS_HEALTH_FILE_ISSUE                  0x08    /* codes 10-12 */
#define EEFS_HEALTH_INFO                        0x10    /* codes 24, 25, 27, and 40 while free >= EEFS_NAND_BADBLOCK_RESERVE */
#define EEFS_HEALTH_OVERFLOW                    0x20    /* findings dropped */
#define EEFS_HEALTH_INCOMPLETE                  0x40    /* not finished (only in a snapshot taken before DONE) */
#define EEFS_HEALTH_SPACE_ISSUE                 0x80    /* code 40 present (with DEGRADED when free < EEFS_NAND_BADBLOCK_RESERVE) */

/*
 * Type Definitions
 */

/* One copy of the directory state */
typedef struct
{
    EEFS_NandEntry_t                    Entry[EEFS_MAX_FILES];
    uint8                               Map[EEFS_NAND_MAX_BLOCKS];
    uint32                              WriteSeqHigh;
} EEFS_NandDirState_t;

/* Write state of one directory mirror */
typedef struct
{
    uint16                              Blocks[EEFS_NAND_MAX_DIR_BLOCKS / 2];  /* good blocks of this mirror, ascending */
    uint16                              Count;
    uint16                              CurIndex;           /* index into Blocks[] of the block holding the newest record, valid if HasState */
    uint16                              NextPage;           /* first free page in Blocks[CurIndex] */
    uint32                              CheckpointSeq;      /* Seq of the checkpoint at page 0 of the current block */
    uint32                              LastSeq;            /* Seq of the last valid record in the chain */
    uint8                               HasState;           /* a valid chain was found at mount or written since */
    uint8                               NeedsResync;        /* write a checkpoint at Seq Volume.LastSeq before the next record */
    uint8                               EnterNewBlock;      /* the next record for this mirror starts a fresh block */
    uint8                               Disabled;           /* no good blocks left */
} EEFS_NandMirror_t;

/* Result of scanning one mirror (docs/nand-implementation.md 5.3); also used by the health check */
typedef struct
{
    uint8                               Valid;              /* chain found */
    uint8                               Torn;               /* page after the chain is not erased, or a higher-Seq block with a torn checkpoint exists */
    uint16                              BlockIdx;           /* ring index of the current block */
    uint16                              NextPage;
    uint32                              CheckpointSeq, LastSeq;
    uint32                              MaxSeqSeen;         /* highest Seq in any readable record header of this mirror, valid or not */
} EEFS_NandChain_t;

/* Health check finding */
typedef struct
{
    uint16                              Code;
    uint8                               Slot;               /* slot or mirror index, 0xFF if n/a */
    uint8                               Reserved;
    uint16                              EntryIndex;         /* 0xFFFF if n/a */
    uint16                              Block;              /* 0xFFFF if n/a */
    uint32                              Aux0, Aux1;
    char                                Path[EEFS_NAND_HEALTH_PATH_SIZE];   /* truncated, NUL-terminated, "" if n/a */
} EEFS_HealthFinding_t;

/* Health check summary */
typedef struct
{
    uint32                              Summary;
    uint32                              FilesChecked, SlotsChecked, PagesRead, CorrectedBits, Restarts, Steps;
    uint32                              FindingCount, Overflow;
    uint32                              DirSeq;             /* Vol->LastSeq when the check completed */
    uint32                              Complete;
    EEFS_HealthFinding_t                Finding[EEFS_NAND_MAX_HEALTH_FINDINGS];
} EEFS_HealthSummary_t;

/* The volume */
typedef struct
{
    EEFS_NandDevice_t                  *Dev;                /* from BaseAddress */
    uint32                              Mounted;
    EEFS_NandDirState_t                 Base;               /* committed state as on media (plus detected media facts, section 4) */
    EEFS_NandDirState_t                 Working;            /* pending state */
    uint8                               ForceDirty[(EEFS_MAX_FILES + 7) / 8];  /* entries to include in the next record regardless of diff */
    uint32                              AllocCursor;        /* round-robin allocation cursor, RAM only (6.9) */
    uint8                               SlotFlags[EEFS_MAX_FILES][2];          /* EEFS_NAND_SLOTFLAG_* */
    uint32                              NextWriteSeq;
    uint32                              LastSeq;            /* newest committed record Seq */
    uint32                              VolumeChangeCounter;
    uint32                              FormatTime;
    uint8                               DirectoryRebuilt;   /* mounted from a RAM rebuild, not yet persisted */
    uint8                               Reserved[3];
    EEFS_NandMirror_t                   Mirror[2];
    uint16                              PendingCleanup[EEFS_NAND_MAX_CLEANUP_BLOCKS];
    uint16                              PendingCleanupCount;
    uint16                              PendingCleanupBase; /* leading entries that came from the media (mount), not from the batch */
    uint16                              PageSize, PagesPerBlock;   /* cached from geometry */
    uint32                              BlockBytes, MaxBlocksPerFile;
    uint8                               PageBuf[EEFS_NAND_MAX_PAGE_SIZE];  /* scratch for building pages / verify */
    uint8                               OobBuf[EEFS_NAND_MAX_OOB_SIZE];
    uint8                               CacheBuf[EEFS_NAND_MAX_PAGE_SIZE]; /* one-page read cache */
    uint32                              CacheBlock, CachePage;
    uint8                               CacheValid;
    struct EEFS_HealthReport           *ActiveReport;       /* health check in progress, or NULL */
    EEFS_HealthSummary_t                LastReport;         /* last completed health report */
} EEFS_InodeTable_t;

/* File descriptor (read only) */
typedef struct
{
    uint32                              InUse;
    uint32                              Mode;               /* always EEFS_FREAD */
    EEFS_InodeTable_t                  *InodeTable;
    uint32                              InodeIndex;         /* entry index */
    uint8                               Slot;               /* slot actually opened */
    uint8                               Fallback;           /* opened the inactive copy under the compatible-fallback rule */
    uint16                              FirstBlock;
    uint32                              FileSize;
    uint32                              ByteOffset;
    uint32                              CurK;               /* index into Blocks[] of CurBlock, 0xFFFFFFFF = none */
    uint16                              CurBlock;
} EEFS_FileDescriptor_t;

/* Resumable health check state (docs/nand-implementation.md section 10).  The caller owns the memory and must not
 * change it while the check is attached to a volume; S may be read between steps (a snapshot carries
 * EEFS_HEALTH_INCOMPLETE).  Every cursor lives here, so the check can stop after any page read and resume. */
typedef struct EEFS_HealthReport
{
    EEFS_HealthSummary_t                S;
    EEFS_InodeTable_t                  *Volume;
    uint32                              ChangeCounter;      /* VolumeChangeCounter at (re)start */
    uint8                               Phase;              /* 0 init, 1 directory, 2 files, 3 ab, 4 scan, 5 done */
    uint8                               Sub;                /* sub-step inside the phase */
    uint8                               Mirror, Slot;
    uint16                              EntryIndex, BlockIdx;
    uint32                              Page, Block, DataOffset;
    uint32                              RunBlockCrc, RunFileCrc, MaxCorrectedInBlock;
    uint32                              ExpectBlockCrc;
    uint32                              StepReads;          /* page reads in the current step */
    uint8                               Marker;             /* bad-marker page cursor (index into the marker pages) */
    uint8                               Verdict;            /* phase 2: 1 ok so far, 2 corrupt (page 0 disagrees with the entry) */
    uint8                               FirstActive;        /* phase 3: ActiveSlot of the first A/B entry, 0xFF none yet */
    uint8                               Reserved;
    uint32                              K;                  /* phase 2: index into Blocks[] */
    uint32                              FirstBlock, BlockCount, FileSize, FileCrc32;   /* phase 2: the slot's page 0 */
    /* phase 1: the chain walk of one candidate block */
    uint32                              WalkBlock, WalkPage, WalkSub, WalkRecSeq, WalkRecCount, WalkLast, WalkCkpt;
    uint8                               WalkValid, WalkTail;    /* WalkTail: 0 none, else the finding code of the page after the chain */
    uint8                               TornCand[2];        /* phase 1: a candidate of the mirror had a torn checkpoint */
    uint32                              WalkTailPage, WalkTailSeq;
    uint32                              ChainBlock[2];      /* block of each mirror's chosen chain */
    uint8                               ChainTail[2];       /* finding code for the page after the chosen chain, 0 none */
    uint8                               Reserved2[2];
    uint32                              ChainTailPage[2], ChainTailSeq[2];
    uint32                              CandSeq[EEFS_NAND_MAX_DIR_BLOCKS];      /* phase 1 scratch */
    uint8                               CandState[EEFS_NAND_MAX_DIR_BLOCKS];    /* 0 none, 1 candidate, 2 walked, 3 torn */
    EEFS_NandChain_t                    Chain[2];
    uint8                               FailedAtStart[(EEFS_MAX_FILES + 7) / 8];  /* ActiveFailed when the check (re)started */
    uint8                               SetByCheck[(EEFS_MAX_FILES + 7) / 8];     /* ActiveFailed set by this check (kept across restarts) */
    uint8                               Claimed[(EEFS_NAND_MAX_BLOCKS + 7) / 8]; /* blocks claimed by a validated page 0 */
    uint8                               SlotResult[EEFS_MAX_FILES][2];          /* 0 unchecked, 1 ok, 2 corrupt, 3 io error, 4 not in use */
} EEFS_HealthReport_t;

#endif

/************************/
/*  End of File Comment */
/************************/
