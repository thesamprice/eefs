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
 * Filename: eefs_nand_format.h
 *
 * Purpose: Constants and pure encode/decode functions of the EEFS NAND on-media format (layout version 1).
 *
 * Design Notes:
 *   docs/nand-format.md is normative.  Everything on the media is little-endian, field by field; nothing here is ever
 *   written to the media as a C struct.  The decoded structures below are RAM representations with natural alignment.
 *   Offsets are computed from the configured sizes (EEFS_NAND_MAX_PATH_SIZE, EEFS_NAND_CFS_NAME_SIZE,
 *   EEFS_NAND_CFS_SYMBOL_SIZE), so a size change moves every later field consistently (and must come with a new
 *   EEFS_NAND_LAYOUT_VERSION).  The layout test asserts that the defaults equal the numbers in the format document.
 *
 *   The functions are pure and reentrant (no I/O, no state; the only global is the CRC table in .rodata).  They are
 *   shared by the library and the bootloader library.
 *
 * References:
 *   docs/nand-format.md, docs/nand-implementation.md section 2.4.
 */

#ifndef _eefs_nand_format_
#define _eefs_nand_format_

/*
 * Includes
 */

#include "common_types.h"
#include "eefs_config.h"
#include "eefs_nand_hal.h"

/*
 * Macro Definitions
 */

#define EEFS_NAND_P0_MAGIC              0x46464545UL    /* "EEFF" */
#define EEFS_NAND_REC_MAGIC             0x4A464545UL    /* "EEFJ" */

/* Supported page sizes */
#define EEFS_NAND_MIN_PAGE_SIZE         2048

/* File metadata page (page 0 of a file's first block), docs/nand-format.md section 3.1 */
#define EEFS_NAND_P0_OFF_MAGIC          0
#define EEFS_NAND_P0_OFF_VERSION        4       /* LayoutVersion, uint16 */
#define EEFS_NAND_P0_OFF_RESERVED0      6       /* uint16 */
#define EEFS_NAND_P0_OFF_HDRCRC         8
#define EEFS_NAND_P0_OFF_WRITESEQ       12
#define EEFS_NAND_P0_OFF_PATH           16
#define EEFS_NAND_P0_OFF_FILETYPE       (EEFS_NAND_P0_OFF_PATH + EEFS_NAND_MAX_PATH_SIZE)
#define EEFS_NAND_P0_OFF_SLOT           (EEFS_NAND_P0_OFF_FILETYPE + 1)
#define EEFS_NAND_P0_OFF_RESERVED1      (EEFS_NAND_P0_OFF_SLOT + 1)            /* uint16 */
#define EEFS_NAND_P0_OFF_ATTR           (EEFS_NAND_P0_OFF_RESERVED1 + 2)
#define EEFS_NAND_P0_OFF_VERSIONNUM     (EEFS_NAND_P0_OFF_ATTR + 4)            /* application Version */
#define EEFS_NAND_P0_OFF_RESERVED2      (EEFS_NAND_P0_OFF_VERSIONNUM + 4)      /* uint32 */
#define EEFS_NAND_P0_OFF_FILESIZE       (EEFS_NAND_P0_OFF_RESERVED2 + 4)
#define EEFS_NAND_P0_OFF_FILECRC        (EEFS_NAND_P0_OFF_FILESIZE + 4)
#define EEFS_NAND_P0_OFF_CDATE          (EEFS_NAND_P0_OFF_FILECRC + 4)
#define EEFS_NAND_P0_OFF_MDATE          (EEFS_NAND_P0_OFF_CDATE + 4)
#define EEFS_NAND_P0_OFF_TYPEINFO       (EEFS_NAND_P0_OFF_MDATE + 4)
#define EEFS_NAND_P0_TYPEINFO_SIZE      192
#define EEFS_NAND_P0_OFF_SIGTYPE        (EEFS_NAND_P0_OFF_TYPEINFO + EEFS_NAND_P0_TYPEINFO_SIZE)   /* uint16 */
#define EEFS_NAND_P0_OFF_RESERVED3      (EEFS_NAND_P0_OFF_SIGTYPE + 2)         /* uint16 */
#define EEFS_NAND_P0_OFF_SIGLEN         (EEFS_NAND_P0_OFF_RESERVED3 + 2)
#define EEFS_NAND_P0_OFF_SIG            (EEFS_NAND_P0_OFF_SIGLEN + 4)
#define EEFS_NAND_P0_SIG_SIZE           1024
#define EEFS_NAND_P0_OFF_BLOCKCOUNT     (EEFS_NAND_P0_OFF_SIG + EEFS_NAND_P0_SIG_SIZE)
#define EEFS_NAND_P0_OFF_BLOCKS         (EEFS_NAND_P0_OFF_BLOCKCOUNT + 4)
#define EEFS_NAND_P0_FIXED_SIZE         EEFS_NAND_P0_OFF_BLOCKS
#define EEFS_NAND_P0_BLOCK_ENTRY_SIZE   6       /* Block (uint16) + Crc32 (uint32) */
#define EEFS_NAND_P0_SIG_START          EEFS_NAND_P0_OFF_SIGTYPE               /* [SIG_START, SIG_END) is excluded from HeaderCrc32 */
#define EEFS_NAND_P0_SIG_END            EEFS_NAND_P0_OFF_BLOCKCOUNT

