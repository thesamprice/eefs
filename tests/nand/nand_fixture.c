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
 * Filename: nand_fixture.c
 *
 * Purpose: Test fixture builder (see nand_fixture.h): EEFS NAND volumes written with the format encoders only.
 *
 */

/*
 * Includes
 */

#include <stdlib.h>
#include "nand_fixture.h"

/*
 * Local Data
 */

static uint8    FxStage[FX_MAX_REC_PAGES][EEFS_NAND_MAX_PAGE_SIZE];    /* record being built */
static uint32   FxStageLen[FX_MAX_REC_PAGES];                          /* payload bytes per staged page */
static uint32   FxStagePage;                                            /* page being filled */
static uint8    FxPage[EEFS_NAND_MAX_PAGE_SIZE];
static uint8    FxPage2[EEFS_NAND_MAX_PAGE_SIZE];
static uint16   FxBlocks[1200];                                         /* Blocks[] of the last copy written */
static uint32   FxCrcs[1200];
static uint32   FxBlockCount;

/*
 * Local Functions
 */

static void Fx_Die(const char *Why)
{
    printf("fixture error: %s\n", Why);
    exit(2);
}

static uint32 Fx_PageSize(const Fx_t *F)
{
    return(F->Dev.Geometry.PageSize);
}

static boolean Fx_IsBad(const Fx_t *F, uint32 Block)
{
    uint8   Oob[EEFS_NAND_MAX_OOB_SIZE];
    uint32  Page;

    for (Page = 0; Page < 2; Page++) {
        NandSim_RawRead(F->Sim, Block, Page, NULL, Oob);
        if (Oob[F->Dev.Geometry.BadMarkerOffset] != 0xFF) {
            return(TRUE);
        }
    }
    return(FALSE);
}

static void Fx_SetMap(Fx_t *F, uint32 Block, uint8 Value)
{
    F->Map[Block] = Value;
    if (F->MapLo > F->MapHi) {
        F->MapLo = F->MapHi = Block;
    }
    else {
        if (Block < F->MapLo) F->MapLo = Block;
        if (Block > F->MapHi) F->MapHi = Block;
    }
}

static void Fx_FreeOwner(Fx_t *F, uint32 Index, uint32 Slot)
{
    uint32  b;
    uint8   Owner = EEFS_NAND_MAP_OWNER(Index, Slot);

    for (b = 0; b < F->Dev.Geometry.BlockCount; b++) {
        if (F->Map[b] == Owner) {
            Fx_SetMap(F, b, EEFS_NAND_MAP_FREE);
        }
    }
    if ((F->Entry[Index].Slot[Slot].InUse != 0) && (F->CleanupCount < FX_MAX_CLEANUP)) {
        F->Cleanup[F->CleanupCount++] = F->Entry[Index].Slot[Slot].FirstBlock;
    }
}

static uint32 Fx_Alloc(Fx_t *F)
{
    while ((F->NextBlock < F->Dev.Geometry.BlockCount) &&
           ((F->Map[F->NextBlock] != EEFS_NAND_MAP_FREE) || (F->Used[F->NextBlock] != 0))) {
        F->NextBlock++;
    }
    if (F->NextBlock >= F->Dev.Geometry.BlockCount) {
        Fx_Die("out of data blocks");
    }
    F->Used[F->NextBlock] = 1;
    return(F->NextBlock++);
}

/* Record staging */

static void Fx_StageBegin(const Fx_t *F, uint8 RecType, uint32 Seq, uint32 Mirror, uint32 Page)
{
    EEFS_NandRecHdr_t   H;

    memset(&H, 0, sizeof(H));
    H.RecType = RecType;
    H.Mirror = (uint8)Mirror;
    H.Seq = Seq;
    H.PageIndex = (uint16)Page;
    H.PageCount = 1;            /* set at the end */
    EEFS_NandBeginRecordPage(FxStage[Page], Fx_PageSize(F), &H);
    FxStageLen[Page] = 0;
}

static uint8 *Fx_StageRoom(const Fx_t *F, uint32 *Avail)
{
    *Avail = Fx_PageSize(F) - EEFS_NAND_REC_HDR_SIZE - FxStageLen[FxStagePage];
    return(&FxStage[FxStagePage][EEFS_NAND_REC_HDR_SIZE + FxStageLen[FxStagePage]]);
}

