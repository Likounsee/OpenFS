# OpenFS architecture

OpenFS is an OS-independent filesystem core. ArchiaOS is the first planned
integration, but it is an adapter and must never become a dependency of the core.

## Layers

1. Format: versioned on-disk structures and validation.
2. Block I/O: checked 64-bit block addressing and explicit flush semantics.
3. Metadata: inodes, allocation, directories, links and timestamps.
4. Transactions: write-ahead journal, buffered transaction device and crash recovery.
5. Filesystem: mount, namespace and file operations.
6. Tools: formatter, fsck and recovery utilities.
7. OS adapters: ArchiaOS, Linux, Windows, BSD and others.

The dependency direction is from higher layers to lower layers. The core must not
include kernel headers or assume a CPU architecture.

## Safety invariants

Every disk-derived integer is untrusted. Addition, multiplication, ranges,
counts and offsets are checked before use. On-disk structures are serialized
explicitly; compiler struct layout is never part of the disk format.

Metadata mutation will not be exposed as durable filesystem behavior until its
transaction ordering and crash recovery rules are implemented and tested.

Checksums are validation mechanisms, not permission to silently overwrite
corruption.

## Future compatibility

Feature flags and version fields are reserved for future compression,
encryption, snapshots, quotas, extents and other extensions. New features must
not reinterpret existing mandatory fields without an explicit format-version
transition.
