# OpenFS — Development Status Log

This file is the chronological engineering log for the OpenFS filesystem-core hardening and feature-completion work.

The main README contains the durable roadmap and completion criteria. This file records concrete work, regressions, fixes, tests, and remaining gaps.

> **Branch:** `OpenFS`  
> **Repository:** `Likounsee/OpenFS`

---

## How to read this log

- **FIX** — a correctness/reliability defect was corrected.
- **TEST** — a regression or interaction test was added.
- **HARDEN** — existing behavior was made safer without changing the intended public semantics.
- **FEATURE** — a new filesystem capability was implemented.
- **DOCS** — project documentation/roadmap was updated.
- **CI** — remote validation status that was actually observed.

A feature is not marked complete just because an API exists. The README completion rule applies.

---

# Current state

## Already implemented in the core

The repository already contains a substantial filesystem implementation covering:

- versioned v1.3 disk format;
- primary/backup superblocks;
- CRC32C metadata checks;
- geometry/bounds/overflow validation;
- block and inode bitmaps;
- inode allocation/freeing and generation reuse;
- checksummed inodes;
- inline extents and extent trees;
- file read/write/truncate;
- directories and namespace operations;
- `.` / `..` and symlinks;
- create/mkdir/unlink/rename;
- hard links;
- permissions and credentials;
- ACLs and inheritance;
- xattrs;
- file handles/descriptors;
- locks/runtime synchronization;
- WAL/journal;
- transaction commit/replay;
- orphan handling;
- CoW/refcounting/clone/discard;
- fsck consistency checking;
- Linux/Windows/ArchiaOS adapter contracts;
- automated CI.

The current work is therefore hardening and extending an already functional filesystem core rather than starting from an empty implementation.

---

# Major reliability fixes already completed

## CoW clone rollback and refcount correctness

### `301bca7`
**FIX — failed CoW clone rollback**

Corrected the failure path of `openfs_cow_clone_inode()`.

The rollback path now uses a valid FREE inode tombstone and checks failures instead of silently assuming that rollback operations succeed.

### `2e0fffa`
**FIX — FSCK exact ownership/refcount validation**

Removed the incorrect assumption that a block with a refcount of at least 2 is automatically valid.

FSCK now reasons about exact ownership/refcount relationships.

### `c602367`
**TEST — exact CoW ownership/refcount and overflow coverage**

Added regression coverage for exact CoW ownership and refcount overflow behavior.

### `7706759ec96c9f33729eb94e2d4fcbf0276c7a86`
**FIX/TEST — surface failed CoW refcount restoration**

A previously hidden rollback failure is now surfaced as:

`OPENFS_ALLOC_CORRUPT`

rather than being treated as successful recovery.

### `4ff17dd9bc7456d27e86bc58ae97b5732057cfae`
**TEST — allocator bitmap API rollback coverage**

Extended rollback testing to include the allocator bitmap API itself.

### Subsequent CoW hardening

The CoW implementation was further hardened for:

- clone/discard rollback;
- transactional clone;
- crash during clone;
- FSCK refcount checks;
- extent-tree cloning;
- uninitialized inode read handling;
- refcount-write rollback;
- nontransactional CoW clone publication durability;
- incomplete rollback signaling;
- unrecoverable rollback failure reporting.

The important design rule established here is:

> If rollback cannot be made reliable, OpenFS must report a corruption-class error instead of pretending the old state is durable.

---

# FSCK hardening

### `ad7037e6d24709dd464062a3dcde25aa06fb5b9a`
**FIX — FSCK cleanup on refcount allocation failure**

FSCK now correctly cleans up bitmap snapshots and temporary buffers when refcount-related allocation fails.

### `be9200982f9831c573a36c99a1d2b2ff0acf9c65`
**HARDEN — inode bitmap tail validation**

FSCK validates inode bitmap tail bits using a snapshot of the bitmap rather than trusting an unbounded tail.

### `10912fa8d8eb4097e3a41a9d07070884f2c411dc`
**TEST — reject stray inode bitmap tail bits**

Added a regression test proving that invalid tail bits are detected.

---

# Truncate and persistence hardening

### `40b693a03c162dd4a2899987613aa34fd43d262f`
**FIX — restore partial truncate tail after flush failure**

A partial tail modification caused by truncate is now restored when the subsequent flush fails.

This protects persistent file contents instead of only restoring in-memory state.

### `ac213112f88887785574c79dc11daecfb56771a3`
**TEST — truncate tail rollback**

