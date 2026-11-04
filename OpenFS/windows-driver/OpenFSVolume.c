#include "OpenFSVolume.h"
#include "OpenFSLayout.h"

#define OPENFS_FCB_POOL_TAG 0x4F734646UL

NTSTATUS
OpenFsCreateVolumeDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT RealDeviceObject,
    _In_ const OPENFS_VOLUME_GEOMETRY* Geometry,
    _Outptr_ PDEVICE_OBJECT* VolumeDeviceObject
    )
{
    NTSTATUS status;
    PDEVICE_OBJECT volumeDeviceObject;
    POPENFS_VCB vcb;
    OPENFS_VOLUME_LAYOUT layout;

    if (DriverObject == NULL ||
        RealDeviceObject == NULL ||
        Geometry == NULL ||
        VolumeDeviceObject == NULL ||
        Geometry->BlockSize == 0 ||
        Geometry->TotalBlocks == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    *VolumeDeviceObject = NULL;

    status = OpenFsCalculateVolumeLayout(
        Geometry->BlockSize,
        Geometry->TotalBlocks,
        Geometry->Version,
        &layout
        );
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /*
     * StackSize is a signed CCHAR. Refuse an impossible stack depth instead
     * of allowing the addition to wrap and corrupt future IRP forwarding.
     */
    if (RealDeviceObject->StackSize >= (CCHAR)0x7f) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    status = IoCreateDevice(
        DriverObject,
        sizeof(OPENFS_VCB),
        NULL,
        FILE_DEVICE_DISK_FILE_SYSTEM,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &volumeDeviceObject
        );
    if (!NT_SUCCESS(status)) {
        return status;
    }

    vcb = (POPENFS_VCB)volumeDeviceObject->DeviceExtension;
    RtlZeroMemory(vcb, sizeof(*vcb));

    status = ExInitializeResourceLite(&vcb->Resource);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(volumeDeviceObject);
        return status;
    }

    ExInitializeRundownProtection(&vcb->RundownProtection);

    ObReferenceObject(RealDeviceObject);

    vcb->Signature = OPENFS_VCB_SIGNATURE;
    vcb->State = OpenFsVolumeInitializing;
    vcb->VolumeDeviceObject = volumeDeviceObject;
    vcb->RealDeviceObject = RealDeviceObject;
    vcb->Vpb = NULL;
    vcb->Namespace = NULL;
    vcb->RootDcb = NULL;
    InitializeListHead(&vcb->FcbList);
    vcb->CachedFcbCount = 0;
    RtlCopyMemory(&vcb->Geometry, Geometry, sizeof(vcb->Geometry));

    volumeDeviceObject->StackSize =
        (CCHAR)(RealDeviceObject->StackSize + 1);
    volumeDeviceObject->AlignmentRequirement =
        RealDeviceObject->AlignmentRequirement;
    volumeDeviceObject->SectorSize = RealDeviceObject->SectorSize;

    /*
     * Do not clear DO_DEVICE_INITIALIZING here. This object is still private:
     * the VPB association and publication sequence belong to the mount path.
     */
    *VolumeDeviceObject = volumeDeviceObject;
    return STATUS_SUCCESS;
}

