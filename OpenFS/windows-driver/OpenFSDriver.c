#include <ntifs.h>

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

static NTSTATUS
OpenFsDispatchCreateClose(
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

    UNREFERENCED_PARAMETER(DeviceObject);
    stack = IoGetCurrentIrpStackLocation(Irp);

    switch (stack->MinorFunction) {
    case IRP_MN_LOAD_FILE_SYSTEM:
        return OpenFsCompleteIrp(Irp, STATUS_SUCCESS);

    case IRP_MN_MOUNT_VOLUME:
        /*
         * Mounting is intentionally not implemented in this bootstrap.
         * Returning UNRECOGNIZED_VOLUME lets Windows continue probing other
         * registered filesystems without claiming or modifying the volume.
         */
        return OpenFsCompleteIrp(Irp, STATUS_UNRECOGNIZED_VOLUME);

    case IRP_MN_USER_FS_REQUEST:
    case IRP_MN_VERIFY_VOLUME:
    case IRP_MN_KERNEL_CALL:
    default:
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
    }
}

static NTSTATUS
OpenFsDispatchUnsupported(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
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

    DriverObject->MajorFunction[IRP_MJ_CREATE] = OpenFsDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = OpenFsDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] =
        OpenFsDispatchFileSystemControl;

    DriverObject->DriverUnload = OpenFsUnload;

    IoRegisterFileSystem(g_OpenFsFileSystemDevice);
    g_OpenFsFileSystemDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}
