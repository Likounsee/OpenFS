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
    int partial_next_write;
    size_t partial_write_bytes;
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
    if (d->partial_next_write) {
        size_t bytes = (size_t)((uint64_t)count * d->block_size);
        size_t partial = d->partial_write_bytes;
        d->partial_next_write = 0;
        if (partial == 0U || partial >= bytes) partial = bytes / 2U;
        memcpy(d->data + (size_t)(first * d->block_size), in, partial);
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


static void test_begin_partial_write_restores_journal_slot(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x64U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);

    uint8_t *slot_before = malloc(d.block_size);
    assert(slot_before != NULL);
    uint64_t slot = s.journal_start;
    assert(v.read(v.context, slot, 1U, slot_before) == OPENFS_IO_OK);
    uint64_t tx = 0U;
    d.partial_next_write = 1;
    d.partial_write_bytes = 113U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_IO_ERROR);
    uint8_t *slot_after = malloc(d.block_size);
    assert(slot_after != NULL);
    assert(v.read(v.context, slot, 1U, slot_after) == OPENFS_IO_OK);
    assert(memcmp(slot_before, slot_after, d.block_size) == 0);
    assert(j.sequence == 0U && j.transaction_id == 0U);
    assert(j.next_record == 0U && j.active_transaction_id == 0U);
    assert(j.recovery_required == 0U);

    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    assert(tx == 1U && j.sequence == 1U);
    free(slot_after);
    free(slot_before);
    free(d.data);
}

static void test_partial_begin_and_failed_rollback_poison_journal(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x66U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);

    /* The first write is partial; the following rollback write also fails. */
    d.partial_next_write = 1;
    d.partial_write_bytes = 17U;
    d.fail_write_after = 1;
    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_CORRUPT);
    assert(j.recovery_required != 0U);
    assert(j.next_record == 0U && j.active_transaction_id == 0U);
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_IO_ERROR);

    /* A restart must not accept the damaged first WAL slot as a valid log. */
    openfs_journal_t reopened;
    assert(openfs_journal_open(&reopened, &v, &s) == OPENFS_JOURNAL_CORRUPT);
    free(d.data);
}

static void test_partial_begin_rollback_flush_failure_poison_journal(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x68U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);

    /*
     * The BEGIN write is partial. Restoring the previous slot succeeds, but
     * its flush fails, so the in-memory journal must remain recovery-gated.
     */
    d.partial_next_write = 1;
    d.partial_write_bytes = 17U;
    d.fail_next_flush = 1;
    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_CORRUPT);
    assert(j.recovery_required != 0U);
    assert(j.next_record == 0U && j.active_transaction_id == 0U);
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_IO_ERROR);
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_IO_ERROR);

    free(d.data);
}

static void test_partial_data_rollback_flush_failure_poison_journal(void)
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

    /* The DATA write is partial; rollback's flush then fails. */
    d.partial_next_write = 1;
    d.partial_write_bytes = 33U;
    d.fail_next_flush = 1;
    assert(openfs_journal_write(&j, &v, tx, "rollback-flush", 14U) ==
           OPENFS_JOURNAL_CORRUPT);
    assert(j.recovery_required != 0U);
    assert(j.active_transaction_id == tx);
    assert(openfs_journal_write(&j, &v, tx, "retry", 5U) == OPENFS_JOURNAL_IO_ERROR);
    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_IO_ERROR);

    free(d.data);
}

static void test_partial_data_and_failed_rollback_poison_journal(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x67U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);

    d.partial_next_write = 1;
    d.partial_write_bytes = 33U;
    d.fail_write_after = 1;
    assert(openfs_journal_write(&j, &v, tx, "partial-data", 12U) ==
           OPENFS_JOURNAL_CORRUPT);
    assert(j.recovery_required != 0U);
    assert(j.active_transaction_id == tx);
    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_IO_ERROR);

    openfs_journal_t reopened;
    assert(openfs_journal_open(&reopened, &v, &s) == OPENFS_JOURNAL_CORRUPT);
    free(d.data);
}

