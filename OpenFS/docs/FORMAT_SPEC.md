# OpenFS disk format — version 1 draft

**Status: experimental draft, not a stable compatibility promise.** V1 and V2 are separate experimental on-disk layouts. V2 reserves a persistent journal and uses it for allocator bitmap mutations; V1 does not. Persistent file operations and mounting are not implemented.

## Compatibility policy

The Rust implementation defines a **new, incompatible OpenFS format**. It does not mount historical OpenFS C v1.x images, including the v1.6 layout identified in the old source audit. Legacy images must be rejected, not guessed at or silently interpreted. A future explicit converter may be developed, but it is not part of the first Rust format's compatibility promise.

OpenFS is an NTFS-inspired filesystem with its own on-disk format; this does not claim compatibility with actual NTFS volumes. The current Rust layout is still experimental and may change before a stable release. No production data should be entrusted to it.

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

format_volume overwrites the provided device. It writes the backup and bitmap, flushes, then writes the primary and flushes again. This is deliberately not atomic or crash-safe. The standalone journal engine is not part of this version 1 layout, so it cannot protect volume formatting or ordinary allocator mutations. open_volume does not modify data and requires both superblocks to decode identically, geometry to match the device, and all reserved/padding bitmap bits to be allocated.

## Version 2 volume layout (experimental)

V2 uses the same 4096-byte superblock record framing, but the version field is 2. V1 and V2 must not be silently interchanged; the V1 opener rejects version 2 and the V2 opener rejects version 1.

- Block 0: primary V2 superblock.
- Bitmap blocks begin at block 1; the bitmap covers the whole volume.
- Immediately after the bitmap: checkpoint A, checkpoint B, then a four-block redo journal ring.
- Data blocks begin after those six journal blocks.
- The final block stores the backup V2 superblock.

V2 opening checks that the primary and backup superblocks match, validates the decoded geometry and reserved/padding bitmap bits, then opens and recovers the journal before returning writable volume state. The journal rejects redo targets in protected superblock blocks and the allocator journals bitmap updates. The journal has a deliberately bounded ring; a transaction must fit entirely in it.

V2 is still experimental: the journal is not yet a general transaction layer for namespace/file metadata; format operations are not crash-atomic; ownership checks and a read-only checker are not complete. Image-based fault injection is not evidence of physical USB durability or safe device discovery.

## Validation and stability gates

The parser treats bytes as untrusted, validates framing, version, checksum, reserved bytes and geometry with checked arithmetic. Tests cover malformed fields even when CRC32C is recomputed, mirrored-superblock mismatch, reserved bitmap corruption, layout sizing, formatting, flush and reopen. Passing tests do not establish transaction recovery, concurrency safety or complete filesystem semantics.

Before physical-device use is considered safe, V2 must have complete crash-cut and flush-order recovery evidence, allocator and metadata mutations must use a proven transaction layer, and safe device discovery/formatting safeguards must exist. Add golden image fixtures for every supported format version. If a serialized layout changes, bump the format version and reject older drafts unless an explicit migration path is implemented.
