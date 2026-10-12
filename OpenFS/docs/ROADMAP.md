# OpenFS roadmap — USB-first delivery plan

Last reviewed: 2026-10-12  
Authoritative branch: `OpenFS`  
Planning baseline: `baae2f1f2fab9dcdaece8106b4bb2ab55a2895b0`  
Primary product goal: a user can safely format a standard USB flash drive as OpenFS, then create, read, write, rename, and delete files through a supported operating system.

OpenFS defines its own filesystem format. It is **not** an NTFS implementation and must not claim NTFS compatibility. Formatting a device is destructive. Image-backed tests do not establish physical-device durability or safe USB handling.

## How progress is measured

Percentages below are planning estimates against the acceptance gates, not code-line counts or release-readiness scores. A phase advances only when its listed evidence exists. Distinguish implementation, static inspection, tests added, tests executed, and behavior verified. Do not mark a phase complete because code merely exists.

The weighted estimate uses these phase weights: P0 10%, P1 15%, P2 20%, P3 10%, P4 20%, P5 10%, P6 10%, P7 5%.

## Current snapshot

- Current implementation batch adds `validate_allocation_ownership`, which checks a caller-supplied ownership inventory against every usable allocation bit and detects duplicate references, orphan allocations, dangling references, and out-of-range blocks. Extent maps now support range removal/truncation that returns physical runs for release, physical-block iteration, and atomic in-memory insertion (failed inserts leave the map unchanged). Code commit `4c3d41a7dda77f3cae3a5f107c667249ac3cb3a1` passed formatting, workspace tests, and Clippy on Linux and Windows; the follow-up extent-insertion/iteration code at `6df1c76d2e81e9b8aa5a1a19bfb5fa4f95cf3ff6` is undergoing CI. Persistent metadata discovery and atomic data/metadata/free transactions are still missing.
- GitHub Actions run [38248099907](https://github.com/Likounsee/OpenFS/actions/runs/38248099907) passed for code commit `e8fd769707f6fc24c5558f0bfee324ccfa1a072b` on Linux and Windows, including formatting, workspace tests, and Clippy. Commit `a33527496cf5bd21b819bebe7c44c22aff74539d` adds journal-replay coverage for padding corruption in the second bitmap segment and its CI passed on Linux and Windows. Commit `fa635c02ed3a27ea9ca3ede2735e1ee09f67f62d` expands replay checks to every reserved bitmap bit for all five supported block sizes; CI run [38248670346](https://github.com/Likounsee/OpenFS/actions/runs/38248670346) passed on Linux and Windows. Commit `0bbdb2a67429c51a2211182e18e57324e4a79e19` extends the replay matrix to padding bits across those five sizes, but CI runs [38249300853](https://github.com/Likounsee/OpenFS/actions/runs/38249300853) and [38249387504](https://github.com/Likounsee/OpenFS/actions/runs/38249387504) failed at formatting only. Commit `79213fb40a9fe0a482bb800da60c29efb9cba65d` applies the rustfmt-requested line wrap; run [38249481025](https://github.com/Likounsee/OpenFS/actions/runs/38249481025) passed on Linux and Windows after the formatting fix; the current branch CI is being rerun for the added bitmap-read failure matrix.
- Recent regressions cover allocator exhaustion/reuse without journal advancement on failed allocation, every reserved-block free rejection, first and last bitmap padding bits across all five supported block sizes, later-segment reserved/padding corruption and journal replay (both reserved-bit and reserved-plus-padding replay matrices pass CI), layout tampering, device-geometry and arithmetic-overflow boundaries, V2 superblock validation, and golden-byte/CRC32C fixtures.
- Implementation batch on `OpenFS`: added public V2 allocator APIs `is_data_block_allocated` and `free_data_block_count`, then optimized bitmap scans with byte-level `trailing_zeros`/`count_ones`. The subsequent allocator batches add `find_contiguous_free_blocks`, atomic `allocate_contiguous_blocks`, `allocate_blocks` for several non-contiguous blocks in one bitmap segment, `free_blocks` for atomic batch release, `allocation_stats`, and read-only `validate_allocation_bitmap`, with reopen/persistence and request-boundary regressions. Batch operations intentionally stay within one bitmap block because the current bounded journal commits one bitmap home-block image per transaction. Linux and Windows CI passed for code commit `1e2b3fcddbcd454da63adabb54262766bc70ab35`; Linux CI (formatting, workspace tests, and Clippy) passed for the newest batch-allocation code commit `22d8adc645dc4af55c55d707482c6f1e40ef1d7a`; Windows CI was still running at the time of this review. These capabilities are implemented, but P1's full ownership/corruption acceptance gate remains open. Commit `3b59965fffb73393670576c93e26d68a5b969c53` adds allocation/free bitmap-read failure injection; commit `c6f177930e7eaabe05bde68fd647826a2b7f9b9b` extends allocation and free recovery tests to checkpoint-write failures across all five supported block sizes, and its Linux/Windows CI passed. Commit `24f08fcde2688c73c9edd095747ef5c7343ab966` adds last-valid-data-block allocation/exhaustion/free/reopen coverage and its Linux/Windows CI passed. Commit `2666ab6238d0082af149cbccefd073941618a791` adds injected I/O failures while scanning and freeing in a later bitmap segment; its CI is pending.
- Newest batch: `validate_allocation_bitmap` now checks reserved/padding bits byte-by-byte rather than iterating every bit; added public-API corruption regressions for reserved and out-of-volume padding bits across all five supported block sizes. Commits `79d44cd4c7f6946d2981d702f8f7a3e563117dae` and `d5636e3232e2b131b77e185d93f75ede31ca05db`; both CI runs were pending when this snapshot was written.
- The V2 layout specification documents geometry formulas, minimum layout, reserved bitmap bits, and disjoint regions.
- V2 journaling currently protects allocator bitmap changes; it is not yet a general transaction layer for persistent file and directory operations.
- Metadata and directory-entry codecs, checksummed extent-map pages, and an in-memory namespace exist, but a complete durable file/namespace workflow does not.
- The read-only V2 checker now validates mirrored superblocks, canonical geometry, bitmap invariants, checkpoint copies, journal record framing/order, and reports free/allocated counts plus pending/recoverable journal state. It deliberately does not replay or repair the journal, verify persistent object ownership, or certify namespace integrity. Parser fuzzing, safe physical-device discovery/formatter, and OS mount adapters remain absent.

The weighted planning estimate is **36%** (10% P0 + 12.75% P1 + 6% P2 + 2.5% P3 + 4.4% P4 + 0.5% P5; rounded to the nearest whole percent). This is a roadmap estimate, not a claim that OpenFS is ready to use on a USB drive. It is not yet a complete end-user filesystem.

| Workstream | Estimate | Evidence and remaining limitation |
| --- | ---: | --- |
| P0 — engineering foundation and format safety | 100% | Baseline gate documented as passed: Linux/Windows formatting, workspace tests, and Clippy on the recorded code commit. This is not physical-device or production validation. |
| P1 — volume layout and allocation | 85% | V2 layout, bitmap bounds, reserved-region checks, single and atomic batch allocation/free, exhaustion/reuse, reopen persistence, allocation statistics, contiguous extent search/allocation, bitmap reserved/padding validation, malformed-superblock checks, geometry validation, failure/recovery regressions, and a caller-supplied ownership-inventory consistency validator exist. Automatic ownership discovery from persistent objects and broader corruption coverage remain. |
| P2 — journal and crash recovery | 30% | Journal encoding, commit/replay/checkpoint paths, and selected torn-write/failure tests exist. Exhaustive crash-cut and flush-order guarantees are not established. |
| P3 — checker, corruption testing, fuzzing | 25% | Read-only checker validates mirrored superblocks, canonical geometry, reserved/padding bitmap invariants, both checkpoint copies with fallback, journal framing/sequence ordering, and counts pending records/committed transactions. A read-only `check-image` CLI entry point is added. Ownership checking, repair, and fuzzing remain missing; CI for the current batch is still running. |
| P4 — persistent files and directories | 22% | Metadata/directory record codecs, checksummed block-sized extent maps, sparse lookup, coalescing, physical-block iteration, atomic in-memory extent insertion, range removal/truncation returning physical runs, and an in-memory namespace with path resolution, rename/move, file-layout updates, removal, and graph validation exist. Persistent metadata discovery, durable file I/O, and journal-atomic metadata/data/free updates remain. |
| P5 — safe removable-device workflow | 5% | Raw-device primitives exist, but trusted device discovery, system/mounted-device exclusion, and safe end-user formatting are missing. |
| P6 — OS integration | 0% | No Windows mount/access adapter, Linux integration, or ArchiaOS adapter. |
| P7 — compatibility, hardening, release | 0% | No release-grade hardware matrix or complete compatibility and recovery acceptance suite. |

## Product scope and platform order

### First usable milestone: a disposable USB drive on Windows

A supported Windows PC must identify a removable device, show stable identity and exact capacity, and format only the explicitly selected target after deliberate confirmation. The resulting volume must support ordinary file operations through a documented adapter. Initial hardware testing is limited to a dedicated, expendable test drive.

### Cross-platform direction

- Keep the portable Rust core and on-disk format independent of OS-specific behavior.
- Target Windows first for the first ordinary-desktop USB workflow; choose and document the adapter architecture before implementing it.
- Implement Linux access as a separate adapter with its own safety and compatibility tests.
- Integrate ArchiaOS only after core APIs stabilize, through its block-device and VFS interfaces.
- Keep the portable core architecture-neutral; add ARM64 validation when supported host adapters and CI environments are available.
- Do not assume another OS can read OpenFS without a compatible driver or adapter.

## Milestones and acceptance gates

### P0 — Trustworthy engineering baseline — COMPLETE (baseline gate only)

- [x] Rust workspace and stable toolchain.
- [x] Linux and Windows CI for formatting, workspace tests, and Clippy.
- [x] Bounds-checked block I/O and shared error model.
- [x] Explicit on-disk parsing, CRC checks, and malformed-input tests.
- [x] Disposable image backend and refusal to overwrite by default.
- [x] Experimental volume format/open/reopen tests.
- [x] Document the distinct experimental V1/V2 layouts and V2's allocator-only transaction scope.
- [x] Review V1/V2 boundaries, reserved regions, journal replay, bitmap invariants, and error paths.
- [x] Reject protected volume-control blocks overlapping V2 journal checkpoint/ring metadata.

**Exit gate:** baseline formatting, workspace tests, and Clippy passed on Linux and Windows on the recorded baseline commit. P0 completion means the engineering baseline is trustworthy enough to continue; it does **not** establish production readiness, general transaction safety, or physical USB durability. Reopen P0 if a concrete integrity/security defect is discovered.

### P1 — Dependable volume layout and allocator — IN PROGRESS (85%)

- [x] Specify V2 layout, primary/mirror metadata, bitmap, journal, reserved regions, alignment, limits, and version compatibility.
- [x] Test allocation/free, exhaustion, reserved-block rejection, double-free behavior, reopen persistence, and reuse.
- [x] Cover bitmap-segment boundaries, reserved/padding bitmap corruption, and canonical-layout tampering.
- [x] Validate superblock checksums, reserved bytes, supported versions, block geometry, and mismatched device geometry.
- [x] Provide read-only allocation-state and free-space query APIs; use byte-level bitmap scanning for allocation and free-space counting rather than checking every data block individually (commits `9fd6b3a`, `a9a0607`; CI pending).
- [x] Cover selected allocator failures and repeated recovery, including pre-commit failures and durable-commit/home-write failures across all five supported block sizes (Linux/Windows CI run `38247258586`).
- [x] Check first and last bitmap padding bits across all supported block sizes, verify exhaustion does not advance the journal checkpoint, and exercise layout overflow boundaries for every supported block size (commits `dd01c9e` and `0509202`; CI links recorded in the snapshot above).
- [x] Confirm in CI the replay regression rejects both backup-superblock and post-volume padding corruption in the second bitmap segment (commit `a33527496cf5bd21b819bebe7c44c22aff74539d`; run `38248538558` passed on Linux and Windows).
- [x] Confirm in CI the replay matrix rejects every reserved bitmap bit across all five supported block sizes (commit `fa635c02ed3a27ea9ca3ede2735e1ee09f67f62d`; run `38248670346` passed on Linux and Windows).
- [x] Confirm in CI the replay matrix rejects reserved bits and the first padding bit across all five supported block sizes (formatting correction `79213fb40a9fe0a482bb800da60c29efb9cba65d`; run `38249481025` passed on Linux and Windows).
- [x] Expose read-only allocator state queries for a valid data block and total free data-block count; these are live bitmap reads, not reservations (`9fd6b3a`, integrated behavior test `dc063ed`; CI pending).
- [x] Add an ownership-inventory validator that compares caller-supplied block ownership against the bitmap and rejects duplicate, dangling, and orphaned allocations (persistent metadata discovery is still missing).
- [ ] Expand corruption coverage across additional malformed bitmap/layout cases.
- [x] Verify format/open/allocate/free/reopen across all five supported block sizes in CI (commit `baae2f1f2fab9dcdaece8106b4bb2ab55a2895b0`; Linux and Windows CI passed). The expanded allocator crash/failure matrix is also green on commit `f29b565af1d68d6f887c3d146934d29a55e79d6a`.

**Exit gate:** deterministic format/open/allocate/free/reopen tests, stable golden fixtures, malformed-layout rejection, and explicit ownership invariants when persistent objects are implemented.

### P2 — Transaction durability and crash recovery — IN PROGRESS (30%)

- [ ] Review Begin/redo/commit, home-block writes, checkpoint ordering, and every flush boundary.
- [ ] Inject failure before, during, and after journal, home-write, checkpoint, and flush operations.
- [ ] Cover torn records and torn home blocks, not only whole-operation errors.
- [ ] Reopen from persisted bytes after every simulated crash and assert allowed old/new states.
- [ ] Test repeated recovery and idempotency, sequence wrap/overflow policy, malformed log lengths, and journal exhaustion.
- [ ] Document what host-file and physical-device flushes do and do not guarantee.
- [ ] Define the transaction contract before extending journaling from bitmap allocation to persistent metadata and file contents.

**Exit gate:** crash-cut matrix passes on disposable images in Linux and Windows CI; recovery is repeatable and never silently accepts ambiguous/corrupt state. Physical hardware claims require separate hardware testing.

### P3 — Read-only checker and hostile-input testing — IN PROGRESS (25%)

- [x] Implement a read-only V2 checker for mirrored superblocks, canonical layout, bitmap reserved/padding invariants, and allocation totals (`check_volume_v2`).
- [x] Validate checkpoint-copy fallback, checkpoint sequence invariants, journal record framing/order, and report pending records/committed transactions without replay or writes.
- [x] Add `openfs check-image <path> <block-size>` for read-only V2 image inspection, including a regression proving image bytes remain unchanged.
- [ ] Extend integrity checking to persistent object ownership and namespace relationships.
- [ ] Add bounded fuzz targets for disk parsers, metadata, and directory/name parsing.
- [ ] Keep repair separate from checking; repair must be explicit and must describe proposed changes before writing.
- [ ] Expand the corruption corpus and CI fixtures beyond current bitmap, superblock, checkpoint, and journal-framing regressions.

**Exit gate:** checker detects known corruption without modifying the volume; fuzzing has documented budgets and regression fixtures for discovered bugs.

### P4 — Persistent namespace and ordinary file operations — IN PROGRESS (22%)

- [x] Specify initial fixed-width object metadata and directory-entry codecs with explicit encoding and CRC validation.
- [x] Add a checksummed block-sized extent-map page, sparse logical-block lookup, extent coalescing, logical/physical region validation, atomic in-memory insertion, physical-block iteration, and range removal/truncation returning physical runs for release (not yet connected to persistent file I/O).
- [x] Implement an in-memory namespace with root creation, insertion, lookup, deterministic child enumeration, path resolution, rename/move, file-layout metadata updates, removal of files and empty directories, and index/reachability validation.
- [ ] Persist root/object metadata and directory entries in V2 blocks with journaled publication.
- [ ] Specify persistent object metadata, directory entries, root directory, file lengths, and block ownership.
- [ ] Implement durable create/open/read/write/truncate/rename/delete and directory enumeration; current rename/remove are in-memory model operations only.
- [ ] Define path normalization, name encoding, case sensitivity, timestamps, permissions, and error behavior.
- [ ] Make data and metadata updates transactional and recoverable; prevent leaks, double allocation, and stale references.
- [ ] Test empty, large, and fragmented files; directory growth; disk-full behavior; rename/delete edge cases; and persistence after reopen.

**Exit gate:** automated tests create nested directories, write and verify bytes, rename/delete files, close and reopen the volume, and verify namespace and contents after every operation and injected failure.

### P5 — Safe USB discovery and formatting — NOT STARTED (5%)

Do not begin destructive hardware testing until the integrity gates in P1–P4 are satisfied.

- [ ] Implement read-only device discovery per OS: stable identity, removable status, capacity, logical sector size, partition layout, and mounted/system-volume status.
- [ ] Separate discovery from opening and writing. Never infer that a path is USB solely from its name.
- [ ] Refuse ambiguous targets, system/boot disks, mounted volumes, unsupported geometry, and devices whose identity or capacity changed after selection.
- [ ] Use OS-supported exclusive access/locking and fail closed if safe exclusion cannot be established.
- [ ] Add a dry-run plan showing exact identity, capacity, partition impact, and destructive actions.
- [ ] Require explicit target confirmation immediately before formatting; never choose a default target.
- [ ] Keep image formatting as the developer default; do not expose an easy-to-misuse raw format command before safeguards exist.

**Exit gate:** automated tests prove refusal cases using mocked device metadata; only then test on a dedicated, expendable USB drive. Record device identity, geometry, format/open/reopen, disconnect/reconnect, and integrity results. Never test on a system disk or valuable drive.

### P6 — OS integration — NOT STARTED (0%)

- [ ] Choose and document the Windows integration route, privileges, signing, installation, and failure requirements.
- [ ] Implement an adapter over stable core APIs and translate file operations/errors without adding OS assumptions to the on-disk format.
- [ ] Define safe mount/unmount, flush, exclusive access, device removal, and dirty-volume behavior.
- [ ] Implement Linux access/mount support as a separate adapter with dedicated tests and documentation.
- [ ] Add ArchiaOS VFS/block-device integration after the core contract stabilizes.
- [ ] Verify ARM64 builds/tests for supported targets.

**Exit gate:** on every claimed OS, a user can access a test volume, create/edit files through normal file APIs, unmount cleanly, reopen, and verify contents. Documentation must distinguish read-only from read/write support.

### P7 — Release hardening and compatibility — NOT STARTED (0%)

- [ ] Define format-version compatibility, upgrade rules, and explicit no-implicit-migration behavior.
- [ ] Run long stress tests, fuzzing, corruption and power-loss simulations, and performance benchmarks.
- [ ] Review concurrency, locking, resource limits, path handling, and the no-unsafe policy for the portable core.
- [ ] Add reproducible release builds and a platform/device compatibility matrix.
- [ ] Publish known limitations and recovery guidance before calling the format stable.

**Exit gate:** required CI and integrity gates are green, the documented USB hardware matrix passes, no known data-loss blocker remains, and a release checklist is signed off.

## Immediate execution order

1. Complete a targeted static review of V2 journal commit/replay/checkpoint ordering and enumerate untested crash boundaries.
2. Add the smallest useful persisted-byte crash/reopen regression tests; inspect the diff and rely on GitHub Actions for execution evidence.
3. Fix demonstrated recovery defects before expanding journal scope.
4. Build the read-only checker and corruption corpus.
5. Define persistent object ownership and transaction boundaries, then implement durable namespace/file operations.
6. Add safe read-only USB discovery and refusal tests.
7. Add explicit destructive formatting only after integrity and safety gates pass.
8. Integrate OS adapters and validate on a dedicated expendable USB drive.

This is USB-first at the product level but integrity-first at the implementation level. A raw block writer is not a usable filesystem and must never be marketed as one.

## Reporting rules

For each progress update, report separately:
- implementation and static inspection;
- tests added versus tests actually executed;
- passing GitHub Actions runs and the exact commit they validate;
- behavior proven versus assumptions and unverified hardware behavior;
- weighted overall estimate, phase estimates, changed files, and commits.

Never describe historical CI as current for a different commit, never mark a phase complete without its exit gate, and never modify `main`.
