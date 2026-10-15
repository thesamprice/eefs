# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

EEFS (EEPROM File System) v2.0.0.0 is NASA GSFC's slot-based file system for memory-mapped
EEPROM/RAM/ROM. It's used to boot cFS/cFE targets on RTEMS, vxWorks or bare metal. This repo is
the open-source baseline release (NASA Open Source Agreement), in plain C89, with a CMake
build and CTest tests added on top. The target platform is RTEMS 6/7. `thesamprice/eefs` is a fork. The default branch is `master`.

The authoritative design description is the big comment block at the top of
`inc/eefs_fileapi.h`. `doc/EEPROM File System Design Notes.docx` covers the same ground in more depth.

The branch `nand-flash` adds a second storage backend for raw NAND flash (`-DEEFS_BACKEND=nand`), described in
the "NAND backend" section below. Its specs are `docs/nand-format.md` (on-media format, normative) and
`docs/nand-implementation.md` (library design, return codes, tests, decisions in section 17).

## Layout and how the pieces fit

- `inc/`: shared public headers. `eefs_fileapi.h` holds the on-media structs, error codes and
  the `EEFS_Lib*` API. `eefs_config.h` holds the compile-time limits (`EEFS_MAX_FILES` = 64,
  `EEFS_MAX_OPEN_FILES` = 20). `common_types.h` holds the per-arch integer typedefs, selected by
  `-D_ix86_`, `__PPC__`, `_m68k_` or `__SPARC__`.
- `libraries/eepromfs/eefs_fileapi.c`: the core, volume-independent library (`EEFS_Lib*`
  functions that operate on an inode table).
- `drivers/`: OS bindings on top of the core.
  - `standalone_driver/`: `EEFS_InitFS/Mount/Open/Read/...` with device and mount tables, for
    systems with no OS file system.
  - `vxworks_driver/`: vxWorks device driver.
  - `rtems_driver/`: registers `"eefs"` with `rtems_filesystem_register` (`init_eefs.c`).
    **It's written against the RTEMS 4.10 filesystem API** (`evalpath_h`, `evalformake_h`,
    `node_type_h`), which RTEMS 4.11 replaced, so it doesn't build on RTEMS 6/7. CMake rejects
    `EEFS_DRIVER=rtems`. On RTEMS 6/7, use the standalone driver. Hooking EEFS into the RTEMS
    POSIX file API would need this driver ported to the current `rtems_filesystem_operations_table`.
  - Each driver has its **own `eefs_macros.h`**, which is the porting layer: EEPROM
    read/write/flush (default `memcpy`), lock/unlock, time and write-protect. The copies
    differ, so change the one that belongs to the target you're working on.
- `libraries/microeefs/`: `MicroEEFS_FindFile(base, name)`, a single-function lookup for
  bootloaders.
- `libraries/page_window/`: page-windowed EEPROM write helper for hardware that needs it.
- `tools/geneepromfs/`: host tool that builds an image from an input list
  (`input.dat` format: `src, eefsName, spareBytes, attribute;`, with `!` comments).
  `geneepromfs [-e big|little] [-s size] [-t time] [-f] [-v] [-m map] INPUT OUTPUT`.
  The default output is big-endian.
- `tools/eefstool/`: host tool that inspects and edits images (`eefstool-img`) or live EEPROM
  over BDM (`eefstool-bdm`). It carries a **forked copy** of `eefs_fileapi.c` in `src/` that
  differs from `libraries/eepromfs/` only by `EEFS_Swap*` byte-swap calls (marked `/* APC */`).
  Fixes to the core library usually need to be mirrored there.
  `tools/geneepromfs/eefs_config.h` is likewise a copy of `inc/eefs_config.h`.

On-media layout: File Allocation Table header (magic `0xEEF51234`, CRC, counts), then
`EEFS_MAX_FILES` FAT entries (relative offset + max slot size), then slots of
`File Header + data + spare`. Offsets are relative, so an image is position-independent.
CRCs are not maintained by the library. Only the boot code checks the FAT CRC.

## Building

CMake (3.21+) is the only build system. The original per-tool Makefiles were removed.

