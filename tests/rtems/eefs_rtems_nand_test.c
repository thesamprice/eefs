/*
 * RTEMS end-to-end test of the EEFS NAND backend (EEFS_BACKEND=nand).
 *
 * The device is the RAM NAND simulator of the host tests at the target geometry: 4096-byte pages, 224 OOB bytes,
 * 128 pages per block (512 KiB blocks), 256 blocks.  The test follows the software-load workflow the backend is
 * built for.  A "power cycle" loses every RAM state of the driver and the library; the NAND keeps its content.
 *
 *   1. Format and mount.
 *   2. Install boot load v1 in slot A: a FIT image, a kernel ELF and a device tree with their load addresses, and a
 *      cFS application and library with priority, stack size and entry symbol.  One commit; the bootloader does not
 *      see the files before it.
 *   3. Upload load v2 into slot B, activate B and mark it compatible with A: one commit (one directory record).
 *   4. Power cycle: the bootloader (microeefs_nand) loads the active load, v2 from B, with every CRC checked.
 *   5. A data page of B's kernel decays: the bootloader falls back to A's kernel and reports ActiveFailed.
 *   6. Power on: the application records the boot report (SetFailed, commit) and starts the health check in a
 *      low-priority task (small steps, a tick of sleep between them) while a high-priority task writes log files and
 *      commits.  The library lock is the EEFS-provided RTEMS lock (eefs_rtems_lock.c: the eefs target itself when
 *      configured with EEFS_LOCK=rtems, otherwise the test's copy of the library built the same way, see
 *      CMakeLists.txt): each write waits for the step it interrupts, that step runs at the writer's priority
 *      (priority inheritance, checked), and the check restarts after each change and ends reporting the decayed
 *      copy.
 *   7. Repair and roll back: CopySlotAll(A, B) rewrites B from the good copies, SetActiveSlotAll(A); one commit.
 *      After a power cycle the bootloader loads v1 from A, and the repaired B copies are v1 too.
 *   8. Upload v2 into B again, activate it, commit; after a power cycle it boots cleanly; promote it to A with
 *      CopySlotAll(B, A) and commit.
 *   9. Power cycle, mount, and a final health check: clean.  Nothing to reclaim, no NAND rule violated.
 *
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
#include "eefs_rtems_lock.h"
#include "microeefs_nand.h"
#include "nand_sim.h"

#define DEVICE          "/NANDDEV0"
#define MOUNT           "/NAND0"
#define LOAD_FILES      5
#define MAX_IMAGE       (600000 + 4096)

/* The driver's tables (eefs_filesys.c): cleared by a power cycle, like the rest of RAM */
extern EEFS_Device_t     EEFS_DeviceTable[EEFS_MAX_DEVICES];
extern EEFS_Volume_t     EEFS_VolumeTable[EEFS_MAX_VOLUMES];

static int               Failures;
static uint8             Page[4096];            /* the bootloader's page buffer */
static uint8             Image[MAX_IMAGE];
static uint8             Loaded[MAX_IMAGE];
static NandSim_t        *Sim;
static EEFS_NandDevice_t Dev;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            Failures++;                                                 \
        }                                                               \
    } while (0)

/*
 * The software load
 */

typedef struct
{
    const char *Name;           /* path on the volume */
    uint8       FileType;
    uint32      Size;           /* v1 size; v2 is 2 bytes larger */
    uint64      DestAddress;    /* BOOT_* */
} LoadFile_t;

static const LoadFile_t Load[LOAD_FILES] = {
    { "/boot/image.fit",        EEFS_FILE_TYPE_BOOT_FIT, 600000, 0x10000000UL },   /* two blocks */
    { "/boot/kernel.elf",       EEFS_FILE_TYPE_BOOT_ELF, 300000, 0x20000000UL },
    { "/boot/board.dtb",        EEFS_FILE_TYPE_BOOT_DTB,  20000, 0x1F000000UL },
    { "/cf/apps/sample_app.so", EEFS_FILE_TYPE_CFS_APP,  150000, 0 },
    { "/cf/lib/sample_lib.so",  EEFS_FILE_TYPE_CFS_LIB,   70000, 0 }
};
#define KERNEL          1

static uint32 file_size(uint32 File, uint32 Version)
{
    return(Load[File].Size + ((Version - 1) * 2));
}

/* The content of a file version */
static void make_image(uint8 *Buf, uint32 Size, uint32 Seed)
{
    uint32  i;

    for (i = 0; i < Size; i++) {
        Buf[i] = (uint8)(((i ^ (Seed * 0x01000193UL)) * 0x9E3779B1UL) >> 24);
    }
}

static void make_file(uint32 File, uint32 Version)
{
    make_image(Image, file_size(File, Version), (File * 16) + Version);
}

