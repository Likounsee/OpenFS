#ifndef OPENFS_FILE_H
#define OPENFS_FILE_H

#include <stddef.h>
#include <stdint.h>
#include "openfs/allocator.h"
#include "openfs/extent.h"
#include "openfs/inode.h"
#include "openfs/transaction.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OPENFS_FILE_OK = 0,
    OPENFS_FILE_INVALID_ARGUMENT = 1,
    OPENFS_FILE_OUT_OF_RANGE = 2,
    OPENFS_FILE_NO_SPACE = 3,
    OPENFS_FILE_IO_ERROR = 4,
    OPENFS_FILE_CORRUPT = 5,
    OPENFS_FILE_TOO_MANY_EXTENTS = 6,
    OPENFS_FILE_ACCESS_DENIED = 7
} openfs_file_result_t;

/* Translate one logical file block to its physical filesystem block. */
openfs_file_result_t openfs_file_map_block(
    const openfs_inode_t *inode,
    uint64_t logical_block,
    uint64_t *physical_block);

openfs_file_result_t openfs_file_map_block_device(
    const openfs_block_device_t *,
    const openfs_superblock_t *,
    const openfs_inode_t *,
    uint64_t,
    uint64_t *);

/* Read up to length bytes from a regular file. Bytes beyond EOF are not read. */
openfs_file_result_t openfs_file_read(
    const openfs_block_device_t *device,
    const openfs_superblock_t *superblock,
    const openfs_inode_t *inode,
    uint64_t offset,
    void *buffer,
    size_t length,
    size_t *bytes_read);

/*
 * Write length bytes. Writes past EOF extend the file and zero-fill the gap.
 * The inode is updated in memory and persisted to the inode table before return.
 * Direct writes are not a transaction; use the transaction-aware API for crash-atomic mutation.
 */
openfs_file_result_t openfs_file_write(
    openfs_block_device_t *device,
    const openfs_superblock_t *superblock,
    openfs_inode_t *inode,
    uint64_t offset,
    const void *buffer,
    size_t length);

/* Shrink or extend a file. Extension is zero-filled. */
openfs_file_result_t openfs_file_truncate(
    openfs_block_device_t *device,
    const openfs_superblock_t *superblock,
    openfs_inode_t *inode,
    uint64_t new_size);
openfs_file_result_t openfs_file_write_tx(openfs_transaction_t *,const openfs_superblock_t *,openfs_inode_t *,uint64_t,const void *,size_t);
openfs_file_result_t openfs_file_truncate_tx(openfs_transaction_t *,const openfs_superblock_t *,openfs_inode_t *,uint64_t);
openfs_file_result_t openfs_file_read_as(openfs_block_device_t *,const openfs_superblock_t *,const openfs_inode_t *,uint32_t,uint32_t,uint64_t,void *,size_t,size_t *);
openfs_file_result_t openfs_file_write_as(openfs_block_device_t *,const openfs_superblock_t *,openfs_inode_t *,uint32_t,uint32_t,uint64_t,const void *,size_t);
openfs_file_result_t openfs_file_truncate_as(openfs_block_device_t *,const openfs_superblock_t *,openfs_inode_t *,uint32_t,uint32_t,uint64_t);

#ifdef __cplusplus
}
#endif
#endif
