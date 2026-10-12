# OpenFS roadmap — USB-first delivery plan

Last reviewed: 2026-10-12  
Authoritative branch: `OpenFS`  
Primary product goal: a user can safely format a standard USB flash drive as OpenFS, then create, read, write, rename, and delete files through a supported operating system. OpenFS defines its own format; it is **not** an NTFS implementation and must not claim NTFS compatibility.

Percentages are planning estimates against acceptance criteria, not feature counts. A phase is complete only when its tests and evidence pass. Never infer hardware support from file-image tests.

## Current snapshot

HEAD reviewed: `ad9f6e61253ab2b2648de8f8285b2e37a77fed65`.

| Workstream | Estimate | Evidence / limitation |
| --- | ---: | --- |
| P0 — engineering foundation and format safety | 70% | Rust workspace, CI, bounded block I/O, explicit parsing, CRC checks, mirrored volume metadata, failure injection, and image/reopen tests exist. Current HEAD's Linux and Windows CI both fail to compile a journal test because `original_sequence` is referenced in the wrong test scope. Fix this before treating later recovery work as validated. |
| P1 — volume layout and allocation | 25% | Experimental V2 layout, reserved regions, bitmap validation, allocation/free, and disposable-file RawDevice coverage exist. Persistent namespace/file operations are not complete. |
| P2 — journal and crash recovery | 30% | Journal encoding and V2 commit/replay/checkpoint paths plus torn-write tests exist. The latest tests do not currently compile; exhaustive crash-cut, flush-order, and real-device guarantees are not established. |
| P3 — checker, corruption testing, fuzzing | 0% | No accepted read-only checker, repair workflow, or parser fuzzing gate. |
| P4 — persistent files and directories | 5% | Some metadata/namespace primitives exist, but no complete durable create/read/write/rename/delete path. |
| P5 — safe removable-device workflow | 5% | Low-level RawDevice exists. There is no trustworthy device discovery, mounted/system-device exclusion, or safe end-user raw formatter. |
| P6 — OS integration | 0% | No Windows mount adapter or Linux userspace/kernel integration; no ArchiaOS adapter. |
| P7 — compatibility, hardening and release | 0% | No release-grade compatibility matrix or hardware acceptance suite. |

The current weighted planning estimate is approximately **17%** (weights: P0 10%, P1 15%, P2 20%, P3 10%, P4 20%, P5 10%, P6 10%, P7 5%). This is not a claim that OpenFS can already be used on a USB drive. It cannot yet.

## Product definition and scope

### First usable milestone: disposable USB drive on Windows

A supported Windows PC can identify a removable USB device, show its model/identity and exact capacity, and format only that explicitly selected device as OpenFS after deliberate confirmation. The user can then browse and modify files through the supported adapter. The first release is limited to a dedicated, expendable test drive and clearly documented supported devices.

### Cross-platform direction

- **Portable Rust core and on-disk format:** no OS-specific behavior in format parsing, allocation, transactions, or namespace semantics.
- **Windows:** first end-user integration target, because it is the first requested ordinary-desktop USB workflow. Select and document the adapter architecture before implementing it; raw block access alone does not make a drive browseable in Explorer.
- **Linux:** provide equivalent safe device handling and a documented mount/access path; validate separately rather than assuming Windows behavior transfers.
- **ArchiaOS:** integrate only after the core API and semantics stabilize, through its block-device and VFS interfaces. Do not couple the experimental core prematurely to ArchiaOS internals.
- **ARM64:** keep the portable core architecture-neutral and add ARM64 CI/build validation when the host adapter and test environment are ready. Do not make ARM64 a prerequisite for fixing current data-integrity failures.

OpenFS is a new filesystem format. Formatting a drive erases the selected target's existing layout/data. Other operating systems will not automatically read OpenFS without an OpenFS driver or adapter.

## Milestones and acceptance gates

### P0 — Restore a trustworthy development baseline (current blocker)

- [x] Rust workspace and stable toolchain.
- [x] Linux and Windows CI for formatting, tests, and Clippy.
- [x] Bounds-checked block I/O and shared error model.
- [x] Explicit on-disk parsing, CRC checks, and malformed-input tests.
- [x] Disposable image backend and refusal to overwrite by default.
- [x] Experimental volume formatting/open/reopen tests.
- [ ] Fix the current `original_sequence` test-scope compile error; require both OS CI jobs green on the exact branch HEAD.
- [ ] Add golden on-disk fixtures and review field, checksum, reserved-byte, and versioning policy.
- [ ] Make geometry and reserved-region constraints explicit and tested.

**Exit gate:** clean fmt, full workspace tests, and Clippy on Linux and Windows for the exact commit. No later milestone is called validated while this gate is red.

### P1 — Make the volume layout and allocator dependable

- [ ] Specify the V2 layout completely: primary/mirror metadata, bitmap, journal, reserved regions, alignment, limits, and version compatibility.
- [ ] Test allocation/free across block boundaries, exhaustion, double-free, reserved-block rejection, and reopen.
- [ ] Verify every allocation bit has a single valid owner once persistent objects exist.
- [ ] Reject inconsistent geometry and metadata without panic, overflow, or unbounded allocation.

**Exit gate:** deterministic format/open/allocate/free/reopen tests, corruption tests, and stable golden fixtures.

### P2 — Finish transaction durability and recovery (priority after P0)

