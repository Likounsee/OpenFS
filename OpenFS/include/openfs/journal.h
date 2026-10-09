#ifndef OPENFS_JOURNAL_H
#define OPENFS_JOURNAL_H
#include <stdint.h>
#include "openfs/format.h"
struct openfs_runtime;
#ifdef __cplusplus
extern "C" {
#endif
#define OPENFS_JOURNAL_MAGIC "OJNL1"
#define OPENFS_JOURNAL_HEADER_SIZE 32U
#define OPENFS_JOURNAL_BLOCK_DATA_HEADER 24U
typedef enum { OPENFS_JOURNAL_OK=0, OPENFS_JOURNAL_INVALID_ARGUMENT=1, OPENFS_JOURNAL_FULL=2, OPENFS_JOURNAL_CORRUPT=3, OPENFS_JOURNAL_IO_ERROR=4 } openfs_journal_result_t;
typedef enum { OPENFS_JOURNAL_BEGIN=1, OPENFS_JOURNAL_DATA=2, OPENFS_JOURNAL_COMMIT=3 } openfs_journal_type_t;
typedef struct { uint64_t transaction_id; uint64_t sequence; uint64_t active_transaction_id; uint64_t next_record; uint8_t commit_record_written; uint8_t recovery_required; uint64_t journal_start; uint64_t journal_blocks; uint32_t block_size; struct openfs_runtime *runtime; } openfs_journal_t;
typedef openfs_journal_result_t (*openfs_journal_replay_fn)(void *,uint64_t,const uint8_t *,uint32_t);
openfs_journal_result_t openfs_journal_open(openfs_journal_t *,const openfs_block_device_t *,const openfs_superblock_t *);
openfs_journal_result_t openfs_journal_begin(openfs_journal_t *,openfs_block_device_t *,uint64_t *);
openfs_journal_result_t openfs_journal_write(openfs_journal_t *,openfs_block_device_t *,uint64_t,const void *,uint32_t);
openfs_journal_result_t openfs_journal_write_block(openfs_journal_t *,openfs_block_device_t *,uint64_t,uint64_t,const void *);
openfs_journal_result_t openfs_journal_commit(openfs_journal_t *,openfs_block_device_t *,uint64_t);
openfs_journal_result_t openfs_journal_checkpoint(openfs_journal_t *,openfs_block_device_t *);
openfs_journal_result_t openfs_journal_replay(const openfs_block_device_t *,const openfs_superblock_t *,openfs_journal_replay_fn,void *);
#ifdef __cplusplus
}
#endif
#endif