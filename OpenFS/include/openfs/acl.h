#ifndef OPENFS_ACL_H
#define OPENFS_ACL_H
#include <stdint.h>
#include "openfs/format.h"
#include "openfs/inode.h"
#include "openfs/xattr.h"
#ifdef __cplusplus
extern "C" {
#endif
#define OPENFS_ACL_USER_OBJ 1U
#define OPENFS_ACL_USER 2U
#define OPENFS_ACL_GROUP_OBJ 4U
#define OPENFS_ACL_GROUP 8U
#define OPENFS_ACL_MASK 16U
#define OPENFS_ACL_OTHER 32U
typedef struct { uint16_t tag; uint32_t id; uint16_t permissions; } openfs_acl_entry_t;
typedef enum { OPENFS_ACL_OK=0, OPENFS_ACL_INVALID_ARGUMENT=1, OPENFS_ACL_NOT_FOUND=2, OPENFS_ACL_EXISTS=3, OPENFS_ACL_NO_SPACE=4, OPENFS_ACL_CORRUPT=5, OPENFS_ACL_IO_ERROR=6, OPENFS_ACL_ACCESS_DENIED=7 } openfs_acl_result_t;
openfs_acl_result_t openfs_acl_set(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,const openfs_acl_entry_t *,uint32_t,int);
openfs_acl_result_t openfs_acl_get(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,openfs_acl_entry_t *,uint32_t *,int);
openfs_acl_result_t openfs_acl_remove(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,int);
openfs_acl_result_t openfs_acl_check_access(openfs_block_device_t *,const openfs_superblock_t *,const openfs_inode_t *,uint32_t,uint32_t,uint8_t);
openfs_acl_result_t openfs_acl_inherit(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,uint64_t,uint32_t,int);
#ifdef __cplusplus
}
#endif
#endif