/* Type-specific area, absolute page offsets (section 3.2) */
#define EEFS_NAND_TI_DESTADDR           EEFS_NAND_P0_OFF_TYPEINFO               /* FIT, ELF, DTB: uint64 */
#define EEFS_NAND_TI_ELF_ENTRY          (EEFS_NAND_TI_DESTADDR + 8)             /* uint64 */
#define EEFS_NAND_TI_ELF_FLAGS          (EEFS_NAND_TI_ELF_ENTRY + 8)            /* uint32 */
#define EEFS_NAND_TI_ELF_END            (EEFS_NAND_TI_ELF_FLAGS + 4)
#define EEFS_NAND_TI_DEST_END           (EEFS_NAND_TI_DESTADDR + 8)             /* end of the FIT/DTB area */
#define EEFS_NAND_TI_NAME               EEFS_NAND_P0_OFF_TYPEINFO               /* APP AppName / LIB LibName */
#define EEFS_NAND_TI_SYMBOL             (EEFS_NAND_TI_NAME + EEFS_NAND_CFS_NAME_SIZE)
#define EEFS_NAND_TI_APP_PRIORITY       (EEFS_NAND_TI_SYMBOL + EEFS_NAND_CFS_SYMBOL_SIZE)  /* uint16 */
#define EEFS_NAND_TI_APP_EXCACTION      (EEFS_NAND_TI_APP_PRIORITY + 2)         /* uint8 */
#define EEFS_NAND_TI_APP_RESERVED       (EEFS_NAND_TI_APP_EXCACTION + 1)        /* uint8 */
#define EEFS_NAND_TI_APP_STACKSIZE      (EEFS_NAND_TI_APP_RESERVED + 1)         /* uint32 */
#define EEFS_NAND_TI_LIB_RESERVED       EEFS_NAND_TI_APP_PRIORITY               /* 8 bytes */
#define EEFS_NAND_TI_LIB_RESERVED_SIZE  8
#define EEFS_NAND_TI_LOADADDR           (EEFS_NAND_TI_APP_STACKSIZE + 4)        /* APP and LIB: uint64 */
#define EEFS_NAND_TI_CFS_END            (EEFS_NAND_TI_LOADADDR + 8)

#if (EEFS_NAND_TI_CFS_END > EEFS_NAND_P0_OFF_SIGTYPE)
#error "EEFS_NAND_CFS_NAME_SIZE + EEFS_NAND_CFS_SYMBOL_SIZE do not fit in the 192-byte type-info area"
#endif

