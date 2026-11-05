#include "OpenFSLayout.h"

NTSTATUS
OpenFsCalculateVolumeLayout(
    _In_ ULONG BlockSize,
    _In_ ULONGLONG TotalBlocks,
    _In_ USHORT FormatVersion,
    _Out_ POPENFS_VOLUME_LAYOUT Layout
    )
{
    ULONGLONG bitsPerBitmapBlock;
    ULONGLONG bitmapBlocks;
    ULONGLONG journalBlocks;
    ULONGLONG dataStart;

    if (Layout == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Layout, sizeof(*Layout));

    if (BlockSize < 4096u || BlockSize > 65536u ||
        (BlockSize & (BlockSize - 1u)) != 0 ||
        TotalBlocks < 12u ||
        (FormatVersion != 2u && FormatVersion != 3u && FormatVersion != 4u &&
         FormatVersion != 5u)) {
        return STATUS_DISK_CORRUPT_ERROR;
    }
    if (TotalBlocks > MAXULONGLONG / BlockSize) {
        return STATUS_INTEGER_OVERFLOW;
    }

    bitsPerBitmapBlock = (ULONGLONG)BlockSize * 8u;
    bitmapBlocks = TotalBlocks / bitsPerBitmapBlock;
    if ((TotalBlocks % bitsPerBitmapBlock) != 0) {
        ++bitmapBlocks;
    }

    journalBlocks = (FormatVersion >= 4u) ? 18u : 6u;
    if (bitmapBlocks > MAXULONGLONG - 1u - journalBlocks) {
        return STATUS_INTEGER_OVERFLOW;
    }

    dataStart = 1u + bitmapBlocks + journalBlocks;
    if (dataStart >= TotalBlocks - 1u) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

    Layout->BitmapStartBlock = 1u;
    Layout->BitmapBlocks = bitmapBlocks;
    Layout->BitmapEndBlock = 1u + bitmapBlocks;
    Layout->JournalStartBlock = Layout->BitmapEndBlock;
    Layout->CheckpointA = Layout->JournalStartBlock;
    Layout->CheckpointB = Layout->JournalStartBlock + 1u;
    Layout->RingStartBlock = Layout->JournalStartBlock + 2u;
    Layout->RingEndBlock = Layout->RingStartBlock +
        ((FormatVersion >= 4u) ? 16u : 4u);
    Layout->DataStartBlock = dataStart;
    Layout->BackupSuperblockBlock = TotalBlocks - 1u;

    return STATUS_SUCCESS;
}

NTSTATUS
OpenFsValidateReadRange(
    _In_ ULONGLONG DeviceLength,
    _In_ ULONGLONG ByteOffset,
    _In_ ULONGLONG Length
    )
{
    if (ByteOffset > (ULONGLONG)MAXLONGLONG ||
        Length > (ULONGLONG)MAXLONGLONG - ByteOffset) {
        return STATUS_INTEGER_OVERFLOW;
    }

    if (Length == 0u || ByteOffset > DeviceLength ||
        Length > DeviceLength - ByteOffset) {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}
