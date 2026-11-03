#include "OpenFSVolume.h"
#include "OpenFSLayout.h"

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
OpenFsReleaseVolumeVpb(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PVPB Vpb
    )
{
    POPENFS_VCB vcb;
    NTSTATUS status = STATUS_SUCCESS;

    if (VolumeDeviceObject == NULL || Vpb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    vcb = (POPENFS_VCB)VolumeDeviceObject->DeviceExtension;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (!ExAcquireRundownProtection(&vcb->RundownProtection)) {
        return STATUS_DELETE_PENDING;
    }

    if (!ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)) {
        ExReleaseRundownProtection(&vcb->RundownProtection);
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
    return status;
}
