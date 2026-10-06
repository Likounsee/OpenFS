#ifndef OPENFS_FILE_LOCK_H
#define OPENFS_FILE_LOCK_H
#include <stdint.h>
#include "openfs/format.h"
#include "openfs/block_device.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { OPENFS_FILE_LOCK_OK=0, OPENFS_FILE_LOCK_INVALID_ARGUMENT=1, OPENFS_FILE_LOCK_BUSY=2, OPENFS_FILE_LOCK_NOT_HELD=3, OPENFS_FILE_LOCK_ERROR=4 } openfs_file_lock_result_t;
typedef enum { OPENFS_FILE_LOCK_SHARED=1, OPENFS_FILE_LOCK_EXCLUSIVE=2 } openfs_file_lock_type_t;
openfs_file_lock_result_t openfs_file_lock_try(openfs_superblock_t *,openfs_block_device_t *,uint64_t,uint64_t,openfs_file_lock_type_t);
openfs_file_lock_result_t openfs_file_lock_unlock(openfs_superblock_t *,openfs_block_device_t *,uint64_t,uint64_t);
#ifdef __cplusplus
}
#endif
#endif
