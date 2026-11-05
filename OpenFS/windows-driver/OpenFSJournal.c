#include "OpenFSJournal.h"
#define JREC_MAGIC "OFSJNL\0\0"
#define JCP_MAGIC "OFSCKP\0\0"
static void w16(UCHAR*p,USHORT v){p[0]=(UCHAR)v;p[1]=(UCHAR)(v>>8);}
static void w32(UCHAR*p,ULONG v){p[0]=(UCHAR)v;p[1]=(UCHAR)(v>>8);p[2]=(UCHAR)(v>>16);p[3]=(UCHAR)(v>>24);}
static void w64(UCHAR*p,ULONGLONG v){w32(p,(ULONG)v);w32(p+4,(ULONG)(v>>32));}
static USHORT r16(const UCHAR*p){return (USHORT)(p[0]|((USHORT)p[1]<<8));}
static ULONG r32(const UCHAR*p){return (ULONG)p[0]|((ULONG)p[1]<<8)|((ULONG)p[2]<<16)|((ULONG)p[3]<<24);}
static ULONGLONG r64(const UCHAR*p){return (ULONGLONG)r32(p)|((ULONGLONG)r32(p+4)<<32);}
static BOOLEAN zeroes(const UCHAR*p,ULONG n){ULONG i;for(i=0;i<n;i++)if(p[i])return FALSE;return TRUE;}
static ULONG record_crc(const UCHAR*h,const UCHAR*p,ULONG n){ULONG crc=0xFFFFFFFFu,i,b;for(i=0;i<36;i++){crc^=h[i];for(b=0;b<8;b++)crc=(crc>>1)^((crc&1u)?0x82F63B78u:0u);}for(i=0;i<n;i++){crc^=p[i];for(b=0;b<8;b++)crc=(crc>>1)^((crc&1u)?0x82F63B78u:0u);}return ~crc;}
NTSTATUS OpenFsDecodeJournalCheckpointBlock(const UCHAR*Block,ULONG BlockSize,ULONGLONG RingBlocks,POPENFS_JOURNAL_CHECKPOINT Cp){
 if(!Block||!Cp||RingBlocks<4||BlockSize<4096||BlockSize>65536||(BlockSize&(BlockSize-1u)))return STATUS_INVALID_PARAMETER;
 RtlZeroMemory(Cp,sizeof(*Cp));
 if(RtlCompareMemory(Block,JCP_MAGIC,8)!=8||r16(Block+8)!=1||r16(Block+10)!=72)return STATUS_DISK_CORRUPT_ERROR;
 if(!zeroes(Block+12,4)||!zeroes(Block+68,4)||!zeroes(Block+72,BlockSize-72))return STATUS_DISK_CORRUPT_ERROR;
 if(r32(Block+64)!=OpenFsCrc32C(Block,64))return STATUS_CRC_ERROR;
 Cp->Generation=r64(Block+16);Cp->CheckpointSequence=r64(Block+24);Cp->NextSequence=r64(Block+32);Cp->NextTransactionId=r64(Block+40);Cp->HeadBlock=r64(Block+48);Cp->TailBlock=r64(Block+56);
 if(!Cp->Generation||Cp->CheckpointSequence==MAXULONGLONG||Cp->NextSequence!=Cp->CheckpointSequence+1u||!Cp->NextTransactionId||Cp->NextTransactionId>Cp->CheckpointSequence+1u||Cp->HeadBlock>=RingBlocks||Cp->TailBlock>=RingBlocks||Cp->HeadBlock!=0||Cp->TailBlock!=0)return STATUS_DISK_CORRUPT_ERROR;
 return STATUS_SUCCESS;
}
NTSTATUS OpenFsDecodeJournalRecordBlock(const UCHAR*Block,ULONG BlockSize,POPENFS_JOURNAL_RECORD_VIEW Rec){
 ULONG len,total;USHORT kind;const UCHAR*payload;
 if(!Block||!Rec||BlockSize<4096||BlockSize>65536||(BlockSize&(BlockSize-1u)))return STATUS_INVALID_PARAMETER;
 RtlZeroMemory(Rec,sizeof(*Rec));
 if(RtlCompareMemory(Block,JREC_MAGIC,8)!=8||r16(Block+8)!=1||r16(Block+10)!=40||r16(Block+14)!=0)return STATUS_DISK_CORRUPT_ERROR;
 len=r32(Block+32);if(len>OPENFS_JOURNAL_MAX_PAYLOAD_BYTES||len>BlockSize-40u)return STATUS_DISK_CORRUPT_ERROR;
 total=40u+len;payload=Block+40;
 if(!zeroes(Block+total,BlockSize-total))return STATUS_DISK_CORRUPT_ERROR;
 if(r32(Block+36)!=record_crc(Block,payload,len))return STATUS_CRC_ERROR;
 kind=r16(Block+12);if(kind<OpenFsJournalBegin||kind>OpenFsJournalAbort||!r64(Block+16)||!r64(Block+24)||(kind!=OpenFsJournalUpdate&&len))return STATUS_DISK_CORRUPT_ERROR;
 Rec->Kind=(OPENFS_JOURNAL_RECORD_KIND)kind;Rec->Sequence=r64(Block+16);Rec->TransactionId=r64(Block+24);Rec->PayloadLength=len;Rec->Payload=payload;return STATUS_SUCCESS;
}

