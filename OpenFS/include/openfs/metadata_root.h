#ifndef OPENFS_METADATA_ROOT_H
#define OPENFS_METADATA_ROOT_H
#include <stdint.h>
#include "openfs/format.h"
#include "openfs/block_device.h"
#ifdef __cplusplus
extern "C" {
#endif
#define OPENFS_METADATA_ROOT_VERSION 1U
#define OPENFS_METADATA_ROOT_FLAG_LEGACY_LAYOUT 1U
typedef enum { OPENFS_METADATA_ROOT_OK=0, OPENFS_METADATA_ROOT_INVALID_ARGUMENT=1, OPENFS_METADATA_ROOT_CORRUPT=2, OPENFS_METADATA_ROOT_IO_ERROR=3 } openfs_metadata_root_result_t;
typedef struct { uint16_t version; uint16_t flags; uint64_t generation; uint64_t inode_table_root; uint64_t directory_root; uint64_t extent_tree_root; uint64_t xattr_root; uint64_t snapshot_catalog_root; } openfs_metadata_root_t;
openfs_metadata_root_result_t openfs_metadata_root_initialize(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,uint64_t);
openfs_metadata_root_result_t openfs_metadata_root_read(const openfs_block_device_t *,const openfs_superblock_t *,uint64_t,openfs_metadata_root_t *);
openfs_metadata_root_result_t openfs_metadata_root_update_generation(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,uint64_t,uint64_t *);
#ifdef __cplusplus
}
#endif
#endif
