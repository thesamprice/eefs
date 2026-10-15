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
 * Filename: nand_sim.h
 *
 * Purpose: RAM NAND simulator for the EEFS NAND tests (host, and the RTEMS QEMU test).  Implements EEFS_NandOps_t
 *   with fault injection, power-cut emulation and NAND programming-rule checks.  Test code: it uses calloc/free.
 *
 * Design Notes:
 *   Storage is allocated per block on the first program (or SetFactoryBad/RawWrite/CorruptBytes) and released by a
 *   successful erase, so an erased block reads 0xFF without any storage.  A 4096-block device with 512 KiB blocks
 *   therefore only costs RAM for the blocks that hold data.
 *
 *   Programming follows NAND rules: bits only go from 1 to 0 (the stored byte is ANDed with the new one).  Each page
 *   has a "programmed" flag, cleared by an erase and set by any program of the page.
 *
 *   Counters count calls (attempts), including calls that fail: Reads (every ReadPage), Programs (every ProgramPage)
 *   and Erases / BlockErases (every EraseBlock).
 *
 *   Strict mode (default on): programming a page whose programmed flag is set, programming page p > 0 while page p-1
 *   of the block is not programmed, or a Block/Page out of range makes the call fail (-1, or
 *   EEFS_NAND_READ_IO_ERROR for a read) and increments Violations.  An OOB-only program (Data == NULL, the
 *   best-effort bad-block marker) is exempt from both page rules.  Out-of-range calls fail in non-strict mode too.
 *
 *   Factory-bad blocks (SetFactoryBad): random data, 0x00 at BadMarkerOffset in the OOB of every BadMarkerPages page.
 *   ReadPage with Data != NULL returns EEFS_NAND_READ_UNCORRECTABLE (with the random bytes); with Data == NULL it
 *   returns 0 and the raw OOB, so a marker scan works.  Program and erase of a factory-bad block fail with -1.
 *
 *   Read injections: Status >= 0 is returned as the corrected bit count (the data is correct); a negative Status
 *   (EEFS_NAND_READ_UNCORRECTABLE or EEFS_NAND_READ_IO_ERROR) is returned with the data bytes XORed with 0x5A.
 *   Program and erase injections fail the call (-1) without changing the media.  Block/Page NANDSIM_ANY matches any
 *   block/page; Times -1 = forever, otherwise the injection is consumed by Times matching calls.
 *
 *   Power cut: after Budget more program+erase calls, the next one (the "torn" one) is applied according to TornMode,
 *   returns -1 and sets PowerCut; every later program/erase returns -1 without effect.  Reads keep working and do
 *   not consume budget.  TornMode:
 *     NANDSIM_TORN_NONE     the torn operation is not applied
 *     NANDSIM_TORN_PARTIAL  program: the first half of the page data is applied, the rest stays as it was (0xFF),
 *                           the OOB is not written; erase: the first half of the pages is erased
 *     NANDSIM_TORN_GARBAGE  program: every byte of the page (data and OOB, when given) is programmed XORed with 0xA5;
 *                           erase: every byte of the block is XORed with 0xA5 (an interrupted erase leaves the block
 *                           in an undefined state: neither the old content nor erased)
 *   A torn page is marked programmed.  NandSim_Reboot clears PowerCut, the budget and the injections.
 *
 * References:
 *   docs/nand-implementation.md section 11.
 */

#ifndef _nand_sim_
#define _nand_sim_

/*
 * Includes
 */

#include <stddef.h>
#include "common_types.h"
#include "eefs_nand_hal.h"

/*
 * Macro Definitions
 */

#define NANDSIM_TORN_NONE           0
#define NANDSIM_TORN_PARTIAL        1
#define NANDSIM_TORN_GARBAGE        2

#define NANDSIM_ANY                 0xFFFFFFFFUL    /* wildcard Block or Page for injections */
#define NANDSIM_MAX_INJECTIONS      32

/*
 * Type Definitions
 */

typedef struct NandSim NandSim_t;

/*
 * Exported Functions
 */

/* Creation.  Create returns NULL for an invalid geometry or when out of memory. */
NandSim_t  *NandSim_Create(const EEFS_NandGeometry_t *Geo);
NandSim_t  *NandSim_Clone(const NandSim_t *Sim);                    /* deep copy: content, flags and counters */
void        NandSim_Destroy(NandSim_t *Sim);
void        NandSim_FillDevice(NandSim_t *Sim, EEFS_NandDevice_t *Dev); /* Geometry copy, Ops, Ctx = Sim */
const EEFS_NandOps_t *NandSim_Ops(void);

/* Faults */
void        NandSim_SetFactoryBad(NandSim_t *Sim, uint32 Block);
void        NandSim_InjectRead(NandSim_t *Sim, uint32 Block, uint32 Page, int32 Status, int32 Times);
void        NandSim_InjectProgramFail(NandSim_t *Sim, uint32 Block, uint32 Page, int32 Times);
void        NandSim_InjectEraseFail(NandSim_t *Sim, uint32 Block, int32 Times);
void        NandSim_ClearInjections(NandSim_t *Sim);

/* Power cut */
void        NandSim_SetPowerBudget(NandSim_t *Sim, uint32 Budget, int TornMode);
int         NandSim_PowerCutHappened(const NandSim_t *Sim);
void        NandSim_Reboot(NandSim_t *Sim);

/* Inspection and direct manipulation (none of these count, consume budget or check rules) */
void        NandSim_CorruptBytes(NandSim_t *Sim, uint32 Block, uint32 Page, uint32 Offset, uint32 Len);  /* XOR 0x5A; Offset >= PageSize reaches the OOB */
void        NandSim_RawErase(NandSim_t *Sim, uint32 Block);        /* also clears a factory-bad mark */
void        NandSim_RawWrite(NandSim_t *Sim, uint32 Block, uint32 Page, const void *Data, const void *Oob);  /* overwrite (not AND); NULL areas untouched */
void        NandSim_RawRead(const NandSim_t *Sim, uint32 Block, uint32 Page, void *Data, void *Oob);         /* stored bytes, no status or injection */
int         NandSim_PageIsErased(const NandSim_t *Sim, uint32 Block, uint32 Page);  /* data and OOB all 0xFF */
int         NandSim_PageIsProgrammed(const NandSim_t *Sim, uint32 Block, uint32 Page);
uint32      NandSim_Reads(const NandSim_t *Sim);
uint32      NandSim_Programs(const NandSim_t *Sim);
uint32      NandSim_Erases(const NandSim_t *Sim);
uint32      NandSim_BlockErases(const NandSim_t *Sim, uint32 Block);
void        NandSim_ResetCounters(NandSim_t *Sim);                 /* Reads, Programs, Erases, BlockErases, Violations */
size_t      NandSim_AllocatedBytes(const NandSim_t *Sim);

/* Strictness */
void        NandSim_SetStrict(NandSim_t *Sim, int Strict);
uint32      NandSim_Violations(const NandSim_t *Sim);

#endif

/************************/
/*  End of File Comment */
/************************/
