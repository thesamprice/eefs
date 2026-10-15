# EEFS NAND backend: implementation specification

This is the internal specification for the NAND backend of EEFS. Engineers implement it phase by
phase (section 14); every interface between phases is pinned here. The on-media format is in
`docs/nand-format.md` (referred to as *FMT*), which is normative for everything written to NAND.
The user-approved requirements are the plan in the PR description; deviations from it are listed in
section 15.

Conventions for all new code: C89 (`-std=c90`, `/* */` comments), NASA banner, `EEFS_` prefixes,
`int8..uint64`/`cpuaddr`/`boolean` from `common_types.h`, no `malloc` in library or bootloader
code, warning-free with `-Wall -W -Wstrict-prototypes -Wundef` on clang (macOS arm64) and
`arm-rtems7-gcc`. `printf` of `uint32` casts to `(unsigned long)`.

## 1. Build and source layout

### 1.1 Files

New:

| File | Contents |
|---|---|
| `inc/eefs_eeprom_types.h` | EEPROM on-media and inode types moved verbatim from `eefs_fileapi.h` (section 2.1) |
| `inc/eefs_nand_hal.h` | HAL: geometry, ops, device, read status codes, file type / slot constants, `EEFS_NandTypeInfo_t`, `EEFS_NandFileInfo_t` |
| `inc/eefs_nand_format.h` | FMT constants (offsets, magic, item types, map values), decoded structs (`EEFS_NandEntry_t`, `EEFS_NandSlot_t`, `EEFS_NandPage0_t`, `EEFS_NandRecHdr_t`, `EEFS_NandVolumeItem_t`), put/get, encode/decode and CRC prototypes |
| `inc/eefs_nand_types.h` | NAND `EEFS_InodeTable_t` (the volume RAM), `EEFS_FileDescriptor_t`, mirror/chain state, health report types and finding codes (they are embedded in the volume, so they live here, not in `eefs_nand.h`) |
| `inc/eefs_nand.h` | NAND public API: new `EEFS_Lib*` prototypes. `MicroEEFS` is *not* here |
| `libraries/eefs_nand/eefs_crc32.c` | CRC-32 (table-driven, 1 KiB table in `.rodata`) |
| `libraries/eefs_nand/eefs_nand_format.c` | Pure encode/decode of FMT structures, item iterator. No I/O, no state. |
| `libraries/eefs_nand/eefs_nand_internal.h` | Internal prototypes shared by the library's .c files (and included by host tests) |
| `libraries/eefs_nand/eefs_nand_io.c` | HAL wrappers: cached page read, program, erase, bad marking, erased-page test |
| `libraries/eefs_nand/eefs_nand_journal.c` | Directory: format, dir scan, replay, commit, abort, resync, cleanup, ring, RepairDirectory |
| `libraries/eefs_nand/eefs_nand_rebuild.c` | Rebuild from metadata pages, RebuildDirectory, ReclaimOrphans |
| `libraries/eefs_nand/eefs_nand_fileapi.c` | `EEFS_Lib*` API: mount, file ops, A/B, descriptors, readdir, stat |
| `libraries/eefs_nand/eefs_nand_health.c` | Resumable health check |
| `libraries/microeefs_nand/microeefs_nand.h`, `microeefs_nand.c` | Bootloader library |
| `tests/nand/nand_sim.h`, `nand_sim.c` | RAM NAND simulator |
| `tests/nand/nand_test.h`, `nand_test_main.c` | Test harness (CHECK macros, suite table, geometry from argv) |
| `tests/nand/test_*.c` | Test suites (section 14) |
| `tests/nand/CMakeLists.txt` | Registers the host tests per geometry |
| `tests/rtems/eefs_rtems_nand_test.c` | RTEMS QEMU test for the NAND backend |

Modified: `inc/eefs_fileapi.h`, `inc/eefs_config.h`, `drivers/standalone_driver/eefs_filesys.[ch]`,
`CMakeLists.txt`, `tests/rtems/CMakeLists.txt`, `CLAUDE.md`, `README.md`. (`tools/CMakeLists.txt` needs no
change: the top-level `CMakeLists.txt` does not add `tools/` for the NAND backend.)

Untouched: `libraries/eepromfs/`, `libraries/microeefs/`, `tools/`, `tests/golden/`, `tests/data/`,
`tests/run_test.cmake`, `tests/CMakeLists.txt`, `tools/geneepromfs/eefs_config.h`.

### 1.2 CMake

`CMakeLists.txt`:

```cmake
set(EEFS_BACKEND "eeprom" CACHE STRING "EEFS storage backend: eeprom or nand")
set_property(CACHE EEFS_BACKEND PROPERTY STRINGS eeprom nand)
```

| | `eeprom` (default, unchanged) | `nand` |
|---|---|---|
| `eefs` sources | `libraries/eepromfs/eefs_fileapi.c` + driver | `EEFS_NAND_LIB_SOURCES` + driver |
| `microeefs` sources | `libraries/microeefs/microeefs.c` | `libraries/microeefs_nand/microeefs_nand.c` (links `eefs_nand_format.c`, `eefs_crc32.c` too) |
| `eefs_headers` definitions | none | `EEFS_BACKEND_NAND` (INTERFACE, so every consumer sees it) |
| `microeefs` include dir | `libraries/microeefs` | `libraries/microeefs_nand` |
| Host tools (`tools/`) | as before | not built; `EEFS_BUILD_TOOLS` forced OFF with a `message(STATUS)` |
| `tests/` (golden) | as before | not added |
| `tests/nand/` | not added | added when `BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING` |
| `tests/rtems/` | `eefs_rtems_test.c` + embedded image | `eefs_rtems_nand_test.c` + `tests/nand/nand_sim.c` |
| Installed headers | as before + `inc/eefs_eeprom_types.h` | shared + `eefs_nand*.h` + `microeefs_nand.h` |

```cmake
set(EEFS_NAND_LIB_SOURCES
    libraries/eefs_nand/eefs_crc32.c
    libraries/eefs_nand/eefs_nand_format.c
    libraries/eefs_nand/eefs_nand_io.c
    libraries/eefs_nand/eefs_nand_journal.c
    libraries/eefs_nand/eefs_nand_rebuild.c
    libraries/eefs_nand/eefs_nand_fileapi.c
    libraries/eefs_nand/eefs_nand_health.c)
```

`EEFS_DRIVER` must be `standalone` when `EEFS_BACKEND` is `nand` (fatal error otherwise). The
`eefs` and `microeefs` targets keep `C_STANDARD 90`, `C_EXTENSIONS OFF`, `EEFS_WARNING_FLAGS`.
`cmake/rtems-toolchain.cmake` needs no change. Phase 1 must verify that an `eeprom` configure
produces an identical build (same source lists, same definitions) and that all four golden tests
pass.

`tests/nand/CMakeLists.txt` builds one static library `eefs_nand_testlib` from
`${EEFS_NAND_LIB_SOURCES}` + `drivers/standalone_driver/eefs_filesys.c` + `nand_sim.c` with
`-DEEFS_BACKEND_NAND -DEEFS_NAND_MAX_PAGE_SIZE=8192 -DEEFS_NAND_MAX_OOB_SIZE=448` (so one binary
serves all geometries and the override path is exercised), one executable `eefs_nand_test` from
`nand_test_main.c` + all `test_*.c`, a second executable `eefs_nand_test_default` built the same
way but linked against the real `eefs` target (default maxima) that is only registered for the
target geometry, and a third, `eefs_nand_test_verifyopen`, linked against a second copy of the
test library (`eefs_nand_testlib_verifyopen`) compiled with `-DEEFS_NAND_VERIFY_ON_OPEN=1`, so that
the optional open-time data verification keeps being built and tested although it is off by
default. Tests are registered as:

```cmake
set(EEFS_NAND_GEOMETRIES 4096:224:128 2048:64:64 8192:448:64)   # PageSize:OobSize:PagesPerBlock
set(EEFS_NAND_TEST_BLOCKS 4096)
foreach(geom_text IN LISTS EEFS_NAND_GEOMETRIES)
    string(REPLACE ":" ";" geom ${geom_text})
    list(GET geom 0 page) ... 
    foreach(suite IN LISTS EEFS_NAND_SUITES)   # layout sim journal fileops powercut rebuild ab health micro
        add_test(NAME nand_${suite}_${page}_${oob}_${ppb}
                 COMMAND eefs_nand_test ${suite} ${page} ${oob} ${ppb} ${EEFS_NAND_TEST_BLOCKS})
    endforeach()
endforeach()
add_test(NAME nand_default_config_4096 COMMAND eefs_nand_test_default fileops 4096 224 128 4096)
add_test(NAME nand_verify_on_open_4096 COMMAND eefs_nand_test_verifyopen fileops 4096 224 128 4096)
add_test(NAME nand_verify_on_open_ab_4096 COMMAND eefs_nand_test_verifyopen ab 4096 224 128 4096)
```

`eefs_nand_test <suite> <page> <oob> <ppb> <blocks> [case-substring]` runs every case of the suite
(or those whose name contains the substring), prints `PASS`/`FAIL` per case and exits non-zero on
any failure. Suites are added to `EEFS_NAND_SUITES` as the phases land.

## 2. Headers

### 2.1 `inc/eefs_fileapi.h` split

The file keeps, unchanged and in the same order: the banner and design comment, the includes
(`common_types.h`, `eefs_config.h`, `eefs_version.h`, `<fcntl.h>`, `<stdio.h>`, `<time.h>`),
`EEFS_FILESYS_MAGIC`, the attribute and mode macros, the error codes, `EEFS_DirectoryDescriptor_t`,
`EEFS_DirectoryEntry_t`, `EEFS_Stat_t` and all `EEFS_Lib*` prototypes.

Changed:

```c
#ifdef EEFS_BACKEND_NAND
#define EEFS_MAX_FILENAME_SIZE          EEFS_NAND_MAX_PATH_SIZE
#else
#define EEFS_MAX_FILENAME_SIZE          40
#endif
...
/* after the error codes, before the shared types */
#ifdef EEFS_BACKEND_NAND
#include "eefs_nand_types.h"      /* EEFS_InodeTable_t, EEFS_FileDescriptor_t for NAND */
#else
#include "eefs_eeprom_types.h"    /* EEFS_FileAllocationTable*_t, EEFS_FileHeader_t, EEFS_InodeTable*_t, EEFS_FileDescriptor_t */
#endif
```

`inc/eefs_eeprom_types.h` receives, byte for byte, these typedefs from the current header:
`EEFS_FileAllocationTableHeader_t`, `EEFS_FileAllocationTableEntry_t`,
`EEFS_FileAllocationTable_t`, `EEFS_FileHeader_t`, `EEFS_InodeTableEntry_t`, `EEFS_InodeTable_t`,
`EEFS_FileDescriptor_t`. `EEFS_DirectoryDescriptor_t`, `EEFS_DirectoryEntry_t` and `EEFS_Stat_t`
stay in `eefs_fileapi.h` after the include (they reference `EEFS_InodeTable_t` and
`EEFS_MAX_FILENAME_SIZE`, both defined by then). The preprocessed EEPROM translation unit is
therefore identical to today's except for line numbers; the golden tests prove the tools unchanged.

New error codes, appended unconditionally after `EEFS_READ_ONLY_FILE_SYSTEM`:

```c
#define EEFS_IO_ERROR                (-11)   /* uncorrectable ECC, read/program/erase failure */
#define EEFS_CORRUPT_FILE            (-12)   /* metadata page or CRC check failed */
#define EEFS_NOT_FORMATTED           (-13)   /* no directory and no metadata page on the device */
#define EEFS_TRANSACTION_ERROR       (-14)   /* pending batch too large (cleanup list full); commit first */
#define EEFS_GEOMETRY_MISMATCH       (-15)   /* device was formatted with other geometry/config */
```

### 2.2 `inc/eefs_config.h` additions

All guarded with `#ifndef` so `-D` overrides work. They are harmless for the EEPROM build (macros
only). `tools/geneepromfs/eefs_config.h` is **not** touched.

```c
#ifndef EEFS_NAND_MAX_PAGE_SIZE
#define EEFS_NAND_MAX_PAGE_SIZE          4096    /* bytes; buffers are sized from this */
#endif
#ifndef EEFS_NAND_MAX_OOB_SIZE
#define EEFS_NAND_MAX_OOB_SIZE           224
#endif
#ifndef EEFS_NAND_MAX_BLOCKS
#define EEFS_NAND_MAX_BLOCKS             4096    /* block map bytes */
#endif
#ifndef EEFS_NAND_MAX_DIR_BLOCKS
#define EEFS_NAND_MAX_DIR_BLOCKS         32      /* upper bound for Geometry.DirRegionBlocks */
#endif
#ifndef EEFS_NAND_MAX_PATH_SIZE
#define EEFS_NAND_MAX_PATH_SIZE          256     /* format-affecting, see EEFS_NAND_LAYOUT_VERSION */
#endif
#ifndef EEFS_NAND_CFS_NAME_SIZE
#define EEFS_NAND_CFS_NAME_SIZE          20      /* format-affecting */
#endif
#ifndef EEFS_NAND_CFS_SYMBOL_SIZE
#define EEFS_NAND_CFS_SYMBOL_SIZE        64      /* format-affecting */
#endif
#ifndef EEFS_NAND_LAYOUT_VERSION
#define EEFS_NAND_LAYOUT_VERSION         1       /* bump when any format-affecting size changes */
#endif
#ifndef EEFS_NAND_MAX_CLEANUP_BLOCKS
#define EEFS_NAND_MAX_CLEANUP_BLOCKS     256     /* pending cleanup list; overflow = EEFS_TRANSACTION_ERROR */
#endif
#ifndef EEFS_NAND_RESERVE_BLOCKS
#define EEFS_NAND_RESERVE_BLOCKS         16      /* free blocks held back from new-file and CopySlot allocations (6.9) */
#endif
#ifndef EEFS_NAND_BADBLOCK_RESERVE
#define EEFS_NAND_BADBLOCK_RESERVE       4       /* free blocks held back from everything but grown-bad replacement (6.9) */
#endif
#ifndef EEFS_NAND_VERIFY_WRITES
#define EEFS_NAND_VERIFY_WRITES          1       /* read back every programmed page */
#endif
#ifndef EEFS_NAND_VERIFY_ON_OPEN
#define EEFS_NAND_VERIFY_ON_OPEN         0       /* 1: Open also checks every block CRC and the file CRC (6.4). Default: page 0 only. */
#endif
#ifndef EEFS_NAND_AUTO_COMMIT
#define EEFS_NAND_AUTO_COMMIT            0
#endif
#ifndef EEFS_NAND_WRITE_RETRIES
#define EEFS_NAND_WRITE_RETRIES          3       /* restarts of a file write after a grown bad block */
#endif
#ifndef EEFS_NAND_WRITESEQ_MOUNT_GAP
#define EEFS_NAND_WRITESEQ_MOUNT_GAP     1024    /* added to WriteSeq at mount, see 5.2 step 8 */
#endif
#ifndef EEFS_NAND_MAX_HEALTH_FINDINGS
#define EEFS_NAND_MAX_HEALTH_FINDINGS    32
#endif
#ifndef EEFS_NAND_HEALTH_PATH_SIZE
#define EEFS_NAND_HEALTH_PATH_SIZE       64      /* truncated path copy in a finding */
#endif
#if defined(EEFS_BACKEND_NAND) && (EEFS_MAX_FILES > 126)
#error "EEFS_MAX_FILES must be <= 126 for the NAND block map encoding"
#endif
#if defined(EEFS_BACKEND_NAND) && (EEFS_NAND_BADBLOCK_RESERVE > EEFS_NAND_RESERVE_BLOCKS)
#error "EEFS_NAND_BADBLOCK_RESERVE must be <= EEFS_NAND_RESERVE_BLOCKS"
#endif
```

(The `EEFS_MAX_FILES` check applies to the NAND backend only, so the EEPROM build keeps its old limits.)
`EEFS_NAND_CREAT_SPARE_BLOCKS`, which phase 1 added, is **removed**: spare blocks only served
in-place growth, which no longer exists (6.2). A file's allocation is always exactly the blocks it
needs.

### 2.3 `inc/eefs_nand_hal.h`

```c
#define EEFS_NAND_READ_UNCORRECTABLE   (-1)    /* ReadPage: ECC could not correct the page */
#define EEFS_NAND_READ_IO_ERROR        (-2)    /* ReadPage: controller/bus error */
/* ReadPage >= 0: success, value = corrected bit count (0 = clean) */

#define EEFS_NAND_SLOT_A               0
#define EEFS_NAND_SLOT_B               1
#define EEFS_NAND_SLOT_NONE            0xFF    /* "unspecified" in API calls; "single copy" on media */

#define EEFS_FILE_TYPE_OTHER           0
#define EEFS_FILE_TYPE_BOOT_FIT        1
#define EEFS_FILE_TYPE_BOOT_ELF        2
#define EEFS_FILE_TYPE_BOOT_DTB        3
#define EEFS_FILE_TYPE_CFS_APP         4
#define EEFS_FILE_TYPE_CFS_LIB         5

#define EEFS_NAND_BADMARKER_PAGE0      0x1
#define EEFS_NAND_BADMARKER_PAGE1      0x2
#define EEFS_NAND_BADMARKER_LAST       0x4

typedef struct {
    uint32 PageSize;          /* 2048, 4096 or 8192 */
    uint32 OobSize;           /* <= EEFS_NAND_MAX_OOB_SIZE */
    uint32 PagesPerBlock;
    uint32 BlockCount;        /* <= EEFS_NAND_MAX_BLOCKS */
    uint32 DirRegionBlocks;   /* even, >= 4, <= EEFS_NAND_MAX_DIR_BLOCKS */
    uint32 BadMarkerOffset;   /* OOB byte index */
    uint32 BadMarkerPages;    /* EEFS_NAND_BADMARKER_* mask, non-zero */
    uint32 EccScrubThreshold; /* corrected bits per page that triggers BLOCK_NEEDS_SCRUB; 0 = never */
} EEFS_NandGeometry_t;

typedef struct {
    int32 (*ReadPage)(void *Ctx, uint32 Block, uint32 Page, void *Data, void *Oob);
    int32 (*ProgramPage)(void *Ctx, uint32 Block, uint32 Page, const void *Data, const void *Oob);
    int32 (*EraseBlock)(void *Ctx, uint32 Block);
} EEFS_NandOps_t;

typedef struct {
    EEFS_NandGeometry_t    Geometry;
    const EEFS_NandOps_t  *Ops;
    void                  *Ctx;
} EEFS_NandDevice_t;
```

HAL contract (document in the header comment):

- `ReadPage`: `Data` receives `PageSize` bytes, `Oob` receives `OobSize` bytes; either may be
  `NULL` to skip. An erased page must read as all `0xFF` with status `>= 0` (controllers that flag
  erased pages as ECC errors must special-case them). A `Data == NULL` read of a factory-bad block
  must return the raw OOB bytes; if it returns an error, the library treats the block as bad.
- `ProgramPage`: `Data` or `Oob` may be `NULL` to leave that area untouched. Programming only the
  OOB is used for the bad-block marker and is best effort (the return value is ignored there).
  The library programs pages of a block in ascending order and never programs a page twice between
  erases.
- `EraseBlock`: returns `0` or `-1`. The library marks a block bad after a failed erase.
- All calls are made under `EEFS_LIB_LOCK`.

```c
typedef union {
    struct { uint64 DestAddress; }                                        Fit;
    struct { uint64 DestAddress; uint64 EntryPoint; uint32 Flags; }        Elf;   /* Flags bit0 = EntryPoint valid */
    struct { uint64 DestAddress; }                                        Dtb;
    struct { char AppName[EEFS_NAND_CFS_NAME_SIZE]; char EntrySymbol[EEFS_NAND_CFS_SYMBOL_SIZE];
             uint16 Priority; uint8 ExceptionAction; uint32 StackSize; uint64 LoadAddress; } App;
    struct { char LibName[EEFS_NAND_CFS_NAME_SIZE]; char EntrySymbol[EEFS_NAND_CFS_SYMBOL_SIZE];
             uint64 LoadAddress; } Lib;
} EEFS_NandTypeInfo_t;

typedef struct {
    uint8   InUse;
    uint8   Corrupt;           /* RAM flag: page 0 or CRC check failed since mount (output only) */
    uint16  FirstBlock;
    uint16  AllocBlocks;
    uint16  Reserved;
    uint32  FileSize;
    uint32  Version;
    uint32  WriteSeq;
    uint32  FileCrc32;
    uint32  ModificationDate;
} EEFS_NandSlotInfo_t;

typedef struct {
    char                 Path[EEFS_NAND_MAX_PATH_SIZE];   /* output: stored path (no @A/@B) */
    uint8                FileType;          /* input + output */
    uint8                Slot;              /* input: slot to write when the name has no suffix (A, B or NONE); output: resolved slot */
    uint8                IsAB;              /* output */
    uint8                ActiveSlot;        /* output */
    uint8                FallbackCompatible;/* output */
    uint8                ActiveFailed;      /* output */
    uint8                Reserved[2];
    uint32               Attributes;        /* input + output */
    uint32               Version;           /* input + output */
    uint32               CreationDate;      /* output */
    uint32               FileSize;          /* output: resolved slot */
    uint32               FileCrc32;         /* output: resolved slot */
    EEFS_NandTypeInfo_t  Type;              /* input + output (from the resolved slot's page 0) */
    EEFS_NandSlotInfo_t  SlotInfo[2];       /* output */
} EEFS_NandFileInfo_t;
```

`EEFS_LibWriteFile` reads only `FileType`, `Slot`, `Attributes`, `Version`, `Type`.

### 2.4 `inc/eefs_nand_format.h`

Constants (all `#define`), with the values of FMT: `EEFS_NAND_P0_MAGIC` (`0x46464545`),
`EEFS_NAND_REC_MAGIC` (`0x4A464545`), every page-0 offset `EEFS_NAND_P0_OFF_*` (MAGIC 0,
VERSION 4, HDRCRC 8, WRITESEQ 12, PATH 16, FILETYPE 272, SLOT 273, ATTR 276, VERSIONNUM 280,
FILESIZE 288, FILECRC 292, CDATE 296, MDATE 300, TYPEINFO 304, SIGTYPE 496, SIGLEN 500, SIG 504,
BLOCKCOUNT 1528, BLOCKS 1532), `EEFS_NAND_P0_FIXED_SIZE` 1532, `EEFS_NAND_P0_BLOCK_ENTRY_SIZE` 6,
`EEFS_NAND_P0_SIG_START` 496, `EEFS_NAND_P0_SIG_END` 1528, type-info offsets `EEFS_NAND_TI_*`,
record header offsets `EEFS_NAND_REC_OFF_*` and `EEFS_NAND_REC_HDR_SIZE` 32, item header size 4,
item types `EEFS_NAND_ITEM_VOLUME 1 / MAPCHUNK 2 / ENTRY 3 / CLEANUP 4`, `EEFS_NAND_REC_CHECKPOINT
1 / EEFS_NAND_REC_TXN 2`, entry record size `EEFS_NAND_ENTRY_REC_SIZE` 328 and slot record size 28
with their field offsets, `EEFS_NAND_VOLUME_ITEM_SIZE` 40, map values `EEFS_NAND_MAP_FREEING 0xFC /
DIR 0xFD / BAD 0xFE / FREE 0xFF`, and `EEFS_NAND_MAP_OWNER(entry, slot)` `((entry)*2 + (slot))`.
These are **computed from the configured sizes** (e.g. `EEFS_NAND_P0_OFF_FILETYPE
(EEFS_NAND_P0_OFF_PATH + EEFS_NAND_MAX_PATH_SIZE)`) so a size change moves them consistently; the
layout test (14.1) asserts the defaults equal the FMT numbers.

Decoded structures (RAM, natural alignment; never written to media as-is):

```c
typedef struct {
    uint8   InUse;
    uint8   Reserved;
    uint16  FirstBlock;
    uint16  AllocBlocks;
    uint16  Reserved2;
    uint32  FileSize;
    uint32  Version;
    uint32  WriteSeq;
    uint32  FileCrc32;
    uint32  ModificationDate;
} EEFS_NandSlot_t;                                    /* 28 bytes */

typedef struct {
    char    Path[EEFS_NAND_MAX_PATH_SIZE];
    uint8   InUse;
    uint8   IsAB;
    uint8   ActiveSlot;
    uint8   FallbackCompatible;
    uint8   ActiveFailed;
    uint8   FileType;
    uint16  Reserved;
    uint32  Attributes;
    uint32  CreationDate;
    EEFS_NandSlot_t Slot[2];
} EEFS_NandEntry_t;                                   /* 328 bytes */

typedef struct {
    uint32  WriteSeq;
    char    Path[EEFS_NAND_MAX_PATH_SIZE];
    uint8   FileType;
    uint8   Slot;                /* 0, 1 or 0xFF */
    uint16  Reserved;
    uint32  Attributes;
    uint32  Version;
    uint32  FileSize;
    uint32  FileCrc32;
    uint32  CreationDate;
    uint32  ModificationDate;
    EEFS_NandTypeInfo_t Type;
    uint16  SigType;
    uint32  SigLength;
    uint32  BlockCount;
} EEFS_NandPage0_t;

typedef struct {
    uint8   RecType;
    uint8   Mirror;
    uint32  Seq;
    uint16  PageIndex;
    uint16  PageCount;
    uint32  PayloadLen;
} EEFS_NandRecHdr_t;

typedef struct {
    uint32  PageSize, OobSize, PagesPerBlock, BlockCount, DirRegionBlocks, MaxFiles;
    uint32  WriteSeqHigh, FormatTime;
    uint16  PathSize, CfsNameSize, CfsSymbolSize;
} EEFS_NandVolumeItem_t;
```

