# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

EEFS (EEPROM File System) v2.0.0.0 is NASA GSFC's slot-based file system for memory-mapped
EEPROM/RAM/ROM. It's used to boot cFS/cFE targets on RTEMS, vxWorks or bare metal. This repo is
the open-source baseline release (NASA Open Source Agreement): plain C89, no tests, no CI,
no top-level build. `thesamprice/eefs` is a fork. The default branch is `master`.

The authoritative design description is the big comment block at the top of
`inc/eefs_fileapi.h`. `doc/EEPROM File System Design Notes.docx` covers the same ground in more depth.

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

Each tool has its own GNU Makefile, written for a 32-bit x86 Linux host circa 2011.

```sh
cd tools/geneepromfs && make            # builds ./geneepromfs
cd tools/eefstool && make eefstool-img  # default 'all' target is the BDM build
```

Known problems on a modern 64-bit host (including this arm64 Mac):

- **`-m32` is mandatory for correctness, not just style.** For `_ix86_`, `common_types.h`
  typedefs `int32`/`uint32` as `long`, which is 8 bytes on LP64. Without `-m32` the build
  produces images with 8-byte fields that targets can't read. (Verified: the magic lands at
  offset 8 instead of 4.) macOS arm64 can't build `-m32` at all. Any real port must switch those
  typedefs to `<stdint.h>` (`int32_t`/`uint32_t`) first, then drop `-m32`.
- With `-Werror`, `geneepromfs.c:259/269` fail on `%d` with a `size_t`/`unsigned long` argument.
- In `eefstool/Makefile`, `INCPATH` has a bad `-I../../inc:../../drivers/standalone_driver`
  (a colon instead of a second `-I`). The BDM target also links `/opt/rtems-4.10/lib/libBDM.a`,
  `-ltblcf` and `-lusb`, which aren't present here.
- The Makefiles' `clean` targets don't remove the built binaries (`geneepromfs`). Don't commit
  build outputs.

To cross-compile the library and drivers for a target, add `libraries/eepromfs/eefs_fileapi.c`
plus the chosen driver to the target build, with `-I inc -I drivers/<driver>` and the right arch
define for `common_types.h`.

## Conventions

- Keep the existing NASA style: the license banner at the top of each file, `/* */` comments
  only (`-ansi`), `EEFS_` prefixes, the project's `int32`/`uint32` types, and return codes from
  the `EEFS_*` error list in `eefs_fileapi.h`.
- On-media structs are big-endian by convention (geneepromfs default). Host tools must byte-swap
  through `eefs_swap.c`.
- No test suite exists. To validate a change, build geneepromfs, generate an image from a small
  input file in a scratch directory, and inspect it with `xxd` or `eefstool-img`.
