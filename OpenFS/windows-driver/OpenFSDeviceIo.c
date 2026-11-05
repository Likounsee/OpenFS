#include <ntifs.h>
#include <ntdddisk.h>
#include "OpenFSDeviceIo.h"
#include "OpenFSBitmap.h"
#include "OpenFSLayout.h"
#include "OpenFSInventory.h"
#include "OpenFSJournal.h"
#include "OpenFSNamespace.h"

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


/*
 * Read a bounded range of complete filesystem blocks from the backing device.
 * This is the common data path for later extent, directory and file reads.
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
    )
{
    ULONGLONG deviceLength;
    ULONGLONG byteOffset;
    ULONGLONG byteLength;
    NTSTATUS status;

    if (DeviceObject == NULL || Buffer == NULL ||
        BlockSize == 0 || TotalBlocks == 0 || BlockCount == 0)
        return STATUS_INVALID_PARAMETER;

    if (TotalBlocks > MAXULONGLONG / BlockSize ||
        FirstBlock >= TotalBlocks ||
        (ULONGLONG)BlockCount > TotalBlocks - FirstBlock)
        return STATUS_DISK_CORRUPT_ERROR;

    if ((ULONGLONG)BlockCount > MAXULONG / BlockSize)
        return STATUS_INTEGER_OVERFLOW;

    byteLength = (ULONGLONG)BlockCount * BlockSize;
    if (BufferLength != (SIZE_T)byteLength)
        return STATUS_INVALID_BUFFER_SIZE;

    deviceLength = TotalBlocks * BlockSize;
    byteOffset = FirstBlock * BlockSize;
    status = OpenFsValidateReadRange(deviceLength, byteOffset, byteLength);
    if (!NT_SUCCESS(status))
        return status;

    return OpenFsReadAtOffset(DeviceObject, byteOffset, deviceLength,
        Buffer, (ULONG)byteLength);
}


/*
 * Write a bounded range of complete filesystem blocks to the backing device.
 *
 * This primitive is intentionally not wired to IRP_MJ_WRITE yet. The future
 * filesystem mutation layer must stage data, commit journal metadata, recover
 * indeterminate commits, and serialize writers before calling it.
 */
NTSTATUS
OpenFsWriteVolumeBlocks(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG BlockSize,
    _In_ ULONGLONG TotalBlocks,
    _In_ ULONGLONG FirstBlock,
    _In_ ULONG BlockCount,
    _In_reads_bytes_(BufferLength) const UCHAR* Buffer,
    _In_ SIZE_T BufferLength
    )
{
    PVOID allocation;
    PUCHAR alignedBuffer;
    SIZE_T allocationLength;
    ULONG_PTR alignmentMask;
    ULONGLONG deviceLength;
    ULONGLONG byteOffset;
    ULONGLONG byteLength;
    LARGE_INTEGER offset;
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    NTSTATUS status;

    if (DeviceObject == NULL || Buffer == NULL ||
        BlockSize == 0 || TotalBlocks == 0 || BlockCount == 0 ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;

    if (TotalBlocks > MAXULONGLONG / BlockSize ||
        FirstBlock >= TotalBlocks ||
        (ULONGLONG)BlockCount > TotalBlocks - FirstBlock)
        return STATUS_DISK_CORRUPT_ERROR;
    if ((ULONGLONG)BlockCount > MAXULONG / BlockSize)
        return STATUS_INTEGER_OVERFLOW;

    byteLength = (ULONGLONG)BlockCount * BlockSize;
    if (BufferLength != (SIZE_T)byteLength)
        return STATUS_INVALID_BUFFER_SIZE;

    deviceLength = TotalBlocks * BlockSize;
    byteOffset = FirstBlock * BlockSize;
    status = OpenFsValidateReadRange(deviceLength, byteOffset, byteLength);
    if (!NT_SUCCESS(status))
        return status;

    if (DeviceObject->SectorSize != 0 &&
        ((byteOffset % DeviceObject->SectorSize) != 0 ||
         (byteLength % DeviceObject->SectorSize) != 0))
        return STATUS_INVALID_PARAMETER;

    alignmentMask = DeviceObject->AlignmentRequirement;
    if (alignmentMask > MAXULONG_PTR - (SIZE_T)byteLength)
        return STATUS_INTEGER_OVERFLOW;
    allocationLength = (SIZE_T)byteLength + (SIZE_T)alignmentMask;
    allocation = ExAllocatePool2(POOL_FLAG_NON_PAGED, allocationLength,
        OPENFS_DEVICE_IO_TAG);
    if (allocation == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    alignedBuffer = (PUCHAR)(((ULONG_PTR)allocation + alignmentMask) &
        ~alignmentMask);
    RtlCopyMemory(alignedBuffer, Buffer, (SIZE_T)byteLength);
    offset.QuadPart = (LONGLONG)byteOffset;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&ioStatus, sizeof(ioStatus));

    irp = IoBuildSynchronousFsdRequest(
        IRP_MJ_WRITE, DeviceObject, alignedBuffer, (ULONG)byteLength,
        &offset, &event, &ioStatus);
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
        /* Keep the dispatch failure when the lower driver left IOSB untouched. */
        ioStatus.Status = status;
    }
    if (NT_SUCCESS(status) && ioStatus.Information != byteLength)
        status = STATUS_DEVICE_DATA_ERROR;

    ExFreePoolWithTag(allocation, OPENFS_DEVICE_IO_TAG);
    return status;
}

/*
 * Force lower-device cached writes to stable storage before a journal
 * publication step is considered durable.
 */
