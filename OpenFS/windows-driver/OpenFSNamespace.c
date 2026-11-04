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


#ifdef OPENFS_HOST_TEST
#include <stdlib.h>
#endif

#define OPENFS_NAMESPACE_POOL_TAG 'nFsO'

static void*
OpenFsNamespaceAllocate(_In_ SIZE_T Bytes)
{
#ifdef OPENFS_HOST_TEST
    return malloc(Bytes);
#else
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, Bytes, OPENFS_NAMESPACE_POOL_TAG);
#endif
}

static void
OpenFsNamespaceFree(void* Buffer)
{
    if (Buffer == NULL) return;
#ifdef OPENFS_HOST_TEST
    free(Buffer);
#else
    ExFreePoolWithTag(Buffer, OPENFS_NAMESPACE_POOL_TAG);
#endif
}

static BOOLEAN
OpenFsNamespaceIdEqual(
    _In_ ULONGLONG LeftLow, _In_ ULONGLONG LeftHigh,
    _In_ ULONGLONG RightLow, _In_ ULONGLONG RightHigh
    )
{
    return LeftLow == RightLow && LeftHigh == RightHigh;
}

static ULONGLONG
OpenFsNamespaceHashId(_In_ ULONGLONG Low, _In_ ULONGLONG High)
{
    ULONGLONG hash = 1469598103934665603ULL;
    hash = (hash ^ Low) * 1099511628211ULL;
    hash = (hash ^ High) * 1099511628211ULL;
    hash ^= hash >> 32;
    return hash;
}

static ULONGLONG
OpenFsNamespaceHashName(_In_ const OPENFS_DIRECTORY_ENTRY* Entry)
{
    ULONGLONG hash = OpenFsNamespaceHashId(
        Entry->ParentIdLow, Entry->ParentIdHigh);
    UCHAR index;
    for (index = 0; index < Entry->NameLength; ++index) {
        hash = (hash ^ Entry->Name[index]) * 1099511628211ULL;
    }
    return hash;
}

static ULONG
OpenFsNamespaceFindRecord(
    _In_ const OPENFS_METADATA_RECORD* Records,
    _In_reads_(Capacity) const ULONG* Slots,
    _In_ SIZE_T Capacity,
    _In_ ULONGLONG IdLow,
    _In_ ULONGLONG IdHigh
    )
{
    SIZE_T slot = (SIZE_T)(OpenFsNamespaceHashId(IdLow, IdHigh) & (Capacity - 1u));
    SIZE_T probes;
    for (probes = 0; probes < Capacity; ++probes) {
        ULONG stored = Slots[slot];
        if (stored == 0) return MAXULONG;
        if (OpenFsNamespaceIdEqual(
                Records[stored - 1u].ObjectIdLow,
                Records[stored - 1u].ObjectIdHigh, IdLow, IdHigh)) {
            return stored - 1u;
        }
        slot = (slot + 1u) & (Capacity - 1u);
    }
    return MAXULONG;
}

