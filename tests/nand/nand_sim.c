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
 * Filename: nand_sim.c
 *
 * Purpose: RAM NAND simulator for the EEFS NAND tests.  See nand_sim.h for the behaviour.
 *
 */

/*
 * Includes
 */

#include <stdlib.h>
#include <string.h>
#include "nand_sim.h"

/*
 * Macro Definitions
 */

#define NANDSIM_INJ_READ            1
#define NANDSIM_INJ_PROGRAM         2
#define NANDSIM_INJ_ERASE           3

#define NANDSIM_CORRUPT_XOR         0x5A
#define NANDSIM_GARBAGE_XOR         0xA5

/*
 * Type Definitions
 */

typedef struct
{
    int                     Kind;
    uint32                  Block;
    uint32                  Page;
    int32                   Status;
    int32                   Times;      /* -1 = forever */
} NandSimInjection_t;

struct NandSim
{
    EEFS_NandGeometry_t     Geo;
    uint32                  Stride;         /* PageSize + OobSize */
    size_t                  BlockBytes;     /* storage of one block: pages, then one programmed flag per page */
    uint8                 **Blocks;         /* NULL = erased, no storage */
    uint8                  *FactoryBad;
    uint32                 *BlockErases;
    uint32                  Reads, Programs, Erases, Violations;
    int                     Strict;
    int                     BudgetActive;
    uint32                  Budget;
    int                     TornMode;
    int                     PowerCut;
    NandSimInjection_t      Inj[NANDSIM_MAX_INJECTIONS];
    uint32                  InjCount;
    size_t                  Allocated;
    uint32                  Random;
};

/*
 * Local Function Prototypes
 */

static int32    NandSim_ReadPage(void *Ctx, uint32 Block, uint32 Page, void *Data, void *Oob);
static int32    NandSim_ProgramPage(void *Ctx, uint32 Block, uint32 Page, const void *Data, const void *Oob);
static int32    NandSim_EraseBlock(void *Ctx, uint32 Block);
static uint8   *NandSim_Storage(NandSim_t *Sim, uint32 Block);
static void     NandSim_FreeStorage(NandSim_t *Sim, uint32 Block);
static uint8   *NandSim_PagePtr(const NandSim_t *Sim, uint8 *Storage, uint32 Page);
static uint8   *NandSim_FlagPtr(const NandSim_t *Sim, uint8 *Storage, uint32 Page);
static int      NandSim_InRange(const NandSim_t *Sim, uint32 Block, uint32 Page);
static int      NandSim_TakeInjection(NandSim_t *Sim, int Kind, uint32 Block, uint32 Page, int32 *Status);
static void     NandSim_AddInjection(NandSim_t *Sim, int Kind, uint32 Block, uint32 Page, int32 Status, int32 Times);
static int      NandSim_ConsumeBudget(NandSim_t *Sim);
static uint8    NandSim_NextRandom(NandSim_t *Sim);

/*
 * Local Data
 */

static const EEFS_NandOps_t NandSim_OpsTable = {
    NandSim_ReadPage,
    NandSim_ProgramPage,
    NandSim_EraseBlock
};

/*
 * Function Definitions
 */

NandSim_t *NandSim_Create(const EEFS_NandGeometry_t *Geo)
{
    NandSim_t  *Sim;

    if ((Geo == NULL) || (Geo->PageSize == 0) || (Geo->OobSize == 0) || (Geo->PagesPerBlock == 0) ||
        (Geo->BlockCount == 0)) {
        return(NULL);
    }

    Sim = (NandSim_t *)calloc(1, sizeof(NandSim_t));
    if (Sim == NULL) {
        return(NULL);
    }
    Sim->Geo = *Geo;
    Sim->Stride = Geo->PageSize + Geo->OobSize;
    Sim->BlockBytes = ((size_t)Sim->Stride + 1) * Geo->PagesPerBlock;
    Sim->Blocks = (uint8 **)calloc(Geo->BlockCount, sizeof(uint8 *));
    Sim->FactoryBad = (uint8 *)calloc(Geo->BlockCount, 1);
    Sim->BlockErases = (uint32 *)calloc(Geo->BlockCount, sizeof(uint32));
    if ((Sim->Blocks == NULL) || (Sim->FactoryBad == NULL) || (Sim->BlockErases == NULL)) {
        NandSim_Destroy(Sim);
        return(NULL);
    }
    Sim->Allocated = sizeof(NandSim_t) + (Geo->BlockCount * (sizeof(uint8 *) + 1 + sizeof(uint32)));
    Sim->Strict = 1;
    Sim->Random = 0x12345678UL;
    return(Sim);
}

