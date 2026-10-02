#include "openfs/format.h"

#include <stdlib.h>
#include <string.h>
#include "openfs/crc32c.h"

#define OPENFS_CHECKSUM_OFFSET 4088U
#define OPENFS_BACKUP_OFFSET(total) ((total) - 1U)

static uint16_t get_u16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8U); }
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}
static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0U;
    for (unsigned i = 0U; i < 8U; ++i) v |= (uint64_t)p[i] << (8U * i);
    return v;
}
static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8U); }
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8U); p[2] = (uint8_t)(v >> 16U); p[3] = (uint8_t)(v >> 24U);
}
static void put_u64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0U; i < 8U; ++i) p[i] = (uint8_t)(v >> (8U * i));
}
static int power_of_two(uint32_t v) { return v != 0U && (v & (v - 1U)) == 0U; }
static int add_overflow(uint64_t a, uint64_t b, uint64_t *out) {
    if (b > UINT64_MAX - a) return 1;
    *out = a + b;
    return 0;
}
static int mul_overflow(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0U && b > UINT64_MAX / a) return 1;
    *out = a * b;
    return 0;
}

static void encode(const openfs_superblock_t *sb, uint8_t *buf) {
    memset(buf, 0, OPENFS_SUPERBLOCK_SIZE);
    memcpy(buf, "OPENFS\0\0", 8U);
    put_u16(buf + 8U, sb->version_major);
    put_u16(buf + 10U, sb->version_minor);
    put_u64(buf + 12U, sb->feature_flags);
    put_u32(buf + 20U, sb->block_size);
    put_u32(buf + 24U, OPENFS_SUPERBLOCK_SIZE);
    put_u64(buf + 28U, sb->total_blocks);
    put_u64(buf + 36U, sb->metadata_start);
    put_u64(buf + 44U, sb->metadata_blocks);
    put_u64(buf + 52U, sb->inode_bitmap_start);
    put_u64(buf + 60U, sb->inode_bitmap_blocks);
    put_u64(buf + 68U, sb->inode_table_start);
    put_u64(buf + 76U, sb->inode_table_blocks);
    put_u64(buf + 84U, sb->journal_start);
    put_u64(buf + 92U, sb->journal_blocks);
    put_u64(buf + 100U, sb->root_inode);
    put_u64(buf + 108U, sb->generation);
    memcpy(buf + 116U, sb->uuid, 16U);
    put_u32(buf + OPENFS_CHECKSUM_OFFSET, 0U);
    put_u32(buf + OPENFS_CHECKSUM_OFFSET, openfs_crc32c(buf, OPENFS_CHECKSUM_OFFSET));
}

static openfs_format_result_t decode(const uint8_t *buf, openfs_superblock_t *sb) {
    if (memcmp(buf, "OPENFS\0\0", 8U) != 0 ||
        get_u32(buf + 24U) != OPENFS_SUPERBLOCK_SIZE) return OPENFS_FORMAT_CORRUPT;
    const uint32_t stored = get_u32(buf + OPENFS_CHECKSUM_OFFSET);
    uint8_t copy[OPENFS_SUPERBLOCK_SIZE];
    memcpy(copy, buf, sizeof(copy));
    put_u32(copy + OPENFS_CHECKSUM_OFFSET, 0U);
    if (stored != openfs_crc32c(copy, OPENFS_CHECKSUM_OFFSET)) return OPENFS_FORMAT_CORRUPT;
    sb->version_major = get_u16(buf + 8U);
    sb->version_minor = get_u16(buf + 10U);
    sb->feature_flags = get_u64(buf + 12U);
    sb->block_size = get_u32(buf + 20U);
    sb->total_blocks = get_u64(buf + 28U);
    sb->metadata_start = get_u64(buf + 36U);
    sb->metadata_blocks = get_u64(buf + 44U);
    sb->inode_bitmap_start = get_u64(buf + 52U);
    sb->inode_bitmap_blocks = get_u64(buf + 60U);
    sb->inode_table_start = get_u64(buf + 68U);
    sb->inode_table_blocks = get_u64(buf + 76U);
    sb->journal_start = get_u64(buf + 84U);
    sb->journal_blocks = get_u64(buf + 92U);
    sb->root_inode = get_u64(buf + 100U);
    sb->generation = get_u64(buf + 108U);
    memcpy(sb->uuid, buf + 116U, 16U);
    return OPENFS_FORMAT_OK;
}