static void make_meta(uint32 File, uint32 Version, uint8 Slot, EEFS_NandFileInfo_t *Meta)
{
    memset(Meta, 0, sizeof(*Meta));
    Meta->FileType = Load[File].FileType;
    Meta->Slot = Slot;
    Meta->Version = Version;
    switch (Load[File].FileType) {
    case EEFS_FILE_TYPE_BOOT_FIT:
        Meta->Type.Fit.DestAddress = Load[File].DestAddress;
        break;
    case EEFS_FILE_TYPE_BOOT_ELF:
        Meta->Type.Elf.DestAddress = Load[File].DestAddress;
        Meta->Type.Elf.EntryPoint = Load[File].DestAddress + 0x1000 + Version;
        Meta->Type.Elf.Flags = EEFS_NAND_ELF_FLAG_ENTRY_VALID;
        break;
    case EEFS_FILE_TYPE_BOOT_DTB:
        Meta->Type.Dtb.DestAddress = Load[File].DestAddress;
        break;
    case EEFS_FILE_TYPE_CFS_APP:
        strcpy(Meta->Type.App.AppName, "SAMPLE_APP");
        strcpy(Meta->Type.App.EntrySymbol, "SAMPLE_APP_Main");
        Meta->Type.App.Priority = (uint16)(50 + Version);
        Meta->Type.App.StackSize = 16384 * Version;
        Meta->Type.App.ExceptionAction = 0;
        break;
    default:
        strcpy(Meta->Type.Lib.LibName, "SAMPLE_LIB");
        strcpy(Meta->Type.Lib.EntrySymbol, "SAMPLE_LIB_Init");
        break;
    }
}

/* The type-specific fields of Info are those make_meta gives the version */
static int type_matches(uint32 File, uint32 Version, const EEFS_NandFileInfo_t *Info)
{
    EEFS_NandFileInfo_t Want;

    make_meta(File, Version, EEFS_NAND_SLOT_NONE, &Want);
    if ((Info->FileType != Want.FileType) || (Info->Version != Version)) return(0);
    switch (Want.FileType) {
    case EEFS_FILE_TYPE_BOOT_FIT:
        return(Info->Type.Fit.DestAddress == Want.Type.Fit.DestAddress);
    case EEFS_FILE_TYPE_BOOT_ELF:
        return((Info->Type.Elf.DestAddress == Want.Type.Elf.DestAddress) &&
               (Info->Type.Elf.EntryPoint == Want.Type.Elf.EntryPoint) && (Info->Type.Elf.Flags == Want.Type.Elf.Flags));
    case EEFS_FILE_TYPE_BOOT_DTB:
        return(Info->Type.Dtb.DestAddress == Want.Type.Dtb.DestAddress);
    case EEFS_FILE_TYPE_CFS_APP:
        return((strcmp(Info->Type.App.AppName, Want.Type.App.AppName) == 0) &&
               (strcmp(Info->Type.App.EntrySymbol, Want.Type.App.EntrySymbol) == 0) &&
               (Info->Type.App.Priority == Want.Type.App.Priority) && (Info->Type.App.StackSize == Want.Type.App.StackSize) &&
               (Info->Type.App.ExceptionAction == Want.Type.App.ExceptionAction));
    default:
        return((strcmp(Info->Type.Lib.LibName, Want.Type.Lib.LibName) == 0) &&
               (strcmp(Info->Type.Lib.EntrySymbol, Want.Type.Lib.EntrySymbol) == 0));
    }
}

static void path_of(char *Path, uint32 File, const char *Suffix)
{
    sprintf(Path, "%s%s%s", MOUNT, Load[File].Name, Suffix);
}

static const char *slot_name(uint8 Slot)
{
    return((Slot == EEFS_NAND_SLOT_A) ? "A" : ((Slot == EEFS_NAND_SLOT_B) ? "B" : "-"));
}

/* Writes version Version of every file of the load into Slot (one batch, not committed) */
static void write_load(uint32 Version, uint8 Slot)
{
    EEFS_NandFileInfo_t Meta;
    char                Path[EEFS_MAX_PATH_SIZE];
    uint32              f;

    for (f = 0; f < LOAD_FILES; f++) {
        make_file(f, Version);
        make_meta(f, Version, Slot, &Meta);
        path_of(Path, f, "");
        CHECK(EEFS_WriteFile(Path, &Meta, Image, file_size(f, Version)) == EEFS_SUCCESS);
    }
}

