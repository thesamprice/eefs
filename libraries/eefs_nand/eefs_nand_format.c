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
 * Filename: eefs_nand_format.c
 *
 * Purpose: Pure encode/decode of the EEFS NAND on-media structures (docs/nand-format.md): little-endian field access,
 *   file metadata pages, directory record pages and their items.  No I/O and no state, so the same code serves the
 *   library and the bootloader library.
 *
 */

/*
 * Includes
 */

#include "common_types.h"
#include "eefs_fileapi.h"
#include "eefs_nand_format.h"
#include <string.h>

/*
 * Local Function Prototypes
 */

static uint32   EEFS_NandPage0Crc(const uint8 *Page, uint32 PageSize);
static uint32   EEFS_NandRecordCrc(const uint8 *Page, uint32 PageSize);
static void     EEFS_NandPutString(uint8 *Dst, const char *Src, uint32 Size);
static void     EEFS_NandGetString(char *Dst, const uint8 *Src, uint32 Size);
static boolean  EEFS_NandIsTerminated(const uint8 *Src, uint32 Size);
static void     EEFS_NandEncodeSlot(uint8 *Dst, const EEFS_NandSlot_t *S);
static void     EEFS_NandDecodeSlot(const uint8 *Src, EEFS_NandSlot_t *S);

/*
 * Function Definitions
 */

/* Little-endian stores */
void EEFS_NandPut16(uint8 *P, uint16 V)
{
    P[0] = (uint8)(V & 0xFF);
    P[1] = (uint8)((V >> 8) & 0xFF);

} /* End of EEFS_NandPut16() */

void EEFS_NandPut32(uint8 *P, uint32 V)
{
    P[0] = (uint8)(V & 0xFF);
    P[1] = (uint8)((V >> 8) & 0xFF);
    P[2] = (uint8)((V >> 16) & 0xFF);
    P[3] = (uint8)((V >> 24) & 0xFF);

} /* End of EEFS_NandPut32() */

void EEFS_NandPut64(uint8 *P, uint64 V)
{
    EEFS_NandPut32(P, (uint32)(V & 0xFFFFFFFFUL));
    EEFS_NandPut32(P + 4, (uint32)(V >> 32));

} /* End of EEFS_NandPut64() */

/* Little-endian loads */
uint16 EEFS_NandGet16(const uint8 *P)
{
    return((uint16)((uint16)P[0] | ((uint16)P[1] << 8)));

} /* End of EEFS_NandGet16() */

uint32 EEFS_NandGet32(const uint8 *P)
{
    return((uint32)P[0] | ((uint32)P[1] << 8) | ((uint32)P[2] << 16) | ((uint32)P[3] << 24));

} /* End of EEFS_NandGet32() */

uint64 EEFS_NandGet64(const uint8 *P)
{
    return((uint64)EEFS_NandGet32(P) | ((uint64)EEFS_NandGet32(P + 4) << 32));

} /* End of EEFS_NandGet64() */

/* Returns TRUE if every byte is 0xFF */
boolean EEFS_NandIsErased(const uint8 *Data, uint32 Length)
{
    uint32  i;

    for (i = 0; i < Length; i++) {
        if (Data[i] != 0xFF) {
            return(FALSE);
        }
    }
    return(TRUE);

} /* End of EEFS_NandIsErased() */

/* Largest Blocks[] array a metadata page can hold */
uint32 EEFS_NandMaxBlocksPerFile(uint32 PageSize)
{
    if (PageSize <= EEFS_NAND_P0_FIXED_SIZE) {
        return(0);
    }
    return((PageSize - EEFS_NAND_P0_FIXED_SIZE) / EEFS_NAND_P0_BLOCK_ENTRY_SIZE);

} /* End of EEFS_NandMaxBlocksPerFile() */

