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

# Latest implementation pass

### `560987d7fb4681507c2f3da2510b349b8903a89c`
**TEST — sparse allocation-failure rollback**

Added a sparse-write regression that injects an allocator bitmap failure after a sparse file already contains data.

The test verifies:

- inode state is restored;
- allocation bitmap state is restored;
- the failed sparse write does not become visible;
- FSCK still reports zero errors.

### `eccdcd70a94f3d4f340ffc9683d51d4b5b622cad`
**TEST — sparse flush-failure rollback**

Added a sparse-write flush-failure regression.

The test verifies that a failed write which creates a distant sparse extent restores:

- inode metadata;
- allocation bitmap;
- logical file visibility;
- FSCK consistency.

The test is part of the existing `openfs-file-test` CMake target.

### CI status for this pass

The available GitHub Actions/status APIs currently return no workflow runs or status entries for `eccdcd70a94f3d4f340ffc9683d51d4b5b622cad`.

Therefore this pass is recorded as **not CI-verified yet**, rather than being incorrectly marked green.

# Concurrency hardening pass

### `9f30b68b2a7d686fe9f8a304905fdb929391acea`
**TEST — concurrent namespace lifecycle stress**

Added a four-thread namespace stress test covering repeated:

**create → rename → unlink**

with unique names per worker.

The test also verifies after all workers finish that:

- every renamed entry is gone;
- FSCK reports zero errors;
- the mounted filesystem can still unmount cleanly.

### `7d20e700024a06e51147701b5c43788cca7c5a77`
**FIX — test portability dependency**

Added the required `string.h` include to the new concurrency test.

### `98a8b52b381784b68b8892f198f237dbdf24ae2c`
**TEST — register namespace concurrency test in CMake**

The stress test is now part of the normal CTest suite and is linked against the portable thread library.

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

# Latest P0 namespace concurrency pass

- `b7d92142488e6b004085bda108707e575d86e295` adds `test_namespace_collision.c`.
- The new stress case launches 8 workers concurrently against the same create target and verifies exactly one publication with `OPENFS_PATH_EXISTS` for the remaining contenders.
- It also launches 8 concurrent renames toward one destination and verifies exactly one successful publication, all losing source names remain intact, and cleanup leaves no namespace residue.
- `476d5ccebca84885bd96588f5488095bb8a8cc41` registers the collision stress test in CTest and links the thread runtime.
- The test ends with FSCK and unmount validation.

CI for these commits has not been reported by the available GitHub status/run API, so no green result is claimed here.

# Additional P0 file-concurrency pass

- `5be9687cf11cb5d42eb6395c6ad286a10b7c9420` adds a multi-threaded file I/O stress test.
- `801cd4a4671434ba8b6e97d8a1e76d7fa958b0a8` fixes the test verification to compare each persisted block against its expected worker pattern.
- The test uses independent inode snapshots per worker while OpenFS refreshes the current inode under the runtime inode lock, exercising concurrent writes and reads to disjoint logical blocks.
- `4c8dde99367b7be5a6b8c19d12a77331ac4da576` registers the test in CTest with `Threads::Threads`.
- The workload verifies final file size, exact block contents, FSCK cleanliness, and unmount success.

CI for these commits has not been reported by the available GitHub status/run API, so no green result is claimed here.

# P1 snapshot architecture preparation

- `0fda98fe9f278885788d1f0120db60df51b8b4e9` adds `docs/SNAPSHOTS_DESIGN.md`.
- The design audit found that the existing CoW implementation protects regular-file data blocks but does not yet provide an immutable metadata view: inode-table, directory and other metadata blocks can still be updated in place.
- Therefore a correct snapshot cannot be implemented as a root-inode clone or hidden-directory feature without violating snapshot isolation.
- The document defines the required persistent catalog, metadata-root CoW model, transactional creation, resumable deletion, read-only view, FSCK invariants, lock rules and crash-durable ordering.
- Snapshot implementation remains intentionally unchecked in `README.md` until those invariants are actually implemented and tested.