/* Reads every file back through the driver (Name + Suffix) and checks content, Stat and GetFileInfo */
static void check_load_files(uint32 Version, const char *Suffix)
{
    EEFS_NandFileInfo_t Info;
    EEFS_Stat_t         St;
    char                Path[EEFS_MAX_PATH_SIZE];
    uint32              Size;
    uint32              Total;
    uint32              f;
    int32               Fd;
    int32               n;

    for (f = 0; f < LOAD_FILES; f++) {
        Size = file_size(f, Version);
        path_of(Path, f, Suffix);
        CHECK(EEFS_Stat(Path, &St) == EEFS_SUCCESS);
        CHECK(St.FileSize == Size);
        CHECK(EEFS_GetFileInfo(Path, &Info) == EEFS_SUCCESS);
        CHECK(type_matches(f, Version, &Info));
        CHECK(Info.FileSize == Size);
        Fd = EEFS_Open(Path, O_RDONLY);
        CHECK(Fd >= 0);
        if (Fd < 0) continue;
        Total = 0;
        while ((n = EEFS_Read(Fd, &Loaded[Total], 8191)) > 0) {
            Total += (uint32)n;
        }
        CHECK(EEFS_Close(Fd) == EEFS_SUCCESS);
        make_file(f, Version);
        CHECK((Total == Size) && (memcmp(Loaded, Image, Size) == 0));
    }
}

/*
 * The bootloader
 */

/* Loads File (with Suffix) as the bootloader does and checks that it gets version Version from Slot, byte for byte,
 * with the expected ActiveFailed.  Prints the boot line. */
static void boot_file(uint32 File, const char *Suffix, uint32 Version, uint8 Slot, uint8 ActiveFailed)
{
    MicroEEFS_NandInfo_t    Info;
    char                    Name[EEFS_MAX_PATH_SIZE];
    int32                   Rc;

    sprintf(Name, "%s%s", Load[File].Name, Suffix);
    memset(Loaded, 0, sizeof(Loaded));
    Rc = MicroEEFS_NandLoadFile(&Dev, Page, Name, Loaded, sizeof(Loaded), &Info);
    CHECK(Rc == EEFS_SUCCESS);
    if (Rc != EEFS_SUCCESS) return;
    make_file(File, Version);
    CHECK(Info.Info.FileSize == file_size(File, Version));
    CHECK(memcmp(Loaded, Image, file_size(File, Version)) == 0);
    CHECK(type_matches(File, Version, &Info.Info));
    CHECK(Info.LoadedSlot == Slot);
    CHECK(Info.Info.ActiveFailed == ActiveFailed);
    CHECK((Info.UsedScan == 0) && (Info.DirectoryValid == 1));
    printf("   boot %-24s v%lu from %s, %6lu bytes", Name, (unsigned long)Info.Info.Version,
           slot_name(Info.LoadedSlot), (unsigned long)Info.Info.FileSize);
    if (Info.Info.FileType <= EEFS_FILE_TYPE_BOOT_DTB) {
        printf(" -> 0x%08lx", (unsigned long)Info.Info.Type.Fit.DestAddress);   /* DestAddress leads every BOOT_* */
    }
    else if (Info.Info.FileType == EEFS_FILE_TYPE_CFS_APP) {
        printf(" %s prio %u stack %lu", Info.Info.Type.App.EntrySymbol, (unsigned)Info.Info.Type.App.Priority,
               (unsigned long)Info.Info.Type.App.StackSize);
    }
    else {
        printf(" %s", Info.Info.Type.Lib.EntrySymbol);
    }
    printf("%s\n", (Info.Info.ActiveFailed != 0) ? "  [ActiveFailed: fallback]" : "");
}

/* The whole active load boots: version Version from Slot, no failure */
static void boot_load(uint32 Version, uint8 Slot)
{
    uint32  f;

    for (f = 0; f < LOAD_FILES; f++) {
        boot_file(f, "", Version, Slot, 0);
    }
}

/*
 * Power and mounting
 */

/* Every RAM state of the driver and the library is lost; the NAND keeps its content */
static void power_cycle(void)
{
    memset(EEFS_DeviceTable, 0, sizeof(EEFS_DeviceTable));
    memset(EEFS_VolumeTable, 0, sizeof(EEFS_VolumeTable));
    EEFS_LibInit();
}

static void power_on_mount(void)
{
    CHECK(EEFS_InitFS(DEVICE, (cpuaddr)&Dev) == EEFS_SUCCESS);
    CHECK(EEFS_Mount(DEVICE, MOUNT) == EEFS_SUCCESS);
}

/* Commits and checks that the commit wrote exactly one directory record */
static void commit_one_record(void)
{
    EEFS_NandVolumeInfo_t   Before;
    EEFS_NandVolumeInfo_t   After;

    CHECK(EEFS_GetVolumeInfo(MOUNT, &Before) == EEFS_SUCCESS);
    CHECK(Before.UncommittedChanges == 1);
    CHECK(EEFS_Commit(MOUNT) == EEFS_SUCCESS);
    CHECK(EEFS_GetVolumeInfo(MOUNT, &After) == EEFS_SUCCESS);
    CHECK(After.LastSeq == Before.LastSeq + 1);
    CHECK((After.UncommittedChanges == 0) && (After.PendingCleanup == 0));
    CHECK((After.MirrorValid[0] == 1) && (After.MirrorValid[1] == 1));
}

