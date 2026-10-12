# Audit of historical OpenFS-old

**Audit date:** 2026-10-12  
**Reference:** `Likounsee/OpenFS-old`, default branch HEAD tree `fdb5dee16266d037c79438a5b20ebccceab3fa24`. The tree contains 136 tracked files (151 entries including directories).  
**Policy:** this repository is historical/reference-only. Do not make implementation changes there. The active Rust work belongs to `Likounsee/OpenFS`, branch `OpenFS`.

This is a repository-wide source and test inventory plus a focused review of the storage format, allocator, inode/extent/directory/file paths, journal/transactions/replay, mount lifecycle, FSCK/scrub, CoW, namespace, handles/locks, and OS adapters. It is not a claim that the legacy suite was rebuilt or that every historical commit diff was reviewed line by line. Test files and CI definitions are evidence of intended coverage, not proof that the latest source passes.

## Executive conclusion

The C implementation is substantially more feature-complete than the new Rust workspace, but it is not production-complete. It has a real filesystem engine and many regression tests; the Rust project currently has foundational block I/O, an experimental volume layout/allocator, record codecs and an in-memory namespace. The correct next step is **not** to add more isolated format structs or port advanced features. It is to settle the new format contract, get the current Rust CI green, then build a small persistent transactional vertical slice with reopen/crash tests.

Do not port the C code wholesale. Preserve its invariants, negative tests and fault scenarios; reimplement the behavior behind Rust types and a smaller public API.

## 1. Repository inventory

The historical repository includes:
- C11 core under `OpenFS/src/` and public interfaces under `OpenFS/include/openfs/`;
- CMake build and GitHub Actions CI, including GCC, Clang, sanitizer, Release and Windows jobs;
- 40+ core test programs covering format, allocation, inodes, extents, files, directories, path operations, links, xattrs/ACLs, mount lifecycle, concurrency, journal, transaction failures, crash cuts, FSCK repair and scrub;
- format-image, format-device, FSCK-device and USB-test utilities under `OpenFS/tools/`;
- Linux, Windows/WinFsp and ArchiaOS block-device/platform glue plus adapter tests;
- architecture, format, handles, locking, orphan, ACL/xattr, snapshots and development-status documentation.

### Core subsystem map

| Historical subsystem | What the source exposes | Main lessons for Rust |
| --- | --- | --- |
| Block device | read/write/flush callbacks, block size/count, explicit range errors | Define short-I/O, concurrency, flush and durability semantics before relying on a device |
| Format/mount | explicit little-endian superblock encoding, CRC32C, primary + backup copies, geometry/feature validation, format modes | Use normative field tables, checked geometry, golden images and malformed-input tests |
| Allocation | block and inode bitmaps, reserved ranges, first-fit allocation/free, rollback on I/O errors | Bitmap, ownership, checksums and refcounts must change atomically |
| Inodes | fixed 256-byte records, generation, mode/UID/GID/link count, timestamps, inline symlink data, extents, xattr pointer, checksum | Validate cross-field invariants, not just record CRC |
| Extents/files | five inline extents; optional depth-0 extent leaf; sparse files; read/write/truncate; seek-data/seek-hole; data checksums | Build bounded mapping and file semantics before optimizing or adding deeper trees |
| Directories/paths | checksummed fixed-size directory entries, name bounds, path traversal, `.`/`..`, trailing slash handling, create/mkdir/unlink/rename/chmod/time changes | Namespace operations touch multiple structures and require transactional publication |
| Links/orphans | hard links, symlinks, link counts, open-but-unlinked inode retention, orphan reclaim at mount | Open handles, namespace links and storage reclamation need a single lifecycle model |
| Journal/transactions | BEGIN/DATA/COMMIT WAL, checksummed block records, buffered final-block writes, replay prevalidation/staging, flush and checkpoint, recovery gating | Specify a state machine and prove every cut point; a record codec alone is far from a transaction engine |
| CoW | persistent block refcounts, clone/discard, transactional refcount variants | Define ownership and exactly-once refcount transitions before snapshots |
| Metadata CoW/root | checksummed metadata block wrapper, generation-bearing metadata root, snapshot-catalog root field | Primitive groundwork is not a persistent snapshot implementation |
| FSCK/repair/scrub | cross-structure consistency scan, diagnostics/progress, bitmap-tail and CoW-refcount repair, read-only scrub | Ship read-only diagnostics before narrowly scoped, opt-in repair |
| Access metadata | mode-bit checks, POSIX-style ACLs/inheritance, xattrs, timestamps | Windows SID/security-descriptor semantics are a separate adapter problem |
| Handles/locking/runtime | open-file object, dup/reference counts, seek/read/write/truncate/stat, advisory byte-range locks, ranked locks, runtime admission/drain | Concurrency/lifecycle is part of correctness, not a late optimization |
| Adapters/tools | Linux file-backed I/O, Windows raw/file I/O and WinFsp/FUSE compatibility, ArchiaOS callback bridge | Keep OS glue outside the portable engine; distinguish block-device glue from a mounted filesystem adapter |