NandSim_t *NandSim_Clone(const NandSim_t *Sim)
{
    NandSim_t  *Copy;
    uint32      b;

    if ((Copy = NandSim_Create(&Sim->Geo)) == NULL) {
        return(NULL);
    }
    memcpy(Copy->FactoryBad, Sim->FactoryBad, Sim->Geo.BlockCount);
    memcpy(Copy->BlockErases, Sim->BlockErases, Sim->Geo.BlockCount * sizeof(uint32));
    for (b = 0; b < Sim->Geo.BlockCount; b++) {
        if (Sim->Blocks[b] != NULL) {
            if (NandSim_Storage(Copy, b) == NULL) {
                NandSim_Destroy(Copy);
                return(NULL);
            }
            memcpy(Copy->Blocks[b], Sim->Blocks[b], Sim->BlockBytes);
        }
    }
    Copy->Reads = Sim->Reads;
    Copy->Programs = Sim->Programs;
    Copy->Erases = Sim->Erases;
    Copy->Violations = Sim->Violations;
    Copy->Strict = Sim->Strict;
    Copy->BudgetActive = Sim->BudgetActive;
    Copy->Budget = Sim->Budget;
    Copy->TornMode = Sim->TornMode;
    Copy->PowerCut = Sim->PowerCut;
    memcpy(Copy->Inj, Sim->Inj, sizeof(Sim->Inj));
    Copy->InjCount = Sim->InjCount;
    Copy->Random = Sim->Random;
    return(Copy);
}

void NandSim_Destroy(NandSim_t *Sim)
{
    uint32  b;

    if (Sim == NULL) {
        return;
    }
    if (Sim->Blocks != NULL) {
        for (b = 0; b < Sim->Geo.BlockCount; b++) {
            free(Sim->Blocks[b]);
        }
    }
    free(Sim->Blocks);
    free(Sim->FactoryBad);
    free(Sim->BlockErases);
    free(Sim);
}

void NandSim_FillDevice(NandSim_t *Sim, EEFS_NandDevice_t *Dev)
{
    Dev->Geometry = Sim->Geo;
    Dev->Ops = &NandSim_OpsTable;
    Dev->Ctx = Sim;
}

const EEFS_NandOps_t *NandSim_Ops(void)
{
    return(&NandSim_OpsTable);
}

/* Faults */

void NandSim_SetFactoryBad(NandSim_t *Sim, uint32 Block)
{
    uint8  *Storage;
    uint32  p;
    uint32  i;

    if ((NandSim_InRange(Sim, Block, 0) == 0) || ((Storage = NandSim_Storage(Sim, Block)) == NULL)) {
        return;
    }
    for (p = 0; p < Sim->Geo.PagesPerBlock; p++) {
        for (i = 0; i < Sim->Stride; i++) {
            NandSim_PagePtr(Sim, Storage, p)[i] = NandSim_NextRandom(Sim);
        }
        *NandSim_FlagPtr(Sim, Storage, p) = 1;
    }
    if (Sim->Geo.BadMarkerOffset < Sim->Geo.OobSize) {
        for (p = 0; p < Sim->Geo.PagesPerBlock; p++) {
            if (((p == 0) && (Sim->Geo.BadMarkerPages & EEFS_NAND_BADMARKER_PAGE0)) ||
                ((p == 1) && (Sim->Geo.BadMarkerPages & EEFS_NAND_BADMARKER_PAGE1)) ||
                ((p == Sim->Geo.PagesPerBlock - 1) && (Sim->Geo.BadMarkerPages & EEFS_NAND_BADMARKER_LAST))) {
                NandSim_PagePtr(Sim, Storage, p)[Sim->Geo.PageSize + Sim->Geo.BadMarkerOffset] = 0x00;
            }
        }
    }
    Sim->FactoryBad[Block] = 1;
}