static NTSTATUS
OpenFsValidateJournalRecordView(
    _In_ const OPENFS_JOURNAL_RECORD_VIEW* Record
    )
{
    if (Record->Kind < OpenFsJournalBegin || Record->Kind > OpenFsJournalAbort ||
        Record->Sequence == 0 || Record->TransactionId == 0 ||
        Record->PayloadLength > OPENFS_JOURNAL_MAX_PAYLOAD_BYTES ||
        (Record->PayloadLength != 0 && Record->Payload == NULL) ||
        (Record->Kind != OpenFsJournalUpdate && Record->PayloadLength != 0))
        return STATUS_DISK_CORRUPT_ERROR;
    return STATUS_SUCCESS;
}

NTSTATUS
OpenFsPlanJournalRecovery(
    _In_reads_(RecordCount) const OPENFS_JOURNAL_RECORD_VIEW* Records,
    _In_ ULONG RecordCount,
    _In_ ULONGLONG CheckpointSequence,
    _In_ ULONGLONG NextTransactionId,
    _Out_ POPENFS_JOURNAL_TRANSACTION_VIEW Transactions,
    _In_ ULONG TransactionCapacity,
    _Out_ PULONG RequiredTransactionCount
    )
{
    ULONGLONG expectedSequence;
    ULONGLONG activeTransaction = 0;
    ULONGLONG expectedTransactionId;
    ULONG activeUpdateCount = 0;
    ULONG committedCount = 0;
    ULONG index;
    BOOLEAN active = FALSE;
    NTSTATUS status;

    if (RequiredTransactionCount == NULL ||
        (RecordCount != 0 && Records == NULL) ||
        (TransactionCapacity != 0 && Transactions == NULL))
        return STATUS_INVALID_PARAMETER;
    *RequiredTransactionCount = 0;

    if (RecordCount == 0)
        return STATUS_SUCCESS;
    if (CheckpointSequence == MAXULONGLONG || NextTransactionId == 0 ||
        NextTransactionId > CheckpointSequence + 1u)
        return STATUS_INTEGER_OVERFLOW;
    expectedSequence = CheckpointSequence + 1u;
    expectedTransactionId = NextTransactionId;

    /* First pass validates the entire stream before touching caller output. */
    for (index = 0; index < RecordCount; ++index) {
        const OPENFS_JOURNAL_RECORD_VIEW* record = &Records[index];
        status = OpenFsValidateJournalRecordView(record);
        if (!NT_SUCCESS(status))
            return status;
        if (record->Sequence != expectedSequence)
            return STATUS_DISK_CORRUPT_ERROR;
        if (index + 1u < RecordCount) {
            if (expectedSequence == MAXULONGLONG)
                return STATUS_INTEGER_OVERFLOW;
            ++expectedSequence;
        }

        switch (record->Kind) {
        case OpenFsJournalBegin:
            if (active || record->TransactionId != expectedTransactionId)
                return STATUS_DISK_CORRUPT_ERROR;
            if (expectedTransactionId == MAXULONGLONG)
                return STATUS_INTEGER_OVERFLOW;
            ++expectedTransactionId;
            active = TRUE;
            activeTransaction = record->TransactionId;
            activeUpdateCount = 0;
            break;
        case OpenFsJournalUpdate:
            if (!active || record->TransactionId != activeTransaction ||
                activeUpdateCount == MAXULONG)
                return STATUS_DISK_CORRUPT_ERROR;
            ++activeUpdateCount;
            break;
        case OpenFsJournalCommit:
            if (!active || record->TransactionId != activeTransaction ||
                activeUpdateCount == 0 || committedCount == MAXULONG)
                return STATUS_DISK_CORRUPT_ERROR;
            ++committedCount;
            active = FALSE;
            activeTransaction = 0;
            activeUpdateCount = 0;
            break;
        case OpenFsJournalAbort:
            if (!active || record->TransactionId != activeTransaction)
                return STATUS_DISK_CORRUPT_ERROR;
            active = FALSE;
            activeTransaction = 0;
            activeUpdateCount = 0;
            break;
        default:
            return STATUS_DISK_CORRUPT_ERROR;
        }
    }

    *RequiredTransactionCount = committedCount;
    if (committedCount > TransactionCapacity)
        return STATUS_BUFFER_TOO_SMALL;

    /* Second pass publishes compact references to committed UPDATE records. */
    {
        ULONG firstUpdate = 0;
        ULONG updateCount = 0;
        ULONG outputIndex = 0;

        for (index = 0; index < RecordCount; ++index) {
            const OPENFS_JOURNAL_RECORD_VIEW* record = &Records[index];
            if (record->Kind == OpenFsJournalBegin) {
                activeTransaction = record->TransactionId;
                updateCount = 0;
            } else if (record->Kind == OpenFsJournalUpdate) {
                if (updateCount == 0)
                    firstUpdate = index;
                ++updateCount;
            } else if (record->Kind == OpenFsJournalCommit) {
                Transactions[outputIndex].TransactionId = activeTransaction;
                Transactions[outputIndex].FirstUpdateIndex = firstUpdate;
                Transactions[outputIndex].UpdateCount = updateCount;
                ++outputIndex;
                updateCount = 0;
            } else if (record->Kind == OpenFsJournalAbort) {
                updateCount = 0;
            }
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
OpenFsEncodeJournalCheckpointBlock(
    _In_ const OPENFS_JOURNAL_CHECKPOINT* Cp,
    _In_ ULONG BlockSize,
    _In_ ULONGLONG RingBlocks,
    _Out_ UCHAR* Block
    )
{
    if (Cp == NULL || Block == NULL || RingBlocks < 4u ||
        BlockSize < 4096u || BlockSize > 65536u ||
        (BlockSize & (BlockSize - 1u)) != 0)
        return STATUS_INVALID_PARAMETER;
    if (Cp->Generation == 0 || Cp->CheckpointSequence == MAXULONGLONG ||
        Cp->NextSequence != Cp->CheckpointSequence + 1u ||
        Cp->NextTransactionId == 0 ||
        Cp->NextTransactionId > Cp->CheckpointSequence + 1u ||
        Cp->HeadBlock != 0 || Cp->TailBlock != 0)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Block, BlockSize);
    RtlCopyMemory(Block, JCP_MAGIC, 8);
    w16(Block + 8, 1);
    w16(Block + 10, OPENFS_JOURNAL_CHECKPOINT_BYTES);
    w64(Block + 16, Cp->Generation);
    w64(Block + 24, Cp->CheckpointSequence);
    w64(Block + 32, Cp->NextSequence);
    w64(Block + 40, Cp->NextTransactionId);
    w64(Block + 48, Cp->HeadBlock);
    w64(Block + 56, Cp->TailBlock);
    w32(Block + 64, OpenFsCrc32C(Block, 64));
    return STATUS_SUCCESS;
}

NTSTATUS
OpenFsEncodeJournalRecordBlock(
    _In_ OPENFS_JOURNAL_RECORD_KIND Kind,
    _In_ ULONGLONG Sequence,
    _In_ ULONGLONG TransactionId,
    _In_reads_bytes_(PayloadLength) const UCHAR* Payload,
    _In_ ULONG PayloadLength,
    _In_ ULONG BlockSize,
    _Out_ UCHAR* Block
    )
{
    if (Block == NULL || BlockSize < 4096u || BlockSize > 65536u ||
        (BlockSize & (BlockSize - 1u)) != 0 ||
        Kind < OpenFsJournalBegin || Kind > OpenFsJournalAbort ||
        Sequence == 0 || TransactionId == 0 ||
        PayloadLength > OPENFS_JOURNAL_MAX_PAYLOAD_BYTES ||
        PayloadLength > BlockSize - OPENFS_JOURNAL_RECORD_HEADER_BYTES ||
        (PayloadLength != 0 && Payload == NULL) ||
        (Kind != OpenFsJournalUpdate && PayloadLength != 0))
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Block, BlockSize);
    RtlCopyMemory(Block, JREC_MAGIC, 8);
    w16(Block + 8, 1);
    w16(Block + 10, OPENFS_JOURNAL_RECORD_HEADER_BYTES);
    w16(Block + 12, (USHORT)Kind);
    w64(Block + 16, Sequence);
    w64(Block + 24, TransactionId);
    w32(Block + 32, PayloadLength);
    if (PayloadLength != 0)
        RtlCopyMemory(Block + OPENFS_JOURNAL_RECORD_HEADER_BYTES,
            Payload, PayloadLength);
    w32(Block + 36, record_crc(Block,
        Block + OPENFS_JOURNAL_RECORD_HEADER_BYTES, PayloadLength));
    return STATUS_SUCCESS;
}

NTSTATUS
OpenFsDecodeRedoImage(
    _In_reads_(RecordCount) const OPENFS_JOURNAL_RECORD_VIEW* Records,
    _In_ ULONG FirstRecord,
    _In_ ULONG RecordCount,
    _In_ ULONG BlockSize,
    _Out_ ULONGLONG* TargetBlock,
    _Out_writes_bytes_(BlockSize) UCHAR* Image
    )
{
    ULONG chunkCapacity;
    ULONG expectedChunks;
    ULONG index;
    ULONG bytesCopied = 0;
    ULONGLONG target;
    USHORT declaredChunks;

    if (Records == NULL || TargetBlock == NULL || Image == NULL ||
        BlockSize < 64u || BlockSize > 65536u ||
        RecordCount > MAXULONG - FirstRecord)
        return STATUS_INVALID_PARAMETER;
    if (RecordCount == 0)
        return STATUS_DISK_CORRUPT_ERROR;
    chunkCapacity = BlockSize - OPENFS_JOURNAL_RECORD_HEADER_BYTES - 12u;
    expectedChunks = (BlockSize + chunkCapacity - 1u) / chunkCapacity;
    if (expectedChunks > 0xFFFFu || RecordCount != expectedChunks)
        return STATUS_DISK_CORRUPT_ERROR;

    if (Records[FirstRecord].Kind != OpenFsJournalUpdate ||
        Records[FirstRecord].Payload == NULL ||
        Records[FirstRecord].PayloadLength < 12u)
        return STATUS_DISK_CORRUPT_ERROR;
    target = (ULONGLONG)Records[FirstRecord].Payload[0] |
        ((ULONGLONG)Records[FirstRecord].Payload[1] << 8) |
        ((ULONGLONG)Records[FirstRecord].Payload[2] << 16) |
        ((ULONGLONG)Records[FirstRecord].Payload[3] << 24) |
        ((ULONGLONG)Records[FirstRecord].Payload[4] << 32) |
        ((ULONGLONG)Records[FirstRecord].Payload[5] << 40) |
        ((ULONGLONG)Records[FirstRecord].Payload[6] << 48) |
        ((ULONGLONG)Records[FirstRecord].Payload[7] << 56);
    declaredChunks = (USHORT)(Records[FirstRecord].Payload[10] |
        ((USHORT)Records[FirstRecord].Payload[11] << 8));
    if (declaredChunks != expectedChunks)
        return STATUS_DISK_CORRUPT_ERROR;

    /* Validate every chunk before changing the caller's output image. */
    for (index = 0; index < expectedChunks; ++index) {
        const OPENFS_JOURNAL_RECORD_VIEW* record = &Records[FirstRecord + index];
        ULONG expectedData = index + 1u < expectedChunks
            ? chunkCapacity : BlockSize - (chunkCapacity * (expectedChunks - 1u));
        ULONGLONG currentTarget;
        USHORT chunkIndex;
        USHORT chunkCount;

        if (record->Kind != OpenFsJournalUpdate ||
            record->TransactionId != Records[FirstRecord].TransactionId ||
            record->Payload == NULL ||
            record->PayloadLength != 12u + expectedData)
            return STATUS_DISK_CORRUPT_ERROR;
        currentTarget = (ULONGLONG)record->Payload[0] |
            ((ULONGLONG)record->Payload[1] << 8) |
            ((ULONGLONG)record->Payload[2] << 16) |
            ((ULONGLONG)record->Payload[3] << 24) |
            ((ULONGLONG)record->Payload[4] << 32) |
            ((ULONGLONG)record->Payload[5] << 40) |
            ((ULONGLONG)record->Payload[6] << 48) |
            ((ULONGLONG)record->Payload[7] << 56);
        chunkIndex = (USHORT)(record->Payload[8] |
            ((USHORT)record->Payload[9] << 8));
        chunkCount = (USHORT)(record->Payload[10] |
            ((USHORT)record->Payload[11] << 8));
        if (currentTarget != target || chunkIndex != index ||
            chunkCount != expectedChunks)
            return STATUS_DISK_CORRUPT_ERROR;
        bytesCopied += expectedData;
    }
    if (bytesCopied != BlockSize)
        return STATUS_DISK_CORRUPT_ERROR;

    bytesCopied = 0;
    for (index = 0; index < expectedChunks; ++index) {
        const OPENFS_JOURNAL_RECORD_VIEW* record = &Records[FirstRecord + index];
        ULONG dataLength = record->PayloadLength - 12u;
        RtlCopyMemory(Image + bytesCopied, record->Payload + 12u, dataLength);
        bytesCopied += dataLength;
    }
    *TargetBlock = target;
    return STATUS_SUCCESS;
}

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
    )
{
    ULONG slot;
    ULONG retained = 0;
    BOOLEAN gapSeen = FALSE;
    NTSTATUS status;

    if (Blocks == NULL || RequiredRecordCount == NULL || RingBlocks < 4u ||
        BlockSize < 4096u || BlockSize > 65536u ||
        (BlockSize & (BlockSize - 1u)) != 0 ||
        NextTransactionId == 0 ||
        (RecordCapacity != 0 && Records == NULL))
        return STATUS_INVALID_PARAMETER;
    *RequiredRecordCount = 0;

    /* Pass one: determine the contiguous post-checkpoint prefix without output. */
    for (slot = 0; slot < RingBlocks; ++slot) {
        OPENFS_JOURNAL_RECORD_VIEW record;
        const UCHAR* block = Blocks[slot];
        if (block == NULL)
            return STATUS_INVALID_PARAMETER;
        if (zeroes(block, BlockSize)) {
            gapSeen = TRUE;
            continue;
        }
        status = OpenFsDecodeJournalRecordBlock(block, BlockSize, &record);
        if (!NT_SUCCESS(status)) {
            gapSeen = TRUE;
            continue;
        }
        if (record.Sequence <= CheckpointSequence) {
            gapSeen = TRUE;
            continue;
        }
        if (record.TransactionId < NextTransactionId)
            return STATUS_DISK_CORRUPT_ERROR;
        if (gapSeen) {
            if (record.Kind == OpenFsJournalCommit)
                return STATUS_DISK_CORRUPT_ERROR;
            continue;
        }
        if (retained == MAXULONG)
            return STATUS_INTEGER_OVERFLOW;
        ++retained;
    }

    *RequiredRecordCount = retained;
    if (retained > RecordCapacity)
        return STATUS_BUFFER_TOO_SMALL;

    /* Pass two: publish zero-copy views for the contiguous prefix only. */
    retained = 0;
    for (slot = 0; slot < RingBlocks; ++slot) {
        OPENFS_JOURNAL_RECORD_VIEW record;
        const UCHAR* block = Blocks[slot];
        if (zeroes(block, BlockSize))
            break;
        status = OpenFsDecodeJournalRecordBlock(block, BlockSize, &record);
        if (!NT_SUCCESS(status) || record.Sequence <= CheckpointSequence)
            break;
        Records[retained++] = record;
    }
    return STATUS_SUCCESS;
}

