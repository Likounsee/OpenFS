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