void NandSim_InjectRead(NandSim_t *Sim, uint32 Block, uint32 Page, int32 Status, int32 Times)
{
    NandSim_AddInjection(Sim, NANDSIM_INJ_READ, Block, Page, Status, Times);
}

void NandSim_InjectProgramFail(NandSim_t *Sim, uint32 Block, uint32 Page, int32 Times)
{
    NandSim_AddInjection(Sim, NANDSIM_INJ_PROGRAM, Block, Page, -1, Times);
}

void NandSim_InjectEraseFail(NandSim_t *Sim, uint32 Block, int32 Times)
{
    NandSim_AddInjection(Sim, NANDSIM_INJ_ERASE, Block, NANDSIM_ANY, -1, Times);
}

void NandSim_ClearInjections(NandSim_t *Sim)
{
    Sim->InjCount = 0;
}

/* Power cut */

void NandSim_SetPowerBudget(NandSim_t *Sim, uint32 Budget, int TornMode)
{
    Sim->BudgetActive = 1;
    Sim->Budget = Budget;
    Sim->TornMode = TornMode;
}

int NandSim_PowerCutHappened(const NandSim_t *Sim)
{
    return(Sim->PowerCut);
}

void NandSim_Reboot(NandSim_t *Sim)
{
    Sim->PowerCut = 0;
    Sim->BudgetActive = 0;
    Sim->Budget = 0;
    Sim->TornMode = NANDSIM_TORN_NONE;
    Sim->InjCount = 0;
}

/* Inspection and direct manipulation */

void NandSim_CorruptBytes(NandSim_t *Sim, uint32 Block, uint32 Page, uint32 Offset, uint32 Len)
{
    uint8  *Storage;
    uint8  *P;
    uint32  i;

    if ((NandSim_InRange(Sim, Block, Page) == 0) || ((Storage = NandSim_Storage(Sim, Block)) == NULL)) {
        return;
    }
    P = NandSim_PagePtr(Sim, Storage, Page);
    for (i = Offset; (i < Offset + Len) && (i < Sim->Stride); i++) {
        P[i] ^= NANDSIM_CORRUPT_XOR;
    }
    *NandSim_FlagPtr(Sim, Storage, Page) = 1;
}

void NandSim_RawErase(NandSim_t *Sim, uint32 Block)
{
    if (NandSim_InRange(Sim, Block, 0) == 0) {
        return;
    }
    NandSim_FreeStorage(Sim, Block);
    Sim->FactoryBad[Block] = 0;
}

void NandSim_RawWrite(NandSim_t *Sim, uint32 Block, uint32 Page, const void *Data, const void *Oob)
{
    uint8  *Storage;
    uint8  *P;

    if ((NandSim_InRange(Sim, Block, Page) == 0) || ((Storage = NandSim_Storage(Sim, Block)) == NULL)) {
        return;
    }
    P = NandSim_PagePtr(Sim, Storage, Page);
    if (Data != NULL) {
        memcpy(P, Data, Sim->Geo.PageSize);
    }
    if (Oob != NULL) {
        memcpy(P + Sim->Geo.PageSize, Oob, Sim->Geo.OobSize);
    }
    *NandSim_FlagPtr(Sim, Storage, Page) = 1;
}

void NandSim_RawRead(const NandSim_t *Sim, uint32 Block, uint32 Page, void *Data, void *Oob)
{
    uint8  *P;

    if (NandSim_InRange(Sim, Block, Page) == 0) {
        return;
    }
    if (Sim->Blocks[Block] == NULL) {
        if (Data != NULL) memset(Data, 0xFF, Sim->Geo.PageSize);
        if (Oob != NULL) memset(Oob, 0xFF, Sim->Geo.OobSize);
        return;
    }
    P = NandSim_PagePtr(Sim, Sim->Blocks[Block], Page);
    if (Data != NULL) memcpy(Data, P, Sim->Geo.PageSize);
    if (Oob != NULL) memcpy(Oob, P + Sim->Geo.PageSize, Sim->Geo.OobSize);
}