static void Fx_StageNext(const Fx_t *F)
{
    const uint8 *Hdr = FxStage[FxStagePage];

    if ((FxStagePage + 1) >= FX_MAX_REC_PAGES) {
        Fx_Die("record too large");
    }
    FxStagePage++;
    Fx_StageBegin(F, Hdr[EEFS_NAND_REC_OFF_RECTYPE], EEFS_NandGet32(&Hdr[EEFS_NAND_REC_OFF_SEQ]),
                  Hdr[EEFS_NAND_REC_OFF_MIRROR], FxStagePage);
}

static uint32 Fx_StageFinish(const Fx_t *F)
{
    uint32  Count = FxStagePage + 1;
    uint32  i;

    for (i = 0; i < Count; i++) {
        EEFS_NandPut16(&FxStage[i][EEFS_NAND_REC_OFF_PAGECOUNT], (uint16)Count);
        EEFS_NandSealRecordPage(FxStage[i], Fx_PageSize(F), FxStageLen[i]);
    }
    return(Count);
}

static void Fx_AddVolume(Fx_t *F)
{
    EEFS_NandVolumeItem_t   V;
    uint8                  *Dst;
    uint32                  Avail;
    uint32                  n;

    memset(&V, 0, sizeof(V));
    V.PageSize = F->Dev.Geometry.PageSize;
    V.OobSize = F->Dev.Geometry.OobSize;
    V.PagesPerBlock = F->Dev.Geometry.PagesPerBlock;
    V.BlockCount = F->VolBlockCount;
    V.DirRegionBlocks = F->Dev.Geometry.DirRegionBlocks;
    V.MaxFiles = EEFS_MAX_FILES;
    V.WriteSeqHigh = F->WriteSeq;
    V.FormatTime = 12345;
    V.PathSize = EEFS_NAND_MAX_PATH_SIZE;
    V.CfsNameSize = EEFS_NAND_CFS_NAME_SIZE;
    V.CfsSymbolSize = EEFS_NAND_CFS_SYMBOL_SIZE;

    Dst = Fx_StageRoom(F, &Avail);
    n = EEFS_NandEncodeVolumeItem(Dst, Avail, &V);
    if (n == 0) {
        Fx_StageNext(F);
        Dst = Fx_StageRoom(F, &Avail);
        n = EEFS_NandEncodeVolumeItem(Dst, Avail, &V);
    }
    FxStageLen[FxStagePage] += n;
}

static void Fx_AddMap(Fx_t *F, uint32 Lo, uint32 Hi)
{
    uint8  *Dst;
    uint32  Avail;
    uint32  Fit;
    uint32  Count;

    while (Lo <= Hi) {
        Dst = Fx_StageRoom(F, &Avail);
        Fit = EEFS_NandMapChunkFit(Avail);
        if (Fit == 0) {
            Fx_StageNext(F);
            continue;
        }
        Count = Hi - Lo + 1;
        if (Count > Fit) {
            Count = Fit;
        }
        FxStageLen[FxStagePage] += EEFS_NandEncodeMapChunk(Dst, Avail, Lo, Count, &F->Map[Lo]);
        Lo += Count;
    }
}

static void Fx_AddEntry(Fx_t *F, uint32 Index)
{
    uint8  *Dst;
    uint32  Avail;
    uint32  n;

    Dst = Fx_StageRoom(F, &Avail);
    n = EEFS_NandEncodeEntryItem(Dst, Avail, (uint16)Index, &F->Entry[Index]);
    if (n == 0) {
        Fx_StageNext(F);
        Dst = Fx_StageRoom(F, &Avail);
        n = EEFS_NandEncodeEntryItem(Dst, Avail, (uint16)Index, &F->Entry[Index]);
    }
    FxStageLen[FxStagePage] += n;
}

static void Fx_AddCleanup(Fx_t *F)
{
    uint8  *Dst;
    uint32  Avail;
    uint32  n;

    if (F->CleanupCount == 0) {
        return;
    }
    Dst = Fx_StageRoom(F, &Avail);
    n = EEFS_NandEncodeCleanupItem(Dst, Avail, F->Cleanup, (uint16)F->CleanupCount);
    if (n == 0) {
        Fx_StageNext(F);
        Dst = Fx_StageRoom(F, &Avail);
        n = EEFS_NandEncodeCleanupItem(Dst, Avail, F->Cleanup, (uint16)F->CleanupCount);
    }
    FxStageLen[FxStagePage] += n;
}

