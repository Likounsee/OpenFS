#include "openfs/extent.h"

#include <limits.h>

static uint8_t *slot(openfs_inode_t *inode, uint32_t index)
{
    return inode->reserved + (size_t)index * OPENFS_EXTENT_RECORD_SIZE;
}

openfs_extent_result_t openfs_inode_get_extent(
    const openfs_inode_t *inode,
    uint32_t index,
    openfs_extent_t *out)
{
    if (inode == NULL || out == NULL) {
        return OPENFS_EXTENT_INVALID_ARGUMENT;
    }
    if (index >= inode->extent_count || index >= OPENFS_EXTENT_MAX) {
        return OPENFS_EXTENT_OUT_OF_RANGE;
    }

    const uint8_t *p = slot((openfs_inode_t *)inode, index);
    uint64_t logical = 0U;
    uint64_t physical = 0U;
    uint64_t count = 0U;
    for (unsigned k = 0U; k < 8U; ++k) {
        logical |= (uint64_t)p[k] << (8U * k);
        physical |= (uint64_t)p[8U + k] << (8U * k);
        count |= (uint64_t)p[16U + k] << (8U * k);
    }
    if (count == 0U ||
        logical > UINT64_MAX - count ||
        physical > UINT64_MAX - count) {
        return OPENFS_EXTENT_CORRUPT;
    }

    out->logical_start = logical;
    out->physical_start = physical;
    out->block_count = count;
    return OPENFS_EXTENT_OK;
}

openfs_extent_result_t openfs_inode_set_extent(
    openfs_inode_t *inode,
    uint32_t index,
    const openfs_extent_t *extent)
{
    if (inode == NULL || extent == NULL) {
        return OPENFS_EXTENT_INVALID_ARGUMENT;
    }
    if (index >= OPENFS_EXTENT_MAX || extent->block_count == 0U) {
        return OPENFS_EXTENT_OUT_OF_RANGE;
    }
    if (extent->logical_start > UINT64_MAX - extent->block_count ||
        extent->physical_start > UINT64_MAX - extent->block_count) {
        return OPENFS_EXTENT_CORRUPT;
    }

    uint8_t *p = slot(inode, index);
    for (unsigned k = 0U; k < 8U; ++k) {
        p[k] = (uint8_t)(extent->logical_start >> (8U * k));
        p[8U + k] = (uint8_t)(extent->physical_start >> (8U * k));
        p[16U + k] = (uint8_t)(extent->block_count >> (8U * k));
    }

    if (index >= inode->extent_count) {
        inode->extent_count = index + 1U;
    }
    inode->flags |= OPENFS_INODE_FLAG_HAS_EXTENTS;
    return OPENFS_EXTENT_OK;
}
