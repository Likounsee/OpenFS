#ifndef OPENFS_FSCK_H
#define OPENFS_FSCK_H
#include <stdint.h>
#include "openfs/mount.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { OPENFS_FSCK_OK=0, OPENFS_FSCK_INVALID_ARGUMENT=1, OPENFS_FSCK_CORRUPT=2, OPENFS_FSCK_IO_ERROR=3 } openfs_fsck_result_t;
typedef void (*openfs_fsck_progress_callback_t)(void *,uint64_t,uint64_t,const char *);
typedef struct openfs_fsck_diagnostic {
    const char *stage;
    const char *reason;
    uint64_t index;
    uint64_t total;
    uint64_t count;
} openfs_fsck_diagnostic_t;
openfs_fsck_result_t openfs_fsck_with_progress_and_diagnostics(openfs_block_device_t *,const openfs_superblock_t *,uint64_t *,openfs_fsck_diagnostic_t *,openfs_fsck_progress_callback_t,void *);
openfs_fsck_result_t openfs_fsck_with_progress(openfs_block_device_t *,const openfs_superblock_t *,uint64_t *,openfs_fsck_progress_callback_t,void *);
openfs_fsck_result_t openfs_fsck(openfs_block_device_t *,const openfs_superblock_t *,uint64_t *);
/* Safely clears only impossible bits beyond the logical block/inode bitmap ranges, then re-runs fsck. */
openfs_fsck_result_t openfs_fsck_repair_bitmap_tails(openfs_block_device_t *,const openfs_superblock_t *,uint64_t *);
#ifdef __cplusplus
}
#endif
#endif