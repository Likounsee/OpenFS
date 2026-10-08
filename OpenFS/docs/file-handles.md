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
that open-file object. A reference should still be retained before another thread or component receives a handle pointer, but mounted-runtime handles now have an additional quiescent lifetime barrier: the final close marks the object closed and retires it instead of immediately destroying its mutex. Concurrent close/stat/read/write waiters can therefore safely observe the closed state. The runtime reclaims retired handle objects only after admission is closed and active runtime users have drained.

## Namespace lifetime

Open-but-unlinked files are tracked through the runtime handle registry and
persistent orphan mechanism. The final handle reference releases the runtime
registry entry and, when the inode is marked orphaned and no references remain,
reclaims the inode's storage. The orphan protocol is journal-aware and is
recovered during mount.

The handle API still requires the caller to retain a reference before handing
a raw handle pointer to another thread. This is necessary because the final
close is allowed to destroy the object itself.

## Concurrency

Handle operations use the HANDLE lock rank and refresh the inode snapshot
under the INODE lock before operations whose result depends on current inode
state. This prevents one open-file object from writing a stale size or extent
map after another handle has modified the inode.

Runtime-owned registries are protected by runtime admission and their own
registry locks. Namespace operations are admitted into the runtime before the
directory lock is acquired, so unmount can safely close admission and wait for
active users to drain.

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