/*
 * RTEMS smoke test for the EEFS NAND backend (EEFS_BACKEND=nand).
 *
 * Phases 2 and 3 (docs/nand-implementation.md section 14): checks, on the target, the CRC-32, the RAM NAND simulator,
 * a metadata page encode/decode round trip through the simulator, and the journaled directory: format and mount
 * through the standalone driver, then commit, unmount and remount through the library with a directory change that
 * must survive.  Then the file operations: files written through the driver, committed by the unmount, read back
 * after a remount, rewritten copy-on-write and renamed, and every committed file loaded byte for byte by the
 * bootloader library (microeefs_nand).  Phase 5: an A/B upload, activation and compatible fallback through the driver,
 * checked against the bootloader library.  Phase 6: the health check runs the way it is meant to be deployed, in a
 * low-priority task that takes small steps and sleeps between them, while a higher-priority task writes and commits
 * files; the library lock (EEFS_LIB_LOCK, routed to an RTEMS mutex by EEFS_LIB_LOCK_HOOKS) is held only inside each
 * step, every change restarts the check, and it finishes with the expected report.  Phase 8 replaces it with the full
 * test.  Prints "EEFS RTEMS TEST PASSED" or "EEFS RTEMS TEST FAILED" and exits.
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

/* A/B (phase 5): v2 uploaded into B through the driver, B activated with A as the compatible fallback in one commit;
 * B's metadata page destroyed: Open and the bootloader both fall back to A and report ActiveFailed; a rollback to A
 * clears it; CopySlotAll promotes A's copy into B. */
