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
 * Filename: nand_vol.c
 *
 * Purpose: Helpers shared by the "fileops" and "powercut" suites (see nand_vol.h).
 *
 */

/*
 * Includes
 */

#include <stdlib.h>
#include "nand_vol.h"

/*
 * Local Data
 */

EEFS_InodeTable_t           V_Vol;
EEFS_InodeTable_t           V_Aux;
EEFS_NandDevice_t           V_Dev;
NandSim_t                  *V_Sim;
uint8                      *V_Data;
uint8                      *V_Data2;
uint8                      *V_Read;
uint8                       V_Page[EEFS_NAND_MAX_PAGE_SIZE];

static EEFS_NandDirState_t  V_StateA;
static EEFS_NandDirState_t  V_StateB;
static uint8                V_Oob[EEFS_NAND_MAX_OOB_SIZE];

/*
 * Function Definitions
 */

uint32 V_BlockBytes(void)
{
    return(NandTest_Geo.PageSize * NandTest_Geo.PagesPerBlock);
}

uint32 V_D0(void)
{
    return(V_BlockBytes() - NandTest_Geo.PageSize);
}

uint32 V_SizeForBlocks(uint32 Blocks)
{
    return((Blocks * V_BlockBytes()) - NandTest_Geo.PageSize);
}

uint32 V_FileOffset(uint32 K, uint32 Within)
{
    return((K == 0) ? Within : (V_D0() + ((K - 1) * V_BlockBytes()) + Within));
}

void V_Attach(NandSim_t *Sim)
{
    if (V_Data == NULL) {       /* test code: the large buffers come from the heap */
        V_Data = (uint8 *)malloc(V_BUF_SIZE);
        V_Data2 = (uint8 *)malloc(V_BUF_SIZE);
        V_Read = (uint8 *)malloc(V_BUF_SIZE);
        if ((V_Data == NULL) || (V_Data2 == NULL) || (V_Read == NULL)) {
            printf("out of memory\n");
            exit(2);
        }
    }
    V_Sim = Sim;
    NandSim_FillDevice(Sim, &V_Dev);
}

int32 V_Mount(EEFS_InodeTable_t *Vol)
{
    memset(Vol, 0, sizeof(*Vol));
    return(EEFS_LibInitFS(Vol, (cpuaddr)&V_Dev));
}

static void V_FormatAndMount(void)
{
    memset(&V_Aux, 0, sizeof(V_Aux));
    CHECK_RC(EEFS_LibFormat(&V_Aux, (cpuaddr)&V_Dev), EEFS_SUCCESS);
    CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);
}

void V_NewVolume(void)
{
    V_Attach(NandTest_NewSim());
    V_FormatAndMount();
}

void V_NewVolumeBlocks(uint32 BlockCount)
{
    EEFS_NandGeometry_t Geo = NandTest_Geo;
    NandSim_t          *Sim;

    Geo.BlockCount = BlockCount;
    Sim = NandSim_Create(&Geo);
    CHECK(Sim != NULL);
    if (Sim == NULL) exit(2);
    V_Attach(Sim);
    V_FormatAndMount();
}

void V_Remount(void)
{
    CHECK_RC(V_Mount(&V_Vol), EEFS_SUCCESS);
}

void V_Destroy(void)
{
    EEFS_LibInit();     /* drops any descriptor a failed case left open */
    memset(&V_Vol, 0, sizeof(V_Vol));
    memset(&V_Aux, 0, sizeof(V_Aux));
    if (V_Sim != NULL) {
        NandSim_Destroy(V_Sim);
        V_Sim = NULL;
    }
}

uint32 V_Ops(void)
{
    return(NandSim_Programs(V_Sim) + NandSim_Erases(V_Sim));
}

/* Cache of generated contents (keyed by Size, Seed and the modifications; the path does not matter) */
#define V_CACHE_SIZE    16
static struct
{
    V_File_t    Key;
    uint8      *Buf;
} V_Cache[V_CACHE_SIZE];
static uint32   V_CacheNext;

static int V_SameContent(const V_File_t *A, const V_File_t *B)
{
    uint32  m;

    if ((A->Size != B->Size) || (A->Seed != B->Seed) || (A->ModCount != B->ModCount)) return(0);
    for (m = 0; m < A->ModCount; m++) {
        if (A->Mod[m] != B->Mod[m]) return(0);
    }
    return(1);
}