NTSTATUS
OpenFsFlushVolumeDevice(
    _In_ PDEVICE_OBJECT DeviceObject
    )
{
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    NTSTATUS status;

    if (DeviceObject == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&ioStatus, sizeof(ioStatus));
    irp = IoBuildSynchronousFsdRequest(
        IRP_MJ_FLUSH_BUFFERS, DeviceObject, NULL, 0, NULL, &event, &ioStatus);
    if (irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    status = IoCallDriver(DeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    } else if (NT_SUCCESS(status)) {
        status = ioStatus.Status;
    } else if (NT_SUCCESS(ioStatus.Status)) {
        /* Keep the dispatch failure when the lower driver left IOSB untouched. */
        ioStatus.Status = status;
    }
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




/*
 * Count unallocated blocks in the data region from the on-disk allocation
 * bitmap. The caller may use this for read-only filesystem size reporting.
 */
NTSTATUS
OpenFsCountFreeDataBlocks(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ const OPENFS_VOLUME_GEOMETRY* Geometry,
    _Out_ ULONGLONG* FreeBlocks
    )
{
    OPENFS_VOLUME_LAYOUT layout;
    ULONGLONG bitsPerPage;
    ULONGLONG firstPage;
    ULONGLONG lastPage;
    ULONGLONG page;
    SIZE_T bitmapBytes;
    PUCHAR bitmap;
    ULONGLONG totalFree = 0;
    NTSTATUS status;

    if (DeviceObject == NULL || Geometry == NULL || FreeBlocks == NULL ||
        Geometry->BlockSize == 0)
        return STATUS_INVALID_PARAMETER;
    *FreeBlocks = 0;

    status = OpenFsCalculateVolumeLayout(Geometry->BlockSize,
        Geometry->TotalBlocks, Geometry->Version, &layout);
    if (!NT_SUCCESS(status)) return status;

    bitsPerPage = (ULONGLONG)Geometry->BlockSize * 8u;
    if (bitsPerPage == 0 || layout.DataStartBlock >= layout.BackupSuperblockBlock)
        return STATUS_DISK_CORRUPT_ERROR;

    firstPage = layout.DataStartBlock / bitsPerPage;
    lastPage = (layout.BackupSuperblockBlock - 1u) / bitsPerPage;
    bitmapBytes = Geometry->BlockSize;
    bitmap = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED,
        bitmapBytes, OPENFS_DEVICE_IO_TAG);
    if (bitmap == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    status = STATUS_SUCCESS;
    for (page = firstPage; page <= lastPage; ++page) {
        ULONGLONG pageBase;
        ULONGLONG pageEnd;
        ULONGLONG rangeStart;
        ULONGLONG rangeEnd;
        ULONGLONG pageFree = 0;
        ULONGLONG bitmapBlock;

        if (page > MAXULONGLONG / bitsPerPage ||
            page == MAXULONGLONG) {
            status = STATUS_INTEGER_OVERFLOW;
            break;
        }
        pageBase = page * bitsPerPage;
        if (pageBase > MAXULONGLONG - bitsPerPage) {
            status = STATUS_INTEGER_OVERFLOW;
            break;
        }
        pageEnd = pageBase + bitsPerPage;
        rangeStart = layout.DataStartBlock > pageBase
            ? layout.DataStartBlock : pageBase;
        rangeEnd = layout.BackupSuperblockBlock < pageEnd
            ? layout.BackupSuperblockBlock : pageEnd;

        /* Bitmap page zero is stored at filesystem block one. */
        bitmapBlock = page + 1u;
        status = OpenFsReadVolumeBlocks(DeviceObject, Geometry->BlockSize,
            Geometry->TotalBlocks, bitmapBlock, 1u, bitmap, bitmapBytes);
        if (!NT_SUCCESS(status)) break;

        status = OpenFsCountFreeBlocksInRange(bitmap, bitmapBytes,
            pageBase, rangeStart, rangeEnd, &pageFree);
        if (!NT_SUCCESS(status)) break;
        if (totalFree > MAXULONGLONG - pageFree) {
            status = STATUS_INTEGER_OVERFLOW;
            break;
        }
        totalFree += pageFree;
    }

    ExFreePoolWithTag(bitmap, OPENFS_DEVICE_IO_TAG);
    if (NT_SUCCESS(status)) *FreeBlocks = totalFree;
    return status;
}


/*
 * Copy a range of data-allocation bits into the Windows volume-bitmap layout.
 * StartingLcn is relative to DataStartBlock; output bit zero represents it.
 */
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
    )
{
    ULONGLONG capacityBits;
    ULONGLONG bitsToRead;
    ULONGLONG firstPhysicalBlock;
    ULONGLONG currentPhysicalBlock;
    ULONGLONG bitsPerPage;
    ULONGLONG copied = 0;
    SIZE_T bitmapPageBytes;
    PUCHAR bitmapPage;
    NTSTATUS status = STATUS_SUCCESS;

    if (DeviceObject == NULL || Geometry == NULL || Bitmap == NULL ||
        BitmapBytes == 0 || BitsRead == NULL || Geometry->BlockSize == 0 ||
        (ULONGLONG)BitmapBytes > MAXULONGLONG / 8u ||
        DataStartBlock > Geometry->TotalBlocks ||
        DataBlockCount > Geometry->TotalBlocks - DataStartBlock ||
        StartingLcn >= DataBlockCount)
        return STATUS_INVALID_PARAMETER;

    *BitsRead = 0;
    capacityBits = (ULONGLONG)BitmapBytes * 8u;
    bitsToRead = DataBlockCount - StartingLcn;
    if (bitsToRead > capacityBits) bitsToRead = capacityBits;
    if (DataStartBlock > MAXULONGLONG - StartingLcn)
        return STATUS_INTEGER_OVERFLOW;
    firstPhysicalBlock = DataStartBlock + StartingLcn;
    currentPhysicalBlock = firstPhysicalBlock;
    bitsPerPage = (ULONGLONG)Geometry->BlockSize * 8u;
    if (bitsPerPage == 0) return STATUS_INTEGER_OVERFLOW;

    bitmapPageBytes = Geometry->BlockSize;
    bitmapPage = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, bitmapPageBytes, OPENFS_DEVICE_IO_TAG);
    if (bitmapPage == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Bitmap, BitmapBytes);
    while (copied < bitsToRead) {
        ULONGLONG pageIndex = currentPhysicalBlock / bitsPerPage;
        ULONGLONG pageBase;
        ULONGLONG pageOffset;
        ULONGLONG chunk;
        ULONGLONG bitmapBlock;

        if (pageIndex > MAXULONGLONG / bitsPerPage ||
            pageIndex == MAXULONGLONG) {
            status = STATUS_INTEGER_OVERFLOW;
            break;
        }
        pageBase = pageIndex * bitsPerPage;
        pageOffset = currentPhysicalBlock - pageBase;
        chunk = bitsPerPage - pageOffset;
        if (chunk > bitsToRead - copied)
            chunk = bitsToRead - copied;

        /* Bitmap page zero is stored at filesystem block one. */
        bitmapBlock = pageIndex + 1u;
        status = OpenFsReadVolumeBlocks(DeviceObject, Geometry->BlockSize,
            Geometry->TotalBlocks, bitmapBlock, 1u,
            bitmapPage, bitmapPageBytes);
        if (!NT_SUCCESS(status)) break;

        status = OpenFsCopyBitmapBits(bitmapPage, bitmapPageBytes,
            pageOffset, chunk, Bitmap, BitmapBytes, copied);
        if (!NT_SUCCESS(status)) break;

        copied += chunk;
        currentPhysicalBlock += chunk;
    }

    ExFreePoolWithTag(bitmapPage, OPENFS_DEVICE_IO_TAG);
    if (NT_SUCCESS(status)) *BitsRead = copied;
    return status;
}


