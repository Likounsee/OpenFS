#ifndef OPENFS_SCRUB_H
#define OPENFS_SCRUB_H

#include <stdint.h>
#include "openfs/format.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OPENFS_SCRUB_OK = 0,
    OPENFS_SCRUB_INVALID_ARGUMENT = 1,
    OPENFS_SCRUB_CORRUPT = 2,
    OPENFS_SCRUB_IO_ERROR = 3
} openfs_scrub_result_t;

/*
 * Read-only integrity pass. It never repairs the filesystem. The pass first
 * runs the complete FSCK consistency verifier, then reads every on-disk block
 * so latent read failures cannot be mistaken for a clean filesystem.
 * errors receives the number of detected consistency/read failures.
 */
openfs_scrub_result_t openfs_scrub(openfs_block_device_t *,
                                   const openfs_superblock_t *,
                                   uint64_t *);

#ifdef __cplusplus
}
#endif

#endif
