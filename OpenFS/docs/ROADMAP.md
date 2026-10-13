# OpenFS roadmap — source-audited plan to a usable filesystem

Last source audit: 2026-10-13  
Authoritative repository/branch: Likounsee/OpenFS, branch OpenFS  
Legacy comparison: Likounsee/OpenFS-old at fdb5dee16266d037c79438a5b20ebccceab3fa24  
Current OpenFS HEAD at audit start: ce36ced79a50676553e7f9bb7d0b1bb3f82fce7c  
Scope: roadmap and planning documents only; this revision does not change implementation code.

## Executive assessment

OpenFS is currently a Rust filesystem-format and storage-engine prototype, not yet a mountable general-purpose filesystem. The Rust workspace has bounded block-device I/O, an experimental canonical layout, bitmap allocation, a bounded redo journal used for allocator changes, metadata/directory codecs, extent maps, an in-memory namespace, disk-backed inventory reconstruction, a persistent inventory descriptor, file-range reads over reconstructed metadata, and a read-only volume checker. It does not yet provide durable file or namespace mutation APIs, a mount/VFS adapter, safe USB discovery, or a repair utility.

The historical C repository is materially more feature-complete: it contains persistent inode/file/directory/path operations, transactions, links, orphan lifecycle, CoW/refcounts, FSCK/scrub, access metadata, handles, locks/runtime lifecycle, tools and platform adapters. These are source-level capabilities, not a reason to copy the C implementation wholesale. The latest historical CI run inspected for this audit, run 38167327262 on legacy HEAD fdb5dee, passed GCC, Clang and ThreadSanitizer but failed the Windows test job because openfs-crash-cut timed out (50/51 tests passed). Legacy functionality must therefore be revalidated before any behavior is used as a parity target.

The first product milestone remains a safely formatted, disposable USB drive that can be used for ordinary file operations through a supported OS. That milestone is blocked by the absence of persistent namespace writes and an OS mount adapter. Formatting an image and reading caller-located metadata do not constitute a usable filesystem.

## Evidence and confidence rules

Use these evidence labels consistently:

- **Implemented**: source code exists and has a reachable API or command.
- **Integrated**: the feature is wired into its intended end-to-end path, not just a standalone helper.
- **Test added**: a regression exists in the repository.
- **CI validated**: the exact commit has a successful relevant CI run.
- **Unverified**: code/tests exist but no passing run for the exact revision was confirmed, or the behavior needs a different environment.
- **Absent**: no implementation was found in the audited Rust tree.
- **Blocked**: a required design or dependency prevents safe completion.

A test being present is not proof it ran; a green CI run is not proof of physical-device durability or of untested crash cuts. Do not mark an item validated solely because it compiles, has a type/structure, or has a README entry. This audit used GitHub source trees, source files, documentation, workflow definitions, commits and Actions job results; it did not execute a local build or physical hardware test.

## Progress model

The phase percentages estimate progress toward the explicit acceptance gates below, not lines of code or feature count. Each phase has a fixed weight and its percentage is estimated from weighted exit criteria. Global progress is the sum of phase percentage multiplied by phase weight, rounded to the nearest whole percent. Recalculate after each substantive milestone; do not average phase percentages equally.

| Phase | Weight | Estimate | What the estimate means |
| --- | ---: | ---: | --- |
| P0 Engineering baseline | 10% | 100% | Baseline checks and development guardrails exist; this is not product readiness |
| P1 Format, layout and allocator | 15% | 82% | Strong image-backed layout/allocator coverage; descriptor publication and ownership recovery remain incomplete |
| P2 Transactions and crash recovery | 20% | 40% | Allocator redo journal and selected torn-write/recovery cases exist; no general metadata transaction contract |
| P3 Read-only verification and hostile input | 10% | 35% | Layout/bitmap/journal checker exists; whole-filesystem ownership checks, fuzzing and repair are incomplete |
| P4 Persistent namespace and file I/O | 20% | 24% | Codecs, in-memory namespace, disk reconstruction and read paths exist; durable writes and mutation are absent |
| P5 Safe device discovery and formatting | 10% | 5% | Raw-device backend exists but not discovery, exclusive ownership, target confirmation or safe end-user formatting |
| P6 OS integration | 10% | 0% | No Rust mount/VFS adapter found |
| P7 Release, compatibility and operational hardening | 5% | 3% | Some docs and CI exist; release gates and hardware/compatibility evidence do not |
| **Overall** | **100%** | **39%** | Weighted engineering progress, not percentage of a production-ready filesystem |

