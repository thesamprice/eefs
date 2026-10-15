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

Each tool has its own GNU Makefile. Both host tools build natively on 64-bit hosts, including
this arm64 Mac (the original `-m32` requirement is gone).

```sh
cd tools/geneepromfs && make            # builds ./geneepromfs (-Werror)
cd tools/eefstool && make eefstool-img  # default 'all' target is the BDM build
```

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
- Regression check: an image from the 64-bit `geneepromfs` is byte-identical to one from the
  original code built `-m32` in `i386/debian:bookworm` Docker (`-t` fixes the timestamp).

Remaining known problems:

- `libraries/page_window/` and the BDM driver still cast pointers to `uint32`. They only build
  for their 32-bit targets. The BDM target also links `/opt/rtems-4.10/lib/libBDM.a`, `-ltblcf`
  and `-lusb`, which aren't present here.
- `eefstool-img --copy_to` adds a FAT slot that `--dir` doesn't list, and `--rename`/`--delete`
  are no-ops. This is existing behavior: the original 32-bit build does exactly the same.
  Reads (`--dir`, `--usage`, `--copy_from`) work.
- The Makefiles' `clean` targets don't remove the built binaries. Don't commit build outputs.

To cross-compile the library and drivers for a target, add `libraries/eepromfs/eefs_fileapi.c`
plus the chosen driver to the target build, with `-I inc -I drivers/<driver>` and the right arch
define for `common_types.h`.

## Conventions

- Keep the existing NASA style: the license banner at the top of each file, `/* */` comments
  only (`-ansi`), `EEFS_` prefixes, the project's `int32`/`uint32`/`cpuaddr` types, and return
  codes from the `EEFS_*` error list in `eefs_fileapi.h`.
- On-media structs are big-endian by convention (geneepromfs default). Host tools must byte-swap
  through `eefs_swap.c`.
- No test suite exists. To validate a change, build geneepromfs, generate an image from a small
  input file in a scratch directory, and inspect it with `xxd` or `eefstool-img`.