Functions (all in `eefs_nand_format.c` / `eefs_crc32.c`; pure, reentrant, no globals except the
CRC table):

```c
#define EEFS_CRC32_INIT  0xFFFFFFFFUL
uint32  EEFS_Crc32Update(uint32 State, const void *Data, uint32 Length);   /* feed bytes */
uint32  EEFS_Crc32Final(uint32 State);                                      /* State ^ 0xFFFFFFFF */
uint32  EEFS_Crc32(const void *Data, uint32 Length);                        /* one-shot */

void    EEFS_NandPut16(uint8 *P, uint16 V);  void EEFS_NandPut32(uint8 *P, uint32 V);  void EEFS_NandPut64(uint8 *P, uint64 V);
uint16  EEFS_NandGet16(const uint8 *P);      uint32 EEFS_NandGet32(const uint8 *P);     uint64 EEFS_NandGet64(const uint8 *P);
boolean EEFS_NandIsErased(const uint8 *Data, uint32 Length);                /* all 0xFF */

/* Page 0 */
uint32  EEFS_NandMaxBlocksPerFile(uint32 PageSize);                         /* (PageSize-1532)/6 */
int32   EEFS_NandEncodePage0(uint8 *Page, uint32 PageSize, const EEFS_NandPage0_t *P,
                             const uint16 *Blocks, const uint32 *BlockCrcs);  /* fills whole page incl. 0xFF tail and HeaderCrc; EEFS_INVALID_ARGUMENT if BlockCount is 0 or too big, SigType or SigLength != 0, FileType > 5, or Path does not start with '/' or is not terminated */
int32   EEFS_NandDecodePage0(const uint8 *Page, uint32 PageSize, uint32 PagesPerBlock, uint32 ExpectBlock, EEFS_NandPage0_t *P);
        /* FMT 3.4 checks 2-7 (ExpectBlock is Blocks[0].Block check; PagesPerBlock is needed for BlockCount * BlockBytes >= PageSize + FileSize; DirRegionBlocks/BlockCount bounds are checked by the caller via EEFS_NandPage0Block). Returns EEFS_SUCCESS or EEFS_CORRUPT_FILE. */
void    EEFS_NandPage0Block(const uint8 *Page, uint32 Index, uint16 *Block, uint32 *Crc);
uint32  EEFS_NandBlockDataRange(uint32 PageSize, uint32 PagesPerBlock, uint32 FileSize, uint32 K,
                                uint32 *Offset);                             /* returns byte count held by Blocks[K], sets its file offset */

/* Record pages */
void    EEFS_NandBeginRecordPage(uint8 *Page, uint32 PageSize, const EEFS_NandRecHdr_t *H);   /* writes header fields, fills [32,PageSize) with 0xFF */
void    EEFS_NandSealRecordPage(uint8 *Page, uint32 PageSize, uint32 PayloadLen);             /* sets PayloadLen and Crc32 */
int32   EEFS_NandDecodeRecordPage(const uint8 *Page, uint32 PageSize, EEFS_NandRecHdr_t *H);
        /* FMT 4.2 page validity plus the item framing of FMT 4.3 (every item fits PayloadLen; VOLUME Len 40, ENTRY Len 332,
           MAPCHUNK Len 8 + Count with Count >= 1, CLEANUP Len 4 + 2 * Count with Count >= 1), so a record whose pages all
           decode can be applied without failing half way; EEFS_SUCCESS / EEFS_CORRUPT_FILE. H is filled on success only. */

/* Items: encoders return bytes written, or 0 if the item does not fit in Avail */
uint32  EEFS_NandEncodeVolumeItem(uint8 *Dst, uint32 Avail, const EEFS_NandVolumeItem_t *V);
uint32  EEFS_NandEncodeMapChunk(uint8 *Dst, uint32 Avail, uint32 StartBlock, uint32 Count, const uint8 *Map);  /* Count is clipped by caller */
uint32  EEFS_NandEncodeEntryItem(uint8 *Dst, uint32 Avail, uint16 Index, const EEFS_NandEntry_t *E);
uint32  EEFS_NandEncodeCleanupItem(uint8 *Dst, uint32 Avail, const uint16 *Blocks, uint16 Count);
int32   EEFS_NandNextItem(const uint8 *Payload, uint32 PayloadLen, uint32 *Cursor,
                          uint8 *Type, const uint8 **Body, uint16 *Len);     /* 1 = item returned, 0 = end, EEFS_CORRUPT_FILE = malformed */
int32   EEFS_NandDecodeVolumeItem(const uint8 *Body, uint16 Len, EEFS_NandVolumeItem_t *V);
int32   EEFS_NandDecodeEntryItem(const uint8 *Body, uint16 Len, uint16 *Index, EEFS_NandEntry_t *E);
/* MAPCHUNK and CLEANUP are decoded inline with Get16/Get32 by the callers */
uint32  EEFS_NandMapChunkFit(uint32 Avail);   /* largest MAPCHUNK Count that fits in Avail bytes (0 if none), for the greedy packer */
```

`EEFS_NandEncodeMapChunk` serializes `0xFC` as `0xFF`. All encoders write every byte of the item
(reserved = 0, string padding = 0).

### 2.5 `inc/eefs_nand_types.h` (the volume RAM)

```c
typedef struct {
    EEFS_NandEntry_t Entry[EEFS_MAX_FILES];
    uint8            Map[EEFS_NAND_MAX_BLOCKS];
    uint32           WriteSeqHigh;
} EEFS_NandDirState_t;

typedef struct {
    uint16  Blocks[EEFS_NAND_MAX_DIR_BLOCKS / 2];   /* good blocks of this mirror, ascending */
    uint16  Count;
    uint16  CurIndex;        /* index into Blocks[] of the block holding the newest record, valid if HasState */
    uint16  NextPage;        /* first free page in Blocks[CurIndex] */
    uint32  CheckpointSeq;   /* Seq of the checkpoint at page 0 of the current block */
    uint32  LastSeq;         /* Seq of the last valid record in the chain */
    uint8   HasState;        /* a valid chain was found at mount or written since */
    uint8   NeedsResync;     /* LastSeq < Volume.LastSeq (or !HasState): write a checkpoint at Seq Volume.LastSeq before the next record */
    uint8   EnterNewBlock;   /* the next record for this mirror starts a fresh block (torn tail, torn entry block, or full) */
    uint8   Disabled;        /* no good blocks left */
} EEFS_NandMirror_t;

typedef struct {             /* result of scanning one mirror (5.3); also used by the health check */
    uint8   Valid;            /* chain found */
    uint8   Torn;             /* page after the chain is not erased, or a higher-Seq block with a torn checkpoint exists */
    uint16  BlockIdx;         /* ring index of the current block */
    uint16  NextPage;
    uint32  CheckpointSeq, LastSeq;
    uint32  MaxSeqSeen;       /* highest Seq in any readable record header of this mirror, valid or not */
} EEFS_NandChain_t;

/* Health report types and finding codes: see section 10.1. They are defined in this header, before
 * EEFS_InodeTable_t, because the volume embeds an EEFS_HealthSummary_t. */

#define EEFS_NAND_SLOTFLAG_CORRUPT     0x01   /* page 0 or CRC failed since mount */
#define EEFS_NAND_SLOTFLAG_CROSSLINK   0x02   /* reconcile found another slot claiming one of its blocks */

typedef struct {
    EEFS_NandDevice_t    *Dev;               /* from BaseAddress */
    uint32                Mounted;
    EEFS_NandDirState_t   Base;              /* committed state as on media (plus the media facts noted in section 4) */
    EEFS_NandDirState_t   Working;           /* pending state */
    uint8                 ForceDirty[(EEFS_MAX_FILES + 7) / 8];   /* entries to include in the next record regardless of diff (section 4) */
    uint32                AllocCursor;       /* round-robin allocation cursor, RAM only (6.9) */
    uint8                 SlotFlags[EEFS_MAX_FILES][2];
    uint32                NextWriteSeq;
    uint32                LastSeq;           /* newest committed record Seq */
    uint32                VolumeChangeCounter;
    uint32                FormatTime;
    uint8                 DirectoryRebuilt;  /* mounted from a RAM rebuild, not yet persisted */
    uint8                 Reserved[3];
    EEFS_NandMirror_t     Mirror[2];
    uint16                PendingCleanup[EEFS_NAND_MAX_CLEANUP_BLOCKS];
    uint16                PendingCleanupCount;
    uint16                PendingCleanupBase;          /* leading entries that came from the media (mount), not from the batch (5.5) */
    uint16                PageSize, PagesPerBlock;     /* cached from geometry */
    uint32                BlockBytes, MaxBlocksPerFile;
    uint8                 PageBuf[EEFS_NAND_MAX_PAGE_SIZE];   /* scratch for building pages / verify */
    uint8                 OobBuf[EEFS_NAND_MAX_OOB_SIZE];
    uint8                 CacheBuf[EEFS_NAND_MAX_PAGE_SIZE];  /* one-page read cache */
    uint32                CacheBlock, CachePage;
    uint8                 CacheValid;
    struct EEFS_HealthReport *ActiveReport;                   /* health check in progress, or NULL (EEFS_HealthReport_t is defined after this struct) */
    EEFS_HealthSummary_t  LastReport;                         /* last completed health report (section 10) */
} EEFS_InodeTable_t;

typedef struct {
    uint32                InUse;
    uint32                Mode;               /* always EEFS_FREAD */
    EEFS_InodeTable_t    *InodeTable;
    uint32                InodeIndex;         /* entry index */
    uint8                 Slot;               /* slot actually opened */
    uint8                 Fallback;           /* opened the inactive copy under the compatible-fallback rule */
    uint16                FirstBlock;
    uint32                FileSize;
    uint32                ByteOffset;
    uint32                CurK;               /* index into Blocks[] of CurBlock, 0xFFFFFFFF = none */
    uint16                CurBlock;
} EEFS_FileDescriptor_t;
```

`EEFS_NandDirState_t` is 20,992 + 4,096 + 4 = 25,092 bytes at the defaults. The volume is
2 x 25,092 + 2 x 4,096 + 224 + 512 (cleanup) + 128 + ~2,700 (`LastReport`) + ~300 = **~61.7 KB**;
two devices (`EEFS_MAX_DEVICES` = 2) need ~124 KB of static RAM. The global descriptor table
adds 20 x 40 bytes. Knobs: `EEFS_MAX_FILES` and `EEFS_NAND_MAX_PATH_SIZE` dominate (each 64-entry
table is 21 KB); `EEFS_NAND_MAX_BLOCKS` is 4 KB per copy. The plan estimated ~15 KB; the
difference is the second copy of the entry table needed for Abort, and the cached health summary.
This budget was accepted by the user on 2026-10-15.

### 2.6 `inc/eefs_nand.h` (public NAND API; includes `eefs_fileapi.h`)

