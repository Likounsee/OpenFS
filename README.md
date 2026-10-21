<p align="center">
  <img src="OpenFS/docs/assets/openfs-logo.svg" alt="OpenFS logo" width="220">
</p>

<h1 align="center">OpenFS</h1>

<p align="center">
  <strong>A filesystem built in Rust, from its on-disk format to persistent file operations.</strong>
  <br>
  A long-term project aiming for a reliable, secure and performant filesystem for Windows, Linux and, eventually, ArchiaOS.
</p>

<p align="center">
  <a href="https://github.com/Likounsee/OpenFS/actions/workflows/ci.yml"><img src="https://github.com/Likounsee/OpenFS/actions/workflows/ci.yml/badge.svg?branch=OpenFS" alt="OpenFS CI"></a>
  <img src="https://img.shields.io/badge/language-Rust-orange.svg" alt="Rust">
  <img src="https://img.shields.io/badge/status-experimental-yellow.svg" alt="Experimental">
</p>

> **Status: experimental — not production-ready.** OpenFS currently provides a Rust workspace for working with filesystem images, including a canonical on-disk layout, a journal and recovery primitives, allocation and ownership verification, a persistent namespace, file I/O, and a development CLI. It is **not yet mountable as a normal OS filesystem**, is not a replacement for NTFS, and must not be trusted with important data.

## Project goals

OpenFS is intended to become an independent filesystem rather than a wrapper around NTFS or another existing format. The long-term goals are to:

- define and maintain a documented, versioned on-disk format;
- store directories, metadata, extents and file contents persistently;
- protect filesystem integrity through strict validation, journaling and crash recovery;
- provide safe, predictable file operations and useful integrity-checking tools;
- build a safe path from image-based development to carefully validated device support;
- integrate with Windows and Linux, and eventually ArchiaOS, through dedicated OS adapters.

These are **goals, not claims that every capability is implemented today**. See the [roadmap](OpenFS/docs/ROADMAP.md) for phase estimates and acceptance gaps.

## What works today

The workspace already contains:

- **On-disk format primitives:** checked layout geometry, superblock validation, checksummed records, metadata and directory-entry codecs.
- **Block I/O:** bounded reads and writes, explicit flushes, image-file handling and deterministic I/O failure injection.
- **Allocation and verification:** bitmap allocation, extent maps, persistent inventory reconstruction and cross-layer ownership checks.
- **Journal components:** journal/checkpoint encoding and recovery paths for supported operations.
- **Persistent namespace and file APIs:** directory and file operations, streaming file import, range I/O, truncation, rename and removal through the development tool.
- **Image CLI:** create, inspect, check, audit and recover development images without presenting physical-device formatting as safe.

The presence of these components does **not** mean all multi-step operations are atomic under every crash scenario. General transaction coverage, recovery guarantees, device safety and OS mounting remain active development areas.

## Repository layout

```text
.
├── README.md
├── .github/workflows/ci.yml
└── OpenFS/
    ├── Cargo.toml                 # Rust workspace
    ├── crates/
    │   ├── openfs-core/           # Shared errors and checked ranges
    │   ├── openfs-block/          # Bounded block I/O and image backend
    │   ├── openfs-format/         # Format, allocator, journal, namespace and file APIs
    │   └── openfs-tool/           # Image-only development CLI
    └── docs/
        ├── assets/openfs-logo.svg
        ├── ARCHITECTURE.md
        ├── FORMAT_SPEC.md
        ├── SECURITY_MODEL.md
        ├── COMPATIBILITY.md
        ├── DEVELOPMENT.md
        └── ROADMAP.md
```

## Quick start

### Requirements

- A stable Rust toolchain (the workspace declares Rust 1.75 as its minimum version).
- Git.

### Build and validate

Run these commands from the Rust workspace directory:

```sh
cd OpenFS
cargo build --workspace
cargo fmt --all -- --check
cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
```