const uint8 *V_Content(const V_File_t *F)
{
    uint32  i;

    for (i = 0; i < V_CACHE_SIZE; i++) {
        if ((V_Cache[i].Buf != NULL) && V_SameContent(&V_Cache[i].Key, F)) return(V_Cache[i].Buf);
    }
    i = V_CacheNext;
    V_CacheNext = (V_CacheNext + 1) % V_CACHE_SIZE;
    free(V_Cache[i].Buf);
    V_Cache[i].Buf = (uint8 *)malloc((F->Size > 0) ? F->Size : 1);
    if (V_Cache[i].Buf == NULL) {
        printf("out of memory\n");
        exit(2);
    }
    V_Cache[i].Key = *F;
    V_Fill(V_Cache[i].Buf, F);
    return(V_Cache[i].Buf);
}

void V_Fill(uint8 *Buf, const V_File_t *F)
{
    uint32  i;
    uint32  x;
    uint32  m;

    for (i = 0; i < F->Size; i++) {
        x = (i ^ (F->Seed * 0x01000193UL)) * 0x9E3779B1UL;
        Buf[i] = (uint8)(x >> 24);
    }
    for (m = 0; m < F->ModCount; m++) {
        if (F->Mod[m] < F->Size) Buf[F->Mod[m]] ^= 0x5A;
    }
}

void V_Meta(EEFS_NandFileInfo_t *Meta, uint8 FileType, uint32 Version)
{
    memset(Meta, 0, sizeof(*Meta));
    Meta->FileType = FileType;
    Meta->Slot = EEFS_NAND_SLOT_NONE;
    Meta->Version = Version;
}

int32 V_WriteName(const char *Name, const V_File_t *F)
{
    EEFS_NandFileInfo_t Meta;

    V_Meta(&Meta, EEFS_FILE_TYPE_OTHER, 1);
    Meta.Attributes = F->Attributes;
    return(EEFS_LibWriteFile(&V_Vol, (char *)Name, &Meta, V_Content(F), F->Size));
}

int32 V_Write(const V_File_t *F)
{
    return(V_WriteName(F->Path, F));
}

void V_CheckFile(EEFS_InodeTable_t *Vol, const V_File_t *F)
{
    V_CheckFileEx(Vol, F, TRUE);
}

void V_CheckFileEx(EEFS_InodeTable_t *Vol, const V_File_t *F, boolean Verify)
{
    EEFS_Stat_t     St;
    int32           Fd;
    int32           n;
    uint32          Total = 0;

    CHECK_RC(EEFS_LibStat(Vol, (char *)F->Path, &St), EEFS_SUCCESS);
    CHECK_EQ(St.FileSize, F->Size);
    CHECK_EQ(St.Attributes, F->Attributes);
    Fd = EEFS_LibOpen(Vol, (char *)F->Path, O_RDONLY, 0);
    CHECK(Fd >= 0);
    if (Fd < 0) {
        printf("    open %s: %ld\n", F->Path, (long)Fd);
        return;
    }
    for (;;) {
        n = EEFS_LibRead(Fd, &V_Read[Total], (Total + 100003UL <= V_BUF_SIZE) ? 100003UL : (V_BUF_SIZE - Total));
        CHECK(n >= 0);
        if (n <= 0) break;
        Total += (uint32)n;
        if (Total >= V_BUF_SIZE) break;
    }
    CHECK_EQ(Total, F->Size);
    if (Total == F->Size) {
        CHECK(memcmp(V_Read, V_Content(F), F->Size) == 0);
    }
    CHECK_RC(EEFS_LibClose(Fd), EEFS_SUCCESS);
    if (Verify == TRUE) {
        CHECK_RC(EEFS_LibVerifyFile(Vol, (char *)F->Path), EEFS_SUCCESS);
    }
}

void V_CheckAbsent(EEFS_InodeTable_t *Vol, const char *Path)
{
    EEFS_Stat_t     St;

    CHECK_RC(EEFS_LibStat(Vol, (char *)Path, &St), EEFS_FILE_NOT_FOUND);
}

int32 V_Entry(EEFS_InodeTable_t *Vol, const char *Path)
{
    return(EEFS_NandFindEntry(Vol, Path));
}

const EEFS_NandSlot_t *V_SlotA(EEFS_InodeTable_t *Vol, const char *Path)
{
    static EEFS_NandSlot_t  None;
    int32                   i = EEFS_NandFindEntry(Vol, Path);

    if (i < 0) {
        memset(&None, 0, sizeof(None));
        return(&None);
    }
    return(&Vol->Working.Entry[i].Slot[0]);
}