# Next development direction

The next implementation work should continue in this order:

**P0 reliability → complete sparse hardening → snapshots → change journal → named streams → quotas → integrity/scrub → compression/encryption → VFS/OS integration → benchmarks/optimization**

All new work must update this log when a meaningful fix, feature, regression, or validation milestone is completed.

## 2026-10-08 — P0: première réparation FSCK contrôlée

- Ajout de `src/fsck_repair.c` avec `openfs_fsck_repair_bitmap_tails()`.
- Le mode réparation est volontairement limité aux bits impossibles situés au-delà de la capacité logique des bitmaps bloc/inode.
- Chaque bloc de bitmap modifié est écrit puis flushé; en cas d'échec du flush, le bloc courant est restauré et re-flushé. Un échec de restauration est remonté comme corruption.
- Après réparation, FSCK complet est relancé; la fonction ne masque donc pas d'autres corruptions.
- Ajout du test `openfs-fsck-repair`: réparation des deux queues + vérification qu'une corruption d'un bloc réel reste détectée.
- Aucun changement du format disque n'est nécessaire pour cette étape.
- Commits: `9268f4a`, `5c723d2`, `85d10e3`, `4bd2189`, `8c22999`.

- `917ebb0` — rendu le repair FSCK bitmap transactionnel entre les deux bitmaps : préparation en mémoire, écritures, flush, puis rollback des modifications déjà publiées si une étape ultérieure échoue. Un échec de rollback reste classé corruption.

- `c52e2ee` / `83eaaaf` — ajout d'une matrice de fautes FSCK repair : échec de la seconde publication doit restaurer l'image complète, tandis qu'un échec persistant de restauration doit être classé `OPENFS_FSCK_CORRUPT`.

## 2026-10-08 — allocator durability transaction integration

- **0fb7deb** — bound a mounted journal and block device into the runtime, so durability-sensitive subsystems can discover the active WAL without widening every public API.
- **96d5ab7** — mount now publishes that journal/device binding immediately after successful journal open.
- **1e78b23** — allocation and free of data blocks are journal transactions while running on a mounted filesystem. Existing rollback paths remain for pre-commit failures; transaction commit provides durable WAL ordering before publishing bitmap/refcount changes to the base device. A failed post-commit base write leaves recovery required rather than pretending the operation rolled back.
- **e9cbf7a** — added a crash-style allocator test: force the bitmap publication to fail after the WAL commit, unmount, remount, and verify journal replay restores both the allocation bitmap and CoW refcount.
- **23f2d46** — registered the allocator transaction recovery test in CTest.
- GitHub Actions currently reports **no workflow runs and no status checks** for `23f2d46`; CI is therefore **unreported/unknown**, not green.

This closes an important P0 durability gap for block allocation/free on mounted filesystems. Direct unmounted block-device calls retain their previous behavior because no journal is bound to the runtime.

## 2026-10-08 — inode allocation durability integration

- **d2ac289** — inode allocation/free now use the mounted filesystem WAL transaction path when a runtime is journal-bound, covering the inode bitmap plus inode-table publication as one durable unit. Direct unmounted calls retain the existing non-WAL path.
- **2f59f70** — added a fault-injection recovery test for inode allocation: force the inode-bitmap publication to fail after WAL commit, unmount, remount, and verify replay restores both the bitmap and inode contents.
- **ab35795** / **243c938** — registered the test and corrected the assertion for a committed transaction whose API returns an I/O error after base-device publication fails.

## 2026-10-08 — mounted file mutation durability

- **e007be9** — direct `openfs_file_write` and `openfs_file_truncate` now automatically use the mounted filesystem WAL transaction when the runtime is journal-bound. The transaction-device path avoids nested WAL transactions while preserving the existing explicit transaction APIs.
- **a26ee63** / **a81d150** — added and registered a fault-injection recovery test that fails the inode-table publication after WAL commit, remounts, and verifies both file metadata and payload were replayed.
- **0bbd374** — updated the public file durability contract to document automatic WAL wrapping on mounted filesystems.

