#include <ntifs.h>
#include <ntdddisk.h>
#include "OpenFSDeviceIo.h"
#include "OpenFSBitmap.h"
#include "OpenFSLayout.h"
#include "OpenFSInventory.h"
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
    _In_ const OPENFS_INVENTORY_DESCRIPTOR* Descriptor
    )
{
    SIZE_T metadataBytes, directoryBytes, recordsBytes, entriesBytes;
    PUCHAR metadataRaw = NULL, directoryRaw = NULL;
    POPENFS_METADATA_RECORD records = NULL;
    POPENFS_DIRECTORY_ENTRY entries = NULL;
    ULONG index, other;
    NTSTATUS status = STATUS_SUCCESS;

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

Exit:
    if (metadataRaw != NULL) ExFreePoolWithTag(metadataRaw, OPENFS_DEVICE_IO_TAG);
    if (directoryRaw != NULL) ExFreePoolWithTag(directoryRaw, OPENFS_DEVICE_IO_TAG);
    if (records != NULL) ExFreePoolWithTag(records, OPENFS_DEVICE_IO_TAG);
    if (entries != NULL) ExFreePoolWithTag(entries, OPENFS_DEVICE_IO_TAG);
    return status;
}

/*
 * Scan allocated data blocks for the unique persistent inventory descriptor.
 * The scan is authoritative: the superblock hint is only an optimization and
 * is deliberately not trusted as proof of uniqueness.
 */
