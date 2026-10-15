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
 * Filename: test_layout.c
 *
 * Purpose: Suite "layout": the on-media format of docs/nand-format.md, checked against the document's own numbers.
 *
 * Design Notes:
 *   Every byte offset and value below is a literal taken from the tables of docs/nand-format.md, and the bytes are
 *   read back with this file's own little-endian helpers and its own bitwise CRC-32, not with the library's
 *   constants or functions.  So the layout cannot drift without a test (and the document) changing.
 *
 */

/*
 * Includes
 */

#include "nand_test.h"

/*
 * Local Data
 */

static uint8    Page[EEFS_NAND_MAX_PAGE_SIZE];
static uint8    Work[EEFS_NAND_MAX_PAGE_SIZE];
static uint16   Blocks[1200];
static uint32   Crcs[1200];

/*
 * Independent helpers (do not use the library's)
 */

static uint32 T_Get16(const uint8 *P)
{
    return((uint32)P[0] | ((uint32)P[1] << 8));
}

static uint32 T_Get32(const uint8 *P)
{
    return((uint32)P[0] | ((uint32)P[1] << 8) | ((uint32)P[2] << 16) | ((uint32)P[3] << 24));
}

static uint64 T_Get64(const uint8 *P)
{
    return((uint64)T_Get32(P) | ((uint64)T_Get32(P + 4) << 32));
}

static uint64 T_U64(uint32 Hi, uint32 Lo)
{
    return(((uint64)Hi << 32) | Lo);
}

/* Bitwise reflected CRC-32 (polynomial 0xEDB88320) */
static uint32 T_CrcUpdate(uint32 Crc, const uint8 *Data, uint32 Length)
{
    uint32  i;
    int     b;

    for (i = 0; i < Length; i++) {
        Crc ^= Data[i];
        for (b = 0; b < 8; b++) {
            Crc = (Crc & 1) ? ((Crc >> 1) ^ 0xEDB88320UL) : (Crc >> 1);
        }
    }
    return(Crc);
}

/* FMT 3.1: HeaderCrc32 over [0,8) + [12,496) + [1528,PageSize) */
static uint32 T_Page0Crc(const uint8 *P, uint32 PageSize)
{
    uint32  Crc = 0xFFFFFFFFUL;

    Crc = T_CrcUpdate(Crc, P, 8);
    Crc = T_CrcUpdate(Crc, P + 12, 496 - 12);
    Crc = T_CrcUpdate(Crc, P + 1528, PageSize - 1528);
    return(Crc ^ 0xFFFFFFFFUL);
}

/* FMT 4.2: Crc32 over [0,28) + [32,PageSize) */
static uint32 T_RecordCrc(const uint8 *P, uint32 PageSize)
{
    uint32  Crc = 0xFFFFFFFFUL;

    Crc = T_CrcUpdate(Crc, P, 28);
    Crc = T_CrcUpdate(Crc, P + 32, PageSize - 32);
    return(Crc ^ 0xFFFFFFFFUL);
}

static void T_Put32(uint8 *P, uint32 V)
{
    P[0] = (uint8)V;
    P[1] = (uint8)(V >> 8);
    P[2] = (uint8)(V >> 16);
    P[3] = (uint8)(V >> 24);
}

static void T_ResealPage0(uint8 *P, uint32 PageSize)
{
    T_Put32(P + 8, T_Page0Crc(P, PageSize));
}

static void T_ResealRecord(uint8 *P, uint32 PageSize)
{
    T_Put32(P + 28, T_RecordCrc(P, PageSize));
}

static int T_AllBytes(const uint8 *P, uint32 Length, uint8 Value)
{
    uint32  i;

    for (i = 0; i < Length; i++) {
        if (P[i] != Value) return(0);
    }
    return(1);
}

/*
 * Cases
 */

