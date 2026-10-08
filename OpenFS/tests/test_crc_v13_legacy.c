#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/format.h"
#include "openfs/journal.h"
#include "openfs/mount.h"
#include "openfs/crc32c.h"

typedef struct { uint8_t *b; uint32_t bs; uint64_t bc; } disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*o){disk_t*d=c;if(!d||!o||!n||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(o,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*i){disk_t*d=c;if(!d||!i||!n||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),i,(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}
static openfs_block_device_t dev(disk_t*d){openfs_block_device_t v={d,d->bs,d->bc,rd,wr,fl};return v;}
static void p32(uint8_t*p,uint32_t v){for(unsigned k=0;k<4;k++)p[k]=(uint8_t)(v>>(8U*k));}
static void p64(uint8_t*p,uint64_t v){for(unsigned k=0;k<8;k++)p[k]=(uint8_t)(v>>(8U*k));}

static void legacy_record(openfs_block_device_t*v,const openfs_superblock_t*s,uint64_t slot,uint8_t type,uint64_t tx,uint64_t seq,const uint8_t*payload,uint32_t len){
    uint8_t b[4096U];assert(s->block_size==sizeof(b));memset(b,0,sizeof(b));memcpy(b,OPENFS_JOURNAL_MAGIC,5U);b[5]=type;p64(b+8U,tx);p64(b+16U,seq);p32(b+24U,len);if(len)memcpy(b+32U,payload,len);p32(b+28U,openfs_crc32c(b,sizeof(b)-4U));assert(v->write(v->context,s->journal_start+slot,1U,b)==OPENFS_IO_OK);
}
static openfs_journal_result_t replay_probe(void*c,uint64_t tx,const uint8_t*p,uint32_t n){uint32_t*hits=c;assert(tx==1U&&n==28U&&memcmp(p,"OJBD1",5U)==0);assert(hits);(*hits)++;return OPENFS_JOURNAL_OK;}

int main(void){
    disk_t d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,(size_t)d.bc);assert(d.b);
    openfs_block_device_t v=dev(&d);uint8_t uuid[16]={0xC1U};openfs_superblock_t s;
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    uint64_t target=s.data_start;uint8_t payload[28U]={0};memcpy(payload,"OJBD1",5U);p64(payload+8U,target);p32(payload+16U,0U);p32(payload+20U,4U);memcpy(payload+24U,"V13!",4U);
    legacy_record(&v,&s,0U,OPENFS_JOURNAL_BEGIN,1U,1U,NULL,0U);
    legacy_record(&v,&s,1U,OPENFS_JOURNAL_DATA,1U,2U,payload,sizeof(payload));
    legacy_record(&v,&s,2U,OPENFS_JOURNAL_COMMIT,1U,3U,NULL,0U);

    uint8_t raw[4096U];
    openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);assert(j.next_record==3U&&j.transaction_id==1U);
    uint32_t hits=0U;assert(openfs_journal_replay(&v,&s,replay_probe,&hits)==OPENFS_JOURNAL_OK&&hits==1U);

    openfs_mount_t m;assert(openfs_mount(&m,&v)==OPENFS_MOUNT_OK);assert(memcmp(d.b+(size_t)(target*d.bs)+0U,"V13!",4U)==0);assert(m.journal.next_record==0U);assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
    openfs_mount_t remount;assert(openfs_mount(&remount,&v)==OPENFS_MOUNT_OK);assert(openfs_unmount(&remount)==OPENFS_MOUNT_OK);

    /* A newly written v1.3 record uses the full-record CRC and remains readable. */
    uint64_t tx=0U;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_write(&j,&v,tx,"NEW",3U)==OPENFS_JOURNAL_OK);assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);

    /* Legacy CRC corruption in payload is rejected. */
    legacy_record(&v,&s,0U,OPENFS_JOURNAL_BEGIN,2U,4U,NULL,0U);
    legacy_record(&v,&s,1U,OPENFS_JOURNAL_DATA,2U,5U,payload,sizeof(payload));
    legacy_record(&v,&s,2U,OPENFS_JOURNAL_COMMIT,2U,6U,NULL,0U);
    assert(v.read(v.context,s.journal_start+1U,1U,raw)==OPENFS_IO_OK);raw[32U+27U]^=0x01U;assert(v.write(v.context,s.journal_start+1U,1U,raw)==OPENFS_IO_OK);
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_CORRUPT);

    /* The legacy span does not cover the final four reserved bytes; the reader rejects them explicitly. */
    legacy_record(&v,&s,0U,OPENFS_JOURNAL_BEGIN,3U,7U,NULL,0U);
    assert(v.read(v.context,s.journal_start,1U,raw)==OPENFS_IO_OK);raw[4095U]=0xA5U;assert(v.write(v.context,s.journal_start,1U,raw)==OPENFS_IO_OK);
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_CORRUPT);

    free(d.b);return 0;
}
