#ifndef OPENFS_DIR_H
#define OPENFS_DIR_H

#include <stdint.h>
#include "openfs/file.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPENFS_DIR_ENTRY_SIZE 256U
#define OPENFS_DIR_NAME_MAX 227U

typedef enum {
    OPENFS_DIR_OK = 0,
    OPENFS_DIR_INVALID_ARGUMENT = 1,
    OPENFS_DIR_NOT_FOUND = 2,
    OPENFS_DIR_EXISTS = 3,
    OPENFS_DIR_NO_SPACE = 4,
    OPENFS_DIR_IO_ERROR = 5,
    OPENFS_DIR_CORRUPT = 6,
    OPENFS_DIR_NAME_TOO_LONG = 7
} openfs_dir_result_t;

typedef struct {
    uint64_t inode_number;
    uint64_t generation;
    uint8_t type;
} openfs_dir_entry_t;

openfs_dir_result_t openfs_dir_lookup(
    const openfs_block_device_t *,
    const openfs_superblock_t *,
    const openfs_inode_t *,
    const char *,
    openfs_dir_entry_t *);

openfs_dir_result_t openfs_dir_add(
    openfs_block_device_t *,
    const openfs_superblock_t *,
    openfs_inode_t *,
    const char *,
    const openfs_dir_entry_t *);

openfs_dir_result_t openfs_dir_remove(
    openfs_block_device_t *,
    const openfs_superblock_t *,
    openfs_inode_t *,
    const char *);

#ifdef __cplusplus
}
#endif
#endif