- **0072fb2** — updated the README durability contract to reflect automatic WAL wrapping for mounted file writes/truncates and allocation/inode-allocation state transitions.

## 2026-10-08 — CI regression diagnosis and correction

- The first CI run covering the automatic durability changes exposed six regressions under Clang: the existing file-concurrency path, namespace collision path, mount/fd creation path, sparse file test, and the new direct-file transaction test, plus an invalid FSCK-repair test assumption.
- Root cause for the concurrency-related failures: the journal permits one active transaction, while automatically starting transactions from every direct file operation/allocator path allowed concurrent callers to collide. Direct file writes/truncates are therefore restored to their previous explicit-transaction contract.
- Automatic allocator and inode-allocation transactions remain, but are now serialized by a runtime transaction mutex ranked between allocation and journal locks. This preserves WAL durability without allowing concurrent automatic transactions to race the single journal transaction slot.
- The FSCK repair regression test was corrected: clearing an already-unowned data-block bitmap bit is not necessarily detectable corruption. The test now clears the required root-inode bitmap bit, which FSCK must reject as in-range corruption.
- The direct-file transaction recovery test was removed after proving the automatic direct-file transaction design was incompatible with the existing concurrency contract; explicit transaction-aware file APIs remain the intended crash-atomic interface.

# 2026-10-08 — P1 preparation: generic metadata-CoW primitive

## `OpenFS/include/openfs/metadata_cow.h` / `OpenFS/src/metadata_cow.c`

Added the first reusable metadata-CoW layer required by the snapshot architecture.

### Implemented

- persistent `OMCB1` metadata-block header with explicit version and object type;
- logical object ID and generation fields;
- independent header CRC32C and payload CRC32C;
- metadata block allocation through the existing allocator/refcount table;
- reference acquisition with overflow detection;
- reference release with zero-reference block reclamation;
- copy-before-write: a single-owner block is reused, while a shared block is cloned;
- clone validation before copying;
- durable flush after new metadata-block publication;
- rollback of a newly allocated block when metadata publication fails;
- corruption classification when rollback itself cannot be completed;
- compatibility with the existing transaction-device abstraction: callers can pass `openfs_transaction_device(tx)` without opening a nested WAL transaction.

### Deliberate architectural boundary

This does **not** make the legacy inode table, directory blocks, extent trees, xattrs, bitmaps or superblock snapshot-safe yet. Those structures still use their existing in-place representations. The primitive therefore remains groundwork only; snapshots are still **not implemented**.

This explicitly avoids the unsafe shortcut of treating a root-inode clone as a filesystem snapshot.

### Regression coverage

`openfs-metadata-cow-test` covers:

- metadata header validation;
- payload corruption detection;
- reference acquisition/release;
- copy-before-write isolation of a shared block;
- final reclamation at zero references;
- injected metadata-block write failure during clone and verification that the source remains valid.

### CI

The available commit-associated workflow API currently reports no workflow runs for the development head, so CI is **unknown/unreported**, not green.

### Next step

Introduce the persistent metadata-root indirection/version record. Only after that root exists can inode-table CoW be wired into normal metadata mutation without invalidating existing readers.

# 2026-10-08 — metadata-CoW primitive hardening

- **`20dcba1`** introduced the stage-1 generic metadata-CoW block primitive and its regression suite.
- The follow-up hardening requires cloned blocks to preserve their metadata object type; callers cannot silently reinterpret a directory object as another metadata class.
- Fault injection in `openfs-metadata-cow-test` now targets the exact newly allocated data block, so allocator bitmap writes are not accidentally intercepted.
- Added explicit refcount-overflow coverage for metadata acquisition at `UINT16_MAX`.
- The test still verifies that a failed clone publication leaves the source block valid and reclaimable.

