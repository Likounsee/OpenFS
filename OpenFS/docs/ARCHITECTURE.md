# Architecture

## Workspace and dependency direction

The Rust workspace lives in `OpenFS/`; CI workflows live in `.github/workflows/`.

```text
openfs-tool ──> openfs-format ──> openfs-block ──> openfs-core
                         └──────────────────────> openfs-core
```

- **`openfs-core`** — shared error types and checked block-range primitives.
- **`openfs-block`** — bounded block-device interface, regular-file image backend, explicit flush contract and deterministic I/O fault injection.
- **`openfs-format`** — versioned volume layout, superblock validation, allocation bitmap, journal/checkpoints, codecs, extent maps, namespace reconstruction, persistent file APIs and consistency reports.
- **`openfs-tool`** — host-side development-image CLI. It is not an OS mount adapter and must not format physical devices.

There is one active volume implementation in `crates/openfs-format/src/volume.rs`. Version-specific layout calculations are kept within this implementation; do not create parallel format-generation implementations.

## Module ownership

| Module | Responsibility |
| --- | --- |
| `volume.rs` | Layout geometry, superblock I/O, format/open/check entry points, bitmap, persistent inventory and file operations |
| `journal.rs` | Journal record encoding, transaction parsing and recovery planning |
| `journal_engine.rs` | Journal persistence, commit/replay/checkpoint sequencing |
| `checkpoint.rs` | Checkpoint encoding and validation |
| `allocator_extents.rs` | Allocation queries, free-space statistics and ownership reconciliation |
| `metadata.rs` / `directory_entry.rs` | Strict record codecs |
| `extent_map.rs` | Logical-to-physical extent mapping and range operations |
| `namespace.rs` | In-memory object graph, path resolution and graph validation |
| `inventory_descriptor.rs` | Persistent inventory descriptor encoding and validation |
| `verifier.rs` | Cross-layer consistency reports |
| `openfs-block` | Device bounds, exact I/O and flush behavior |
| `openfs-tool` | CLI validation and image-only workflows |

## Safety and compatibility rules

1. Decode disk bytes field-by-field; never serialize native Rust struct layout.
2. Validate magic, version, checksums, reserved bytes, geometry and arithmetic before trusting disk data.
3. Validate block ranges and allocation state before I/O. Propagate I/O failures.
4. After an uncertain journal commit or home-block write, reopen and recover before further mutation.
5. Do not free storage until ownership checks prove it is no longer referenced by the discoverable state.
6. Use fallible allocations for disk-sized staging data wherever possible; allocation failure must return an error before publication.
7. Historical OpenFS C images are not implicitly compatible. Reject them unless a separately reviewed converter is implemented.
8. Image-backed fault injection and host flush calls do not prove physical-media durability.
9. Formatting is destructive. Keep the CLI image-only until device discovery, exclusive access and recovery gates are satisfied.

## Current limits

The canonical format is version 4 with a 16-block journal ring; earlier Rust versions use version-specific geometry. The redo journal covers bounded block updates and is used for allocator bitmap and descriptor ownership transitions, but does not make every multi-step persistent file operation one transaction. Persistent file and namespace APIs exist; operation-specific crash ordering, stale-writer exclusion, large-volume scaling and OS integration remain incomplete.

## Development order

1. Keep formatting, workspace checks and Clippy green on Linux and Windows.
2. Fix correctness and allocation hazards in production paths before adding low-value tests.
3. Close persistence ordering and recovery gaps for each file/namespace mutation.
4. Enforce exclusive writer access and improve checker/discovery scaling.
5. Only then develop verified raw-device workflows and OS integration.
