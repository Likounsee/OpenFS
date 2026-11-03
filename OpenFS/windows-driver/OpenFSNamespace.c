#include "OpenFSNamespace.h"

static ULONGLONG
OpenFsNamespaceReadU64Le(_In_reads_bytes_(8) const UCHAR* Bytes)
{
    ULONGLONG value = 0;
    ULONG index;
    for (index = 0; index < 8; ++index) {
        value |= ((ULONGLONG)Bytes[index]) << (index * 8);
    }
    return value;
}

static ULONG
OpenFsNamespaceReadU32Le(_In_reads_bytes_(4) const UCHAR* Bytes)
{
    return ((ULONG)Bytes[0]) |
        ((ULONG)Bytes[1] << 8) |
        ((ULONG)Bytes[2] << 16) |
        ((ULONG)Bytes[3] << 24);
}

static BOOLEAN
OpenFsNamespaceIsZero(
    _In_reads_bytes_(Length) const UCHAR* Bytes,
    _In_ SIZE_T Length
    )
{
    SIZE_T index;
    for (index = 0; index < Length; ++index) {
        if (Bytes[index] != 0) return FALSE;
    }
    return TRUE;
}

/* Strict UTF-8 validation plus the format's path-component restrictions. */
static BOOLEAN
OpenFsNamespaceValidName(
    _In_reads_bytes_(Length) const UCHAR* Name,
    _In_ UCHAR Length
    )
{
    SIZE_T i = 0;

    if (Name == NULL || Length == 0 ||
        Length > OPENFS_DIRECTORY_NAME_MAX_BYTES) return FALSE;
    if ((Length == 1 && Name[0] == '.') ||
        (Length == 2 && Name[0] == '.' && Name[1] == '.')) return FALSE;

    while (i < Length) {
        ULONG codePoint;
        UCHAR first = Name[i++];
        ULONG continuationCount;
        ULONG minimum;

        if (first < 0x80) {
            codePoint = first;
            continuationCount = 0;
            minimum = 0;
        } else if (first >= 0xC2 && first <= 0xDF) {
            codePoint = first & 0x1Fu;
            continuationCount = 1;
            minimum = 0x80;
        } else if (first >= 0xE0 && first <= 0xEF) {
            codePoint = first & 0x0Fu;
            continuationCount = 2;
            minimum = 0x800;
        } else if (first >= 0xF0 && first <= 0xF4) {
            codePoint = first & 0x07u;
            continuationCount = 3;
            minimum = 0x10000;
        } else {
            return FALSE;
        }

        if (i + continuationCount > Length) return FALSE;
        while (continuationCount-- != 0) {
            UCHAR next = Name[i++];
            if ((next & 0xC0u) != 0x80u) return FALSE;
            codePoint = (codePoint << 6) | (next & 0x3Fu);
        }

        if ((codePoint < minimum) || codePoint > 0x10FFFFu ||
            (codePoint >= 0xD800u && codePoint <= 0xDFFFu)) return FALSE;
        if (codePoint == '/' || codePoint == '\\' || codePoint == 0 ||
            codePoint <= 0x1Fu ||
            (codePoint >= 0x7Fu && codePoint <= 0x9Fu)) return FALSE;
    }

    return TRUE;
}