static NTSTATUS
OpenFsReadPackedInventory(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ const UCHAR* BlockList,
    _In_ ULONG BlockCount,
    _In_ ULONG RecordCount,
    _In_ ULONG RecordBytes,
    _In_ ULONG BlockSize,
    _In_ ULONGLONG DeviceLength,
    _Out_writes_bytes_(DestinationBytes) UCHAR* Destination,
    _In_ SIZE_T DestinationBytes
    )
{
    ULONGLONG requiredBytes = (ULONGLONG)RecordCount * RecordBytes;
    ULONGLONG requiredBlocks;
    ULONG index;
    SIZE_T copied = 0;
    PUCHAR blockBuffer;

    if (BlockSize == 0 || RecordBytes == 0 ||
        (requiredBytes != 0 && Destination == NULL) ||
        requiredBytes != DestinationBytes) return STATUS_INVALID_PARAMETER;
    requiredBlocks = requiredBytes / BlockSize;
    if ((requiredBytes % BlockSize) != 0) ++requiredBlocks;
    if (requiredBlocks != BlockCount || (BlockCount != 0 && BlockList == NULL))
        return STATUS_DISK_CORRUPT_ERROR;
    if (BlockCount == 0) return STATUS_SUCCESS;

    blockBuffer = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, BlockSize, OPENFS_DEVICE_IO_TAG);
    if (blockBuffer == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    for (index = 0; index < BlockCount; ++index) {
        ULONGLONG block = OpenFsInventoryDescriptorReadBlock(
            BlockList + ((SIZE_T)index * 8u));
        SIZE_T remaining = DestinationBytes - copied;
        ULONG take = (ULONG)(remaining < BlockSize ? remaining : BlockSize);
        if (block > MAXULONGLONG / BlockSize) {
            ExFreePoolWithTag(blockBuffer, OPENFS_DEVICE_IO_TAG);
            return STATUS_INTEGER_OVERFLOW;
        }
        {
            NTSTATUS status = OpenFsReadAtOffset(
                DeviceObject, block * BlockSize, DeviceLength, blockBuffer, BlockSize);
            if (!NT_SUCCESS(status)) {
                ExFreePoolWithTag(blockBuffer, OPENFS_DEVICE_IO_TAG);
                return status;
            }
        }
        RtlCopyMemory(Destination + copied, blockBuffer, take);
        copied += take;
    }
    ExFreePoolWithTag(blockBuffer, OPENFS_DEVICE_IO_TAG);
    return copied == DestinationBytes ? STATUS_SUCCESS : STATUS_DISK_CORRUPT_ERROR;
}

static NTSTATUS
OpenFsValidateDiscoveredNamespace(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONGLONG DeviceLength,
    _In_ const OPENFS_VOLUME_GEOMETRY* Geometry,
    _In_ const OPENFS_VOLUME_LAYOUT* Layout,
    _In_ const OPENFS_INVENTORY_DESCRIPTOR* Descriptor,
    _Outptr_ POPENFS_NAMESPACE_SNAPSHOT* NamespaceSnapshot
    )
{
    SIZE_T metadataBytes, directoryBytes, recordsBytes, entriesBytes;
    PUCHAR metadataRaw = NULL, directoryRaw = NULL;
    POPENFS_METADATA_RECORD records = NULL;
    POPENFS_DIRECTORY_ENTRY entries = NULL;
    ULONG index, other;
    NTSTATUS status = STATUS_SUCCESS;

    if (NamespaceSnapshot == NULL) return STATUS_INVALID_PARAMETER;
    *NamespaceSnapshot = NULL;

    /* An empty descriptor is valid before the first namespace publication. */
    if (Descriptor->MetadataRecordCount == 0 &&
        Descriptor->DirectoryEntryCount == 0) {
        return OpenFsCreateNamespaceSnapshot(NULL, 0, NULL, 0,
            Descriptor->RootIdLow, Descriptor->RootIdHigh, NamespaceSnapshot);
    }
    if (Descriptor->MetadataRecordCount == 0 ||
        Descriptor->MetadataRecordCount > MAXULONG / sizeof(OPENFS_METADATA_RECORD) ||
        Descriptor->DirectoryEntryCount > MAXULONG / sizeof(OPENFS_DIRECTORY_ENTRY) ||
        (SIZE_T)Descriptor->MetadataRecordCount >
            ((SIZE_T)-1) / OPENFS_METADATA_RECORD_BYTES ||
        (SIZE_T)Descriptor->DirectoryEntryCount >
            ((SIZE_T)-1) / OPENFS_DIRECTORY_ENTRY_BYTES) {
        return STATUS_DISK_CORRUPT_ERROR;
    }
    metadataBytes = (SIZE_T)Descriptor->MetadataRecordCount *
        OPENFS_METADATA_RECORD_BYTES;
    directoryBytes = (SIZE_T)Descriptor->DirectoryEntryCount *
        OPENFS_DIRECTORY_ENTRY_BYTES;
    recordsBytes = (SIZE_T)Descriptor->MetadataRecordCount *
        sizeof(OPENFS_METADATA_RECORD);
    entriesBytes = (SIZE_T)Descriptor->DirectoryEntryCount *
        sizeof(OPENFS_DIRECTORY_ENTRY);

    metadataRaw = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, metadataBytes, OPENFS_DEVICE_IO_TAG);
    records = (POPENFS_METADATA_RECORD)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, recordsBytes, OPENFS_DEVICE_IO_TAG);
    if (directoryBytes != 0) {
        directoryRaw = (PUCHAR)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, directoryBytes, OPENFS_DEVICE_IO_TAG);
        entries = (POPENFS_DIRECTORY_ENTRY)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, entriesBytes, OPENFS_DEVICE_IO_TAG);
    }
    if (metadataRaw == NULL || records == NULL ||
        (directoryBytes != 0 && (directoryRaw == NULL || entries == NULL))) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    status = OpenFsReadPackedInventory(
        DeviceObject, Descriptor->MetadataBlockList,
        Descriptor->MetadataBlockCount, Descriptor->MetadataRecordCount,
        OPENFS_METADATA_RECORD_BYTES, Geometry->BlockSize, DeviceLength,
        metadataRaw, metadataBytes);
    if (!NT_SUCCESS(status)) goto Exit;
    if (directoryBytes != 0) {
        status = OpenFsReadPackedInventory(
            DeviceObject, Descriptor->DirectoryBlockList,
            Descriptor->DirectoryBlockCount, Descriptor->DirectoryEntryCount,
            OPENFS_DIRECTORY_ENTRY_BYTES, Geometry->BlockSize, DeviceLength,
            directoryRaw, directoryBytes);
        if (!NT_SUCCESS(status)) goto Exit;
    }

    for (index = 0; index < Descriptor->MetadataRecordCount; ++index) {
        status = OpenFsDecodeMetadataRecord(
            metadataRaw + ((SIZE_T)index * OPENFS_METADATA_RECORD_BYTES),
            &records[index]);
        if (!NT_SUCCESS(status)) goto Exit;
        if (records[index].HasExtent) {
            for (other = 0; other < Descriptor->MetadataBlockCount; ++other) {
                if (records[index].ExtentRootBlock ==
                    OpenFsInventoryDescriptorReadBlock(
                        Descriptor->MetadataBlockList + ((SIZE_T)other * 8u))) {
                    status = STATUS_DISK_CORRUPT_ERROR;
                    goto Exit;
                }
            }
            for (other = 0; other < Descriptor->DirectoryBlockCount; ++other) {
                if (records[index].ExtentRootBlock ==
                    OpenFsInventoryDescriptorReadBlock(
                        Descriptor->DirectoryBlockList + ((SIZE_T)other * 8u))) {
                    status = STATUS_DISK_CORRUPT_ERROR;
                    goto Exit;
                }
            }
        }
    }
    for (index = 0; index < Descriptor->DirectoryEntryCount; ++index) {
        status = OpenFsDecodeDirectoryEntry(
            directoryRaw + ((SIZE_T)index * OPENFS_DIRECTORY_ENTRY_BYTES),
            &entries[index]);
        if (!NT_SUCCESS(status)) goto Exit;
    }

    status = OpenFsValidateNamespaceRecords(
        records, Descriptor->MetadataRecordCount,
        entries, Descriptor->DirectoryEntryCount,
        Descriptor->RootIdLow, Descriptor->RootIdHigh,
        Layout->DataStartBlock, Layout->BackupSuperblockBlock);
    if (!NT_SUCCESS(status)) goto Exit;

    /* Every extent-map root must be allocated, not just physically in range. */
    for (index = 0; index < Descriptor->MetadataRecordCount; ++index) {
        ULONGLONG root, bitmapIndex, bitmapBlock, localBit;
        PUCHAR bitmap;
        if (!records[index].HasExtent) continue;
        root = records[index].ExtentRootBlock;
        bitmapIndex = root / ((ULONGLONG)Geometry->BlockSize * 8u);
        if (bitmapIndex > MAXULONGLONG - Layout->BitmapStartBlock) {
            status = STATUS_INTEGER_OVERFLOW;
            goto Exit;
        }
        bitmapBlock = Layout->BitmapStartBlock + bitmapIndex;
        if (bitmapBlock > MAXULONGLONG / Geometry->BlockSize) {
            status = STATUS_INTEGER_OVERFLOW;
            goto Exit;
        }
        bitmap = (PUCHAR)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, Geometry->BlockSize, OPENFS_DEVICE_IO_TAG);
        if (bitmap == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }
        status = OpenFsReadAtOffset(
            DeviceObject, bitmapBlock * Geometry->BlockSize,
            DeviceLength, bitmap, Geometry->BlockSize);
        if (NT_SUCCESS(status)) {
            localBit = root % ((ULONGLONG)Geometry->BlockSize * 8u);
            if ((bitmap[(SIZE_T)(localBit / 8u)] &
                (UCHAR)(1u << (localBit % 8u))) == 0) {
                status = STATUS_DISK_CORRUPT_ERROR;
            }
        }
        ExFreePoolWithTag(bitmap, OPENFS_DEVICE_IO_TAG);
        if (!NT_SUCCESS(status)) goto Exit;
    }

    status = OpenFsCreateNamespaceSnapshot(records,
        Descriptor->MetadataRecordCount, entries,
        Descriptor->DirectoryEntryCount, Descriptor->RootIdLow,
        Descriptor->RootIdHigh, NamespaceSnapshot);