/* Every offset constant equals the FMT number; derived capacities equal the FMT tables */
static void layout_page0_offsets(void)
{
    /* 3.1 fixed fields */
    CHECK_EQ(EEFS_NAND_P0_MAGIC, 0x46464545UL);
    CHECK_EQ(EEFS_NAND_P0_OFF_MAGIC, 0);
    CHECK_EQ(EEFS_NAND_P0_OFF_VERSION, 4);
    CHECK_EQ(EEFS_NAND_P0_OFF_RESERVED0, 6);
    CHECK_EQ(EEFS_NAND_P0_OFF_HDRCRC, 8);
    CHECK_EQ(EEFS_NAND_P0_OFF_WRITESEQ, 12);
    CHECK_EQ(EEFS_NAND_P0_OFF_PATH, 16);
    CHECK_EQ(EEFS_NAND_P0_OFF_FILETYPE, 272);
    CHECK_EQ(EEFS_NAND_P0_OFF_SLOT, 273);
    CHECK_EQ(EEFS_NAND_P0_OFF_RESERVED1, 274);
    CHECK_EQ(EEFS_NAND_P0_OFF_ATTR, 276);
    CHECK_EQ(EEFS_NAND_P0_OFF_VERSIONNUM, 280);
    CHECK_EQ(EEFS_NAND_P0_OFF_RESERVED2, 284);
    CHECK_EQ(EEFS_NAND_P0_OFF_FILESIZE, 288);
    CHECK_EQ(EEFS_NAND_P0_OFF_FILECRC, 292);
    CHECK_EQ(EEFS_NAND_P0_OFF_CDATE, 296);
    CHECK_EQ(EEFS_NAND_P0_OFF_MDATE, 300);
    CHECK_EQ(EEFS_NAND_P0_OFF_TYPEINFO, 304);
    CHECK_EQ(EEFS_NAND_P0_TYPEINFO_SIZE, 192);
    CHECK_EQ(EEFS_NAND_P0_OFF_SIGTYPE, 496);
    CHECK_EQ(EEFS_NAND_P0_OFF_RESERVED3, 498);
    CHECK_EQ(EEFS_NAND_P0_OFF_SIGLEN, 500);
    CHECK_EQ(EEFS_NAND_P0_OFF_SIG, 504);
    CHECK_EQ(EEFS_NAND_P0_SIG_SIZE, 1024);
    CHECK_EQ(EEFS_NAND_P0_OFF_BLOCKCOUNT, 1528);
    CHECK_EQ(EEFS_NAND_P0_OFF_BLOCKS, 1532);
    CHECK_EQ(EEFS_NAND_P0_FIXED_SIZE, 1532);
    CHECK_EQ(EEFS_NAND_P0_BLOCK_ENTRY_SIZE, 6);
    CHECK_EQ(EEFS_NAND_P0_SIG_START, 496);
    CHECK_EQ(EEFS_NAND_P0_SIG_END, 1528);

    /* 3.2 type-specific area */
    CHECK_EQ(EEFS_NAND_TI_DESTADDR, 304);
    CHECK_EQ(EEFS_NAND_TI_ELF_ENTRY, 312);
    CHECK_EQ(EEFS_NAND_TI_ELF_FLAGS, 320);
    CHECK_EQ(EEFS_NAND_TI_NAME, 304);
    CHECK_EQ(EEFS_NAND_TI_SYMBOL, 324);
    CHECK_EQ(EEFS_NAND_TI_APP_PRIORITY, 388);
    CHECK_EQ(EEFS_NAND_TI_APP_EXCACTION, 390);
    CHECK_EQ(EEFS_NAND_TI_APP_RESERVED, 391);
    CHECK_EQ(EEFS_NAND_TI_APP_STACKSIZE, 392);
    CHECK_EQ(EEFS_NAND_TI_LIB_RESERVED, 388);
    CHECK_EQ(EEFS_NAND_TI_LIB_RESERVED_SIZE, 8);
    CHECK_EQ(EEFS_NAND_TI_LOADADDR, 396);

    /* 4.2 record page header */
    CHECK_EQ(EEFS_NAND_REC_MAGIC, 0x4A464545UL);
    CHECK_EQ(EEFS_NAND_REC_OFF_MAGIC, 0);
    CHECK_EQ(EEFS_NAND_REC_OFF_VERSION, 4);
    CHECK_EQ(EEFS_NAND_REC_OFF_RECTYPE, 6);
    CHECK_EQ(EEFS_NAND_REC_OFF_MIRROR, 7);
    CHECK_EQ(EEFS_NAND_REC_OFF_SEQ, 8);
    CHECK_EQ(EEFS_NAND_REC_OFF_PAGEINDEX, 12);
    CHECK_EQ(EEFS_NAND_REC_OFF_PAGECOUNT, 14);
    CHECK_EQ(EEFS_NAND_REC_OFF_PAYLOADLEN, 16);
    CHECK_EQ(EEFS_NAND_REC_OFF_RESERVED, 20);
    CHECK_EQ(EEFS_NAND_REC_OFF_CRC, 28);
    CHECK_EQ(EEFS_NAND_REC_HDR_SIZE, 32);
    CHECK_EQ(EEFS_NAND_REC_CHECKPOINT, 1);
    CHECK_EQ(EEFS_NAND_REC_TXN, 2);

    /* 4.3 items */
    CHECK_EQ(EEFS_NAND_ITEM_HDR_SIZE, 4);
    CHECK_EQ(EEFS_NAND_ITEM_VOLUME, 1);
    CHECK_EQ(EEFS_NAND_ITEM_MAPCHUNK, 2);
    CHECK_EQ(EEFS_NAND_ITEM_ENTRY, 3);
    CHECK_EQ(EEFS_NAND_ITEM_CLEANUP, 4);
    CHECK_EQ(EEFS_NAND_VOLUME_ITEM_SIZE, 40);
    CHECK_EQ(EEFS_NAND_VOL_OFF_PAGESIZE, 0);
    CHECK_EQ(EEFS_NAND_VOL_OFF_OOBSIZE, 4);
    CHECK_EQ(EEFS_NAND_VOL_OFF_PAGESPERBLOCK, 8);
    CHECK_EQ(EEFS_NAND_VOL_OFF_BLOCKCOUNT, 12);
    CHECK_EQ(EEFS_NAND_VOL_OFF_DIRBLOCKS, 16);
    CHECK_EQ(EEFS_NAND_VOL_OFF_MAXFILES, 20);
    CHECK_EQ(EEFS_NAND_VOL_OFF_WRITESEQHIGH, 24);
    CHECK_EQ(EEFS_NAND_VOL_OFF_FORMATTIME, 28);
    CHECK_EQ(EEFS_NAND_VOL_OFF_PATHSIZE, 32);
    CHECK_EQ(EEFS_NAND_VOL_OFF_NAMESIZE, 34);
    CHECK_EQ(EEFS_NAND_VOL_OFF_SYMBOLSIZE, 36);
    CHECK_EQ(EEFS_NAND_VOL_OFF_RESERVED, 38);
    CHECK_EQ(EEFS_NAND_MAP_OFF_START, 0);
    CHECK_EQ(EEFS_NAND_MAP_OFF_COUNT, 4);
    CHECK_EQ(EEFS_NAND_MAP_OFF_BYTES, 8);
    CHECK_EQ(EEFS_NAND_MAP_FREEING, 0xFC);
    CHECK_EQ(EEFS_NAND_MAP_DIR, 0xFD);
    CHECK_EQ(EEFS_NAND_MAP_BAD, 0xFE);
    CHECK_EQ(EEFS_NAND_MAP_FREE, 0xFF);
    CHECK_EQ(EEFS_NAND_MAP_OWNER(0, 0), 0x00);
    CHECK_EQ(EEFS_NAND_MAP_OWNER(5, 1), 11);
    CHECK_EQ(EEFS_NAND_MAP_OWNER(125, 1), 0xFB);
    CHECK_EQ(EEFS_NAND_ENT_OFF_INDEX, 0);
    CHECK_EQ(EEFS_NAND_ENT_OFF_RECORD, 4);
    CHECK_EQ(EEFS_NAND_ENTRY_ITEM_SIZE, 332);
    CHECK_EQ(EEFS_NAND_ENTRY_REC_SIZE, 328);
    CHECK_EQ(EEFS_NAND_ER_OFF_PATH, 0);
    CHECK_EQ(EEFS_NAND_ER_OFF_INUSE, 256);
    CHECK_EQ(EEFS_NAND_ER_OFF_ISAB, 257);
    CHECK_EQ(EEFS_NAND_ER_OFF_ACTIVESLOT, 258);
    CHECK_EQ(EEFS_NAND_ER_OFF_FALLBACK, 259);
    CHECK_EQ(EEFS_NAND_ER_OFF_FAILED, 260);
    CHECK_EQ(EEFS_NAND_ER_OFF_FILETYPE, 261);
    CHECK_EQ(EEFS_NAND_ER_OFF_RESERVED, 262);
    CHECK_EQ(EEFS_NAND_ER_OFF_ATTR, 264);
    CHECK_EQ(EEFS_NAND_ER_OFF_CDATE, 268);
    CHECK_EQ(EEFS_NAND_ER_OFF_SLOT0, 272);
    CHECK_EQ(EEFS_NAND_ER_OFF_SLOT1, 300);
    CHECK_EQ(EEFS_NAND_SLOT_REC_SIZE, 28);
    CHECK_EQ(EEFS_NAND_SR_OFF_INUSE, 0);
    CHECK_EQ(EEFS_NAND_SR_OFF_FIRSTBLOCK, 2);
    CHECK_EQ(EEFS_NAND_SR_OFF_ALLOCBLOCKS, 4);
    CHECK_EQ(EEFS_NAND_SR_OFF_RESERVED2, 6);
    CHECK_EQ(EEFS_NAND_SR_OFF_FILESIZE, 8);
    CHECK_EQ(EEFS_NAND_SR_OFF_VERSION, 12);
    CHECK_EQ(EEFS_NAND_SR_OFF_WRITESEQ, 16);
    CHECK_EQ(EEFS_NAND_SR_OFF_FILECRC, 20);
    CHECK_EQ(EEFS_NAND_SR_OFF_MDATE, 24);
    CHECK_EQ(EEFS_NAND_CLN_OFF_COUNT, 0);
    CHECK_EQ(EEFS_NAND_CLN_OFF_BLOCKS, 4);

    /* 1.2 configured sizes of layout version 1 */
    CHECK_EQ(EEFS_NAND_LAYOUT_VERSION, 1);
    CHECK_EQ(EEFS_NAND_MAX_PATH_SIZE, 256);
    CHECK_EQ(EEFS_NAND_CFS_NAME_SIZE, 20);
    CHECK_EQ(EEFS_NAND_CFS_SYMBOL_SIZE, 64);

    /* the RAM structs have the documented sizes (spec 2.4) */
    CHECK_EQ(sizeof(EEFS_NandSlot_t), 28);
    CHECK_EQ(sizeof(EEFS_NandEntry_t), 328);

    /* 3.3: maximum Blocks[] and maximum file size per geometry */
    CHECK_EQ(EEFS_NandMaxBlocksPerFile(2048), 86);
    CHECK_EQ(EEFS_NandMaxBlocksPerFile(4096), 427);
    CHECK_EQ(EEFS_NandMaxBlocksPerFile(8192), 1110);
    CHECK_EQ(86UL * 2048 * 64 - 2048, 11270144UL);
    CHECK_EQ(427UL * 4096 * 128 - 4096, 223866880UL);
    CHECK_EQ(1110UL * 8192 * 64 - 8192, 581951488UL);
    CHECK_EQ(EEFS_NandMaxBlocksPerFile(NandTest_Geo.PageSize), (NandTest_Geo.PageSize - 1532) / 6);
}

/* CRC-32 check value, empty input, and Update/Final chaining */
static void crc32_check_value(void)
{
    static const char   Check[] = "123456789";
    static const char   Fox[] = "The quick brown fox jumps over the lazy dog";
    uint32              State;
    uint32              i;
    uint8               Buffer[1000];

    CHECK_EQ(EEFS_Crc32(Check, 9), 0xCBF43926UL);
    CHECK_EQ(EEFS_Crc32(Fox, (uint32)strlen(Fox)), 0x414FA339UL);
    CHECK_EQ(EEFS_Crc32(Check, 0), 0);
    CHECK_EQ(EEFS_Crc32Final(EEFS_CRC32_INIT), 0);
    CHECK_EQ(EEFS_CRC32_INIT, 0xFFFFFFFFUL);

    /* chaining in uneven pieces equals the one-shot CRC */
    State = EEFS_CRC32_INIT;
    State = EEFS_Crc32Update(State, Check, 1);
    State = EEFS_Crc32Update(State, Check + 1, 0);
    State = EEFS_Crc32Update(State, Check + 1, 5);
    State = EEFS_Crc32Update(State, Check + 6, 3);
    CHECK_EQ(EEFS_Crc32Final(State), 0xCBF43926UL);

    /* the table matches the bitwise definition over all byte values */
    for (i = 0; i < sizeof(Buffer); i++) {
        Buffer[i] = (uint8)((i * 131) ^ (i >> 3));
    }
    CHECK_EQ(EEFS_Crc32(Buffer, sizeof(Buffer)), T_CrcUpdate(0xFFFFFFFFUL, Buffer, sizeof(Buffer)) ^ 0xFFFFFFFFUL);
}

