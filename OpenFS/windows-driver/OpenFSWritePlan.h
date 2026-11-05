#pragma once

#include "OpenFSExtentMap.h"

/*
 * Build the complete physical-block list touched by a byte-range write.
 *
 * This is a side-effect-free preflight: it never writes storage and rejects
 * EOF extension and sparse holes. Callers must validate the full extent map
 * and journal capacity before staging or publishing any data.
 */
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
    _Out_ ULONGLONG* PhysicalBlocks,
    _In_ ULONG PhysicalBlockCapacity,
    _Out_ PULONG RequiredBlockCount
    );