```sh
# Host: libeefs.a, libmicroeefs.a, tools/geneepromfs, tools/eefstool-img, plus 4 CTest tests
cmake -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure

# RTEMS 7 cross build (the local RTEMS 7 install at ~/rtems/7 has arm, i386, riscv and microblaze
# toolchains). This also builds tests/rtems/eefs_rtems_test.exe and runs it under QEMU.
cmake -B build-rtems -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/rtems-toolchain.cmake \
      -DRTEMS_PREFIX=$HOME/rtems/7 -DRTEMS_VERSION=7 -DRTEMS_ARCH=arm -DRTEMS_BSP=xilinx_zynq_a9_qemu
cmake --build build-rtems && ctest --test-dir build-rtems --output-on-failure
```

- Targets: `eefs` (core plus the `EEFS_DRIVER` driver), `microeefs`, `geneepromfs`,
  `eefstool-img`, and `eefstool-bdm` (behind `EEFS_BUILD_BDM_TOOL`). They're exported as `eefs::*` for
  `find_package(eefs)`.
- `EEFS_MACROS_DIR` selects the `eefs_macros.h` porting layer. `EEFS_ARCH_DEFINE` is optional:
  `common_types.h` falls back to the compiler's `__BYTE_ORDER__` for any CPU without its own block.
- `cmake/rtems-toolchain.cmake` reads `ABI_FLAGS` from the BSP's
  `lib/pkgconfig/<arch>-rtems<ver>-<bsp>.pc`, so any RTEMS 6/7 BSP works without pkg-config.
  Host tools default to OFF in cross builds.
- Tests (`tests/`): `golden/` holds images and a map file generated by the **original, unmodified
  release built `-m32` on i386**. The current tools must reproduce them byte for byte. Don't
  regenerate the golden files with the current tools. `tests/rtems/` embeds `golden/little.img`
  and checks reads, stat, `MicroEEFS_FindFile`, and create/write on the target.
- Endianness: the target library reads images natively, without byte swapping, so a
  little-endian target (ARM, RISC-V) needs `geneepromfs -e little`. The default is big-endian.

64-bit portability rules (keep them when editing):

- `common_types.h` defines `int8`…`uint64` from `<stdint.h>`. The arch blocks only set
  endianness and `boolean`. Never reintroduce `long`-based typedefs: on LP64 they make on-media
  fields 8 bytes and corrupt images.
- Memory addresses use `cpuaddr` (`uintptr_t`): `InodeTable->BaseAddress` and the `BaseAddress`
  parameters of `EEFS_LibInitFS`, `EEFS_InitFS`, `EEFS_DevCreate` and `MicroEEFS_FindFile`.
  Pointer-to-offset math is done as `(uint32)((cpuaddr)ptr - BaseAddress)`.
- On-media structs contain only fixed-width types. `EEFS_FileHeader_t` dates are `uint32`
  (not `time_t`, which is 64-bit on modern hosts and on RTEMS 5+). `EEFS_Stat_t` keeps `time_t`.
- `printf` of `uint32` values casts to `(unsigned long)` with `%lu`/`%lx`, and pointers use `%p`.

Remaining known problems:

- `libraries/page_window/` and the BDM driver still cast pointers to `uint32`. They only build
  for their 32-bit targets. The BDM target also links `/opt/rtems-4.10/lib/libBDM.a`, `-ltblcf`
  and `-lusb`, which aren't present here.
- `eefstool-img --copy_to` adds a FAT slot that `--dir` doesn't list, and `--rename`/`--delete`
  are no-ops. This is existing behavior: the original 32-bit build does exactly the same.
  Reads (`--dir`, `--usage`, `--copy_from`) work.
- The vxWorks driver (`EEFS_DRIVER=vxworks`) is wired into CMake but untested, because no
  vxWorks toolchain is available.

## Conventions

- Keep the existing NASA style: the license banner at the top of each file, `/* */` comments
  only (`-ansi`), `EEFS_` prefixes, the project's `int32`/`uint32`/`cpuaddr` types, and return
  codes from the `EEFS_*` error list in `eefs_fileapi.h`.
- On-media structs are big-endian by convention (geneepromfs default). Host tools must byte-swap
  through `eefs_swap.c`.
- Validate changes with both the host `ctest` run and the RTEMS QEMU `ctest` run.

## NAND backend

Selected with `-DEEFS_BACKEND=nand` (default `eeprom`). It reuses the `EEFS_Lib*` API, the standalone driver and the
`eefs`/`microeefs` targets, but the sources are different: none of `libraries/eepromfs`, `libraries/microeefs` or
`tools/` is built.

### Module map

