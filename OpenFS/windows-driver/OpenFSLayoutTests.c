#include <stdio.h>
#include "OpenFSLayout.h"

static unsigned int g_tests;

#define CHECK(Condition) do { \
    ++g_tests; \
    if (!(Condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\\n", __FILE__, __LINE__, #Condition); \
        return 1; \
    } \
} while (0)

int
main(void)
{
    OPENFS_VOLUME_LAYOUT layout;

    CHECK(OpenFsCalculateVolumeLayout(4096u, 50u, 4u, &layout) == STATUS_SUCCESS);
    CHECK(layout.BitmapStartBlock == 1u);
    CHECK(layout.BitmapBlocks == 1u);
    CHECK(layout.BitmapEndBlock == 2u);
    CHECK(layout.JournalStartBlock == 2u);
    CHECK(layout.CheckpointA == 2u && layout.CheckpointB == 3u);
    CHECK(layout.RingStartBlock == 4u && layout.RingEndBlock == 20u);
    CHECK(layout.DataStartBlock == 20u);
    CHECK(layout.BackupSuperblockBlock == 49u);

    CHECK(OpenFsCalculateVolumeLayout(4096u, 50u, 2u, &layout) == STATUS_SUCCESS);
    CHECK(layout.RingEndBlock == 8u);
    CHECK(layout.DataStartBlock == 8u);

    CHECK(OpenFsCalculateVolumeLayout(4096u, 50u, 3u, &layout) == STATUS_SUCCESS);
    CHECK(layout.RingEndBlock == 8u);

    CHECK(OpenFsCalculateVolumeLayout(4096u, 50u, 5u, &layout) == STATUS_SUCCESS);
    CHECK(layout.RingEndBlock == 20u);
    CHECK(layout.DataStartBlock == 20u);

    /* A second bitmap page must extend the data region without shifting its base. */
    CHECK(OpenFsCalculateVolumeLayout(4096u, 33000u, 4u, &layout) == STATUS_SUCCESS);
    CHECK(layout.BitmapBlocks == 2u);
    CHECK(layout.BitmapEndBlock == 3u);
    CHECK(layout.JournalStartBlock == 3u);
    CHECK(layout.RingStartBlock == 5u && layout.RingEndBlock == 21u);
    CHECK(layout.DataStartBlock == 21u);
    CHECK(layout.BackupSuperblockBlock == 32999u);

    CHECK(OpenFsCalculateVolumeLayout(2048u, 50u, 4u, &layout)
        == STATUS_DISK_CORRUPT_ERROR);
    CHECK(OpenFsCalculateVolumeLayout(4096u, 50u, 1u, &layout)
        == STATUS_DISK_CORRUPT_ERROR);
    CHECK(OpenFsCalculateVolumeLayout(4096u, 12u, 4u, &layout)
        == STATUS_DISK_CORRUPT_ERROR);
    CHECK(OpenFsCalculateVolumeLayout(4096u, MAXULONGLONG / 4096u + 1u, 4u, &layout)
        == STATUS_INTEGER_OVERFLOW);
    CHECK(OpenFsCalculateVolumeLayout(4096u, 50u, 4u, NULL)
        == STATUS_INVALID_PARAMETER);

    /* Read ranges are half-open and must fit both the device and IRP offset. */
    CHECK(OpenFsValidateReadRange(4096u, 0u, 4096u) == STATUS_SUCCESS);
    CHECK(OpenFsValidateReadRange(4096u, 4096u, 1u)
        == STATUS_INVALID_PARAMETER);
    CHECK(OpenFsValidateReadRange(4096u, 4095u, 1u) == STATUS_SUCCESS);
    CHECK(OpenFsValidateReadRange(4096u, 4095u, 2u)
        == STATUS_INVALID_PARAMETER);
    CHECK(OpenFsValidateReadRange(4096u, 0u, 0u)
        == STATUS_INVALID_PARAMETER);
    CHECK(OpenFsValidateReadRange(MAXLONGLONG, (ULONGLONG)MAXLONGLONG, 1u)
        == STATUS_INTEGER_OVERFLOW);

    puts("OpenFS volume layout tests passed.");
    printf("%u checks passed.\\n", g_tests);
    return 0;
}
