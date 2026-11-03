#pragma once

#include "OpenFSSuperblock.h"

#define OPENFS_INVENTORY_DESCRIPTOR_MAGIC "OFSMINV\0"
#define OPENFS_INVENTORY_DESCRIPTOR_HEADER_BYTES 48u
#define OPENFS_INVENTORY_DESCRIPTOR_CHECKSUM_OFFSET 40u

typedef struct _OPENFS_INVENTORY_DESCRIPTOR {
    ULONGLONG RootIdLow;
    ULONGLONG RootIdHigh;
    ULONG MetadataRecordCount;
    ULONG DirectoryEntryCount;
    ULONG MetadataBlockCount;
    ULONG DirectoryBlockCount;
    const UCHAR* MetadataBlockList;
    const UCHAR* DirectoryBlockList;
    ULONG BlockSize;
} OPENFS_INVENTORY_DESCRIPTOR, *POPENFS_INVENTORY_DESCRIPTOR;

NTSTATUS
OpenFsDecodeInventoryDescriptor(
    _In_reads_bytes_(BlockSize) const UCHAR* Bytes,
    _In_ ULONG BlockSize,
    _Out_ POPENFS_INVENTORY_DESCRIPTOR Descriptor
    );

NTSTATUS
OpenFsValidateInventoryDescriptorRanges(
    _In_ const OPENFS_INVENTORY_DESCRIPTOR* Descriptor,
    _In_ ULONGLONG DescriptorBlock,
    _In_ ULONGLONG TotalBlocks,
    _In_ ULONGLONG DataStartBlock,
    _In_ ULONGLONG BackupSuperblockBlock
    );

ULONGLONG
OpenFsInventoryDescriptorReadBlock(
    _In_reads_bytes_(8) const UCHAR* EncodedBlock
    );

/*
 * Check that the descriptor and every inventory block located on one bitmap
 * page are marked allocated. Call once for each bitmap page intersecting the
 * descriptor's physical references.
 */
NTSTATUS
OpenFsValidateInventoryDescriptorBitmapPage(
    _In_ const OPENFS_INVENTORY_DESCRIPTOR* Descriptor,
    _In_ ULONGLONG DescriptorBlock,
    _In_reads_bytes_(BitmapBytes) const UCHAR* Bitmap,
    _In_ SIZE_T BitmapBytes,
    _In_ ULONGLONG BitmapBaseBlock,
    _In_ ULONGLONG TotalBlocks
    );
