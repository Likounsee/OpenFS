# OpenFS

OpenFS (Open File System) is a portable, OS-independent filesystem designed to
work across operating systems through small OS-specific adapters.

ArchiaOS is a planned integration, but the filesystem core itself does not
depend on ArchiaOS, Linux, Windows, BSD, or any CPU architecture.

## Development roadmap

- **P0 — Foundations / Correctness:** CLOSED
- **P1 — Transactions / Journal / Recovery:** CLOSED
- **P2 — Crash-Cut Hardening:** CLOSED
- **P3-A — Error Path / Double Failure:** CLOSED
- **P3-B — Boundary Matrix:** IN PROGRESS
- **P3-C — Final Release Validation:** PLANNED — final validation before release.

## Where we are

OpenFS has a substantial filesystem core implemented and is currently in the **final robustness, crash-consistency, and integration phase**. The
core remains an integration component rather than a turnkey mounted desktop
filesystem.

Current overall progress is approximately **98% of the planned project scope**.
This is an engineering estimate, not a release-readiness metric.

### Implemented

- portable block-device API;
- versioned v1.3 on-disk format;
- primary and backup superblocks;
- root-directory default traversal permissions (0755);
- CRC32C checks for metadata;
- geometry, bounds, and overflow validation;
- block and inode allocation bitmaps;
- first-fit data block allocation/freeing;
- inode allocation/freeing with generation reuse protection;
- checksummed inodes;
- persistent five-extent inode storage in the v1.3 format;
- versioned v1.3 depth-0 extent-tree leaves with checksummed overflow extents and files beyond five extents;
- extent-backed file read/write/truncate;
- fixed-size checksummed directory entries with lookup/add/remove and deleted-slot reuse;
- absolute path traversal with `.`, `..`, and symlink handling;
- create, mkdir, unlink, rename, hard links, and symbolic links;
- parent-path symlink following for namespace mutations;
- inode and path-level permission-bit access checks, root/superuser bypass, and automatic creation/content/metadata timestamps;
- credential-aware namespace mutation APIs (create/mkdir/unlink/rename/chmod/timestamps), including directory execute checks during traversal;
- credential-aware file read/write/truncate APIs and link/symlink creation APIs;
- sticky-directory ownership checks for credential-aware unlink/rename;
- filesystem mount/unmount with primary/backup superblock fallback;
- checksummed journal records;
- transaction ownership, BEGIN/DATA/COMMIT handling and committed-transaction replay;
- journal checkpoint/reclamation;
- transaction-aware namespace and file mutation support;
- journal-full handling without partial transaction publication;
- transaction fault-injection coverage for journal/data/flush/checkpoint failures;
- namespace transaction coverage for link/symlink commit and abort paths;
- transaction poisoning after failed transactional namespace/file mutations;
- extent-tree allocation, readback, shrink, and checksum-corruption regression coverage beyond the five-inline-extent limit;
- truncate-shrink ordering that preserves inode/block consistency when inode persistence fails;
- recovery tests verifying committed transactions remain recoverable after final-write failures;
- `fsck` consistency checking with allocation, extent, alias, inode, directory,
  generation, and link-count invariants;
- CMake build with GCC/Clang warning flags and MSVC-compatible warning configuration;
- CMake build with GCC/Clang warning flags, Linux and ArchiaOS adapter contract tests, and automated GitHub Actions CI with Debug, CTest, and ASan/UBSan;

### Remaining hardening / integration

- broader crash-cut testing for less frequently used namespace/error combinations;
- direct (non-transactional) API partial-I/O rollback hardening where practical; transactional APIs remain the crash-atomic interface;
- broader corruption/fault-injection matrix and compatibility regression images;
- Windows adapter is verified by the GitHub Actions Windows build and adapter integration test;
- ArchiaOS adapter integration against the actual ArchiaOS storage subsystem (the repository currently verifies only the documented callback contract);
- optional multi-level extent-tree nodes if a supported workload needs more than the current 169 overflow extents per 4 KiB leaf;
- additional fsck repair capabilities (fsck remains deliberately read-only);

### Scope boundary

The filesystem core is intentionally independent of the host OS. Linux and
ArchiaOS adapters now implement and test the documented block-device contract;
the Windows adapter is present but requires Windows-host CI/integration to be
verified. The current extent-tree implementation deliberately stops at one
checksummed leaf: a single 4 KiB leaf holds 169 overflow extents in addition to
four inline extents, so deeper nodes are only needed if that documented limit
is insufficient for a supported workload.

Any on-disk format evolution remains versioned and documented so existing v1.2
images are not silently broken.

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

See `OpenFS/docs/architecture.md`, `OpenFS/docs/format.md`, `OpenFS/docs/extent-tree-v1.3.md`, and `OpenFS/docs/adapters.md`.

### Verified robustness guarantees

The current CI-verified core includes persistent rollback tests for truncate grow/shrink
failures, partial-tail zeroing failures, existing-file partial writes, extent-tree
root allocation rollback, and allocation/free failures. The tests validate persistent
inode state, allocation bitmaps, remountability, and fsck where the fixture is a
fully reachable filesystem state.

Journal checkpoint clearing is performed backwards. This is intentional: if a
checkpoint is interrupted, the remaining journal prefix is still a syntactically
valid prefix for replay instead of leaving a cleared first block followed by stale
records. The final filesystem writes are flushed before checkpointing, so replaying
a surviving committed prefix is safe.

Direct file APIs attempt rollback on write/truncate persistence and flush failures.
If the underlying device cannot complete the rollback or its final flush, the API
returns a corruption-class error rather than claiming that the old state is
durable. Transactional APIs remain the stronger crash-atomic interface.
