eefs
====

EEPROM File System

This is the EEPROM File System Project (EEFS). It is a simple file system for memory devices such as EEPROM, RAM, ROM, etc. The original (EEPROM) backend is not intended for block oriented devices such as disks and flash devices; for raw NAND flash there is a separate [NAND backend](#nand-backend).

It can be used as a simple file system to boot an embedded system running vxWorks, RTEMS, or even no operating system. An EEFS image can be created on the development host, providing a single file to burn into an image that is loaded on a target. The file system is easy to understand, debug, and dump. 

There are drivers for RTEMS, vxWorks, and there is a standalone API for systems that do not have a file system. 

There is even a "microeefs" interface that allows the lookup of a file from a single function. This allows the bootloader to locate an image in EEPROM by the file name with a minimal amount of code. 
Future releases will include the ability to allow multiple EEFS volumes ( volumes in RAM and EEPROM at the same time ) 



Building
--------

EEFS builds with CMake 3.21 or newer.

Host build (the `eefs` and `microeefs` libraries, plus the `geneepromfs` and `eefstool-img` tools):

    cmake -B build
    cmake --build build
    ctest --test-dir build

Cross build for an RTEMS 6 or 7 BSP (libraries only, plus an RTEMS test app that runs under QEMU for the Zynq BSP):

    cmake -B build-rtems -DCMAKE_TOOLCHAIN_FILE=cmake/rtems-toolchain.cmake \
          -DRTEMS_PREFIX=$HOME/rtems/7 -DRTEMS_VERSION=7 \
          -DRTEMS_ARCH=arm -DRTEMS_BSP=xilinx_zynq_a9_qemu
    cmake --build build-rtems
    ctest --test-dir build-rtems

Useful options:

* `EEFS_BACKEND` selects the storage backend: `eeprom` (default) or `nand` (see [NAND backend](#nand-backend)).
* `EEFS_LOCK` selects the library lock: `none` (default, one task), `hooks` or `rtems` (see [Locking](#locking)).
* `EEFS_DRIVER` selects the OS binding: `standalone` (default) or `vxworks`. The `rtems` driver in `drivers/rtems_driver` targets the RTEMS 4.10 filesystem API and does not build on RTEMS 4.11 or later, so use `standalone` on RTEMS 6/7.
* `EEFS_MACROS_DIR` points at the directory holding the `eefs_macros.h` porting layer (EEPROM read/write, locking, time), so you can port to new hardware without editing the tree.
* `EEFS_BUILD_TOOLS` builds the host tools. It defaults to ON for native builds and OFF for cross builds.
* `EEFS_BUILD_BDM_TOOL` builds `eefstool-bdm`, which needs the RTEMS 4.10 BDM headers and libraries under `EEFS_BDM_ROOT`.

Installing exports CMake targets, so other projects can use `find_package(eefs)` and link `eefs::eefs` or `eefs::microeefs`.

NAND backend
------------

`-DEEFS_BACKEND=nand` builds EEFS for **raw NAND flash** instead of memory-mapped EEPROM. It is meant for a
boot and software-load volume on RTEMS 6/7: a bootloader loads a FIT image, kernel ELF, device tree and cFS
applications and libraries from it, and the running software uploads new loads into a second slot, tests them
and rolls back or promotes them. The API is the EEFS one (`EEFS_Lib*` and the standalone driver), plus the NAND
calls in `inc/eefs_nand.h`.

The on-media format is specified in [docs/nand-format.md](docs/nand-format.md) (normative, written for bootloader
authors); the library design, every return code and the test plan are in
[docs/nand-implementation.md](docs/nand-implementation.md).

### Building and testing

    # host: libraries plus 30 CTest tests (about 6 minutes; run them serially)
    cmake -B build-nand -G Ninja -DEEFS_BACKEND=nand
    cmake --build build-nand --clean-first
    ctest --test-dir build-nand --output-on-failure

    # RTEMS 7, Zynq under QEMU, with the EEFS-provided RTEMS lock: runs the end-to-end test (about 2 seconds)
    cmake -B build-rtems-nand -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/rtems-toolchain.cmake \
          -DRTEMS_PREFIX=$HOME/rtems/7 -DRTEMS_ARCH=arm -DRTEMS_BSP=xilinx_zynq_a9_qemu \
          -DEEFS_BACKEND=nand -DEEFS_LOCK=rtems
    cmake --build build-rtems-nand --clean-first
    ctest --test-dir build-rtems-nand --output-on-failure

The NAND backend needs `EEFS_DRIVER=standalone`; the EEPROM host tools (`geneepromfs`, `eefstool`) are not built
for it. The EEPROM build is unchanged by the NAND work.

### The HAL you implement

A BSP describes the device with an `EEFS_NandDevice_t` (`inc/eefs_nand_hal.h`) and passes its address as the
`BaseAddress` of `EEFS_Format`, `EEFS_InitFS` and the bootloader calls:

```c
static int32 MyRead(void *Ctx, uint32 Block, uint32 Page, void *Data, void *Oob);
static int32 MyProgram(void *Ctx, uint32 Block, uint32 Page, const void *Data, const void *Oob);
static int32 MyErase(void *Ctx, uint32 Block);
static const EEFS_NandOps_t MyOps = { MyRead, MyProgram, MyErase };
static EEFS_NandDevice_t    Dev;    /* Dev.Geometry = ..., Dev.Ops = &MyOps, Dev.Ctx = controller state */

EEFS_InitFS("/NANDDEV0", (cpuaddr)&Dev);        /* EEFS_Format("/NANDDEV0", (cpuaddr)&Dev) the first time */
EEFS_Mount("/NANDDEV0", "/NAND0");
```

* `ReadPage` reads `PageSize` data bytes into `Data` and `OobSize` bytes into `Oob` (either may be `NULL`). It
  returns **>= 0 on success, the number of bit errors the ECC corrected** (0 = clean; the health check reports
  blocks at or above `Geometry.EccScrubThreshold`), `EEFS_NAND_READ_UNCORRECTABLE` (-1) when the ECC could not
  correct the page, or `EEFS_NAND_READ_IO_ERROR` (-2) for a controller or bus error. **An erased page must read
  as all `0xFF` with a status >= 0**, even if the controller's ECC flags it. A read with `Data == NULL` of a
  factory-bad block must return the raw OOB.
* `ProgramPage` returns 0 or -1; `Data` or `Oob` may be `NULL` to leave that area alone (an OOB-only program is
  the best-effort bad-block marker). `EraseBlock` returns 0 or -1. A failed program or erase makes the library
  mark the block bad and use another one.
* Blocks are numbered `0..BlockCount-1` across all chips; chip select is the HAL's business. All calls are made
  under the library lock.

### Geometry

| `EEFS_NandGeometry_t` field | Target value | Rule |
|---|---|---|
| `PageSize` | 4096 | power of two, 2048 .. `EEFS_NAND_MAX_PAGE_SIZE` (default 4096); 512 is rejected |
| `OobSize` | 224 | 1 .. `EEFS_NAND_MAX_OOB_SIZE` (default 224) |
| `PagesPerBlock` | 128 (512 KiB blocks) | room for a directory checkpoint plus a transaction: >= 16 at 4096-byte pages (4096 blocks, 64 files) |
| `BlockCount` | up to 4096 | `DirRegionBlocks + 1` .. `EEFS_NAND_MAX_BLOCKS` (default 4096) |
| `DirRegionBlocks` | 16 | even, 4 .. `EEFS_NAND_MAX_DIR_BLOCKS` (32): two mirrored rings of directory blocks |
| `BadMarkerOffset`, `BadMarkerPages` | 0, `EEFS_NAND_BADMARKER_PAGE0` | where the factory bad-block marker is |
| `EccScrubThreshold` | part-specific | corrected bits per page that make the health check suggest a rewrite; 0 = never |

A file occupies whole blocks; the first page of its first block is a metadata page with its path, type, version,
size, CRCs and the list of its blocks. The largest file is 213.5 MiB at the target geometry. The limits and buffer
sizes are compile-time macros in `inc/eefs_config.h` (all `-D` overridable); a volume needs about 62 KB of RAM.

### Batches and commits

Files are written whole with `EEFS_WriteFile(Path, &Meta, Data, Size)`; `Meta` (`EEFS_NandFileInfo_t`) carries the
file type (`OTHER`, `BOOT_FIT`, `BOOT_ELF`, `BOOT_DTB`, `CFS_APP`, `CFS_LIB`), the version, the slot and the
type-specific fields (destination address, ELF entry override, cFS name, entry symbol, priority, stack size, load
address). Reading uses `EEFS_Open`/`EEFS_Read` (read-only), `EEFS_Stat` and `EEFS_GetFileInfo`.

Every change (write, remove, rename, attributes, A/B settings) goes into a **batch** that is invisible on the media
until `EEFS_Commit` makes the whole batch durable at once with one directory record (written to both mirrors);
`EEFS_Abort` discards it and `EEFS_UnMount` commits it. A file is never rewritten in place: a rewrite writes a new
first block and new blocks only for the data that changed, so **a power cut at any point leaves either the old or
the new state, never a mixed or torn file**. Free space: `EEFS_GetVolumeInfo`; a small reserve of blocks is held
back so existing files can always be rewritten.

### A/B software loads

A file can have two copies, A and B, selected by a name suffix (`/boot/kernel.elf@B`) or `Meta.Slot`. The plain
name resolves to the active copy. The typical update:

```c
EEFS_WriteFile("/NAND0/boot/kernel.elf", &Meta /* Slot = EEFS_NAND_SLOT_B, Version = 2 */, Image, Size);
/* ... every file of the new load ... */
EEFS_SetActiveSlotAll("/NAND0", EEFS_NAND_SLOT_B);      /* boot B next time */
EEFS_SetFallbackCompatibleAll("/NAND0", 1);             /* A may be loaded if a B copy is bad */
EEFS_Commit("/NAND0");                                  /* all of it, atomically */
```

If an active copy fails its CRC the bootloader falls back to the other copy (when `FallbackCompatible` is set) and
reports `ActiveFailed`; the application records that with `EEFS_SetFailed` and decides to roll back
(`EEFS_SetActiveSlotAll(.., A)`), repair (`EEFS_CopySlot`/`EEFS_CopySlotAll` from the good copy) or promote a tested
load (`EEFS_CopySlotAll(.., B, A)`), followed by a commit.

### Health check

`EEFS_HealthCheckStart`, then `EEFS_HealthCheckStep(MountPoint, MaxPageReads)` until it returns `EEFS_SUCCESS`,
then `EEFS_HealthCheckSummary`. It reads the whole volume (directory mirrors, every data page against its CRCs, A/B
state, orphans, bad-block markers, free space) without writing, holds the lock only for `MaxPageReads` page reads
per step and restarts by itself when the volume changes, so it is meant to run after every power-on in a
low-priority task next to the writers. Findings (codes in `inc/eefs_nand_types.h`) are deduplicated and summarized
in a bit mask; a corrupt active copy is flagged `ActiveFailed` for the next commit. Repairs: `EEFS_CopySlot`,
rewriting a file, `EEFS_ReclaimOrphans` (erases unreferenced metadata pages), `EEFS_RepairDirectory` and
`EEFS_RebuildDirectory`.

### Locking

The library holds one library-wide lock around every call. By default it is empty (`EEFS_LOCK=none`, one task).
When several tasks use EEFS (for example the background health check plus writer tasks), configure:

* `-DEEFS_LOCK=rtems` (RTEMS 6/7 only): EEFS provides the lock, `drivers/standalone_driver/eefs_rtems_lock.c`, a
  statically initialized RTEMS recursive mutex with **priority inheritance**. There is nothing to create or
  initialize and no Classic semaphore to configure. `EEFS_RtemsLockWaits()` counts contended acquisitions.
* `-DEEFS_LOCK=hooks`: the application provides `void EEFS_LibLockHook(void)` and `void EEFS_LibUnlockHook(void)`,
  implementing one recursive mutex.

Both require the standalone driver; the definitions they add are exported with the `eefs` target.

### Bootloader library

`microeefs` (`libraries/microeefs_nand/microeefs_nand.h`) loads a file with only the HAL's `ReadPage`, the geometry,
one page buffer and about 1.5 KB of stack, and **never writes**:

```c
MicroEEFS_NandInfo_t Info;
int32 rc = MicroEEFS_NandLoadFile(&Dev, PageBuf, "/boot/image.fit", (void *)Dest, MaxSize, &Info);
/* Info.Info.Type.Fit.DestAddress, Info.LoadedSlot, Info.Info.ActiveFailed -> hand to the OS via boot info */
```

It finds the file through the directory (or, if no directory survives, by scanning metadata pages), applies the A/B
choice and the compatible fallback, and checks every block CRC and the file CRC. `MicroEEFS_NandSetSlotHook` lets a
BSP override the slot choice (for example a boot counter).

### NAND assumptions

* **NOP = 1**: a page is programmed once between erases (the library never re-programs a page; the only exception
  is the best-effort OOB bad-block marker of a block being retired).
* **In-order programming**: the pages of a block are programmed in ascending order.
* **Erased pages read `0xFF`** with a non-error status.
* **The OOB bad-block marker byte is programmable** on its own, outside the ECC bytes (best effort: the directory
  records the bad block either way).

### Known limitations

* Files are written whole from RAM, and the lock is held for the whole write (a large image blocks other EEFS
  calls for its duration).
* There is no POSIX mount: use the standalone driver API (`EEFS_Open`, `EEFS_WriteFile`, ...).
* All testing uses a RAM NAND simulator (host and QEMU); a real controller must meet the HAL contract above.
