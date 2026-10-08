#ifndef OPENFS_FORMAT_H
#define OPENFS_FORMAT_H
#include <stdint.h>
#include "openfs/block_device.h"
struct openfs_runtime;
#ifdef __cplusplus
extern "C" {
#endif
#define OPENFS_FORMAT_VERSION_MAJOR 1U
#define OPENFS_FORMAT_VERSION_MINOR 5U
#define OPENFS_FEATURE_EXTENT_TREE (1ULL << 0)
#define OPENFS_FEATURE_COW (1ULL << 1)
#define OPENFS_FEATURE_METADATA_ROOT (1ULL << 2)
#define OPENFS_COW_MAX_REFCOUNT UINT16_MAX
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
typedef enum openfs_validation_code {
    OPENFS_VALIDATION_OK = 0,
    OPENFS_VALIDATION_INVALID_ARGUMENT = 1,
    OPENFS_VALIDATION_FORMAT_VERSION = 100,
    OPENFS_VALIDATION_FEATURE_FLAGS = 101,
    OPENFS_VALIDATION_EXTENT_VERSION = 102,
    OPENFS_VALIDATION_BLOCK_SIZE = 103,
    OPENFS_VALIDATION_GEOMETRY = 104,
    OPENFS_VALIDATION_METADATA_GEOMETRY = 105,
    OPENFS_VALIDATION_BLOCK_BITMAP_CHAIN = 106,
    OPENFS_VALIDATION_INODE_BITMAP_CHAIN = 107,
    OPENFS_VALIDATION_INODE_TABLE_CHAIN = 108,
    OPENFS_VALIDATION_JOURNAL_CHAIN = 109,
    OPENFS_VALIDATION_DATA_END = 110,
    OPENFS_VALIDATION_REGION_SIZE = 111,
    OPENFS_VALIDATION_METADATA_END = 112,
    OPENFS_VALIDATION_INODE_TABLE_SIZE = 113,
    OPENFS_VALIDATION_INODE_COUNT = 114,
    OPENFS_VALIDATION_INODE_BITMAP_CAPACITY = 115,
    OPENFS_VALIDATION_BLOCK_BITMAP_CAPACITY = 116,
    OPENFS_VALIDATION_REFCOUNT_CHAIN = 117,
    OPENFS_VALIDATION_REFCOUNT_CAPACITY = 118,
    OPENFS_VALIDATION_METADATA_ROOT = 119,
    OPENFS_VALIDATION_RESERVED_FIELDS = 120
} openfs_validation_code_t;
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
    uint64_t refcount_start;
    uint64_t refcount_blocks;
    uint64_t metadata_root_block;
    uint64_t metadata_root_generation;
    /* Runtime-only pointer; never serialized to disk. */
    struct openfs_runtime *runtime;
} openfs_superblock_t;
openfs_format_result_t openfs_format_ex(openfs_block_device_t *, const uint8_t uuid[16], uint32_t flags);
openfs_format_result_t openfs_format(openfs_block_device_t *, const uint8_t uuid[16]);
openfs_format_result_t openfs_read_superblock(openfs_block_device_t *, openfs_superblock_t *);
openfs_format_result_t openfs_validate_superblock(const openfs_block_device_t *, const openfs_superblock_t *);
const char *openfs_validate_superblock_reason(const openfs_block_device_t *, const openfs_superblock_t *);
openfs_validation_code_t openfs_validate_superblock_code(const openfs_block_device_t *, const openfs_superblock_t *);
openfs_format_result_t openfs_prepare_superblock(openfs_block_device_t *, const uint8_t uuid[16], openfs_superblock_t *);
#ifdef __cplusplus
}
#endif
#endif
