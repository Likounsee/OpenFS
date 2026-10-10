#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/journal.h"
#include "openfs/crc32c.h"
typedef struct{uint8_t*b;uint32_t bs;uint64_t bc;uint64_t read_count;uint64_t fail_read_at;uint64_t mutate_read_at;uint64_t mutate_block;int mutate_payload;}D;
static openfs_io_result_t r(void*c,uint64_t f,uint32_t n,void*x){
    D*d=c;d->read_count++;
    if(d->fail_read_at!=0U&&d->read_count==d->fail_read_at)return OPENFS_IO_IO_ERROR;
    if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;
    memcpy(x,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));
    if(d->mutate_payload&&d->read_count==d->mutate_read_at&&f==d->mutate_block&&n==1U&&d->bs>=33U){
        uint8_t*b=(uint8_t*)x;
        b[32U]^=0x01U;
        b[28U]=b[29U]=b[30U]=b[31U]=0U;
        uint32_t crc=openfs_crc32c(b,d->bs);
        b[28U]=(uint8_t)crc;b[29U]=(uint8_t)(crc>>8U);b[30U]=(uint8_t)(crc>>16U);b[31U]=(uint8_t)(crc>>24U);
    }
    return OPENFS_IO_OK;
}
static openfs_io_result_t w(void*c,uint64_t f,uint32_t n,const void*x){D*d=c;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),x,(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t f(void*c){(void)c;return OPENFS_IO_OK;}
static openfs_journal_result_t cb(void*c,uint64_t tx,const uint8_t*p,uint32_t n){uint32_t *hits=c;if(hits==NULL)return OPENFS_JOURNAL_OK;assert(tx==1U&&n==3U&&memcmp(p,"abc",3U)==0);(*hits)++;return OPENFS_JOURNAL_OK;}
static void write_raw(uint8_t *b,uint32_t type,uint64_t tx,uint64_t seq,uint32_t len,int valid_crc){
    memset(b,0,4096U);memcpy(b,OPENFS_JOURNAL_MAGIC,5U);b[5]=(uint8_t)type;
    for(unsigned k=0;k<8U;k++){b[8U+k]=(uint8_t)(tx>>(8U*k));b[16U+k]=(uint8_t)(seq>>(8U*k));}
    b[24U]=(uint8_t)len;b[25U]=(uint8_t)(len>>8U);b[26U]=(uint8_t)(len>>16U);b[27U]=(uint8_t)(len>>24U);
    uint32_t crc=openfs_crc32c(b,4096U);if(!valid_crc)crc^=0xA5A5A5A5U;
    b[28U]=(uint8_t)crc;b[29U]=(uint8_t)(crc>>8U);b[30U]=(uint8_t)(crc>>16U);b[31U]=(uint8_t)(crc>>24U);
}
static void journal_corruption_matrix(void){
    D d={0};d.bs=4096U;v.block_count=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={21U};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    uint8_t raw[4096U];
    write_raw(raw,OPENFS_JOURNAL_BEGIN,1U,1U,4096U,1);assert(v.write(v.context,s.journal_start,1U,raw)==OPENFS_IO_OK);assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_CORRUPT);
    memset(d.b+(size_t)(s.journal_start*d.bs),0,d.bs);write_raw(raw,OPENFS_JOURNAL_BEGIN,1U,1U,0U,0);assert(v.write(v.context,s.journal_start,1U,raw)==OPENFS_IO_OK);assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_CORRUPT);
    memset(d.b+(size_t)(s.journal_start*d.bs),0,d.bs);write_raw(raw,OPENFS_JOURNAL_BEGIN,1U,1U,0U,1);assert(v.write(v.context,s.journal_start,1U,raw)==OPENFS_IO_OK);write_raw(raw,OPENFS_JOURNAL_DATA,1U,2U,1U,1);raw[32U]=0x5AU;assert(v.write(v.context,s.journal_start+1U,1U,raw)==OPENFS_IO_OK);assert(openfs_journal_replay(&v,&s,cb,NULL)==OPENFS_JOURNAL_CORRUPT);
    memset(d.b+(size_t)(s.journal_start*d.bs),0,2U*d.bs);write_raw(raw,OPENFS_JOURNAL_BEGIN,UINT64_MAX,1U,0U,1);assert(v.write(v.context,s.journal_start,1U,raw)==OPENFS_IO_OK);write_raw(raw,OPENFS_JOURNAL_COMMIT,UINT64_MAX,2U,0U,1);assert(v.write(v.context,s.journal_start+1U,1U,raw)==OPENFS_IO_OK);assert(openfs_journal_replay(&v,&s,cb,NULL)==OPENFS_JOURNAL_OK);
    memset(d.b+(size_t)(s.journal_start*d.bs),0,2U*d.bs);write_raw(raw,OPENFS_JOURNAL_BEGIN,7U,1U,0U,1);assert(v.write(v.context,s.journal_start,1U,raw)==OPENFS_IO_OK);write_raw(raw,OPENFS_JOURNAL_COMMIT,8U,2U,0U,1);assert(v.write(v.context,s.journal_start+1U,1U,raw)==OPENFS_IO_OK);assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_CORRUPT);assert(openfs_journal_replay(&v,&s,cb,NULL)==OPENFS_JOURNAL_CORRUPT);
    memset(d.b+(size_t)(s.journal_start*d.bs),0,3U*d.bs);write_raw(raw,OPENFS_JOURNAL_BEGIN,9U,1U,0U,1);assert(v.write(v.context,s.journal_start,1U,raw)==OPENFS_IO_OK);write_raw(raw,OPENFS_JOURNAL_DATA,9U,2U,1U,1);raw[32U]=1U;assert(v.write(v.context,s.journal_start+1U,1U,raw)==OPENFS_IO_OK);write_raw(raw,OPENFS_JOURNAL_BEGIN,10U,3U,0U,1);assert(v.write(v.context,s.journal_start+2U,1U,raw)==OPENFS_IO_OK);assert(openfs_journal_replay(&v,&s,cb,NULL)==OPENFS_JOURNAL_CORRUPT);
    memset(d.b+(size_t)(s.journal_start*d.bs),0,3U*d.bs);
    uint8_t full_payload[4064U];
    memset(full_payload,0x5AU,sizeof(full_payload));
    memset(raw,0,sizeof(raw));
    write_raw(raw,OPENFS_JOURNAL_BEGIN,11U,1U,0U,1);
    assert(v.write(v.context,s.journal_start,1U,raw)==OPENFS_IO_OK);
    write_raw(raw,OPENFS_JOURNAL_DATA,11U,2U,4064U,1);
    memcpy(raw+32U,full_payload,sizeof(full_payload));
    raw[28U]=raw[29U]=raw[30U]=raw[31U]=0U;
    uint32_t full_crc=openfs_crc32c(raw,4096U);
    raw[28U]=(uint8_t)full_crc;raw[29U]=(uint8_t)(full_crc>>8U);raw[30U]=(uint8_t)(full_crc>>16U);raw[31U]=(uint8_t)(full_crc>>24U);
    assert(v.write(v.context,s.journal_start+1U,1U,raw)==OPENFS_IO_OK);
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    assert(v.read(v.context,s.journal_start+1U,1U,raw)==OPENFS_IO_OK);
    raw[4095U]^=0x01U;
    assert(v.write(v.context,s.journal_start+1U,1U,raw)==OPENFS_IO_OK);
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_CORRUPT);
    /* A committed transaction cannot accept more DATA records. */
    memset(d.b+(size_t)(s.journal_start*d.bs),0,3U*d.bs);
    write_raw(raw,OPENFS_JOURNAL_BEGIN,12U,1U,0U,1);
    assert(v.write(v.context,s.journal_start,1U,raw)==OPENFS_IO_OK);
    write_raw(raw,OPENFS_JOURNAL_COMMIT,12U,2U,0U,1);
    assert(v.write(v.context,s.journal_start+1U,1U,raw)==OPENFS_IO_OK);
    write_raw(raw,OPENFS_JOURNAL_DATA,12U,3U,0U,1);
    assert(v.write(v.context,s.journal_start+2U,1U,raw)==OPENFS_IO_OK);
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_CORRUPT);
    assert(openfs_journal_replay(&v,&s,cb,NULL)==OPENFS_JOURNAL_CORRUPT);

    /* BEGIN transaction IDs must increase strictly, even after a crash. */
    memset(d.b+(size_t)(s.journal_start*d.bs),0,2U*d.bs);
    write_raw(raw,OPENFS_JOURNAL_BEGIN,12U,1U,0U,1);
    assert(v.write(v.context,s.journal_start,1U,raw)==OPENFS_IO_OK);
    write_raw(raw,OPENFS_JOURNAL_BEGIN,11U,2U,0U,1);
    assert(v.write(v.context,s.journal_start+1U,1U,raw)==OPENFS_IO_OK);
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_CORRUPT);
    assert(openfs_journal_replay(&v,&s,cb,NULL)==OPENFS_JOURNAL_CORRUPT);

    free(d.b);
}