int NandSim_PageIsErased(const NandSim_t *Sim, uint32 Block, uint32 Page)
{
    const uint8 *P;
    uint32       i;

    if (NandSim_InRange(Sim, Block, Page) == 0) {
        return(0);
    }
    if (Sim->Blocks[Block] == NULL) {
        return(1);
    }
    P = NandSim_PagePtr(Sim, Sim->Blocks[Block], Page);
    for (i = 0; i < Sim->Stride; i++) {
        if (P[i] != 0xFF) {
            return(0);
        }
    }
    return(1);
}

int NandSim_PageIsProgrammed(const NandSim_t *Sim, uint32 Block, uint32 Page)
{
    if ((NandSim_InRange(Sim, Block, Page) == 0) || (Sim->Blocks[Block] == NULL)) {
        return(0);
    }
    return(*NandSim_FlagPtr(Sim, Sim->Blocks[Block], Page) != 0);
}

uint32 NandSim_Reads(const NandSim_t *Sim)
{
    return(Sim->Reads);
}

uint32 NandSim_Programs(const NandSim_t *Sim)
{
    return(Sim->Programs);
}

uint32 NandSim_Erases(const NandSim_t *Sim)
{
    return(Sim->Erases);
}

uint32 NandSim_BlockErases(const NandSim_t *Sim, uint32 Block)
{
    if (Block >= Sim->Geo.BlockCount) {
        return(0);
    }
    return(Sim->BlockErases[Block]);
}

void NandSim_ResetCounters(NandSim_t *Sim)
{
    Sim->Reads = 0;
    Sim->Programs = 0;
    Sim->Erases = 0;
    Sim->Violations = 0;
    memset(Sim->BlockErases, 0, Sim->Geo.BlockCount * sizeof(uint32));
}

size_t NandSim_AllocatedBytes(const NandSim_t *Sim)
{
    return(Sim->Allocated);
}

void NandSim_SetStrict(NandSim_t *Sim, int Strict)
{
    Sim->Strict = Strict;
}

uint32 NandSim_Violations(const NandSim_t *Sim)
{
    return(Sim->Violations);
}

/*
 * HAL operations
 */

static int32 NandSim_ReadPage(void *Ctx, uint32 Block, uint32 Page, void *Data, void *Oob)
{
    NandSim_t  *Sim = (NandSim_t *)Ctx;
    int32       Status = 0;
    uint8      *Bytes;
    uint32      i;

    Sim->Reads++;
    if (NandSim_InRange(Sim, Block, Page) == 0) {
        if (Sim->Strict) Sim->Violations++;
        return(EEFS_NAND_READ_IO_ERROR);
    }

    NandSim_RawRead(Sim, Block, Page, Data, Oob);

    if (Sim->FactoryBad[Block]) {
        return((Data != NULL) ? EEFS_NAND_READ_UNCORRECTABLE : 0);
    }

    if (NandSim_TakeInjection(Sim, NANDSIM_INJ_READ, Block, Page, &Status) && (Status < 0) && (Data != NULL)) {
        Bytes = (uint8 *)Data;
        for (i = 0; i < Sim->Geo.PageSize; i++) {
            Bytes[i] ^= NANDSIM_CORRUPT_XOR;
        }
    }
    return(Status);
}

