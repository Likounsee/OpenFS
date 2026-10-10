# OpenFS

OpenFS (Open File System) is a portable, OS-independent filesystem designed to work across operating systems through small OS-specific adapters.

ArchiaOS is a planned integration, but the filesystem core itself does not depend on ArchiaOS, Linux, Windows, BSD, or any CPU architecture.

> **Project goal:** evolve OpenFS into a production-grade general-purpose filesystem with reliability, recovery, integrity, concurrency, and OS integration comparable in scope to mature filesystems such as NTFS.

OpenFS is **not release-ready yet**. Development is intentionally incremental: a feature is not considered complete merely because an API exists. A feature is complete only when its on-disk representation, implementation, error paths, transaction semantics, crash recovery, fsck interaction, regression tests, interaction tests, and documentation are covered.

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
- transaction fault-injection coverage, including allocator metadata failures after staging and wrapper cleanup after commit errors;
- journal replay prevalidation before publication callbacks, transaction/sequence ownership checks, and detection of block changes between reads;
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
| On-disk format, superblocks and metadata checksums | Implemented; further compatibility and failure validation remains |
| Allocation / bitmaps / inode allocation | Implemented; cross-structure ownership audit remains |
| Inodes / extents / directories / namespace | Implemented; full crash/concurrency matrix not yet verified |
| Hard links / symlinks / orphan handling | Implemented in the engine; end-to-end failure matrix remains |
| ACL / permissions / xattrs | Engine APIs implemented; native Windows security mapping not established |
| File handles / descriptors / range locks | Implemented; complete Windows sharing/delete semantics not established |
| WAL / transactions / replay / checkpoint | Implemented and actively hardened; full required failure matrix and CI are unverified |
| CoW / refcounting | Implemented and actively hardened |
| Generic metadata-CoW primitive | Stage-1 groundwork only; metadata roots are not generally migrated |
| FSCK consistency checking | Implemented; full invariant coverage not established |
| FSCK repair | Partial; exposed repair scope is limited |
| Data checksums / read-only scrub | Partial; scrub reports issues but does not repair corrupted file contents |
| Sparse files / SEEK_DATA / SEEK_HOLE | Engine support implemented; dedicated edge-case, crash and adapter coverage remains |
| Snapshots | Planned; existing metadata-CoW groundwork is not a complete snapshot feature |
| Named streams / NTFS ADS | Not implemented end to end |
| Change journal / quotas / compression / encryption / reparse points | Planned; no complete implementation confirmed |
| Windows / WinFsp adapter | Partial and read-only in the audited source; read-write operations remain incomplete |
| Linux VFS/kernel integration | Planned; userspace adapter support is distinct from native kernel integration |
| Benchmarks / performance tuning | No reproducible NTFS comparison verified |

---

# Roadmap and verified progress

This roadmap separates **code that exists** from **work that is verified complete**. Statuses below reflect source inspection, not a claim that tests passed. The detailed task list and file-level audit are maintained in [ROADMAP_P0_P3.md](ROADMAP_P0_P3.md).

## P0 — Reliability, correctness and recovery

**Overall status: IN PROGRESS — not complete.** The core has a WAL, transaction/replay logic, runtime locking, CoW/refcounts, FSCK diagnostics and read-only scrub. The remaining work is to prove their combined behavior under concurrency, corruption and failures.

| ID | Workstream | Current evidence | Remaining exit criteria | Status |
|---|---|---|---|---|
| P0-001 | Locking and lifecycle | Runtime admission, lock layer, handles and concurrency tests exist; a past rename lock-order inversion was fixed. | Audit all lock paths and interleavings; deterministic shutdown/unmount races; run race detection where available. | In progress |
| P0-002 | WAL, transactions and recovery | WAL commit/replay, sequence and transaction ownership checks, and failure-path tests exist. | Exercise partial writes/flushes/checkpoints and double failures; prove replay idempotence and old-or-new state at every cut point. | In progress |
| P0-003 | Block ownership and refcounts | Allocation bitmaps, inode allocation, extents and CoW/refcount machinery exist. | Prove cross-structure ownership invariants and detect leaks, double references and free-but-referenced blocks. | Not complete |
| P0-004 | FSCK and conservative repair | Consistency diagnostics exist; exposed repairs cover selected bitmap-tail and CoW-refcount cases. | Establish read-only boundaries, preflight and idempotence; broaden corruption fixtures without destructive ambiguous repairs. | Partial |
| P0-005 | Checksums and scrub | Optional data checksums and a read-only scrub path report block-level issues. | Complete persistent-layer coverage, corruption-plus-crash tests and reliable issue attribution; do not imply scrub repairs damaged data. | In progress |
| P0-006 | Bounds and malformed input | Geometry/bounds checks and boundary tests exist. | Audit arithmetic/conversions and fuzz disk parsers with sanitizer-backed, persistent regression seeds. | Not complete |
| P0-007 | CI on exact revision | GCC/Clang, sanitizer, Release and Windows jobs are defined in the workflow. | Obtain successful results for every required job on the exact final commit. The audit found no usable CI status. | Unverified |
| P0-008 | End-to-end crash cuts | Journal, namespace, crash-cut and double-failure test sources exist. | Inject failures around every relevant write/flush for multi-structure mutations; remount, replay and run FSCK after each cut. | Not complete |

