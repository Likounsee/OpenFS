#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "openfs/format.h"
#include "openfs/journal.h"
#include "openfs/crc32c.h"
#include "openfs/runtime.h"
#include "openfs/transaction.h"

typedef struct {
    uint8_t *data;
    uint32_t block_size;
    uint64_t block_count;
    int fail_next_write;
    int fail_write_count;
    int fail_write_after;
    int fail_next_flush;
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
    if (d->fail_write_count > 0) {
        d->fail_write_count--;
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
    disk_t *d = ctx;
    if (d->fail_next_flush) {
        d->fail_next_flush = 0;
        return OPENFS_IO_IO_ERROR;
    }
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


static openfs_journal_result_t replay_noop(void *, uint64_t, const uint8_t *, uint32_t);



static void test_data_restore_failure_requires_recovery(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);

    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x6BU};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t sb;
    assert(openfs_read_superblock(&v, &sb) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &sb) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);

    d.fail_write_count = 2; /* Fail DATA write and restoration of its slot. */
    assert(openfs_journal_write(&j, &v, tx, "payload", 7U) == OPENFS_JOURNAL_CORRUPT);
    assert(j.recovery_required != 0U);
    assert(openfs_journal_write(&j, &v, tx, "retry", 5U) == OPENFS_JOURNAL_IO_ERROR);
    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_IO_ERROR);
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_IO_ERROR);
    free(d.data);
}

static void test_begin_restore_failure_requires_recovery(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);

    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x6AU};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);

    uint64_t tx = 0U;
    d.fail_write_count = 2; /* Fail BEGIN write and its compensating restore. */
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_CORRUPT);
    assert(j.recovery_required != 0U);
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_IO_ERROR);
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_IO_ERROR);
    free(d.data);
}

static void test_commit_flush_failure_requires_recovery(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);

    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x69U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);

    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_write(&j, &v, tx, "committed", 9U) == OPENFS_JOURNAL_OK);

    /* The COMMIT record reaches the device, but its durability cannot be confirmed. */
    d.fail_next_flush = 1;
    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_IO_ERROR);
    assert(j.commit_record_written != 0U);
    assert(j.recovery_required != 0U);
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_IO_ERROR);
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_IO_ERROR);

    /* Reopening re-evaluates the on-disk log instead of erasing an uncertain commit. */
    openfs_journal_t reopened;
    assert(openfs_journal_open(&reopened, &v, &s) == OPENFS_JOURNAL_OK);
    assert(reopened.recovery_required != 0U);
    assert(openfs_journal_checkpoint(&reopened, &v) == OPENFS_JOURNAL_IO_ERROR);
    assert(openfs_journal_replay(&v, &s, replay_noop, NULL) == OPENFS_JOURNAL_OK);
    free(d.data);
}

static void test_reopen_uncommitted_wal_requires_replay(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x6AU};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_write(&j, &v, tx, "uncommitted", 11U) == OPENFS_JOURNAL_OK);

    openfs_journal_t reopened;
    assert(openfs_journal_open(&reopened, &v, &s) == OPENFS_JOURNAL_OK);
    assert(reopened.commit_record_written == 0U);
    assert(reopened.recovery_required != 0U);
    assert(openfs_journal_checkpoint(&reopened, &v) == OPENFS_JOURNAL_IO_ERROR);
    assert(openfs_journal_replay(&v, &s, replay_noop, NULL) == OPENFS_JOURNAL_OK);
    free(d.data);
}

static openfs_journal_result_t replay_noop(void *ctx, uint64_t tx, const uint8_t *data, uint32_t len)
{
    (void)ctx; (void)tx; (void)data; (void)len;
    return OPENFS_JOURNAL_OK;
}

static openfs_journal_result_t replay_fail(void *ctx, uint64_t tx, const uint8_t *data, uint32_t len)
{
    (void)ctx; (void)tx; (void)data; (void)len;
    return OPENFS_JOURNAL_IO_ERROR;
}

