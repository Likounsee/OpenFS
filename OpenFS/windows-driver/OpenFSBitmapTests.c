#include <stdio.h>
#include <string.h>
#include "OpenFSBitmap.h"

static unsigned int g_tests;

#define CHECK(Condition) do { \
    ++g_tests; \
    if (!(Condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #Condition); \
        return 1; \
    } \
} while (0)

static void
SetBit(UCHAR* Bitmap, ULONGLONG Block, BOOLEAN Allocated)
{
    UCHAR mask = (UCHAR)(1u << (Block % 8u));
    if (Allocated) {
        Bitmap[Block / 8u] |= mask;
    } else {
        Bitmap[Block / 8u] &= (UCHAR)~mask;
    }
}

static void
SetAllAllocated(UCHAR* Bitmap, SIZE_T Bytes)
{
    memset(Bitmap, 0xff, Bytes);
}

int
main(void)
{
    /* One real bitmap page covers 32768 volume blocks. */
    UCHAR bitmap[4096];
    NTSTATUS status;

    SetAllAllocated(bitmap, sizeof(bitmap));
    for (ULONGLONG block = 6u; block < 49u; ++block) {
        SetBit(bitmap, block, FALSE);
    }

    status = OpenFsValidateBitmapBlock(
        bitmap, sizeof(bitmap), 0u, 50u, 2u, 2u, 4u, 6u);
    CHECK(status == STATUS_SUCCESS);

    SetBit(bitmap, 1u, FALSE);
    CHECK(OpenFsValidateBitmapBlock(
        bitmap, sizeof(bitmap), 0u, 50u, 2u, 2u, 4u, 6u)
        == STATUS_DISK_CORRUPT_ERROR);
    SetBit(bitmap, 1u, TRUE);

    SetBit(bitmap, 2u, FALSE);
    CHECK(OpenFsValidateBitmapBlock(
        bitmap, sizeof(bitmap), 0u, 50u, 2u, 2u, 4u, 6u)
        == STATUS_DISK_CORRUPT_ERROR);
    SetBit(bitmap, 2u, TRUE);

    SetBit(bitmap, 4u, FALSE);
    CHECK(OpenFsValidateBitmapBlock(
        bitmap, sizeof(bitmap), 0u, 50u, 2u, 2u, 4u, 6u)
        == STATUS_DISK_CORRUPT_ERROR);
    SetBit(bitmap, 4u, TRUE);

    SetBit(bitmap, 49u, FALSE);
    CHECK(OpenFsValidateBitmapBlock(
        bitmap, sizeof(bitmap), 0u, 50u, 2u, 2u, 4u, 6u)
        == STATUS_DISK_CORRUPT_ERROR);
    SetBit(bitmap, 49u, TRUE);

    SetBit(bitmap, 50u, FALSE);
    CHECK(OpenFsValidateBitmapBlock(
        bitmap, sizeof(bitmap), 0u, 50u, 2u, 2u, 4u, 6u)
        == STATUS_DISK_CORRUPT_ERROR);
    SetBit(bitmap, 50u, TRUE);

    /* Exercise reserved bits and padding on a second bitmap page. */
    SetAllAllocated(bitmap, sizeof(bitmap));
    for (ULONGLONG block = 21u; block < 32768u; ++block) {
        SetBit(bitmap, block, FALSE);
    }
    CHECK(OpenFsValidateBitmapBlock(
        bitmap, sizeof(bitmap), 0u, 33000u, 3u, 3u, 5u, 21u)
        == STATUS_SUCCESS);

    SetAllAllocated(bitmap, sizeof(bitmap));
    for (ULONGLONG block = 0u; block < 33000u - 32768u - 1u; ++block) {
        SetBit(bitmap, block, FALSE);
    }
    CHECK(OpenFsValidateBitmapBlock(
        bitmap, sizeof(bitmap), 32768u, 33000u, 3u, 3u, 5u, 21u)
        == STATUS_SUCCESS);
    SetBit(bitmap, 33000u - 32768u - 1u, FALSE);
    CHECK(OpenFsValidateBitmapBlock(
        bitmap, sizeof(bitmap), 32768u, 33000u, 3u, 3u, 5u, 21u)
        == STATUS_DISK_CORRUPT_ERROR);
    SetBit(bitmap, 33000u - 32768u - 1u, TRUE);
    SetBit(bitmap, 33000u - 32768u, FALSE);
    CHECK(OpenFsValidateBitmapBlock(
        bitmap, sizeof(bitmap), 32768u, 33000u, 3u, 3u, 5u, 21u)
        == STATUS_DISK_CORRUPT_ERROR);
    SetBit(bitmap, 33000u - 32768u, TRUE);

    CHECK(OpenFsValidateBitmapBlock(
        bitmap, sizeof(bitmap), 0u, 50u, 2u, 3u, 4u, 6u)
        == STATUS_INVALID_PARAMETER);
    CHECK(OpenFsValidateBitmapBlock(
        NULL, sizeof(bitmap), 0u, 50u, 2u, 2u, 4u, 6u)
        == STATUS_INVALID_PARAMETER);


    /* Count free bits only in the requested data range. */
    SetAllAllocated(bitmap, sizeof(bitmap));
    for (ULONGLONG block = 100u; block < 120u; ++block)
        SetBit(bitmap, block, FALSE);
    {
        ULONGLONG freeBlocks = 999u;
        CHECK(OpenFsCountFreeBlocksInRange(bitmap, sizeof(bitmap),
            0u, 96u, 128u, &freeBlocks) == STATUS_SUCCESS);
        CHECK(freeBlocks == 20u);
        CHECK(OpenFsCountFreeBlocksInRange(bitmap, sizeof(bitmap),
            0u, 32760u, 32777u, &freeBlocks) == STATUS_INVALID_PARAMETER);
        CHECK(OpenFsCountFreeBlocksInRange(bitmap, sizeof(bitmap),
            0u, 120u, 100u, &freeBlocks) == STATUS_INVALID_PARAMETER);
        CHECK(OpenFsCountFreeBlocksInRange(bitmap, sizeof(bitmap),
            0u, 96u, 128u, NULL) == STATUS_INVALID_PARAMETER);
        CHECK(OpenFsCountFreeBlocksInRange(bitmap, sizeof(bitmap),
            MAXULONGLONG - 7u, MAXULONGLONG - 7u,
            MAXULONGLONG, &freeBlocks) == STATUS_INTEGER_OVERFLOW);
    }


    /* Byte-wise popcount must preserve partial first/last-byte boundaries. */
    SetAllAllocated(bitmap, sizeof(bitmap));
    {
        ULONGLONG freeBlocks = 0;
        for (ULONGLONG block = 7u; block < 18u; ++block)
            SetBit(bitmap, block, FALSE);
        CHECK(OpenFsCountFreeBlocksInRange(bitmap, sizeof(bitmap),
            0u, 5u, 21u, &freeBlocks) == STATUS_SUCCESS);
        CHECK(freeBlocks == 11u);
        CHECK(OpenFsCountFreeBlocksInRange(bitmap, sizeof(bitmap),
            0u, 8u, 16u, &freeBlocks) == STATUS_SUCCESS);
        CHECK(freeBlocks == 8u);
    }

    /* A completely free bitmap page is counted without per-block iteration. */
    memset(bitmap, 0, sizeof(bitmap));
    {
        ULONGLONG freeBlocks = 0;
        CHECK(OpenFsCountFreeBlocksInRange(bitmap, sizeof(bitmap),
            0u, 0u, 32768u, &freeBlocks) == STATUS_SUCCESS);
        CHECK(freeBlocks == 32768u);
    }


    /* Bit copies preserve unrelated edge bits in the destination buffer. */
    {
        UCHAR source[4] = { 0x96u, 0x3cu, 0xa5u, 0x5au };
        UCHAR destination[4];
        memset(destination, 0xff, sizeof(destination));
        CHECK(OpenFsCopyBitmapBits(source, sizeof(source), 3u, 13u,
            destination, sizeof(destination), 5u) == STATUS_SUCCESS);
        for (ULONGLONG bit = 0; bit < 32u; ++bit) {
            BOOLEAN expected = bit >= 5u && bit < 18u
                ? ((source[(bit - 5u + 3u) / 8u] &
                    (UCHAR)(1u << ((bit - 5u + 3u) % 8u))) != 0)
                : TRUE;
            BOOLEAN actual = (destination[bit / 8u] &
                (UCHAR)(1u << (bit % 8u))) != 0;
            CHECK(actual == expected);
        }

        /* Simulate two adjacent on-disk bitmap pages feeding one output. */
        {
            UCHAR pageA[2] = { 0xacu, 0x53u };
            UCHAR pageB[2] = { 0x69u, 0xc6u };
            UCHAR joined[3] = { 0u, 0u, 0u };
            CHECK(OpenFsCopyBitmapBits(pageA, sizeof(pageA), 5u, 11u,
                joined, sizeof(joined), 0u) == STATUS_SUCCESS);
            CHECK(OpenFsCopyBitmapBits(pageB, sizeof(pageB), 0u, 7u,
                joined, sizeof(joined), 11u) == STATUS_SUCCESS);
            for (ULONGLONG bit = 0; bit < 18u; ++bit) {
                BOOLEAN expected = bit < 11u
                    ? ((pageA[(bit + 5u) / 8u] &
                        (UCHAR)(1u << ((bit + 5u) % 8u))) != 0)
                    : ((pageB[(bit - 11u) / 8u] &
                        (UCHAR)(1u << ((bit - 11u) % 8u))) != 0);
                BOOLEAN actual = (joined[bit / 8u] &
                    (UCHAR)(1u << (bit % 8u))) != 0;
                CHECK(actual == expected);
            }
            CHECK((joined[2] & 0xfcu) == 0u);
        }

        memset(destination, 0, sizeof(destination));
        CHECK(OpenFsCopyBitmapBits(source, sizeof(source), 0u, 32u,
            destination, sizeof(destination), 0u) == STATUS_SUCCESS);
        CHECK(memcmp(source, destination, sizeof(source)) == 0);
        CHECK(OpenFsCopyBitmapBits(source, sizeof(source), 31u, 2u,
            destination, sizeof(destination), 0u) == STATUS_BUFFER_TOO_SMALL);
        CHECK(OpenFsCopyBitmapBits(source, sizeof(source), 0u, 2u,
            destination, sizeof(destination), 31u) == STATUS_BUFFER_TOO_SMALL);
        CHECK(OpenFsCopyBitmapBits(NULL, sizeof(source), 0u, 1u,
            destination, sizeof(destination), 0u) == STATUS_INVALID_PARAMETER);
    }

    puts("OpenFS bitmap validation tests passed.");
    printf("%u checks passed.\n", g_tests);
    return 0;
}
