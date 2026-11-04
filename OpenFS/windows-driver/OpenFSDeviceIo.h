#pragma once

#include <ntifs.h>
#include "OpenFSSuperblock.h"

/*
 * Read-only recognition probe for a partition/volume device supplied by
 * IRP_MN_MOUNT_VOLUME. A successful probe means the bytes are a valid OpenFS
 * volume; it does not mean that a VCB/VPB mount has been established.
 */
NTSTATUS
OpenFsProbeVolume(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _Out_ POPENFS_VOLUME_GEOMETRY Geometry,
    _Outptr_ POPENFS_NAMESPACE_SNAPSHOT* NamespaceSnapshot
    );
