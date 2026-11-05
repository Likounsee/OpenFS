# Native Windows Explorer support

## Required architecture

OpenFS will expose volumes directly through its native Windows WDM filesystem driver. **Dokany is not part of the supported architecture and must not be reintroduced as a fallback.**

The intended user experience is to mount an OpenFS volume through the Windows mount manager and use File Explorer normally: create folders, copy files, overwrite, rename, move and delete them.

## Current status

The WDM driver supports validated volume probing/mounting and read operations. An experimental, non-journaled overwrite path has now been added for isolated VM testing only. It can overwrite bytes inside an existing regular file only when every touched logical block is already allocated and the write remains within the current EOF.

This is not general Explorer write support and is not safe for valuable data. File creation, extension, sparse-hole allocation, rename, move, delete, metadata updates and crash-atomic writes remain unsupported.

The following kernel operations must be implemented and validated before enabling writes:

- Create/open dispositions and correct share-access semantics.
- File allocation, extent-map updates and EOF changes.
- `IRP_MJ_WRITE` including cached/non-cached I/O rules and paging I/O policy.
- `IRP_MJ_SET_INFORMATION` for EOF/allocation size, rename and disposition.
- Flush ordering, journal commit/recovery and rollback.
- Cleanup/close lifetime, oplocks/locking policy, and safe volume dismount.

## Safety rule

Keep the experimental overwrite path restricted to disposable disk images and isolated VMs. Do not use it on a physical disk or any volume containing valuable data. Do not use Dokany, a user-mode filesystem fallback, or a second simultaneous mount path to bypass missing kernel functionality.

All write tests must first run against disposable disk images in CI and an isolated VM. Physical USB tests come only after the image tests and driver signing/loading workflow are validated.

## Implementation notes

The WDM block-I/O layer now has bounded `OpenFsWriteVolumeBlocks` and
`OpenFsFlushVolumeDevice` primitives. They validate block ranges, byte counts,
sector alignment and `PASSIVE_LEVEL`, and use aligned nonpaged buffers for
synchronous lower-device requests.

The current experimental `IRP_MJ_WRITE` path calls these primitives for
bounded in-place overwrites. It holds the VCB resource exclusively, uses
read/modify/write for partial blocks, rejects sparse holes and EOF extension,
and flushes after a successful request. It does not emit REDO records and is
not crash-atomic: a power loss or I/O failure can leave the target file's data
partially updated. It must remain VM-only until journal integration and
fault-injection tests are complete.

The format library already has persistent namespace operations for creating
empty files/directories, renaming/moving and removing objects, plus journaled
allocation primitives. It does not yet provide a complete persistent non-empty
file-data write operation suitable for direct invocation by the WDM driver.
The remaining integration work must bridge those semantics into the kernel
without duplicating or weakening the on-disk invariants.

## Write-range preflight added

The native WDM project now includes `OpenFSWritePlan.c/.h`. Its side-effect-free planner converts a byte range into the complete physical-block list using a validated extent map. It rejects EOF extension, sparse holes, out-of-range physical blocks, duplicate physical targets, arithmetic overflow, and insufficient caller capacity before any write can begin. Host C tests cover multi-extent and partial-block ranges, zero-length writes, EOF extension, sparse holes, capacity limits, and aliased physical blocks.

This is deliberately not advertised as Explorer write support: the planner does not stage block images or emit redo records. The VCB still has no live persistent-journal writer context, and the kernel driver does not yet have a C implementation of the on-disk redo transaction engine. The next required layer is journal open/recovery/commit in the WDM code, followed by safe allocation and `IRP_MJ_WRITE` integration.

The FCB layer now exposes `OpenFsPlanFcbWrite`, which loads the file's extent map, validates that the FCB still belongs to the mounted VCB, checks the mounted-volume state, and delegates to the write planner while holding rundown protection and the VCB resource. This is the kernel-side bridge for future `IRP_MJ_WRITE` staging; it remains preflight-only and does not alter disk contents.


## Native journal codec groundwork

The WDM driver now has a C decoder for the existing on-disk journal checkpoint
and single-block record formats. It validates CRC32C, format versions, reserved
bytes, zero padding, sequence and transaction identifiers, ring-relative
checkpoint positions, record kinds, and control-record payload rules. Record
payloads are exposed as zero-copy views into the caller-owned block buffer.