static NTSTATUS
OpenFsDiscoverInventoryDescriptor(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ const OPENFS_VOLUME_GEOMETRY* Geometry,
    _In_ ULONGLONG DeviceLength
    )
{
    OPENFS_VOLUME_LAYOUT layout;
    ULONGLONG bitsPerBitmapBlock;
    ULONGLONG firstBitmap;
    ULONGLONG lastBitmap;
    ULONGLONG bitmapIndex;
    ULONGLONG discoveredBlock = MAXULONGLONG;
    PUCHAR bitmap;
    PUCHAR candidate;
    NTSTATUS status;

    status = OpenFsCalculateVolumeLayout(
        Geometry->BlockSize, Geometry->TotalBlocks, Geometry->Version, &layout);
    if (!NT_SUCCESS(status)) return status;

    bitsPerBitmapBlock = (ULONGLONG)Geometry->BlockSize * 8u;
    firstBitmap = layout.DataStartBlock / bitsPerBitmapBlock;
    if (layout.BackupSuperblockBlock == 0) return STATUS_DISK_CORRUPT_ERROR;
    lastBitmap = (layout.BackupSuperblockBlock - 1u) / bitsPerBitmapBlock;

    bitmap = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, Geometry->BlockSize, OPENFS_DEVICE_IO_TAG);
    candidate = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, Geometry->BlockSize, OPENFS_DEVICE_IO_TAG);
    if (bitmap == NULL || candidate == NULL) {
        if (bitmap != NULL) ExFreePoolWithTag(bitmap, OPENFS_DEVICE_IO_TAG);
        if (candidate != NULL) ExFreePoolWithTag(candidate, OPENFS_DEVICE_IO_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = STATUS_SUCCESS;
    for (bitmapIndex = firstBitmap; bitmapIndex <= lastBitmap; ++bitmapIndex) {
        ULONGLONG base = bitmapIndex * bitsPerBitmapBlock;
        ULONGLONG start = layout.DataStartBlock > base ? layout.DataStartBlock : base;
        ULONGLONG end = layout.BackupSuperblockBlock;
        ULONGLONG pageEnd = base + bitsPerBitmapBlock;
        ULONGLONG block;

        if (end > pageEnd) end = pageEnd;
        if (bitmapIndex > MAXULONGLONG / Geometry->BlockSize - 1u) {
            status = STATUS_INTEGER_OVERFLOW;
            goto Exit;
        }
        status = OpenFsReadAtOffset(
            DeviceObject,
            (bitmapIndex + 1u) * Geometry->BlockSize,
            DeviceLength,
            bitmap,
            Geometry->BlockSize);
        if (!NT_SUCCESS(status)) goto Exit;

        for (block = start; block < end; ++block) {
            ULONGLONG local = block - base;
            if ((bitmap[(SIZE_T)(local / 8u)] &
                (UCHAR)(1u << (local % 8u))) == 0) continue;

            if (block > MAXULONGLONG / Geometry->BlockSize) {
                status = STATUS_INTEGER_OVERFLOW;
                goto Exit;
            }
            status = OpenFsReadAtOffset(
                DeviceObject,
                block * Geometry->BlockSize,
                DeviceLength,
                candidate,
                Geometry->BlockSize);
            if (!NT_SUCCESS(status)) goto Exit;

            if (RtlCompareMemory(candidate, OPENFS_INVENTORY_DESCRIPTOR_MAGIC, 8) != 8) {
                continue;
            }
            {
                OPENFS_INVENTORY_DESCRIPTOR descriptor;
                ULONG listIndex;
                status = OpenFsDecodeInventoryDescriptor(
                    candidate, Geometry->BlockSize, &descriptor);
                if (!NT_SUCCESS(status)) goto Exit;
                status = OpenFsValidateInventoryDescriptorRanges(
                    &descriptor, block, Geometry->TotalBlocks,
                    layout.DataStartBlock, layout.BackupSuperblockBlock);
                if (!NT_SUCCESS(status)) goto Exit;
                if (discoveredBlock != MAXULONGLONG) {
                    status = STATUS_DISK_CORRUPT_ERROR;
                    goto Exit;
                }
                discoveredBlock = block;

                /* Validate allocation of the descriptor and every referenced block.
                 * Each referenced bitmap page is read independently; this is correct
                 * but intentionally conservative until a bounded page cache is added.
                 */
                for (listIndex = 0; listIndex < 2; ++listIndex) {
                    const UCHAR* list = listIndex == 0
                        ? descriptor.MetadataBlockList : descriptor.DirectoryBlockList;
                    ULONG count = listIndex == 0
                        ? descriptor.MetadataBlockCount : descriptor.DirectoryBlockCount;
                    ULONG index;
                    for (index = 0; index <= count; ++index) {
                        ULONGLONG referenced = index == count
                            ? block
                            : OpenFsInventoryDescriptorReadBlock(list + ((SIZE_T)index * 8u));
                        ULONGLONG refBitmapIndex = referenced / bitsPerBitmapBlock;
                        if (refBitmapIndex > MAXULONGLONG / Geometry->BlockSize - 1u) {
                            status = STATUS_INTEGER_OVERFLOW;
                            goto Exit;
                        }
                        status = OpenFsReadAtOffset(
                            DeviceObject,
                            (refBitmapIndex + 1u) * Geometry->BlockSize,
                            DeviceLength,
                            bitmap,
                            Geometry->BlockSize);
                        if (!NT_SUCCESS(status)) goto Exit;
                        status = OpenFsValidateInventoryDescriptorBitmapPage(
                            &descriptor, block, bitmap, Geometry->BlockSize,
                            refBitmapIndex * bitsPerBitmapBlock, Geometry->TotalBlocks);
                        if (!NT_SUCCESS(status)) goto Exit;
                    }
                }
                status = OpenFsValidateDiscoveredNamespace(
                    DeviceObject, DeviceLength, Geometry, &layout, &descriptor);
                if (!NT_SUCCESS(status)) goto Exit;
            }
        }
    }

Exit:
    ExFreePoolWithTag(candidate, OPENFS_DEVICE_IO_TAG);
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
    if (NT_SUCCESS(status)) {
        status = OpenFsDiscoverInventoryDescriptor(targetDeviceObject, Geometry, deviceLength);
    }

Exit:
    ExFreePoolWithTag(records, OPENFS_PROBE_BUFFER_TAG);
    ObDereferenceObject(targetDeviceObject);
    return status;
}
