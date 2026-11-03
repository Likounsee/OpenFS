#include "OpenFSInventory.h"
#include "OpenFSNamespace.h"

#ifdef OPENFS_HOST_TEST
#include <stdlib.h>
#define OPENFS_ALLOC(bytes) malloc(bytes)
#define OPENFS_FREE(pointer) free(pointer)
#else
#define OPENFS_ALLOC(bytes) ExAllocatePool2(POOL_FLAG_NON_PAGED, (bytes), 'dInO')
#define OPENFS_FREE(pointer) ExFreePoolWithTag((pointer), 'dInO')
#endif

#define OPENFS_INVENTORY_MAX_BLOCK_SIZE 65536u

static USHORT
OpenFsInventoryReadU16(_In_reads_bytes_(2) const UCHAR* Bytes)
{
    return (USHORT)(Bytes[0] | ((USHORT)Bytes[1] << 8));
}

static ULONG
OpenFsInventoryReadU32(_In_reads_bytes_(4) const UCHAR* Bytes)
{
    return (ULONG)Bytes[0] | ((ULONG)Bytes[1] << 8) |
        ((ULONG)Bytes[2] << 16) | ((ULONG)Bytes[3] << 24);
}

ULONGLONG
OpenFsInventoryDescriptorReadBlock(_In_reads_bytes_(8) const UCHAR* Bytes)
{
    ULONGLONG value = 0;
    ULONG i;
    for (i = 0; i < 8; ++i) value |= ((ULONGLONG)Bytes[i]) << (i * 8);
    return value;
}

static BOOLEAN
OpenFsInventoryAllZero(_In_reads_bytes_(Length) const UCHAR* Bytes, _In_ SIZE_T Length)
{
    SIZE_T i;
    for (i = 0; i < Length; ++i) if (Bytes[i] != 0) return FALSE;
    return TRUE;
}

/* CRC32C over the whole descriptor while treating bytes 40..43 as zero. */
static ULONG
OpenFsInventoryDescriptorCrc(_In_reads_bytes_(Length) const UCHAR* Bytes, _In_ SIZE_T Length)
{
    ULONG crc = 0xFFFFFFFFu;
    SIZE_T i;
    for (i = 0; i < Length; ++i) {
        UCHAR value = (i >= 40 && i < 44) ? 0 : Bytes[i];
        ULONG bit;
        crc ^= value;
        for (bit = 0; bit < 8; ++bit) {
            ULONG mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0x82F63B78u & mask);
        }
    }
    return ~crc;
}

static BOOLEAN
OpenFsInventoryBytesNeeded(_In_ ULONG Count, _In_ ULONG RecordSize,
    _In_ ULONG BlockSize, _Out_ PULONG Blocks)
{
    ULONGLONG bytes = (ULONGLONG)Count * RecordSize;
    ULONGLONG blocks = bytes == 0 ? 0 : (bytes + BlockSize - 1) / BlockSize;
    if (blocks > MAXULONG) return FALSE;
    *Blocks = (ULONG)blocks;
    return TRUE;
}

