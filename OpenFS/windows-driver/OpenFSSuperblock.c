#include <ntifs.h>
#include "OpenFSSuperblock.h"

static ULONG
OpenFsReadU32Le(_In_reads_bytes_(4) const UCHAR* Bytes)
{
    return ((ULONG)Bytes[0]) |
           ((ULONG)Bytes[1] << 8) |
           ((ULONG)Bytes[2] << 16) |
           ((ULONG)Bytes[3] << 24);
}

static ULONGLONG
OpenFsReadU64Le(_In_reads_bytes_(8) const UCHAR* Bytes)
{
    return ((ULONGLONG)OpenFsReadU32Le(Bytes)) |
           ((ULONGLONG)OpenFsReadU32Le(Bytes + 4) << 32);
}

ULONG
OpenFsCrc32C(
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ SIZE_T Length
    )
{
    ULONG crc = 0xFFFFFFFFu;
    SIZE_T index;
    ULONG bit;

    for (index = 0; index < Length; ++index) {
        crc ^= Data[index];
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0x82F63B78u : 0u);
        }
    }
    return ~crc;
}

static NTSTATUS
OpenFsDecodeSuperblock(
    _In_reads_bytes_(OPENFS_SUPERBLOCK_BYTES) const UCHAR* Bytes,
    _Out_ POPENFS_VOLUME_GEOMETRY Geometry
    )
{
    USHORT version;
    USHORT headerSize;
    ULONG blockSize;
    ULONGLONG totalBlocks;
    ULONG storedCrc;
    SIZE_T reservedStart;
    SIZE_T index;

    if (RtlCompareMemory(Bytes, "OPENFS\0\0", 8) != 8) {
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    version = (USHORT)(Bytes[8] | ((USHORT)Bytes[9] << 8));
    headerSize = (USHORT)(Bytes[10] | ((USHORT)Bytes[11] << 8));
    if (version < 2 || version > 4 || headerSize != 48) {
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    storedCrc = OpenFsReadU32Le(Bytes + OPENFS_SUPERBLOCK_CHECKSUM_OFFSET);
    if (storedCrc != OpenFsCrc32C(Bytes, OPENFS_SUPERBLOCK_CHECKSUM_OFFSET)) {
        return STATUS_CRC_ERROR;
    }

    reservedStart = (version >= 3) ? 56u : 48u;
    for (index = reservedStart; index < OPENFS_SUPERBLOCK_CHECKSUM_OFFSET; ++index) {
        if (Bytes[index] != 0) {
            return STATUS_DISK_CORRUPT_ERROR;
        }
    }
    if (Bytes[4092] != 0 || Bytes[4093] != 0 ||
        Bytes[4094] != 0 || Bytes[4095] != 0) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

    blockSize = OpenFsReadU32Le(Bytes + 12);
    totalBlocks = OpenFsReadU64Le(Bytes + 16);
    if (blockSize < 4096 || blockSize > 65536 ||
        (blockSize & (blockSize - 1)) != 0 || totalBlocks < 12) {
        return STATUS_DISK_CORRUPT_ERROR;
    }
    if (totalBlocks > MAXULONGLONG / blockSize) {
        return STATUS_INTEGER_OVERFLOW;
    }
    Geometry->BlockSize = blockSize;
    Geometry->TotalBlocks = totalBlocks;
    Geometry->Features = OpenFsReadU64Le(Bytes + 24);
    Geometry->Version = version;
    RtlCopyMemory(Geometry->Uuid, Bytes + 32, sizeof(Geometry->Uuid));

    return STATUS_SUCCESS;
}

NTSTATUS
OpenFsValidateSuperblocks(
    _In_reads_bytes_(OPENFS_SUPERBLOCK_BYTES) const UCHAR* Primary,
    _In_reads_bytes_(OPENFS_SUPERBLOCK_BYTES) const UCHAR* Backup,
    _In_ ULONGLONG DeviceLengthBytes,
    _Out_ POPENFS_VOLUME_GEOMETRY Geometry
    )
{
    OPENFS_VOLUME_GEOMETRY primaryGeometry;
    OPENFS_VOLUME_GEOMETRY backupGeometry;
    NTSTATUS status;

    RtlZeroMemory(&primaryGeometry, sizeof(primaryGeometry));
    RtlZeroMemory(&backupGeometry, sizeof(backupGeometry));
    RtlZeroMemory(Geometry, sizeof(*Geometry));

    status = OpenFsDecodeSuperblock(Primary, &primaryGeometry);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = OpenFsDecodeSuperblock(Backup, &backupGeometry);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (primaryGeometry.BlockSize != backupGeometry.BlockSize ||
        primaryGeometry.TotalBlocks != backupGeometry.TotalBlocks ||
        primaryGeometry.Features != backupGeometry.Features ||
        primaryGeometry.Version != backupGeometry.Version ||
        RtlCompareMemory(primaryGeometry.Uuid, backupGeometry.Uuid, 16) != 16) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

    if (primaryGeometry.TotalBlocks >
        MAXULONGLONG / primaryGeometry.BlockSize) {
        return STATUS_INTEGER_OVERFLOW;
    }
    if (primaryGeometry.TotalBlocks * primaryGeometry.BlockSize != DeviceLengthBytes) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

    /*
     * Validate the version-specific reserved layout before accepting the
     * volume. the newest supported on-disk layout reserves two checkpoints plus a
     * 16-block journal; older layouts retain the historical four-block journal.
     */
    {
        ULONGLONG bitsPerBitmapBlock =
            (ULONGLONG)primaryGeometry.BlockSize * 8u;
        ULONGLONG bitmapBlocks =
            primaryGeometry.TotalBlocks / bitsPerBitmapBlock;
        ULONGLONG reservedJournalBlocks =
            (primaryGeometry.Version == 4) ? 18u : 6u;
        ULONGLONG dataStart;

        if ((primaryGeometry.TotalBlocks % bitsPerBitmapBlock) != 0) {
            ++bitmapBlocks;
        }
        if (bitmapBlocks > MAXULONGLONG - 1u - reservedJournalBlocks) {
            return STATUS_INTEGER_OVERFLOW;
        }
        dataStart = 1u + bitmapBlocks + reservedJournalBlocks;
        if (dataStart >= primaryGeometry.TotalBlocks - 1u) {
            return STATUS_DISK_CORRUPT_ERROR;
        }
    }

    /*
     * For the newer on-disk layout, the locator hint at offset 48 may differ between independently
     * checksummed copies after an interrupted hint update. The stable fields,
     * reserved bytes, and decoded geometry are compared above.
     */
    if (primaryGeometry.Version < 3 &&
        RtlCompareMemory(Primary, Backup, OPENFS_SUPERBLOCK_BYTES) != OPENFS_SUPERBLOCK_BYTES) {
        return STATUS_DISK_CORRUPT_ERROR;
    }
    if (primaryGeometry.Version >= 3) {
        if (RtlCompareMemory(Primary, Backup, 48) != 48 ||
            RtlCompareMemory(Primary + 56, Backup + 56,
                OPENFS_SUPERBLOCK_CHECKSUM_OFFSET - 56) !=
                OPENFS_SUPERBLOCK_CHECKSUM_OFFSET - 56 ||
            RtlCompareMemory(Primary + 4092, Backup + 4092, 4) != 4) {
            return STATUS_DISK_CORRUPT_ERROR;
        }
    }

    *Geometry = primaryGeometry;
    return STATUS_SUCCESS;
}