The current estimates are deliberately lower than the older roadmap where the older numbers treated many unchecked checklist items as done. A phase can have high implementation progress and still fail its exit gate.

Status meanings:
- **Not implemented** — no usable implementation found.
- **Partially implemented** — useful subset exists, but core behavior or integration is missing.
- **Implemented, not validated** — implementation/tests exist but the relevant exact revision has no confirmed passing validation.
- **Validated** — stated acceptance criteria passed in the cited automated or manual environment.
- **Blocked** — dependent design, safety condition or prerequisite remains open.
- **Replaced / deferred** — not part of the current implementation path; rationale is recorded.

When reporting a task, state implementation status separately from test status and exact CI evidence. A CI pass applies only to the SHA shown by that run.

## Repository and CI baseline checked for this revision

- OpenFS branch HEAD before this documentation change: ce36ced79a50676553e7f9bb7d0b1bb3f82fce7c.
- Latest Rust Actions run found: 38375379163 on ce36ced79a50676553e7f9bb7d0b1bb3f82fce7c; Linux and Windows jobs both concluded success. Each job runs rustfmt check, cargo test --workspace and Clippy with warnings denied.
- Previous code run 38375058616 on 1a7d2ea32cd21a4c6674914b4945a2ea878aa258 also passed both Linux and Windows.
- Legacy HEAD inspected: fdb5dee16266d037c79438a5b20ebccceab3fa24. Its latest Actions run found, 38167327262, failed on Windows because openfs-crash-cut timed out; GCC, Clang and ThreadSanitizer jobs passed. The previous legacy run 38166246104 was successful on an earlier commit, not on current legacy HEAD.
- The Rust CI currently covers formatting, workspace tests and Clippy on Linux and Windows. It does not currently show dedicated fuzzing, sanitizers, benchmarks, power-loss hardware testing, or mounted-filesystem integration.
- No physical USB device was accessed. No local commands were run during this documentation-only audit. Current Rust CI evidence above is remote GitHub Actions evidence.

## Source inventory and actual Rust capabilities

Workspace modules are openfs-core, openfs-block, openfs-format and openfs-tool. Rust forbids unsafe code at workspace lint level. The core crate provides checked block-range arithmetic and shared errors. The block crate defines BlockDevice read/write/flush, image and raw-file backends, and deterministic operation/write-tear injection. The format crate owns the layout, superblock, bitmap allocator, journal/checkpoints, metadata/directory codecs, extent maps, inventory descriptor, namespace reconstruction and verification. The tool crate provides development image commands, including a read-only check command.

Capabilities found in source:
- Volume: format/open/check, canonical geometry, primary/mirror superblocks, allocation/free, allocation/free-space queries, persistent inventory discovery/publication, namespace reconstruction from supplied or discovered inventory, extent-map loading, file range/content/path reads.
- Journal: bounded redo ring, checkpoint pair, sequence checks, protected home-target checks, replay and retry paths; current use is allocator metadata, not arbitrary multi-object filesystem transactions.
- Metadata: fixed-width checksummed metadata and directory records; checksummed extent-map pages; namespace graph and path APIs in memory.
- Verifier: persistent inventory/ownership consistency report for caller-located or descriptor-discovered records, including orphan/dangling diagnostics.
- CLI: image-focused development tooling; not a mount service or safe destructive raw-device workflow.

