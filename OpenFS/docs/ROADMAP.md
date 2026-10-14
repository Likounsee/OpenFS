# OpenFS roadmap — source-audited plan to a usable filesystem

Last source audit: 2026-10-13  
Authoritative repository/branch: Likounsee/OpenFS, branch OpenFS only  
Code baseline audited: 97ebd67d4470dc72000430b41d9686940c72c3a2  
Current branch HEAD before this roadmap revision: 97ebd67d4470dc72000430b41d9686940c72c3a2  
Legacy comparison: Likounsee/OpenFS-old at fdb5dee16266d037c79438a5b20ebccceab3fa24  
Scope: record allocator recovery and descriptor lifetime protections, cross-structure verifier regressions, orphan-publication refusal, descriptor publication write/flush recovery, journal commit-flush uncertainty recovery, protected-target prevalidation before replay, malformed nonterminal journal rejection, and preflight of transaction-ID, checkpoint-generation, and record-sequence overflow before commit/recovery writes. The latest implementation batch also tests recovery sequence overflow before redo replay. This documentation commit does not change filesystem code.

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

The phase percentages estimate progress toward the explicit acceptance gates below, not lines of code or feature count. Each phase has a fixed weight. The initial estimates are provisional until the per-task scoring ledger below is applied; the ledger is the source of truth for subsequent updates. Global progress is the sum of each phase's weight multiplied by its earned phase fraction, divided by the sum of all phase weights, then rounded to the nearest whole percentage point. Do not average phase percentages equally.

**Units and rounding:** phase weights are percentage points of the project and sum to 100. Inside each phase, task weights are normalized points and sum to 100. A task earns between zero and its full task weight; its earned points are based on the strongest evidence actually available for its acceptance criteria, not on the number of files or tests. Phase progress = earned task points / 100 × 100%. Calculate with full precision and round only the displayed phase and global results to the nearest whole percent (0.5 rounds upward). A displayed integer is not a claim of measurement precision; retain the earned-point ledger to make changes reproducible.

**Evidence credit rule:** for each task, score each stated acceptance criterion once, then sum its weighted criterion contributions. A criterion with no implementation earns 0%; code present but not reachable from the intended path earns at most 25%; integrated behavior without a relevant regression test earns at most 50%; a regression test added but not confirmed executed earns at most 60%; a test confirmed executed locally or in a run earns at most 75%; exact-SHA relevant CI success earns at most 90%; 100% is reserved for all task acceptance criteria being met with evidence appropriate to the claim. These are ceilings, not automatic awards. Do not award the same criterion again under another task. For a cross-cutting test, credit it to the task whose behavior it exercises, not again to a generic testing task. Record test-added, test-executed, CI-validated, and physical-device-validated as separate evidence fields.

**Scoring reproducibility limitation:** the task ledger below records weights and current earned points, but some earned values are still expert estimates justified by narrative evidence rather than sums from published subcriteria. Therefore, the phase values and the 39% global figure are **provisional engineering estimates**, not a fully mechanical measurement. Do not describe the score as fully reproducible until every non-zero partial task is split into named, weighted acceptance criteria with an evidence link/status per criterion. Until that decomposition is complete, preserve the current figures for continuity, disclose this limitation, and change a score only when the cited evidence changes. Never invent criterion weights retroactively merely to make an existing estimate appear exact.

| Phase | Weight | Estimate | What the estimate means |
| --- | ---: | ---: | --- |
| P0 Engineering baseline | 10% | 100% | Baseline checks and development guardrails exist; this is not product readiness |
| P1 Format, layout and allocator | 15% | 86% | Strong image-backed layout/allocator coverage; descriptor replacement and broader ownership recovery remain incomplete |
| P2 Transactions and crash recovery | 20% | 40% | Allocator redo journal and selected torn-write/recovery cases exist; no general metadata transaction contract |
| P3 Read-only verification and hostile input | 10% | 35% | Layout/bitmap/journal checker exists; whole-filesystem ownership checks, fuzzing and repair are incomplete |
| P4 Persistent namespace and file I/O | 20% | 21% | Codecs, in-memory namespace, disk reconstruction and read paths exist; durable writes and mutation are absent |
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

