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
