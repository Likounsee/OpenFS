# OpenFS

OpenFS is a new, open-source filesystem project written primarily in **Rust**. Its long-term goal is to provide a reliable, secure, and high-performance filesystem with broad platform support, including Windows, Linux, and eventually ArchiaOS.

> **Development status: early foundation stage.** This repository currently contains only this project overview. The Rust engine, on-disk format, image backend, recovery protocol, and automated CI have not yet been implemented in this repository. OpenFS is not currently a usable filesystem and is not a replacement for NTFS.

## Vision

The project aims to combine explicit on-disk formats, carefully specified persistence guarantees, strong corruption detection, and reproducible testing. Performance will be measured against NTFS using equivalent workloads before any comparative claims are made.

The implementation is being started afresh in Rust. The historical [OpenFS-old repository](https://github.com/Likounsee/OpenFS-old) is a read-only technical reference for previous C implementation choices, failure handling, recovery tests, and platform adapters. Its code and design will be audited rather than copied blindly.

## Repository layout

- `/README.md` — this project overview, shown on GitHub.
- [`/OpenFS/README.md`](OpenFS/README.md) — technical guide for the Rust workspace.
- `/OpenFS/crates/` — Rust components, added when their boundaries are justified.
- `/OpenFS/tests/`, `benches/`, and `fuzz/` — integration tests, benchmarks, and fuzz targets as they become available.
- `/OpenFS/docs/` — architecture, format, recovery, security, compatibility, roadmap, and legacy audit documents.
- `/.github/workflows/` — GitHub Actions workflows (GitHub only automatically discovers workflows in this root-level directory).

The project itself belongs under `OpenFS/`; Git history remains in Git and is not stored in a project folder.

## Roadmap

| Phase | Focus | Status |
| --- | --- | --- |
| P0 | Foundations, safety, image backend, documentation, CI | In progress — repository bootstrap |
| P1 | On-disk format and essential file/directory operations | Not started |
| P2 | Transactions, journal, and crash recovery | Not started |
| P3 | Integrity checks, corruption handling, and verification tools | Not started |
| P4 | Reproducible benchmarks and evidence-driven optimization | Not started |
| P5 | Advanced features (ACLs, links, sparse files, quotas, etc.) | Not started |
| P6 | Windows integration through an appropriate adapter | Not started |
| P7 | Hardening, compatibility, and stable release process | Not started |

Phase completion will be based on explicit acceptance criteria and verifiable test evidence, not on planned features or documentation alone.

## Building and testing

The Rust workspace is not yet present, so Cargo commands are **not available yet**. Once it is initialized, commands will be run from `OpenFS/`, including:

```sh
cargo fmt --all -- --check
cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
```

These commands will only be described as passing after they have actually run in CI or another recorded execution environment.

## Current limits

- No filesystem engine or stable disk format is implemented in this repository yet.
- No image-file block device, formatter, mount implementation, checker, or repair utility is available here yet.
- No compatibility with NTFS is claimed.
- No performance results are available.
- Filesystem durability depends on well-defined flush semantics and the underlying device; these guarantees still need to be specified and tested.
- Windows integration and kernel-level work are future milestones, not current capabilities.

## Contributing

Development takes place on the **`OpenFS`** branch. Keep changes scoped, tested, documented, and committed with clear messages. Do not make development commits to `main`. New disk-format behavior must be explicitly encoded and documented rather than relying on native Rust struct layout. Error paths, malformed input, bounds, and interrupted I/O should be tested alongside normal behavior.

See the [technical README](OpenFS/README.md) and the roadmap under `OpenFS/docs/` as they are added.
