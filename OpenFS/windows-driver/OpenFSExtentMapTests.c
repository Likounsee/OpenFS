#include <stdio.h>
#include "OpenFSExtentMap.h"

#define TEST_BLOCK_SIZE 4096u

static ULONG g_Checks;
static ULONG g_Failures;

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
WriteU16Le(UCHAR* Bytes, USHORT Value)
{
    Bytes[0] = (UCHAR)Value;
    Bytes[1] = (UCHAR)(Value >> 8);
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
    WriteU32Le(Bytes, (ULONG)Value);
    WriteU32Le(Bytes + 4, (ULONG)(Value >> 32));
}

static void
WriteExtent(UCHAR* Bytes, ULONG Index,
    ULONGLONG LogicalStart, ULONGLONG PhysicalStart, ULONGLONG BlockCount)
{
    ULONG offset = OPENFS_EXTENT_MAP_HEADER_BYTES +
        Index * OPENFS_EXTENT_MAP_ENTRY_BYTES;
    WriteU64Le(Bytes + offset, LogicalStart);
    WriteU64Le(Bytes + offset + 8, PhysicalStart);
    WriteU64Le(Bytes + offset + 16, BlockCount);
}

static void
FinalizeBlock(UCHAR* Bytes)
{
    WriteU32Le(Bytes + TEST_BLOCK_SIZE - 4,
        OpenFsCrc32C(Bytes, TEST_BLOCK_SIZE - 4));
}

static void
MakeBlock(UCHAR* Bytes, ULONG Count)
{
    RtlZeroMemory(Bytes, TEST_BLOCK_SIZE);
    RtlCopyMemory(Bytes, OPENFS_EXTENT_MAP_MAGIC, 8);
    WriteU16Le(Bytes + 8, 1);
    WriteU16Le(Bytes + 10, OPENFS_EXTENT_MAP_HEADER_BYTES);
    WriteU32Le(Bytes + 12, Count);
    if (Count > 0)
        WriteExtent(Bytes, 0, 0, 30, 2);
    if (Count > 1)
        WriteExtent(Bytes, 1, 5, 40, 1);
    FinalizeBlock(Bytes);
}

static NTSTATUS
Decode(const UCHAR* Bytes, POPENFS_EXTENT Extents,
    ULONG Capacity, PULONG Count, ULONGLONG FileSize)
{
    return OpenFsDecodeExtentMapBlock(Bytes, TEST_BLOCK_SIZE, FileSize,
        20, 100, 101, Extents, Capacity, Count);
}

