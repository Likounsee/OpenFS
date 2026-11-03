#pragma once

#include "OpenFSSuperblock.h"

/*
 * Validate the reserved and padding bits in one allocation-bitmap block.
 * Data-region allocation bits are intentionally not constrained.
 */
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
    );