static void test_data_partial_write_restores_journal_slot(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x65U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);

    uint8_t *slot_before = malloc(d.block_size);
    uint8_t *slot_after = malloc(d.block_size);
    assert(slot_before != NULL && slot_after != NULL);
    uint64_t slot = s.journal_start + j.next_record;
    assert(v.read(v.context, slot, 1U, slot_before) == OPENFS_IO_OK);
    uint64_t sequence_before = j.sequence;
    uint64_t record_before = j.next_record;
    d.partial_next_write = 1;
    d.partial_write_bytes = 127U;
    assert(openfs_journal_write(&j, &v, tx, "partial-data", 12U) == OPENFS_JOURNAL_IO_ERROR);
    assert(v.read(v.context, slot, 1U, slot_after) == OPENFS_IO_OK);
    assert(memcmp(slot_before, slot_after, d.block_size) == 0);
    assert(j.sequence == sequence_before && j.next_record == record_before);
    assert(j.active_transaction_id == tx && j.recovery_required == 0U);

    assert(openfs_journal_write(&j, &v, tx, "partial-data", 12U) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_OK);
    free(slot_after);
    free(slot_before);
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



typedef struct { unsigned hits; uint8_t effect[7]; } replay_flush_state_t;

static openfs_journal_result_t replay_count_effect(void *ctx, uint64_t tx,
                                                    const uint8_t *data, uint32_t len)
{
    replay_flush_state_t *state = (replay_flush_state_t *)ctx;
    if (state == NULL || tx != 1U || len != 7U ||
        memcmp(data, "payload", 7U) != 0) {
        return OPENFS_JOURNAL_CORRUPT;
    }
    state->hits++;
    /* Model an idempotent replay effect: overwrite the same logical value. */
    memcpy(state->effect, data, len);
    return OPENFS_JOURNAL_OK;
}

/*
 * A callback can apply its effects before replay's final flush fails.
 * Recovery must stay gated, and retry may invoke the callback again.
 */
static void test_replay_flush_failure_keeps_recovery_gated(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);

    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x6FU};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t sb;
    assert(openfs_read_superblock(&v, &sb) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &sb) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_write(&j, &v, tx, "payload", 7U) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_OK);

    replay_flush_state_t state = {0U};
    d.fail_next_flush = 1;
    assert(openfs_journal_recover(&j, &v, &sb, replay_count_effect, &state) ==
           OPENFS_JOURNAL_IO_ERROR);
    assert(state.hits == 1U);
    assert(memcmp(state.effect, "payload", 7U) == 0);
    assert(j.recovery_required == 1U);
    uint64_t blocked_tx = 0U;
    assert(openfs_journal_begin(&j, &v, &blocked_tx) == OPENFS_JOURNAL_IO_ERROR);
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_IO_ERROR);

    assert(openfs_journal_recover(&j, &v, &sb, replay_count_effect, &state) ==
           OPENFS_JOURNAL_OK);
    assert(state.hits == 2U);
    assert(memcmp(state.effect, "payload", 7U) == 0);
    assert(j.recovery_required == 0U);
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_OK);
    free(d.data);
}

static openfs_journal_result_t replay_noop(void *, uint64_t, const uint8_t *, uint32_t);
static openfs_journal_result_t replay_count(void *, uint64_t, const uint8_t *, uint32_t);



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

typedef struct { uint32_t hits; } committed_replay_state_t;
static openfs_journal_result_t replay_committed_payload(void *ctx,uint64_t tx,const uint8_t *data,uint32_t len)
{
    committed_replay_state_t *state=(committed_replay_state_t *)ctx;
    if(state==NULL||tx!=1U||len!=9U||memcmp(data,"committed",9U)!=0)return OPENFS_JOURNAL_CORRUPT;
    state->hits++;
    return OPENFS_JOURNAL_OK;
}

