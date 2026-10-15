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
 * Filename: eefs_nand_io.c
 *
 * Purpose: HAL wrappers of the NAND backend: cached page read, program with read-back verify, erase, bad-block marking and
 *   the factory bad-marker test (docs/nand-implementation.md section 3.1).
 *
 * Design Notes:
 *   Every HAL call of the library goes through these wrappers.  ReadPage status codes map to EEFS_SUCCESS (corrected
 *   bits reported) or EEFS_IO_ERROR; a failed program, erase or read-back verify is EEFS_IO_ERROR and the caller
 *   decides whether to mark the block bad.  The one-page read cache (CacheBuf) is invalidated by every program and
 *   erase.  EEFS_NandMarkBad also takes a directory block out of its mirror ring.
 *
 */

/*
 * Includes
 */

#include "common_types.h"
#include "eefs_fileapi.h"
#include "eefs_macros.h"
#include "eefs_nand_internal.h"
#include <string.h>

/*
 * Local Function Prototypes
 */

static void     EEFS_NandRemoveFromRing(EEFS_InodeTable_t *Vol, uint32 Block);

/*
 * Function Definitions
 */

/* Uncached page read */
int32 EEFS_NandReadPage(EEFS_InodeTable_t *Vol, uint32 Block, uint32 Page, uint8 *Data, uint8 *Oob, uint32 *Corrected)
{
    int32   Status;

    if ((Data == Vol->CacheBuf) || (Oob == Vol->CacheBuf)) {
        Vol->CacheValid = FALSE;
    }
    Status = Vol->Dev->Ops->ReadPage(Vol->Dev->Ctx, Block, Page, Data, Oob);
    if (Status >= 0) {
        if (Corrected != NULL) *Corrected = (uint32)Status;
        return(EEFS_SUCCESS);
    }
    if (Corrected != NULL) {
        *Corrected = (Status == EEFS_NAND_READ_UNCORRECTABLE) ? 0xFFFFFFFFUL : 0;
    }
    return(EEFS_IO_ERROR);

} /* End of EEFS_NandReadPage() */

/* Cached page read (data area only) */
const uint8 *EEFS_NandReadCached(EEFS_InodeTable_t *Vol, uint32 Block, uint32 Page, int32 *Status)
{
    if ((Vol->CacheValid == TRUE) && (Vol->CacheBlock == Block) && (Vol->CachePage == Page)) {
        *Status = EEFS_SUCCESS;
        return(Vol->CacheBuf);
    }
    Vol->CacheValid = FALSE;
    if (EEFS_NandReadPage(Vol, Block, Page, Vol->CacheBuf, NULL, NULL) != EEFS_SUCCESS) {
        *Status = EEFS_IO_ERROR;
        return(NULL);
    }
    Vol->CacheBlock = Block;
    Vol->CachePage = Page;
    Vol->CacheValid = TRUE;
    *Status = EEFS_SUCCESS;
    return(Vol->CacheBuf);

} /* End of EEFS_NandReadCached() */

/* Drops the cached page */
void EEFS_NandInvalidateCache(EEFS_InodeTable_t *Vol)
{
    Vol->CacheValid = FALSE;

} /* End of EEFS_NandInvalidateCache() */

/* Programs a page; the read-back verify uses PageBuf */
int32 EEFS_NandProgramPage(EEFS_InodeTable_t *Vol, uint32 Block, uint32 Page, const uint8 *Data)
{
    return(EEFS_NandProgramPageVerified(Vol, Block, Page, Data, Vol->PageBuf));

} /* End of EEFS_NandProgramPage() */

/* Programs a page and, with EEFS_NAND_VERIFY_WRITES, reads it back into Scratch and compares */
int32 EEFS_NandProgramPageVerified(EEFS_InodeTable_t *Vol, uint32 Block, uint32 Page, const uint8 *Data, uint8 *Scratch)
{
    EEFS_NandInvalidateCache(Vol);
    if (Vol->Dev->Ops->ProgramPage(Vol->Dev->Ctx, Block, Page, Data, NULL) != 0) {
        return(EEFS_IO_ERROR);
    }
#if EEFS_NAND_VERIFY_WRITES
    if ((const uint8 *)Scratch == Data) {   /* never read back over the source */
        Scratch = ((const uint8 *)Vol->CacheBuf == Data) ? Vol->PageBuf : Vol->CacheBuf;
    }
    if (EEFS_NandReadPage(Vol, Block, Page, Scratch, NULL, NULL) != EEFS_SUCCESS) {
        return(EEFS_IO_ERROR);
    }
    if (memcmp(Scratch, Data, Vol->PageSize) != 0) {
        return(EEFS_IO_ERROR);
    }
#else
    (void)Scratch;
#endif
    return(EEFS_SUCCESS);

} /* End of EEFS_NandProgramPageVerified() */

/* Erases a block */
int32 EEFS_NandEraseBlock(EEFS_InodeTable_t *Vol, uint32 Block)
{
    EEFS_NandInvalidateCache(Vol);
    if (Vol->Dev->Ops->EraseBlock(Vol->Dev->Ctx, Block) != 0) {
        return(EEFS_IO_ERROR);
    }
    return(EEFS_SUCCESS);

} /* End of EEFS_NandEraseBlock() */