## 2. Historical on-disk format and compatibility

The **current C source** declares format major 1, minor 6, with feature flags for extent trees, CoW, metadata roots and data checksums. Its layout includes the primary superblock, a backup at the last block, block and inode bitmaps, inode table, journal, refcount table, data-checksum table and data region. Inodes are 256 bytes; directory records are 256 bytes with a maximum name length of 227 bytes. Extent-tree support is a single depth-0 leaf, not an unbounded B-tree.

There is documentation drift in the old repository: `OpenFS/docs/format.md` describes v1.3, `extent-tree-v1.3.md` is historical, and the root README describes v1.5, while the source header declares v1.6. Treat serialized source code plus tests as evidence to reconcile, not as permission to silently select an old document as normative.

The Rust format is a **new, incompatible draft**: its superblock/header and metadata record layouts are not the legacy v1.6 layout. The current compatibility contract must explicitly say that legacy images are not yet mountable by Rust. Before the Rust formatter is treated as stable, choose one:
1. deliberately define a new OpenFS on-disk format and state that legacy v1.x images require a future converter; or
2. formally specify a migration/compatibility target and add versioned golden fixtures before any persistent namespace is written.

Do not imply that OpenFS can mount actual NTFS volumes. The project is an NTFS-inspired/general-purpose filesystem with its own format, not an NTFS on-disk implementation.

## 3. What exists in the legacy engine—and what it does not prove

### Implemented in the historical C source

- Explicit byte serialization, CRC32C, geometry checks, mirrored superblocks and format-time initialization.
- Block/inode allocation, inode generations, fixed-width metadata, extent mapping and depth-0 overflow extent trees.
- File read/write/truncate, sparse extents and seek-data/seek-hole APIs.
- Directories, path traversal, hard links, symlinks, rename, unlink, chmod and timestamps.
- POSIX-like mode/ACL checks, xattrs, file handles, duplicate references and advisory byte-range locks.
- WAL, transactions, replay, checkpoint, recovery-required state, copy-on-write/refcounts and persistent orphan recovery.
- FSCK, a small set of repair operations, and read-only scrub/data checksums.
- Linux and ArchiaOS block adapters; Windows block-device glue and a WinFsp/FUSE-compatible mount adapter.

### Known limitations or incomplete areas in the historical project

- The WinFsp/FUSE mount adapter is read-only: writes return `-EROFS`; create/mkdir/rename/truncate callbacks are missing and flush/fsync/release are unset. This is not a Windows read-write filesystem integration.
- Windows-native security descriptors, SID/ACL mapping, full share modes and complete Windows delete/rename semantics are not demonstrated.
- Metadata-CoW is explicitly a stage-1 primitive. A snapshot catalog/API, immutable snapshot views, atomic create/delete, snapshot-aware FSCK and complete snapshot lifecycle are absent.
- Xattrs do not establish NTFS Alternate Data Streams (ADS) compatibility.
- No complete quotas, compression, encryption/key management, reparse-point semantics or USN-like change journal were found.
- FSCK repair APIs are limited: bitmap tail cleanup and CoW refcount rebuild. They do not constitute general automatic repair for arbitrary inode/extent/namespace/journal corruption.
- Scrub is read-only; it detects/report issues and does not reconstruct corrupted file content.
- Extent trees have depth zero only. Very large fragmented files can eventually exceed leaf capacity.
- Direct/unmounted mutation paths and mounted transaction-aware paths have different durability guarantees; the API must not advertise one global atomicity guarantee.
- No reproducible benchmark suite or continuous parser-fuzzing target was found in the tracked tree.
- A userspace Linux adapter is not native Linux VFS/kernel integration.

## 4. Test and CI evidence to preserve

The old suite contains unusually useful fault-injection patterns. The Rust rewrite should port the **scenarios**, not the C harness:

- journal BEGIN/DATA/COMMIT partial writes, slot restoration, rollback failure, failed flush, checkpoint clear/write/flush failure and recovery gating;
- validate-all-before-publication, reject transaction/sequence mismatches, record mutation between validation and replay, and no callbacks after a later malformed record;
- replay retry after callback failure, partial multi-block publication and final flush failure; repeated replay must converge to the same final bytes;
- process-level crash cuts around COMMIT, each home-block write, final flush and checkpoint, followed by remount, FSCK and another remount;
- namespace crash cases for create, unlink, rename replacement, symlink and hard-link publication; compare paths **and inode identities**, not merely path existence;
- allocator/refcount/checksum cross-structure corruption, failed rollback and repair preflight failures;
- sparse write/truncate boundaries, extent overlap, root bounds, partial data writes and failed restoration;
- open-unlink-close, orphan recovery, close/unmount races, duplicate handles, lock handoff and namespace/file concurrency.