- Code/source baseline audited for this roadmap: commit [44efa42408258247fdcb99890799f19b8e9c482f](https://github.com/Likounsee/OpenFS/commit/44efa42408258247fdcb99890799f19b8e9c482f).
- Earlier roadmap-only commit: [7e0f50eabe49ac4aeb1e763d9f7fe1181a1bf618](https://github.com/Likounsee/OpenFS/commit/7e0f50eabe49ac4aeb1e763d9f7fe1181a1bf618). It changes documentation, not filesystem implementation.
- CI run [38379103899](https://github.com/Likounsee/OpenFS/actions/runs/38379103899) completed successfully for exact SHA `44efa42408258247fdcb99890799f19b8e9c482f`; Linux and Windows jobs both passed rustfmt check, `cargo test --workspace` and Clippy with warnings denied.
- CI run [38379956462](https://github.com/Likounsee/OpenFS/actions/runs/38379956462) completed successfully for exact roadmap-only SHA `7e0f50eabe49ac4aeb1e763d9f7fe1181a1bf618`. This validates the configured automated checks on that revision; it does not constitute new implementation evidence.
- Latest roadmap-only commit before this revision: [4b0ee02b974ceecc10379378b72d20e168d4d6bf](https://github.com/Likounsee/OpenFS/commit/4b0ee02b974ceecc10379378b72d20e168d4d6bf); its exact-SHA CI run [38423229047](https://github.com/Likounsee/OpenFS/actions/runs/38423229047) passed Linux and Windows. This is documentation validation only, not new implementation evidence.
- Earlier run 38375379163 on `ce36ced79a50676553e7f9bb7d0b1bb3f82fce7c` and run 38375058616 on `1a7d2ea32cd21a4c6674914b4945a2ea878aa258` are historical evidence only; neither is the latest run for the current branch.
- Legacy HEAD inspected: `fdb5dee16266d037c79438a5b20ebccceab3fa24`. Its latest Actions run found, 38167327262, failed on Windows because `openfs-crash-cut` timed out; GCC, Clang and ThreadSanitizer jobs passed. The previous legacy run 38166246104 succeeded on an earlier commit, not current legacy HEAD.
- Rust CI covers formatting, workspace tests and Clippy on Linux and Windows. The latest ownership-validation batch is exact SHA `c7b93f1ee77869529d869ce4d3ae15ef6dae52aa`, validated by run [38437650554](https://github.com/Likounsee/OpenFS/actions/runs/38437650554) with both platform jobs successful. It bounds extent-allocation bitmap caching to one page while scanning sorted physical ranges, replaces per-range linear root-alias checks with binary search, and adds `namespace_extent_map_root_checks_scale_across_all_supported_block_sizes` for 32 file roots at each supported block size. No dedicated fuzzing, sanitizers, benchmarks, power-loss hardware testing or mounted-filesystem integration was evidenced in the inspected workflows.
- Exact-SHA CI run [38453937847](https://github.com/Likounsee/OpenFS/actions/runs/38453937847) passed for `536a744d46e0c0b12616f92e0f07bbcbe69343a7` on both Linux and Windows (formatting, workspace tests, Clippy). The `refuses_to_publish_when_existing_allocations_have_no_owner` regression now exercises all five supported block sizes (4, 8, 16, 32 and 64 KiB) and confirms a rejected inventory publication leaves the orphan allocated, consumes no descriptor block, and remains undiscoverable.
- Exact-SHA CI run [38455410660](https://github.com/Likounsee/OpenFS/actions/runs/38455410660) passed Linux and Windows formatting, workspace tests, and Clippy for `7db08563db31540bb62f0d1f7eb2ab245857e017`. It validates the five-size staging-flush failure regression after closing and reopening the image, including undiscoverability and successful retry.
- Exact-SHA CI run [38456414882](https://github.com/Likounsee/OpenFS/actions/runs/38456414882) passed formatting, workspace tests and Clippy on Linux and Windows for `a68968118edee7e854bdda0233ed88843278c970`. This includes the journal commit-record flush-failure recovery regression: after the commit record has been written but its flush reports an error, reopening replays the committed redo transaction. The image backend exposes writes before the injected flush error; this is an uncertain-outcome recovery test, not physical power-loss proof.
- Exact-SHA CI run [38457627241](https://github.com/Likounsee/OpenFS/actions/runs/38457627241) passed formatting, workspace tests and Clippy on Linux and Windows for `38dc8b8e75d0b468d1ec4892ab3d9c468db605ad`. `protected_recovery_validates_all_targets_before_replaying_any_update` constructs a durable transaction with one valid home target followed by a protected control-block target and confirms recovery rejects the transaction without partially applying the first update or modifying the protected block.
- Exact-SHA CI run [38458063369](https://github.com/Likounsee/OpenFS/actions/runs/38458063369) passed formatting, workspace tests and Clippy on Linux and Windows for `996b4566f60f196d618c14f17d0de1b728b8299d`. `malformed_nonterminal_record_before_later_valid_records_fails_closed` damages the BEGIN record of a durable but uncheckpointed transaction and confirms recovery refuses the malformed interior record rather than treating it like a safely ignorable terminal append.
- Exact-SHA run [38459392863](https://github.com/Likounsee/OpenFS/actions/runs/38459392863) passed formatting, workspace tests and Clippy on Linux and Windows for `97ebd67d4470dc72000430b41d9686940c72c3a2`. `counter_overflow_is_rejected_before_any_persistent_write` covers transaction-ID and checkpoint-generation overflow; `recovery_counter_overflow_is_rejected_before_replaying_home_blocks` confirms recovery rejects exhausted checkpoint generation before replay.
- Exact-SHA run [38460415085](https://github.com/Likounsee/OpenFS/actions/runs/38460415085) passed formatting, workspace tests and Clippy on Linux and Windows for `3c8efe9f8eae716412634e8b91ba2831d259ab85`. `counter_overflow_is_rejected_before_any_persistent_write` now also covers record-sequence overflow before the first persistent write; `recovery_sequence_overflow_is_rejected_before_replaying_home_blocks` confirms sequence exhaustion is rejected before home-block replay and leaves home bytes unchanged. The preceding implementation commit `5d19382d525b20c0283262b4f98b83de4982ec75` was corrected for rustfmt before this passing run; its first CI attempt failed only the formatting check.
- Exact-SHA run [38460922848](https://github.com/Likounsee/OpenFS/actions/runs/38460922848) passed formatting, `cargo test --workspace` and Clippy on Linux and Windows for `87947011d4ef2cf51b2ef025d0bc042fc116d946`. `every_commit_io_failure_recovers_to_a_complete_home_image` injects failures at all ten I/O operations for a one-block 4 KiB transaction and checks that reopen yields the old image before a written COMMIT, or the fully committed image once the COMMIT write is present. This software image test does not model a physical device that loses acknowledged writes or lies about flush.
- Descriptor publication and journal recovery now have separate injected staging-write, staging-flush, journal-commit-flush, uncommitted-transaction, torn-record, replay-failure, protected-target and counter-overflow regression coverage. The crash-cut matrix is still not exhaustive across every device/flush boundary, malformed-ring arrangement and real storage failure mode; a normal CI pass is not physical durability evidence.
- No physical USB device was accessed and no local commands were run. All CI claims here refer to remote GitHub Actions evidence and the exact SHA stated above.

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
**Status: Partially implemented; validation advancing — 86%. Weight: 15%.**

- **Current P1 work (2026-10-14):** [descriptor publication and ownership contract](P1_DESCRIPTOR_PUBLICATION_CONTRACT.md) documents descriptor states, the bitmap/journal commit point, ownership invariants, uncertain-I/O handling and required crash-cut evidence. Commit [`63b84cc`](https://github.com/Likounsee/OpenFS/commit/63b84cc695c5626e0f8a49896c9de636bb8d12d8). Regression `refused_descriptor_publication_preserves_exact_image_bytes_across_block_sizes` was added in [`70acfe1`](https://github.com/Likounsee/OpenFS/commit/70acfe1963eb78b3be3f4ccc8d90d44b40074b34); it checks exact image-byte preservation after rejected publication at all five block sizes. At this roadmap edit, execution and exact-SHA CI for that test are **not yet confirmed**, so it earns no validated progress credit.
- **Format compatibility policy (2026-10-14):** [FORMAT_COMPATIBILITY_POLICY.md](FORMAT_COMPATIBILITY_POLICY.md) now specifies immutable released versions, fail-closed handling of unknown versions/features, deterministic encoding, explicit conversion, recovery compatibility and golden-fixture acceptance criteria. Commit [`6abf422`](https://github.com/Likounsee/OpenFS/commit/6abf422d8cee32a5b16661a44084fe3ba5cc3d10). This is a written policy only; binary golden fixtures, compatibility tests and conversion remain unimplemented and receive no completion credit.
- **Descriptor format regression (2026-10-14):** `descriptor_has_stable_golden_bytes_and_checksum` now pins the 4 KiB descriptor header, CRC32C (`0xd4b32501`), zero padding and decode result. Commit [`2d1f9fd`](https://github.com/Likounsee/OpenFS/commit/2d1f9fd2a6c2ea5cf93ac7ec927cfc1df8a89e1f). This adds a stable descriptor-byte fixture in test source, not a complete golden volume image; no execution or exact-SHA CI result is confirmed yet.
- **Cross-list alias rejection regression (2026-10-14):** the fixture uses record counts consistent with both one-block lists and expects the descriptor codec's duplicate-physical-block rejection before any disk writes. Commit [`4e2d25f`](https://github.com/Likounsee/OpenFS/commit/4e2d25fda337b8bd91e424f85497b7f45ab306a9); exact-SHA CI run [38528841110](https://github.com/Likounsee/OpenFS/actions/runs/38528841110) passed on Linux and Windows. The test compares the complete image before/after and verifies that no descriptor becomes discoverable.
- **Invalid-inventory publication regression (2026-10-14):** commit [`746b3bd`](https://github.com/Likounsee/OpenFS/commit/746b3bd55034d885deeabab0761b5f3a385ac334) checks that an inventory whose referenced metadata block has no valid metadata record is rejected without changing a single image byte, consuming a free block, or making a descriptor discoverable, at all five supported block sizes (4, 8, 16, 32 and 64 KiB). Exact-SHA CI run [38529074047](https://github.com/Likounsee/OpenFS/actions/runs/38529074047) passed on Linux and Windows.
- **Sparse and duplicate descriptor discovery regressions (2026-10-14):** the sparse-volume test places a descriptor on the final data block and checks discovery across all five block sizes; the duplicate-descriptor test verifies fail-closed behavior when two allocated descriptors are present. Commits [`c946b6a`](https://github.com/Likounsee/OpenFS/commit/c946b6aa420d0f5ee2e2e9e243ab8169696c03e6), [`6fcf649`](https://github.com/Likounsee/OpenFS/commit/6fcf6494433517df49963b10f92a17dd499c75bc) and [`d81cd1e`](https://github.com/Likounsee/OpenFS/commit/d81cd1e16f3467d4b110432ed2e0bd58de72f40d). Exact-SHA CI run [38529717836](https://github.com/Likounsee/OpenFS/actions/runs/38529717836) passed formatting, workspace tests and Clippy on Linux and Windows.

Implemented:
- Canonical layout calculation with checked geometry and supported block sizes from 4 KiB through 64 KiB.
- Primary and backup superblocks; bitmap, checkpoint copies and four-block redo ring; reserved/padding bitmap validation.
- Allocation/free, exhaustion, double-free rejection, reuse after reopen, free-space and free-extent queries, contiguous allocation/free helpers and fault injection.
- Journaled bitmap allocation changes and layout/bitmap corruption tests.
- Persistent metadata/directory codecs, extent-map page codec and ownership verifier foundations.
- Persistent inventory descriptor publication/discovery, bounded batched reads and fail-closed handling for malformed, duplicated or unallocated references.
- Descriptor publication stages the encoded descriptor in a free data block and flushes it before journaling the allocation bitmap. The bitmap commit is the visibility point: discovery scans allocated blocks, so an uncommitted staged descriptor remains invisible after reopen. Tests fail each journal/home/checkpoint write position and each flush boundary across all five supported block sizes, requiring recovery to either the old state or the complete descriptor. Torn-prefix writes to the staging block are tested at five prefix lengths per block size; the descriptor remains invisible, and reuse clears stale descriptor magic.
- Allocator recovery regression: commits [`1eaf85ed7fa001249666c57f7947d1b58f0d3d13`](https://github.com/Likounsee/OpenFS/commit/1eaf85ed7fa001249666c57f7947d1b58f0d3d13) and [`32984d4e12dd46339fa48adc7d1f893eab93aa7c`](https://github.com/Likounsee/OpenFS/commit/32984d4e12dd46339fa48adc7d1f893eab93aa7c) cover stale descriptor staging through single, batch, contiguous and best-fit allocation, plus a simulated scrub-flush failure that must leave the bitmap allocation free and allow a safe retry. CI run [38448265235](https://github.com/Likounsee/OpenFS/actions/runs/38448265235) passed Linux and Windows for the fail-closed regression test.
- Persistent descriptor lifetime: commits [`09c169de84876e6fb71f4aff901f1860e32700de`](https://github.com/Likounsee/OpenFS/commit/09c169de84876e6fb71f4aff901f1860e32700de), [`780fd760a1ff226197a7729da395029582b46077`](https://github.com/Likounsee/OpenFS/commit/780fd760a1ff226197a7729da395029582b46077), [`ba9e006db0364b2f497925bba76b1bb7c334a319`](https://github.com/Likounsee/OpenFS/commit/ba9e006db0364b2f497925bba76b1bb7c334a319) and [`c8ee192bf14f62e943c360f595ba8a169619786c`](https://github.com/Likounsee/OpenFS/commit/c8ee192bf14f62e943c360f595ba8a169619786c) prevent the generic single, batch and contiguous free APIs from freeing a block containing the persistent inventory descriptor; a five-block-size regression checks that the descriptor remains allocated and discoverable. Exact-SHA CI run [38448632465](https://github.com/Likounsee/OpenFS/actions/runs/38448632465) passed Linux and Windows formatting, workspace tests and Clippy.

Open blockers:
- The initial descriptor publication now has an explicit visibility point and tested old-or-complete recovery for journal write failures, flush failures, and torn-prefix staging writes. This does not prove physical-device power-loss behavior and does not solve general multi-object transactions.
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
- Commit preflights transaction ID and checkpoint generation before its first write; recovery preflights sequence, transaction ID and checkpoint generation before replay. Tests cover commit-side transaction-ID, checkpoint-generation and record-sequence overflow, plus recovery-side generation and record-sequence overflow with home-byte preservation.
- Exact-SHA Rust CI run [38460415085](https://github.com/Likounsee/OpenFS/actions/runs/38460415085) passed formatting, workspace tests and Clippy on Linux and Windows for `3c8efe9f8eae716412634e8b91ba2831d259ab85`.
- Exact-SHA Rust CI run [38460922848](https://github.com/Likounsee/OpenFS/actions/runs/38460922848) passed formatting, workspace tests and Clippy on Linux and Windows for `87947011d4ef2cf51b2ef025d0bc042fc116d946`; it verifies all ten injected commit I/O failure points for a single 4 KiB home-block update and checks old-or-committed home bytes after reopen.

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
**Status: Partially implemented, read-only only — 21%. Weight: 20%.**

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
7. **P4.7 Semantics — Partially implemented in memory only.** Path resolution, rename/remove validation and case-sensitivity behavior have in-memory coverage; durable on-disk semantics and the full first-release contract remain undefined. Specify path normalization, absolute/relative paths, trailing separators, name encoding, timestamps, permissions and file type behavior.
8. **P4.8 Read API exists; durable write API not implemented.** Range/content/path reads and shared errors exist, but there is no complete durable mutation API or handle lifecycle. Distinguish EOF, no-space, corruption, unsupported operation and I/O failure.
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

1. **P1/P2 first deliverable — descriptor commit/recovery contract:** write down the descriptor states and single commit point; specify allocation ownership before/after publication; define how open behaves after any write or flush error. Do not add persistent mutation APIs before this contract is reviewed.
2. **P1/P2 proof step:** add a fault-injection case for every descriptor/journal write and flush boundary, preserve the exact image bytes, remount after each cut, and assert one unambiguous descriptor with no unexplained allocation. Include repeated recovery and the five supported block sizes; record test names and exact-SHA Linux/Windows results. If a case cannot establish old-or-new state, mark it blocked rather than increasing progress.
3. **P2 follow-up:** expand persisted-byte crash cuts for every journal write, flush, checkpoint and replay failure, including repeated recovery; keep Linux/Windows CI green on exact SHAs.
4. **P3:** finish read-only diagnostics and malformed-input corpus, then add bounded fuzz targets.
5. **P4:** define a canonical metadata root/index and implement a minimal durable vertical slice: create file, write bytes, reopen, read bytes, check ownership. Add mkdir/rename/unlink only after this path is transactional.
6. **P3/P4:** build whole-volume verification from the canonical root and add explicit repair design only after checker coverage.
7. **P5:** build mocked read-only device discovery/refusal logic; destructive formatting remains unavailable until the data-integrity gates pass.
8. **P6:** choose the Windows adapter and integrate only against the stable persistent API; then Linux and ArchiaOS.
9. **P7:** add stress/fuzz/benchmark/release validation after correctness gates, not in place of them.

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


## Reproducible task-weight ledger (source of truth for percentages)

The ledger assigns 100 normalized points within every phase. “Weight” is task importance within its phase; “earned” is the current evidence-based estimate, not a claim that all task acceptance criteria pass. Task IDs refer to the numbered tasks in the phase sections above. Where a phase's narrative includes extra cross-cutting work, it is included in the closest task and must not be scored a second time.

| Phase/task | Task weight / 100 | Earned points | Current evidence and reason for credit |
| --- | ---: | ---: | --- |
| P0.1 Workspace, crate boundaries and error model | 25 | 25 | Workspace manifests and public crate sources exist; the architecture builds in the tested workspace. |
| P0.2 Bounded block I/O and device abstractions | 20 | 20 | `openfs-core` and `openfs-block`; image/raw-file device and fault-injection implementations. |
| P0.3 Format encoding and geometry validation | 20 | 20 | `openfs-format/src/lib.rs`, `volume.rs`; format/open and malformed geometry tests. |
| P0.4 Non-destructive development image behavior | 15 | 15 | `openfs-tool/src/main.rs`; overwrite-refusal and size-boundary tests. |
| P0.5 Cross-platform baseline CI | 10 | 10 | Exact-SHA Linux and Windows formatting, workspace tests and Clippy pass in run 38379103899. |
| P0.6 Contributor safety/evidence contract | 10 | 10 | Existing development docs and this roadmap's explicit evidence policy; reopen if a concrete gap is found. |
| **P0 total** | **100** | **100** | **100%** |
| P1.1 Format geometry, superblocks and reserved areas | 20 | 18 | Format/open validation and layout tests exist; golden fixtures and stable format policy remain missing. |
| P1.2 Allocation/free and persistence across reopen | 20 | 19 | Allocation/free APIs and reopen tests in `volume.rs`; broader crash-cut coverage remains incomplete. |
| P1.3 Cross-structure block ownership | 15 | 11 | Inventory/extent ownership validation exists; regressions `discovered_verification_rejects_file_data_aliasing_descriptor_at_every_block_size`, `caller_supplied_inventory_rejects_metadata_directory_block_alias_at_every_block_size`, `caller_supplied_inventory_rejects_file_data_aliasing_metadata_at_every_block_size`, and `discovery_rejects_descriptor_aliasing_its_inventory_at_every_block_size` cover four ownership collisions at all five supported block sizes. Exact-SHA CI run [38451134612](https://github.com/Likounsee/OpenFS/actions/runs/38451134612) passed Linux and Windows for the first two; run [38451666041](https://github.com/Likounsee/OpenFS/actions/runs/38451666041) passed Linux and Windows for metadata/file-data alias rejection; run [38452124961](https://github.com/Likounsee/OpenFS/actions/runs/38452124961) passed Linux and had Windows validation still in progress at this audit for descriptor/inventory self-alias rejection. Whole-volume canonical-root traversal and all ownership transitions do not. |
| P1.4 Persistent descriptor publication/recovery | 20 | 16 | Discovery, ambiguity rejection, failed-write recovery, stale-stage cleanup across all four allocation paths, and protection from generic free APIs exist; safe descriptor replacement protocol is absent. |
| P1.5 Scalable metadata locator | 15 | 12 | Descriptor scan works, including batched reads, but scans the data region and is not a final scalable index. |
| P1.6 Golden images and format compatibility policy | 10 | 7 | Compatibility notes exist; fixtures, version-negotiation tests and conversion are absent. |
| **P1 total** | **100** | **83** | **83%** |
| P2.1 Journal state machine and error/poison contract | 15 | 10 | Redo/checkpoint sequencing and protected-target validation exist; no general transaction API/error contract. |
| P2.2 Write/tear fault-injection harness | 20 | 14 | Fail/tear device wrappers and multiple prefix tests exist; exhaustive write/flush cut matrix is absent. |
| P2.3 Replay, checkpoint and idempotence | 20 | 12 | Selected torn commit/home/checkpoint and repeated recovery tests exist; coverage is not exhaustive. |
| P2.4 Transaction capacity and journal expansion design | 20 | 0 | Four-block ring remains bounded; no general capacity design or implementation. |
| P2.5 Atomic namespace/file transactions | 20 | 0 | Persistent create/write/rename/delete are not implemented. |
| P2.6 Process/OS/power-loss durability contract | 5 | 4 | Documentation distinguishes software tests from hardware durability; physical evidence is absent. |
| **P2 total** | **100** | **40** | **40%** |
| P3.1 Read-only structural volume check | 20 | 12 | `check_volume` and CLI check exist; only the covered corruption cases are evidenced. |
| P3.2 Full persistent object traversal | 20 | 8 | Inventory-based verification exists, but canonical root discovery/full-volume traversal is blocked. |
| P3.3 Stable diagnostics and machine-readable report | 15 | 5 | Reports/errors identify some anomalies; stable diagnostic-code contract is not complete. |
| P3.4 Bounded parser fuzz targets and corpus | 15 | 0 | No Rust fuzz target/CI budget was found in the audited tree. |
| P3.5 Explicit repair plan/apply protocol | 15 | 0 | No repair utility or dry-run/apply protocol found. |
| P3.6 Data-checksum policy and validation | 10 | 5 | The limitation is documented; no general persistent file-data checksum scheme is implemented. |
| P3.7 Corruption regression coverage | 5 | 5 | Multiple format, bitmap, journal, descriptor and namespace corruption regressions exist; coverage remains bounded to those cases. |
| **P3 total** | **100** | **35** | **35%** |
| P4.1 Canonical metadata root/locator | 10 | 4 | Persistent descriptor and discovery are a starting point, not a canonical scalable root/index. |
| P4.2 Read-only reconstruction from disk | 10 | 5 | Descriptor-driven inventory reads and namespace reconstruction exist; no durable mount lifecycle. |
| P4.3 Durable create/mkdir | 15 | 0 | In-memory namespace insertion is not durable creation. |
| P4.4 Durable write/truncate | 20 | 0 | File range/content reads exist; persistent writes and truncation do not. |
| P4.5 Durable rename/unlink/rmdir | 15 | 0 | In-memory namespace operations exist; persistent mutations do not. |
| P4.6 Directory growth and large inventory support | 10 | 0 | No durable directory-growth path found. |
| P4.7 In-memory namespace/path semantics | 10 | 5 | In-memory path resolution, rename/remove validation and case-sensitivity tests exist; durable semantics remain undefined and unintegrated. |
| P4.8 Read APIs and shared errors | 5 | 5 | `read_file_range`, `read_file_contents` and `read_path_range` plus shared errors exist; no durable write/mutation API or complete handle lifecycle exists. Credit applies only to the implemented read surface. |
| P4.9 Persistent end-to-end mutation tests | 5 | 2 | Image-backed reconstruction/read tests exist, but create-write-close-reopen-read mutation tests cannot pass before mutation exists. |
| **P4 total** | **100** | **21** | **21%** |
| P5.1 Read-only device discovery/identity | 20 | 2 | Raw-file backend exists; OS-level removable-device identity/discovery is absent. |
| P5.2 Exclusive access and system-volume refusal | 20 | 0 | No implementation found. |
| P5.3 Destructive-operation dry run | 15 | 0 | No implementation found. |
| P5.4 Explicit confirmation and identity revalidation | 15 | 0 | No implementation found. |
| P5.5 Mocked safety/refusal tests | 15 | 0 | No discovery/refusal workflow to test. |
| P5.6 Dedicated physical-device validation | 15 | 3 | Hardware test requirements are documented, but no hardware execution evidence exists; documentation alone receives only minimal planning credit. |
| **P5 total** | **100** | **5** | **5%** |
| P6.1 First OS adapter selection and contract | 15 | 0 | No Rust mount/VFS adapter found. |
| P6.2 Stable core-to-OS operation mapping | 20 | 0 | Blocked by persistent mutation API. |
| P6.3 Mount/unmount and in-flight operation lifecycle | 15 | 0 | No adapter lifecycle found. |
| P6.4 Windows normal-file-API integration | 20 | 0 | No Windows adapter found. |
| P6.5 Linux adapter and integration tests | 15 | 0 | No Linux adapter found. |
| P6.6 ArchiaOS integration | 10 | 0 | No Rust ArchiaOS VFS/block integration found. |
| P6.7 ARM64 target evidence | 5 | 0 | No target/runner validation evidence found. |
| **P6 total** | **100** | **0** | **0%** |
| P7.1 Format version/feature policy | 15 | 3 | Compatibility constraints are documented; no frozen release contract/golden fixture suite. |
| P7.2 Safe conversion/migration | 10 | 0 | No converter found; remains conditional on whether migration is in scope. |
| P7.3 Stress, scale and resource-limit tests | 20 | 0 | No dedicated stress/scale release gate found. |
| P7.4 Fuzz/sanitizer evidence for Rust | 15 | 0 | No Rust fuzz/sanitizer CI found. |
| P7.5 Performance benchmarks | 15 | 0 | No benchmark gate found. |
| P7.6 Security review and threat-model validation | 15 | 0 | Requirements are listed, but no completed review evidence found. |
| P7.7 Reproducible release/support/recovery artifacts | 10 | 0 | No release-grade evidence found. |
| **P7 total** | **100** | **3** | **3%** |

### Recalculation

The phase weights are P0=10, P1=15, P2=20, P3=10, P4=20, P5=10, P6=10 and P7=5; they sum to 100. Earned phase points in the current provisional ledger are respectively 100, 83, 40, 35, 21, 5, 0 and 3.

`Global = (10×100 + 15×83 + 20×40 + 10×35 + 20×21 + 10×5 + 10×0 + 5×3) / 100 = 38.8` percent, rounded to **39%**.

The arithmetic is reproducible; some partial task awards are not yet fully derivable from criterion-level subweights. Accordingly, **39% is a provisional weighted estimate** until those subcriteria are published. The former P4 estimate of 24% is corrected to **21%** because durable mutation and end-to-end mutation acceptance criteria are absent. P1 is now estimated at 83% as a provisional implementation-progress score after the cross-path stale-descriptor fix and exact-SHA Linux/Windows CI pass; its exit gate remains blocked and it must not be called complete. A documentation-only commit does not change implementation scores.

## Evidence register for implemented work

These references are intended to make important claims quickly auditable without repeating every link in every task. Paths are relative to the repository root.

| Claim | Source/API and representative tests | Change/CI evidence | Limits |
| --- | --- | --- | --- |
| Format and geometry | `OpenFS/crates/openfs-format/src/lib.rs` (`Superblock::encode/decode`); `volume.rs` (`VolumeLayout::calculate`, `format_volume`, `open_volume`, `check_volume`) | Rust CI run [38379103899](https://github.com/Likounsee/OpenFS/actions/runs/38379103899), exact tested SHA `44efa42408258247fdcb99890799f19b8e9c482f` | CI proves the configured automated suite only; not physical-device durability. |
| Allocation, descriptor discovery and failed-publication behavior | `volume.rs`: `allocate_block`, `free_block`, `publish_persistent_inventory`, `discover_persistent_inventory`; `allocator_extents.rs`: `clear_stale_descriptor_magic`, `allocate_blocks`, `allocate_contiguous_blocks`; regression `every_allocation_path_scrubs_stale_descriptor_staging` covers all five block sizes for batch and contiguous allocation. Existing tests include `descriptor_publication_is_atomic_across_every_journal_write_failure`, `descriptor_publication_flush_failures_recover_old_or_complete_state`, `torn_descriptor_staging_writes_remain_invisible_and_safe_to_reuse`, `discovery_fails_closed_on_corrupt_allocated_descriptor`, `discovery_fails_closed_for_corrupt_descriptors_at_every_block_size`; `inventory_descriptor.rs`: `descriptor_codec_round_trips_every_supported_block_size`, `descriptor_decoder_fails_closed_for_header_and_shape_corruption`, `rejects_overlapping_or_mis_sized_inventory_lists_at_every_block_size` | Descriptor publication/corruption regression batch [339945c](https://github.com/Likounsee/OpenFS/commit/339945c907bc0ae5f17aa502436e88934d7cf842), followed by ownership-scan optimization and multi-root regression [c7b93f1](https://github.com/Likounsee/OpenFS/commit/c7b93f1ee77869529d869ce4d3ae15ef6dae52aa) and allocator stale-descriptor fix [9fd6d85](https://github.com/Likounsee/OpenFS/commit/9fd6d857235723f9474109d73666be89994962f6); exact-SHA CI run [38439682402](https://github.com/Likounsee/OpenFS/actions/runs/38439682402) passed Linux and Windows (formatting, workspace tests, Clippy) on `9fd6d857235723f9474109d73666be89994962f6`. The new regression test passed on Linux and Windows; extent allocation validation retains one bitmap page while scanning sorted ranges and root alias checks use binary search. Earlier descriptor-inventory validation retains its separate eight-page bounded cache. | Covers software-level descriptor corruption and selected write/flush cuts across supported block sizes; does not prove physical-device power-loss behavior, provide automatic orphan repair, safe descriptor replacement, or remove the full-data-region discovery scan. |
| Journal/checkpoint recovery | `OpenFS/crates/openfs-format/src/journal_engine.rs`: `PersistentJournal::commit`, `open_journal_protected`, `validate_checkpoint_geometry`; tests `replays_durable_commit_if_home_write_fails`, `ignores_uncommitted_transaction_after_commit_write_failure`, `torn_terminal_commit_record_is_ignored_and_home_data_is_preserved`, `torn_home_write_is_repaired_from_durable_redo_record`, `torn_checkpoint_write_recovers_from_durable_redo_for_multiple_prefixes`, `recovery_io_failure_can_be_retried_without_losing_committed_redo`, `every_commit_io_failure_recovers_to_a_consistent_home_block`, `open_rejects_persisted_checkpoint_sequence_gap`, `protected_recovery_validates_all_targets_before_replaying_any_update`, `malformed_nonterminal_record_before_later_valid_records_fails_closed`, `counter_overflow_is_rejected_before_any_persistent_write`, and `recovery_counter_overflow_is_rejected_before_replaying_home_blocks` | Historical checkpoint validation/torn-checkpoint/reuse commits remain listed above; exact-SHA CI run [38456414882](https://github.com/Likounsee/OpenFS/actions/runs/38456414882) validates journal commit-flush recovery on Linux and Windows; run [38457627241](https://github.com/Likounsee/OpenFS/actions/runs/38457627241) validates all-target prevalidation before replay; run [38458063369](https://github.com/Likounsee/OpenFS/actions/runs/38458063369) validates malformed interior record rejection; run [38459392863](https://github.com/Likounsee/OpenFS/actions/runs/38459392863) validates counter-overflow preflight on Linux and Windows. | Selected injected I/O cuts and image-backed recovery tests only; not a general namespace transaction protocol and not physical power-loss evidence. |
| In-memory namespace and path invariants | `OpenFS/crates/openfs-format/src/namespace.rs`: `Namespace::from_records`, `resolve_path`, `insert`, `rename`, `remove`, `validate`; tests for duplicate names, rename/move, removal, generation overflow, traversal and index consistency | Code CI run 38379103899 passed the full workspace at exact code SHA 44efa42408258247fdcb99890799f19b8e9c482f; latest roadmap-only CI run 38423229047 passed at SHA 4b0ee02b974ceecc10379378b72d20e168d4d6bf | In-memory correctness does not prove durable namespace mutations. |
| Read paths and read-only checker | `volume.rs`: `read_file_range`, `read_file_contents`, `read_path_range`, `check_volume`; CLI `OpenFS/crates/openfs-tool/src/main.rs`: `check_image`; test `checks_current_image_without_modifying_it` | Code CI run 38379103899 passed on exact SHA 44efa42408258247fdcb99890799f19b8e9c482f; latest roadmap-only CI run 38423229047 passed on exact SHA 4b0ee02b974ceecc10379378b72d20e168d4d6bf | Does not provide whole-volume repair, durable write/truncate or mount integration. |
| Cross-structure ownership verifier | `OpenFS/crates/openfs-format/src/verifier.rs`: `verify_discovered_persistent_inventory` composes descriptor, inventory, extent-root, file-data and bitmap ownership; regression `discovered_verification_rejects_file_data_aliasing_descriptor_at_every_block_size` creates a real on-image alias and requires fail-closed `Error::Corrupt` at 4096, 8192, 16384, 32768 and 65536-byte blocks. | Test commit [51abae3](https://github.com/Likounsee/OpenFS/commit/51abae381e8dbb108dfb84e9eb3c5a29cf90bb26); exact-SHA CI run [38450124058](https://github.com/Likounsee/OpenFS/actions/runs/38450124058) passed formatting, workspace tests and Clippy on Linux and Windows. | Software image regression only; no physical-device power-loss or end-to-end durable namespace mutation evidence. |\n| Cross-platform CI contract | `.github/workflows/ci.yml`: formatting, `cargo test --workspace`, Clippy with warnings denied on Linux and Windows | Latest code SHA `c7b93f1ee77869529d869ce4d3ae15ef6dae52aa`: [run 38437650554](https://github.com/Likounsee/OpenFS/actions/runs/38437650554), both jobs success. | CI validates only the configured automated checks. No fuzzing, sanitizer, benchmark, mounted OS integration or physical hardware job. |

Evidence vocabulary is mandatory: **Implemented** means code/API exists; **Integrated** means connected to the intended end-to-end path; **Test added** means a matching test exists in source; **Test executed** means a recorded runner actually ran it; **CI validated** means the relevant job succeeded on the exact cited SHA; **Non vérifié** means evidence does not cover the claim; **Bloqué** means a safety prerequisite or unresolved defect prevents progression. For historical commits whose exact run cannot be retrieved, treat the run reference above as historical recorded evidence and do not extrapolate its result to later SHAs.

## Dependency matrix (P1–P7)

| Blocked phase/task | Required dependency | Technical reason | Objective unblock condition and proof | Phases unblocked |
| --- | --- | --- | --- | --- |
| P1.3–P1.4 ownership and descriptor lifecycle | P2.1 transaction state machine plus explicit descriptor/root ownership invariants | Allocation can commit before descriptor publication; failed publication can leave a detectable allocated orphan, and replacement cannot safely choose between old/new descriptors without a commit point. | Written state machine and on-disk ownership contract; injected failure at each write/flush boundary; reopen yields one unambiguous committed descriptor and no unexplained allocation. | P4 durable root and mutations; P3 full traversal. |
| P2.2–P2.5 transaction/recovery | P2.1 error/commit contract and P1 protected layout | A journal with a bounded ring and selected recovery tests cannot safely provide multi-object atomicity until capacity, replay and uncertain-I/O behavior are specified. | Crash-cut matrix for every journal/flush/checkpoint/replay boundary; repeat replay; compare persisted bytes and require old-or-new state only; prove writes are refused after indeterminate recovery. | P4 mutations; later P6 adapter. |
| P4.1–P4.2 canonical root and reconstruction | P1 descriptor lifecycle plus P2 commit protocol | Reads currently depend on inventory/descriptor structures and cannot establish a durable, scalable root locator under mutation. | Versioned root/index discoverable without caller-supplied lists; reopen reconstructs identical graph; malformed pointers fail closed; large/sparse image test. | P3 whole-volume verifier; P4 durable mutations; P6 API adapter planning. |
| P4.3–P4.6 durable file/namespace operations | P4.1 root, P2 atomic transaction semantics, P1 ownership/allocation contract | Create/write/truncate/rename/delete must update object records, directory entries, extent maps and allocation ownership atomically. | For each operation, injected failure at each persistent step; reopen is old or new complete state; checker finds no alias, dangling pointer or leak. | P3 full traversal; P6 OS integration. |
| P3.2–P3.5 full verification/repair | P4.1 root and defined ownership/reference model; checker remains read-only | Without a canonical root, a full traversal cannot find all objects. Repair before reliable diagnostics and ownership reconstruction risks making corruption worse. | Read-only scan covers every reachable object; deterministic diagnostics and byte-for-byte image preservation; repairs, if later added, require explicit preview/apply and post-check. | P5 safety confidence; P7 release gate. |
| P5.1–P5.5 safe discovery/format | P4 durable API for useful use; OS-specific identity/partition APIs and destructive safety policy | A raw path is not proof of a removable USB identity; writing can overwrite system/partition data. | Mocked tests refuse mounted/system/ambiguous/changed devices; revalidate stable identity and geometry immediately before confirmation and first write. | Physical test stage and P7 operational release, not core image development. |
| P6.1–P6.6 OS integration | P4 durable APIs and error semantics; P2 flush/uncertain-I/O contract; P3 diagnostics for failure handling | OS callbacks need stable semantics for file operations, flush, shutdown, errors and failed recovery. | Normal OS file API integration tests for create/read/write/rename/delete, clean unmount/remount and byte verification on the claimed OS. | P7 release qualification for that OS. |
| P7.1–P7.7 compatibility and release | P1 format/version contract, P2 recovery evidence, P3 checker, P4 functional API, P5/P6 claimed platform validation | A release cannot promise compatibility, data safety or platform support from source presence or image-only unit tests. | Versioned golden images; compatibility/rejection tests; stress/fuzz results; recovery guide; supported-platform matrix; no unresolved data-loss blocker. | Public/production release. |

The dependencies are gates, not a requirement to stop all parallel work: tests, fuzz harness design, diagnostics design, OS-adapter research and mocked discovery can proceed in parallel as long as they do not expose unsafe mutation or claim unsupported behavior.

## First usable filesystem milestones

These milestones are acceptance gates, not assertions that the milestone has already been reached.

| Milestone | Status at this audit | Acceptance criteria | Required evidence |
| --- | --- | --- | --- |
| **A — Format and recovery reliability** | **Partially achieved; not complete** | Format/reopen images; allocate/free/reopen without leaks; reject corrupt layouts; recover deterministically at every covered interruption. | Image-backed regression suite, exact-SHA Linux/Windows CI, exhaustive persisted-byte crash-cut matrix. |
| **B — First persistent file** | **Not achieved; blocked** | Create file, write known bytes, close/reopen, read identical bytes; ownership checker finds no leaks/aliases. | End-to-end image test with injected failure at each commit boundary and reopen verification. |
| **C — Complete namespace operations** | **Not achieved; blocked** | mkdir, empty/fragmented files, rename, truncate and delete; no-space and interruption behavior is coherent. | Persistent operation tests, crash matrix, checker pass after every scenario. |
| **D — Verification and reliability** | **Partially achieved; incomplete** | Traverse all persistent objects, stable diagnostics, malformed-image corpus, bounded fuzzing; repairs explicit and never silent. | Read-only image preservation, deterministic reports, fuzz corpus/regression fixtures, separate repair dry-run/apply tests if repair is implemented. |
| **E — OS integration and normal use** | **Not achieved; blocked** | Stable persistent API and first OS adapter; ordinary OS file APIs; clean unmount/remount with bytes preserved. | Integration test against real OS filesystem APIs on every claimed platform. |
| **F — Physical device and publication** | **Not achieved; blocked** | Safe removable-device identity, refuse system/mounted/ambiguous targets, explicit confirmation, dedicated disposable USB test, support/compatibility matrix. | Mocked refusal tests plus recorded hardware test on dedicated media; never infer physical durability from image tests. |

## Final consistency checks for roadmap maintenance

Before changing any percentage or status in a future update:
1. Confirm branch and exact HEAD; inspect the task's current source, tests and workflow.
2. Update the earned points only for acceptance criteria whose evidence changed; keep the task weight fixed unless the scope changes and the reason is documented.
3. Recompute every phase as earned points / 100 and the global value using phase weights; never edit the displayed total by intuition.
4. Verify every cited CI run's `head_sha` matches the claim. Label code-validation runs separately from documentation-only runs; the latter do not create implementation evidence.
5. Verify dependency gates and milestone status against the actual implementation, not just task completion percentages.
6. Review the diff and ensure this roadmap-only mission did not change filesystem source, tests, scripts or workflow code.


## Scoring ledger: limitation and next scoring pass

The task-level ledger is the current source for the published **provisional** percentages, but it is not yet a fully mechanical score: non-zero partial task awards remain expert estimates based on the evidence summaries in their rows. Do not call the 39% value fully reproducible. Do not invent criterion weights retroactively merely to preserve it.

Before claiming a mechanically reproducible score, replace each non-zero partial task row with named acceptance criteria and fixed subweights. For every criterion record: stable ID; weight within the task; expected behavior; source/API path; regression test name; whether the test was added, executed and passed; exact CI run and tested SHA where applicable; earned credit; and a one-line justification. Subweights must sum exactly to the parent task weight, and earned criterion points must sum to the parent earned points. Each acceptance criterion may be credited once only. Keep absent or unverified behavior at zero; recalculate phase and global percentages from the criterion ledger and disclose any change from 39%.

Until that work is done, retain the existing values only as provisional estimates and use the exit gates—not percentages alone—to decide whether a phase can advance. This revision fixes status/evidence inconsistencies but does not claim that the score has become mechanically reproducible.
