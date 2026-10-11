# OpenFS Rust workspace

This directory is the Rust project root. The root-level repository README remains at ../README.md; GitHub Actions workflows live in the repository-root .github/workflows/.

## Current workspace

- openfs-core: shared errors and checked block-range validation.
- openfs-block: file-backed image device; bounded block reads/writes, explicit sync, safe create-by-default behavior with a separately named destructive truncate API, and a test-support feature for one-shot read/write/flush failure injection and a mixed-state torn-write model (not a simulation of physical-device firmware).
- openfs-format: initial 4096-byte, little-endian superblock encoding/decoding draft with CRC32C.

The format and public APIs are early drafts and may change before the first stable format release. No file/directory namespace, allocator, transaction journal, recovery engine, mount adapter, or repair utility exists yet.

## Toolchain and commands

Use the Rust stable toolchain from rust-toolchain.toml. Run commands here:

    cargo fmt --all -- --check
    cargo test --workspace
    cargo clippy --workspace --all-targets -- -D warnings

## Engineering conventions

- Treat all disk-derived fields as untrusted input.
- Use checked arithmetic for offsets, lengths, and capacities.
- Encode disk fields explicitly; native Rust struct layout is never the disk format.
- Propagate I/O errors and distinguish malformed input from device failures.
- Keep the core platform-neutral and safe Rust by default.
- Add tests for invalid sizes, ranges, truncated/malformed input, persistence and I/O failure behavior.
- Do not claim durability beyond the actual flush contract or measured hardware behavior.

See docs/ARCHITECTURE.md, docs/FORMAT_SPEC.md, docs/RECOVERY_DESIGN.md, docs/SECURITY_MODEL.md, and docs/ROADMAP.md.