/* Encodes a whole metadata page */
int32 EEFS_NandEncodePage0(uint8 *Page, uint32 PageSize, const EEFS_NandPage0_t *P,
                           const uint16 *Blocks, const uint32 *BlockCrcs)
{
    uint32  i;
    uint8  *Entry;

    if ((Page == NULL) || (P == NULL) || (Blocks == NULL) || (BlockCrcs == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    if ((P->BlockCount < 1) || (P->BlockCount > EEFS_NandMaxBlocksPerFile(PageSize)) ||
        (P->SigType != 0) || (P->SigLength != 0) || (P->FileType > EEFS_FILE_TYPE_MAX) ||
        (P->Path[0] != '/') || (EEFS_NandIsTerminated((const uint8 *)P->Path, EEFS_NAND_MAX_PATH_SIZE) == FALSE)) {
        return(EEFS_INVALID_ARGUMENT);
    }

    /* Unused areas are 0xFF; the fixed fields before the type info are written over zeros (reserved = 0) */
    memset(Page, 0xFF, PageSize);
    memset(Page, 0, EEFS_NAND_P0_OFF_TYPEINFO);

    EEFS_NandPut32(&Page[EEFS_NAND_P0_OFF_MAGIC], EEFS_NAND_P0_MAGIC);
    EEFS_NandPut16(&Page[EEFS_NAND_P0_OFF_VERSION], EEFS_NAND_LAYOUT_VERSION);
    EEFS_NandPut32(&Page[EEFS_NAND_P0_OFF_WRITESEQ], P->WriteSeq);
    EEFS_NandPutString(&Page[EEFS_NAND_P0_OFF_PATH], P->Path, EEFS_NAND_MAX_PATH_SIZE);
    Page[EEFS_NAND_P0_OFF_FILETYPE] = P->FileType;
    Page[EEFS_NAND_P0_OFF_SLOT] = P->Slot;
    EEFS_NandPut32(&Page[EEFS_NAND_P0_OFF_ATTR], P->Attributes);
    EEFS_NandPut32(&Page[EEFS_NAND_P0_OFF_VERSIONNUM], P->Version);
    EEFS_NandPut32(&Page[EEFS_NAND_P0_OFF_FILESIZE], P->FileSize);
    EEFS_NandPut32(&Page[EEFS_NAND_P0_OFF_FILECRC], P->FileCrc32);
    EEFS_NandPut32(&Page[EEFS_NAND_P0_OFF_CDATE], P->CreationDate);
    EEFS_NandPut32(&Page[EEFS_NAND_P0_OFF_MDATE], P->ModificationDate);

    /* Type-specific area: bytes a type does not define stay 0xFF */
    switch (P->FileType) {
        case EEFS_FILE_TYPE_BOOT_FIT:
            EEFS_NandPut64(&Page[EEFS_NAND_TI_DESTADDR], P->Type.Fit.DestAddress);
            break;
        case EEFS_FILE_TYPE_BOOT_ELF:
            EEFS_NandPut64(&Page[EEFS_NAND_TI_DESTADDR], P->Type.Elf.DestAddress);
            EEFS_NandPut64(&Page[EEFS_NAND_TI_ELF_ENTRY], P->Type.Elf.EntryPoint);
            EEFS_NandPut32(&Page[EEFS_NAND_TI_ELF_FLAGS], P->Type.Elf.Flags);
            break;
        case EEFS_FILE_TYPE_BOOT_DTB:
            EEFS_NandPut64(&Page[EEFS_NAND_TI_DESTADDR], P->Type.Dtb.DestAddress);
            break;
        case EEFS_FILE_TYPE_CFS_APP:
            memset(&Page[EEFS_NAND_TI_NAME], 0, EEFS_NAND_TI_CFS_END - EEFS_NAND_TI_NAME);
            EEFS_NandPutString(&Page[EEFS_NAND_TI_NAME], P->Type.App.AppName, EEFS_NAND_CFS_NAME_SIZE);
            EEFS_NandPutString(&Page[EEFS_NAND_TI_SYMBOL], P->Type.App.EntrySymbol, EEFS_NAND_CFS_SYMBOL_SIZE);
            EEFS_NandPut16(&Page[EEFS_NAND_TI_APP_PRIORITY], P->Type.App.Priority);
            Page[EEFS_NAND_TI_APP_EXCACTION] = P->Type.App.ExceptionAction;
            EEFS_NandPut32(&Page[EEFS_NAND_TI_APP_STACKSIZE], P->Type.App.StackSize);
            EEFS_NandPut64(&Page[EEFS_NAND_TI_LOADADDR], P->Type.App.LoadAddress);
            break;
        case EEFS_FILE_TYPE_CFS_LIB:
            memset(&Page[EEFS_NAND_TI_NAME], 0, EEFS_NAND_TI_CFS_END - EEFS_NAND_TI_NAME);
            EEFS_NandPutString(&Page[EEFS_NAND_TI_NAME], P->Type.Lib.LibName, EEFS_NAND_CFS_NAME_SIZE);
            EEFS_NandPutString(&Page[EEFS_NAND_TI_SYMBOL], P->Type.Lib.EntrySymbol, EEFS_NAND_CFS_SYMBOL_SIZE);
            EEFS_NandPut64(&Page[EEFS_NAND_TI_LOADADDR], P->Type.Lib.LoadAddress);
            break;
        default:    /* EEFS_FILE_TYPE_OTHER: nothing */
            break;
    }

    /* Unsigned: SigType 0, SigLength 0, Sig all 0xFF */
    EEFS_NandPut16(&Page[EEFS_NAND_P0_OFF_SIGTYPE], 0);
    EEFS_NandPut16(&Page[EEFS_NAND_P0_OFF_RESERVED3], 0);
    EEFS_NandPut32(&Page[EEFS_NAND_P0_OFF_SIGLEN], 0);

    EEFS_NandPut32(&Page[EEFS_NAND_P0_OFF_BLOCKCOUNT], P->BlockCount);
    for (i = 0; i < P->BlockCount; i++) {
        Entry = &Page[EEFS_NAND_P0_OFF_BLOCKS + (i * EEFS_NAND_P0_BLOCK_ENTRY_SIZE)];
        EEFS_NandPut16(Entry, Blocks[i]);
        EEFS_NandPut32(Entry + 2, BlockCrcs[i]);
    }

    EEFS_NandPut32(&Page[EEFS_NAND_P0_OFF_HDRCRC], EEFS_NandPage0Crc(Page, PageSize));
    return(EEFS_SUCCESS);

} /* End of EEFS_NandEncodePage0() */

/* Decodes and validates a metadata page */
int32 EEFS_NandDecodePage0(const uint8 *Page, uint32 PageSize, uint32 PagesPerBlock, uint32 ExpectBlock,
                           EEFS_NandPage0_t *P)
{
    uint32  BlockCount;
    uint32  FileSize;
    uint16  FirstBlock;
    uint32  FirstCrc;

    if ((Page == NULL) || (P == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    if (EEFS_NandMaxBlocksPerFile(PageSize) < 1) {
        return(EEFS_CORRUPT_FILE);
    }

    if ((EEFS_NandGet32(&Page[EEFS_NAND_P0_OFF_MAGIC]) != EEFS_NAND_P0_MAGIC) ||
        (EEFS_NandGet16(&Page[EEFS_NAND_P0_OFF_VERSION]) != EEFS_NAND_LAYOUT_VERSION)) {
        return(EEFS_CORRUPT_FILE);
    }
    if (EEFS_NandGet32(&Page[EEFS_NAND_P0_OFF_HDRCRC]) != EEFS_NandPage0Crc(Page, PageSize)) {
        return(EEFS_CORRUPT_FILE);
    }
    if (EEFS_NandGet16(&Page[EEFS_NAND_P0_OFF_SIGTYPE]) != 0) {
        return(EEFS_CORRUPT_FILE);
    }
    if ((Page[EEFS_NAND_P0_OFF_PATH] != '/') ||
        (EEFS_NandIsTerminated(&Page[EEFS_NAND_P0_OFF_PATH], EEFS_NAND_MAX_PATH_SIZE) == FALSE)) {
        return(EEFS_CORRUPT_FILE);
    }

    BlockCount = EEFS_NandGet32(&Page[EEFS_NAND_P0_OFF_BLOCKCOUNT]);
    FileSize = EEFS_NandGet32(&Page[EEFS_NAND_P0_OFF_FILESIZE]);
    if ((BlockCount < 1) || (BlockCount > EEFS_NandMaxBlocksPerFile(PageSize)) ||
        (((uint64)BlockCount * PageSize * PagesPerBlock) < ((uint64)PageSize + FileSize))) {
        return(EEFS_CORRUPT_FILE);
    }

    EEFS_NandPage0Block(Page, 0, &FirstBlock, &FirstCrc);
    if (FirstBlock != ExpectBlock) {
        return(EEFS_CORRUPT_FILE);
    }

    memset(P, 0, sizeof(EEFS_NandPage0_t));
    P->WriteSeq = EEFS_NandGet32(&Page[EEFS_NAND_P0_OFF_WRITESEQ]);
    EEFS_NandGetString(P->Path, &Page[EEFS_NAND_P0_OFF_PATH], EEFS_NAND_MAX_PATH_SIZE);
    P->FileType = Page[EEFS_NAND_P0_OFF_FILETYPE];
    P->Slot = Page[EEFS_NAND_P0_OFF_SLOT];
    P->Attributes = EEFS_NandGet32(&Page[EEFS_NAND_P0_OFF_ATTR]);
    P->Version = EEFS_NandGet32(&Page[EEFS_NAND_P0_OFF_VERSIONNUM]);
    P->FileSize = FileSize;
    P->FileCrc32 = EEFS_NandGet32(&Page[EEFS_NAND_P0_OFF_FILECRC]);
    P->CreationDate = EEFS_NandGet32(&Page[EEFS_NAND_P0_OFF_CDATE]);
    P->ModificationDate = EEFS_NandGet32(&Page[EEFS_NAND_P0_OFF_MDATE]);
    P->SigType = 0;
    P->SigLength = EEFS_NandGet32(&Page[EEFS_NAND_P0_OFF_SIGLEN]);
    P->BlockCount = BlockCount;

    switch (P->FileType) {
        case EEFS_FILE_TYPE_BOOT_FIT:
            P->Type.Fit.DestAddress = EEFS_NandGet64(&Page[EEFS_NAND_TI_DESTADDR]);
            break;
        case EEFS_FILE_TYPE_BOOT_ELF:
            P->Type.Elf.DestAddress = EEFS_NandGet64(&Page[EEFS_NAND_TI_DESTADDR]);
            P->Type.Elf.EntryPoint = EEFS_NandGet64(&Page[EEFS_NAND_TI_ELF_ENTRY]);
            P->Type.Elf.Flags = EEFS_NandGet32(&Page[EEFS_NAND_TI_ELF_FLAGS]);
            break;
        case EEFS_FILE_TYPE_BOOT_DTB:
            P->Type.Dtb.DestAddress = EEFS_NandGet64(&Page[EEFS_NAND_TI_DESTADDR]);
            break;
        case EEFS_FILE_TYPE_CFS_APP:
            EEFS_NandGetString(P->Type.App.AppName, &Page[EEFS_NAND_TI_NAME], EEFS_NAND_CFS_NAME_SIZE);
            EEFS_NandGetString(P->Type.App.EntrySymbol, &Page[EEFS_NAND_TI_SYMBOL], EEFS_NAND_CFS_SYMBOL_SIZE);
            P->Type.App.Priority = EEFS_NandGet16(&Page[EEFS_NAND_TI_APP_PRIORITY]);
            P->Type.App.ExceptionAction = Page[EEFS_NAND_TI_APP_EXCACTION];
            P->Type.App.StackSize = EEFS_NandGet32(&Page[EEFS_NAND_TI_APP_STACKSIZE]);
            P->Type.App.LoadAddress = EEFS_NandGet64(&Page[EEFS_NAND_TI_LOADADDR]);
            break;
        case EEFS_FILE_TYPE_CFS_LIB:
            EEFS_NandGetString(P->Type.Lib.LibName, &Page[EEFS_NAND_TI_NAME], EEFS_NAND_CFS_NAME_SIZE);
            EEFS_NandGetString(P->Type.Lib.EntrySymbol, &Page[EEFS_NAND_TI_SYMBOL], EEFS_NAND_CFS_SYMBOL_SIZE);
            P->Type.Lib.LoadAddress = EEFS_NandGet64(&Page[EEFS_NAND_TI_LOADADDR]);
            break;
        default:    /* OTHER or a type this reader does not know: no type info */
            break;
    }

    return(EEFS_SUCCESS);

} /* End of EEFS_NandDecodePage0() */

/* Returns Blocks[Index] of an encoded metadata page */
void EEFS_NandPage0Block(const uint8 *Page, uint32 Index, uint16 *Block, uint32 *Crc)
{
    const uint8    *Entry = &Page[EEFS_NAND_P0_OFF_BLOCKS + (Index * EEFS_NAND_P0_BLOCK_ENTRY_SIZE)];

    if (Block != NULL) {
        *Block = EEFS_NandGet16(Entry);
    }
    if (Crc != NULL) {
        *Crc = EEFS_NandGet32(Entry + 2);
    }

} /* End of EEFS_NandPage0Block() */

/* Number of file bytes held by Blocks[K], and their file offset */
uint32 EEFS_NandBlockDataRange(uint32 PageSize, uint32 PagesPerBlock, uint32 FileSize, uint32 K, uint32 *Offset)
{
    uint64  BlockBytes = (uint64)PageSize * PagesPerBlock;
    uint64  Start;
    uint64  Length;
    uint32  Count;

    if (K == 0) {
        Start = 0;
        Length = BlockBytes - PageSize;
    }
    else {
        Start = (BlockBytes - PageSize) + ((uint64)(K - 1) * BlockBytes);
        Length = BlockBytes;
    }

    if (Start >= FileSize) {
        Count = 0;
    }
    else if ((FileSize - Start) < Length) {
        Count = (uint32)(FileSize - Start);
    }
    else {
        Count = (uint32)Length;
    }

    if (Offset != NULL) {
        *Offset = (Start > 0xFFFFFFFFUL) ? 0xFFFFFFFFUL : (uint32)Start;
    }
    return(Count);

} /* End of EEFS_NandBlockDataRange() */

/* Starts a record page: header fields, PayloadLen 0, empty payload area (0xFF) */
void EEFS_NandBeginRecordPage(uint8 *Page, uint32 PageSize, const EEFS_NandRecHdr_t *H)
{
    memset(Page, 0, EEFS_NAND_REC_HDR_SIZE);
    memset(&Page[EEFS_NAND_REC_HDR_SIZE], 0xFF, PageSize - EEFS_NAND_REC_HDR_SIZE);

    EEFS_NandPut32(&Page[EEFS_NAND_REC_OFF_MAGIC], EEFS_NAND_REC_MAGIC);
    EEFS_NandPut16(&Page[EEFS_NAND_REC_OFF_VERSION], EEFS_NAND_LAYOUT_VERSION);
    Page[EEFS_NAND_REC_OFF_RECTYPE] = H->RecType;
    Page[EEFS_NAND_REC_OFF_MIRROR] = H->Mirror;
    EEFS_NandPut32(&Page[EEFS_NAND_REC_OFF_SEQ], H->Seq);
    EEFS_NandPut16(&Page[EEFS_NAND_REC_OFF_PAGEINDEX], H->PageIndex);
    EEFS_NandPut16(&Page[EEFS_NAND_REC_OFF_PAGECOUNT], H->PageCount);

} /* End of EEFS_NandBeginRecordPage() */

/* Finishes a record page: PayloadLen and Crc32 */
void EEFS_NandSealRecordPage(uint8 *Page, uint32 PageSize, uint32 PayloadLen)
{
    EEFS_NandPut32(&Page[EEFS_NAND_REC_OFF_PAYLOADLEN], PayloadLen);
    EEFS_NandPut32(&Page[EEFS_NAND_REC_OFF_CRC], EEFS_NandRecordCrc(Page, PageSize));

} /* End of EEFS_NandSealRecordPage() */

/* Validates a record page and decodes its header */
int32 EEFS_NandDecodeRecordPage(const uint8 *Page, uint32 PageSize, EEFS_NandRecHdr_t *H)
{
    EEFS_NandRecHdr_t   Hdr;
    uint32              Cursor;
    uint8               Type;
    const uint8        *Body;
    uint16              Len;
    uint32              Count;
    int32               Status;

    if ((Page == NULL) || (H == NULL)) {
        return(EEFS_INVALID_ARGUMENT);
    }
    if (PageSize <= EEFS_NAND_REC_HDR_SIZE) {
        return(EEFS_CORRUPT_FILE);
    }

    if ((EEFS_NandGet32(&Page[EEFS_NAND_REC_OFF_MAGIC]) != EEFS_NAND_REC_MAGIC) ||
        (EEFS_NandGet16(&Page[EEFS_NAND_REC_OFF_VERSION]) != EEFS_NAND_LAYOUT_VERSION)) {
        return(EEFS_CORRUPT_FILE);
    }

    Hdr.RecType = Page[EEFS_NAND_REC_OFF_RECTYPE];
    Hdr.Mirror = Page[EEFS_NAND_REC_OFF_MIRROR];
    Hdr.Seq = EEFS_NandGet32(&Page[EEFS_NAND_REC_OFF_SEQ]);
    Hdr.PageIndex = EEFS_NandGet16(&Page[EEFS_NAND_REC_OFF_PAGEINDEX]);
    Hdr.PageCount = EEFS_NandGet16(&Page[EEFS_NAND_REC_OFF_PAGECOUNT]);
    Hdr.PayloadLen = EEFS_NandGet32(&Page[EEFS_NAND_REC_OFF_PAYLOADLEN]);

    if (((Hdr.RecType != EEFS_NAND_REC_CHECKPOINT) && (Hdr.RecType != EEFS_NAND_REC_TXN)) ||
        (Hdr.PageIndex >= Hdr.PageCount) ||
        (Hdr.PayloadLen > (PageSize - EEFS_NAND_REC_HDR_SIZE))) {
        return(EEFS_CORRUPT_FILE);
    }
    if (EEFS_NandGet32(&Page[EEFS_NAND_REC_OFF_CRC]) != EEFS_NandRecordCrc(Page, PageSize)) {
        return(EEFS_CORRUPT_FILE);
    }

    /* Item framing: an item that overruns the payload, or a known item with an inconsistent length, makes the page
     * (and so the record) invalid.  Checking it here lets readers validate a whole record before applying any of it. */
    Cursor = 0;
    while ((Status = EEFS_NandNextItem(&Page[EEFS_NAND_REC_HDR_SIZE], Hdr.PayloadLen, &Cursor, &Type, &Body, &Len)) == 1) {
        switch (Type) {
            case EEFS_NAND_ITEM_VOLUME:
                if (Len != EEFS_NAND_VOLUME_ITEM_SIZE) return(EEFS_CORRUPT_FILE);
                break;
            case EEFS_NAND_ITEM_ENTRY:
                if (Len != EEFS_NAND_ENTRY_ITEM_SIZE) return(EEFS_CORRUPT_FILE);
                break;
            case EEFS_NAND_ITEM_MAPCHUNK:
                if (Len <= EEFS_NAND_MAPCHUNK_HDR_SIZE) return(EEFS_CORRUPT_FILE);
                Count = EEFS_NandGet32(&Body[EEFS_NAND_MAP_OFF_COUNT]);
                if (Count != (uint32)(Len - EEFS_NAND_MAPCHUNK_HDR_SIZE)) return(EEFS_CORRUPT_FILE);
                break;
            case EEFS_NAND_ITEM_CLEANUP:
                if (Len < (EEFS_NAND_CLEANUP_HDR_SIZE + 2)) return(EEFS_CORRUPT_FILE);
                Count = EEFS_NandGet16(&Body[EEFS_NAND_CLN_OFF_COUNT]);
                if ((Count < 1) || ((EEFS_NAND_CLEANUP_HDR_SIZE + (2 * Count)) != Len)) return(EEFS_CORRUPT_FILE);
                break;
            default:    /* unknown items are skipped by length */
                break;
        }
    }
    if (Status != 0) {
        return(EEFS_CORRUPT_FILE);
    }

    *H = Hdr;
    return(EEFS_SUCCESS);

} /* End of EEFS_NandDecodeRecordPage() */

/* VOLUME item */
uint32 EEFS_NandEncodeVolumeItem(uint8 *Dst, uint32 Avail, const EEFS_NandVolumeItem_t *V)
{
    uint8  *Body = &Dst[EEFS_NAND_ITEM_HDR_SIZE];

    if (Avail < (EEFS_NAND_ITEM_HDR_SIZE + EEFS_NAND_VOLUME_ITEM_SIZE)) {
        return(0);
    }

    memset(Dst, 0, EEFS_NAND_ITEM_HDR_SIZE + EEFS_NAND_VOLUME_ITEM_SIZE);
    Dst[EEFS_NAND_ITEM_OFF_TYPE] = EEFS_NAND_ITEM_VOLUME;
    EEFS_NandPut16(&Dst[EEFS_NAND_ITEM_OFF_LEN], EEFS_NAND_VOLUME_ITEM_SIZE);
    EEFS_NandPut32(&Body[EEFS_NAND_VOL_OFF_PAGESIZE], V->PageSize);
    EEFS_NandPut32(&Body[EEFS_NAND_VOL_OFF_OOBSIZE], V->OobSize);
    EEFS_NandPut32(&Body[EEFS_NAND_VOL_OFF_PAGESPERBLOCK], V->PagesPerBlock);
    EEFS_NandPut32(&Body[EEFS_NAND_VOL_OFF_BLOCKCOUNT], V->BlockCount);
    EEFS_NandPut32(&Body[EEFS_NAND_VOL_OFF_DIRBLOCKS], V->DirRegionBlocks);
    EEFS_NandPut32(&Body[EEFS_NAND_VOL_OFF_MAXFILES], V->MaxFiles);
    EEFS_NandPut32(&Body[EEFS_NAND_VOL_OFF_WRITESEQHIGH], V->WriteSeqHigh);
    EEFS_NandPut32(&Body[EEFS_NAND_VOL_OFF_FORMATTIME], V->FormatTime);
    EEFS_NandPut16(&Body[EEFS_NAND_VOL_OFF_PATHSIZE], V->PathSize);
    EEFS_NandPut16(&Body[EEFS_NAND_VOL_OFF_NAMESIZE], V->CfsNameSize);
    EEFS_NandPut16(&Body[EEFS_NAND_VOL_OFF_SYMBOLSIZE], V->CfsSymbolSize);

    return(EEFS_NAND_ITEM_HDR_SIZE + EEFS_NAND_VOLUME_ITEM_SIZE);

} /* End of EEFS_NandEncodeVolumeItem() */

/* Largest MAPCHUNK Count that fits in Avail bytes */
uint32 EEFS_NandMapChunkFit(uint32 Avail)
{
    uint32  Count;

    if (Avail <= (EEFS_NAND_ITEM_HDR_SIZE + EEFS_NAND_MAPCHUNK_HDR_SIZE)) {
        return(0);
    }
    Count = Avail - (EEFS_NAND_ITEM_HDR_SIZE + EEFS_NAND_MAPCHUNK_HDR_SIZE);
    if (Count > (EEFS_NAND_MAX_ITEM_LEN - EEFS_NAND_MAPCHUNK_HDR_SIZE)) {
        Count = EEFS_NAND_MAX_ITEM_LEN - EEFS_NAND_MAPCHUNK_HDR_SIZE;
    }
    return(Count);

} /* End of EEFS_NandMapChunkFit() */

/* MAPCHUNK item */
uint32 EEFS_NandEncodeMapChunk(uint8 *Dst, uint32 Avail, uint32 StartBlock, uint32 Count, const uint8 *Map)
{
    uint8  *Body = &Dst[EEFS_NAND_ITEM_HDR_SIZE];
    uint32  i;

    if ((Count < 1) || (Count > EEFS_NandMapChunkFit(Avail))) {
        return(0);
    }

    Dst[EEFS_NAND_ITEM_OFF_TYPE] = EEFS_NAND_ITEM_MAPCHUNK;
    Dst[EEFS_NAND_ITEM_OFF_RESERVED] = 0;
    EEFS_NandPut16(&Dst[EEFS_NAND_ITEM_OFF_LEN], (uint16)(EEFS_NAND_MAPCHUNK_HDR_SIZE + Count));
    EEFS_NandPut32(&Body[EEFS_NAND_MAP_OFF_START], StartBlock);
    EEFS_NandPut32(&Body[EEFS_NAND_MAP_OFF_COUNT], Count);
    for (i = 0; i < Count; i++) {
        Body[EEFS_NAND_MAP_OFF_BYTES + i] = (Map[i] == EEFS_NAND_MAP_FREEING) ? EEFS_NAND_MAP_FREE : Map[i];
    }

    return(EEFS_NAND_ITEM_HDR_SIZE + EEFS_NAND_MAPCHUNK_HDR_SIZE + Count);

} /* End of EEFS_NandEncodeMapChunk() */

/* ENTRY item */
uint32 EEFS_NandEncodeEntryItem(uint8 *Dst, uint32 Avail, uint16 Index, const EEFS_NandEntry_t *E)
{
    uint8  *Body = &Dst[EEFS_NAND_ITEM_HDR_SIZE];
    uint8  *Rec = &Body[EEFS_NAND_ENT_OFF_RECORD];

    if (Avail < (EEFS_NAND_ITEM_HDR_SIZE + EEFS_NAND_ENTRY_ITEM_SIZE)) {
        return(0);
    }

    memset(Dst, 0, EEFS_NAND_ITEM_HDR_SIZE + EEFS_NAND_ENTRY_ITEM_SIZE);
    Dst[EEFS_NAND_ITEM_OFF_TYPE] = EEFS_NAND_ITEM_ENTRY;
    EEFS_NandPut16(&Dst[EEFS_NAND_ITEM_OFF_LEN], EEFS_NAND_ENTRY_ITEM_SIZE);
    EEFS_NandPut16(&Body[EEFS_NAND_ENT_OFF_INDEX], Index);

    if (E->InUse != 0) {
        EEFS_NandPutString(&Rec[EEFS_NAND_ER_OFF_PATH], E->Path, EEFS_NAND_MAX_PATH_SIZE);
    }
    Rec[EEFS_NAND_ER_OFF_INUSE] = E->InUse;
    Rec[EEFS_NAND_ER_OFF_ISAB] = E->IsAB;
    Rec[EEFS_NAND_ER_OFF_ACTIVESLOT] = E->ActiveSlot;
    Rec[EEFS_NAND_ER_OFF_FALLBACK] = E->FallbackCompatible;
    Rec[EEFS_NAND_ER_OFF_FAILED] = E->ActiveFailed;
    Rec[EEFS_NAND_ER_OFF_FILETYPE] = E->FileType;
    EEFS_NandPut32(&Rec[EEFS_NAND_ER_OFF_ATTR], E->Attributes);
    EEFS_NandPut32(&Rec[EEFS_NAND_ER_OFF_CDATE], E->CreationDate);
    EEFS_NandEncodeSlot(&Rec[EEFS_NAND_ER_OFF_SLOT0], &E->Slot[0]);
    EEFS_NandEncodeSlot(&Rec[EEFS_NAND_ER_OFF_SLOT1], &E->Slot[1]);

    return(EEFS_NAND_ITEM_HDR_SIZE + EEFS_NAND_ENTRY_ITEM_SIZE);

} /* End of EEFS_NandEncodeEntryItem() */

/* CLEANUP item */
uint32 EEFS_NandEncodeCleanupItem(uint8 *Dst, uint32 Avail, const uint16 *Blocks, uint16 Count)
{
    uint8  *Body = &Dst[EEFS_NAND_ITEM_HDR_SIZE];
    uint32  Len = EEFS_NAND_CLEANUP_HDR_SIZE + (2 * (uint32)Count);
    uint32  i;

    if ((Count < 1) || (Len > EEFS_NAND_MAX_ITEM_LEN) || (Avail < (EEFS_NAND_ITEM_HDR_SIZE + Len))) {
        return(0);
    }

    Dst[EEFS_NAND_ITEM_OFF_TYPE] = EEFS_NAND_ITEM_CLEANUP;
    Dst[EEFS_NAND_ITEM_OFF_RESERVED] = 0;
    EEFS_NandPut16(&Dst[EEFS_NAND_ITEM_OFF_LEN], (uint16)Len);
    EEFS_NandPut16(&Body[EEFS_NAND_CLN_OFF_COUNT], Count);
    EEFS_NandPut16(&Body[EEFS_NAND_CLN_OFF_RESERVED], 0);
    for (i = 0; i < Count; i++) {
        EEFS_NandPut16(&Body[EEFS_NAND_CLN_OFF_BLOCKS + (2 * i)], Blocks[i]);
    }

    return(EEFS_NAND_ITEM_HDR_SIZE + Len);

} /* End of EEFS_NandEncodeCleanupItem() */

/* Item iterator */
int32 EEFS_NandNextItem(const uint8 *Payload, uint32 PayloadLen, uint32 *Cursor,
                        uint8 *Type, const uint8 **Body, uint16 *Len)
{
    uint32          Remain;
    uint16          ItemLen;
    const uint8    *Item;

    if (*Cursor == PayloadLen) {
        return(0);
    }
    if (*Cursor > PayloadLen) {
        return(EEFS_CORRUPT_FILE);
    }

    Remain = PayloadLen - *Cursor;
    if (Remain < EEFS_NAND_ITEM_HDR_SIZE) {
        return(EEFS_CORRUPT_FILE);
    }
    Item = &Payload[*Cursor];
    ItemLen = EEFS_NandGet16(&Item[EEFS_NAND_ITEM_OFF_LEN]);
    if ((uint32)ItemLen > (Remain - EEFS_NAND_ITEM_HDR_SIZE)) {
        return(EEFS_CORRUPT_FILE);
    }

    *Type = Item[EEFS_NAND_ITEM_OFF_TYPE];
    *Body = &Item[EEFS_NAND_ITEM_HDR_SIZE];
    *Len = ItemLen;
    *Cursor += EEFS_NAND_ITEM_HDR_SIZE + ItemLen;
    return(1);

} /* End of EEFS_NandNextItem() */

/* VOLUME item body */
int32 EEFS_NandDecodeVolumeItem(const uint8 *Body, uint16 Len, EEFS_NandVolumeItem_t *V)
{
    if (Len != EEFS_NAND_VOLUME_ITEM_SIZE) {
        return(EEFS_CORRUPT_FILE);
    }

    V->PageSize = EEFS_NandGet32(&Body[EEFS_NAND_VOL_OFF_PAGESIZE]);
    V->OobSize = EEFS_NandGet32(&Body[EEFS_NAND_VOL_OFF_OOBSIZE]);
    V->PagesPerBlock = EEFS_NandGet32(&Body[EEFS_NAND_VOL_OFF_PAGESPERBLOCK]);
    V->BlockCount = EEFS_NandGet32(&Body[EEFS_NAND_VOL_OFF_BLOCKCOUNT]);
    V->DirRegionBlocks = EEFS_NandGet32(&Body[EEFS_NAND_VOL_OFF_DIRBLOCKS]);
    V->MaxFiles = EEFS_NandGet32(&Body[EEFS_NAND_VOL_OFF_MAXFILES]);
    V->WriteSeqHigh = EEFS_NandGet32(&Body[EEFS_NAND_VOL_OFF_WRITESEQHIGH]);
    V->FormatTime = EEFS_NandGet32(&Body[EEFS_NAND_VOL_OFF_FORMATTIME]);
    V->PathSize = EEFS_NandGet16(&Body[EEFS_NAND_VOL_OFF_PATHSIZE]);
    V->CfsNameSize = EEFS_NandGet16(&Body[EEFS_NAND_VOL_OFF_NAMESIZE]);
    V->CfsSymbolSize = EEFS_NandGet16(&Body[EEFS_NAND_VOL_OFF_SYMBOLSIZE]);

    return(EEFS_SUCCESS);

} /* End of EEFS_NandDecodeVolumeItem() */

/* ENTRY item body */
int32 EEFS_NandDecodeEntryItem(const uint8 *Body, uint16 Len, uint16 *Index, EEFS_NandEntry_t *E)
{
    const uint8    *Rec = &Body[EEFS_NAND_ENT_OFF_RECORD];

    if ((Len != EEFS_NAND_ENTRY_ITEM_SIZE) ||
        (EEFS_NandIsTerminated(&Rec[EEFS_NAND_ER_OFF_PATH], EEFS_NAND_MAX_PATH_SIZE) == FALSE)) {
        return(EEFS_CORRUPT_FILE);
    }

    memset(E, 0, sizeof(EEFS_NandEntry_t));
    *Index = EEFS_NandGet16(&Body[EEFS_NAND_ENT_OFF_INDEX]);
    EEFS_NandGetString(E->Path, &Rec[EEFS_NAND_ER_OFF_PATH], EEFS_NAND_MAX_PATH_SIZE);
    E->InUse = Rec[EEFS_NAND_ER_OFF_INUSE];
    E->IsAB = Rec[EEFS_NAND_ER_OFF_ISAB];
    E->ActiveSlot = Rec[EEFS_NAND_ER_OFF_ACTIVESLOT];
    E->FallbackCompatible = Rec[EEFS_NAND_ER_OFF_FALLBACK];
    E->ActiveFailed = Rec[EEFS_NAND_ER_OFF_FAILED];
    E->FileType = Rec[EEFS_NAND_ER_OFF_FILETYPE];
    E->Attributes = EEFS_NandGet32(&Rec[EEFS_NAND_ER_OFF_ATTR]);
    E->CreationDate = EEFS_NandGet32(&Rec[EEFS_NAND_ER_OFF_CDATE]);
    EEFS_NandDecodeSlot(&Rec[EEFS_NAND_ER_OFF_SLOT0], &E->Slot[0]);
    EEFS_NandDecodeSlot(&Rec[EEFS_NAND_ER_OFF_SLOT1], &E->Slot[1]);

    return(EEFS_SUCCESS);

} /* End of EEFS_NandDecodeEntryItem() */

/*
 * Local Functions
 */

/* HeaderCrc32 of a metadata page: every byte except the field itself and the signature block */
static uint32 EEFS_NandPage0Crc(const uint8 *Page, uint32 PageSize)
{
    uint32  State;

    State = EEFS_Crc32Update(EEFS_CRC32_INIT, Page, EEFS_NAND_P0_OFF_HDRCRC);
    State = EEFS_Crc32Update(State, &Page[EEFS_NAND_P0_OFF_WRITESEQ], EEFS_NAND_P0_SIG_START - EEFS_NAND_P0_OFF_WRITESEQ);
    State = EEFS_Crc32Update(State, &Page[EEFS_NAND_P0_SIG_END], PageSize - EEFS_NAND_P0_SIG_END);
    return(EEFS_Crc32Final(State));

} /* End of EEFS_NandPage0Crc() */

/* Crc32 of a record page: every byte except the field itself */
static uint32 EEFS_NandRecordCrc(const uint8 *Page, uint32 PageSize)
{
    uint32  State;

    State = EEFS_Crc32Update(EEFS_CRC32_INIT, Page, EEFS_NAND_REC_OFF_CRC);
    State = EEFS_Crc32Update(State, &Page[EEFS_NAND_REC_HDR_SIZE], PageSize - EEFS_NAND_REC_HDR_SIZE);
    return(EEFS_Crc32Final(State));

} /* End of EEFS_NandRecordCrc() */

/* Copies at most Size-1 characters of Src into a Size-byte field and zero pads it (so it is always terminated) */
static void EEFS_NandPutString(uint8 *Dst, const char *Src, uint32 Size)
{
    uint32  i;

    for (i = 0; (i < (Size - 1)) && (Src[i] != '\0'); i++) {
        Dst[i] = (uint8)Src[i];
    }
    for (; i < Size; i++) {
        Dst[i] = 0;
    }

} /* End of EEFS_NandPutString() */

/* Copies a Size-byte string field and makes sure the copy is terminated */
static void EEFS_NandGetString(char *Dst, const uint8 *Src, uint32 Size)
{
    memcpy(Dst, Src, Size);
    Dst[Size - 1] = '\0';

} /* End of EEFS_NandGetString() */

/* TRUE if a NUL occurs within the first Size bytes */
static boolean EEFS_NandIsTerminated(const uint8 *Src, uint32 Size)
{
    return((memchr(Src, 0, Size) != NULL) ? TRUE : FALSE);

} /* End of EEFS_NandIsTerminated() */

/* Slot record (28 bytes); a slot that is not in use is written as zeros */
static void EEFS_NandEncodeSlot(uint8 *Dst, const EEFS_NandSlot_t *S)
{
    memset(Dst, 0, EEFS_NAND_SLOT_REC_SIZE);
    if (S->InUse == 0) {
        return;
    }
    Dst[EEFS_NAND_SR_OFF_INUSE] = S->InUse;
    EEFS_NandPut16(&Dst[EEFS_NAND_SR_OFF_FIRSTBLOCK], S->FirstBlock);
    EEFS_NandPut16(&Dst[EEFS_NAND_SR_OFF_ALLOCBLOCKS], S->AllocBlocks);
    EEFS_NandPut32(&Dst[EEFS_NAND_SR_OFF_FILESIZE], S->FileSize);
    EEFS_NandPut32(&Dst[EEFS_NAND_SR_OFF_VERSION], S->Version);
    EEFS_NandPut32(&Dst[EEFS_NAND_SR_OFF_WRITESEQ], S->WriteSeq);
    EEFS_NandPut32(&Dst[EEFS_NAND_SR_OFF_FILECRC], S->FileCrc32);
    EEFS_NandPut32(&Dst[EEFS_NAND_SR_OFF_MDATE], S->ModificationDate);

} /* End of EEFS_NandEncodeSlot() */

static void EEFS_NandDecodeSlot(const uint8 *Src, EEFS_NandSlot_t *S)
{
    S->InUse = Src[EEFS_NAND_SR_OFF_INUSE];
    S->Reserved = 0;
    S->FirstBlock = EEFS_NandGet16(&Src[EEFS_NAND_SR_OFF_FIRSTBLOCK]);
    S->AllocBlocks = EEFS_NandGet16(&Src[EEFS_NAND_SR_OFF_ALLOCBLOCKS]);
    S->Reserved2 = 0;
    S->FileSize = EEFS_NandGet32(&Src[EEFS_NAND_SR_OFF_FILESIZE]);
    S->Version = EEFS_NandGet32(&Src[EEFS_NAND_SR_OFF_VERSION]);
    S->WriteSeq = EEFS_NandGet32(&Src[EEFS_NAND_SR_OFF_WRITESEQ]);
    S->FileCrc32 = EEFS_NandGet32(&Src[EEFS_NAND_SR_OFF_FILECRC]);
    S->ModificationDate = EEFS_NandGet32(&Src[EEFS_NAND_SR_OFF_MDATE]);

} /* End of EEFS_NandDecodeSlot() */

/************************/
/*  End of File Comment */
/************************/
