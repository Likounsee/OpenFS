#include "openfs/scrub.h"

#include <stdlib.h>
#include <stdint.h>
#include "openfs/fsck.h"
#include "openfs/bitmap.h"
#include "openfs/data_checksum.h"

static void report_issue(openfs_scrub_issue_callback_t callback,
                         void *context,
                         uint64_t block,
                         uint32_t issue)
{
    if (callback != NULL) callback(context, block, issue);
}

openfs_scrub_result_t openfs_scrub_ex(openfs_block_device_t *d,
                                      const openfs_superblock_t *s,
                                      uint64_t *errors,
                                      openfs_scrub_issue_callback_t callback,
                                      void *context)
{
    if (errors == NULL || !openfs_block_device_is_valid(d) || s == NULL)
        return OPENFS_SCRUB_INVALID_ARGUMENT;

    *errors = 0U;

    uint8_t *block = (uint8_t *)malloc(d->block_size);
    if (block == NULL)
        return OPENFS_SCRUB_IO_ERROR;

    int saw_io_error = 0;
    for (uint64_t b = 0U; b < d->block_count; ++b) {
        int block_read_ok = 1;
        if (d->read(d->context, b, 1U, block) != OPENFS_IO_OK) {
            block_read_ok = 0;
            saw_io_error = 1;
            (*errors)++;
            report_issue(callback, context, b, OPENFS_SCRUB_ISSUE_READ);
        }

        if (block_read_ok &&
            (s->feature_flags & OPENFS_FEATURE_DATA_CHECKSUM) != 0U &&
            b >= s->data_start && b - s->data_start < s->data_blocks &&
            b != s->metadata_root_block) {
            int used = 0;
            openfs_bitmap_result_t br = openfs_bitmap_test(
                d, s->block_bitmap_start, s->block_bitmap_blocks, b, &used);
            if (br != OPENFS_BITMAP_OK) {
                saw_io_error = 1;
                (*errors)++;
                report_issue(callback, context, b, OPENFS_SCRUB_ISSUE_READ);
                continue;
            }
            if (used) {
                uint32_t expected = 0U;
                if (openfs_data_checksum_get(d, s, b, &expected) != 0) {
                    saw_io_error = 1;
                    (*errors)++;
                    report_issue(callback, context, b, OPENFS_SCRUB_ISSUE_READ);
                    continue;
                }
                if (expected != openfs_data_checksum(block, d->block_size)) {
                    (*errors)++;
                    report_issue(callback, context, b,
                                 OPENFS_SCRUB_ISSUE_CHECKSUM);
                }
            }
        }
    }
    free(block);

    /*
     * Check data checksums before fsck: fsck also detects checksum mismatches,
     * but its summary cannot identify the damaged data block for the detailed
     * scrub callback. The scan above can report that precise location and
     * continue collecting independent checksum/read failures.
     */
    if (saw_io_error) return OPENFS_SCRUB_IO_ERROR;
    if (*errors != 0U) return OPENFS_SCRUB_CORRUPT;

    uint64_t fsck_errors = 0U;
    openfs_fsck_result_t fr = openfs_fsck(d, s, &fsck_errors);
    if (fr != OPENFS_FSCK_OK) {
        *errors = fsck_errors == 0U ? 1U : fsck_errors;
        report_issue(callback, context, UINT64_MAX,
                     OPENFS_SCRUB_ISSUE_FILESYSTEM);
        return fr == OPENFS_FSCK_IO_ERROR ? OPENFS_SCRUB_IO_ERROR
                                          : OPENFS_SCRUB_CORRUPT;
    }
    return OPENFS_SCRUB_OK;
}

openfs_scrub_result_t openfs_scrub(openfs_block_device_t *d,
                                   const openfs_superblock_t *s,
                                   uint64_t *errors)
{
    return openfs_scrub_ex(d, s, errors, NULL, NULL);
}