static void check_nand_ab(void)
{
    EEFS_NandGeometry_t     Geo;
    static EEFS_NandDevice_t Dev;   /* not on the stack: its address must differ from the other devices' */
    EEFS_NandFileInfo_t     Meta;
    EEFS_NandFileInfo_t     FInfo;
    MicroEEFS_NandInfo_t    Info;
    NandSim_t              *Sim;
    uint32                  Size1 = 70000;
    uint32                  Size2 = 90001;

    memset(&Geo, 0, sizeof(Geo));
    Geo.PageSize = 4096;
    Geo.OobSize = 224;
    Geo.PagesPerBlock = 128;
    Geo.BlockCount = 256;
    Geo.DirRegionBlocks = 16;
    Geo.BadMarkerPages = EEFS_NAND_BADMARKER_PAGE0;
    /* the driver has two device slots: the device of check_nand_files is formatted again */
    Sim = NandSim_Create(&Geo);
    CHECK(Sim != NULL);
    if (Sim == NULL) return;
    NandSim_FillDevice(Sim, &Dev);
    make_image(Image, Size1, 11);
    make_image(Image2, Size2, 12);

    CHECK(EEFS_Format("/NANDDEV1", (cpuaddr)&Dev) == EEFS_SUCCESS);
    CHECK(EEFS_Mount("/NANDDEV1", "/NAND1") == EEFS_SUCCESS);
    memset(&Meta, 0, sizeof(Meta));
    Meta.FileType = EEFS_FILE_TYPE_BOOT_FIT;
    Meta.Slot = EEFS_NAND_SLOT_NONE;
    Meta.Version = 1;
    CHECK(EEFS_WriteFile("/NAND1/boot/app.fit", &Meta, Image, Size1) == EEFS_SUCCESS);
    CHECK(EEFS_Commit("/NAND1") == EEFS_SUCCESS);

    /* upload v2 into B, activate it with v1 as the compatible fallback, one commit */
    Meta.Slot = EEFS_NAND_SLOT_B;
    Meta.Version = 2;
    CHECK(EEFS_WriteFile("/NAND1/boot/app.fit", &Meta, Image2, Size2) == EEFS_SUCCESS);
    CHECK(EEFS_SetActiveSlotAll("/NAND1", EEFS_NAND_SLOT_B) == 1);
    CHECK(EEFS_SetFallbackCompatibleAll("/NAND1", 1) == 1);
    check_micro(&Dev, "/boot/app.fit", Image, Size1, EEFS_FILE_TYPE_BOOT_FIT, 1);   /* not committed yet */
    CHECK(EEFS_Commit("/NAND1") == EEFS_SUCCESS);
    check_read("/NAND1/boot/app.fit", Image2, Size2);
    check_read("/NAND1/boot/app.fit@A", Image, Size1);
    check_micro(&Dev, "/boot/app.fit", Image2, Size2, EEFS_FILE_TYPE_BOOT_FIT, 2);
    CHECK(EEFS_GetFileInfo("/NAND1/boot/app.fit", &FInfo) == EEFS_SUCCESS);
    CHECK(FInfo.IsAB == 1 && FInfo.ActiveSlot == EEFS_NAND_SLOT_B && FInfo.FallbackCompatible == 1);
    CHECK(FInfo.SlotInfo[0].Version == 1 && FInfo.SlotInfo[1].Version == 2);

    /* B's metadata page destroyed: both the library and the bootloader fall back to A and report ActiveFailed */
    NandSim_CorruptBytes(Sim, FInfo.SlotInfo[1].FirstBlock, 0, 20, 4);
    CHECK(EEFS_UnMount("/NAND1") == EEFS_SUCCESS);
    CHECK(EEFS_Mount("/NANDDEV1", "/NAND1") == EEFS_SUCCESS);
    check_read("/NAND1/boot/app.fit@A", Image, Size1);
    {
        EEFS_Stat_t St;
        int32       Fd = EEFS_Open("/NAND1/boot/app.fit", O_RDONLY);
        int32       n;
        uint32      Total = 0;

        CHECK(Fd >= 0);
        if (Fd >= 0) {
            while ((n = EEFS_Read(Fd, &Loaded[Total], 4097)) > 0) Total += (uint32)n;
            CHECK(Total == Size1);
            CHECK(memcmp(Loaded, Image, Size1) == 0);
            CHECK(EEFS_Close(Fd) == EEFS_SUCCESS);
        }
        CHECK(EEFS_Stat("/NAND1/boot/app.fit", &St) == EEFS_SUCCESS);
        CHECK(St.FileSize == Size2);        /* Stat reports the active slot and never falls back */
    }
    CHECK(EEFS_GetFileInfo("/NAND1/boot/app.fit@A", &FInfo) == EEFS_SUCCESS);
    CHECK(FInfo.ActiveFailed == 1);
    memset(Loaded, 0, sizeof(Loaded));
    CHECK(MicroEEFS_NandLoadFile(&Dev, Page, "/boot/app.fit", Loaded, sizeof(Loaded), &Info) == EEFS_SUCCESS);
    CHECK(Info.LoadedSlot == EEFS_NAND_SLOT_A && Info.Info.ActiveFailed == 1);
    CHECK(memcmp(Loaded, Image, Size1) == 0);

    /* roll back to A (clears ActiveFailed), then promote A's copy into B */
    CHECK(EEFS_SetActiveSlot("/NAND1/boot/app.fit", EEFS_NAND_SLOT_A) == EEFS_SUCCESS);
    CHECK(EEFS_CopySlotAll("/NAND1", EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_B) == 1);
    CHECK(EEFS_UnMount("/NAND1") == EEFS_SUCCESS);
    check_micro(&Dev, "/boot/app.fit", Image, Size1, EEFS_FILE_TYPE_BOOT_FIT, 1);
    check_micro(&Dev, "/boot/app.fit@B", Image, Size1, EEFS_FILE_TYPE_BOOT_FIT, 1);
    CHECK(MicroEEFS_NandFindFile(&Dev, Page, "/boot/app.fit", &Info) == EEFS_SUCCESS);
    CHECK(Info.Info.ActiveFailed == 0 && Info.Info.ActiveSlot == EEFS_NAND_SLOT_A);
    CHECK(NandSim_Violations(Sim) == 0);
    NandSim_Destroy(Sim);
}

/*
 * Phase 6: the background health check
 */

#define BG_WRITES       3
#define BG_STEP_READS   4
#define BG_TRIGGER_READS 150

