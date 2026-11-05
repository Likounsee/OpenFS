#include "OpenFSJournal.h"
#define JREC_MAGIC "OFSJNL\0\0"
#define JCP_MAGIC "OFSCKP\0\0"
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
 if(!Cp->Generation||Cp->CheckpointSequence==MAXULONGLONG||Cp->NextSequence!=Cp->CheckpointSequence+1u||!Cp->NextTransactionId||Cp->NextTransactionId>Cp->CheckpointSequence+1u||Cp->HeadBlock>=RingBlocks||Cp->TailBlock>=RingBlocks)return STATUS_DISK_CORRUPT_ERROR;
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
