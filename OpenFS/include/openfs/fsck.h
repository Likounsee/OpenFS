#ifndef OPENFS_FSCK_H
#define OPENFS_FSCK_H
#include <stdint.h>
#include "openfs/mount.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { OPENFS_FSCK_OK=0, OPENFS_FSCK_INVALID_ARGUMENT=1, OPENFS_FSCK_CORRUPT=2, OPENFS_FSCK_IO_ERROR=3 } openfs_fsck_result_t;
openfs_fsck_result_t openfs_fsck(openfs_block_device_t *,const openfs_superblock_t *,uint64_t *);
#ifdef __cplusplus
}
#endif
#endif