/* Builds a CHECKPOINT (full state) or a TXN (staged changes); returns its page count */
static uint32 Fx_BuildRecord(Fx_t *F, uint8 RecType, uint32 Seq, uint32 Mirror)
{
    uint32  i;

    FxStagePage = 0;
    Fx_StageBegin(F, RecType, Seq, Mirror, 0);
    Fx_AddVolume(F);
    if (RecType == EEFS_NAND_REC_CHECKPOINT) {
        Fx_AddMap(F, 0, F->Dev.Geometry.BlockCount - 1);
        for (i = 0; i < EEFS_MAX_FILES; i++) {
            if (F->Entry[i].InUse != 0) {
                Fx_AddEntry(F, i);
            }
        }
    }
    else {
        if (F->MapLo <= F->MapHi) {
            Fx_AddMap(F, F->MapLo, F->MapHi);
        }
        for (i = 0; i < EEFS_MAX_FILES; i++) {
            if (F->Changed[i] != 0) {
                Fx_AddEntry(F, i);
            }
        }
    }
    Fx_AddCleanup(F);
    return(Fx_StageFinish(F));
}

static void Fx_WriteStaged(Fx_t *F, uint32 Mirror, uint32 Count)
{
    uint32  i;

    F->LastRec[Mirror].Block = F->CurBlock[Mirror];
    F->LastRec[Mirror].Page = F->NextPage[Mirror];
    F->LastRec[Mirror].Count = Count;
    for (i = 0; i < Count; i++) {
        NandSim_RawWrite(F->Sim, F->CurBlock[Mirror], F->NextPage[Mirror] + i, FxStage[i], NULL);
    }
    F->NextPage[Mirror] += Count;
}

/* Moves a mirror to its next good ring block and erases it */
static void Fx_EnterBlock(Fx_t *F, uint32 Mirror, boolean First)
{
    uint32  Half = F->Dev.Geometry.DirRegionBlocks / 2;
    uint32  Base = Mirror * Half;
    uint32  b = First ? (Base + Half - 1) : F->CurBlock[Mirror];
    uint32  Tries;

    for (Tries = 0; Tries < Half; Tries++) {
        b = (b + 1 < Base + Half) ? (b + 1) : Base;
        if (Fx_IsBad(F, b) == FALSE) {
            break;
        }
    }
    if (Tries == Half) {
        Fx_Die("no good ring block");
    }
    NandSim_RawErase(F->Sim, b);
    F->CurBlock[Mirror] = b;
    F->NextPage[Mirror] = 0;
}

static void Fx_Init(Fx_t *F)
{
    uint32  b;

    memset(F, 0, sizeof(Fx_t));
    F->Sim = NandTest_NewSim();
    NandSim_FillDevice(F->Sim, &F->Dev);
    for (b = 0; b < F->Dev.Geometry.BlockCount; b++) {
        F->Map[b] = (b < F->Dev.Geometry.DirRegionBlocks) ? EEFS_NAND_MAP_DIR : EEFS_NAND_MAP_FREE;
    }
    F->MapLo = 1;
    F->MapHi = 0;
    F->NextBlock = F->Dev.Geometry.DirRegionBlocks;
    F->MirrorMask = 3;
    F->VolBlockCount = F->Dev.Geometry.BlockCount;
    F->WriteSeq = 100;
}

/*
 * Exported Functions
 */

void Fx_Blank(Fx_t *F)
{
    Fx_Init(F);
}

void Fx_Format(Fx_t *F)
{
    Fx_Init(F);
    Fx_WriteFormat(F);
}

void Fx_WriteFormat(Fx_t *F)
{
    uint32  m;
    uint32  Count;

    F->Seq = 1;
    for (m = 0; m < 2; m++) {
        Fx_EnterBlock(F, m, TRUE);
        Count = Fx_BuildRecord(F, EEFS_NAND_REC_CHECKPOINT, 1, m);
        Fx_WriteStaged(F, m, Count);
        F->MirrorSeq[m] = 1;
    }
}

void Fx_Free(Fx_t *F)
{
    if (F->Sim != NULL) {
        NandSim_Destroy(F->Sim);
        F->Sim = NULL;
    }
}

void Fx_Pattern(uint8 *Data, uint32 Size, uint32 Seed)
{
    uint32  i;
    uint32  x = Seed * 2654435761UL + 1;

    for (i = 0; i < Size; i++) {
        x = x * 1103515245UL + 12345UL;
        Data[i] = (uint8)(x >> 16);
    }
}