static void replay_validates_entire_wal_before_callbacks(void){
    D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);
    openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={33U};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);
    openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    uint64_t tx=0U;assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_write(&j,&v,tx,"abc",3U)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
    uint8_t garbage[4096U]={0};garbage[17U]=0xA5U;
    assert(v.write(v.context,s.journal_start+3U,1U,garbage)==OPENFS_IO_OK);
    uint32_t hits=0U;
    assert(openfs_journal_replay(&v,&s,cb,&hits)==OPENFS_JOURNAL_CORRUPT);
    assert(hits==0U);
    free(d.b);
}


static void replay_rejects_undersized_blocks_before_crc(void){
    D d={0};d.bs=16U;d.bc=2U;
    d.b=calloc((size_t)d.bs,(size_t)d.bc);assert(d.b);
    memcpy(d.b+(size_t)d.bs,OPENFS_JOURNAL_MAGIC,5U);
    openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};
    openfs_superblock_t s={0};
    s.block_size=d.bs;s.journal_start=1U;s.journal_blocks=1U;
    uint32_t hits=0U;
    assert(openfs_journal_replay(&v,&s,cb,&hits)==OPENFS_JOURNAL_INVALID_ARGUMENT);
    assert(hits==0U&&d.read_count==0U);
    free(d.b);
}

