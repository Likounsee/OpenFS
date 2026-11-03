#include "OpenFSVolume.h"

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

    if (DriverObject == NULL ||
        RealDeviceObject == NULL ||
        Geometry == NULL ||
        VolumeDeviceObject == NULL ||
        Geometry->BlockSize == 0 ||
        Geometry->TotalBlocks == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    *VolumeDeviceObject = NULL;

    /*
     * StackSize is a signed CCHAR. Refuse an impossible stack depth instead
     * of allowing the addition to wrap and corrupt future IRP forwarding.
     */
    if (RealDeviceObject->StackSize >= MAXIMUM_CCHAR) {
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
     * This helper is for unpublished objects only. Once VPB ownership or
     * mounted-volume references exist, teardown must go through dismount.
     */
    if (vcb->Vpb != NULL ||
        (vcb->State != OpenFsVolumeInitializing &&
         vcb->State != OpenFsVolumeDead)) {
        return STATUS_DEVICE_BUSY;
    }

    if (vcb->State == OpenFsVolumeInitializing) {
        if (!OpenFsVolumeStateTryTransition(
                &vcb->State,
                OpenFsVolumeInitializing,
                OpenFsVolumeDead)) {
            return STATUS_INVALID_DEVICE_STATE;
        }
    }

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
