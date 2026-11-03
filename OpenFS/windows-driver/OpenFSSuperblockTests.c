#include "OpenFSSuperblock.h"
#include <stdio.h>
#include <stdlib.h>

#define TEST_DEVICE_BLOCKS 128u
#define TEST_BLOCK_SIZE 4096u
#define TEST_DEVICE_BYTES ((ULONGLONG)TEST_DEVICE_BLOCKS * TEST_BLOCK_SIZE)

static unsigned int g_tests = 0;

#define CHECK(Condition) do { \
    ++g_tests; \
    if (!(Condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #Condition); \
        return 1; \
    } \
} while (0)

static void
WriteU16(UCHAR* Bytes, SIZE_T Offset, USHORT Value)
{
    Bytes[Offset] = (UCHAR)Value;
    Bytes[Offset + 1] = (UCHAR)(Value >> 8);
}

static void
WriteU32(UCHAR* Bytes, SIZE_T Offset, ULONG Value)
{
    Bytes[Offset] = (UCHAR)Value;
    Bytes[Offset + 1] = (UCHAR)(Value >> 8);
    Bytes[Offset + 2] = (UCHAR)(Value >> 16);
    Bytes[Offset + 3] = (UCHAR)(Value >> 24);
}

static void
WriteU64(UCHAR* Bytes, SIZE_T Offset, ULONGLONG Value)
{
    WriteU32(Bytes, Offset, (ULONG)Value);
    WriteU32(Bytes, Offset + 4, (ULONG)(Value >> 32));
}

static void
Seal(UCHAR* Bytes)
{
    WriteU32(Bytes, OPENFS_SUPERBLOCK_CHECKSUM_OFFSET,
        OpenFsCrc32C(Bytes, OPENFS_SUPERBLOCK_CHECKSUM_OFFSET));
}

static void
MakeSuperblock(UCHAR* Bytes, USHORT Version, ULONGLONG TotalBlocks)
{
    SIZE_T i;
    memset(Bytes, 0, OPENFS_SUPERBLOCK_BYTES);
    memcpy(Bytes, "OPENFS\0\0", 8);
    WriteU16(Bytes, 8, Version);
    WriteU16(Bytes, 10, 48);
    WriteU32(Bytes, 12, TEST_BLOCK_SIZE);
    WriteU64(Bytes, 16, TotalBlocks);
    WriteU64(Bytes, 24, 0x0102030405060708ULL);
    for (i = 0; i < 16; ++i) {
        Bytes[32 + i] = (UCHAR)i;
    }
    Seal(Bytes);
}

static int
TestCrc32cKnownVector(void)
{
    static const UCHAR input[] = "123456789";
    CHECK(OpenFsCrc32C(input, sizeof(input) - 1) == 0xE3069283u);
    return 0;
}

static int
TestRejectsUnsupportedVersionAndHeader(void)
{
    UCHAR primary[OPENFS_SUPERBLOCK_BYTES];
    UCHAR backup[OPENFS_SUPERBLOCK_BYTES];
    OPENFS_VOLUME_GEOMETRY geometry;

    MakeSuperblock(primary, 5, TEST_DEVICE_BLOCKS);
    memcpy(backup, primary, sizeof(backup));
    CHECK(OpenFsValidateSuperblocks(primary, backup, TEST_DEVICE_BYTES, &geometry)
        == STATUS_UNRECOGNIZED_VOLUME);

    MakeSuperblock(primary, 4, TEST_DEVICE_BLOCKS);
    memcpy(backup, primary, sizeof(backup));
    WriteU16(primary, 10, 64);
    Seal(primary);
    CHECK(OpenFsValidateSuperblocks(primary, backup, TEST_DEVICE_BYTES, &geometry)
        == STATUS_UNRECOGNIZED_VOLUME);
    return 0;
}

static int
TestValidRevisions(void)
{
    UCHAR primary[OPENFS_SUPERBLOCK_BYTES];
    UCHAR backup[OPENFS_SUPERBLOCK_BYTES];
    OPENFS_VOLUME_GEOMETRY geometry;
    USHORT version;

    for (version = 2; version <= 4; ++version) {
        MakeSuperblock(primary, version, TEST_DEVICE_BLOCKS);
        memcpy(backup, primary, sizeof(backup));
        if (version >= 3) {
            WriteU64(primary, 48, 0x1111222233334444ULL);
            WriteU64(backup, 48, 0xAAAABBBBCCCCDDDDULL);
            Seal(primary);
            Seal(backup);
        }
        CHECK(OpenFsValidateSuperblocks(primary, backup, TEST_DEVICE_BYTES, &geometry)
            == STATUS_SUCCESS);
        CHECK(geometry.Version == version);
        CHECK(geometry.BlockSize == TEST_BLOCK_SIZE);
        CHECK(geometry.TotalBlocks == TEST_DEVICE_BLOCKS);
        CHECK(geometry.Features == 0x0102030405060708ULL);
        CHECK(geometry.Uuid[15] == 15);
    }
    return 0;
}

