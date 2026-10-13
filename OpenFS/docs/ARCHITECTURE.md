# Architecture

## Workspace and dependency direction

The Rust workspace lives in `OpenFS/`; CI workflows live in `.github/workflows/`.

```text
openfs-tool ──> openfs-format ──> openfs-block ──> openfs-core
                         └──────────────────────> openfs-core
```

- **`openfs-core`** — shared error types and checked block-range primitives.
- **`openfs-block`** — bounded block-device interface, regular-file image backend, explicit flush contract, and deterministic I/O fault injection for tests.
- **`openfs-format`** — the canonical on-disk layout, superblock validation, allocation bitmap, journal/checkpoints, metadata and directory-entry codecs, extent maps, namespace reconstruction, and consistency reports.
- **`openfs-tool`** — host-side development-image CLI. It is not an OS mount adapter and must not format physical devices.

There is one active volume implementation in `src/volume.rs`. Do not introduce parallel implementations named after format generations. Keep format identification in the on-disk superblock so unsupported layouts fail closed; that identifier is a compatibility guard, not a reason to maintain multiple live implementations.

## Module ownership

| Module | Responsibility |
| --- | --- |
| `volume.rs` | Layout geometry, superblock I/O, format/open/check entry points, bitmap and persistent inventory operations |
| `journal.rs` | Journal record encoding, transaction parsing and recovery planning |
| `journal_engine.rs` | Journal persistence, commit/replay/checkpoint sequencing |
| `checkpoint.rs` | Checkpoint record encoding and validation |
| `allocator_extents.rs` | Allocation queries, free-space statistics and ownership reconciliation |
| `metadata.rs` / `directory_entry.rs` | Strict fixed-record codecs |
| `extent_map.rs` | Logical-to-physical extent mapping and range operations |
| `namespace.rs` | In-memory object graph, path resolution and graph validation |
| `inventory_descriptor.rs` | Persistent inventory descriptor encoding and validation |
| `verifier.rs` | Cross-layer consistency reports |
| `openfs-block` | Device bounds, exact I/O and flush behavior |
| `openfs-tool` | CLI argument validation and image-only workflows |

Keep each module responsible for one domain. Shared invariants should live next to the type that enforces them, and public entry points should orchestrate modules rather than duplicate their validation logic.

## Safety and compatibility rules

1. Decode disk bytes field-by-field; never serialize native Rust struct layout.
2. Validate magic, the format identifier, checksums, reserved bytes, geometry, and arithmetic before trusting disk data.
3. Validate block ranges and allocation state before I/O. Propagate I/O failures.
4. After an uncertain journal commit or home-block write, reopen and recover before further mutation.
5. Do not free storage until ownership checks prove it is unreachable.
6. No unsafe code is allowed by workspace lint policy.
7. Historical OpenFS C images are not silently compatible. Reject them unless a separately reviewed converter is implemented.
8. Image tests and host flush calls do not prove physical USB durability.
9. Formatting is destructive. Keep the CLI image-only until device discovery, exclusive access, and recovery gates are satisfied.

## Current limits

The journal protects allocator bitmap changes, but it is not yet a general transaction layer for durable namespace/file mutations. The descriptor publication sequence can leave a detectable orphan if interrupted between allocation and descriptor write. File reads are available over reconstructed metadata and extent maps, but durable create/write/rename/delete is not complete.

## Development order

1. Keep formatting, workspace tests, and Clippy green on Linux and Windows.
2. Add crash-cut and flush-order coverage around journal commit, replay, and checkpointing.
3. Design a bounded transaction protocol for durable namespace and extent-map updates.
4. Implement persistent create/write/rename/delete with ownership-safe allocation and release.
5. Expand checker/recovery tests before any physical-device or OS integration.