Exit:
    if (metadataRaw != NULL) ExFreePoolWithTag(metadataRaw, OPENFS_DEVICE_IO_TAG);
    if (directoryRaw != NULL) ExFreePoolWithTag(directoryRaw, OPENFS_DEVICE_IO_TAG);
    if (records != NULL) ExFreePoolWithTag(records, OPENFS_DEVICE_IO_TAG);
    if (entries != NULL) ExFreePoolWithTag(entries, OPENFS_DEVICE_IO_TAG);
    return status;
}



static NTSTATUS
OpenFsInsertBitmapPage(
    _Inout_updates_(Capacity) ULONGLONG* Slots,
    _In_ SIZE_T Capacity,
    _In_ ULONGLONG PageIndex
    )
{
    SIZE_T slot = (SIZE_T)((PageIndex * 11400714819323198485ULL) &
        (Capacity - 1u));
    SIZE_T probes;
    for (probes = 0; probes < Capacity; ++probes) {
        if (Slots[slot] == PageIndex) return STATUS_SUCCESS;
        if (Slots[slot] == MAXULONGLONG) {
            Slots[slot] = PageIndex;
            return STATUS_SUCCESS;
        }
        slot = (slot + 1u) & (Capacity - 1u);
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}

/* Inspect one allocated block for the persistent inventory descriptor. */
static NTSTATUS
OpenFsInspectInventoryCandidate(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ const OPENFS_VOLUME_GEOMETRY* Geometry,
    _In_ ULONGLONG DeviceLength,
    _In_ const OPENFS_VOLUME_LAYOUT* Layout,
    _In_ ULONGLONG Block,
    _In_reads_bytes_(BlockSize) const UCHAR* Candidate,
    _Inout_ ULONGLONG* DiscoveredBlock,
    _Outptr_ POPENFS_NAMESPACE_SNAPSHOT* NamespaceSnapshot
    )
{
    ULONGLONG bitsPerBitmapBlock =
        (ULONGLONG)Geometry->BlockSize * 8u;
    SIZE_T referenceCount, capacity = 1u, required, slot;
    ULONGLONG* bitmapPages = NULL;
    PUCHAR bitmap = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG index;

    if (RtlCompareMemory(Candidate, OPENFS_INVENTORY_DESCRIPTOR_MAGIC, 8) != 8)
        return STATUS_SUCCESS;
    {
        OPENFS_INVENTORY_DESCRIPTOR descriptor;
        status = OpenFsDecodeInventoryDescriptor(
            Candidate, Geometry->BlockSize, &descriptor);
        if (!NT_SUCCESS(status)) return status;
        status = OpenFsValidateInventoryDescriptorRanges(
            &descriptor, Block, Geometry->TotalBlocks,
            Layout->DataStartBlock, Layout->BackupSuperblockBlock);
        if (!NT_SUCCESS(status)) return status;
        if (*DiscoveredBlock != MAXULONGLONG)
            return STATUS_DISK_CORRUPT_ERROR;
        *DiscoveredBlock = Block;

        referenceCount = (SIZE_T)descriptor.MetadataBlockCount +
            (SIZE_T)descriptor.DirectoryBlockCount + 1u;
        if (referenceCount > ((SIZE_T)-1) / 2u)
            return STATUS_INTEGER_OVERFLOW;
        required = referenceCount * 2u;
        while (capacity < required) {
            if (capacity > ((SIZE_T)-1) / 2u)
                return STATUS_INTEGER_OVERFLOW;
            capacity <<= 1;
        }
        if (capacity > ((SIZE_T)-1) / sizeof(ULONGLONG))
            return STATUS_INTEGER_OVERFLOW;
        bitmapPages = (ULONGLONG*)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, capacity * sizeof(ULONGLONG),
            OPENFS_DEVICE_IO_TAG);
        bitmap = (PUCHAR)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, Geometry->BlockSize, OPENFS_DEVICE_IO_TAG);
        if (bitmapPages == NULL || bitmap == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto CandidateExit;
        }
        for (slot = 0; slot < capacity; ++slot)
            bitmapPages[slot] = MAXULONGLONG;

        status = OpenFsInsertBitmapPage(
            bitmapPages, capacity, Block / bitsPerBitmapBlock);
        if (!NT_SUCCESS(status)) goto CandidateExit;
        for (index = 0; index < descriptor.MetadataBlockCount; ++index) {
            ULONGLONG referenced = OpenFsInventoryDescriptorReadBlock(
                descriptor.MetadataBlockList + ((SIZE_T)index * 8u));
            status = OpenFsInsertBitmapPage(
                bitmapPages, capacity, referenced / bitsPerBitmapBlock);
            if (!NT_SUCCESS(status)) goto CandidateExit;
        }
        for (index = 0; index < descriptor.DirectoryBlockCount; ++index) {
            ULONGLONG referenced = OpenFsInventoryDescriptorReadBlock(
                descriptor.DirectoryBlockList + ((SIZE_T)index * 8u));
            status = OpenFsInsertBitmapPage(
                bitmapPages, capacity, referenced / bitsPerBitmapBlock);
            if (!NT_SUCCESS(status)) goto CandidateExit;
        }

        /* Read each distinct bitmap page once, even when many references share it. */
        for (slot = 0; slot < capacity; ++slot) {
            ULONGLONG pageIndex = bitmapPages[slot];
            if (pageIndex == MAXULONGLONG) continue;
            if (pageIndex > MAXULONGLONG / Geometry->BlockSize - 1u) {
                status = STATUS_INTEGER_OVERFLOW;
                goto CandidateExit;
            }
            status = OpenFsReadAtOffset(
                DeviceObject, (pageIndex + 1u) * Geometry->BlockSize,
                DeviceLength, bitmap, Geometry->BlockSize);
            if (!NT_SUCCESS(status)) goto CandidateExit;
            status = OpenFsValidateInventoryDescriptorBitmapPage(
                &descriptor, Block, bitmap, Geometry->BlockSize,
                pageIndex * bitsPerBitmapBlock, Geometry->TotalBlocks);
            if (!NT_SUCCESS(status)) goto CandidateExit;
        }

        status = OpenFsValidateDiscoveredNamespace(
            DeviceObject, DeviceLength, Geometry, Layout, &descriptor,
            NamespaceSnapshot);
    }

