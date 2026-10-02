#ifndef OPENFS_EXTENT_H
#define OPENFS_EXTENT_H
#include <stdint.h>
#include "openfs/block_device.h"
#include "openfs/inode.h"
#ifdef __cplusplus
extern "C" {
#endif
#define OPENFS_EXTENT_MAX 16U
typedef enum { OPENFS_EXTENT_OK=0, OPENFS_EXTENT_INVALID_ARGUMENT=1, OPENFS_EXTENT_OUT_OF_RANGE=2, OPENFS_EXTENT_NO_SPACE=3, OPENFS_EXTENT_CORRUPT=4 } openfs_extent_result_t;
typedef struct { uint64_t logical_start, physical_start, block_count; } openfs_extent_t;
openfs_extent_result_t openfs_inode_get_extent(const openfs_inode_t *,uint32_t,openfs_extent_t *);
openfs_extent_result_t openfs_inode_set_extent(openfs_inode_t *,uint32_t,const openfs_extent_t *);
#ifdef __cplusplus
}
#endif
#endif
