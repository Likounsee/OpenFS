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

- Current branch head at review: `baae2f1f2fab9dcdaece8106b4bb2ab55a2895b0`.
- GitHub Actions run [38236874023](https://github.com/Likounsee/OpenFS/actions/runs/38236874023) passed for this exact commit on Linux and Windows, including formatting, workspace tests, and Clippy.
- GitHub Actions for that commit report Linux and Windows jobs successful for formatting, workspace tests, and Clippy.
- Recent regressions cover allocator exhaustion/reuse, bitmap segment boundaries, layout tampering, reserved/padding bitmap corruption, device-geometry mismatches, allocator failure recovery, V2 superblock validation, and golden-byte/CRC32C fixtures.
- The V2 layout specification documents geometry formulas, minimum layout, reserved bitmap bits, and disjoint regions.
- V2 journaling currently protects allocator bitmap changes; it is not yet a general transaction layer for persistent file and directory operations.
- Metadata codecs, directory-entry primitives, and an in-memory namespace exist, but a complete durable file/namespace workflow does not.
- No accepted read-only volume checker, parser fuzzing gate, safe physical-device discovery/formatter, or OS mount adapter is present in the current roadmap scope.

The weighted planning estimate is **28%** (10% P0 + 10.5% P1 + 6% P2 + 1% P4 + 0.5% P5). This is a roadmap estimate, not a claim that OpenFS is ready to use on a USB drive. It is not yet a complete end-user filesystem.

| Workstream | Estimate | Evidence and remaining limitation |
| --- | ---: | --- |
| P0 — engineering foundation and format safety | 100% | Baseline gate documented as passed: Linux/Windows formatting, workspace tests, and Clippy on the recorded code commit. This is not physical-device or production validation. |
| P1 — volume layout and allocation | 70% | V2 layout, bitmap bounds, reserved-region checks, allocation/free, exhaustion/reuse, reopen persistence, malformed-superblock checks, geometry validation, and failure/recovery regressions exist. Ownership validation for persistent objects and broader corruption coverage remain. |
| P2 — journal and crash recovery | 30% | Journal encoding, commit/replay/checkpoint paths, and selected torn-write/failure tests exist. Exhaustive crash-cut and flush-order guarantees are not established. |
| P3 — checker, corruption testing, fuzzing | 0% | No accepted read-only checker, explicit repair workflow, or parser fuzzing CI gate. |
| P4 — persistent files and directories | 5% | Some metadata and namespace primitives exist; complete durable create/read/write/rename/delete is missing. |
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

### P1 — Dependable volume layout and allocator — IN PROGRESS (70%)

- [x] Specify V2 layout, primary/mirror metadata, bitmap, journal, reserved regions, alignment, limits, and version compatibility.
- [x] Test allocation/free, exhaustion, reserved-block rejection, double-free behavior, reopen persistence, and reuse.
- [x] Cover bitmap-segment boundaries, reserved/padding bitmap corruption, and canonical-layout tampering.
- [x] Validate superblock checksums, reserved bytes, supported versions, block geometry, and mismatched device geometry.
- [x] Cover selected allocator failures and repeated recovery.
- [ ] Verify every allocated block has exactly one valid owner once persistent objects exist.
- [ ] Expand corruption coverage across additional malformed bitmap/layout cases.
- [x] Verify format/open/allocate/free/reopen across all five supported block sizes in CI (commit `baae2f1f2fab9dcdaece8106b4bb2ab55a2895b0`; Linux and Windows CI passed).

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

### P3 — Read-only checker and hostile-input testing — NOT STARTED (0%)

- [ ] Implement a read-only checker for superblocks, layout, journal, bitmap, reserved areas, and later namespace ownership.
- [ ] Add bounded fuzz targets for disk parsers, metadata, and directory/name parsing.
- [ ] Keep repair separate from checking; repair must be explicit and must describe proposed changes before writing.
- [ ] Add corrupt, truncated, oversized, and inconsistent image fixtures to CI.

**Exit gate:** checker detects known corruption without modifying the volume; fuzzing has documented budgets and regression fixtures for discovered bugs.

### P4 — Persistent namespace and ordinary file operations — NOT STARTED (5%)

- [ ] Specify persistent object metadata, directory entries, root directory, file lengths, and block ownership.
- [ ] Implement create/open/read/write/truncate/rename/delete and directory enumeration.
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
