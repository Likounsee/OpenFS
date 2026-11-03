#include <ntifs.h>
#include <ntdddisk.h>
#include "OpenFSDeviceIo.h"
#include "OpenFSBitmap.h"
#include "OpenFSLayout.h"

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
    _In_ ULONGLONG DeviceLength,
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

    if (Destination == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = OpenFsValidateReadRange(DeviceLength, ByteOffset, Length);
    if (!NT_SUCCESS(status)) {
        return status;
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
        status = ioStatus.Status;
    } else if (NT_SUCCESS(status)) {
        status = ioStatus.Status;
    } else if (NT_SUCCESS(ioStatus.Status)) {
        /*
         * Some lower drivers fail the dispatch without updating the
         * supplied IO_STATUS_BLOCK. Preserve the returned failure rather
         * than accidentally treating the zero-initialized status as success.
         */
        ioStatus.Status = status;
    } else {
        status = ioStatus.Status;
    }
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
        status = ioStatus.Status;
    } else if (NT_SUCCESS(status)) {
        status = ioStatus.Status;
    } else if (NT_SUCCESS(ioStatus.Status)) {
        ioStatus.Status = status;
    } else {
        status = ioStatus.Status;
    }
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


/*
 * Validate reserved metadata and padding bits on every bitmap page before
 * accepting the volume. The portable validator is shared with host tests.
 */
static NTSTATUS
OpenFsValidateAllocationBitmap(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ const OPENFS_VOLUME_GEOMETRY* Geometry,
    _In_ ULONGLONG DeviceLength
    )
{
    OPENFS_VOLUME_LAYOUT layout;
    ULONGLONG bitsPerBitmapBlock;
    ULONGLONG bitmapBlocks;
    ULONGLONG bitmapIndex;
    SIZE_T bitmapBytes;
    UCHAR* bitmap;
    NTSTATUS status = STATUS_SUCCESS;

    status = OpenFsCalculateVolumeLayout(
        Geometry->BlockSize,
        Geometry->TotalBlocks,
        Geometry->Version,
        &layout
        );
    if (!NT_SUCCESS(status)) {
        return status;
    }

    bitsPerBitmapBlock = (ULONGLONG)Geometry->BlockSize * 8u;
    bitmapBlocks = layout.BitmapBlocks;

    bitmapBytes = (SIZE_T)Geometry->BlockSize;
    bitmap = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        bitmapBytes,
        OPENFS_DEVICE_IO_TAG
        );
    if (bitmap == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (bitmapIndex = 0; bitmapIndex < bitmapBlocks; ++bitmapIndex) {
        ULONGLONG bitmapBase;
        ULONGLONG bitmapDeviceBlock;
        ULONGLONG bitmapOffset;

        if (bitmapIndex > MAXULONGLONG / bitsPerBitmapBlock) {
            status = STATUS_INTEGER_OVERFLOW;
            goto Exit;
        }
        bitmapBase = bitmapIndex * bitsPerBitmapBlock;

        if (bitmapIndex > Geometry->TotalBlocks - 1u) {
            status = STATUS_DISK_CORRUPT_ERROR;
            goto Exit;
        }
        bitmapDeviceBlock = 1u + bitmapIndex;
        if (bitmapDeviceBlock > MAXULONGLONG / Geometry->BlockSize) {
            status = STATUS_INTEGER_OVERFLOW;
            goto Exit;
        }
        bitmapOffset = bitmapDeviceBlock * Geometry->BlockSize;

        status = OpenFsReadAtOffset(
            DeviceObject,
            bitmapOffset,
            DeviceLength,
            bitmap,
            (ULONG)bitmapBytes
            );
        if (!NT_SUCCESS(status)) {
            goto Exit;
        }

        status = OpenFsValidateBitmapBlock(
            bitmap,
            bitmapBytes,
            bitmapBase,
            Geometry->TotalBlocks,
            layout.BitmapEndBlock,
            layout.JournalStartBlock,
            layout.RingStartBlock,
            layout.RingEndBlock
            );
        if (!NT_SUCCESS(status)) {
            goto Exit;
        }
    }

Exit:
    ExFreePoolWithTag(bitmap, OPENFS_DEVICE_IO_TAG);
    return status;
}

NTSTATUS
OpenFsProbeVolume(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _Out_ POPENFS_VOLUME_GEOMETRY Geometry
    )
{
    PDEVICE_OBJECT targetDeviceObject;
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

    targetDeviceObject = IoGetAttachedDeviceReference(VolumeDeviceObject);
    if (targetDeviceObject == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    status = OpenFsQueryDeviceLength(targetDeviceObject, &deviceLength);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(targetDeviceObject);
        return status;
    }

    records = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED,
        OPENFS_SUPERBLOCK_BYTES * 2u, OPENFS_PROBE_BUFFER_TAG);
    if (records == NULL) {
        ObDereferenceObject(targetDeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    primary = records;
    backup = records + OPENFS_SUPERBLOCK_BYTES;

    status = OpenFsReadAtOffset(targetDeviceObject, 0, deviceLength, primary,
        OPENFS_SUPERBLOCK_BYTES);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    if (RtlCompareMemory(primary, "OPENFS\0\0", 8) != 8) {
        status = STATUS_UNRECOGNIZED_VOLUME;
        goto Exit;
    }

    version = (USHORT)(primary[8] | ((USHORT)primary[9] << 8));
    headerSize = (USHORT)(primary[10] | ((USHORT)primary[11] << 8));
    blockSize = OpenFsDeviceReadU32Le(primary + 12);
    totalBlocks = OpenFsDeviceReadU64Le(primary + 16);

    if (version != 2 && version != 3 && version != 4) {
        status = STATUS_UNRECOGNIZED_VOLUME;
        goto Exit;
    }

    if (headerSize != 48 ||
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

    status = OpenFsReadAtOffset(targetDeviceObject, backupOffset, deviceLength, backup,
        OPENFS_SUPERBLOCK_BYTES);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    status = OpenFsValidateSuperblocks(primary, backup, deviceLength, Geometry);
    if (NT_SUCCESS(status)) {
        status = OpenFsValidateAllocationBitmap(targetDeviceObject, Geometry, deviceLength);
    }

Exit:
    ExFreePoolWithTag(records, OPENFS_PROBE_BUFFER_TAG);
    ObDereferenceObject(targetDeviceObject);
    return status;
}