Critical capabilities not found in the Rust workspace:
- Persistent create/write/truncate/rename/unlink/mkdir/rmdir and durable directory growth.
- Canonical, bounded, scalable metadata region/index; descriptor discovery currently scans the data region.
- Atomic transaction spanning namespace records, extent maps, allocation bitmap and data publication.
- Automatic safe recovery/repair of orphaned allocation after interrupted descriptor publication.
- Persistent inode generation/link-count lifecycle, symlinks/hard links, permissions/ACL/xattrs, timestamps and open-unlinked inode handling.
- Mount/unmount, VFS integration, file handles, locking/concurrency control and OS adapters.
- Full FSCK with repair, a standalone scrub command, fuzz targets and performance/load benchmarks.
- USB identity discovery, partition/system-volume refusal, exclusive device ownership, and a user-confirmed formatter.
- Stable format release policy, versioned golden images and a legacy image converter.

## Comparison with OpenFS-old: preserve behavior, not the implementation

The legacy source tree contains a C core with format, bitmap/allocator, inode allocation, inode, extent, file, directory, path, link, journal, transaction, mount, orphan, FSCK and repair, scrub, CoW, metadata CoW/root, xattr, ACL, time, file descriptors, file locks and runtime lifecycle modules. It has 50+ registered CTest cases in the current CMake configuration and adapter/tool code for Linux, Windows and ArchiaOS. The legacy test inventory includes file/directory/path operations, concurrency, crash cuts, journal failure paths, FSCK repair failures, orphan handling, checksums, links and transaction ordering. The current legacy Windows crash-cut timeout is a real validation issue, so do not equate test count with a fully green release.

### Reimplement or adapt from legacy (in dependency order)

1. **Format contract and error boundaries** — compare the C v1.6 superblock/feature flags, primary+backup geometry, reserved regions, journal and metadata layout with the Rust draft. Rust intentionally defines a new incompatible format; reject legacy images clearly. Do not silently import the C v1.6 layout or call it compatible.
2. **Transactional allocator/inode lifecycle** — reuse the legacy tests for allocation rollback, inode bitmap/generation reuse, exact ownership, overflow and disk-full cases as test-design references. Reimplement against Rust types and the new format.
3. **File and extent semantics** — adapt tests and invariants for fragmented extents, sparse holes, EOF, partial blocks, truncate growth/shrink, zero-fill, overflow, freeing blocks only after references are removed, and failed I/O rollback. Rust currently has extent maps and read paths but no persistent writes/truncate.
4. **Directory/path semantics** — adapt tests for nested paths, parent references, name limits, trailing slash, file-as-intermediate, rename collision, moving directories, non-empty directory removal and case sensitivity. Current Rust namespace APIs are in-memory and must not be treated as durable.
5. **Transaction/crash matrix** — use legacy transaction, replay-idempotence, crash-cut, double-failure and namespace-crash tests as scenarios, not as proof the Rust journal already supports the same semantics. Specify every on-disk mutation and commit point before porting.
6. **Checker/repair/scrub** — first implement a non-mutating whole-volume checker; only then design explicit, previewable repairs. Adapt FSCK diagnostics, exact block ownership/refcount reconciliation, bitmap tails, root/parent/link invariants and scrub read-error coverage.
7. **Lifecycle/concurrency** — later adapt open handles, open-but-unlinked objects, orphan recovery, runtime admission/drain, lock ordering, file-range locks and concurrent namespace/file operations. These depend on durable mutation and a defined mount lifecycle.
8. **Advanced optional features** — CoW/refcounts, clone/discard, metadata CoW/root, snapshots, ACLs, xattrs and links should follow durable baseline operations, and each must be independently specified and tested. Do not make these prerequisites for the first simple USB file workflow unless the format design requires them.
9. **OS glue/tools** — port concepts from Linux/Windows/ArchiaOS adapters and format/fsck tools only after the Rust core API is stable. Keep OS-specific code out of openfs-format.