All functions take the lock (`EEFS_LIB_LOCK`/`EEFS_LIB_UNLOCK` from the driver's `eefs_macros.h`)
for their whole duration unless stated. `Name` arguments accept `path`, `path@A` or `path@B`
(section 6.1). Return codes listed are exhaustive for the function (plus `EEFS_INVALID_ARGUMENT` for
NULL pointers / unmounted volume everywhere).

```c
/* Format the device behind BaseAddress (an EEFS_NandDevice_t *). Scratch is a volume that must not be mounted; it is
 * used as working RAM and left unmounted (zeroed). Section 5.1.
 * EEFS_SUCCESS, EEFS_INVALID_ARGUMENT (geometry), EEFS_DEVICE_IS_BUSY (Scratch mounted), EEFS_IO_ERROR (a mirror has
 * fewer than 2 good blocks, or the checkpoint could not be written to either mirror). */
int32 EEFS_LibFormat(EEFS_InodeTable_t *Scratch, cpuaddr BaseAddress);

/* Whole-file write. Section 6.3. Data may be NULL only when Size == 0.
 * EEFS_SUCCESS, EEFS_INVALID_ARGUMENT (bad name, bad type/slot, Meta->Type SigType, Size > max), EEFS_PERMISSION_DENIED
 * (read-only attribute, file open, write-protected FS), EEFS_NO_SPACE_LEFT_ON_DEVICE (entries or blocks),
 * EEFS_TRANSACTION_ERROR (cleanup list full), EEFS_IO_ERROR (retries exhausted). */
int32 EEFS_LibWriteFile(EEFS_InodeTable_t *T, char *Name, const EEFS_NandFileInfo_t *Meta, const void *Data, uint32 Size);

/* Section 5.4 / 5.5. Commit: EEFS_SUCCESS (also when nothing to write), EEFS_IO_ERROR (no mirror accepted the record).
 * Abort: EEFS_SUCCESS, EEFS_DEVICE_IS_BUSY (a file opened from a batch-created slot is still open). */
int32 EEFS_LibCommit(EEFS_InodeTable_t *T);
int32 EEFS_LibAbort(EEFS_InodeTable_t *T);

/* Reads page 0 and every data page of the named copy (explicit slot, or active slot; no fallback) and checks all CRCs.
 * EEFS_SUCCESS, EEFS_FILE_NOT_FOUND, EEFS_CORRUPT_FILE, EEFS_IO_ERROR. Sets SlotFlags CORRUPT and, for the active
 * slot, ActiveFailed (6.8). */
int32 EEFS_LibVerifyFile(EEFS_InodeTable_t *T, char *Name);

/* Fills Info from the directory entry and the resolved slot's page 0 (one page read; type info comes only from page 0).
 * EEFS_SUCCESS, EEFS_FILE_NOT_FOUND, EEFS_CORRUPT_FILE (page 0 invalid; Info is still filled from the directory, Type
 * zeroed), EEFS_IO_ERROR. */
int32 EEFS_LibGetFileInfo(EEFS_InodeTable_t *T, char *Name, EEFS_NandFileInfo_t *Info);

/* Space and state of the volume (6.9). No NAND reads. EEFS_SUCCESS. */
typedef struct {
    uint32  BlockCount, DirRegionBlocks, DataBlocks;   /* DataBlocks = BlockCount - DirRegionBlocks */
    uint32  FreeBlocks;             /* map == FREE, including blocks awaiting a pending cleanup erase */
    uint32  FreeingBlocks;          /* freed in the open batch; free after the next commit */
    uint32  UsedBlocks;             /* owned by a slot in Working (committed or pending) */
    uint32  BadBlocks;              /* map == BAD, both regions */
    uint32  ReserveBlocks;          /* EEFS_NAND_RESERVE_BLOCKS */
    uint32  BadBlockReserve;        /* EEFS_NAND_BADBLOCK_RESERVE */
    uint32  AvailableForNew;        /* FreeBlocks > ReserveBlocks ? FreeBlocks - ReserveBlocks : 0 */
    uint32  AvailableForModify;     /* FreeBlocks > BadBlockReserve ? FreeBlocks - BadBlockReserve : 0 */
    uint32  FilesInUse, SlotsInUse;
    uint32  LastSeq, NextWriteSeq;
    uint32  PendingCleanup;         /* entries on the list (from media and from the batch) */
    uint8   DirectoryRebuilt;
    uint8   UncommittedChanges;     /* Working != Base, or ForceDirty set */
    uint8   MirrorValid[2];         /* Mirror[m].HasState && !Disabled */
} EEFS_NandVolumeInfo_t;
int32 EEFS_LibGetVolumeInfo(EEFS_InodeTable_t *T, EEFS_NandVolumeInfo_t *Info);

/* A/B. Section 7. Name must not carry a suffix for these (INVALID_ARGUMENT). As delivered, every A/B call also returns
 * READ_ONLY_FILE_SYSTEM when EEFS_LIB_IS_WRITE_PROTECTED; the "All" counts are the entries the setting was applied to,
 * whether or not a value changed; a non-zero Flag is stored as 1. */
int32 EEFS_LibSetActiveSlot(EEFS_InodeTable_t *T, char *Name, uint8 Slot);        /* SUCCESS, FILE_NOT_FOUND (entry or that slot not in use), INVALID_ARGUMENT (not IsAB, bad slot) */
int32 EEFS_LibSetActiveSlotAll(EEFS_InodeTable_t *T, uint8 Slot);                 /* >= 0: IsAB entries with Slot in use (all set); INVALID_ARGUMENT */
int32 EEFS_LibSetFallbackCompatible(EEFS_InodeTable_t *T, char *Name, uint8 Flag);/* SUCCESS, FILE_NOT_FOUND, INVALID_ARGUMENT (not IsAB) */
int32 EEFS_LibSetFallbackCompatibleAll(EEFS_InodeTable_t *T, uint8 Flag);         /* >= 0: IsAB entries (all set) */
int32 EEFS_LibSetFailed(EEFS_InodeTable_t *T, char *Name);                        /* SUCCESS, FILE_NOT_FOUND, INVALID_ARGUMENT (not IsAB) */
int32 EEFS_LibClearFailed(EEFS_InodeTable_t *T, char *Name);
int32 EEFS_LibCopySlot(EEFS_InodeTable_t *T, char *Name, uint8 From, uint8 To);  /* SUCCESS, FILE_NOT_FOUND (entry or From not in use), INVALID_ARGUMENT (From == To, not IsAB), PERMISSION_DENIED (To open, or read-only), NO_SPACE_LEFT_ON_DEVICE, TRANSACTION_ERROR, CORRUPT_FILE (source), IO_ERROR */
int32 EEFS_LibCopySlotAll(EEFS_InodeTable_t *T, uint8 From, uint8 To);           /* >= 0 copies done, or the first error (copies already done stay in Working) */

/* Directory maintenance. Section 8. */
int32 EEFS_LibRepairDirectory(EEFS_InodeTable_t *T);     /* commits pending changes and writes a fresh CHECKPOINT to both mirrors; clears DirectoryRebuilt. SUCCESS, IO_ERROR */
int32 EEFS_LibRebuildDirectory(EEFS_InodeTable_t *T);    /* discards RAM directory state (requires no open files/dir and no uncommitted changes) and rebuilds from page 0s. SUCCESS, DEVICE_IS_BUSY, TRANSACTION_ERROR, NOT_FORMATTED, IO_ERROR */
int32 EEFS_LibReclaimOrphans(EEFS_InodeTable_t *T);      /* erases blocks with a valid page 0 that no Working slot references. >= 0 blocks erased, or READ_ONLY_FILE_SYSTEM (as delivered, 8.4) */

/* Health check. Section 10. */
int32 EEFS_LibHealthCheckStart(EEFS_InodeTable_t *T, EEFS_HealthReport_t *Report);   /* SUCCESS, DEVICE_IS_BUSY (another report active on T) */
int32 EEFS_LibHealthCheckStep(EEFS_InodeTable_t *T, uint32 MaxPageReads);            /* 1 = more, EEFS_SUCCESS = done, INVALID_ARGUMENT (not started) */
int32 EEFS_LibHealthCheckSummary(EEFS_InodeTable_t *T, EEFS_HealthSummary_t *Out);   /* copy of the last completed report; SUCCESS, EEFS_ERROR if none completed yet */
int32 EEFS_LibHealthCheckCancel(EEFS_InodeTable_t *T);                                /* detaches ActiveReport; SUCCESS (also when none) */
```

The existing `EEFS_Lib*` prototypes keep their signatures. NAND semantics:

| Function | NAND behaviour |
|---|---|
| `EEFS_LibInit` | zero descriptor table and directory descriptor (as EEPROM) |
| `EEFS_LibInitFS(T, BaseAddress)` | mount (5.2). `EEFS_SUCCESS` (also when rebuilt), `EEFS_INVALID_ARGUMENT` (geometry, `T` already mounted), `EEFS_GEOMETRY_MISMATCH`, `EEFS_NOT_FORMATTED`, `EEFS_IO_ERROR` (every dir block unreadable *and* rebuild scan failed) |
| `EEFS_LibFreeFS(T)` | `EEFS_DEVICE_IS_BUSY` if files/dir open or a health check active; `EEFS_TRANSACTION_ERROR` if `Working != Base` (uncommitted changes, caller must Commit or Abort); else zero `T` |
| `EEFS_LibOpen(T, Name, Flags, Attr)` | only `O_RDONLY` (exactly `Flags == O_RDONLY`); anything else `EEFS_UNSUPPORTED_OPTION`. Resolution 6.1/7.3. `EEFS_FILE_NOT_FOUND`, `EEFS_CORRUPT_FILE`, `EEFS_IO_ERROR`, `EEFS_NO_FREE_FILE_DESCRIPTOR` |
| `EEFS_LibCreat` | `EEFS_UNSUPPORTED_OPTION` |
| `EEFS_LibWrite` | `EEFS_UNSUPPORTED_OPTION` |
| `EEFS_LibClose` | frees the descriptor |
| `EEFS_LibRead` | 6.7; bytes read, `0` at EOF, `EEFS_IO_ERROR` (uncorrectable), `EEFS_INVALID_ARGUMENT` |
| `EEFS_LibLSeek` | as EEPROM (SEEK_SET/CUR/END, clamp to EOF) |
| `EEFS_LibRemove` | 6.5 |
| `EEFS_LibRename` | 6.6 |
| `EEFS_LibStat` / `EEFS_LibFstat` | from RAM entry (no NAND read): `InodeIndex`, `Crc` = `FileCrc32`, `Attributes`, `FileSize`, dates, `Filename` = path. For an A/B name without suffix: the active slot, no fallback. `EEFS_FILE_NOT_FOUND` |
| `EEFS_LibSetFileAttributes` | 6.6 (first-block rewrite) |
| `EEFS_LibOpenDir/ReadDir/CloseDir` | iterates entries `0..EEFS_MAX_FILES-1` of `Working`; `Filename` = path, `InUse`, `InodeIndex`, `FileHeaderPointer = NULL`, `MaxFileSize` = active slot's `AllocBlocks * BlockBytes - PageSize` (0 if not in use) |
| `EEFS_LibHasOpenFiles/HasOpenDir/FileDescriptor2Pointer/GetFileDescriptorsInUse/HighWaterMark/GetMaxFiles/GetMaxOpenFiles/PrintOpenFiles` | as EEPROM |
| `EEFS_LibChkDsk(T, Flags)` | prints the Working directory (entries, slots, flags, map summary, mirror state) and runs `EEFS_LibVerifyFile` on every in-use slot; returns `EEFS_SUCCESS` or the first error |

### 2.7 Standalone driver additions (`eefs_filesys.[ch]`, all under `#ifdef EEFS_BACKEND_NAND`)

```c
#ifdef EEFS_BACKEND_NAND
#define EEFS_MAX_PATH_SIZE   (EEFS_MAX_MOUNTPOINT_SIZE + EEFS_MAX_FILENAME_SIZE + 2)   /* 274: mount point + path + "@A" */
#else
#define EEFS_MAX_PATH_SIZE   64
#endif

int32 EEFS_Format(char *DeviceName, cpuaddr BaseAddress);
      /* If DeviceName is in the device table: it must have no mounted volume (else EEFS_ERROR); its InodeTable is
         re-used. Otherwise a device entry is allocated as EEFS_InitFS does. Calls EEFS_LibFormat then EEFS_LibInitFS;
         on failure frees a newly allocated entry. After success the device is initialized and can be mounted. */
int32 EEFS_WriteFile(char *Path, const EEFS_NandFileInfo_t *Meta, const void *Data, uint32 Size);
int32 EEFS_Commit(char *MountPoint);
int32 EEFS_Abort(char *MountPoint);
int32 EEFS_VerifyFile(char *Path);
int32 EEFS_GetFileInfo(char *Path, EEFS_NandFileInfo_t *Info);
int32 EEFS_GetVolumeInfo(char *MountPoint, EEFS_NandVolumeInfo_t *Info);
int32 EEFS_SetActiveSlot(char *Path, uint8 Slot);
int32 EEFS_SetActiveSlotAll(char *MountPoint, uint8 Slot);
int32 EEFS_SetFallbackCompatible(char *Path, uint8 Flag);
int32 EEFS_SetFallbackCompatibleAll(char *MountPoint, uint8 Flag);
int32 EEFS_SetFailed(char *Path);
int32 EEFS_ClearFailed(char *Path);
int32 EEFS_CopySlot(char *Path, uint8 From, uint8 To);
int32 EEFS_CopySlotAll(char *MountPoint, uint8 From, uint8 To);
int32 EEFS_RepairDirectory(char *MountPoint);
int32 EEFS_RebuildDirectory(char *MountPoint);
int32 EEFS_ReclaimOrphans(char *MountPoint);
int32 EEFS_HealthCheckStart(char *MountPoint, EEFS_HealthReport_t *Report);
int32 EEFS_HealthCheckStep(char *MountPoint, uint32 MaxPageReads);
int32 EEFS_HealthCheckSummary(char *MountPoint, EEFS_HealthSummary_t *Out);
int32 EEFS_HealthCheckCancel(char *MountPoint);                 /* added in phase 6 */
#endif
```

`EEFS_Format` on a device that is in the table but not mounted, and whose volume is initialized, first releases it
with `EEFS_LibFreeFS` (returning that code if it fails, e.g. `EEFS_TRANSACTION_ERROR`), since `EEFS_LibFormat` needs an
unmounted scratch volume. The `BaseAddress` must not belong to another device (`EEFS_ERROR`).

Unlike the existing wrappers (which collapse every failure to `EEFS_ERROR`), the new wrappers
**return the `EEFS_Lib*` code unchanged** when the path resolved, and `EEFS_ERROR` only for path /
volume / device lookup failures. `EEFS_UnMount` under NAND calls `EEFS_LibCommit` first and fails
with `EEFS_ERROR` if the commit fails (per the plan: unmount commits).

`EEFS_SplitPath` change: today it copies the file name **without** the second `/`. Under
`EEFS_BACKEND_NAND` it must keep it, so that `"/NAND0/cf/apps/x.so"` gives `MountPoint = "/NAND0"`
and `Filename = "/cf/apps/x.so"`:

```c
#ifdef EEFS_BACKEND_NAND
                    strncpy(SplitPath->Filename, &(InputPath[i]), EEFS_MAX_PATH_SIZE - 1);
#else
                    strncpy(SplitPath->Filename, &(InputPath[i+1]), EEFS_MAX_PATH_SIZE);
#endif
```

(the `-1` guarantees termination for the larger NAND buffer; the EEPROM line stays byte-identical).
The length check `strlen(InputPath) < EEFS_MAX_PATH_SIZE` picks up the larger constant
automatically. `EEFS_OpenDir("/NAND0")` is unchanged. `ReadDir` returns full stored paths
(`/cf/apps/x.so`) without the mount point, matching the EEPROM driver's behaviour of returning the
stored name.

## 3. Internal module interfaces (`eefs_nand_internal.h`)

These are the functions later phases call; names and semantics are fixed here. `Vol` is the
`EEFS_InodeTable_t *`.

### 3.1 I/O (`eefs_nand_io.c`, phase 2)

```c
int32   EEFS_NandReadPage(Vol, uint32 Block, uint32 Page, uint8 *Data, uint8 *Oob, uint32 *Corrected);
        /* uncached; Data/Oob may be NULL. SUCCESS, EEFS_IO_ERROR (both UNCORRECTABLE and IO_ERROR map here; *Corrected = 0xFFFFFFFF for uncorrectable) */
const uint8 *EEFS_NandReadCached(Vol, uint32 Block, uint32 Page, int32 *Status);
        /* returns Vol->CacheBuf (valid until the next NAND operation) or NULL with *Status = EEFS_IO_ERROR */
void    EEFS_NandInvalidateCache(Vol);
int32   EEFS_NandProgramPage(Vol, uint32 Block, uint32 Page, const uint8 *Data);
        /* programs Data (Oob NULL); with EEFS_NAND_VERIFY_WRITES reads back into PageBuf... see note; SUCCESS or EEFS_IO_ERROR. Invalidates the cache. */
int32   EEFS_NandProgramPageVerified(Vol, uint32 Block, uint32 Page, const uint8 *Data, uint8 *Scratch);
        /* as above but the read-back goes into Scratch (PageSize bytes) and is compared byte for byte */
int32   EEFS_NandEraseBlock(Vol, uint32 Block);          /* SUCCESS or EEFS_IO_ERROR; invalidates the cache */
void    EEFS_NandMarkBad(Vol, uint32 Block);             /* Working.Map[Block] = BAD; best-effort OOB marker on the lowest BadMarkerPages page; removes Block from any mirror ring (sets Disabled when empty); VolumeChangeCounter++ */
boolean EEFS_NandHasBadMarker(Vol, uint32 Block);        /* reads the OOB of every BadMarkerPages page (Data NULL); TRUE if any byte at BadMarkerOffset != 0xFF or a read fails */
int32   EEFS_NandReadPage0(Vol, uint32 Block, EEFS_NandPage0_t *P, const uint8 **Raw);
        /* cached read + EEFS_NandDecodePage0(PagesPerBlock from the geometry, ExpectBlock = Block) + bounds check of every Blocks[i] (>= DirRegionBlocks, < BlockCount). SUCCESS / EEFS_CORRUPT_FILE / EEFS_IO_ERROR. *Raw = cache pointer for Blocks[] access. */
boolean EEFS_NandPageIsErased(Vol, uint32 Block, uint32 Page);   /* uncached data read into PageBuf; a read error counts as not erased */
```

`EEFS_NandProgramPage` uses `Vol->PageBuf` as the read-back scratch; callers that are building the
page *in* `PageBuf` (record pages, page 0) must use `EEFS_NandProgramPageVerified` with
`Vol->CacheBuf` as scratch (the cache is invalidated anyway). As delivered, a scratch equal to the
source is replaced by the other buffer, and directory record pages are verified the same way (a byte
compare of the read-back in `CacheBuf`, done by `EEFS_NandWriteRecord`), which is stricter than a
header decode and needs no extra buffer. `EEFS_NandMarkBad` removes a directory block from its ring by
shifting the ring down; if the removed block was the current one, `CurIndex` moves to its predecessor
(so the next `EnterBlock` lands on the block that followed it) and `EnterNewBlock` is set.

### 3.2 Journal (`eefs_nand_journal.c`, phase 2)

`EEFS_NandChain_t` is defined in `eefs_nand_types.h` (2.5).

```c
int32   EEFS_NandBuildRings(Vol);                 /* fills Mirror[m].Blocks from Working.Map (DIR blocks that are not BAD, checked with EEFS_NandHasBadMarker at mount) */
int32   EEFS_NandScanMirror(Vol, uint8 M, EEFS_NandChain_t *C);     /* FMT 5 steps 1-3 for one mirror; reads only; never returns IO_ERROR (unreadable blocks are skipped) */
int32   EEFS_NandApplyChain(Vol, uint8 M, const EEFS_NandChain_t *C, EEFS_NandDirState_t *Dst,
                            uint16 *Cleanup, uint16 *CleanupCount, uint32 *MaxFilesSeen);
        /* second pass: re-reads the chain and applies items into Dst (FMT 5 step 5). SUCCESS, EEFS_GEOMETRY_MISMATCH, EEFS_IO_ERROR (a page that validated in pass 1 failed now) */
int32   EEFS_NandWriteRecord(Vol, uint8 M, uint8 RecType, uint32 Seq, const EEFS_NandDirState_t *State,
                             const EEFS_NandDirState_t *Base /* NULL for CHECKPOINT */, const uint8 *EntryMask,
                             const uint16 *Cleanup, uint16 CleanupCount);
        /* serializes and programs one record into mirror M at Mirror[M].NextPage (caller guarantees it fits, or has entered a new block). Verifies each page. SUCCESS or EEFS_IO_ERROR (caller handles bad marking + retry). */
uint32  EEFS_NandRecordPages(Vol, uint8 RecType, const EEFS_NandDirState_t *State, const EEFS_NandDirState_t *Base,
                             const uint8 *EntryMask, uint16 CleanupCount);   /* dry run of the serializer: page count */
int32   EEFS_NandEnterBlock(Vol, uint8 M);         /* advance to the next ring block and erase it; on erase failure mark bad and try the next; SUCCESS or EEFS_IO_ERROR (mirror Disabled) */
int32   EEFS_NandWriteRecordToMirror(Vol, uint8 M, uint32 Seq, boolean ForceCheckpoint, const uint8 *EntryMask,
                                     const uint16 *Cleanup, uint16 CleanupCount);
        /* decides TXN vs CHECKPOINT (5.4 step 3), enters a block when needed, retries on failure up to EEFS_NAND_WRITE_RETRIES marking blocks bad; updates Mirror[M] on success */
int32   EEFS_NandResync(Vol);                      /* for each mirror with NeedsResync && !Disabled: EnterBlock + CHECKPOINT at Seq Vol->LastSeq with State = Base; clears the flag */
int32   EEFS_NandCommit(Vol, boolean ForceCheckpointBoth);   /* 5.4 */
int32   EEFS_NandAbort(Vol);                                 /* 5.5 */
int32   EEFS_NandRunPendingCleanup(Vol);                     /* 5.6; SUCCESS always (failed erases mark bad) */
int32   EEFS_NandAddCleanup(Vol, uint16 Block);              /* append to PendingCleanup; EEFS_TRANSACTION_ERROR when full */
boolean EEFS_NandCleanupHasRoom(Vol, uint32 Needed);
void    EEFS_NandForgetCleanup(Vol, uint16 Block);           /* remove Block if present (called by the allocator) */
void    EEFS_NandDiff(Vol, uint8 *EntryMask, uint32 *MapRanges);   /* computes the dirty entry mask (memcmp + ForceDirty) and counts changed map bytes */

/* Added in phase 2 (journal.c), so that EEFS_LibFormat / EEFS_LibInitFS stay thin and phase 4 can hook the mount: */
#define EEFS_NAND_MASK_BYTES / EEFS_NAND_MASK_TEST(m,i) / EEFS_NAND_MASK_SET(m,i)   /* entry bit masks (EntryMask, ForceDirty) */
int32   EEFS_NandCheckGeometry(Vol);               /* 5.1 step 1 on Vol->Dev; fills the cached sizes; uses Vol->Base as scratch. SUCCESS / INVALID_ARGUMENT */
uint32  EEFS_NandMaxCheckpointPages(Vol);          /* dry run of a CHECKPOINT with every entry in use and the full map */
int32   EEFS_NandFormat(Vol);                      /* 5.1 steps 2-6 */
int32   EEFS_NandMount(Vol);                       /* 5.2 steps 2-9 (step 5 returns EEFS_NOT_FORMATTED until phase 4) */
boolean EEFS_NandHasUncommitted(Vol, boolean IncludeForceDirty);   /* Working != Base (entries, map, batch cleanup entries) */
extern EEFS_FileDescriptor_t EEFS_FileDescriptorTable[];          /* declared in the internal header for Abort's busy check */
```

### 3.3 Allocation and lookup (`eefs_nand_fileapi.c`, phase 3; declared in the internal header so phases 4-6 use them)

```c
int32   EEFS_NandParseName(const char *Name, char *Path /* EEFS_NAND_MAX_PATH_SIZE */, uint8 *SlotSpec);
        /* strips a trailing "@A"/"@B" (case-sensitive); SlotSpec = A, B or NONE. EEFS_INVALID_ARGUMENT if Path is empty, does not start with '/', is >= EEFS_NAND_MAX_PATH_SIZE, or contains a second '@'. */
int32   EEFS_NandFindEntry(Vol, const char *Path);           /* index in Working, or EEFS_FILE_NOT_FOUND */
int32   EEFS_NandAllocEntry(Vol);                            /* lowest index with Working.Entry[i].InUse == 0, or EEFS_NO_SPACE_LEFT_ON_DEVICE */
#define EEFS_NAND_ALLOC_NEW       0    /* new file, CopySlot: must leave >= EEFS_NAND_RESERVE_BLOCKS free */
#define EEFS_NAND_ALLOC_MODIFY    1    /* copy-on-write modify, Rename, SetAttr: must leave >= EEFS_NAND_BADBLOCK_RESERVE free */
#define EEFS_NAND_ALLOC_REPLACE   2    /* replacement for a grown bad block: may take the last free block */
uint32  EEFS_NandFreeCount(Vol);                                             /* number of Working.Map bytes == FREE in [DirRegionBlocks, BlockCount) */
int32   EEFS_NandAllocBlocks(Vol, uint32 Count, uint16 *Out, uint8 Owner, uint8 Class);
        /* 6.9: fails with EEFS_NO_SPACE_LEFT_ON_DEVICE, leaving the map untouched, unless FreeCount - Count >= reserve(Class);
           otherwise takes Count FREE blocks round-robin from Vol->AllocCursor, marks them Owner, forgets them in PendingCleanup,
           and leaves AllocCursor after the last block taken. */
void    EEFS_NandReleaseBlocks(Vol, const uint16 *Blocks, uint32 Count);   /* back to FREE (used when a write fails or a batch slot is dropped) */
void    EEFS_NandFreeSlot(Vol, uint32 Entry, uint8 Slot);                   /* every Working.Map byte == OWNER(Entry,Slot) -> FREEING; adds FirstBlock to cleanup (caller checked room); zeroes the slot record */
void    EEFS_NandFreeBlocksExcept(Vol, uint32 Entry, uint8 Slot, const uint16 *Keep, uint32 KeepCount);
        /* every Working.Map byte == OWNER(Entry,Slot) whose block is not in Keep[] -> FREEING (copy-on-write, 6.3 step 7) */
uint32  EEFS_NandBlocksNeeded(Vol, uint32 Size);                             /* max(1, ceil((PageSize+Size)/BlockBytes)) */
boolean EEFS_NandBlockUnchanged(Vol, uint16 OldBlock, uint32 K, uint32 OldCrc, uint32 OldLen, const uint8 *NewSlice, uint32 NewLen);
        /* 6.3 step 5: TRUE iff OldLen == NewLen, CRC32(NewSlice) == OldCrc and every byte of the old block's data pages for
           position K (read uncached, page by page) equals NewSlice. Any read error -> FALSE. */
int32   EEFS_NandValidateSlot(Vol, uint32 Entry, uint8 Slot, boolean Full);  /* page 0 (+ all block CRCs when Full); sets/clears SLOTFLAG_CORRUPT; SUCCESS / CORRUPT_FILE / IO_ERROR */
int32   EEFS_NandWriteCopy(Vol, uint32 SrcEntry, uint8 SrcSlot, EEFS_NandPage0_t *NewP0, uint16 *DstBlocks);
        /* copies the data of (SrcEntry,SrcSlot) page by page into DstBlocks and writes NewP0 as page 0 of DstBlocks[0]; used by
           CopySlot, Rename, SetAttr. Ordering, verify and retry rules in 7.5; on a grown bad block it replaces that entry of
           DstBlocks[] in place (hence non-const) and restarts. */
void    EEFS_NandTouch(Vol);                                                 /* VolumeChangeCounter++ */
```

### 3.4 Rebuild (`eefs_nand_rebuild.c`, phase 4)

```c
int32   EEFS_NandRebuild(Vol);              /* 8.1; fills Base = Working; SUCCESS, EEFS_NOT_FORMATTED */
int32   EEFS_NandReclaimOrphans(Vol);       /* 8.4 */
```

## 4. Block map and state model

- `Working` is what every API call reads and modifies. `Base` is the committed state as last read
  from or written to the media, with one class of exception: facts the library *detects* about the
  media rather than pending changes are recorded in both `Base` and `Working` with `ForceDirty[i]`
  set, so that Abort keeps them and the next commit persists them. They are `ActiveFailed` set by
  reconcile, Open, VerifyFile or the health check when the active copy fails validation, and the
  copied-field repair of reconcile (5.2 step 6) when a valid page 0 disagrees with its entry.
- **File content never changes in place.** Every rewrite of a slot allocates a new first block
  and new blocks for the changed data (copy-on-write, 6.2), so `Base` always describes an intact
  committed copy and the directory's copied fields are never stale in a committed state.
- **Dirty set at commit** = entries where `memcmp(&Working.Entry[i], &Base.Entry[i])` differs,
  plus `ForceDirty`; map bytes where `Working.Map[b] != Base.Map[b]`.
- **Bad blocks** are marked in `Working.Map` only (`0xFE`). Abort preserves them (5.5), so they are
  "sticky" and get persisted by the next commit through the normal diff.
- **FREEING (`0xFC`)**: blocks released by Remove, Rename, SetAttr, CopySlot and copy-on-write
  modifies inside the current batch. The allocator never hands them out. At commit they are
  serialized as `0xFF` and set to `0xFF` in both states after the record is written. Abort returns
  them to their `Base` value. FREEING is purely map state: a modify that replaces hundreds of data
  blocks costs hundreds of map bytes and **one** cleanup-list entry.
- The **pending-cleanup list holds first blocks only**: a slot's old first block goes on it whenever
  the slot is removed or rewritten, so a committed removal or rewrite is followed by an erase of
  the superseded page 0. Data blocks need no cleanup: they carry no page 0, and they are erased
  when reallocated.
- `VolumeChangeCounter` increments on every call that changes `Working`, `Base`, `SlotFlags`, the
  map or the media (WriteFile, Remove, Rename, SetFileAttributes, CopySlot(+All), SetActiveSlot(+All),
  SetFallbackCompatible(+All), SetFailed, ClearFailed, Commit, Abort, RepairDirectory,
  RebuildDirectory, ReclaimOrphans, MarkBad, and VerifyFile/Open when they set a flag). As delivered
  (phase 6), `EEFS_NandProgramPageVerified` and `EEFS_NandEraseBlock` also bump it, so every media change
  (including the pending-cleanup erases at the start of a mutating call and the programs of a write that
  later fails) restarts a running health check, whichever call made it.

## 5. Directory algorithms

### 5.1 Format (`EEFS_LibFormat`)

1. Validate: `Scratch` not mounted, `Dev` non-NULL, geometry per FMT 1.1; `PageSize` power of two in
   `[2048, EEFS_NAND_MAX_PAGE_SIZE]`; `OobSize in [1, EEFS_NAND_MAX_OOB_SIZE]`;
   `BlockCount in [DirRegionBlocks+1, EEFS_NAND_MAX_BLOCKS]`; `DirRegionBlocks` even, `>= 4`,
   `<= EEFS_NAND_MAX_DIR_BLOCKS`; `BadMarkerOffset < OobSize`; `BadMarkerPages != 0`;
   `PagesPerBlock >= 2 * (MaxCheckpointPages + 1)` where `MaxCheckpointPages =
   EEFS_NandRecordPages(CHECKPOINT with all EEFS_MAX_FILES entries in use and the full map)`.
   Failure: `EEFS_INVALID_ARGUMENT`. Zero `Scratch`, set `Dev` and cached geometry.
2. Map: `[0, DirRegionBlocks)` = DIR, rest FREE. **Scan every block** `0..BlockCount-1` with
   `EEFS_NandHasBadMarker`; bad ones become BAD. Nothing has been erased yet.
3. Erase every non-BAD block (dir and data). An erase failure marks the block BAD (OOB marker
   best effort). This removes every stale metadata page, so a later rebuild cannot see pre-format
   files.
4. `EEFS_NandBuildRings`. If either mirror has fewer than 2 good blocks: `EEFS_IO_ERROR` (the
   device is left erased; the caller may retry with a larger `DirRegionBlocks`).
5. State: all entries unused, `WriteSeqHigh = 0`, `FormatTime = EEFS_LIB_TIME`, `LastSeq = 1`,
   `Base = Working`, `PendingCleanupCount = 0`. Both mirrors: `CurIndex = 0`, `NextPage = 0`,
   `HasState = 0`.
6. Write a CHECKPOINT `Seq = 1` to mirror 0 then mirror 1 via `EEFS_NandWriteRecordToMirror(M, 1,
   ForceCheckpoint = TRUE, ...)` (the first block of each ring; already erased, so `EnterBlock`
   is skipped when `HasState == 0` and the block's page 0 is erased). If neither mirror accepts
   it: `EEFS_IO_ERROR`. If one does: `EEFS_SUCCESS` (the other is resynced by the first commit).
7. Zero `Scratch`.

Power loss: on a blank device, either no valid checkpoint (mount returns `EEFS_NOT_FORMATTED`) or a
valid empty volume. When a device that holds a volume is formatted again, blocks are erased in
ascending order, so a cut while the directory region is being erased can leave the old volume
mountable from the mirror that was not erased yet (the complete old state: its data blocks are erased
later), and a cut while the data region is being erased leaves no directory but some old metadata
pages: the mount then rebuilds (8.1) and recovers the old files that were not erased yet, until the last
old metadata page is erased (`EEFS_NOT_FORMATTED`). In every case formatting again succeeds. (Tested by the
`powercut_sweep_format` case: old volume, then a rebuilt directory, then `NOT_FORMATTED`, then empty, in that
order, at every cut point. Its synthetic files have no data, so their pages fail the first-block CRC and the
rebuilt directory is empty.)

### 5.2 Mount (`EEFS_LibInitFS`)

Never writes NAND. Reads at most `DirRegionBlocks x (1 + BadMarkerPages count) + 2 x 2 x
PagesPerBlock + 2 x EEFS_MAX_FILES` pages.

1. Validate `T` not mounted and the geometry as in 5.1 step 1 (`EEFS_INVALID_ARGUMENT`). Zero
   `T`, set `Dev`, cached sizes, `MaxBlocksPerFile`.
2. `Working.Map`: `[0, DirRegionBlocks)` = DIR, rest FREE. For each dir block run
   `EEFS_NandHasBadMarker`; bad ones = BAD. `EEFS_NandBuildRings`.
3. `EEFS_NandScanMirror` for both mirrors. Choose `M* = argmax LastSeq` among mirrors with
   `Valid`, tie -> mirror 0. If none is valid: go to step 5.
4. `EEFS_NandApplyChain(M*)` into `Working` (first resetting entries/map, keeping the BAD dir
   marks from step 2: after applying, re-apply BAD for dir blocks found bad in step 2 and for any
   `0xFE` already in the map). `EEFS_GEOMETRY_MISMATCH` propagates. `LastSeq = Chain.LastSeq`,
   `PendingCleanup` = the chain's list. Mirror bookkeeping for each `m`: `HasState = Valid`;
   `CurIndex/NextPage/CheckpointSeq/LastSeq` from its chain; `NeedsResync = !Valid || LastSeq <
   Vol->LastSeq`; `EnterNewBlock = Torn || NeedsResync`. Go to step 6.
5. **No directory**: `EEFS_NandRebuild` (8.1). `EEFS_NOT_FORMATTED` if it finds no valid metadata
   page (FMT 3.4). On success `DirectoryRebuilt = 1`, `LastSeq = max(MaxSeqSeen over both mirrors) + 1`,
   both mirrors `NeedsResync = EnterNewBlock = 1`, `HasState = 0`, no pending cleanup. Nothing is
   written: the first commit (its resync checkpoint, even when the batch is empty) or
   `EEFS_LibRepairDirectory` persists the rebuilt state. As delivered, steps 6-8 are
   `EEFS_NandMountFinish` (journal.c), which `EEFS_LibRebuildDirectory` shares.
6. **Reconcile.** For each entry `i` in use, each slot `s` in use: `EEFS_NandReadPage0(FirstBlock)`.
   - Invalid (`CORRUPT_FILE`/`IO_ERROR`): `SlotFlags[i][s] |= CORRUPT`. If `s` is the active slot
     of an `IsAB` entry: `ActiveFailed = 1` in `Working` and `Base`, `ForceDirty[i]`. Map bytes
     stay as the directory says.
   - Valid but `Path != Entry.Path`, or page `Slot` is A/B and `!= s`: treat as invalid (same
     handling).
   - Valid: for each `Blocks[k].Block = b`: if `Working.Map[b]` is FREE, FREEING, or
     `OWNER(i,s)`: set `Working.Map[b] = Base.Map[b] = OWNER(i,s)`; if it is another owner, DIR or
     BAD: `SlotFlags[i][s] |= CROSSLINK` (first claimant keeps the block; health reports it). Then
     every map byte still equal to `OWNER(i,s)` that is not in `Blocks[]` becomes FREE (defensive:
     a committed map and a committed page 0 should never disagree). If `BlockCount != AllocBlocks`
     or the copied fields (`FileSize`, `Version`, `WriteSeq`, `FileCrc32`, `ModificationDate`)
     differ from the entry (which, with copy-on-write, indicates a corrupted record rather than an
     expected state): page 0 wins; update them in `Working` and `Base` and set `ForceDirty[i]` so
     the next commit repairs the snapshot. Track `MaxPageWriteSeq`.
7. `Base = Working` for everything not already mirrored (entries, map), `ForceDirty` as set above.
   `AllocCursor` = the data block after the highest block whose map value is an owner (wrapping
   to `DirRegionBlocks`); `DirRegionBlocks` when nothing is owned (6.9).
8. `NextWriteSeq = max(Working.WriteSeqHigh, MaxPageWriteSeq) + 1 + EEFS_NAND_WRITESEQ_MOUNT_GAP`.
   The gap keeps a committed rewrite ahead of orphans that were assigned higher sequence numbers
   in an uncommitted batch before a power cut, so a rebuild prefers the committed copy (bounded
   insurance; see FMT 6 caveats).
9. `Mounted = 1`, `VolumeChangeCounter = 1`. Return `EEFS_SUCCESS`.

### 5.3 Chain scan details (`EEFS_NandScanMirror`)

```
for each ring block b (ascending): read page 0 (uncached, Data+Oob)
    skip if read failed or not a valid record page or RecType != CHECKPOINT or PageIndex != 0
    candidate (b, Seq); MaxSeqSeen = max(MaxSeqSeen, Seq)
sort candidates by Seq descending
for each candidate:
    p = 0; LastSeq = none
    loop:
        read page p; if not a valid record page: break
        H = header; if H.PageIndex != 0: break
        if p == 0 and H.RecType != CHECKPOINT: break (malformed block, ignore candidate)
        if p > 0 and (H.RecType != TXN or H.Seq != LastSeq + 1): break
        if p + H.PageCount > PagesPerBlock: break
        read pages p+1 .. p+PageCount-1; each must be valid, same Seq/RecType, PageIndex in order; else break
        LastSeq = H.Seq; (p == 0: CheckpointSeq = H.Seq); p += H.PageCount
    if no valid checkpoint at p == 0: continue with the next candidate (this block is torn-at-entry: set Torn for the chain we eventually pick)
    C.Valid = 1; C.BlockIdx = ring index of b; C.CheckpointSeq; C.LastSeq; C.NextPage = p
    C.Torn |= (p < PagesPerBlock and page p is not erased (read it; any read error counts as not erased))
    return
C.Valid = 0
```

A page that fails validation and is not erased stops the chain and sets `Torn` even when it is
followed by erased pages: the mirror will move to a new block (section 5.4) rather than program
after a possibly half-programmed page.

Ties (as delivered): two blocks of a mirror can carry a checkpoint with the same `Seq` (for example a
resync checkpoint when the mirror's own chain already ended at that `Seq`). The scan therefore walks
every candidate whose checkpoint `Seq` equals the best one found and keeps the longest chain (highest
`LastSeq`), instead of stopping at the first valid candidate. `Torn` is also set when any candidate
with a `Seq` at least that of the chosen checkpoint has an invalid checkpoint.

### 5.4 Commit (`EEFS_NandCommit`)

```
if PendingCleanupBase > 0: EEFS_NandRunPendingCleanup            /* 5.6 (it only handles the media-loaded entries) */
EEFS_NandResync                                                   /* lagging/missing mirrors get a CHECKPOINT at Seq LastSeq */
compute dirty: EntryMask, MapChanged, Cleanup = first blocks of FREEING slots... (collected during the batch in PendingCleanup, see below)
if !ForceCheckpointBoth and no dirty entries and no changed map bytes and PendingCleanupCount == 0: return SUCCESS
Seq = LastSeq + 1
Working.WriteSeqHigh = NextWriteSeq - 1
written = 0
for M in 0, 1:
    if Mirror[M].Disabled: continue
    rc = EEFS_NandWriteRecordToMirror(M, Seq, ForceCheckpointBoth, EntryMask, PendingCleanup, PendingCleanupCount)
    if rc == SUCCESS: written++                 /* a failed mirror has EnterNewBlock set by the retry loop */
if written == 0: Working.WriteSeqHigh = Base.WriteSeqHigh; update resync flags; return EEFS_IO_ERROR    /* nothing committed; Working intact (bad marks stay) */
/* committed */
for every b: if Working.Map[b] == FREEING: Working.Map[b] = FREE
Base = Working; clear ForceDirty; LastSeq = Seq; DirectoryRebuilt = 0
update resync flags: for each enabled mirror, NeedsResync = EnterNewBlock = 1 iff !HasState or Mirror.LastSeq < LastSeq
run cleanup now: for each b in PendingCleanup: if b is a data block, Working.Map[b] == FREE and page 0 of b is not erased: EraseBlock(b) (failure -> MarkBad); PendingCleanupCount = 0
EEFS_NandTouch
return SUCCESS
```

Note the pending-cleanup list used *inside* the batch: `EEFS_NandFreeSlot` appends the slot's
first block to `PendingCleanup` during the batch (so the list doubles as "this batch's cleanup").
Capacity is checked (`EEFS_NandCleanupHasRoom`) by every operation before it changes anything, and
`EEFS_TRANSACTION_ERROR` is returned if the operation would overflow it. Abort clears the entries
added during the batch (5.5). A cleanup list loaded at mount is drained by
`EEFS_NandRunPendingCleanup` at the start of the first mutating call (see 5.6), before any
allocation can hand out one of those blocks.

`EEFS_NandWriteRecordToMirror(M, Seq, Force, ...)`:

```
retries = 0
loop:
    Pages = EEFS_NandRecordPages(TXN...)
    UseCheckpoint = Force || !Mirror[M].HasState || Mirror[M].EnterNewBlock || Mirror[M].NeedsResync || Mirror[M].NextPage + Pages > PagesPerBlock
    if UseCheckpoint and (Mirror[M].HasState or Mirror[M].EnterNewBlock or page 0 of ring block CurIndex is not erased):
        if EEFS_NandEnterBlock(M) != SUCCESS: return EEFS_IO_ERROR          /* mirror Disabled */
    RecType = UseCheckpoint ? CHECKPOINT : TXN
    rc = EEFS_NandWriteRecord(M, RecType, Seq, Working, UseCheckpoint ? NULL : Base, EntryMask, Cleanup, Count)
    if rc == SUCCESS:
        Mirror[M]: HasState = 1; LastSeq = Seq; if CHECKPOINT: CheckpointSeq = Seq; NextPage += pages written; EnterNewBlock = 0; NeedsResync = 0
        return SUCCESS
    /* program or verify failure in the current block */
    EEFS_NandMarkBad(ring block CurIndex)      /* removes it from the ring; CurIndex now points at the next block */
    Mirror[M].EnterNewBlock = 1
    if ++retries > EEFS_NAND_WRITE_RETRIES or Mirror[M].Disabled: return EEFS_IO_ERROR
```

The resync flag rule (as delivered) replaces the first revision's "`NeedsResync = 1` on any failed
mirror": a mirror is only resynced when it really lags, so a resync never writes a checkpoint whose
`Seq` equals the mirror's own last record; a failed mirror that does not lag (nothing was committed)
simply starts a new block with a CHECKPOINT at the next commit. `NeedsResync` also forces a CHECKPOINT
in `EEFS_NandWriteRecordToMirror`, so a mirror whose resync failed is never given a TXN that cannot
chain onto its records. `EEFS_NandResync` writes `State = Base` with the first `PendingCleanupBase`
cleanup entries (none at commit time, since the pending cleanup has just run). After a rebuild (5.2 step 5,
8.3) both mirrors need a resync, and the resync checkpoint of `Base` at `LastSeq` is what persists the
rebuilt state: as delivered, `EEFS_NandCommit` clears `DirectoryRebuilt` as soon as a mirror holds a record
at `LastSeq`, so a `Commit` with nothing else to write still persists a rebuilt directory.

`EEFS_NandEnterBlock(M)`: `CurIndex = (CurIndex + 1) % Count` (or 0 if `!HasState`), erase that
block; on failure `MarkBad` it (the ring shrinks, indexes shift: recompute `CurIndex` to the block
that followed) and try the next, until success or the ring is empty (`Disabled`, `EEFS_IO_ERROR`).
`NextPage = 0`. Entering never erases the block holding the newest valid state of that mirror
unless the ring has a single block (then the other mirror protects the state; format guarantees
two).

`EEFS_NandWriteRecord`: builds pages one at a time in `PageBuf`: begin page (header with
`PageIndex`, `PageCount` from the dry run), append items with the encoders (VOLUME first; then
MAPCHUNKs: for CHECKPOINT the whole map in greedy chunks that fill each page; for TXN the changed
ranges, where a *range* is a maximal run of changed bytes extended to merge gaps of fewer than 16
unchanged bytes; then ENTRY items for `EntryMask` bits in index order; then CLEANUP), moving to the
next page when an encoder returns 0; seal; program with verify (decode the read-back header and
compare `Seq`, `PageIndex`, `RecType`, CRC). Any failure returns `EEFS_IO_ERROR` immediately. The
dry run (`EEFS_NandRecordPages`) runs the same packing without programming, so `PageCount` is exact.

Crash points: see FMT 8. In addition, a crash inside `MarkBad`'s OOB write is harmless (the map
mark is persisted by the next record or lost, in which case the block is simply retried later).

### 5.5 Abort (`EEFS_NandAbort`)

```
if any open descriptor refers to (i,s) whose Working slot differs from Base slot: return EEFS_DEVICE_IS_BUSY
for each (i,s) with Working.Entry[i].Slot[s].InUse and Working FirstBlock != Base FirstBlock (slot created or rewritten in the batch):
    best effort: erase Working FirstBlock only (its page 0 would otherwise be an orphan); failure -> MarkBad.
    Never touch the other blocks of the slot: with copy-on-write they may be shared with the committed copy.
    (As delivered: only when FirstBlock is a data block that is FREE in Base and not BAD, and its page 0 is not erased.
    Phase 3 adds the same erase for the batch's own cleanup-list entries that are FREE in Base and FREEING in Working:
    first blocks the batch created and then superseded itself.)
for each b: if Working.Map[b] == BAD: keep; else Working.Map[b] = Base.Map[b]
Working.Entry[] = Base.Entry[]; Working.WriteSeqHigh = Base.WriteSeqHigh
PendingCleanupCount = count loaded at mount or left by the last commit that is still pending (i.e. remove entries added during the batch: the list is kept as [mount-pending | batch-added], tracked by PendingCleanupBase index)
ForceDirty unchanged (detected media facts still need persisting, section 4)
AllocCursor unchanged
EEFS_NandTouch; return SUCCESS
```

`PendingCleanupBase` is a `uint16` in the volume: the number of leading entries that came from
media (mount) rather than from the batch; `RunPendingCleanup` sets it to 0 and `Commit` resets it.

### 5.6 Pending cleanup (`EEFS_NandRunPendingCleanup`)

Called at the top of **every mutating API** (WriteFile, Remove, Rename, SetFileAttributes,
CopySlot(+All), Commit, RepairDirectory, ReclaimOrphans) when `PendingCleanupBase > 0`, before the
operation allocates or changes anything: for each of the first `PendingCleanupBase` blocks: if its
page 0 is not erased, erase it (failure -> `MarkBad`). Then remove them from the list and set
`PendingCleanupBase = 0`. As delivered, a listed block is only erased if it is a data block whose
`Working.Map` byte is FREE (as FMT 4.3.4 guarantees for a well-formed record), so a corrupt or stale
list can never erase a live file. `EEFS_NandAddCleanup` ignores a block that is already listed. The list of a commit is executed inside that commit (5.4), so after a
clean commit nothing is pending. Mount never runs it.

## 6. File operations

### 6.1 Name resolution

`EEFS_NandParseName` splits `Name` into `Path` and `SlotSpec`. Rules shared by all lookups:

| Caller | `SlotSpec` NONE | `SlotSpec` A/B |
|---|---|---|
| Open / Stat / GetFileInfo / VerifyFile | `IsAB ? ActiveSlot : A`. Open applies the fallback rule (7.3); the others never fall back. | that slot (must be in use, else `EEFS_FILE_NOT_FOUND`); never falls back |
| WriteFile | existing entry: `IsAB ? ActiveSlot : A`, unless `Meta->Slot` is A/B which acts like a suffix; new entry: `Meta->Slot` if A/B (sets `IsAB`), else A with `IsAB = 0` | that slot; sets `IsAB = 1` on the entry |
| Remove | the whole entry (all slots) | that slot only (6.5) |
| Rename / SetFileAttributes / SetActiveSlot / SetFallbackCompatible / SetFailed / ClearFailed / CopySlot | the entry | `EEFS_INVALID_ARGUMENT` |

A stored path of 255 characters is the maximum (`strlen < EEFS_NAND_MAX_PATH_SIZE`); the suffix
is not stored. Paths must begin with `/`; no other validation (the volume is flat: `/cf/apps/x.so`
is one name and `/cf` is not a directory). As delivered, `@` is reserved: after a trailing `@A`/`@B`
is removed, a path that still contains `@` is `EEFS_INVALID_ARGUMENT` (this is what "a second `@`"
means, and it also rejects unknown suffixes such as `@C` or `@a`).

### 6.2 Copy-on-write rule (user decision, 2026-10-15)

A slot's blocks are **never erased or programmed again while the slot is live**. Rewriting a slot
that is in use:

- writes a **new first block** always (its page 0 carries the new `WriteSeq`, `FileSize`, CRCs and
  `Blocks[]`, so it can never be reused);
- writes a fresh block for every data position `k >= 1` whose content changed, or which did not
  exist before (growth);
- **keeps** the old physical block, listed again at the same position, for every data position
  whose content is unchanged (6.3 step 5 defines "unchanged" safely);
- marks the old first block and every replaced or dropped old block FREEING, and puts the old
  first block on the cleanup list.

The slot's `FirstBlock` therefore changes on every rewrite, which **is a directory change**: it is
staged in the pending batch and made live by the next `EEFS_Commit`. The user accepted this on
2026-10-15; it supersedes the plan's "modifying a file does not update the directory". What it buys:
a power cut at any point before the commit leaves the old copy intact and the new blocks free
(the ordinary rollback path); a reader never sees a torn or mixed file; a failed rewrite never
destroys the old data; and the per-rewrite cost is proportional to the amount of changed data plus
one block, not to the file size.

There is no fallback to an in-place write under any circumstance. If the free space (after the
reserve rule, 6.9) does not cover the new first block plus the changed blocks, the call returns
`EEFS_NO_SPACE_LEFT_ON_DEVICE` and changes nothing.

### 6.3 WriteFile (`EEFS_LibWriteFile`)

```
 1. ParseName; Meta != NULL; FileType <= 5; Meta->Slot in {A, B, NONE}; Data != NULL || Size == 0.
    EEFS_LIB_IS_WRITE_PROTECTED -> EEFS_READ_ONLY_FILE_SYSTEM.
 2. RunPendingCleanup if needed.
 3. Resolve entry i (FindEntry or AllocEntry) and slot s (6.1). Existing entry with READONLY attribute -> EEFS_PERMISSION_DENIED
    (write a new entry instead). Any open descriptor on (i, s) -> EEFS_PERMISSION_DENIED.
 4. Need = EEFS_NandBlocksNeeded(Size). Need > MaxBlocksPerFile (a size no metadata page can describe) -> EEFS_INVALID_ARGUMENT
    (as delivered, matching the return list of 2.6; a file that merely does not fit the free space is NO_SPACE in step 5).
    Mode = slot in use ? MODIFY : NEW. MODIFY needs cleanup room for 1 block (EEFS_NandCleanupHasRoom) else EEFS_TRANSACTION_ERROR.
 5. Build the position table Pos[0..Need-1], each {Block, Kept}:
      NEW:    all positions new.
      MODIFY: read the old page 0 (EEFS_NandReadPage0 of Slot[s].FirstBlock) -> OldCount, OldBlocks[], OldCrc[], OldSize.
              If it is unreadable/corrupt (the slot is flagged CORRUPT): no position can be kept; all new.
              Position 0 is always new. For k in 1 .. Need-1:
                  if k < OldCount: (NewOff, NewLen) = EEFS_NandBlockDataRange(Size, k); OldLen = EEFS_NandBlockDataRange(OldSize, k);
                      Kept = EEFS_NandBlockUnchanged(OldBlocks[k], k, OldCrc[k], OldLen, Data + NewOff, NewLen)
                             (length equal, CRC equal, then a byte compare of the old block's data pages against Data; a CRC
                             match alone is only probabilistic and a false "unchanged" would keep wrong data silently, so the
                             compare is mandatory; a read error during the compare means "changed")
                  else Kept = FALSE
              Kept positions take Block = OldBlocks[k].
      NewCount = number of positions with Kept == FALSE (>= 1).
      EEFS_NandAllocBlocks(NewCount, NewList, OWNER(i,s), NEW ? ALLOC_NEW : ALLOC_MODIFY) -> EEFS_NO_SPACE_LEFT_ON_DEVICE
          (nothing changed yet; a fresh entry allocated in step 3 is simply left unused). Assign NewList to the new positions in order.
 6. Attempt (repeat up to EEFS_NAND_WRITE_RETRIES times):
      P0: WriteSeq = NextWriteSeq++; Path; FileType, Slot = (IsAB ? s : NONE), Attributes, Version from Meta; FileSize = Size;
          FileCrc32 = CRC(Data, Size); CreationDate = existing entry's CreationDate or now; ModificationDate = now; Type = Meta->Type;
          SigType = 0; BlockCount = Need; Blocks[k] = Pos[k].Block; Crc[k] = CRC of the slice of Data for k (kept positions
          reproduce OldCrc[k] by construction).
      Program order: every NEW position k = 1 .. Need-1 ascending, then position 0. Kept positions are not touched at all.
          For each new k: EraseBlock(Pos[k].Block); for each page that holds data (k == 0: page 0 = encoded P0 built in PageBuf;
          data pages from page 1): build the page (memcpy the data slice, 0xFF tail) in PageBuf and EEFS_NandProgramPageVerified.
          Pages beyond the data are not programmed.
      On any erase/program/verify failure at a new block b = Pos[k].Block:
          EEFS_NandMarkBad(b); allocate one replacement for position k with ALLOC_REPLACE (EEFS_NO_SPACE_LEFT_ON_DEVICE -> fail);
          restart the attempt (Blocks[] changed, so P0 is rebuilt; new blocks already programmed are erased again as the attempt
          proceeds; kept blocks are still never touched).
      After the last retry fails (or no replacement block): ReleaseBlocks(all new blocks), return EEFS_IO_ERROR (or
          EEFS_NO_SPACE_LEFT_ON_DEVICE). The slot is unchanged in every mode: the old copy was never written to.
 7. Success. Working.Entry[i]: InUse = 1, Path, FileType, Attributes, CreationDate (new entries), IsAB per 6.1.
      MODIFY: EEFS_NandFreeBlocksExcept(i, s, kept blocks) (old first block and every replaced/dropped old block -> FREEING) and
      EEFS_NandAddCleanup(old FirstBlock) *before* assigning the new slot record.
      Slot[s] = {InUse 1, FirstBlock = Pos[0].Block, AllocBlocks = Need, FileSize, Version, WriteSeq, FileCrc32, ModificationDate}.
      If s is the active slot and ActiveFailed was set, clear ActiveFailed in Working (a rewrite repairs it). Clear SlotFlags[i][s].
 8. EEFS_NandTouch. If EEFS_NAND_AUTO_COMMIT: EEFS_NandCommit. Return EEFS_SUCCESS.
```

Cost of a MODIFY: one page read per page of every kept candidate (only positions whose length and
CRC match are read), one erase + the data pages of every changed block, and one block for page 0.

Power loss during step 6 (NEW or MODIFY): directory unchanged; the old copy, if any, is intact
because none of its blocks was erased or programmed; the new blocks are free in the committed map;
possibly an orphan page 0 that shares the kept blocks with the live copy, which a rebuild resolves by
the overlap rule (8.1). Position 0 is written last, so such an orphan exists only if every other new
block is complete; the first block's own data pages, however, follow its page 0 (pages are programmed
in ascending order), so an orphan's first block may be incomplete. A rebuild therefore checks
`Blocks[0].Crc32` before it accepts a candidate (8.1). The `powercut` sweeps assert this invariant
after every cut: no metadata page on the device lists a block at position >= 1 whose data does not
match its CRC. Power loss after the commit but before the cleanup erase: the new copy is live and
the old first block is on the pending cleanup list (5.6).

A/B: writing the inactive slot is the same procedure; A and B never share blocks (CopySlot copies
every block), so a modify of one slot cannot touch the other. Writing the active slot is equally
safe (the old copy stays live until the commit), which removes the plan's warning about in-place
writes to the active copy.

The lock is held for the whole call (a 200 MB write blocks readers for its duration; see risks).

### 6.4 Open and validation

`EEFS_LibOpen(T, Name, O_RDONLY, 0)`: resolve (6.1 / 7.3). `EEFS_NandValidateSlot(i, s, Full =
EEFS_NAND_VERIFY_ON_OPEN)`: reads page 0 (`CORRUPT_FILE` on any FMT 3.4 failure, `Path` mismatch
or explicit `Slot` mismatch) and, only when `Full`, every data page accumulating the per-block
CRCs and the file CRC (`CORRUPT_FILE` on mismatch, `IO_ERROR` on uncorrectable). **By default
(`EEFS_NAND_VERIFY_ON_OPEN = 0`, user decision 2026-10-15) Open checks page 0 only**; data CRCs are
checked by the bootloader, `EEFS_LibVerifyFile` and the health check. With copy-on-write a live copy
is never partially rewritten, so the open-time data check only guards against media decay, which
the health check covers. Failure sets
`SLOTFLAG_CORRUPT`; for the active slot of an `IsAB` entry also `ActiveFailed = 1` in `Working`
and `Base` with `ForceDirty[i]`. Success clears `SLOTFLAG_CORRUPT`. The descriptor records
`InodeIndex`, `Slot`, `Fallback`, `FirstBlock`, `FileSize` (from page 0), `CurK = none`.

### 6.5 Remove

```
ParseName; RunPendingCleanup; i = FindEntry (-> FILE_NOT_FOUND); READONLY -> PERMISSION_DENIED; write-protected -> READ_ONLY_FILE_SYSTEM
NONE: any open descriptor on i -> PERMISSION_DENIED; cleanup room for (slots in use) else TRANSACTION_ERROR;
      FreeSlot for each in-use slot; Entry.InUse = 0 (zero the whole entry in Working).
A/B:  slot not in use -> FILE_NOT_FOUND; descriptor open on (i,s) -> PERMISSION_DENIED;
      s == ActiveSlot and the other slot in use -> PERMISSION_DENIED (switch first);
      FreeSlot(i, s); if no slot remains in use: zero the entry; else if s was the active slot: ActiveSlot = other, ActiveFailed = 0.
Touch; AUTO_COMMIT.
```

### 6.6 Rename and SetFileAttributes (first-block rewrite)

Only the **first block** of each in-use slot is rewritten: a new block receives the new page 0
(new `Path` or `Attributes`) followed by verbatim copies of the old first block's data pages. Every
other block of the file stays where it is and keeps its CRC. The cost is one block per slot instead
of the whole file. They are transactional (nothing in `Base` changes). This is the copy-on-write
rule of 6.2 with every data position kept.

```
Rename(Old, New): both parse with SlotSpec NONE (else INVALID_ARGUMENT); FindEntry(New) must fail (-> PERMISSION_DENIED, as EEPROM);
    i = FindEntry(Old) (-> FILE_NOT_FOUND); READONLY -> PERMISSION_DENIED; open descriptors on i -> PERMISSION_DENIED.
SetFileAttributes(Name, Attr): Attr in {NONE, READONLY} else INVALID_ARGUMENT; i = FindEntry; open descriptors -> PERMISSION_DENIED.
    (READONLY files may have attributes changed: this is how READONLY is cleared.)
Common:
    cleanup room for (slots in use); for each in-use slot s (A then B):
        read page 0 of the slot (CORRUPT_FILE aborts the whole operation with no change; blocks allocated so far are released)
        allocate ONE block NB with ALLOC_MODIFY -> NO_SPACE_LEFT_ON_DEVICE (release an earlier slot's NB; no change)
        NewP0 = old P0 with Path/Attributes replaced, Blocks[0].Block = NB (Blocks[0].Crc and Blocks[1..] unchanged),
                WriteSeq = NextWriteSeq++, ModificationDate unchanged (Rename) / now (SetAttr)
        erase NB; program NewP0 at page 0; copy pages 1..LastUsedPage(block 0) from the old first block (ReadPage -> ProgramPage);
        verify per 7.5 (Blocks[0].Crc over the copied data); grown bad -> mark bad, pick another NB, retry (7.5 retry limit); IO_ERROR -> release, no change
    then for each slot: map[old FirstBlock] = FREEING + cleanup(old FirstBlock); map[NB] = OWNER(i, s);
         slot record FirstBlock = NB, WriteSeq = new (AllocBlocks and other blocks unchanged)
    Rename: Entry.Path = New.  SetAttr: Entry.Attributes = Attr.
    Touch; AUTO_COMMIT.
```

As delivered: `SetFileAttributes` with the value the entry already has returns `EEFS_SUCCESS` and
writes nothing. A copy-source CRC mismatch is `EEFS_CORRUPT_FILE` (the source slot is flagged), and a
missing replacement block is reported as `EEFS_IO_ERROR`, as in 7.5.

Power loss: before the commit the old first block is intact and the directory still names it, so
the file is unchanged; after the commit the new first block is authoritative and the cleanup erase
of the old first block is idempotent. Until that erase, the old and the new page 0 both list
`Blocks[1..]`: mount ignores this (reconcile follows the directory's `FirstBlock`), the health check
does not report the old page 0 as an orphan or cross-link while it is on the pending cleanup list,
and a rebuild resolves it by the overlap rule (8.1): the higher `WriteSeq` wins.

Free space of one block per in-use slot is required.

### 6.7 Read

`EEFS_LibRead(fd, Buf, Len)`: `n = min(Len, FileSize - ByteOffset)`. While `n > 0`: compute the
block index `k` and page for `ByteOffset` (FMT 3.3): `ByteOffset < D0` -> `k = 0`, `page = 1 +
ByteOffset / PageSize`, `off = ByteOffset % PageSize`; else `r = ByteOffset - D0`, `k = 1 + r /
BlockBytes`, `page = (r % BlockBytes) / PageSize`, `off = r % PageSize`. If `k != CurK`: read page
0 of `FirstBlock` (cached) and fetch `Blocks[k]` -> `CurBlock` (an invalid page 0 here returns
`EEFS_CORRUPT_FILE`). Read `(CurBlock, page)` through the cache; copy `min(n, PageSize - off)`.
Uncorrectable -> `EEFS_IO_ERROR` (bytes already copied are lost; the offset is not advanced).
Returns bytes read. A descriptor is bound to the `FirstBlock` it was opened with; a copy-on-write
rewrite of the same slot would strand it after the commit's cleanup erase, which is why WriteFile,
Rename and SetFileAttributes refuse to touch an open slot, and Abort refuses while a descriptor on
a batch-created copy exists (5.5).

### 6.8 VerifyFile and GetFileInfo

`EEFS_LibVerifyFile`: resolve without fallback; `EEFS_NandValidateSlot(i, s, Full = TRUE)`; on
failure of the active slot set `ActiveFailed` (as 6.4). `EEFS_LibGetFileInfo`: fill from the
`Working` entry (both `SlotInfo[]`, with `Corrupt` from `SlotFlags`), then read page 0 of the
resolved slot for `Type`, `FileSize`, `FileCrc32`, `Version` (page 0 wins over the entry when they
differ); `CORRUPT_FILE` leaves `Type` zeroed and returns the code.

### 6.9 Block allocation, the reserve pool and volume info (user decision, 2026-10-15)

**Reserve.** A number of free blocks is held back so that an existing file can always be rewritten
(copy-on-write needs new blocks even to change one byte) and a grown bad block can always be
replaced, however full the volume is with new files. The reserve is a **count**, not a fixed set of
blocks; which physical blocks are free changes all the time.

| Allocation class | Used by | Allowed iff, after taking `Count` blocks, free blocks remaining `>=` |
|---|---|---|
| `EEFS_NAND_ALLOC_NEW` | WriteFile on a slot not in use (new file or new A/B copy), CopySlot | `EEFS_NAND_RESERVE_BLOCKS` (16) |
| `EEFS_NAND_ALLOC_MODIFY` | WriteFile on a slot in use (6.3), Rename, SetFileAttributes (6.6) | `EEFS_NAND_BADBLOCK_RESERVE` (4) |
| `EEFS_NAND_ALLOC_REPLACE` | replacement of a block that failed to erase/program/verify during any write (6.3, 7.5, 6.6) | 0 |

"Free" means `Working.Map[b] == FREE` for `b >= DirRegionBlocks`; FREEING blocks do not count
until the commit, so a batch that removes files does not gain space for itself until it commits
(this keeps rollback safe). An allocation that would break its class's floor fails with
`EEFS_NO_SPACE_LEFT_ON_DEVICE` before anything is written or changed. `EEFS_NAND_BADBLOCK_RESERVE
<= EEFS_NAND_RESERVE_BLOCKS` is enforced at compile time. Both default to small numbers because
the reserve is also unusable capacity; a volume with many large files that are rewritten often
should raise `EEFS_NAND_RESERVE_BLOCKS` to the largest expected per-rewrite change plus one.

**Round-robin cursor.** `EEFS_NandAllocBlocks` scans `Working.Map` from `Vol->AllocCursor` upward,
wrapping from `BlockCount - 1` to `DirRegionBlocks`, taking FREE blocks in the order found until
`Count` are taken; it then leaves `AllocCursor` just after the last block taken. A block freed by a
rewrite is therefore not reused until the cursor has gone around the whole data area, which spreads
erase cycles over every free block instead of hammering the lowest-numbered ones (first-fit would
reuse the same handful of blocks for every small rewrite). The cursor lives in RAM only: at mount
it is set to the block after the highest owned block (`DirRegionBlocks` if none), which resumes
roughly where the previous session left off; after a reboot a different starting point costs
nothing in correctness and only negligibly in wear balance, so persisting it (which would need a
new VOLUME field, i.e. a format change) is not worth it. Format sets it to `DirRegionBlocks`.
Abort leaves it where it is.

**Volume info.** `EEFS_LibGetVolumeInfo` (2.6) reports the counts from `Working.Map` and the
entry table without any NAND read, so housekeeping can show free space, the reserve levels and
whether a batch is open. The health check adds `FREE_BELOW_RESERVE` (10.3) when the free count has
fallen under `EEFS_NAND_RESERVE_BLOCKS`.

## 7. A/B

### 7.1 State

Per entry: `IsAB`, `ActiveSlot`, `FallbackCompatible`, `ActiveFailed` (FMT 4.3.3), all in the
directory only; page 0 carries `Slot` and `Version` so a rebuild recovers locations and versions.

### 7.2 Setters

- `SetActiveSlot(Name, s)`: entry must be `IsAB` (else `INVALID_ARGUMENT`), `Slot[s].InUse` (else
  `FILE_NOT_FOUND`). `ActiveSlot = s`; `ActiveFailed = 0` (switching clears the flag). Touch.
- `SetActiveSlotAll(s)`: for every in-use `IsAB` entry with `Slot[s].InUse`: as above; returns the
  count. Entries lacking slot `s` are skipped (reported later by health as `AB_ACTIVE_MISSING` only
  if their active slot is missing, which this does not cause).
- `SetFallbackCompatible(Name, f)` / `...All(f)`: `IsAB` required; sets the flag (any non-zero `f` is
  stored as 1). Returns count for All.
- `SetFailed(Name)` / `ClearFailed(Name)`: `IsAB` required; set/clear `ActiveFailed`. `SetFailed`
  is what the application calls after the bootloader reported `ActiveFailed` through the boot-info
  handoff. All of these write `Working` only and need a commit. As delivered: the volume must not be
  write-protected (`EEFS_READ_ONLY_FILE_SYSTEM`); `READONLY` files are not refused (the flags are directory
  metadata, not file content); setting a value an entry already has still counts for the `All` variants, and
  `SetActiveSlot` to the slot that is already active still clears `ActiveFailed`.

### 7.3 Open resolution with fallback

```
s = SlotSpec != NONE ? SlotSpec : (IsAB ? ActiveSlot : A)
if !Slot[s].InUse: return FILE_NOT_FOUND
rc = ValidateSlot(i, s, VERIFY_ON_OPEN)
if rc == SUCCESS: open s, Fallback = 0
else if SlotSpec == NONE && IsAB && Slot[1-s].InUse && FallbackCompatible:
    rc2 = ValidateSlot(i, 1-s, VERIFY_ON_OPEN)
    if rc2 == SUCCESS: open 1-s, Fallback = 1 (ActiveFailed was set by the failed validation)
    else return rc2
else return rc
```

`Stat` never falls back (it reports the active slot's directory data; `ActiveFailed` is visible
through `GetFileInfo`).

### 7.4 CopySlot

```
ParseName (NONE only); From != To, both in {A, B}; i = FindEntry; IsAB required... (a single-copy file can be copied into B to opt in:
    if !IsAB and From == A: allowed, sets IsAB = 1 on success)
Slot[From].InUse else FILE_NOT_FOUND; descriptor open on (i, To) -> PERMISSION_DENIED; READONLY -> PERMISSION_DENIED
RunPendingCleanup; cleanup room for 1 if Slot[To].InUse else TRANSACTION_ERROR
read From's page 0 (CORRUPT_FILE/IO_ERROR -> flag the source as a failed validation (SLOTFLAG_CORRUPT, ActiveFailed if it is the active
    copy) and return, no change); allocate AllocBlocks(From) blocks for OWNER(i, To) with ALLOC_NEW (NO_SPACE -> return)
NewP0 = From's P0 with Slot = To, WriteSeq = NextWriteSeq++; Blocks[] = new list; CRCs carried over
EEFS_NandWriteCopy(i, From, NewP0, NewBlocks)  -> CORRUPT_FILE (source block CRC mismatch while copying: release, return) / IO_ERROR (release, return)
if Slot[To].InUse: FreeBlocksExcept(i, To, NewBlocks) + AddCleanup(old FirstBlock)
    (not FreeSlot: the new blocks already carry OWNER(i, To), and FreeSlot would mark them FREEING as well)
Slot[To] = {InUse 1, FirstBlock, AllocBlocks, FileSize, Version (From's), WriteSeq (new), FileCrc32, ModificationDate (From's)}
if To == ActiveSlot: ActiveFailed = 0 (repaired). Clear SlotFlags[i][To]. IsAB = 1. Touch; AUTO_COMMIT.
```

`CopySlotAll(From, To)`: for each in-use `IsAB` entry with `Slot[From].InUse`: `CopySlot`; stop at
the first error and return it (earlier copies stay in `Working`; the caller decides to Commit or
Abort); otherwise return the count. Single-copy entries are not included (opting in is a per-file
`CopySlot`). A source whose data fails its CRC while being copied is `EEFS_CORRUPT_FILE` with the
destination blocks released (and the first one erased if its page 0 was written), so the volume's free
count is as before the failing entry.

### 7.5 `EEFS_NandWriteCopy`

Copies `(SrcEntry, SrcSlot)` into `DstBlocks` with the same ordering as WriteFile (blocks 1..n-1
then 0, pages ascending). For each destination page: read the corresponding source page through
the cache (uncorrectable -> `EEFS_IO_ERROR`), accumulate the source block CRC, program the
destination page with verify. After each source block, compare its CRC with the source page 0's
`Blocks[k].Crc32` -> mismatch `EEFS_CORRUPT_FILE` (set source `SLOTFLAG_CORRUPT`). On a
destination erase/program/verify failure: `MarkBad`, allocate one replacement block for that
position with `ALLOC_REPLACE` (`NO_SPACE` -> `EEFS_IO_ERROR`), rebuild `NewP0` (`Blocks[]` changed) and restart the
copy, up to `EEFS_NAND_WRITE_RETRIES` times. The destination page 0 is encoded into `PageBuf` and
written last. On every failure, including a source page 0 that no longer reads before anything was
written, the destination blocks that are still owned are released (as delivered in phase 5; phase 3 left
them allocated in that early case). The copy goes page by page through the HAL; a BSP with a hardware block buffer may
optimize inside its `ProgramPage`.

## 8. Rebuild, repair, orphans

### 8.1 `EEFS_NandRebuild` (FMT 6)

```
pass 1: for b in [DirRegionBlocks, BlockCount): if Working.Map[b] == BAD skip;
        read page 0 + OOB (uncached). OOB marker set -> Map[b] = BAD, continue. ReadPage0 invalid -> continue.
        The data pages of b (Blocks[0]) do not match Blocks[0].Crc32 -> continue (an interrupted write: page 0
        precedes the first block's own data pages; every other block of a candidate is complete, 6.3).
        slot = P0.Slot in {A,B} ? P0.Slot : A. Find entry with Path (linear search of a scratch table of EEFS_MAX_FILES {Path, Winner[2] = {Block, WriteSeq, Explicit}});
        new path: take a free row, or ignore if full. If Winner[slot] empty or P0.WriteSeq > Winner.WriteSeq: Winner = (b, WriteSeq, P0.Slot != NONE).
        MaxPageWriteSeq = max(...).
pass 2: process the winners in DESCENDING WriteSeq order (overlap rule): read its page 0 again; if any of its Blocks[]
        is already OWNER of a winner processed earlier (higher WriteSeq), discard this winner entirely (orphan; this is the
        superseded page 0 of a copy-on-write rewrite, Rename or SetAttr, which shares its kept data blocks with the newer
        page 0, whose cleanup erase was cut; or the page 0 of an uncommitted rewrite that lost to the committed copy).
        Otherwise: Entry (row): InUse 1, Path, IsAB = any Explicit, FileType/Attributes/CreationDate from the active slot's P0
        (ActiveSlot = A present ? A : B), FallbackCompatible 0, ActiveFailed 0; Slot[s] = fields from P0; each Blocks[k] -> OWNER(row, s).
        A row whose winners were all discarded is dropped.
if no valid metadata page was seen: return EEFS_NOT_FORMATTED
Base = Working. NextWriteSeq per 5.2 step 8.
```

The scratch table (64 x (256 + 2 x 8) ~ 17 KB) is **built inside `Base.Entry[]`**, which is
unused during the rebuild, to avoid extra static RAM; pass 2 then writes `Working` and finally
copies to `Base`.

As delivered:

- **Bad-block markers** are read with `EEFS_NandHasBadMarker` (every `BadMarkerPages` page, as at
  format), not only from the OOB of page 0.
- **Candidate order.** Pass 1 compares `WriteSeq` before it checks `Blocks[0].Crc32`, so the data
  pages are read only for a candidate that would win. A candidate that fails the check is skipped
  without replacing the current winner, so an older complete copy still wins over a newer torn one.
  Ties in pass 2 (equal `WriteSeq`, which only a crafted image has) go to the lower first block.
- **`EEFS_NOT_FORMATTED`** is returned only when the data region holds no valid metadata page at all
  (FMT 3.4), matching the user requirement and the bootloader's scan. A device whose only metadata
  pages fail the first-block CRC mounts with an empty rebuilt directory. (The first revision said
  "no row", which would also have refused such a device.)
- **`IsAB`** is set from the accepted winners only (a discarded explicit-slot winner does not make
  the entry A/B).
- A winner that lists a block whose map value is BAD (a bad-block marker) is accepted; the block
  stays BAD and the mount's reconcile step flags the slot `CROSSLINK` (`rebuild_crosslink_flagged`).
- A path that finds the scratch table full is ignored: with more than `EEFS_MAX_FILES` paths on the
  media, the paths whose first metadata page comes last in block order are left out (their blocks
  stay free). A row taken by a path whose winners are all discarded is not reused.
- `Working.WriteSeqHigh` = the highest `WriteSeq` of every valid metadata page seen (also of losers and
  rejected pages), so `NextWriteSeq` (5.2 step 8) is above every page on the device.
- `FormatTime` is not recoverable and is 0 until a checkpoint is written.

### 8.2 `EEFS_LibRepairDirectory`

`RunPendingCleanup`; then `EEFS_NandCommit(ForceCheckpointBoth = TRUE)`: both mirrors enter a new
block and receive a CHECKPOINT with `Seq = LastSeq + 1` (uncommitted changes are committed by the
same record). Clears `DirectoryRebuilt`. `EEFS_IO_ERROR` if neither mirror accepts it. (After a
rebuild, the resync of 5.4 first writes a CHECKPOINT at `LastSeq` into a new block of each mirror,
then the forced CHECKPOINT at `LastSeq + 1` goes into the next block.)

### 8.3 `EEFS_LibRebuildDirectory`

Requires no open files/dir (`DEVICE_IS_BUSY`), `Working == Base` apart from `ForceDirty`
(`TRANSACTION_ERROR`), no active health check. Re-runs mount steps 2, 5 (rebuild), 6, 7, 8 on the
mounted volume. Use: a directory that mounted but is believed wrong.

As delivered: a pending cleanup loaded from the media is run first (so a committed removal whose
cleanup erase was cut does not come back); step 2 is not re-run (the directory-region map, with its
bad blocks, and the mirror rings in RAM are current; data-region BAD marks are kept too). `LastSeq` =
`max(LastSeq, MaxSeqSeen of both mirrors) + 1`; the mirrors keep their state but get `NeedsResync =
EnterNewBlock = 1`, so the next commit writes a CHECKPOINT of the rebuilt state into a new block of each
ring without erasing the current one. `ForceDirty` and `SlotFlags` are cleared before the rebuild, and
`NextWriteSeq` never decreases. Nothing is written. When the rebuild finds no metadata page
(`EEFS_NOT_FORMATTED`), the volume is mounted again from its directory (still nothing written; the last
health summary is kept) and the code is returned; if that mount fails too, the volume is left unmounted.

### 8.4 `EEFS_LibReclaimOrphans`

`RunPendingCleanup`. For each `b` in `[DirRegionBlocks, BlockCount)` with `Working.Map[b] ==
FREE`: read page 0; if it is a valid metadata page (any content), erase `b` (failure ->
`MarkBad`). Blocks that are owned (including by uncommitted batch slots) or FREEING are never
touched, so a batch in progress is safe. Returns the number erased. Reads `BlockCount -
DirRegionBlocks` pages and holds the lock throughout.

As delivered (phase 6): a block is erased only if it is FREE in both `Working` and `Base` (as the allocator
requires), is not on the pending cleanup list (either part), and `EEFS_NandReadPage0` validates its page 0
(FMT 3.4 with `ExpectBlock = b`; a factory-bad block fails the read and is skipped). A failed erase marks the
block bad and is not counted, so `EEFS_IO_ERROR` is never returned; `EEFS_READ_ONLY_FILE_SYSTEM` is returned
when `EEFS_LIB_IS_WRITE_PROTECTED`. The volume is touched only when something was erased. The internal
`EEFS_NandReclaimOrphans` does the work under the caller's lock.

## 9. Bootloader library (`libraries/microeefs_nand`)

Links only `microeefs_nand.c`, `eefs_nand_format.c`, `eefs_crc32.c`. No globals except the hook
pointer. Needs `EEFS_BACKEND_NAND` defined (the `microeefs` target gets it from `eefs_headers`).

```c
typedef uint8 (*MicroEEFS_NandSlotHook_t)(const char *Name, uint8 DirectorySlot, void *HookCtx);

typedef struct {
    EEFS_NandFileInfo_t Info;         /* Path, FileType, Version, Attributes, FileSize, FileCrc32, Type, IsAB, ActiveSlot,
                                         FallbackCompatible, ActiveFailed (directory value OR this boot's failure), SlotInfo[2] */
    uint8   LoadedSlot;               /* slot whose data was loaded/validated */
    uint8   UsedScan;                 /* 1 if the directory was unusable and the scan fallback was used */
    uint8   DirectoryValid;
    uint8   Reserved;
    uint32  DirSeq;                   /* LastSeq of the chain used, 0 when scanned */
} MicroEEFS_NandInfo_t;

void  MicroEEFS_NandSetSlotHook(MicroEEFS_NandSlotHook_t Hook, void *HookCtx);   /* NULL = default (returns DirectorySlot) */
int32 MicroEEFS_NandFindFile(const EEFS_NandDevice_t *Dev, uint8 *PageBuffer, const char *Name, MicroEEFS_NandInfo_t *Info);
int32 MicroEEFS_NandLoadFile(const EEFS_NandDevice_t *Dev, uint8 *PageBuffer, const char *Name, void *Dest, uint32 MaxSize,
                             MicroEEFS_NandInfo_t *Info);
```

`PageBuffer` is `Dev->Geometry.PageSize` bytes supplied by the caller. The OOB is read with a
small stack buffer only at `BadMarkerOffset` (the HAL is asked for the whole OOB into a stack
array of `EEFS_NAND_MAX_OOB_SIZE`; bootloaders with tight stacks can define it smaller).

Algorithms: FMT 7.2-7.4 exactly. `FindFile` validates page 0 of the resolved slot (and applies the
fallback rule at page-0 level: if the primary's page 0 is invalid and the other is compatible and
valid, it reports the other as `LoadedSlot` with `ActiveFailed = 1`). `LoadFile` loads and checks
CRCs (full rule). Return codes: `EEFS_SUCCESS`, `EEFS_INVALID_ARGUMENT`, `EEFS_FILE_NOT_FOUND`,
`EEFS_GEOMETRY_MISMATCH` (VOLUME item disagrees), `EEFS_CORRUPT_FILE`, `EEFS_IO_ERROR`,
`EEFS_NO_SPACE_LEFT_ON_DEVICE` (`FileSize > MaxSize`), `EEFS_NOT_FORMATTED` (no directory and the
scan found no metadata page at all). Page reads: `DirRegionBlocks + 2 x 2 x PagesPerBlock`
(two passes over at most two chains) + 1 for page 0 + 1 per data page loaded (full pages are read
straight into the destination and only the file's last, partial page goes through `PageBuffer`, so page 0
is never re-read); scan fallback adds `BlockCount - DirRegionBlocks`, plus the first block's data pages of
each page 0 of the name that would win (the first-block CRC check of FMT 7.2 step 3, added in phase 4).

Candidate tracking: one `EEFS_NandEntry_t` (328 bytes) plus its index; `MAPCHUNK` and `CLEANUP`
items are skipped by length. A checkpoint resets the candidate before its items are applied.

## 10. Health check (`eefs_nand_health.c`)

### 10.1 Types (`inc/eefs_nand_types.h`, before `EEFS_InodeTable_t`; `EEFS_HealthReport_t` after it)

```c
/* Finding codes */
#define EEFS_HEALTH_DIR_MIRROR_MISSING        1   /* Slot = mirror; no valid chain */
#define EEFS_HEALTH_DIR_MIRROR_MISMATCH       2   /* Slot = lagging mirror; Aux0 = its LastSeq, Aux1 = the other's */
#define EEFS_HEALTH_DIR_RECORD_CORRUPT        3   /* Slot = mirror, Block, Aux0 = page: a non-erased page that fails validation inside/after the chain */
#define EEFS_HEALTH_DIR_UNCOMMITTED_TXN       4   /* Slot = mirror, Block, Aux0 = page, Aux1 = Seq: a torn record with a readable header after the chain */
#define EEFS_HEALTH_DIR_CLEANUP_PENDING       5   /* Block: listed in the newest record's CLEANUP and still holds a non-erased page 0 */
#define EEFS_HEALTH_DIR_REBUILT_NOT_PERSISTED 6
#define EEFS_HEALTH_FILE_CORRUPT             10   /* Path, Slot, Block = first block whose CRC failed (or FirstBlock for a bad page 0), Aux0 = 0 page0 / 1 block crc / 2 file crc */
#define EEFS_HEALTH_FILE_IO_ERROR            11   /* Path, Slot, Block, Aux0 = page */
#define EEFS_HEALTH_BLOCK_NEEDS_SCRUB        12   /* Path, Slot, Block, Aux0 = max corrected bits seen in the block */
#define EEFS_HEALTH_AB_A_CORRUPT             20
#define EEFS_HEALTH_AB_B_CORRUPT             21
#define EEFS_HEALTH_AB_ACTIVE_CORRUPT        22
#define EEFS_HEALTH_AB_ACTIVE_MISSING        23
#define EEFS_HEALTH_AB_VERSION_MISMATCH      24   /* Aux0 = Version A, Aux1 = Version B (informational) */
#define EEFS_HEALTH_AB_FALLBACK_UNAVAILABLE  25   /* informational */
#define EEFS_HEALTH_AB_FAILED_FLAG_SET       26
#define EEFS_HEALTH_AB_LOAD_MIXED            27   /* one finding; Path = first entry whose ActiveSlot differs from entry 0's (informational) */
#define EEFS_HEALTH_DIR_PAGE0_MISMATCH       30   /* Path, Slot, Block, Aux0 = reason (1 Path, 2 Slot, 3 BlockCount, 4 copied fields): page 0 disagrees with the directory entry */
/* 31 was DIR_PAGE0_STALE (removed with in-place modify; copied fields can no longer be legitimately stale, see 10.3). Do not reuse. */
#define EEFS_HEALTH_MAP_MISMATCH             32   /* Block, Aux0 = map value, Aux1 = OWNER expected or 0xFFFF: map disagrees with the Blocks[] that claim/should claim it */
#define EEFS_HEALTH_BLOCK_CROSSLINKED        33   /* Path, Slot (second claimant), Block, Aux0 = OWNER of the first claimant */
#define EEFS_HEALTH_BADBLOCK_UNRECORDED      34   /* Block */
#define EEFS_HEALTH_ORPHAN_FILE              35   /* Path (from page 0), Slot (page 0 Slot), Block, Aux0 = WriteSeq */
#define EEFS_HEALTH_FREE_BELOW_RESERVE       40   /* Aux0 = free blocks, Aux1 = EEFS_NAND_RESERVE_BLOCKS; informational unless free < EEFS_NAND_BADBLOCK_RESERVE */

/* Summary bits */
#define EEFS_HEALTH_OK            0x00
#define EEFS_HEALTH_DEGRADED      0x01   /* any non-informational finding */
#define EEFS_HEALTH_AB_ISSUE      0x02   /* codes 20-23, 26 */
#define EEFS_HEALTH_DIR_ISSUE     0x04   /* codes 1-6, 30, 32-35 */
#define EEFS_HEALTH_FILE_ISSUE    0x08   /* codes 10-12 */
#define EEFS_HEALTH_INFO          0x10   /* codes 24, 25, 27, and 40 while free >= EEFS_NAND_BADBLOCK_RESERVE */
#define EEFS_HEALTH_OVERFLOW      0x20   /* findings dropped */
#define EEFS_HEALTH_INCOMPLETE    0x40   /* not finished (only in a snapshot taken before DONE) */
#define EEFS_HEALTH_SPACE_ISSUE   0x80   /* code 40 present (with DEGRADED when free < EEFS_NAND_BADBLOCK_RESERVE) */

typedef struct {
    uint16  Code;
    uint8   Slot;              /* slot or mirror index, 0xFF if n/a */
    uint8   Reserved;
    uint16  EntryIndex;        /* 0xFFFF if n/a */
    uint16  Block;             /* 0xFFFF if n/a */
    uint32  Aux0, Aux1;
    char    Path[EEFS_NAND_HEALTH_PATH_SIZE];   /* truncated, NUL-terminated, "" if n/a */
} EEFS_HealthFinding_t;                          /* 80 bytes */

typedef struct {
    uint32  Summary;
    uint32  FilesChecked, SlotsChecked, PagesRead, CorrectedBits, Restarts, Steps;
    uint32  FindingCount, Overflow;
    uint32  DirSeq;            /* Vol->LastSeq when the check completed */
    uint32  Complete;
    EEFS_HealthFinding_t Finding[EEFS_NAND_MAX_HEALTH_FINDINGS];
} EEFS_HealthSummary_t;                          /* 2,604 bytes at the defaults */

typedef struct EEFS_HealthReport {
    EEFS_HealthSummary_t S;
    EEFS_InodeTable_t   *Volume;
    uint32  ChangeCounter;     /* VolumeChangeCounter at (re)start */
    uint8   Phase;             /* 0 init, 1 directory, 2 files, 3 ab, 4 scan, 5 done */
    uint8   Sub;               /* sub-step inside the phase */
    uint8   Mirror, Slot;
    uint16  EntryIndex, BlockIdx;
    uint32  Page, Block, DataOffset;
    uint32  RunBlockCrc, RunFileCrc, MaxCorrectedInBlock;
    uint32  ExpectBlockCrc;
    /* added in phase 6: every cursor of the resumable check */
    uint32  StepReads;                                   /* page reads in the current step */
    uint8   Marker, Verdict, FirstActive, Reserved;      /* bad-marker page cursor; phase 2 slot verdict; phase 3 LOAD_MIXED state */
    uint32  K, FirstBlock, BlockCount, FileSize, FileCrc32;           /* phase 2: Blocks[] index and the slot's page 0 */
    uint32  WalkBlock, WalkPage, WalkSub, WalkRecSeq, WalkRecCount, WalkLast, WalkCkpt;   /* phase 1 chain walk */
    uint8   WalkValid, WalkTail, TornCand[2];
    uint32  WalkTailPage, WalkTailSeq;
    uint32  ChainBlock[2];
    uint8   ChainTail[2], Reserved2[2];
    uint32  ChainTailPage[2], ChainTailSeq[2];
    uint32  CandSeq[EEFS_NAND_MAX_DIR_BLOCKS];           /* phase 1 scratch */
    uint8   CandState[EEFS_NAND_MAX_DIR_BLOCKS];         /* 0 none, 1 candidate, 2 walked, 3 torn */
    EEFS_NandChain_t Chain[2];
    uint8   FailedAtStart[(EEFS_MAX_FILES + 7) / 8];     /* ActiveFailed when the check (re)started */
    uint8   SetByCheck[(EEFS_MAX_FILES + 7) / 8];        /* ActiveFailed set by this check (kept across restarts) */
    uint8   Claimed[EEFS_NAND_MAX_BLOCKS / 8];           /* blocks claimed by a validated page 0 */
    uint8   SlotResult[EEFS_MAX_FILES][2];               /* 0 unchecked, 1 ok, 2 corrupt, 3 io error, 4 not in use */
} EEFS_HealthReport_t;                               /* ~3.5 KB at the defaults */
```

### 10.2 Protocol

- `Start(T, Report)`: `T->ActiveReport` must be NULL (else `DEVICE_IS_BUSY`); zero `Report`, set
  `Volume`, `ChangeCounter = T->VolumeChangeCounter`, `Phase = 1`. Returns `SUCCESS`.
- `Step(T, MaxPageReads)`: `EEFS_LIB_LOCK`; `R = T->ActiveReport` (NULL -> `INVALID_ARGUMENT`);
  `R->S.Steps++`. If `T->VolumeChangeCounter != R->ChangeCounter`: `Restarts++`, clear findings,
  counters (except `Restarts`, `Steps`), `Claimed`, `SlotResult`, set `Phase = 1`, `ChangeCounter
  = current`. Then run the state machine until `MaxPageReads` pages have been read in this step
  (every HAL read counts, including cached misses only, i.e. count actual `ReadPage` calls) or the
  phase machine reaches DONE. `MaxPageReads = 0` is treated as 1. On DONE: `S.Complete = 1`,
  `S.DirSeq = T->LastSeq`, compute `Summary`, copy `S` into `T->LastReport`, set
  `T->ActiveReport = NULL`, return `SUCCESS`. Else return `1`. `EEFS_LIB_UNLOCK` before return.
- `Summary(T, Out)`: copies `T->LastReport` if `Complete`, else `EEFS_ERROR`.
- Findings are appended with `AddFinding`; after `EEFS_NAND_MAX_HEALTH_FINDINGS` they increment
  `Overflow`. Each finding is reported once per (Code, EntryIndex, Slot, Block) tuple (the adder
  dedupes against the existing list).
- The check **never writes NAND**. Its only side effects on RAM are `SLOTFLAG_CORRUPT` and
  `ActiveFailed` (6.4 semantics) when a slot fails in phase 2, which do *not* bump
  `VolumeChangeCounter` from inside the check (to avoid self-restart).

### 10.3 Phases and exact conditions

**Phase 1, directory.** Sub 0: for each dir block (cursor `BlockIdx`), read page 0 with OOB; bad
marker set while `Working.Map[b] != BAD` -> `BADBLOCK_UNRECORDED`; record a candidate exactly as
5.3. Sub 1 (per mirror, cursor `Mirror`, `Page`): walk the best candidate page by page, as 5.3,
building `Chain[m]`. After the chain: if the next page is not erased: decode its header; readable
header with `PageIndex == 0` and `Seq == LastSeq + 1` -> `DIR_UNCOMMITTED_TXN`, otherwise
`DIR_RECORD_CORRUPT`. Any candidate block with a higher `Seq` whose checkpoint was torn ->
`DIR_UNCOMMITTED_TXN` for that block (`Aux1 = Seq`). Sub 2 (no reads): `!Chain[m].Valid` ->
`DIR_MIRROR_MISSING(m)` for each such mirror; both valid and `LastSeq` differ ->
`DIR_MIRROR_MISMATCH`; both missing and `!DirectoryRebuilt` is impossible after a successful
mount, but report both. `T->DirectoryRebuilt` -> `DIR_REBUILT_NOT_PERSISTED`. Sub 3: for each
block in `T->PendingCleanup[0..PendingCleanupBase)`: read page 0; not erased ->
`DIR_CLEANUP_PENDING`. (Blocks added during the current batch are not pending-from-media and are
not reported.)

**Phase 2, files.** Cursor `(EntryIndex, Slot, BlockIdx, Page)`. For each in-use entry and slot
(`Working`): read page 0 of `FirstBlock`:
- invalid -> `FILE_CORRUPT(Aux0 = 0)`, `SlotResult = 2` (or `3` and `FILE_IO_ERROR` on read
  failure); set `SLOTFLAG_CORRUPT`; next slot.
- `Path` differs (`Aux0 = 1`), or page `Slot` is explicit and differs (`2`), or `BlockCount !=
  AllocBlocks` (`3`), or `Version`/`WriteSeq`/`FileSize`/`FileCrc32`/`ModificationDate` differ from
  the entry (`4`) -> `DIR_PAGE0_MISMATCH`, then continue checking the content anyway. (With
  copy-on-write a committed entry always matches the page 0 it was committed with, so any of these
  is a real inconsistency, not an expected transient; reconcile repairs reason 4 at the next mount.)
- for each `Blocks[k]`: already in `Claimed` -> `BLOCK_CROSSLINKED` (Aux0 = `Working.Map[b]`);
  else set it; `Working.Map[b] != OWNER(i,s)` -> `MAP_MISMATCH(Aux1 = OWNER)`.
- then for each `Blocks[k]`, each data page (page 0 is re-read at each block transition to fetch
  `Blocks[k]`, counted in `PagesRead`): read uncached with `Corrected`; uncorrectable ->
  `FILE_IO_ERROR`, `SlotResult = 3`, abandon the slot; `Corrected >= EccScrubThreshold > 0` ->
  track `MaxCorrectedInBlock`; accumulate `RunBlockCrc` over the clipped range and `RunFileCrc`.
  At the end of a block: `MaxCorrectedInBlock >= threshold` -> `BLOCK_NEEDS_SCRUB`; `RunBlockCrc !=
  Blocks[k].Crc32` -> `FILE_CORRUPT(Block = b, Aux0 = 1)`, `SlotResult = 2`, abandon the slot.
  After the last block: `RunFileCrc != FileCrc32` -> `FILE_CORRUPT(Block = FirstBlock, Aux0 = 2)`.
  Otherwise `SlotResult = 1`. `SlotsChecked++`; `FilesChecked++` per entry. Slots not in use get
  `SlotResult = 4`.
Also, after all entries: for every block `b >= DirRegionBlocks` with `Working.Map[b]` an owner
value whose `Claimed` bit is clear -> `MAP_MISMATCH(Aux0 = map value, Aux1 = 0xFFFF)` (the map
says owned but no valid page 0 lists it; this covers blocks of slots whose page 0 is invalid as
well, which is intended: the map cannot be trusted for them). Map values `DIR` at `b >=
DirRegionBlocks` or owner `>= 2 * EEFS_MAX_FILES` -> `MAP_MISMATCH` too. No reads.

**Phase 3, A/B** (no reads). For each in-use `IsAB` entry: `SlotResult[A] == 2 or 3` -> `AB_A_CORRUPT`;
`[B]` -> `AB_B_CORRUPT`; `SlotResult[ActiveSlot] in {2,3}` -> `AB_ACTIVE_CORRUPT` and set
`ActiveFailed = 1` in `Working` and `Base`, `ForceDirty`; `!Slot[ActiveSlot].InUse` ->
`AB_ACTIVE_MISSING`; both in use and `Version` differ -> `AB_VERSION_MISMATCH`;
`!Slot[1-ActiveSlot].InUse || !FallbackCompatible` -> `AB_FALLBACK_UNAVAILABLE`;
`ActiveFailed` was already set at phase start -> `AB_FAILED_FLAG_SET`. Across entries: if any two
`IsAB` entries have different `ActiveSlot` -> one `AB_LOAD_MIXED`.

**Phase 4, scan.** Cursor `Block` from `DirRegionBlocks`. Read page 0 with OOB (uncached). Bad
marker set and `Working.Map[b] != BAD` -> `BADBLOCK_UNRECORDED`. Valid metadata page (FMT 3.4,
`ExpectBlock = b`), `Claimed` bit clear and `b` not on the pending cleanup list -> `ORPHAN_FILE`
(a block awaiting its cleanup erase, that is the old first block of any copy-on-write rewrite,
modify, Rename or SetAttr, is already reported by `DIR_CLEANUP_PENDING`). Such a superseded page 0
lists blocks that the live copy also lists; it is **not** reported as `BLOCK_CROSSLINKED` either:
cross-links are detected only among the directory's own slots in phase 2. (Blocks owned by
uncommitted batch slots have been claimed in phase 2 because phase 2 iterates `Working`, so an open
batch does not produce false orphans; and a batch's rewritten slot shares its kept blocks with the
committed copy that `Base` still names, which is likewise not a cross-link because phase 2 reads
`Working` only.)

**DONE.** Space check (no reads): `Free = EEFS_NandFreeCount(T)`; `Free < EEFS_NAND_RESERVE_BLOCKS`
-> `FREE_BELOW_RESERVE(Aux0 = Free, Aux1 = EEFS_NAND_RESERVE_BLOCKS)`, informational while `Free >=
EEFS_NAND_BADBLOCK_RESERVE`, degraded below it. Then `Summary` from the finding codes per the bit
table.

Cost: phase 1 <= `DirRegionBlocks + 4 x PagesPerBlock`; phase 2 = every data page of every slot +
`BlockCount` page-0 re-reads at most; phase 4 = `BlockCount - DirRegionBlocks`.

### 10.4 As delivered (phase 6)

Structure. Every unit of work reads at most one page, uncached, into `Vol->PageBuf` (or the OOB into
`Vol->OobBuf`), finishes with it and keeps everything it needs in the report, so a step can end after any read
and the report does not depend on `MaxPageReads` (the `health_maxpagereads_1_equals_one_big_step` case compares
1, 13 and unlimited). `Step` runs units while `StepReads < MaxPageReads`; with `MaxPageReads = 1` a check takes
`PagesRead + 1` steps. `PagesRead` counts every HAL read of the check; `CorrectedBits` sums the corrected bits
of the data reads (an OOB-only marker read reports none).

Restart. `H_Begin` keeps `Restarts`, `Steps` and `SetByCheck`, clears the rest. `AB_FAILED_FLAG_SET` means
"set before this check looked": `FailedAtStart` is taken at every (re)start and excludes the flags this check set
itself (`SetByCheck`), so a restart does not turn the check's own finding into `AB_FAILED_FLAG_SET`; a new check
started later does report it. The check's own flagging (`EEFS_NandSlotFailed`, now exported from
`eefs_nand_fileapi.c`) bumps the change counter, which the check puts back so that it never restarts itself.

