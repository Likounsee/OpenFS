#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "openfs/format.h"
#include "openfs/journal.h"

typedef struct {
    uint8_t *data;
    uint32_t block_size;
    uint64_t block_count;
    int fail_next_write;
    int fail_write_after;
} disk_t;

static openfs_io_result_t read_blocks(void *ctx, uint64_t first, uint32_t count, void *out)
{
    disk_t *d = ctx;
    if (d == NULL || out == NULL || count == 0U ||
        first >= d->block_count || (uint64_t)count > d->block_count - first) {
        return OPENFS_IO_OUT_OF_RANGE;
    }
    memcpy(out, d->data + (size_t)(first * d->block_size),
           (size_t)((uint64_t)count * d->block_size));
    return OPENFS_IO_OK;
}

static openfs_io_result_t write_blocks(void *ctx, uint64_t first, uint32_t count, const void *in)
{
    disk_t *d = ctx;
    if (d == NULL || in == NULL || count == 0U ||
        first >= d->block_count || (uint64_t)count > d->block_count - first) {
        return OPENFS_IO_OUT_OF_RANGE;
    }
    if (d->fail_next_write) {
        d->fail_next_write = 0;
        return OPENFS_IO_IO_ERROR;
    }
    if (d->fail_write_after > 0) {
        d->fail_write_after--;
        if (d->fail_write_after == 0) return OPENFS_IO_IO_ERROR;
    }
    memcpy(d->data + (size_t)(first * d->block_size), in,
           (size_t)((uint64_t)count * d->block_size));
    return OPENFS_IO_OK;
}

static openfs_io_result_t flush_blocks(void *ctx)
{
    (void)ctx;
    return OPENFS_IO_OK;
}

static openfs_block_device_t device(disk_t *d)
{
    openfs_block_device_t v = {
        d, d->block_size, d->block_count,
        read_blocks, write_blocks, flush_blocks
    };
    return v;
}

static void test_data_failure_restores_sequence(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);

    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x62U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);

    openfs_superblock_t s;
    assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);

    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);

    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    uint64_t sequence_before = j.sequence;

    d.fail_next_write = 1;
    assert(openfs_journal_write(&j, &v, tx, "data", 4U) == OPENFS_JOURNAL_IO_ERROR);
    assert(j.sequence == sequence_before);

    assert(openfs_journal_write(&j, &v, tx, "data", 4U) == OPENFS_JOURNAL_OK);

    openfs_journal_t reopened;
    assert(openfs_journal_open(&reopened, &v, &s) == OPENFS_JOURNAL_OK);

    free(d.data);
}

static void test_begin_failure_restores_sequence(void)
{
    disk_t d = {0};
    d.block_size = 4096U; d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count); assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x63U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s; assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j; assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U;
    assert(j.sequence == 0U);
    d.fail_next_write = 1;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_IO_ERROR);
    assert(j.sequence == 0U);
    assert(j.transaction_id == 0U);
    assert(j.active_transaction_id == 0U);
    assert(j.next_record == 0U);
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    assert(tx == 1U);
    assert(j.sequence == 1U);
    free(d.data);
}


static void test_block_write_partial_failure_is_abortable(void)
{
    disk_t d = {0};
    d.block_size = 4096U; d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x67U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s; assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j; assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U; assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    uint8_t block[4096]; memset(block, 0xD8U, sizeof(block));
    uint64_t sequence_before = j.sequence;
    uint64_t record_before = j.next_record;
    d.fail_write_after = 2;
    assert(openfs_journal_write_block(&j, &v, tx, s.data_start + 9U, block) == OPENFS_JOURNAL_IO_ERROR);
    assert(j.active_transaction_id == tx);
    assert(j.next_record == record_before + 1U);
    assert(j.sequence == sequence_before + 1U);
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_INVALID_ARGUMENT);
    j.active_transaction_id = 0U;
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_OK);
    free(d.data);
}

static void test_block_write_failure_keeps_partial_transaction_abortable(void)
{
    disk_t d = {0};
    d.block_size = 4096U; d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x66U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s; assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j; assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U; assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    uint8_t block[4096]; memset(block, 0xC7U, sizeof(block));
    uint64_t sequence_before = j.sequence;
    uint64_t record_before = j.next_record;
    d.fail_next_write = 1;
    assert(openfs_journal_write_block(&j, &v, tx, s.data_start + 8U, block) == OPENFS_JOURNAL_IO_ERROR);
    assert(j.active_transaction_id == tx);
    assert(j.next_record == record_before);
    assert(j.sequence == sequence_before);
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_INVALID_ARGUMENT);
    j.active_transaction_id = 0U;
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_OK);
    assert(j.next_record == 0U);
    free(d.data);
}

int main(void)
{
    test_data_failure_restores_sequence();
    test_begin_failure_restores_sequence();
    test_block_write_failure_keeps_partial_transaction_abortable();
    test_block_write_partial_failure_is_abortable();
    return 0;
}
