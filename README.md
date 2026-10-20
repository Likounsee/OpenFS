# OpenFS

OpenFS is an open-source filesystem project written primarily in Rust. Its long-term goal is a reliable, secure, high-performance filesystem for Windows, Linux, and eventually ArchiaOS.

> **Status: experimental.** The workspace contains a canonical journaled volume layout, a bounded block-image backend, allocation and ownership checks, persistent namespace operations, and block-bounded streaming file reads and writes. The CLI supports image-based file operations, including range I/O, streaming import from standard input, truncation, rename, and removal. It is not yet a mountable or production-ready filesystem and is not a replacement for NTFS.

## Repository structure

- `README.md` — project overview and high-level status.
- `OpenFS/` — Rust workspace and technical documentation.
  - `crates/openfs-core/` — shared errors and checked ranges.
  - `crates/openfs-block/` — block-device contract and image backend.
  - `crates/openfs-format/` — canonical disk layout and filesystem primitives.
  - `crates/openfs-tool/` — development-image CLI.
  - `docs/ARCHITECTURE.md` — module boundaries and safety rules.
  - `docs/FORMAT_SPEC.md` — on-disk layout and record rules.
  - `docs/DEVELOPMENT.md` — workflow for future features and fixes.
  - `docs/ROADMAP.md` — phase gates and measured progress.
- `.github/workflows/ci.yml` — Linux and Windows formatting, tests, and Clippy.

There is one active volume implementation in `OpenFS/crates/openfs-format/src/volume.rs`. Do not create parallel volume implementations distinguished by generation labels. The on-disk format identifier remains a strict compatibility check, not a source-file naming convention.

## Development

From the workspace directory:

```sh
cd OpenFS
cargo fmt --all -- --check
cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
```

All development commits belong on the `OpenFS` branch. Validate untrusted disk bytes, use checked arithmetic, propagate I/O errors, and test failure paths as well as successful paths. Formatting is destructive; the CLI is intentionally limited to regular image files.

## Limits

Although image-backed namespace and file mutations exist, a general transaction layer for namespace and extent-map updates, complete crash-recovery guarantees, physical-device discovery safeguards, and OS mounting remain unfinished. File replacement, truncation, range writes, and streaming imports now preflight existing extent allocations against the allocation bitmap before mutating the file. Do not use important data or claim physical-device durability based on image tests.