GitHub Actions is configured to run formatting, workspace tests and Clippy on Linux and Windows. Check the [CI runs for the `OpenFS` branch](https://github.com/Likounsee/OpenFS/actions/workflows/ci.yml?query=branch%3AOpenFS) before treating a change as validated.

### Create a development image

From `OpenFS/`:

```sh
cargo run -p openfs-tool -- create-image ./openfs.img 1024
cargo run -p openfs-tool -- inspect-image ./openfs.img
cargo run -p openfs-tool -- check-image ./openfs.img 4096
```

The example creates a 1 GiB image. OpenFS supports block sizes of 4096, 8192, 16384, 32768 and 65536 bytes where accepted by the relevant command.

### Create a namespace and work with a file

```sh
cargo run -p openfs-tool -- init-namespace ./openfs.img 4096
cargo run -p openfs-tool -- mkdir ./openfs.img 4096 /docs
cargo run -p openfs-tool -- touch ./openfs.img 4096 /docs/hello.txt
cargo run -p openfs-tool -- import ./openfs.img 4096 ./README.md /docs/hello.txt
cargo run -p openfs-tool -- cat ./openfs.img 4096 /docs/hello.txt
cargo run -p openfs-tool -- export ./openfs.img 4096 /docs/hello.txt ./hello-export.txt
cargo run -p openfs-tool -- tree ./openfs.img 4096 /
cargo run -p openfs-tool -- stat ./openfs.img 4096 /docs/hello.txt
```

### Useful diagnostics

```sh
# Inspect without writing to the image
cargo run -p openfs-tool -- inspect-image ./openfs.img
cargo run -p openfs-tool -- check-image ./openfs.img 4096

# Audit persistent namespace and allocation ownership
cargo run -p openfs-tool -- audit-image ./openfs.img 4096

# Open the writable recovery path
cargo run -p openfs-tool -- recover-image ./openfs.img 4096

# Display directory contents and free-space information
cargo run -p openfs-tool -- ls ./openfs.img 4096 /
cargo run -p openfs-tool -- space ./openfs.img 4096
```

**Important:** `check-image` is the non-mutating first check and does not replay pending journal records. `audit-image` opens the writable volume path and may replay committed transactions before checking ownership. `recover-image` deliberately uses the writable recovery path. Back up an image before running commands that may mutate it, especially if its state is uncertain. An audit reports inconsistencies; it is not an automatic repair tool.

For additional examples and exact CLI behavior, see the [Rust workspace README](OpenFS/README.md).

## Safety and compatibility

- Treat every byte read from an image as untrusted input.
- Do not assume a successful image test proves physical-device or power-loss durability.
- Do not use OpenFS with important data.
- The development CLI is designed around regular image files; physical-device formatting and OS mounting are not ready for general use.
- Exclusive file locks help coordinate cooperating OpenFS processes, but external programs may ignore advisory locks.
- OpenFS defines its own format. It does not read or write NTFS volumes, and historical OpenFS formats are not implicitly compatible.

See the [security model](OpenFS/docs/SECURITY_MODEL.md), [format specification](OpenFS/docs/FORMAT_SPEC.md), and [compatibility notes](OpenFS/docs/COMPATIBILITY.md) for details.

## Development principles

Contributions and changes should follow the project's engineering rules:

1. Keep one canonical volume implementation and preserve explicit format-version checks.
2. Validate disk data before trusting it; use checked arithmetic and propagate I/O errors.
3. Never free a block until ownership checks prove it is no longer referenced.
4. Specify commit points, recovery behavior and crash cut-points for persistent mutations.
5. Test malformed data, aliasing, allocation failures, I/O failures and reopen/recovery behavior.
6. Keep Linux and Windows CI green; report tests as passed only when they actually pass.
7. Keep raw-device and OS integration work behind explicit safety and recovery gates.

Read the [architecture guide](OpenFS/docs/ARCHITECTURE.md) and [development guide](OpenFS/docs/DEVELOPMENT.md) before making changes.

## Roadmap

Progress is tracked in [`docs/ROADMAP.md`](OpenFS/docs/ROADMAP.md) using the P0–P3 engineering phases. The current documented overall estimate is **60%**, an engineering estimate rather than a measure of production readiness. The largest remaining work is around general transaction/recovery guarantees, complete durable namespace and file semantics, safe device discovery and formatting, and OS integration.

## License

The workspace declares **MIT OR Apache-2.0**. See the crate manifests and repository license files for the project's current licensing details.
