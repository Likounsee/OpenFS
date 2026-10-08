#ifndef OPENFS_METADATA_COW_H
#define OPENFS_METADATA_COW_H
#include <stdint.h>
#include "openfs/block_device.h"
#include "openfs/format.h"
#ifdef __cplusplus
extern "C" {
#endif
#define OPENFS_METADATA_COW_MAGIC "OMCB1"
#define OPENFS_METADATA_COW_VERSION 1U
#define OPENFS_METADATA_COW_HEADER_SIZE 40U
typedef enum { OPENFS_METADATA_COW_OK=0, OPENFS_METADATA_COW_INVALID_ARGUMENT=1, OPENFS_METADATA_COW_UNSUPPORTED=2, OPENFS_METADATA_COW_OUT_OF_SPACE=3, OPENFS_METADATA_COW_OVERFLOW=4, OPENFS_METADATA_COW_CORRUPT=5, OPENFS_METADATA_COW_IO_ERROR=6 } openfs_metadata_cow_result_t;
typedef enum { OPENFS_METADATA_COW_TYPE_INVALID=0, OPENFS_METADATA_COW_TYPE_METADATA_ROOT=1, OPENFS_METADATA_COW_TYPE_INODE_TABLE=2, OPENFS_METADATA_COW_TYPE_DIRECTORY=3, OPENFS_METADATA_COW_TYPE_EXTENT_TREE=4, OPENFS_METADATA_COW_TYPE_XATTR=5, OPENFS_METADATA_COW_TYPE_SNAPSHOT_CATALOG=6 } openfs_metadata_cow_type_t;
typedef struct { uint16_t version; uint16_t type; uint32_t flags; uint64_t logical_id; uint64_t generation; uint32_t payload_crc32c; uint32_t header_crc32c; } openfs_metadata_cow_header_t;
/* This is a stage-1 metadata-CoW primitive. It uses existing data-region allocation/refcounts; it does not make legacy metadata roots snapshot-safe by itself. Callers inside a transaction must pass openfs_transaction_device(tx). */
openfs_metadata_cow_result_t openfs_metadata_cow_validate_block(const openfs_block_device_t *,const openfs_superblock_t *,uint64_t,openfs_metadata_cow_header_t *);
openfs_metadata_cow_result_t openfs_metadata_cow_initialize_block(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,openfs_metadata_cow_type_t,uint64_t,uint64_t,uint32_t,const uint8_t *);
openfs_metadata_cow_result_t openfs_metadata_cow_write_payload(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,openfs_metadata_cow_type_t,uint64_t,uint64_t,uint32_t,const uint8_t *);
openfs_metadata_cow_result_t openfs_metadata_cow_alloc(openfs_block_device_t *,const openfs_superblock_t *,openfs_metadata_cow_type_t,uint64_t,uint64_t,uint32_t,uint64_t *);
openfs_metadata_cow_result_t openfs_metadata_cow_clone(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,openfs_metadata_cow_type_t,uint64_t,uint64_t,uint64_t *);
openfs_metadata_cow_result_t openfs_metadata_cow_acquire_opaque(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,uint16_t *);
openfs_metadata_cow_result_t openfs_metadata_cow_release_opaque(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,uint16_t *);
openfs_metadata_cow_result_t openfs_metadata_cow_clone_opaque(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,uint64_t *);
openfs_metadata_cow_result_t openfs_metadata_cow_copy_before_write_opaque(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,uint64_t *);
openfs_metadata_cow_result_t openfs_metadata_cow_acquire(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,uint16_t *);
openfs_metadata_cow_result_t openfs_metadata_cow_release(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,uint16_t *);
openfs_metadata_cow_result_t openfs_metadata_cow_copy_before_write(openfs_block_device_t *,const openfs_superblock_t *,uint64_t,openfs_metadata_cow_type_t,uint64_t,uint64_t,uint64_t *);
#ifdef __cplusplus
}
#endif
#endif
