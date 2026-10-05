# OpenFS on-disk format v1.3

All integers are little-endian. Offsets and lengths are measured in filesystem blocks unless stated otherwise.

## Device requirements

Block size is a power of two from 4096 through 65536 bytes. The device block size must equal the filesystem block size and the filesystem currently requires at least 64 blocks.

## Superblocks

Block 0 is the primary superblock. The final block is a backup superblock. The header occupies the first 4096 bytes of each superblock. CRC32C is stored at offset 4088 and covers bytes 0..4087.

## Metadata layout

Block 1 is reserved. Block 2 is also reserved for future metadata. The metadata area starts at block 2. Block 3 onward is split into:

1. block bitmap;
2. inode bitmap;
3. inode table;
4. journal area;
5. data area.

The final block is reserved for the backup superblock. The block bitmap marks every reserved block as used during formatting.

The inode bitmap size is calculated from the inode table size, so large filesystems can have more than one bitmap block. The bitmap has one bit per inode. Bit 0 is reserved for the root inode and is set during formatting.

The inode table uses 256-byte inode records. Legacy inodes without the extent-tree flag keep up to five 24-byte extents (120 bytes total) in bytes 96..215. Tree-backed inodes use four inline extents and reserve bytes 208..215 (the tail of the fifth legacy extent slot) for a 64-bit extent-tree root block. Symlink inline data uses the same 120-byte on-disk union area, with the first 80 bytes available for the target. Symlink size is the target length and does not include a trailing NUL; the core adds a temporary terminator when exposing a C string. Every inode has a magic value and CRC32C checksum. The root inode is inode 1 and starts as a directory with itself as parent.

The journal area stores one checksummed record per filesystem block. A record has a 32-byte little-endian header: magic `OJNL1`, type, transaction id, sequence, payload length and CRC32C. New writers calculate CRC32C over the complete journal block with the CRC field zeroed. The reader also accepts legacy v1.3 records whose historical CRC covered `block_size - 4` bytes, provided the final four reserved bytes are zero; this preserves backward readability without changing the v1.3 format number. `BEGIN`, `DATA` and `COMMIT` records define transactions; only transactions with a valid commit record are replayed. Block-write DATA payloads use the `OJBD1` marker, target block number, 32-bit block offset, chunk length and chunk bytes. A committed transaction is applied to its final blocks only after the commit record is durable. After final writes are flushed, the journal is checkpointed and reclaimed. Mount replays committed records before making the filesystem available. The transaction wrapper buffers final-block writes in memory, journals them first, commits the journal, then writes the final blocks; aborted transactions never alter the final device blocks.

## Superblock fields

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | Magic `OPENFS` |
| 8 | 2 | Major version |
| 10 | 2 | Minor version |
| 12 | 8 | Feature flags |
| 20 | 4 | Block size |
| 24 | 4 | Header size (4096) |
| 28 | 8 | Total blocks |
| 36 | 8 | Metadata start |
| 44 | 8 | Metadata blocks |
| 52 | 8 | Block bitmap start |
| 60 | 8 | Block bitmap blocks |
| 68 | 8 | Inode bitmap start |
| 76 | 8 | Inode bitmap blocks |
| 84 | 8 | Inode table start |
| 92 | 8 | Inode table blocks |
| 100 | 8 | Journal start |
| 108 | 8 | Journal blocks |
| 116 | 8 | Data start |
| 124 | 8 | Data blocks |
| 132 | 8 | Root inode |
| 140 | 8 | Superblock generation |
| 148 | 16 | UUID |
| 4088 | 4 | CRC32C |

Unused bytes are reserved and written as zero.

## Extent tree v1.3\n\nA tree-backed inode points to a checksummed depth-0 leaf block. The block header is 32 bytes followed by 24-byte extent records. With a 4096-byte filesystem block, one leaf stores 169 overflow extents in addition to the four inline extents. The tree root is allocated from the data-area bitmap and is accounted for as metadata by fsck. Tree root bounds, magic, depth, entry count, extent ordering and CRC32C are validated before use.\n\n## Compatibility

Unknown major versions are rejected. A newer minor version is rejected until its compatibility rules are explicitly implemented. v1.3 journal records written with the historical `block_size - 4` CRC span remain readable by the current reader. Newly written v1.3 records always use the full-record CRC. A legacy record with a non-zero reserved four-byte tail is rejected as corrupt.

## Inode timestamps

Each 256-byte inode stores UTC nanoseconds in three 64-bit fields:

| Offset | Size | Field |
|---:|---:|---|
| 48 | 8 | access time (`atime_ns`) |
| 56 | 8 | modification time (`mtime_ns`) |
| 64 | 8 | metadata/change time (`ctime_ns`) |

These fields are part of the checksummed inode payload. Formatting and inode
allocation initialize them when the portable runtime clock succeeds. The core
updates modification/change timestamps for content and metadata operations;
automatic atime-on-read is deliberately not enabled.

## Feature compatibility

v1.3 images may advertise `OPENFS_FEATURE_EXTENT_TREE`. The current validator
rejects unknown feature bits and rejects the extent-tree feature when paired
with a minor version older than 1.3. Images without that feature retain the
legacy five-inline-extent interpretation. Reserved bytes are never reinterpreted
without the corresponding feature flag.