static void test_failed_transaction_can_be_retired_before_recovery(void)
{
    disk_t d = {0};
    d.block_size = 4096U; d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x6FU};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s; assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j; assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    openfs_runtime_t runtime;
    assert(openfs_runtime_init(&runtime));
    j.runtime = &runtime;

    openfs_transaction_t tx;
    assert(openfs_transaction_begin(&tx, &v, &j) == OPENFS_TRANSACTION_OK);
    openfs_block_device_t *td = openfs_transaction_device(&tx);
    assert(td != NULL);
    uint8_t block[4096]; memset(block, 0xA7U, sizeof(block));

    /* Fail the DATA write and its compensating restore. */
    d.fail_write_count = 2;
    assert(td->write(td->context, s.data_start + 12U, 1U, block) == OPENFS_IO_IO_ERROR);
    assert(j.recovery_required != 0U);
    assert(j.active_transaction_id == tx.txid);
    assert(openfs_transaction_abort(&tx) == OPENFS_TRANSACTION_CORRUPT);
    assert(tx.active == 0);
    assert(j.active_transaction_id == 0U);

    /* A failed transaction must not permanently block replay on this journal. */
    assert(openfs_journal_recover(&j, &v, &s, replay_noop, NULL) == OPENFS_JOURNAL_OK);
    assert(j.recovery_required == 0U);
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_OK);
    assert(openfs_runtime_shutdown_if_unused(&runtime));
    free(d.data);
}

static void test_recovery_failure_keeps_checkpoint_blocked(void)
{
    disk_t d = {0};
    d.block_size = 4096U; d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x6DU};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s; assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j; assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_write(&j, &v, tx, "recover-me", 10U) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_OK);

    openfs_journal_t reopened;
    assert(openfs_journal_open(&reopened, &v, &s) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_recover(&reopened, &v, &s, replay_fail, NULL) == OPENFS_JOURNAL_IO_ERROR);
    assert(reopened.recovery_required != 0U);
    assert(openfs_journal_checkpoint(&reopened, &v) == OPENFS_JOURNAL_IO_ERROR);
    assert(openfs_journal_recover(&reopened, &v, &s, replay_noop, NULL) == OPENFS_JOURNAL_OK);
    assert(reopened.recovery_required == 0U);
    assert(openfs_journal_checkpoint(&reopened, &v) == OPENFS_JOURNAL_OK);
    free(d.data);
}

static openfs_journal_result_t replay_count(void *ctx, uint64_t tx, const uint8_t *data, uint32_t len)
{
    unsigned *calls = (unsigned *)ctx;
    (void)tx; (void)data; (void)len;
    (*calls)++;
    return OPENFS_JOURNAL_OK;
}

static void test_recovery_refuses_live_transaction_before_replay(void)
{
    disk_t d = {0};
    d.block_size = 4096U; d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x6CU};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s; assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j; assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_write(&j, &v, tx, "live", 4U) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_OK);
    /* Simulate a stale in-memory live-transaction marker over a committed WAL. */
    j.active_transaction_id = tx;

    unsigned replay_calls = 0U;
    assert(openfs_journal_recover(&j, &v, &s, replay_count, &replay_calls) ==
           OPENFS_JOURNAL_INVALID_ARGUMENT);
    assert(replay_calls == 0U);
    assert(j.active_transaction_id == tx);
    assert(j.recovery_required == 0U);
    j.active_transaction_id = 0U;
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_OK);
    free(d.data);
}

typedef struct {
    openfs_runtime_t *runtime;
    int shutdown_result;
    unsigned replay_calls;
} recovery_runtime_ctx_t;

static openfs_journal_result_t replay_attempt_runtime_shutdown(void *ctx, uint64_t tx,
                                                               const uint8_t *data, uint32_t len)
{
    recovery_runtime_ctx_t *state = (recovery_runtime_ctx_t *)ctx;
    (void)tx; (void)data; (void)len;
    state->replay_calls++;
    state->shutdown_result = openfs_runtime_shutdown_if_unused(state->runtime);
    return OPENFS_JOURNAL_OK;
}

