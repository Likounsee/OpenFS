# 2026-10-08 — P0 checkpoint: transaction ordering verified in CI

- Run `37838299193` completed green for the current corrected head: GCC, Clang, GCC/Clang sanitizer stages, and Windows all passed.
- The suite now contains 42 tests; the new transaction-order regression passes and verifies the durable COMMIT-before-publication-before-checkpoint sequence at the real block-device boundary.
- P0 is **~92%**: this adds concrete validation to P0.1 but does not close the remaining durable data-integrity and full FSCK-repair architecture gaps.

# 2026-10-08 — P0: transaction ordering regression fixture corrected

- CI found the new ordering test was observing formatter writes before the test's fault-observation window was armed, causing a false failure.
- The fixture now arms observation only after BEGIN; no filesystem behavior or assertion was weakened.
- The CI run also confirmed the remaining 41 existing tests passed before the new regression aborted.

# 2026-10-08 — P0: explicit transaction durability ordering test

- Added `openfs-transaction-order`, a regression that observes the real block-device writes during a transaction commit.
- It asserts the durable COMMIT record is written before any final filesystem publication, and that journal checkpoint clearing occurs only after publication.
- This makes the required `BEGIN/DATA → COMMIT → publication → flush → checkpoint` ordering an executable regression rather than documentation-only behavior.

# 2026-10-08 — CI: replay idempotence + scrub baseline fully validated

- Run `37836444650` is green across GCC, Clang, both sanitizer builds, and Windows.
- The 41-test CTest suite passes, including `openfs-journal-replay-idempotent` and `openfs-scrub`.
- The new coverage is therefore CI-validated, while P0 remains below 100% because the persistent data-checksum/ownership-claim and full FSCK-repair gaps are not yet closed.

# 2026-10-08 — P0: second replay-test defect corrected

- CI exposed a second defect in the regression harness: its callback was trying to infer the target from payload contents rather than carrying the intended target explicitly.
- The test now uses a fixed target in its disk context and a valid 1024-byte journal DATA payload. It directly verifies first replay and repeated replay byte-for-byte.

# 2026-10-08 — P0: replay test CI failure diagnosed and corrected

- CI run 37835860443 built GCC/Clang successfully but both test jobs failed on the new replay regression because the test attempted to put a 4096-byte block-data record into the journal API's single-record payload limit.
- The regression was corrected to use a valid 1024-byte DATA record while preserving the essential property: replay once and replay twice must produce the same recovered bytes.
- The failure was a test defect, not a weakened filesystem invariant; no existing test was removed or relaxed.

# 2026-10-08 — P0: WAL replay idempotence regression added

- Added an end-to-end journal replay regression that constructs a durable BEGIN/DATA/COMMIT sequence and applies the same committed WAL twice.
- The second replay must leave the already recovered block byte-for-byte unchanged; this explicitly covers the repeated-replay property instead of relying only on mount-time replay.
- The scrub baseline and this replay regression are now registered in the normal CTest suite.

# 2026-10-08 — P0: read-only scrub baseline implemented

- Added `openfs_scrub()` as a strictly read-only integrity pass.
- Scrub first executes the complete existing FSCK verifier, preserving all checksum, ownership, extent, namespace, journal, and format checks already enforced there.
- Scrub then reads every physical block in the device, so a latent read failure cannot be reported as a clean filesystem.
- Added regression coverage for a clean image, inode checksum corruption, and injected read failure.
- This is deliberately not counted as completion of the data-checksum requirement: regular data blocks still have no per-block on-disk checksum in the v1.3 format. A future format-integrity change must define and persist such coverage before P0.7 can be marked complete.

# 2026-10-08 — P0 90% checkpoint revalidated

- Cleanup is now validated end-to-end after the allocator transaction test regression: the test closes all owned runtime resources supported by the public API and no temporary diagnostics remain.
- The experimental transaction-device owner routing was reverted rather than weakening the established locking/CoW architecture.
- CI run 37834456826 is green: GCC, Clang, both sanitizer configurations, and Windows all passed.
- P0 remains at ~90%. This milestone is considered reached, but not 100%: the remaining P0 work is deliberately reserved for durable allocation ownership/claim semantics, final namespace/metadata crash-cut interactions, broader FSCK repair coverage, checksums/scrub, and the final concurrency/durability interaction pass.