### Legacy hazards and compatibility rules

- Legacy documentation has format-version drift (v1.3/v1.5 documents versus source declaring v1.6); treat source plus tests as evidence and resolve discrepancies before any parity claim.
- Legacy C has a much broader feature surface, but C rollback, locking and lifecycle paths are complex and require fresh validation. The current Windows timeout is not to be ignored.
- Rust format is a separate draft. No migration/conversion tool is present; future conversion must be explicit and tested against versioned golden fixtures.
- NTFS is not an on-disk compatibility target. “NTFS-inspired” or feature comparison must not be described as NTFS mount compatibility.

## P0 — Engineering baseline and safe development contract
**Status: Validated as a baseline only — 100%. Weight: 10%.**

Implemented and validated:
- Rust workspace and four-crate dependency direction.
- Bounds-checked block I/O and shared errors.
- Image/raw-file device abstractions and I/O fault injection.
- Explicit superblock encoding/decoding, CRC and reserved-byte checks.
- Linux/Windows CI for formatting, workspace tests and Clippy.
- Image creation refuses overwrite unless the explicitly destructive API is selected.

Remaining rule: reopen this phase if a concrete integrity or safety defect is found. P0 completion is not production readiness.

**Exit criteria:** baseline CI green on both OS jobs for the exact SHA; no known untriaged baseline blocker; contributors know how to distinguish image tests from physical-device guarantees.

## P1 — Canonical format, layout and allocator
**Status: Partially implemented; recent CI validated — 82%. Weight: 15%.**

Implemented:
- Canonical layout calculation with checked geometry and supported block sizes from 4 KiB through 64 KiB.
- Primary and backup superblocks; bitmap, checkpoint copies and four-block redo ring; reserved/padding bitmap validation.
- Allocation/free, exhaustion, double-free rejection, reuse after reopen, free-space and free-extent queries, contiguous allocation/free helpers and fault injection.
- Journaled bitmap allocation changes and layout/bitmap corruption tests.
- Persistent metadata/directory codecs, extent-map page codec and ownership verifier foundations.
- Persistent inventory descriptor publication/discovery, bounded batched reads and fail-closed handling for malformed, duplicated or unallocated references.
- Latest code test for a failed descriptor home write leaves a detectable orphan and prevents publication over unresolved ownership anomalies, across all five block sizes. Rust CI run 38375058616 passed Linux and Windows formatting, workspace tests and Clippy. Latest HEAD CI run 38375379163 also passed both jobs.

Open blockers:
- Descriptor allocation and descriptor block publication are not atomic. A failure can leave an allocated orphan; current behavior detects and refuses to proceed rather than repairing it.
- Safe descriptor replacement is absent; overwriting/duplicating descriptors is not a supported update protocol.
- Discovery scans the whole data region and is not suitable as a final scalable metadata locator.
- Ownership checking depends on the current inventory representation; no full durable namespace mutation path uses it yet.
- Golden image fixtures and an explicit stable format release contract remain prerequisites before format freeze.

**Tasks and acceptance criteria**
1. **P1.1 Format invariants — Implemented; validation ongoing.** Keep golden fixtures for each supported block size; malformed geometry, reserved bytes, version, backup mismatch, overflow and truncated image must fail closed without writes.
2. **P1.2 Allocator invariants — Implemented; CI validated for recorded batches.** Every successful allocate/free must persist across reopen; no reserved/double-free/out-of-range allocation; exhaustion must not advance the checkpoint or corrupt prior allocations.
3. **P1.3 Ownership contract — Partially implemented.** Define the owner of each metadata block, extent root and data block. Reject aliases, dangling references, allocated orphans and free referenced blocks. Add tests for every cross-inventory collision.
4. **P1.4 Descriptor lifecycle — Partially implemented / blocked on transaction design.** Design a versioned descriptor/index publication protocol. Test failures at allocation commit, descriptor write prefixes, flush, reopen and repeated discovery. No replacement until a single unambiguous committed descriptor can be recovered.
5. **P1.5 Metadata locator scalability — Not implemented.** Replace full-data-region scan with a canonical locator/index before scale claims; test descriptor discovery when metadata is sparse and near the end of large images.
6. **P1.6 Format compatibility — Partially documented.** Add golden images and explicit version-change rules before writing durable namespace mutations.