static void test_recovery_holds_runtime_admission_until_replay_finishes(void)
{
    disk_t d = {0};
    d.block_size = 4096U; d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x6EU};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s; assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j; assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_write(&j, &v, tx, "runtime-check", 13U) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_OK);

    openfs_journal_t reopened;
    assert(openfs_journal_open(&reopened, &v, &s) == OPENFS_JOURNAL_OK);
    openfs_runtime_t runtime;
    assert(openfs_runtime_init(&runtime));
    reopened.runtime = &runtime;
    recovery_runtime_ctx_t ctx = { &runtime, -1, 0U };
    assert(openfs_journal_recover(&reopened, &v, &s,
                                  replay_attempt_runtime_shutdown, &ctx) == OPENFS_JOURNAL_OK);
    assert(ctx.replay_calls == 1U);
    assert(ctx.shutdown_result == 0);
    assert(reopened.recovery_required == 0U);
    assert(openfs_journal_checkpoint(&reopened, &v) == OPENFS_JOURNAL_OK);
    assert(openfs_runtime_shutdown_if_unused(&runtime));
    free(d.data);
}

static void test_replay_rejects_interleaved_transactions(void)
{
    disk_t d = {0};
    d.block_size = 4096U; d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x68U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s; assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j; assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U; assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_write(&j, &v, tx, "a", 1U) == OPENFS_JOURNAL_OK);
    uint64_t seq = j.sequence + 1U;
    uint8_t block[4096]; memset(block, 0, sizeof(block));
    memcpy(block, OPENFS_JOURNAL_MAGIC, 5U);
    block[5] = OPENFS_JOURNAL_BEGIN;
    uint64_t tx2 = tx + 1U;
    for(unsigned i=0U;i<8U;i++){block[8U+i]=(uint8_t)(tx2>>(8U*i));block[16U+i]=(uint8_t)(seq>>(8U*i));}
    uint32_t crc = openfs_crc32c(block, sizeof(block));
    block[28U]=(uint8_t)crc; block[29U]=(uint8_t)(crc>>8U); block[30U]=(uint8_t)(crc>>16U); block[31U]=(uint8_t)(crc>>24U);
    assert(v.write(v.context, s.journal_start + j.next_record, 1U, block) == OPENFS_IO_OK);
    memset(block, 0, sizeof(block)); memcpy(block, OPENFS_JOURNAL_MAGIC, 5U); block[5] = OPENFS_JOURNAL_DATA;
    for(unsigned i=0U;i<8U;i++){block[8U+i]=(uint8_t)(tx>>(8U*i));block[16U+i]=(uint8_t)((seq+1U)>>(8U*i));}
    block[24U]=1U; block[32U]=98U; crc = openfs_crc32c(block, sizeof(block));
    block[28U]=(uint8_t)crc; block[29U]=(uint8_t)(crc>>8U); block[30U]=(uint8_t)(crc>>16U); block[31U]=(uint8_t)(crc>>24U);
    assert(v.write(v.context, s.journal_start + j.next_record + 1U, 1U, block) == OPENFS_IO_OK);
    assert(openfs_journal_replay(&v, &s, replay_noop, NULL) == OPENFS_JOURNAL_CORRUPT);
    free(d.data);
}

int main(void)
{
    test_data_failure_restores_sequence();
    test_begin_failure_restores_sequence();
    test_block_write_failure_keeps_partial_transaction_abortable();
    test_block_write_partial_failure_is_abortable();
    test_replay_rejects_interleaved_transactions();
    test_recovery_failure_keeps_checkpoint_blocked();
    test_failed_transaction_can_be_retired_before_recovery();
    test_recovery_refuses_live_transaction_before_replay();
    test_recovery_holds_runtime_admission_until_replay_finishes();
    test_commit_flush_failure_requires_recovery();
    test_reopen_uncommitted_wal_requires_replay();
    test_begin_restore_failure_requires_recovery();
    test_data_restore_failure_requires_recovery();
    return 0;
}