static void test_partial_commit_write_restores_slot_and_can_retry(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x68U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_write(&j, &v, tx, "retryable", 9U) == OPENFS_JOURNAL_OK);

    uint64_t slot = s.journal_start + j.next_record;
    uint8_t *before = malloc(d.block_size);
    uint8_t *after = malloc(d.block_size);
    assert(before != NULL && after != NULL);
    assert(v.read(v.context, slot, 1U, before) == OPENFS_IO_OK);
    uint64_t sequence_before = j.sequence;
    uint64_t records_before = j.next_record;
    d.partial_next_write = 1;
    d.partial_write_bytes = 29U;
    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_IO_ERROR);
    assert(v.read(v.context, slot, 1U, after) == OPENFS_IO_OK);
    assert(memcmp(before, after, d.block_size) == 0);
    assert(j.sequence == sequence_before && j.next_record == records_before);
    assert(j.active_transaction_id == tx && j.recovery_required == 0U);

    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_OK);
    assert(j.active_transaction_id == 0U && j.commit_record_written != 0U);
    assert(j.next_record == records_before + 1U);
    assert(openfs_journal_recover(&j, &v, &s, replay_noop, NULL) == OPENFS_JOURNAL_OK);
    free(after);
    free(before);
    free(d.data);
}

static void test_partial_commit_rollback_flush_failure_poison_journal(void)
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
    assert(openfs_journal_write(&j, &v, tx, "before-commit", 13U) ==
           OPENFS_JOURNAL_OK);

    /* The COMMIT write is partial; restoring its slot cannot be confirmed durable. */
    d.partial_next_write = 1;
    d.partial_write_bytes = 17U;
    d.fail_next_flush = 1;
    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_CORRUPT);
    assert(j.recovery_required != 0U);
    assert(j.active_transaction_id == tx);
    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_IO_ERROR);
    assert(openfs_journal_write(&j, &v, tx, "blocked", 7U) == OPENFS_JOURNAL_IO_ERROR);
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
    committed_replay_state_t replay_state = {0U};
    assert(openfs_journal_recover(&reopened, &v, &s,
                                  replay_committed_payload, &replay_state) ==
           OPENFS_JOURNAL_OK);
    assert(replay_state.hits == 1U);
    assert(reopened.recovery_required == 0U);
    assert(openfs_journal_checkpoint(&reopened, &v) == OPENFS_JOURNAL_OK);
    assert(reopened.next_record == 0U);
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
    unsigned replay_calls = 0U;
    assert(openfs_journal_replay(&v, &s, replay_count, &replay_calls) == OPENFS_JOURNAL_OK);
    assert(replay_calls == 0U); /* An uncommitted DATA record must never be applied. */
    assert(openfs_journal_recover(&reopened, &v, &s, replay_noop, NULL) == OPENFS_JOURNAL_OK);
    assert(reopened.recovery_required == 0U);
    assert(openfs_journal_checkpoint(&reopened, &v) == OPENFS_JOURNAL_OK);
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
    openfs_journal_t *journal;
    openfs_block_device_t *device;
    int shutdown_result;
    openfs_journal_result_t begin_result;
    unsigned replay_calls;
} recovery_runtime_ctx_t;