# 2026-10-08 — P0 validation cleanup after transaction-device audit

- The explicit transaction-device routing experiment was deliberately reverted after CI exposed that the existing file transaction path has additional CoW/extent interactions that must be integrated as one design rather than incrementally bypassing the established locks.
- The allocator transaction regression was restored to its focused contract (allocation abort/commit) and now closes its journal, unmounts the test filesystem, and frees its backing image so sanitizer runs do not report leaked mount/journal resources.
- No temporary diagnostics remain in the tree.
- The branch remains on the previously validated P0 milestone: **~90%**. The remaining 10% is intentionally reserved for durable allocation ownership/claim semantics, final namespace/metadata crash-cut interactions, broader FSCK repair coverage, checksums/scrub, and the final concurrency/durability pass.

### Validation

A previous 39/39 run was green before this audit experiment. The current head is being revalidated after cleanup; no failure is being treated as resolved until CI reports green again.

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


# 2026-10-08 — P0 sparse/concurrency validation

- `ec3a69c0`: fixed a real sparse-file invariant bug in `openfs_inode_validate()`. The validator incorrectly required the first extent to start at logical block 0, which rejected valid files whose first allocation is intentionally preceded by a hole.
- The failure was exposed by the 8-worker file-concurrency stress test under Clang/GCC ASan/UBSan: writes such as a first allocation at logical block 3 returned `OPENFS_FILE_CORRUPT` even though the sparse extent layout was valid.
- `539d6663`: corrected the sparse allocation fault-injection test to target the refcount block, exercising an actual allocation-publication failure while allowing rollback to complete instead of accidentally failing the rollback bitmap write.
- `d3adef33`, `128c505f`, `4b386e1a`: removed temporary diagnostics after the failure was isolated; the concurrency test is back to clean assertions.

### CI

Run `37821272250` for `d2a98009739f6c9260647f0d82c280c9e88a5ff0` completed **green**: GCC, Clang, both sanitizer passes, and Windows all passed.

### P0 checkpoint

The latest validated suite is 39/39 tests passing across GCC, Clang + ASan/UBSan, and Windows. Snapshot implementation remains blocked as designed: live inode-table, directory, extent-tree and xattr metadata are still not redirected through the persistent metadata root.
# 2026-10-08 — P0 WAL replay retry validation

- `a7f43a93` aligned FSCK sparse-extent validation with the inode validator: a valid leading sparse hole no longer causes a false corruption report.
- `1be37d5c` added a persisted leading-hole regression: namespace-created file, first allocation at logical block 3, FSCK validation, unlink, and second FSCK.
- `54af693c` added a P0 recovery regression for a committed transaction whose final publication fails: the first remount is deliberately forced to fail during WAL replay, then a second remount must retry the same committed DATA and recover successfully.
- The first version of that regression exposed a test-injection mistake: one write fault was consumed by the original post-COMMIT publication failure, so the replay was not actually faulted. `ac725df8` reserves a second exact write failure and now validates the intended replay-failure/retry path.
- CI run `37826013502` for `ac725df8` completed **green**: GCC, Clang, both sanitizer passes, and Windows.

### P0 checkpoint

Estimated P0 completion: **~80%**.

The WAL/recovery matrix is stronger, including a real replay failure followed by retry. Remaining P0 blockers are still allocation ownership/reservation semantics, broader crash-cut coverage around allocator/metadata publication, transactional FSCK repair guarantees, and data-integrity checksums/scrub. Snapshots remain blocked until the persistent metadata-root is wired into live metadata paths.
# 2026-10-08 — P0 transactional FSCK bitmap repair

- `1020aa5f` changed mounted `openfs_fsck_repair_bitmap_tails()` to stage both bitmap repairs through the existing transaction/WAL device when a live runtime journal is available.
- The repair now commits block-bitmap and inode-bitmap tail corrections as one transaction instead of exposing a crash window between the two writes.
- `611d23cb` prevents the legacy direct rollback path from running after a transactional commit has started; a post-COMMIT failure is treated as recovery-required/corruption rather than silently overwriting a durable WAL decision.
- `023f414e` added a mounted regression that faults the second WAL DATA record, verifies that neither bitmap is partially repaired, then retries successfully.
- CI run `37826757878` for `023f414e` completed **green**: GCC, Clang, both sanitizer passes, and Windows.