NTSTATUS
OpenFsDestroyVolumeDevice(
    _In_ PDEVICE_OBJECT VolumeDeviceObject
    )
{
    NTSTATUS status;
    POPENFS_VCB vcb;

    if (VolumeDeviceObject == NULL) return STATUS_INVALID_PARAMETER;
    ObReferenceObject(VolumeDeviceObject);

    vcb = (POPENFS_VCB)VolumeDeviceObject->DeviceExtension;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!OpenFsVolumeStateCanDestroy(vcb->State, vcb->Vpb != NULL)) {
        ExReleaseResourceLite(&vcb->Resource);
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_DEVICE_BUSY;
    }
    if (!OpenFsVolumeStateTryTransition(&vcb->State,
            OpenFsVolumeInitializing, OpenFsVolumeDead)) {
        ExReleaseResourceLite(&vcb->Resource);
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INVALID_DEVICE_STATE;
    }
    ExReleaseResourceLite(&vcb->Resource);

    ExRundownCompleted(&vcb->RundownProtection);
    ExWaitForRundownProtectionRelease(&vcb->RundownProtection);

    status = ExDeleteResourceLite(&vcb->Resource);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(VolumeDeviceObject);
        return status;
    }
    vcb->Signature = 0;
    while (!IsListEmpty(&vcb->FcbList)) {
        PLIST_ENTRY link = RemoveHeadList(&vcb->FcbList);
        POPENFS_FCB fcb = CONTAINING_RECORD(link, OPENFS_FCB, CacheLink);
        ExFreePoolWithTag(fcb, OPENFS_FCB_POOL_TAG);
    }
    vcb->RootDcb = NULL;
    vcb->CachedFcbCount = 0;
    OpenFsFreeNamespaceSnapshot(vcb->Namespace);
    vcb->Namespace = NULL;
    if (vcb->RealDeviceObject != NULL) {
        ObDereferenceObject(vcb->RealDeviceObject);
        vcb->RealDeviceObject = NULL;
    }
    IoDeleteDevice(VolumeDeviceObject);
    ObDereferenceObject(VolumeDeviceObject);
    return STATUS_SUCCESS;
}


NTSTATUS
OpenFsTransitionVolumeState(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ OPENFS_VOLUME_STATE Expected,
    _In_ OPENFS_VOLUME_STATE Next
    )
{
    POPENFS_VCB vcb;
    NTSTATUS status = STATUS_SUCCESS;

    if (VolumeDeviceObject == NULL) return STATUS_INVALID_PARAMETER;
    ObReferenceObject(VolumeDeviceObject);
    vcb = (POPENFS_VCB)VolumeDeviceObject->DeviceExtension;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!ExAcquireRundownProtection(&vcb->RundownProtection)) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_DELETE_PENDING;
    }
    if (!ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)) {
        ExReleaseRundownProtection(&vcb->RundownProtection);
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (vcb->Signature != OPENFS_VCB_SIGNATURE ||
        !OpenFsVolumeStateTryTransition(&vcb->State, Expected, Next)) {
        status = STATUS_INVALID_DEVICE_STATE;
    }
    ExReleaseResourceLite(&vcb->Resource);
    ExReleaseRundownProtection(&vcb->RundownProtection);
    ObDereferenceObject(VolumeDeviceObject);
    return status;
}


NTSTATUS
OpenFsQueryVolumeState(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _Out_ OPENFS_VOLUME_STATE* State
    )
{
    POPENFS_VCB vcb;
    NTSTATUS status = STATUS_SUCCESS;

    if (VolumeDeviceObject == NULL || State == NULL)
        return STATUS_INVALID_PARAMETER;
    ObReferenceObject(VolumeDeviceObject);
    vcb = (POPENFS_VCB)VolumeDeviceObject->DeviceExtension;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!ExAcquireRundownProtection(&vcb->RundownProtection)) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_DELETE_PENDING;
    }
    if (!ExAcquireResourceSharedLite(&vcb->Resource, TRUE)) {
        ExReleaseRundownProtection(&vcb->RundownProtection);
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (vcb->Signature != OPENFS_VCB_SIGNATURE) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else {
        *State = vcb->State;
    }
    ExReleaseResourceLite(&vcb->Resource);
    ExReleaseRundownProtection(&vcb->RundownProtection);
    ObDereferenceObject(VolumeDeviceObject);
    return status;
}


/*
 * VPB fields that describe the real-device association must be inspected
 * while holding the I/O manager's VPB spin lock. Keep this short: acquiring
 * the lock raises IRQL to DISPATCH_LEVEL, so no ERESOURCE may be held here.
 */
