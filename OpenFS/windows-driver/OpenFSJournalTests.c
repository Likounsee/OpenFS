#include "OpenFSJournal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
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
  stream[3].TransactionId=3;
  assert(OpenFsPlanJournalRecovery(stream,5,0,1,tx,2,&required)==STATUS_DISK_CORRUPT_ERROR);
  stream[3].TransactionId=2; stream[4].TransactionId=99;
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
  memset(image,0xCC,sizeof(image));
  assert(OpenFsDecodeRedoImage(chunks,0,2,4096,&target,image)==STATUS_SUCCESS&&target==42);
  assert(image[0]==0&&image[4043]==(UCHAR)(4043&0xff)&&image[4044]==9&&image[4095]==60);
  p16(p1+8,0);
  memset(image,0xCC,sizeof(image));
  assert(OpenFsDecodeRedoImage(chunks,0,2,4096,&target,image)==STATUS_DISK_CORRUPT_ERROR&&image[0]==0xCC);
 }

 {
  UCHAR ring[4][4096]; const UCHAR* slots[4]; OPENFS_JOURNAL_RECORD_VIEW scanned[4];
  OPENFS_JOURNAL_TRANSACTION_VIEW tx[2]; ULONG required=0, k;
  record(ring[0],OpenFsJournalBegin,1,1,NULL,0);
  record(ring[1],OpenFsJournalUpdate,2,1,payload,3);
  record(ring[2],OpenFsJournalCommit,3,1,NULL,0);
  RtlZeroMemory(ring[3],4096);
  for(k=0;k<4;k++)slots[k]=ring[k];
  assert(OpenFsScanJournalRing(slots,4,4096,0,1,scanned,4,&required)==STATUS_SUCCESS&&required==3);
  assert(OpenFsPlanJournalRecovery(scanned,required,0,1,tx,2,&required)==STATUS_SUCCESS&&required==1);
  RtlZeroMemory(ring[1],4096);
  record(ring[2],OpenFsJournalCommit,3,1,NULL,0);
  assert(OpenFsScanJournalRing(slots,4,4096,0,1,scanned,4,&required)==STATUS_DISK_CORRUPT_ERROR);
  record(ring[2],OpenFsJournalUpdate,3,1,payload,3);
  assert(OpenFsScanJournalRing(slots,4,4096,0,1,scanned,4,&required)==STATUS_SUCCESS&&required==1);
 }

 {
  OPENFS_JOURNAL_CHECKPOINT a={1,0,1,1,0,0}, bcp={2,0,1,1,0,0}, selected;
  UCHAR active=9;
  assert(OpenFsSelectJournalCheckpoint(STATUS_SUCCESS,&a,STATUS_SUCCESS,&bcp,&selected,&active)==STATUS_SUCCESS&&active==1&&selected.Generation==2);
  bcp.Generation=3;
  assert(OpenFsSelectJournalCheckpoint(STATUS_SUCCESS,&a,STATUS_SUCCESS,&bcp,&selected,&active)==STATUS_DISK_CORRUPT_ERROR);
  bcp.Generation=2;bcp.NextTransactionId=2;
  assert(OpenFsSelectJournalCheckpoint(STATUS_SUCCESS,&a,STATUS_SUCCESS,&bcp,&selected,&active)==STATUS_DISK_CORRUPT_ERROR);
  bcp.Generation=2;bcp.NextTransactionId=1;bcp.CheckpointSequence=1;bcp.NextSequence=2;
  assert(OpenFsSelectJournalCheckpoint(STATUS_SUCCESS,&a,STATUS_SUCCESS,&bcp,&selected,&active)==STATUS_DISK_CORRUPT_ERROR);
  bcp.Generation=2;bcp.NextTransactionId=1;bcp.CheckpointSequence=0;bcp.NextSequence=1;
  assert(OpenFsSelectJournalCheckpoint(STATUS_SUCCESS,&a,STATUS_SUCCESS,&bcp,&selected,&active)==STATUS_SUCCESS&&active==1);
  assert(OpenFsSelectJournalCheckpoint(STATUS_DISK_CORRUPT_ERROR,NULL,STATUS_SUCCESS,&bcp,&selected,&active)==STATUS_SUCCESS&&active==1);
 }

 
 {
  OPENFS_JOURNAL_RECORD_VIEW redoRecords[4];
  OPENFS_JOURNAL_TRANSACTION_VIEW redoTx[2];
  ULONGLONG redoTargets[4], target=42;
  UCHAR p0[4056], p1[64], scratch[4096];
  ULONG requiredTargets=0, k;
  RtlZeroMemory(p0,sizeof(p0)); RtlZeroMemory(p1,sizeof(p1));
  for(k=0;k<4044;k++) p0[12+k]=(UCHAR)(k&0xff);
  for(k=0;k<52;k++) p1[12+k]=(UCHAR)((k+9)&0xff);
  p64(p0,target); p16(p0+8,0); p16(p0+10,2);
  p64(p1,target); p16(p1+8,1); p16(p1+10,2);
  redoRecords[0].Kind=OpenFsJournalUpdate; redoRecords[0].TransactionId=3;
  redoRecords[0].Payload=p0; redoRecords[0].PayloadLength=sizeof(p0);
  redoRecords[1].Kind=OpenFsJournalUpdate; redoRecords[1].TransactionId=3;
  redoRecords[1].Payload=p1; redoRecords[1].PayloadLength=sizeof(p1);
  redoTx[0].TransactionId=3; redoTx[0].FirstUpdateIndex=0; redoTx[0].UpdateCount=2;
  assert(OpenFsValidateRedoTargets(redoRecords,2,redoTx,1,4096,40,100,
      redoTargets,4,&requiredTargets,scratch)==STATUS_SUCCESS);
  assert(requiredTargets==1&&redoTargets[0]==42);
  assert(OpenFsValidateRedoTargets(redoRecords,2,redoTx,1,4096,50,100,
      redoTargets,4,&requiredTargets,scratch)==STATUS_DISK_CORRUPT_ERROR);
  redoRecords[2]=redoRecords[0]; redoRecords[3]=redoRecords[1];
  redoTx[0].UpdateCount=4;
  assert(OpenFsValidateRedoTargets(redoRecords,4,redoTx,1,4096,40,100,
      redoTargets,4,&requiredTargets,scratch)==STATUS_DISK_CORRUPT_ERROR);
  redoTx[0].UpdateCount=2; redoTx[1].TransactionId=4;
  redoTx[1].FirstUpdateIndex=2; redoTx[1].UpdateCount=2;
  redoRecords[2].TransactionId=4; redoRecords[3].TransactionId=4;
  assert(OpenFsValidateRedoTargets(redoRecords,4,redoTx,2,4096,40,100,
      redoTargets,4,&requiredTargets,scratch)==STATUS_SUCCESS);
  assert(requiredTargets==2&&redoTargets[0]==42&&redoTargets[1]==42);
 }


 {
  OPENFS_JOURNAL_REDO_UPDATE update;
  OPENFS_JOURNAL_RECORD_VIEW decoded[16];
  UCHAR image[4096], encoded[4*4096], scratch[4096];
  ULONG count=0, i;
  ULONGLONG commit=0,next=0;
  for(i=0;i<sizeof(image);i++) image[i]=(UCHAR)(i*7u);
  update.TargetBlock=77; update.Image=image;
  assert(OpenFsBuildJournalTransaction(&update,1,4096,4,64,100,9,5,
      encoded,sizeof(encoded),&count,&commit,&next,scratch)==STATUS_SUCCESS);
  assert(count==4&&commit==12&&next==13);
  for(i=0;i<count;i++)
    assert(OpenFsDecodeJournalRecordBlock(encoded+i*4096,4096,&decoded[i])==STATUS_SUCCESS);
  assert(decoded[0].Kind==OpenFsJournalBegin&&decoded[0].Sequence==9);
  assert(decoded[1].Kind==OpenFsJournalUpdate&&decoded[1].Sequence==10);
  assert(decoded[2].Kind==OpenFsJournalUpdate&&decoded[2].Sequence==11);
  assert(decoded[3].Kind==OpenFsJournalCommit&&decoded[3].Sequence==12);
  {
    UCHAR rebuilt[4096]; ULONGLONG target=0;
    assert(OpenFsDecodeRedoImage(decoded,1,2,4096,&target,rebuilt)==STATUS_SUCCESS);
    assert(target==77&&memcmp(rebuilt,image,sizeof(image))==0);
  }
  update.TargetBlock=100;
  memset(encoded,0xA5,sizeof(encoded));
  assert(OpenFsBuildJournalTransaction(&update,1,4096,4,64,100,9,5,
      encoded,sizeof(encoded),&count,&commit,&next,scratch)==STATUS_DISK_CORRUPT_ERROR);
  assert(encoded[0]==0xA5);
  {
    OPENFS_JOURNAL_REDO_UPDATE tooMany[2];
    tooMany[0]=update; tooMany[1].TargetBlock=78; tooMany[1].Image=image;
    assert(OpenFsBuildJournalTransaction(tooMany,2,4096,4,64,100,9,5,
        encoded,sizeof(encoded),&count,&commit,&next,scratch)==STATUS_BUFFER_TOO_SMALL);
  }
 }


 {
  static UCHAR largeImage[65536], largeEncoded[4*65536], largeScratch[65536];
  OPENFS_JOURNAL_REDO_UPDATE updates[2];
  OPENFS_JOURNAL_RECORD_VIEW records[4];
  ULONG count=0, k;
  ULONGLONG commit=0,next=0,target=0;
  UCHAR rebuilt[65536];
  for(k=0;k<sizeof(largeImage);k++) largeImage[k]=(UCHAR)((k*13u)+5u);
  updates[0].TargetBlock=88; updates[0].Image=largeImage;
  assert(OpenFsBuildJournalTransaction(updates,1,65536,4,64,100,20,9,
      largeEncoded,sizeof(largeEncoded),&count,&commit,&next,largeScratch)==STATUS_SUCCESS);
  assert(count==4&&commit==23&&next==24);
  for(k=0;k<count;k++)
    assert(OpenFsDecodeJournalRecordBlock(largeEncoded+k*65536,65536,&records[k])==STATUS_SUCCESS);
  assert(OpenFsDecodeRedoImage(records,1,2,65536,&target,rebuilt)==STATUS_SUCCESS);
  assert(target==88&&memcmp(rebuilt,largeImage,sizeof(largeImage))==0);

  updates[1]=updates[0];
  memset(largeEncoded,0x6B,sizeof(largeEncoded));
  assert(OpenFsBuildJournalTransaction(updates,2,65536,4,64,100,20,9,
      largeEncoded,sizeof(largeEncoded),&count,&commit,&next,largeScratch)==STATUS_DISK_CORRUPT_ERROR);
  assert(largeEncoded[0]==0x6B);
  updates[1].TargetBlock=89;
  assert(OpenFsBuildJournalTransaction(updates,1,65536,4,64,100,
      MAXULONGLONG-2u,9,largeEncoded,sizeof(largeEncoded),&count,&commit,&next,
      largeScratch)==STATUS_INTEGER_OVERFLOW);
 }

 puts("OpenFS journal codec and recovery-plan tests passed");return 0;
}
