# OpenFS Rust workspace

This directory is the Rust project root. The root-level repository README remains at ../README.md; GitHub Actions workflows live in the repository-root .github/workflows/.

## Current workspace

- `openfs-core`: shared errors and checked block-range validation.
- `openfs-block`: file-backed image device; bounded block reads/writes, explicit sync, safe create-by-default behavior with a separately named destructive truncate API, and test-support for one-shot read/write/flush failure injection and a mixed-state torn-write model (not a simulation of physical-device firmware).
- `openfs-format`: explicit little-endian superblock encoding/decoding with CRC32C; experimental V1 and V2 volume layouts; V2 allocation bitmap validation with bounded corruption diagnostics (exact first 64 invalid reserved/padding block numbers plus total count), one-pass free/allocated data-block counts, allocation statistics cross-checked against usable volume capacity, allocated/free block inventories using byte-level set-bit scans, free-space fragmentation statistics using byte-level run detection, plus a read-only inventory of contiguous free extents split at bitmap-segment boundaries and a deterministic best-fit query that selects the smallest extent satisfying a request, and regression coverage for inventories and fragmented runs across all five supported block sizes; invalid reserved/padding bits are consistently rejected by allocation statistics, free-block inventory, and fragmentation queries. It also provides caller-supplied ownership-inventory consistency checks with bounded orphan/dangling samples (64 block numbers per category, total counts, and truncation flags), extent-map-to-bitmap ownership validation (including cross-file physical aliases), and journaled block allocation/free; persistent journal/checkpoint encoding, commit/replay, and recovery paths; metadata/directory-entry codecs; checksummed block-sized extent maps with sparse logical-to-physical lookup, coalescing, atomic in-memory extent insertion, validated sorted physical-block inventories, physical-block iteration, range removal/truncation that returns physical runs for release, bounds/overlap validation, sparse logical-length reporting, checked allocated-byte/logical-capacity reporting, file-byte-length validation, and validation that physical runs stay inside the allocatable data region; in-memory namespace insert/lookup/path resolution/path reconstruction, cursor-based paginated directory listings, rename/move/file-layout updates/remove and graph consistency validation. The ownership validator requires a caller-supplied inventory; persistent metadata discovery is not implemented. Extent maps and namespace operations are not yet integrated into persistent file I/O.
- `openfs-tool`: workspace utility crate; its capabilities should not be confused with a safe physical-device formatter.

The V1 and V2 layouts are separate experimental formats. V2 journals allocator bitmap mutations, but the journal is not yet a general transaction layer for persistent file and directory metadata. A read-only V2 image checker is available; it validates superblocks, layout, bitmap invariants, and journal framing without replaying or repairing the journal. There is no complete durable create/read/write/rename/delete workflow, no ownership/namespace integrity checker, no repair utility, and no OS mount adapter. This is not yet a usable end-user filesystem and must not be used with valuable data.

## Toolchain and commands

Use the Rust stable toolchain from rust-toolchain.toml. Run commands here:

    cargo fmt --all -- --check
    cargo test --workspace
    cargo clippy --workspace --all-targets -- -D warnings

Inspect a V2 image without modifying it (supply its actual block size):

    cargo run -p openfs-tool -- check-image ./volume.img 4096

The checker is read-only and does not replay pending journal records. The existing `create-image` command still creates the V1 development format; it does not create a V2 image or format a physical USB drive.

The repository-root GitHub Actions workflow runs formatting, workspace tests, and Clippy on Linux and Windows. Check the exact commit's Actions results before treating a change as validated; the presence of a workflow or a previous green commit is not evidence that the latest commit passed.

## Engineering conventions

- Treat all disk-derived fields as untrusted input.
- Use checked arithmetic for offsets, lengths, and capacities.
- Encode disk fields explicitly; native Rust struct layout is never the disk format.
- Propagate I/O errors and distinguish malformed input from device failures.
- Keep the core platform-neutral and safe Rust by default.
- Add tests for invalid sizes, ranges, truncated/malformed input, persistence and I/O failure behavior.
- Do not claim durability beyond the actual flush contract or measured hardware behavior.
- Keep hardware formatting unavailable until device discovery, target confirmation, mounted/system-device exclusion, and integrity/recovery gates are implemented and validated.

See docs/ARCHITECTURE.md, docs/FORMAT_SPEC.md, docs/RECOVERY_DESIGN.md, docs/SECURITY_MODEL.md, and docs/ROADMAP.md.
