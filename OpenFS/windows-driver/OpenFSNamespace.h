#pragma once

#include "OpenFSSuperblock.h"

#define OPENFS_METADATA_RECORD_BYTES 80u
#define OPENFS_DIRECTORY_ENTRY_BYTES 128u
#define OPENFS_DIRECTORY_NAME_MAX_BYTES 64u
#define OPENFS_METADATA_NO_EXTENT MAXULONGLONG

typedef struct _OPENFS_METADATA_RECORD {
    ULONGLONG ObjectIdLow;
    ULONGLONG ObjectIdHigh;
    ULONGLONG ParentIdLow;
    ULONGLONG ParentIdHigh;
    ULONGLONG Size;
    ULONGLONG ExtentRootBlock;
    ULONG Generation;
    UCHAR Kind;
    UCHAR Flags;
    BOOLEAN HasExtent;
} OPENFS_METADATA_RECORD, *POPENFS_METADATA_RECORD;

typedef struct _OPENFS_DIRECTORY_ENTRY {
    ULONGLONG ParentIdLow;
    ULONGLONG ParentIdHigh;
    ULONGLONG ObjectIdLow;
    ULONGLONG ObjectIdHigh;
    UCHAR Kind;
    UCHAR NameLength;
    UCHAR Name[OPENFS_DIRECTORY_NAME_MAX_BYTES];
} OPENFS_DIRECTORY_ENTRY, *POPENFS_DIRECTORY_ENTRY;

NTSTATUS
OpenFsDecodeMetadataRecord(
    _In_reads_bytes_(OPENFS_METADATA_RECORD_BYTES) const UCHAR* Bytes,
    _Out_ POPENFS_METADATA_RECORD Record
    );

NTSTATUS
OpenFsDecodeDirectoryEntry(
    _In_reads_bytes_(OPENFS_DIRECTORY_ENTRY_BYTES) const UCHAR* Bytes,
    _Out_ POPENFS_DIRECTORY_ENTRY Entry
    );

/*
 * Validate the complete in-memory namespace reconstructed from decoded records.
 * Records and entries are caller-owned arrays; no allocation is performed here.
 */
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
    );

/* Immutable decoded namespace owned by a mounted-volume context. */
typedef struct _OPENFS_NAMESPACE_SNAPSHOT {
    ULONG RecordCount;
    ULONG EntryCount;
    ULONG RootRecordIndex;
    ULONGLONG RootIdLow;
    ULONGLONG RootIdHigh;
    POPENFS_METADATA_RECORD Records;
    POPENFS_DIRECTORY_ENTRY Entries;
} OPENFS_NAMESPACE_SNAPSHOT, *POPENFS_NAMESPACE_SNAPSHOT;

NTSTATUS
OpenFsCreateNamespaceSnapshot(
    _In_reads_(RecordCount) const OPENFS_METADATA_RECORD* Records,
    _In_ ULONG RecordCount,
    _In_reads_(EntryCount) const OPENFS_DIRECTORY_ENTRY* Entries,
    _In_ ULONG EntryCount,
    _In_ ULONGLONG RootIdLow,
    _In_ ULONGLONG RootIdHigh,
    _Outptr_ POPENFS_NAMESPACE_SNAPSHOT* Snapshot
    );

VOID
OpenFsFreeNamespaceSnapshot(
    _In_opt_ POPENFS_NAMESPACE_SNAPSHOT Snapshot
    );
