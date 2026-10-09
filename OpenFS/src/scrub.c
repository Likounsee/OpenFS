#include "openfs/scrub.h"

#include <stdlib.h>
#include "openfs/fsck.h"
#include "openfs/bitmap.h"
#include "openfs/data_checksum.h"

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
        if (d->read(d->context, b, 1U, block) != OPENFS_IO_OK) { free(block); *errors = 1U; return OPENFS_SCRUB_IO_ERROR; }
        if ((s->feature_flags & OPENFS_FEATURE_DATA_CHECKSUM) != 0U && b >= s->data_start && b - s->data_start < s->data_blocks && b != s->metadata_root_block) { int used=0; if(openfs_bitmap_test(d,s->block_bitmap_start,s->block_bitmap_blocks,b,&used)!=OPENFS_BITMAP_OK){free(block);*errors=1U;return OPENFS_SCRUB_IO_ERROR;} if(used){uint32_t expected=0U;if(openfs_data_checksum_get(d,s,b,&expected)!=0||expected!=openfs_data_checksum(block,d->block_size)){(*errors)++;}} }
    }

    free(block);
    return *errors == 0U ? OPENFS_SCRUB_OK : OPENFS_SCRUB_CORRUPT;
}
