/*
 * RTEMS smoke test for the EEFS NAND backend (EEFS_BACKEND=nand).
 *
 * Phases 2 and 3 (docs/nand-implementation.md section 14): checks, on the target, the CRC-32, the RAM NAND simulator,
 * a metadata page encode/decode round trip through the simulator, and the journaled directory: format and mount
 * through the standalone driver, then commit, unmount and remount through the library with a directory change that
 * must survive.  Then the file operations: files written through the driver, committed by the unmount, read back
 * after a remount, rewritten copy-on-write and renamed, and every committed file loaded byte for byte by the
 * bootloader library (microeefs_nand).  Phase 8 replaces it with the full test.
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

static int               Failures;
static uint8             Page[4096];
static EEFS_InodeTable_t Vol;           /* about 62 KB: static, not on the task stack */
static uint8             Image[600000];
static uint8             Image2[600000];
static uint8             Loaded[600000];

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

    /* format and mount through the standalone driver */
    CHECK(EEFS_Format("/NANDDEV0", (cpuaddr)&Dev) == EEFS_SUCCESS);
    CHECK(EEFS_Mount("/NANDDEV0", "/NAND0") == EEFS_SUCCESS);
    CHECK(EEFS_Commit("/NAND0") == EEFS_SUCCESS);          /* nothing to commit */
    CHECK(EEFS_Abort("/NAND0") == EEFS_SUCCESS);
    CHECK(EEFS_RepairDirectory("/NAND0") == EEFS_SUCCESS); /* fresh checkpoint in both mirrors */
    CHECK(EEFS_UnMount("/NAND0") == EEFS_SUCCESS);

    /* a directory change committed through the library survives an unmount and a remount */
    memset(&Vol, 0, sizeof(Vol));
    CHECK(EEFS_LibInitFS(&Vol, (cpuaddr)&Dev) == EEFS_SUCCESS);
    CHECK(Vol.LastSeq == 2);
    strcpy(Vol.Working.Entry[3].Path, "/cf/apps/sample_app.so");
    Vol.Working.Entry[3].InUse = 1;
    Vol.Working.Entry[3].FileType = EEFS_FILE_TYPE_CFS_APP;
    Vol.Working.Entry[3].Attributes = EEFS_ATTRIBUTE_READONLY;
    CHECK(EEFS_LibFreeFS(&Vol) == EEFS_TRANSACTION_ERROR);  /* uncommitted batch */
    CHECK(EEFS_LibCommit(&Vol) == EEFS_SUCCESS);
    CHECK(Vol.LastSeq == 3);
    CHECK(EEFS_LibFreeFS(&Vol) == EEFS_SUCCESS);
    CHECK(EEFS_LibInitFS(&Vol, (cpuaddr)&Dev) == EEFS_SUCCESS);
    CHECK(Vol.LastSeq == 3);
    CHECK(Vol.Working.Entry[3].InUse == 1);
    CHECK(strcmp(Vol.Working.Entry[3].Path, "/cf/apps/sample_app.so") == 0);
    CHECK(Vol.Working.Entry[3].Attributes == EEFS_ATTRIBUTE_READONLY);
    CHECK(Vol.Mirror[0].LastSeq == 3 && Vol.Mirror[1].LastSeq == 3);
    CHECK(EEFS_LibFreeFS(&Vol) == EEFS_SUCCESS);

    NandSim_Destroy(Sim);
}

static void make_image(uint8 *Buf, uint32 Size, uint32 Seed)
{
    uint32  i;

    for (i = 0; i < Size; i++) {
        Buf[i] = (uint8)(((i ^ (Seed * 0x01000193UL)) * 0x9E3779B1UL) >> 24);
    }
}

/* Reads Path back through the driver and compares it with Expected */
static void check_read(char *Path, const uint8 *Expected, uint32 Size)
{
    EEFS_Stat_t St;
    uint32      Total = 0;
    int32       Fd;
    int32       n;

    CHECK(EEFS_Stat(Path, &St) == EEFS_SUCCESS);
    CHECK(St.FileSize == Size);
    Fd = EEFS_Open(Path, O_RDONLY);
    CHECK(Fd >= 0);
    if (Fd < 0) return;
    while ((n = EEFS_Read(Fd, &Loaded[Total], 4097)) > 0) {
        Total += (uint32)n;
    }
    CHECK(Total == Size);
    CHECK(memcmp(Loaded, Expected, Size) == 0);
    CHECK(EEFS_Close(Fd) == EEFS_SUCCESS);
    CHECK(EEFS_VerifyFile(Path) == EEFS_SUCCESS);
}

/* The bootloader library loads Name from the device and gets exactly Expected */
static void check_micro(const EEFS_NandDevice_t *Dev, const char *Name, const uint8 *Expected, uint32 Size,
                        uint8 FileType, uint32 Version)
{
    MicroEEFS_NandInfo_t    Info;

    memset(Loaded, 0, sizeof(Loaded));
    CHECK(MicroEEFS_NandLoadFile(Dev, Page, Name, Loaded, sizeof(Loaded), &Info) == EEFS_SUCCESS);
    CHECK(Info.Info.FileSize == Size);
    CHECK(Info.Info.FileType == FileType);
    CHECK(Info.Info.Version == Version);
    CHECK(Info.UsedScan == 0);
    CHECK(memcmp(Loaded, Expected, Size) == 0);
}