static BOOLEAN
OpenFsJournalCheckpointsEqual(
    _In_ const OPENFS_JOURNAL_CHECKPOINT* Left,
    _In_ const OPENFS_JOURNAL_CHECKPOINT* Right
    )
{
    return Left->Generation == Right->Generation &&
        Left->CheckpointSequence == Right->CheckpointSequence &&
        Left->NextSequence == Right->NextSequence &&
        Left->NextTransactionId == Right->NextTransactionId &&
        Left->HeadBlock == Right->HeadBlock &&
        Left->TailBlock == Right->TailBlock;
}



NTSTATUS
OpenFsBuildJournalTransaction(
    _In_reads_(UpdateCount) const OPENFS_JOURNAL_REDO_UPDATE* Updates,
    _In_ ULONG UpdateCount,
    _In_ ULONG BlockSize,
    _In_ ULONG RingBlocks,
    _In_ ULONGLONG FirstAllowedBlock,
    _In_ ULONGLONG EndAllowedBlock,
    _In_ ULONGLONG FirstSequence,
    _In_ ULONGLONG TransactionId,
    _Out_writes_bytes_(EncodedBytes) UCHAR* EncodedBlocks,
    _In_ SIZE_T EncodedBytes,
    _Out_ PULONG RequiredRecordCount,
    _Out_ PULONGLONG CommitSequence,
    _Out_ PULONGLONG NextSequence,
    _Out_writes_bytes_(BlockSize) UCHAR* PayloadScratch
    )
{
    ULONG chunkCapacity, chunksPerImage, required, i, slot;
    SIZE_T requiredBytes;
    ULONGLONG sequence;
    NTSTATUS status;

    if (Updates == NULL || UpdateCount == 0 || BlockSize < 4096u ||
        BlockSize > 65536u || (BlockSize & (BlockSize - 1u)) != 0 ||
        RingBlocks < 4u || FirstAllowedBlock >= EndAllowedBlock ||
        FirstSequence == 0 || TransactionId == 0 || EncodedBlocks == NULL ||
        RequiredRecordCount == NULL || CommitSequence == NULL ||
        NextSequence == NULL || PayloadScratch == NULL)
        return STATUS_INVALID_PARAMETER;

    chunkCapacity = BlockSize - OPENFS_JOURNAL_RECORD_HEADER_BYTES - 12u;
    chunksPerImage = (BlockSize + chunkCapacity - 1u) / chunkCapacity;
    if (UpdateCount > (MAXULONG - 2u) / chunksPerImage)
        return STATUS_INTEGER_OVERFLOW;
    required = 2u + UpdateCount * chunksPerImage;
    if (required > RingBlocks)
        return STATUS_BUFFER_TOO_SMALL;
    if ((SIZE_T)RingBlocks > ((SIZE_T)-1) / BlockSize)
        return STATUS_INTEGER_OVERFLOW;
    requiredBytes = (SIZE_T)RingBlocks * BlockSize;
    if (EncodedBytes < requiredBytes)
        return STATUS_BUFFER_TOO_SMALL;
    if (FirstSequence > MAXULONGLONG - required)
        return STATUS_INTEGER_OVERFLOW;
    if (TransactionId == MAXULONGLONG)
        return STATUS_INTEGER_OVERFLOW;

    /* Validate all targets and duplicate constraints before touching output. */
    for (i = 0; i < UpdateCount; ++i) {
        ULONG j;
        if (Updates[i].Image == NULL ||
            Updates[i].TargetBlock < FirstAllowedBlock ||
            Updates[i].TargetBlock >= EndAllowedBlock)
            return STATUS_DISK_CORRUPT_ERROR;
        for (j = 0; j < i; ++j)
            if (Updates[j].TargetBlock == Updates[i].TargetBlock)
                return STATUS_DISK_CORRUPT_ERROR;
    }

    RtlZeroMemory(EncodedBlocks, requiredBytes);
    sequence = FirstSequence;
    slot = 0;
    status = OpenFsEncodeJournalRecordBlock(OpenFsJournalBegin, sequence++,
        TransactionId, NULL, 0, BlockSize, EncodedBlocks);
    if (!NT_SUCCESS(status))
        return status;
    ++slot;

    for (i = 0; i < UpdateCount; ++i) {
        ULONG chunk;
        for (chunk = 0; chunk < chunksPerImage; ++chunk) {
            ULONG offset = chunk * chunkCapacity;
            ULONG dataBytes = (BlockSize - offset < chunkCapacity)
                ? BlockSize - offset : chunkCapacity;
            RtlZeroMemory(PayloadScratch, 12u + dataBytes);
            w64(PayloadScratch, Updates[i].TargetBlock);
            w16(PayloadScratch + 8, (USHORT)chunk);
            w16(PayloadScratch + 10, (USHORT)chunksPerImage);
            RtlCopyMemory(PayloadScratch + 12, Updates[i].Image + offset, dataBytes);
            status = OpenFsEncodeJournalRecordBlock(OpenFsJournalUpdate,
                sequence++, TransactionId, PayloadScratch, 12u + dataBytes,
                BlockSize, EncodedBlocks + ((SIZE_T)slot * BlockSize));
            if (!NT_SUCCESS(status))
                return status;
            ++slot;
        }
    }

    *CommitSequence = sequence;
    status = OpenFsEncodeJournalRecordBlock(OpenFsJournalCommit, sequence++,
        TransactionId, NULL, 0, BlockSize,
        EncodedBlocks + ((SIZE_T)slot * BlockSize));
    if (!NT_SUCCESS(status))
        return status;

    *RequiredRecordCount = required;
    *NextSequence = sequence;
    return STATUS_SUCCESS;
}


