# OpenFS

OpenFS is a new, open-source filesystem project written primarily in **Rust**. Its long-term goal is a reliable, secure, high-performance filesystem with broad platform support, including Windows, Linux, and eventually ArchiaOS.

> **Status: early P0 foundations.** The Rust workspace and initial block-image and superblock primitives are being established. This is not yet a mountable or production-ready filesystem and is not a replacement for NTFS.

## Vision

OpenFS aims to combine an explicit, versioned disk format, defined persistence semantics, strong corruption detection, recoverability, and reproducible performance testing. Comparisons with NTFS will be made only after equivalent workloads have been measured.

The new implementation starts afresh in Rust. The historical [OpenFS-old repository](https://github.com/Likounsee/OpenFS-old) is a read-only technical reference for prior C implementation choices, failure handling, recovery tests, and OS adapters. Its code is being audited rather than copied blindly.

## Repository layout

- /README.md — this overview.
- [OpenFS/README.md](OpenFS/README.md) — technical guide and Cargo commands.
- OpenFS/Cargo.toml — Rust workspace manifest.
- OpenFS/crates/ — implemented Rust components.
- OpenFS/docs/ — architecture, disk-format draft, recovery/security design, compatibility, roadmap, benchmarks, and legacy audit.
- /.github/workflows/ — root-level GitHub Actions workflows.

The project code belongs under OpenFS/. Git commits are part of the repository history, not files inside that directory.

## Roadmap

| Phase | Focus | Status |
| --- | --- | --- |
| P0 | Foundations, safety, image backend, docs, CI | In progress |
| P1 | Format and essential file/directory operations | Not started |
| P2 | Transactions, journal, crash recovery | Not started |
| P3 | Integrity checks, corruption handling, verification tools | Not started |
| P4 | Reproducible benchmarks and evidence-driven optimization | Not started |
| P5 | Advanced features such as ACLs, links, sparse files, quotas | Not started |
| P6 | Windows integration through a suitable adapter | Not started |
| P7 | Hardening and stable release process | Not started |

## What exists now

The initial Rust workspace contains:
- shared error and checked block-range primitives;
- a file-backed block-image device with explicit block bounds, exact-length I/O, a flush operation based on File::sync_all, and non-destructive image creation by default;
- an explicitly encoded 4096-byte superblock draft with little-endian fields, version checks, reserved-byte validation, and CRC32C;
- unit tests for normal I/O, invalid ranges and buffers, persistence across reopen, malformed superblocks, and deterministic read/write/flush failure injection;
- a root-level CI workflow for formatting, workspace tests, and Clippy.

These primitives do **not** yet implement file allocation, directories, file operations, mounting, journaling, recovery, or repair. Their actual CI status is visible in [GitHub Actions](https://github.com/Likounsee/OpenFS/actions).

## Build and test

Run from the project directory:

    cd OpenFS
    cargo fmt --all -- --check
    cargo test --workspace
    cargo clippy --workspace --all-targets -- -D warnings

## Limits and contribution rules

No NTFS compatibility or performance parity is claimed; no comparative benchmark results are available. The image backend is a development primitive, not a guarantee of physical-media durability beyond the host OS and device's reported synchronization semantics. Windows mounting, kernel integration, and production recovery remain future work.

Development commits belong on the **OpenFS** branch, never main. Disk formats must be encoded explicitly rather than by copying native Rust struct memory. Validate untrusted disk data, check arithmetic and ranges, propagate I/O errors, and test failure paths alongside successful operations. See [the technical README](OpenFS/README.md) and [roadmap](OpenFS/docs/ROADMAP.md).
