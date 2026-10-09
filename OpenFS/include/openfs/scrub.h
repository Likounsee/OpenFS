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
 * errors receives the number of detected consistency/read/checksum failures.
 */
openfs_scrub_result_t openfs_scrub(openfs_block_device_t *,
                                   const openfs_superblock_t *,
                                   uint64_t *);

/*
 * Optional detailed scrub reporting. The callback is invoked for each issue
 * that can be localized by the scrub pass. block is the physical block for
 * read/checksum issues; UINT64_MAX denotes a filesystem-wide FSCK failure
 * whose exact block is not exposed by FSCK. issue is one of the constants
 * below. The callback must not mutate the device while it is being scrubbed.
 * Passing a NULL callback is equivalent to openfs_scrub().
 */
#define OPENFS_SCRUB_ISSUE_FILESYSTEM 1U
#define OPENFS_SCRUB_ISSUE_READ       2U
#define OPENFS_SCRUB_ISSUE_CHECKSUM   3U

typedef void (*openfs_scrub_issue_callback_t)(void *context,
                                              uint64_t block,
                                              uint32_t issue);

openfs_scrub_result_t openfs_scrub_ex(openfs_block_device_t *,
                                      const openfs_superblock_t *,
                                      uint64_t *,
                                      openfs_scrub_issue_callback_t,
                                      void *);

#ifdef __cplusplus
}
#endif

#endif