static void recover_invalid_range_does_not_stick_gate(void){
    D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);
    openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};
    uint8_t uuid[16]={38U};assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    uint32_t hits=0U;
    v.block_count=s.journal_start+s.journal_blocks-1U;
    assert(openfs_journal_recover(&j,&v,&s,cb,&hits)==OPENFS_JOURNAL_INVALID_ARGUMENT);
    assert(j.recovery_required==0U&&hits==0U);
    d.bc=256U;
    uint64_t tx=0U;
    assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);
    free(d.b);
}
static void replay_read_failure_does_not_partially_apply(void){
    D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);
    openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={34U};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);
    openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    uint64_t tx=0U;assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK&&tx==1U);
    assert(openfs_journal_write(&j,&v,tx,"abc",3U)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
    uint32_t hits=0U;
    d.read_count=0U;
    /* Fail in the second scan after the DATA callback would previously have run. */
    d.fail_read_at=s.journal_blocks+4U;
    assert(openfs_journal_replay(&v,&s,cb,&hits)==OPENFS_JOURNAL_IO_ERROR);
    assert(hits==0U);
    d.fail_read_at=0U;d.read_count=0U;
    assert(openfs_journal_replay(&v,&s,cb,&hits)==OPENFS_JOURNAL_OK&&hits==1U);
    free(d.b);
}

typedef struct{uint32_t hits;uint8_t payload[4U];}replay_capture_t;
static openfs_journal_result_t capture_replay_payload(void*c,uint64_t tx,const uint8_t*p,uint32_t n){
    replay_capture_t*state=(replay_capture_t*)c;
    if(state==NULL||tx!=1U||n!=3U)return OPENFS_JOURNAL_CORRUPT;
    state->hits++;memcpy(state->payload,p,n);state->payload[n]=0U;return OPENFS_JOURNAL_OK;
}
static void replay_uses_the_payload_that_was_validated(void){
    D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);
    openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={36U};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);
    openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    uint64_t tx=0U;assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK&&tx==1U);
    assert(openfs_journal_write(&j,&v,tx,"abc",3U)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
    replay_capture_t state={0};
    d.read_count=0U;
    /*
     * The second read of the DATA slot returns a changed payload with a
     * recomputed valid record CRC. The first-pass fingerprint must catch it
     * before any callback; retrying without the transient read mutation works.
     */
    d.mutate_payload=1;
    d.mutate_block=s.journal_start+1U;
    d.mutate_read_at=s.journal_blocks+2U;
    assert(openfs_journal_replay(&v,&s,capture_replay_payload,&state)==OPENFS_JOURNAL_CORRUPT);
    assert(state.hits==0U);
    d.mutate_payload=0;
    d.read_count=0U;
    assert(openfs_journal_replay(&v,&s,capture_replay_payload,&state)==OPENFS_JOURNAL_OK);
    assert(state.hits==1U);
    assert(memcmp(state.payload,"abc",3U)==0);
    free(d.b);
}