NTSTATUS
OpenFsDecodeInventoryDescriptor(
    _In_reads_bytes_(BlockSize) const UCHAR* Bytes,
    _In_ ULONG BlockSize,
    _Out_ POPENFS_INVENTORY_DESCRIPTOR Descriptor
    )
{
    OPENFS_INVENTORY_DESCRIPTOR decoded;
    ULONG metadataBlocks, directoryBlocks;
    ULONGLONG requiredBytes, usedEnd;
    ULONG storedCrc;

    if (Bytes == NULL || Descriptor == NULL) return STATUS_INVALID_PARAMETER;
    if (BlockSize < 4096 || BlockSize > OPENFS_INVENTORY_MAX_BLOCK_SIZE ||
        (BlockSize & (BlockSize - 1)) != 0) return STATUS_INVALID_PARAMETER;
    if (RtlCompareMemory(Bytes, OPENFS_INVENTORY_DESCRIPTOR_MAGIC, 8) != 8)
        return STATUS_DISK_CORRUPT_ERROR;
    if (OpenFsInventoryReadU16(Bytes + 8) != 1)
        return STATUS_UNRECOGNIZED_VOLUME;
    if (OpenFsInventoryReadU16(Bytes + 10) != OPENFS_INVENTORY_DESCRIPTOR_HEADER_BYTES)
        return STATUS_DISK_CORRUPT_ERROR;
    if (!OpenFsInventoryAllZero(Bytes + 36, 4) ||
        !OpenFsInventoryAllZero(Bytes + 44, 4)) return STATUS_DISK_CORRUPT_ERROR;

    storedCrc = OpenFsInventoryReadU32(Bytes + OPENFS_INVENTORY_DESCRIPTOR_CHECKSUM_OFFSET);
    if (storedCrc != OpenFsInventoryDescriptorCrc(Bytes, BlockSize))
        return STATUS_CRC_ERROR;

    RtlZeroMemory(&decoded, sizeof(decoded));
    decoded.RootIdLow = OpenFsInventoryDescriptorReadBlock(Bytes + 12);
    decoded.RootIdHigh = OpenFsInventoryDescriptorReadBlock(Bytes + 20);
    decoded.MetadataRecordCount = OpenFsInventoryReadU32(Bytes + 28);
    decoded.DirectoryEntryCount = OpenFsInventoryReadU32(Bytes + 32);
    decoded.BlockSize = BlockSize;
    if (decoded.RootIdLow == 0 && decoded.RootIdHigh == 0)
        return STATUS_DISK_CORRUPT_ERROR;

    if (!OpenFsInventoryBytesNeeded(decoded.MetadataRecordCount,
            OPENFS_METADATA_RECORD_BYTES, BlockSize, &metadataBlocks) ||
        !OpenFsInventoryBytesNeeded(decoded.DirectoryEntryCount,
            OPENFS_DIRECTORY_ENTRY_BYTES, BlockSize, &directoryBlocks))
        return STATUS_DISK_CORRUPT_ERROR;

    requiredBytes = ((ULONGLONG)metadataBlocks + directoryBlocks) * sizeof(ULONGLONG);
    usedEnd = OPENFS_INVENTORY_DESCRIPTOR_HEADER_BYTES + requiredBytes;
    if (usedEnd > BlockSize) return STATUS_DISK_CORRUPT_ERROR;
    if (!OpenFsInventoryAllZero(Bytes + (SIZE_T)usedEnd, BlockSize - (SIZE_T)usedEnd))
        return STATUS_DISK_CORRUPT_ERROR;

    decoded.MetadataBlockCount = metadataBlocks;
    decoded.DirectoryBlockCount = directoryBlocks;
    decoded.MetadataBlockList = Bytes + OPENFS_INVENTORY_DESCRIPTOR_HEADER_BYTES;
    decoded.DirectoryBlockList = decoded.MetadataBlockList + ((SIZE_T)metadataBlocks * 8);
    *Descriptor = decoded;
    return STATUS_SUCCESS;
}

/*
 * Lists are allowed in arbitrary order. Open addressing checks uniqueness in O(n)
 * expected time and also rejects the descriptor block appearing in its own lists.
 */