uint32 Fx_WriteCopy(Fx_t *F, const char *Path, uint8 PageSlot, uint8 FileType, uint32 Version,
                    const EEFS_NandTypeInfo_t *Type, const uint8 *Data, uint32 Size, EEFS_NandSlot_t *Out)
{
    const EEFS_NandGeometry_t  *Geo = &F->Dev.Geometry;
    uint32                      PageSize = Geo->PageSize;
    uint32                      BlockBytes = PageSize * Geo->PagesPerBlock;
    uint32                      k;
    uint32                      Count;
    uint32                      Offset;
    uint32                      Done;
    uint32                      Chunk;
    uint32                      Page;
    EEFS_NandPage0_t            P;

    FxBlockCount = (uint32)(((uint64)PageSize + Size + BlockBytes - 1) / BlockBytes);
    if (FxBlockCount > EEFS_NandMaxBlocksPerFile(PageSize)) {
        Fx_Die("file too large");
    }
    for (k = 0; k < FxBlockCount; k++) {
        FxBlocks[k] = (uint16)Fx_Alloc(F);
    }

    for (k = 0; k < FxBlockCount; k++) {
        Count = EEFS_NandBlockDataRange(PageSize, Geo->PagesPerBlock, Size, k, &Offset);
        FxCrcs[k] = EEFS_Crc32((Count > 0) ? &Data[Offset] : (const uint8 *)"", Count);
        Page = (k == 0) ? 1 : 0;
        for (Done = 0; Done < Count; Done += Chunk, Page++) {
            Chunk = ((Count - Done) < PageSize) ? (Count - Done) : PageSize;
            memset(FxPage, 0xFF, PageSize);
            memcpy(FxPage, &Data[Offset + Done], Chunk);
            NandSim_RawWrite(F->Sim, FxBlocks[k], Page, FxPage, NULL);
        }
    }

    memset(&P, 0, sizeof(P));
    P.WriteSeq = ++F->WriteSeq;
    strncpy(P.Path, Path, sizeof(P.Path) - 1);
    P.FileType = FileType;
    P.Slot = PageSlot;
    P.Attributes = 0;
    P.Version = Version;
    P.FileSize = Size;
    P.FileCrc32 = EEFS_Crc32((Size > 0) ? Data : (const uint8 *)"", Size);
    P.CreationDate = 1000 + P.WriteSeq;
    P.ModificationDate = 2000 + P.WriteSeq;
    if (Type != NULL) {
        P.Type = *Type;
    }
    P.BlockCount = FxBlockCount;
    if (EEFS_NandEncodePage0(FxPage, PageSize, &P, FxBlocks, FxCrcs) != EEFS_SUCCESS) {
        Fx_Die("page 0 encode");
    }
    NandSim_RawWrite(F->Sim, FxBlocks[0], 0, FxPage, NULL);

    if (Out != NULL) {
        memset(Out, 0, sizeof(*Out));
        Out->InUse = 1;
        Out->FirstBlock = FxBlocks[0];
        Out->AllocBlocks = (uint16)FxBlockCount;
        Out->FileSize = Size;
        Out->Version = Version;
        Out->WriteSeq = P.WriteSeq;
        Out->FileCrc32 = P.FileCrc32;
        Out->ModificationDate = P.ModificationDate;
    }
    return(FxBlocks[0]);
}

uint32 Fx_PutFile(Fx_t *F, uint32 Index, uint8 Slot, uint8 PageSlot, const char *Path, uint8 FileType,
                  uint32 Version, const EEFS_NandTypeInfo_t *Type, const uint8 *Data, uint32 Size)
{
    EEFS_NandEntry_t   *E = &F->Entry[Index];
    EEFS_NandSlot_t     S;
    uint32              k;

    Fx_FreeOwner(F, Index, Slot);
    Fx_WriteCopy(F, Path, PageSlot, FileType, Version, Type, Data, Size, &S);
    for (k = 0; k < FxBlockCount; k++) {
        Fx_SetMap(F, FxBlocks[k], EEFS_NAND_MAP_OWNER(Index, Slot));
    }

    if (E->InUse == 0) {
        memset(E, 0, sizeof(*E));
        E->InUse = 1;
        E->CreationDate = 1000 + S.WriteSeq;
    }
    strncpy(E->Path, Path, sizeof(E->Path) - 1);
    E->FileType = FileType;
    if (PageSlot != EEFS_NAND_SLOT_NONE) {
        E->IsAB = 1;
    }
    E->Slot[Slot] = S;
    Fx_Touch(F, Index);
    return(S.FirstBlock);
}

