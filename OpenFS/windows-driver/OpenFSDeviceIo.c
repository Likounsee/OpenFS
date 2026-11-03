#include <ntifs.h>
#include <ntdddisk.h>
#include "OpenFSDeviceIo.h"

#define OPENFS_DEVICE_IO_TAG 'iFsO'
#define OPENFS_PROBE_BUFFER_TAG 'pFsO'

static ULONG
OpenFsDeviceReadU32Le(_In_reads_bytes_(4) const UCHAR* Bytes)
{
    return ((ULONG)Bytes[0]) |
           ((ULONG)Bytes[1] << 8) |
           ((ULONG)Bytes[2] << 16) |
           ((ULONG)Bytes[3] << 24);
}

static ULONGLONG
OpenFsDeviceReadU64Le(_In_reads_bytes_(8) const UCHAR* Bytes)
{
    return ((ULONGLONG)OpenFsDeviceReadU32Le(Bytes)) |
           ((ULONGLONG)OpenFsDeviceReadU32Le(Bytes + 4) << 32);
}

static NTSTATUS
OpenFsReadAtOffset(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONGLONG ByteOffset,
    _Out_writes_bytes_(Length) UCHAR* Destination,
    _In_ ULONG Length
    )
{
    PVOID allocation;
    PUCHAR alignedBuffer;
    SIZE_T allocationLength;
    ULONG_PTR alignmentMask;
    LARGE_INTEGER offset;
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    NTSTATUS status;

    if (Length == 0 || Destination == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (DeviceObject->SectorSize != 0 &&
        ((ByteOffset % DeviceObject->SectorSize) != 0 ||
         (Length % DeviceObject->SectorSize) != 0)) {
        return STATUS_INVALID_PARAMETER;
    }

    alignmentMask = DeviceObject->AlignmentRequirement;
    if (alignmentMask > MAXULONG_PTR - Length) {
        return STATUS_INTEGER_OVERFLOW;
    }
    allocationLength = (SIZE_T)Length + (SIZE_T)alignmentMask;
    allocation = ExAllocatePool2(POOL_FLAG_NON_PAGED, allocationLength,
        OPENFS_DEVICE_IO_TAG);
    if (allocation == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    alignedBuffer = (PUCHAR)(((ULONG_PTR)allocation + alignmentMask) &
        ~alignmentMask);
    offset.QuadPart = (LONGLONG)ByteOffset;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&ioStatus, sizeof(ioStatus));

    irp = IoBuildSynchronousFsdRequest(
        IRP_MJ_READ,
        DeviceObject,
        alignedBuffer,
        Length,
        &offset,
        &event,
        &ioStatus
        );
    if (irp == NULL) {
        ExFreePoolWithTag(allocation, OPENFS_DEVICE_IO_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(DeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }
    status = ioStatus.Status;
    if (NT_SUCCESS(status)) {
        if (ioStatus.Information != Length) {
            status = STATUS_DEVICE_DATA_ERROR;
        } else {
            RtlCopyMemory(Destination, alignedBuffer, Length);
        }
    }

    ExFreePoolWithTag(allocation, OPENFS_DEVICE_IO_TAG);
    return status;
}

static NTSTATUS
OpenFsQueryDeviceLength(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Out_ ULONGLONG* LengthBytes
    )
{
    GET_LENGTH_INFORMATION lengthInformation;
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    NTSTATUS status;

    RtlZeroMemory(&lengthInformation, sizeof(lengthInformation));
    RtlZeroMemory(&ioStatus, sizeof(ioStatus));
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
        IOCTL_DISK_GET_LENGTH_INFO,
        DeviceObject,
        NULL,
        0,
        &lengthInformation,
        sizeof(lengthInformation),
        FALSE,
        &event,
        &ioStatus
        );
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(DeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }
    status = ioStatus.Status;
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (ioStatus.Information < sizeof(lengthInformation) ||
        lengthInformation.Length.QuadPart <= 0) {
        return STATUS_DEVICE_DATA_ERROR;
    }

    *LengthBytes = (ULONGLONG)lengthInformation.Length.QuadPart;
    return STATUS_SUCCESS;
}

NTSTATUS
OpenFsProbeVolume(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _Out_ POPENFS_VOLUME_GEOMETRY Geometry
    )
{
    PUCHAR records;
    PUCHAR primary;
    PUCHAR backup;
    ULONGLONG deviceLength;
    ULONGLONG totalBlocks;
    ULONGLONG backupOffset;
    ULONG blockSize;
    USHORT version;
    USHORT headerSize;
    NTSTATUS status;

    if (VolumeDeviceObject == NULL || Geometry == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Geometry, sizeof(*Geometry));

    status = OpenFsQueryDeviceLength(VolumeDeviceObject, &deviceLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    records = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED,
        OPENFS_SUPERBLOCK_BYTES * 2u, OPENFS_PROBE_BUFFER_TAG);
    if (records == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    primary = records;
    backup = records + OPENFS_SUPERBLOCK_BYTES;

    status = OpenFsReadAtOffset(VolumeDeviceObject, 0, primary,
        OPENFS_SUPERBLOCK_BYTES);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    if (RtlCompareMemory(primary, "OPENFS\\0\\0", 8) != 8) {
        status = STATUS_UNRECOGNIZED_VOLUME;
        goto Exit;
    }

    version = (USHORT)(primary[8] | ((USHORT)primary[9] << 8));
    headerSize = (USHORT)(primary[10] | ((USHORT)primary[11] << 8));
    blockSize = OpenFsDeviceReadU32Le(primary + 12);
    totalBlocks = OpenFsDeviceReadU64Le(primary + 16);

    if ((version != 2 && version != 3 && version != 4) ||
        headerSize != 48 ||
        blockSize < OPENFS_SUPERBLOCK_BYTES ||
        blockSize > 65536 ||
        (blockSize & (blockSize - 1)) != 0 ||
        totalBlocks < 12 ||
        totalBlocks > MAXULONGLONG / blockSize) {
        status = STATUS_DISK_CORRUPT_ERROR;
        goto Exit;
    }

    if (totalBlocks * blockSize != deviceLength) {
        status = STATUS_DISK_CORRUPT_ERROR;
        goto Exit;
    }

    backupOffset = (totalBlocks - 1) * blockSize;
    if (backupOffset > deviceLength ||
        deviceLength - backupOffset < OPENFS_SUPERBLOCK_BYTES) {
        status = STATUS_DISK_CORRUPT_ERROR;
        goto Exit;
    }

    status = OpenFsReadAtOffset(VolumeDeviceObject, backupOffset, backup,
        OPENFS_SUPERBLOCK_BYTES);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    status = OpenFsValidateSuperblocks(primary, backup, deviceLength, Geometry);

Exit:
    ExFreePoolWithTag(records, OPENFS_PROBE_BUFFER_TAG);
    return status;
}