uint32 V_RawBlocks(uint32 Block, uint16 *Blocks, uint32 *Crcs)
{
    EEFS_NandPage0_t    P;
    uint32              k;

    NandSim_RawRead(V_Sim, Block, 0, V_Page, NULL);
    if (EEFS_NandDecodePage0(V_Page, NandTest_Geo.PageSize, NandTest_Geo.PagesPerBlock, Block, &P) != EEFS_SUCCESS) {
        return(0);
    }
    for (k = 0; k < P.BlockCount; k++) {
        EEFS_NandPage0Block(V_Page, k, &Blocks[k], &Crcs[k]);
    }
    return(P.BlockCount);
}

void V_Take(const EEFS_InodeTable_t *Vol, V_Snap_t *S)
{
    uint32  b;
    uint32  i;

    memset(S, 0, sizeof(*S));
    memcpy(S->Entry, Vol->Working.Entry, sizeof(S->Entry));
    for (i = 0; i < EEFS_MAX_FILES; i++) {
        S->Entry[i].CreationDate = 0;
        S->Entry[i].Slot[0].ModificationDate = 0;
        S->Entry[i].Slot[1].ModificationDate = 0;
    }
    for (b = Vol->Dev->Geometry.DirRegionBlocks; b < Vol->Dev->Geometry.BlockCount; b++) {
        S->Map[b] = (Vol->Working.Map[b] == EEFS_NAND_MAP_FREEING) ? EEFS_NAND_MAP_FREE : Vol->Working.Map[b];
    }
    S->LastSeq = Vol->LastSeq;
}

int V_Same(const V_Snap_t *A, const V_Snap_t *B)
{
    return(memcmp(A, B, sizeof(*A)) == 0);
}

void V_CheckMirrorsAgree(EEFS_InodeTable_t *Vol)
{
    EEFS_NandChain_t    C[2];
    static uint16       Cl[EEFS_NAND_MAX_CLEANUP_BLOCKS];
    uint16              N;
    uint32              MaxFiles;
    uint32              Dir = Vol->Dev->Geometry.DirRegionBlocks;
    uint32              Data = Vol->Dev->Geometry.BlockCount - Dir;

    CHECK_RC(EEFS_NandScanMirror(Vol, 0, &C[0]), EEFS_SUCCESS);
    CHECK_RC(EEFS_NandScanMirror(Vol, 1, &C[1]), EEFS_SUCCESS);
    CHECK(C[0].Valid && C[1].Valid);
    if (!(C[0].Valid && C[1].Valid)) return;
    CHECK_EQ(C[0].LastSeq, Vol->LastSeq);
    CHECK_EQ(C[1].LastSeq, Vol->LastSeq);
    CHECK_RC(EEFS_NandApplyChain(Vol, 0, &C[0], &V_StateA, Cl, &N, &MaxFiles), EEFS_SUCCESS);
    CHECK_RC(EEFS_NandApplyChain(Vol, 1, &C[1], &V_StateB, Cl, &N, &MaxFiles), EEFS_SUCCESS);
    CHECK(memcmp(V_StateA.Entry, V_StateB.Entry, sizeof(V_StateA.Entry)) == 0);
    CHECK(memcmp(&V_StateA.Map[Dir], &V_StateB.Map[Dir], Data) == 0);
    CHECK(memcmp(V_StateA.Entry, Vol->Base.Entry, sizeof(V_StateA.Entry)) == 0);
    CHECK(memcmp(&V_StateA.Map[Dir], &Vol->Base.Map[Dir], Data) == 0);
}

/* Fast CRC-32 for test-side checks (slicing by 8; the same CRC as EEFS_Crc32) */
static uint32   V_CrcTab[8][256];
static int      V_CrcReady;

