#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "openfs/transaction.h"
#include "openfs/format.h"
#include "openfs/mount.h"
#include "openfs/runtime.h"
#include "openfs/path.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)
typedef struct{uint8_t*b;uint32_t bs;uint64_t bc;uint64_t fail_start;int fail_data;int arm_flush_fail;int flush_failed;uint64_t arm_block;int fail_flush;uint64_t fail_exact;int fail_exact_enabled;int fail_exact_once;uint64_t partial_block;size_t partial_bytes;size_t partial_next_bytes;int partial_enabled;int partial_once;int partial_change_after_once;}D;
static openfs_io_result_t r(void*c,uint64_t f,uint32_t n,void*x){D*d=c;if(n==0U||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(x,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t w(void*c,uint64_t f,uint32_t n,const void*x){D*d=c;if(d->fail_exact_enabled&&f==d->fail_exact){if(d->fail_exact_once)d->fail_exact_enabled=0;return OPENFS_IO_IO_ERROR;}if(d->fail_data&&f>=d->fail_start)return OPENFS_IO_IO_ERROR;if(d->partial_enabled&&f==d->partial_block){size_t bytes=(size_t)((uint64_t)n*d->bs);if(d->partial_bytes!=0U&&d->partial_bytes<bytes)bytes=d->partial_bytes;memcpy(d->b+(size_t)(f*d->bs),x,bytes);if(d->partial_once){if(d->partial_change_after_once)d->partial_bytes=d->partial_next_bytes;else d->partial_enabled=0;}return OPENFS_IO_IO_ERROR;}if(n==0U||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),x,(size_t)((uint64_t)n*d->bs));if(d->arm_flush_fail&&f==d->arm_block)d->flush_failed=1;return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){D*d=c;if(d->flush_failed||d->fail_flush)return OPENFS_IO_IO_ERROR;return OPENFS_IO_OK;}
static int commit_full_cleans_active_transaction(void){
D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bc,d.bs);CHECK(d.b);
openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};uint8_t uuid[16]={13U};CHECK(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
openfs_superblock_t s;CHECK(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);openfs_runtime_t runtime;CHECK(openfs_runtime_init(&runtime));j.runtime=&runtime;
openfs_transaction_t t;CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);
while(j.next_record<j.journal_blocks)CHECK(openfs_journal_write(&j,&v,t.txid,"x",1U)==OPENFS_JOURNAL_OK);
CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_FULL);CHECK(t.active==0);CHECK(j.active_transaction_id==0);
CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_OK);CHECK(openfs_runtime_shutdown_if_unused(&runtime));
free(d.b);return 0;
}
static int double_begin_preserves_transaction(void){
D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bc,d.bs);CHECK(d.b);
openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};CHECK(openfs_transaction_from_device(&v)==NULL);uint8_t uuid[16]={23U};CHECK(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
openfs_superblock_t s;CHECK(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
openfs_transaction_t t;CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);uint64_t tx=t.txid;CHECK(t.active==1&&t.txid==tx);openfs_block_device_t*td=openfs_transaction_device(&t);CHECK(td!=NULL);
uint64_t target=s.data_start+7U;uint8_t a[4096];memset(a,0x4DU,sizeof(a));CHECK(td->write(td->context,target,1U,a)==OPENFS_IO_OK);uint64_t pending=t.pending_count;CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_INVALID_ARGUMENT);CHECK(t.active==1&&t.txid==tx&&t.pending_count==pending&&t.pending!=NULL);CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_OK);free(d.b);return 0;
}
static int poisoned_namespace_transaction(void){D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bc,d.bs);CHECK(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};uint8_t uuid[16]={7U};CHECK(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);openfs_superblock_t s;CHECK(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);openfs_transaction_t t;CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);uint64_t ino=0U;CHECK(openfs_path_create_tx(&t,&s,"/poison",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);CHECK(openfs_path_create_tx(&t,&s,"/poison",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_EXISTS);CHECK(t.failed==1);CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_IO_ERROR);CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_OK);CHECK(openfs_path_lookup(&v,&s,"/poison",&ino)==OPENFS_PATH_NOT_FOUND);free(d.b);return 0;}
static int checkpoint_partial_write_rollback(void){
D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bc,d.bs);CHECK(d.b);
openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};uint8_t uuid[16]={12U};CHECK(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
openfs_superblock_t s;CHECK(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);uint64_t tx=0U;CHECK(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);uint8_t seed[4096];memset(seed,0xA6U,sizeof(seed));CHECK(openfs_journal_write(&j,&v,tx,seed,3U)==OPENFS_JOURNAL_OK);CHECK(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
d.partial_block=s.journal_start;d.partial_bytes=1024U;d.partial_enabled=1;d.partial_once=1;
CHECK(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_IO_ERROR);d.partial_enabled=0;
CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
free(d.b);

D e={0};e.bs=4096U;e.bc=256U;e.b=calloc((size_t)e.bc,e.bs);CHECK(e.b);
openfs_block_device_t wdev={&e,e.bs,e.bc,r,w,fl};uint8_t uuid2[16]={14U};CHECK(openfs_format(&wdev,uuid2)==OPENFS_FORMAT_OK);
openfs_superblock_t sb;CHECK(openfs_read_superblock(&wdev,&sb)==OPENFS_FORMAT_OK);openfs_journal_t k;CHECK(openfs_journal_open(&k,&wdev,&sb)==OPENFS_JOURNAL_OK);uint64_t tx2=0U;CHECK(openfs_journal_begin(&k,&wdev,&tx2)==OPENFS_JOURNAL_OK);uint8_t long_seed[2048];memset(long_seed,0xB7U,sizeof(long_seed));CHECK(openfs_journal_write(&k,&wdev,tx2,seed,3U)==OPENFS_JOURNAL_OK);CHECK(openfs_journal_write(&k,&wdev,tx2,long_seed,sizeof(long_seed))==OPENFS_JOURNAL_OK);CHECK(openfs_journal_write(&k,&wdev,tx2,long_seed,sizeof(long_seed))==OPENFS_JOURNAL_OK);CHECK(openfs_journal_write(&k,&wdev,tx2,long_seed,sizeof(long_seed))==OPENFS_JOURNAL_OK);CHECK(openfs_journal_commit(&k,&wdev,tx2)==OPENFS_JOURNAL_OK);
e.partial_block=sb.journal_start+4U;e.partial_bytes=2048U;e.partial_next_bytes=1024U;e.partial_enabled=1;e.partial_once=1;e.partial_change_after_once=1;
CHECK(openfs_journal_checkpoint(&k,&wdev)==OPENFS_JOURNAL_CORRUPT);e.partial_enabled=0;CHECK(e.b[(size_t)((sb.journal_start+4U)*e.bs+32U+1500U)]==0U);
CHECK(openfs_journal_open(&k,&wdev,&sb)==OPENFS_JOURNAL_CORRUPT);
free(e.b);return 0;
}
static int partial_commit_record_is_aborted_safely(void){
D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bc,d.bs);CHECK(d.b);
openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};uint8_t uuid[16]={16U};CHECK(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
openfs_superblock_t s;CHECK(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
openfs_transaction_t t;uint64_t target=s.data_start+25U;uint8_t a[4096];memset(a,0x61U,sizeof(a));
CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);openfs_block_device_t *td=openfs_transaction_device(&t);CHECK(td!=NULL);CHECK(td->write(td->context,target,1U,a)==OPENFS_IO_OK);
d.partial_block=s.journal_start+j.next_record;d.partial_bytes=512U;d.partial_enabled=1;d.partial_once=1;CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_IO_ERROR);CHECK(t.active==1&&t.committed==0&&j.active_transaction_id==t.txid);openfs_transaction_t blocked;CHECK(openfs_transaction_begin(&blocked,&v,&j)!=OPENFS_TRANSACTION_OK);
d.partial_enabled=0;CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_OK);openfs_mount_t m;CHECK(openfs_mount(&m,&v)==OPENFS_MOUNT_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))!=0);CHECK(openfs_unmount(&m)==OPENFS_MOUNT_OK);
free(d.b);return 0;
}
static int committed_partial_publication_replays(void){
D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bc,d.bs);CHECK(d.b);
openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};uint8_t uuid[16]={15U};CHECK(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
openfs_superblock_t s;CHECK(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
openfs_transaction_t t;uint64_t target=s.data_start+24U;uint8_t a[4096];memset(a,0x7CU,sizeof(a));
CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);openfs_block_device_t *td=openfs_transaction_device(&t);CHECK(td!=NULL);CHECK(td->write(td->context,target,1U,a)==OPENFS_IO_OK);
d.partial_block=target;d.partial_bytes=512U;d.partial_enabled=1;CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_IO_ERROR);CHECK(t.active==1&&t.committed==1);CHECK(j.recovery_required==1U);CHECK(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_IO_ERROR);
d.partial_enabled=0;openfs_mount_t m;CHECK(openfs_mount(&m,&v)==OPENFS_MOUNT_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))==0);CHECK(openfs_unmount(&m)==OPENFS_MOUNT_OK);
free(d.b);return 0;
}
static int committed_checkpoint_rollback_failure_is_corruption(void){
D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bc,d.bs);CHECK(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};uint8_t uuid[16]={13U};CHECK(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);openfs_superblock_t s;CHECK(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);openfs_transaction_t t;uint64_t target=s.data_start+31U;uint8_t a[4096];memset(a,0x6DU,sizeof(a));CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);openfs_block_device_t*td=openfs_transaction_device(&t);CHECK(td!=NULL);CHECK(td->write(td->context,target,1U,a)==OPENFS_IO_OK);d.partial_block=s.journal_start;d.partial_bytes=2048U;d.partial_next_bytes=1024U;d.partial_enabled=1;d.partial_once=1;d.partial_change_after_once=1;CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_CORRUPT);CHECK(t.active==1&&t.committed==1&&t.recovery_required==1);d.partial_enabled=0;CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_CORRUPT);CHECK(t.active==0);free(d.b);return 0;}
static int checkpoint_failure_recovery(void){
D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bc,d.bs);CHECK(d.b);
openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};uint8_t uuid[16]={11U};CHECK(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
openfs_superblock_t s;CHECK(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
openfs_transaction_t t;uint64_t target=s.data_start+12U;uint8_t a[4096];memset(a,0x3CU,sizeof(a));
CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);openfs_block_device_t *td=openfs_transaction_device(&t);CHECK(td!=NULL);CHECK(td->write(td->context,target,1U,a)==OPENFS_IO_OK);
d.fail_exact=s.journal_start+1U;d.fail_exact_enabled=1;d.fail_exact_once=1;CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_IO_ERROR);CHECK(t.active==1&&t.committed==1);CHECK(j.recovery_required==1U);CHECK(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_IO_ERROR);
d.fail_exact_enabled=0;CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_CORRUPT);CHECK(t.active==0);CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_IO_ERROR);
openfs_mount_t recovered;CHECK(openfs_mount(&recovered,&v)==OPENFS_MOUNT_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))==0);CHECK(openfs_unmount(&recovered)==OPENFS_MOUNT_OK);
free(d.b);return 0;}

