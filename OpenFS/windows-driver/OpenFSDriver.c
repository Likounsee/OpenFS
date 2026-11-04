#include <ntifs.h>
#include "OpenFSSuperblock.h"
#include "OpenFSDeviceIo.h"
#include "OpenFSVolume.h"

static PDEVICE_OBJECT g_OpenFsFileSystemDevice = NULL;

static NTSTATUS
OpenFsCompleteIrp(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status
    )
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/*
 * Until a mounted volume device object and a file-control path exist, every
 * operation other than the explicitly supported filesystem-control requests
 * must fail deterministically. In particular, never let an unset major
 * function pointer become an accidental dispatch path.
 */
static NTSTATUS
OpenFsDispatchUnsupported(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
}

/*
 * The filesystem control device is not a mounted volume. Accepting CREATE
 * here would falsely advertise file access that the driver cannot service.
 */
static NTSTATUS
OpenFsDispatchCreate(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
}

static NTSTATUS
OpenFsDispatchClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return OpenFsCompleteIrp(Irp, STATUS_SUCCESS);
}

static NTSTATUS
OpenFsDispatchFileSystemControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;

    if (DeviceObject != g_OpenFsFileSystemDevice) {
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
    }

    stack = IoGetCurrentIrpStackLocation(Irp);

    switch (stack->MinorFunction) {
    case IRP_MN_LOAD_FILE_SYSTEM:
        return OpenFsCompleteIrp(Irp, STATUS_SUCCESS);

    case IRP_MN_MOUNT_VOLUME:
    {
        OPENFS_VOLUME_GEOMETRY geometry;
        POPENFS_NAMESPACE_SNAPSHOT namespaceSnapshot = NULL;
        PDEVICE_OBJECT privateVolumeDevice = NULL;
        PDEVICE_OBJECT volumeDeviceObject;
        NTSTATUS status;

        volumeDeviceObject = stack->Parameters.MountVolume.DeviceObject;
        if (volumeDeviceObject == NULL) {
            return OpenFsCompleteIrp(Irp, STATUS_INVALID_PARAMETER);
        }

        status = OpenFsProbeVolume(volumeDeviceObject, &geometry,
            &namespaceSnapshot);
        if (NT_SUCCESS(status)) {
            /* Keep the validated namespace in a private VCB. It is not published:
             * root FCB/DCB and file dispatch are still absent. Roll back cleanly. */
            status = OpenFsCreateVolumeDevice(DeviceObject->DriverObject,
                volumeDeviceObject, &geometry, &privateVolumeDevice);
            if (NT_SUCCESS(status)) {
                if (namespaceSnapshot != NULL) {
                    status = OpenFsSetVolumeNamespace(privateVolumeDevice,
                        namespaceSnapshot);
                    if (NT_SUCCESS(status)) namespaceSnapshot = NULL;
                }
                {
                    NTSTATUS destroyStatus =
                        OpenFsDestroyVolumeDevice(privateVolumeDevice);
                    if (NT_SUCCESS(status) && !NT_SUCCESS(destroyStatus))
                        status = destroyStatus;
                }
            }
            if (namespaceSnapshot != NULL) {
                OpenFsFreeNamespaceSnapshot(namespaceSnapshot);
                namespaceSnapshot = NULL;
            }
            if (!NT_SUCCESS(status))
                return OpenFsCompleteIrp(Irp, status);
            DbgPrintEx(
                DPFLTR_IHVDRIVER_ID,
                DPFLTR_INFO_LEVEL,
                "OpenFS 1.0: valid volume detected (format revision %u, block size %lu, blocks %I64u); VCB/VPB mount is not implemented yet\n",
                (ULONG)geometry.Version,
                geometry.BlockSize,
                geometry.TotalBlocks
                );
            return OpenFsCompleteIrp(Irp, STATUS_NOT_IMPLEMENTED);
        }

        return OpenFsCompleteIrp(Irp, status);
    }

    case IRP_MN_USER_FS_REQUEST:
    case IRP_MN_VERIFY_VOLUME:
    case IRP_MN_KERNEL_CALL:
    default:
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
    }
}

static VOID
OpenFsUnload(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);

    if (g_OpenFsFileSystemDevice != NULL) {
        IoUnregisterFileSystem(g_OpenFsFileSystemDevice);
        IoDeleteDevice(g_OpenFsFileSystemDevice);
        g_OpenFsFileSystemDevice = NULL;
    }
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    UNICODE_STRING deviceName;
    ULONG majorFunction;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlInitUnicodeString(&deviceName, L"\\FileSystem\\OpenFS");

    status = IoCreateDevice(
        DriverObject,
        0,
        &deviceName,
        FILE_DEVICE_DISK_FILE_SYSTEM,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &g_OpenFsFileSystemDevice
        );

    if (!NT_SUCCESS(status)) {
        g_OpenFsFileSystemDevice = NULL;
        return status;
    }

    for (majorFunction = 0;
         majorFunction <= IRP_MJ_MAXIMUM_FUNCTION;
         ++majorFunction) {
        DriverObject->MajorFunction[majorFunction] =
            OpenFsDispatchUnsupported;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = OpenFsDispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = OpenFsDispatchClose;
    DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] =
        OpenFsDispatchFileSystemControl;

    DriverObject->DriverUnload = OpenFsUnload;

    IoRegisterFileSystem(g_OpenFsFileSystemDevice);
    g_OpenFsFileSystemDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}