static rtems_id                 LockId;             /* the library lock (a recursive, priority-inheriting mutex) */
static rtems_id                 DoneSem;
static rtems_id                 WriterGo;
static rtems_id                 HealthId;
static volatile uint32          HealthReads;        /* page reads made by the health task */
static volatile uint32          Triggers;
static volatile uint32          WaitsBefore[BG_WRITES];
static volatile uint32          WaitsAfterWrite[BG_WRITES];
static volatile uint32          LockContended;      /* times a task had to wait for the library lock */
static volatile uint32          WriterCommits;
static volatile uint32          HealthSteps;
static volatile uint32          WriterDoneAtHealthEnd;
static volatile int32           HealthRc;
static EEFS_HealthReport_t      HealthRep;          /* the caller's report: static, about 3.5 KB */
static EEFS_HealthSummary_t     HealthSum;

/* EEFS_LIB_LOCK / EEFS_LIB_UNLOCK of the library built with EEFS_LIB_LOCK_HOOKS */
void EEFS_LibLockHook(void)
{
    if (LockId == 0) return;
    if (rtems_semaphore_obtain(LockId, RTEMS_NO_WAIT, 0) == RTEMS_UNSATISFIED) {
        LockContended++;
        (void)rtems_semaphore_obtain(LockId, RTEMS_WAIT, RTEMS_NO_TIMEOUT);
    }
}

void EEFS_LibUnlockHook(void)
{
    if (LockId == 0) return;
    (void)rtems_semaphore_release(LockId);
}

/* The device seen by the library: the simulator, except that a page read made by the health task releases the
 * writer every BG_TRIGGER_READS reads (BG_WRITES times).  The writer has the higher priority, so it runs at once,
 * inside the health check's step, while the health task holds the library lock: its write must wait for the lock
 * (priority inheritance lets the step finish), and the check restarts at its next step. */
static int32 HookRead(void *Ctx, uint32 Block, uint32 Page, void *Data, void *Oob)
{
    rtems_id    Self;

    if ((HealthId != 0) && (rtems_task_ident(RTEMS_WHO_AM_I, RTEMS_SEARCH_LOCAL_NODE, &Self) == RTEMS_SUCCESSFUL) &&
        (Self == HealthId) && (Triggers < BG_WRITES) && ((++HealthReads % BG_TRIGGER_READS) == 0)) {
        Triggers++;
        (void)rtems_semaphore_release(WriterGo);
    }
    return(NandSim_Ops()->ReadPage(Ctx, Block, Page, Data, Oob));
}

static int32 HookProgram(void *Ctx, uint32 Block, uint32 Page, const void *Data, const void *Oob)
{
    return(NandSim_Ops()->ProgramPage(Ctx, Block, Page, Data, Oob));
}

static int32 HookErase(void *Ctx, uint32 Block)
{
    return(NandSim_Ops()->EraseBlock(Ctx, Block));
}

static const EEFS_NandOps_t HookOps = { HookRead, HookProgram, HookErase };

/* Low priority: start the check and take small steps until it is done, sleeping one tick between steps */
static rtems_task HealthTask(rtems_task_argument Arg)
{
    int32   Status;

    (void)Arg;
    Status = EEFS_HealthCheckStart("/NAND0", &HealthRep);
    CHECK(Status == EEFS_SUCCESS);
    if (Status == EEFS_SUCCESS) {
        do {
            Status = EEFS_HealthCheckStep("/NAND0", BG_STEP_READS);
            HealthSteps++;
            (void)rtems_task_wake_after(1);
        } while (Status == 1);
    }
    HealthRc = Status;
    WriterDoneAtHealthEnd = (WriterCommits == BG_WRITES);
    (void)rtems_semaphore_release(DoneSem);
    rtems_task_exit();
}

