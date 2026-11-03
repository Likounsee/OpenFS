#pragma once

#ifdef OPENFS_HOST_TEST
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t UCHAR;
typedef uint16_t USHORT;
typedef uint32_t ULONG;
typedef uint64_t ULONGLONG;
typedef size_t SIZE_T;
typedef int32_t NTSTATUS;
typedef int BOOLEAN;

#define TRUE 1
#define FALSE 0

#define MAXULONGLONG UINT64_MAX
#define MAXLONGLONG INT64_MAX
#define _In_
#define _Out_
#define _In_reads_bytes_(Length)
#define _Out_writes_bytes_(Length)

#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#define STATUS_UNRECOGNIZED_VOLUME ((NTSTATUS)0xC000014FL)
#define STATUS_CRC_ERROR ((NTSTATUS)0xC000003FL)
#define STATUS_DISK_CORRUPT_ERROR ((NTSTATUS)0xC0000032L)
#define STATUS_INTEGER_OVERFLOW ((NTSTATUS)0xC0000095L)
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)

static inline SIZE_T
OpenFsHostCompareMemory(const void* Left, const void* Right, SIZE_T Length)
{
    const UCHAR* a = (const UCHAR*)Left;
    const UCHAR* b = (const UCHAR*)Right;
    SIZE_T i;
    for (i = 0; i < Length && a[i] == b[i]; ++i) { }
    return i;
}

#define RtlCompareMemory(Left, Right, Length) OpenFsHostCompareMemory((Left), (Right), (Length))
#define RtlZeroMemory(Destination, Length) memset((Destination), 0, (Length))
#define RtlCopyMemory(Destination, Source, Length) memcpy((Destination), (Source), (Length))
#else
#include <ntifs.h>
#endif

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