static NTSTATUS
OpenFsValidateMountVpb(
    _In_ PVPB Vpb,
    _In_ PDEVICE_OBJECT RealDeviceObject
    )
{
    KIRQL oldIrql;
    NTSTATUS status = STATUS_SUCCESS;

    IoAcquireVpbSpinLock(&oldIrql);

    if (Vpb->RealDevice != RealDeviceObject) {
        status = STATUS_INVALID_PARAMETER;
    } else if ((Vpb->Flags & VPB_REMOVE_PENDING) != 0) {
        status = STATUS_DELETE_PENDING;
    } else if ((Vpb->Flags & VPB_MOUNTED) != 0 ||
               Vpb->DeviceObject != NULL) {
        /*
         * Do not reserve a VPB already published for another mounted
         * filesystem. The I/O manager owns VPB_MOUNTED; this code only
         * observes it while holding the VPB spin lock.
         */
        status = STATUS_DEVICE_BUSY;
    }

    IoReleaseVpbSpinLock(oldIrql);
    return status;
}

NTSTATUS
OpenFsReserveVolumeVpb(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PVPB Vpb
    )
{
    POPENFS_VCB vcb;
    NTSTATUS status = STATUS_SUCCESS;

    if (VolumeDeviceObject == NULL || Vpb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * The caller must already own a valid device-object pointer. Take our
     * own reference before touching the extension so IoDeleteDevice cannot
     * reclaim it while this operation is in flight.
     */
    ObReferenceObject(VolumeDeviceObject);

    vcb = (POPENFS_VCB)VolumeDeviceObject->DeviceExtension;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (!ExAcquireRundownProtection(&vcb->RundownProtection)) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_DELETE_PENDING;
    }

    if (!ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)) {
        ExReleaseRundownProtection(&vcb->RundownProtection);
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (vcb->Signature != OPENFS_VCB_SIGNATURE ||
        vcb->State != OpenFsVolumeInitializing) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else if (vcb->Vpb != NULL) {
        status = STATUS_DEVICE_BUSY;
    } else {
        /*
         * Validate the I/O-manager-owned VPB while the VCB resource is held.
         * This keeps the VCB's RealDeviceObject stable during validation and
         * follows one lock order everywhere: VCB resource, then VPB spinlock.
         * Never acquire an ERESOURCE while holding the VPB spinlock.
         */
        status = OpenFsValidateMountVpb(Vpb, vcb->RealDeviceObject);
        if (NT_SUCCESS(status)) {
            vcb->Vpb = Vpb;
        }
    }

    ExReleaseResourceLite(&vcb->Resource);
    ExReleaseRundownProtection(&vcb->RundownProtection);
    ObDereferenceObject(VolumeDeviceObject);
    return status;
}

/*
 * Publish a fully initialized VDO through the I/O manager's VPB. The caller
 * must have completed all volume/root initialization before invoking this
 * final mount-commit operation. VPB_MOUNTED is intentionally left to the
 * I/O manager after IRP_MN_MOUNT_VOLUME completes successfully.
 */
NTSTATUS
OpenFsPublishVolumeVpb(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PVPB Vpb
    )
{
    POPENFS_VCB vcb;
    KIRQL oldIrql;
    NTSTATUS status = STATUS_SUCCESS;

    if (VolumeDeviceObject == NULL || Vpb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* The caller must hold a valid reference while entering this routine. */
    ObReferenceObject(VolumeDeviceObject);
    vcb = (POPENFS_VCB)VolumeDeviceObject->DeviceExtension;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (!ExAcquireRundownProtection(&vcb->RundownProtection)) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_DELETE_PENDING;
    }

    if (!ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)) {
        ExReleaseRundownProtection(&vcb->RundownProtection);
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (vcb->Signature != OPENFS_VCB_SIGNATURE ||
        vcb->State != OpenFsVolumeInitializing ||
        vcb->Vpb != Vpb) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else {
        /*
         * Validation and publication must be one VPB-spin-lock transaction:
         * otherwise another filesystem could claim this VPB between the
         * checks and the DeviceObject assignment.
         */
        IoAcquireVpbSpinLock(&oldIrql);

        if (Vpb->RealDevice != vcb->RealDeviceObject) {
            status = STATUS_INVALID_PARAMETER;
        } else if ((Vpb->Flags & VPB_REMOVE_PENDING) != 0) {
            status = STATUS_DELETE_PENDING;
        } else if ((Vpb->Flags & VPB_MOUNTED) != 0 ||
                   Vpb->DeviceObject != NULL) {
            status = STATUS_DEVICE_BUSY;
        } else {
            /*
             * All dispatch-visible state must already be initialized.
             * Clear this flag immediately before making the VDO reachable.
             */
            VolumeDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
            Vpb->DeviceObject = VolumeDeviceObject;
            vcb->State = OpenFsVolumeMounted;
        }

        IoReleaseVpbSpinLock(oldIrql);
    }

    ExReleaseResourceLite(&vcb->Resource);
    ExReleaseRundownProtection(&vcb->RundownProtection);
    ObDereferenceObject(VolumeDeviceObject);
    return status;
}