The old roadmap records CI successes for earlier SHAs, but later entries explicitly mark additional changes as awaiting CI. This audit did not run the old C build; therefore no statement here means that the current historical HEAD passes its entire matrix.

## 5. Current Rust gap map

The active Rust branch currently contains `openfs-core`, `openfs-block` and `openfs-format`. The format crate has a mirrored-superblock experimental volume layout, bitmap allocation/free, fixed metadata and directory-entry codecs, an in-memory namespace, and a draft journal record codec. It does **not** yet provide a mountable persistent filesystem.

| Capability | Rust status | Required next work |
| --- | --- | --- |
| Block/image I/O and shared errors | Foundation exists | Audit bounds, failure semantics and device contract |
| Superblock/volume layout | Experimental, not legacy-compatible | Freeze format version/features/reserved bytes and add golden fixtures |
| Metadata/directory entry codecs | Codecs exist | Persist object table and directory indexes inside transactions |
| Namespace | In-memory only | Durable create/lookup/list/rename/remove and reopen tests |
| Allocation | Bitmap allocator exists | Make allocation, metadata ownership and free atomic under WAL |
| File data/extents/sparse files | Not implemented as a complete engine | Add extent format, mapping, read/write/truncate and sparse semantics |
| Journal | Record codec only | Specify transaction state machine; then bounded log writer, durable commit, replay, checkpoint and recovery gate |
| Mount/unmount/runtime | Not implemented as a filesystem lifecycle | Recovery-before-exposure, error state, serialization and clean teardown |
| Integrity | Codec checksums exist; no general checker | Read-only FSCK/scrub, ownership accounting, corruption matrix and fuzzing |
| Links/orphans/ACL/xattrs/handles/CoW | Not implemented end to end | Defer until persistent file/namespace semantics are correct |
| OS mount adapters | No filesystem mount adapter | Stabilize core semantics first; raw block I/O adapters are not enough |
| Benchmarks/release hardening | Not implemented | Add after correctness gates |

## 6. Reprioritized delivery plan

### P0 — Make the current Rust baseline trustworthy
1. Get `cargo fmt --all -- --check`, `cargo test --workspace` and `cargo clippy --workspace --all-targets -- -D warnings` green on Linux and Windows for the exact branch HEAD. Fix CI failures rather than weakening checks.
2. Decide the new format's compatibility policy; make the format spec, encoder, validator, formatter and golden fixtures agree.
3. Define device semantics: short reads/writes, flush barrier contract, concurrency, destructive format behavior, and errors after uncertain persistence.
4. Add parser/property/fuzz tests and deterministic fault injection for format and volume metadata.
5. Keep `OpenFS-old` read-only.

### P1 — Build the smallest persistent vertical slice
1. Specify a durable journal state machine before writing a journal writer: BEGIN/update/COMMIT ordering, sequence ownership, flush barriers, checkpoint protocol, malformed-tail policy, and retry/recovery states.
2. Implement the bounded WAL writer and replay on a test block device. Verify persisted bytes after every injected write/flush failure and process-crash cut.
3. Add a persistent object/inode table and directory storage; make create/mkdir/lookup/list one transactionally coherent feature slice.
4. Add file extent allocation and read/write/truncate, including zero-filled gaps, sparse holes and checksums; keep all related bitmap, extent, inode and checksum updates in the same transaction.
5. Implement mount/reopen so it validates format and recovers the WAL before exposing namespace operations.

### P2 — Integrity and core semantics
1. Read-only FSCK ownership accounting and scrub.
2. Narrow opt-in repair only where ownership is unambiguous; refuse unsafe guesses.
3. Rename/remove/link lifecycle, generations, open handles and orphans.
4. Persistent corruption corpus, bounded fuzzing and full crash-cut matrix for each multi-structure mutation.

### P3 — Compatibility and advanced features
1. Choose Windows integration target and implement read-write callbacks only after core transactions are stable.
2. Add credentials/ACL/xattrs, then CoW and snapshots only with durable reference ownership and recovery.
3. Consider ADS, quotas, compression, encryption, reparse points and change journal as separately specified features, not a single vague "NTFS compatibility" task.
4. Add benchmarks and performance optimization after the functional baseline is reproducible; decide separately whether native Linux kernel integration is a project goal.

## 7. Acceptance rule

A feature is not "done" because its struct, codec or API exists. Require:
- explicit on-disk format and bounds;
- normal-path and malformed-input tests;
- read/write/flush failure injection;
- persisted-state verification after reopen;
- crash-cut/replay tests for every multi-structure mutation;
- corruption-checker interaction;
- documented semantics and an exact green CI SHA.

