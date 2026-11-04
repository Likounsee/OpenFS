#pragma once

#include "OpenFSSuperblock.h"

#define OPENFS_EXTENT_MAP_MAGIC "OFSEXT\0\0"
#define OPENFS_EXTENT_MAP_HEADER_BYTES 16u
#define OPENFS_EXTENT_MAP_ENTRY_BYTES 24u
#define OPENFS_EXTENT_MAP_CHECKSUM_BYTES 4u

typedef struct _OPENFS_EXTENT {
    ULONGLONG LogicalStart;
    ULONGLONG PhysicalStart;
    ULONGLONG BlockCount;
} OPENFS_EXTENT, *POPENFS_EXTENT;

/*
 * Decode and fully validate one checksummed extent-map block.
 * Extents are copied only after the complete page has passed validation.
 */
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
    );

/* Map one logical file block to physical storage; sparse holes return FALSE. */
NTSTATUS
OpenFsMapLogicalBlock(
    _In_reads_(ExtentCount) const OPENFS_EXTENT* Extents,
    _In_ ULONG ExtentCount,
    _In_ ULONGLONG LogicalBlock,
    _Out_ PULONGLONG PhysicalBlock,
    _Out_ PBOOLEAN IsAllocated
    );