/* High priority: whenever released (from inside a step of the check), write a file and commit */
static rtems_task WriterTask(rtems_task_argument Arg)
{
    static char             Path[32];
    EEFS_NandFileInfo_t     Meta;
    uint32                  i;

    (void)Arg;
    memset(&Meta, 0, sizeof(Meta));
    Meta.FileType = EEFS_FILE_TYPE_OTHER;
    Meta.Slot = EEFS_NAND_SLOT_NONE;
    Meta.Version = 1;
    for (i = 0; i < BG_WRITES; i++) {
        (void)rtems_semaphore_obtain(WriterGo, RTEMS_WAIT, RTEMS_NO_TIMEOUT);
        WaitsBefore[i] = LockContended;
        sprintf(Path, "/NAND0/bg/file%lu", (unsigned long)i);
        CHECK(EEFS_WriteFile(Path, &Meta, Image2, 200000 + (i * 4096)) == EEFS_SUCCESS);
        WaitsAfterWrite[i] = LockContended;
        CHECK(EEFS_Commit("/NAND0") == EEFS_SUCCESS);
        WriterCommits++;
    }
    (void)rtems_semaphore_release(DoneSem);
    rtems_task_exit();
}

static void check_nand_health_background(void)
{
    EEFS_NandGeometry_t     Geo;
    static EEFS_NandDevice_t Dev;   /* not on the stack: its address must differ from the other devices' */
    EEFS_NandFileInfo_t     Meta;
    EEFS_NandFileInfo_t     FInfo;
    EEFS_NandVolumeInfo_t   VInfo;
    EEFS_HealthReport_t    *Other = &HealthRep;
    NandSim_t              *Sim;
    rtems_id                Health;
    rtems_id                Writer;
    rtems_status_code       Sc;
    uint32                  i;
    uint32                  Bad = 0;

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
    Dev.Ops = &HookOps;
    make_image(Image, 300000, 21);
    make_image(Image2, 300000, 22);

    /* a volume with a boot image, a library and an A/B application whose inactive copy B has decayed */
    CHECK(EEFS_Format("/NANDDEV0", (cpuaddr)&Dev) == EEFS_SUCCESS);
    CHECK(EEFS_Mount("/NANDDEV0", "/NAND0") == EEFS_SUCCESS);
    memset(&Meta, 0, sizeof(Meta));
    Meta.FileType = EEFS_FILE_TYPE_BOOT_FIT;
    Meta.Slot = EEFS_NAND_SLOT_NONE;
    Meta.Version = 3;
    CHECK(EEFS_WriteFile("/NAND0/boot/image.fit", &Meta, Image, 300000) == EEFS_SUCCESS);
    Meta.FileType = EEFS_FILE_TYPE_CFS_LIB;
    CHECK(EEFS_WriteFile("/NAND0/cf/lib.so", &Meta, Image2, 70000) == EEFS_SUCCESS);
    Meta.FileType = EEFS_FILE_TYPE_CFS_APP;
    Meta.Slot = EEFS_NAND_SLOT_A;
    CHECK(EEFS_WriteFile("/NAND0/cf/app.so", &Meta, Image, 150000) == EEFS_SUCCESS);
    Meta.Slot = EEFS_NAND_SLOT_B;
    CHECK(EEFS_WriteFile("/NAND0/cf/app.so", &Meta, Image, 150000) == EEFS_SUCCESS);
    CHECK(EEFS_SetFallbackCompatible("/NAND0/cf/app.so", 1) == EEFS_SUCCESS);
    CHECK(EEFS_Commit("/NAND0") == EEFS_SUCCESS);
    CHECK(EEFS_GetFileInfo("/NAND0/cf/app.so", &FInfo) == EEFS_SUCCESS);
    CHECK(FInfo.IsAB == 1 && FInfo.ActiveSlot == EEFS_NAND_SLOT_A);
    NandSim_CorruptBytes(Sim, FInfo.SlotInfo[1].FirstBlock, 5, 100, 1);   /* B, data page 5 */

    /* the API: no report yet, one check at a time, cancel */
    CHECK(EEFS_HealthCheckSummary("/NAND0", &HealthSum) == EEFS_ERROR);
    CHECK(EEFS_HealthCheckStart("/NAND0", &HealthRep) == EEFS_SUCCESS);
    CHECK(EEFS_HealthCheckStart("/NAND0", Other) == EEFS_DEVICE_IS_BUSY);
    CHECK(EEFS_HealthCheckStep("/NAND0", 1) == 1);
    CHECK(EEFS_HealthCheckCancel("/NAND0") == EEFS_SUCCESS);
    CHECK(EEFS_HealthCheckStep("/NAND0", 1) == EEFS_INVALID_ARGUMENT);

    /* the check in a low-priority task, the writes in a high-priority one */
    Sc = rtems_semaphore_create(rtems_build_name('B', 'G', 'D', 'N'), 0, RTEMS_COUNTING_SEMAPHORE | RTEMS_PRIORITY,
                                0, &DoneSem);
    CHECK(Sc == RTEMS_SUCCESSFUL);
    Sc = rtems_semaphore_create(rtems_build_name('B', 'G', 'G', 'O'), 0, RTEMS_COUNTING_SEMAPHORE | RTEMS_PRIORITY,
                                0, &WriterGo);
    CHECK(Sc == RTEMS_SUCCESSFUL);
    Sc = rtems_task_create(rtems_build_name('H', 'L', 'T', 'H'), 200, 32 * 1024, RTEMS_DEFAULT_MODES,
                           RTEMS_DEFAULT_ATTRIBUTES, &Health);
    CHECK(Sc == RTEMS_SUCCESSFUL);
    HealthId = Health;
    Sc = rtems_task_create(rtems_build_name('W', 'R', 'T', 'R'), 50, 32 * 1024, RTEMS_DEFAULT_MODES,
                           RTEMS_DEFAULT_ATTRIBUTES, &Writer);
    CHECK(Sc == RTEMS_SUCCESSFUL);
    CHECK(rtems_task_start(Health, HealthTask, 0) == RTEMS_SUCCESSFUL);
    CHECK(rtems_task_start(Writer, WriterTask, 0) == RTEMS_SUCCESSFUL);
    CHECK(rtems_semaphore_obtain(DoneSem, RTEMS_WAIT, 30000) == RTEMS_SUCCESSFUL);
    CHECK(rtems_semaphore_obtain(DoneSem, RTEMS_WAIT, 30000) == RTEMS_SUCCESSFUL);

    /* the expected report: the check restarted for the writes, then went through the final state */
    CHECK(HealthRc == EEFS_SUCCESS);
    CHECK(WriterCommits == BG_WRITES);
    CHECK(WriterDoneAtHealthEnd == 1);
    CHECK(EEFS_HealthCheckSummary("/NAND0", &HealthSum) == EEFS_SUCCESS);
    CHECK(memcmp(&HealthSum, &HealthRep.S, sizeof(HealthSum)) == 0);
    CHECK(EEFS_GetVolumeInfo("/NAND0", &VInfo) == EEFS_SUCCESS);
    printf("health check in the background: %lu steps, %lu restarts, %lu pages read, lock waits %lu, summary 0x%lx\n",
           (unsigned long)HealthSum.Steps, (unsigned long)HealthSum.Restarts, (unsigned long)HealthSum.PagesRead,
           (unsigned long)LockContended, (unsigned long)HealthSum.Summary);
    CHECK(HealthSum.Complete == 1);
    CHECK(Triggers == BG_WRITES);
    CHECK(LockContended == BG_WRITES);          /* each write waited once, for the step it interrupted */
    for (i = 0; i < BG_WRITES; i++) {
        CHECK(WaitsAfterWrite[i] == WaitsBefore[i] + 1);
    }
    CHECK(HealthSum.Restarts == BG_WRITES);     /* each write + commit restarted the check once */
    CHECK(HealthSum.Steps == HealthSteps);
    CHECK(HealthSum.DirSeq == VInfo.LastSeq);
    CHECK(HealthSum.FilesChecked == 3 + BG_WRITES);
    CHECK(HealthSum.SlotsChecked == 4 + BG_WRITES);
    CHECK(HealthSum.Summary == (EEFS_HEALTH_DEGRADED | EEFS_HEALTH_AB_ISSUE | EEFS_HEALTH_FILE_ISSUE));
    CHECK(HealthSum.FindingCount == 2);
    for (i = 0; i < HealthSum.FindingCount; i++) {
        if (HealthSum.Finding[i].Code == EEFS_HEALTH_FILE_CORRUPT) {
            CHECK(HealthSum.Finding[i].Slot == EEFS_NAND_SLOT_B && HealthSum.Finding[i].Aux0 == 1);
            CHECK(HealthSum.Finding[i].Block == FInfo.SlotInfo[1].FirstBlock);
        }
        else if (HealthSum.Finding[i].Code == EEFS_HEALTH_AB_B_CORRUPT) {
            CHECK(strcmp(HealthSum.Finding[i].Path, "/cf/app.so") == 0);
        }
        else {
            Bad++;
        }
    }
    CHECK(Bad == 0);
    CHECK(EEFS_GetFileInfo("/NAND0/cf/app.so", &FInfo) == EEFS_SUCCESS);
    CHECK(FInfo.ActiveFailed == 0);             /* the inactive copy is bad; the active one is fine */
    CHECK(FInfo.SlotInfo[1].Corrupt == 1);

    /* repair B from A; the next check is clean; nothing to reclaim */
    CHECK(EEFS_CopySlot("/NAND0/cf/app.so", EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_B) == EEFS_SUCCESS);
    CHECK(EEFS_Commit("/NAND0") == EEFS_SUCCESS);
    CHECK(EEFS_HealthCheckStart("/NAND0", &HealthRep) == EEFS_SUCCESS);
    while (EEFS_HealthCheckStep("/NAND0", 64) == 1) {
    }
    CHECK(EEFS_HealthCheckSummary("/NAND0", &HealthSum) == EEFS_SUCCESS);
    CHECK(HealthSum.Summary == EEFS_HEALTH_OK && HealthSum.FindingCount == 0 && HealthSum.Restarts == 0);
    CHECK(EEFS_ReclaimOrphans("/NAND0") == 0);
    CHECK(EEFS_UnMount("/NAND0") == EEFS_SUCCESS);
    CHECK(NandSim_Violations(Sim) == 0);
    NandSim_Destroy(Sim);
}

