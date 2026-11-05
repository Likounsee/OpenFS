# Native Windows Explorer support

## Required architecture

OpenFS will expose volumes directly through its native Windows WDM filesystem driver. **Dokany is not part of the supported architecture and must not be reintroduced as a fallback.**

The intended user experience is to mount an OpenFS volume through the Windows mount manager and use File Explorer normally: create folders, copy files, overwrite, rename, move and delete them.

## Current status

The WDM driver currently supports validated volume probing/mounting and read-only operations. Native write support is not complete. In particular, do not claim that Explorer can yet copy files through the native driver.

The following kernel operations must be implemented and validated before enabling writes:

- Create/open dispositions and correct share-access semantics.
- File allocation, extent-map updates and EOF changes.
- `IRP_MJ_WRITE` including cached/non-cached I/O rules and paging I/O policy.
- `IRP_MJ_SET_INFORMATION` for EOF/allocation size, rename and disposition.
- Flush ordering, journal commit/recovery and rollback.
- Cleanup/close lifetime, oplocks/locking policy, and safe volume dismount.

## Safety rule

Until these operations are complete, keep the native WDM driver read-only. Do not use Dokany, a user-mode filesystem fallback, or a second simultaneous mount path to bypass missing kernel functionality.

All write tests must first run against disposable disk images in CI and an isolated VM. Physical USB tests come only after the image tests and driver signing/loading workflow are validated.

## Implementation notes

The WDM block-I/O layer now has bounded `OpenFsWriteVolumeBlocks` and
`OpenFsFlushVolumeDevice` primitives. They validate block ranges, byte counts,
sector alignment and `PASSIVE_LEVEL`, and use aligned nonpaged buffers for
synchronous lower-device requests.

These are low-level building blocks only: they are not called by
`IRP_MJ_WRITE`, CREATE, or SET_INFORMATION, and do not by themselves provide
transactions, allocation, or crash safety. Keep the driver read-only until a
complete mutation path uses them with journal recovery and tests.

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
