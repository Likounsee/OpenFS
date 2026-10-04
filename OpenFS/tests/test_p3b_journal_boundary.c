#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/format.h"
#include "openfs/journal.h"
#include "openfs/fsck.h"
#include "openfs/mount.h"
#include "openfs/crc32c.h"

typedef struct { uint8_t *b; uint32_t bs; uint64_t bc; } disk_t;
static openfs_journal_result_t replay_ok(void *ctx,uint64_t tx,const uint8_t *data,uint32_t len){(void)ctx;(void)tx;(void)data;(void)len;return OPENFS_JOURNAL_OK;}
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*o){disk_t*d=c;if(!d||!o||!n||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(o,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*i){disk_t*d=c;if(!d||!i||!n||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),i,(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}
static openfs_block_device_t dev(disk_t*d){openfs_block_device_t v={d,d->bs,d->bc,rd,wr,fl};return v;}
static void setup(disk_t*d,openfs_block_device_t*v,openfs_superblock_t*s){memset(d,0,sizeof(*d));d->bs=4096U;d->bc=256U;d->b=calloc((size_t)d->bs,(size_t)d->bc);assert(d->b);*v=dev(d);uint8_t u[16]={0xB1U};assert(openfs_format(v,u)==OPENFS_FORMAT_OK);assert(openfs_read_superblock(v,s)==OPENFS_FORMAT_OK);}
static void p32(uint8_t*p,uint32_t v){for(unsigned k=0;k<4;k++)p[k]=(uint8_t)(v>>(8U*k));}
static void p64(uint8_t*p,uint64_t v){for(unsigned k=0;k<8;k++)p[k]=(uint8_t)(v>>(8U*k));}
static void raw_record(openfs_block_device_t*v,const openfs_superblock_t*s,uint64_t slot,uint8_t type,uint64_t tx,uint64_t seq,const uint8_t*payload,uint32_t len){
    uint8_t b[4096U];memset(b,0,sizeof(b));memcpy(b,OPENFS_JOURNAL_MAGIC,5U);b[5]=type;p64(b+8U,tx);p64(b+16U,seq);p32(b+24U,len);if(len)memcpy(b+32U,payload,len);p32(b+28U,0U);p32(b+28U,openfs_crc32c(b,sizeof(b)));assert(v->write(v->context,s->journal_start+slot,1U,b)==OPENFS_IO_OK);
}
static uint32_t data_payload(uint8_t*p,uint64_t target,uint8_t value){memset(p,0,25U);memcpy(p,"OJBD1",5U);p64(p+8U,target);p32(p+16U,0U);p32(p+20U,1U);p[24U]=value;return 25U;}
static void assert_clean_after_mount(openfs_block_device_t*v,openfs_superblock_t*s,uint64_t target,uint8_t value){(void)s;
    openfs_mount_t m;assert(openfs_mount(&m,v)==OPENFS_MOUNT_OK);assert(m.journal.next_record==0U);assert(m.journal.active_transaction_id==0U);assert(m.journal.commit_record_written==0U);
    uint8_t b[4096U];assert(v->read(v->context,target,1U,b)==OPENFS_IO_OK);assert(b[0]==value);uint64_t errors=0;assert(openfs_fsck(v,&m.superblock,&errors)==OPENFS_FSCK_OK&&errors==0U);assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
    openfs_mount_t m2;assert(openfs_mount(&m2,v)==OPENFS_MOUNT_OK);assert(m2.journal.next_record==0U);errors=0;assert(openfs_fsck(v,&m2.superblock,&errors)==OPENFS_FSCK_OK&&errors==0U);assert(openfs_unmount(&m2)==OPENFS_MOUNT_OK);
}
static void empty_boundary(void){
    disk_t d;openfs_block_device_t v;openfs_superblock_t s;setup(&d,&v,&s);openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);assert(j.next_record==0U);assert(j.active_transaction_id==0U);assert(j.commit_record_written==0U);
    assert(openfs_journal_replay(&v,&s,replay_ok,NULL)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);assert(j.next_record==0U);openfs_journal_t r;assert(openfs_journal_open(&r,&v,&s)==OPENFS_JOURNAL_OK);assert(r.next_record==0U);free(d.b);
}
static void physical_boundaries(void){
    disk_t d;openfs_block_device_t v;openfs_superblock_t s;setup(&d,&v,&s);uint64_t target=s.data_start+1U;uint8_t payload[25];uint64_t jb=s.journal_blocks;assert(jb>=8U);
    /* next_record=1: one incomplete BEGIN, then replay/remount/fsck must remain safe. */
    raw_record(&v,&s,0U,OPENFS_JOURNAL_BEGIN,1U,1U,NULL,0U);openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);assert(j.next_record==1U);assert(j.commit_record_written==0U);assert(openfs_journal_replay(&v,&s,[](void*,uint64_t,const uint8_t*,uint32_t){return OPENFS_JOURNAL_OK;},NULL)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);assert(j.next_record==0U);
    /* Leave exactly one slot and use it as DATA: then use the same last slot as COMMIT in a fresh exact-capacity transaction. */
    setup(&d,&v,&s);jb=s.journal_blocks;target=s.data_start+1U;data_payload(payload,target,0xA5U);
    raw_record(&v,&s,0U,OPENFS_JOURNAL_BEGIN,1U,1U,NULL,0U);
    for(uint64_t slot=1U;slot<jb-1U;slot++)raw_record(&v,&s,slot,OPENFS_JOURNAL_DATA,1U,slot+1U,payload,sizeof(payload));
    raw_record(&v,&s,jb-1U,OPENFS_JOURNAL_DATA,1U,jb,payload,sizeof(payload));
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);assert(j.next_record==jb);assert(j.commit_record_written==0U);
    assert(openfs_journal_replay(&v,&s,[](void*,uint64_t,const uint8_t*,uint32_t){return OPENFS_JOURNAL_OK;},NULL)==OPENFS_JOURNAL_OK);assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);assert(j.next_record==0U);
    /* Exact-full committed journal: BEGIN + (jb-2) DATA + COMMIT. */
    setup(&d,&v,&s);jb=s.journal_blocks;target=s.data_start+1U;data_payload(payload,target,0x5CU);
    raw_record(&v,&s,0U,OPENFS_JOURNAL_BEGIN,1U,1U,NULL,0U);
    for(uint64_t slot=1U;slot<jb-1U;slot++)raw_record(&v,&s,slot,OPENFS_JOURNAL_DATA,1U,slot+1U,payload,sizeof(payload));
    raw_record(&v,&s,jb-1U,OPENFS_JOURNAL_COMMIT,1U,jb,NULL,0U);
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);assert(j.next_record==jb);assert(j.active_transaction_id==0U);assert(j.commit_record_written==1U);assert(j.transaction_id==1U);assert(j.sequence==jb);
    uint64_t dummy=0U;assert(openfs_journal_begin(&j,&v,&dummy)==OPENFS_JOURNAL_INVALID_ARGUMENT);
    assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);assert(j.next_record==0U);assert(j.commit_record_written==0U);
    assert_clean_after_mount(&v,&s,target,0x5CU);
    free(d.b);
}
static void exact_api_full_and_last_commit(void){
    disk_t d;openfs_block_device_t v;openfs_superblock_t s;setup(&d,&v,&s);openfs_journal_t j;uint64_t tx=0;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);assert(j.next_record==1U);
    while(j.next_record+1U<j.journal_blocks)assert(openfs_journal_write(&j,&v,tx,"x",1U)==OPENFS_JOURNAL_OK);
    assert(j.next_record==j.journal_blocks-1U);assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);assert(j.next_record==j.journal_blocks);assert(j.commit_record_written==1U);assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_INVALID_ARGUMENT);assert(openfs_journal_write(&j,&v,tx,"x",1U)==OPENFS_JOURNAL_INVALID_ARGUMENT);assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);assert(j.next_record==0U);free(d.b);
}
int main(void){empty_boundary();physical_boundaries();exact_api_full_and_last_commit();return 0;}