NTSTATUS
OpenFsValidateInventoryDescriptorRanges(
    _In_ const OPENFS_INVENTORY_DESCRIPTOR* Descriptor,
    _In_ ULONGLONG DescriptorBlock,
    _In_ ULONGLONG TotalBlocks,
    _In_ ULONGLONG DataStartBlock,
    _In_ ULONGLONG BackupSuperblockBlock
    )
{
    SIZE_T count, capacity = 1, bytes, i;
    ULONGLONG* slots;
    const UCHAR* lists[2];
    ULONG lengths[2];
    ULONG list;

    if (Descriptor == NULL || Descriptor->BlockSize < 4096 ||
        Descriptor->BlockSize > OPENFS_INVENTORY_MAX_BLOCK_SIZE ||
        (Descriptor->BlockSize & (Descriptor->BlockSize - 1)) != 0 ||
        (Descriptor->MetadataBlockCount != 0 && Descriptor->MetadataBlockList == NULL) ||
        (Descriptor->DirectoryBlockCount != 0 && Descriptor->DirectoryBlockList == NULL) ||
        DataStartBlock >= BackupSuperblockBlock || BackupSuperblockBlock >= TotalBlocks)
        return STATUS_INVALID_PARAMETER;

    if (DescriptorBlock < DataStartBlock || DescriptorBlock >= BackupSuperblockBlock ||
        DescriptorBlock >= TotalBlocks) return STATUS_DISK_CORRUPT_ERROR;
    count = (SIZE_T)Descriptor->MetadataBlockCount + (SIZE_T)Descriptor->DirectoryBlockCount;
    if (count < (SIZE_T)Descriptor->MetadataBlockCount)
        return STATUS_INTEGER_OVERFLOW;
    if (count == 0) return STATUS_SUCCESS;
    if (count > ((SIZE_T)-1) / 2) return STATUS_INTEGER_OVERFLOW;
    while (capacity < count * 2) {
        if (capacity > ((SIZE_T)-1) / 2) return STATUS_INTEGER_OVERFLOW;
        capacity *= 2;
    }
    if (capacity > ((SIZE_T)-1) / sizeof(ULONGLONG)) return STATUS_INTEGER_OVERFLOW;
    bytes = capacity * sizeof(ULONGLONG);
    slots = (ULONGLONG*)OPENFS_ALLOC(bytes);
    if (slots == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    for (i = 0; i < capacity; ++i) slots[i] = MAXULONGLONG;

    lists[0] = Descriptor->MetadataBlockList;
    lists[1] = Descriptor->DirectoryBlockList;
    lengths[0] = Descriptor->MetadataBlockCount;
    lengths[1] = Descriptor->DirectoryBlockCount;

    for (list = 0; list < 2; ++list) {
        ULONG index;
        for (index = 0; index < lengths[list]; ++index) {
            ULONGLONG block = OpenFsInventoryDescriptorReadBlock(
                lists[list] + ((SIZE_T)index * 8));
            SIZE_T slot = (SIZE_T)((block ^ (block >> 32)) *
                11400714819323198485ull) & (capacity - 1);
            if (block == DescriptorBlock || block < DataStartBlock ||
                block >= BackupSuperblockBlock || block >= TotalBlocks) {
                OPENFS_FREE(slots);
                return STATUS_DISK_CORRUPT_ERROR;
            }
            while (slots[slot] != MAXULONGLONG) {
                if (slots[slot] == block) {
                    OPENFS_FREE(slots);
                    return STATUS_DISK_CORRUPT_ERROR;
                }
                slot = (slot + 1) & (capacity - 1);
            }
            slots[slot] = block;
        }
    }
    OPENFS_FREE(slots);
    return STATUS_SUCCESS;
}

static NTSTATUS
OpenFsInventoryRequireAllocated(
    _In_reads_bytes_(BitmapBytes) const UCHAR* Bitmap,
    _In_ SIZE_T BitmapBytes,
    _In_ ULONGLONG BitmapBaseBlock,
    _In_ ULONGLONG BitmapEndBlock,
    _In_ ULONGLONG Block
    )
{
    ULONGLONG local;
    SIZE_T byteIndex;
    UCHAR mask;

    if (Block < BitmapBaseBlock || Block >= BitmapEndBlock) {
        return STATUS_SUCCESS;
    }
    local = Block - BitmapBaseBlock;
    byteIndex = (SIZE_T)(local / 8u);
    if (byteIndex >= BitmapBytes) return STATUS_DISK_CORRUPT_ERROR;
    mask = (UCHAR)(1u << (local % 8u));
    return (Bitmap[byteIndex] & mask) != 0
        ? STATUS_SUCCESS : STATUS_DISK_CORRUPT_ERROR;
}

NTSTATUS
OpenFsValidateInventoryDescriptorBitmapPage(
    _In_ const OPENFS_INVENTORY_DESCRIPTOR* Descriptor,
    _In_ ULONGLONG DescriptorBlock,
    _In_reads_bytes_(BitmapBytes) const UCHAR* Bitmap,
    _In_ SIZE_T BitmapBytes,
    _In_ ULONGLONG BitmapBaseBlock,
    _In_ ULONGLONG TotalBlocks
    )
{
    ULONGLONG bits, bitmapEnd;
    ULONG list;

    if (Descriptor == NULL || Bitmap == NULL || BitmapBytes == 0 ||
        (ULONGLONG)BitmapBytes > MAXULONGLONG / 8u ||
        TotalBlocks == 0) return STATUS_INVALID_PARAMETER;

    bits = (ULONGLONG)BitmapBytes * 8u;
    if (BitmapBaseBlock % bits != 0 ||
        BitmapBaseBlock > MAXULONGLONG - bits ||
        BitmapBaseBlock >= TotalBlocks) return STATUS_INVALID_PARAMETER;
    bitmapEnd = BitmapBaseBlock + bits;
    if (bitmapEnd > TotalBlocks) bitmapEnd = TotalBlocks;

    if (DescriptorBlock >= TotalBlocks) return STATUS_DISK_CORRUPT_ERROR;
    {
        NTSTATUS status = OpenFsInventoryRequireAllocated(
            Bitmap, BitmapBytes, BitmapBaseBlock, bitmapEnd, DescriptorBlock);
        if (!NT_SUCCESS(status)) return status;
    }

    for (list = 0; list < 2; ++list) {
        const UCHAR* blocks = list == 0
            ? Descriptor->MetadataBlockList : Descriptor->DirectoryBlockList;
        ULONG count = list == 0
            ? Descriptor->MetadataBlockCount : Descriptor->DirectoryBlockCount;
        ULONG index;
        if (count != 0 && blocks == NULL) return STATUS_INVALID_PARAMETER;
        for (index = 0; index < count; ++index) {
            ULONGLONG block = OpenFsInventoryDescriptorReadBlock(
                blocks + ((SIZE_T)index * 8u));
            NTSTATUS status = OpenFsInventoryRequireAllocated(
                Bitmap, BitmapBytes, BitmapBaseBlock, bitmapEnd, block);
            if (!NT_SUCCESS(status)) return status;
        }
    }
    return STATUS_SUCCESS;
}
