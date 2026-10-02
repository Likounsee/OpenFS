#ifndef OPENFS_INODE_ALLOC_H
#define OPENFS_INODE_ALLOC_H

#include <stdint.h>
#include "openfs/format.h"
#include "openfs/inode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OPENFS_INODE_ALLOC_OK = 0,
    OPENFS_INODE_ALLOC_INVALID_ARGUMENT = 1,
    OPENFS_INODE_ALLOC_OUT_OF_SPACE = 2,
    OPENFS_INODE_ALLOC_CORRUPT = 3,
    OPENFS_INODE_ALLOC_IO_ERROR = 4
} openfs_inode_alloc_result_t;

openfs_inode_alloc_result_t openfs_inode_alloc(
    openfs_block_device_t *,
    const openfs_superblock_t *,
    uint64_t parent_inode,
    uint32_t mode,
    uint64_t *inode_number);

openfs_inode_alloc_result_t openfs_inode_free(
    openfs_block_device_t *,
    const openfs_superblock_t *,
    uint64_t inode_number);

#ifdef __cplusplus
}
#endif
#endif