static uint32 last_seq(void)
{
    EEFS_NandVolumeInfo_t   Info;

    CHECK(EEFS_GetVolumeInfo(MOUNT, &Info) == EEFS_SUCCESS);
    return(Info.LastSeq);
}

/*
 * The power-on health check in the background
 */

#define BG_WRITES       3
#define BG_STEP_READS   4
#define BG_TRIGGER_READS 150
#define HEALTH_PRIO     200
#define WRITER_PRIO     50

static rtems_id                 DoneSem;
static rtems_id                 WriterGo;
static rtems_id                 HealthId;
static volatile uint32          HealthReads;        /* page reads made by the health task */
static volatile uint32          Triggers;
static volatile uint32          WaitsBefore[BG_WRITES];
static volatile uint32          WaitsAfterWrite[BG_WRITES];
static volatile rtems_task_priority PrioWhileWaited[BG_WRITES];   /* the health task's priority while the writer waited */
static volatile uint32          WriterCommits;
static volatile uint32          HealthSteps;
static volatile uint32          WriterDoneAtHealthEnd;
static volatile int32           HealthRc;
static EEFS_HealthReport_t      HealthRep;          /* the caller's report: static, about 3.5 KB */
static EEFS_HealthSummary_t     HealthSum;
static uint8                    LogData[220000];

/* The device seen by the library: the simulator, except that a page read made by the health task releases the
 * writer every BG_TRIGGER_READS reads (BG_WRITES times).  The writer has the higher priority, so it runs at once,
 * inside the health check's step, while the health task holds the library lock: it blocks on the lock, the health
 * task inherits its priority and finishes the step, and the check restarts at its next step. */
static int32 HookRead(void *Ctx, uint32 Block, uint32 PageNo, void *Data, void *Oob)
{
    rtems_id            Self;
    rtems_id            Scheduler;
    rtems_task_priority Prio = 0;

    if ((HealthId != 0) && (rtems_task_ident(RTEMS_WHO_AM_I, RTEMS_SEARCH_LOCAL_NODE, &Self) == RTEMS_SUCCESSFUL) &&
        (Self == HealthId) && (Triggers < BG_WRITES) && ((++HealthReads % BG_TRIGGER_READS) == 0)) {
        (void)rtems_semaphore_release(WriterGo);    /* the writer runs now and blocks on the library lock */
        if ((rtems_task_get_scheduler(RTEMS_SELF, &Scheduler) == RTEMS_SUCCESSFUL) &&
            (rtems_task_get_priority(RTEMS_SELF, Scheduler, &Prio) == RTEMS_SUCCESSFUL)) {
            PrioWhileWaited[Triggers] = Prio;
        }
        Triggers++;
    }
    return(NandSim_Ops()->ReadPage(Ctx, Block, PageNo, Data, Oob));
}

static int32 HookProgram(void *Ctx, uint32 Block, uint32 PageNo, const void *Data, const void *Oob)
{
    return(NandSim_Ops()->ProgramPage(Ctx, Block, PageNo, Data, Oob));
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
    Status = EEFS_HealthCheckStart(MOUNT, &HealthRep);
    CHECK(Status == EEFS_SUCCESS);
    if (Status == EEFS_SUCCESS) {
        do {
            Status = EEFS_HealthCheckStep(MOUNT, BG_STEP_READS);
            HealthSteps++;
            (void)rtems_task_wake_after(1);
        } while (Status == 1);
    }
    HealthRc = Status;
    WriterDoneAtHealthEnd = (WriterCommits == BG_WRITES);
    (void)rtems_semaphore_release(DoneSem);
    rtems_task_exit();
}

/* High priority: whenever released (from inside a step of the check), write a log file and commit */
static rtems_task WriterTask(rtems_task_argument Arg)
{
    char                    Path[EEFS_MAX_PATH_SIZE];
    EEFS_NandFileInfo_t     Meta;
    uint32                  i;

    (void)Arg;
    memset(&Meta, 0, sizeof(Meta));
    Meta.FileType = EEFS_FILE_TYPE_OTHER;
    Meta.Slot = EEFS_NAND_SLOT_NONE;
    Meta.Version = 1;
    for (i = 0; i < BG_WRITES; i++) {
        (void)rtems_semaphore_obtain(WriterGo, RTEMS_WAIT, RTEMS_NO_TIMEOUT);
        WaitsBefore[i] = EEFS_RtemsLockWaits();
        sprintf(Path, "%s/log/boot%lu.log", MOUNT, (unsigned long)i);
        CHECK(EEFS_WriteFile(Path, &Meta, LogData, 200000 + (i * 4096)) == EEFS_SUCCESS);
        WaitsAfterWrite[i] = EEFS_RtemsLockWaits();
        CHECK(EEFS_Commit(MOUNT) == EEFS_SUCCESS);
        WriterCommits++;
    }
    (void)rtems_semaphore_release(DoneSem);
    rtems_task_exit();
}