static int
TestRejectsCorruption(void)
{
    UCHAR primary[OPENFS_SUPERBLOCK_BYTES];
    UCHAR backup[OPENFS_SUPERBLOCK_BYTES];
    OPENFS_VOLUME_GEOMETRY geometry;

    MakeSuperblock(primary, 4, TEST_DEVICE_BLOCKS);
    memcpy(backup, primary, sizeof(backup));
    backup[100] ^= 0x80;
    CHECK(OpenFsValidateSuperblocks(primary, backup, TEST_DEVICE_BYTES, &geometry)
        == STATUS_CRC_ERROR);

    MakeSuperblock(primary, 4, TEST_DEVICE_BLOCKS);
    memcpy(backup, primary, sizeof(backup));
    WriteU64(backup, 24, 0x9988776655443322ULL);
    Seal(backup);
    CHECK(OpenFsValidateSuperblocks(primary, backup, TEST_DEVICE_BYTES, &geometry)
        == STATUS_DISK_CORRUPT_ERROR);

    MakeSuperblock(primary, 2, TEST_DEVICE_BLOCKS);
    memcpy(backup, primary, sizeof(backup));
    primary[48] = 1;
    Seal(primary);
    CHECK(OpenFsValidateSuperblocks(primary, backup, TEST_DEVICE_BYTES, &geometry)
        == STATUS_DISK_CORRUPT_ERROR);

    MakeSuperblock(primary, 4, TEST_DEVICE_BLOCKS);
    memcpy(backup, primary, sizeof(backup));
    CHECK(OpenFsValidateSuperblocks(primary, backup, TEST_DEVICE_BYTES - TEST_BLOCK_SIZE,
        &geometry) == STATUS_DISK_CORRUPT_ERROR);

    MakeSuperblock(primary, 4, 12);
    memcpy(backup, primary, sizeof(backup));
    CHECK(OpenFsValidateSuperblocks(primary, backup, 12ULL * TEST_BLOCK_SIZE, &geometry)
        == STATUS_DISK_CORRUPT_ERROR);
    return 0;
}

static int
TestRejectsBadGeometryAndMagic(void)
{
    UCHAR primary[OPENFS_SUPERBLOCK_BYTES];
    UCHAR backup[OPENFS_SUPERBLOCK_BYTES];
    OPENFS_VOLUME_GEOMETRY geometry;

    MakeSuperblock(primary, 4, TEST_DEVICE_BLOCKS);
    memcpy(backup, primary, sizeof(backup));
    primary[0] = 'X';
    Seal(primary);
    CHECK(OpenFsValidateSuperblocks(primary, backup, TEST_DEVICE_BYTES, &geometry)
        == STATUS_UNRECOGNIZED_VOLUME);

    MakeSuperblock(primary, 4, TEST_DEVICE_BLOCKS);
    memcpy(backup, primary, sizeof(backup));
    WriteU32(primary, 12, 6000);
    Seal(primary);
    CHECK(OpenFsValidateSuperblocks(primary, backup, TEST_DEVICE_BYTES, &geometry)
        == STATUS_DISK_CORRUPT_ERROR);

    MakeSuperblock(primary, 4, UINT64_MAX);
    memcpy(backup, primary, sizeof(backup));
    CHECK(OpenFsValidateSuperblocks(primary, backup, TEST_DEVICE_BYTES, &geometry)
        == STATUS_INTEGER_OVERFLOW);
    return 0;
}

int
main(void)
{
    CHECK(TestCrc32cKnownVector() == 0);
    CHECK(TestRejectsUnsupportedVersionAndHeader() == 0);
    CHECK(TestValidRevisions() == 0);
    CHECK(TestRejectsCorruption() == 0);
    CHECK(TestRejectsBadGeometryAndMagic() == 0);
    printf("OpenFS superblock host tests: %u checks passed\n", g_tests);
    return 0;
}