static int32 NandSim_ProgramPage(void *Ctx, uint32 Block, uint32 Page, const void *Data, const void *Oob)
{
    NandSim_t      *Sim = (NandSim_t *)Ctx;
    const uint8    *D = (const uint8 *)Data;
    const uint8    *O = (const uint8 *)Oob;
    uint8          *Storage;
    uint8          *P;
    uint32          Count;
    uint32          i;
    int32           Status;

    Sim->Programs++;
    if (Sim->PowerCut) {
        return(-1);
    }
    if (NandSim_InRange(Sim, Block, Page) == 0) {
        if (Sim->Strict) Sim->Violations++;
        return(-1);
    }

    if (NandSim_ConsumeBudget(Sim) == 0) {
        /* the torn operation */
        Sim->PowerCut = 1;
        if ((Sim->TornMode != NANDSIM_TORN_NONE) && (Sim->FactoryBad[Block] == 0) &&
            ((Storage = NandSim_Storage(Sim, Block)) != NULL)) {
            P = NandSim_PagePtr(Sim, Storage, Page);
            if (Sim->TornMode == NANDSIM_TORN_PARTIAL) {
                Count = Sim->Geo.PageSize / 2;
                for (i = 0; (D != NULL) && (i < Count); i++) {
                    P[i] &= D[i];
                }
            }
            else {
                for (i = 0; (D != NULL) && (i < Sim->Geo.PageSize); i++) {
                    P[i] &= (uint8)(D[i] ^ NANDSIM_GARBAGE_XOR);
                }
                for (i = 0; (O != NULL) && (i < Sim->Geo.OobSize); i++) {
                    P[Sim->Geo.PageSize + i] &= (uint8)(O[i] ^ NANDSIM_GARBAGE_XOR);
                }
            }
            *NandSim_FlagPtr(Sim, Storage, Page) = 1;
        }
        return(-1);
    }

    if (Sim->FactoryBad[Block]) {
        return(-1);
    }

    if ((D != NULL) && Sim->Strict) {
        if (NandSim_PageIsProgrammed(Sim, Block, Page) ||
            ((Page > 0) && (NandSim_PageIsProgrammed(Sim, Block, Page - 1) == 0))) {
            Sim->Violations++;
            return(-1);
        }
    }

    if (NandSim_TakeInjection(Sim, NANDSIM_INJ_PROGRAM, Block, Page, &Status)) {
        return(-1);
    }

    if ((D == NULL) && (O == NULL)) {
        return(0);
    }
    if ((Storage = NandSim_Storage(Sim, Block)) == NULL) {
        return(-1);
    }
    P = NandSim_PagePtr(Sim, Storage, Page);
    for (i = 0; (D != NULL) && (i < Sim->Geo.PageSize); i++) {
        P[i] &= D[i];
    }
    for (i = 0; (O != NULL) && (i < Sim->Geo.OobSize); i++) {
        P[Sim->Geo.PageSize + i] &= O[i];
    }
    *NandSim_FlagPtr(Sim, Storage, Page) = 1;
    return(0);
}

static int32 NandSim_EraseBlock(void *Ctx, uint32 Block)
{
    NandSim_t  *Sim = (NandSim_t *)Ctx;
    uint8      *Storage;
    uint32      p;
    uint32      i;
    int32       Status;

    Sim->Erases++;
    if (Block < Sim->Geo.BlockCount) {
        Sim->BlockErases[Block]++;
    }
    if (Sim->PowerCut) {
        return(-1);
    }
    if (NandSim_InRange(Sim, Block, 0) == 0) {
        if (Sim->Strict) Sim->Violations++;
        return(-1);
    }

    if (NandSim_ConsumeBudget(Sim) == 0) {
        /* the torn operation */
        Sim->PowerCut = 1;
        if ((Sim->FactoryBad[Block] == 0) && (Sim->Blocks[Block] != NULL)) {
            Storage = Sim->Blocks[Block];
            if (Sim->TornMode == NANDSIM_TORN_PARTIAL) {
                for (p = 0; p < Sim->Geo.PagesPerBlock / 2; p++) {
                    memset(NandSim_PagePtr(Sim, Storage, p), 0xFF, Sim->Stride);
                    *NandSim_FlagPtr(Sim, Storage, p) = 0;
                }
            }
            else if (Sim->TornMode == NANDSIM_TORN_GARBAGE) {
                for (p = 0; p < Sim->Geo.PagesPerBlock; p++) {
                    for (i = 0; i < Sim->Stride; i++) {
                        NandSim_PagePtr(Sim, Storage, p)[i] ^= NANDSIM_GARBAGE_XOR;
                    }
                    *NandSim_FlagPtr(Sim, Storage, p) = 1;
                }
            }
        }
        else if ((Sim->FactoryBad[Block] == 0) && (Sim->TornMode == NANDSIM_TORN_GARBAGE) &&
                 ((Storage = NandSim_Storage(Sim, Block)) != NULL)) {
            /* an erased block whose erase is interrupted is left undefined too */
            for (p = 0; p < Sim->Geo.PagesPerBlock; p++) {
                for (i = 0; i < Sim->Stride; i++) {
                    NandSim_PagePtr(Sim, Storage, p)[i] ^= NANDSIM_GARBAGE_XOR;
                }
                *NandSim_FlagPtr(Sim, Storage, p) = 1;
            }
        }
        return(-1);
    }

    if (Sim->FactoryBad[Block]) {
        return(-1);
    }
    if (NandSim_TakeInjection(Sim, NANDSIM_INJ_ERASE, Block, NANDSIM_ANY, &Status)) {
        return(-1);
    }

    NandSim_FreeStorage(Sim, Block);
    return(0);
}

