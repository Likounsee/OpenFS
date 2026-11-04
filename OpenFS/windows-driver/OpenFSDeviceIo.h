#pragma once

#include <ntifs.h>
#include "OpenFSNamespace.h"

/*
 * Read a bounded range of complete blocks from the backing volume device.
 * BufferLength must equal BlockSize * BlockCount exactly.
 */
NTSTATUS
OpenFsReadVolumeBlocks(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG BlockSize,
    _In_ ULONGLONG TotalBlocks,
    _In_ ULONGLONG FirstBlock,
    _In_ ULONG BlockCount,
    _Out_writes_bytes_(BufferLength) UCHAR* Buffer,
    _In_ SIZE_T BufferLength
    );

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



/* Read data-region allocation bits into a Windows-compatible bitmap buffer. */
NTSTATUS
OpenFsReadDataBitmap(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ const OPENFS_VOLUME_GEOMETRY* Geometry,
    _In_ ULONGLONG DataStartBlock,
    _In_ ULONGLONG DataBlockCount,
    _In_ ULONGLONG StartingLcn,
    _Out_writes_bytes_(BitmapBytes) UCHAR* Bitmap,
    _In_ SIZE_T BitmapBytes,
    _Out_ PULONGLONG BitsRead
    );


/* Count free data-region blocks by reading the validated on-disk bitmap. */
NTSTATUS
OpenFsCountFreeDataBlocks(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ const OPENFS_VOLUME_GEOMETRY* Geometry,
    _Out_ ULONGLONG* FreeBlocks
    );