Bad-block markers (phases 1 and 4). A block whose map value is `BAD` is skipped (no read). Otherwise each
`BadMarkerPages` page is read OOB-only (`Data = NULL`, as `EEFS_NandHasBadMarker` and the HAL contract require
for factory-bad blocks); a marker byte other than `0xFF` gives `BADBLOCK_UNRECORDED` and the block is not looked
at further. A failed OOB read is not reported as a marker (the page 0 read reports the block). The cost of phases
1 and 4 is therefore one read per marker page more per block than 10.3 says.

Phase 1. Candidates are the directory blocks that are not `BAD`, by the 5.3 rules (highest `Seq` first, the lower
block on a tie, the walk stops below the chosen checkpoint, equal `Seq` candidates are all walked and the longest
chain wins). The page where the chosen chain stops is classified during the walk, without a re-read: erased =
nothing; a record whose first page is valid but a later page is not = `DIR_UNCOMMITTED_TXN` (`Aux0` = its first
page, `Aux1` = its `Seq`); a page that fails validation but whose raw header has the record magic, layout version,
`PageIndex 0` and `Seq = LastSeq + 1` = `DIR_UNCOMMITTED_TXN`; anything else, including a read error =
`DIR_RECORD_CORRUPT` (`Aux0` = page, `Aux1` = 0). A candidate whose checkpoint is torn gives
`DIR_UNCOMMITTED_TXN` (`Aux0 = 0`, `Aux1` = its `Seq`) only when its `Seq` is strictly above the chosen
checkpoint's (an abandoned block with an equal `Seq` is harmless). These findings carry `Slot` = mirror and
`Block` = the directory block. `DIR_MIRROR_MISMATCH` carries `Slot` = the lagging mirror.

