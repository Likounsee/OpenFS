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
    UCHAR bitmap[8];
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

    CHECK(OpenFsValidateBitmapBlock(
        bitmap, sizeof(bitmap), 0u, 50u, 2u, 3u, 4u, 6u)
        == STATUS_INVALID_PARAMETER);
    CHECK(OpenFsValidateBitmapBlock(
        NULL, sizeof(bitmap), 0u, 50u, 2u, 2u, 4u, 6u)
        == STATUS_INVALID_PARAMETER);

    puts("OpenFS bitmap validation tests passed.");
    printf("%u checks passed.\n", g_tests);
    return 0;
}