/* Little-endian put/get produce the documented byte order and round-trip */
static void putget_roundtrip(void)
{
    uint8   B[8];
    uint64  V64 = T_U64(0x01020304UL, 0x05060708UL);

    EEFS_NandPut16(B, 0x1234);
    CHECK_EQ(B[0], 0x34);
    CHECK_EQ(B[1], 0x12);
    CHECK_EQ(EEFS_NandGet16(B), 0x1234);

    EEFS_NandPut32(B, 0x11223344UL);
    CHECK_EQ(B[0], 0x44);
    CHECK_EQ(B[1], 0x33);
    CHECK_EQ(B[2], 0x22);
    CHECK_EQ(B[3], 0x11);
    CHECK_EQ(EEFS_NandGet32(B), 0x11223344UL);
    EEFS_NandPut32(B, 0xFFFFFFFFUL);
    CHECK_EQ(EEFS_NandGet32(B), 0xFFFFFFFFUL);

    EEFS_NandPut64(B, V64);
    CHECK_EQ(B[0], 0x08);
    CHECK_EQ(B[3], 0x05);
    CHECK_EQ(B[4], 0x04);
    CHECK_EQ(B[7], 0x01);
    CHECK(EEFS_NandGet64(B) == V64);
    CHECK(T_Get64(B) == V64);
    EEFS_NandPut64(B, T_U64(0xFFFFFFFFUL, 0xFFFFFFFEUL));
    CHECK(EEFS_NandGet64(B) == T_U64(0xFFFFFFFFUL, 0xFFFFFFFEUL));

    memset(B, 0xFF, sizeof(B));
    CHECK(EEFS_NandIsErased(B, sizeof(B)) == TRUE);
    CHECK(EEFS_NandIsErased(B, 0) == TRUE);
    B[7] = 0xFE;
    CHECK(EEFS_NandIsErased(B, sizeof(B)) == FALSE);
}

/* Builds a metadata page description of the given type and block count */
static void T_MakePage0(EEFS_NandPage0_t *P, uint8 FileType, uint32 BlockCount)
{
    uint32  BlockBytes = NandTest_Geo.PageSize * NandTest_Geo.PagesPerBlock;
    uint32  i;

    memset(P, 0, sizeof(*P));
    P->WriteSeq = 0xA1B2C3D4UL;
    strcpy(P->Path, "/cf/apps/sample_app.so");
    P->FileType = FileType;
    P->Slot = (FileType % 3 == 0) ? EEFS_NAND_SLOT_A : ((FileType % 3 == 1) ? EEFS_NAND_SLOT_B : EEFS_NAND_SLOT_NONE);
    P->Attributes = EEFS_ATTRIBUTE_READONLY;
    P->Version = 0x00020003UL;
    P->FileSize = (BlockCount * BlockBytes) - NandTest_Geo.PageSize;   /* largest size the allocation allows */
    P->FileCrc32 = 0xDEADBEEFUL;
    P->CreationDate = 1700000000UL;
    P->ModificationDate = 1700000123UL;
    P->BlockCount = BlockCount;

    switch (FileType) {
        case EEFS_FILE_TYPE_BOOT_FIT:
            P->Type.Fit.DestAddress = T_U64(0x00000001UL, 0x80000000UL);
            break;
        case EEFS_FILE_TYPE_BOOT_ELF:
            P->Type.Elf.DestAddress = T_U64(0x00000002UL, 0x10000000UL);
            P->Type.Elf.EntryPoint = T_U64(0x00000002UL, 0x10000400UL);
            P->Type.Elf.Flags = EEFS_NAND_ELF_FLAG_ENTRY_VALID;
            break;
        case EEFS_FILE_TYPE_BOOT_DTB:
            P->Type.Dtb.DestAddress = T_U64(0, 0x0FF00000UL);
            break;
        case EEFS_FILE_TYPE_CFS_APP:
            strcpy(P->Type.App.AppName, "SAMPLE_APP");
            strcpy(P->Type.App.EntrySymbol, "SAMPLE_APP_Main");
            P->Type.App.Priority = 0x0132;
            P->Type.App.ExceptionAction = 1;
            P->Type.App.StackSize = 0x00004000UL;
            P->Type.App.LoadAddress = T_U64(0x00000003UL, 0x00100000UL);
            break;
        case EEFS_FILE_TYPE_CFS_LIB:
            strcpy(P->Type.Lib.LibName, "SAMPLE_LIB");
            strcpy(P->Type.Lib.EntrySymbol, "SAMPLE_LIB_Init");
            P->Type.Lib.LoadAddress = T_U64(0x00000004UL, 0x00200000UL);
            break;
        default:
            break;
    }

    for (i = 0; i < BlockCount; i++) {
        Blocks[i] = (uint16)(NandTest_Geo.DirRegionBlocks + 100 + i);
        Crcs[i] = 0xC0000000UL + (i * 0x01010101UL);
    }
}

/* Checks an encoded page against the FMT 3.1-3.3 tables, byte by byte */
static void T_CheckPage0Bytes(const uint8 *Pg, const EEFS_NandPage0_t *P)
{
    uint32  PageSize = NandTest_Geo.PageSize;
    uint32  TypeEnd = 304;
    uint32  i;

    CHECK(memcmp(Pg, "EEFF", 4) == 0);
    CHECK_EQ(T_Get32(Pg + 0), 0x46464545UL);
    CHECK_EQ(T_Get16(Pg + 4), 1);
    CHECK_EQ(T_Get16(Pg + 6), 0);
    CHECK_EQ(T_Get32(Pg + 8), T_Page0Crc(Pg, PageSize));
    CHECK_EQ(T_Get32(Pg + 12), P->WriteSeq);
    CHECK(strcmp((const char *)(Pg + 16), P->Path) == 0);
    CHECK(T_AllBytes(Pg + 16 + strlen(P->Path), 256 - strlen(P->Path), 0x00));
    CHECK_EQ(Pg[272], P->FileType);
    CHECK_EQ(Pg[273], P->Slot);
    CHECK_EQ(T_Get16(Pg + 274), 0);
    CHECK_EQ(T_Get32(Pg + 276), P->Attributes);
    CHECK_EQ(T_Get32(Pg + 280), P->Version);
    CHECK_EQ(T_Get32(Pg + 284), 0);
    CHECK_EQ(T_Get32(Pg + 288), P->FileSize);
    CHECK_EQ(T_Get32(Pg + 292), P->FileCrc32);
    CHECK_EQ(T_Get32(Pg + 296), P->CreationDate);
    CHECK_EQ(T_Get32(Pg + 300), P->ModificationDate);

    /* 3.2: defined fields at their offsets, everything else in [304,496) is 0xFF */
    switch (P->FileType) {
        case EEFS_FILE_TYPE_BOOT_FIT:
            CHECK(T_Get64(Pg + 304) == P->Type.Fit.DestAddress);
            TypeEnd = 312;
            break;
        case EEFS_FILE_TYPE_BOOT_ELF:
            CHECK(T_Get64(Pg + 304) == P->Type.Elf.DestAddress);
            CHECK(T_Get64(Pg + 312) == P->Type.Elf.EntryPoint);
            CHECK_EQ(T_Get32(Pg + 320), P->Type.Elf.Flags);
            TypeEnd = 324;
            break;
        case EEFS_FILE_TYPE_BOOT_DTB:
            CHECK(T_Get64(Pg + 304) == P->Type.Dtb.DestAddress);
            TypeEnd = 312;
            break;
        case EEFS_FILE_TYPE_CFS_APP:
            CHECK(strcmp((const char *)(Pg + 304), P->Type.App.AppName) == 0);
            CHECK(T_AllBytes(Pg + 304 + strlen(P->Type.App.AppName), 20 - strlen(P->Type.App.AppName), 0));
            CHECK(strcmp((const char *)(Pg + 324), P->Type.App.EntrySymbol) == 0);
            CHECK(T_AllBytes(Pg + 324 + strlen(P->Type.App.EntrySymbol), 64 - strlen(P->Type.App.EntrySymbol), 0));
            CHECK_EQ(T_Get16(Pg + 388), P->Type.App.Priority);
            CHECK_EQ(Pg[390], P->Type.App.ExceptionAction);
            CHECK_EQ(Pg[391], 0);
            CHECK_EQ(T_Get32(Pg + 392), P->Type.App.StackSize);
            CHECK(T_Get64(Pg + 396) == P->Type.App.LoadAddress);
            TypeEnd = 404;
            break;
        case EEFS_FILE_TYPE_CFS_LIB:
            CHECK(strcmp((const char *)(Pg + 304), P->Type.Lib.LibName) == 0);
            CHECK(T_AllBytes(Pg + 304 + strlen(P->Type.Lib.LibName), 20 - strlen(P->Type.Lib.LibName), 0));
            CHECK(strcmp((const char *)(Pg + 324), P->Type.Lib.EntrySymbol) == 0);
            CHECK(T_AllBytes(Pg + 388, 8, 0x00));
            CHECK(T_Get64(Pg + 396) == P->Type.Lib.LoadAddress);
            TypeEnd = 404;
            break;
        default:
            break;
    }
    CHECK(T_AllBytes(Pg + TypeEnd, 496 - TypeEnd, 0xFF));

    /* unsigned signature block */
    CHECK_EQ(T_Get16(Pg + 496), 0);
    CHECK_EQ(T_Get16(Pg + 498), 0);
    CHECK_EQ(T_Get32(Pg + 500), 0);
    CHECK(T_AllBytes(Pg + 504, 1024, 0xFF));

    /* 3.3 Blocks[] and the unused tail */
    CHECK_EQ(T_Get32(Pg + 1528), P->BlockCount);
    for (i = 0; i < P->BlockCount; i++) {
        CHECK_EQ(T_Get16(Pg + 1532 + (6 * i)), Blocks[i]);
        CHECK_EQ(T_Get32(Pg + 1534 + (6 * i)), Crcs[i]);
    }
    CHECK(T_AllBytes(Pg + 1532 + (6 * P->BlockCount), PageSize - (1532 + (6 * P->BlockCount)), 0xFF));
}

