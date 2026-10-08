#include "openfs/scrub.h"

#include <stdlib.h>
#include "openfs/fsck.h"

openfs_scrub_result_t openfs_scrub(openfs_block_device_t *d,
                                   const openfs_superblock_t *s,
                                   uint64_t *errors)
{
    if (errors == NULL || !openfs_block_device_is_valid(d) || s == NULL)
        return OPENFS_SCRUB_INVALID_ARGUMENT;

    *errors = 0U;

    uint64_t fsck_errors = 0U;
    openfs_fsck_result_t fr = openfs_fsck(d, s, &fsck_errors);
    if (fr != OPENFS_FSCK_OK) {
        *errors = fsck_errors == 0U ? 1U : fsck_errors;
        return fr == OPENFS_FSCK_IO_ERROR ? OPENFS_SCRUB_IO_ERROR
                                          : OPENFS_SCRUB_CORRUPT;
    }

    uint8_t *block = (uint8_t *)malloc(d->block_size);
    if (block == NULL)
        return OPENFS_SCRUB_IO_ERROR;

    for (uint64_t b = 0U; b < d->block_count; ++b) {
        if (d->read(d->context, b, 1U, block) != OPENFS_IO_OK) {
            free(block);
            *errors = 1U;
            return OPENFS_SCRUB_IO_ERROR;
        }
    }

    free(block);
    return OPENFS_SCRUB_OK;
}