Added regression coverage for the flush-failure rollback path.

---

# Journal and transaction recovery hardening

### `aeabedfa776bb7e88306171938fdfa0dd5f949aa`
**FIX — distinguish abandoned transactions from true interleaving**

Journal replay now permits an abandoned transaction to be skipped when the record sequence is otherwise valid, while still rejecting actual transaction interleaving/corruption.

### `f3b24e80fc2a1a921e9bb027fe0d106aba3422fa`
**TEST/FIX — journal replay corruption test**

Ensured the journal replay corruption regression test compiles and exercises the intended path.

### `950cf28a81e7d10d573dc0429e492dae208b4cb4`
**TEST — reject data after abandoned journal transaction**

Added a regression case proving that invalid data following an abandoned transaction is rejected.

### `a44f24bcd1425e1d121efab81da4ac08ef409ed1`
**FIX — propagate journal rollback corruption from transaction begin**

A rollback failure during journal transaction initialization is no longer hidden by a generic transaction error.

### `214aab7052e0bdbc17949e8d1ef42de2aee28c54`
**FIX — preserve transaction recovery state on WAL corruption**

Transaction state now preserves the fact that recovery is required when WAL corruption occurs.

### `ba737b5c7fbf94f025585b34c38b4472a1eb284f`
**TEST — poisoned transaction after WAL rollback corruption**

Added coverage proving that a transaction cannot silently continue after an unrecoverable WAL rollback failure.

### `81c4de009ceb2ca88e108450e53d8545f6dc04f6`
**TEST — restore transaction test entry point**

Restored the intended transaction regression-test entry point.

### `368a50402ce79889ac687989fd22a0fe6f373972`
**FIX — reverted unsafe transaction state change**

An unsafe repeated-transaction-begin state change was identified and reverted rather than keeping a risky semantic change.

This is an important part of the development process: unsafe fixes are removed instead of weakening invariants to make tests pass.

### `9bd85d9ebe51cd09e3e6eb491ed75888f365e0fa`
**FIX — restore journal slots after failed WAL writes**

Failed WAL writes now attempt to restore the overwritten journal slot.

### `45439b829da158f3a514b5bb4808f27105fb365e`
**FIX — initialize journal slot from journal state**

Journal slot initialization was corrected so rollback has the correct prior journal state available.

---

# Transaction semantics currently enforced

The transaction layer now distinguishes:

- inactive transaction;
- failed transaction;
- commit started;
- recovery required;
- successful commit;
- abort after recovery corruption.

The intended durable sequence remains:

**BEGIN/DATA → durable COMMIT → final writes → flush → checkpoint**

If WAL corruption or a rollback failure makes recovery uncertain, the transaction is poisoned instead of continuing as though nothing happened.

---

# Sparse file implementation

Sparse files were already partially implemented before the latest development pass, including:

- sparse writes;
- holes;
- sparse truncate extension;
- sparse truncate shrink;
- preservation of holes during extent representation changes;
- extent-tree sparse handling;
- reclaiming only mapped blocks.

The following regression tests already existed:

- `sparse_write_zeroes_intermediate_blocks`;
- `sparse_truncate_extension_keeps_holes`;
- `sparse_truncate_shrink_zeros_discarded_tail`;
- `sparse_truncate_shrink_reclaims_only_mapped_blocks`;
- `sparse_truncate_shrink_tree_to_inline_preserves_holes`;
- `sparse_truncate_shrink_tree_keeps_root_and_holes`.

---

# Sparse seeking work

### `e99bb2d629d1999b431f7216ba3b1f7d8e59d16d`
**FEATURE — expose sparse seeking API**

Added public APIs for:

- `openfs_file_seek_data()`;
- `openfs_file_seek_hole()`.

### `5d99638f9fc2af6feaac53fc622f843aaf4bce4f`
**FEATURE — implement sparse seeking**

Implemented the actual extent-aware SEEK_DATA/SEEK_HOLE logic.

Behavior includes:

- return the current offset when already inside data;
- find the next data extent;
- return the end of a data extent for SEEK_HOLE;
- identify holes between extents;
- use EOF as the final implicit hole boundary;
- reject out-of-range data seeks.

### `0bb083a017b49376b9019a07a5de3c710c7cceba`
**TEST — sparse seek regression coverage**

Added `sparse_seek_data_and_hole_reports_extents`.

### `2e6d035bbcf82ce21159b49d2e1ff9e7fe35a489`
**HARDEN — synchronize sparse seek operations**