Phase 2. The per-slot verdict is what `EEFS_NandValidateSlot(Full = TRUE)` would decide: a page 0 that fails
FMT 3.4 (`FILE_CORRUPT`, `Aux0 0`) or cannot be read (`FILE_IO_ERROR`, `Aux0 0`), a block or file CRC mismatch
or an uncorrectable data page fail the slot, and so does `DIR_PAGE0_MISMATCH` with reason 1 (`Path`) or 2
(`Slot`), since such a page is not this copy (Open and the bootloader refuse it); reasons 3 and 4 do not (the
content is checked and may be good). Only the first reason found is reported (the finding is deduplicated per
code, entry, slot and block). A failed slot gets `EEFS_NandSlotFailed` (SLOTFLAG_CORRUPT; for the active copy of
an A/B entry `ActiveFailed` in Working, and in Base when Base holds the same slot record, with `ForceDirty`); a
good one has SLOTFLAG_CORRUPT cleared, as `EEFS_NandValidateSlot` does. A listed block already claimed gives
`BLOCK_CROSSLINKED` (`Aux0` = its map value) and is not checked against the map again; otherwise a map value
other than `OWNER(i, s)` gives `MAP_MISMATCH` (`Aux0` = map value, `Aux1` = owner). `MAP_MISMATCH` carries no
entry or slot (`0xFFFF`/`0xFF`, path ""). The page 0 re-read at a block transition is also re-validated (a page
that no longer decodes, or whose `BlockCount` changed, fails the slot with `FILE_CORRUPT Aux0 0`).
`BLOCK_NEEDS_SCRUB` uses the maximum corrected bits of the block's data reads (and page 0 for `Blocks[0]`).

