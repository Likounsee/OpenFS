# OpenFS

OpenFS (Open File System) is a portable, OS-independent filesystem designed to
work across operating systems through small OS-specific adapters.

ArchiaOS is a planned integration, but the filesystem core itself does not
depend on ArchiaOS, Linux, Windows, BSD, or any CPU architecture.

## Current status

OpenFS is in active hardening and integration work. The filesystem core is implemented and is being validated through regression tests, boundary tests, filesystem-checking tests, adapter tests, and continuous integration.

The project should be considered **advanced development / hardening**, not a final release. Release readiness depends on the complete test matrix being green and the remaining integration work being completed.

### Implemented

- portable block-device API;
- versioned v1.3 on-disk format with fast formatting and optional full-zero formatting;
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

### Current audit status

The current hardening pass checks corruption handling, crash consistency, rollback paths, permissions, symlink and directory semantics, on-disk invariants, overflow boundaries, adapters, and test/CI consistency. Confirmed findings are corrected with regression coverage.
The current CMake configuration registers **25 CTest cases** on Linux and Windows (23 core tests plus the platform-independent ArchiaOS adapter test and the host adapter test). The final audit must verify that full matrix rather than relying on the historical 23/23 figure.

### Remaining hardening / integration

- ArchiaOS adapter integration against the actual ArchiaOS storage subsystem (the repository currently verifies only the documented callback contract);
- Windows adapter integration is verified by the GitHub Actions Windows build and adapter integration test;
- optional multi-level extent-tree nodes if a supported workload needs more than the current 169 overflow extents per 4 KiB leaf;
- additional fsck repair capabilities (fsck remains deliberately read-only);

### v1.3 journal CRC compatibility

The journal writer uses the current full-record CRC32C calculation. The reader also accepts the legacy v1.3 CRC span used by earlier development images, while requiring the legacy reserved tail bytes to remain zero. Thus legacy v1.3 journal records are backward-readable without changing the v1.3 format number. New records continue to use the full-record CRC.

### Scope boundary

The filesystem core is intentionally independent of the host OS. Linux and
ArchiaOS adapters now implement and test the documented block-device contract;
the Windows adapter is present and its documented callback contract is verified by the
GitHub Actions Windows build and adapter integration test.
- Core path APIs are not generally thread-safe/reentrant; callers must serialize concurrent path operations. The current extent-tree implementation deliberately stops at one
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