/* Directory record page header (section 4.2) */
#define EEFS_NAND_REC_OFF_MAGIC         0
#define EEFS_NAND_REC_OFF_VERSION       4       /* uint16 */
#define EEFS_NAND_REC_OFF_RECTYPE       6       /* uint8 */
#define EEFS_NAND_REC_OFF_MIRROR        7       /* uint8 */
#define EEFS_NAND_REC_OFF_SEQ           8
#define EEFS_NAND_REC_OFF_PAGEINDEX     12      /* uint16 */
#define EEFS_NAND_REC_OFF_PAGECOUNT     14      /* uint16 */
#define EEFS_NAND_REC_OFF_PAYLOADLEN    16
#define EEFS_NAND_REC_OFF_RESERVED      20      /* 8 bytes */
#define EEFS_NAND_REC_OFF_CRC           28
#define EEFS_NAND_REC_HDR_SIZE          32

#define EEFS_NAND_REC_CHECKPOINT        1
#define EEFS_NAND_REC_TXN               2

/* Items (section 4.3) */
#define EEFS_NAND_ITEM_HDR_SIZE         4       /* Type (uint8), Reserved (uint8), Len (uint16) */
#define EEFS_NAND_ITEM_OFF_TYPE         0
#define EEFS_NAND_ITEM_OFF_RESERVED     1
#define EEFS_NAND_ITEM_OFF_LEN          2

#define EEFS_NAND_ITEM_VOLUME           1
#define EEFS_NAND_ITEM_MAPCHUNK         2
#define EEFS_NAND_ITEM_ENTRY            3
#define EEFS_NAND_ITEM_CLEANUP          4

/* VOLUME body (section 4.3.1) */
#define EEFS_NAND_VOL_OFF_PAGESIZE      0
#define EEFS_NAND_VOL_OFF_OOBSIZE       4
#define EEFS_NAND_VOL_OFF_PAGESPERBLOCK 8
#define EEFS_NAND_VOL_OFF_BLOCKCOUNT    12
#define EEFS_NAND_VOL_OFF_DIRBLOCKS     16
#define EEFS_NAND_VOL_OFF_MAXFILES      20
#define EEFS_NAND_VOL_OFF_WRITESEQHIGH  24
#define EEFS_NAND_VOL_OFF_FORMATTIME    28
#define EEFS_NAND_VOL_OFF_PATHSIZE      32      /* uint16 */
#define EEFS_NAND_VOL_OFF_NAMESIZE      34      /* uint16 */
#define EEFS_NAND_VOL_OFF_SYMBOLSIZE    36      /* uint16 */
#define EEFS_NAND_VOL_OFF_RESERVED      38      /* uint16 */
#define EEFS_NAND_VOLUME_ITEM_SIZE      40      /* body length */

/* MAPCHUNK body (section 4.3.2) */
#define EEFS_NAND_MAP_OFF_START         0
#define EEFS_NAND_MAP_OFF_COUNT         4
#define EEFS_NAND_MAP_OFF_BYTES         8
#define EEFS_NAND_MAPCHUNK_HDR_SIZE     8       /* body length = 8 + Count */

/* Block map values */
#define EEFS_NAND_MAP_FREEING           0xFC    /* RAM only; serialized as 0xFF */
#define EEFS_NAND_MAP_DIR               0xFD
#define EEFS_NAND_MAP_BAD               0xFE
#define EEFS_NAND_MAP_FREE              0xFF
#define EEFS_NAND_MAP_OWNER(entry, slot)    ((uint8)((entry) * 2 + (slot)))
#define EEFS_NAND_MAP_IS_OWNER(value)       ((value) < EEFS_NAND_MAP_FREEING)
#define EEFS_NAND_MAP_OWNER_ENTRY(value)    ((value) / 2)
#define EEFS_NAND_MAP_OWNER_SLOT(value)     ((value) % 2)

/* ENTRY body (section 4.3.3): Index (uint16), Reserved (uint16), entry record */
#define EEFS_NAND_ENT_OFF_INDEX         0
#define EEFS_NAND_ENT_OFF_RESERVED      2
#define EEFS_NAND_ENT_OFF_RECORD        4