/* Decoded fields equal the encoded description */
static void T_CheckDecoded(const EEFS_NandPage0_t *D, const EEFS_NandPage0_t *P)
{
    CHECK_EQ(D->WriteSeq, P->WriteSeq);
    CHECK(strcmp(D->Path, P->Path) == 0);
    CHECK_EQ(D->FileType, P->FileType);
    CHECK_EQ(D->Slot, P->Slot);
    CHECK_EQ(D->Attributes, P->Attributes);
    CHECK_EQ(D->Version, P->Version);
    CHECK_EQ(D->FileSize, P->FileSize);
    CHECK_EQ(D->FileCrc32, P->FileCrc32);
    CHECK_EQ(D->CreationDate, P->CreationDate);
    CHECK_EQ(D->ModificationDate, P->ModificationDate);
    CHECK_EQ(D->SigType, 0);
    CHECK_EQ(D->SigLength, 0);
    CHECK_EQ(D->BlockCount, P->BlockCount);
    switch (P->FileType) {
        case EEFS_FILE_TYPE_BOOT_FIT:
            CHECK(D->Type.Fit.DestAddress == P->Type.Fit.DestAddress);
            break;
        case EEFS_FILE_TYPE_BOOT_ELF:
            CHECK(D->Type.Elf.DestAddress == P->Type.Elf.DestAddress);
            CHECK(D->Type.Elf.EntryPoint == P->Type.Elf.EntryPoint);
            CHECK_EQ(D->Type.Elf.Flags, P->Type.Elf.Flags);
            break;
        case EEFS_FILE_TYPE_BOOT_DTB:
            CHECK(D->Type.Dtb.DestAddress == P->Type.Dtb.DestAddress);
            break;
        case EEFS_FILE_TYPE_CFS_APP:
            CHECK(strcmp(D->Type.App.AppName, P->Type.App.AppName) == 0);
            CHECK(strcmp(D->Type.App.EntrySymbol, P->Type.App.EntrySymbol) == 0);
            CHECK_EQ(D->Type.App.Priority, P->Type.App.Priority);
            CHECK_EQ(D->Type.App.ExceptionAction, P->Type.App.ExceptionAction);
            CHECK_EQ(D->Type.App.StackSize, P->Type.App.StackSize);
            CHECK(D->Type.App.LoadAddress == P->Type.App.LoadAddress);
            break;
        case EEFS_FILE_TYPE_CFS_LIB:
            CHECK(strcmp(D->Type.Lib.LibName, P->Type.Lib.LibName) == 0);
            CHECK(strcmp(D->Type.Lib.EntrySymbol, P->Type.Lib.EntrySymbol) == 0);
            CHECK(D->Type.Lib.LoadAddress == P->Type.Lib.LoadAddress);
            break;
        default:
            break;
    }
}

/* Decode Page as read from ExpectBlock */
static int32 T_Decode(const uint8 *Pg, uint32 ExpectBlock, EEFS_NandPage0_t *D)
{
    return(EEFS_NandDecodePage0(Pg, NandTest_Geo.PageSize, NandTest_Geo.PagesPerBlock, ExpectBlock, D));
}