/*
 * Roll back only our own reservation and only before the volume is mounted.
 * A mounted VPB must be detached through the dedicated dismount path.
 */
NTSTATUS
OpenFsReleaseVolumeVpb(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PVPB Vpb
    )
{
    POPENFS_VCB vcb;
    NTSTATUS status = STATUS_SUCCESS;

    if (VolumeDeviceObject == NULL || Vpb == NULL)
        return STATUS_INVALID_PARAMETER;

    ObReferenceObject(VolumeDeviceObject);
    vcb = (POPENFS_VCB)VolumeDeviceObject->DeviceExtension;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!ExAcquireRundownProtection(&vcb->RundownProtection)) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_DELETE_PENDING;
    }
    if (!ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)) {
        ExReleaseRundownProtection(&vcb->RundownProtection);
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (vcb->Signature != OPENFS_VCB_SIGNATURE) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else if (vcb->State != OpenFsVolumeInitializing) {
        status = STATUS_DEVICE_BUSY;
    } else if (vcb->Vpb != Vpb) {
        status = STATUS_NOT_FOUND;
    } else {
        vcb->Vpb = NULL;
    }

    ExReleaseResourceLite(&vcb->Resource);
    ExReleaseRundownProtection(&vcb->RundownProtection);
    ObDereferenceObject(VolumeDeviceObject);
    return status;
}

