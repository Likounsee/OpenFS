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

    if (VolumeDeviceObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    vcb = (POPENFS_VCB)VolumeDeviceObject->DeviceExtension;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    /*
     * Serialize teardown against mount-state updates. Only Initializing is
     * eligible: accepting Dead here would let two concurrent destroy callers
     * both attempt to delete the same ERESOURCE and device object.
     */
    if (!ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (!OpenFsVolumeStateCanDestroy(vcb->State, vcb->Vpb != NULL)) {
        ExReleaseResourceLite(&vcb->Resource);
        return STATUS_DEVICE_BUSY;
    }

    if (!OpenFsVolumeStateTryTransition(
            &vcb->State,
            OpenFsVolumeInitializing,
            OpenFsVolumeDead)) {
        ExReleaseResourceLite(&vcb->Resource);
        return STATUS_INVALID_DEVICE_STATE;
    }

    ExReleaseResourceLite(&vcb->Resource);

    /* Stop new users and wait for all VCB accessors already in flight. */
    ExRundownCompleted(&vcb->RundownProtection);
    ExWaitForRundownProtectionRelease(&vcb->RundownProtection);

    status = ExDeleteResourceLite(&vcb->Resource);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    vcb->Signature = 0;
    if (vcb->RealDeviceObject != NULL) {
        ObDereferenceObject(vcb->RealDeviceObject);
        vcb->RealDeviceObject = NULL;
    }

    IoDeleteDevice(VolumeDeviceObject);
    return STATUS_SUCCESS;
}

/*
 * Serialize lifecycle transitions through the VCB resource. The expected
 * state is checked while exclusively locked, so competing mount/dismount
 * workers cannot both claim the same transition.
 */
NTSTATUS
OpenFsTransitionVolumeState(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ OPENFS_VOLUME_STATE Expected,
    _In_ OPENFS_VOLUME_STATE Next
    )
{
    POPENFS_VCB vcb;
    NTSTATUS status = STATUS_SUCCESS;

    if (VolumeDeviceObject == NULL) {
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
    } else if (!OpenFsVolumeStateTryTransition(&vcb->State, Expected, Next)) {
        status = STATUS_INVALID_DEVICE_STATE;
    }

    ExReleaseResourceLite(&vcb->Resource);
    ExReleaseRundownProtection(&vcb->RundownProtection);
    return status;
}

/* Return a consistent state snapshot without exposing the VCB layout. */
NTSTATUS
OpenFsQueryVolumeState(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _Out_ OPENFS_VOLUME_STATE* State
    )
{
    POPENFS_VCB vcb;
    NTSTATUS status = STATUS_SUCCESS;

    if (VolumeDeviceObject == NULL || State == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    vcb = (POPENFS_VCB)VolumeDeviceObject->DeviceExtension;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (!ExAcquireRundownProtection(&vcb->RundownProtection)) {
        return STATUS_DELETE_PENDING;
    }

    if (!ExAcquireResourceSharedLite(&vcb->Resource, TRUE)) {
        ExReleaseRundownProtection(&vcb->RundownProtection);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (vcb->Signature != OPENFS_VCB_SIGNATURE) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else {
        *State = vcb->State;
    }

    ExReleaseResourceLite(&vcb->Resource);
    ExReleaseRundownProtection(&vcb->RundownProtection);
    return status;
}


/*
 * Hold the VCB's rundown reference and resource while reserving the VPB.
 * The I/O manager owns the VPB; OpenFS only records the reservation here.
 */
/*
 * VPB fields that describe the real-device association must be inspected
 * while holding the I/O manager's VPB spin lock. Keep this short: acquiring
 * the lock raises IRQL to DISPATCH_LEVEL, so no ERESOURCE may be held here.
 */
static BOOLEAN
OpenFsVpbBelongsToRealDevice(
    _In_ PVPB Vpb,
    _In_ PDEVICE_OBJECT RealDeviceObject
    )
{
    KIRQL oldIrql;
    BOOLEAN matches;

    IoAcquireVpbSpinLock(&oldIrql);
    matches = (Vpb->RealDevice == RealDeviceObject);
    IoReleaseVpbSpinLock(oldIrql);

    return matches;
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

    vcb = (POPENFS_VCB)VolumeDeviceObject->DeviceExtension;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    /*
     * RealDevice is an I/O-manager-owned VPB field. Validate it under the
     * VPB spin lock before taking the VCB resource (the spin lock raises
     * IRQL to DISPATCH_LEVEL and must never be nested with an ERESOURCE).
     */
    if (!OpenFsVpbBelongsToRealDevice(Vpb, vcb->RealDeviceObject)) {
        return STATUS_INVALID_PARAMETER;
    }

    if (!ExAcquireRundownProtection(&vcb->RundownProtection)) {
        return STATUS_DELETE_PENDING;
    }

    if (!ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)) {
        ExReleaseRundownProtection(&vcb->RundownProtection);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (vcb->Signature != OPENFS_VCB_SIGNATURE ||
        vcb->State != OpenFsVolumeInitializing) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else if (vcb->Vpb != NULL) {
        status = STATUS_DEVICE_BUSY;
    } else {
        vcb->Vpb = Vpb;
    }

    ExReleaseResourceLite(&vcb->Resource);
    ExReleaseRundownProtection(&vcb->RundownProtection);
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