/* Runs the health check in a priority-200 task while a priority-50 task writes and commits BG_WRITES log files */
static void health_check_in_background(void)
{
    rtems_id            Health;
    rtems_id            Writer;
    rtems_status_code   Sc;
    uint32              Waits0 = EEFS_RtemsLockWaits();
    uint32              i;

    make_image(LogData, sizeof(LogData), 99);
    Sc = rtems_semaphore_create(rtems_build_name('B', 'G', 'D', 'N'), 0, RTEMS_COUNTING_SEMAPHORE | RTEMS_PRIORITY,
                                0, &DoneSem);
    CHECK(Sc == RTEMS_SUCCESSFUL);
    Sc = rtems_semaphore_create(rtems_build_name('B', 'G', 'G', 'O'), 0, RTEMS_COUNTING_SEMAPHORE | RTEMS_PRIORITY,
                                0, &WriterGo);
    CHECK(Sc == RTEMS_SUCCESSFUL);
    Sc = rtems_task_create(rtems_build_name('H', 'L', 'T', 'H'), HEALTH_PRIO, 32 * 1024, RTEMS_DEFAULT_MODES,
                           RTEMS_DEFAULT_ATTRIBUTES, &Health);
    CHECK(Sc == RTEMS_SUCCESSFUL);
    HealthId = Health;
    Sc = rtems_task_create(rtems_build_name('W', 'R', 'T', 'R'), WRITER_PRIO, 32 * 1024, RTEMS_DEFAULT_MODES,
                           RTEMS_DEFAULT_ATTRIBUTES, &Writer);
    CHECK(Sc == RTEMS_SUCCESSFUL);
    CHECK(rtems_task_start(Health, HealthTask, 0) == RTEMS_SUCCESSFUL);
    CHECK(rtems_task_start(Writer, WriterTask, 0) == RTEMS_SUCCESSFUL);
    CHECK(rtems_semaphore_obtain(DoneSem, RTEMS_WAIT, 30000) == RTEMS_SUCCESSFUL);
    CHECK(rtems_semaphore_obtain(DoneSem, RTEMS_WAIT, 30000) == RTEMS_SUCCESSFUL);
    HealthId = 0;
    (void)rtems_semaphore_delete(DoneSem);
    (void)rtems_semaphore_delete(WriterGo);

    CHECK(HealthRc == EEFS_SUCCESS);
    CHECK(WriterCommits == BG_WRITES);
    CHECK(WriterDoneAtHealthEnd == 1);
    CHECK(Triggers == BG_WRITES);
    CHECK(EEFS_RtemsLockWaits() - Waits0 == BG_WRITES);     /* each write waited once, for the step it interrupted */
    for (i = 0; i < BG_WRITES; i++) {
        CHECK(WaitsAfterWrite[i] == WaitsBefore[i] + 1);
        CHECK(PrioWhileWaited[i] == WRITER_PRIO);          /* priority inheritance */
    }
}

/* Number of findings with Code in the summary */
static uint32 count_findings(const EEFS_HealthSummary_t *S, uint16 Code)
{
    uint32  i;
    uint32  n = 0;

    for (i = 0; (i < S->FindingCount) && (i < EEFS_NAND_MAX_HEALTH_FINDINGS); i++) {
        if (S->Finding[i].Code == Code) n++;
    }
    return(n);
}

static const EEFS_HealthFinding_t *find_finding(const EEFS_HealthSummary_t *S, uint16 Code)
{
    uint32  i;

    for (i = 0; (i < S->FindingCount) && (i < EEFS_NAND_MAX_HEALTH_FINDINGS); i++) {
        if (S->Finding[i].Code == Code) return(&S->Finding[i]);
    }
    return(NULL);
}

static void print_health(const char *What, const EEFS_HealthSummary_t *S)
{
    uint32  i;

    printf("   %s: summary 0x%02lx, %lu findings, %lu files / %lu copies checked, %lu pages read in %lu steps, "
           "%lu restarts\n", What, (unsigned long)S->Summary, (unsigned long)S->FindingCount,
           (unsigned long)S->FilesChecked, (unsigned long)S->SlotsChecked, (unsigned long)S->PagesRead,
           (unsigned long)S->Steps, (unsigned long)S->Restarts);
    for (i = 0; (i < S->FindingCount) && (i < EEFS_NAND_MAX_HEALTH_FINDINGS); i++) {
        printf("     finding %2u %-24s slot %s block %5u aux %lu/%lu\n", (unsigned)S->Finding[i].Code,
               S->Finding[i].Path, slot_name(S->Finding[i].Slot), (unsigned)S->Finding[i].Block,
               (unsigned long)S->Finding[i].Aux0, (unsigned long)S->Finding[i].Aux1);
    }
}