Phase 3. The A/B findings carry `EntryIndex`, `Path` and: `AB_A/B_CORRUPT` and `AB_ACTIVE_CORRUPT` `Slot` = the
copy and `Block` = its first block; `AB_ACTIVE_MISSING` and `AB_FAILED_FLAG_SET` `Slot` = the active slot;
`AB_FALLBACK_UNAVAILABLE` `Slot` = the inactive slot; `AB_VERSION_MISMATCH` `Slot 0xFF`; `AB_LOAD_MIXED` (one per
volume) `Slot` = that entry's active slot, `Aux0` = the first A/B entry's active slot, `Aux1` = this one's.
`ActiveFailed` for `AB_ACTIVE_CORRUPT` was already set by the phase 2 verdict (same path as a failed
validation).

Phase 4. Page 0 is read only for blocks that no valid page 0 claimed and that are not on the pending cleanup list
(either part): a claimed block cannot be an orphan, and a listed block is a superseded first block (copy-on-write
modify, Rename, SetFileAttributes, CopySlot replacing a copy, Remove), reported by phase 1 as `DIR_CLEANUP_PENDING`
when it comes from the media and never as an orphan or a cross-link, even when its `Blocks[]` lists blocks that
are free now. `ORPHAN_FILE` carries `Slot` = the page's `Slot` (`0xFF` for a single copy), `Aux0` = its `WriteSeq`,
the page's path, no entry.

Summary bits are accumulated as findings are added, also for findings that no longer fit (`Overflow` counts those,
duplicates of a dropped finding included). `FREE_BELOW_RESERVE` carries no slot, entry or block.

Cancel returns `EEFS_SUCCESS` whether or not a check was attached. `Start` on an unmounted volume is
`EEFS_INVALID_ARGUMENT`. The driver gained `EEFS_HealthCheckCancel(MountPoint)`.

## 11. Simulator (`tests/nand/nand_sim.[ch]`)

Host-only (uses `calloc`/`free`); also compiled into the RTEMS test. Implements
`EEFS_NandOps_t`.

```c
typedef struct NandSim NandSim_t;      /* opaque; one static instance per test, or heap via NandSim_Create */

#define NANDSIM_TORN_NONE      0   /* the operation that exceeds the budget is not applied */
#define NANDSIM_TORN_PARTIAL   1   /* program: the first half of the page is applied, the rest stays 0xFF; erase: half the pages erased */
#define NANDSIM_TORN_GARBAGE   2   /* program: every byte of the page is XORed with a pattern (unreadable CRC) */

NandSim_t *NandSim_Create(const EEFS_NandGeometry_t *Geo);          /* lazily allocates a block on its first program; a successful erase releases it; erased blocks read 0xFF without storage */
void       NandSim_Destroy(NandSim_t *Sim);
void       NandSim_FillDevice(NandSim_t *Sim, EEFS_NandDevice_t *Dev); /* Geometry copy, Ops, Ctx = Sim */

/* Faults */
void   NandSim_SetFactoryBad(NandSim_t *Sim, uint32 Block);              /* marker byte 0x00 in the OOB of every BadMarkerPages page; data random */
void   NandSim_InjectRead(NandSim_t *Sim, uint32 Block, uint32 Page, int32 Status, int32 Times);
       /* Status >= 0: corrected bits reported; EEFS_NAND_READ_UNCORRECTABLE / IO_ERROR. Page 0xFFFFFFFF = any page. Times -1 = forever. */
void   NandSim_InjectProgramFail(NandSim_t *Sim, uint32 Block, uint32 Page, int32 Times);
void   NandSim_InjectEraseFail(NandSim_t *Sim, uint32 Block, int32 Times);
void   NandSim_ClearInjections(NandSim_t *Sim);

/* Power cut: after Budget more program+erase operations, every program/erase fails with -1 and sets PowerCut; the
 * Budget-th+1 operation is applied according to TornMode. Reads keep working (they do not consume budget). */
void   NandSim_SetPowerBudget(NandSim_t *Sim, uint32 Budget, int TornMode);
int    NandSim_PowerCutHappened(const NandSim_t *Sim);
void   NandSim_Reboot(NandSim_t *Sim);     /* clears PowerCut, budget (unlimited) and injections; content persists */

/* Inspection and direct manipulation for tests */
void   NandSim_CorruptBytes(NandSim_t *Sim, uint32 Block, uint32 Page, uint32 Offset, uint32 Len);  /* XOR 0x5A */
void   NandSim_RawErase(NandSim_t *Sim, uint32 Block);
void   NandSim_RawWrite(NandSim_t *Sim, uint32 Block, uint32 Page, const void *Data, const void *Oob);
int    NandSim_PageIsErased(const NandSim_t *Sim, uint32 Block, uint32 Page);
uint32 NandSim_Reads(const NandSim_t *Sim);  uint32 NandSim_Programs(const NandSim_t *Sim);  uint32 NandSim_Erases(const NandSim_t *Sim);
uint32 NandSim_BlockErases(const NandSim_t *Sim, uint32 Block);
void   NandSim_ResetCounters(NandSim_t *Sim);
size_t NandSim_AllocatedBytes(const NandSim_t *Sim);

/* Strictness (default on): programming a non-erased page, programming page p while page p-1 of the block is erased
 * (out of order), or a Block/Page out of range makes the op return -1 and increments Violations. */
void   NandSim_SetStrict(NandSim_t *Sim, int Strict);
uint32 NandSim_Violations(const NandSim_t *Sim);
```

`ReadPage` honours `Data == NULL` / `Oob == NULL`. A read of a factory-bad block with `Data !=
NULL` returns `EEFS_NAND_READ_UNCORRECTABLE` (its data is garbage), with `Data == NULL` returns 0
(raw OOB) so the marker scan works. Erased pages return 0 and all `0xFF`.

Additions made in phase 1 (see `tests/nand/nand_sim.h` for the full contract): `NandSim_Ops()`,
`NandSim_RawRead`, `NandSim_PageIsProgrammed`, the `NANDSIM_ANY` wildcard (block or page) for injections; counters count
calls including failed ones; `ResetCounters` also clears `Violations`; an OOB-only program (the bad-block marker) is
exempt from the strict page rules; `TORN_GARBAGE` applied to an erase XORs every byte of the block with `0xA5` (the
block is left neither erased nor intact); `RawErase` also clears a factory-bad mark.

Power-cut sweeps are written as: for `cut = 1 ..` `{ fresh sim from a saved image (NandSim_Clone
copies the content); SetPowerBudget(cut, mode); run the operation; if !PowerCutHappened break;
Reboot; remount; assert invariants }` with `NandSim_Clone(const NandSim_t *) -> NandSim_t *`
added to the API for that purpose, and run for `TornMode` NONE, PARTIAL and GARBAGE.

## 12. Budgets (summary)

| Item | 2048/64/64 | 4096/224/128 | 8192/448/64 |
|---|---|---|---|
| Page 0 fixed bytes | 1532 | 1532 | 1532 |
| Max `Blocks[]` / max file | 86 / 10.75 MiB | 427 / 213.5 MiB | 1110 / 555 MiB |
| ENTRY items per record page | 6 | 12 | 24 |
| Checkpoint pages, 64 files, 4096 blocks | 13 | 7 | 4 |
| Minimum `PagesPerBlock` (2 x (ckpt + 1)) | 28 | 16 | 10 |
| Volume RAM (defaults, `EEFS_MAX_FILES` 64) | ~61.7 KB (buffers sized by `EEFS_NAND_MAX_PAGE_SIZE`, not the geometry) | | |
| Two devices | ~124 KB | | |
| Bootloader RAM | caller page buffer + ~1.5 KB stack (measured, `arm-rtems7-gcc -O2`) | | |
| Mount reads | <= 16 + 4 x 64 + 128 = 400 | <= 16 + 4 x 128 + 128 = 656 | <= 16 + 4 x 64 + 128 = 400 |

## 13. Locking and concurrency

Every public function takes `EEFS_LIB_LOCK` on entry and releases on exit, as the EEPROM backend
does, including the long ones (WriteFile, CopySlot, Rename, Commit, ReclaimOrphans). The health
check holds it only inside `Step`.

As delivered (phase 6): the standalone driver's `eefs_macros.h` leaves `EEFS_LIB_LOCK` empty (single-task use)
unless `EEFS_LIB_LOCK_HOOKS` is defined, in which case the macros call `EEFS_LibLockHook()` and
`EEFS_LibUnlockHook()`, which the application provides; they must implement one library-wide recursive mutex
(on RTEMS a binary semaphore with priority inheritance). The RTEMS NAND test builds its own copy of the library
with the hooks (`eefs_rtems_locked` in `tests/rtems/CMakeLists.txt`); the `eefs` target is unchanged. The descriptor table and the single directory descriptor are
shared across volumes exactly as in the EEPROM backend (same globals, same helper functions
re-implemented in `eefs_nand_fileapi.c`).

## 14. Phased delivery

Phases are implemented sequentially by different engineers; each phase leaves the tree building
(`cmake -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure`
for both `-DEEFS_BACKEND=eeprom` and `-DEEFS_BACKEND=nand`, and the RTEMS configure/build from
`CLAUDE.md` for `eeprom`) with every registered test passing. "Done" always includes a
warning-free build with the project flags on clang and `arm-rtems7-gcc`.

Parallelism: **1** first. Then **2 -> 3** strictly sequential. **7** (bootloader) may start right
after 1, in parallel with 2 and 3, because it depends only on `eefs_nand_format.[ch]`, the CRC, the
simulator and FMT; its tests build volumes with the encoders until 3 lands, then add the
integration cases. **4** and **5** may run in parallel after 3 (4 touches
`eefs_nand_rebuild.c` and the mount step 5 branch in `eefs_nand_journal.c`; 5 touches only
`eefs_nand_fileapi.c` and the driver; agree on who edits `eefs_nand_internal.h`). **6** after 5.
**8** last.

### Phase 1: backend split, CMake, HAL, CRC, format module, simulator

Creates: `inc/eefs_eeprom_types.h`, `inc/eefs_nand_hal.h`, `inc/eefs_nand_format.h`,
`inc/eefs_nand_types.h`, `inc/eefs_nand.h`, `libraries/eefs_nand/eefs_crc32.c`,
`eefs_nand_format.c`, `eefs_nand_internal.h` (prototypes of sections 3.1-3.4, no bodies yet),
`eefs_nand_io.c`, `eefs_nand_journal.c`, `eefs_nand_rebuild.c`, `eefs_nand_fileapi.c`,
`eefs_nand_health.c` as **stubs**: every public `EEFS_Lib*` function exists, validates its pointer
arguments and returns `EEFS_UNSUPPORTED_OPTION` (or NULL / 0 for pointer and count returns);
`EEFS_LibInit` is real. `libraries/microeefs_nand/microeefs_nand.[ch]` as stubs returning
`EEFS_UNSUPPORTED_OPTION`. `tests/nand/nand_sim.[ch]` complete (section 11), `nand_test.h`,
`nand_test_main.c`, `test_layout.c`, `test_sim.c`, `tests/nand/CMakeLists.txt`.
Modifies: `inc/eefs_fileapi.h`, `inc/eefs_config.h`, `CMakeLists.txt`,
`drivers/standalone_driver/eefs_filesys.[ch]` (only the `EEFS_MAX_PATH_SIZE`/`SplitPath` change and
the new wrapper prototypes with bodies that call the Lib stubs), `tests/rtems/CMakeLists.txt`
(select source by backend; the NAND test source is a placeholder that prints PASSED).