int
main(void)
{
    UCHAR bytes[TEST_BLOCK_SIZE];
    OPENFS_EXTENT extents[4];
    ULONG count = 99;
    NTSTATUS status;
    ULONGLONG physical;
    BOOLEAN allocated;

    MakeBlock(bytes, 2);
    status = Decode(bytes, extents, 4, &count, 6u * TEST_BLOCK_SIZE);
    Check(status == STATUS_SUCCESS && count == 2,
        "valid sparse extent map decodes");
    Check(extents[0].LogicalStart == 0 &&
        extents[0].PhysicalStart == 30 && extents[0].BlockCount == 2 &&
        extents[1].LogicalStart == 5 &&
        extents[1].PhysicalStart == 40 && extents[1].BlockCount == 1,
        "decoded extent fields are preserved");

    count = 99;
    Check(Decode(bytes, extents, 1, &count, 6u * TEST_BLOCK_SIZE) ==
        STATUS_BUFFER_TOO_SMALL && count == 2,
        "insufficient capacity reports required extent count without copying");

    count = 0;
    Check(OpenFsDecodeExtentMapBlock(bytes, TEST_BLOCK_SIZE,
        6u * TEST_BLOCK_SIZE, 20, 100, 101, NULL, 0, &count) ==
        STATUS_BUFFER_TOO_SMALL && count == 2,
        "null output sizing pass reports validated extent count");

    MakeBlock(bytes, 2);
    WriteExtent(bytes, 1, 1, 40, 1);
    FinalizeBlock(bytes);
    Check(Decode(bytes, extents, 4, &count, 6u * TEST_BLOCK_SIZE) ==
        STATUS_DISK_CORRUPT_ERROR,
        "logical extent overlap is rejected");

    MakeBlock(bytes, 2);
    WriteExtent(bytes, 1, 5, 31, 1);
    FinalizeBlock(bytes);
    Check(Decode(bytes, extents, 4, &count, 6u * TEST_BLOCK_SIZE) ==
        STATUS_DISK_CORRUPT_ERROR,
        "physical extent alias is rejected");

    MakeBlock(bytes, 1);
    WriteExtent(bytes, 0, 0, 99, 2);
    FinalizeBlock(bytes);
    Check(Decode(bytes, extents, 4, &count, 2u * TEST_BLOCK_SIZE) ==
        STATUS_DISK_CORRUPT_ERROR,
        "extent crossing backup superblock is rejected");

    MakeBlock(bytes, 1);
    WriteExtent(bytes, 0, 2, 30, 1);
    FinalizeBlock(bytes);
    Check(Decode(bytes, extents, 4, &count, 2u * TEST_BLOCK_SIZE) ==
        STATUS_DISK_CORRUPT_ERROR,
        "extent beyond EOF is rejected");

    MakeBlock(bytes, 1);
    WriteExtent(bytes, 0, 0, 30, 0);
    FinalizeBlock(bytes);
    Check(Decode(bytes, extents, 4, &count, TEST_BLOCK_SIZE) ==
        STATUS_DISK_CORRUPT_ERROR,
        "zero-length extent is rejected");

    MakeBlock(bytes, 1);
    bytes[TEST_BLOCK_SIZE - 8] = 1;
    FinalizeBlock(bytes);
    Check(Decode(bytes, extents, 4, &count, TEST_BLOCK_SIZE) ==
        STATUS_DISK_CORRUPT_ERROR,
        "nonzero padding is rejected");

    MakeBlock(bytes, 1);
    bytes[100] ^= 0x80;
    Check(Decode(bytes, extents, 4, &count, TEST_BLOCK_SIZE) ==
        STATUS_CRC_ERROR,
        "checksum corruption is rejected");

    MakeBlock(bytes, 0);
    bytes[0] ^= 0x01;
    FinalizeBlock(bytes);
    Check(Decode(bytes, extents, 4, &count, 0) ==
        STATUS_DISK_CORRUPT_ERROR,
        "invalid extent-map magic is rejected");

    MakeBlock(bytes, 0);
    Check(Decode(bytes, extents, 0, &count, 0) == STATUS_SUCCESS && count == 0,
        "empty sparse file accepts an empty extent map");

    MakeBlock(bytes, 2);
    Check(Decode(bytes, extents, 4, &count, 6u * TEST_BLOCK_SIZE) ==
        STATUS_SUCCESS, "extent map reloads before logical lookup tests");
    Check(OpenFsMapLogicalBlock(extents, count, 0, &physical, &allocated) ==
        STATUS_SUCCESS && allocated && physical == 30,
        "logical block maps to first physical block");
    Check(OpenFsMapLogicalBlock(extents, count, 1, &physical, &allocated) ==
        STATUS_SUCCESS && allocated && physical == 31,
        "logical block maps within a contiguous run");
    Check(OpenFsMapLogicalBlock(extents, count, 2, &physical, &allocated) ==
        STATUS_SUCCESS && !allocated && physical == 0,
        "sparse hole is reported without a physical address");
    Check(OpenFsMapLogicalBlock(extents, count, 5, &physical, &allocated) ==
        STATUS_SUCCESS && allocated && physical == 40,
        "logical block maps to a later fragmented run");
    Check(OpenFsMapLogicalBlock(extents, count, 6, &physical, &allocated) ==
        STATUS_SUCCESS && !allocated,
        "logical block after final extent is a hole");
    Check(OpenFsMapLogicalBlock(NULL, 0, 0, &physical, &allocated) ==
        STATUS_SUCCESS && !allocated,
        "empty extent map maps every logical block as sparse");

    printf("OpenFS extent-map tests: %lu checks, %lu failures\n",
        (unsigned long)g_Checks, (unsigned long)g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
