#pragma once

#include <ntifs.h>
#include "OpenFSSuperblock.h"
#include "OpenFSVolumeState.h"

#define OPENFS_VCB_SIGNATURE 0x4F564342UL

/*
 * One VCB lives in the extension of each unpublished volume device object.
 * Resource protects State and the future VPB association. The VDO remains
 * private until a later mount implementation atomically associates its VPB.
 */
typedef struct _OPENFS_VCB {
    ULONG Signature;
    ERESOURCE Resource;
    EX_RUNDOWN_REF RundownProtection;
    OPENFS_VOLUME_STATE State;
    PDEVICE_OBJECT VolumeDeviceObject;
    PDEVICE_OBJECT RealDeviceObject;
    PVPB Vpb;
    OPENFS_VOLUME_GEOMETRY Geometry;
    POPENFS_NAMESPACE_SNAPSHOT Namespace;
} OPENFS_VCB, *POPENFS_VCB;

/*
 * Create an initialized, but not yet mounted or published, volume device.
 * The caller owns the returned device and must destroy it on mount rollback.
 */
NTSTATUS
OpenFsCreateVolumeDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT RealDeviceObject,
    _In_ const OPENFS_VOLUME_GEOMETRY* Geometry,
    _Outptr_ PDEVICE_OBJECT* VolumeDeviceObject
    );

/*
 * Destroy only a private, unmounted volume device. A VCB with an associated
 * VPB or a mounted/dismounting state is deliberately not eligible.
 */
NTSTATUS
OpenFsDestroyVolumeDevice(
    _In_ PDEVICE_OBJECT VolumeDeviceObject
    );

/* Thread-safe VCB state accessors for the future mount/dismount path. */
NTSTATUS
OpenFsTransitionVolumeState(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ OPENFS_VOLUME_STATE Expected,
    _In_ OPENFS_VOLUME_STATE Next
    );

NTSTATUS
OpenFsQueryVolumeState(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _Out_ OPENFS_VOLUME_STATE* State
    );

/*
 * Reserve the real device's VPB while a mount attempt is being prepared.
 * Reservation is not a mount: it does not set VPB_MOUNTED or publish a VDO.
 */
NTSTATUS
OpenFsReserveVolumeVpb(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PVPB Vpb
    );

/*
 * Final mount commit: publish a fully initialized VDO through its reserved
 * VPB. The I/O manager, not OpenFS, sets VPB_MOUNTED after IRP completion.
 */
NTSTATUS
OpenFsPublishVolumeVpb(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PVPB Vpb
    );

/* Release a failed mount attempt's VPB reservation. */
NTSTATUS
OpenFsReleaseVolumeVpb(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PVPB Vpb
    );
