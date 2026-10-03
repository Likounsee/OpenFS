#ifndef OPENFS_PATH_H
#define OPENFS_PATH_H
#define OPENFS_PATH_MAX 4096U
#include <stdint.h>
#include "openfs/dir.h"
#include "openfs/inode_alloc.h"
#include "openfs/transaction.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { OPENFS_PATH_OK=0, OPENFS_PATH_INVALID_ARGUMENT=1, OPENFS_PATH_NOT_FOUND=2, OPENFS_PATH_EXISTS=3, OPENFS_PATH_NOT_DIRECTORY=4, OPENFS_PATH_NAME_TOO_LONG=5, OPENFS_PATH_IO_ERROR=6, OPENFS_PATH_CORRUPT=7, OPENFS_PATH_NO_SPACE=8, OPENFS_PATH_ACCESS_DENIED=9 } openfs_path_result_t;
openfs_path_result_t openfs_path_lookup(openfs_block_device_t *,const openfs_superblock_t *,const char *,uint64_t *);
openfs_path_result_t openfs_path_lookup_follow(openfs_block_device_t *,const openfs_superblock_t *,const char *,uint64_t *);
openfs_path_result_t openfs_path_create(openfs_block_device_t *,const openfs_superblock_t *,const char *,uint32_t,uint64_t *);
openfs_path_result_t openfs_path_mkdir(openfs_block_device_t *,const openfs_superblock_t *,const char *,uint64_t *);
openfs_path_result_t openfs_path_unlink(openfs_block_device_t *,const openfs_superblock_t *,const char *);
openfs_path_result_t openfs_path_rename(openfs_block_device_t *,const openfs_superblock_t *,const char *,const char *);
openfs_path_result_t openfs_path_chmod(openfs_block_device_t *,const openfs_superblock_t *,const char *,uint32_t);
openfs_path_result_t openfs_path_set_times(openfs_block_device_t *,const openfs_superblock_t *,const char *,uint64_t,uint64_t);
openfs_path_result_t openfs_path_check_access(openfs_block_device_t *,const openfs_superblock_t *,const char *,uint32_t,uint32_t,uint8_t);
openfs_path_result_t openfs_path_create_tx(openfs_transaction_t *,const openfs_superblock_t *,const char *,uint32_t,uint64_t *);
openfs_path_result_t openfs_path_mkdir_tx(openfs_transaction_t *,const openfs_superblock_t *,const char *,uint64_t *);
openfs_path_result_t openfs_path_unlink_tx(openfs_transaction_t *,const openfs_superblock_t *,const char *);
openfs_path_result_t openfs_path_rename_tx(openfs_transaction_t *,const openfs_superblock_t *,const char *,const char *);
#ifdef __cplusplus
}
#endif
#endif
