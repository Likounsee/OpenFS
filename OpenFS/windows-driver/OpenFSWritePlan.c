#include "OpenFSWritePlan.h"

NTSTATUS
OpenFsBuildWriteBlockPlan(
    _In_reads_(ExtentCount) const OPENFS_EXTENT* Extents,
    _In_ ULONG ExtentCount,
    _In_ ULONGLONG FileSize,
    _In_ ULONG BlockSize,
    _In_ ULONGLONG ByteOffset,
    _In_ ULONG ByteLength,
    _In_ ULONGLONG DataStartBlock,
    _In_ ULONGLONG BackupSuperblockBlock,
    _In_ ULONGLONG TotalBlocks,
    _Out_writes_(PhysicalBlockCapacity) ULONGLONG* PhysicalBlocks,
    _In_ ULONG PhysicalBlockCapacity,
    _Out_ PULONG RequiredBlockCount
    )
{
    ULONGLONG end;
    ULONGLONG firstLogical;
    ULONGLONG lastLogical;
    ULONGLONG logical;
    ULONGLONG count64;
    ULONG count;
    ULONG index;

    if (RequiredBlockCount == NULL ||
        (ExtentCount != 0 && Extents == NULL) ||
        (PhysicalBlockCapacity != 0 && PhysicalBlocks == NULL) ||
        BlockSize < 4096u || BlockSize > 65536u ||
        (BlockSize & (BlockSize - 1u)) != 0 ||
        TotalBlocks == 0 || DataStartBlock >= BackupSuperblockBlock ||
        BackupSuperblockBlock >= TotalBlocks)
        return STATUS_INVALID_PARAMETER;

    *RequiredBlockCount = 0;
    if (ByteLength == 0)
        return STATUS_SUCCESS;

    if (ByteOffset >= FileSize ||
        ByteOffset > MAXULONGLONG - (ULONGLONG)ByteLength)
        return STATUS_INVALID_PARAMETER;
    end = ByteOffset + (ULONGLONG)ByteLength;
    if (end > FileSize)
        return STATUS_INVALID_PARAMETER;

    firstLogical = ByteOffset / BlockSize;
    lastLogical = (end - 1u) / BlockSize;
    count64 = lastLogical - firstLogical + 1u;
    if (count64 > MAXULONG)
        return STATUS_INTEGER_OVERFLOW;
    count = (ULONG)count64;
    *RequiredBlockCount = count;
    if (count > PhysicalBlockCapacity)
        return STATUS_BUFFER_TOO_SMALL;

    for (index = 0; index < count; ++index) {
        ULONGLONG physical = 0;
        BOOLEAN allocated = FALSE;
        NTSTATUS status;

        logical = firstLogical + index;
        status = OpenFsMapLogicalBlock(
            Extents, ExtentCount, logical, &physical, &allocated);
        if (!NT_SUCCESS(status))
            return status;
        if (!allocated)
            return STATUS_NOT_FOUND;
        if (physical < DataStartBlock || physical >= BackupSuperblockBlock ||
            physical >= TotalBlocks)
            return STATUS_DISK_CORRUPT_ERROR;

        /* A corrupt caller-supplied map must not direct two VCNs to one block. */
        {
            ULONG previous;
            for (previous = 0; previous < index; ++previous) {
                if (PhysicalBlocks[previous] == physical)
                    return STATUS_DISK_CORRUPT_ERROR;
            }
        }
        PhysicalBlocks[index] = physical;
    }

    return STATUS_SUCCESS;
}