/* Entry record, offsets relative to the record */
#define EEFS_NAND_ER_OFF_PATH           0
#define EEFS_NAND_ER_OFF_INUSE          (EEFS_NAND_ER_OFF_PATH + EEFS_NAND_MAX_PATH_SIZE)
#define EEFS_NAND_ER_OFF_ISAB           (EEFS_NAND_ER_OFF_INUSE + 1)
#define EEFS_NAND_ER_OFF_ACTIVESLOT     (EEFS_NAND_ER_OFF_ISAB + 1)
#define EEFS_NAND_ER_OFF_FALLBACK       (EEFS_NAND_ER_OFF_ACTIVESLOT + 1)
#define EEFS_NAND_ER_OFF_FAILED         (EEFS_NAND_ER_OFF_FALLBACK + 1)
#define EEFS_NAND_ER_OFF_FILETYPE       (EEFS_NAND_ER_OFF_FAILED + 1)
#define EEFS_NAND_ER_OFF_RESERVED       (EEFS_NAND_ER_OFF_FILETYPE + 1)        /* uint16 */
#define EEFS_NAND_ER_OFF_ATTR           (EEFS_NAND_ER_OFF_RESERVED + 2)
#define EEFS_NAND_ER_OFF_CDATE          (EEFS_NAND_ER_OFF_ATTR + 4)
#define EEFS_NAND_ER_OFF_SLOT0          (EEFS_NAND_ER_OFF_CDATE + 4)
#define EEFS_NAND_ER_OFF_SLOT1          (EEFS_NAND_ER_OFF_SLOT0 + EEFS_NAND_SLOT_REC_SIZE)
#define EEFS_NAND_ENTRY_REC_SIZE        (EEFS_NAND_ER_OFF_SLOT1 + EEFS_NAND_SLOT_REC_SIZE)
#define EEFS_NAND_ENTRY_ITEM_SIZE       (EEFS_NAND_ENT_OFF_RECORD + EEFS_NAND_ENTRY_REC_SIZE)   /* body length */

/* Slot record, offsets relative to the slot record */
#define EEFS_NAND_SR_OFF_INUSE          0
#define EEFS_NAND_SR_OFF_RESERVED       1
#define EEFS_NAND_SR_OFF_FIRSTBLOCK     2       /* uint16 */
#define EEFS_NAND_SR_OFF_ALLOCBLOCKS    4       /* uint16 */
#define EEFS_NAND_SR_OFF_RESERVED2      6       /* uint16 */
#define EEFS_NAND_SR_OFF_FILESIZE       8
#define EEFS_NAND_SR_OFF_VERSION        12
#define EEFS_NAND_SR_OFF_WRITESEQ       16
#define EEFS_NAND_SR_OFF_FILECRC        20
#define EEFS_NAND_SR_OFF_MDATE          24
#define EEFS_NAND_SLOT_REC_SIZE         28

/* CLEANUP body (section 4.3.4) */
#define EEFS_NAND_CLN_OFF_COUNT         0       /* uint16 */
#define EEFS_NAND_CLN_OFF_RESERVED      2       /* uint16 */
#define EEFS_NAND_CLN_OFF_BLOCKS        4       /* uint16 each */
#define EEFS_NAND_CLEANUP_HDR_SIZE      4       /* body length = 4 + 2 * Count */

/* Largest item body (Len is a uint16) */
#define EEFS_NAND_MAX_ITEM_LEN          0xFFFF

/* CRC-32 */
#define EEFS_CRC32_INIT                 0xFFFFFFFFUL

/*
 * Type Definitions
 */

/* Slot record (RAM) */
typedef struct
{
    uint8                               InUse;
    uint8                               Reserved;
    uint16                              FirstBlock;         /* block holding the metadata page */
    uint16                              AllocBlocks;        /* blocks owned by this slot = the metadata page's BlockCount */
    uint16                              Reserved2;
    uint32                              FileSize;           /* copies of the metadata page values at the last commit */
    uint32                              Version;
    uint32                              WriteSeq;
    uint32                              FileCrc32;
    uint32                              ModificationDate;
} EEFS_NandSlot_t;                                          /* 28 bytes */