This is a codec only. It does not yet select the newest valid checkpoint,
scan the ring, validate transaction ordering across records, replay REDO,
write journal records, or publish checkpoints. Those steps must be implemented
and tested before any mutating IRP is enabled.


The C journal layer also validates decoded record streams against the single-writer
protocol: contiguous sequence numbers, strictly increasing transaction IDs,
non-interleaved BEGIN/UPDATE/COMMIT/ABORT sequences, and at least one REDO update
for every committed transaction. It returns index ranges for committed updates
only; a final transaction without COMMIT is ignored. Output capacity is checked
before any transaction descriptors are published. This is still planning only:
ring scanning, torn-tail classification, redo application and durable checkpoint
publication remain unimplemented.


The journal codec now also encodes zero-padded checkpoint and record blocks with
the exact little-endian field layout and CRC32C used by the Rust format library.
The host tests round-trip both encoders through the strict decoders and reject
invalid control-record payloads and unsupported non-zero ring positions.


## REDO image decoding and transaction-ID continuity

The journal recovery planner now checks that each post-checkpoint BEGIN consumes
exactly the checkpoint's next transaction ID, incrementing by one for every
subsequent BEGIN. Gaps and reuse are rejected instead of being accepted as merely
increasing IDs.

A bounded REDO image decoder validates the complete chunk sequence (target block,
transaction ID, chunk index/count and exact data lengths) before changing its
caller-provided output buffer. It reconstructs one complete filesystem block
without allocating memory, allowing recovery to preflight and replay images one
at a time. This still does not perform ring scanning, checkpoint selection or
home-block replay itself.


## Ring scanning

A bounded in-memory ring scanner now mirrors the Rust recovery rule for the
post-checkpoint record prefix. It ignores records already covered by the
checkpoint, retains only a contiguous prefix from ring slot zero, rejects a
COMMIT found after a gap or malformed slot, and ignores an uncommitted suffix.
Returned payloads remain zero-copy views into caller-owned ring buffers. The
scanner is deliberately separate from block-device I/O and REDO application;
the caller must keep those buffers alive and pass the result through transaction
and REDO validation before replay.


## Checkpoint pair selection

The journal layer now includes a pure checkpoint-pair selector matching the
format's generation and durable-counter rules: it accepts the initial
generation-1/generation-2 pair, rejects conflicting copies at one generation,
generation gaps, counter regressions and sequence advancement without a new
transaction ID, and chooses the newer valid copy when the pair is consistent.
The WDM device-I/O layer now reads both checkpoint blocks during probing and
the mount path runs recovery before publishing the VCB namespace.


## Journal scan and mount-time recovery

The WDM probe reads both on-disk checkpoint blocks and the complete bounded
journal ring, selects the valid checkpoint pair, scans the post-checkpoint
prefix, and validates transaction ordering. During mount, a separate recovery
pass validates every committed REDO image and its transaction ID before any
home-block write. The portable target validator rejects out-of-data-region
targets and duplicate targets within one transaction; it is also covered by
host C tests.

Recovery clears an uncommitted suffix, replays committed home blocks, flushes
them, publishes the alternate checkpoint copy, flushes again, and then clears
the now-stale ring before returning. The namespace is probed again after replay
so the published snapshot reflects recovered metadata. If replay fails before
checkpoint publication, committed REDO remains available for another mount
attempt. If ring cleanup fails after checkpoint publication, remaining records
are stale by sequence and can be safely cleaned on a later mount.

This is a recovery engine, not yet a complete write engine: there is still no
journal transaction writer connected to file mutations, no allocation/namespace
mutation transaction, and no Explorer write support.


## Native transaction encoder (host-tested)

The WDM journal now has a portable transaction encoder that prepares a complete
BEGIN / UPDATE-chunk / COMMIT sequence in caller-owned ring storage before any
persistent write. It validates block targets, rejects duplicates, checks ring
capacity and sequence/transaction counter overflow, and produces the same
chunk payload format consumed by the REDO decoder. Host C tests decode the
resulting records and reconstruct the original full-block image.

This is preparation only: it is not yet connected to device I/O, does not
publish a durable COMMIT, and does not make file writes available to Windows.