CandidateExit:
    if (bitmap != NULL) ExFreePoolWithTag(bitmap, OPENFS_DEVICE_IO_TAG);
    if (bitmapPages != NULL) ExFreePoolWithTag(bitmapPages, OPENFS_DEVICE_IO_TAG);
    return status;
}

/*
 * Candidate blocks are collected only while physically contiguous. Read up to
 * 64 adjacent blocks in one IRP, matching the bounded discovery strategy used
 * by the canonical format implementation instead of issuing one IRP per block.
 */
static NTSTATUS
OpenFsFlushInventoryScanBatch(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ const OPENFS_VOLUME_GEOMETRY* Geometry,
    _In_ ULONGLONG DeviceLength,
    _In_ const OPENFS_VOLUME_LAYOUT* Layout,
    _In_reads_(BlockCount) const ULONGLONG* Blocks,
    _In_ ULONG BlockCount,
    _Out_writes_bytes_(BatchBytes) UCHAR* Batch,
    _In_ ULONG BatchBytes,
    _Inout_ ULONGLONG* DiscoveredBlock,
    _Outptr_ POPENFS_NAMESPACE_SNAPSHOT* NamespaceSnapshot
    )
{
    ULONG expectedBytes;
    ULONG index;
    NTSTATUS status;
    ULONGLONG byteOffset;

    if (BlockCount == 0) return STATUS_SUCCESS;
    if (Blocks == NULL || Batch == NULL || DiscoveredBlock == NULL ||
        BlockCount > 64u ||
        BlockCount > MAXULONG / Geometry->BlockSize) {
        return STATUS_INVALID_PARAMETER;
    }
    expectedBytes = BlockCount * Geometry->BlockSize;
    if (BatchBytes < expectedBytes ||
        Blocks[0] > MAXULONGLONG / Geometry->BlockSize) {
        return STATUS_INTEGER_OVERFLOW;
    }
    byteOffset = Blocks[0] * Geometry->BlockSize;
    status = OpenFsReadAtOffset(
        DeviceObject, byteOffset, DeviceLength, Batch, expectedBytes);
    if (!NT_SUCCESS(status)) return status;

    for (index = 0; index < BlockCount; ++index) {
        if (index != 0 && Blocks[index] != Blocks[index - 1u] + 1u)
            return STATUS_INVALID_PARAMETER;
        status = OpenFsInspectInventoryCandidate(
            DeviceObject, Geometry, DeviceLength, Layout, Blocks[index],
            Batch + ((SIZE_T)index * Geometry->BlockSize), DiscoveredBlock,
            NamespaceSnapshot);
        if (!NT_SUCCESS(status)) return status;
    }
    return STATUS_SUCCESS;
}

/*
 * Scan allocated data blocks for the unique persistent inventory descriptor.
 * Bitmap bits are gathered into bounded contiguous batches; the superblock
 * hint remains non-authoritative, so uniqueness is established by full scan.
 */
