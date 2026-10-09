#include "openfs/data_checksum.h"
#include "openfs/runtime.h"
#include "openfs/crc32c.h"

#include <stdlib.h>

uint32_t openfs_data_checksum(const void *data, uint32_t length)
{
    return openfs_crc32c(data, length);
}

/*
 * Validate all geometry before doing arithmetic or using the checksum-table
 * offset to index a buffer. These helpers are public and can be called with a
 * caller-provided superblock, not only one returned by mount/format.
 */
static int locate(const openfs_block_device_t *d,
                  const openfs_superblock_t *s,
                  uint64_t block, uint64_t *table_block, uint32_t *offset)
{
    if (!openfs_block_device_is_valid(d) || s == NULL ||
        table_block == NULL || offset == NULL ||
        s->block_size != d->block_size || s->block_size < 4U ||
        (s->block_size % 4U) != 0U ||
        s->total_blocks == 0U || s->total_blocks > d->block_count ||
        s->data_start > s->total_blocks ||
        s->data_blocks > s->total_blocks - s->data_start ||
        s->data_checksum_start > s->total_blocks ||
        s->data_checksum_blocks > s->total_blocks - s->data_checksum_start ||
        s->data_blocks == 0U || s->data_checksum_blocks == 0U ||
        block < s->data_start || block - s->data_start >= s->data_blocks)
        return 0;

    const uint64_t entries_per_block = (uint64_t)s->block_size / 4U;
    const uint64_t required_blocks =
        s->data_blocks / entries_per_block +
        ((s->data_blocks % entries_per_block) != 0U ? 1U : 0U);
    if (s->data_checksum_blocks < required_blocks)
        return 0;

    const uint64_t index = block - s->data_start;
    const uint64_t relative_table_block = index / entries_per_block;
    if (relative_table_block >= s->data_checksum_blocks)
        return 0;

    *table_block = s->data_checksum_start + relative_table_block;
    *offset = (uint32_t)((index % entries_per_block) * 4U);
    return *table_block < d->block_count &&
           *offset <= d->block_size - 4U;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8U);
    p[2] = (uint8_t)(value >> 16U);
    p[3] = (uint8_t)(value >> 24U);
}

int openfs_data_checksum_get(const openfs_block_device_t *d,
                             const openfs_superblock_t *s,
                             uint64_t block, uint32_t *out)
{
    if (!openfs_block_device_is_valid(d) || out == NULL)
        return -1;

    uint64_t table_block;
    uint32_t offset;
    if (!locate(d, s, block, &table_block, &offset))
        return -1;

    uint8_t *buffer = (uint8_t *)malloc(d->block_size);
    if (buffer == NULL)
        return -1;
    const int ok = d->read(d->context, table_block, 1U, buffer) == OPENFS_IO_OK;
    if (ok)
        *out = get32(buffer + offset);
    free(buffer);
    return ok ? 0 : -1;
}

int openfs_data_checksum_set(const openfs_block_device_t *d,
                             const openfs_superblock_t *s,
                             uint64_t block, uint32_t value)
{
    if (!openfs_block_device_is_valid(d) || s == NULL)
        return -1;

    uint64_t table_block;
    uint32_t offset;
    if (!locate(d, s, block, &table_block, &offset))
        return -1;

    uint8_t *buffer = (uint8_t *)malloc(d->block_size);
    if (buffer == NULL)
        return -1;

    openfs_runtime_t *runtime = s->runtime;
    int entered = 0;
    int locked = 0;
    if (runtime != NULL) {
        if (!openfs_runtime_enter(runtime)) {
            free(buffer);
            return -1;
        }
        entered = 1;
        if (openfs_mutex_lock(&runtime->checksum_lock,
                              OPENFS_LOCK_RANK_CHECKSUM) != OPENFS_LOCK_OK) {
            openfs_runtime_leave(runtime);
            free(buffer);
            return -1;
        }
        locked = 1;
    }

    int ok = d->read(d->context, table_block, 1U, buffer) == OPENFS_IO_OK;
    if (ok) {
        put32(buffer + offset, value);
        ok = d->write(d->context, table_block, 1U, buffer) == OPENFS_IO_OK;
    }

    if (locked)
        (void)openfs_mutex_unlock(&runtime->checksum_lock);
    if (entered)
        openfs_runtime_leave(runtime);
    free(buffer);
    return ok ? 0 : -1;
}

int openfs_data_checksum_set_tx(openfs_transaction_t *transaction,
                                const openfs_superblock_t *superblock,
                                uint64_t block, uint32_t value)
{
    openfs_block_device_t *device = openfs_transaction_device(transaction);
    if (device == NULL)
        return -1;
    return openfs_data_checksum_set(device, superblock, block, value);
}
