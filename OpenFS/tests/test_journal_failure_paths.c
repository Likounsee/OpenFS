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
    int partial_write;
    size_t partial_bytes;
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
    if (d->partial_write) {
        d->partial_write = 0;
        size_t bytes=(size_t)((uint64_t)count*d->block_size);
        if(d->partial_bytes!=0U&&d->partial_bytes<bytes)bytes=d->partial_bytes;
        memcpy(d->data+(size_t)(first*d->block_size),in,bytes);
        return OPENFS_IO_IO_ERROR;
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



static void test_rejected_append_restores_sequence(void)
{
    disk_t d={0};
    d.block_size=4096U; d.block_count=256U;
    d.data=calloc((size_t)d.block_size,(size_t)d.block_count);
    assert(d.data!=NULL);
    openfs_block_device_t v=device(&d);
    uint8_t uuid[16]={0x65U};
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_superblock_t s; assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    openfs_journal_t j; assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    uint64_t tx=0U; assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);
    uint64_t sequence_before=j.sequence;
    uint8_t *oversized=malloc((size_t)j.block_size);
    assert(oversized!=NULL);
    memset(oversized,0xA9U,(size_t)j.block_size);
    assert(openfs_journal_write(&j,&v,tx,oversized,j.block_size)==OPENFS_JOURNAL_INVALID_ARGUMENT);
    assert(j.sequence==sequence_before&&j.next_record==1U);
    free(oversized);
    assert(openfs_journal_write(&j,&v,tx,"ok",2U)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);
    free(d.data);
}

static void test_partial_append_is_rolled_back(void)
{
    disk_t d={0};
    d.block_size=4096U; d.block_count=256U;
    d.data=calloc((size_t)d.block_size,(size_t)d.block_count);
    assert(d.data!=NULL);
    openfs_block_device_t v=device(&d);
    uint8_t uuid[16]={0x64U};
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_superblock_t s; assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    openfs_journal_t j; assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    uint64_t tx=0U;
    d.partial_write=1; d.partial_bytes=512U;
    assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_IO_ERROR);
    assert(j.sequence==0U&&j.transaction_id==0U&&j.next_record==0U);
    openfs_journal_t reopened;
    assert(openfs_journal_open(&reopened,&v,&s)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK&&tx==1U);
    d.partial_write=1; d.partial_bytes=1024U;
    uint64_t sequence_before=j.sequence;
    assert(openfs_journal_write(&j,&v,tx,"data",4U)==OPENFS_JOURNAL_IO_ERROR);
    assert(j.sequence==sequence_before&&j.next_record==1U);
    assert(openfs_journal_write(&j,&v,tx,"data",4U)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);
    free(d.data);
}

int main(void)
{
    test_data_failure_restores_sequence();
    test_begin_failure_restores_sequence();
    test_partial_append_is_rolled_back();
    test_rejected_append_restores_sequence();
    return 0;
}
