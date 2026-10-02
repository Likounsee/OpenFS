# OpenFS on-disk format v1

All integers are little-endian. Offsets and lengths are measured in filesystem blocks unless stated otherwise.

## Device requirements

Block size is a power of two from 4096 through 65536 bytes. The device block size must equal the filesystem block size and the filesystem currently requires at least 32 blocks.

## Superblocks

Block 0 is the primary superblock. The final block is a backup superblock. The header occupies the first 4096 bytes of each superblock. CRC32C is stored at offset 4088 and covers bytes 0..4087.

## Metadata layout

Block 1 is reserved. Block 2 is also reserved for future metadata. The remaining metadata area is split into three parts:

1. inode bitmap;
2. inode table;
3. journal area.

The inode bitmap size is calculated from the inode table size, so large filesystems can have more than one bitmap block. The bitmap has one bit per inode. Bit 0 is reserved for the root inode and is set during formatting.

The inode table uses 256-byte inode records. Every inode has a magic value and CRC32C checksum. The root inode is inode 1 and starts as a directory with itself as parent.

The journal area is reserved by the formatter, but its transaction record format is **not stable yet**. No metadata operation will be considered durable until the journal ordering and recovery rules are defined.

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

Unused bytes are reserved and written as zero.

## Compatibility

Unknown major versions are rejected. A newer minor version is rejected until its compatibility rules are explicitly implemented.