**Exit gate:** deterministic format/open/allocate/free/reopen and corruption tests pass on Linux/Windows; descriptor lifecycle and ownership invariants have a safe recovery story; no unresolved known allocator/format corruption blocker. P1 must not be called complete while descriptor publication can leave unrecoverable ownership anomalies.

## P2 — Transaction protocol and crash recovery
**Status: Partially implemented — 40%. Weight: 20%.**

Implemented:
- Bounded single-writer redo journal, checkpoint copies, sequence and framing checks, protected home-target checks and recovery of committed allocator changes.
- Tests for commit/reopen, failed home writes, ignored incomplete commit, torn terminal COMMIT, torn home writes, torn checkpoint writes across several byte prefixes, retryable recovery and repeated journal use after recovery.
- Latest Rust CI confirms workspace tests and Clippy on Linux and Windows for the recorded SHA.

Absent/incomplete:
- General transactions spanning metadata records, directory entries, extent-map blocks and data blocks.
- A formal public transaction API and a defined indeterminate-result/write-poisoning contract for every device error.
- Full crash-cut matrix across all block sizes, all journal writes/flushes/checkpoint transitions and all namespace mutations.
- Transaction capacity beyond the four-block ring. Do not solve this by silently allowing oversized transactions or overwriting uncheckpointed records.
- Proof of physical-device durability; host flush semantics are only software ordering evidence.

**Tasks and acceptance criteria**
1. **P2.1 Normative state machine — Blocked on design.** Document BEGIN/redo/COMMIT/home writes/flush/checkpoint states, sequence overflow, journal-full behavior, and which errors make the live writer unusable until remount.
2. **P2.2 Fault-injection harness — Partially implemented.** Fail or tear each write and flush in turn; preserve exact on-disk bytes; remount after each cut and compare every home block.
3. **P2.3 Recovery correctness — Partially implemented.** Repeat replay and inject failures during replay/checkpoint. Committed transactions must be replayed completely and idempotently; uncommitted transactions must not be published.
4. **P2.4 Capacity design — Not implemented.** Define bounded transaction size and preflight space. Consider multi-record redo or a larger/versioned journal only with a format revision and fixtures.
5. **P2.5 Namespace transaction — Not implemented.** Atomic create/rename/delete must update namespace records, directory indexes, extent ownership and bitmap in a defined order. Crash outcomes must be exactly old or new state, not mixed state.
6. **P2.6 Durability contract — Partially documented.** Distinguish process crash, OS crash, power loss and hardware that lies about flush; hardware guarantees require dedicated devices and test plans.

**Exit gate:** persisted-byte crash-cut matrix is green on Linux and Windows; each mutation recovers to one permitted complete state; replay is idempotent; journal exhaustion and uncertain I/O fail closed. Only then expand the journal to general filesystem transactions.

## P3 — Read-only verifier, corruption corpus and repair design
**Status: Partially implemented — 35%. Weight: 10%.**

Implemented:
- check_volume validates superblocks, canonical layout, bitmap reserved/padding invariants, allocation totals and journal/checkpoint state without replay or modifying the image.
- CLI check-image path and image-byte-preservation regression.
- Persistent inventory verifier composes namespace reconstruction, extent maps and allocation ownership, with orphan/dangling reports when inventories are available.