static openfs_journal_result_t replay_attempt_runtime_shutdown(void *ctx, uint64_t tx,
                                                               const uint8_t *data, uint32_t len)
{
    recovery_runtime_ctx_t *state = (recovery_runtime_ctx_t *)ctx;
    (void)tx; (void)data; (void)len;
    state->replay_calls++;
    state->shutdown_result = openfs_runtime_shutdown_if_unused(state->runtime);
    uint64_t attempted_tx = 0U;
    state->begin_result = openfs_journal_begin(state->journal, state->device, &attempted_tx);
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
    recovery_runtime_ctx_t ctx = { &runtime, &reopened, &v, -1,
                                          OPENFS_JOURNAL_OK, 0U };
    assert(openfs_journal_recover(&reopened, &v, &s,
                                  replay_attempt_runtime_shutdown, &ctx) == OPENFS_JOURNAL_OK);
    assert(ctx.replay_calls == 1U);
    assert(ctx.shutdown_result == 0);
    /* Replay callbacks run outside the journal mutex, but the recovery gate
     * must still reject a concurrent/new transaction until replay completes. */
    assert(ctx.begin_result == OPENFS_JOURNAL_IO_ERROR);
    assert(reopened.active_transaction_id == 0U);
    assert(reopened.recovery_required == 0U);
    assert(openfs_journal_checkpoint(&reopened, &v) == OPENFS_JOURNAL_OK);
    /* A successful recovery must release the gate for later transactions. */
    uint64_t post_recovery_tx = 0U;
    assert(openfs_journal_begin(&reopened, &v, &post_recovery_tx) == OPENFS_JOURNAL_OK);
    assert(post_recovery_tx == 2U);
    assert(openfs_journal_commit(&reopened, &v, post_recovery_tx) == OPENFS_JOURNAL_OK);
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


static void test_partial_commit_write_and_failed_restore_stay_gated(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x74U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);

    openfs_transaction_t transaction;
    assert(openfs_transaction_begin(&transaction, &v, &j) == OPENFS_TRANSACTION_OK);
    openfs_block_device_t *td = openfs_transaction_device(&transaction);
    assert(td != NULL);
    uint8_t block[4096];
    memset(block, 0xA4U, sizeof(block));
    assert(td->write(td->context, s.data_start + 13U, 1U, block) == OPENFS_IO_OK);

    /* Leave a partial COMMIT header on disk, then fail its compensating restore. */
    d.partial_next_write = 1;
    d.partial_write_bytes = 24U;
    d.fail_write_count = 1;
    assert(openfs_transaction_commit(&transaction) == OPENFS_TRANSACTION_CORRUPT);
    assert(j.recovery_required != 0U);
    /* The pending filesystem block must not be published without a valid COMMIT. */
    assert(d.data[(size_t)((s.data_start + 13U) * (uint64_t)s.block_size)] == 0U);
    assert(openfs_transaction_abort(&transaction) == OPENFS_TRANSACTION_CORRUPT);
    assert(transaction.active == 0);

    unsigned replay_calls = 0U;
    assert(openfs_journal_recover(&j, &v, &s, replay_count, &replay_calls) ==
           OPENFS_JOURNAL_CORRUPT);
    assert(replay_calls == 0U);
    assert(j.recovery_required != 0U);
    uint64_t blocked_tx = 0U;
    assert(openfs_journal_begin(&j, &v, &blocked_tx) == OPENFS_JOURNAL_IO_ERROR);
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_IO_ERROR);

    openfs_journal_t reopened;
    assert(openfs_journal_open(&reopened, &v, &s) == OPENFS_JOURNAL_CORRUPT);
    free(d.data);
}

static void test_corrupt_wal_recovery_stays_gated(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x73U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_write(&j, &v, tx, "must-not-apply", 14U) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_OK);

    openfs_journal_t reopened;
    assert(openfs_journal_open(&reopened, &v, &s) == OPENFS_JOURNAL_OK);
    /* Damage the committed DATA payload without updating its CRC. */
    size_t data_offset = (size_t)((s.journal_start + 1U) * (uint64_t)s.block_size) +
                         OPENFS_JOURNAL_HEADER_SIZE;
    d.data[data_offset] ^= 0x80U;

    openfs_journal_t rejected;
    assert(openfs_journal_open(&rejected, &v, &s) == OPENFS_JOURNAL_CORRUPT);
    unsigned replay_calls = 0U;
    assert(openfs_journal_recover(&reopened, &v, &s, replay_count, &replay_calls) ==
           OPENFS_JOURNAL_CORRUPT);
    assert(replay_calls == 0U);
    assert(reopened.recovery_required != 0U);
    assert(openfs_journal_begin(&reopened, &v, &tx) == OPENFS_JOURNAL_IO_ERROR);
    assert(openfs_journal_checkpoint(&reopened, &v) == OPENFS_JOURNAL_IO_ERROR);
    free(d.data);
}