/* File table entry (RAM) */
typedef struct
{
    char                                Path[EEFS_NAND_MAX_PATH_SIZE];
    uint8                               InUse;
    uint8                               IsAB;
    uint8                               ActiveSlot;
    uint8                               FallbackCompatible;
    uint8                               ActiveFailed;
    uint8                               FileType;
    uint16                              Reserved;
    uint32                              Attributes;
    uint32                              CreationDate;
    EEFS_NandSlot_t                     Slot[2];
} EEFS_NandEntry_t;                                         /* 328 bytes */

/* Decoded file metadata page (without the Blocks[] array, which is accessed with EEFS_NandPage0Block) */
typedef struct
{
    uint32                              WriteSeq;
    char                                Path[EEFS_NAND_MAX_PATH_SIZE];
    uint8                               FileType;
    uint8                               Slot;               /* EEFS_NAND_SLOT_A, _B or _NONE */
    uint16                              Reserved;
    uint32                              Attributes;
    uint32                              Version;
    uint32                              FileSize;
    uint32                              FileCrc32;
    uint32                              CreationDate;
    uint32                              ModificationDate;
    EEFS_NandTypeInfo_t                 Type;
    uint16                              SigType;
    uint32                              SigLength;
    uint32                              BlockCount;
} EEFS_NandPage0_t;

/* Decoded record page header */
typedef struct
{
    uint8                               RecType;            /* EEFS_NAND_REC_CHECKPOINT or EEFS_NAND_REC_TXN */
    uint8                               Mirror;
    uint32                              Seq;
    uint16                              PageIndex;
    uint16                              PageCount;
    uint32                              PayloadLen;
} EEFS_NandRecHdr_t;

/* Decoded VOLUME item */
typedef struct
{
    uint32                              PageSize, OobSize, PagesPerBlock, BlockCount, DirRegionBlocks, MaxFiles;
    uint32                              WriteSeqHigh, FormatTime;
    uint16                              PathSize, CfsNameSize, CfsSymbolSize;
} EEFS_NandVolumeItem_t;

/*
 * Exported Functions
 */

/* CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).  Feed bytes with Update starting from EEFS_CRC32_INIT, then
 * Final.  EEFS_Crc32("123456789") = 0xCBF43926; the CRC of zero bytes is 0. */
uint32  EEFS_Crc32Update(uint32 State, const void *Data, uint32 Length);
uint32  EEFS_Crc32Final(uint32 State);
uint32  EEFS_Crc32(const void *Data, uint32 Length);

/* Little-endian field access */
void    EEFS_NandPut16(uint8 *P, uint16 V);
void    EEFS_NandPut32(uint8 *P, uint32 V);
void    EEFS_NandPut64(uint8 *P, uint64 V);
uint16  EEFS_NandGet16(const uint8 *P);
uint32  EEFS_NandGet32(const uint8 *P);
uint64  EEFS_NandGet64(const uint8 *P);

/* TRUE if every byte is 0xFF */
boolean EEFS_NandIsErased(const uint8 *Data, uint32 Length);

/* Page 0 */

/* Largest Blocks[] array a metadata page can hold: (PageSize - 1532) / 6 */
uint32  EEFS_NandMaxBlocksPerFile(uint32 PageSize);

/* Encodes a whole metadata page into Page (PageSize bytes): fixed fields, type info for P->FileType, unsigned
 * signature block (all 0xFF), Blocks[0..P->BlockCount) from Blocks/BlockCrcs, 0xFF tail and HeaderCrc32.
 * Returns EEFS_SUCCESS, or EEFS_INVALID_ARGUMENT when BlockCount is 0 or too big for the page, SigType or SigLength is
 * not 0, FileType is unknown, or Path does not start with '/' or is not NUL-terminated within EEFS_NAND_MAX_PATH_SIZE. */
int32   EEFS_NandEncodePage0(uint8 *Page, uint32 PageSize, const EEFS_NandPage0_t *P,
                             const uint16 *Blocks, const uint32 *BlockCrcs);

/* Decodes and validates a metadata page read from block ExpectBlock: docs/nand-format.md 3.4 checks 2-7 except the
 * [DirRegionBlocks, BlockCount) bounds of Blocks[], which the caller checks with EEFS_NandPage0Block (it knows the
 * geometry).  Returns EEFS_SUCCESS or EEFS_CORRUPT_FILE.  P is filled on success (and is undefined on failure). */
