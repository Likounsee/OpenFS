# OpenFS

OpenFS (Open File System) is a portable, OS-independent filesystem designed to work across operating systems through small OS-specific adapters.

ArchiaOS is a planned integration, but the filesystem core itself does not depend on ArchiaOS, Linux, Windows, BSD, or any CPU architecture.

> **Project goal:** evolve OpenFS into a production-grade general-purpose filesystem with reliability, recovery, integrity, concurrency, and OS integration comparable in scope to mature filesystems such as NTFS.

OpenFS is **not release-ready yet**. Development is intentionally incremental: a feature is not considered complete merely because an API exists. A feature is complete only when its on-disk representation, implementation, error paths, transaction semantics, crash recovery, fsck interaction, regression tests, interaction tests, and documentation are covered.

---

## P0 reliability status — 2026-10-10

**Estimated progress: approximately 55% of the P0 work completed.** This is a rough engineering estimate based on the remaining roadmap items and the fixes/tests recorded in the development log; it is not a test pass rate, a formal completion metric, or a release-readiness claim. Treat this current section as the present estimate; higher percentages in older log entries are historical checkpoints from narrower scopes and do not override this status.

Recent work has hardened runtime admission and shutdown, file-lock handle lifetime, blocking lock handoff, CoW allocation/refcount consistency, and FSCK refcount-repair ownership preflight. Regression coverage now includes shutdown while a conflicting lock request is blocked, refusal to free or mutate the refcount of a CoW block whose bitmap says it is free, FSCK rejection of bitmap/ownership mismatches and bitmap-read failures, and journal recovery rejecting transaction starts during replay while permitting them again after successful recovery.

### P0 work still blocking completion

- **Concurrency:** finish the audit of shared mutable state, lock ordering, handle/mount lifetimes, and races across namespace, file I/O, CoW/refcounts, and unmount.
- **WAL and crash recovery:** close remaining post-COMMIT recovery/ownership gaps, including the case where replay restores an allocation/refcount but the resulting block has no durable namespace owner.
- **FSCK and repair:** complete safe repair architecture and expand ownership, journal, link-count, and corruption fixtures without weakening consistency checks.
- **Integrity:** extend persistent data-integrity coverage, corruption reporting, and scrub/repair capabilities.
- **Validation:** compile and run the regression suite on supported GCC/Clang and Windows/MSVC configurations, then verify sanitizer and CI results for the current branch head.

