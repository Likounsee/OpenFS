#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "openfs/transaction.h"
#include "openfs/allocator.h"
#include "openfs/bitmap.h"
#include "openfs/format.h"
#include "openfs/mount.h"
#include "openfs/runtime.h"
#include "openfs/path.h"
#include "openfs/file.h"
#include "openfs/inode.h"
#include "openfs/fsck.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)
typedef struct{uint8_t*b;uint32_t bs;uint64_t bc;uint64_t fail_start;int fail_data;int arm_flush_fail;int flush_failed;uint64_t arm_block;int fail_flush;uint64_t fail_exact;int fail_exact_enabled;int fail_exact_once;uint64_t partial_block;size_t partial_bytes;size_t partial_next_bytes;int partial_enabled;int partial_once;int partial_change_after_once;}D;
static openfs_io_result_t r(void*c,uint64_t f,uint32_t n,void*x){D*d=c;if(n==0U||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(x,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t w(void*c,uint64_t f,uint32_t n,const void*x){D*d=c;if(n==0U||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;if(d->fail_exact_enabled&&f==d->fail_exact){if(d->fail_exact_once)d->fail_exact_enabled=0;return OPENFS_IO_IO_ERROR;}if(d->fail_data&&f>=d->fail_start)return OPENFS_IO_IO_ERROR;if(d->partial_enabled&&f==d->partial_block){size_t bytes=(size_t)((uint64_t)n*d->bs);if(d->partial_bytes!=0U&&d->partial_bytes<bytes)bytes=d->partial_bytes;memcpy(d->b+(size_t)(f*d->bs),x,bytes);if(d->partial_once){if(d->partial_change_after_once)d->partial_bytes=d->partial_next_bytes;else d->partial_enabled=0;}return OPENFS_IO_IO_ERROR;}memcpy(d->b+(size_t)(f*d->bs),x,(size_t)((uint64_t)n*d->bs));if(d->arm_flush_fail&&f==d->arm_block)d->flush_failed=1;return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){D*d=c;if(d->flush_failed||d->fail_flush)return OPENFS_IO_IO_ERROR;return OPENFS_IO_OK;}
static int mock_device_rejects_out_of_range_before_fault_injection(void)
{
    D d = {0};
    d.bs = 4U;
    d.bc = 2U;
    d.b = calloc((size_t)d.bc, d.bs);
    CHECK(d.b);
    uint8_t input[8];
    memset(input, 0xA5U, sizeof(input));
    d.partial_enabled = 1;
    d.partial_block = d.bc - 1U;
    d.partial_bytes = 2U;

    CHECK(w(&d, d.bc, 1U, input) == OPENFS_IO_OUT_OF_RANGE);
    CHECK(d.partial_enabled == 1);
    CHECK(w(&d, d.bc - 1U, 2U, input) == OPENFS_IO_OUT_OF_RANGE);
    CHECK(d.partial_enabled == 1);
    for (size_t i = 0U; i < (size_t)d.bc * d.bs; ++i) {
        CHECK(d.b[i] == 0U);
    }

    free(d.b);
    return 0;
}
static openfs_journal_result_t recovery_payload_cb(void *ctx,uint64_t tx,const uint8_t *payload,uint32_t length){uint32_t *hits=(uint32_t*)ctx;if(tx!=1U||length!=3U||memcmp(payload,"abc",3U)!=0)return OPENFS_JOURNAL_CORRUPT;(*hits)++;return OPENFS_JOURNAL_OK;}
static int commit_flush_failure_recovers_same_journal(void){
D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bc,d.bs);CHECK(d.b);
openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};uint8_t uuid[16]={32U};CHECK(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
openfs_superblock_t s;CHECK(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
uint64_t tx=0U;CHECK(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);CHECK(openfs_journal_write(&j,&v,tx,"abc",3U)==OPENFS_JOURNAL_OK);
d.fail_flush=1;int committed=0;CHECK(openfs_journal_commit_transaction(&j,&v,tx,&committed)==OPENFS_JOURNAL_IO_ERROR);CHECK(committed==1);CHECK(j.recovery_required==1U&&j.publication_in_progress==0U);
uint32_t hits=0U;CHECK(openfs_journal_recover(&j,&v,&s,recovery_payload_cb,&hits)==OPENFS_JOURNAL_IO_ERROR);CHECK(hits==1U);CHECK(j.recovery_required==1U);d.fail_flush=0;CHECK(openfs_journal_recover(&j,&v,&s,recovery_payload_cb,&hits)==OPENFS_JOURNAL_OK);CHECK(hits==2U);CHECK(j.recovery_required==0U&&j.publication_in_progress==0U);CHECK(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);
free(d.b);return 0;
}
static int checkpoint_cannot_discard_pending_publication(void){
D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bc,d.bs);CHECK(d.b);
openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};uint8_t uuid[16]={31U};CHECK(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
openfs_superblock_t s;CHECK(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
uint64_t tx=0U;CHECK(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);uint8_t payload[16]={1U,2U,3U};CHECK(openfs_journal_write(&j,&v,tx,payload,sizeof(payload))==OPENFS_JOURNAL_OK);
int committed=0;CHECK(openfs_journal_commit_transaction(&j,&v,tx,&committed)==OPENFS_JOURNAL_OK);CHECK(committed==1&&j.publication_in_progress==1U);
uint64_t blocked_tx=0U;CHECK(openfs_journal_begin(&j,&v,&blocked_tx)==OPENFS_JOURNAL_IO_ERROR);CHECK(blocked_tx==0U);CHECK(openfs_journal_write(&j,&v,tx,payload,sizeof(payload))==OPENFS_JOURNAL_IO_ERROR);
CHECK(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_IO_ERROR);CHECK(j.next_record!=0U);int second_commit=0;CHECK(openfs_journal_commit_transaction(&j,&v,tx,&second_commit)==OPENFS_JOURNAL_IO_ERROR);CHECK(second_commit==0);CHECK(j.publication_in_progress==1U&&j.recovery_required==0U);
CHECK(openfs_journal_checkpoint_transaction(&j,&v)==OPENFS_JOURNAL_OK);CHECK(j.publication_in_progress==0U&&j.next_record==0U);
free(d.b);return 0;
}
static int failed_wal_write_and_failed_rollback_poison_journal(void){
D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bc,d.bs);CHECK(d.b);
openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};uint8_t uuid[16]={34U};CHECK(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
openfs_superblock_t s;CHECK(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
d.partial_block=s.journal_start;d.partial_bytes=128U;d.partial_enabled=1;
uint64_t tx=0U;CHECK(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_CORRUPT);
CHECK(j.recovery_required==1U);CHECK(j.next_record==0U);CHECK(j.active_transaction_id==0U);
d.partial_enabled=0;
CHECK(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_IO_ERROR);
CHECK(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_IO_ERROR);
/* The failed rollback returned an error, but the simulated device's bytes are restored. A fresh scan may safely reopen the clean WAL. */
CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
free(d.b);return 0;
}
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



static int allocator_checksum_failure_poison_transaction(void)
{
    D d = {0};
    d.bs = 4096U;
    d.bc = 256U;
    d.b = calloc((size_t)d.bc, d.bs);
    CHECK(d.b);
    openfs_block_device_t v = {&d, d.bs, d.bc, r, w, fl};
    uint8_t uuid[16] = {42U};
    CHECK(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    CHECK(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    CHECK(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);

    /* Simulate a checksum-enabled volume with an invalid checksum-table
     * geometry. The failure happens after the allocation bit is staged. */
    s.feature_flags |= OPENFS_FEATURE_DATA_CHECKSUM;
    s.data_checksum_blocks = 0U;
    openfs_transaction_t t;
    CHECK(openfs_transaction_begin(&t, &v, &j) == OPENFS_TRANSACTION_OK);
    uint64_t block = UINT64_MAX;
    CHECK(openfs_alloc_block_tx(&t, &s, &block) == OPENFS_ALLOC_IO_ERROR);
    CHECK(t.failed == 1);
    CHECK(openfs_transaction_commit(&t) == OPENFS_TRANSACTION_IO_ERROR);
    CHECK(openfs_transaction_abort(&t) == OPENFS_TRANSACTION_OK);

    int used = 1;
    CHECK(openfs_bitmap_test(&v, s.block_bitmap_start,
        s.block_bitmap_blocks, s.data_start, &used) == OPENFS_BITMAP_OK);
    CHECK(used == 0);
    free(d.b);
    return 0;
}

static int allocator_metadata_failure_poison_transaction(void)
{
    D d = {0};
    d.bs = 4096U;
    d.bc = 256U;
    d.b = calloc((size_t)d.bc, d.bs);
    CHECK(d.b);
    openfs_block_device_t v = {&d, d.bs, d.bc, r, w, fl};
    uint8_t uuid[16] = {41U};
    CHECK(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    CHECK(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    CHECK(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);

    /* Force the COW metadata update to fail validation after the allocation
     * bitmap has already been staged in this transaction. */
    s.feature_flags |= OPENFS_FEATURE_COW;
    s.refcount_blocks = 0U;
    openfs_transaction_t t;
    CHECK(openfs_transaction_begin(&t, &v, &j) == OPENFS_TRANSACTION_OK);
    uint64_t block = UINT64_MAX;
    CHECK(openfs_alloc_block_tx(&t, &s, &block) == OPENFS_ALLOC_CORRUPT);
    CHECK(t.failed == 1);
    CHECK(openfs_transaction_commit(&t) == OPENFS_TRANSACTION_IO_ERROR);
    CHECK(openfs_transaction_abort(&t) == OPENFS_TRANSACTION_OK);

    int used = 1;
    CHECK(openfs_bitmap_test(&v, s.block_bitmap_start,
        s.block_bitmap_blocks, s.data_start, &used) == OPENFS_BITMAP_OK);
    CHECK(used == 0);
    free(d.b);
    return 0;
}


static int committed_namespace_transaction_replays_as_a_unit(void)
{
    D d = {0};
    d.bs = 4096U;
    d.bc = 256U;
    d.b = calloc((size_t)d.bc, d.bs);
    CHECK(d.b);
    openfs_block_device_t v = {&d, d.bs, d.bc, r, w, fl};
    uint8_t uuid[16] = {43U};
    CHECK(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    CHECK(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    CHECK(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);

    openfs_transaction_t t;
    CHECK(openfs_transaction_begin(&t, &v, &j) == OPENFS_TRANSACTION_OK);
    uint64_t inode = 0U;
    CHECK(openfs_path_create_tx(&t, &s, "/recovered-atomically",
        OPENFS_INODE_MODE_REGULAR, &inode) == OPENFS_PATH_OK);
    CHECK(inode != 0U && t.pending_count > 0U);

    /* Fail publication after COMMIT; the journal must remain the authority. */
    d.fail_start = s.data_start;
    d.fail_data = 1;
    CHECK(openfs_transaction_commit(&t) == OPENFS_TRANSACTION_IO_ERROR);
    CHECK(t.committed == 1 && t.recovery_required == 1);
    d.fail_data = 0;
    CHECK(openfs_transaction_abort(&t) == OPENFS_TRANSACTION_CORRUPT);

    openfs_mount_t recovered;
    CHECK(openfs_mount(&recovered, &v) == OPENFS_MOUNT_OK);
    uint64_t recovered_inode = 0U;
    CHECK(openfs_path_lookup(&v, &recovered.superblock,
        "/recovered-atomically", &recovered_inode) == OPENFS_PATH_OK);
    CHECK(recovered_inode == inode);
    CHECK(openfs_unmount(&recovered) == OPENFS_MOUNT_OK);
    free(d.b);
    return 0;
}


static int committed_file_allocation_replays_with_inode_owner(void)
{
    D d = {0};
    d.bs = 4096U;
    d.bc = 256U;
    d.b = calloc((size_t)d.bc, d.bs);
    CHECK(d.b);
    openfs_block_device_t v = {&d, d.bs, d.bc, r, w, fl};
    uint8_t uuid[16] = {44U};
    CHECK(openfs_format(&v, uuid) == OPENFS_FORMAT_OK);
    openfs_superblock_t s;
    CHECK(openfs_read_superblock(&v, &s) == OPENFS_FORMAT_OK);
    openfs_journal_t j;
    CHECK(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);

    openfs_transaction_t t;
    CHECK(openfs_transaction_begin(&t, &v, &j) == OPENFS_TRANSACTION_OK);
    uint64_t inode_number = 0U;
    CHECK(openfs_path_create_tx(&t, &s, "/file-owner-replay",
        OPENFS_INODE_MODE_REGULAR, &inode_number) == OPENFS_PATH_OK);
    CHECK(openfs_transaction_commit(&t) == OPENFS_TRANSACTION_OK);

    uint64_t inode_count =
        (s.inode_table_blocks * (uint64_t)s.block_size) / OPENFS_INODE_SIZE;
    openfs_inode_t inode;
    CHECK(openfs_inode_read(&v, s.inode_table_start, inode_number,
        inode_count, &inode) == OPENFS_INODE_OK);

    const uint8_t payload[] = "durable owner";
    CHECK(openfs_transaction_begin(&t, &v, &j) == OPENFS_TRANSACTION_OK);
    CHECK(openfs_file_write_tx(&t, &s, &inode, 0U, payload,
        sizeof(payload)) == OPENFS_FILE_OK);
    CHECK(t.pending_count > 0U);

    /* Fail the exact data-block publication, not the WAL COMMIT record. */
    uint64_t allocated_data_block = 0U;
    CHECK(openfs_file_map_block(&inode, 0U, &allocated_data_block) == OPENFS_FILE_OK);
    d.fail_exact = allocated_data_block;
    d.fail_exact_enabled = 1;
    d.fail_exact_once = 1;
    CHECK(openfs_transaction_commit(&t) == OPENFS_TRANSACTION_IO_ERROR);
    CHECK(t.committed == 1 && t.recovery_required == 1);
    d.fail_exact_enabled = 0;
    CHECK(openfs_transaction_abort(&t) == OPENFS_TRANSACTION_CORRUPT);

    openfs_mount_t recovered;
    CHECK(openfs_mount(&recovered, &v) == OPENFS_MOUNT_OK);
    openfs_inode_t persisted;
    CHECK(openfs_inode_read(&v, recovered.superblock.inode_table_start,
        inode_number, inode_count, &persisted) == OPENFS_INODE_OK);
    uint8_t actual[sizeof(payload)];
    size_t bytes_read = 0U;
    CHECK(openfs_file_read(&v, &recovered.superblock, &persisted, 0U,
        actual, sizeof(actual), &bytes_read) == OPENFS_FILE_OK);
    CHECK(bytes_read == sizeof(payload));
    CHECK(memcmp(actual, payload, sizeof(payload)) == 0);

    uint64_t errors = 0U;
    CHECK(openfs_fsck(&v, &recovered.superblock, &errors) == OPENFS_FSCK_OK);
    CHECK(errors == 0U);
    CHECK(openfs_unmount(&recovered) == OPENFS_MOUNT_OK);
    free(d.b);
    return 0;
}

int main(void){
CHECK(mock_device_rejects_out_of_range_before_fault_injection()==0);
CHECK(allocator_metadata_failure_poison_transaction()==0);
CHECK(committed_namespace_transaction_replays_as_a_unit()==0);CHECK(committed_file_allocation_replays_with_inode_owner()==0);
CHECK(allocator_checksum_failure_poison_transaction()==0);
CHECK(commit_flush_failure_recovers_same_journal()==0);
CHECK(failed_wal_write_and_failed_rollback_poison_journal()==0);
CHECK(checkpoint_cannot_discard_pending_publication()==0);
CHECK(wal_corruption_poison_aborts_without_checkpoint()==0);CHECK(commit_full_cleans_active_transaction()==0);CHECK(double_begin_preserves_transaction()==0);
CHECK(poisoned_namespace_transaction()==0);
CHECK(checkpoint_partial_write_rollback()==0);
CHECK(partial_commit_record_is_aborted_safely()==0);
CHECK(committed_partial_publication_replays()==0);
CHECK(checkpoint_failure_recovery()==0);
CHECK(committed_checkpoint_rollback_failure_is_corruption()==0);
D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bc,d.bs);CHECK(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};uint8_t uuid[16]={9U};CHECK(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);openfs_superblock_t s;CHECK(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);uint64_t target=s.data_start+10U;uint8_t a[4096],b[4096],readback[4096];memset(a,0xA5U,sizeof(a));memset(b,0x5AU,sizeof(b));openfs_transaction_t t={0};d.fail_exact=s.journal_start;d.fail_exact_enabled=1;d.fail_exact_once=0;CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_CORRUPT);CHECK(t.active==0);d.fail_exact_enabled=0;d.fail_exact_once=1;CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);openfs_block_device_t*td=openfs_transaction_device(&t);CHECK(td!=NULL);CHECK(td->write(td->context,target,1U,a)==OPENFS_IO_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))!=0);CHECK(td->read(td->context,target,1U,readback)==OPENFS_IO_OK&&memcmp(readback,a,sizeof(a))==0);CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))==0);CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);td=openfs_transaction_device(&t);CHECK(td->write(td->context,target,1U,b)==OPENFS_IO_OK);CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))==0);CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);td=openfs_transaction_device(&t);while(j.next_record<j.journal_blocks)CHECK(openfs_journal_write(&j,&v,t.txid,"x",1U)==OPENFS_JOURNAL_OK);CHECK(td->write(td->context,target,1U,b)==OPENFS_IO_IO_ERROR);CHECK(td->write(td->context,target,1U,b)==OPENFS_IO_IO_ERROR);CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_IO_ERROR);CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))==0);CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);td=openfs_transaction_device(&t);CHECK(td->write(td->context,target,1U,b)==OPENFS_IO_OK);d.fail_start=s.journal_start+s.journal_blocks;d.fail_data=1;CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_IO_ERROR);CHECK(t.active==1&&t.failed==1&&t.committed==1);CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_CORRUPT);CHECK(t.active==0);d.fail_data=0;openfs_mount_t recovered;CHECK(openfs_mount(&recovered,&v)==OPENFS_MOUNT_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),b,sizeof(b))==0);CHECK(openfs_unmount(&recovered)==OPENFS_MOUNT_OK);CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);td=openfs_transaction_device(&t);d.arm_block=target;d.arm_flush_fail=1;CHECK(td->write(td->context,target,1U,a)==OPENFS_IO_OK);CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_IO_ERROR);d.arm_flush_fail=0;d.flush_failed=0;CHECK(openfs_mount(&recovered,&v)==OPENFS_MOUNT_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))==0);CHECK(openfs_unmount(&recovered)==OPENFS_MOUNT_OK);CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);td=openfs_transaction_device(&t);CHECK(td->write(td->context,target,1U,b)==OPENFS_IO_OK);d.fail_flush=1;CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_IO_ERROR);CHECK(t.active==1&&t.commit_started==1&&t.committed==1&&t.recovery_required==1);d.fail_flush=0;CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_CORRUPT);CHECK(t.active==0);CHECK(openfs_mount(&recovered,&v)==OPENFS_MOUNT_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),b,sizeof(b))==0);CHECK(openfs_unmount(&recovered)==OPENFS_MOUNT_OK);CHECK(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);CHECK(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);td=openfs_transaction_device(&t);CHECK(td->write(td->context,target,1U,a)==OPENFS_IO_OK);d.fail_exact=s.journal_start+j.next_record;d.fail_exact_enabled=1;d.fail_exact_once=1;CHECK(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_IO_ERROR);CHECK(t.active==1&&t.committed==0);d.fail_exact_enabled=0;CHECK(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_OK);CHECK(openfs_mount(&recovered,&v)==OPENFS_MOUNT_OK);CHECK(memcmp(d.b+(size_t)(target*d.bs),b,sizeof(b))==0);CHECK(openfs_unmount(&recovered)==OPENFS_MOUNT_OK);free(d.b);return 0;}