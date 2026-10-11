# Architecture

## Current dependency graph

    openfs-format -> openfs-block -> openfs-core
    openfs-format -> openfs-core

- **openfs-core**: shared error taxonomy and overflow-safe block-range validation.
- **openfs-block**: mutable block-device trait and regular-file image implementation.
- **openfs-format**: byte-level superblock encoding/decoding, CRC32C, mirrored volume formatting, bitmap initialization and read-only open-time validation.

The format crate now implements the first experimental volume layout and reserved-block bitmap checks. File namespace, object metadata, a usable allocator, journal, recovery, checker, and CLI remain unimplemented.

## Rules

1. Dependencies point downward toward core primitives; no cycles.
2. Disk bytes are untrusted and are decoded field-by-field.
3. All block ranges are checked before I/O; arithmetic is checked.
4. Device I/O errors propagate to callers.
5. No unsafe code is allowed by the workspace lint policy.
6. Metadata mutation must not be advertised as durable until transaction ordering and recovery are implemented.

## Image I/O contract

The image backend uses fixed-size blocks and rejects invalid block sizes, empty/misaligned buffers, zero-length images, and ranges outside the image. Creating an image refuses to overwrite an existing path; destructive replacement requires the explicit create_truncate API. Reads use read_exact, writes use write_all, and flush uses File::sync_all. This is a host synchronization request, not a universal physical-media durability guarantee. The backend is mutable (&mut self) and does not itself promise concurrent access synchronization.

## Volume format limits

Formatting is destructive and currently has no transactional rollback. Open-time validation checks mirrored superblocks and reserved bitmap bits, but does not scan every allocation bit for ownership conflicts because no object metadata exists yet. The bitmap is initialized for future allocation; no allocation/free API is exposed.

## Future layers

The planned direction is format -> block I/O -> metadata/allocation -> transactions and recovery -> filesystem namespace -> tools -> OS adapters. Platform adapters must not leak OS-specific assumptions into the portable format or core.

## Deterministic failure injection

The block crate exposes a one-shot FaultInjectingDevice wrapper in unit-test builds or when the test-support feature is enabled. It fails the next selected read, write, or flush operation before delegating to the image device, then automatically permits the next attempt. This verifies error propagation and retry behavior at the operation boundary; it does not simulate short writes, torn blocks, process crashes, or persistence reordering. Those require additional backend-level tests before transaction recovery can be designed and accepted.
