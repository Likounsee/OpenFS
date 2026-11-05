#pragma once
#include "OpenFSSuperblock.h"
#define OPENFS_JOURNAL_RECORD_HEADER_BYTES 40u
#define OPENFS_JOURNAL_CHECKPOINT_BYTES 72u
#define OPENFS_JOURNAL_MAX_PAYLOAD_BYTES (1024u * 1024u)
typedef enum _OPENFS_JOURNAL_RECORD_KIND { OpenFsJournalBegin=1, OpenFsJournalUpdate=2, OpenFsJournalCommit=3, OpenFsJournalAbort=4 } OPENFS_JOURNAL_RECORD_KIND;
typedef struct _OPENFS_JOURNAL_CHECKPOINT { ULONGLONG Generation,CheckpointSequence,NextSequence,NextTransactionId,HeadBlock,TailBlock; } OPENFS_JOURNAL_CHECKPOINT,*POPENFS_JOURNAL_CHECKPOINT;
typedef struct _OPENFS_JOURNAL_RECORD_VIEW { OPENFS_JOURNAL_RECORD_KIND Kind; ULONGLONG Sequence,TransactionId; ULONG PayloadLength; const UCHAR* Payload; } OPENFS_JOURNAL_RECORD_VIEW,*POPENFS_JOURNAL_RECORD_VIEW;
/* Payload is a zero-copy view into Block and must not outlive it. */
NTSTATUS OpenFsDecodeJournalCheckpointBlock(_In_reads_bytes_(BlockSize) const UCHAR* Block,_In_ ULONG BlockSize,_In_ ULONGLONG RingBlocks,_Out_ POPENFS_JOURNAL_CHECKPOINT Checkpoint);
NTSTATUS OpenFsDecodeJournalRecordBlock(_In_reads_bytes_(BlockSize) const UCHAR* Block,_In_ ULONG BlockSize,_Out_ POPENFS_JOURNAL_RECORD_VIEW Record);

typedef struct _OPENFS_JOURNAL_TRANSACTION_VIEW {
    ULONGLONG TransactionId;
    ULONG FirstUpdateIndex;
    ULONG UpdateCount;
} OPENFS_JOURNAL_TRANSACTION_VIEW, *POPENFS_JOURNAL_TRANSACTION_VIEW;

/*
 * Validate a decoded, ordered journal stream and describe only committed
 * transactions. An unfinished final transaction is intentionally omitted.
 * On STATUS_BUFFER_TOO_SMALL, RequiredTransactionCount reports the full need
 * and the caller's output array remains untouched.
 */
NTSTATUS
OpenFsPlanJournalRecovery(
    _In_reads_(RecordCount) const OPENFS_JOURNAL_RECORD_VIEW* Records,
    _In_ ULONG RecordCount,
    _In_ ULONGLONG CheckpointSequence,
    _Out_writes_(TransactionCapacity) POPENFS_JOURNAL_TRANSACTION_VIEW Transactions,
    _In_ ULONG TransactionCapacity,
    _Out_ PULONG RequiredTransactionCount
    );