static rtems_task Init(rtems_task_argument ignored)
{
    rtems_status_code   Sc;

    (void)ignored;

    /* the library lock first: every EEFS_Lib call takes it */
    Sc = rtems_semaphore_create(rtems_build_name('E', 'E', 'F', 'S'), 1,
                                RTEMS_BINARY_SEMAPHORE | RTEMS_PRIORITY | RTEMS_INHERIT_PRIORITY, 0, &LockId);
    if (Sc != RTEMS_SUCCESSFUL) {
        printf("FAIL: library lock not created\n");
        LockId = 0;
        Failures++;
    }

    printf("\nEEFS %d.%d.%d RTEMS NAND test\n", EEFS_MAJOR_VERSION, EEFS_MINOR_VERSION, EEFS_REVISION);

    EEFS_LibInit();
    CHECK(EEFS_Crc32("123456789", 9) == 0xCBF43926UL);
    check_nand_format();
    check_nand_files();
    check_nand_ab();
    check_nand_health_background();

    printf(Failures == 0 ? "EEFS RTEMS TEST PASSED\n" : "EEFS RTEMS TEST FAILED (%d)\n", Failures);
    exit(Failures == 0 ? 0 : 1);
}

#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CONSOLE_DRIVER
#define CONFIGURE_MAXIMUM_TASKS            3
#define CONFIGURE_MAXIMUM_SEMAPHORES       3
#define CONFIGURE_MICROSECONDS_PER_TICK    1000
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_ATTRIBUTES     RTEMS_FLOATING_POINT
#define CONFIGURE_INIT_TASK_STACK_SIZE     (32 * 1024)
#define CONFIGURE_EXTRA_TASK_STACKS        (2 * 32 * 1024)
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