static NTSTATUS
OpenFsDiscoverInventoryDescriptor(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ const OPENFS_VOLUME_GEOMETRY* Geometry,
    _In_ ULONGLONG DeviceLength,
    _Outptr_ POPENFS_NAMESPACE_SNAPSHOT* NamespaceSnapshot
    )
{
    OPENFS_VOLUME_LAYOUT layout;
    ULONGLONG bitsPerBitmapBlock;
    ULONGLONG firstBitmap, lastBitmap, bitmapIndex;
    ULONGLONG discoveredBlock = MAXULONGLONG;
    ULONGLONG pendingBlocks[64];
    ULONG pendingCount = 0;
    ULONG batchBytes;
    PUCHAR bitmap;
    PUCHAR batch;
    NTSTATUS status;

    status = OpenFsCalculateVolumeLayout(
        Geometry->BlockSize, Geometry->TotalBlocks, Geometry->Version, &layout);
    if (!NT_SUCCESS(status)) return status;

    bitsPerBitmapBlock = (ULONGLONG)Geometry->BlockSize * 8u;
    firstBitmap = layout.DataStartBlock / bitsPerBitmapBlock;
    if (layout.BackupSuperblockBlock == 0) return STATUS_DISK_CORRUPT_ERROR;
    lastBitmap = (layout.BackupSuperblockBlock - 1u) / bitsPerBitmapBlock;

    if (NamespaceSnapshot == NULL) return STATUS_INVALID_PARAMETER;
    *NamespaceSnapshot = NULL;

    if (Geometry->BlockSize > MAXULONG / 64u)
        return STATUS_INTEGER_OVERFLOW;
    batchBytes = Geometry->BlockSize * 64u;
    bitmap = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, Geometry->BlockSize, OPENFS_DEVICE_IO_TAG);
    batch = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, batchBytes, OPENFS_DEVICE_IO_TAG);
    if (bitmap == NULL || batch == NULL) {
        if (bitmap != NULL) ExFreePoolWithTag(bitmap, OPENFS_DEVICE_IO_TAG);
        if (batch != NULL) ExFreePoolWithTag(batch, OPENFS_DEVICE_IO_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = STATUS_SUCCESS;
    for (bitmapIndex = firstBitmap; bitmapIndex <= lastBitmap; ++bitmapIndex) {
        ULONGLONG base = bitmapIndex * bitsPerBitmapBlock;
        ULONGLONG start = layout.DataStartBlock > base
            ? layout.DataStartBlock : base;
        ULONGLONG end = layout.BackupSuperblockBlock;
        ULONGLONG pageEnd;
        ULONGLONG block;

        if (base > MAXULONGLONG - bitsPerBitmapBlock) {
            status = STATUS_INTEGER_OVERFLOW;
            goto Exit;
        }
        pageEnd = base + bitsPerBitmapBlock;
        if (end > pageEnd) end = pageEnd;
        if (bitmapIndex > MAXULONGLONG / Geometry->BlockSize - 1u) {
            status = STATUS_INTEGER_OVERFLOW;
            goto Exit;
        }
        status = OpenFsReadAtOffset(
            DeviceObject, (bitmapIndex + 1u) * Geometry->BlockSize,
            DeviceLength, bitmap, Geometry->BlockSize);
        if (!NT_SUCCESS(status)) goto Exit;

        for (block = start; block < end; ++block) {
            ULONGLONG local = block - base;
            if ((bitmap[(SIZE_T)(local / 8u)] &
                (UCHAR)(1u << (local % 8u))) == 0) continue;

            if (pendingCount != 0 &&
                pendingBlocks[pendingCount - 1u] != block - 1u) {
                status = OpenFsFlushInventoryScanBatch(
                    DeviceObject, Geometry, DeviceLength, &layout,
                    pendingBlocks, pendingCount, batch, batchBytes,
                    &discoveredBlock, NamespaceSnapshot);
                if (!NT_SUCCESS(status)) goto Exit;
                pendingCount = 0;
            }
            pendingBlocks[pendingCount++] = block;
            if (pendingCount == RTL_NUMBER_OF(pendingBlocks)) {
                status = OpenFsFlushInventoryScanBatch(
                    DeviceObject, Geometry, DeviceLength, &layout,
                    pendingBlocks, pendingCount, batch, batchBytes,
                    &discoveredBlock, NamespaceSnapshot);
                if (!NT_SUCCESS(status)) goto Exit;
                pendingCount = 0;
            }
        }
    }
    if (pendingCount != 0) {
        status = OpenFsFlushInventoryScanBatch(
            DeviceObject, Geometry, DeviceLength, &layout,
            pendingBlocks, pendingCount, batch, batchBytes,
            &discoveredBlock, NamespaceSnapshot);
    }

Exit:
    ExFreePoolWithTag(batch, OPENFS_DEVICE_IO_TAG);
    ExFreePoolWithTag(bitmap, OPENFS_DEVICE_IO_TAG);
    return status;
}

/*
 * Read and validate the persistent journal before publishing a mounted volume.
 *
 * This driver is still read-only: if the ring contains a committed transaction,
 * its home blocks may be stale and the namespace/bitmap must not be exposed until
 * a recovery-capable implementation replays REDO and durably advances a checkpoint.
 * An incomplete tail is safe to ignore because COMMIT is the durability boundary.
 */
static NTSTATUS
OpenFsValidateJournalOnDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ const OPENFS_VOLUME_GEOMETRY* Geometry,
    _In_ const OPENFS_VOLUME_LAYOUT* Layout
    )
{
    UCHAR* checkpointBuffer = NULL;
    UCHAR* ringBuffer = NULL;
    const UCHAR* ringBlocks[16];
    OPENFS_JOURNAL_CHECKPOINT firstCheckpoint;
    OPENFS_JOURNAL_CHECKPOINT secondCheckpoint;
    OPENFS_JOURNAL_CHECKPOINT selectedCheckpoint;
    OPENFS_JOURNAL_RECORD_VIEW records[16];
    OPENFS_JOURNAL_TRANSACTION_VIEW transactions[16];
    ULONG ringCount;
    ULONG recordCount = 0;
    ULONG transactionCount = 0;
    ULONG index;
    SIZE_T ringBytes;
    NTSTATUS firstStatus;
    NTSTATUS secondStatus;
    NTSTATUS status;
    UCHAR activeCopy;

    if (DeviceObject == NULL || Geometry == NULL || Layout == NULL ||
        Geometry->BlockSize == 0 ||
        Layout->RingEndBlock <= Layout->RingStartBlock)
        return STATUS_INVALID_PARAMETER;

    ringCount = (ULONG)(Layout->RingEndBlock - Layout->RingStartBlock);
    if (ringCount < 4u || ringCount > RTL_NUMBER_OF(ringBlocks) ||
        ringCount > MAXULONG / Geometry->BlockSize)
        return STATUS_DISK_CORRUPT_ERROR;

    checkpointBuffer = (UCHAR*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, Geometry->BlockSize, OPENFS_DEVICE_IO_TAG);
    if (checkpointBuffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    firstStatus = OpenFsReadVolumeBlocks(DeviceObject, Geometry->BlockSize,
        Geometry->TotalBlocks, Layout->CheckpointA, 1u,
        checkpointBuffer, Geometry->BlockSize);
    if (NT_SUCCESS(firstStatus)) {
        firstStatus = OpenFsDecodeJournalCheckpointBlock(
            checkpointBuffer, Geometry->BlockSize, ringCount, &firstCheckpoint);
    }

    secondStatus = OpenFsReadVolumeBlocks(DeviceObject, Geometry->BlockSize,
        Geometry->TotalBlocks, Layout->CheckpointB, 1u,
        checkpointBuffer, Geometry->BlockSize);
    if (NT_SUCCESS(secondStatus)) {
        secondStatus = OpenFsDecodeJournalCheckpointBlock(
            checkpointBuffer, Geometry->BlockSize, ringCount, &secondCheckpoint);
    }
    ExFreePoolWithTag(checkpointBuffer, OPENFS_DEVICE_IO_TAG);
    checkpointBuffer = NULL;

    status = OpenFsSelectJournalCheckpoint(firstStatus,
        NT_SUCCESS(firstStatus) ? &firstCheckpoint : NULL,
        secondStatus,
        NT_SUCCESS(secondStatus) ? &secondCheckpoint : NULL,
        &selectedCheckpoint, &activeCopy);
    if (!NT_SUCCESS(status))
        return status;

    ringBytes = (SIZE_T)ringCount * Geometry->BlockSize;
    ringBuffer = (UCHAR*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, ringBytes, OPENFS_DEVICE_IO_TAG);
    if (ringBuffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    status = OpenFsReadVolumeBlocks(DeviceObject, Geometry->BlockSize,
        Geometry->TotalBlocks, Layout->RingStartBlock, ringCount,
        ringBuffer, ringBytes);
    if (!NT_SUCCESS(status))
        goto Exit;

    for (index = 0; index < ringCount; ++index) {
        ringBlocks[index] = ringBuffer +
            ((SIZE_T)index * Geometry->BlockSize);
    }

    status = OpenFsScanJournalRing(ringBlocks, ringCount,
        Geometry->BlockSize, selectedCheckpoint.CheckpointSequence,
        selectedCheckpoint.NextTransactionId, records,
        RTL_NUMBER_OF(records), &recordCount);
    if (!NT_SUCCESS(status))
        goto Exit;

    status = OpenFsPlanJournalRecovery(records, recordCount,
        selectedCheckpoint.CheckpointSequence,
        selectedCheckpoint.NextTransactionId, transactions,
        RTL_NUMBER_OF(transactions), &transactionCount);
    if (!NT_SUCCESS(status))
        goto Exit;

    /* Structural validation is read-only; mount-time replay is separate. */
    UNREFERENCED_PARAMETER(transactionCount);

Exit:
    ExFreePoolWithTag(ringBuffer, OPENFS_DEVICE_IO_TAG);
    return status;
}

NTSTATUS
OpenFsRecoverVolumeJournal(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ const OPENFS_VOLUME_GEOMETRY* Geometry
    )
{
    OPENFS_VOLUME_LAYOUT layout;
    OPENFS_JOURNAL_CHECKPOINT cpA, cpB, cp;
    OPENFS_JOURNAL_RECORD_VIEW records[16];
    OPENFS_JOURNAL_TRANSACTION_VIEW txs[16];
    const UCHAR* ringBlocks[16];
    UCHAR *cpBuf = NULL, *ringBuf = NULL, *image = NULL, *zero = NULL;
    ULONGLONG targets[16], highestSequence, highestTx, nextTx;
    ULONG ringCount, recordCount = 0, txCount = 0, i, targetCount;
    ULONG blockSize, chunkCapacity, chunksPerImage, lastCommit = MAXULONG;
    SIZE_T ringBytes;
    NTSTATUS aStatus, bStatus, status;
    UCHAR activeCopy, nextCopy;

    if (DeviceObject == NULL || Geometry == NULL ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;
    blockSize = Geometry->BlockSize;
    status = OpenFsCalculateVolumeLayout(blockSize, Geometry->TotalBlocks,
        Geometry->Version, &layout);
    if (!NT_SUCCESS(status)) return status;
    ringCount = (ULONG)(layout.RingEndBlock - layout.RingStartBlock);
    if (ringCount < 4u || ringCount > RTL_NUMBER_OF(ringBlocks))
        return STATUS_DISK_CORRUPT_ERROR;

    cpBuf = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, blockSize, OPENFS_DEVICE_IO_TAG);
    ringBytes = (SIZE_T)ringCount * blockSize;
    ringBuf = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, ringBytes, OPENFS_DEVICE_IO_TAG);
    image = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, blockSize, OPENFS_DEVICE_IO_TAG);
    zero = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, blockSize, OPENFS_DEVICE_IO_TAG);
    if (!cpBuf || !ringBuf || !image || !zero) { status = STATUS_INSUFFICIENT_RESOURCES; goto ExitRecovery; }
    RtlZeroMemory(zero, blockSize);

    aStatus = OpenFsReadVolumeBlocks(DeviceObject, blockSize, Geometry->TotalBlocks,
        layout.CheckpointA, 1u, cpBuf, blockSize);
    if (NT_SUCCESS(aStatus)) aStatus = OpenFsDecodeJournalCheckpointBlock(cpBuf, blockSize, ringCount, &cpA);
    bStatus = OpenFsReadVolumeBlocks(DeviceObject, blockSize, Geometry->TotalBlocks,
        layout.CheckpointB, 1u, cpBuf, blockSize);
    if (NT_SUCCESS(bStatus)) bStatus = OpenFsDecodeJournalCheckpointBlock(cpBuf, blockSize, ringCount, &cpB);
    status = OpenFsSelectJournalCheckpoint(aStatus, NT_SUCCESS(aStatus) ? &cpA : NULL,
        bStatus, NT_SUCCESS(bStatus) ? &cpB : NULL, &cp, &activeCopy);
    if (!NT_SUCCESS(status)) goto ExitRecovery;

    status = OpenFsReadVolumeBlocks(DeviceObject, blockSize, Geometry->TotalBlocks,
        layout.RingStartBlock, ringCount, ringBuf, ringBytes);
    if (!NT_SUCCESS(status)) goto ExitRecovery;
    for (i = 0; i < ringCount; ++i) ringBlocks[i] = ringBuf + ((SIZE_T)i * blockSize);
    status = OpenFsScanJournalRing(ringBlocks, ringCount, blockSize,
        cp.CheckpointSequence, cp.NextTransactionId, records, RTL_NUMBER_OF(records), &recordCount);
    if (!NT_SUCCESS(status)) goto ExitRecovery;
    status = OpenFsPlanJournalRecovery(records, recordCount, cp.CheckpointSequence,
        cp.NextTransactionId, txs, RTL_NUMBER_OF(txs), &txCount);
    if (!NT_SUCCESS(status)) goto ExitRecovery;
    if (recordCount == 0) { status = STATUS_SUCCESS; goto ExitRecovery; }
    if (cp.Generation == MAXULONGLONG) { status = STATUS_INTEGER_OVERFLOW; goto ExitRecovery; }
    nextCopy = (UCHAR)(1u - activeCopy);

    if (txCount == 0) {
        highestSequence = records[recordCount - 1u].Sequence;
        if (highestSequence == MAXULONGLONG) { status = STATUS_INTEGER_OVERFLOW; goto ExitRecovery; }
        nextTx = cp.NextTransactionId;
        for (i = 0; i < recordCount; ++i) if (records[i].Kind == OpenFsJournalBegin) {
            if (nextTx == MAXULONGLONG) { status = STATUS_INTEGER_OVERFLOW; goto ExitRecovery; }
            ++nextTx;
        }
        cp.Generation++; cp.CheckpointSequence = highestSequence;
        cp.NextSequence = highestSequence + 1u; cp.NextTransactionId = nextTx;
        cp.HeadBlock = 0; cp.TailBlock = 0;
        status = OpenFsEncodeJournalCheckpointBlock(&cp, blockSize, ringCount, cpBuf);
        if (!NT_SUCCESS(status)) goto ExitRecovery;
        status = OpenFsWriteVolumeBlocks(DeviceObject, blockSize, Geometry->TotalBlocks,
            nextCopy ? layout.CheckpointB : layout.CheckpointA, 1u, cpBuf, blockSize);
        if (!NT_SUCCESS(status)) goto ExitRecovery;
        status = OpenFsFlushVolumeDevice(DeviceObject);
        if (!NT_SUCCESS(status)) goto ExitRecovery;
        for (i = 0; i < ringCount; ++i) {
            status = OpenFsWriteVolumeBlocks(DeviceObject, blockSize, Geometry->TotalBlocks,
                layout.RingStartBlock + i, 1u, zero, blockSize);
            if (!NT_SUCCESS(status)) goto ExitRecovery;
        }
        status = OpenFsFlushVolumeDevice(DeviceObject);
        goto ExitRecovery;
    }

    highestTx = txs[txCount - 1u].TransactionId;
    if (highestTx == MAXULONGLONG) { status = STATUS_INTEGER_OVERFLOW; goto ExitRecovery; }
    highestSequence = 0;
    for (i = 0; i < recordCount; ++i)
        if (records[i].Kind == OpenFsJournalCommit && records[i].TransactionId == highestTx) {
            highestSequence = records[i].Sequence; lastCommit = i;
        }
    if (lastCommit == MAXULONG || highestSequence == 0 || highestSequence == MAXULONGLONG) {
        status = STATUS_DISK_CORRUPT_ERROR; goto ExitRecovery;
    }

    chunkCapacity = blockSize - OPENFS_JOURNAL_RECORD_HEADER_BYTES - 12u;
    if (chunkCapacity == 0) { status = STATUS_DISK_CORRUPT_ERROR; goto ExitRecovery; }
    chunksPerImage = (blockSize + chunkCapacity - 1u) / chunkCapacity;

    /* Preflight all committed images and reject metadata or duplicate targets before replay. */
    for (i = 0; i < txCount; ++i) {
        ULONG cursor = txs[i].FirstUpdateIndex, end, prior;
        if (cursor > recordCount || txs[i].UpdateCount > recordCount - cursor) {
            status = STATUS_DISK_CORRUPT_ERROR; goto ExitRecovery;
        }
        end = cursor + txs[i].UpdateCount; targetCount = 0;
        while (cursor < end) {
            ULONGLONG target;
            if (chunksPerImage > end - cursor) { status = STATUS_DISK_CORRUPT_ERROR; goto ExitRecovery; }
            status = OpenFsDecodeRedoImage(records, cursor, chunksPerImage, blockSize, &target, image);
            if (!NT_SUCCESS(status)) goto ExitRecovery;
            if (target < layout.DataStartBlock || target >= layout.BackupSuperblockBlock) {
                status = STATUS_DISK_CORRUPT_ERROR; goto ExitRecovery;
            }
            for (prior = 0; prior < targetCount; ++prior)
                if (targets[prior] == target) { status = STATUS_DISK_CORRUPT_ERROR; goto ExitRecovery; }
            if (targetCount >= RTL_NUMBER_OF(targets)) { status = STATUS_DISK_CORRUPT_ERROR; goto ExitRecovery; }
            targets[targetCount++] = target; cursor += chunksPerImage;
        }
        if (cursor != end) { status = STATUS_DISK_CORRUPT_ERROR; goto ExitRecovery; }
    }

    /* Erase only the incomplete suffix; keep committed REDO until checkpoint publication. */
    for (i = lastCommit + 1u; i < ringCount; ++i) {
        status = OpenFsWriteVolumeBlocks(DeviceObject, blockSize, Geometry->TotalBlocks,
            layout.RingStartBlock + i, 1u, zero, blockSize);
        if (!NT_SUCCESS(status)) goto ExitRecovery;
    }
    if (lastCommit + 1u < ringCount) {
        status = OpenFsFlushVolumeDevice(DeviceObject);
        if (!NT_SUCCESS(status)) goto ExitRecovery;
    }

    for (i = 0; i < txCount; ++i) {
        ULONG cursor = txs[i].FirstUpdateIndex, end = cursor + txs[i].UpdateCount;
        while (cursor < end) {
            ULONGLONG target;
            status = OpenFsDecodeRedoImage(records, cursor, chunksPerImage, blockSize, &target, image);
            if (!NT_SUCCESS(status)) goto ExitRecovery;
            status = OpenFsWriteVolumeBlocks(DeviceObject, blockSize, Geometry->TotalBlocks,
                target, 1u, image, blockSize);
            if (!NT_SUCCESS(status)) goto ExitRecovery;
            cursor += chunksPerImage;
        }
    }
    status = OpenFsFlushVolumeDevice(DeviceObject);
    if (!NT_SUCCESS(status)) goto ExitRecovery;

    cp.Generation++; cp.CheckpointSequence = highestSequence;
    cp.NextSequence = highestSequence + 1u; cp.NextTransactionId = highestTx + 1u;
    cp.HeadBlock = 0; cp.TailBlock = 0;
    status = OpenFsEncodeJournalCheckpointBlock(&cp, blockSize, ringCount, cpBuf);
    if (!NT_SUCCESS(status)) goto ExitRecovery;
    status = OpenFsWriteVolumeBlocks(DeviceObject, blockSize, Geometry->TotalBlocks,
        nextCopy ? layout.CheckpointB : layout.CheckpointA, 1u, cpBuf, blockSize);
    if (!NT_SUCCESS(status)) goto ExitRecovery;
    status = OpenFsFlushVolumeDevice(DeviceObject);

