# OpenFS file handles

The core file-handle API is the first layer above the existing inode/file APIs
that models an OS-style open file object.

## Current API

The file-handle object contains:

- the backing block device and filesystem geometry;
- a private inode snapshot;
- the current file offset;
- access/open flags;
- a reference count;
- an internal mutex.

The API currently provides:

- open with RDONLY, WRONLY, and RDWR;
- CREAT, EXCL, TRUNC, and APPEND;
- read;
- write;
- seek;
- truncate;
- stat;
- dup;
- explicit retain/release-style reference management through retain and close.

dup shares the same open-file object and therefore shares its current file
offset, matching the usual descriptor-duplication model.

## Lifetime

The handle has a reference count. Closing one reference does not destroy the
underlying object while other references still exist.

The handle mutex serializes offset and inode-snapshot updates performed through
that open-file object. A reference must be retained before another thread or
component receives a handle pointer; the final close may destroy the object.
Concurrent operations are supported while at least one valid reference exists.
A caller must not race a final close against an operation that has not first
acquired its own reference.

## Current limitation

The first implementation deliberately does not yet implement the full
persistent orphan/unlink protocol. In particular, an inode whose final
directory link is removed must eventually be governed by a persistent orphan
mechanism so that open-but-unlinked files remain safe across crashes.

That work belongs to the next namespace-lifetime phase. The current handle API
therefore must not be treated as the final implementation of POSIX/Windows
unlink-while-open semantics.

## Design direction

    OS descriptor
        |
    OpenFS file handle
        |
    inode / namespace locking
        |
    OpenFS file API
        |
    transaction / journal
        |
    block device

The API is intentionally independent of Windows, Linux, and ArchiaOS so the
same open-file semantics can be reused by the future VFS and native filesystem
drivers.