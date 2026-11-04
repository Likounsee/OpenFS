#include <stdio.h>
#include <string.h>
#include "OpenFSNamespace.h"

static unsigned int g_Checks;
static unsigned int g_Failures;

static void
Check(int Condition, const char* Name)
{
    ++g_Checks;
    if (!Condition) {
        ++g_Failures;
        fprintf(stderr, "FAIL: %s\n", Name);
    }
}

static void
WriteU32Le(UCHAR* Bytes, ULONG Value)
{
    Bytes[0] = (UCHAR)Value;
    Bytes[1] = (UCHAR)(Value >> 8);
    Bytes[2] = (UCHAR)(Value >> 16);
    Bytes[3] = (UCHAR)(Value >> 24);
}

static void
WriteU64Le(UCHAR* Bytes, ULONGLONG Value)
{
    ULONG index;
    for (index = 0; index < 8; ++index) {
        Bytes[index] = (UCHAR)(Value >> (index * 8));
    }
}

static void
Seal(UCHAR* Bytes, SIZE_T ChecksumOffset, SIZE_T ChecksumLength)
{
    WriteU32Le(Bytes + ChecksumOffset,
        OpenFsCrc32C(Bytes, ChecksumLength));
}

static void
MakeMetadata(UCHAR* Bytes, UCHAR Kind, ULONGLONG Id, ULONGLONG Parent,
    ULONGLONG Size, ULONGLONG Extent)
{
    memset(Bytes, 0, OPENFS_METADATA_RECORD_BYTES);
    memcpy(Bytes, "OFSMETA\0", 8);
    Bytes[8] = 1;
    Bytes[10] = Kind;
    WriteU64Le(Bytes + 12, Id);
    WriteU64Le(Bytes + 28, Parent);
    WriteU64Le(Bytes + 44, Size);
    WriteU64Le(Bytes + 52, Extent);
    WriteU32Le(Bytes + 60, 7);
    Seal(Bytes, 76, 76);
}

static void
MakeDirectoryEntry(UCHAR* Bytes, const UCHAR* Name, UCHAR NameLength,
    ULONGLONG Parent, ULONGLONG Id)
{
    memset(Bytes, 0, OPENFS_DIRECTORY_ENTRY_BYTES);
    memcpy(Bytes, "OFSDENT\0", 8);
    Bytes[8] = 1;
    Bytes[10] = 1;
    Bytes[11] = NameLength;
    WriteU64Le(Bytes + 12, Parent);
    WriteU64Le(Bytes + 28, Id);
    memcpy(Bytes + 44, Name, NameLength);
    Seal(Bytes, 124, 124);
}