NTSTATUS
OpenFsDecodeMetadataRecord(
    _In_reads_bytes_(OPENFS_METADATA_RECORD_BYTES) const UCHAR* Bytes,
    _Out_ POPENFS_METADATA_RECORD Record
    )
{
    OPENFS_METADATA_RECORD decoded;
    ULONG storedCrc;

    if (Bytes == NULL || Record == NULL) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&decoded, sizeof(decoded));

    if (RtlCompareMemory(Bytes, "OFSMETA\0", 8) != 8 ||
        Bytes[8] != 1 || Bytes[9] != 0) {
        return STATUS_DISK_CORRUPT_ERROR;
    }
    if (Bytes[10] != 1 && Bytes[10] != 2) {
        return STATUS_DISK_CORRUPT_ERROR;
    }
    if (!OpenFsNamespaceIsZero(Bytes + 64, 12)) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

    storedCrc = OpenFsNamespaceReadU32Le(Bytes + 76);
    if (storedCrc != OpenFsCrc32C(Bytes, 76)) return STATUS_CRC_ERROR;

    decoded.Kind = Bytes[10];
    decoded.Flags = Bytes[11];
    decoded.ObjectIdLow = OpenFsNamespaceReadU64Le(Bytes + 12);
    decoded.ObjectIdHigh = OpenFsNamespaceReadU64Le(Bytes + 20);
    decoded.ParentIdLow = OpenFsNamespaceReadU64Le(Bytes + 28);
    decoded.ParentIdHigh = OpenFsNamespaceReadU64Le(Bytes + 36);
    decoded.Size = OpenFsNamespaceReadU64Le(Bytes + 44);
    decoded.ExtentRootBlock = OpenFsNamespaceReadU64Le(Bytes + 52);
    decoded.Generation = OpenFsNamespaceReadU32Le(Bytes + 60);
    decoded.HasExtent = decoded.ExtentRootBlock != OPENFS_METADATA_NO_EXTENT;

    if ((decoded.ObjectIdLow == 0 && decoded.ObjectIdHigh == 0) ||
        (decoded.ObjectIdLow == decoded.ParentIdLow &&
         decoded.ObjectIdHigh == decoded.ParentIdHigh)) {
        return STATUS_DISK_CORRUPT_ERROR;
    }
    if (decoded.Kind == 2 &&
        (decoded.Size != 0 || decoded.HasExtent)) {
        return STATUS_DISK_CORRUPT_ERROR;
    }
    if (decoded.HasExtent && decoded.Size == 0) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

    *Record = decoded;
    return STATUS_SUCCESS;
}

NTSTATUS
OpenFsDecodeDirectoryEntry(
    _In_reads_bytes_(OPENFS_DIRECTORY_ENTRY_BYTES) const UCHAR* Bytes,
    _Out_ POPENFS_DIRECTORY_ENTRY Entry
    )
{
    OPENFS_DIRECTORY_ENTRY decoded;
    UCHAR nameLength;
    ULONG storedCrc;

    if (Bytes == NULL || Entry == NULL) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&decoded, sizeof(decoded));

    if (RtlCompareMemory(Bytes, "OFSDENT\0", 8) != 8 ||
        Bytes[8] != 1 || Bytes[9] != 0) {
        return STATUS_DISK_CORRUPT_ERROR;
    }
    if (Bytes[10] != 1 && Bytes[10] != 2) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

    nameLength = Bytes[11];
    if (!OpenFsNamespaceValidName(Bytes + 44, nameLength)) {
        return STATUS_DISK_CORRUPT_ERROR;
    }
    if (!OpenFsNamespaceIsZero(Bytes + 44 + nameLength,
            OPENFS_DIRECTORY_NAME_MAX_BYTES - nameLength) ||
        !OpenFsNamespaceIsZero(Bytes + 108, 16)) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

    storedCrc = OpenFsNamespaceReadU32Le(Bytes + 124);
    if (storedCrc != OpenFsCrc32C(Bytes, 124)) return STATUS_CRC_ERROR;

    decoded.Kind = Bytes[10];
    decoded.NameLength = nameLength;
    decoded.ParentIdLow = OpenFsNamespaceReadU64Le(Bytes + 12);
    decoded.ParentIdHigh = OpenFsNamespaceReadU64Le(Bytes + 20);
    decoded.ObjectIdLow = OpenFsNamespaceReadU64Le(Bytes + 28);
    decoded.ObjectIdHigh = OpenFsNamespaceReadU64Le(Bytes + 36);

    if ((decoded.ParentIdLow == 0 && decoded.ParentIdHigh == 0) ||
        (decoded.ObjectIdLow == 0 && decoded.ObjectIdHigh == 0) ||
        (decoded.ParentIdLow == decoded.ObjectIdLow &&
         decoded.ParentIdHigh == decoded.ObjectIdHigh)) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

    RtlCopyMemory(decoded.Name, Bytes + 44, nameLength);
    *Entry = decoded;
    return STATUS_SUCCESS;
}
