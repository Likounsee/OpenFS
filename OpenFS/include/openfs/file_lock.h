#ifndef OPENFS_FILE_LOCK_H
#define OPENFS_FILE_LOCK_H
#include <stdint.h>
#include "openfs/fd.h"
#ifdef __cplusplus
extern "C" {
#endif
#define OPENFS_FILE_LOCK_SHARED 1U
#define OPENFS_FILE_LOCK_EXCLUSIVE 2U
#define OPENFS_FILE_LOCK_BLOCK 0x00000001U
typedef enum {
    OPENFS_FILE_LOCK_OK=0,
    OPENFS_FILE_LOCK_INVALID_ARGUMENT=1,
    OPENFS_FILE_LOCK_CONFLICT=2,
    OPENFS_FILE_LOCK_CLOSED=3,
    OPENFS_FILE_LOCK_NOT_FOUND=4,
    OPENFS_FILE_LOCK_IO_ERROR=5
} openfs_file_lock_result_t;
openfs_file_lock_result_t openfs_file_lock(openfs_file_handle_t *,uint64_t,uint64_t,uint32_t,uint32_t);
openfs_file_lock_result_t openfs_file_unlock(openfs_file_handle_t *,uint64_t,uint64_t);
openfs_file_lock_result_t openfs_file_lock_test(openfs_file_handle_t *,uint64_t,uint64_t,uint32_t *);
void openfs_file_lock_release_all(openfs_file_handle_t *);
#ifdef __cplusplus
}
#endif
#endif
