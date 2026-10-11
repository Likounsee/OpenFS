# OpenFS disk format — version 1 draft

**Status: experimental draft, not a stable compatibility promise.** Formatting and validation exist; allocation, journaling, recovery, file operations and mount support do not.

## Superblock record

Each superblock occupies the first 4096 bytes of a filesystem block; any remaining bytes in that block must be zero. Integer fields are little-endian and are encoded explicitly, never through Rust struct memory layout.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic: OPENFS\\0\\0 |
| 8 | 2 | Format version (1) |
| 10 | 2 | Header size (48) |
| 12 | 4 | Block size in bytes |
| 16 | 8 | Total volume blocks |
| 24 | 8 | Feature flags |
| 32 | 16 | Volume UUID bytes |
| 48–4087 | 4040 | Reserved; must be zero |
| 4088 | 4 | CRC32C, little-endian |
| 4092–4095 | 4 | Reserved tail; must be zero |

CRC32C uses the reflected Castagnoli polynomial and covers bytes 0 through 4087. Supported filesystem blocks are powers of two from 4096 through 65536 bytes.

## Version 1 volume layout

- Block 0: primary superblock.
- Blocks 1 through 1 + bitmap_blocks - 1: allocation bitmap.
- Blocks data_start through total_blocks - 2: currently unstructured data region.
- Block total_blocks - 1: backup superblock.

bits_per_bitmap_block = block_size * 8; bitmap_blocks = ceil(total_blocks / bits_per_bitmap_block); data_start = 1 + bitmap_blocks. Formatting rejects a volume if no data block remains before the backup superblock. Bitmap bits are least-significant-bit first within each byte. A set bit means allocated. The primary superblock, bitmap blocks, backup superblock, and padding bits beyond the volume end are marked allocated. Other bitmap bits begin free. This is an initial allocation map, not yet a production allocator.

format_volume overwrites the provided device. It writes the backup and bitmap, flushes, then writes the primary and flushes again. This is deliberately not described as atomic or crash-safe; there is no journal yet. open_volume does not modify data and requires both superblocks to decode identically, geometry to match the device, and all reserved/padding bitmap bits to be allocated.

## Validation

The parser treats bytes as untrusted, validates framing, version, checksum, reserved bytes and geometry with checked arithmetic. Tests cover malformed fields even when CRC32C is recomputed, mirrored-superblock mismatch, reserved bitmap corruption, layout sizing, formatting, flush and reopen. Passing tests do not establish transaction recovery, concurrency safety or complete filesystem semantics.
