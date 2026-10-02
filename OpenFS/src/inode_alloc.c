#include "openfs/inode_alloc.h"

#include <limits.h>
#include <string.h>
#include "openfs/bitmap.h"

static openfs_inode_alloc_result_t inode_count(
    const openfs_superblock_t *sb,
    uint64_t *count)
{
    if (sb == NULL || count == NULL) return OPENFS_INODE_ALLOC_INVALID_ARGUMENT;
    if (sb->block_size != 0U &&
        sb->inode_table_blocks > UINT64_MAX / sb->block_size) {
        return OPENFS_INODE_ALLOC_CORRUPT;
    }
    *count = (sb->inode_table_blocks * (uint64_t)sb->block_size) / OPENFS_INODE_SIZE;
    return *count == 0U ? OPENFS_INODE_ALLOC_CORRUPT : OPENFS_INODE_ALLOC_OK;
}

static openfs_inode_alloc_result_t valid(
    const openfs_block_device_t *d,
    const openfs_superblock_t *sb)
{
    if (!openfs_block_device_is_valid(d) || sb == NULL) {
        return OPENFS_INODE_ALLOC_INVALID_ARGUMENT;
    }
    if (sb->block_size != d->block_size ||
        sb->inode_bitmap_blocks == 0U ||
        sb->inode_table_blocks == 0U) {
        return OPENFS_INODE_ALLOC_CORRUPT;
    }
    return OPENFS_INODE_ALLOC_OK;
}

openfs_inode_alloc_result_t openfs_inode_alloc(
    openfs_block_device_t *d,
    const openfs_superblock_t *sb,
    uint64_t parent,
    uint32_t mode,
    uint64_t *out)
{
    openfs_inode_alloc_result_t r = valid(d, sb);
    if (r != OPENFS_INODE_ALLOC_OK || out == NULL || parent == 0U) {
        return r != OPENFS_INODE_ALLOC_OK ? r : OPENFS_INODE_ALLOC_INVALID_ARGUMENT;
    }
    if (mode != OPENFS_INODE_MODE_REGULAR &&
        mode != OPENFS_INODE_MODE_DIRECTORY &&
        mode != OPENFS_INODE_MODE_SYMLINK) {
        return OPENFS_INODE_ALLOC_INVALID_ARGUMENT;
    }

    uint64_t count = 0U;
    r = inode_count(sb, &count);
    if (r != OPENFS_INODE_ALLOC_OK) return r;

    if (count < 2U) return OPENFS_INODE_ALLOC_OUT_OF_SPACE;
    for (uint64_t n = 2U; n <= count; ++n) {
        int used = 0;
        if (openfs_bitmap_test(d, sb->inode_bitmap_start, sb->inode_bitmap_blocks, n - 1U, &used) != OPENFS_BITMAP_OK) {
            return OPENFS_INODE_ALLOC_IO_ERROR;
        }
        if (used) continue;

        if (openfs_bitmap_set(d, sb->inode_bitmap_start, sb->inode_bitmap_blocks, n - 1U, 1) != OPENFS_BITMAP_OK) {
            return OPENFS_INODE_ALLOC_IO_ERROR;
        }

        openfs_inode_t inode;
        memset(&inode, 0, sizeof(inode));
        inode.inode_number = n;
        inode.generation = 1U;
        inode.parent_inode = parent;
        inode.link_count = 1U;
        inode.mode = mode;

        if (openfs_inode_write(d, sb->inode_table_start, count, &inode) != OPENFS_INODE_OK) {
            (void)openfs_bitmap_set(d, sb->inode_bitmap_start, sb->inode_bitmap_blocks, n - 1U, 0);
            return OPENFS_INODE_ALLOC_IO_ERROR;
        }
        *out = n;
        return OPENFS_INODE_ALLOC_OK;
    }
    return OPENFS_INODE_ALLOC_OUT_OF_SPACE;
}

openfs_inode_alloc_result_t openfs_inode_free(
    openfs_block_device_t *d,
    const openfs_superblock_t *sb,
    uint64_t n)
{
    openfs_inode_alloc_result_t r = valid(d, sb);
    if (r != OPENFS_INODE_ALLOC_OK || n == 0U) {
        return r != OPENFS_INODE_ALLOC_OK ? r : OPENFS_INODE_ALLOC_INVALID_ARGUMENT;
    }

    uint64_t count = 0U;
    r = inode_count(sb, &count);
    if (r != OPENFS_INODE_ALLOC_OK || n > count) {
        return OPENFS_INODE_ALLOC_OUT_OF_SPACE;
    }
    if (n == sb->root_inode) {
        return OPENFS_INODE_ALLOC_CORRUPT;
    }

    int used = 0;
    if (openfs_bitmap_test(d, sb->inode_bitmap_start, sb->inode_bitmap_blocks, n - 1U, &used) != OPENFS_BITMAP_OK) {
        return OPENFS_INODE_ALLOC_IO_ERROR;
    }
    if (!used) return OPENFS_INODE_ALLOC_CORRUPT;

    openfs_inode_t inode;
    if (openfs_inode_read(d, sb->inode_table_start, n, count, &inode) != OPENFS_INODE_OK) {
        return OPENFS_INODE_ALLOC_CORRUPT;
    }
    if (inode.link_count != 0U || inode.mode != OPENFS_INODE_MODE_FREE) {
        return OPENFS_INODE_ALLOC_CORRUPT;
    }

    if (openfs_bitmap_set(d, sb->inode_bitmap_start, sb->inode_bitmap_blocks, n - 1U, 0) != OPENFS_BITMAP_OK) {
        return OPENFS_INODE_ALLOC_IO_ERROR;
    }
    return OPENFS_INODE_ALLOC_OK;
}