/* Records a grown bad block: map, best-effort OOB marker, mirror ring */
void EEFS_NandMarkBad(EEFS_InodeTable_t *Vol, uint32 Block)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      Page;

    if (Block >= Geo->BlockCount) {
        return;
    }
    Vol->Working.Map[Block] = EEFS_NAND_MAP_BAD;

    if ((Geo->BadMarkerPages & EEFS_NAND_BADMARKER_PAGE0) != 0) {
        Page = 0;
    }
    else if ((Geo->BadMarkerPages & EEFS_NAND_BADMARKER_PAGE1) != 0) {
        Page = 1;
    }
    else {
        Page = Geo->PagesPerBlock - 1;
    }
    memset(Vol->OobBuf, 0xFF, Geo->OobSize);
    Vol->OobBuf[Geo->BadMarkerOffset] = 0x00;
    EEFS_NandInvalidateCache(Vol);
    (void)Vol->Dev->Ops->ProgramPage(Vol->Dev->Ctx, Block, Page, NULL, Vol->OobBuf);   /* best effort */

    if (Block < Geo->DirRegionBlocks) {
        EEFS_NandRemoveFromRing(Vol, Block);
    }
    Vol->VolumeChangeCounter++;

} /* End of EEFS_NandMarkBad() */

/* Factory or grown bad-block marker test */
boolean EEFS_NandHasBadMarker(EEFS_InodeTable_t *Vol, uint32 Block)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    uint32                      Pages[3];
    uint32                      Count = 0;
    uint32                      i;

    if ((Geo->BadMarkerPages & EEFS_NAND_BADMARKER_PAGE0) != 0) Pages[Count++] = 0;
    if ((Geo->BadMarkerPages & EEFS_NAND_BADMARKER_PAGE1) != 0) Pages[Count++] = 1;
    if ((Geo->BadMarkerPages & EEFS_NAND_BADMARKER_LAST) != 0) Pages[Count++] = Geo->PagesPerBlock - 1;

    for (i = 0; i < Count; i++) {
        if (EEFS_NandReadPage(Vol, Block, Pages[i], NULL, Vol->OobBuf, NULL) != EEFS_SUCCESS) {
            return(TRUE);
        }
        if (Vol->OobBuf[Geo->BadMarkerOffset] != 0xFF) {
            return(TRUE);
        }
    }
    return(FALSE);

} /* End of EEFS_NandHasBadMarker() */

/* Erased-page test (data area) */
boolean EEFS_NandPageIsErased(EEFS_InodeTable_t *Vol, uint32 Block, uint32 Page)
{
    if (EEFS_NandReadPage(Vol, Block, Page, Vol->PageBuf, NULL, NULL) != EEFS_SUCCESS) {
        return(FALSE);
    }
    return(EEFS_NandIsErased(Vol->PageBuf, Vol->PageSize));

} /* End of EEFS_NandPageIsErased() */

/* Reads and validates a metadata page through the cache */
int32 EEFS_NandReadPage0(EEFS_InodeTable_t *Vol, uint32 Block, EEFS_NandPage0_t *P, const uint8 **Raw)
{
    const EEFS_NandGeometry_t  *Geo = &Vol->Dev->Geometry;
    const uint8                *Data;
    int32                       Status;
    uint32                      i;
    uint16                      B;
    uint32                      Crc;

    if (Raw != NULL) *Raw = NULL;
    if ((Block < Geo->DirRegionBlocks) || (Block >= Geo->BlockCount)) {
        return(EEFS_CORRUPT_FILE);
    }
    if ((Data = EEFS_NandReadCached(Vol, Block, 0, &Status)) == NULL) {
        return(EEFS_IO_ERROR);
    }
    if (EEFS_NandDecodePage0(Data, Geo->PageSize, Geo->PagesPerBlock, Block, P) != EEFS_SUCCESS) {
        return(EEFS_CORRUPT_FILE);
    }
    for (i = 0; i < P->BlockCount; i++) {
        EEFS_NandPage0Block(Data, i, &B, &Crc);
        if ((B < Geo->DirRegionBlocks) || (B >= Geo->BlockCount)) {
            return(EEFS_CORRUPT_FILE);
        }
    }
    if (Raw != NULL) *Raw = Data;
    return(EEFS_SUCCESS);

} /* End of EEFS_NandReadPage0() */

/*
 * Local Functions
 */

/* Takes a directory block out of its mirror ring.  CurIndex is moved so that the next EEFS_NandEnterBlock goes to the
 * block that followed the removed one; if the removed block held the mirror's current state, the next record must
 * start a fresh block. */
static void EEFS_NandRemoveFromRing(EEFS_InodeTable_t *Vol, uint32 Block)
{
    EEFS_NandMirror_t  *Mir;
    uint32              m;
    uint32              i;
    uint32              Idx;

    for (m = 0; m < 2; m++) {
        Mir = &Vol->Mirror[m];
        for (Idx = 0; Idx < Mir->Count; Idx++) {
            if (Mir->Blocks[Idx] == Block) break;
        }
        if (Idx >= Mir->Count) {
            continue;
        }
        for (i = Idx; (i + 1) < Mir->Count; i++) {
            Mir->Blocks[i] = Mir->Blocks[i + 1];
        }
        Mir->Count--;
        if (Mir->Count == 0) {
            Mir->Disabled = TRUE;
            Mir->CurIndex = 0;
            Mir->EnterNewBlock = TRUE;
        }
        else if (Mir->CurIndex > Idx) {
            Mir->CurIndex--;
        }
        else if (Mir->CurIndex == Idx) {
            Mir->CurIndex = (uint16)((Idx == 0) ? ((uint32)Mir->Count - 1) : (Idx - 1));
            Mir->EnterNewBlock = TRUE;
        }
    }

} /* End of EEFS_NandRemoveFromRing() */

/************************/
/*  End of File Comment */
/************************/
