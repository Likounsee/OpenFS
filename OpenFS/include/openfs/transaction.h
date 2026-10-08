#ifndef OPENFS_TRANSACTION_H
#define OPENFS_TRANSACTION_H
#include <stdint.h>
#include "openfs/block_device.h"
#include "openfs/journal.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { OPENFS_TRANSACTION_OK=0, OPENFS_TRANSACTION_INVALID_ARGUMENT=1, OPENFS_TRANSACTION_IO_ERROR=2, OPENFS_TRANSACTION_FULL=3, OPENFS_TRANSACTION_CORRUPT=4 } openfs_transaction_result_t;
typedef struct openfs_transaction_pending openfs_transaction_pending_t;
typedef struct { openfs_block_device_t *base; openfs_block_device_t device; openfs_journal_t *journal; uint64_t txid; uint64_t state_magic; openfs_transaction_pending_t *pending; uint64_t pending_count; uint64_t pending_capacity; int active; int failed; int commit_started; int committed; int recovery_required; } openfs_transaction_t;
openfs_transaction_result_t openfs_transaction_begin(openfs_transaction_t *,openfs_block_device_t *,openfs_journal_t *);
openfs_block_device_t *openfs_transaction_device(openfs_transaction_t *);
openfs_transaction_result_t openfs_transaction_commit(openfs_transaction_t *);
openfs_transaction_result_t openfs_transaction_abort(openfs_transaction_t *);
#ifdef __cplusplus
}
#endif
#endif
