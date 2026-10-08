#ifndef OPENFS_COW_H
#define OPENFS_COW_H
#include <stdint.h>
#include "openfs/block_device.h"
#include "openfs/format.h"
#include "openfs/inode.h"
#include "openfs/transaction.h"
typedef struct openfs_inode openfs_inode_t;
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    OPENFS_COW_OK=0,
    OPENFS_COW_INVALID_ARGUMENT=1,
    OPENFS_COW_UNSUPPORTED=2,
    OPENFS_COW_OUT_OF_RANGE=3,
    OPENFS_COW_OVERFLOW=4,
    OPENFS_COW_CORRUPT=5,
    OPENFS_COW_IO_ERROR=6
} openfs_cow_result_t;
openfs_cow_result_t openfs_cow_refcount_get(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,uint16_t *);
openfs_cow_result_t openfs_cow_refcount_get_tx(openfs_transaction_t *,const openfs_superblock_t *,uint64_t,uint16_t *);
openfs_cow_result_t openfs_cow_refcount_set(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,uint16_t);
openfs_cow_result_t openfs_cow_refcount_set_tx(openfs_transaction_t *,const openfs_superblock_t *,uint64_t,uint16_t);
openfs_cow_result_t openfs_cow_refcount_inc(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,uint16_t *);
openfs_cow_result_t openfs_cow_refcount_inc_tx(openfs_transaction_t *,const openfs_superblock_t *,uint64_t,uint16_t *);
openfs_cow_result_t openfs_cow_refcount_dec(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,uint16_t *);
openfs_cow_result_t openfs_cow_refcount_dec_tx(openfs_transaction_t *,const openfs_superblock_t *,uint64_t,uint16_t *);
openfs_cow_result_t openfs_cow_discard_inode(openfs_block_device_t *,const openfs_superblock_t *,openfs_inode_t *);
openfs_cow_result_t openfs_cow_discard_inode_number(openfs_block_device_t *,const openfs_superblock_t *,uint64_t);
openfs_cow_result_t openfs_cow_clone_inode(openfs_block_device_t *,const openfs_superblock_t *,const openfs_inode_t *,uint64_t,uint64_t *);
#ifdef __cplusplus
}
#endif
#endif
