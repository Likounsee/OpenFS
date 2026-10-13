# OpenFS disk format

**Status: experimental development format.** The workspace has one canonical volume layout and one implementation. The format identifier in the superblock is validated explicitly; unknown identifiers are rejected. This is not a production compatibility promise.

OpenFS defines its own filesystem. It is not an NTFS implementation and does not mount historical OpenFS C images. Do not use it for important data or format physical devices.

## Superblock

The primary superblock is in block 0 and the matching backup is in the final block. Each occupies the first 4096 bytes of its filesystem block; any remainder must be zero. Fields are encoded explicitly in little-endian order.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic: `OPENFS\0\0` |
| 8 | 2 | Format identifier; currently accepts value 2 |
| 10 | 2 | Header size (48) |
| 12 | 4 | Filesystem block size |
| 16 | 8 | Total blocks |
| 24 | 8 | Feature flags |
| 32 | 16 | Volume UUID |
| 48–4087 | 4040 | Reserved; must be zero |
| 4088 | 4 | CRC32C over bytes 0–4087 |
| 4092–4095 | 4 | Reserved tail; must be zero |

Supported filesystem block sizes are powers of two from 4096 through 65536 bytes. Geometry arithmetic is checked for overflow. Primary and backup superblocks must decode identically and match the actual device.

## Canonical volume layout

For block size `B` and volume length `N` blocks:

- Block 0: primary superblock.
- Bitmap starts at block 1 and covers all volume blocks.
- Immediately after the bitmap: checkpoint A, checkpoint B, then a four-block redo journal ring.
- The data region begins after the two checkpoints and four journal blocks.
- Block `N - 1`: backup superblock.

Let `bits_per_bitmap_block = B * 8`, `bitmap_blocks = ceil(N / bits_per_bitmap_block)`, `checkpoint_a = 1 + bitmap_blocks`, `checkpoint_b = checkpoint_a + 1`, `ring_start = checkpoint_b + 1`, `data_start = ring_start + 4`, and `backup_superblock = N - 1`. The volume must have at least 12 blocks and at least one data block after all reserved regions.

Bitmap bits are least-significant-bit first within each byte; set means allocated. The superblocks, bitmap blocks, checkpoints, journal ring, and padding bits beyond the end of the volume must be marked allocated. Only blocks in the data interval may be allocated or freed by the volume allocator.

## Journal and recovery

The journal is a bounded single-writer redo log. It validates record framing, sequence and checkpoint state, rejects writes to protected volume-control blocks, and replays committed records during writable open. A read-only check does not replay records or mutate the image.

The four-block ring limits transaction size. The journal currently protects bitmap allocator changes; it is not yet a general transaction manager for namespace records, directory entries, extent maps, and descriptor publication. Descriptor allocation and descriptor writing are not one atomic transaction, so interruption can leave an orphan allocation which the checker can report.

## Records, ownership and verification

Metadata records, directory entries, extent-map pages, checkpoints, journal records, and the persistent inventory descriptor use explicit codecs and checksums where defined. Decoders reject malformed lengths, invalid fields, duplicate ownership, out-of-range pointers, and references to free blocks. Structural corruption is fatal; bounded orphan/dangling diagnostics are reported separately where recovery can safely continue.

## Validation gates

Before production use, OpenFS still needs journal-aware persistent namespace mutations, stronger crash-cut and flush-order evidence, safe physical-device discovery/exclusion, and OS integration. Passing image-backed tests does not establish physical-media durability.