NTSTATUS
OpenFsSetVolumeNamespace(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ POPENFS_NAMESPACE_SNAPSHOT NamespaceSnapshot
    )
{
    POPENFS_VCB vcb;
    NTSTATUS status = STATUS_SUCCESS;

    if (VolumeDeviceObject == NULL || NamespaceSnapshot == NULL)
        return STATUS_INVALID_PARAMETER;
    ObReferenceObject(VolumeDeviceObject);
    vcb = (POPENFS_VCB)VolumeDeviceObject->DeviceExtension;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!ExAcquireRundownProtection(&vcb->RundownProtection)) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_DELETE_PENDING;
    }
    if (!ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)) {
        ExReleaseRundownProtection(&vcb->RundownProtection);
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (vcb->Signature != OPENFS_VCB_SIGNATURE ||
        vcb->State != OpenFsVolumeInitializing) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else if (vcb->Namespace != NULL || vcb->RootDcb != NULL) {
        status = STATUS_DEVICE_BUSY;
    } else if (NamespaceSnapshot->Records == NULL ||
        NamespaceSnapshot->RootRecordIndex >= NamespaceSnapshot->RecordCount ||
        NamespaceSnapshot->Records[NamespaceSnapshot->RootRecordIndex].Kind != 2u ||
        NamespaceSnapshot->Records[NamespaceSnapshot->RootRecordIndex].ObjectIdLow != NamespaceSnapshot->RootIdLow ||
        NamespaceSnapshot->Records[NamespaceSnapshot->RootRecordIndex].ObjectIdHigh != NamespaceSnapshot->RootIdHigh) {
        status = STATUS_DISK_CORRUPT_ERROR;
    } else {
        POPENFS_DCB rootDcb = (POPENFS_DCB)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, sizeof(*rootDcb), OPENFS_FCB_POOL_TAG);
        if (rootDcb == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            const OPENFS_METADATA_RECORD* rootRecord = NULL;
            ULONG index;
            RtlZeroMemory(rootDcb, sizeof(*rootDcb));
            if (NamespaceSnapshot->RootRecordIndex <
                NamespaceSnapshot->RecordCount) {
                rootRecord = &NamespaceSnapshot->Records[
                    NamespaceSnapshot->RootRecordIndex];
            }
            rootDcb->Fcb.Signature = OPENFS_FCB_SIGNATURE;
            rootDcb->Fcb.NodeType = OPENFS_DCB_NODE_TYPE;
            rootDcb->Fcb.ObjectIdLow = NamespaceSnapshot->RootIdLow;
            rootDcb->Fcb.ObjectIdHigh = NamespaceSnapshot->RootIdHigh;
            rootDcb->Fcb.Kind = 2u;
            rootDcb->Fcb.IsRoot = TRUE;
            InitializeListHead(&rootDcb->Fcb.CacheLink);
            if (rootRecord != NULL) {
                rootDcb->Fcb.FileSize = rootRecord->Size;
                rootDcb->Fcb.ExtentRootBlock = rootRecord->ExtentRootBlock;
                rootDcb->Fcb.Generation = rootRecord->Generation;
            }
            for (index = 0; index < NamespaceSnapshot->EntryCount; ++index) {
                const OPENFS_DIRECTORY_ENTRY* entry =
                    &NamespaceSnapshot->Entries[index];
                if (entry->ParentIdLow == NamespaceSnapshot->RootIdLow &&
                    entry->ParentIdHigh == NamespaceSnapshot->RootIdHigh)
                    ++rootDcb->ChildCount;
            }
            vcb->RootDcb = rootDcb;
            InsertTailList(&vcb->FcbList, &rootDcb->Fcb.CacheLink);
            vcb->CachedFcbCount = 1;
            vcb->Namespace = NamespaceSnapshot;
        }
    }
    ExReleaseResourceLite(&vcb->Resource);
    ExReleaseRundownProtection(&vcb->RundownProtection);
    ObDereferenceObject(VolumeDeviceObject);
    return status;
}


/*
 * Resolve a single directory component and cache its FCB for the lifetime
 * of the VCB. Nodes are never evicted while mounted: callers may safely keep
 * an FCB pointer in FILE_OBJECT->FsContext until volume teardown.
 */