**P0 completion rule:** do not mark P0 complete until the required failure/concurrency matrix has actually run, recovery and FSCK results are reviewed, and the required CI jobs are green on the exact commit.

## P1 — Core semantics and usable OS integration

**Overall status: NOT COMPLETE.** The engine exposes several core APIs, but the Windows adapter inspected in the source is read-only and does not provide a complete read-write filesystem interface.

| ID | Workstream | Current evidence | Remaining exit criteria | Status |
|---|---|---|---|---|
| P1-001 | Windows read-write adapter | The current WinFsp/FUSE compatibility adapter reports read-only behavior; writes return `-EROFS`, and mutation/durability callbacks are missing or incomplete. | Implement and test create/open/read/write/truncate/rename/unlink/readdir/stat/flush/fsync/release, error mapping and real Windows handle behavior. | Not implemented end to end |
| P1-002 | Handles, sharing and delete semantics | File descriptors/handles, reference counting and range-lock APIs exist. | Specify and test sharing modes, open-handle rename/unlink, deferred deletion, duplication/close races and inode reuse. | In progress / unverified |
| P1-003 | Windows permissions and ACLs | Engine ACL and access-check functionality exists. | Define and test SID/security-descriptor conversion, inheritance and fail-closed authorization in the adapter. | Not implemented end to end |
| P1-004 | Durability API and format compatibility | Transactional APIs, direct file operations and a versioned on-disk format exist. | Document and test flush/commit guarantees, feature flags, unknown-feature rejection and supported-version/upgrade policy. | In progress |

## P2 — Snapshots and advanced filesystem capabilities

**Overall status: MOSTLY PLANNED.** A metadata-CoW primitive and snapshot-related root field are groundwork only; they do not constitute a complete snapshot implementation.

| ID | Workstream | Current evidence | Remaining exit criteria | Status |
|---|---|---|---|---|
| P2-001 | Persistent snapshots | Metadata-CoW/catalogue groundwork exists; no complete public snapshot API/catalogue was found in the audited source. | Persistent IDs, atomic create/delete, immutable read-only views, correct refcounts, recovery and FSCK support. | Not implemented end to end |
| P2-002 | Named streams / NTFS ADS | Extended attributes exist, but they do not establish Alternate Data Stream semantics. | Define persistent stream storage and Windows mapping; test independent stream lifecycle, security, recovery and FSCK. | Not implemented |
| P2-003 | Quotas, compression, encryption and reparse points | No complete end-to-end implementation was confirmed for these feature families. | Evaluate separately; specify on-disk/security semantics before implementation and include crash recovery, integrity and FSCK coverage. | Planned |
| P2-004 | Persistent change journal | No durable USN-like change-journal API was confirmed. | Transactionally publish ordered change IDs/records; define retention, reset/overflow and recovery semantics. | Not implemented |

## P3 — Performance, maturity and longer-term integration

**Overall status: PLANNED.** Performance and platform maturity work should follow demonstrated P0 correctness and the required P1 semantics.

| ID | Workstream | Remaining exit criteria | Status |
|---|---|---|---|
| P3-001 | Reproducible benchmarks | Publish repeatable workloads and raw results for sequential/random I/O, small files, large directories, concurrency, transactions, CoW and FSCK; compare only under documented conditions. | Not started / unverified |
| P3-002 | Profile-guided optimization | Establish a baseline, optimize measured bottlenecks one at a time, then rerun correctness and performance suites. | Planned |
| P3-003 | Continuous fuzzing, upgrades and releases | Maintain parser fuzz corpora/seeds; test every supported format version and document backup, upgrade, rollback and reproducible release steps. | Planned |
| P3-004 | Native Linux integration decision | Explicitly decide and document userspace/FUSE versus kernel/VFS integration scope; validate the supported adapter operations. | Decision pending |

## Verification note

The source audit used commit `43263f22703e508b3ed9940c0b1c3c310b73a2b6` as its reference. **No build or tests were run during that audit, and the CI status for that revision could not be confirmed.** Test files existing in the repository are not evidence that those tests passed. See [the detailed P0–P3 audit](ROADMAP_P0_P3.md) for per-task files, tests, dependencies and measurable acceptance criteria.

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
