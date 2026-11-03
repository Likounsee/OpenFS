#pragma once

#include <ntifs.h>

#define OPENFS_SUPERBLOCK_BYTES 4096u
#define OPENFS_SUPERBLOCK_CHECKSUM_OFFSET 4088u

typedef struct _OPENFS_VOLUME_GEOMETRY {
    ULONG BlockSize;
    ULONGLONG TotalBlocks;
    ULONGLONG Features;
    USHORT Version;
    UCHAR Uuid[16];
} OPENFS_VOLUME_GEOMETRY, *POPENFS_VOLUME_GEOMETRY;

ULONG
OpenFsCrc32C(
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ SIZE_T Length
    );

NTSTATUS
OpenFsValidateSuperblocks(
    _In_reads_bytes_(OPENFS_SUPERBLOCK_BYTES) const UCHAR* Primary,
    _In_reads_bytes_(OPENFS_SUPERBLOCK_BYTES) const UCHAR* Backup,
    _In_ ULONGLONG DeviceLengthBytes,
    _Out_ POPENFS_VOLUME_GEOMETRY Geometry
    );