Tests (suite `layout`): `layout_page0_offsets` (every `EEFS_NAND_P0_OFF_*`, `TI_*`, record and
entry offsets equal the FMT numbers; `EEFS_NandMaxBlocksPerFile` = 86/427/1110);
`crc32_check_value` (`"123456789"` -> `0xCBF43926`, empty -> 0, Update/Final chaining);
`putget_roundtrip`; `page0_encode_decode_each_type` (all six types, 1 and max blocks, tail is
`0xFF`, HeaderCrc excludes `[496,1528)`: flipping a Sig byte keeps it valid, flipping any other
byte breaks it; decode rejects wrong `ExpectBlock`, `SigType != 0`, path without `/`);
`record_page_seal_decode` (torn page detection: a flipped byte at offset 31 and at `PageSize-1`
both fail); `items_roundtrip` (VOLUME, MAPCHUNK with `0xFC` -> `0xFF`, ENTRY, CLEANUP; iterator
stops at `PayloadLen`; a `Len` past the payload gives `EEFS_CORRUPT_FILE`; encoders return 0 when
`Avail` is too small). Suite `sim`: `sim_erase_program_read`, `sim_lazy_alloc` (4096 blocks x 128
x 4096: after writing 3 blocks `AllocatedBytes < 4 MiB`), `sim_factory_bad`, `sim_inject_read`,
`sim_program_erase_fail`, `sim_power_budget_and_reboot` (all three torn modes), `sim_strict_nop_and_order`.

Done when: `eeprom` host build passes the 4 golden tests with no change in any generated file;
`eeprom` RTEMS QEMU test passes; `nand` host build passes `nand_layout_*` and `nand_sim_*` for all
three geometries; `nand` RTEMS build compiles and links.

As delivered: the descriptor-table helpers that only inspect the shared globals (`EEFS_LibHasOpenFiles`,
`HasOpenDir`, `FileDescriptor2Pointer`, `GetFileDescriptorsInUse`, `GetFileDescriptorsHighWaterMark`, `GetMaxFiles`,
`GetMaxOpenFiles`, `PrintOpenFiles`) already behave as in the EEPROM backend, and `EEFS_LibCreat`/`EEFS_LibWrite`
already have their final behaviour (`EEFS_UNSUPPORTED_OPTION`). The driver wrappers of 2.7 are complete (path and
mount-point resolution, `EEFS_Format` device handling); later phases only implement the `EEFS_Lib*` bodies they call,
plus the `EEFS_UnMount` commit (phase 3). `MicroEEFS_NandSetSlotHook` already stores the hook. The `layout` suite
also has `block_data_range` (FMT 3.3 ranges) and `capacity_table` (the FMT 4.6 page counts, packed with the encoders
the way 5.4 describes). `eefs_nand_test_default` is built from phase 1 on and registered in phase 3. The phase 1
`eefs_rtems_nand_test.c` checks the CRC, the simulator and a page 0 round trip on the target and expects
`EEFS_Format` to return `EEFS_UNSUPPORTED_OPTION`; phase 2 must update that expectation.

### Phase 2: journaled directory

Implements: `eefs_nand_io.c` (3.1), `eefs_nand_journal.c` (3.2, 5.1-5.6), and in
`eefs_nand_fileapi.c`: `EEFS_LibInitFS` (5.2 without step 5: no directory -> `EEFS_NOT_FORMATTED`
for now, and step 6 reconcile limited to reading page 0s, since no files can exist yet; the step is
coded but exercised in phase 3), `EEFS_LibFreeFS`, `EEFS_LibCommit`, `EEFS_LibAbort`,
`EEFS_LibFormat`, `EEFS_LibRepairDirectory`, the map/cleanup helpers `EEFS_NandFreeSlot`,
`EEFS_NandFreeBlocksExcept`, `EEFS_NandReleaseBlocks`, `EEFS_NandAddCleanup`,
`EEFS_NandCleanupHasRoom`, `EEFS_NandForgetCleanup`, `EEFS_NandTouch` (3.3, the parts that only
touch the map and the cleanup list), and the driver wrappers `EEFS_Format`, `EEFS_Commit`,
`EEFS_Abort`, `EEFS_RepairDirectory`. **Not** in phase 2: the allocator (`EEFS_NandAllocBlocks`,
`EEFS_NandFreeCount`, the reserve rule and `AllocCursor` seeding, 6.9) and everything else in 3.3,
which are phase 3; phase 2 tests set `Working.Map` bytes directly to simulate allocations. Phase 2
also does the header housekeeping that the 2026-10-15 decisions require, none of which changes
journal logic: in `inc/eefs_config.h` set `EEFS_NAND_VERIFY_ON_OPEN` to 0, add
`EEFS_NAND_RESERVE_BLOCKS` and `EEFS_NAND_BADBLOCK_RESERVE` with their compile check, remove
`EEFS_NAND_CREAT_SPARE_BLOCKS`; in `inc/eefs_nand_types.h` remove `EEFS_HEALTH_DIR_PAGE0_STALE`
(leave the comment that 31 is retired), add `EEFS_HEALTH_FREE_BELOW_RESERVE` 40 and
`EEFS_HEALTH_SPACE_ISSUE 0x80`, fix the INFO/`DIR_PAGE0_MISMATCH` comments, add `AllocCursor` to
`EEFS_InodeTable_t`; in `inc/eefs_nand.h` add `EEFS_NandVolumeInfo_t` and `EEFS_LibGetVolumeInfo`
(a stub until phase 3) and in `eefs_filesys.[ch]` the `EEFS_GetVolumeInfo` wrapper; in
`eefs_nand_internal.h` the `EEFS_NAND_ALLOC_*` constants, the new `EEFS_NandAllocBlocks` signature
(`Class` parameter), `EEFS_NandFreeCount`, `EEFS_NandFreeBlocksExcept` and `EEFS_NandBlockUnchanged`.
`test_layout.c` must keep passing (none of this is on-media). Tests manipulate `Working` directly
through `eefs_nand_internal.h` to simulate file operations (set entry fields and map bytes, write a
fake but valid page 0 with the encoders).

Tests (suite `journal`): `format_rejects_512_page`, `format_rejects_bad_geometry` (odd dir blocks,
`PagesPerBlock` too small, `BadMarkerPages = 0`), `format_scans_markers_before_erase` (factory bad
in dir and data regions: map BAD, never erased, `Erases` count = good blocks),
`format_erases_all_good_blocks` (pre-seeded junk pages are gone), `format_checkpoint_seq1_both_mirrors`
(page 0 of block 0 and of block 8 decode as CHECKPOINT Seq 1 with VOLUME + full map, no entries),
`format_fails_with_one_good_block_in_mirror`, `mount_empty_volume_reads_only` (`Programs/Erases`
unchanged), `mount_unformatted_returns_not_formatted`, `mount_geometry_mismatch`,
`commit_nothing_dirty_writes_nothing`, `commit_txn_visible_after_remount` (entries + map ranges +
cleanup; one record of the expected page count in each mirror), `commit_entry_only_in_use_in_checkpoint`,
`abort_restores_working_and_keeps_bad_marks`, `abort_erases_batch_page0s`,
`ring_rotation_300_commits` (checkpoint appears exactly when a TXN would not fit; every block in
each ring gets erased; mount after each 50 commits agrees with RAM), `mirror0_erased_mounts_from_mirror1_and_resyncs`
(after the next commit mirror 0 has a CHECKPOINT at `LastSeq` then the TXN), `mirror_lagging_one_record`
(cut between mirrors via budget; remount shows the committed state; next commit resyncs),
`torn_tail_record_ignored_and_block_abandoned` (corrupt the last record's last page; remount gives
`Seq-1`; the next commit writes a CHECKPOINT in the next ring block), `torn_entry_checkpoint_falls_back_to_previous_block`,
`dir_block_program_failure_marks_bad_and_retries` (map shows `0xFE` after the commit, ring shrinks),
`pending_cleanup_rerun_before_first_mutating_op` (cut during cleanup; remount; `PendingCleanupBase > 0`;
a Commit erases them; the "an allocation of such a block forgets it" half moves to phase 3 with the
allocator), `freeing_is_map_state_only` (free 300 blocks of several synthetic slots in one batch:
the cleanup list grows by the number of slots, the TXN carries the map ranges, and the remount
agrees),
`writeseq_gap_after_mount`, `powercut_sweep_commit` (sweep every budget value through a 3-entry
commit for all torn modes: after reboot+mount the state equals exactly pre or post; mount never
programs/erases; a following commit succeeds and both mirrors agree).

Done when: `nand_journal_*` passes for all three geometries; `eeprom` tests still pass.

As delivered: all the cases above, plus `mount_reconcile_follows_page0` (step 6 with synthetic slots: page 0 wins
for ownership and copied fields, `ForceDirty` persists the repair, a destroyed page 0 flags the slot),
`freefs_and_abort_busy_rules`, `ring_wraps_around_factory_bad_dir_blocks` (factory-bad blocks at the start and end
of a mirror; the rings wrap around them and they are never erased), and these power-cut sweeps, each run for every
cut point of the operation in all three torn modes, with the checks of the file header comment of
`tests/nand/test_journal.c` (exactly pre or post state, monotonic in the cut point, remount reads only, every slot
validates, a follow-up commit succeeds with both mirrors agreeing, cleanup done iff free): `powercut_sweep_commit`
(modify + remove with cleanup + add), `powercut_sweep_commit_multipage_txn` (the same with a TXN of at least three
pages), `powercut_sweep_commit_enters_block` (both mirrors full: erase + CHECKPOINT, with a factory-bad block
skipped by the ring), `powercut_sweep_commit_with_resync` (mirror 1 one record behind),
`powercut_sweep_pending_cleanup_then_commit`, `powercut_sweep_repair_directory`, `powercut_sweep_format` (on a
64-block device that holds a volume), and two double-fault sweeps (`powercut_double_sweep_commit`,
`powercut_double_sweep_enters_block`) that also cut the recovery commit at every point. The RTEMS NAND test formats
and mounts through the driver, then commits, unmounts and remounts through the library. `EEFS_LibGetVolumeInfo` and
the `EEFS_GetVolumeInfo` wrapper exist and return `EEFS_UNSUPPORTED_OPTION` until phase 3; mount sets
`AllocCursor = DirRegionBlocks` until the phase 3 allocator seeds it.

### Phase 3: file operations

