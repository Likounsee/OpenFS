#include "openfs/extent.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/crc32c.h"

static uint8_t *slot(openfs_inode_t *inode, uint32_t index)
{
    return inode->reserved + (size_t)index * OPENFS_EXTENT_RECORD_SIZE;
}

static uint64_t load64(const uint8_t *p)
{
    uint64_t v = 0U;
    for (unsigned k = 0U; k < 8U; ++k) v |= (uint64_t)p[k] << (8U * k);
    return v;
}

static void store16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8U);
}

static uint16_t load16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8U);
}

static void store32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8U);
    p[2] = (uint8_t)(v >> 16U);
    p[3] = (uint8_t)(v >> 24U);
}

static void store64(uint8_t *p, uint64_t v)
{
    for (unsigned k = 0U; k < 8U; ++k) p[k] = (uint8_t)(v >> (8U * k));
}

static void encode_extent(uint8_t *p, const openfs_extent_t *e)
{
    store64(p, e->logical_start);
    store64(p + 8U, e->physical_start);
    store64(p + 16U, e->block_count);
}

static int decode_extent(const uint8_t *p, openfs_extent_t *e)
{
    e->logical_start = load64(p);
    e->physical_start = load64(p + 8U);
    e->block_count = load64(p + 16U);
    if (e->block_count == 0U ||
        e->logical_start > UINT64_MAX - e->block_count ||
        e->physical_start > UINT64_MAX - e->block_count) {
        return 0;
    }
    return 1;
}

static int validate_extent_order(const openfs_extent_t *e, uint32_t count)
{
    uint64_t previous_end = 0U, previous_physical_end = 0U;
    for (uint32_t n = 0U; n < count; ++n) {
        if (e[n].block_count == 0U ||
            e[n].logical_start > UINT64_MAX - e[n].block_count ||
            e[n].physical_start > UINT64_MAX - e[n].block_count) {
            return 0;
        }
        if (n != 0U && (e[n].logical_start < previous_end || e[n].physical_start < previous_physical_end)) return 0;
        previous_end = e[n].logical_start + e[n].block_count;previous_physical_end = e[n].physical_start + e[n].block_count;
    }
    return 1;
}

openfs_extent_result_t openfs_inode_get_extent(
    const openfs_inode_t *inode,
    uint32_t index,
    openfs_extent_t *out)
{
    if (inode == NULL || out == NULL) return OPENFS_EXTENT_INVALID_ARGUMENT;
    if (index >= inode->extent_count || index >= OPENFS_EXTENT_MAX || ((inode->flags & OPENFS_INODE_FLAG_EXTENT_TREE) != 0U && index >= OPENFS_INODE_TREE_INLINE_EXTENT_MAX)) {
        return OPENFS_EXTENT_OUT_OF_RANGE;
    }
    const uint8_t *p = slot((openfs_inode_t *)inode, index);
    if (!decode_extent(p, out)) return OPENFS_EXTENT_CORRUPT;
    return OPENFS_EXTENT_OK;
}

