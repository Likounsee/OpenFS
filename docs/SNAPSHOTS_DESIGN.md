# OpenFS Snapshot Architecture

## Status

Design phase for the P1 snapshot milestone. No snapshot API is advertised as implemented until persistence, crash recovery, deletion, FSCK, concurrency and fault-injection are complete.

## Why the current CoW layer is not enough

OpenFS already has data-block reference counting and a regular-file CoW clone path. That is not sufficient for filesystem snapshots: inode-table blocks, directory metadata, extent-tree blocks, bitmaps, refcount metadata, and the superblock/journal state can currently be updated in place. A snapshot that only remembers a root inode would therefore observe metadata changes made by the live filesystem.

A correct snapshot must provide a stable view of all blocks reachable from the snapshot root metadata, while the live filesystem can continue mutating its own view.

## Required architecture

### 1. Persistent snapshot catalog

Add a dedicated on-disk snapshot catalog described by the superblock. Each record must contain at least:
- snapshot ID (never reused while a prior record can still be referenced)
- creation generation/time
- root metadata reference
- snapshot state (`CREATING`, `COMMITTED`, `DELETING`)
- checksum/version
- deletion progress cursor when deletion is interrupted

The catalog must have a fixed, validated geometry or a separately allocated metadata region. It must never be implemented as a hidden directory or an xattr workaround.

### 2. Immutable metadata view

Introduce a metadata-root abstraction so a snapshot points to a stable inode-table/metadata root instead of the live inode table. Every metadata block reachable from that root becomes CoW-protected.

The first implementation should cover:
1. inode table blocks
2. directory/data blocks
3. extent-tree blocks
4. xattr blocks when those become snapshot-visible
5. bitmap/refcount metadata required by the snapshot implementation

The live superblock/journal remain global recovery infrastructure; snapshot state changes themselves must be journaled.

### 3. General CoW block primitive

Extend the current data-only refcount logic into a block-level primitive with:
- acquire/release
- overflow/underflow detection
- transactional rollback
- durable publication ordering
- copy-before-write when reference count > 1
- explicit metadata/data block type information where required by FSCK

The existing `openfs_cow_clone_inode()` should be refactored to use this primitive rather than remaining a special-case regular-file operation.

### 4. Snapshot creation transaction

Creation must be atomic from the mount/recovery point of view:

BEGIN → reserve catalog slot → acquire snapshot metadata root → persist all required references → durable COMMIT → publish snapshot

A crash before COMMIT must leave no visible snapshot. A crash after COMMIT must leave a complete snapshot that can be mounted/read.

### 5. Snapshot deletion

Deletion cannot assume all referenced blocks fit in one transaction. Use resumable `DELETING` state with a durable cursor. Each batch releases a bounded number of references. Recovery resumes deletion after a crash.

Only after the final reference release is durable may the catalog slot return to `FREE`.

### 6. Snapshot reads

Add an explicit read-only snapshot handle/context. Snapshot reads must never mutate live inode metadata, timestamps, directory entries, allocation state, or reference counts except through the snapshot manager's internal bookkeeping.

### 7. FSCK

FSCK must validate:
- catalog checksum/version/state
- unique snapshot IDs
- valid metadata-root references
- reference-count reachability for every live/snapshot view
- no snapshot references a free/out-of-range block
- no block has an impossible reference count
- interrupted `CREATING` / `DELETING` states are classified and safely recovered
- live and snapshot ownership agree with the refcount table

Repair must be conservative: automatic repair is allowed only when ownership is unambiguous. Ambiguous snapshot ownership is a corruption error, not a guess.

## API target

The eventual public API should expose operations equivalent to:
- create snapshot
- open/list snapshots
- query snapshot metadata
- delete snapshot
- open a snapshot read-only
- close snapshot

IDs must be persistent and independent of inode numbers.

## Concurrency rules

Snapshot operations participate in the existing runtime lock hierarchy. They must not introduce an inverse ordering between allocation, directory, inode and journal locks. Long deletion work must not hold global locks for the entire scan.

## Durability contract

For snapshot creation, the durable order is:

catalog intent → reference acquisition → metadata-root publication → COMMIT → flush → catalog state COMMITTED

For deletion:

state DELETING + cursor → reference releases → durable cursor advancement → final FREE publication

A rollback failure is corruption-class and must remain visible to recovery/FSCK.

## Implementation order

1. Add on-disk geometry/version validation for a snapshot catalog.
2. Add general metadata-block CoW primitives and fault-injection tests.
3. Make inode-table updates CoW-aware.
4. Make directory and extent-tree metadata updates CoW-aware.
5. Implement snapshot catalog transactions.
6. Implement snapshot create/open/read.
7. Implement resumable deletion.
8. Extend FSCK check/repair.
9. Add crash-cut and concurrency matrices.
10. Only then mark snapshots implemented in the main README.

## Non-goals

Do not implement snapshots by:
- copying only the root inode
- cloning only regular files
- using a hidden directory
- claiming a snapshot is read-only while sharing mutable inode-table blocks
- silently treating an interrupted deletion as completed

Those approaches would provide a misleading API without snapshot correctness guarantees.