NTSTATUS
OpenFsValidateRedoTargets(
    _In_reads_(RecordCount) const OPENFS_JOURNAL_RECORD_VIEW* Records,
    _In_ ULONG RecordCount,
    _In_reads_(TransactionCount) const OPENFS_JOURNAL_TRANSACTION_VIEW* Transactions,
    _In_ ULONG TransactionCount,
    _In_ ULONG BlockSize,
    _In_ ULONGLONG FirstAllowedBlock,
    _In_ ULONGLONG EndAllowedBlock,
    _Out_ ULONGLONG* Targets,
    _In_ ULONG TargetCapacity,
    _Out_ PULONG RequiredTargetCount,
    _Out_writes_bytes_(BlockSize) UCHAR* ScratchImage
    )
{
    ULONGLONG planned[16];
    ULONG chunkCapacity, chunksPerImage, plannedCount = 0, txIndex;
    if ((RecordCount != 0 && Records == NULL) ||
        (TransactionCount != 0 && Transactions == NULL) ||
        RequiredTargetCount == NULL || ScratchImage == NULL ||
        BlockSize < 64u || BlockSize > 65536u ||
        FirstAllowedBlock >= EndAllowedBlock ||
        TransactionCount > 16u ||
        (TargetCapacity != 0 && Targets == NULL))
        return STATUS_INVALID_PARAMETER;
    *RequiredTargetCount = 0;
    chunkCapacity = BlockSize - OPENFS_JOURNAL_RECORD_HEADER_BYTES - 12u;
    if (chunkCapacity == 0)
        return STATUS_INVALID_PARAMETER;
    chunksPerImage = (BlockSize + chunkCapacity - 1u) / chunkCapacity;

    for (txIndex = 0; txIndex < TransactionCount; ++txIndex) {
        const OPENFS_JOURNAL_TRANSACTION_VIEW* tx = &Transactions[txIndex];
        ULONG cursor, end, txTargets[16], txTargetCount = 0;
        if (tx->UpdateCount == 0 || tx->FirstUpdateIndex > RecordCount ||
            tx->UpdateCount > RecordCount - tx->FirstUpdateIndex ||
            tx->UpdateCount % chunksPerImage != 0)
            return STATUS_DISK_CORRUPT_ERROR;
        cursor = tx->FirstUpdateIndex;
        end = cursor + tx->UpdateCount;
        while (cursor < end) {
            ULONGLONG target;
            ULONG prior;
            NTSTATUS status = OpenFsDecodeRedoImage(Records, cursor,
                chunksPerImage, BlockSize, &target, ScratchImage);
            if (!NT_SUCCESS(status))
                return status;
            if (Records[cursor].TransactionId != tx->TransactionId)
                return STATUS_DISK_CORRUPT_ERROR;
            if (target < FirstAllowedBlock || target >= EndAllowedBlock)
                return STATUS_DISK_CORRUPT_ERROR;
            for (prior = 0; prior < txTargetCount; ++prior)
                if (planned[txTargets[prior]] == target)
                    return STATUS_DISK_CORRUPT_ERROR;
            if (plannedCount >= 16u)
                return STATUS_BUFFER_TOO_SMALL;
            planned[plannedCount] = target;
            txTargets[txTargetCount++] = plannedCount;
            ++plannedCount;
            cursor += chunksPerImage;
        }
    }

    *RequiredTargetCount = plannedCount;
    if (plannedCount > TargetCapacity)
        return STATUS_BUFFER_TOO_SMALL;
    if (plannedCount != 0)
        RtlCopyMemory(Targets, planned, plannedCount * sizeof(ULONGLONG));
    return STATUS_SUCCESS;
}


