# Architecture

## Current dependency graph

    openfs-format -> openfs-block -> openfs-core
    openfs-format -> openfs-core

- **openfs-core**: shared error taxonomy and checked block-range validation.
- **openfs-block**: bounded block-device interface, regular-file image backend, explicit flush contract, and test-only deterministic I/O fault/torn-write injection.
- **openfs-format**: explicit v1/v2 superblock codecs, mirrored volume layouts, bitmap initialization and validation, metadata and directory-entry codecs, namespace primitives, journal record/checkpoint codecs, and a persistent single-writer redo journal with replay/checkpoint logic.
- **openfs-tool**: host-side command-line utility; it is not an OS mount adapter or a safe physical-device formatter.

The Rust workspace is experimental. V1 and V2 are separate formats: V1 does not contain the persistent journal, while V2 reserves checkpoint and journal-ring blocks and currently journals bitmap allocator mutations. The journal is not yet a general transaction layer for persistent namespace operations. The existence of metadata and namespace primitives does not mean durable create/read/write/rename/delete is complete.

## Rules

1. Dependencies point downward toward core primitives; no cycles.
2. Disk bytes are untrusted and are decoded field-by-field; reserved bytes, version, checksums, and geometry must be validated.
3. All block ranges and layout arithmetic are checked before I/O.
4. Device I/O errors propagate to callers. After an uncertain journal commit/home-write error, reopen and recover before further mutation.
5. No unsafe code is allowed by the workspace lint policy.
6. Never claim physical-device durability from file-image tests or host flush calls alone.
7. Keep V1 compatibility behavior explicit: V2 volumes must not be silently treated as V1, and historical C-format images must be rejected unless a dedicated converter is implemented.

## Image I/O contract

The image backend uses fixed-size blocks and rejects invalid block sizes, empty/misaligned buffers, zero-length images, and ranges outside the image. Creating an image refuses to overwrite an existing path; destructive replacement requires the explicit `create_truncate` API. Reads use `read_exact`, writes use `write_all`, and flush uses `File::sync_all`. This is a host synchronization request, not a universal physical-media durability guarantee. The backend is mutable (`&mut self`) and does not itself promise concurrent access synchronization.

## Volume formats and limits

- **V1**: primary superblock, bitmap, data region, and backup superblock. Formatting is destructive and not transactional. The standalone journal does not protect V1 formatting or allocator mutations.
- **V2**: primary and backup superblocks, bitmap, two checkpoint blocks, a four-block journal ring, and data region. The current V2 allocator journals bitmap updates. Its fixed journal capacity and current transaction scope are intentionally limited.
- Both layouts validate their reserved/padding bitmap bits on open, but cannot yet verify allocation ownership across persistent objects because the complete file/directory data model and ownership graph are not implemented.
- Formatting and RawDevice tests use disposable regular files. They do not establish safe discovery, exclusion, or formatting of physical USB/system devices.

## Current development order

1. Close the cross-module P0 format/safety review and maintain green Linux and Windows CI.
2. Expand journal crash-cut and flush-order coverage, including reopen from persisted bytes and repeated recovery.
3. Define persistent object ownership and connect all metadata mutations to a proven transaction/recovery layer.
4. Complete persistent namespace/file operations, then the read-only checker and hostile-input tests.
5. Add safe device discovery and OS integration only after the data-integrity gates pass.

Platform adapters must not leak OS-specific assumptions into the portable format or core.