static int wal_corruption_poison_aborts_without_checkpoint(void){
D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bc,d.bs);CHECK(d.b);
openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};uint8_t uuid[16]={21U};CHECK(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
openfs_superblock_t s;CHECK(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
openfs_transaction_t t;CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);
openfs_block_device_t *td=openfs_transaction_device(&t);CHECK(td!=NULL);uint8_t a[4096];memset(a,0x3AU,sizeof(a));
d.partial_block=s.journal_start+j.next_record;d.partial_bytes=512U;d.partial_next_bytes=1024U;d.partial_enabled=1;d.partial_once=1;d.partial_change_after_once=1;
CHECK(td->write(td->context,s.data_start+8U,1U,a)==OPENFS_IO_IO_ERROR);
CHECK(t.failed==1&&t.recovery_required==1&&t.active==1&&j.active_transaction_id==t.txid);
d.partial_enabled=0;
CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_CORRUPT);CHECK(t.active==0);CHECK(j.active_transaction_id==0U);CHECK(j.recovery_required==1U);CHECK(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_IO_ERROR);
free(d.b);return 0;
}

int main(void){
CHECK(wal_corruption_poison_aborts_without_checkpoint()==0);CHECK(commit_full_cleans_active_transaction()==0);CHECK(double_begin_preserves_transaction()==0);
CHECK(poisoned_namespace_transaction()==0);
CHECK(checkpoint_partial_write_rollback()==0);
CHECK(partial_commit_record_is_aborted_safely()==0);
CHECK(committed_partial_publication_replays()==0);
CHECK(checkpoint_failure_recovery()==0);
CHECK(committed_checkpoint_rollback_failure_is_corruption()==0);
D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bc,d.bs);CHECK(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};uint8_t uuid[16]={9U};CHECK(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);openfs_superblock_t s;CHECK(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);uint64_t target=s.data_start+10U;uint8_t a[4096],b[4096],readback[4096];memset(a,0xA5U,sizeof(a));memset(b,0x5AU,sizeof(b));openfs_transaction_t t;d.fail_exact=s.journal_start;d.fail_exact_enabled=1;d.fail_exact_once=0;CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_CORRUPT);CHECK(t.active==0);d.fail_exact_enabled=0;d.fail_exact_once=1;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);openfs_block_device_t*td=openfs_transaction_device(&t);CHECK(td!=NULL);CHECK(td->write(td->context,target,1U,a)==OPENFS_IO_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))!=0);CHECK(td->read(td->context,target,1U,readback)==OPENFS_IO_OK&&memcmp(readback,a,sizeof(a))==0);CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))==0);CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);td=openfs_transaction_device(&t);CHECK(td->write(td->context,target,1U,b)==OPENFS_IO_OK);CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))==0);CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);td=openfs_transaction_device(&t);while(j.next_record<j.journal_blocks)CHECK(openfs_journal_write(&j,&v,t.txid,"x",1U)==OPENFS_JOURNAL_OK);CHECK(td->write(td->context,target,1U,b)==OPENFS_IO_IO_ERROR);CHECK(td->write(td->context,target,1U,b)==OPENFS_IO_IO_ERROR);CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_IO_ERROR);CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))==0);CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);td=openfs_transaction_device(&t);CHECK(td->write(td->context,target,1U,b)==OPENFS_IO_OK);d.fail_start=s.journal_start+s.journal_blocks;d.fail_data=1;CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_IO_ERROR);CHECK(t.active==1&&t.failed==1&&t.committed==1);CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_CORRUPT);CHECK(t.active==0);d.fail_data=0;openfs_mount_t recovered;CHECK(openfs_mount(&recovered,&v)==OPENFS_MOUNT_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),b,sizeof(b))==0);CHECK(openfs_unmount(&recovered)==OPENFS_MOUNT_OK);CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);td=openfs_transaction_device(&t);d.arm_block=target;d.arm_flush_fail=1;CHECK(td->write(td->context,target,1U,a)==OPENFS_IO_OK);CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_IO_ERROR);d.arm_flush_fail=0;d.flush_failed=0;CHECK(openfs_mount(&recovered,&v)==OPENFS_MOUNT_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))==0);CHECK(openfs_unmount(&recovered)==OPENFS_MOUNT_OK);CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);td=openfs_transaction_device(&t);CHECK(td->write(td->context,target,1U,b)==OPENFS_IO_OK);d.fail_flush=1;CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_IO_ERROR);CHECK(t.active==1&&t.commit_started==1&&t.committed==1&&t.recovery_required==1);d.fail_flush=0;CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_CORRUPT);CHECK(t.active==0);CHECK(openfs_mount(&recovered,&v)==OPENFS_MOUNT_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),b,sizeof(b))==0);CHECK(openfs_unmount(&recovered)==OPENFS_MOUNT_OK);CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);td=openfs_transaction_device(&t);CHECK(td->write(td->context,target,1U,a)==OPENFS_IO_OK);d.fail_exact=s.journal_start+j.next_record;d.fail_exact_enabled=1;d.fail_exact_once=1;CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_IO_ERROR);CHECK(t.active==1&&t.committed==0);d.fail_exact_enabled=0;CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_OK);CHECK(openfs_mount(&recovered,&v)==OPENFS_MOUNT_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),b,sizeof(b))==0);CHECK(openfs_unmount(&recovered)==OPENFS_MOUNT_OK);free(d.b);return 0;}