Public sparse-seek APIs now participate in the runtime inode locking model and refresh the inode when a runtime context exists.

### `477f01fcacc2042ad39f8f34504b9204befac823`
**FIX — adjacent sparse extents**

Fixed SEEK_HOLE behavior when two logical extents are adjacent but physically non-contiguous.

The implementation now continues through adjacent logical data instead of incorrectly reporting a hole.

### `585f38c1851bf825b997203c00ad3c4532488b3a`
**TEST — adjacent non-contiguous sparse extents**

Added a regression test that inserts a physical spacer block between logically adjacent file blocks and verifies that SEEK_HOLE still reaches EOF.

### `35850c3782f67cdd3865b3d86c3b444813631c1e`
**FIX — sparse test lifetime**

Corrected the test fixture lifetime so the backing disk remains alive for the entire adjacent-extent test.

### `39bf5fed4bb94599d1c05b7da021926617fb5506`
**TEST — FSCK sparse layout**

Added FSCK validation after the adjacent sparse extent scenario.

---

# CI / validation history

A known fully green CI run:

### Run `37766536105`
Validated commit:

`10912fa8d8eb4097e3a41a9d07070884f2c411dc`

Covered:

- GCC build/test;
- GCC sanitizers;
- Clang build/test;
- Clang sanitizers;
- Windows/MSVC.

Another known successful run:

### Run `37765875919`
Validated:

`950cf28a81e7d10d573dc0429e492dae208b4cb4`

For newer sparse-seeking commits, the available GitHub workflow/status API currently returned no workflow runs/status entries. Therefore those commits are **not being falsely marked green** in this log.

---

# Latest documented milestone

### `39bf5fed4bb94599d1c05b7da021926617fb5506`

Sparse seeking and its adjacent-extent/FSCK regression coverage were completed through the latest documented sparse milestone.

The roadmap documentation was then expanded so that future development cannot lose track of:

- missing NTFS-class capabilities;
- P0/P1/P2/P3 priorities;
- completion requirements;
- previously fixed corruption/recovery bugs;
- known CI facts.

---

# Remaining P0 work

1. Complete the concurrency audit.
2. Stress all shared runtime state.
3. Finish WAL/transaction fault-injection coverage.
4. Finish FSCK repair architecture.
5. Expand corruption testing.
6. Define/implement stronger data-integrity checks.
7. Add scrub.
8. Validate durability/cache semantics.
9. Add crash tests for sparse writes and truncate.
10. Verify every failure path is either recoverable or explicitly corruption-class.

---

# Remaining P1 work

1. Snapshots.
2. Named streams / ADS.
3. Persistent change journal.
4. Quotas.
5. Finish sparse integration with handles/adapters.
6. Add full cross-feature tests.

---

# Remaining P2 work

1. Compression.
2. Encryption/key architecture.
3. Reparse-like objects.
4. Notifications.
5. Windows-specific metadata/security semantics.

---

# Remaining P3 work

1. Persistent file IDs.
2. Page/file cache.
3. Read-ahead.
4. Write-back.
5. fsync/fdatasync/direct I/O.
6. mmap.
7. Linux VFS.
8. Windows/WinFSP integration.
9. OS notifications.
10. Benchmark suite.
11. Performance optimization based on measured workloads.

---

# Engineering principles learned from previous failures

### Never assume rollback succeeded

Every rollback operation that can fail must be checked.

### Persistent state matters more than memory state

A successful in-memory rollback is not enough if the old disk state was not made durable.

### Corruption is preferable to a false success

If OpenFS cannot guarantee that the old state is recoverable, it must return a corruption-class error.

### FSCK must validate actual ownership

Heuristics such as “refcount >= 2 must be correct” are insufficient.

### Tests must reproduce the real failure

Fault injection should cover:

- write failure;
- flush failure;
- allocation failure;
- journal failure;
- rollback failure;
- corruption;
- crash/restart.

### Do not weaken invariants to make CI green

When a proposed state transition is unsafe, revert it and preserve the stronger invariant.

### CI status must be factual

A commit is only described as CI-green when an actual successful remote run/status was observed.

---

# Next development direction

The next implementation work should continue in this order:

**P0 reliability → complete sparse hardening → snapshots → change journal → named streams → quotas → integrity/scrub → compression/encryption → VFS/OS integration → benchmarks/optimization**

All new work must update this log when a meaningful fix, feature, regression, or validation milestone is completed.
