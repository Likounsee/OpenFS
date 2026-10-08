# Adapter contract

OpenFS core is intentionally OS-independent. Platform code must stay outside
`OpenFS/src/` and communicate through `openfs_block_device_t`.

## Required callbacks

An adapter supplies:

- `read(context, first_block, count, buffer)`;
- `write(context, first_block, count, buffer)`;
- `flush(context)`;
- `block_size`;
- `block_count`.

Callbacks must:

1. reject zero-length or out-of-range requests;
2. never expose partial success as `OPENFS_IO_OK`;
3. preserve the filesystem block size for every request;
4. make `flush` a durability barrier for the backing medium;
5. serialize concurrent access if the backing object is not intrinsically thread-safe.

## Platform split

A Linux adapter should normally use a file descriptor, block-device descriptor,
or another explicitly managed backing object. A Windows adapter should use the
corresponding Win32/NT I/O handle. An ArchiaOS adapter should map the same
contract onto its storage subsystem.

These adapters are integration layers: filesystem policy, metadata layout,
journal semantics, allocation, and path handling remain in the OpenFS core.

## Crash-consistency requirement

The transaction layer relies on the callback contract:

`BEGIN/DATA -> durable COMMIT -> final writes -> flush -> checkpoint`.

An adapter that reports successful `flush` without providing the required
durability guarantee can invalidate crash-recovery assumptions.

## Testing adapters

Every adapter should provide fault injection for:

- read failure;
- final-data write failure;
- journal write failure;
- flush failure;
- short/out-of-range requests;
- interruption between COMMIT and final writes.

The core test suite uses an in-memory block device for deterministic testing.
Platform adapters should add their own integration tests without changing core
on-disk semantics.

## Reference implementations

The repository now contains platform glue under `OpenFS/adapters/`:

- `linux/` — file-backed adapter using `pread`/`pwrite` and `fsync`;
- `windows/` — Win32 file-backed adapter using overlapped `ReadFile`/`WriteFile` and `FlushFileBuffers`;
- `archiaos/` — callback bridge for the ArchiaOS storage subsystem.

The Linux and ArchiaOS adapters have deterministic contract tests in
`OpenFS/adapters/tests/`. The Windows implementation is compiled and exercised by the GitHub Actions
`windows-latest` job, including the file-backed adapter integration test.

The adapters deliberately expose only a block device to the core. They do not
translate filesystem paths, permissions, directory operations, journal policy,
or on-disk structures.

## Durability boundary

`flush` is not merely a cache flush hint. It is the adapter's acknowledgement
that writes issued before it satisfy the durability guarantee required by the
OpenFS transaction layer. If the underlying storage API cannot provide that
guarantee, the adapter must return `OPENFS_IO_IO_ERROR` rather than claiming
success.

The Linux and Windows file adapters reject zero-length/out-of-range requests
and reject successful short transfers. The ArchiaOS bridge validates bounds
before delegating to the supplied storage callbacks; the ArchiaOS subsystem
must provide the same no-partial-success and durability guarantees.

## Concurrency

An adapter must serialize access when its backing object is not intrinsically
thread-safe. The Linux and Windows implementations use position-explicit I/O
so they do not depend on a mutable shared file position for ordinary block
requests. ArchiaOS callback implementations are responsible for their own
concurrency policy.

## Read-only mode

A read-only adapter may expose `OPENFS_IO_READ_ONLY` from `write`. OpenFS mount
and read paths can use such a device for inspection, while mutation APIs must
surface the write failure instead of silently changing semantics.

## Core concurrency boundary

OpenFS core path APIs are not a general thread-safe/reentrant interface. In particular, path lookup currently uses the C `strtok()` stateful tokenizer. Callers must serialize concurrent path operations that share a process. This is a documented limitation rather than a release-blocking defect because the public API does not promise concurrent/reentrant path calls.
