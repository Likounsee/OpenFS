#ifndef OPENFS_ORPHAN_H
#define OPENFS_ORPHAN_H
#include "openfs/block_device.h"
#include "openfs/format.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    OPENFS_ORPHAN_OK = 0,
    OPENFS_ORPHAN_INVALID_ARGUMENT = 1,
    OPENFS_ORPHAN_NOT_ORPHAN = 2,
    OPENFS_ORPHAN_IO_ERROR = 3,
    OPENFS_ORPHAN_CORRUPT = 4
} openfs_orphan_result_t;
openfs_orphan_result_t openfs_orphan_reclaim(openfs_block_device_t *, const openfs_superblock_t *, uint64_t inode_number);
openfs_orphan_result_t openfs_orphan_recover_all(openfs_block_device_t *, const openfs_superblock_t *);
#ifdef __cplusplus
}
#endif
#endif