NTSTATUS
OpenFsLookupChildFcb(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ POPENFS_FCB ParentFcb,
    _In_reads_bytes_(NameLength) const UCHAR* Name,
    _In_ UCHAR NameLength,
    _Outptr_ POPENFS_FCB* ChildFcb
    )
{
    POPENFS_VCB vcb;
    PLIST_ENTRY link;
    ULONG recordIndex = MAXULONG;
    ULONG index;
    const OPENFS_METADATA_RECORD* record;
    POPENFS_FCB cached = NULL;
    POPENFS_FCB allocated = NULL;
    NTSTATUS status;

    if (VolumeDeviceObject == NULL || ParentFcb == NULL ||
        Name == NULL || NameLength == 0 || ChildFcb == NULL)
        return STATUS_INVALID_PARAMETER;
    *ChildFcb = NULL;

    ObReferenceObject(VolumeDeviceObject);
    vcb = (POPENFS_VCB)VolumeDeviceObject->DeviceExtension;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!ExAcquireRundownProtection(&vcb->RundownProtection)) {
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_DELETE_PENDING;
    }
    if (!ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)) {
        ExReleaseRundownProtection(&vcb->RundownProtection);
        ObDereferenceObject(VolumeDeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = STATUS_INVALID_DEVICE_STATE;
    if (vcb->Signature != OPENFS_VCB_SIGNATURE ||
        (vcb->State != OpenFsVolumeInitializing &&
         vcb->State != OpenFsVolumeMounted) ||
        vcb->Namespace == NULL || IsListEmpty(&vcb->FcbList))
        goto Exit;

    /* Validate pointer ownership before dereferencing it as a parent node. */
    for (link = vcb->FcbList.Flink; link != &vcb->FcbList;
         link = link->Flink) {
        POPENFS_FCB candidate =
            CONTAINING_RECORD(link, OPENFS_FCB, CacheLink);
        if (candidate == ParentFcb) {
            cached = candidate;
            break;
        }
    }
    if (cached == NULL || cached->Signature != OPENFS_FCB_SIGNATURE ||
        cached->Kind != 2u || cached->NodeType != OPENFS_DCB_NODE_TYPE) {
        status = STATUS_NOT_A_DIRECTORY;
        goto Exit;
    }

    status = OpenFsLookupNamespaceChild(vcb->Namespace,
        cached->ObjectIdLow, cached->ObjectIdHigh, Name, NameLength,
        &recordIndex);
    if (!NT_SUCCESS(status)) goto Exit;
    if (recordIndex >= vcb->Namespace->RecordCount) {
        status = STATUS_DISK_CORRUPT_ERROR;
        goto Exit;
    }

    record = &vcb->Namespace->Records[recordIndex];
    for (link = vcb->FcbList.Flink; link != &vcb->FcbList;
         link = link->Flink) {
        POPENFS_FCB candidate =
            CONTAINING_RECORD(link, OPENFS_FCB, CacheLink);
        if (candidate->ObjectIdLow == record->ObjectIdLow &&
            candidate->ObjectIdHigh == record->ObjectIdHigh) {
            *ChildFcb = candidate;
            status = STATUS_SUCCESS;
            goto Exit;
        }
    }

    if (record->Kind == 2u) {
        POPENFS_DCB dcb = (POPENFS_DCB)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, sizeof(*dcb), OPENFS_FCB_POOL_TAG);
        if (dcb == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }
        RtlZeroMemory(dcb, sizeof(*dcb));
        allocated = &dcb->Fcb;
        for (index = 0; index < vcb->Namespace->EntryCount; ++index) {
            const OPENFS_DIRECTORY_ENTRY* entry =
                &vcb->Namespace->Entries[index];
            if (entry->ParentIdLow == record->ObjectIdLow &&
                entry->ParentIdHigh == record->ObjectIdHigh)
                ++dcb->ChildCount;
        }
    } else if (record->Kind == 1u) {
        allocated = (POPENFS_FCB)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, sizeof(*allocated), OPENFS_FCB_POOL_TAG);
        if (allocated == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }
        RtlZeroMemory(allocated, sizeof(*allocated));
    } else {
        status = STATUS_DISK_CORRUPT_ERROR;
        goto Exit;
    }

    allocated->Signature = OPENFS_FCB_SIGNATURE;
    allocated->NodeType = record->Kind == 2u
        ? OPENFS_DCB_NODE_TYPE : OPENFS_FCB_NODE_TYPE;
    allocated->ObjectIdLow = record->ObjectIdLow;
    allocated->ObjectIdHigh = record->ObjectIdHigh;
    allocated->ParentIdLow = record->ParentIdLow;
    allocated->ParentIdHigh = record->ParentIdHigh;
    allocated->FileSize = record->Size;
    allocated->ExtentRootBlock = record->ExtentRootBlock;
    allocated->Generation = record->Generation;
    allocated->Kind = record->Kind;
    allocated->IsRoot = FALSE;
    InitializeListHead(&allocated->CacheLink);
    InsertTailList(&vcb->FcbList, &allocated->CacheLink);
    ++vcb->CachedFcbCount;
    *ChildFcb = allocated;
    allocated = NULL;
    status = STATUS_SUCCESS;

Exit:
    if (allocated != NULL)
        ExFreePoolWithTag(allocated, OPENFS_FCB_POOL_TAG);
    ExReleaseResourceLite(&vcb->Resource);
    ExReleaseRundownProtection(&vcb->RundownProtection);
    ObDereferenceObject(VolumeDeviceObject);
    return status;
}