/* All six types, 1 and max blocks: byte layout, decode, and what the header CRC covers */
static void page0_encode_decode_each_type(void)
{
    uint32              PageSize = NandTest_Geo.PageSize;
    uint32              MaxBlocks = EEFS_NandMaxBlocksPerFile(PageSize);
    EEFS_NandPage0_t    P;
    EEFS_NandPage0_t    D;
    uint8               Type;
    uint32              n;
    uint32              Count;
    uint32              Off;
    uint32              Step;
    uint32              BlockBytes = PageSize * NandTest_Geo.PagesPerBlock;

    for (Type = EEFS_FILE_TYPE_OTHER; Type <= EEFS_FILE_TYPE_CFS_LIB; Type++) {
        for (n = 0; n < 2; n++) {
            Count = (n == 0) ? 1 : MaxBlocks;
            T_MakePage0(&P, Type, Count);
            CHECK_RC(EEFS_NandEncodePage0(Page, PageSize, &P, Blocks, Crcs), EEFS_SUCCESS);
            T_CheckPage0Bytes(Page, &P);
            CHECK_RC(T_Decode(Page, Blocks[0], &D), EEFS_SUCCESS);
            T_CheckDecoded(&D, &P);

            /* Blocks[] accessor */
            {
                uint16 B;
                uint32 C;
                EEFS_NandPage0Block(Page, Count - 1, &B, &C);
                CHECK_EQ(B, Blocks[Count - 1]);
                CHECK_EQ(C, Crcs[Count - 1]);
            }

            /* the signature block [496,1528) is not covered: changing SigLength or a Sig byte keeps the page valid
             * (SigType itself must stay 0) */
            memcpy(Work, Page, PageSize);
            Work[498] ^= 0x01;
            Work[500] ^= 0x40;
            Work[504] ^= 0x01;
            Work[1000] ^= 0x80;
            Work[1527] ^= 0x01;
            CHECK_RC(T_Decode(Work, Blocks[0], &D), EEFS_SUCCESS);
            memcpy(Work, Page, PageSize);
            Work[496] = 1;
            CHECK_RC(T_Decode(Work, Blocks[0], &D), EEFS_CORRUPT_FILE);

            /* any other byte (including the 0xFF tail) is covered.  Exhaustive for the APP/max case, sampled for the
             * others to keep the run short. */
            Step = ((Type == EEFS_FILE_TYPE_CFS_APP) && (n == 1)) ? 1 : 13;
            for (Off = 0; Off < PageSize; Off += Step) {
                if ((Off >= 496) && (Off < 1528)) {
                    continue;
                }
                memcpy(Work, Page, PageSize);
                Work[Off] ^= 0x01;
                if (T_Decode(Work, Blocks[0], &D) != EEFS_CORRUPT_FILE) {
                    CHECK_EQ(Off, 0xFFFFFFFFUL);    /* reports the offset that was not covered */
                    break;
                }
            }
        }
    }

    /* Decode rejections (each page re-sealed with a valid CRC, so the named check is what rejects it) */
    T_MakePage0(&P, EEFS_FILE_TYPE_CFS_APP, 2);
    CHECK_RC(EEFS_NandEncodePage0(Page, PageSize, &P, Blocks, Crcs), EEFS_SUCCESS);

    CHECK_RC(T_Decode(Page, Blocks[0] + 1, &D), EEFS_CORRUPT_FILE);     /* wrong ExpectBlock */
    CHECK_RC(T_Decode(Page, Blocks[1], &D), EEFS_CORRUPT_FILE);

    memcpy(Work, Page, PageSize);
    Work[16] = 'c';                                                     /* path without a leading '/' */
    T_ResealPage0(Work, PageSize);
    CHECK_RC(T_Decode(Work, Blocks[0], &D), EEFS_CORRUPT_FILE);

    memcpy(Work, Page, PageSize);
    memset(Work + 17, 'x', 255);                                        /* path not terminated within 256 bytes */
    T_ResealPage0(Work, PageSize);
    CHECK_RC(T_Decode(Work, Blocks[0], &D), EEFS_CORRUPT_FILE);
    Work[16 + 255] = 0;                                                 /* 255 characters + NUL is the maximum */
    T_ResealPage0(Work, PageSize);
    CHECK_RC(T_Decode(Work, Blocks[0], &D), EEFS_SUCCESS);
    CHECK_EQ(strlen(D.Path), 255);

    memcpy(Work, Page, PageSize);
    Work[4] = 2;                                                        /* other layout version */
    T_ResealPage0(Work, PageSize);
    CHECK_RC(T_Decode(Work, Blocks[0], &D), EEFS_CORRUPT_FILE);

    memcpy(Work, Page, PageSize);
    Work[3] = 'J';                                                      /* record magic is not a page 0 */
    T_ResealPage0(Work, PageSize);
    CHECK_RC(T_Decode(Work, Blocks[0], &D), EEFS_CORRUPT_FILE);

    memcpy(Work, Page, PageSize);
    T_Put32(Work + 1528, 0);                                            /* BlockCount 0 */
    T_ResealPage0(Work, PageSize);
    CHECK_RC(T_Decode(Work, Blocks[0], &D), EEFS_CORRUPT_FILE);

    memcpy(Work, Page, PageSize);
    T_Put32(Work + 1528, MaxBlocks + 1);                                /* BlockCount past the page */
    T_ResealPage0(Work, PageSize);
    CHECK_RC(T_Decode(Work, Blocks[0], &D), EEFS_CORRUPT_FILE);

    memcpy(Work, Page, PageSize);
    T_Put32(Work + 288, (2 * BlockBytes) - PageSize + 1);               /* FileSize does not fit 2 blocks */
    T_ResealPage0(Work, PageSize);
    CHECK_RC(T_Decode(Work, Blocks[0], &D), EEFS_CORRUPT_FILE);

    memcpy(Work, Page, PageSize);
    Work[296] ^= 0xFF;                                                  /* any covered field, re-sealed: valid */
    T_ResealPage0(Work, PageSize);
    CHECK_RC(T_Decode(Work, Blocks[0], &D), EEFS_SUCCESS);

    memset(Work, 0xFF, PageSize);                                       /* erased page */
    CHECK_RC(T_Decode(Work, Blocks[0], &D), EEFS_CORRUPT_FILE);

    /* Encode rejections */
    T_MakePage0(&P, EEFS_FILE_TYPE_OTHER, 1);
    P.BlockCount = 0;
    CHECK_RC(EEFS_NandEncodePage0(Page, PageSize, &P, Blocks, Crcs), EEFS_INVALID_ARGUMENT);
    P.BlockCount = MaxBlocks + 1;
    CHECK_RC(EEFS_NandEncodePage0(Page, PageSize, &P, Blocks, Crcs), EEFS_INVALID_ARGUMENT);
    T_MakePage0(&P, EEFS_FILE_TYPE_OTHER, 1);
    P.SigType = 1;
    CHECK_RC(EEFS_NandEncodePage0(Page, PageSize, &P, Blocks, Crcs), EEFS_INVALID_ARGUMENT);
    T_MakePage0(&P, EEFS_FILE_TYPE_OTHER, 1);
    P.FileType = 6;
    CHECK_RC(EEFS_NandEncodePage0(Page, PageSize, &P, Blocks, Crcs), EEFS_INVALID_ARGUMENT);
    T_MakePage0(&P, EEFS_FILE_TYPE_OTHER, 1);
    P.Path[0] = 'x';
    CHECK_RC(EEFS_NandEncodePage0(Page, PageSize, &P, Blocks, Crcs), EEFS_INVALID_ARGUMENT);
    T_MakePage0(&P, EEFS_FILE_TYPE_OTHER, 1);
    memset(P.Path + 1, 'p', sizeof(P.Path) - 1);
    CHECK_RC(EEFS_NandEncodePage0(Page, PageSize, &P, Blocks, Crcs), EEFS_INVALID_ARGUMENT);
}

/* 3.3 data layout: the byte range each Blocks[k] holds */
static void block_data_range(void)
{
    uint32  PageSize = NandTest_Geo.PageSize;
    uint32  BlockBytes = PageSize * NandTest_Geo.PagesPerBlock;
    uint32  D0 = BlockBytes - PageSize;
    uint32  Offset;

    CHECK_EQ(EEFS_NandBlockDataRange(PageSize, NandTest_Geo.PagesPerBlock, 0, 0, &Offset), 0);
    CHECK_EQ(Offset, 0);
    CHECK_EQ(EEFS_NandBlockDataRange(PageSize, NandTest_Geo.PagesPerBlock, 1, 0, &Offset), 1);
    CHECK_EQ(EEFS_NandBlockDataRange(PageSize, NandTest_Geo.PagesPerBlock, D0, 0, &Offset), D0);
    CHECK_EQ(EEFS_NandBlockDataRange(PageSize, NandTest_Geo.PagesPerBlock, D0, 1, &Offset), 0);
    CHECK_EQ(Offset, D0);
    CHECK_EQ(EEFS_NandBlockDataRange(PageSize, NandTest_Geo.PagesPerBlock, D0 + 1, 0, &Offset), D0);
    CHECK_EQ(EEFS_NandBlockDataRange(PageSize, NandTest_Geo.PagesPerBlock, D0 + 1, 1, &Offset), 1);
    CHECK_EQ(Offset, D0);
    CHECK_EQ(EEFS_NandBlockDataRange(PageSize, NandTest_Geo.PagesPerBlock, D0 + (3 * BlockBytes) + 17, 2, &Offset), BlockBytes);
    CHECK_EQ(Offset, D0 + BlockBytes);
    CHECK_EQ(EEFS_NandBlockDataRange(PageSize, NandTest_Geo.PagesPerBlock, D0 + (3 * BlockBytes) + 17, 4, &Offset), 17);
    CHECK_EQ(Offset, D0 + (3 * BlockBytes));
    CHECK_EQ(EEFS_NandBlockDataRange(PageSize, NandTest_Geo.PagesPerBlock, D0 + (3 * BlockBytes) + 17, 5, &Offset), 0);
}

