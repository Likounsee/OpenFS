# OpenFS

OpenFS (Open File System) is a portable, modern filesystem designed as an
independent core with OS-specific adapters.

ArchiaOS is the first planned integration. The core does not depend on ArchiaOS,
Linux, Windows, BSD, or a CPU architecture.

## Current status

**Foundation phase — on-disk superblock and block-device contract implemented.**

Implemented and tested:
- portable block-device API;
- explicit little-endian superblock serialization;
- primary and backup superblocks;
- CRC32C metadata integrity checking;
- version and geometry validation;
- overflow-safe range validation;
- formatter/remount regression tests;
- corruption rejection tests;
- CMake build;
- GitHub Actions build, tests and ASan/UBSan checks.

Not implemented yet:
- allocation bitmaps;
- inodes;
- directories and namespace operations;
- file extents/data mapping;
- journal transactions and crash recovery;
- permissions/links/timestamps;
- fsck and recovery tooling;
- OS adapters.

These features will be added only after their on-disk format and invariants are
specified and covered by regression tests.

## Design

See:
- docs/architecture.md
- docs/format.md

The intended stack is:

OpenFS Core -> Block Device API -> OS adapter

The OpenFS core never calls an OS-specific API.
