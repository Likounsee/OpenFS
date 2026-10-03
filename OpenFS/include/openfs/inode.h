#ifndef OPENFS_INODE_H
#define OPENFS_INODE_H
#include <stdint.h>
#include "openfs/block_device.h"
#ifdef __cplusplus
extern "C" {
#endif
#define OPENFS_INODE_SIZE 256U
#define OPENFS_INODE_MODE_FREE 0U
#define OPENFS_INODE_MODE_REGULAR 0100000U
#define OPENFS_INODE_MODE_DIRECTORY 0040000U
#define OPENFS_INODE_MODE_SYMLINK 0120000U
#define OPENFS_INODE_TYPE_MASK 0170000U
#define OPENFS_INODE_PERMISSION_MASK 07777U
#define OPENFS_INODE_FLAG_INLINE_DATA 1U
#define OPENFS_INODE_FLAG_HAS_EXTENTS 2U
#define OPENFS_INODE_FLAG_EXTENT_TREE 4U
#define OPENFS_INODE_INLINE_EXTENT_MAX 5U
#define OPENFS_INODE_TREE_INLINE_EXTENT_MAX 4U
typedef enum { OPENFS_INODE_OK=0, OPENFS_INODE_INVALID_ARGUMENT=1, OPENFS_INODE_OUT_OF_RANGE=2, OPENFS_INODE_CORRUPT=3, OPENFS_INODE_IO_ERROR=4, OPENFS_INODE_ACCESS_DENIED=5 } openfs_inode_result_t;
typedef struct openfs_inode {
    uint64_t inode_number, generation, size, blocks, parent_inode, link_count;
    uint64_t atime_ns, mtime_ns, ctime_ns;
    uint32_t mode, flags, uid, gid, extent_count, reserved0;
    uint8_t inline_data[80];
    uint8_t reserved[128];
} openfs_inode_t;
openfs_inode_result_t openfs_inode_read(const openfs_block_device_t *, uint64_t, uint64_t, uint64_t, openfs_inode_t *);
openfs_inode_result_t openfs_inode_write(const openfs_block_device_t *, uint64_t, uint64_t, const openfs_inode_t *);
openfs_inode_result_t openfs_inode_validate(const openfs_inode_t *, uint64_t);
openfs_inode_result_t openfs_inode_check_access(const openfs_inode_t *, uint32_t uid, uint32_t gid, uint8_t requested);
#ifdef __cplusplus
}
#endif
#endif