Incomplete:
- Full whole-volume traversal from a canonical metadata root/locator is not yet possible for every image.
- No standalone FSCK repair command; no explicit repair preview/plan/apply protocol.
- No fuzz target, long-running malformed-input corpus or sanitizer/fuzz CI.
- Current checker cannot prove data content checksums because the Rust format has no general persistent data checksum table.
- Read-only check and writable open are distinct: check must never replay or repair silently.

**Tasks and acceptance criteria**
1. **P3.1 Read-only structural check — Partially validated.** Corrupt each superblock/checkpoint/bitmap/journal field and verify deterministic diagnostics, no writes and no panic.
2. **P3.2 Full object traversal — Blocked by P4 metadata locator.** Validate every object, parent/reachability edge, directory record, extent map, allocated block and owner count from disk.
3. **P3.3 Diagnostics contract — Not implemented.** Stable machine-readable codes, block/object identifiers, deterministic ordering and severity; avoid opaque error-only output for recoverable ownership anomalies.
4. **P3.4 Fuzzing — Not implemented.** Add bounded fuzz targets for superblock, journal/checkpoint, metadata/directory codecs, extent-map and descriptor decoders. Keep discovered failures as regression fixtures.
5. **P3.5 Repair planning — Not implemented.** Read-only scan first; explicit dry-run report, narrow repair set, backup/rollback policy and second verification pass before any repair write.
6. **P3.6 Data integrity — Not implemented.** Decide whether the new format needs data checksums, then specify their on-disk coverage and transaction semantics before enabling them.

**Exit gate:** checker covers every reachable persistent object, reports known corruption, never mutates, and malformed input is fuzzed within a documented budget. Repair remains opt-in and separate from checking.

## P4 — Durable namespace, files and directories
**Status: Partially implemented, read-only only — 24%. Weight: 20%.**

Implemented:
- Fixed-width checksummed metadata records and directory entries.
- Checksummed extent-map pages, sparse logical mapping, extent coalescing, bounds/alias validation and physical range removal in memory.
- In-memory namespace create/lookup/list/path/rename/remove validation.
- Disk-backed inventory readers, descriptor-based discovery, namespace reconstruction, extent-map loading, and file range/content/path reads including fragmented extents, sparse zero-fill and EOF handling.
- Tests for malformed records, graph inconsistencies, ownership aliases, free blocks and read failures. The Rust public surface does not currently expose persistent create/write/truncate/rename/delete.

Required design:
- Choose the persistent metadata root/index and allocation/ownership model.
- Specify directory storage and growth, file lengths, object IDs/generations, data initialization, sparse semantics, name normalization/case policy and error mapping.
- Decide the supported first-release semantics before implementing optional POSIX/Windows features.
- Keep data blocks and metadata publication transactionally consistent; do not release an old block before the new metadata no longer references it.

**Tasks and acceptance criteria**
1. **P4.1 Canonical metadata locator/root — Not implemented.** Versioned root points to bounded metadata and directory structures; validate all pointers before use.
2. **P4.2 Durable read-only mount model — Partially implemented.** Open from only on-disk roots without caller-supplied block lists; remount must reconstruct the same object graph.
3. **P4.3 Durable create/mkdir — Not implemented.** Allocate IDs and blocks, publish parent entry and metadata atomically; failure/reopen must show either absent or complete object.
4. **P4.4 Durable write/truncate — Not implemented.** Test empty/large/fragmented/sparse files, partial blocks, overwrite, grow/shrink, no-space and every I/O failure. Old data must remain readable or a documented committed new version must be complete.
5. **P4.5 Durable rename/unlink/rmdir — Not implemented.** Test collisions, moving directories, non-empty directories, same-name cases, stale references, link counts when supported and crash cuts.
6. **P4.6 Directory growth and large inventories — Not implemented.** Test capacity boundaries, pagination, bounded memory and no duplicate/lost entries.
7. **P4.7 Semantics — Not implemented.** Specify path normalization, absolute/relative paths, trailing separators, case sensitivity, name encoding, timestamps, permissions and file type behavior.
8. **P4.8 Read/write API and errors — Not implemented.** Provide explicit handles or safe APIs; distinguish EOF, no-space, corruption, unsupported operation and I/O failure.
9. **P4.9 Persistent end-to-end tests — Not implemented.** Create nested trees, write known bytes, close/reopen, read, rename, delete, verify ownership, and inject failures after every durable step.

