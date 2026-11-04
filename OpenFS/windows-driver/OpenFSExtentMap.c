#include "OpenFSExtentMap.h"

static USHORT
OpenFsExtentReadU16Le(_In_reads_bytes_(2) const UCHAR* Bytes)
{
    return (USHORT)(((USHORT)Bytes[0]) | ((USHORT)Bytes[1] << 8));
}

static ULONG
OpenFsExtentReadU32Le(_In_reads_bytes_(4) const UCHAR* Bytes)
{
    return ((ULONG)Bytes[0]) |
        ((ULONG)Bytes[1] << 8) |
        ((ULONG)Bytes[2] << 16) |
        ((ULONG)Bytes[3] << 24);
}

static ULONGLONG
OpenFsExtentReadU64Le(_In_reads_bytes_(8) const UCHAR* Bytes)
{
    return ((ULONGLONG)OpenFsExtentReadU32Le(Bytes)) |
        ((ULONGLONG)OpenFsExtentReadU32Le(Bytes + 4) << 32);
}

static BOOLEAN
OpenFsExtentRangesOverlap(
    _In_ ULONGLONG FirstStart,
    _In_ ULONGLONG FirstEnd,
    _In_ ULONGLONG SecondStart,
    _In_ ULONGLONG SecondEnd
    )
{
    return FirstStart < SecondEnd && SecondStart < FirstEnd;
}

NTSTATUS
OpenFsDecodeExtentMapBlock(
    _In_reads_bytes_(BlockSize) const UCHAR* Bytes,
    _In_ ULONG BlockSize,
    _In_ ULONGLONG FileSize,
    _In_ ULONGLONG DataStartBlock,
    _In_ ULONGLONG BackupSuperblockBlock,
    _In_ ULONGLONG TotalBlocks,
    _Out_ POPENFS_EXTENT Extents,
    _In_ ULONG ExtentCapacity,
    _Out_ PULONG ExtentCount
    )
{
    ULONG count;
    ULONG checksumOffset;
    ULONG index;
    ULONG capacity;
    ULONGLONG requiredLogicalBlocks;
    ULONGLONG previousLogicalEnd = 0;

    if (ExtentCount == NULL)
        return STATUS_INVALID_PARAMETER;
    *ExtentCount = 0;

    if (Bytes == NULL || (Extents == NULL && ExtentCapacity != 0) ||
        BlockSize < 4096u || BlockSize > 65536u ||
        (BlockSize & (BlockSize - 1u)) != 0 ||
        TotalBlocks == 0 || DataStartBlock >= BackupSuperblockBlock ||
        BackupSuperblockBlock >= TotalBlocks)
        return STATUS_INVALID_PARAMETER;

    if (RtlCompareMemory(Bytes, OPENFS_EXTENT_MAP_MAGIC, 8) != 8 ||
        OpenFsExtentReadU16Le(Bytes + 8) != 1u ||
        OpenFsExtentReadU16Le(Bytes + 10) != OPENFS_EXTENT_MAP_HEADER_BYTES)
        return STATUS_DISK_CORRUPT_ERROR;

    checksumOffset = BlockSize - OPENFS_EXTENT_MAP_CHECKSUM_BYTES;
    if (OpenFsExtentReadU32Le(Bytes + checksumOffset) !=
        OpenFsCrc32C(Bytes, checksumOffset))
        return STATUS_CRC_ERROR;

    count = OpenFsExtentReadU32Le(Bytes + 12);
    capacity = (checksumOffset - OPENFS_EXTENT_MAP_HEADER_BYTES) /
        OPENFS_EXTENT_MAP_ENTRY_BYTES;
    if (count > capacity)
        return STATUS_DISK_CORRUPT_ERROR;
    if (count > ExtentCapacity) {
        /* Report the validated allocation size; no extent is copied yet. */
        *ExtentCount = count;
        return STATUS_BUFFER_TOO_SMALL;
    }

    requiredLogicalBlocks = FileSize / BlockSize;
    if ((FileSize % BlockSize) != 0) {
        if (requiredLogicalBlocks == MAXULONGLONG)
            return STATUS_INTEGER_OVERFLOW;
        ++requiredLogicalBlocks;
    }

    for (index = 0; index < count; ++index) {
        ULONG offset = OPENFS_EXTENT_MAP_HEADER_BYTES +
            (index * OPENFS_EXTENT_MAP_ENTRY_BYTES);
        ULONGLONG logicalStart = OpenFsExtentReadU64Le(Bytes + offset);
        ULONGLONG physicalStart = OpenFsExtentReadU64Le(Bytes + offset + 8);
        ULONGLONG blockCount = OpenFsExtentReadU64Le(Bytes + offset + 16);
        ULONGLONG logicalEnd;
        ULONGLONG physicalEnd;
        ULONG previous;

        if (blockCount == 0 ||
            logicalStart > MAXULONGLONG - blockCount ||
            physicalStart > MAXULONGLONG - blockCount)
            return STATUS_DISK_CORRUPT_ERROR;

        logicalEnd = logicalStart + blockCount;
        physicalEnd = physicalStart + blockCount;
        if ((index != 0 && logicalStart < previousLogicalEnd) ||
            logicalEnd > requiredLogicalBlocks ||
            physicalStart < DataStartBlock ||
            physicalEnd > BackupSuperblockBlock)
            return STATUS_DISK_CORRUPT_ERROR;

        /* Physical runs may be in any order, but must never alias. */
        for (previous = 0; previous < index; ++previous) {
            ULONG previousOffset = OPENFS_EXTENT_MAP_HEADER_BYTES +
                (previous * OPENFS_EXTENT_MAP_ENTRY_BYTES);
            ULONGLONG previousStart =
                OpenFsExtentReadU64Le(Bytes + previousOffset + 8);
            ULONGLONG previousCount =
                OpenFsExtentReadU64Le(Bytes + previousOffset + 16);
            ULONGLONG previousEnd;

            if (previousStart > MAXULONGLONG - previousCount)
                return STATUS_DISK_CORRUPT_ERROR;
            previousEnd = previousStart + previousCount;
            if (OpenFsExtentRangesOverlap(
                    physicalStart, physicalEnd, previousStart, previousEnd))
                return STATUS_DISK_CORRUPT_ERROR;
        }
        previousLogicalEnd = logicalEnd;
    }

    if (FileSize == 0 && count != 0)
        return STATUS_DISK_CORRUPT_ERROR;

    for (index = OPENFS_EXTENT_MAP_HEADER_BYTES +
            (count * OPENFS_EXTENT_MAP_ENTRY_BYTES);
         index < checksumOffset; ++index) {
        if (Bytes[index] != 0)
            return STATUS_DISK_CORRUPT_ERROR;
    }

    for (index = 0; index < count; ++index) {
        ULONG offset = OPENFS_EXTENT_MAP_HEADER_BYTES +
            (index * OPENFS_EXTENT_MAP_ENTRY_BYTES);
        Extents[index].LogicalStart = OpenFsExtentReadU64Le(Bytes + offset);
        Extents[index].PhysicalStart = OpenFsExtentReadU64Le(Bytes + offset + 8);
        Extents[index].BlockCount = OpenFsExtentReadU64Le(Bytes + offset + 16);
    }
    *ExtentCount = count;
    return STATUS_SUCCESS;
}