ExitRecovery:
    if (zero) ExFreePoolWithTag(zero, OPENFS_DEVICE_IO_TAG);
    if (image) ExFreePoolWithTag(image, OPENFS_DEVICE_IO_TAG);
    if (ringBuf) ExFreePoolWithTag(ringBuf, OPENFS_DEVICE_IO_TAG);
    if (cpBuf) ExFreePoolWithTag(cpBuf, OPENFS_DEVICE_IO_TAG);
    return status;
}

NTSTATUS
OpenFsProbeVolume(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _Out_ POPENFS_VOLUME_GEOMETRY Geometry,
    _Outptr_ POPENFS_NAMESPACE_SNAPSHOT* NamespaceSnapshot
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

    if (VolumeDeviceObject == NULL || Geometry == NULL || NamespaceSnapshot == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Geometry, sizeof(*Geometry));
    *NamespaceSnapshot = NULL;

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
    if (NT_SUCCESS(status)) {
        OPENFS_VOLUME_LAYOUT layout;
        status = OpenFsCalculateVolumeLayout(Geometry->BlockSize,
            Geometry->TotalBlocks, Geometry->Version, &layout);
        if (NT_SUCCESS(status))
            status = OpenFsValidateJournalOnDevice(targetDeviceObject,
                Geometry, &layout);
    }
    if (NT_SUCCESS(status)) {
        status = OpenFsDiscoverInventoryDescriptor(targetDeviceObject, Geometry,
            deviceLength, NamespaceSnapshot);
    }

Exit:
    ExFreePoolWithTag(records, OPENFS_PROBE_BUFFER_TAG);
    if (!NT_SUCCESS(status) && *NamespaceSnapshot != NULL) {
        OpenFsFreeNamespaceSnapshot(*NamespaceSnapshot);
        *NamespaceSnapshot = NULL;
    }
    ObDereferenceObject(targetDeviceObject);
    return status;
}