/* A foreground health check run to completion */
static void health_check_now(void)
{
    CHECK(EEFS_HealthCheckStart(MOUNT, &HealthRep) == EEFS_SUCCESS);
    while (EEFS_HealthCheckStep(MOUNT, 64) == 1) {
    }
    CHECK(EEFS_HealthCheckSummary(MOUNT, &HealthSum) == EEFS_SUCCESS);
}

/*
 * The workflow
 */

static void run_workflow(void)
{
    EEFS_NandGeometry_t     Geo;
    EEFS_NandVolumeInfo_t   VInfo;
    EEFS_NandFileInfo_t     FInfo;
    EEFS_Stat_t             St;
    MicroEEFS_NandInfo_t    MInfo;
    const EEFS_HealthFinding_t *F;
    char                    Path[EEFS_MAX_PATH_SIZE];
    uint32                  Seq;
    uint32                  KernelB;
    uint32                  KernelEntry;
    uint32                  f;

    memset(&Geo, 0, sizeof(Geo));
    Geo.PageSize = 4096;
    Geo.OobSize = 224;
    Geo.PagesPerBlock = 128;
    Geo.BlockCount = 256;
    Geo.DirRegionBlocks = 16;
    Geo.BadMarkerOffset = 0;
    Geo.BadMarkerPages = EEFS_NAND_BADMARKER_PAGE0;
    Sim = NandSim_Create(&Geo);
    CHECK(Sim != NULL);
    if (Sim == NULL) return;
    NandSim_FillDevice(Sim, &Dev);
    Dev.Ops = &HookOps;

    /* 1. format and mount */
    CHECK(EEFS_Format(DEVICE, (cpuaddr)&Dev) == EEFS_SUCCESS);
    CHECK(EEFS_Mount(DEVICE, MOUNT) == EEFS_SUCCESS);
    CHECK(EEFS_GetVolumeInfo(MOUNT, &VInfo) == EEFS_SUCCESS);
    CHECK((VInfo.DataBlocks == 240) && (VInfo.FreeBlocks == 240) && (VInfo.FilesInUse == 0) && (VInfo.LastSeq == 1));
    printf("1. formatted %lu blocks of 512 KiB (4096 + 224 bytes x 128 pages): %lu data blocks free, mounted on %s\n",
           (unsigned long)VInfo.BlockCount, (unsigned long)VInfo.FreeBlocks, MOUNT);

    /* 2. boot load v1 into slot A, one commit; the bootloader sees nothing before it */
    write_load(1, EEFS_NAND_SLOT_A);
    CHECK(MicroEEFS_NandFindFile(&Dev, Page, Load[0].Name, &MInfo) == EEFS_FILE_NOT_FOUND);
    commit_one_record();
    check_load_files(1, "");
    check_load_files(1, "@A");
    path_of(Path, KERNEL, "");
    CHECK(EEFS_GetFileInfo(Path, &FInfo) == EEFS_SUCCESS);
    CHECK((FInfo.IsAB == 1) && (FInfo.ActiveSlot == EEFS_NAND_SLOT_A) && (FInfo.SlotInfo[1].InUse == 0));
    CHECK(EEFS_Stat(Path, &St) == EEFS_SUCCESS);
    KernelEntry = St.InodeIndex;
    CHECK(EEFS_GetVolumeInfo(MOUNT, &VInfo) == EEFS_SUCCESS);
    printf("2. load v1 written to slot A and committed (Seq %lu): %lu files, %lu blocks used\n",
           (unsigned long)VInfo.LastSeq, (unsigned long)VInfo.FilesInUse, (unsigned long)VInfo.UsedBlocks);
    boot_load(1, EEFS_NAND_SLOT_A);

    /* 3. upload v2 into B, activate it, compatible with A: one commit */
    write_load(2, EEFS_NAND_SLOT_B);
    CHECK(EEFS_SetActiveSlotAll(MOUNT, EEFS_NAND_SLOT_B) == LOAD_FILES);
    CHECK(EEFS_SetFallbackCompatibleAll(MOUNT, 1) == LOAD_FILES);
    printf("3. load v2 uploaded to slot B, activated and marked compatible; before the commit the bootloader still "
           "loads v1:\n");
    boot_file(0, "", 1, EEFS_NAND_SLOT_A, 0);
    Seq = last_seq();
    commit_one_record();
    check_load_files(2, "");
    check_load_files(1, "@A");
    CHECK(EEFS_GetFileInfo(Path, &FInfo) == EEFS_SUCCESS);
    CHECK((FInfo.ActiveSlot == EEFS_NAND_SLOT_B) && (FInfo.FallbackCompatible == 1) && (FInfo.ActiveFailed == 0));
    CHECK((FInfo.SlotInfo[0].Version == 1) && (FInfo.SlotInfo[1].Version == 2));
    KernelB = FInfo.SlotInfo[1].FirstBlock;
    printf("   all of it committed with one directory record (Seq %lu -> %lu)\n", (unsigned long)Seq,
           (unsigned long)(Seq + 1));

    /* 4. power cycle: the bootloader loads the active load */
    power_cycle();
    printf("4. power cycle: the bootloader loads the active load\n");
    boot_load(2, EEFS_NAND_SLOT_B);

    /* 5. B's kernel decays (data page 5): the bootloader falls back to A's kernel and reports ActiveFailed */
    NandSim_CorruptBytes(Sim, KernelB, 5, 100, 1);
    printf("5. power cycle after a data page of B's kernel decayed (block %lu page 5)\n", (unsigned long)KernelB);
    boot_file(KERNEL, "", 1, EEFS_NAND_SLOT_A, 1);
    CHECK(MicroEEFS_NandLoadFile(&Dev, Page, "/boot/kernel.elf@B", Loaded, sizeof(Loaded), &MInfo) == EEFS_CORRUPT_FILE);
    for (f = 0; f < LOAD_FILES; f++) {
        if (f != KERNEL) boot_file(f, "", 2, EEFS_NAND_SLOT_B, 0);
    }

    /* 6. power on: record the boot report, then the health check in the background next to a writer */
    power_on_mount();
    CHECK(EEFS_SetFailed(Path) == EEFS_SUCCESS);           /* the boot report: the active kernel failed */
    commit_one_record();
    printf("6. power on: ActiveFailed recorded; health check (priority %d, %d reads per step) next to a writer "
           "(priority %d)\n", HEALTH_PRIO, BG_STEP_READS, WRITER_PRIO);
    health_check_in_background();
    CHECK(EEFS_HealthCheckSummary(MOUNT, &HealthSum) == EEFS_SUCCESS);
    CHECK(memcmp(&HealthSum, &HealthRep.S, sizeof(HealthSum)) == 0);
    print_health("report", &HealthSum);
    printf("   the writer waited for the lock %d times, at priority %lu in the health task each time\n", BG_WRITES,
           (unsigned long)PrioWhileWaited[0]);
    CHECK(HealthSum.Complete == 1);
    CHECK(HealthSum.Restarts == BG_WRITES);                 /* each write + commit restarted the check once */
    CHECK(HealthSum.Steps == HealthSteps);
    CHECK(HealthSum.DirSeq == last_seq());
    CHECK(HealthSum.FilesChecked == LOAD_FILES + BG_WRITES);
    CHECK(HealthSum.SlotsChecked == (2 * LOAD_FILES) + BG_WRITES);
    CHECK(HealthSum.Summary == (EEFS_HEALTH_DEGRADED | EEFS_HEALTH_AB_ISSUE | EEFS_HEALTH_FILE_ISSUE | EEFS_HEALTH_INFO));
    CHECK(HealthSum.FindingCount == 4 + LOAD_FILES);
    CHECK(count_findings(&HealthSum, EEFS_HEALTH_AB_VERSION_MISMATCH) == LOAD_FILES);   /* v1 in A, v2 in B */
    F = find_finding(&HealthSum, EEFS_HEALTH_FILE_CORRUPT);
    CHECK((F != NULL) && (F->EntryIndex == KernelEntry) && (F->Slot == EEFS_NAND_SLOT_B) && (F->Block == KernelB) &&
          (F->Aux0 == 1));                                   /* block CRC */
    F = find_finding(&HealthSum, EEFS_HEALTH_AB_B_CORRUPT);
    CHECK((F != NULL) && (F->EntryIndex == KernelEntry) && (strcmp(F->Path, Load[KERNEL].Name) == 0));
    F = find_finding(&HealthSum, EEFS_HEALTH_AB_ACTIVE_CORRUPT);
    CHECK((F != NULL) && (F->EntryIndex == KernelEntry) && (F->Slot == EEFS_NAND_SLOT_B));
    F = find_finding(&HealthSum, EEFS_HEALTH_AB_FAILED_FLAG_SET);
    CHECK((F != NULL) && (F->EntryIndex == KernelEntry));
    CHECK(EEFS_GetFileInfo(Path, &FInfo) == EEFS_SUCCESS);
    CHECK((FInfo.ActiveFailed == 1) && (FInfo.SlotInfo[1].Corrupt == 1) && (FInfo.SlotInfo[0].Corrupt == 0));

    /* 7. repair B from the good copies and roll the load back to A: one commit */
    CHECK(EEFS_CopySlotAll(MOUNT, EEFS_NAND_SLOT_A, EEFS_NAND_SLOT_B) == LOAD_FILES);
    CHECK(EEFS_SetActiveSlotAll(MOUNT, EEFS_NAND_SLOT_A) == LOAD_FILES);
    commit_one_record();
    CHECK(EEFS_GetFileInfo(Path, &FInfo) == EEFS_SUCCESS);
    CHECK((FInfo.ActiveSlot == EEFS_NAND_SLOT_A) && (FInfo.ActiveFailed == 0) && (FInfo.SlotInfo[1].Version == 1));
    check_load_files(1, "@B");
    power_cycle();
    printf("7. B repaired with CopySlotAll(A, B), rolled back to A, one commit; power cycle:\n");
    boot_load(1, EEFS_NAND_SLOT_A);
    boot_file(KERNEL, "@B", 1, EEFS_NAND_SLOT_B, 0);

    /* 8. v2 again into B, activated, committed; it boots; promote it to A */
    power_on_mount();
    write_load(2, EEFS_NAND_SLOT_B);
    CHECK(EEFS_SetActiveSlotAll(MOUNT, EEFS_NAND_SLOT_B) == LOAD_FILES);
    commit_one_record();
    power_cycle();
    printf("8. v2 uploaded to B again and activated; power cycle:\n");
    boot_load(2, EEFS_NAND_SLOT_B);
    power_on_mount();
    CHECK(EEFS_CopySlotAll(MOUNT, EEFS_NAND_SLOT_B, EEFS_NAND_SLOT_A) == LOAD_FILES);
    commit_one_record();
    check_load_files(2, "@A");
    check_load_files(2, "@B");
    printf("   v2 promoted to A with CopySlotAll(B, A) in one commit (Seq %lu)\n", (unsigned long)last_seq());

    /* 9. power cycle, mount, final health check: clean */
    power_cycle();
    power_on_mount();
    health_check_now();
    printf("9. power cycle and mount; final health check:\n");
    print_health("report", &HealthSum);
    CHECK((HealthSum.Summary == EEFS_HEALTH_OK) && (HealthSum.FindingCount == 0) && (HealthSum.Restarts == 0));
    CHECK(HealthSum.FilesChecked == LOAD_FILES + BG_WRITES);
    CHECK(EEFS_ReclaimOrphans(MOUNT) == 0);
    CHECK(EEFS_GetVolumeInfo(MOUNT, &VInfo) == EEFS_SUCCESS);
    CHECK((VInfo.FilesInUse == LOAD_FILES + BG_WRITES) && (VInfo.SlotsInUse == (2 * LOAD_FILES) + BG_WRITES));
    CHECK((VInfo.UncommittedChanges == 0) && (VInfo.PendingCleanup == 0) && (VInfo.BadBlocks == 0));
    CHECK(VInfo.UsedBlocks + VInfo.FreeBlocks == VInfo.DataBlocks);
    printf("   %lu files, %lu copies, %lu blocks used, %lu free, directory Seq %lu\n",
           (unsigned long)VInfo.FilesInUse, (unsigned long)VInfo.SlotsInUse, (unsigned long)VInfo.UsedBlocks,
           (unsigned long)VInfo.FreeBlocks, (unsigned long)VInfo.LastSeq);
    check_load_files(2, "");
    CHECK(EEFS_UnMount(MOUNT) == EEFS_SUCCESS);
    boot_file(KERNEL, "", 2, EEFS_NAND_SLOT_B, 0);
    printf("   NAND rule violations: %lu, %lu programs, %lu erases\n", (unsigned long)NandSim_Violations(Sim),
           (unsigned long)NandSim_Programs(Sim), (unsigned long)NandSim_Erases(Sim));
    CHECK(NandSim_Violations(Sim) == 0);
    NandSim_Destroy(Sim);
}

static rtems_task Init(rtems_task_argument ignored)
{
    (void)ignored;

    printf("\nEEFS %d.%d.%d RTEMS NAND test\n", EEFS_MAJOR_VERSION, EEFS_MINOR_VERSION, EEFS_REVISION);

    EEFS_LibInit();
    CHECK(EEFS_Crc32("123456789", 9) == 0xCBF43926UL);
    run_workflow();

    printf(Failures == 0 ? "EEFS RTEMS TEST PASSED\n" : "EEFS RTEMS TEST FAILED (%d)\n", Failures);
    exit(Failures == 0 ? 0 : 1);
}

/* Three tasks and two semaphores: the library lock is a self-contained RTEMS mutex and needs no Classic object */
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CONSOLE_DRIVER
#define CONFIGURE_MAXIMUM_TASKS            3
#define CONFIGURE_MAXIMUM_SEMAPHORES       2
#define CONFIGURE_MICROSECONDS_PER_TICK    1000
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_ATTRIBUTES     RTEMS_FLOATING_POINT
#define CONFIGURE_INIT_TASK_STACK_SIZE     (32 * 1024)
#define CONFIGURE_EXTRA_TASK_STACKS        (2 * 32 * 1024)
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
