#include "OpenFSBitmap.h"

static BOOLEAN
OpenFsBitmapBlockIsAllocated(
    _In_reads_bytes_(BitmapBytes) const UCHAR* Bitmap,
    _In_ SIZE_T BitmapBytes,
    _In_ ULONGLONG LocalBlock
    )
{
    ULONGLONG byteIndex = LocalBlock / 8u;

    if (byteIndex >= BitmapBytes) {
        return FALSE;
    }

    return (Bitmap[(SIZE_T)byteIndex] &
        (UCHAR)(1u << (LocalBlock % 8u))) != 0;
}

static BOOLEAN
OpenFsBitmapRangeIsAllocated(
    _In_reads_bytes_(BitmapBytes) const UCHAR* Bitmap,
    _In_ SIZE_T BitmapBytes,
    _In_ ULONGLONG BitmapBaseBlock,
    _In_ ULONGLONG RangeStart,
    _In_ ULONGLONG RangeEnd
    )
{
    ULONGLONG first = (BitmapBaseBlock > RangeStart)
        ? BitmapBaseBlock : RangeStart;
    ULONGLONG bitmapEnd = BitmapBaseBlock + ((ULONGLONG)BitmapBytes * 8u);
    ULONGLONG end = (bitmapEnd < RangeEnd) ? bitmapEnd : RangeEnd;
    ULONGLONG block;

    for (block = first; block < end; ++block) {
        if (!OpenFsBitmapBlockIsAllocated(
                Bitmap,
                BitmapBytes,
                block - BitmapBaseBlock)) {
            return FALSE;
        }
    }

    return TRUE;
}

NTSTATUS
OpenFsValidateBitmapBlock(
    _In_reads_bytes_(BitmapBytes) const UCHAR* Bitmap,
    _In_ SIZE_T BitmapBytes,
    _In_ ULONGLONG BitmapBaseBlock,
    _In_ ULONGLONG TotalBlocks,
    _In_ ULONGLONG BitmapEndBlock,
    _In_ ULONGLONG JournalStartBlock,
    _In_ ULONGLONG RingStartBlock,
    _In_ ULONGLONG RingEndBlock
    )
{
    ULONGLONG bitmapBits;
    ULONGLONG bitmapLimit;

    if (Bitmap == NULL || BitmapBytes == 0 ||
        (ULONGLONG)BitmapBytes > MAXULONGLONG / 8u ||
        TotalBlocks < 2u) {
        return STATUS_INVALID_PARAMETER;
    }

    bitmapBits = (ULONGLONG)BitmapBytes * 8u;
    if (BitmapBaseBlock % bitmapBits != 0 ||
        BitmapBaseBlock > MAXULONGLONG - bitmapBits) {
        return STATUS_INVALID_PARAMETER;
    }
    bitmapLimit = BitmapBaseBlock + bitmapBits;

    if (BitmapEndBlock < 2u ||
        BitmapEndBlock > TotalBlocks ||
        JournalStartBlock != BitmapEndBlock ||
        JournalStartBlock > MAXULONGLONG - 2u ||
        RingStartBlock != JournalStartBlock + 2u ||
        RingEndBlock < RingStartBlock ||
        RingEndBlock >= TotalBlocks - 1u) {
        return STATUS_INVALID_PARAMETER;
    }

    if (!OpenFsBitmapRangeIsAllocated(
            Bitmap, BitmapBytes, BitmapBaseBlock, 0u, BitmapEndBlock) ||
        !OpenFsBitmapRangeIsAllocated(
            Bitmap, BitmapBytes, BitmapBaseBlock,
            JournalStartBlock, JournalStartBlock + 2u) ||
        !OpenFsBitmapRangeIsAllocated(
            Bitmap, BitmapBytes, BitmapBaseBlock, RingStartBlock, RingEndBlock) ||
        !OpenFsBitmapRangeIsAllocated(
            Bitmap, BitmapBytes, BitmapBaseBlock,
            TotalBlocks - 1u, TotalBlocks)) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

    if (TotalBlocks < bitmapLimit &&
        !OpenFsBitmapRangeIsAllocated(
            Bitmap, BitmapBytes, BitmapBaseBlock, TotalBlocks, bitmapLimit)) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

    return STATUS_SUCCESS;
}


/*
 * Count free allocation bits in the intersection of a requested block range
 * and this bitmap page. RangeEnd is exclusive. Requiring the complete range
 * to fit in the page prevents silently under-counting across bitmap pages.
 */
NTSTATUS
OpenFsCountFreeBlocksInRange(
    _In_reads_bytes_(BitmapBytes) const UCHAR* Bitmap,
    _In_ SIZE_T BitmapBytes,
    _In_ ULONGLONG BitmapBaseBlock,
    _In_ ULONGLONG RangeStart,
    _In_ ULONGLONG RangeEnd,
    _Out_ ULONGLONG* FreeBlocks
    )
{
    ULONGLONG bitmapBits;
    ULONGLONG bitmapEnd;
    ULONGLONG block;
    ULONGLONG freeCount = 0;

    if (Bitmap == NULL || BitmapBytes == 0 || FreeBlocks == NULL ||
        (ULONGLONG)BitmapBytes > MAXULONGLONG / 8u ||
        RangeEnd < RangeStart) {
        return STATUS_INVALID_PARAMETER;
    }

    bitmapBits = (ULONGLONG)BitmapBytes * 8u;
    if (BitmapBaseBlock > MAXULONGLONG - bitmapBits) {
        return STATUS_INTEGER_OVERFLOW;
    }
    bitmapEnd = BitmapBaseBlock + bitmapBits;
    if (RangeStart < BitmapBaseBlock || RangeEnd > bitmapEnd) {
        return STATUS_INVALID_PARAMETER;
    }

    for (block = RangeStart; block < RangeEnd; ++block) {
        ULONGLONG local = block - BitmapBaseBlock;
        if (!OpenFsBitmapBlockIsAllocated(Bitmap, BitmapBytes, local)) {
            if (freeCount == MAXULONGLONG)
                return STATUS_INTEGER_OVERFLOW;
            ++freeCount;
        }
    }

    *FreeBlocks = freeCount;
    return STATUS_SUCCESS;
}
