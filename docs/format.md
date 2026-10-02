# OpenFS on-disk format v1

All integers are little-endian. Offsets and lengths are measured in filesystem
blocks unless stated otherwise.

## Device requirements

Block size is a power of two from 4096 through 65536 bytes. The current core
requires the device block size to equal the filesystem block size and requires
at least 32 blocks.

## Superblocks

Block 0 is the primary superblock. The final block is a backup superblock.
The header occupies the first 4096 bytes of each superblock. CRC32C is stored at
offset 4088 and covers bytes 0..4087.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | Magic OPENFS |
| 8 | 2 | Major version |
| 10 | 2 | Minor version |
| 12 | 8 | Feature flags |
| 20 | 4 | Block size |
| 24 | 4 | Header size (4096) |
| 28 | 8 | Total blocks |
| 36 | 8 | Metadata start |
| 44 | 8 | Metadata blocks |
| 52 | 8 | Inode bitmap start |
| 60 | 8 | Inode bitmap blocks |
| 68 | 8 | Inode table start |
| 76 | 8 | Inode table blocks |
| 84 | 8 | Journal start |
| 92 | 8 | Journal blocks |
| 100 | 8 | Root inode |
| 108 | 8 | Superblock generation |
| 116 | 16 | UUID |
| 4088 | 4 | CRC32C |

Unassigned bytes are reserved and written as zero.

## Initial layout

The formatter currently establishes block 0 as the primary superblock, block 1 as
reserved, a metadata region from block 2 to the penultimate block, and the final
block as the backup superblock. The metadata region is divided into an inode
bitmap, inode table and journal.

The inode, allocation and journal record layouts are not declared stable yet;
they will be specified together with their transaction invariants before the
corresponding durable APIs are implemented.

## Compatibility

Unknown major versions are rejected. Minor-version compatibility must be
explicitly implemented before a reader accepts a newer minor version.