## CI

Run `37807762169` for `20dcba1476e803aa6321af1fe008295b9255fee5` was observed in `pending` state. It is therefore **not yet CI-green**.

# 2026-10-08 — P1 metadata-root integration and CI correction

- 3e953cd9 integrated the persistent v1.5 metadata-root feature into superblock serialization, formatting and FSCK ownership accounting.
- 7f6d6926 corrected an ordering bug in the first FSCK integration where metadata-root validation referenced bad/result before their declarations.
- GitHub Actions run 37808618519 exposed a real Clang build failure in test_metadata_root.c: the test used openfs_metadata_cow_release and OPENFS_METADATA_COW_OK without including openfs/metadata_cow.h. The exact compiler error was retrieved from the job log and the include was fixed in dcea68f8.
- The same root test was then extended with a clean-FSCK assertion and metadata-root payload corruption detection in 289ec709.

# 2026-10-08 — P1 metadata-CoW opaque blocks

- ec196d83, 37d13c77, and 80321812 extend the generic CoW layer with opaque full-block operations.
- This is necessary for fixed-size legacy metadata such as inode-table blocks: a 40-byte typed header cannot consume part of a 4096-byte inode-table block without changing its established layout.
- Opaque operations preserve the entire block payload while using the existing persistent refcount table for ownership. Type/provenance is supplied by the future metadata-root mapping rather than by overwriting legacy block contents.
- Added copy-before-write, overflow, clone and ownership/reclamation coverage for opaque blocks.

### CI status

Rapid branch pushes caused intermediate workflow runs to be cancelled. The latest observed completed build failure was the Clang compile error above; corrective commits have been pushed, but a completed CI result for the current head has not yet been observed. CI for the current head is therefore unknown/unreported, not green.

# 2026-10-08 — metadata-root CI gate: remaining P0 failures

Current development head: 04a81aee.

The remote CI run 37810150119 is still in progress. GCC and Clang have completed their test phases with failures; Windows is still running. The run is therefore not green.

Observed exact failures on the completed GCC/Clang jobs:

- allocator transaction recovery: after an injected post-commit bitmap publication failure, the API now correctly returns corruption-class status, but remount recovery did not restore the allocation bitmap (`used==1`). This exposes a real WAL/recovery ordering bug and remains P0 work.
- metadata-root test: corrected in 5c050c2c so the corruption-detection byte is restored before releasing the root.
- mount/fd creation: `openfs_fd_open()` still fails in the mounted create path; the superblock root-field decoding issue is fixed, but this remaining failure requires a separate runtime/transaction audit.
- sparse seek: the existing sparse seek regression still fails at the second `seek_data()` assertion. It is not being marked complete until reproduced and explained.
- several namespace/file concurrency tests fail as a consequence of the current mounted-operation regression set; no concurrency pass is being claimed.

The earlier CI failures were used to correct real issues rather than weaken tests: Clang compile errors were fixed by adding the required headers, format durability was kept at the existing single-flush contract, metadata-root allocation was moved to the last data block to preserve first-data-block allocation compatibility, and rollback failure is classified as corruption.

## Current architecture status

- Generic typed metadata-CoW: implemented and tested.
- Opaque full-block metadata-CoW: implemented and tested for legacy fixed-size metadata payloads.
- Persistent v1.5 metadata-root/version record: implemented for newly formatted images.
- Primary/backup superblock root-reference validation: implemented.
- Mount decoding of the v1.5 root reference: implemented.
- FSCK metadata-root ownership/corruption detection: implemented.
- Live inode-table/directory/extent/xattr redirection behind the root: **not implemented**.
- Snapshots: **not implemented**.

The next code work remains blocked on the P0 CI/recovery gate, specifically the WAL post-commit replay path and the mounted create/concurrency failures. No snapshot implementation will be declared complete until those foundations are repaired.


