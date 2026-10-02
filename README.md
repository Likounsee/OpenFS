# OpenFS

OpenFS (Open File System) is a portable filesystem designed to work on different
operating systems through small OS-specific adapters.

ArchiaOS is the first planned integration, but the filesystem core itself does
not depend on ArchiaOS, Linux, Windows, BSD, or any CPU architecture.

## Where we are

We are building the real filesystem core step by step. It is **not yet ready to
be used as a normal everyday filesystem**.

Already implemented:
- portable block-device API;
- versioned on-disk format;
- primary and backup superblocks;
- CRC32C checks for metadata;
- strong geometry and overflow checks;
- automatic initial inode bitmap;
- block allocation bitmap and reserved data area;
- first-fit data block allocation and freeing;
- extent-backed file read/write/truncate;
- inode allocation and freeing;
- fixed-size checksummed directory entries with lookup/add/remove and deleted-slot reuse;
- absolute path traversal with `.`, `..`, and namespace create/mkdir/unlink/rename operations;
- hard links and basic symbolic-link storage/readlink operations;
- initial read-only filesystem consistency checking (`fsck` core), including extent-range/order and allocation-bitmap cross-checks;- basic inode permission-bit and timestamp update APIs;
- checksummed journal records with committed-transaction replay, block-write chunks, transaction ownership checks and checkpoint/reclamation;
- mount/unmount with primary/backup superblock fallback and committed-journal replay;
- inode table;
- checksummed inodes;
- root directory inode creation;
- bitmap and inode regression tests;
- CMake build and automated CI with sanitizers.

Still to build:
- complete symlink path resolution and loop handling;
- complete permission enforcement and automatic timestamp semantics;
- integrate transaction-aware APIs into every mutation path and complete journal reservation/space management;
- expand crash-consistency ordering and interrupted-write recovery tests;
- fsck and recovery tools;
- Linux, Windows and ArchiaOS adapters.

## Project structure

Everything belonging to the filesystem is inside `OpenFS/`:

- `OpenFS/include/` — public API;
- `OpenFS/src/` — filesystem implementation;
- `OpenFS/tests/` — automated tests;
- `OpenFS/docs/` — technical documentation.

The GitHub Actions configuration stays in `.github/` because GitHub requires
workflows to be stored there.

## Architecture

The planned structure is:

```text
Operating System
      |
      v
OpenFS adapter
      |
      v
OpenFS core
      |
      v
Block device API
      |
      v
Disk / SSD / image / virtual disk
```

The core must never call an OS-specific API directly.

## Development rule

We first define how data is stored on disk, then implement the code, then add
tests for normal cases and corrupted data. A bug found later should become a
regression test whenever possible.

See `OpenFS/docs/architecture.md` and `OpenFS/docs/format.md`.
