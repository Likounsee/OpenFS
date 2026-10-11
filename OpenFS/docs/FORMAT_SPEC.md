# OpenFS disk format — initial draft

**Status: draft, not a stable compatibility promise.** No formatter or mountable volume exists yet.

## Superblock record v1

The current crate encodes one fixed-size 4096-byte record. Integer fields are little-endian. The encoding is explicit and never uses Rust struct memory layout.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic: OPENFS\0\0 |
| 8 | 2 | Format version (1) |
| 10 | 2 | Header size (48) |
| 12 | 4 | Block size in bytes |
| 16 | 8 | Total volume blocks |
| 24 | 8 | Feature flags |
| 32 | 16 | Volume UUID bytes |
| 48–4087 | 4040 | Reserved; must be zero |
| 4088 | 4 | CRC32C, little-endian |
| 4092–4095 | 4 | Reserved tail; must be zero |

CRC32C uses the reflected Castagnoli polynomial and covers bytes 0 through 4087. The checksum field is outside that range. Version 1 currently validates block sizes from 4096 through 65536 bytes (power of two) and requires at least two blocks. This is only a superblock record draft, not a complete volume layout.

## Validation

The parser treats all bytes as untrusted, checks the fixed framing and version, verifies the checksum before trusting payload fields, and independently validates decoded block size and volume length even if a modified record has a recomputed checksum. Tests include the standard CRC32C check value, each defined field, reserved regions, unsupported versions, and arithmetic overflow. A passing parser test does not establish crash consistency or a complete volume layout.

Decode requires exactly 4096 bytes, valid magic, supported version, expected header size, matching CRC32C, zero reserved bytes, and valid field bounds. Before a stable format release, add golden byte fixtures and define mirrored superblocks, complete volume layout, feature compatibility, and migration rules.