/* Record page header layout, CRC coverage and validity rules (FMT 4.2) */
static void record_page_seal_decode(void)
{
    uint32                  PageSize = NandTest_Geo.PageSize;
    EEFS_NandRecHdr_t       H;
    EEFS_NandRecHdr_t       D;
    EEFS_NandVolumeItem_t   V;
    uint32                  Len;

    memset(&V, 0, sizeof(V));
    V.PageSize = PageSize;
    memset(&H, 0, sizeof(H));
    H.RecType = EEFS_NAND_REC_CHECKPOINT;
    H.Mirror = 1;
    H.Seq = 0x01020304UL;
    H.PageIndex = 2;
    H.PageCount = 3;
    H.PayloadLen = 0x7777;      /* ignored by Begin */

    memset(Page, 0x00, PageSize);
    EEFS_NandBeginRecordPage(Page, PageSize, &H);
    CHECK(T_AllBytes(Page + 32, PageSize - 32, 0xFF));
    Len = EEFS_NandEncodeVolumeItem(Page + 32, PageSize - 32, &V);
    CHECK_EQ(Len, 44);
    EEFS_NandSealRecordPage(Page, PageSize, Len);

    CHECK(memcmp(Page, "EEFJ", 4) == 0);
    CHECK_EQ(T_Get32(Page + 0), 0x4A464545UL);
    CHECK_EQ(T_Get16(Page + 4), 1);
    CHECK_EQ(Page[6], 1);
    CHECK_EQ(Page[7], 1);
    CHECK_EQ(T_Get32(Page + 8), 0x01020304UL);
    CHECK_EQ(T_Get16(Page + 12), 2);
    CHECK_EQ(T_Get16(Page + 14), 3);
    CHECK_EQ(T_Get32(Page + 16), 44);
    CHECK(T_AllBytes(Page + 20, 8, 0x00));
    CHECK_EQ(T_Get32(Page + 28), T_RecordCrc(Page, PageSize));
    CHECK_EQ(Page[32], EEFS_NAND_ITEM_VOLUME);
    CHECK(T_AllBytes(Page + 32 + 44, PageSize - 32 - 44, 0xFF));

    CHECK_RC(EEFS_NandDecodeRecordPage(Page, PageSize, &D), EEFS_SUCCESS);
    CHECK_EQ(D.RecType, 1);
    CHECK_EQ(D.Mirror, 1);
    CHECK_EQ(D.Seq, 0x01020304UL);
    CHECK_EQ(D.PageIndex, 2);
    CHECK_EQ(D.PageCount, 3);
    CHECK_EQ(D.PayloadLen, 44);

    /* torn page detection: the last header byte before the payload and the last byte of the page are covered */
    memcpy(Work, Page, PageSize);
    Work[31] ^= 0x01;
    CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_CORRUPT_FILE);
    memcpy(Work, Page, PageSize);
    Work[PageSize - 1] ^= 0x01;
    CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_CORRUPT_FILE);
    memcpy(Work, Page, PageSize);
    Work[20] ^= 0x01;
    CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_CORRUPT_FILE);
    memcpy(Work, Page, PageSize);
    memset(Work + (PageSize / 2), 0xFF, PageSize / 2);                  /* half-programmed page */
    Work[PageSize / 2] = 0x00;
    CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_CORRUPT_FILE);
    memset(Work, 0xFF, PageSize);                                       /* erased page */
    CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_CORRUPT_FILE);

    /* header rules, with a valid CRC */
    memcpy(Work, Page, PageSize);
    Work[12] = 3;                                                       /* PageIndex == PageCount */
    T_ResealRecord(Work, PageSize);
    CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_CORRUPT_FILE);
    memcpy(Work, Page, PageSize);
    Work[6] = 3;                                                        /* RecType */
    T_ResealRecord(Work, PageSize);
    CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_CORRUPT_FILE);
    memcpy(Work, Page, PageSize);
    Work[6] = 2;                                                        /* TXN is fine */
    Work[7] = 0;                                                        /* Mirror is informational */
    T_ResealRecord(Work, PageSize);
    CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_SUCCESS);
    CHECK_EQ(D.RecType, EEFS_NAND_REC_TXN);
    memcpy(Work, Page, PageSize);
    Work[4] = 2;                                                        /* LayoutVersion */
    T_ResealRecord(Work, PageSize);
    CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_CORRUPT_FILE);
    memcpy(Work, Page, PageSize);
    T_Put32(Work + 16, PageSize - 31);                                  /* PayloadLen past the page */
    T_ResealRecord(Work, PageSize);
    CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_CORRUPT_FILE);
    memcpy(Work, Page, PageSize);
    T_Put32(Work + 16, 0);                                              /* empty payload */
    T_ResealRecord(Work, PageSize);
    CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_SUCCESS);

    /* item framing makes the page invalid: an item running past PayloadLen, a known item with a wrong Len */
    memcpy(Work, Page, PageSize);
    T_Put32(Work + 16, 43);
    T_ResealRecord(Work, PageSize);
    CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_CORRUPT_FILE);
    memcpy(Work, Page, PageSize);
    Work[32 + 2] = 39;                                                  /* VOLUME with Len 39 */
    T_Put32(Work + 16, 43);
    T_ResealRecord(Work, PageSize);
    CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_CORRUPT_FILE);
    memcpy(Work, Page, PageSize);
    Work[32 + 0] = 99;                                                  /* unknown item types are skipped */
    T_ResealRecord(Work, PageSize);
    CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_SUCCESS);
}

