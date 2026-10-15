# EEFS NAND on-media format (layout version 1)

This document is the normative description of what the EEFS NAND backend writes to a raw NAND
device. It is written for bootloader authors and for anyone who has to read or repair a volume
without the library. `docs/nand-implementation.md` describes the library that produces it.

Everything here is **little-endian, field by field**. There are no C structs on the media; the
library serializes with explicit put/get helpers (`libraries/eefs_nand/eefs_nand_format.c`), so the
layout does not depend on the compiler, the ABI or the host endianness.

## 1. Terms and conventions

| Term | Meaning |
|---|---|
| Page | The NAND program unit. `PageSize` data bytes plus `OobSize` out-of-band bytes. |
| Block | The NAND erase unit, `PagesPerBlock` pages. `BlockBytes = PageSize * PagesPerBlock`. |
| Erased | Every data byte of the page reads `0xFF`. |
| Record | One directory log entry (a CHECKPOINT or a TXN), made of 1..n consecutive pages. |
| Item | A typed, length-prefixed element inside a record's payload. |
| Slot | One copy of a file: A (0) or B (1). Single-copy files use slot A. |
| Entry | One file-table row, indexed 0..`MaxFiles-1`. |
| CRC-32 | IEEE 802.3 reflected CRC, polynomial `0xEDB88320`, init `0xFFFFFFFF`, final XOR `0xFFFFFFFF`. `CRC32("123456789") = 0xCBF43926`. The CRC of zero bytes is `0x00000000`. |

Integer fields are unsigned little-endian unless noted. Strings are NUL-terminated and the bytes
after the NUL are `0x00`. Fields named *Reserved* are written as `0x00`. Regions described as
*unused* are written as `0xFF` (left erased).

### 1.1 Geometry

The format supports:

| Parameter | Constraint |
|---|---|
| `PageSize` | 2048, 4096 or 8192 (a power of two, >= 2048). 512-byte pages are **rejected**. |
| `OobSize` | >= 1, <= the build's `EEFS_NAND_MAX_OOB_SIZE`. |
| `PagesPerBlock` | >= 2 * (max checkpoint pages + 1), see 4.6. 64 and 128 are the tested values. |
| `BlockCount` | >= `DirRegionBlocks + 1`, <= the build's `EEFS_NAND_MAX_BLOCKS` (4096). Blocks are numbered `0..BlockCount-1` across all chips; chip select is the HAL's business. |
| `DirRegionBlocks` | Even, >= 4, <= the build's `EEFS_NAND_MAX_DIR_BLOCKS` (32). Default 16. |
| `BadMarkerOffset` | OOB byte index of the bad-block marker. Default 0. |
| `BadMarkerPages` | Bitmask of pages that carry the factory marker: bit 0 = page 0, bit 1 = page 1, bit 2 = last page. |

A block is **bad** when the byte at `BadMarkerOffset` of any page selected by `BadMarkerPages`
is not `0xFF`. The library marks grown-bad blocks by programming `0x00` into that byte of the
lowest selected page, best effort.

### 1.2 Configured sizes

Layout version 1 is defined with the following sizes. They are compile-time settings
(`inc/eefs_config.h`), and **a build that changes any of them must also change
`EEFS_NAND_LAYOUT_VERSION`**, because every offset after the changed field moves. Readers reject
records and metadata pages whose layout version is not their own.

| Setting | Default | Used by |
|---|---|---|
| `EEFS_NAND_MAX_PATH_SIZE` | 256 | `Path` fields (max path length 255 characters + NUL) |
| `EEFS_NAND_CFS_NAME_SIZE` | 20 | `AppName` / `LibName` (matches `OS_MAX_API_NAME`) |
| `EEFS_NAND_CFS_SYMBOL_SIZE` | 64 | `EntrySymbol` |
| `EEFS_MAX_FILES` | 64 | Entry indexes; the format allows up to 126 |

## 2. Device layout

```
block 0 .. DirRegionBlocks/2 - 1      directory mirror 0 (ring of good blocks)
block DirRegionBlocks/2 .. DirRegionBlocks - 1   directory mirror 1 (ring of good blocks)
block DirRegionBlocks .. BlockCount-1 data blocks: files, free, bad
```

A file occupies `AllocBlocks` whole blocks anywhere in the data area. They need not be
contiguous. Page 0 of the file's **first block** is the metadata page (section 3). File data
starts at page 1 of the first block and continues through page `PagesPerBlock-1`, then from page 0
of the second block, and so on, in the order given by the metadata page's `Blocks[]` array.