openfs_format_result_t openfs_validate_superblock(const openfs_block_device_t *device, const openfs_superblock_t *sb) {
    if (!openfs_block_device_is_valid(device) || sb == NULL) return OPENFS_FORMAT_INVALID_ARGUMENT;
    if (sb->version_major != OPENFS_FORMAT_VERSION_MAJOR || sb->version_minor > OPENFS_FORMAT_VERSION_MINOR)
        return OPENFS_FORMAT_CORRUPT;
    if (sb->block_size < OPENFS_MIN_BLOCK_SIZE || sb->block_size > OPENFS_MAX_BLOCK_SIZE ||
        !power_of_two(sb->block_size) || sb->block_size != device->block_size)
        return OPENFS_FORMAT_UNSUPPORTED_DEVICE;
    if (sb->total_blocks != device->block_count || sb->total_blocks < 32U || sb->root_inode == 0U)
        return OPENFS_FORMAT_CORRUPT;
    if (sb->metadata_start != 2U || sb->metadata_blocks != sb->total_blocks - 3U ||
        sb->inode_bitmap_start != 3U || sb->inode_bitmap_blocks != 1U ||
        sb->inode_table_start != 4U || sb->inode_table_blocks == 0U ||
        sb->journal_start != sb->inode_table_start + sb->inode_table_blocks ||
        sb->journal_blocks < 4U)
        return OPENFS_FORMAT_CORRUPT;

    const uint64_t ranges[][2] = {
        {sb->metadata_start, sb->metadata_blocks},
        {sb->inode_bitmap_start, sb->inode_bitmap_blocks},
        {sb->inode_table_start, sb->inode_table_blocks},
        {sb->journal_start, sb->journal_blocks}
    };
    for (size_t i = 0U; i < sizeof(ranges) / sizeof(ranges[0]); ++i) {
        uint64_t end = 0U;
        if (add_overflow(ranges[i][0], ranges[i][1], &end) ||
            ranges[i][0] < 2U || end > sb->total_blocks - 1U)
            return OPENFS_FORMAT_CORRUPT;
    }
    uint64_t metadata_end = 0U;
    uint64_t inode_bitmap_end = 0U;
    uint64_t inode_table_end = 0U;
    uint64_t journal_end = 0U;
    if (add_overflow(sb->metadata_start, sb->metadata_blocks, &metadata_end) ||
        add_overflow(sb->inode_bitmap_start, sb->inode_bitmap_blocks, &inode_bitmap_end) ||
        add_overflow(sb->inode_table_start, sb->inode_table_blocks, &inode_table_end) ||
        add_overflow(sb->journal_start, sb->journal_blocks, &journal_end) ||
        metadata_end != sb->total_blocks - 1U ||
        inode_bitmap_end != sb->inode_table_start ||
        inode_table_end != sb->journal_start ||
        journal_end != sb->total_blocks - 1U)
        return OPENFS_FORMAT_CORRUPT;
    uint64_t inode_bytes = 0U;
    if (mul_overflow(sb->inode_table_blocks, sb->block_size, &inode_bytes) ||
        inode_bytes < OPENFS_INODE_SIZE) return OPENFS_FORMAT_CORRUPT;
    return OPENFS_FORMAT_OK;
}

openfs_format_result_t openfs_read_superblock(openfs_block_device_t *device, openfs_superblock_t *out) {
    if (!openfs_block_device_is_valid(device) || out == NULL) return OPENFS_FORMAT_INVALID_ARGUMENT;
    if (device->block_size < OPENFS_SUPERBLOCK_SIZE) return OPENFS_FORMAT_UNSUPPORTED_DEVICE;
    uint8_t *buffer = (uint8_t *)malloc(device->block_size);
    if (buffer == NULL) return OPENFS_FORMAT_IO_ERROR;
    if (device->read(device->context, 0U, 1U, buffer) != OPENFS_IO_OK) {
        free(buffer);
        return OPENFS_FORMAT_IO_ERROR;
    }
    openfs_format_result_t result = decode(buffer, out);
    free(buffer);
    if (result != OPENFS_FORMAT_OK) return result;
    return openfs_validate_superblock(device, out);
}

openfs_format_result_t openfs_format(openfs_block_device_t *device, const uint8_t uuid[16]) {
    if (!openfs_block_device_is_valid(device) || uuid == NULL) return OPENFS_FORMAT_INVALID_ARGUMENT;
    if (device->block_size < OPENFS_SUPERBLOCK_SIZE || device->block_size > OPENFS_MAX_BLOCK_SIZE ||
        !power_of_two(device->block_size) || device->block_count < 32U)
        return OPENFS_FORMAT_UNSUPPORTED_DEVICE;

    const uint64_t total = device->block_count;
    const uint64_t metadata_start = 2U;
    const uint64_t metadata_blocks = total - 3U;
    const uint64_t inode_bitmap_start = 3U;
    const uint64_t inode_bitmap_blocks = 1U;
    const uint64_t inode_table_start = 4U;
    const uint64_t inode_table_blocks = metadata_blocks / 2U;
    const uint64_t journal_start = inode_table_start + inode_table_blocks;
    const uint64_t journal_blocks = metadata_blocks - 2U - inode_table_blocks;
    if (journal_blocks < 4U || journal_start > total - 1U || journal_blocks > (total - 1U) - journal_start)
        return OPENFS_FORMAT_TOO_SMALL;

    openfs_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.version_major = OPENFS_FORMAT_VERSION_MAJOR;
    sb.version_minor = OPENFS_FORMAT_VERSION_MINOR;
    sb.block_size = device->block_size;
    sb.total_blocks = total;
    sb.metadata_start = metadata_start;
    sb.metadata_blocks = metadata_blocks;
    sb.inode_bitmap_start = inode_bitmap_start;
    sb.inode_bitmap_blocks = inode_bitmap_blocks;
    sb.inode_table_start = inode_table_start;
    sb.inode_table_blocks = inode_table_blocks;
    sb.journal_start = journal_start;
    sb.journal_blocks = journal_blocks;
    sb.root_inode = 1U;
    sb.generation = 1U;
    memcpy(sb.uuid, uuid, 16U);

    if (openfs_validate_superblock(device, &sb) != OPENFS_FORMAT_OK)
        return OPENFS_FORMAT_CORRUPT;

    uint8_t *buffer = (uint8_t *)calloc(1U, device->block_size);
    if (buffer == NULL) return OPENFS_FORMAT_IO_ERROR;
    encode(&sb, buffer);

    const uint64_t backup = OPENFS_BACKUP_OFFSET(total);
    const int ok = device->write(device->context, 0U, 1U, buffer) == OPENFS_IO_OK &&
                   device->write(device->context, backup, 1U, buffer) == OPENFS_IO_OK &&
                   device->flush(device->context) == OPENFS_IO_OK;
    free(buffer);
    return ok ? OPENFS_FORMAT_OK : OPENFS_FORMAT_IO_ERROR;
}
