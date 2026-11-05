#include "OpenFSJournal.h"
#include <assert.h>
#include <stdio.h>
static void p16(UCHAR*p,USHORT v){p[0]=(UCHAR)v;p[1]=(UCHAR)(v>>8);}
static void p32(UCHAR*p,ULONG v){p[0]=(UCHAR)v;p[1]=(UCHAR)(v>>8);p[2]=(UCHAR)(v>>16);p[3]=(UCHAR)(v>>24);}
static void p64(UCHAR*p,ULONGLONG v){p32(p,(ULONG)v);p32(p+4,(ULONG)(v>>32));}
static ULONG crc_record(UCHAR*b,const UCHAR*p,ULONG n){ULONG c=0xFFFFFFFFu,i,k;for(i=0;i<36;i++){c^=b[i];for(k=0;k<8;k++)c=(c>>1)^((c&1u)?0x82F63B78u:0u);}for(i=0;i<n;i++){c^=p[i];for(k=0;k<8;k++)c=(c>>1)^((c&1u)?0x82F63B78u:0u);}return ~c;}
static void checkpoint(UCHAR*b){RtlZeroMemory(b,4096);RtlCopyMemory(b,"OFSCKP\0\0",8);p16(b+8,1);p16(b+10,72);p64(b+16,3);p64(b+24,8);p64(b+32,9);p64(b+40,5);p64(b+48,0);p64(b+56,0);p32(b+64,OpenFsCrc32C(b,64));}
static void record(UCHAR*b,USHORT kind,ULONGLONG seq,ULONGLONG tx,const UCHAR*p,ULONG n){RtlZeroMemory(b,4096);RtlCopyMemory(b,"OFSJNL\0\0",8);p16(b+8,1);p16(b+10,40);p16(b+12,kind);p64(b+16,seq);p64(b+24,tx);p32(b+32,n);if(n)RtlCopyMemory(b+40,p,n);p32(b+36,crc_record(b,p,n));}
int main(void){
 UCHAR b[4096],payload[3]={1,2,3};OPENFS_JOURNAL_CHECKPOINT cp;OPENFS_JOURNAL_RECORD_VIEW r;
 checkpoint(b);assert(OpenFsDecodeJournalCheckpointBlock(b,4096,8,&cp)==STATUS_SUCCESS);assert(cp.Generation==3&&cp.NextTransactionId==5&&cp.HeadBlock==0&&cp.TailBlock==0);
 b[64]^=1;assert(OpenFsDecodeJournalCheckpointBlock(b,4096,8,&cp)==STATUS_CRC_ERROR);
 checkpoint(b);b[90]=1;assert(OpenFsDecodeJournalCheckpointBlock(b,4096,8,&cp)==STATUS_DISK_CORRUPT_ERROR);
 checkpoint(b);p64(b+48,8);p32(b+64,OpenFsCrc32C(b,64));assert(OpenFsDecodeJournalCheckpointBlock(b,4096,8,&cp)==STATUS_DISK_CORRUPT_ERROR);
 record(b,OpenFsJournalUpdate,12,7,payload,3);assert(OpenFsDecodeJournalRecordBlock(b,4096,&r)==STATUS_SUCCESS);assert(r.Sequence==12&&r.TransactionId==7&&r.PayloadLength==3&&RtlCompareMemory(r.Payload,payload,3)==3);
 b[36]^=1;assert(OpenFsDecodeJournalRecordBlock(b,4096,&r)==STATUS_CRC_ERROR);
 record(b,OpenFsJournalBegin,1,1,NULL,0);b[4095]=1;assert(OpenFsDecodeJournalRecordBlock(b,4096,&r)==STATUS_DISK_CORRUPT_ERROR);
 record(b,99,1,1,NULL,0);assert(OpenFsDecodeJournalRecordBlock(b,4096,&r)==STATUS_DISK_CORRUPT_ERROR);
 record(b,OpenFsJournalBegin,1,1,payload,3);assert(OpenFsDecodeJournalRecordBlock(b,4096,&r)==STATUS_DISK_CORRUPT_ERROR);
 record(b,OpenFsJournalCommit,1,1,NULL,0);p64(b+16,0);p32(b+36,crc_record(b,NULL,0));assert(OpenFsDecodeJournalRecordBlock(b,4096,&r)==STATUS_DISK_CORRUPT_ERROR);
 {
  OPENFS_JOURNAL_RECORD_VIEW stream[5]; OPENFS_JOURNAL_TRANSACTION_VIEW tx[2];
  ULONG required=0;
  record(b,OpenFsJournalBegin,1,1,NULL,0);assert(OpenFsDecodeJournalRecordBlock(b,4096,&stream[0])==STATUS_SUCCESS);
  record(b,OpenFsJournalUpdate,2,1,payload,3);assert(OpenFsDecodeJournalRecordBlock(b,4096,&stream[1])==STATUS_SUCCESS);
  record(b,OpenFsJournalCommit,3,1,NULL,0);assert(OpenFsDecodeJournalRecordBlock(b,4096,&stream[2])==STATUS_SUCCESS);
  record(b,OpenFsJournalBegin,4,2,NULL,0);assert(OpenFsDecodeJournalRecordBlock(b,4096,&stream[3])==STATUS_SUCCESS);
  record(b,OpenFsJournalUpdate,5,2,payload,3);assert(OpenFsDecodeJournalRecordBlock(b,4096,&stream[4])==STATUS_SUCCESS);
  assert(OpenFsPlanJournalRecovery(stream,5,0,1,tx,2,&required)==STATUS_SUCCESS);
  assert(required==1&&tx[0].TransactionId==1&&tx[0].FirstUpdateIndex==1&&tx[0].UpdateCount==1);
  assert(OpenFsPlanJournalRecovery(stream,5,0,1,tx,0,&required)==STATUS_BUFFER_TOO_SMALL&&required==1);
  stream[4].TransactionId=99;
  assert(OpenFsPlanJournalRecovery(stream,5,0,1,tx,2,&required)==STATUS_DISK_CORRUPT_ERROR);
 }

 {
  OPENFS_JOURNAL_CHECKPOINT outCp={3,8,9,5,0,0}, decodedCp;
  OPENFS_JOURNAL_RECORD_VIEW decodedRecord;
  assert(OpenFsEncodeJournalCheckpointBlock(&outCp,4096,8,b)==STATUS_SUCCESS);
  assert(OpenFsDecodeJournalCheckpointBlock(b,4096,8,&decodedCp)==STATUS_SUCCESS);
  assert(decodedCp.Generation==outCp.Generation&&decodedCp.CheckpointSequence==outCp.CheckpointSequence);
  assert(OpenFsEncodeJournalRecordBlock(OpenFsJournalUpdate,77,9,payload,3,4096,b)==STATUS_SUCCESS);
  assert(OpenFsDecodeJournalRecordBlock(b,4096,&decodedRecord)==STATUS_SUCCESS);
  assert(decodedRecord.Sequence==77&&decodedRecord.TransactionId==9&&decodedRecord.PayloadLength==3);
  assert(OpenFsEncodeJournalRecordBlock(OpenFsJournalCommit,78,9,payload,3,4096,b)==STATUS_INVALID_PARAMETER);
  outCp.HeadBlock=1;
  assert(OpenFsEncodeJournalCheckpointBlock(&outCp,4096,8,b)==STATUS_INVALID_PARAMETER);
 }

 {
  OPENFS_JOURNAL_RECORD_VIEW chunks[2]; UCHAR p0[12+4044],p1[12+52],image[4096];
  ULONGLONG target=0; ULONG k;
  RtlZeroMemory(p0,sizeof(p0));RtlZeroMemory(p1,sizeof(p1));
  p64(p0,42);p16(p0+8,0);p16(p0+10,2);
  p64(p1,42);p16(p1+8,1);p16(p1+10,2);
  for(k=0;k<4044;k++)p0[12+k]=(UCHAR)(k&0xff);
  for(k=0;k<52;k++)p1[12+k]=(UCHAR)((k+9)&0xff);
  chunks[0].Kind=OpenFsJournalUpdate;chunks[0].Sequence=1;chunks[0].TransactionId=3;chunks[0].PayloadLength=sizeof(p0);chunks[0].Payload=p0;
  chunks[1].Kind=OpenFsJournalUpdate;chunks[1].Sequence=2;chunks[1].TransactionId=3;chunks[1].PayloadLength=sizeof(p1);chunks[1].Payload=p1;
  RtlFillMemory(image,sizeof(image),0xCC);
  assert(OpenFsDecodeRedoImage(chunks,0,2,4096,&target,image)==STATUS_SUCCESS&&target==42);
  assert(image[0]==0&&image[4043]==(UCHAR)(4043&0xff)&&image[4044]==9&&image[4095]==60);
  p16(p1+8,0);
  RtlFillMemory(image,sizeof(image),0xCC);
  assert(OpenFsDecodeRedoImage(chunks,0,2,4096,&target,image)==STATUS_DISK_CORRUPT_ERROR&&image[0]==0xCC);
 }
 puts("OpenFS journal codec and recovery-plan tests passed");return 0;
}
