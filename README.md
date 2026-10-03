# OpenFS

OpenFS (Open File System) is a portable, OS-independent filesystem designed to
work across operating systems through small OS-specific adapters.

ArchiaOS is a planned integration, but the filesystem core itself does not
depend on ArchiaOS, Linux, Windows, BSD, or any CPU architecture.

## Where we are

OpenFS has a substantial filesystem core implemented and is currently in the
**robustness, crash-consistency, permissions, and scalability phase**. It is
**not yet ready to be used as a normal everyday filesystem**.

Current overall progress is approximately **85% of the planned project scope**.
This is an engineering estimate, not a release-readiness metric.

### Implemented

- portable block-device API;
- versioned v1.2 on-disk format;
- primary and backup superblocks;
- CRC32C checks for metadata;
- geometry, bounds, and overflow validation;
- block and inode allocation bitmaps;
- first-fit data block allocation/freeing;
- inode allocation/freeing with generation reuse protection;
- checksummed inodes;
- persistent five-extent inode storage in the v1.2 format;
- extent-backed file read/write/truncate;
- fixed-size checksummed directory entries with lookup/add/remove and deleted-slot reuse;
- absolute path traversal with `.`, `..`, and symlink handling;
- create, mkdir, unlink, rename, hard links, and symbolic links;
- parent-path symlink following for namespace mutations;
- inode and path-level permission-bit access checks and timestamp update APIs;
- credential-aware namespace mutation APIs (create/mkdir/unlink/rename/chmod/timestamps), including directory execute checks during traversal;
- credential-aware file read/write/truncate APIs and link/symlink creation APIs;
- filesystem mount/unmount with primary/backup superblock fallback;
- checksummed journal records;
- transaction ownership, BEGIN/DATA/COMMIT handling and committed-transaction replay;
- journal checkpoint/reclamation;
- transaction-aware namespace and file mutation support;
- journal-full handling without partial transaction publication;
- transaction fault-injection coverage for journal/data/flush/checkpoint failures;
- namespace transaction coverage for link/symlink commit and abort paths;
- transaction poisoning after failed transactional namespace/file mutations;
- extent-limit allocation rollback coverage when the five-inline-extent limit is reached;
- truncate-shrink ordering that preserves inode/block consistency when inode persistence fails;
- recovery tests verifying committed transactions remain recoverable after final-write failures;
- `fsck` consistency checking with allocation, extent, alias, inode, directory,
  generation, and link-count invariants;
- CMake build with GCC/Clang warning flags and MSVC-compatible warning configuration;
- CMake build and automated GitHub Actions CI with Debug, CTest, and ASan/UBSan.

### In active development

- complete permission enforcement and automatic timestamp semantics (credential-aware core APIs are now present; adapter/open semantics remain);
- broader authorization-aware namespace checks for mutation operations;
- stronger transaction poisoning/failed-state handling and crash consistency;
- broader fault-injection coverage around every WAL phase;
- atomicity of create/mkdir/unlink/rename/link/symlink operations;
- stronger file/truncate overflow and partial-I/O handling;
- larger files and extent-tree/indirect extent storage beyond five fragmented extents;
- additional fsck repair/recovery capabilities (the current fsck remains deliberately read-only);
- Linux, Windows, and ArchiaOS adapters;
- expanded documentation and compatibility guarantees.

### Planned hardening

Before considering the core production-ready, OpenFS still needs extensive
testing for corrupted media, interrupted writes, power-loss scenarios,
allocation inconsistencies, malformed metadata, extreme file sizes, and
adapter-specific behavior.

Any on-disk format evolution must remain versioned and documented so existing
v1.2 images are not silently broken.

## Project structure

Everything belonging to the filesystem is inside `OpenFS/`:

- `OpenFS/include/` — public API;
- `OpenFS/src/` — filesystem implementation;
- `OpenFS/tests/` — automated tests;
- `OpenFS/docs/` — technical documentation.

GitHub Actions configuration is kept in `.github/`.

## Architecture

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

Development follows:

**OBSERVE → REPRODUCE/TEST → CORRECT → BUILD → TARGETED TEST → FULL SUITE → CI**

The current implementation must always be inspected before changing it.
Discovered bugs should become regression tests whenever possible, and existing
tests must never be removed or weakened.

For transaction durability, the intended WAL ordering is:

**BEGIN/DATA → durable COMMIT → final writes → flush → checkpoint**

A transaction that has reached durable COMMIT must remain recoverable even if a
later final write, flush, or checkpoint operation fails.

See `OpenFS/docs/architecture.md`, `OpenFS/docs/format.md`, and `OpenFS/docs/adapters.md`.