- [ ] Review write ordering for Begin/redo/commit, home blocks, checkpoint, and flush boundaries.
- [ ] Inject failure before, during, and after every journal, home-write, checkpoint, and flush boundary.
- [ ] Cover torn records and torn home blocks, not just whole-operation errors.
- [ ] Reopen from the persisted bytes after each simulated crash and verify only allowed old/new states.
- [ ] Test repeated recovery, idempotency, sequence wrap/overflow policy, malformed log lengths, and log exhaustion.
- [ ] Document what `flush` can and cannot guarantee for host files and physical USB controllers.

**Exit gate:** crash-cut matrix passes on disposable images on Linux and Windows; recovery is repeatable and never silently accepts ambiguous/corrupt state.

### P3 — Read-only checker and hostile-input testing

- [ ] Add a read-only volume checker for superblocks, journal, bitmap, reserved areas, and later namespace ownership.
- [ ] Add bounded fuzz targets for all disk parsers and directory/name parsing.
- [ ] Keep repair separate from checking; repair must be explicit, explain proposed changes, and never silently overwrite data.
- [ ] Add corrupt/truncated/oversized image corpus to CI.

**Exit gate:** checker reports corruption without modifying the volume; fuzz targets run under documented budgets and have regression fixtures for found bugs.

### P4 — Persistent namespace and ordinary file operations

- [ ] Specify persistent object metadata, directory entries, root directory, file lengths, and block ownership.
- [ ] Implement create/open/read/write/truncate/rename/delete and directory enumeration.
- [ ] Define path normalization, name encoding, case sensitivity, timestamps, permission semantics, and error behavior.
- [ ] Make metadata and data updates transactional and recoverable; prevent leaks, double allocation, and stale references.
- [ ] Test empty files, large files, fragmented files, directory growth, disk-full behavior, rename/delete edge cases, and reopen persistence.

**Exit gate:** an automated test can create nested directories, write and verify bytes, rename/delete files, close/reopen the volume, and verify the resulting namespace and contents after every operation.

### P5 — Safe USB discovery and formatting

This milestone must not start destructive hardware testing until P0–P4 acceptance gates relevant to integrity have passed.

- [ ] Implement read-only device discovery per OS: stable identity, removable status, capacity, logical sector size, partition layout, and mounted/system-volume status.
- [ ] Separate discovery from opening and writing. Never infer that a path is a USB device solely from its name.
- [ ] Refuse ambiguous targets, system/boot disks, mounted volumes, unsupported geometry, and devices whose identity/capacity changed after selection.
- [ ] Use OS-supported exclusive access/locking where available and fail closed when safe exclusion cannot be established.
- [ ] Add a dry-run plan that displays exact device identity, capacity, partition impact, and every destructive action.
- [ ] Require explicit target confirmation immediately before formatting; never provide an implicit default target.
- [ ] Keep image formatting as the default developer workflow. Do not add an easy-to-misuse raw format command before these safeguards exist.

**Exit gate:** automated tests prove all refusal cases using mocked device metadata; only then test on a dedicated, expendable USB drive. Record before/after device identity, geometry, format/open/reopen, disconnect/reconnect, and integrity checks. Never use a system disk or valuable drive.

### P6 — Make OpenFS browseable through OS integration

- [ ] Choose and document the Windows integration route and its privilege, signing, install, and failure requirements.
- [ ] Implement the adapter against stable core interfaces; translate file operations and errors without putting Windows assumptions in the on-disk format.
- [ ] Define safe mount/unmount, flush, exclusive access, device removal, and dirty-volume behavior.
- [ ] Implement Linux access/mount support as a separate adapter with its own tests and documentation.
- [ ] Add ArchiaOS VFS/block-device adapter after the core contract is stable.
- [ ] Verify ARM64 builds and tests for supported targets.

**Exit gate:** on each claimed OS, a user can mount/access a test OpenFS volume, create and edit files through normal file APIs, unmount cleanly, reopen, and verify contents. Documentation must distinguish read-only from read/write support.

### P7 — Release hardening and compatibility

- [ ] Define a versioned format compatibility policy, upgrade rules, and explicit no-implicit-migration behavior.
- [ ] Run long stress tests, fuzzing, corruption and power-loss simulations, and performance benchmarks.
- [ ] Review concurrency, locking, resource limits, path handling, and all unsafe-code policy exceptions (prefer no unsafe in the portable core).
- [ ] Add reproducible release builds and a platform/device compatibility matrix.
- [ ] Publish known limitations and recovery guidance before calling the format stable.

**Exit gate:** all required CI and integrity gates green, documented USB hardware matrix passed, no known data-loss blocker, and a release checklist signed off.

## Immediate execution order

1. Fix the failing `original_sequence` test scope and get Linux + Windows CI green.
2. Validate the torn-write tests' operation indices and recovery assertions; do not weaken assertions merely to make CI pass.
3. Audit V2 journal/checkpoint geometry and expand the crash-cut matrix.
4. Complete persistent namespace/file operations.
5. Build safe read-only USB discovery and refusal tests.
6. Add the explicit destructive formatter only after safeguards and recovery pass.
7. Integrate OS adapters and validate a dedicated physical USB drive.

This order is intentionally USB-first at the product level, but integrity-first at the implementation level. A working raw block writer is not a usable filesystem and must never be marketed as one.