int32   EEFS_NandDecodePage0(const uint8 *Page, uint32 PageSize, uint32 PagesPerBlock, uint32 ExpectBlock,
                             EEFS_NandPage0_t *P);

/* Blocks[Index] of an encoded metadata page */
void    EEFS_NandPage0Block(const uint8 *Page, uint32 Index, uint16 *Block, uint32 *Crc);

/* Number of file bytes held by Blocks[K] of a file of FileSize bytes (clipped, may be 0); *Offset receives the file
 * offset of the block's first byte (docs/nand-format.md 3.3). */
uint32  EEFS_NandBlockDataRange(uint32 PageSize, uint32 PagesPerBlock, uint32 FileSize, uint32 K, uint32 *Offset);

/* Record pages */

/* Writes the header fields of H (PayloadLen and Crc32 are set by Seal) and fills [32, PageSize) with 0xFF */
void    EEFS_NandBeginRecordPage(uint8 *Page, uint32 PageSize, const EEFS_NandRecHdr_t *H);

/* Sets PayloadLen and the page Crc32 (over [0,28) + [32,PageSize)) */
void    EEFS_NandSealRecordPage(uint8 *Page, uint32 PageSize, uint32 PayloadLen);

/* Validates a record page (docs/nand-format.md 4.2 page validity, plus the item framing of 4.3: every item fits the
 * payload, and VOLUME, MAPCHUNK, ENTRY and CLEANUP items have a Len consistent with their type) and decodes its
 * header.  Returns EEFS_SUCCESS or EEFS_CORRUPT_FILE. */
int32   EEFS_NandDecodeRecordPage(const uint8 *Page, uint32 PageSize, EEFS_NandRecHdr_t *H);

/* Items: encoders write the whole item (header + body, reserved = 0, string padding = 0) and return the bytes
 * written, or 0 if the item does not fit in Avail (or the arguments describe an invalid item). */
uint32  EEFS_NandEncodeVolumeItem(uint8 *Dst, uint32 Avail, const EEFS_NandVolumeItem_t *V);
uint32  EEFS_NandEncodeMapChunk(uint8 *Dst, uint32 Avail, uint32 StartBlock, uint32 Count, const uint8 *Map);
        /* Map points at the byte of StartBlock; FREEING (0xFC) is serialized as FREE (0xFF).  Count >= 1, clipped by
         * the caller to what fits (EEFS_NandMapChunkFit). */
uint32  EEFS_NandEncodeEntryItem(uint8 *Dst, uint32 Avail, uint16 Index, const EEFS_NandEntry_t *E);
uint32  EEFS_NandEncodeCleanupItem(uint8 *Dst, uint32 Avail, const uint16 *Blocks, uint16 Count);

/* Largest MAPCHUNK Count that fits in Avail bytes (0 if none) */
uint32  EEFS_NandMapChunkFit(uint32 Avail);

/* Item iterator over a page payload (Payload = page + 32).  Returns 1 and the item at *Cursor (advancing it), 0 at
 * the end of the payload, or EEFS_CORRUPT_FILE when an item header or body runs past PayloadLen. */
int32   EEFS_NandNextItem(const uint8 *Payload, uint32 PayloadLen, uint32 *Cursor,
                          uint8 *Type, const uint8 **Body, uint16 *Len);

/* Item decoders.  EEFS_SUCCESS or EEFS_CORRUPT_FILE (wrong Len, or an unterminated Path).  MAPCHUNK and CLEANUP are
 * decoded inline with EEFS_NandGet16/32 by the callers. */
int32   EEFS_NandDecodeVolumeItem(const uint8 *Body, uint16 Len, EEFS_NandVolumeItem_t *V);
int32   EEFS_NandDecodeEntryItem(const uint8 *Body, uint16 Len, uint16 *Index, EEFS_NandEntry_t *E);

#endif

/************************/
/*  End of File Comment */
/************************/
