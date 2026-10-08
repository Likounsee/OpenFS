#ifndef OPENFS_XATTR_H
#define OPENFS_XATTR_H
#include <stdint.h>
#include <stddef.h>
#include "openfs/format.h"
#include "openfs/inode.h"
#ifdef __cplusplus
extern "C" {
#endif
#define OPENFS_XATTR_CREATE 1U
#define OPENFS_XATTR_REPLACE 2U
typedef enum { OPENFS_XATTR_OK=0, OPENFS_XATTR_INVALID_ARGUMENT=1, OPENFS_XATTR_NOT_FOUND=2, OPENFS_XATTR_EXISTS=3, OPENFS_XATTR_NAME_TOO_LONG=4, OPENFS_XATTR_NO_SPACE=5, OPENFS_XATTR_CORRUPT=6, OPENFS_XATTR_IO_ERROR=7 } openfs_xattr_result_t;
openfs_xattr_result_t openfs_xattr_get(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,const char *,void *,size_t,size_t *);
openfs_xattr_result_t openfs_xattr_set(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,const char *,const void *,size_t,uint32_t);
openfs_xattr_result_t openfs_xattr_remove(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,const char *);
openfs_xattr_result_t openfs_xattr_list(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,char *,size_t,size_t *);
openfs_xattr_result_t openfs_xattr_validate_inode(const openfs_block_device_t *,const openfs_superblock_t *,const openfs_inode_t *);
#ifdef __cplusplus
}
#endif
#endif
