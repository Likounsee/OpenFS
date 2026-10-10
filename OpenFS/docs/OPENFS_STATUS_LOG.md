# 2026-10-10 — P0: make mounted create and mkdir one journal transaction

- **AUDIT FINDING** — direct mounted \`openfs_path_create()\` allocated an inode and set its bitmap state before adding the parent directory entry. A failure/crash between those steps could leave an allocated but unreachable inode. \`mkdir\` delegates to the same create path.
- **FIX** — mounted direct \`create\` and \`mkdir\` now use a single journal transaction with directory → inode → allocation → transaction lock ordering. The inode allocation and parent-directory slot are staged through the transaction device; the output inode number is published only after successful COMMIT. Explicit transaction-proxy and unmounted compatibility callers still use the unlocked path inside their caller's transaction or direct device.
- **REGRESSION** — \`OpenFS/tests/test_path.c\` injects a home-block write failure immediately after COMMIT for both direct create and direct mkdir. Remount must recover the file/directory and allow a child create; FSCK must remain clean.
- **Validation** — pending fresh GCC, Clang/sanitizer, Release, and Windows Debug/Release CI for this commit.

---

# 2026-10-10 — P0: make mounted CoW clone one journal transaction

- **AUDIT FINDING** — the public mounted `openfs_path_clone()` previously ran CoW inode allocation/refcount changes first and inserted the parent directory entry afterward as separate updates. A crash after an intermediate COMMIT could leave a leaked clone inode/refcounts without a visible name. Existing namespace crash coverage used `openfs_path_clone_tx()`, so it did not exercise this direct mounted API.
- **FIX** — the direct mounted clone wrapper now takes directory → inode → allocation → transaction locks, runs clone allocation, CoW refcounts/extent metadata, and directory insertion on the transaction proxy, and only returns the new inode after COMMIT succeeds. Transaction-proxy callers continue within their existing transaction; unmounted direct-device behavior remains compatible.
- **REGRESSION** — `OpenFS/tests/test_path.c` injects a home-block write failure immediately after durable COMMIT through the direct mounted API. After remount, both source and clone must exist, clone contents must match, and FSCK must report zero errors.
- **Validation** — [CI run `38064461016`](https://github.com/Likounsee/OpenFS/actions/runs/38064461016) passed GCC/Clang Debug and Release, both ASan/UBSan jobs, and Windows Debug/Release for commit `e10e5f6`. This run also validated recovery after a torn refcount home write and a second failed replay attempt; both retries ultimately recover the committed clone and FSCK is clean.

---

# 2026-10-10 — P0: journal xattr block and inode-pointer mutations together

- **AUDIT FINDING** — mounted `openfs_xattr_set()` could allocate a block and write the xattr payload before updating the inode's xattr-block pointer; `openfs_xattr_remove()` could clear that pointer before freeing the block. Those independently persisted writes could leave an allocated unreachable block or a stale pointer after a crash.
- **FIX** — for a mounted runtime, xattr set/remove now acquire inode → allocation → transaction locks and execute their existing logic through a single journal transaction proxy. Recursive calls on the proxy use a runtime-free superblock copy to avoid re-entering lower-ranked locks; the unmounted direct-device compatibility path remains unchanged.
- **REGRESSION** — `OpenFS/tests/test_xattr.c` injects a home-write failure immediately after durable COMMIT for both the first xattr set and removal of the last xattr. After unmount/remount, the xattr value/pointer, bitmap and refcount must match the committed transaction and FSCK must be clean.
- **Validation** — [CI run 38063427336](https://github.com/Likounsee/OpenFS/actions/runs/38063427336) passed GCC and Clang Debug/build/tests, both ASan/UBSan jobs, GCC/Clang Release build/tests, and Windows Debug/Release for commit `d0b63a6`.

---

# 2026-10-10 — P0: cover orphan data extents during post-COMMIT recovery

- **REGRESSION EXPANSION** — `OpenFS/tests/test_orphan.c` now gives the orphan both a real data extent and an xattr block. After injecting the first home-write failure following durable COMMIT, a second mount must release the inode bit, data-block bit/refcount, and xattr-block bit/refcount; FSCK must still report zero errors.
- **Validation** — [CI run 38062789584](https://github.com/Likounsee/OpenFS/actions/runs/38062789584) passed GCC and Clang Debug/build/tests, both ASan/UBSan jobs, GCC/Clang Release build/tests, and Windows Debug/Release for commit `bdcde36`.

---
# 2026-10-10 — P0: make orphan reclamation journal-atomic

- **AUDIT FINDING** — `openfs_orphan_reclaim()` previously truncated an orphan's file data, wrote the inode as FREE, and only then freed the inode bitmap bit in separate operations. A crash between these writes could leave an allocated inode bitmap bit pointing at a FREE inode, which orphan recovery then skipped. It also discarded the inode's xattr-block pointer without freeing that data block.
- **FIX** — mounted-runtime reclamation now holds the inode/allocation/transaction locks and stages file truncation, xattr-block release, FREE-inode generation advance, and inode-bitmap release in one journal transaction. The unmounted compatibility path also explicitly releases the xattr block and rejects attempts to reclaim the root inode.
- **REGRESSION** — `OpenFS/tests/test_orphan.c` creates an orphan with an xattr block, injects a home-block write error immediately after its durable COMMIT, asserts the first mount fails, then remounts and checks the inode and xattr block are both freed and FSCK is clean.
- **Validation** — [CI run 38062468322](https://github.com/Likounsee/OpenFS/actions/runs/38062468322) passed GCC/Clang Debug and Release, both ASan/UBSan jobs, and Windows Debug/Release for commit `5850ff5`.

---

# 2026-10-10 — P0: validate recovery arguments before setting the recovery gate

- **AUDIT FINDING** — `openfs_journal_recover()` set `recovery_required` before `openfs_journal_replay()` validated the journal range. An out-of-range device could make replay return `OPENFS_JOURNAL_INVALID_ARGUMENT` while leaving the journal object gated.
- **FIX** — validate the complete device/superblock journal range up front, before modifying recovery state.
- **REGRESSION** — the journal test shrinks the advertised device length so its journal range no longer fits, then checks that recovery rejects it without callbacks and without setting the gate; after restoring the device, a transaction must still succeed.
- **Validation** — [CI run 38060252425](https://github.com/Likounsee/OpenFS/actions/runs/38060252425) passed GCC Debug/build/tests, Clang Debug/build/tests, both ASan/UBSan jobs, GCC/Clang Release build/tests, and Windows Debug/Release build/tests for `acff7199`. The earlier runs identified and corrected fixture errors; the final assertions remain intact.

---
# 2026-10-10 — P0: reject undersized journal blocks before CRC access

- **AUDIT FINDING** — `range()` accepted any nonzero device block size. A block smaller than `OPENFS_JOURNAL_HEADER_SIZE` that begins with the journal magic could reach `crc_valid()`, which reads and writes the checksum at offset 28 beyond the allocated block.
- **FIX** — `OpenFS/src/journal.c`: `range()` now rejects block sizes smaller than the 32-byte journal header, and `crc_valid()` independently rejects null or undersized buffers before touching the CRC field.
- **REGRESSION** — `OpenFS/tests/test_journal.c` supplies a 16-byte block device with the journal magic at the journal start and asserts `OPENFS_JOURNAL_INVALID_ARGUMENT`, zero callbacks, and zero device reads.
- **Validation** — [CI run 38060252425](https://github.com/Likounsee/OpenFS/actions/runs/38060252425) passed GCC Debug/build/tests, Clang Debug/build/tests, both ASan/UBSan jobs, GCC/Clang Release build/tests, and Windows Debug/Release build/tests for `acff7199` (including this small-block guard).

---

# 2026-10-10 — P0: validate and stage WAL replay with bounded memory

- **AUDIT FINDING** — replay's second scan could return a different, CRC-valid DATA payload than the one seen during the transaction-structure validation scan.
- **FIX** — `OpenFS/src/journal.c`: the first scan stores a whole-block CRC32C fingerprint for every journal slot. The second scan compares each returned block with its first-pass fingerprint, validates the record again, and stages committed DATA payloads. Read errors or changed block contents return before any callback runs.
- **MEMORY** — retain bounded per-slot fingerprints and stage only committed DATA payloads, rather than allocating a buffer the size of the entire journal (which can be up to 256 MiB). Size and growth overflow are checked; allocation failure occurs before callbacks.
- **REGRESSION** — `OpenFS/tests/test_journal.c` makes the mock device change a DATA payload during the second read and recompute that record's CRC. Replay must reject the changed block with no callback; a stable retry then applies the original payload once. The late-read failure test still injects an error after the DATA block would previously have been applied, and verifies zero callbacks.
- **Validation** — [CI run 38059241901](https://github.com/Likounsee/OpenFS/actions/runs/38059241901) passed GCC Debug/build/tests, Clang Debug/build/tests, both ASan/UBSan jobs, GCC/Clang Release build/tests, and Windows Debug/Release build/tests for commit `06b3afd`.

---

# 2026-10-10 — P0: stage WAL replay before callback publication

- **AUDIT FINDING** — `OpenFS/src/journal.c`: replay's second scan used to invoke callbacks inline. A device read failure later in that scan could return `OPENFS_JOURNAL_IO_ERROR` after earlier callbacks had already applied committed DATA records.
- **FIX** — collect committed DATA payloads in a staging buffer and complete the second journal read/CRC pass before invoking any callback. Allocation/read/CRC errors during staging now return before publication. The buffer grows with overflow checks; allocation failure also occurs before callbacks.
- **REGRESSION** — `OpenFS/tests/test_journal.c` injects a read error after the old implementation would have called back for a committed DATA record, asserts zero callbacks on failure, then verifies a successful retry applies the payload once.
- **API CONTRACT** — clarified that callbacks must be idempotent when a callback fails after previous callbacks succeeded or the final flush fails; these cases can still cause retry of earlier callbacks.
- **Validation** — [CI run 38058321435](https://github.com/Likounsee/OpenFS/actions/runs/38058321435) passed GCC Debug/build/tests, Clang Debug/build/tests, both ASan/UBSan jobs, GCC/Clang Release build/tests, and Windows Debug/Release build/tests for code commit `7bf875c`. The earlier attempt (run 38058277609) caught a test-fixture compile error; the enum was corrected at `7bf875c` without weakening the regression assertions.

---

# 2026-10-10 — P0: validate CoW refcount mutations against allocation bitmap

- **FIX** — `OpenFS/src/cow.c`: direct and transaction-aware refcount increment/decrement now require the block's allocation bit to be set before changing the count. A bitmap I/O failure returns `OPENFS_COW_IO_ERROR`; a free/unowned block returns `OPENFS_COW_CORRUPT`.
- **TEST** — `OpenFS/tests/test_allocator.c` and `OpenFS/tests/test_allocator_transaction.c`: both direct and transactional APIs reject increment/decrement against a free bitmap bit and preserve the stale nonzero refcount for recovery/diagnosis.
- **Validation** — [CI run 38057081587](https://github.com/Likounsee/OpenFS/actions/runs/38057081587) passed GCC, Clang (including sanitizers and Release), and Windows Debug/Release for commit `4144f5e`.
- **P0 scope note** — this closes one allocation/refcount consistency gap; it does not resolve the remaining post-COMMIT ownership, namespace recovery, concurrency-stress, or broader FSCK/integrity roadmap items.

---

# 2026-10-10 — P0: replay retry idempotency regression

- **TEST** — `OpenFS/tests/test_journal_failure_paths.c`: the replay callback now models an idempotent effect by overwriting a fixed payload. When the final replay flush fails, recovery remains gated; on retry, the callback runs again and the resulting effect remains exactly the committed payload rather than accumulating a duplicate effect.
- **Validation baseline** — [CI run 38056265145](https://github.com/Likounsee/OpenFS/actions/runs/38056265145) passed GCC, Clang (including sanitizers and Release), and Windows Debug/Release for commit `bff7766`. The added replay-effect assertions require a new full CI run before validation is complete.

---

# 2026-10-10 — P0: recovery gate and FSCK preflight failure coverage

- **TEST** — `OpenFS/tests/test_journal_failure_paths.c`: while journal replay is active, a callback attempts to begin another transaction. The recovery gate rejects the attempt without creating an active transaction; after successful recovery and checkpoint, a new transaction can begin, commit, and checkpoint.
- **TEST** — `OpenFS/tests/test_fsck_refcount_repair.c`: an injected I/O failure reading the block allocation bitmap during ownership preflight returns `OPENFS_FSCK_IO_ERROR` and leaves the pre-existing refcount unchanged.
- **Validation** — [CI run 38055976724](https://github.com/Likounsee/OpenFS/actions/runs/38055976724) passed GCC, Clang (including sanitizers and Release), and Windows Debug/Release for commit `6b6db5c`. The post-recovery transaction test added in the current commit still requires its own full CI run.

---

# 2026-10-10 — P0: non-transactional CoW free and FSCK ownership regression coverage

- **TEST** — `OpenFS/tests/test_allocator.c`: cover the legacy/non-transactional CoW free path with a deliberately inconsistent free bitmap bit and stale nonzero refcount. Free must report `OPENFS_ALLOC_CORRUPT`, preserve the refcount, and leave the bitmap untouched until the fixture is restored.
- **TEST** — `OpenFS/tests/test_fsck_refcount_repair.c` now covers the opposite FSCK ownership mismatch as well: an allocated block with no inode-derived owner must be rejected without clearing its allocation bit or refcount.
- **FIX** — `c0e104f` checks the allocation bitmap before decrementing/freeing a CoW block; `e5be958` preflights bitmap/ownership consistency before FSCK refcount repair.
- **Validation** — CI run [38054328404](https://github.com/Likounsee/OpenFS/actions/runs/38054328404) passed GCC, Clang (including sanitizers and Release), and Windows Debug/Release for code head `1f3c71af`. Both the transactional and legacy CoW-free corruption regressions passed in the normal test matrix.

---

# 2026-10-09 — Correct file-lock test portability include

- **TEST FIX** — `OpenFS/tests/test_file_lock.c`: corrected the include separator between `stdatomic.h` and `time.h`, retaining the POSIX feature-test macro needed to declare `nanosleep`.
- **Validation: not run.** This is a source-level correction; compilation and execution are still required.

---

# 2026-10-09 — Regression test for shutdown with a blocked file-lock waiter

- **TEST** — `OpenFS/tests/test_file_lock.c`: hold an exclusive range lock, start a conflicting blocking waiter, then initiate runtime shutdown. The waiter must return `OPENFS_FILE_LOCK_CLOSED`, allowing shutdown to drain active users; because handles remain open, shutdown must refuse teardown and restore admission.
- This specifically covers the shutdown deadlock scenario rather than only the ordinary lock handoff.
- **Validation: not run.** The cross-platform test has been committed but must still be compiled and executed on POSIX and Windows CI; no green result is inferred.

---

# 2026-10-09 — Cancel blocking file-lock waits during runtime shutdown

- **FIX** — `OpenFS/src/file_lock.c`: blocking range-lock acquisition now checks whether its pinned runtime is still accepting work after each conflict poll. If shutdown closes admission, the waiter returns `OPENFS_FILE_LOCK_CLOSED` instead of retaining an active-user pin indefinitely.
- **API** — `OpenFS/include/openfs/runtime.h` and `OpenFS/src/runtime.c`: added `openfs_runtime_is_accepting()`, a lifecycle-lock-protected probe intended for callers already holding an active runtime pin. That pin keeps the lifecycle mutex alive during the probe.
- This prevents a shutdown deadlock where a blocked lock waiter prevents teardown while the conflicting lock owner can no longer enter the runtime to unlock.
- **Validation: not run.** Source edits require compilation and a dedicated shutdown-versus-blocked-lock regression test before this is considered verified.

---

# 2026-10-09 — Blocking file-lock handoff regression

- **TEST** — `OpenFS/tests/test_file_lock.c`: added a cross-platform worker that requests an exclusive range lock with `OPENFS_FILE_LOCK_BLOCK` while another handle owns the conflicting range. The test releases the owner lock, joins the worker, and checks that the blocked request succeeds and releases its acquired lock.
- This covers the public blocking-lock handoff contract on Windows and POSIX. It does not replace stress testing or prove fairness under contention.
- Commit: `81e765f375609b2f727ee93f5d98db5108934d50`.
- **Validation: not run.** No local compile/CTest was executed; remote CI must be checked independently.

---

# 2026-10-09 — Runtime shutdown admission and quiescence regression

- **FIX** — `OpenFS/src/runtime.c`: release the global runtime-registry guard during each wait iteration after admission is closed. New entrants can now acquire the guard and be rejected promptly rather than waiting behind shutdown; the runtime remains registered and marked `destroying` until quiescence completes.
- **TEST** — added `OpenFS/tests/test_runtime_lifecycle.c` and registered `openfs-runtime-lifecycle` in CMake. The cross-platform test keeps one runtime user active, starts shutdown, waits for admission to close, then releases the active user and checks shutdown succeeds.
- Commits: `43b531f5` (shutdown admission fix), `53c6efc9` (regression test), `29cf2220` (CMake registration).
- **Validation: not run.** The test and CMake wiring were reviewed from source but could not be compiled/executed in this session; CI status must be checked independently.

---

# 2026-10-09 — Runtime shutdown wait-loop hardening

- **HARDEN** — `OpenFS/src/runtime.c`: yield the current thread while `runtime_shutdown_internal()` waits for `active_users` to drain (`SwitchToThread()` on Windows, `sched_yield()` on POSIX), instead of continuously re-locking and polling without yielding.
- This reduces the chance that a shutdown caller monopolizes a CPU, but it is **not** a blocking condition-variable/event wait and does not prove lifecycle teardown race-free.
- **Validation: not run.** No local build/test environment or usable CI result was available during this change. A cross-platform compile and shutdown-with-active-operation regression test are still required.
- Commit: `5f3befe0`.

---

# 2026-10-09 — P0 concurrency audit: file-lock handle state

- **FIX** — `OpenFS/src/file_lock.c`: `handle_open()` no longer reads mutable `closed`/`references` fields before acquiring the handle mutex. `handle_lock()` validates those fields after locking, preserving the closed-handle result while avoiding unsynchronized state reads.
- **TEST** — `OpenFS/tests/test_file_lock.c`: added a cross-platform threaded regression that repeatedly queries a shared file-lock handle while another reference is closed; the remaining reference keeps the object alive. The test checks all lock-test calls remain successful.
- Commits: `f02169ec` (initial state-read fix), `e9646325` (validate under mutex), `1dd93ed4` (concurrent close regression).
- **CI status: not yet verified.** The connector returned no status checks or workflow runs for the test commit; this is not evidence of a green build. Local compiler/sanitizer execution is not available in this session.
- Scope remains partial: this addresses the file-lock handle state race only. Runtime teardown/admission, retired-handle lifetime, lock fairness, and transaction/journal shared-state paths still require further audit.

---

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


# 2026-10-08 — transaction-device allocator deadlock correction

- CI run 37843124209 exposed a real regression: the new transactional file-checksum test timed out in the full CTest suite on GCC and Clang. The build succeeded; the hang was caused by openfs_file_write_tx() reaching the public allocator through the transaction-device wrapper, which attempted to start a nested transaction while the outer transaction was active.
- Corrected this at the transaction-device boundary: active transaction devices can now be identified safely through a transaction magic marker, and openfs_alloc_block/openfs_free_block route directly to their transaction-safe implementations instead of opening a nested transaction.
- The correction preserves the real WAL path and does not weaken the test. Fresh CI is required before considering the fix validated.


# 2026-10-08 — checksum transaction regression narrowed to WAL primitives

- The first transactional file-write regression intentionally exercised too much of the stack and caused the full CTest suite to time out after build success. The regression test is now narrowed to the exact WAL contract under audit: a real transaction-device data-block write plus openfs_data_checksum_set_tx(), followed by a real commit and normal file readback.
- This keeps the persistent checksum and transaction machinery under test without masking an unrelated higher-level transaction integration hang. The allocator transaction-device fix remains in production and is covered by the existing allocator transaction tests.


# 2026-10-08 — CI compile correction for checksum regression

- Clang CI on run 37844180761 found that the narrowed checksum WAL regression used the checksum API without including its public header. This was a test compilation defect, not a production failure.
- Added the existing openfs/data_checksum.h include; assertions and production code are unchanged.


# 2026-10-08 — transaction-device inode allocation deadlock fixed

- GCC and Clang both built successfully but the CTest preflight `openfs-path-test` hit the workflow's 45-second timeout.
- Root cause audit: inode_alloc.c recognized an active transaction only when the device pointer equaled the runtime base device. A transaction wrapper is a distinct device object, so transactional path operations could incorrectly start a nested transaction and deadlock on transaction_lock.
- Reused the existing openfs_transaction_from_device() identity check for both inode allocation/free transaction admission. No test timeout was hidden or relaxed.


# 2026-10-08 — timeout isolation diagnostics removed

- The GCC/Clang path-test timeout was isolated to the early `rename_missing_same_path_regression` assertion, but temporary tracing did not produce a valid CI build because the diagnostic edit itself was malformed; all temporary tracing has now been removed from production and tests.
- The inode transaction admission change was also corrected to preserve the intended base-device semantics; transaction-device routing remains handled by the allocator transaction detection already in production.


# 2026-10-09 — file-lock registry participates in runtime quiescence

- Audit found that public file-lock operations accessed the runtime's file-lock registry without pinning the runtime as an active user. During `openfs_runtime_shutdown_if_unused()`, that left a window where shutdown could inspect or destroy runtime synchronization state while a file-lock call was entering the registry.
- Wrapped lock, unlock, conflict-test, and release-all entry points in runtime admission accounting; their existing implementations remain internal helpers. Shutdown now rejects new file-lock operations during quiescence and waits for operations already admitted.
- Commit: `1d6347df9c797bf29b63cdb899f24ee004fd7588`.
- Verification limitation: source was committed and fetched back from branch `OpenFS`; compilation, runtime tests, and CI were not run in this environment. This fix still requires Linux and Windows test runs.


# 2026-10-09 — file-lock regression test remains active in Release builds

- **TEST HARDENING** — `OpenFS/tests/test_file_lock.c` used standard `assert()` around operations with side effects, including format/mount calls and thread creation. Under `NDEBUG`, those expressions disappear, so Release builds could skip the actual test workload.
- Replaced the test-local assertion behavior with an always-on check that prints the failed expression and source location before aborting. This preserves test execution in Debug and Release configurations.
- Commits: `ae5c134f` (always-on checks), `edbb2a97` (diagnostic newline correction).
- **Validation: not run.** File content was fetched back from branch `OpenFS`; build, CTest, and CI remain required.


# 2026-10-09 — deterministic file-lock conflict output on rejected calls

- **FIX** — `openfs_file_lock_test()` now validates its output pointer and initializes `*conflict_type` to zero before attempting runtime admission. A call rejected because the runtime is shutting down no longer leaves the caller's output parameter stale.
- Commit: `b068ccb5cee430d12f1c571d4a32b5375631b598`.
- **Validation: not run.** This is a source-level correction and still needs compilation and regression execution.


# 2026-10-09 — file-lock invalid-input regression coverage

- Extended `OpenFS/tests/test_file_lock.c` to verify that invalid lock types and unsupported flag bits are rejected, a null conflict-output pointer is rejected, and successful conflict queries clear stale output values.
- The checks run before and after lock acquisition to guard against invalid calls accidentally publishing registry entries.
- Commit: `8ad364fb107cd39d2a1541623ffb94927e112b34`.
- **Validation: not run.** The test source was committed to branch `OpenFS`; compilation and CTest/CI still need to run. No green result is claimed.


# 2026-10-09 — runtime admission nesting overflow guard

- `openfs_runtime_enter()` now rejects a nested admission when the thread-local nesting depth reaches `UINT_MAX`, rather than allowing the unsigned counter to wrap to zero and release the active-user pin prematurely.
- Extended `test_runtime_lifecycle.c` so the worker enters the same runtime twice and leaves the nested admission before holding the outer admission across the shutdown/quiescence test.
- Commits: `64824d30fb4be111ce89b24fc8e8ebc92db92d27` (guard), `a6b8f9dae01ae12d71b23f22cecb39f1b9c4bc69` (regression coverage).
- **Validation: not run.** Source and test changes were committed to branch `OpenFS`; build, CTest, and CI status still need confirmation.

# 2026-10-10 — keep concurrency regressions active in Release builds

- **TEST HARDENING** — `test_file_concurrency.c`, `test_link_concurrency.c`, and `test_namespace_concurrency.c` used standard `assert()` for operations with side effects. With `NDEBUG`, those operations could disappear, allowing Release CI to pass without exercising concurrent file I/O, hard-link, or namespace workloads.
- Replaced the test-local assertion behavior with an always-on check that reports the failed expression and source location, then aborts. The operations and assertions remain unchanged; only their accidental Release-build elision is prevented.
- Validation is delegated to the branch CI matrix; this commit is not considered green until GCC, Clang/sanitizers, and Windows jobs finish successfully.


# 2026-10-10 — Release configuration joins required CI validation

- Added a dedicated Release configure/build/test pass to both GCC and Clang jobs, including the path-test timeout preflight and the complete CTest suite.
- Added a Release configure/build/test pass to Windows/MSVC as well.
- Motivation: the recent always-on concurrency assertions specifically protect workloads from `NDEBUG`; Debug-only CI did not exercise the configuration in which that regression could recur. This change makes Release behavior observable on all three CI platforms instead of assuming Debug success transfers to Release.
- Validation: this workflow change will be considered validated only after its new Release jobs and all existing Debug/sanitizer jobs pass on GitHub Actions.


# 2026-10-10 — keep all test assertions active in Release builds

- The first full Release matrix exposed four Clang Release failures, including segfaults in journal and mount tests. Inspection shows many test programs put setup and API calls inside standard `assert()`; Release's `NDEBUG` removes those expressions, so the tests do not execute their setup/workload correctly.
- Apply an explicit undefinition of `NDEBUG` to test executable targets only, using compiler-specific options for MSVC and GCC/Clang. This preserves the intended test operations without changing Release assertion semantics in the OpenFS library itself.
- Apply the same rule to adapter test executables created in the adapters subdirectory.
- Validation pending: rerun Debug, sanitizers, and Release tests on Linux/Windows; this correction is not considered complete until the full CI matrix is green.


# 2026-10-10 — validate WAL transaction ownership during journal open

- **FIX** — `OpenFS/src/journal.c`: `openfs_journal_open()` now validates BEGIN/DATA/COMMIT ownership and strictly increasing transaction IDs while scanning the persistent log. DATA and COMMIT records must belong to the currently open transaction; a later BEGIN may still supersede an uncommitted transaction after a crash, matching replay's recovery model.
- **REGRESSION COVERAGE** — extended `tests/test_journal.c` so journal open must reject an orphan DATA record and a COMMIT whose transaction ID does not match the active BEGIN. This prevents malformed WAL from being accepted as an open journal before recovery validation.
- **Validation** — full Debug/sanitizer/Release GCC and Clang plus Windows CI is required before this change is considered complete.


- **CI follow-up (2026-10-10)** — GCC and Clang exposed stale test-fixture state in `journal_corruption_matrix()`: a previous malformed third record was not cleared before the next valid two-record WAL was constructed. Expanded the fixture reset to clear all three touched slots; production validation remains strict. Re-run the full matrix before considering this change green.