typedef struct{int fail;uint32_t hits;} replay_failure_state_t;
static openfs_journal_result_t replay_fail_once(void*c,uint64_t tx,const uint8_t*p,uint32_t n){
    replay_failure_state_t*state=(replay_failure_state_t*)c;
    if(state==NULL||tx!=1U||n!=3U||memcmp(p,"abc",3U)!=0)return OPENFS_JOURNAL_CORRUPT;
    if(state->fail)return OPENFS_JOURNAL_IO_ERROR;
    state->hits++;
    return OPENFS_JOURNAL_OK;
}
static void replay_callback_failure_keeps_recovery_gate(void){
    D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);
    openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={35U};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);
    openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    uint64_t tx=0U;assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_write(&j,&v,tx,"abc",3U)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
    replay_failure_state_t state={1,0U};
    assert(openfs_journal_recover(&j,&v,&s,replay_fail_once,&state)==OPENFS_JOURNAL_IO_ERROR);
    assert(state.hits==0U&&j.recovery_required==1U);
    uint64_t blocked=0U;assert(openfs_journal_begin(&j,&v,&blocked)==OPENFS_JOURNAL_IO_ERROR);
    assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_IO_ERROR);
    state.fail=0;
    assert(openfs_journal_recover(&j,&v,&s,replay_fail_once,&state)==OPENFS_JOURNAL_OK);
    assert(state.hits==1U&&j.recovery_required==0U);
    assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);
    free(d.b);
}

static openfs_journal_result_t replay_tx2(void*c,uint64_t tx,const uint8_t*p,uint32_t n){uint32_t *hits=c;if(tx!=2U||n!=3U||memcmp(p,"tx2",3U)!=0)return OPENFS_JOURNAL_CORRUPT;(*hits)++;return OPENFS_JOURNAL_OK;}
static void replay_abandoned_transaction(void){
    D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);
    openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={22U};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);
    openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    uint64_t tx=0U;assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK&&tx==1U);
    assert(openfs_journal_write(&j,&v,tx,"tx1",3U)==OPENFS_JOURNAL_OK);
    uint64_t slot=j.next_record;uint8_t raw[4096U];
    write_raw(raw,OPENFS_JOURNAL_BEGIN,2U,3U,0U,1);assert(v.write(v.context,s.journal_start+slot,1U,raw)==OPENFS_IO_OK);
    write_raw(raw,OPENFS_JOURNAL_DATA,2U,4U,3U,1);memcpy(raw+32U,"tx2",3U);raw[28]=raw[29]=raw[30]=raw[31]=0U;uint32_t crc=openfs_crc32c(raw,4096U);raw[28]=(uint8_t)crc;raw[29]=(uint8_t)(crc>>8U);raw[30]=(uint8_t)(crc>>16U);raw[31]=(uint8_t)(crc>>24U);assert(v.write(v.context,s.journal_start+slot+1U,1U,raw)==OPENFS_IO_OK);
    write_raw(raw,OPENFS_JOURNAL_COMMIT,2U,5U,0U,1);assert(v.write(v.context,s.journal_start+slot+2U,1U,raw)==OPENFS_IO_OK);
    uint32_t hits=0U;assert(openfs_journal_replay(&v,&s,replay_tx2,&hits)==OPENFS_JOURNAL_OK&&hits==1U);
    free(d.b);
}