**Exit gate:** image-backed create/read/write/truncate/rename/delete and directory operations survive reopen and injected crashes; checker reports no leaks, aliases or dangling references. No OS mount should precede this gate.

## P5 — Safe device discovery and destructive formatting
**Status: Bare groundwork only — 5%. Weight: 10%.**

The raw-file device backend is real, but it does not establish that a path is a USB device or safely selected. Opening is non-destructive; later writes can overwrite partition tables and user data. There is no end-user raw-device formatter or safe discovery flow.

Tasks:
1. **P5.1 Read-only discovery — Not implemented.** Per-OS identity, serial/device path, removable status, exact capacity, sector geometry, partition table and mounted/system-volume status.
2. **P5.2 Exclusive ownership — Not implemented.** Acquire OS-supported exclusive access; reject mounted, busy, boot/system, ambiguous, unsupported or changed devices.
3. **P5.3 Dry-run plan — Not implemented.** Show exact stable identity, capacity, partitions and every destructive action before any write.
4. **P5.4 Explicit confirmation — Not implemented.** Revalidate identity/geometry immediately before format; no default target and no path-name heuristics.
5. **P5.5 Mocked refusal tests — Not implemented.** Prove no writes occur for every unsafe/ambiguous condition.
6. **P5.6 Hardware validation — Blocked.** Only after P1–P4 gates: dedicated expendable USB, disconnect/reconnect, format/open/reopen, read/write, flush, removal during I/O and checker pass. Never test on valuable or system disks.

**Exit gate:** all unsafe target cases are refused, target identity is revalidated, and the dedicated hardware test record is reproducible. Image-only tests do not satisfy this gate.

## P6 — OS integration and normal file APIs
**Status: Not implemented — 0%. Weight: 10%.**

No Rust mount/VFS or OS filesystem adapter was found in the current workspace. Legacy adapters are references only.

Tasks:
1. **P6.1 Choose Windows route — Blocked by P4.** Document WinFsp or other chosen integration, privilege/signing/installer requirements, error mapping and failure modes.
2. **P6.2 Adapter contract — Not implemented.** Map file operations, handles, flush, unmount and device removal to stable core APIs without OS assumptions in the format crate.
3. **P6.3 Safe lifecycle — Not implemented.** Prevent unmount races, reject operations after shutdown starts, drain in-flight operations and report dirty/failed state.
4. **P6.4 Windows integration — Not implemented.** Integration tests through normal OS APIs, not direct library calls alone.
5. **P6.5 Linux adapter — Not implemented.** Separate mount/userspace adapter, permissions/error semantics and unmount tests.
6. **P6.6 ArchiaOS VFS/block integration — Not implemented.** Port only after the portable contract stabilizes.
7. **P6.7 ARM64 — Deferred.** Build/test where target and runner support are available; do not claim support from architecture-neutral source alone.

**Exit gate:** users can create/edit/read/rename/delete files via normal OS APIs, unmount cleanly, remount and verify bytes on each claimed platform.

## P7 — Release, compatibility and long-term reliability
**Status: Planning only — 3%. Weight: 5%.**