### P0 checkpoint

Estimated P0 completion: **~81%**.

This closes a concrete crash-consistency hole in FSCK repair. Remaining major P0 work is durable allocation ownership/claim semantics, broader allocator + metadata crash-cut coverage, repair coverage beyond bitmap tails, and data-block integrity checksums/scrub.
# 2026-10-08 — P0 transaction-aware allocation groundwork

- `7e8c834d` exposes `openfs_alloc_block_tx()` and `openfs_free_block_tx()` so allocation/refcount publication can participate in an already-open transaction without creating a nested WAL transaction.
- `1a6fde95` routes the existing mounted allocator path through those primitives while preserving the current public behavior.
- `9bc05c1f` adds commit/abort coverage: allocation inside an explicit transaction disappears on abort and becomes durable on commit, followed by normal reclamation.

### P0 checkpoint

Estimated P0 completion: **~82%**.

This is groundwork for closing the allocator-to-metadata publication gap: future file/namespace mutations can now reserve and publish allocation in the same transaction. It is not yet the durable ownership/claim ledger itself, so orphaned post-allocation/pre-publication crash semantics remain an open P0 item.

CI run `37827572569` for `9bc05c1f` completed **green**: GCC, Clang, both sanitizer passes, and Windows.

# 2026-10-08 — P0: atomic file transactions and journal capacity

- **Transaction-device ownership routing** (`9b993652`, `a9b72d64`, `833f806e`) now lets allocator calls made through an explicit transaction device use the already-open WAL transaction instead of opening a nested transaction. The owner fast path also avoids recursively taking the allocation mutex while the transaction-aware allocator is already executing under its caller's transaction context.
- **Explicit file transaction APIs** (`6d50d902`) now execute directly against the staged transaction device instead of re-entering the public mounted-file wrapper. This preserves the caller's staged inode view and allows data-block allocation, CoW refcount changes, payload writes and inode publication to remain in one transaction.
- **Journal sizing** (`c9ac886c`) was increased for newly formatted images: the default journal reservation is now `total/8` with a 12-block minimum. A 4096-byte journal DATA target currently consumes two OJBD records, so the previous 8-block journal could not hold a realistic multi-block file transaction (BEGIN + bitmap + refcount + data + inode + COMMIT). Existing on-disk journals remain valid; the new geometry gives formatted images enough headroom for these atomic mutations.
- `41d67ba0` adds regression coverage proving an explicit file write can allocate a data block and then abort without publishing the allocation or inode mutation.
- `af571a5d` adds a crash-style regression: force the first base-device publication after a durable COMMIT to fail, abort the in-memory transaction as recovery-required, remount, and verify that WAL replay restores the inode, allocation state and exact 4 KiB payload.
- The allocator transaction regression remains registered as `openfs-allocator-transaction` and now covers both allocation abort/commit semantics and file-level atomic/recovery behavior.

### CI

Run **37832156724** for `a20f2cab7a736949b99cf723cb8bdb77948c8939` completed **green**: GCC, Clang, and Windows all passed.

### P0 checkpoint

Estimated P0 completion: **~90%**.

The remaining P0 work is now concentrated on final crash-cut coverage across namespace/metadata interactions, durable ownership/claim semantics for committed-but-not-yet-published metadata, broader FSCK repair coverage, integrity checksums/scrub, and a final concurrency/durability interaction pass. Snapshot implementation remains blocked until the persistent metadata root is wired into all live metadata paths.
# 2026-10-08 — P0 on-disk format hardening

- `dc2e369e` hardens superblock decoding: the reserved serialized region (bytes 196..4087) must remain zero even when an attacker recomputes the superblock CRC. This closes a format-validation gap where unknown future fields could otherwise be silently accepted as a valid current layout.
- `53c8bb79` adds a regression that mutates a reserved superblock byte, recomputes the CRC, and requires `openfs_read_superblock()` to reject the image.

### P0 checkpoint

This closes one remaining on-disk format-validation invariant. It does **not** close the larger P0 ownership/refcount-repair and persistent user-data integrity gaps; no artificial 100% claim is made.