NTSTATUS
OpenFsValidateNamespaceRecords(
    _In_reads_(RecordCount) const OPENFS_METADATA_RECORD* Records,
    _In_ ULONG RecordCount,
    _In_reads_(EntryCount) const OPENFS_DIRECTORY_ENTRY* Entries,
    _In_ ULONG EntryCount,
    _In_ ULONGLONG RootIdLow,
    _In_ ULONGLONG RootIdHigh,
    _In_ ULONGLONG DataStartBlock,
    _In_ ULONGLONG BackupSuperblockBlock
    )
{
    SIZE_T capacity = 1u;
    SIZE_T required;
    SIZE_T slot;
    ULONG* recordSlots = NULL;
    ULONG* nameSlots = NULL;
    ULONGLONG* extentSlots = NULL;
    UCHAR* entrySeen = NULL;
    UCHAR* states = NULL;
    ULONG* stack = NULL;
    ULONG rootIndex;
    ULONG index;
    NTSTATUS status = STATUS_SUCCESS;

    if ((RecordCount != 0 && Records == NULL) ||
        (EntryCount != 0 && Entries == NULL) ||
        RecordCount == 0 || (RootIdLow == 0 && RootIdHigh == 0) ||
        DataStartBlock >= BackupSuperblockBlock) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((ULONGLONG)RecordCount - 1u != EntryCount)
        return STATUS_DISK_CORRUPT_ERROR;
    required = (SIZE_T)RecordCount * 2u;
    while (capacity < required) {
        if (capacity > ((SIZE_T)-1) / 2u) return STATUS_INTEGER_OVERFLOW;
        capacity <<= 1;
    }
    if (capacity > ((SIZE_T)-1) / sizeof(ULONG) ||
        capacity > ((SIZE_T)-1) / sizeof(ULONGLONG)) {
        return STATUS_INTEGER_OVERFLOW;
    }

    recordSlots = (ULONG*)OpenFsNamespaceAllocate(capacity * sizeof(ULONG));
    nameSlots = (ULONG*)OpenFsNamespaceAllocate(capacity * sizeof(ULONG));
    extentSlots = (ULONGLONG*)OpenFsNamespaceAllocate(capacity * sizeof(ULONGLONG));
    entrySeen = (UCHAR*)OpenFsNamespaceAllocate((SIZE_T)RecordCount);
    states = (UCHAR*)OpenFsNamespaceAllocate((SIZE_T)RecordCount);
    stack = (ULONG*)OpenFsNamespaceAllocate((SIZE_T)RecordCount * sizeof(ULONG));
    if (recordSlots == NULL || nameSlots == NULL || extentSlots == NULL ||
        entrySeen == NULL || states == NULL || stack == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    RtlZeroMemory(recordSlots, capacity * sizeof(ULONG));
    RtlZeroMemory(nameSlots, capacity * sizeof(ULONG));
    RtlZeroMemory(entrySeen, (SIZE_T)RecordCount);
    RtlZeroMemory(states, (SIZE_T)RecordCount);
    for (slot = 0; slot < capacity; ++slot) extentSlots[slot] = MAXULONGLONG;

    /* Build the object-ID index and reject duplicate metadata IDs. */
    for (index = 0; index < RecordCount; ++index) {
        const OPENFS_METADATA_RECORD* record = &Records[index];
        if (record->ObjectIdLow == 0 && record->ObjectIdHigh == 0) {
            status = STATUS_DISK_CORRUPT_ERROR;
            goto Exit;
        }
        slot = (SIZE_T)(OpenFsNamespaceHashId(
            record->ObjectIdLow, record->ObjectIdHigh) & (capacity - 1u));
        while (recordSlots[slot] != 0) {
            ULONG previous = recordSlots[slot] - 1u;
            if (OpenFsNamespaceIdEqual(
                    Records[previous].ObjectIdLow, Records[previous].ObjectIdHigh,
                    record->ObjectIdLow, record->ObjectIdHigh)) {
                status = STATUS_DISK_CORRUPT_ERROR;
                goto Exit;
            }
            slot = (slot + 1u) & (capacity - 1u);
        }
        recordSlots[slot] = index + 1u;
    }

    rootIndex = OpenFsNamespaceFindRecord(
        Records, recordSlots, capacity, RootIdLow, RootIdHigh);
    if (rootIndex == MAXULONG ||
        Records[rootIndex].Kind != 2u ||
        Records[rootIndex].ParentIdLow != 0 ||
        Records[rootIndex].ParentIdHigh != 0 ||
        Records[rootIndex].Size != 0 || Records[rootIndex].HasExtent) {
        status = STATUS_DISK_CORRUPT_ERROR;
        goto Exit;
    }

    /* Validate extent ownership and reject duplicate extent-map roots. */
    for (index = 0; index < RecordCount; ++index) {
        const OPENFS_METADATA_RECORD* record = &Records[index];
        if (record->Kind != 1u && record->Kind != 2u) {
            status = STATUS_DISK_CORRUPT_ERROR;
            goto Exit;
        }
        if (record->Kind == 2u && record->HasExtent) {
            status = STATUS_DISK_CORRUPT_ERROR;
            goto Exit;
        }
        if (record->Kind == 1u && record->Size == 0 && record->HasExtent) {
            status = STATUS_DISK_CORRUPT_ERROR;
            goto Exit;
        }
        if (record->HasExtent) {
            if (record->ExtentRootBlock < DataStartBlock ||
                record->ExtentRootBlock >= BackupSuperblockBlock) {
                status = STATUS_DISK_CORRUPT_ERROR;
                goto Exit;
            }
            slot = (SIZE_T)((record->ExtentRootBlock *
                11400714819323198485ULL) & (capacity - 1u));
            while (extentSlots[slot] != MAXULONGLONG) {
                if (extentSlots[slot] == record->ExtentRootBlock) {
                    status = STATUS_DISK_CORRUPT_ERROR;
                    goto Exit;
                }
                slot = (slot + 1u) & (capacity - 1u);
            }
            extentSlots[slot] = record->ExtentRootBlock;
        }
    }

    /* Validate directory edges, unique object parents, and unique sibling names. */
    for (index = 0; index < EntryCount; ++index) {
        const OPENFS_DIRECTORY_ENTRY* entry = &Entries[index];
        ULONG parentIndex, objectIndex;
        if (entry->NameLength == 0 ||
            entry->NameLength > OPENFS_DIRECTORY_NAME_MAX_BYTES) {
            status = STATUS_DISK_CORRUPT_ERROR;
            goto Exit;
        }
        parentIndex = OpenFsNamespaceFindRecord(
            Records, recordSlots, capacity,
            entry->ParentIdLow, entry->ParentIdHigh);
        objectIndex = OpenFsNamespaceFindRecord(
            Records, recordSlots, capacity,
            entry->ObjectIdLow, entry->ObjectIdHigh);
        if (parentIndex == MAXULONG || objectIndex == MAXULONG ||
            objectIndex == rootIndex || Records[parentIndex].Kind != 2u ||
            Records[objectIndex].ParentIdLow != entry->ParentIdLow ||
            Records[objectIndex].ParentIdHigh != entry->ParentIdHigh ||
            Records[objectIndex].Kind != entry->Kind || entrySeen[objectIndex]) {
            status = STATUS_DISK_CORRUPT_ERROR;
            goto Exit;
        }
        entrySeen[objectIndex] = 1u;

        slot = (SIZE_T)(OpenFsNamespaceHashName(entry) & (capacity - 1u));
        while (nameSlots[slot] != 0) {
            const OPENFS_DIRECTORY_ENTRY* previous =
                &Entries[nameSlots[slot] - 1u];
            if (OpenFsNamespaceIdEqual(
                    previous->ParentIdLow, previous->ParentIdHigh,
                    entry->ParentIdLow, entry->ParentIdHigh) &&
                previous->NameLength == entry->NameLength &&
                RtlCompareMemory(previous->Name, entry->Name,
                    entry->NameLength) == entry->NameLength) {
                status = STATUS_DISK_CORRUPT_ERROR;
                goto Exit;
            }
            slot = (slot + 1u) & (capacity - 1u);
        }
        nameSlots[slot] = index + 1u;
    }

    for (index = 0; index < RecordCount; ++index) {
        if (index != rootIndex && entrySeen[index] == 0) {
            status = STATUS_DISK_CORRUPT_ERROR;
            goto Exit;
        }
    }

    /*
     * Every parent chain must reach root. A three-state walk detects cycles in
     * linear time while recordSlots resolves each parent ID in expected O(1).
     */
    states[rootIndex] = 2u;
    for (index = 0; index < RecordCount; ++index) {
        ULONG current = index;
        ULONG depth = 0;
        if (states[index] == 2u) continue;
        while (current != rootIndex && states[current] == 0u) {
            ULONG parentIndex;
            states[current] = 1u;
            stack[depth++] = current;
            parentIndex = OpenFsNamespaceFindRecord(
                Records, recordSlots, capacity,
                Records[current].ParentIdLow, Records[current].ParentIdHigh);
            if (parentIndex == MAXULONG || Records[parentIndex].Kind != 2u) {
                status = STATUS_DISK_CORRUPT_ERROR;
                goto Exit;
            }
            current = parentIndex;
        }
        if (current != rootIndex && states[current] == 1u) {
            status = STATUS_DISK_CORRUPT_ERROR;
            goto Exit;
        }
        while (depth != 0) states[stack[--depth]] = 2u;
    }

Exit:
    OpenFsNamespaceFree(recordSlots);
    OpenFsNamespaceFree(nameSlots);
    OpenFsNamespaceFree(extentSlots);
    OpenFsNamespaceFree(entrySeen);
    OpenFsNamespaceFree(states);
    OpenFsNamespaceFree(stack);
    return status;
}

#define OPENFS_SNAPSHOT_POOL_TAG 'sFsO'

NTSTATUS
OpenFsCreateNamespaceSnapshot(
    _In_reads_(RecordCount) const OPENFS_METADATA_RECORD* Records,
    _In_ ULONG RecordCount,
    _In_reads_(EntryCount) const OPENFS_DIRECTORY_ENTRY* Entries,
    _In_ ULONG EntryCount,
    _In_ ULONGLONG RootIdLow,
    _In_ ULONGLONG RootIdHigh,
    _Outptr_ POPENFS_NAMESPACE_SNAPSHOT* Snapshot
    )
{
    POPENFS_NAMESPACE_SNAPSHOT snapshot;
    ULONG index;
    SIZE_T recordsBytes, entriesBytes;

    if (Snapshot == NULL || (RecordCount != 0 && Records == NULL) ||
        (EntryCount != 0 && Entries == NULL) ||
        (RootIdLow == 0 && RootIdHigh == 0))
        return STATUS_INVALID_PARAMETER;
    *Snapshot = NULL;
    recordsBytes = (SIZE_T)RecordCount * sizeof(*Records);
    entriesBytes = (SIZE_T)EntryCount * sizeof(*Entries);
    if ((RecordCount != 0 &&
         recordsBytes / sizeof(*Records) != (SIZE_T)RecordCount) ||
        (EntryCount != 0 &&
         entriesBytes / sizeof(*Entries) != (SIZE_T)EntryCount))
        return STATUS_INTEGER_OVERFLOW;
    snapshot = (POPENFS_NAMESPACE_SNAPSHOT)OpenFsNamespaceAllocate(sizeof(*snapshot));
    if (snapshot == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(snapshot, sizeof(*snapshot));
    snapshot->RootRecordIndex = MAXULONG;
    if (recordsBytes != 0) {
        snapshot->Records = (POPENFS_METADATA_RECORD)OpenFsNamespaceAllocate(recordsBytes);
        if (snapshot->Records == NULL) goto NoMemory;
        RtlCopyMemory(snapshot->Records, Records, recordsBytes);
    }
    if (entriesBytes != 0) {
        snapshot->Entries = (POPENFS_DIRECTORY_ENTRY)OpenFsNamespaceAllocate(entriesBytes);
        if (snapshot->Entries == NULL) goto NoMemory;
        RtlCopyMemory(snapshot->Entries, Entries, entriesBytes);
    }
    for (index = 0; index < RecordCount; ++index) {
        if (Records[index].ObjectIdLow == RootIdLow &&
            Records[index].ObjectIdHigh == RootIdHigh) {
            snapshot->RootRecordIndex = index;
            break;
        }
    }
    if (RecordCount != 0 && snapshot->RootRecordIndex == MAXULONG)
        goto InvalidRoot;
    snapshot->RecordCount = RecordCount;
    snapshot->EntryCount = EntryCount;
    snapshot->RootIdLow = RootIdLow;
    snapshot->RootIdHigh = RootIdHigh;
    *Snapshot = snapshot;
    return STATUS_SUCCESS;

InvalidRoot:
    OpenFsFreeNamespaceSnapshot(snapshot);
    return STATUS_DISK_CORRUPT_ERROR;
NoMemory:
    OpenFsFreeNamespaceSnapshot(snapshot);
    return STATUS_INSUFFICIENT_RESOURCES;
}

VOID
OpenFsFreeNamespaceSnapshot(_In_opt_ POPENFS_NAMESPACE_SNAPSHOT Snapshot)
{
    if (Snapshot == NULL) return;
    OpenFsNamespaceFree(Snapshot->Records);
    OpenFsNamespaceFree(Snapshot->Entries);
    OpenFsNamespaceFree(Snapshot);
}

NTSTATUS
OpenFsLookupNamespaceChild(
    _In_ const OPENFS_NAMESPACE_SNAPSHOT* Snapshot,
    _In_ ULONGLONG ParentIdLow,
    _In_ ULONGLONG ParentIdHigh,
    _In_reads_bytes_(NameLength) const UCHAR* Name,
    _In_ UCHAR NameLength,
    _Out_ PULONG RecordIndex
    )
{
    ULONG index;
    BOOLEAN parentIsDirectory = FALSE;

    if (Snapshot == NULL || RecordIndex == NULL || Name == NULL ||
        NameLength == 0 || NameLength > OPENFS_DIRECTORY_NAME_MAX_BYTES ||
        (ParentIdLow == 0 && ParentIdHigh == 0))
        return STATUS_INVALID_PARAMETER;
    *RecordIndex = MAXULONG;

    for (index = 0; index < Snapshot->RecordCount; ++index) {
        const OPENFS_METADATA_RECORD* record = &Snapshot->Records[index];
        if (record->ObjectIdLow == ParentIdLow &&
            record->ObjectIdHigh == ParentIdHigh) {
            parentIsDirectory = (record->Kind == 2u);
            break;
        }
    }
    if (!parentIsDirectory &&
        !(Snapshot->RecordCount == 0 &&
          ParentIdLow == Snapshot->RootIdLow &&
          ParentIdHigh == Snapshot->RootIdHigh))
        return STATUS_NOT_FOUND;

    for (index = 0; index < Snapshot->EntryCount; ++index) {
        const OPENFS_DIRECTORY_ENTRY* entry = &Snapshot->Entries[index];
        if (entry->ParentIdLow != ParentIdLow ||
            entry->ParentIdHigh != ParentIdHigh ||
            entry->NameLength != NameLength)
            continue;
        if (RtlCompareMemory(entry->Name, Name, NameLength) == NameLength) {
            ULONG recordIndex;
            for (recordIndex = 0; recordIndex < Snapshot->RecordCount;
                 ++recordIndex) {
                const OPENFS_METADATA_RECORD* record =
                    &Snapshot->Records[recordIndex];
                if (record->ObjectIdLow == entry->ObjectIdLow &&
                    record->ObjectIdHigh == entry->ObjectIdHigh) {
                    *RecordIndex = recordIndex;
                    return STATUS_SUCCESS;
                }
            }
            return STATUS_DISK_CORRUPT_ERROR;
        }
    }
    return STATUS_NOT_FOUND;
}