/* Items: byte layout of VOLUME, MAPCHUNK, ENTRY, CLEANUP, the iterator, and Avail handling (FMT 4.3) */
static void items_roundtrip(void)
{
    static const uint8      Map[6] = { 0xFC, 0x00, 0xFB, 0xFD, 0xFE, 0xFF };
    static const uint16     Cleanup[3] = { 17, 4095, 300 };
    EEFS_NandVolumeItem_t   V;
    EEFS_NandVolumeItem_t   VD;
    EEFS_NandEntry_t        E;
    EEFS_NandEntry_t        ED;
    uint8                  *Buf = Work;
    uint8                  *Ent;
    uint32                  Len;
    uint32                  Total;
    uint32                  Cursor;
    uint8                   Type;
    const uint8            *Body;
    uint16                  BodyLen;
    uint16                  Index;
    int32                   Rc;

    /* VOLUME (4.3.1) */
    V.PageSize = 4096;
    V.OobSize = 224;
    V.PagesPerBlock = 128;
    V.BlockCount = 4096;
    V.DirRegionBlocks = 16;
    V.MaxFiles = 64;
    V.WriteSeqHigh = 0x10203040UL;
    V.FormatTime = 1700000000UL;
    V.PathSize = 256;
    V.CfsNameSize = 20;
    V.CfsSymbolSize = 64;
    memset(Buf, 0xEE, 64);
    CHECK_EQ(EEFS_NandEncodeVolumeItem(Buf, 43, &V), 0);
    CHECK_EQ(EEFS_NandEncodeVolumeItem(Buf, 44, &V), 44);
    CHECK_EQ(Buf[0], 1);
    CHECK_EQ(Buf[1], 0);
    CHECK_EQ(T_Get16(Buf + 2), 40);
    CHECK_EQ(T_Get32(Buf + 4 + 0), 4096);
    CHECK_EQ(T_Get32(Buf + 4 + 4), 224);
    CHECK_EQ(T_Get32(Buf + 4 + 8), 128);
    CHECK_EQ(T_Get32(Buf + 4 + 12), 4096);
    CHECK_EQ(T_Get32(Buf + 4 + 16), 16);
    CHECK_EQ(T_Get32(Buf + 4 + 20), 64);
    CHECK_EQ(T_Get32(Buf + 4 + 24), 0x10203040UL);
    CHECK_EQ(T_Get32(Buf + 4 + 28), 1700000000UL);
    CHECK_EQ(T_Get16(Buf + 4 + 32), 256);
    CHECK_EQ(T_Get16(Buf + 4 + 34), 20);
    CHECK_EQ(T_Get16(Buf + 4 + 36), 64);
    CHECK_EQ(T_Get16(Buf + 4 + 38), 0);
    CHECK_EQ(Buf[44], 0xEE);                                            /* nothing written past the item */
    CHECK_RC(EEFS_NandDecodeVolumeItem(Buf + 4, 40, &VD), EEFS_SUCCESS);
    CHECK(memcmp(&VD, &V, sizeof(V)) == 0);
    CHECK_RC(EEFS_NandDecodeVolumeItem(Buf + 4, 39, &VD), EEFS_CORRUPT_FILE);

    /* MAPCHUNK (4.3.2): 0xFC is serialized as 0xFF */
    CHECK_EQ(EEFS_NandEncodeMapChunk(Buf, 17, 100, 6, Map), 0);
    CHECK_EQ(EEFS_NandEncodeMapChunk(Buf, 18, 100, 0, Map), 0);
    CHECK_EQ(EEFS_NandEncodeMapChunk(Buf, 18, 100, 6, Map), 18);
    CHECK_EQ(Buf[0], 2);
    CHECK_EQ(Buf[1], 0);
    CHECK_EQ(T_Get16(Buf + 2), 14);
    CHECK_EQ(T_Get32(Buf + 4), 100);
    CHECK_EQ(T_Get32(Buf + 8), 6);
    CHECK_EQ(Buf[12], 0xFF);
    CHECK_EQ(Buf[13], 0x00);
    CHECK_EQ(Buf[14], 0xFB);
    CHECK_EQ(Buf[15], 0xFD);
    CHECK_EQ(Buf[16], 0xFE);
    CHECK_EQ(Buf[17], 0xFF);
    CHECK_EQ(EEFS_NandMapChunkFit(12), 0);
    CHECK_EQ(EEFS_NandMapChunkFit(13), 1);
    CHECK_EQ(EEFS_NandMapChunkFit(4064 - 44), 4008);                    /* the 4.6 worked example */

    /* ENTRY (4.3.3) */
    memset(&E, 0, sizeof(E));
    strcpy(E.Path, "/cf/apps/x.so");
    E.InUse = 1;
    E.IsAB = 1;
    E.ActiveSlot = 1;
    E.FallbackCompatible = 1;
    E.ActiveFailed = 1;
    E.FileType = EEFS_FILE_TYPE_CFS_APP;
    E.Reserved = 0x5555;                                                /* reserved fields are written as 0 */
    E.Attributes = 1;
    E.CreationDate = 1700000001UL;
    E.Slot[0].InUse = 1;
    E.Slot[0].Reserved = 0x77;
    E.Slot[0].FirstBlock = 0x0123;
    E.Slot[0].AllocBlocks = 0x0045;
    E.Slot[0].FileSize = 0x11111111UL;
    E.Slot[0].Version = 0x22222222UL;
    E.Slot[0].WriteSeq = 0x33333333UL;
    E.Slot[0].FileCrc32 = 0x44444444UL;
    E.Slot[0].ModificationDate = 0x55555555UL;
    E.Slot[1].InUse = 0;                                                /* unused slot: all zero on media */
    E.Slot[1].FirstBlock = 99;
    E.Slot[1].FileSize = 1234;
    memset(Buf, 0xEE, 400);
    CHECK_EQ(EEFS_NandEncodeEntryItem(Buf, 335, 7, &E), 0);
    CHECK_EQ(EEFS_NandEncodeEntryItem(Buf, 336, 7, &E), 336);
    CHECK_EQ(Buf[0], 3);
    CHECK_EQ(Buf[1], 0);
    CHECK_EQ(T_Get16(Buf + 2), 332);
    CHECK_EQ(T_Get16(Buf + 4), 7);
    CHECK_EQ(T_Get16(Buf + 6), 0);
    Ent = Buf + 8;                                                      /* entry record */
    CHECK(strcmp((const char *)Ent, "/cf/apps/x.so") == 0);
    CHECK(T_AllBytes(Ent + 13, 256 - 13, 0x00));
    CHECK_EQ(Ent[256], 1);
    CHECK_EQ(Ent[257], 1);
    CHECK_EQ(Ent[258], 1);
    CHECK_EQ(Ent[259], 1);
    CHECK_EQ(Ent[260], 1);
    CHECK_EQ(Ent[261], EEFS_FILE_TYPE_CFS_APP);
    CHECK_EQ(T_Get16(Ent + 262), 0);
    CHECK_EQ(T_Get32(Ent + 264), 1);
    CHECK_EQ(T_Get32(Ent + 268), 1700000001UL);
    CHECK_EQ(Ent[272 + 0], 1);
    CHECK_EQ(Ent[272 + 1], 0);
    CHECK_EQ(T_Get16(Ent + 272 + 2), 0x0123);
    CHECK_EQ(T_Get16(Ent + 272 + 4), 0x0045);
    CHECK_EQ(T_Get16(Ent + 272 + 6), 0);
    CHECK_EQ(T_Get32(Ent + 272 + 8), 0x11111111UL);
    CHECK_EQ(T_Get32(Ent + 272 + 12), 0x22222222UL);
    CHECK_EQ(T_Get32(Ent + 272 + 16), 0x33333333UL);
    CHECK_EQ(T_Get32(Ent + 272 + 20), 0x44444444UL);
    CHECK_EQ(T_Get32(Ent + 272 + 24), 0x55555555UL);
    CHECK(T_AllBytes(Ent + 300, 28, 0x00));
    CHECK_EQ(Buf[336], 0xEE);
    CHECK_RC(EEFS_NandDecodeEntryItem(Buf + 4, 332, &Index, &ED), EEFS_SUCCESS);
    CHECK_EQ(Index, 7);
    CHECK(strcmp(ED.Path, E.Path) == 0);
    CHECK_EQ(ED.InUse, 1);
    CHECK_EQ(ED.IsAB, 1);
    CHECK_EQ(ED.ActiveSlot, 1);
    CHECK_EQ(ED.FallbackCompatible, 1);
    CHECK_EQ(ED.ActiveFailed, 1);
    CHECK_EQ(ED.FileType, EEFS_FILE_TYPE_CFS_APP);
    CHECK_EQ(ED.Reserved, 0);
    CHECK_EQ(ED.Attributes, 1);
    CHECK_EQ(ED.CreationDate, 1700000001UL);
    CHECK_EQ(ED.Slot[0].InUse, 1);
    CHECK_EQ(ED.Slot[0].FirstBlock, 0x0123);
    CHECK_EQ(ED.Slot[0].AllocBlocks, 0x0045);
    CHECK_EQ(ED.Slot[0].FileSize, 0x11111111UL);
    CHECK_EQ(ED.Slot[0].Version, 0x22222222UL);
    CHECK_EQ(ED.Slot[0].WriteSeq, 0x33333333UL);
    CHECK_EQ(ED.Slot[0].FileCrc32, 0x44444444UL);
    CHECK_EQ(ED.Slot[0].ModificationDate, 0x55555555UL);
    CHECK_EQ(ED.Slot[1].InUse, 0);
    CHECK_EQ(ED.Slot[1].FirstBlock, 0);
    CHECK_EQ(ED.Slot[1].FileSize, 0);
    CHECK_RC(EEFS_NandDecodeEntryItem(Buf + 4, 331, &Index, &ED), EEFS_CORRUPT_FILE);
    memset(Buf + 8, 'x', 256);                                          /* unterminated path */
    CHECK_RC(EEFS_NandDecodeEntryItem(Buf + 4, 332, &Index, &ED), EEFS_CORRUPT_FILE);

    E.InUse = 0;                                                        /* not in use: the path is written as zeros */
    CHECK_EQ(EEFS_NandEncodeEntryItem(Buf, 336, 125, &E), 336);
    CHECK(T_AllBytes(Buf + 8, 256, 0x00));
    CHECK_EQ(T_Get16(Buf + 4), 125);

    /* CLEANUP (4.3.4) */
    CHECK_EQ(EEFS_NandEncodeCleanupItem(Buf, 13, Cleanup, 3), 0);
    CHECK_EQ(EEFS_NandEncodeCleanupItem(Buf, 14, Cleanup, 0), 0);
    CHECK_EQ(EEFS_NandEncodeCleanupItem(Buf, 14, Cleanup, 3), 14);
    CHECK_EQ(Buf[0], 4);
    CHECK_EQ(Buf[1], 0);
    CHECK_EQ(T_Get16(Buf + 2), 10);
    CHECK_EQ(T_Get16(Buf + 4), 3);
    CHECK_EQ(T_Get16(Buf + 6), 0);
    CHECK_EQ(T_Get16(Buf + 8), 17);
    CHECK_EQ(T_Get16(Buf + 10), 4095);
    CHECK_EQ(T_Get16(Buf + 12), 300);

    /* Iterator over a payload: VOLUME, MAPCHUNK, ENTRY, CLEANUP, an unknown type */
    E.InUse = 1;
    Total = 0;
    Total += EEFS_NandEncodeVolumeItem(Buf + Total, 1000, &V);
    Total += EEFS_NandEncodeMapChunk(Buf + Total, 1000, 100, 6, Map);
    Total += EEFS_NandEncodeEntryItem(Buf + Total, 1000, 3, &E);
    Total += EEFS_NandEncodeCleanupItem(Buf + Total, 1000, Cleanup, 3);
    Buf[Total] = 99;
    Buf[Total + 1] = 0;
    Buf[Total + 2] = 3;
    Buf[Total + 3] = 0;
    Buf[Total + 4] = 0xAA;
    Buf[Total + 5] = 0xBB;
    Buf[Total + 6] = 0xCC;
    Total += 7;
    CHECK_EQ(Total, 44 + 18 + 336 + 14 + 7);

    Cursor = 0;
    CHECK_RC(EEFS_NandNextItem(Buf, Total, &Cursor, &Type, &Body, &BodyLen), 1);
    CHECK_EQ(Type, EEFS_NAND_ITEM_VOLUME);
    CHECK(Body == Buf + 4);
    CHECK_EQ(BodyLen, 40);
    CHECK_RC(EEFS_NandNextItem(Buf, Total, &Cursor, &Type, &Body, &BodyLen), 1);
    CHECK_EQ(Type, EEFS_NAND_ITEM_MAPCHUNK);
    CHECK_EQ(BodyLen, 14);
    CHECK_EQ(T_Get32(Body + 0), 100);
    CHECK_RC(EEFS_NandNextItem(Buf, Total, &Cursor, &Type, &Body, &BodyLen), 1);
    CHECK_EQ(Type, EEFS_NAND_ITEM_ENTRY);
    CHECK_RC(EEFS_NandDecodeEntryItem(Body, BodyLen, &Index, &ED), EEFS_SUCCESS);
    CHECK_EQ(Index, 3);
    CHECK_RC(EEFS_NandNextItem(Buf, Total, &Cursor, &Type, &Body, &BodyLen), 1);
    CHECK_EQ(Type, EEFS_NAND_ITEM_CLEANUP);
    CHECK_EQ(T_Get16(Body), 3);
    CHECK_RC(EEFS_NandNextItem(Buf, Total, &Cursor, &Type, &Body, &BodyLen), 1);
    CHECK_EQ(Type, 99);
    CHECK_EQ(BodyLen, 3);
    CHECK_EQ(Body[2], 0xCC);
    CHECK_RC(EEFS_NandNextItem(Buf, Total, &Cursor, &Type, &Body, &BodyLen), 0);
    CHECK_EQ(Cursor, Total);
    CHECK_RC(EEFS_NandNextItem(Buf, Total, &Cursor, &Type, &Body, &BodyLen), 0);

    /* the iterator stops at PayloadLen (bytes after it are padding) */
    Cursor = 0;
    CHECK_RC(EEFS_NandNextItem(Buf, 44, &Cursor, &Type, &Body, &BodyLen), 1);
    CHECK_RC(EEFS_NandNextItem(Buf, 44, &Cursor, &Type, &Body, &BodyLen), 0);

    /* an item whose 4 + Len exceeds the remaining payload is corrupt, as is a partial item header */
    Cursor = 0;
    CHECK_RC(EEFS_NandNextItem(Buf, 43, &Cursor, &Type, &Body, &BodyLen), EEFS_CORRUPT_FILE);
    Cursor = 0;
    CHECK_RC(EEFS_NandNextItem(Buf, 44 + 17, &Cursor, &Type, &Body, &BodyLen), 1);
    Rc = EEFS_NandNextItem(Buf, 44 + 17, &Cursor, &Type, &Body, &BodyLen);
    CHECK_RC(Rc, EEFS_CORRUPT_FILE);
    Cursor = 0;
    CHECK_RC(EEFS_NandNextItem(Buf, 47, &Cursor, &Type, &Body, &BodyLen), 1);
    CHECK_RC(EEFS_NandNextItem(Buf, 47, &Cursor, &Type, &Body, &BodyLen), EEFS_CORRUPT_FILE);

    /* MAPCHUNK and CLEANUP framing inside a record page */
    {
        EEFS_NandRecHdr_t   H;
        EEFS_NandRecHdr_t   D;
        uint32              PageSize = NandTest_Geo.PageSize;

        memset(&H, 0, sizeof(H));
        H.RecType = EEFS_NAND_REC_TXN;
        H.PageCount = 1;
        EEFS_NandBeginRecordPage(Page, PageSize, &H);
        Len = EEFS_NandEncodeMapChunk(Page + 32, PageSize - 32, 0, 6, Map);
        Len += EEFS_NandEncodeCleanupItem(Page + 32 + Len, PageSize - 32 - Len, Cleanup, 3);
        EEFS_NandSealRecordPage(Page, PageSize, Len);
        CHECK_RC(EEFS_NandDecodeRecordPage(Page, PageSize, &D), EEFS_SUCCESS);
        memcpy(Work, Page, PageSize);
        Work[32 + 8] = 7;                                               /* MAPCHUNK Count != Len - 8 */
        T_ResealRecord(Work, PageSize);
        CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_CORRUPT_FILE);
        memcpy(Work, Page, PageSize);
        Work[32 + 18 + 4] = 2;                                          /* CLEANUP Count != (Len - 4) / 2 */
        T_ResealRecord(Work, PageSize);
        CHECK_RC(EEFS_NandDecodeRecordPage(Work, PageSize, &D), EEFS_CORRUPT_FILE);
    }
}