**Validation:** the file-lock shutdown regression, CoW-free/refcount consistency checks, FSCK ownership-preflight tests, journal recovery-gate tests (including post-recovery transaction admission), and idempotent replay-effect regression passed the observed GCC, Clang (including sanitizers and Release), and Windows Debug/Release matrix in [CI run 38057081587](https://github.com/Likounsee/OpenFS/actions/runs/38057081587). This validates that code head, not the entire P0 roadmap. OpenFS remains not release-ready until the outstanding recovery/ownership, concurrency, FSCK, and integrity gaps are closed and revalidated.

---

## Project status

OpenFS is in an active **filesystem-core hardening and feature-completion phase**.

The existing core already provides a substantial filesystem foundation:

- portable block-device API;
- versioned v1.5 on-disk format;
- primary and backup superblocks;
- CRC32C metadata integrity;
- geometry, bounds, and overflow validation;
- block and inode allocation bitmaps;
- inode allocation/freeing with generation reuse protection;
- checksummed inodes;
- five inline inode extents;
- checksummed depth-0 extent trees for files exceeding inline extent capacity;
- extent-backed read/write/truncate;
- checksummed fixed-size directory entries;
- deleted-directory-slot reuse;
- absolute path traversal;
- `.` and `..`;
- symlinks and parent-path symlink following;
- create, mkdir, unlink, rename and hard links;
- permissions, credentials and root/superuser bypass;
- timestamps;
- extended attributes;
- POSIX-style ACLs and inheritance;
- file handles/descriptors;
- handle duplication/reference counting;
- filesystem mount/unmount with superblock fallback;
- checksummed WAL/journal records;
- read-only integrity scrub with block-level issue callbacks, continued scanning after localized I/O failures, and multi-issue reporting;
- transactions and committed-transaction replay;
- mounted allocator and inode-allocation state transitions are WAL-backed and replayable after post-COMMIT publication failures; direct file writes/truncates remain explicit-transaction APIs to preserve concurrent I/O semantics;
- journal checkpoint/reclamation;
- transaction-aware namespace/file mutation;
- allocation/free operations poison their transaction if a later refcount or checksum metadata step fails, preventing partial allocator updates from being committed;
- transaction fault-injection tests cover both CoW refcount-table and data-checksum-table failures after an allocation bit is staged;
- transaction fault-injection coverage;
- copy-on-write cloning and refcounting;
- discard/free handling for shared CoW blocks;
- orphan handling;
- fsck consistency checking;
- Linux, Windows and ArchiaOS adapter contracts;
- CMake builds for GCC/Clang/MSVC;
- automated GitHub Actions CI.

### Current feature state

| Area | Status |
|---|---|
| On-disk format | Implemented / hardened |
| Allocation / bitmaps | Implemented / hardened |
| Inodes | Implemented / hardened |
| Extents / extent tree | Implemented / hardened |
| Directories / namespace | Implemented / hardened |
| Hard links / symlinks | Implemented |
| ACL / permissions / xattrs | Implemented |
| File handles | Implemented |
| WAL / transactions / replay | Implemented / actively hardened |
| CoW / refcounting | Implemented / actively hardened |
| Generic metadata-CoW block primitive | **Implemented as stage-1 groundwork; metadata roots not yet migrated** |
| Orphans | Implemented |
| FSCK consistency checking | Implemented / actively hardened |
| Sparse files | **Implemented, final hardening ongoing** |
| SEEK_DATA / SEEK_HOLE | **Implemented, regression-tested** |
| Snapshots | Planned |
| Named streams / ADS | Planned |
| Change journal | Planned |
| Quotas | Planned |
| Compression | Planned |
| Encryption / key management | Planned |
| Reparse-like objects | Planned |
| Data checksums / scrub | Partial; read-only scrub can report block-level read/checksum issues via `openfs_scrub_ex` |
| Controlled FSCK repair | Partial / planned expansion |
| Cache / read-ahead / writeback | Planned |
| mmap / direct I/O | Planned |
| Linux VFS integration | Planned |
| Windows/WinFSP integration | Planned |
| Notifications | Planned |
| Persistent file IDs | Planned |
| Benchmarks / performance tuning | Planned |

---

# Roadmap

## P0 — Reliability, correctness and recovery

These are the highest priority. Advanced features must not be built on top of unsafe core semantics.

### 1. Concurrency audit

- [ ] Audit every shared mutable structure.
- [ ] Verify mount lifetime locking.
- [ ] Verify inode locking.
- [ ] Verify directory locking.
- [ ] Verify allocation/bitmap locking.
- [ ] Verify journal locking.
- [ ] Verify CoW/refcount locking.
- [ ] Verify lock ordering and recursive call paths.
- [ ] Detect and eliminate lock-order inversions.
- [ ] Close multi-step namespace TOCTOU windows.
- [ ] Stress concurrent create/remove/rename.
- [ ] Stress concurrent read/write/truncate.
- [ ] Stress concurrent hard-link/symlink operations.
- [ ] Stress concurrent CoW/refcount operations.
- [ ] Test unmount/lifetime races.
- [ ] Test concurrent snapshots with active writers once snapshots exist.

The existing locking layer is deliberately conservative. Finer-grained locking may be introduced later, but correctness comes first.

### 2. WAL, transaction and crash recovery

- [ ] Audit every BEGIN/DATA/COMMIT/checkpoint transition.
- [ ] Test partial journal writes.
- [ ] Test partial flushes.
- [ ] Test corrupted journal records.
- [ ] Test interrupted checkpointing.
- [ ] Test abandoned transactions.
- [ ] Test true transaction interleaving/corruption.
- [ ] Preserve a sticky recovery-required state when appropriate.
- [ ] Verify rollback state after rollback failure.
- [ ] Verify remount/replay after every injected failure.
- [ ] Ensure a durable COMMIT remains recoverable even when later final writes fail.

Required ordering:

**BEGIN/DATA → durable COMMIT → final writes → flush → checkpoint**

### 3. FSCK and repair

- [ ] Separate detection from repair.
- [ ] Add safe read-only check mode.
- [ ] Add controlled repair mode.
- [ ] Validate inode bitmap tails.
- [ ] Validate block bitmap tails.
- [ ] Validate inode ownership.
- [ ] Validate extent ownership.
- [ ] Validate CoW/refcount ownership exactly.
- [ ] Validate directory reachability.
- [ ] Validate link counts.
- [ ] Validate generations.
- [ ] Validate orphan state.
- [ ] Validate journal state.
- [ ] Validate snapshots once implemented.
- [ ] Validate streams once implemented.
- [ ] Validate quotas once implemented.
- [ ] Ensure repair never silently destroys recoverable data.
- [ ] Add corruption fixtures and repair regression tests.

### 4. Integrity and corruption detection

- [ ] Define checksum coverage for all persistent metadata.
- [ ] Add/extend data-integrity checks where appropriate.
- [ ] Detect silent corruption during reads.
- [ ] Add scrub support.
- [ ] Report corrupted objects precisely.
- [ ] Repair corrupted structures where a safe source exists.
- [ ] Test corruption at every persistent layer.
- [ ] Test corruption combined with crash recovery.

---

# P1 — Core filesystem features

## 5. Snapshots

Implement snapshots using the existing CoW/refcount foundation.

- [ ] Persistent snapshot metadata.
- [x] Stage-1 generic metadata-CoW block header/refcount/copy-before-write primitive (not sufficient for snapshots by itself).
- [ ] Atomic snapshot creation.
- [ ] Read-only snapshot view.
- [ ] Persistent snapshot IDs.
- [ ] Snapshot listing.
- [ ] Snapshot deletion.
- [ ] Snapshot lifetime/refcount accounting.
- [ ] CoW interaction.
- [ ] Snapshot rollback.
- [ ] Crash recovery.
- [ ] FSCK support.
- [ ] Snapshot + write interaction tests.
- [ ] Snapshot + CoW interaction tests.
- [ ] Snapshot + truncate tests.
- [ ] Snapshot + rename/link tests.
- [ ] Snapshot + crash tests.
- [ ] Snapshot + corruption tests.

## 6. Sparse files

- [x] Hole-aware writes.
- [x] Sparse truncate extension.
- [x] Sparse truncate shrink.
- [x] Reclaim only mapped blocks.
- [x] Preserve holes when converting extent representations.
- [x] Sparse extent-tree handling.
- [x] `SEEK_DATA`.
- [x] `SEEK_HOLE`.
- [x] Synchronize sparse seeking with runtime inode locking.
- [x] Handle adjacent logical extents correctly even when their physical blocks are non-contiguous.
- [x] FSCK coverage for sparse layouts.
- [ ] Crash/fault-injection coverage for every sparse write/truncate path.
- [ ] Full handle/descriptor integration.
- [ ] Linux/Windows adapter exposure where supported.
- [ ] Interaction tests with CoW and snapshots.

## 7. Named streams / Alternate Data Streams

- [ ] Define persistent stream metadata.
- [ ] Main/default stream semantics.
- [ ] Named stream create/open/read/write/truncate.
- [ ] Stream delete/rename.
- [ ] Independent stream sizes and allocation.
- [ ] Stream permissions/security semantics.
- [ ] Stream transactions.
- [ ] Stream crash recovery.
- [ ] Stream FSCK.
- [ ] Stream + CoW.
- [ ] Stream + snapshots.
- [ ] Windows ADS mapping.

## 8. Change journal

This is deliberately separate from the recovery WAL.

- [ ] Persistent change records.
- [ ] Monotonic journal/change IDs.
- [ ] Create/delete records.
- [ ] Rename records.
- [ ] Write/size/metadata change records.
- [ ] Link/unlink records.
- [ ] Transactional publication.
- [ ] Recovery semantics.
- [ ] Incremental reader API.
- [ ] Rotation/pruning.
- [ ] FSCK validation.
- [ ] Crash tests.
- [ ] Windows/OS notification integration.

## 9. Quotas

- [ ] Per-user quota.
- [ ] Per-group quota.
- [ ] Optional project quota.
- [ ] Block limits.
- [ ] Inode/file-count limits.
- [ ] Soft limits.
- [ ] Hard limits.
- [ ] Persistent quota metadata.
- [ ] Transactional accounting.
- [ ] Correct rollback on failed allocation.
- [ ] FSCK accounting verification.
- [ ] Crash/recovery tests.

---

# P2 — Advanced filesystem capabilities

## 10. Compression

- [ ] Define compression format.
- [ ] Define compressed extent representation.
- [ ] Read/write path.
- [ ] Partial block updates.
- [ ] CoW interaction.
- [ ] Snapshot interaction.
- [ ] Checksums.
- [ ] FSCK.
- [ ] Crash recovery.
- [ ] Benchmark real workloads.

## 11. Encryption

First establish architecture before implementation.

- [ ] File/directory encryption model.
- [ ] Key management architecture.
- [ ] Key rotation.
- [ ] Authentication/integrity.
- [ ] Encrypted data vs metadata boundaries.
- [ ] Crash recovery.
- [ ] Snapshot semantics.
- [ ] CoW semantics.
- [ ] FSCK semantics.
- [ ] Never store plaintext encryption keys on disk.

## 12. Reparse-like objects / Windows semantics

- [ ] Reparse-point representation.
- [ ] Junction-like behavior.
- [ ] Stable object semantics.
- [ ] Security integration.
- [ ] FSCK support.
- [ ] Crash tests.
- [ ] Windows mapping.

## 13. Notifications

- [ ] Namespace change notifications.
- [ ] Create/delete/rename.
- [ ] Write/metadata changes.
- [ ] Directory watching.
- [ ] Transaction ordering.
- [ ] Change-journal integration.
- [ ] Linux integration.
- [ ] Windows integration.

---

# P3 — OS integration, I/O and performance

## 14. Persistent file IDs

- [ ] Stable file IDs.
- [ ] Generation/reuse semantics.
- [ ] Rename stability.
- [ ] Hard-link stability.
- [ ] Unlink semantics.
- [ ] Reboot stability.
- [ ] Snapshot semantics.
- [ ] FSCK validation.

## 15. Cache and I/O

- [ ] Page/file cache.
- [ ] Read-ahead.
- [ ] Write-back.
- [ ] Dirty-state tracking.
- [ ] Ordering/barriers.
- [ ] `fsync`.
- [ ] `fdatasync`.
- [ ] Direct I/O where appropriate.
- [ ] mmap.
- [ ] Power-loss simulation.
- [ ] Cache/recovery interaction tests.

## 16. Linux integration

- [ ] VFS layer.
- [ ] Mount/unmount.
- [ ] inode/file operations.
- [ ] page cache integration.
- [ ] mmap.
- [ ] ACL/xattr mapping.
- [ ] POSIX locking semantics.
- [ ] notifications.
- [ ] credentials/process integration.
- [ ] sparse seeking.
- [ ] stable error mapping.

## 17. Windows integration

- [ ] WinFSP/native integration architecture.
- [ ] ADS.
- [ ] Windows ACL/security descriptor mapping.
- [ ] reparse points.
- [ ] file locking.
- [ ] notifications.
- [ ] persistent file IDs.
- [ ] timestamps/metadata semantics.
- [ ] durability semantics.
- [ ] stable Windows error mapping.

The Windows/native driver is intentionally **not the immediate priority**. It must sit on stable handle, VFS, locking, cache and error semantics.

## 18. Benchmarks and optimization

- [ ] Sequential read/write.
- [ ] Random read/write.
- [ ] Small-file workloads.
- [ ] Large-file workloads.
- [ ] Large directories.
- [ ] Rename/unlink.
- [ ] Hard links.
- [ ] fsync.
- [ ] Transactions.
- [ ] CoW.
- [ ] Snapshots.
- [ ] FSCK.
- [ ] Compare against appropriate reference filesystems.
- [ ] Optimize only from measurements.

---

# Cross-feature completion rule

A feature is **not complete** until all applicable items exist:

1. on-disk format;
2. public/internal API;
3. implementation;
4. validation and error handling;
5. transaction semantics;
6. crash recovery;
7. persistence/durability;
8. FSCK detection;
9. FSCK repair where safe;
10. concurrency behavior;
11. regression tests;
12. fault-injection tests;
13. interaction tests with existing features;
14. adapter/OS semantics where applicable;
15. documentation.

This rule applies especially to snapshots, sparse files, streams, quotas, compression and encryption.

---

# NTFS-level feature audit

OpenFS is not required to copy NTFS internals. The objective is to reach comparable **general-purpose filesystem capability, reliability and OS usability** using an architecture appropriate to OpenFS.

The audit therefore covers:

- files/directories;
- persistent file IDs;
- hard links;
- symbolic/reparse links;
- ACL/security descriptors;
- extended attributes;
- named streams/ADS;
- sparse files;
- compression;
- encryption architecture;
- quotas;
- change journal;
- notifications;
- locking;
- durability;
- crash recovery;
- integrity;
- fsck/repair;
- snapshots;
- CoW;
- cache/I/O;
- VFS integration;
- Windows integration;
- Linux/POSIX semantics;
- performance.

The comparison must distinguish **implemented**, **partially implemented**, **architecturally supported**, **planned**, and **missing** instead of pretending that an API alone makes a feature equivalent to NTFS.

---

# Development and debugging rule

Development follows:

**OBSERVE → REPRODUCE/TEST → CORRECT → BUILD → TARGETED TEST → FULL SUITE → CI**

Rules:

- inspect the current implementation before changing it;
- never remove or weaken an existing regression test to make a change pass;
- turn every discovered bug into a regression test when practical;
- keep commits small and coherent;
- preserve crash consistency;
- prefer explicit corruption-class errors when rollback cannot be guaranteed;
- validate persistence, not only in-memory state;
- use GitHub Actions as the authoritative remote build/test validation;
- never claim CI is green without an actual successful run.

---

# Locking architecture

The runtime lock hierarchy is intentionally ordered:

    MOUNT
      ↓
    DIRECTORY
      ↓
    INODE
      ↓
    ALLOCATION
      ↓
    JOURNAL

The mounted superblock carries a runtime-only lock context. It is never serialized to disk.

The initial locking strategy is deliberately conservative and coarse-grained. Finer-grained locking can be introduced only after correctness and lock-order behavior are proven.

---

# Architecture

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

The filesystem core must never directly call OS-specific APIs.

---

# Project structure

- `OpenFS/include/` — public API;
- `OpenFS/src/` — filesystem implementation;
- `OpenFS/tests/` — automated tests;
- `OpenFS/docs/` — technical documentation;
- `.github/` — CI configuration.

---

# Documentation

Technical documentation:

- `OpenFS/docs/architecture.md`
- `OpenFS/docs/format.md`
- `OpenFS/docs/extent-tree-v1.3.md`
- `OpenFS/docs/adapters.md`
- `OpenFS/docs/file-handles.md`
- `OpenFS/docs/orphans.md`
- `OpenFS/docs/OPENFS_STATUS_LOG.md` — chronological development, fixes, regressions and remaining work.

The status log is intentionally maintained separately from this roadmap so the README remains a stable project contract while the log records what was actually changed.

---

# Durability contract

For transaction durability, the intended ordering is:

**BEGIN/DATA → durable COMMIT → final writes → flush → checkpoint**

A transaction that has reached durable COMMIT must remain recoverable even if a later final write, flush or checkpoint operation fails.

Direct file APIs attempt rollback on persistence/flush failures. If rollback itself cannot be made durable, OpenFS returns a corruption-class error instead of falsely reporting success.

Transactional APIs remain the stronger crash-atomic interface.

---

# License

See the repository license.