Tasks:
1. **P7.1 Format version policy — Partially documented.** Define feature negotiation, incompatibility rejection, golden fixtures, migration rules and no implicit conversion. Rust draft is not compatible with legacy C v1.x.
2. **P7.2 Upgrade/conversion — Not implemented / optional.** If conversion is added, use a separate read-only source and new target image, validate both, and never overwrite the only copy.
3. **P7.3 Stress and resource limits — Not implemented.** Large-volume, fragmentation, many-file, near-full, repeated mount/unmount and sustained I/O tests.
4. **P7.4 Fuzz/sanitizer matrix — Not implemented for Rust.** Add fuzzing, Miri where appropriate, sanitizer-equivalent checks and minimized regression cases.
5. **P7.5 Benchmarks — Not implemented as a release gate.** Establish correctness-preserving baselines for sequential/random I/O, metadata operations, fragmented reads and checker throughput; avoid premature tuning.
6. **P7.6 Security review — Not implemented.** Hostile image inputs, resource exhaustion, symlink/path traversal once supported, concurrent external modification and raw-device privilege boundaries.
7. **P7.7 Release operations — Not implemented.** Reproducible builds, signed artifacts where appropriate, support matrix, known limitations, recovery guide and rollback/recovery instructions.

**Exit gate:** all earlier exit gates pass, no known data-loss blocker remains, compatibility and recovery behavior are documented, and the supported OS/device matrix is validated.

## Cross-cutting reliability requirements

These requirements apply to every phase and must not be deferred to release:
- Check all disk-derived lengths, offsets, counts and arithmetic for overflow and bounds before I/O or allocation.
- Reject malformed, truncated, unsupported-version, checksum-invalid and ambiguous state without guessing or silently repairing.
- Preserve the distinction between absent/uncommitted and committed/recoverable operations.
- Test no-space and every I/O error path, including failed rollback and failed flush; if the state is uncertain, stop further writes until recovery.
- Check ownership across bitmap, metadata records, directory entries, extent-map roots and data extents; no alias, dangling pointer or silent leak.
- Every mutation needs a defined commit point and persisted-byte crash tests before it is considered durable.
- Keep checker read-only and repair explicit, previewable and independently validated.
- Do not claim multi-writer concurrency until locking, serialization and race tests exist.
- Define data-checksum coverage or explicitly document that regular file contents are not checksummed by the current Rust format.
- Distinguish process termination tests from OS crash, power-loss and physical-device tests.
- Never claim NTFS compatibility or legacy OpenFS image compatibility without an implemented and tested conversion/driver path.

## Immediate execution order

1. **P1/P2:** settle descriptor lifecycle and transaction capacity; do not add new persistent mutation APIs before a general commit/recovery contract exists.
2. **P2:** expand persisted-byte crash cuts for every journal write, flush, checkpoint and replay failure, including repeated recovery; keep Linux/Windows CI green on exact SHAs.
3. **P3:** finish read-only diagnostics and malformed-input corpus, then add bounded fuzz targets.
4. **P4:** define a canonical metadata root/index and implement a minimal durable vertical slice: create file, write bytes, reopen, read bytes, check ownership. Add mkdir/rename/unlink only after this path is transactional.
5. **P3/P4:** build whole-volume verification from the canonical root and add explicit repair design only after checker coverage.
6. **P5:** build mocked read-only device discovery/refusal logic; destructive formatting remains unavailable until the data-integrity gates pass.
7. **P6:** choose the Windows adapter and integrate only against the stable persistent API; then Linux and ArchiaOS.
8. **P7:** add stress/fuzz/benchmark/release validation after correctness gates, not in place of them.

## Required format for future progress reports

Every development update must report:
- Current branch HEAD and exact code/documentation commit SHA.
- Phase percentages and the weighted overall estimate using the formula above.
- Implementation status versus integration status.
- Tests added versus tests executed versus tests passed.
- Exact GitHub Actions run URL and tested SHA for any CI claim.
- Remaining blockers, including any failed or pending CI job.
- Whether the evidence is image-backed, host-device-backed, or physical-hardware-backed.

Never mark a phase complete while its exit gate is false. Never report the latest successful CI for a different SHA as validation of a new change. Do not modify the main branch or OpenFS-old as part of this roadmap.
