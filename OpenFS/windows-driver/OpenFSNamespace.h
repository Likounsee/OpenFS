#pragma once

#include "OpenFSSuperblock.h"

#define OPENFS_METADATA_RECORD_BYTES 80u
#define OPENFS_DIRECTORY_ENTRY_BYTES 128u
#define OPENFS_DIRECTORY_NAME_MAX_BYTES 64u
#define OPENFS_METADATA_NO_EXTENT UINT64_MAX

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
