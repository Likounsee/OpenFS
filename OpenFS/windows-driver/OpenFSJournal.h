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
    _In_ ULONGLONG NextTransactionId,
    _Out_ POPENFS_JOURNAL_TRANSACTION_VIEW Transactions,
    _In_ ULONG TransactionCapacity,
    _Out_ PULONG RequiredTransactionCount
    );

/* Encode validated records/checkpoints into zero-padded filesystem blocks. */
NTSTATUS
OpenFsEncodeJournalCheckpointBlock(
    _In_ const OPENFS_JOURNAL_CHECKPOINT* Checkpoint,
    _In_ ULONG BlockSize,
    _In_ ULONGLONG RingBlocks,
    _Out_ UCHAR* Block
    );
NTSTATUS
OpenFsEncodeJournalRecordBlock(
    _In_ OPENFS_JOURNAL_RECORD_KIND Kind,
    _In_ ULONGLONG Sequence,
    _In_ ULONGLONG TransactionId,
    _In_reads_bytes_(PayloadLength) const UCHAR* Payload,
    _In_ ULONG PayloadLength,
    _In_ ULONG BlockSize,
    _Out_ UCHAR* Block
    );

/*
 * Reconstruct one full home-block REDO image from its UPDATE chunk records.
 * The destination remains untouched when validation fails.
 */
NTSTATUS
OpenFsDecodeRedoImage(
    _In_reads_(RecordCount) const OPENFS_JOURNAL_RECORD_VIEW* Records,
    _In_ ULONG FirstRecord,
    _In_ ULONG RecordCount,
    _In_ ULONG BlockSize,
    _Out_ ULONGLONG* TargetBlock,
    _Out_writes_bytes_(BlockSize) UCHAR* Image
    );

/*
 * Scan an in-memory snapshot of the bounded journal ring. Blocks are caller-owned
 * and must remain stable while returned payload views are in use.
 */
NTSTATUS
OpenFsScanJournalRing(
    _In_reads_(RingBlocks) const UCHAR* const* Blocks,
    _In_ ULONG RingBlocks,
    _In_ ULONG BlockSize,
    _In_ ULONGLONG CheckpointSequence,
    _In_ ULONGLONG NextTransactionId,
    _Out_ POPENFS_JOURNAL_RECORD_VIEW Records,
    _In_ ULONG RecordCapacity,
    _Out_ PULONG RequiredRecordCount
    );

/* Select the newest semantically consistent checkpoint copy. */
NTSTATUS
OpenFsSelectJournalCheckpoint(
    _In_ NTSTATUS FirstStatus,
    _In_opt_ const OPENFS_JOURNAL_CHECKPOINT* First,
    _In_ NTSTATUS SecondStatus,
    _In_opt_ const OPENFS_JOURNAL_CHECKPOINT* Second,
    _Out_ POPENFS_JOURNAL_CHECKPOINT Selected,
    _Out_ UCHAR* ActiveCopy
    );
