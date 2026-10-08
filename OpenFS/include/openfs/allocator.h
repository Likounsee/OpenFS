#ifndef OPENFS_ALLOCATOR_H
#define OPENFS_ALLOCATOR_H
#include <stdint.h>
#include "openfs/format.h"
#include "openfs/transaction.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { OPENFS_ALLOC_OK=0, OPENFS_ALLOC_INVALID_ARGUMENT=1, OPENFS_ALLOC_OUT_OF_SPACE=2, OPENFS_ALLOC_CORRUPT=3, OPENFS_ALLOC_IO_ERROR=4 } openfs_alloc_result_t;
openfs_alloc_result_t openfs_alloc_block(openfs_block_device_t *,const openfs_superblock_t *,uint64_t *);
/* Transaction variants operate on the transaction device and never open a nested transaction. */
openfs_alloc_result_t openfs_alloc_block_tx(openfs_transaction_t *,const openfs_superblock_t *,uint64_t *);
openfs_alloc_result_t openfs_free_block_tx(openfs_transaction_t *,const openfs_superblock_t *,uint64_t);
openfs_alloc_result_t openfs_free_block(openfs_block_device_t *,const openfs_superblock_t *,uint64_t);
#ifdef __cplusplus
}
#endif
#endif
