#ifndef OPENFS_EXTENT_H
#define OPENFS_EXTENT_H
#include <stdint.h>
#include "openfs/block_device.h"
#include "openfs/inode.h"
#include "openfs/format.h"
#ifdef __cplusplus
extern "C" {
#endif
#define OPENFS_EXTENT_RECORD_SIZE 24U
#define OPENFS_EXTENT_MAX 5U
#define OPENFS_EXTENT_TREE_MAGIC "OEXT1\0\0\0"
#define OPENFS_EXTENT_TREE_HEADER_SIZE 32U
#define OPENFS_EXTENT_TREE_RECORD_SIZE 24U
typedef enum { OPENFS_EXTENT_OK=0, OPENFS_EXTENT_INVALID_ARGUMENT=1, OPENFS_EXTENT_OUT_OF_RANGE=2, OPENFS_EXTENT_NO_SPACE=3, OPENFS_EXTENT_CORRUPT=4, OPENFS_EXTENT_IO_ERROR=5 } openfs_extent_result_t;
typedef struct { uint64_t logical_start, physical_start, block_count; } openfs_extent_t;
openfs_extent_result_t openfs_inode_get_extent(const openfs_inode_t *,uint32_t,openfs_extent_t *);
openfs_extent_result_t openfs_inode_set_extent(openfs_inode_t *,uint32_t,const openfs_extent_t *);
uint64_t openfs_inode_get_extent_tree_root(const openfs_inode_t *);
/* Memory-only setter: filesystem/device invariants are enforced by openfs_extent_tree_write() before on-disk tree writes. */
openfs_extent_result_t openfs_inode_set_extent_tree_root(openfs_inode_t *,uint64_t);
uint32_t openfs_extent_tree_capacity(uint32_t block_size);
openfs_extent_result_t openfs_extent_tree_read(const openfs_block_device_t *,const openfs_superblock_t *,const openfs_inode_t *,uint32_t,openfs_extent_t *);
openfs_extent_result_t openfs_extent_tree_write(const openfs_block_device_t *,const openfs_superblock_t *,const openfs_inode_t *,const openfs_extent_t *,uint32_t);
#ifdef __cplusplus
}
#endif
#endif