NTSTATUS
OpenFsSelectJournalCheckpoint(
    _In_ NTSTATUS FirstStatus,
    _In_opt_ const OPENFS_JOURNAL_CHECKPOINT* First,
    _In_ NTSTATUS SecondStatus,
    _In_opt_ const OPENFS_JOURNAL_CHECKPOINT* Second,
    _Out_ POPENFS_JOURNAL_CHECKPOINT Selected,
    _Out_ UCHAR* ActiveCopy
    )
{
    const OPENFS_JOURNAL_CHECKPOINT* newer;
    const OPENFS_JOURNAL_CHECKPOINT* older;
    UCHAR newerCopy;

    if (Selected == NULL || ActiveCopy == NULL ||
        (NT_SUCCESS(FirstStatus) && First == NULL) ||
        (NT_SUCCESS(SecondStatus) && Second == NULL))
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Selected, sizeof(*Selected));
    *ActiveCopy = 0;

    if (!NT_SUCCESS(FirstStatus) && !NT_SUCCESS(SecondStatus))
        return FirstStatus;
    if (NT_SUCCESS(FirstStatus) && !NT_SUCCESS(SecondStatus)) {
        *Selected = *First;
        *ActiveCopy = 0;
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(FirstStatus) && NT_SUCCESS(SecondStatus)) {
        *Selected = *Second;
        *ActiveCopy = 1;
        return STATUS_SUCCESS;
    }

    if (First->Generation == Second->Generation) {
        if (!OpenFsJournalCheckpointsEqual(First, Second))
            return STATUS_DISK_CORRUPT_ERROR;
        *Selected = *First;
        *ActiveCopy = 0;
        return STATUS_SUCCESS;
    }
    if (First->Generation > Second->Generation) {
        newer = First; older = Second; newerCopy = 0;
    } else {
        newer = Second; older = First; newerCopy = 1;
    }

    if (newer->Generation - older->Generation != 1u ||
        newer->CheckpointSequence < older->CheckpointSequence ||
        newer->NextTransactionId < older->NextTransactionId)
        return STATUS_DISK_CORRUPT_ERROR;

    if (newer->CheckpointSequence == older->CheckpointSequence) {
        if (!(newer->Generation == 2u && older->Generation == 1u &&
              newer->CheckpointSequence == 0u &&
              newer->NextTransactionId == 1u &&
              older->NextTransactionId == 1u))
            return STATUS_DISK_CORRUPT_ERROR;
    } else if (newer->NextTransactionId == older->NextTransactionId) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

    *Selected = *newer;
    *ActiveCopy = newerCopy;
    return STATUS_SUCCESS;
}