void Fx_Remove(Fx_t *F, uint32 Index)
{
    Fx_FreeOwner(F, Index, 0);
    Fx_FreeOwner(F, Index, 1);
    memset(&F->Entry[Index], 0, sizeof(EEFS_NandEntry_t));
    Fx_Touch(F, Index);
}

void Fx_Rename(Fx_t *F, uint32 Index, const char *NewPath)
{
    const EEFS_NandGeometry_t  *Geo = &F->Dev.Geometry;
    EEFS_NandEntry_t           *E = &F->Entry[Index];
    EEFS_NandSlot_t            *S;
    EEFS_NandPage0_t            P;
    uint32                      s;
    uint32                      k;
    uint32                      Page;
    uint32                      Old;
    uint32                      New;

    for (s = 0; s < 2; s++) {
        S = &E->Slot[s];
        if (S->InUse == 0) {
            continue;
        }
        Old = S->FirstBlock;
        Fx_RawPage(F, Old, 0, FxPage2);
        if (EEFS_NandDecodePage0(FxPage2, Geo->PageSize, Geo->PagesPerBlock, Old, &P) != EEFS_SUCCESS) {
            Fx_Die("rename: old page 0 invalid");
        }
        for (k = 0; k < P.BlockCount; k++) {
            EEFS_NandPage0Block(FxPage2, k, &FxBlocks[k], &FxCrcs[k]);
        }
        New = Fx_Alloc(F);
        for (Page = 1; Page < Geo->PagesPerBlock; Page++) {
            if (NandSim_PageIsErased(F->Sim, Old, Page) == 0) {
                Fx_RawPage(F, Old, Page, FxPage);
                NandSim_RawWrite(F->Sim, New, Page, FxPage, NULL);
            }
        }
        FxBlocks[0] = (uint16)New;
        memset(P.Path, 0, sizeof(P.Path));
        strncpy(P.Path, NewPath, sizeof(P.Path) - 1);
        P.WriteSeq = ++F->WriteSeq;
        if (EEFS_NandEncodePage0(FxPage, Geo->PageSize, &P, FxBlocks, FxCrcs) != EEFS_SUCCESS) {
            Fx_Die("rename: encode");
        }
        NandSim_RawWrite(F->Sim, New, 0, FxPage, NULL);

        Fx_SetMap(F, Old, EEFS_NAND_MAP_FREE);
        Fx_SetMap(F, New, EEFS_NAND_MAP_OWNER(Index, s));
        if (F->CleanupCount < FX_MAX_CLEANUP) {
            F->Cleanup[F->CleanupCount++] = (uint16)Old;
        }
        S->FirstBlock = (uint16)New;
        S->WriteSeq = P.WriteSeq;
    }
    memset(E->Path, 0, sizeof(E->Path));
    strncpy(E->Path, NewPath, sizeof(E->Path) - 1);
    Fx_Touch(F, Index);
}

void Fx_Touch(Fx_t *F, uint32 Index)
{
    F->Changed[Index] = 1;
}

void Fx_Commit(Fx_t *F)
{
    uint32  Seq = F->Seq + 1;
    uint32  m;
    uint32  Count;
    boolean Checkpoint;

    for (m = 0; m < 2; m++) {
        if ((F->MirrorMask & (1U << m)) == 0) {
            continue;
        }
        Checkpoint = ((F->ForceCheckpoint != 0) || (F->MirrorSeq[m] != F->Seq)) ? TRUE : FALSE;
        if (Checkpoint == FALSE) {
            Count = Fx_BuildRecord(F, EEFS_NAND_REC_TXN, Seq, m);
            if ((F->NextPage[m] + Count) > F->Dev.Geometry.PagesPerBlock) {
                Checkpoint = TRUE;
            }
            else {
                Fx_WriteStaged(F, m, Count);
            }
        }
        if (Checkpoint == TRUE) {
            Fx_EnterBlock(F, m, FALSE);
            Count = Fx_BuildRecord(F, EEFS_NAND_REC_CHECKPOINT, Seq, m);
            Fx_WriteStaged(F, m, Count);
        }
        F->MirrorSeq[m] = Seq;
    }

    memset(F->Changed, 0, sizeof(F->Changed));
    F->MapLo = 1;
    F->MapHi = 0;
    F->CleanupCount = 0;
    F->ForceCheckpoint = 0;
    F->Seq = Seq;
}

void Fx_RawPage(Fx_t *F, uint32 Block, uint32 Page, uint8 *Data)
{
    NandSim_RawRead(F->Sim, Block, Page, Data, NULL);
}

/************************/
/*  End of File Comment */
/************************/
