# Architecture

## Current dependency graph

    openfs-format -> openfs-core
    openfs-block  -> openfs-core

- **openfs-core**: shared error taxonomy and overflow-safe block-range validation.
- **openfs-block**: mutable block-device trait and regular-file image implementation.
- **openfs-format**: byte-level superblock encoding/decoding and CRC32C validation.

These are the only implemented crates at this stage. File namespace, metadata, allocator, journal, recovery, checker, and CLI crates must be added when each has a concrete API and tests, not as empty placeholders.

## Rules

1. Dependencies point downward toward core primitives; no cycles.
2. Disk bytes are untrusted and are decoded field-by-field.
3. All block ranges are checked before I/O; arithmetic is checked.
4. Device I/O errors propagate to callers.
5. No unsafe code is allowed by the workspace lint policy.
6. Metadata mutation must not be advertised as durable until transaction ordering and recovery are implemented.

## Image I/O contract

The image backend uses fixed-size blocks and rejects invalid block sizes, empty/misaligned buffers, zero-length images, and ranges outside the image. Reads use read_exact, writes use write_all, and flush uses File::sync_all. This is a host synchronization request, not a universal physical-media durability guarantee. The backend is mutable (&mut self) and does not itself promise concurrent access synchronization.

## Future layers

The planned direction is format -> block I/O -> metadata/allocation -> transactions and recovery -> filesystem namespace -> tools -> OS adapters. Platform adapters must not leak OS-specific assumptions into the portable format or core.

## Deterministic failure injection

The block crate exposes a one-shot FaultInjectingDevice wrapper in unit-test builds or when the test-support feature is enabled. It fails the next selected read, write, or flush operation before delegating to the image device, then automatically permits the next attempt. This verifies error propagation and retry behavior at the operation boundary; it does not simulate short writes, torn blocks, process crashes, or persistence reordering. Those require additional backend-level tests before transaction recovery can be designed and accepted.
