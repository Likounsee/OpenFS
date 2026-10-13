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

- Repository architecture cleanup: there is now one active volume implementation in `crates/openfs-format/src/volume.rs`; public entry points use `format_volume`, `open_volume`, and `check_volume`. The parallel version-named module and obsolete standalone-layout integration tests were removed so the test suite targets the canonical journaled layout. Contributor/module guidance is in [DEVELOPMENT.md](DEVELOPMENT.md). The on-disk format identifier remains validated for fail-closed compatibility; it does not imply multiple active implementations.

- Latest P1 I/O follow-up bounds inventory reads to runs of at most 64 contiguous blocks, preserves caller-defined record order for fragmented inventories, and caches bitmap pages while validating inventory reads, descriptor discovery, and descriptor publication. This reduces redundant block-device operations without changing the on-disk format or weakening allocation checks. Existing image-backed publication/reconstruction/discovery regressions cover these paths; formatting, workspace tests, and Clippy have not been executed in this environment, so this batch is not counted as newly CI-validated.

- Latest P1 batch strengthens `OpenedVolume::publish_persistent_inventory`: before allocating a descriptor block, it reconstructs and validates the proposed namespace, validates all referenced extent-map pages and physical ownership, and refuses to publish over orphaned allocations or dangling ownership references. A regression checks that invalid inventory is rejected without consuming a block. CI for code commit [06e13fe0](https://github.com/Likounsee/OpenFS/commit/06e13fe0b053f896cf1ee769f28870c57a4610e9) is running: [38361083979](https://github.com/Likounsee/OpenFS/actions/runs/38361083979). The descriptor and allocation are still not one atomic journal transaction: the current four-block ring cannot hold the two full-block redo images in a single transaction, so a crash between allocation and descriptor write can leave a detectable orphan. Remaining: safe descriptor replacement, journal-aware namespace/file mutations, broader crash-cut coverage, and scalable discovery beyond scanning allocated blocks.

- P1 end-to-end verification batch adds public `OpenedVolume::verify_persistent_inventory` and `PersistentInventoryReport`. It composes the existing disk-backed record readers, namespace graph validation, extent-map page decoding, and bitmap ownership reconciliation; inventory blocks and extent roots are included as owners, so index/data aliases are not misreported as leaks. Fatal structural/corruption errors remain `Err`, while bounded deterministic orphan/dangling diagnostics are returned in a successful report. Image-backed regressions cover a consistent root-only volume and a recoverable orphan allocation. This is a caller-located verifier, not automatic discovery: canonical layout still has no canonical inventory-region descriptor. CI must pass on the final commit before counting this batch as validated.

- P1 persistent file-read batch adds `OpenedVolume::read_file_range`, `read_file_contents`, and `read_path_range`. Reads operate on reconstructed disk metadata and loaded extent maps, handle fragmented extents and sparse holes, clamp reads at EOF, reject directories/missing objects/missing maps, and recheck the bitmap immediately before exposing each physical block. Image-backed regressions cover whole-file and path-based range reads, sparse zero-fill, partial final blocks, EOF, invalid objects, and freed data extents. This is actual persistent data reading, but remains read-only and caller-located; it is not yet a persistent metadata discovery or mutation layer. CI must pass on the final commit before counting this batch as validated.

- P1 disk-backed reconstruction batch adds `OpenedVolume::read_metadata_inventory`, `read_directory_inventory`, and `reconstruct_namespace`. The caller supplies physical block lists and record counts; OpenFS reads the packed records from the image, checks exact block-count arithmetic, data-region bounds, allocation bitmap state, duplicates and cross-inventory aliases, decodes record checksums, then validates the complete namespace graph. Image-backed regressions cover nested path resolution and rejection of free, reserved, repeated, under-sized and aliased inventory blocks. Implementation: [88961fb3](https://github.com/Likounsee/OpenFS/commit/88961fb313132f4e575a4fdcae2cae9209ed1288). Namespace reconstruction also validates extent-map root pointers against data-region bounds and the persistent bitmap, rejects roots that alias inventory blocks, and exposes `validate_namespace_extent_roots` for standalone checking. It still does not decode extent-map pages. This remains caller-located: no canonical metadata region or automatic discovery exists. CI must pass on the final code/documentation commit before this batch is counted as validated.

- P1 extent-page reconstruction batch adds `Namespace::objects`, `OpenedVolume::read_extent_map`, and `read_namespace_extent_maps`. It decodes checksummed extent-map pages from the caller-located root blocks, validates each map against the file's byte size and the volume data region, verifies that roots and every referenced data block are allocated, and rejects a map that claims its own root, any namespace extent-map root, or physical data already owned by another file. The new image-backed tests cover successful load, freed-data rejection, and cross-file physical alias detection. This advances disk-backed ownership validation, but namespace inventories are still caller-located and this is not a durable read/write filesystem path. CI must pass on the final commit before counting this batch as validated.

- P1 persistent-record preparation batch adds `decode_metadata_records` and `decode_directory_entries`: strict packed-byte decoders that reject partial records and validate each fixed-width record's checksum and semantic fields. `Namespace::from_records` rebuilds the in-memory namespace from decoded metadata and directory-entry inventories, rejects duplicate IDs/names, and validates root, parent, reachability, and graph consistency before returning. Regression tests cover successful nested reconstruction and malformed inventories. Commits: [a13bbc70](https://github.com/Likounsee/OpenFS/commit/a13bbc70e9cc41ad168636f3fccfaa3ec4e1f112), [412cad5d](https://github.com/Likounsee/OpenFS/commit/412cad5d84d7a1d14a07991119e7a642901c7221), [97cd8785](https://github.com/Likounsee/OpenFS/commit/97cd878591987a05c2f3dbc03181fb3d0c675464), [65ebdc51](https://github.com/Likounsee/OpenFS/commit/65ebdc51c458a4cc59fe1c782342b66efa711d0d). This is a reconstruction/codec layer only: no volume region is yet assigned to these inventories, so automatic on-disk discovery and ownership checking remain unimplemented. CI must pass on the final documentation commit before this batch is counted as validated.

- Namespace ownership follow-up adds `validated_extent_root_inventory(data_start, data_end)` and `extent_root_owner_count()`: decoded in-memory namespace records can now expose sorted extent-map root blocks, reject roots outside the allocatable region, and detect multiple objects claiming one root. `Namespace::validate()` also rejects duplicate extent-root ownership without requiring volume bounds. Tests cover ordering, bounds, duplicate roots, and owner counts. Code commits: [b5041bc6](https://github.com/Likounsee/OpenFS/commit/b5041bc66c34d9b036d40e26d48b8b11257568d0), [02d5480b](https://github.com/Likounsee/OpenFS/commit/02d5480b6ec535443db0e5d95a260bfebd099a69); README: [4c339020](https://github.com/Likounsee/OpenFS/commit/4c339020a78b5cf88069ac09f2351b748e7b24f1). This bridges in-memory metadata to ownership accounting but does not scan persistent metadata records from disk. CI must pass on the final roadmap commit before counting this batch as validated.

- Latest P1 ownership-scan batch adds `ExtentMap::validated_physical_block_count`, which validates logical/physical bounds and aliases before reporting storage ownership, and coalesces adjacent physical ranges both within each extent map and across maps before comparing against bitmap bytes. This reduces the ownership scan's range work without expanding ranges into per-block vectors. Regression coverage now checks coalesced ranges across gaps, validated counts, and rejection of invalid bounds. Implementation commits: [f17f23f1](https://github.com/Likounsee/OpenFS/commit/f17f23f156e001ae3036cbd53c7bfe341643a723), [611193b8](https://github.com/Likounsee/OpenFS/commit/611193b8da8226d5997c6a09e1a1434f08ef7369); README: [a00d5ecd](https://github.com/Likounsee/OpenFS/commit/a00d5ecda4b7a53fe639db3c4c92818486322142). CI must pass on the final roadmap commit before this batch is counted as validated. Persistent owner discovery remains the largest P1 gap.

- P1 follow-up now exposes validated physical extent ranges as sorted start/end pairs and compares those ranges directly with bitmap bytes for extent-map ownership reports. It no longer expands every referenced block into a per-block vector for this API; it still detects cross-map physical aliases, orphaned allocations, and references to freed blocks. Regression coverage checks physical-range sorting/bounds and fragmented ownership diagnostics across all five supported block sizes. Implementation commits: [163deb70](https://github.com/Likounsee/OpenFS/commit/163deb704edecced6e935c28aa42c572b5774cbb), [f8847374](https://github.com/Likounsee/OpenFS/commit/f884737408686bd319cf90be4a572c12257aeeed), [7e075c27](https://github.com/Likounsee/OpenFS/commit/7e075c27a0a4d076e7bbb3dce7af4dac6c3dbef3), [686876dc](https://github.com/Likounsee/OpenFS/commit/686876dcc4a8bb0b52ad36d858f13bd9459c110f). Rustfmt correction: [4e962e5f](https://github.com/Likounsee/OpenFS/commit/4e962e5faf978b830f3e335e7c4405add9af12a1); README: [672d405d](https://github.com/Likounsee/OpenFS/commit/672d405d4f229e02d2f82d858cb034ac13f4703f). CI must pass on the final documentation commit before this batch is considered validated. Persistent metadata discovery remains absent.

- P1 ownership-scan follow-up replaces the per-data-block comparison loop with a sorted merge between caller-supplied owner blocks and set bits in bitmap bytes. It still rejects duplicate/out-of-region owners, preserves ascending bounded orphan/dangling diagnostics, and records owner references to freed blocks as dangling. A regression covers orphan and dangling reports across all five supported block sizes. Implementation commit [cbd0d819](https://github.com/Likounsee/OpenFS/commit/cbd0d8198ff69d55fe2a7056801168da33ac29a8), regression commit [81477aed](https://github.com/Likounsee/OpenFS/commit/81477aeddef3a22b840c983304a0fa8c5bcbd3aa), README commit [e75efdcc](https://github.com/Likounsee/OpenFS/commit/e75efdcc6b51844696950f5b5a9ef2e59c7fcc67). CI for this batch must be confirmed on the final documentation commit before counting it as validated. This improves diagnostic scan cost but does not discover owners from persistent metadata.

- Latest batch streams `free_extent_inventory` directly from bitmap bytes, avoiding the intermediate vector of every free block, and changes `find_contiguous_free_blocks` to iterate free bits with byte masks instead of checking every block bit individually. Segment boundaries remain hard run boundaries. Regression coverage verifies fragmented runs, search across bitmap-byte boundaries, skipping allocated prefixes/gaps, sorted non-overlapping extent output, and the existing five-block-size allocator/free cases. The previous atomic contiguous-free batch passed Linux/Windows CI on commit `386a5b31c35a50312d7caa7e85ec419872e75586` (run [38306228478](https://github.com/Likounsee/OpenFS/actions/runs/38306228478)); CI for the current streaming/search batch is pending.

- Previous batch optimizes read-only free-space queries: free-block inventory now scans bitmap bytes and extracts zero bits rather than testing every data block individually; fragmentation statistics detect free runs from byte-level masks and use checked run/count arithmetic. Tests compare free and allocated inventories, per-block allocation state, free counts, and fragmented-run results across all five supported block sizes. The prior bounded ownership diagnostics batch passed Linux/Windows CI on commit `74a0a79adadeb5c2cee7dc26b9d5ccb9f5e10cf8` (run [38302795167](https://github.com/Likounsee/OpenFS/actions/runs/38302795167)).

- Latest implementation batch adds namespace path reconstruction, cursor-based paginated directory listing, extent-map logical-length reporting and physical-bound validation, plus checked allocated-byte/logical-capacity reporting and file-byte-length validation for sparse extent maps. The latest extent-map commits are `02838ddba3e506b5ec7adf2158f3dbb67372aafd`, `0ae813c9a9018de18e0d0efbd1419891f2dd839a`, and boundary-test correction `ae9b98605a9776c0e280376c18fa73c87f45b945`. Tests were added but have not yet been observed passing in CI. It builds on `validate_allocation_ownership` (caller-supplied inventory), range removal/truncation, physical-block iteration, and atomic in-memory extent insertion. Latest code commits: namespace `76455e07f20af7cd1ae23c7f4b05132dfc2a9329`, extent bounds `a24e0b8654853d18d2fb2b77fccc90c5fa7a0960`; CI for both is in progress at the time of this snapshot. Persistent metadata discovery and atomic data/metadata/free transactions are still missing. The latest CI also exposed an MSRV violation in namespace pagination (`Option::is_none_or`, stable only since Rust 1.82 despite the 1.75 MSRV); this was replaced with `map_or` in commit `ec09b30c0fb858069ebf74ec0ddf31178199d6fe`.
- GitHub Actions run [38248099907](https://github.com/Likounsee/OpenFS/actions/runs/38248099907) passed for code commit `e8fd769707f6fc24c5558f0bfee324ccfa1a072b` on Linux and Windows, including formatting, workspace tests, and Clippy. Commit `a33527496cf5bd21b819bebe7c44c22aff74539d` adds journal-replay coverage for padding corruption in the second bitmap segment and its CI passed on Linux and Windows. Commit `fa635c02ed3a27ea9ca3ede2735e1ee09f67f62d` expands replay checks to every reserved bitmap bit for all five supported block sizes; CI run [38248670346](https://github.com/Likounsee/OpenFS/actions/runs/38248670346) passed on Linux and Windows. Commit `0bbdb2a67429c51a2211182e18e57324e4a79e19` extends the replay matrix to padding bits across those five sizes, but CI runs [38249300853](https://github.com/Likounsee/OpenFS/actions/runs/38249300853) and [38249387504](https://github.com/Likounsee/OpenFS/actions/runs/38249387504) failed at formatting only. Commit `79213fb40a9fe0a482bb800da60c29efb9cba65d` applies the rustfmt-requested line wrap; run [38249481025](https://github.com/Likounsee/OpenFS/actions/runs/38249481025) passed on Linux and Windows after the formatting fix; the current branch CI is being rerun for the added bitmap-read failure matrix.
- Recent regressions cover allocator exhaustion/reuse without journal advancement on failed allocation, every reserved-block free rejection, first and last bitmap padding bits across all five supported block sizes, later-segment reserved/padding corruption and journal replay (both reserved-bit and reserved-plus-padding replay matrices pass CI), layout tampering, device-geometry and arithmetic-overflow boundaries, canonical layout superblock validation, and golden-byte/CRC32C fixtures.
- Implementation batch on `OpenFS`: added public canonical layout allocator APIs `is_data_block_allocated` and `free_data_block_count`, then optimized bitmap scans with byte-level `trailing_zeros`/`count_ones`. A new `allocated_data_block_inventory` API now returns the validated, ascending list of allocated data blocks for diagnostics; it deliberately does not claim object ownership discovery. Its reopen/free regression was added, and CI is pending. The subsequent allocator batches add `find_contiguous_free_blocks`, atomic `allocate_contiguous_blocks`, `allocate_blocks` for several non-contiguous blocks in one bitmap segment, `free_blocks` for atomic batch release, `allocation_stats`, and read-only `validate_allocation_bitmap`, with reopen/persistence and request-boundary regressions. Batch operations intentionally stay within one bitmap block because the current bounded journal commits one bitmap home-block image per transaction. Linux and Windows CI passed for code commit `1e2b3fcddbcd454da63adabb54262766bc70ab35`; Linux CI (formatting, workspace tests, and Clippy) passed for the newest batch-allocation code commit `22d8adc645dc4af55c55d707482c6f1e40ef1d7a`; Windows CI was still running at the time of this review. These capabilities are implemented, but P1's full ownership/corruption acceptance gate remains open. Commit `3b59965fffb73393670576c93e26d68a5b969c53` adds allocation/free bitmap-read failure injection; commit `c6f177930e7eaabe05bde68fd647826a2b7f9b9b` extends allocation and free recovery tests to checkpoint-write failures across all five supported block sizes, and its Linux/Windows CI passed. Commit `24f08fcde2688c73c9edd095747ef5c7343ab966` adds last-valid-data-block allocation/exhaustion/free/reopen coverage and its Linux/Windows CI passed. Commit `2666ab6238d0082af149cbccefd073941618a791` adds injected I/O failures while scanning and freeing in a later bitmap segment; its CI is pending.
- Newest batch: `validate_allocation_bitmap` now checks reserved/padding bits byte-by-byte rather than iterating every bit; added public-API corruption regressions for reserved and out-of-volume padding bits across all five supported block sizes. Commits `79d44cd4c7f6946d2981d702f8f7a3e563117dae` and `d5636e3232e2b131b77e185d93f75ede31ca05db`; both CI runs were pending when this snapshot was written. Commit `9974f1db575e8e7e956ecec0b63977145366a897` adds a sparse-image regression for padding corruption in the second bitmap segment and verifies both the validator and allocated-block inventory reject it; CI is pending for this new commit. Commit `14827f4e9df58e3eb0b730817a8330166bcf3a3e` changes inventory enumeration from per-block scanning to byte-level set-bit iteration; commit `813a88984c230a56e409bec0c0470c25853e0991` extends the persistent inventory regression across all five supported block sizes. CI is pending for these latest changes.
- P1 follow-up `validated_physical_block_inventory` on `ExtentMap` now validates logical length, physical data-region bounds, and physical non-overlap before returning a sorted block inventory suitable for combining into the allocator's caller-supplied ownership map. Commit `1a2d935682a01ef5f05da2768805a6e95c0cbc9c`; regression covers ordering, bounds, and overlapping physical extents. CI is pending for this addition.
- P1 integration follow-up adds `OpenedVolume::validate_extent_map_ownership`: callers can provide each discovered file's extent map and logical block count, and OpenFS validates per-map logical/physical bounds, combines the block inventories, then compares them against the persistent allocation bitmap. This bridges extent-map validation to bitmap ownership checks and detects cross-file physical aliases, orphan allocations, and references to free/out-of-region blocks. It deliberately does not claim automatic persistent metadata discovery. Implementation commit `4f1a393c5dfc04bf9d6db8c2b86f8897283fa4da`; image-backed regression commit `a6204fb7d248ac94b7beeb587c354b36537a0804`; README update `5c766cd7e79fdcbe28af7f241bccdf6d9422897d`. CI for this batch has not yet been checked; do not count it as validated until Linux and Windows pass.
- P1 diagnostic follow-up adds `AllocationOwnershipReport` and `allocation_ownership_report` / `extent_map_ownership_report`, returning exact orphaned-allocation and dangling-reference block numbers while preserving strict pass/fail APIs. Free-space diagnostics now also expose `free_data_block_inventory` and `free_extent_stats` (free-run count and largest free run, bounded by the current one-bitmap-block transaction design). Image-backed tests cover free inventory and fragmentation changes after allocation/free. Latest commits: ownership report `be5f51cbdf430e66e68fe2e26c9b6802f93e0563`, public export `a64ee829c89e407e00908eea741bff55e62d1340`, regression tests `202ffa6250dc05d35548323d869840f0b6aac9b3`, free-space implementation `21ddcd837bf0c0fb5ad51c0a29dec6944948a4b0`, export `e97960aa9d59c6ece464a17066606387869df69e`, tests `8d992c3832aa08a8f49f590678ce0c7a837a6199`, README `79fb025c960f416116fb2126270fede76bd23243`. Prior complete CI run `38300563410` passed on Linux and Windows for commit `04e11229731a7fbfdda625bf6e709586f9653971`; the newest free-space batch still requires its own CI run.
- Latest P1 diagnostic batch adds `AllocationBitmapReport` and `allocation_bitmap_report`: the read-only scan reports the total number of free reserved/padding bits and a bounded sample of up to 64 offending block numbers, with an explicit truncation flag. The strict `validate_allocation_bitmap` keeps its existing error contract by delegating to the detailed report. Added exact-block and bounded-memory regression tests; implementation commit `9b6d386c9cebab52bb824b08e5f00f1ffa25b69f`, public export `60bba28ac52f773d51b307395fac8d306e94e933`, README `d85b7fa99d883f7169afee26006d5c6b090bc98c`. CI has not yet validated this batch; retain P1 at 85% until Linux and Windows pass.
- Follow-up allocator batch extends `AllocationBitmapReport` with free/allocated usable-data counts computed in the same bitmap pass. `allocation_stats` now rejects reserved/padding corruption and verifies `free + allocated == data_blocks` before returning statistics. Added a regression covering batch allocation, partial free, report counts, and the public statistics API; README updated. Implementation/test commit `c9b4646faf467f193dfd6d3c31946df256415564`, README `dcb9a53a67676917279f9d71aa1b1167e49d5853`. The preceding batch CI is green; this follow-up's CI is pending.

- The canonical layout specification documents geometry formulas, minimum layout, reserved bitmap bits, and disjoint regions.
- canonical layout journaling currently protects allocator bitmap changes; it is not yet a general transaction layer for persistent file and directory operations.
- Metadata and directory-entry codecs, checksummed extent-map pages, and an in-memory namespace exist, but a complete durable file/namespace workflow does not.
- The read-only canonical layout checker now validates mirrored superblocks, canonical geometry, bitmap invariants, checkpoint copies, journal record framing/order, and reports free/allocated counts plus pending/recoverable journal state. It deliberately does not replay or repair the journal, verify persistent object ownership, or certify namespace integrity. Parser fuzzing, safe physical-device discovery/formatter, and OS mount adapters remain absent.

The weighted planning estimate is **38%** (10% P0 + 13.5% P1 + 6% P2 + 2.5% P3 + 5.2% P4 + 0.5% P5; rounded to the nearest whole percent). P1 now has descriptor-driven discovery, namespace reconstruction, ownership reconciliation, and validated initial descriptor publication; safe descriptor replacement, durable namespace mutation, and broader crash-cut coverage remain open. This is a roadmap estimate, not a claim that OpenFS is ready to use on a USB drive. It is not yet a complete end-user filesystem.

| Workstream | Estimate | Evidence and remaining limitation |
| --- | ---: | --- |
| P0 — engineering foundation and format safety | 100% | Baseline gate documented as passed: Linux/Windows formatting, workspace tests, and Clippy on the recorded code commit. This is not physical-device or production validation. |
| P1 — volume layout and allocation | 90% | canonical layout, bitmap bounds, reserved-region checks, single and atomic batch allocation/free, exhaustion/reuse, reopen persistence, allocation statistics, contiguous extent search/allocation, bitmap reserved/padding validation, malformed-superblock checks, failure/recovery regressions, checksummed inventory descriptors, automatic allocated-block discovery, descriptor-driven namespace reconstruction, extent-map page decoding, bitmap ownership reconciliation including the descriptor block, and validated initial descriptor publication with reopen/discovery regression coverage exist. Safe descriptor replacement and durable namespace/file mutation remain. |
| P2 — journal and crash recovery | 30% | Journal encoding, commit/replay/checkpoint paths, and selected torn-write/failure tests exist. Exhaustive crash-cut and flush-order guarantees are not established. |
| P3 — checker, corruption testing, fuzzing | 25% | Read-only checker validates mirrored superblocks, canonical geometry, reserved/padding bitmap invariants, both checkpoint copies with fallback, journal framing/sequence ordering, and counts pending records/committed transactions. A read-only `check-image` CLI entry point is added. Ownership checking, repair, and fuzzing remain missing; CI for the current batch is still running. |
| P4 — persistent files and directories | 26% | Metadata/directory record codecs, checksummed block-sized extent maps, sparse lookup and logical-length reporting, coalescing, physical-block iteration, physical data-region bounds validation, atomic in-memory extent insertion, range removal/truncation returning physical runs, and an in-memory namespace with path resolution/reconstruction, cursor-based paginated listing, rename/move, file-layout updates, removal, and graph validation exist. Persistent metadata discovery, durable file I/O, and journal-atomic metadata/data/free updates remain. |
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
- [x] Document the distinct experimental legacy draft and canonical layouts and canonical layout's allocator-only transaction scope.
- [x] Review legacy draft and canonical layout boundaries, reserved regions, journal replay, bitmap invariants, and error paths.
- [x] Reject protected volume-control blocks overlapping canonical layout journal checkpoint/ring metadata.

**Exit gate:** baseline formatting, workspace tests, and Clippy passed on Linux and Windows on the recorded baseline commit. P0 completion means the engineering baseline is trustworthy enough to continue; it does **not** establish production readiness, general transaction safety, or physical USB durability. Reopen P0 if a concrete integrity/security defect is discovered.

### P1 — Dependable volume layout and allocator — IN PROGRESS (90%)

- [x] Specify canonical layout, primary/mirror metadata, bitmap, journal, reserved regions, alignment, limits, and version compatibility.
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
- [x] Add ownership reconciliation for caller-located inventories and descriptor-discovered inventories; include the descriptor, inventory blocks, extent-map roots, and file extents, and report orphaned allocations and dangling references deterministically.
- [x] Add validated initial descriptor publication, with journaled allocation and a flushed descriptor write; verify descriptor discovery and namespace reconstruction after reopen; detect interrupted publication as an orphan allocation.
- [x] Batch reads of contiguous allocated blocks during descriptor discovery, with a fixed maximum batch size to bound memory; regression covers a descriptor beyond the first batch.
- [ ] Add safe descriptor replacement without ambiguous copies; a larger or redesigned journal transaction is needed to atomically commit a full descriptor block and its bitmap allocation. Optimize discovery beyond scanning every allocated block.
- [x] Expand corruption coverage across malformed reserved/padding bitmap cases, allocation count consistency, and namespace lifecycle/index invariants (CI run `38302439267` passed on Linux and Windows).
- [x] Scan free-block inventories and free-space fragmentation by bitmap byte masks, with cross-block-size inventory/fragmentation regressions (CI pending for the current batch).\n- [x] Expose a read-only free-extent inventory with segment-bounded runs, and cross-check its results against fragmentation statistics (implementation and regression tests added; current CI pending).
- [x] Add deterministic best-fit free-extent selection with smallest-fit and no-fit regressions (implementation and tests added; current CI pending).
- [x] Add atomic best-fit contiguous allocation with bitmap revalidation and journaled commit, plus allocation accounting/no-fit regressions (CI pending).
- [x] Add atomic contiguous extent freeing and harden first-fit contiguous allocation with bitmap revalidation, checked ranges, and multi-size regressions (CI pending).
- [x] Stream free-extent inventory directly from bitmap bytes and accelerate first-fit search by enumerating free bits; test fragmented ranges and byte-boundary runs (CI pending).
- [x] Verify format/open/allocate/free/reopen across all five supported block sizes in CI (commit `baae2f1f2fab9dcdaece8106b4bb2ab55a2895b0`; Linux and Windows CI passed). The expanded allocator crash/failure matrix is also green on commit `f29b565af1d68d6f887c3d146934d29a55e79d6a`.

**Exit gate:** deterministic format/open/allocate/free/reopen tests, stable golden fixtures, malformed-layout rejection, and explicit ownership invariants when persistent objects are implemented.

### P2 — Transaction durability and crash recovery — IN PROGRESS (30%)

- [ ] Review Begin/redo/commit, home-block writes, checkpoint ordering, and every flush boundary.
- [ ] Inject failure before, during, and after journal, home-write, checkpoint, and flush operations.
- [x] Cover torn terminal commit records and repair torn home-block writes at multiple byte-prefix cut points in the journal engine tests.
- [ ] Extend torn-record and home-block crash cuts across all supported block sizes, not only whole-operation errors.
- [ ] Reopen from persisted bytes after every simulated crash and assert allowed old/new states.
- [ ] Test repeated recovery and idempotency, sequence wrap/overflow policy, malformed log lengths, and journal exhaustion.
- [ ] Document what host-file and physical-device flushes do and do not guarantee.
- [ ] Define the transaction contract before extending journaling from bitmap allocation to persistent metadata and file contents.

**Exit gate:** crash-cut matrix passes on disposable images in Linux and Windows CI; recovery is repeatable and never silently accepts ambiguous/corrupt state. Physical hardware claims require separate hardware testing.

### P3 — Read-only checker and hostile-input testing — IN PROGRESS (25%)

- [x] Implement a read-only canonical layout checker for mirrored superblocks, canonical layout, bitmap reserved/padding invariants, and allocation totals (`check_volume`).
- [x] Validate checkpoint-copy fallback, checkpoint sequence invariants, journal record framing/order, and report pending records/committed transactions without replay or writes.
- [x] Add `openfs check-image <path> <block-size>` for read-only canonical layout image inspection, including a regression proving image bytes remain unchanged.
- [ ] Extend integrity checking to persistent object ownership and namespace relationships.
- [ ] Add bounded fuzz targets for disk parsers, metadata, and directory/name parsing.
- [ ] Keep repair separate from checking; repair must be explicit and must describe proposed changes before writing.
- [ ] Expand the corruption corpus and CI fixtures beyond current bitmap, superblock, checkpoint, and journal-framing regressions.

**Exit gate:** checker detects known corruption without modifying the volume; fuzzing has documented budgets and regression fixtures for discovered bugs.

### P4 — Persistent namespace and ordinary file operations — IN PROGRESS (22%)

- [x] Specify initial fixed-width object metadata and directory-entry codecs with explicit encoding and CRC validation.
- [x] Add a checksummed block-sized extent-map page, sparse logical-block lookup, extent coalescing, logical/physical region validation, atomic in-memory insertion, physical-block iteration, range removal/truncation returning physical runs for release, checked allocated-byte/logical-capacity reporting, and validation against byte-sized file lengths (not yet connected to persistent file I/O).
- [x] Implement an in-memory namespace with root creation, insertion, lookup, deterministic child enumeration, path resolution, rename/move, file-layout metadata updates, removal of files and empty directories, and index/reachability validation.
- [ ] Persist root/object metadata and directory entries in canonical layout blocks with journaled publication.
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

1. Complete a targeted static review of canonical layout journal commit/replay/checkpoint ordering and enumerate untested crash boundaries.
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