/*
 * Local Functions
 */

/* Returns the storage of a block, allocating it (erased) when needed */
static uint8 *NandSim_Storage(NandSim_t *Sim, uint32 Block)
{
    uint8  *Storage;

    if (Sim->Blocks[Block] == NULL) {
        Storage = (uint8 *)malloc(Sim->BlockBytes);
        if (Storage == NULL) {
            return(NULL);
        }
        memset(Storage, 0xFF, Sim->Stride * Sim->Geo.PagesPerBlock);
        memset(Storage + ((size_t)Sim->Stride * Sim->Geo.PagesPerBlock), 0, Sim->Geo.PagesPerBlock);
        Sim->Blocks[Block] = Storage;
        Sim->Allocated += Sim->BlockBytes;
    }
    return(Sim->Blocks[Block]);
}

static void NandSim_FreeStorage(NandSim_t *Sim, uint32 Block)
{
    if (Sim->Blocks[Block] != NULL) {
        free(Sim->Blocks[Block]);
        Sim->Blocks[Block] = NULL;
        Sim->Allocated -= Sim->BlockBytes;
    }
}

static uint8 *NandSim_PagePtr(const NandSim_t *Sim, uint8 *Storage, uint32 Page)
{
    return(Storage + ((size_t)Page * Sim->Stride));
}

static uint8 *NandSim_FlagPtr(const NandSim_t *Sim, uint8 *Storage, uint32 Page)
{
    return(Storage + ((size_t)Sim->Stride * Sim->Geo.PagesPerBlock) + Page);
}

static int NandSim_InRange(const NandSim_t *Sim, uint32 Block, uint32 Page)
{
    return((Block < Sim->Geo.BlockCount) && (Page < Sim->Geo.PagesPerBlock));
}

/* Finds a matching injection, consumes one use of it and returns 1 (with its Status), or returns 0 */
static int NandSim_TakeInjection(NandSim_t *Sim, int Kind, uint32 Block, uint32 Page, int32 *Status)
{
    NandSimInjection_t *Inj;
    uint32              i;

    for (i = 0; i < Sim->InjCount; i++) {
        Inj = &Sim->Inj[i];
        if ((Inj->Kind == Kind) && (Inj->Times != 0) &&
            ((Inj->Block == NANDSIM_ANY) || (Inj->Block == Block)) &&
            ((Inj->Page == NANDSIM_ANY) || (Inj->Page == Page))) {
            if (Inj->Times > 0) {
                Inj->Times--;
            }
            *Status = Inj->Status;
            return(1);
        }
    }
    return(0);
}

static void NandSim_AddInjection(NandSim_t *Sim, int Kind, uint32 Block, uint32 Page, int32 Status, int32 Times)
{
    NandSimInjection_t *Inj;

    if ((Sim->InjCount >= NANDSIM_MAX_INJECTIONS) || (Times == 0)) {
        return;
    }
    Inj = &Sim->Inj[Sim->InjCount++];
    Inj->Kind = Kind;
    Inj->Block = Block;
    Inj->Page = Page;
    Inj->Status = Status;
    Inj->Times = Times;
}

/* Returns 1 if the operation may proceed normally, 0 if it is the torn one */
static int NandSim_ConsumeBudget(NandSim_t *Sim)
{
    if (Sim->BudgetActive == 0) {
        return(1);
    }
    if (Sim->Budget == 0) {
        return(0);
    }
    Sim->Budget--;
    return(1);
}

static uint8 NandSim_NextRandom(NandSim_t *Sim)
{
    Sim->Random = (Sim->Random * 1103515245UL) + 12345UL;
    return((uint8)(Sim->Random >> 16));
}

/************************/
/*  End of File Comment */
/************************/
