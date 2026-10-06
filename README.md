# OpenFS

OpenFS (Open File System) is a portable, OS-independent filesystem designed to
work across operating systems through small OS-specific adapters.

ArchiaOS is a planned integration, but the filesystem core itself does not
depend on ArchiaOS, Linux, Windows, BSD, or any CPU architecture.

## Current status

OpenFS is in **active filesystem-core hardening**. The on-disk format, allocation,
inodes, extents, directories, namespace operations, permissions, transactions,
journal recovery, adapters, and fsck are already implemented. The current
development phase is moving from single-threaded correctness toward a filesystem
that can safely serve concurrent OS workloads.

OpenFS is **not release-ready yet**. The next major milestone is a thread-safe
core with explicit locking semantics, followed by real file handles/descriptors,
namespace race protection, security metadata, advanced allocation features,
VFS integration, and finally native OS filesystem integration.

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
- absolute path traversal with ., .., and symlink handling;
- create, mkdir, unlink, rename, hard links, and symbolic links;
- parent-path symlink following for namespace mutations;
- inode and path-level permission-bit access checks, root/superuser bypass, and automatic timestamps;
- credential-aware namespace mutation APIs;
- credential-aware file read/write/truncate APIs and link/symlink creation APIs;
- sticky-directory ownership checks for credential-aware unlink/rename;
- filesystem mount/unmount with primary/backup superblock fallback;
- checksummed journal records;
- transaction ownership, BEGIN/DATA/COMMIT handling and committed-transaction replay;
- journal checkpoint/reclamation;
- transaction-aware namespace and file mutation support;
- journal-full handling without partial transaction publication;
- transaction fault-injection coverage;
- extent-tree allocation, readback, shrink, and checksum-corruption regression coverage;
- truncate-shrink ordering that preserves inode/block consistency when inode persistence fails;
- recovery tests verifying committed transactions remain recoverable after final-write failures;
- fsck consistency checking with allocation, extent, alias, inode, directory, generation, and link-count invariants;
- CMake build with GCC/Clang/MSVC support;
- Linux, Windows, and ArchiaOS adapter contracts;
- automated GitHub Actions CI with Debug, CTest, and ASan/UBSan coverage.

### Current hardening phase

The current development order is:

1. **Thread safety and internal locking**
   - portable mutex/rwlock primitives;
   - lock ownership and lock-order validation;
   - mount lifecycle protection;
   - inode, directory, allocation, and journal locking;
   - concurrency regression tests;
   - elimination of lock-order inversions and race-prone shared state.

2. **File handles / descriptors — implementation started**
   - open/close lifecycle (implemented in the core handle API);
   - read/write/seek/truncate through handles;
   - open flags and access modes;
   - descriptor duplication/reference counting (implemented in the core handle API);
   - reference-counted objects;
   - correct lifetime semantics when a pathname is unlinked.

3. **Namespace and security hardening**
   - TOCTOU and path-race protection;
   - stable inode/path lookup semantics;
   - ACLs and ACL inheritance;
   - extended attributes (xattrs);
   - file locking;
   - sparse files.

4. **Advanced filesystem features**
   - copy-on-write;
   - snapshots;
   - snapshot rollback;
   - user/group quotas;
   - volume encryption and key management;
   - compression.

5. **Integrity and repair**
   - controlled fsck repair;
   - periodic scrub;
   - stronger corruption detection;
   - assisted/automatic repair where safe;
   - complete cross-checking of filesystem structures.

6. **VFS and operating-system integration**
   - stable VFS layer;
   - definitive mount/unmount API;
   - definitive block-device API;
   - filesystem/page cache;
   - OS credentials/process integration;
   - stable public filesystem API;
   - root filesystem support;
   - boot support where an OS integration requires it.

7. **Native filesystem drivers**
   - native Windows filesystem driver;
   - Linux integration;
   - ArchiaOS integration;
   - other OS adapters where useful.

The Windows driver is intentionally **not the immediate priority**. It will be
built on top of stable file-handle, VFS, cache, locking, and error semantics.
This avoids coupling kernel integration to APIs that are still evolving.

## Locking architecture

OpenFS now has a portable internal locking layer and a runtime lock set for
mounted filesystems. The lock hierarchy is intentionally ordered from outer
filesystem lifetime to the most global mutable journal state:

    MOUNT
      ↓
    DIRECTORY
      ↓
    INODE
      ↓
    ALLOCATION
      ↓
    JOURNAL

The runtime currently provides dedicated directory, inode, allocation, and
journal mutexes. The locks are recursive where required by the existing call
graph, and lock acquisition/release order is checked per thread. Out-of-order
unlock attempts are rejected instead of silently corrupting the lock-order
tracking state.

The mounted superblock carries a runtime-only pointer to this lock context. It
is never serialized into the on-disk superblock.

The current locking pass is deliberately conservative: it provides a correct
coarse-grained synchronization layer first. Per-inode/per-directory sharding
and finer-grained concurrency can be introduced later without changing the
on-disk format.

## Scope boundary

The filesystem core remains independent of the host OS. OS-specific behavior
belongs in adapters or, later, in native filesystem integrations.

OpenFS is intended to become a **real general-purpose filesystem**, not merely
a formatting library or a userspace demonstration. Features are prioritized
according to what a general-purpose operating system actually needs.

## Project structure

Everything belonging to the filesystem is inside OpenFS/:

- OpenFS/include/ — public API;
- OpenFS/src/ — filesystem implementation;
- OpenFS/tests/ — automated tests;
- OpenFS/docs/ — technical documentation.

GitHub Actions configuration is kept in .github/.

## Architecture

                    Operating System
                           |
                    VFS / Native API
                           |
                    OS integration
                           |
                    +-------------+
                    |   OpenFS    |
                    |    core     |
                    +-------------+
                           |
                    Block device API
                           |
                  Disk / SSD / image

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

## Verified robustness guarantees

The current CI-verified core includes persistent rollback tests for truncate
grow/shrink failures, partial-tail zeroing failures, existing-file partial
writes, extent-tree root allocation rollback, and allocation/free failures.
The tests validate persistent inode state, allocation bitmaps, remountability,
and fsck where the fixture represents a fully reachable filesystem state.

Journal checkpoint clearing is performed backwards. This is intentional: if a
checkpoint is interrupted, the remaining journal prefix is still a syntactically
valid prefix for replay instead of leaving a cleared first block followed by
stale records. Final filesystem writes are flushed before checkpointing.

Direct file APIs attempt rollback on write/truncate persistence and flush
failures. If the underlying device cannot complete the rollback or its final
flush, the API returns a corruption-class error rather than claiming that the
old state is durable. Transactional APIs remain the stronger crash-atomic
interface.

## Documentation

See:

- OpenFS/docs/architecture.md
- OpenFS/docs/format.md
- OpenFS/docs/extent-tree-v1.3.md
- OpenFS/docs/adapters.md

The documentation will evolve alongside the stable filesystem API and VFS.