static uint32 V_Crc32Update(uint32 C, const uint8 *P, uint32 Len)
{
    uint32  i;
    uint32  j;
    uint32  k;

    uint32  T;

    if (!V_CrcReady) {
        for (i = 0; i < 256; i++) {
            T = i;
            for (j = 0; j < 8; j++) T = (T & 1) ? (0xEDB88320UL ^ (T >> 1)) : (T >> 1);
            V_CrcTab[0][i] = T;
        }
        for (i = 0; i < 256; i++) {
            for (k = 1; k < 8; k++) {
                V_CrcTab[k][i] = (V_CrcTab[k - 1][i] >> 8) ^ V_CrcTab[0][V_CrcTab[k - 1][i] & 0xFF];
            }
        }
        V_CrcReady = 1;
    }
    while (Len >= 8) {
        uint32 Lo = C ^ ((uint32)P[0] | ((uint32)P[1] << 8) | ((uint32)P[2] << 16) | ((uint32)P[3] << 24));
        uint32 Hi = (uint32)P[4] | ((uint32)P[5] << 8) | ((uint32)P[6] << 16) | ((uint32)P[7] << 24);
        C = V_CrcTab[7][Lo & 0xFF] ^ V_CrcTab[6][(Lo >> 8) & 0xFF] ^ V_CrcTab[5][(Lo >> 16) & 0xFF] ^
            V_CrcTab[4][Lo >> 24] ^ V_CrcTab[3][Hi & 0xFF] ^ V_CrcTab[2][(Hi >> 8) & 0xFF] ^
            V_CrcTab[1][(Hi >> 16) & 0xFF] ^ V_CrcTab[0][Hi >> 24];
        P += 8;
        Len -= 8;
    }
    while (Len-- > 0) {
        C = V_CrcTab[0][(C ^ *P++) & 0xFF] ^ (C >> 8);
    }
    return(C);
}

/* Rebuild safety (docs/nand-implementation.md 6.3, 10): a metadata page on the device, referenced or not, never lists a
 * data block (position >= 1) whose content does not match its CRC.  Blocks carrying a bad-block marker are skipped
 * (a rebuild ignores them). */
static int V_IsLiveFirst(const EEFS_InodeTable_t *Vol, uint32 Block)
{
    uint32  i;
    uint32  s;

    for (i = 0; i < EEFS_MAX_FILES; i++) {
        for (s = 0; s < 2; s++) {
            if (Vol->Working.Entry[i].InUse && Vol->Working.Entry[i].Slot[s].InUse &&
                (Vol->Working.Entry[i].Slot[s].FirstBlock == Block)) return(1);
        }
    }
    return(0);
}

void V_CheckOrphansComplete(const EEFS_InodeTable_t *Live)
{
    const EEFS_NandGeometry_t  *Geo = &V_Dev.Geometry;
    EEFS_NandPage0_t            P;
    uint16                      B;
    uint32                      Crc;
    uint32                      Run;
    uint32                      Len;
    uint32                      Done;
    uint32                      n;
    uint32                      p;
    uint32                      b;
    uint32                      k;

    for (b = Geo->DirRegionBlocks; b < Geo->BlockCount; b++) {
        if (!NandSim_PageIsProgrammed(V_Sim, b, 0)) continue;
        if ((Live != NULL) && V_IsLiveFirst(Live, b)) continue;     /* checked by VerifyFile */
        NandSim_RawRead(V_Sim, b, 0, V_Page, V_Oob);
        if (V_Oob[Geo->BadMarkerOffset] != 0xFF) continue;
        if (EEFS_NandDecodePage0(V_Page, Geo->PageSize, Geo->PagesPerBlock, b, &P) != EEFS_SUCCESS) continue;
        for (k = 1; k < P.BlockCount; k++) {
            NandSim_RawRead(V_Sim, b, 0, V_Page, NULL);
            EEFS_NandPage0Block(V_Page, k, &B, &Crc);
            Len = EEFS_NandBlockDataRange(Geo->PageSize, Geo->PagesPerBlock, P.FileSize, k, NULL);
            Run = EEFS_CRC32_INIT;
            for (p = 0, Done = 0; Done < Len; p++) {
                NandSim_RawRead(V_Sim, B, p, V_Page, NULL);
                n = Len - Done;
                if (n > Geo->PageSize) n = Geo->PageSize;
                Run = V_Crc32Update(Run, V_Page, n);
                Done += n;
            }
            if (EEFS_Crc32Final(Run) != Crc) {
                printf("    metadata page at block %lu (%s, WriteSeq %lu) lists block %lu at position %lu with bad data\n",
                       (unsigned long)b, P.Path, (unsigned long)P.WriteSeq, (unsigned long)B, (unsigned long)k);
                CHECK(EEFS_Crc32Final(Run) == Crc);
                return;
            }
        }
    }
}

void V_CheckSlotsValid(EEFS_InodeTable_t *Vol)
{
    uint32  i;
    uint32  s;

    for (i = 0; i < EEFS_MAX_FILES; i++) {
        for (s = 0; s < 2; s++) {
            if (Vol->Working.Entry[i].InUse && Vol->Working.Entry[i].Slot[s].InUse) {
                CHECK_EQ(Vol->SlotFlags[i][s], 0);
            }
        }
    }
}

/************************/
/*  End of File Comment */
/************************/