int main(void){
    replay_rejects_undersized_blocks_before_crc();recover_invalid_range_does_not_stick_gate();replay_read_failure_does_not_partially_apply();replay_uses_the_payload_that_was_validated();replay_callback_failure_keeps_recovery_gate();replay_validates_entire_wal_before_callbacks();replay_abandoned_transaction();journal_corruption_matrix();D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={0};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);uint64_t tx=0;j.transaction_id=UINT64_MAX;assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_FULL);j.transaction_id=0U;j.sequence=UINT64_MAX;assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_FULL);j.sequence=0U;assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK&&tx==1U);j.sequence=UINT64_MAX;assert(openfs_journal_write(&j,&v,tx,"abc",3U)==OPENFS_JOURNAL_FULL);assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_FULL);j.sequence=1U;assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_INVALID_ARGUMENT);assert(openfs_journal_write(&j,&v,2U,"abc",3U)==OPENFS_JOURNAL_INVALID_ARGUMENT);assert(openfs_journal_write(&j,&v,tx,"abc",3U)==OPENFS_JOURNAL_OK);uint32_t hits=0;assert(openfs_journal_replay(&v,&s,cb,&hits)==OPENFS_JOURNAL_OK&&hits==0U);assert(openfs_journal_commit(&j,&v,2U)==OPENFS_JOURNAL_INVALID_ARGUMENT);assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_replay(&v,&s,cb,&hits)==OPENFS_JOURNAL_OK&&hits==1U);uint8_t seq_gap[4096];assert(v.read(v.context,s.journal_start+1U,1U,seq_gap)==OPENFS_IO_OK);seq_gap[16]=4U;seq_gap[17]=0U;seq_gap[18]=0U;seq_gap[19]=0U;uint32_t seq_crc=openfs_crc32c(seq_gap,4092U);seq_gap[28]=(uint8_t)seq_crc;seq_gap[29]=(uint8_t)(seq_crc>>8U);seq_gap[30]=(uint8_t)(seq_crc>>16U);seq_gap[31]=(uint8_t)(seq_crc>>24U);assert(v.write(v.context,s.journal_start+1U,1U,seq_gap)==OPENFS_IO_OK);assert(openfs_journal_replay(&v,&s,cb,&hits)==OPENFS_JOURNAL_CORRUPT);assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);uint8_t garbage[4096]={0};garbage[17]=0xA5U;assert(v.write(v.context,s.journal_start+1U,1U,garbage)==OPENFS_IO_OK);assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_CORRUPT);assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);uint8_t leading_gap[4096]={0};memcpy(leading_gap,"OJNL1",5U);leading_gap[5]=OPENFS_JOURNAL_BEGIN;leading_gap[8]=7U;leading_gap[16]=1U;uint32_t leading_crc=openfs_crc32c(leading_gap,4092U);leading_gap[28]=(uint8_t)leading_crc;leading_gap[29]=(uint8_t)(leading_crc>>8U);leading_gap[30]=(uint8_t)(leading_crc>>16U);leading_gap[31]=(uint8_t)(leading_crc>>24U);assert(v.write(v.context,s.journal_start+1U,1U,leading_gap)==OPENFS_IO_OK);assert(openfs_journal_replay(&v,&s,cb,&hits)==OPENFS_JOURNAL_CORRUPT);assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);uint8_t orphan_data[4096]={0};memcpy(orphan_data,"OJNL1",5U);orphan_data[5]=OPENFS_JOURNAL_DATA;orphan_data[8]=99U;orphan_data[16]=1U;orphan_data[24]=1U;orphan_data[32]=0x5AU;uint32_t orphan_crc=openfs_crc32c(orphan_data,4092U);orphan_data[28]=(uint8_t)orphan_crc;orphan_data[29]=(uint8_t)(orphan_crc>>8U);orphan_data[30]=(uint8_t)(orphan_crc>>16U);orphan_data[31]=(uint8_t)(orphan_crc>>24U);assert(v.write(v.context,s.journal_start,1U,orphan_data)==OPENFS_IO_OK);assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_CORRUPT);assert(openfs_journal_replay(&v,&s,cb,&hits)==OPENFS_JOURNAL_CORRUPT);assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);uint8_t bad[4096]={0};memcpy(bad,"OJNL1",5U);bad[5]=OPENFS_JOURNAL_COMMIT;bad[8]=99U;uint32_t crc=openfs_crc32c(bad,4092U);bad[28]=(uint8_t)crc;bad[29]=(uint8_t)(crc>>8U);bad[30]=(uint8_t)(crc>>16U);bad[31]=(uint8_t)(crc>>24U);assert(v.write(v.context,s.journal_start,1U,bad)==OPENFS_IO_OK);assert(openfs_journal_replay(&v,&s,cb,&hits)==OPENFS_JOURNAL_CORRUPT);assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK&&tx==2U);while(j.next_record+1U<j.journal_blocks)assert(openfs_journal_write(&j,&v,tx,"x",1U)==OPENFS_JOURNAL_OK);uint8_t block[4096]={0};uint64_t before=j.next_record;assert(openfs_journal_write_block(&j,&v,tx,s.journal_start,block)==OPENFS_JOURNAL_FULL);assert(j.next_record==before);assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);j.sequence=UINT64_MAX;assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK&&j.sequence==0U);assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);free(d.b);return 0;}