static void test_checkpoint_write_failure_restores_entire_wal(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x71U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_write(&j, &v, tx, "checkpoint", 10U) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_OK);

    size_t wal_size = (size_t)(s.journal_blocks * (uint64_t)s.block_size);
    size_t wal_offset = (size_t)(s.journal_start * (uint64_t)s.block_size);
    uint8_t *before = malloc(wal_size);
    assert(before != NULL);
    memcpy(before, d.data + wal_offset, wal_size);
    uint64_t records_before = j.next_record;
    /* Two slots clear; the third write fails. Rollback must restore the full WAL. */
    d.fail_write_after = 3;
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_IO_ERROR);
    assert(j.recovery_required == 0U);
    assert(j.next_record == records_before);
    assert(memcmp(before, d.data + wal_offset, wal_size) == 0);

    openfs_journal_t reopened;
    assert(openfs_journal_open(&reopened, &v, &s) == OPENFS_JOURNAL_OK);
    assert(reopened.next_record == records_before);
    assert(openfs_journal_recover(&reopened, &v, &s, replay_noop, NULL) ==
           OPENFS_JOURNAL_OK);
    assert(reopened.recovery_required == 0U);
    assert(openfs_journal_checkpoint(&reopened, &v) == OPENFS_JOURNAL_OK);
    assert(reopened.next_record == 0U);
    free(before);
    free(d.data);
}

static void test_checkpoint_rollback_failure_poison_journal(void)
{
    disk_t d = {0};
    d.block_size = 4096U;
    d.block_count = 256U;
    d.data = calloc((size_t)d.block_size, (size_t)d.block_count);
    assert(d.data != NULL);
    openfs_block_device_t v = device(&d);
    uint8_t uuid[16] = {0x72U};
    assert(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    assert(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    uint64_t tx = 0U;
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_write(&j, &v, tx, "checkpoint", 10U) == OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j, &v, tx) == OPENFS_JOURNAL_OK);

    /* Force the clear phase to fail at flush, then fail a restore write. */
    d.fail_next_flush = 1;
    /* Let every clear write succeed, then fail the first rollback write. */
    d.fail_write_after = (int)s.journal_blocks + 1;
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_CORRUPT);
    assert(j.recovery_required != 0U);
    assert(openfs_journal_checkpoint(&j, &v) == OPENFS_JOURNAL_IO_ERROR);
    assert(openfs_journal_begin(&j, &v, &tx) == OPENFS_JOURNAL_IO_ERROR);

    /*
     * Simulate a process restart after rollback itself failed. The on-disk WAL
     * now contains a zeroed first slot followed by restored records. Reopening
     * and replay must reject that non-contiguous log before invoking callbacks.
     */
    openfs_journal_t reopened;
    assert(openfs_journal_open(&reopened, &v, &s) == OPENFS_JOURNAL_CORRUPT);
    unsigned replay_calls = 0U;
    assert(openfs_journal_replay(&v, &s, replay_count, &replay_calls) ==
           OPENFS_JOURNAL_CORRUPT);
    assert(replay_calls == 0U);
    assert(openfs_journal_recover(&j, &v, &s, replay_count, &replay_calls) ==
           OPENFS_JOURNAL_CORRUPT);
    assert(replay_calls == 0U);
    assert(j.recovery_required != 0U);
    free(d.data);
}

int main(void)
{
    test_data_failure_restores_sequence();
    test_begin_failure_restores_sequence();
    test_begin_partial_write_restores_journal_slot();
    test_partial_begin_and_failed_rollback_poison_journal();
    test_partial_begin_rollback_flush_failure_poison_journal();
    test_data_partial_write_restores_journal_slot();
    test_partial_data_and_failed_rollback_poison_journal();
    test_partial_data_rollback_flush_failure_poison_journal();
    test_block_write_failure_keeps_partial_transaction_abortable();
    test_block_write_partial_failure_is_abortable();
    test_replay_rejects_interleaved_transactions();
    test_recovery_failure_keeps_checkpoint_blocked();
    test_failed_transaction_can_be_retired_before_recovery();
    test_recovery_refuses_live_transaction_before_replay();
    test_recovery_holds_runtime_admission_until_replay_finishes();
    test_partial_commit_rollback_flush_failure_poison_journal();
    test_partial_commit_write_restores_slot_and_can_retry();
    test_commit_flush_failure_requires_recovery();
    test_reopen_uncommitted_wal_requires_replay();
    test_begin_restore_failure_requires_recovery();
    test_data_restore_failure_requires_recovery();
    test_replay_flush_failure_keeps_recovery_gated();
    test_partial_commit_write_and_failed_restore_stay_gated();
    test_corrupt_wal_recovery_stays_gated();
    test_checkpoint_write_failure_restores_entire_wal();
    test_checkpoint_rollback_failure_poison_journal();
    return 0;
}