- `inc/eefs_nand_hal.h`: geometry, `EEFS_NandOps_t` (ReadPage/ProgramPage/EraseBlock) and its contract, file types,
  slots, `EEFS_NandFileInfo_t`. `inc/eefs_nand_format.h`: every on-media offset and constant (computed from the
  sizes in `eefs_config.h`), decoded structs, encode/decode prototypes. `inc/eefs_nand_types.h`: the volume
  (`EEFS_InodeTable_t`: `Base` = committed, `Working` = pending directory state), health finding codes and report
  types; included by `eefs_fileapi.h` under `EEFS_BACKEND_NAND` (the EEPROM types moved verbatim to
  `inc/eefs_eeprom_types.h`). `inc/eefs_nand.h`: the NAND-only `EEFS_Lib*` calls. `inc/eefs_config.h`: the
  `EEFS_NAND_*` limits, all `#ifndef`-guarded.
- `libraries/eefs_nand/`: `eefs_crc32.c` (CRC-32), `eefs_nand_format.c` (pure serialization, no I/O),
  `eefs_nand_io.c` (HAL wrappers: cached read, verified program, erase, bad marking), `eefs_nand_journal.c`
  (format, mount, the mirrored directory log, commit/abort/resync/cleanup), `eefs_nand_rebuild.c` (rebuild from
  metadata pages, ReclaimOrphans), `eefs_nand_fileapi.c` (the API: allocator, copy-on-write writes, A/B, open/read,
  descriptors), `eefs_nand_health.c` (the resumable health check). `eefs_nand_internal.h` is shared by these and by
  the host tests.
- `libraries/microeefs_nand/`: the bootloader library (read only; links only `microeefs_nand.c`,
  `eefs_nand_format.c`, `eefs_crc32.c`).
- `drivers/standalone_driver/`: `eefs_filesys.[ch]` gain the NAND wrappers (`EEFS_Format`, `EEFS_WriteFile`,
  `EEFS_Commit`, A/B, health; they return the `EEFS_Lib*` code unchanged) under `#ifdef EEFS_BACKEND_NAND`;
  `eefs_rtems_lock.[ch]` is the RTEMS lock of `EEFS_LOCK=rtems`; `eefs_macros.h` routes the lock to hooks when
  `EEFS_LIB_LOCK_HOOKS` is defined.
- `tests/nand/`: host tests. `nand_sim.[ch]` is the RAM NAND simulator (fault injection, power-cut budget with three
  torn modes, strict NOP/order checking); `nand_fixture.[ch]` builds volumes with the encoders only (bootloader
  tests); `nand_vol.[ch]` wraps a library volume; `test_<suite>.c` per suite. `tests/rtems/eefs_rtems_nand_test.c`
  is the RTEMS end-to-end test (it also compiles `tests/nand/nand_sim.c`).

### Build and test every configuration

Always build with `--clean-first` before reporting results, and run ctest serially (never two suites at once).

```sh
R="-DCMAKE_TOOLCHAIN_FILE=cmake/rtems-toolchain.cmake -DRTEMS_PREFIX=$HOME/rtems/7 -DRTEMS_ARCH=arm -DRTEMS_BSP=xilinx_zynq_a9_qemu"
# (bash; in zsh write the flags out, $R does not split)
cmake -B build -G Ninja                                    # EEPROM host: 4 golden tests, seconds
cmake -B build-nand -G Ninja -DEEFS_BACKEND=nand           # NAND host: 30 tests, about 6 minutes
cmake -B build-rtems -G Ninja $R                           # EEPROM RTEMS QEMU: 1 test, about 1 s
cmake -B build-rtems-nand -G Ninja $R -DEEFS_BACKEND=nand  # NAND RTEMS QEMU: 1 test, about 2 s
cmake -B build-rtems-nand-lock -G Ninja $R -DEEFS_BACKEND=nand -DEEFS_LOCK=rtems   # the shipped RTEMS lock
# for each:  cmake --build <dir> --clean-first && ctest --test-dir <dir> --output-on-failure
```

- NAND host tests: suites `layout sim journal micro fileops powercut ab rebuild health` on geometries 4096/224/128,
  2048/64/64 and 8192/448/64 (27 tests), plus `nand_default_config_4096` (fileops against the real `eefs` target),
  `nand_verify_on_open_4096` and `nand_verify_on_open_ab_4096` (a test library built with
  `EEFS_NAND_VERIFY_ON_OPEN=1`). The power-cut sweeps dominate the six minutes; the test code is built `-O2` when no
  build type is set. Run one suite or case directly:
  `build-nand/tests/nand/eefs_nand_test <suite> 4096 224 128 4096 [case-substring]`.
