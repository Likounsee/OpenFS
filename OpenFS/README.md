# OpenFS Rust workspace

This directory is the Rust project root. Repository-level overview is in `../README.md`; GitHub Actions workflows live in `../.github/workflows/`.

## Workspace crates

- `openfs-core`: shared errors and checked block-range validation.
- `openfs-block`: bounded block reads/writes, explicit flush, safe create-by-default image handling, and deterministic read/write/flush failure injection.
- `openfs-format`: the single canonical volume layout; checksummed superblocks; bitmap allocation and fragmentation statistics; journal/checkpoint encoding and replay; metadata, directory-entry, extent-map, namespace, persistent inventory and ownership-verification primitives. Public entry points are `format_volume`, `open_volume`, and `check_volume`.
- `openfs-tool`: safe-first command-line tools for regular image files only.

The disk format uses an explicit identifier checked on open. Keep one active layout implementation; do not add a parallel format implementation for an alternate generation. See [architecture](docs/ARCHITECTURE.md), [format specification](docs/FORMAT_SPEC.md), and [development guide](docs/DEVELOPMENT.md).

## Commands

Run from this directory:

```sh
cargo fmt --all -- --check
cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
```

Create a new 1 GiB development image:

```sh
cargo run -p openfs-tool -- create-image ./openfs.img 1024
```

Inspect the image without writing:

```sh
cargo run -p openfs-tool -- inspect-image ./openfs.img
cargo run -p openfs-tool -- check-image ./openfs.img 4096
```

The checker is read-only and does not replay pending journal records. Opening a volume for normal writable operations performs journal recovery.

## Current limits

The journal currently protects allocator bitmap changes, but does not yet provide a general transaction layer for persistent namespace and extent-map mutations. Persistent inventory publication can leave a detectable orphan if interrupted between descriptor allocation and descriptor write. File reads support sparse zero-fill, ranges and EOF, but durable file/directory mutation and mounting are not complete.

Image-backed tests do not establish physical-device durability. Do not use important data or physical USB devices.