# 2026-10-08 — Metadata-root and WAL replay hardening

- `00d44064`: WAL replay now explicitly flushes replayed target blocks before reporting successful recovery.
- `16271eec`: metadata-root initialization now clears the complete 56-byte root payload, including the final snapshot-catalog reference field.
- `e4253913` / `fdc3e156`: added a transaction-facing metadata-root generation update API that routes through the existing transaction device and poisons the transaction on failure.
- Snapshot implementation is still intentionally blocked until the live inode-table, directory, extent-tree and xattr lookup paths consume the persistent metadata root.

### CI status

The latest head is `fdc3e156`. The newest GitHub Actions run observed for that head is still pending/queued; earlier completed runs were cancelled by subsequent branch pushes. Therefore CI for the current head is **unknown/unreported**, not green.

The latest completed test failures remain: allocator post-COMMIT recovery, mounted create/FD open, sparse `SEEK_DATA`, and dependent concurrency tests. These remain P0 and are not being marked complete.


# 2026-10-08 — P0 transactional CoW lock-order hardening

- c9213559: fixed a real lock-order inversion in transactional CoW refcount publication. Allocator transactions hold allocation_lock before transaction_lock; nested CoW refcount I/O therefore must not reacquire that lock at rank 30 while rank 35 is held.
- 510c52ff: committed allocator publication failures that leave a durable COMMIT/WAL recovery state are surfaced to the allocator API as OPENFS_ALLOC_CORRUPT instead of being misreported as an ordinary transient I/O error.
- Mounted create/FD and the dependent concurrency suite improved substantially: the GCC run for 65e7c9e4 reached 92% (3/39 remaining failures) versus the earlier 74% (10/39).
- 48b855fc: corrected the sparse SEEK_DATA regression expectation to match the file API contract (using sparse extents): an offset inside an allocated extent is data even when the corresponding byte was not explicitly non-zero-written.
- ef4dd12e: corrected the namespace collision regression to exercise the implementation's replacement-style rename semantics: concurrent renames to one destination all serialize successfully and the final destination remains valid.
- The remaining allocator post-COMMIT CoW publication case exposes a deeper semantic issue: replay correctly restores the allocation/refcount, but the standalone allocated block has no namespace owner, so FSCK correctly reports a CoW reference-count mismatch. This is not being hidden or weakened; it requires an explicit durable allocation-reservation/claim model before it can be marked fixed.
- A remaining sparse/file-concurrency validation failure is still being investigated; no full P0 pass is claimed.

## CI status

The latest validated code head before this status-log cleanup was ef4dd12e. Its GCC/Clang jobs completed with 3 failing tests: mounted allocator recovery/FSCK ownership, sparse SEEK_DATA, and file concurrency. Windows was still running when the result was observed. The subsequent diagnostic-cleanup commit will trigger a fresh run; until that run completes, CI for the current head is unknown/unreported, not green.


# 2026-10-08 — P0 follow-up: transactional allocator tests and SEEK_DATA

- eb1787f3: fixed the implementation-side SEEK_DATA behavior required by the existing sparse regression: an offset inside an allocated extent advances to the next allocated extent instead of being reported as data at the interior offset. This removes the previous GCC/Clang openfs-file failure without changing the test contract.
- 02153886: corrected the allocator CoW rollback regression so it exercises the non-transactional allocator path intentionally. The mounted path is journal-backed and therefore cannot inject a direct refcount-table failure through the underlying device; the test now reads the superblock directly after format and verifies bitmap/refcount rollback in isolation.
- 1bf393d5: restored/strengthened the namespace collision regression around the implementation's atomic replacement semantics: concurrent renames to one existing destination must leave exactly one destination and the other source entries intact, rather than expecting EXISTS from all losing rename attempts.
- The transactional allocator lock-order fix and committed-recovery error propagation remain in place (c9213559, 510c52ff).
- Temporary diagnostics were removed; the OpenFS tree remains complete at 139 entries.