The directory (section 4) is a log of records in the two mirrors. It records the block map (who
owns each block) and the file table (paths, slots, A/B state). The metadata page is the authority
for a file's **content** (size, CRCs, block list); the directory is the authority for **which
files exist and the A/B state**.

## 3. File metadata page (page 0 of a file's first block)

Magic `"EEFF"`. Total fixed part: **1532 bytes**, followed by the `Blocks[]` array. The rest of the
page, after the last `Blocks[]` element, is unused (`0xFF`).

### 3.1 Fixed fields

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | `Magic` | Bytes `45 45 46 46` (`"EEFF"`). Read as LE uint32: `0x46464545`. |
| 4 | 2 | `LayoutVersion` | 1 |
| 6 | 2 | Reserved | 0 |
| 8 | 4 | `HeaderCrc32` | CRC-32 over the page bytes `[0,8) + [12,496) + [1528,PageSize)`. That is: the whole page except this field and the signature block `[496,1528)`. It covers the unused tail, so the tail must be `0xFF`. |
| 12 | 4 | `WriteSeq` | Volume-wide monotonic write counter assigned when the page was built. On rebuild, the highest `WriteSeq` for a given (`Path`, slot) wins. |
| 16 | 256 | `Path` | Absolute path, e.g. `/cf/apps/sample_app.so`. NUL-terminated, zero padded. Must start with `/`. |
| 272 | 1 | `FileType` | 0 OTHER, 1 BOOT_FIT, 2 BOOT_ELF, 3 BOOT_DTB, 4 CFS_APP, 5 CFS_LIB |
| 273 | 1 | `Slot` | 0 = A, 1 = B, `0xFF` = NONE (single-copy file, never written with an explicit slot) |
| 274 | 2 | Reserved | 0 |
| 276 | 4 | `Attributes` | Bit 0 = `EEFS_ATTRIBUTE_READONLY`. Other bits reserved 0. |
| 280 | 4 | `Version` | Application image version, opaque to EEFS |
| 284 | 4 | Reserved | 0 |
| 288 | 4 | `FileSize` | Bytes of file data |
| 292 | 4 | `FileCrc32` | CRC-32 over the `FileSize` data bytes |
| 296 | 4 | `CreationDate` | Seconds since the epoch, `uint32` |
| 300 | 4 | `ModificationDate` | Seconds since the epoch, `uint32` |
| 304 | 192 | `TypeInfo` | Type-specific area, see 3.2 |
| 496 | 2 | `SigType` | 0 = unsigned. Other values reserved for a future secure-boot scheme. |
| 498 | 2 | Reserved | 0 |
| 500 | 4 | `SigLength` | 0 when `SigType` = 0 |
| 504 | 1024 | `Sig` | Signature bytes; bytes past `SigLength` are `0xFF`. When unsigned, all 1024 bytes are `0xFF`. |
| 1528 | 4 | `BlockCount` | Number of `Blocks[]` elements = the slot's `AllocBlocks`, >= 1 |
| 1532 | 6 * n | `Blocks[]` | See 3.3 |

The future signature covers the page with bytes `[496,1528)` set to `0x00`, followed by the
`FileSize` data bytes. Readers of version 1 must accept `SigType` = 0 only and treat any other
value as a corrupt page (it would mean a newer, incompatible writer).

### 3.2 Type-specific area (offsets are absolute page offsets)

Bytes in `[304,496)` that the type does not define are unused (`0xFF`).

| `FileType` | Offset | Size | Field |
|---|---|---|---|
| BOOT_FIT (1) | 304 | 8 | `DestAddress` (uint64): where the loader places the FIT image |
| BOOT_ELF (2) | 304 | 8 | `DestAddress` (uint64): where the raw ELF file is staged |
| | 312 | 8 | `EntryPoint` (uint64): entry override |
| | 320 | 4 | `Flags`: bit 0 = `EntryPoint` is valid and overrides the ELF header. Other bits 0. |
| BOOT_DTB (3) | 304 | 8 | `DestAddress` (uint64) |
| CFS_APP (4) | 304 | 20 | `AppName` (NUL-terminated) |
| | 324 | 64 | `EntrySymbol` (NUL-terminated) |
| | 388 | 2 | `Priority` (uint16) |
| | 390 | 1 | `ExceptionAction`: 0 = restart the app, 1 = reset the processor |
| | 391 | 1 | Reserved (0) |
| | 392 | 4 | `StackSize` (uint32) |
| | 396 | 8 | `LoadAddress` (uint64), 0 = let the loader choose |
| CFS_LIB (5) | 304 | 20 | `LibName` |
| | 324 | 64 | `EntrySymbol` (library init function) |
| | 388 | 8 | Reserved (0) |
| | 396 | 8 | `LoadAddress` (uint64) |
| OTHER (0) | | | nothing |

