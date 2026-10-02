#ifndef OPENFS_LINK_H
#define OPENFS_LINK_H
#include <stdint.h>
#include "openfs/path.h"
#ifdef __cplusplus
extern "C" {
#endif
openfs_path_result_t openfs_link(openfs_block_device_t *,const openfs_superblock_t *,const char *,const char *);
openfs_path_result_t openfs_symlink(openfs_block_device_t *,const openfs_superblock_t *,const char *,const char *);
openfs_path_result_t openfs_readlink(openfs_block_device_t *,const openfs_superblock_t *,const char *,char *,uint64_t);
openfs_path_result_t openfs_resolve_symlink(openfs_block_device_t *,const openfs_superblock_t *,const char *,char *,uint64_t,uint32_t);
openfs_path_result_t openfs_link_tx(openfs_transaction_t *,const openfs_superblock_t *,const char *,const char *);
openfs_path_result_t openfs_symlink_tx(openfs_transaction_t *,const openfs_superblock_t *,const char *,const char *);
#ifdef __cplusplus
}
#endif
#endif