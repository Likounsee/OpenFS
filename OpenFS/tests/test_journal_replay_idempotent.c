#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "openfs/format.h"
#include "openfs/journal.h"

#define BS 4096U
#define BC 256U

typedef struct { uint8_t *bytes; uint32_t bs; uint64_t bc; uint64_t target; } disk_t;
static openfs_io_result_t rd(void *ctx,uint64_t first,uint32_t count,void*out){disk_t*d=(disk_t*)ctx;if(count==0U||first>=d->bc||(uint64_t)count>d->bc-first)return OPENFS_IO_OUT_OF_RANGE;memcpy(out,d->bytes+(size_t)(first*d->bs),(size_t)count*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void *ctx,uint64_t first,uint32_t count,const void*in){disk_t*d=(disk_t*)ctx;if(count==0U||first>=d->bc||(uint64_t)count>d->bc-first)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->bytes+(size_t)(first*d->bs),in,(size_t)count*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void *ctx){(void)ctx;return OPENFS_IO_OK;}
static openfs_journal_result_t apply(void *ctx,uint64_t tx,const uint8_t*data,uint32_t len){disk_t*d=(disk_t*)ctx;(void)tx;if(d==NULL||data==NULL||len!=1024U||d->target>=d->bc)return OPENFS_JOURNAL_CORRUPT;memcpy(d->bytes+(size_t)(d->target*d->bs),data,len);return OPENFS_JOURNAL_OK;}
typedef struct { disk_t *disk; unsigned calls; unsigned fail_after_apply_once; } retry_ctx_t;
static openfs_journal_result_t apply_then_fail_once(void *ctx,uint64_t tx,const uint8_t*data,uint32_t len){
    retry_ctx_t *r=(retry_ctx_t*)ctx;
    if(r==NULL||r->disk==NULL)return OPENFS_JOURNAL_INVALID_ARGUMENT;
    r->calls++;
    openfs_journal_result_t applied=apply(r->disk,tx,data,len);
    if(applied!=OPENFS_JOURNAL_OK)return applied;
    if(r->fail_after_apply_once){r->fail_after_apply_once=0U;return OPENFS_JOURNAL_IO_ERROR;}
    return OPENFS_JOURNAL_OK;
}
static void test_replay_retry_after_callback_failure(void){
    disk_t d={0};d.bs=BS;d.bc=BC;d.bytes=(uint8_t*)calloc((size_t)d.bc,d.bs);assert(d.bytes);
    openfs_block_device_t v={&d,d.bs,d.bc,rd,wr,fl};uint8_t uuid[16]={0x52U,0x52U,0x54U};
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    uint64_t target=s.data_start+15U;d.target=target;uint64_t tx=0U;
    assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);
    uint8_t payload[1024U];memset(payload,0x6BU,sizeof(payload));
    assert(openfs_journal_write(&j,&v,tx,payload,sizeof(payload))==OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
    retry_ctx_t ctx={&d,0U,1U};
    assert(openfs_journal_replay(&v,&s,apply_then_fail_once,&ctx)==OPENFS_JOURNAL_IO_ERROR);
    assert(ctx.calls==1U);
    assert(memcmp(d.bytes+(size_t)(target*d.bs),payload,sizeof(payload))==0);
    assert(openfs_journal_replay(&v,&s,apply_then_fail_once,&ctx)==OPENFS_JOURNAL_OK);
    assert(ctx.calls==2U);
    assert(memcmp(d.bytes+(size_t)(target*d.bs),payload,sizeof(payload))==0);
    free(d.bytes);
}

typedef struct { unsigned calls; unsigned fail_on_second_once; uint8_t effects[2][1024U]; unsigned applied[2]; } multi_retry_ctx_t;
static openfs_journal_result_t apply_two_then_fail(void *ctx,uint64_t tx,const uint8_t*data,uint32_t len){
    multi_retry_ctx_t *r=(multi_retry_ctx_t*)ctx;
    if(r==NULL||data==NULL||tx!=1U||len!=1024U)return OPENFS_JOURNAL_CORRUPT;
    unsigned index=data[0]==0xA1U?0U:(data[0]==0xB2U?1U:2U);
    if(index>1U)return OPENFS_JOURNAL_CORRUPT;
    r->calls++;
    memcpy(r->effects[index],data,len);
    r->applied[index]++;
    if(r->fail_on_second_once&&r->calls==2U){r->fail_on_second_once=0U;return OPENFS_JOURNAL_IO_ERROR;}
    return OPENFS_JOURNAL_OK;
}
static void test_replay_retry_after_partial_multi_record_publication(void){
    disk_t d={0};d.bs=BS;d.bc=BC;d.bytes=(uint8_t*)calloc((size_t)d.bc,d.bs);assert(d.bytes);
    openfs_block_device_t v={&d,d.bs,d.bc,rd,wr,fl};uint8_t uuid[16]={0x52U,0x52U,0x54U,0x02U};
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    uint64_t tx=0U;assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);
    uint8_t first[1024U],second[1024U];memset(first,0xA1U,sizeof(first));memset(second,0xB2U,sizeof(second));
    assert(openfs_journal_write(&j,&v,tx,first,sizeof(first))==OPENFS_JOURNAL_OK);
    assert(openfs_journal_write(&j,&v,tx,second,sizeof(second))==OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
    multi_retry_ctx_t ctx={0};ctx.fail_on_second_once=1U;
    assert(openfs_journal_replay(&v,&s,apply_two_then_fail,&ctx)==OPENFS_JOURNAL_IO_ERROR);
    assert(ctx.calls==2U&&ctx.applied[0]==1U&&ctx.applied[1]==1U);
    assert(ctx.effects[0][0]==0xA1U&&ctx.effects[1][0]==0xB2U);
    assert(openfs_journal_replay(&v,&s,apply_two_then_fail,&ctx)==OPENFS_JOURNAL_OK);
    assert(ctx.calls==4U&&ctx.applied[0]==2U&&ctx.applied[1]==2U);
    assert(ctx.effects[0][0]==0xA1U&&ctx.effects[1][0]==0xB2U);
    free(d.bytes);
}
int main(void){test_replay_retry_after_partial_multi_record_publication();test_replay_retry_after_callback_failure();disk_t d={0};d.bs=BS;d.bc=BC;d.bytes=(uint8_t*)calloc((size_t)d.bc,d.bs);assert(d.bytes);openfs_block_device_t v={&d,d.bs,d.bc,rd,wr,fl};uint8_t uuid[16]={0x52U,0x50U,0x4CU};assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);uint64_t tx=0U,target=s.data_start+12U;d.target=target;assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);uint8_t block[1024U];memset(block,0xD3U,sizeof(block));assert(openfs_journal_write(&j,&v,tx,block,sizeof(block))==OPENFS_JOURNAL_OK);assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_replay(&v,&s,apply,&d)==OPENFS_JOURNAL_OK);assert(memcmp(d.bytes+(size_t)(target*d.bs),block,sizeof(block))==0);uint8_t snapshot[1024U];memcpy(snapshot,d.bytes+(size_t)(target*d.bs),sizeof(snapshot));assert(openfs_journal_replay(&v,&s,apply,&d)==OPENFS_JOURNAL_OK);assert(memcmp(d.bytes+(size_t)(target*d.bs),snapshot,sizeof(snapshot))==0);free(d.bytes);return 0;}