int
main(void)
{
    UCHAR bytes[OPENFS_DIRECTORY_ENTRY_BYTES];
    OPENFS_METADATA_RECORD metadata;
    OPENFS_DIRECTORY_ENTRY entry;
    static const UCHAR fileName[] = "file.txt";
    static const UCHAR utf8Name[] = { 'c', 'a', 'f', 0xC3, 0xA9 };

    MakeMetadata(bytes, 2, 1, 0, 0, OPENFS_METADATA_NO_EXTENT);
    Check(OpenFsDecodeMetadataRecord(bytes, &metadata) == STATUS_SUCCESS,
        "root directory metadata decodes");
    Check(metadata.ObjectIdLow == 1 && metadata.ParentIdLow == 0 &&
        metadata.Kind == 2 && !metadata.HasExtent,
        "root metadata fields decoded");

    MakeMetadata(bytes, 1, 42, 1, 8192, 17);
    Check(OpenFsDecodeMetadataRecord(bytes, &metadata) == STATUS_SUCCESS,
        "file metadata with extent decodes");
    Check(metadata.Size == 8192 && metadata.ExtentRootBlock == 17 &&
        metadata.Generation == 7,
        "file metadata fields decoded");

    MakeMetadata(bytes, 1, 42, 1, 8192, 17);
    bytes[11] ^= 1;
    Check(OpenFsDecodeMetadataRecord(bytes, &metadata) == STATUS_CRC_ERROR,
        "metadata checksum corruption rejected");

    MakeMetadata(bytes, 1, 42, 1, 8192, 17);
    bytes[64] = 1;
    Seal(bytes, 76, 76);
    Check(OpenFsDecodeMetadataRecord(bytes, &metadata) == STATUS_DISK_CORRUPT_ERROR,
        "metadata reserved bytes rejected");

    MakeMetadata(bytes, 1, 0, 1, 1, 17);
    Check(OpenFsDecodeMetadataRecord(bytes, &metadata) == STATUS_DISK_CORRUPT_ERROR,
        "zero metadata object ID rejected");

    MakeMetadata(bytes, 1, 42, 42, 1, 17);
    Check(OpenFsDecodeMetadataRecord(bytes, &metadata) == STATUS_DISK_CORRUPT_ERROR,
        "self-parent metadata rejected");

    MakeMetadata(bytes, 2, 42, 1, 0, 17);
    Check(OpenFsDecodeMetadataRecord(bytes, &metadata) == STATUS_DISK_CORRUPT_ERROR,
        "directory metadata cannot own extents");

    MakeMetadata(bytes, 1, 42, 1, 0, 17);
    Check(OpenFsDecodeMetadataRecord(bytes, &metadata) == STATUS_DISK_CORRUPT_ERROR,
        "empty file metadata cannot own extents");

    MakeDirectoryEntry(bytes, fileName, (UCHAR)(sizeof(fileName) - 1), 1, 42);
    Check(OpenFsDecodeDirectoryEntry(bytes, &entry) == STATUS_SUCCESS,
        "directory entry decodes");
    Check(entry.ParentIdLow == 1 && entry.ObjectIdLow == 42 &&
        entry.NameLength == sizeof(fileName) - 1 &&
        memcmp(entry.Name, fileName, sizeof(fileName) - 1) == 0,
        "directory entry fields decoded");

    MakeDirectoryEntry(bytes, utf8Name, (UCHAR)sizeof(utf8Name), 1, 43);
    Check(OpenFsDecodeDirectoryEntry(bytes, &entry) == STATUS_SUCCESS,
        "valid multibyte UTF-8 name decodes");

    MakeDirectoryEntry(bytes, fileName, (UCHAR)(sizeof(fileName) - 1), 1, 42);
    bytes[44] = '/';
    Seal(bytes, 124, 124);
    Check(OpenFsDecodeDirectoryEntry(bytes, &entry) == STATUS_DISK_CORRUPT_ERROR,
        "path separator in name rejected");

    MakeDirectoryEntry(bytes, fileName, (UCHAR)(sizeof(fileName) - 1), 1, 42);
    bytes[44] = 0xC0;
    bytes[45] = 0xAF;
    Seal(bytes, 124, 124);
    Check(OpenFsDecodeDirectoryEntry(bytes, &entry) == STATUS_DISK_CORRUPT_ERROR,
        "overlong UTF-8 rejected");

    MakeDirectoryEntry(bytes, (const UCHAR*)"..", 2, 1, 42);
    Check(OpenFsDecodeDirectoryEntry(bytes, &entry) == STATUS_DISK_CORRUPT_ERROR,
        "dot-dot path component rejected");

    MakeDirectoryEntry(bytes, fileName, (UCHAR)(sizeof(fileName) - 1), 1, 42);
    bytes[44 + sizeof(fileName)] = 1;
    Seal(bytes, 124, 124);
    Check(OpenFsDecodeDirectoryEntry(bytes, &entry) == STATUS_DISK_CORRUPT_ERROR,
        "directory name padding rejected");

    MakeDirectoryEntry(bytes, fileName, (UCHAR)(sizeof(fileName) - 1), 1, 42);
    bytes[108] = 1;
    Seal(bytes, 124, 124);
    Check(OpenFsDecodeDirectoryEntry(bytes, &entry) == STATUS_DISK_CORRUPT_ERROR,
        "directory reserved bytes rejected");

    MakeDirectoryEntry(bytes, fileName, (UCHAR)(sizeof(fileName) - 1), 1, 42);
    bytes[11] = 0;
    Seal(bytes, 124, 124);
    Check(OpenFsDecodeDirectoryEntry(bytes, &entry) == STATUS_DISK_CORRUPT_ERROR,
        "empty directory name rejected");

    MakeDirectoryEntry(bytes, fileName, (UCHAR)(sizeof(fileName) - 1), 0, 42);
    Check(OpenFsDecodeDirectoryEntry(bytes, &entry) == STATUS_DISK_CORRUPT_ERROR,
        "zero directory parent ID rejected");

    MakeDirectoryEntry(bytes, fileName, (UCHAR)(sizeof(fileName) - 1), 42, 42);
    Check(OpenFsDecodeDirectoryEntry(bytes, &entry) == STATUS_DISK_CORRUPT_ERROR,
        "directory entry cannot reference its parent as itself");

    MakeDirectoryEntry(bytes, fileName, (UCHAR)(sizeof(fileName) - 1), 1, 42);
    bytes[12] ^= 1;
    Check(OpenFsDecodeDirectoryEntry(bytes, &entry) == STATUS_CRC_ERROR,
        "directory entry checksum corruption rejected");

    Check(OpenFsDecodeMetadataRecord(NULL, &metadata) == STATUS_INVALID_PARAMETER,
        "null metadata input rejected");
    Check(OpenFsDecodeDirectoryEntry(NULL, &entry) == STATUS_INVALID_PARAMETER,
        "null directory input rejected");


    {
        OPENFS_METADATA_RECORD records[3];
        OPENFS_DIRECTORY_ENTRY entries[2];
        memset(records, 0, sizeof(records));
        memset(entries, 0, sizeof(entries));
        records[0].ObjectIdLow = 1;
        records[0].Kind = 2;
        records[0].Generation = 1;
        records[1].ObjectIdLow = 42;
        records[1].ParentIdLow = 1;
        records[1].Kind = 1;
        records[1].Size = 8192;
        records[1].Generation = 1;
        records[1].HasExtent = TRUE;
        records[1].ExtentRootBlock = 17;
        records[2].ObjectIdLow = 43;
        records[2].ParentIdLow = 1;
        records[2].Kind = 1;
        records[2].Size = 4096;
        records[2].Generation = 1;
        records[2].HasExtent = TRUE;
        records[2].ExtentRootBlock = 18;
        entries[0].ParentIdLow = 1;
        entries[0].ObjectIdLow = 42;
        entries[0].Kind = 1;
        entries[0].NameLength = 8;
        memcpy(entries[0].Name, "file.txt", 8);
        entries[1].ParentIdLow = 1;
        entries[1].ObjectIdLow = 43;
        entries[1].Kind = 1;
        entries[1].NameLength = 9;
        memcpy(entries[1].Name, "other.txt", 9);
        Check(OpenFsValidateNamespaceRecords(records,3,entries,2,1,0,10,100)==STATUS_SUCCESS,
            "valid root and two-file namespace accepted");

        {
            POPENFS_NAMESPACE_SNAPSHOT snapshot = NULL;
            Check(OpenFsCreateNamespaceSnapshot(records, 3, entries, 2,
                1, 0, &snapshot) == STATUS_SUCCESS,
                "validated namespace snapshot created");
            Check(snapshot != NULL && snapshot->RecordCount == 3 &&
                snapshot->EntryCount == 2 && snapshot->RootRecordIndex == 0,
                "snapshot retains counts and root index");
            if (snapshot != NULL) {
                Check(snapshot->Records != records && snapshot->Entries != entries,
                    "snapshot owns independent copies");
                Check(snapshot->Records[1].ObjectIdLow == 42 &&
                    snapshot->Entries[0].NameLength == 8,
                    "snapshot copies decoded namespace content");
            }
            OpenFsFreeNamespaceSnapshot(snapshot);
            snapshot = NULL;
            Check(OpenFsCreateNamespaceSnapshot(records, 3, entries, 2,
                99, 0, &snapshot) == STATUS_DISK_CORRUPT_ERROR &&
                snapshot == NULL,
                "snapshot rejects a missing root record");
            Check(OpenFsCreateNamespaceSnapshot(NULL, 0, NULL, 0,
                0, 0, &snapshot) == STATUS_INVALID_PARAMETER,
                "snapshot rejects a zero root identifier");
            {
                ULONG foundIndex = MAXULONG;
                Check(OpenFsCreateNamespaceSnapshot(records, 3, entries, 2,
                    1, 0, &snapshot) == STATUS_SUCCESS,
                    "snapshot recreated for namespace lookup");
                Check(OpenFsLookupNamespaceChild(snapshot, 1, 0,
                    (const UCHAR*)"file.txt", 8, &foundIndex) == STATUS_SUCCESS &&
                    foundIndex == 1,
                    "namespace lookup resolves a root child");
                Check(OpenFsLookupNamespaceChild(snapshot, 1, 0,
                    (const UCHAR*)"File.txt", 8, &foundIndex) == STATUS_NOT_FOUND,
                    "namespace lookup preserves case-sensitive names");
                Check(OpenFsLookupNamespaceChild(snapshot, 42, 0,
                    (const UCHAR*)"missing", 7, &foundIndex) == STATUS_NOT_FOUND,
                    "namespace lookup refuses a non-directory parent");
                Check(OpenFsResolveNamespacePath(snapshot,
                    (const UCHAR*)"", 0, &foundIndex) == STATUS_SUCCESS &&
                    foundIndex == 0,
                    "empty path resolves to root");
                Check(OpenFsResolveNamespacePath(snapshot,
                    NULL, 0, &foundIndex) == STATUS_SUCCESS &&
                    foundIndex == 0,
                    "null zero-length path resolves to root");
                Check(OpenFsResolveNamespacePath(snapshot,
                    (const UCHAR*)"\\\\file.txt", 10, &foundIndex) == STATUS_SUCCESS &&
                    foundIndex == 1,
                    "root-relative Windows path resolves a file");
                Check(OpenFsResolveNamespacePath(snapshot,
                    (const UCHAR*)"\\\\/file.txt", 11, &foundIndex) == STATUS_SUCCESS &&
                    foundIndex == 1,
                    "mixed leading separators are normalized");
                Check(OpenFsResolveNamespacePath(snapshot,
                    (const UCHAR*)"/file.txt/", 10, &foundIndex) == STATUS_NOT_A_DIRECTORY,
                    "trailing separator requires a directory");
                Check(OpenFsResolveNamespacePath(snapshot,
                    (const UCHAR*)"missing", 7, &foundIndex) == STATUS_NOT_FOUND,
                    "path resolver reports missing component");
                Check(OpenFsResolveNamespacePath(snapshot,
                    (const UCHAR*)"file.txt\\child", 14, &foundIndex) == STATUS_NOT_A_DIRECTORY,
                    "path resolver rejects traversal through a file");
                Check(OpenFsResolveNamespacePath(snapshot,
                    (const UCHAR*)"..", 2, &foundIndex) == STATUS_OBJECT_PATH_SYNTAX_BAD,
                    "path resolver rejects parent traversal");
                Check(OpenFsResolveNamespacePath(snapshot,
                    (const UCHAR*)"toolong-component-name-that-exceeds-sixty-four-bytes-xxxxxxxxxxxxxxxx", 69,
                    &foundIndex) == STATUS_OBJECT_NAME_INVALID,
                    "path resolver rejects oversized components");
                OpenFsFreeNamespaceSnapshot(snapshot);
                snapshot = NULL;
            }
        }

        memcpy(entries[1].Name, "File.txt", 8);
        entries[1].NameLength = 8;
        Check(OpenFsValidateNamespaceRecords(records,3,entries,2,1,0,10,100)==STATUS_SUCCESS,
            "directory names remain case-sensitive");
        memcpy(entries[1].Name, "other.txt", 9);
        entries[1].NameLength = 9;

        memcpy(entries[1].Name, "file.txt", 8);
        entries[1].NameLength = 8;
        Check(OpenFsValidateNamespaceRecords(records,3,entries,2,1,0,10,100)==STATUS_DISK_CORRUPT_ERROR,
            "duplicate sibling names rejected");
        memcpy(entries[1].Name, "other.txt", 9);
        entries[1].NameLength = 9;

        records[2].ExtentRootBlock = 17;
        Check(OpenFsValidateNamespaceRecords(records,3,entries,2,1,0,10,100)==STATUS_DISK_CORRUPT_ERROR,
            "duplicate extent-root ownership rejected");
        records[2].ExtentRootBlock = 18;

        entries[1].ObjectIdLow = 99;
        Check(OpenFsValidateNamespaceRecords(records,3,entries,2,1,0,10,100)==STATUS_DISK_CORRUPT_ERROR,
            "directory entry to missing object rejected");
        entries[1].ObjectIdLow = 43;

        records[2].ParentIdLow = 77;
        Check(OpenFsValidateNamespaceRecords(records,3,entries,2,1,0,10,100)==STATUS_DISK_CORRUPT_ERROR,
            "orphan parent chain rejected");
        records[2].ParentIdLow = 1;

        records[2].ExtentRootBlock = 100;
        Check(OpenFsValidateNamespaceRecords(records,3,entries,2,1,0,10,100)==STATUS_DISK_CORRUPT_ERROR,
            "extent root outside data region rejected");
        records[2].ExtentRootBlock = 18;

        records[2].ObjectIdLow = 42;
        Check(OpenFsValidateNamespaceRecords(records,3,entries,2,1,0,10,100)==STATUS_DISK_CORRUPT_ERROR,
            "duplicate object ID rejected");
        records[2].ObjectIdLow = 0;
        Check(OpenFsValidateNamespaceRecords(records,3,entries,2,1,0,10,100)==STATUS_DISK_CORRUPT_ERROR,
            "zero object ID rejected");
        records[2].ObjectIdLow = 43;

        records[1].ParentIdLow = 42;
        Check(OpenFsValidateNamespaceRecords(records,3,entries,2,1,0,10,100)==STATUS_DISK_CORRUPT_ERROR,
            "namespace parent cycle rejected");
        records[1].ParentIdLow = 1;

        records[0].Kind = 1;
        Check(OpenFsValidateNamespaceRecords(records,3,entries,2,1,0,10,100)==STATUS_DISK_CORRUPT_ERROR,
            "root must be a directory");
    }

    printf("OpenFS namespace codec tests: %u checks, %u failures\n",
        g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
