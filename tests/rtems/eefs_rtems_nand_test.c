/*
 * RTEMS smoke test for the EEFS NAND backend (EEFS_BACKEND=nand).
 *
 * Phase 1 placeholder (docs/nand-implementation.md section 14): the file system calls are stubs, so this only
 * checks, on the target, the pieces that exist: the CRC-32, the RAM NAND simulator, and a metadata page
 * encode/decode round trip through the simulator.  Phase 8 replaces it with the full test.
 * Prints "EEFS RTEMS TEST PASSED" or "EEFS RTEMS TEST FAILED" and exits.
 */

#include <rtems.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_types.h"
#include "eefs_fileapi.h"
#include "eefs_filesys.h"
#include "eefs_nand.h"
#include "eefs_nand_format.h"
#include "microeefs_nand.h"
#include "nand_sim.h"

static int   Failures;
static uint8 Page[4096];

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            Failures++;                                                 \
        }                                                               \
    } while (0)

static void check_nand_format(void)
{
    EEFS_NandGeometry_t Geo;
    EEFS_NandDevice_t   Dev;
    EEFS_NandPage0_t    P;
    EEFS_NandPage0_t    D;
    NandSim_t          *Sim;
    uint16              Blocks[2] = { 40, 41 };
    uint32              Crcs[2] = { 0x11111111, 0x22222222 };

    memset(&Geo, 0, sizeof(Geo));
    Geo.PageSize = 4096;
    Geo.OobSize = 224;
    Geo.PagesPerBlock = 128;
    Geo.BlockCount = 256;
    Geo.DirRegionBlocks = 16;
    Geo.BadMarkerPages = EEFS_NAND_BADMARKER_PAGE0;

    Sim = NandSim_Create(&Geo);
    CHECK(Sim != NULL);
    if (Sim == NULL) return;
    NandSim_FillDevice(Sim, &Dev);

    memset(&P, 0, sizeof(P));
    P.WriteSeq = 7;
    strcpy(P.Path, "/cf/apps/sample_app.so");
    P.FileType = EEFS_FILE_TYPE_CFS_APP;
    P.Slot = EEFS_NAND_SLOT_B;
    P.FileSize = 600000;
    P.BlockCount = 2;
    strcpy(P.Type.App.AppName, "SAMPLE_APP");
    P.Type.App.LoadAddress = 0x100000;
    CHECK(EEFS_NandEncodePage0(Page, Geo.PageSize, &P, Blocks, Crcs) == EEFS_SUCCESS);
    CHECK(Dev.Ops->EraseBlock(Dev.Ctx, 40) == 0);
    CHECK(Dev.Ops->ProgramPage(Dev.Ctx, 40, 0, Page, NULL) == 0);
    memset(Page, 0, sizeof(Page));
    CHECK(Dev.Ops->ReadPage(Dev.Ctx, 40, 0, Page, NULL) == 0);
    CHECK(EEFS_NandDecodePage0(Page, Geo.PageSize, Geo.PagesPerBlock, 40, &D) == EEFS_SUCCESS);
    CHECK(strcmp(D.Path, P.Path) == 0);
    CHECK(D.FileSize == 600000);
    CHECK(D.Slot == EEFS_NAND_SLOT_B);
    CHECK(strcmp(D.Type.App.AppName, "SAMPLE_APP") == 0);
    CHECK(D.Type.App.LoadAddress == 0x100000);

    /* the file system itself is not implemented yet (phase 2 onwards) */
    CHECK(EEFS_Format("/NANDDEV0", (cpuaddr)&Dev) == EEFS_UNSUPPORTED_OPTION);

    NandSim_Destroy(Sim);
}

static rtems_task Init(rtems_task_argument ignored)
{
    (void)ignored;

    printf("\nEEFS %d.%d.%d RTEMS NAND test\n", EEFS_MAJOR_VERSION, EEFS_MINOR_VERSION, EEFS_REVISION);

    EEFS_LibInit();
    CHECK(EEFS_Crc32("123456789", 9) == 0xCBF43926UL);
    check_nand_format();

    printf(Failures == 0 ? "EEFS RTEMS TEST PASSED\n" : "EEFS RTEMS TEST FAILED (%d)\n", Failures);
    exit(Failures == 0 ? 0 : 1);
}

#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CONSOLE_DRIVER
#define CONFIGURE_MAXIMUM_TASKS            1
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_ATTRIBUTES     RTEMS_FLOATING_POINT
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