### 3.3 `Blocks[]` and the data layout

Element `i` (offset `1532 + 6*i`):

| Offset | Size | Field |
|---|---|---|
| +0 | 2 | `Block`: physical block number |
| +2 | 4 | `Crc32`: CRC-32 of the file data bytes that live in this block |

Data bytes are laid out as follows. Let `D0 = BlockBytes - PageSize` (the data capacity of the
first block, which loses page 0).

| Element | Pages used | File offsets held |
|---|---|---|
| `Blocks[0]` | 1 .. `PagesPerBlock-1` | `[0, D0)` |
| `Blocks[k]`, k >= 1 | 0 .. `PagesPerBlock-1` | `[D0 + (k-1)*BlockBytes, D0 + k*BlockBytes)` |

Each range is clipped to `FileSize`. `Crc32` covers exactly the clipped range; a block that
holds no data (because the file is smaller than its allocation) has `Crc32 = 0x00000000`.

Only pages that hold data (and page 0 of `Blocks[0]`) are programmed. The part of the last data
page past `FileSize` is `0xFF`. Pages after the last data page are left erased. The OOB of data
pages is not used by EEFS (the controller's ECC owns it).

The number of blocks a file needs is `ceil((PageSize + FileSize) / BlockBytes)`, minimum 1. The
maximum `BlockCount` is `floor((PageSize - 1532) / 6)`:

| Geometry | `BlockBytes` | Max `Blocks[]` | Max file size |
|---|---|---|---|
| 2048 / 64 pages | 128 KiB | 86 | 86 * 131072 - 2048 = 11,270,144 bytes (10.75 MiB) |
| 4096 / 128 pages | 512 KiB | 427 | 427 * 524288 - 4096 = 223,866,880 bytes (213.5 MiB) |
| 8192 / 64 pages | 512 KiB | 1110 | 1110 * 524288 - 8192 = 581,951,488 bytes (555 MiB) |

`FileSize` is also limited to `0xFFFFFFFF` and by the number of free data blocks.

`Blocks[]` is the only block-chaining structure: data blocks carry no header and no "next block"
pointer. That is deliberate: a modification that changes one data block then costs one new data
block plus one new first block (3.5), whereas per-block next pointers would force every block before
the changed one to be rewritten as well.

### 3.4 Validity of a metadata page

A metadata page is **valid** when all of the following hold:

1. the page read succeeded (correctable ECC errors are fine);
2. `Magic` is `"EEFF"` and `LayoutVersion` is 1;
3. `HeaderCrc32` matches;
4. `SigType` is 0;
5. `Path[0] == '/'` and `Path` is NUL-terminated within 256 bytes;
6. `1 <= BlockCount <= floor((PageSize-1532)/6)` and `BlockCount * BlockBytes >= PageSize + FileSize`;
7. every `Blocks[i].Block` is in `[DirRegionBlocks, BlockCount)`, and `Blocks[0].Block` is the block the page was read from.

A file's **content** is valid when its metadata page is valid and every `Blocks[i].Crc32` and
`FileCrc32` match the data read.

### 3.5 Copy-on-write: how a file is modified

A file's blocks are never erased and rewritten in place. Rewriting a file produces:

- a **new first block** with a new metadata page (new `WriteSeq`, `FileSize`, `FileCrc32`,
  `Blocks[]`) and the data pages of the first block;
- a fresh block for every data block whose content changed;
- the **same physical block**, listed again in the new `Blocks[]`, for every data block whose
  content did not change.

The directory then switches the slot's `FirstBlock` to the new block in the next commit, and the
old first block is erased by that commit's cleanup. Renaming a file or changing its attributes is
the degenerate case where only the first block is new and every data block is kept.

Consequences for readers:

- Between the commit and the cleanup erase, the old and the new metadata page both exist and list
  some of the same blocks. The directory names exactly one of them. A reader that has no directory
  (section 6) resolves the overlap by `WriteSeq`: the newer page wins and the older one is an
  orphan.
- Before the commit, the old copy is untouched (its blocks are still owned by it in the committed
  map) and the new blocks are free space in the committed map. A power cut leaves the old copy.
- Two slots (A and B) of one file never share blocks: `CopySlot` copies every block.

## 4. The directory log

### 4.1 Mirrors and rings

`DirRegionBlocks` is split in halves. Mirror 0 uses blocks `[0, DirRegionBlocks/2)`, mirror 1 uses
`[DirRegionBlocks/2, DirRegionBlocks)`. Within a mirror, the good blocks in ascending order form a
**ring**. Each mirror holds a copy of the same sequence of records; a reader needs only one good
mirror.

Rules:

- Records are made of whole pages and **never cross a block boundary**.
- Pages within a block are programmed in order (page 0 first). A record occupies consecutive
  pages.
- **The first record in a block is always a CHECKPOINT** (a full snapshot of the directory
  state). Later records in the same block are TXNs that modify it. Therefore a single block always
  contains the complete state of the directory at the time of its last record: checkpoint plus
  the TXNs after it. CHECKPOINT records appear only at page 0 of a block.
- When the next record does not fit in the current block, the writer **enters the next ring
  block**: it erases that block (which holds the oldest data of the mirror) and writes a
  CHECKPOINT carrying the new state. The previous block keeps a complete, valid state until the
  new checkpoint is complete, so a power cut while entering a block loses nothing.
- Every record carries a volume-wide sequence number `Seq`. `Seq` increases by one per commit,
  identically in both mirrors. Format writes `Seq = 1`.
- A commit writes the record to mirror 0, then to mirror 1, then performs the cleanup erases
  (4.5). A reader therefore may find mirror 1 one record behind mirror 0.

### 4.2 Record page header (32 bytes, at offset 0 of every record page)

Magic `"EEFJ"`.

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | `Magic` | Bytes `45 45 46 4A` (`"EEFJ"`). LE uint32 `0x4A464545`. |
| 4 | 2 | `LayoutVersion` | 1 |
| 6 | 1 | `RecType` | 1 = CHECKPOINT, 2 = TXN |
| 7 | 1 | `Mirror` | 0 or 1: the mirror this copy was written to. Informational; readers ignore it. |
| 8 | 4 | `Seq` | Record sequence number |
| 12 | 2 | `PageIndex` | 0-based index of this page within the record |
| 14 | 2 | `PageCount` | Pages in the record, >= 1 |
| 16 | 4 | `PayloadLen` | Bytes of item data in this page, starting at offset 32. `<= PageSize - 32`. |
| 20 | 8 | Reserved | 0 |
| 28 | 4 | `Crc32` | CRC-32 over `[0,28) + [32,PageSize)`: the whole page except this field. Bytes after the payload are `0xFF` and are covered. |
| 32 | `PayloadLen` | Items | See 4.3 |

A **page** is valid when: the read succeeded, `Magic` and `LayoutVersion` match, `RecType` is 1
or 2, `PageIndex < PageCount`, `PayloadLen <= PageSize - 32`, and `Crc32` matches.

A **record** starting at page `p` is valid when pages `p .. p+PageCount-1` are all valid, all
have the same `Seq` and `RecType`, their `PageIndex` is `0 .. PageCount-1` in order, and
`p + PageCount <= PagesPerBlock`. A record that fails any of these is **torn** and must be
ignored entirely (none of its items may be applied).

### 4.3 Items

The payload of a record page is a sequence of items. Items **never span pages**; a writer that
cannot fit the next item in the current page pads the page with `0xFF` and continues in the next
page of the same record. There is no alignment requirement.

Item header (4 bytes):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `Type` |
| 1 | 1 | Reserved (0) |
| 2 | 2 | `Len`: body length in bytes |
| 4 | `Len` | Body |

Readers must skip items with an unknown `Type` using `Len` (none are defined in version 1, but
this keeps the iterator robust) and must stop at `PayloadLen`. An item whose `4 + Len` exceeds the
remaining payload makes the page, and therefore the record, invalid. So does an item of a type defined below whose
`Len` disagrees with its definition (VOLUME `Len` other than 40, ENTRY other than 332, MAPCHUNK other
than `8 + Count` or with `Count` 0, CLEANUP other than `4 + 2 * Count` or with `Count` 0).

#### 4.3.1 VOLUME (Type 1, Len 40)

Present exactly once in every record, as the first item.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `PageSize` |
| 4 | 4 | `OobSize` |
| 8 | 4 | `PagesPerBlock` |
| 12 | 4 | `BlockCount` |
| 16 | 4 | `DirRegionBlocks` |
| 20 | 4 | `MaxFiles`: the writer's `EEFS_MAX_FILES` |
| 24 | 4 | `WriteSeqHigh`: highest `WriteSeq` assigned to any metadata page at commit time (committed or not) |
| 28 | 4 | `FormatTime`: `uint32` seconds, set by format |
| 32 | 2 | `PathSize` (256) |
| 34 | 2 | `CfsNameSize` (20) |
| 36 | 2 | `CfsSymbolSize` (64) |
| 38 | 2 | Reserved (0) |

A reader must check `PageSize`, `PagesPerBlock`, `BlockCount`, `DirRegionBlocks` against the
geometry it was given and `PathSize`/`CfsNameSize`/`CfsSymbolSize` against its build; a mismatch
means the device was formatted for a different configuration (the library reports
`EEFS_GEOMETRY_MISMATCH`). `MaxFiles` greater than the reader's `EEFS_MAX_FILES` is also a
mismatch, since entries beyond the reader's table could exist.

#### 4.3.2 MAPCHUNK (Type 2, Len 8 + Count)

A run of block-map bytes.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `StartBlock` |
| 4 | 4 | `Count`, >= 1 |
| 8 | `Count` | Map bytes for blocks `StartBlock .. StartBlock+Count-1` |

Map byte values:

| Value | Meaning |
|---|---|
| `0x00 .. 0xFB` | Owned by entry `value / 2`, slot `value % 2` (so entry index <= 125) |
| `0xFC` | Never on media. RAM-only "freeing" state (freed in an uncommitted batch). Serialized as `0xFF`. |
| `0xFD` | Directory region block |
| `0xFE` | Bad block |
| `0xFF` | Free |

A CHECKPOINT carries the whole map `[0, BlockCount)` in one or more chunks. A TXN carries only the
ranges that changed. Replay is "copy the bytes": later chunks overwrite earlier ones.

#### 4.3.3 ENTRY (Type 3, Len 4 + 328 = 332)

A complete snapshot of one file-table entry. Replay is "overwrite entry `Index`". A CHECKPOINT
carries an ENTRY item for every entry that is in use; **entries absent from a checkpoint are not in
use**. A TXN carries the entries that changed in that commit (including entries that became
unused, which are written with `InUse = 0`).

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `Index`: entry index, `< MaxFiles` |
| 2 | 2 | Reserved (0) |
| 4 | 328 | Entry record, below |

Entry record (328 bytes):

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 256 | `Path` | Absolute path, NUL-terminated, zero padded. Meaningless when `InUse` = 0 (written as zeros). |
| 256 | 1 | `InUse` | 0 or 1 |
| 257 | 1 | `IsAB` | 1 if the file opted into A/B (a slot was ever written explicitly) |
| 258 | 1 | `ActiveSlot` | 0 = A, 1 = B. Always 0 when `IsAB` = 0. |
| 259 | 1 | `FallbackCompatible` | 1 if the inactive copy may be loaded when the active one fails |
| 260 | 1 | `ActiveFailed` | 1 if the active copy failed a CRC/ECC check and nobody has repaired it |
| 261 | 1 | `FileType` | As in 3.1 |
| 262 | 2 | Reserved | 0 |
| 264 | 4 | `Attributes` | As in 3.1 |
| 268 | 4 | `CreationDate` | `uint32` seconds |
| 272 | 28 | `Slot[0]` (A) | Slot record, below |
| 300 | 28 | `Slot[1]` (B) | Slot record |

Slot record (28 bytes):

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 1 | `InUse` | 0 or 1. When 0, the remaining fields are 0. |
| 1 | 1 | Reserved | 0 |
| 2 | 2 | `FirstBlock` | Block holding the metadata page |
| 4 | 2 | `AllocBlocks` | Blocks owned by this slot = the metadata page's `BlockCount` |
| 6 | 2 | Reserved | 0 |
| 8 | 4 | `FileSize` | Copy of the metadata page value at the last commit that included this entry |
| 12 | 4 | `Version` | Copy of the metadata page value |
| 16 | 4 | `WriteSeq` | Copy of the metadata page value |
| 20 | 4 | `FileCrc32` | Copy of the metadata page value |
| 24 | 4 | `ModificationDate` | Copy of the metadata page value |

Every change to a file's content writes a new first block (copy-on-write, see 3.5) and is
committed together with the directory, so in a committed state the copied fields always equal the
metadata page of `FirstBlock`. A difference means corruption; the metadata page wins.

#### 4.3.4 CLEANUP (Type 4, Len 4 + 2 * Count)

Blocks whose erase is pending after this commit: the first blocks of slots that this commit
removed or superseded. They are already `0xFF` (free) in the map of the same record.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `Count`, >= 1 |
| 2 | 2 | Reserved (0) |
| 4 | 2 * `Count` | Block numbers |

The list in the **newest valid record** is the pending-cleanup list of the volume. A record without
a CLEANUP item means nothing is pending (every commit performs the previous pending cleanup before
writing its own record). Readers that do not write (bootloaders) can ignore this item; its purpose
is to let the library finish an interrupted cleanup and to let the health check report it.

### 4.4 Record composition

| Record | Items, in order |
|---|---|
| CHECKPOINT | VOLUME, MAPCHUNK(s) covering `[0, BlockCount)`, one ENTRY per in-use entry, CLEANUP if pending |
| TXN | VOLUME, MAPCHUNK(s) for changed ranges (possibly none), ENTRY per changed entry (possibly none), CLEANUP if any |

Readers must not depend on this order, but writers produce it. A TXN whose map and entry set are
unchanged (e.g. one that only carries a refreshed `WriteSeqHigh`) is legal but the library does not
write one; a commit with nothing to write does not touch the media.

### 4.5 Cleanup erases

After both mirrors hold the new record, the library erases each block listed in CLEANUP. Those
blocks hold the metadata pages of removed or superseded copies; erasing them guarantees that a
future rebuild (section 6) cannot resurrect a file that was deliberately removed. Data blocks other
than the first are not erased until they are reused.

### 4.6 Capacities

Payload area per page = `PageSize - 32`. Item sizes: VOLUME 44, ENTRY 336, MAPCHUNK 12 + bytes,
CLEANUP 8 + 2 per block.

| | 2048 | 4096 | 8192 |
|---|---|---|---|
| Payload bytes per page | 2016 | 4064 | 8160 |
| ENTRY items per full page | 6 | 12 | 24 |
| CHECKPOINT pages, 4096 blocks, 64 files in use | 13 | 7 | 4 |
| CHECKPOINT pages, 4096 blocks, 20 files in use | 6 | 3 | 2 |
| TXN pages, 20 changed entries, few map ranges | 4 | 2 | 1 |

Worked example, 4096-byte pages, 4096 blocks, 64 files: page 0 holds VOLUME (44) and a MAPCHUNK of
4008 blocks (4020); page 1 holds the remaining 88 map bytes (100) and 11 entries (3696); pages 2..5
hold 12 entries each; page 6 holds the last 5. The maximum checkpoint size for a configuration is
`MaxCheckpointPages`; the library requires `PagesPerBlock >= 2 * (MaxCheckpointPages + 1)` so that
a block always has room for a checkpoint plus at least one TXN.

## 5. Finding the current directory state

This is the algorithm both the library and the bootloader use. It reads at most
`DirRegionBlocks + 2 * PagesPerBlock` pages (plus the second pass over the chosen chains).

1. For each directory block `b` in `[0, DirRegionBlocks)`: read page 0 (data and OOB). If the OOB
   bad marker is set, or the read fails, or the page is not a valid record page with `RecType =
   CHECKPOINT` and `PageIndex = 0`, the block is not a candidate. Otherwise remember `(b, Seq)`.
2. For each mirror, order its candidates by `Seq` descending. For each candidate in that order:
   walk the block's chain (step 3). The first candidate whose first record (the checkpoint) is
   valid is that mirror's **current block**; candidates with a higher `Seq` whose checkpoint is
   torn are ignored (the writer was interrupted while entering that block).
3. Walking a chain: start at page 0. Read the record header, validate the record (4.2) by reading
   all its pages. If valid, note `LastSeq = Seq`, advance to page `p + PageCount`, and repeat while
   `p < PagesPerBlock`. A TXN's `Seq` must be exactly `LastSeq + 1`; otherwise stop. Stop at the
   first page that is not a valid record header (an erased page, a torn record, or garbage).
   The chain is `Checkpoint(C) .. TXN(L)` with `L >= C`.
4. Pick the mirror with the greater `L`. On a tie pick mirror 0. If neither mirror has a valid
   checkpoint, the directory is lost (section 6).
5. Apply the chosen chain in order: for each record, for each item: VOLUME -> check geometry, take
   `WriteSeqHigh`; MAPCHUNK -> copy bytes; ENTRY -> overwrite entry `Index`; CLEANUP -> replace the
   pending-cleanup list. Before applying a CHECKPOINT, start from "all entries unused, map all
   `0xFF`". Because records must be validated before any of their items are applied and a reader
   may have only one page buffer, this is done in two passes per record: validate every page, then
   re-read and apply.

The result is the committed directory state as of `Seq = L`. Anything written to the data area
that the state does not reference is either free space or an orphan (section 6).

Readers that need only one file (bootloaders) apply ENTRY items selectively, see 7.2.

## 6. Rebuilding without a directory

Used only when neither mirror has a valid checkpoint.

1. For each block `b` in `[DirRegionBlocks, BlockCount)`: read page 0 with OOB. If the bad marker
   is set, `b` is bad. If the page is a valid metadata page (3.4), it is a candidate for
   (`Path`, slot) where slot = `Slot` if A or B, else A.
2. For each (`Path`, slot), the candidate with the highest `WriteSeq` wins; on a tie, the lower
   block number wins. Losers are orphans.
3. **Overlap rule:** take the winners in descending `WriteSeq` order. A winner that lists a block
   already claimed by a winner with a higher `WriteSeq` is discarded entirely (orphan). This
   resolves the superseded page 0 left by any copy-on-write rewrite (3.5: a modification, a
   rename or an attribute change, whose old and new page 0 share the unchanged data blocks) when
   its cleanup erase was cut.
   Each remaining winner claims the blocks in its `Blocks[]`. Group winners by `Path` into entries (at most
   `EEFS_MAX_FILES`; further paths are ignored and remain orphans). For each entry:
   `IsAB` = 1 if any winner has `Slot` A or B explicitly; `ActiveSlot` = A if slot A is present,
   else B; `FallbackCompatible` = 0; `ActiveFailed` = 0; `Attributes`, `FileType`,
   `CreationDate` from the active slot's page. All other blocks are free.

Writers program a file's data blocks before its first block, but within the first block page 0
necessarily comes before the block's own data pages. An interrupted write can therefore leave a valid
metadata page whose first block holds incomplete data (every other listed block is complete). A
rebuilder should check `Blocks[0].Crc32` against the first block's data before accepting a candidate
in step 1 and treat a candidate that fails it as an orphan (the library's rebuild does; a reader that
skips the check may pick such a copy and then fail its CRC check when loading it).

The library reports the device as not formatted (`EEFS_NOT_FORMATTED`) only when neither mirror has a
valid checkpoint and the data region holds no valid metadata page at all.

A rebuild recovers every completely written file. It cannot distinguish committed from
uncommitted: it may include files from a batch that was never committed, and a removed or
superseded copy whose cleanup erase was interrupted. The library mounts the rebuilt state in RAM
and persists it at the next commit or `EEFS_RepairDirectory`.

## 7. Bootloader procedure

### 7.1 Inputs

The bootloader needs `ReadPage`, the geometry, one page buffer of `PageSize` bytes and about one
kilobyte of stack. It never writes.

### 7.2 Finding a file

1. Run section 5 steps 1-4 to pick the mirror and chain.
2. Replay the chain tracking a single **candidate**: for each ENTRY item with index `i`:
   - if `i` equals the candidate's index: the item replaces the candidate if `InUse` = 1 and its
     `Path` equals the name, otherwise it clears the candidate (the entry was removed or renamed);
   - otherwise, if `InUse` = 1 and `Path` equals the name, the item becomes the candidate.
   Skip MAPCHUNK and CLEANUP. Check VOLUME against the geometry.
3. If step 1 found no valid checkpoint in either mirror, use the **scan fallback**: run section 6
   for the one name only (read every data block's page 0, keep the highest `WriteSeq` per slot,
   the lower block on a tie). A candidate is accepted only if its first block's data matches its
   `Blocks[0].Crc32` (section 6: a page 0 left by an interrupted write can be complete while its
   own block's data is not); a rejected candidate does not displace an older accepted one.
   The result has `IsAB` per section 6, `ActiveSlot` = A if present else B, and
   `FallbackCompatible` = 0. The scan is **never** used when the directory is valid but a metadata
   page is bad: scanning could pick up a removed, superseded or incompatible copy. The scan does
   not apply the overlap rule (it tracks one name); the library's rebuild does.
4. No candidate: `EEFS_FILE_NOT_FOUND`.

### 7.3 Choosing the slot and loading

```
DirSlot    = IsAB ? ActiveSlot : A
TrySlot    = SlotHook(Name, DirSlot)            /* BSP hook; default returns DirSlot */
if Slot[TrySlot].InUse == 0: TrySlot = DirSlot
Other      = 1 - TrySlot

rc = LoadSlot(TrySlot)                          /* 7.4 */
if rc == SUCCESS: LoadedSlot = TrySlot; ActiveFailed = entry.ActiveFailed; return SUCCESS
if rc == NO_SPACE_LEFT_ON_DEVICE: return rc     /* destination too small: not a failed copy, no fallback */
ActiveFailed = 1
if Slot[Other].InUse && entry.FallbackCompatible:
    rc2 = LoadSlot(Other)
    if rc2 == SUCCESS: LoadedSlot = Other; return SUCCESS with ActiveFailed = 1
    return rc2 with ActiveFailed = 1
return rc with ActiveFailed = 1                 /* EEFS_CORRUPT_FILE or EEFS_IO_ERROR */
```

A name with an explicit `@A` / `@B` suffix (the library's name rule) selects that copy only: it must be
in use (else `EEFS_FILE_NOT_FOUND`), the hook is not called and there is no fallback.

The hook exists for automatic rollback after a boot that crashed without a CRC failure (for
example a boot counter in persistent RAM). The hook's choice is tried first, as the BSP's explicit
instruction. If that copy fails, the other slot is tried only when `FallbackCompatible` is set,
which is the same rule that applies to the directory's choice.

`ActiveFailed` and `LoadedSlot` are returned to the BSP, which hands them to the application
through its boot-info mechanism. The application records the failure with `EEFS_SetFailed` at its
next commit.

### 7.4 Loading one slot

1. Read page 0 of `Slot.FirstBlock`. Validate per 3.4, and additionally require `Path` equals the
   name and `Slot` is `0xFF` or equals the slot being loaded. Failure: `EEFS_CORRUPT_FILE`
   (`EEFS_IO_ERROR` if the read itself failed).
2. If `FileSize > MaxSize`: `EEFS_NO_SPACE_LEFT_ON_DEVICE`.
3. For each `Blocks[k]`, read the data pages into the destination in order (3.3), computing the
   per-block CRC as they arrive; compare with `Blocks[k].Crc32`. Mismatch: `EEFS_CORRUPT_FILE`.
   Uncorrectable ECC or read error: `EEFS_IO_ERROR`.
4. Compare the CRC of the whole destination with `FileCrc32`. Mismatch: `EEFS_CORRUPT_FILE`.
5. Return `FileType`, `FileSize`, `Version` and the type-specific fields.

Reading `Blocks[k]` for `k >= 1` requires page 0 again (the `Blocks[]` array). A loader with one
page buffer re-reads page 0 at each block transition; it is one extra page read per block.

## 8. Power-loss guarantees (summary)

| Interrupted operation | What a reader sees |
|---|---|
| Writing a new file, before commit | Directory unchanged. The partially written blocks are free in the map; a complete metadata page there is an orphan. |
| Rewriting an existing file (copy-on-write, 3.5), before commit | Directory unchanged and the old copy intact: none of its blocks was erased or programmed. The new blocks are free in the committed map; a complete new metadata page there is an orphan that shares the kept data blocks with the live copy (resolved by `WriteSeq` on a rebuild). The reader sees exactly the old content. |
| Rewriting an existing file, after commit, before cleanup | The new copy is live. The old metadata page still exists; its block is on the pending cleanup list and is erased by the next commit. |
| Commit, during mirror 0's record | Both mirrors still end with `Seq = S-1`. The torn record is ignored. |
| Commit, between mirror 0 and mirror 1 | Mirror 0 ends with `S`, mirror 1 with `S-1`. The state is `S`. The library brings mirror 1 up to date with a checkpoint at its next commit. |
| Commit, during cleanup | State `S`, pending cleanup in the newest record. The library finishes it before its next write. |
| Entering a new ring block (erase, or writing the checkpoint) | The previous block still holds the complete state `S-1`. |
| Format | Either no valid checkpoint (`EEFS_NOT_FORMATTED`, format again) or a complete empty volume. When an existing volume is formatted again, a cut while the directory region is being erased can also leave the complete old volume (from the mirror not yet erased). |
