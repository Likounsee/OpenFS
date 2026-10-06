#ifndef OPENFS_FORMAT_H
#define OPENFS_FORMAT_H
#include <stdint.h>
#include "openfs/block_device.h"
#ifdef __cplusplus
extern "C" {
#endif
#define OPENFS_FORMAT_VERSION_MAJOR 1U
#define OPENFS_FORMAT_VERSION_MINOR 3U
#define OPENFS_FEATURE_EXTENT_TREE (1ULL << 0)
#define OPENFS_SUPERBLOCK_SIZE 4096U
#define OPENFS_MIN_BLOCK_SIZE 4096U
#define OPENFS_MAX_BLOCK_SIZE 65536U
#define OPENFS_INODE_SIZE 256U

#define OPENFS_FORMAT_FLAG_NONE 0U
#define OPENFS_FORMAT_FLAG_FULL_ZERO (1U << 0)
typedef enum openfs_format_result {
    OPENFS_FORMAT_OK = 0,
    OPENFS_FORMAT_INVALID_ARGUMENT = 1,
    OPENFS_FORMAT_UNSUPPORTED_DEVICE = 2,
    OPENFS_FORMAT_TOO_SMALL = 3,
    OPENFS_FORMAT_CORRUPT = 4,
    OPENFS_FORMAT_IO_ERROR = 5
} openfs_format_result_t;
typedef struct openfs_superblock {
    uint16_t version_major;
    uint16_t version_minor;
    uint64_t feature_flags;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t metadata_start;
    uint64_t metadata_blocks;
    uint64_t block_bitmap_start;
    uint64_t block_bitmap_blocks;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t journal_start;
    uint64_t journal_blocks;
    uint64_t data_start;
    uint64_t data_blocks;
    uint64_t root_inode;
    uint64_t generation;
    uint8_t uuid[16];
} openfs_superblock_t;
openfs_format_result_t openfs_format(openfs_block_device_t *, const uint8_t uuid[16]);
openfs_format_result_t openfs_read_superblock(openfs_block_device_t *, openfs_superblock_t *);
openfs_format_result_t openfs_validate_superblock(const openfs_block_device_t *, const openfs_superblock_t *);
#ifdef __cplusplus
}
#endif
#endif
