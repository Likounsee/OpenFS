#pragma once

#include <ntifs.h>
#include "OpenFSSuperblock.h"
#include "OpenFSVolumeState.h"
#include "OpenFSNamespace.h"
#include "OpenFSExtentMap.h"

#define OPENFS_VCB_SIGNATURE 0x4F564342UL
#define OPENFS_FCB_SIGNATURE 0x4F464342UL
#define OPENFS_FCB_NODE_TYPE 0x4F46434EUL
#define OPENFS_DCB_NODE_TYPE 0x4F444342UL

typedef struct _OPENFS_FCB {
    ULONG Signature;
    ULONG NodeType;
    ULONGLONG ObjectIdLow;
    ULONGLONG ObjectIdHigh;
    ULONGLONG ParentIdLow;
    ULONGLONG ParentIdHigh;
    ULONGLONG FileSize;
    ULONGLONG ExtentRootBlock;
    ULONG Generation;
    UCHAR Kind;
    BOOLEAN IsRoot;
    BOOLEAN ExtentsLoaded;
    ULONG ExtentCount;
    POPENFS_EXTENT Extents;
    LIST_ENTRY CacheLink;
} OPENFS_FCB, *POPENFS_FCB;

typedef struct _OPENFS_DCB {
    OPENFS_FCB Fcb;
    ULONG ChildCount;
} OPENFS_DCB, *POPENFS_DCB;

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
    POPENFS_DCB RootDcb;
    LIST_ENTRY FcbList;
    ULONG CachedFcbCount;
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

/* Resolve and cache one child FCB/DCB. Returned nodes stay valid until VCB teardown. */
NTSTATUS
OpenFsLookupChildFcb(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ POPENFS_FCB ParentFcb,
    _In_reads_bytes_(NameLength) const UCHAR* Name,
    _In_ UCHAR NameLength,
    _Outptr_ POPENFS_FCB* ChildFcb
    );


/* Lazily load and validate one FCB's on-disk extent map. */
NTSTATUS
OpenFsLoadFcbExtents(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ POPENFS_FCB FileFcb
    );

/* Read a byte range from a cached regular-file extent map (EOF is short read). */
NTSTATUS
OpenFsReadFcbData(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ POPENFS_FCB FileFcb,
    _In_ ULONGLONG ByteOffset,
    _Out_writes_bytes_(Length) UCHAR* Buffer,
    _In_ ULONG Length,
    _Out_ PULONG BytesRead
    );

/* Resolve a complete UTF-8 root-relative path and cache each FCB/DCB. */
NTSTATUS
OpenFsLookupPathFcb(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_reads_bytes_(PathLength) const UCHAR* Path,
    _In_ USHORT PathLength,
    _Outptr_ POPENFS_FCB* FileFcb
    );

/* Transfer ownership of a fully validated immutable namespace to the private VCB. */
NTSTATUS
OpenFsSetVolumeNamespace(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ POPENFS_NAMESPACE_SNAPSHOT NamespaceSnapshot
    );