openfs_extent_result_t openfs_inode_set_extent(
    openfs_inode_t *inode,
    uint32_t index,
    const openfs_extent_t *extent)
{
    if (inode == NULL || extent == NULL) return OPENFS_EXTENT_INVALID_ARGUMENT;
    if (index >= OPENFS_EXTENT_MAX || index > inode->extent_count || ((inode->flags & OPENFS_INODE_FLAG_EXTENT_TREE) != 0U && index >= OPENFS_INODE_TREE_INLINE_EXTENT_MAX) || extent->block_count == 0U) {
        return OPENFS_EXTENT_OUT_OF_RANGE;
    }
    if (extent->logical_start > UINT64_MAX - extent->block_count ||
        extent->physical_start > UINT64_MAX - extent->block_count) {
        return OPENFS_EXTENT_CORRUPT;
    }
    uint8_t old_record[OPENFS_EXTENT_RECORD_SIZE];
    uint32_t old_flags = inode->flags;
    uint32_t old_count = inode->extent_count;
    uint8_t *p = slot(inode, index);
    memcpy(old_record, p, sizeof(old_record));
    encode_extent(p, extent);
    if (index > 0U) {
        openfs_extent_t prev;
        if (!decode_extent(slot(inode, index - 1U), &prev) ||
            prev.logical_start > UINT64_MAX - prev.block_count ||
            prev.physical_start > UINT64_MAX - prev.block_count ||
            extent->logical_start < prev.logical_start + prev.block_count ||
            extent->physical_start < prev.physical_start + prev.block_count) {
            memcpy(p, old_record, sizeof(old_record)); inode->flags = old_flags; inode->extent_count = old_count; return OPENFS_EXTENT_CORRUPT;
        }
    }
    if (index + 1U < old_count) {
        openfs_extent_t next;
        if (!decode_extent(slot(inode, index + 1U), &next) ||
            extent->logical_start > UINT64_MAX - extent->block_count ||
            extent->physical_start > UINT64_MAX - extent->block_count ||
            extent->logical_start + extent->block_count > next.logical_start ||
            extent->physical_start + extent->block_count > next.physical_start) {
            memcpy(p, old_record, sizeof(old_record)); inode->flags = old_flags; inode->extent_count = old_count; return OPENFS_EXTENT_CORRUPT;
        }
    }
    if (index >= inode->extent_count) inode->extent_count = index + 1U;
    inode->flags |= OPENFS_INODE_FLAG_HAS_EXTENTS;
    return OPENFS_EXTENT_OK;
}

uint64_t openfs_inode_get_extent_tree_root(const openfs_inode_t *inode)
{
    if (inode == NULL || (inode->flags & OPENFS_INODE_FLAG_EXTENT_TREE) == 0U) return 0U;
    return load64(inode->reserved + 112U);
}

openfs_extent_result_t openfs_inode_set_extent_tree_root(openfs_inode_t *inode, uint64_t root)
{
    if (inode == NULL || root == 0U) return OPENFS_EXTENT_INVALID_ARGUMENT;
    store64(inode->reserved + 112U, root);
    inode->flags |= OPENFS_INODE_FLAG_EXTENT_TREE;
    inode->flags |= OPENFS_INODE_FLAG_HAS_EXTENTS;
    return OPENFS_EXTENT_OK;
}

uint32_t openfs_extent_tree_capacity(uint32_t block_size)
{
    if (block_size < OPENFS_EXTENT_TREE_HEADER_SIZE + OPENFS_EXTENT_TREE_RECORD_SIZE) return 0U;
    uint64_t cap = (block_size - OPENFS_EXTENT_TREE_HEADER_SIZE) / OPENFS_EXTENT_TREE_RECORD_SIZE;
    return cap > UINT32_MAX ? UINT32_MAX : (uint32_t)cap;
}