/* FMT 4.6: packing a CHECKPOINT the way the writer does (VOLUME, greedy MAPCHUNKs, ENTRYs, items never span pages)
 * gives the documented page counts for 4096 blocks */
static uint32 T_CheckpointPages(uint32 PageSize, uint32 BlockCount, uint32 Files, uint32 *EntriesPerPage)
{
    static uint8            Map[4096];
    static uint8            Scratch[8192];      /* independent of this build's EEFS_NAND_MAX_PAGE_SIZE */
    EEFS_NandVolumeItem_t   V;
    EEFS_NandEntry_t        E;
    uint32                  Avail = PageSize - 32;
    uint32                  Used = 0;
    uint32                  Pages = 1;
    uint32                  Next = 0;
    uint32                  Count;
    uint32                  n;
    uint32                  Len;

    memset(&V, 0, sizeof(V));
    memset(&E, 0, sizeof(E));
    memset(Map, 0xFF, sizeof(Map));
    E.InUse = 1;
    strcpy(E.Path, "/x");

    Used += EEFS_NandEncodeVolumeItem(Scratch, Avail - Used, &V);
    while (Next < BlockCount) {
        Count = EEFS_NandMapChunkFit(Avail - Used);
        if (Count > BlockCount - Next) Count = BlockCount - Next;
        if (Count == 0) {
            Pages++;
            Used = 0;
            continue;
        }
        Used += EEFS_NandEncodeMapChunk(Scratch, Avail - Used, Next, Count, Map + Next);
        Next += Count;
    }
    for (n = 0; n < Files; n++) {
        Len = EEFS_NandEncodeEntryItem(Scratch, Avail - Used, (uint16)n, &E);
        if (Len == 0) {
            Pages++;
            Used = 0;
            Len = EEFS_NandEncodeEntryItem(Scratch, Avail - Used, (uint16)n, &E);
        }
        Used += Len;
    }

    /* entries per full page */
    Used = 0;
    *EntriesPerPage = 0;
    while ((Len = EEFS_NandEncodeEntryItem(Scratch, Avail - Used, 0, &E)) != 0) {
        Used += Len;
        (*EntriesPerPage)++;
    }
    return(Pages);
}

static void capacity_table(void)
{
    uint32  PerPage;

    /* | | 2048 | 4096 | 8192 | rows of FMT 4.6 */
    CHECK_EQ(T_CheckpointPages(2048, 4096, 64, &PerPage), 13);
    CHECK_EQ(PerPage, 6);
    CHECK_EQ(T_CheckpointPages(4096, 4096, 64, &PerPage), 7);
    CHECK_EQ(PerPage, 12);
    CHECK_EQ(T_CheckpointPages(8192, 4096, 64, &PerPage), 4);
    CHECK_EQ(PerPage, 24);
    CHECK_EQ(T_CheckpointPages(2048, 4096, 20, &PerPage), 6);
    CHECK_EQ(T_CheckpointPages(4096, 4096, 20, &PerPage), 3);
    CHECK_EQ(T_CheckpointPages(8192, 4096, 20, &PerPage), 2);

    /* payload bytes per page */
    CHECK_EQ(2048 - EEFS_NAND_REC_HDR_SIZE, 2016);
    CHECK_EQ(4096 - EEFS_NAND_REC_HDR_SIZE, 4064);
    CHECK_EQ(8192 - EEFS_NAND_REC_HDR_SIZE, 8160);

    /* minimum PagesPerBlock = 2 x (checkpoint pages + 1), implementation spec section 12 */
    CHECK_EQ(2 * (T_CheckpointPages(2048, 4096, 64, &PerPage) + 1), 28);
    CHECK_EQ(2 * (T_CheckpointPages(4096, 4096, 64, &PerPage) + 1), 16);
    CHECK_EQ(2 * (T_CheckpointPages(8192, 4096, 64, &PerPage) + 1), 10);
}

/*
 * Suite table
 */

const NandTest_Case_t NandTest_LayoutCases[] = {
    { "layout_page0_offsets",           layout_page0_offsets },
    { "crc32_check_value",              crc32_check_value },
    { "putget_roundtrip",               putget_roundtrip },
    { "page0_encode_decode_each_type",  page0_encode_decode_each_type },
    { "block_data_range",               block_data_range },
    { "record_page_seal_decode",        record_page_seal_decode },
    { "items_roundtrip",                items_roundtrip },
    { "capacity_table",                 capacity_table },
    { NULL,                             NULL }
};

/************************/
/*  End of File Comment */
/************************/