Implements: section 6 in `eefs_nand_fileapi.c` (the allocator with the reserve rule and the
round-robin cursor, 6.9; WriteFile with copy-on-write, 6.2/6.3; Open/Close/Read/LSeek, Stat/Fstat,
OpenDir/ReadDir/CloseDir, Remove, Rename, SetFileAttributes, VerifyFile, GetFileInfo,
GetVolumeInfo, ChkDsk, `EEFS_NandWriteCopy`, `EEFS_NandValidateSlot`, `EEFS_NandBlockUnchanged`,
name parsing with suffix parsing present but A/B behaviour limited to "explicit slot works, no
fallback" until phase 5), mount reconcile (5.2 step 6) and `AllocCursor` seeding exercised, and the
driver wrappers `EEFS_WriteFile`, `EEFS_VerifyFile`, `EEFS_GetFileInfo`, `EEFS_GetVolumeInfo`,
`EEFS_UnMount` commit. The executables `eefs_nand_test_default` and `eefs_nand_test_verifyopen` are
registered (1.2).

Tests (suite `fileops`): `write_read_sizes` (0, 1, `PageSize-1`, `PageSize`, exactly `D0`,
`D0+1`, 3 blocks + 17 bytes: content, Stat size, pages programmed = expected, pages after data
erased), `write_all_types_roundtrip_via_getfileinfo`, `path_255_ok_256_rejected`, `name_rules`
(no leading `/`, second `@`, `@C`), `signature_block_present_and_excluded`,
`cow_modify_keeps_unchanged_blocks` (write a 6-block file; rewrite it with bytes changed only in
position 3: exactly two blocks are erased and programmed, the new first block and position 3's
replacement; positions 1, 2, 4, 5 keep their physical blocks and CRCs in the new page 0; the old
first block and the old position-3 block are FREEING; the cleanup list grew by exactly one; the
dir region saw no program; after Commit the old first block is erased and the two replaced blocks
are FREE), `cow_modify_byte_compare_reads_kept_blocks` (`Reads` grows by the data pages of every
kept position, proving the byte compare runs even though the CRCs matched), `cow_modify_changed_crc_skips_compare`
(a position whose CRC differs is not read), `cow_modify_grow_and_shrink` (grow: new positions
appended, all old kept where unchanged; shrink: dropped old blocks FREEING, the shortened last
position rewritten because its length differs), `cow_modify_of_corrupt_slot_rewrites_all` (page 0
of the slot destroyed: nothing kept, the slot is repaired by the rewrite), `cow_modify_no_space_no_change`
(fill the volume until `FreeBlocks == EEFS_NAND_BADBLOCK_RESERVE + 1`; a modify that needs 2 new
blocks returns `EEFS_NO_SPACE_LEFT_ON_DEVICE`; `Programs`, `Erases`, the directory and the map are
unchanged; a modify needing 1 block succeeds), `reserve_new_file_vs_modify` (with
`FreeBlocks == EEFS_NAND_RESERVE_BLOCKS + 2`: a 3-block new file fails with `NO_SPACE`, a 3-block
modify of an existing file succeeds; then with `FreeBlocks == EEFS_NAND_BADBLOCK_RESERVE`: a 1-block
modify fails), `reserve_badblock_replacement_may_use_everything` (`FreeBlocks ==
EEFS_NAND_BADBLOCK_RESERVE`, inject a program failure during a modify that was allowed with
`FreeBlocks == EEFS_NAND_BADBLOCK_RESERVE + 1`: the replacement succeeds and the file is complete),
`freeing_not_counted_as_free_until_commit` (remove a file, then a new file of that size fails with
`NO_SPACE`; after Commit it succeeds), `allocator_round_robin` (consecutive allocations advance
through the data area; a block freed and committed is not reused until the cursor wraps; after
remount the cursor starts after the highest owned block; `GetVolumeInfo` counts match the map),
`get_volume_info_counts` (free/freeing/used/bad/available/files/slots/uncommitted before and after
writes, removes and Commit), `remove_then_commit_erases_page0`, `rename_rewrites_first_block_only`
(only one new block programmed; `Blocks[1..]` unchanged; old first block erased by cleanup),
`setattr_rewrites_first_block_and_readonly_rules`, `open_write_flags_unsupported` (`O_WRONLY`,
`O_RDWR`, `O_CREAT`, `Creat`, `Write`), `read_lseek_across_blocks`, `batch_20_files_one_commit_one_record`
(dir `Programs` before commit = 0; after = pages of one record x 2 mirrors; all visible),
`reboot_before_commit_rolls_back`, `abort_frees_space_and_reusable` (Abort erases only the new
first blocks of the batch, never a kept block: the old copies read back intact),
`freed_blocks_not_reused_before_commit` (remove + write a new file of the same size: different
blocks), `uncorrectable_read_gives_io_error` (inject an uncorrectable read on a data page after a
successful Open: `Read` returns `EEFS_IO_ERROR`; `VerifyFile` returns `EEFS_IO_ERROR`),
`open_checks_page0_only_by_default` (`#if !EEFS_NAND_VERIFY_ON_OPEN`: corrupt a data page; Open
succeeds, `Reads` during Open = 1, `VerifyFile` reports `CORRUPT_FILE`) and
`open_verifies_data_when_enabled` (`#if EEFS_NAND_VERIFY_ON_OPEN`: the same corruption makes Open
return `EEFS_CORRUPT_FILE` and set `SLOTFLAG_CORRUPT`; run by `nand_verify_on_open_4096`),
`grown_bad_block_recovered` (program fail on a new data block during NEW and during a MODIFY: file
complete, `0xFE` in map, OOB marker set; in the MODIFY case the kept blocks were never erased),
`write_retries_exhausted_io_error` (NEW and MODIFY: the old copy is intact and still the slot's
content after the failure), `verify_file_detects_corruption`, `stat_readdir_full_paths`,
`split_path_driver` (`"/NAND0/cf/apps/x.so"` -> `/NAND0` + `/cf/apps/x.so`; too long rejected),
`max_file_size_rejected`, `reconcile_page0_wins_block_ownership` (write a file and Commit; rewrite
it in a batch with one block changed; cut power before Commit; remount: the directory still names
the old first block, the new first block and the replaced block are FREE, the kept blocks are
owned by the old copy per its page 0, and the old content reads back), `pending_cleanup_block_forgotten_on_alloc`
(the half moved here from phase 2).
Suite `powercut`: `powercut_sweep_write_new` (after every cut and remount the directory is
unchanged and nothing is owned by the new file), `powercut_sweep_modify` (6-block file, rewrite
with two positions changed; after every cut and remount, Open succeeds and reads **either exactly
the old or exactly the new content**, never `EEFS_CORRUPT_FILE`, never a mix; which one is "old"
iff the cut came before the commit record completed; if "new", the pending cleanup is finished by
the next commit and the old first block is erased), `powercut_sweep_remove_rename_setattr`,
`powercut_sweep_batch_and_cleanup` (5 files written, 2 removed, 1 renamed, 1 modified, then
Commit; after each cut the directory is exactly pre or post; if post, pending cleanup is finished
by the next commit).

Done when: `nand_fileops_*`, `nand_powercut_*`, `nand_default_config_4096`,
`nand_verify_on_open_4096` pass; `eeprom` unchanged.

As delivered: all the cases above, plus `cow_modify_rewrites_decayed_block` (a kept candidate whose
bytes decayed on the media while its CRC still matches the intended content is rewritten: the byte
compare is what catches it), `micro_matches_library_state` in the `micro` suite (the bootloader
library loads, byte for byte and with the library's `GetFileInfo` view, every file written by
`EEFS_LibWriteFile` and committed, after a copy-on-write modify, a rename, a remove, an attribute
change, grow/shrink, a re-add, ring-block changes and a remount; an uncommitted rewrite stays
invisible to it), and these `powercut` sweeps, each over every program and erase of the operation and
its commit, in all three torn modes: `write_new`, `modify` (6 blocks, two positions changed),
`remove`, `rename`, `setattr`, `batch` (5 new files, 2 removes, 1 rename, 1 modify, one commit) and
`grown_bad_program` / `grown_bad_erase` / `grown_bad_new` (a destination block fails during the write,
is marked bad and replaced). After each cut the sweep remounts (no program or erase), requires
exactly the pre or the post directory (monotonic in the cut point), reads every file of that state
back byte for byte, checks that no metadata page on the device lists a position >= 1 with bad data,
then does a follow-up write and commit and checks the cleanup, both mirrors and the files again. The
NAND test programs are compiled with `-O2` when no `CMAKE_BUILD_TYPE` is set (the sweeps run about
ten thousand mounts per geometry). Implementation notes for later phases:

- The write job keeps its block lists in static scratch arrays sized by
  `EEFS_NAND_MAX_FILE_BLOCKS` = `(EEFS_NAND_MAX_PAGE_SIZE - 1532) / 6` (about 4.3 KB at the defaults,
  with two page-0 structures), shared by all volumes under `EEFS_LIB_LOCK`, which must therefore be
  one library-wide lock (it already guards the shared descriptor table).
- `EEFS_NandWriteCopy` is implemented for phase 5 (whole-slot copy with per-block source CRC checks;
  it assigns `NewP0->WriteSeq` itself; `DstBlocks[]` must already be allocated for the destination).
  Rename and SetFileAttributes use the same write job with every position except 0 kept.
- `EEFS_NandValidateSlot` failures set `SLOTFLAG_CORRUPT` and, for the active copy of an A/B entry,
  `ActiveFailed` in `Working` (and in `Base` when `Base` holds the same slot record) with `ForceDirty`.
  Open does not fall back yet (phase 5).
- A new entry written to slot B only gets `ActiveSlot = B`.
- The allocator only takes blocks that are FREE in both `Working` and `Base`, so a block the committed
  directory references is never handed out even if the map were inconsistent.
- Abort also erases the first blocks the batch created and then superseded itself (they are on the
  batch's cleanup list and FREE in `Base`), so a batch that rewrote a file twice leaves no orphan.
- `EEFS_Crc32Update2` (CRC module) feeds the same bytes into a block CRC and a file CRC in one pass;
  WriteFile and ValidateSlot use it.
- The byte compare of a kept candidate reads the old block's data pages (`EEFS_NandBlockUnchanged`
  semantics); WriteFile matches length and CRC first and does not recompute the CRC for the compare.
- After a commit, the newest record still lists the blocks its cleanup erased; a remount reloads that
  list and the next mutating call re-runs it, which is a no-op for erased blocks (FMT 4.3.4).

### Phase 4: rebuild

Implements: `eefs_nand_rebuild.c` (8.1, 8.3), mount step 5, `EEFS_LibRebuildDirectory`, driver
`EEFS_RebuildDirectory`.

Tests (suite `rebuild`): `rebuild_after_both_mirrors_erased_matches_committed_set` (10 files,
some removed and cleaned, then raw-erase the dir region; mount returns `SUCCESS` with
`DirectoryRebuilt`; file set, sizes, CRCs match; reads work), `rebuild_highest_writeseq_wins`
(two page 0s for the same path: an old orphan with a lower seq loses), `rebuild_overlap_rule_after_cut_rename` (rename committed, cleanup cut, both mirrors lost: the old-name page 0 shares `Blocks[1..]` and loses to the higher `WriteSeq`; only the new name is rebuilt), `rebuild_includes_uncommitted_and_uncleaned_copies`
(documented caveat asserted explicitly), `rebuild_not_formatted_when_no_page0`,
`rebuild_bad_blocks_from_oob`, `rebuild_crosslink_flagged`, `repair_directory_persists_and_clears_flag`
(both mirrors get a CHECKPOINT with `Seq > MaxSeqSeen`; remount is normal),
`rebuild_directory_api_requires_idle`, `rebuild_ab_defaults` (A active if present else B;
`FallbackCompatible = 0`; `IsAB` from explicit slots).

Done when: `nand_rebuild_*` passes.

As delivered: all the cases above, plus `rebuild_max_files_overflow` (more paths on the media than
`EEFS_MAX_FILES`), `rebuild_directory_api_requires_idle` also covering the pending cleanup run first and
`EEFS_NOT_FORMATTED` leaving the volume mounted, `repair_directory_persists_and_clears_flag` also covering a
plain Commit persisting a rebuilt state, and these sweeps, each over every program and erase of the operation
and its commit in all three torn modes, on a device of `DirRegionBlocks + 112` blocks: `rebuild_sweep_cut_rename`,
`rebuild_sweep_cut_modify` (the first block and two others change), `rebuild_sweep_cut_new_write` and
`rebuild_sweep_batch_then_rebuild` (the `powercut` batch). After each cut the directory region is erased and the
mount must rebuild without writing; the state must be exactly the state before or after the operation
(non-decreasing in the cut point; for the batch, each name holds one of the versions the batch can leave and the
renamed file appears under exactly one name), every slot must pass `EEFS_LibVerifyFile`, `NextWriteSeq` must be
above every metadata page, and `EEFS_LibRepairDirectory` followed by a remount must give the same state from the
checkpoint with both mirrors in agreement. `rebuild_sweep_repair_directory` cuts `RepairDirectory` itself on a
rebuilt volume. The `micro` suite gained `micro_scan_rejects_torn_first_block` (the bootloader scan's first-block
CRC check), and `micro_scan_fallback_when_no_directory` damages a second data block instead of the first so that its
"no fallback" case still applies. Mount step 5 is in `EEFS_NandMount`; mount steps 6-8 became
`EEFS_NandMountFinish` (declared in `eefs_nand_internal.h`). The journal case `powercut_sweep_format` now expects a
rebuilt (empty) directory between the old volume and `NOT_FORMATTED`.

### Phase 5: A/B

Implements: section 7 (setters, CopySlot(+All), Open fallback, `ActiveFailed` propagation in
Open/Verify/reconcile), Remove of a single slot (6.5), driver wrappers `EEFS_SetActiveSlot(+All)`,
`EEFS_SetFallbackCompatible(+All)`, `EEFS_SetFailed`, `EEFS_ClearFailed`, `EEFS_CopySlot(+All)`.

Tests (suite `ab`): `ab_upload_activate_compatible_one_commit` (3 files to `@B` v2 with A v1
present; `SetActiveSlotAll(B)`; `SetFallbackCompatibleAll(1)`; one Commit = one record per mirror;
GetFileInfo shows both slots, B active; Open by name yields B's content), `ab_explicit_suffix_open_and_stat`,
`ab_promote_copyslot_b_to_a` (A's content equals old B, A's Version = B's, old A page 0 erased
after commit, A active after `SetActiveSlotAll(A)`), `ab_rollback_setactive_all_a`,
`ab_powercut_sweep_upload_activate_promote` (after every cut and remount: the directory is exactly
one of the step states and `MicroEEFS`-style validation of the active slots succeeds, using
`VerifyFile`), `ab_active_corrupt_compatible_falls_back` (B active and corrupted; Open by name
returns a descriptor on A with `Fallback = 1` and A's content; `ActiveFailed` set in RAM and
persisted by the next commit), `ab_active_corrupt_not_compatible_fails` (`EEFS_CORRUPT_FILE`, flag set),
`ab_explicit_name_never_falls_back`, `ab_setfailed_persists_and_clears` (remount keeps it; rewrite
of the active slot clears; CopySlot into active clears; SetActiveSlot clears),
`ab_remove_slot_rules`, `ab_copyslot_all_stops_at_first_error`, `ab_single_copy_opt_in_via_copyslot`,
`ab_modify_inactive_slot_leaves_active_untouched` (copy-on-write rewrite of B keeps B's unchanged
blocks and never touches any block of A; A's content and page 0 are byte-identical before and
after), `ab_modify_active_slot_is_safe` (rewrite the active slot in a batch, cut power before
Commit: the old active copy is intact and loads), `ab_rebuild_recovers_locations_versions`.

Done when: `nand_ab_*` and `nand_verify_on_open_ab_4096` pass.

As delivered: the setters, `CopySlot(+All)` and the Open fallback in `eefs_nand_fileapi.c` (the driver
wrappers already existed); `ActiveFailed` propagation from Open, VerifyFile and reconcile was already in
place from phases 2 and 3 and is now exercised. Suite `ab` (`tests/nand/test_ab.c`, all three geometries,
and `nand_verify_on_open_ab_4096`) has the cases above except that `ab_powercut_sweep_upload_activate_promote`
is three sweeps (`ab_powercut_sweep_upload_activate`, `..._promote_copyslot_all`, `..._rollback`), plus
`ab_workflow_upload_activate_promote_rollback` (the whole workflow with the directory checked after each step),
`ab_copyslot_reserve_and_own_blocks` (ALLOC_NEW floor with no change on `NO_SPACE`, while a rewrite of the same
slot still fits; A and B disjoint and each owned in the map), `ab_copyslot_open_rules` and
`ab_bootloader_agrees_with_library` (library Open and `MicroEEFS_NandLoadFile` agree on return code, slot,
content and `ActiveFailed` for good, corrupt-page-0, page-0 ECC, data-CRC and data ECC faults of the active copy,
with and without `FallbackCompatible`, a committed `SetFailed`, both copies bad, explicit names and a B-only
file; the data faults only with `EEFS_NAND_VERIFY_ON_OPEN`, since by default Open checks page 0 only and
VerifyFile is what detects them). The sweeps cut every program and erase of the upload + `SetActiveSlotAll(B)` +
`SetFallbackCompatibleAll(1)` + Commit, of `CopySlotAll(B, A)` + `SetActiveSlotAll(A)` + Commit, and of the
rollback commit, in all three torn modes; after each cut the directory is exactly pre or post, every copy reads
back exactly under its `@A`/`@B` name, and every file boots its active load through `microeefs_nand` with no
fallback. `ab_rebuild_recovers_locations_versions` checks the bootloader scan now and the library rebuild once
phase 4 is merged (it detects the phase 4 stub of `EEFS_LibRebuildDirectory`; after the merge it is strict).
Mutations run and reverted: Open ignoring `FallbackCompatible`, CopySlot freeing its new blocks (`FreeSlot`),
CopySlot sharing the source's data blocks, CopySlot with `ALLOC_MODIFY`, and SetActiveSlot keeping
`ActiveFailed`; each made at least one `ab` case fail. Notes for phase 6:

- `ActiveFailed` is set by a failed validation of the active copy (Open, including when it then falls back;
  VerifyFile; mount reconcile; a CopySlot or rewrite whose source page 0 or CRC fails) and by `SetFailed`. The
  first kind is recorded in `Base` too (when `Base` holds the same slot record) with `ForceDirty`, so Abort keeps
  it and the next commit persists it. It is cleared by `ClearFailed`, `SetActiveSlot(+All)` (even to the same
  slot), a rewrite of the active copy and a `CopySlot` into the active slot. A rewrite or copy into the inactive
  slot does not clear it. The health check's `AB_ACTIVE_CORRUPT` finding (`health_ab_active_corrupt_sets_flag`)
  should set it through the same path (`EEFS_NandValidateSlot`).
- CopySlot never shares blocks: after `CopySlot(B, A)` the old A copy's data blocks are FREEING (FREE after the
  commit) and its first block is on the cleanup list, as for Remove. Until that cleanup erase runs, the old
  page 0 lists blocks that may already be free or reallocated; like a superseded copy-on-write page 0 it must be
  reported as `DIR_CLEANUP_PENDING` only, not as an orphan or cross-link.
- After an upload and activation the two slots normally carry different `Version`s (B being tested); after a
  promote they are equal. `FallbackCompatible` is set only by the setters (rebuild resets it to 0).
- A descriptor opened by the fallback has `Fallback = 1` and `Slot` = the inactive slot.
- `test_ab.c` has static helpers for A/B fixtures and faults (`A_MakeF`, `A_CorruptPage0`, `A_CorruptData`,
  `A_InjectEcc`, `A_CheckBoot`) that the health tests can copy.

### Phase 6: health check and ReclaimOrphans

Implements: `eefs_nand_health.c` (section 10), `EEFS_LibReclaimOrphans` (8.4), driver wrappers
`EEFS_HealthCheckStart/Step/Summary`, `EEFS_ReclaimOrphans`.

Tests (suite `health`): `health_clean_volume_ok` (Summary `OK`, counters plausible, `PagesRead`
equals the computed expectation), one case per finding code, each building the fault deliberately
and asserting code, `EntryIndex`, `Path`, `Slot`, `Block` (and `Aux` where defined):
`health_dir_mirror_missing`, `health_dir_mirror_mismatch`, `health_dir_record_corrupt`,
`health_dir_uncommitted_txn`, `health_dir_cleanup_pending`, `health_dir_rebuilt_not_persisted`,
`health_file_corrupt_page0`, `health_file_corrupt_block_crc`, `health_file_corrupt_file_crc`,
`health_file_io_error`, `health_block_needs_scrub`, `health_ab_a_corrupt`, `health_ab_b_corrupt`,
`health_ab_active_corrupt_sets_flag`, `health_ab_active_missing`, `health_ab_version_mismatch`,
`health_ab_fallback_unavailable`, `health_ab_failed_flag_set`, `health_ab_load_mixed`,
`health_dir_page0_mismatch` (one sub-case per `Aux0` reason 1-4), `health_map_mismatch`,
`health_block_crosslinked`, `health_badblock_unrecorded`, `health_orphan_file`,
`health_free_below_reserve` (informational at `BADBLOCK_RESERVE <= free < RESERVE_BLOCKS`,
degraded below `BADBLOCK_RESERVE`, absent at `free == RESERVE_BLOCKS`),
`health_cow_superseded_page0_not_orphan_or_crosslink` (modify + Commit with the cleanup erase cut:
the old first block yields `DIR_CLEANUP_PENDING` only; after `RunPendingCleanup` the report is
clean); plus
`health_maxpagereads_1_equals_one_big_step` (identical finding lists and counters except `Steps`),
`health_restart_on_commit_during_check` (`Restarts == 1`, report correct), `health_open_batch_no_false_orphans`,
`health_powercut_sweep_never_crashes_and_matches_expectation` (after each cut of the batch sweep,
run the check to completion and compare to the expected set derived from pre/post),
`reclaim_orphans_erases_only_unreferenced`.

Done when: `nand_health_*` passes.

As delivered: `eefs_nand_health.c` (section 10, with the details of 10.4), `EEFS_LibReclaimOrphans` in
`eefs_nand_rebuild.c` (8.4), the driver wrapper `EEFS_HealthCheckCancel`, the `EEFS_LIB_LOCK_HOOKS` option of the
standalone `eefs_macros.h` (section 13), the change counter bumped by every program and erase (section 4), and
`EEFS_NandSlotFailed` exported for the health check. Suite `health` (`tests/nand/test_health.c`, all three
geometries; the finding cases run on a device of `DirRegionBlocks + 112` blocks, the clean case also on the
full-size one) has the cases above, with these differences: `health_ab_b_corrupt` and
`health_ab_active_corrupt_sets_flag` are one case, `health_ab_b_corrupt_and_active_corrupt_sets_flag`, run for a
corrupt page 0, a data CRC and an uncorrectable data page of the active copy B (the flag is set by the check also
in the default build where Open checks page 0 only, survives Abort, is persisted by the commit, is reported as
`AB_FAILED_FLAG_SET` by the next check, and the bootloader falls back to A); the `DIR_UNCOMMITTED_TXN` case covers
a real commit cut after the first page of a multi-page TXN, a page with a readable header but a broken CRC, a
two-page record whose second page is missing and a torn checkpoint above the chain;
`health_restart_on_commit_during_check` is `health_restart_on_change_during_check` (restarts after an uncommitted
WriteFile, a commit, and a rewrite + commit of the file being checked; an empty commit does not restart it; the
check setting `ActiveFailed` does not restart it; the final report is exact); and there are
`health_cow_superseded_page0_not_orphan_or_crosslink` (modify + rename + CopySlot replacing a copy, also checked
while the batch is open), `health_cancel_and_summary`, `health_findings_overflow` (40 orphans: 32 findings,
`Overflow` 8), `reclaim_orphans_erases_only_unreferenced` (per-block erase counts; a FREEING page 0 missing from
the cleanup list and a listed free block are each protected on their own) and
`reclaim_orphans_powercut_sweep_no_damage`. Every check run verifies the protocol: each step reads at most
`MaxPageReads` pages, `PagesRead` equals the HAL reads, nothing is programmed or erased, the volume's summary equals
the report, and the summary bits follow from the codes. The power-cut sweep runs the `powercut` batch on every
program and erase in the three torn modes and compares the report after the remount with expectations derived
from the raw media: an orphan for every unclaimed, unlisted valid metadata page (always a block of the batch),
`DIR_CLEANUP_PENDING` for the listed blocks from the media that still hold a page 0, `BADBLOCK_UNRECORDED` for
the marker bytes a GARBAGE-torn erase leaves in the OOB, `DIR_MIRROR_MISMATCH` when the mirrors' chains end at
different `Seq`, a torn-tail finding for exactly the mirrors whose scan is `Torn`, and nothing else; then
ReclaimOrphans erases exactly the orphans, and after a follow-up commit only the torn-erase markers remain. The
RTEMS NAND test runs the check in a priority-200 task with `MaxPageReads` 4 and a one-tick sleep between steps
while a priority-50 task writes and commits three files; a HAL wrapper releases the writer from inside the health
task's page reads, so every write waits for the lock once (priority inheritance), the check restarts three times
and ends with the expected two findings for a decayed inactive copy.

Mutations run and reverted (each failed at least one `health` case): pending-cleanup blocks reported as
orphans; only the media part of the cleanup list excluded from the orphan scan; no restart on a volume change;
a failed active copy not setting `ActiveFailed`; the check's own flagging restarting it; page reads not counted
against `MaxPageReads`; ReclaimOrphans erasing FREEING blocks, ignoring the cleanup list, or erasing any free
block.

Notes for phase 8:

- The health check runs for real only with a lock: an application with several tasks must define
  `EEFS_LIB_LOCK_HOOKS` (or its own `eefs_macros.h`); `tests/rtems/eefs_rtems_nand_test.c` shows the hooks and the
  RTEMS configuration (3 tasks, 3 semaphores, 1 ms tick).
- The report memory (`EEFS_HealthReport_t`, ~3.5 KB) is the caller's and must stay untouched while attached.
- After a power cut in the GARBAGE torn mode of the simulator, an interrupted erase leaves non-0xFF bytes in the
  OOB marker position, which the check reports as `BADBLOCK_UNRECORDED` until the block is erased again.
- `PagesRead` of a clean volume is computed exactly by `H_ExpectedReads` in `test_health.c` (section 10.4 rules).

### Phase 7: bootloader library

Implements: `libraries/microeefs_nand/microeefs_nand.[ch]` (section 9), CMake `microeefs` for the
NAND backend (sources + include dir + install), `tests/nand/test_micro.c`.

Tests (suite `micro`): `micro_find_via_journal` (checkpoint + several TXNs incl. a rename away and
back, remove, re-add at another index: `Info` matches the library's GetFileInfo),
`micro_load_via_journal_all_types` (loads into a buffer; content and `Type` fields match),
`micro_rejects_corrupt_page0_and_data` (`CORRUPT_FILE`), `micro_io_error`,
`micro_scan_fallback_when_no_directory` (`UsedScan = 1`, highest `WriteSeq` wins),
`micro_no_scan_when_directory_valid_but_page0_bad` (returns `CORRUPT_FILE`, `UsedScan = 0`, even
though an older valid copy exists on the device), `micro_ab_active_and_explicit_slot`,
`micro_ab_fallback_compatible` (`SUCCESS`, `LoadedSlot` = inactive, `ActiveFailed = 1`),
`micro_ab_no_fallback_when_incompatible` (`CORRUPT_FILE`, `ActiveFailed = 1`),
`micro_hook_override_honoured_and_compatible_rule_applies`, `micro_torn_txn_ignored`,
`micro_picks_longest_mirror_chain`, `micro_maxsize_too_small`, `micro_geometry_mismatch`.
Until phase 3 lands, the fixtures are built with the encoders + simulator raw writes; afterwards
the cases are re-pointed at volumes produced by the library (`micro_matches_library_state`).

Done when: `nand_micro_*` passes; `microeefs` for NAND builds for `arm-rtems7` with no warnings.

### Phase 8: RTEMS QEMU test and docs

Implements: `tests/rtems/eefs_rtems_nand_test.c` (simulator in RAM at 4096/224/128 with 256
blocks; Format, mount, write 5 files of different types in one batch + Commit, read back, Stat,
GetFileInfo, A/B upload/activate/commit, `MicroEEFS_NandLoadFile` of a BOOT_ELF typed file,
health check to completion with `OK`; prints `EEFS RTEMS TEST PASSED`), `tests/rtems/CMakeLists.txt`
registration for the NAND backend (`eefs_rtems_qemu` runs whichever backend is configured), README
and CLAUDE.md sections for the NAND backend (build lines for both backends, test matrix, the format
doc pointer), final pass over `docs/nand-format.md` for anything the implementation changed.

Done when: `cmake -B build-rtems-nand -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/rtems-toolchain.cmake
-DRTEMS_PREFIX=$HOME/rtems/7 -DRTEMS_VERSION=7 -DRTEMS_ARCH=arm -DRTEMS_BSP=xilinx_zynq_a9_qemu
-DEEFS_BACKEND=nand && cmake --build build-rtems-nand && ctest --test-dir build-rtems-nand
--output-on-failure` passes, and the same for the default backend, and the full host matrix passes
for both backends.

## 15. Deviations from the approved plan

1. **Journal entry kinds.** The plan's `ADD/REMOVE/RENAME/RELOCATE/SETATTR/BADBLOCK/WRITESLOT/
   SETACTIVE/SETFALLBACK/SETFAILED/CLEARFAILED` entries are realized as full ENTRY snapshots plus
   MAPCHUNK ranges (FMT 4.3). Replay is "overwrite entry i / copy map bytes", needs no page-0 reads
   (the plan's ADD/RELOCATE did) and cannot depend on page-0 contents. Bad blocks are map bytes.
   Equivalent semantics, simpler and more robust replay, and the bootloader can track one entry
   by index.
2. **No `EEFS_NAND_CHECKPOINT_INTERVAL`.** A CHECKPOINT is written exactly when a mirror enters
   a ring block (first record of every block). Each block therefore holds a complete state;
   compaction and "erase blocks older than the previous checkpoint" fall out of the ring. Replay
   cost is bounded by one block per mirror.
3. **Mount takes the mirror with the longest valid chain** instead of merging records by `Seq`
   across mirrors. Because every block contains checkpoint + TXNs, the longer chain is a superset
   state; merging would never yield more.
4. **Torn tail handling.** Instead of "the next record starts at the first fully erased page",
   a mirror whose current block has a non-erased page after its chain (or a torn entry block)
   writes its next record into a freshly erased block. This avoids programming after a page that
   may have been partially programmed (a NOP=1 hazard on many parts) at the cost of one erase.
5. **`EEFS_NAND_MAX_TXN_ENTRIES` replaced by `EEFS_NAND_MAX_CLEANUP_BLOCKS`.** With snapshot
   records the only per-batch list that can grow without bound is the cleanup list (one block per
   freed slot, repeated rewrites of the same file free several). `EEFS_TRANSACTION_ERROR` is
   returned, before any media change, when an operation would overflow it.
6. **Bootloader scan fallback only when no valid directory exists** (never when the directory is
   valid but a page 0 is bad), because a scan could load a removed, superseded or incompatible
   copy. In that case the A/B compatible-fallback rule applies instead.
7. **Health check restarts from phase 1** on any volume change, clearing findings, rather than
   restarting "the affected phase"; `VolumeChangeCounter` increments on every mutating call, not
   only commits. Simpler and always correct.
8. **Added:** `EEFS_GEOMETRY_MISMATCH` (-15); finding `FREE_BELOW_RESERVE` (40) and summary bit
   `SPACE_ISSUE`; config macros `EEFS_NAND_VERIFY_ON_OPEN`, `EEFS_NAND_RESERVE_BLOCKS`,
   `EEFS_NAND_BADBLOCK_RESERVE`, `EEFS_NAND_WRITESEQ_MOUNT_GAP`, `EEFS_NAND_MAX_DIR_BLOCKS`,
   `EEFS_NAND_LAYOUT_VERSION`, `EEFS_NAND_HEALTH_PATH_SIZE`, `EEFS_NAND_WRITE_RETRIES`; API
   `EEFS_LibHealthCheckCancel`, `EEFS_LibSetFallbackCompatibleAll`, `EEFS_LibRebuildDirectory` as a
   public call, `EEFS_LibGetVolumeInfo`. Finding 31 (`DIR_PAGE0_STALE`) existed in the first
   revision of this spec and in the phase 1 header and is retired (see 24).
9. **Open checks page 0 only by default** (`EEFS_NAND_VERIFY_ON_OPEN = 0`, user decision
   2026-10-15). The first revision defaulted to a full CRC check at open to catch an interrupted
   in-place write; with copy-on-write (24) a live copy is never partially rewritten, so the data
   CRCs are left to the bootloader, `VerifyFile` and the health check. The option stays and is
   built and tested with it on (`nand_verify_on_open_4096`).
10. **Write order: new blocks 1..n-1 first, the new first block (with page 0) last**, so an
    interrupted write leaves no page 0 (no orphan) unless the data of every other block is complete. The
    first block's own data pages follow its page 0 (ascending page order), so a rebuild checks
    `Blocks[0].Crc32` of a candidate (8.1).
11. **Page-0 `Slot` is A / B / NONE (0xFF)** and the directory's `IsAB` is set when a slot was
    ever written explicitly; rebuild sets `ActiveSlot = A if present else B` (the plan said
    always A, which would make a B-only file unopenable after a rebuild).
12. **The directory entry caches `FileSize`, `FileCrc32`, `ModificationDate` per slot**, so Stat
    and ReadDir need no NAND reads. With copy-on-write they are never legitimately stale in a
    committed state; a difference is reported as `DIR_PAGE0_MISMATCH` and repaired by reconcile.
13. **Checkpoints carry only in-use entries**; absence means not in use.
14. **Abort erases the first block of every slot the batch created** (best effort) so aborted
    batches leave no orphans for a rebuild to pick up; the plan left them to be erased on reuse.
15. **`EEFS_Format(DeviceName, BaseAddress)`** takes the device pointer and registers/initializes
    the device, because `EEFS_InitFS` cannot succeed on an unformatted device and the standalone
    driver has no "remove device" call.
16. **Rename/SetAttr rewrite only the first block** (user decision, 2026-10-15): new page 0 plus
    a copy of block 0's data pages into one new block; the other blocks stay. CopySlot still copies
    the whole slot (the other slot needs its own blocks). Copies go page by page through the HAL
    (`ReadPage`/`ProgramPage`); a BSP may exploit its controller buffer inside `ProgramPage`.
17. **`EEFS_LibWriteFile` has no separate `Type` parameter**; the type is `Meta->FileType`.
18. **`EEFS_LibReclaimOrphans` scans the device itself** rather than consuming a health report
    (which could be stale) and never touches owned or FREEING blocks.
19. **RAM budget** is ~62 KB per volume (two copies of the entry table for Abort, two page buffers,
    the cached health summary), not the ~15 KB the plan estimated. Accepted by the user on
    2026-10-15.
20. **The BSP slot hook is a function pointer set at run time** (`MicroEEFS_NandSetSlotHook`), not a
    link-time override, since C89 has no portable weak symbols.
21. **`EEFS_SetFallbackCompatible` "name or all"** is two functions (`...` and `...All`), and the
    `All` variants return counts.
22. **Mount reconcile repairs disagreeing slot snapshots** (`ForceDirty`) from page 0 and sets
    `ActiveFailed` for an invalid active copy, so the first commit after a crash repairs the
    directory; the plan only mentioned reporting `EEFS_CORRUPT_FILE`.

Decisions of 2026-10-15 (user), superseding earlier text of this spec and of the plan:

23. **Reserved spare pool and round-robin allocation** (6.9). `EEFS_NAND_RESERVE_BLOCKS` (16) free
    blocks are held back from new-file and CopySlot allocations; rewrites of existing files
    (copy-on-write modify, Rename, SetAttr) may use them down to `EEFS_NAND_BADBLOCK_RESERVE` (4);
    grown-bad-block replacement may take the last block. The allocator is round-robin from a RAM
    cursor rather than first-fit, so erase cycles spread over all free blocks. The cursor is not
    persisted (a reboot only moves the starting point; persisting it would need a format change).
    `EEFS_LibGetVolumeInfo` and the `FREE_BELOW_RESERVE` finding report the levels.
24. **Copy-on-write replaces in-place modify entirely** (6.2, 6.3). The plan said "modifying a file
    does not update the directory" and the first revision of this spec implemented that as an
    in-place erase-and-rewrite of the slot's blocks, which left a torn file (`EEFS_CORRUPT_FILE`)
    on power loss and destroyed the old data on a write failure. Now every rewrite writes a new
    first block plus fresh blocks for the changed data, keeps unchanged blocks (same length, CRC
    match, and a byte compare against the old block, because a CRC match alone is probabilistic),
    and changes `FirstBlock`, which **is a directory change** staged in the batch and made live by
    `EEFS_Commit`. The user accepted this consequence. Power loss before the commit leaves the old
    copy; there is no fallback to an in-place write, and insufficient space (after the reserve
    rule) is `EEFS_NO_SPACE_LEFT_ON_DEVICE` with no change. The A/B inactive-slot upload is the
    same path. Removed with it: `EEFS_NAND_CREAT_SPARE_BLOCKS`, finding `DIR_PAGE0_STALE`, the
    in-place tests and the `EEFS_CORRUPT_FILE`-after-cut expectation.
25. **No per-block "next block" header.** The user floated a next-block pointer in the first page
    of each block. It was not adopted: page 0's `Blocks[]` already lets a rewrite touch only the
    changed blocks plus the first block, whereas a pointer in each block would have to be rewritten
    in every block that precedes a changed one (the rewrite cascades back to the start), and data
    blocks would lose a page each. The format keeps `Blocks[]` and header-less data blocks (FMT 3.3).
26. **`EEFS_NAND_VERIFY_ON_OPEN` defaults to 0** (see 9) and the **RAM budget is accepted** (see 19).

## 16. Open risks and questions

- **Lock duration.** WriteFile, CopySlot, Rename, SetAttr and ReclaimOrphans hold the single
  library lock for their whole duration (hundreds of ms to seconds for large files), blocking all
  readers on every volume. Acceptable for an infrequently written boot volume; otherwise a future
  change could release the lock between blocks (the batch model already tolerates it).
- **Whole file in RAM.** `EEFS_LibWriteFile` takes the complete file in memory (copy-on-write
  compares the new data against the old blocks, so it also needs the whole new image at once). A
  callback or chunked variant can be added later without format change.
- **Reserve sizing.** `EEFS_NAND_RESERVE_BLOCKS` = 16 guarantees a rewrite only if at most 15 data
  blocks (7.5 MB at the target geometry) change while the volume is otherwise full of new files.
  Deployments that rewrite large images on a nearly full volume should raise it; `GetVolumeInfo`
  and `FREE_BELOW_RESERVE` make the margin visible.
- **NAND assumptions:** pages programmed once between erases, in ascending order; an erased page
  reads `0xFF` with status >= 0; the OOB byte at `BadMarkerOffset` is programmable independently of
  the controller's ECC bytes. Controllers whose ECC covers the OOB marker byte may reject the
  marker write (it is best effort; the map still records the block as bad).
- **`EEFS_UnMount` commits implicitly** (plan). An application that wanted to drop a batch must
  call `EEFS_Abort` first.
- **Rebuild caveats** (FMT 6) are inherent; `WRITESEQ_MOUNT_GAP` and Abort-time erasure reduce
  but do not eliminate them. With copy-on-write an uncommitted rewrite's page 0 shares blocks with
  the committed copy; the overlap rule (8.1) makes the higher `WriteSeq` win, which after a crash
  before commit is the *uncommitted* copy when both are complete (the same documented caveat as
  for a new file in an uncommitted batch).
- **Signature scheme** left for the future; version 1 readers reject `SigType != 0`.