static void check_nand_files(void)
{
    EEFS_NandGeometry_t     Geo;
    EEFS_NandDevice_t       Dev;
    EEFS_NandFileInfo_t     Meta;
    EEFS_NandVolumeInfo_t   VInfo;
    MicroEEFS_NandInfo_t    Info;
    NandSim_t              *Sim;
    uint32                  Size = (4096 * 128) + 1000;     /* two blocks: 127 data pages, then 5096 bytes */
    uint32                  Size2 = 3000;

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
    make_image(Image, Size, 1);
    make_image(Image2, Size2, 2);

    /* write through the driver; the unmount commits */
    CHECK(EEFS_Format("/NANDDEV1", (cpuaddr)&Dev) == EEFS_SUCCESS);
    CHECK(EEFS_Mount("/NANDDEV1", "/NAND1") == EEFS_SUCCESS);
    memset(&Meta, 0, sizeof(Meta));
    Meta.FileType = EEFS_FILE_TYPE_BOOT_ELF;
    Meta.Slot = EEFS_NAND_SLOT_NONE;
    Meta.Version = 7;
    Meta.Type.Elf.DestAddress = 0x10000000UL;
    CHECK(EEFS_WriteFile("/NAND1/boot/app.elf", &Meta, Image, Size) == EEFS_SUCCESS);
    Meta.FileType = EEFS_FILE_TYPE_OTHER;
    Meta.Version = 1;
    CHECK(EEFS_WriteFile("/NAND1/etc/cfg", &Meta, Image2, Size2) == EEFS_SUCCESS);
    CHECK(MicroEEFS_NandFindFile(&Dev, Page, "/boot/app.elf", &Info) == EEFS_FILE_NOT_FOUND);   /* not committed */
    check_read("/NAND1/boot/app.elf", Image, Size);
    check_read("/NAND1/etc/cfg", Image2, Size2);
    CHECK(EEFS_GetVolumeInfo("/NAND1", &VInfo) == EEFS_SUCCESS);
    CHECK(VInfo.FilesInUse == 2 && VInfo.UncommittedChanges == 1);
    CHECK(EEFS_UnMount("/NAND1") == EEFS_SUCCESS);
    check_micro(&Dev, "/boot/app.elf", Image, Size, EEFS_FILE_TYPE_BOOT_ELF, 7);
    check_micro(&Dev, "/etc/cfg", Image2, Size2, EEFS_FILE_TYPE_OTHER, 1);

    /* remount the library on the device, read back, then a copy-on-write rewrite and a rename */
    memset(&Vol, 0, sizeof(Vol));
    CHECK(EEFS_LibInitFS(&Vol, (cpuaddr)&Dev) == EEFS_SUCCESS);
    CHECK(EEFS_LibVerifyFile(&Vol, "/boot/app.elf") == EEFS_SUCCESS);
    {
        int32   Fd = EEFS_LibOpen(&Vol, "/etc/cfg", O_RDONLY, 0);
        CHECK(Fd >= 0);
        if (Fd >= 0) {
            CHECK(EEFS_LibRead(Fd, Loaded, sizeof(Loaded)) == (int32)Size2);
            CHECK(memcmp(Loaded, Image2, Size2) == 0);
            CHECK(EEFS_LibClose(Fd) == EEFS_SUCCESS);
        }
    }
    Image[Size - 5] ^= 0x5A;                    /* only the second block changes */
    Meta.FileType = EEFS_FILE_TYPE_BOOT_ELF;
    Meta.Version = 8;
    CHECK(EEFS_LibWriteFile(&Vol, "/boot/app.elf", &Meta, Image, Size) == EEFS_SUCCESS);
    CHECK(EEFS_LibRename(&Vol, "/etc/cfg", "/etc/cfg.old") == EEFS_SUCCESS);
    CHECK(MicroEEFS_NandFindFile(&Dev, Page, "/boot/app.elf", &Info) == EEFS_SUCCESS);
    CHECK(Info.Info.Version == 7);                  /* until the commit the loader sees the committed copy */
    CHECK(EEFS_LibCommit(&Vol) == EEFS_SUCCESS);
    CHECK(EEFS_LibFreeFS(&Vol) == EEFS_SUCCESS);
    check_micro(&Dev, "/boot/app.elf", Image, Size, EEFS_FILE_TYPE_BOOT_ELF, 8);
    check_micro(&Dev, "/etc/cfg.old", Image2, Size2, EEFS_FILE_TYPE_OTHER, 1);
    CHECK(MicroEEFS_NandFindFile(&Dev, Page, "/etc/cfg", &Info) == EEFS_FILE_NOT_FOUND);
    CHECK(NandSim_Violations(Sim) == 0);
    NandSim_Destroy(Sim);
}

static rtems_task Init(rtems_task_argument ignored)
{
    (void)ignored;

    printf("\nEEFS %d.%d.%d RTEMS NAND test\n", EEFS_MAJOR_VERSION, EEFS_MINOR_VERSION, EEFS_REVISION);

    EEFS_LibInit();
    CHECK(EEFS_Crc32("123456789", 9) == 0xCBF43926UL);
    check_nand_format();
    check_nand_files();

    printf(Failures == 0 ? "EEFS RTEMS TEST PASSED\n" : "EEFS RTEMS TEST FAILED (%d)\n", Failures);
    exit(Failures == 0 ? 0 : 1);
}

#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CONSOLE_DRIVER
#define CONFIGURE_MAXIMUM_TASKS            1
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_ATTRIBUTES     RTEMS_FLOATING_POINT
#define CONFIGURE_INIT_TASK_STACK_SIZE     (32 * 1024)
#define CONFIGURE_EXTRA_TASK_STACKS        (32 * 1024)
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