- `EEFS_LOCK=hooks` on the host: `nand_default_config_4096` then runs with counting hooks from
  `tests/nand/nand_test_main.c`, which fail a case that leaves the lock held.
- The RTEMS NAND test must print `EEFS RTEMS TEST PASSED` within QEMU's 60 s timeout (it takes about 2 s). It always
  uses the EEFS RTEMS lock: the `eefs` target itself with `EEFS_LOCK=rtems`, else the test-private
  `eefs_rtems_locked` copy built from the same sources.
- Cross-build checks without QEMU: the same configure with `-DRTEMS_ARCH=i386 -DRTEMS_BSP=pc386` or
  `-DRTEMS_ARCH=riscv -DRTEMS_BSP=esp32c3db` (both backends, and `EEFS_LOCK=rtems`); `eefs` and `microeefs` must build
  without warnings. The NAND RTEMS test needs several MB of RAM, so without a QEMU command it is excluded from `all`
  (esp32c3db cannot link it); build it by name (`--target eefs_rtems_test.exe`) to check that it compiles.
- EEPROM byte-identity check: build the default `eeprom` configuration (host and arm) from the previous commit
  (`git archive`) and from the tree, and `cmp` `libeefs.a`, `libmicroeefs.a` and the tool objects.

### Invariants future edits must keep

- **Power-loss ordering.** A change reaches the media in this order: new data blocks (positions 1..n-1), then the
  new first block (page 0 first, then its data pages), then at commit the directory record to mirror 0, then to
  mirror 1, then the cleanup erases of superseded first blocks. A commit is atomic at the record: a torn record is
  ignored and the previous state stands. The power-cut sweeps (`powercut`, `journal`, `ab`, `rebuild`, `health`)
  check exactly-old-or-new after every program and erase in three torn modes; keep them passing and extend them for
  any new write path.
- **Never write in place.** A live slot's blocks are never erased or programmed again (copy-on-write: new first
  block always, new blocks for changed data, unchanged blocks kept after a length + CRC + byte compare). Pages are
  programmed once per erase, in ascending order (the simulator's strict mode fails anything else). The allocator
  never hands out a block that is not FREE in both `Working` and `Base`.
- **Authority.** Page 0 (the metadata page) is authoritative for a file's content (size, CRCs, block list); the
  directory is authoritative for which files exist and for the A/B state (`IsAB`, `ActiveSlot`,
  `FallbackCompatible`, `ActiveFailed`). Mount reconciles the copied fields from page 0; a rebuild without a
  directory can recover files but resets the A/B flags.
- **The bootloader never writes.** `microeefs_nand` calls only `ReadPage`, uses only the caller's page buffer and
  about 1.5 KB of stack, and has no globals except the slot hook. Keep it free of library dependencies beyond
  `eefs_nand_format.c` and `eefs_crc32.c`.
- **The format is versioned.** Everything on the media is serialized field by field, little-endian, through
  `eefs_nand_format.c` (no structs on the media). Any change to a format-affecting size (`EEFS_NAND_MAX_PATH_SIZE`,
  `EEFS_NAND_CFS_NAME_SIZE`, `EEFS_NAND_CFS_SYMBOL_SIZE`) or layout needs a new `EEFS_NAND_LAYOUT_VERSION` and an
  update of `docs/nand-format.md`; the `layout` suite pins every offset to the document's numbers.
- **The EEPROM build stays byte-identical.** NAND code lives under `EEFS_BACKEND_NAND` or in NAND-only sources; with
  the default options an `eeprom` configure must compile the same sources with the same definitions to the same
  objects, and the golden tests must pass unchanged.
- One library-wide recursive lock guards everything (the descriptor tables and the write job's static scratch are
  shared by all volumes); every `EEFS_LIB_LOCK` must be paired with exactly one `EEFS_LIB_UNLOCK` on every path. The
  health check holds it only inside a step and must never write NAND.
- Docs and headers move together: finding codes, error codes and `EEFS_NAND_*` macros in the headers must match
  `docs/nand-implementation.md` (sections 2 and 10), and on-media numbers must match `docs/nand-format.md`.