openfs_extent_result_t openfs_extent_tree_read(
    const openfs_block_device_t *device,
    const openfs_inode_t *inode,
    uint32_t index,
    openfs_extent_t *out)
{
    if (!openfs_block_device_is_valid(device) || device->block_size==0U || inode == NULL || out == NULL) {
        return OPENFS_EXTENT_INVALID_ARGUMENT;
    }
    uint64_t root = openfs_inode_get_extent_tree_root(inode);
    if (root == 0U || root >= device->block_count) return OPENFS_EXTENT_CORRUPT;
    if (inode->extent_count < OPENFS_INODE_TREE_INLINE_EXTENT_MAX + 1U ||
        (inode->flags & OPENFS_INODE_FLAG_EXTENT_TREE) == 0U) return OPENFS_EXTENT_OUT_OF_RANGE;
    uint32_t cap = openfs_extent_tree_capacity(device->block_size);
    uint64_t tree_count64 = inode->extent_count - OPENFS_INODE_TREE_INLINE_EXTENT_MAX;
    if (cap == 0U || tree_count64 > UINT16_MAX || index >= tree_count64 || index >= cap) return OPENFS_EXTENT_OUT_OF_RANGE;

    uint8_t *block = malloc(device->block_size);
    if (block == NULL) return OPENFS_EXTENT_IO_ERROR;
    if (device->read(device->context, root, 1U, block) != OPENFS_IO_OK) {
        free(block);
        return OPENFS_EXTENT_IO_ERROR;
    }
    if (inode->generation == 0U || memcmp(block, OPENFS_EXTENT_TREE_MAGIC, 8U) != 0 ||
        load16(block + 8U) != 0U ||
        load16(block + 10U) != (uint16_t)tree_count64 ||
        load16(block + 12U) != (uint16_t)cap ||
        load64(block + 16U) != inode->generation) {
        free(block);
        return OPENFS_EXTENT_CORRUPT;
    }
    uint32_t stored = (uint32_t)block[24U] | ((uint32_t)block[25U] << 8U) |
        ((uint32_t)block[26U] << 16U) | ((uint32_t)block[27U] << 24U);
    memset(block + 24U, 0, 4U);
    if (stored != openfs_crc32c(block, device->block_size)) {
        free(block);
        return OPENFS_EXTENT_CORRUPT;
    }
    uint16_t entries = load16(block + 10U);
    if (index >= entries) {
        free(block);
        return OPENFS_EXTENT_OUT_OF_RANGE;
    }
    if (!decode_extent(block + OPENFS_EXTENT_TREE_HEADER_SIZE +
        (size_t)index * OPENFS_EXTENT_TREE_RECORD_SIZE, out)) {
        free(block);
        return OPENFS_EXTENT_CORRUPT;
    }
    free(block);
    return OPENFS_EXTENT_OK;
}

openfs_extent_result_t openfs_extent_tree_write(
    const openfs_block_device_t *device,
    const openfs_inode_t *inode,
    const openfs_extent_t *extents,
    uint32_t count)
{
    if (!openfs_block_device_is_valid(device) || device->block_size==0U || inode == NULL ||
        (count != 0U && extents == NULL)) return OPENFS_EXTENT_INVALID_ARGUMENT;
    uint64_t root = openfs_inode_get_extent_tree_root(inode);
    uint32_t cap = openfs_extent_tree_capacity(device->block_size);
    if (root == 0U || root >= device->block_count || inode->generation==0U || cap == 0U || count > cap || count > UINT16_MAX ||
        inode->extent_count < OPENFS_INODE_TREE_INLINE_EXTENT_MAX + 1U ||
        count != inode->extent_count - OPENFS_INODE_TREE_INLINE_EXTENT_MAX ||
        !validate_extent_order(extents, count)) return OPENFS_EXTENT_CORRUPT;

    uint8_t *block = calloc(1U, device->block_size);
    if (block == NULL) return OPENFS_EXTENT_IO_ERROR;
    memcpy(block, OPENFS_EXTENT_TREE_MAGIC, 8U);
    store16(block + 8U, 0U);
    store16(block + 10U, (uint16_t)count);
    store16(block + 12U, (uint16_t)cap);
    store16(block + 14U, 0U);
    store64(block + 16U, inode->generation);
    for (uint32_t n = 0U; n < count; ++n) {
        encode_extent(block + OPENFS_EXTENT_TREE_HEADER_SIZE +
            (size_t)n * OPENFS_EXTENT_TREE_RECORD_SIZE, &extents[n]);
    }
    store32(block + 24U, 0U);
    store32(block + 24U, openfs_crc32c(block, device->block_size));
    openfs_io_result_t io = device->write(device->context, root, 1U, block);
    free(block);
    return io == OPENFS_IO_OK ? OPENFS_EXTENT_OK : OPENFS_EXTENT_IO_ERROR;
}