NTSTATUS
OpenFsMapLogicalBlock(
    _In_reads_(ExtentCount) const OPENFS_EXTENT* Extents,
    _In_ ULONG ExtentCount,
    _In_ ULONGLONG LogicalBlock,
    _Out_ ULONGLONG* PhysicalBlock,
    _Out_ BOOLEAN* IsAllocated
    )
{
    ULONG low = 0;
    ULONG high = ExtentCount;

    if (PhysicalBlock == NULL || IsAllocated == NULL ||
        (ExtentCount != 0 && Extents == NULL))
        return STATUS_INVALID_PARAMETER;

    *PhysicalBlock = 0;
    *IsAllocated = FALSE;

    /* Upper-bound search: find the final run starting at/before the block. */
    while (low < high) {
        ULONG middle = low + ((high - low) / 2u);
        if (Extents[middle].LogicalStart <= LogicalBlock)
            low = middle + 1u;
        else
            high = middle;
    }

    if (low == 0)
        return STATUS_SUCCESS;

    {
        const OPENFS_EXTENT* extent = &Extents[low - 1u];
        ULONGLONG displacement = LogicalBlock - extent->LogicalStart;
        if (displacement >= extent->BlockCount)
            return STATUS_SUCCESS;
        if (extent->PhysicalStart > MAXULONGLONG - displacement)
            return STATUS_DISK_CORRUPT_ERROR;
        *PhysicalBlock = extent->PhysicalStart + displacement;
        *IsAllocated = TRUE;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
OpenFsGetLogicalRun(
    _In_reads_(ExtentCount) const OPENFS_EXTENT* Extents,
    _In_ ULONG ExtentCount,
    _In_ ULONGLONG LogicalBlock,
    _In_ ULONGLONG LogicalBlockCount,
    _Out_ ULONGLONG* NextLogicalBlock,
    _Out_ ULONGLONG* PhysicalBlock,
    _Out_ BOOLEAN* IsAllocated
    )
{
    ULONG low = 0;
    ULONG high = ExtentCount;
    ULONGLONG runEnd;

    if (NextLogicalBlock == NULL || PhysicalBlock == NULL ||
        IsAllocated == NULL || (ExtentCount != 0 && Extents == NULL))
        return STATUS_INVALID_PARAMETER;

    *NextLogicalBlock = 0;
    *PhysicalBlock = 0;
    *IsAllocated = FALSE;

    if (LogicalBlockCount == 0 || LogicalBlock >= LogicalBlockCount)
        return STATUS_INVALID_PARAMETER;

    /* Locate the final extent whose logical start is <= the requested VCN. */
    while (low < high) {
        ULONG middle = low + ((high - low) / 2u);
        if (Extents[middle].LogicalStart <= LogicalBlock)
            low = middle + 1u;
        else
            high = middle;
    }

    if (low != 0) {
        const OPENFS_EXTENT* extent = &Extents[low - 1u];
        ULONGLONG displacement = LogicalBlock - extent->LogicalStart;

        if (displacement < extent->BlockCount) {
            if (extent->LogicalStart > MAXULONGLONG - extent->BlockCount ||
                extent->PhysicalStart > MAXULONGLONG - displacement)
                return STATUS_DISK_CORRUPT_ERROR;
            runEnd = extent->LogicalStart + extent->BlockCount;
            *PhysicalBlock = extent->PhysicalStart + displacement;
            *IsAllocated = TRUE;
        } else {
            runEnd = LogicalBlockCount;
            if (low < ExtentCount && Extents[low].LogicalStart < runEnd)
                runEnd = Extents[low].LogicalStart;
        }
    } else {
        runEnd = LogicalBlockCount;
        if (ExtentCount != 0 && Extents[0].LogicalStart < runEnd)
            runEnd = Extents[0].LogicalStart;
    }

    if (runEnd > LogicalBlockCount)
        runEnd = LogicalBlockCount;
    if (runEnd <= LogicalBlock)
        return STATUS_DISK_CORRUPT_ERROR;

    *NextLogicalBlock = runEnd;
    return STATUS_SUCCESS;
}