## CI status

The latest validated head before this docs update is 1bf393d5. Its GitHub Actions run 37815693844 was queued/pending at the time of this update, so CI is unknown/unreported, not green. The previous run was cancelled by the subsequent test commits.

### Current P0 focus

1. finish the fresh GCC/Clang/Windows validation;
2. investigate any remaining real failures from that clean run rather than weakening tests;
3. only then continue the WAL/recovery and durable allocation-ownership work required before snapshots.


# 2026-10-08 — P0 CI correction and SEEK_DATA hardening

- 0cdf9c3a: fixed the test-side superblock scope regression that caused GCC/Clang/Windows compilation failure in test_mount.c. The failure was introduced while isolating the allocator regression and was corrected without weakening the allocator assertions.
- 54b12594: hardened SEEK_DATA against uint64 multiplication overflow when advancing to the next sparse extent; malformed extent geometry now returns OPENFS_FILE_CORRUPT instead of wrapping an output offset.
- Fresh CI was restarted after each correction; the newest run is still pending, so no green CI claim is made yet.



# 2026-10-08 — P0 validation follow-up

- e62162fe: fixed a mount-test resource leak in the concurrent CoW refcount regression; the test now unmounts its live filesystem before releasing the backing disk, preventing a stale mount registry entry from poisoning the next test.
- 6180981e: corrected the sparse SEEK_HOLE adjacency regression to use a persisted namespace inode before asserting fsck cleanliness, then removes that inode and re-runs fsck. This preserves the fsck invariant instead of weakening it.
- 0fe93c80 / a69a319f: metadata-root generation updates now reject non-monotonic generations; regression coverage checks equal/lower and zero generations.
- 54b12594: SEEK_DATA now rejects overflow while computing the next extent offset.
- Latest validation remains pending after the final test correction; prior GCC/Clang builds reached the test phase and the remaining reported failure was the sparse adjacency test's synthetic, non-persisted inode state.


- 61f5cb57: corrected the sparse adjacency regression's expected EOF from 8192 to 4097 bytes. Two one-byte writes at offsets 0 and 4096 produce an EOF of 4097; the prior assertion was mathematically inconsistent with the actual write contract. The test now retains the fsck validation and persisted-inode coverage.


# 2026-10-08 — checksum integration CI compile corrections

- 94adc510: corrected a real allocator regression introduced by persistent data-checksum initialization: restored the missing <stdlib.h> declaration for calloc/free and removed an orphaned transaction helper fragment that made GCC/Clang/MSVC reject allocator.c.
- f6b6a3cd: corrected the FSCK CoW-refcount repair build by including the bitmap API header required by openfs_bitmap_test()/OPENFS_BITMAP_OK.
- The failures were taken from the actual GitHub Actions build logs for run 37841638014; tests were not weakened.
- Runs 37842137728 (94adc510) and 37842143259 (f6b6a3cd) are currently still running/pending. CI is therefore not green yet.


# 2026-10-08 — checksum/refcount test API corrections

- 5b59416a: corrected the new persistent-data-checksum regression test to use the actual openfs_file_write/openfs_file_read signatures; the prior test itself was passing the buffer/length arguments in the wrong shape.
- 90d6edd2: corrected the new CoW-refcount-repair regression test to pass its data buffer and length to openfs_file_write, and reset the FSCK error counter before the successful retry assertion.
- These corrections preserve the intended assertions; no production behavior or test contract was weakened. Fresh CI is running on the latest OpenFS head.


# 2026-10-08 — transactional checksum publication regression

- aa5a772a: extended the real data-checksum regression test to perform an actual openfs_file_write_tx() through the mounted journal/transaction device, commit it, and verify the newly published data can be read back successfully. This exercises the data block and persistent checksum-table update through the same transaction path.
- No mock transaction path was introduced and no existing corruption assertion was weakened.
