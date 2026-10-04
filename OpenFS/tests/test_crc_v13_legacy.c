#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/format.h"
#include "openfs/journal.h"
#include "openfs/crc32c.h"

typedef struct { uint8_t *b; uint32_t bs; uint64_t bc; } disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*o){disk_t*d=c;if(!d||!o||!n||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(o,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*i){disk_t*d=c;if(!d||!i||!n||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),i,(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}
static openfs_block_device_t dev(disk_t*d){openfs_block_device_t v={d,d->bs,d->bc,rd,wr,fl};return v;}
static void p32(uint8_t*p,uint32_t v){for(unsigned k=0;k<4;k++)p[k]=(uint8_t)(v>>(8U*k));}
static void p64(uint8_t*p,uint64_t v){for(unsigned k=0;k<8;k++)p[k]=(uint8_t)(v>>(8U*k));}

/*
 * Historical writer reconstruction from commit a409's parent (0352...):
 * p32(b+28, openfs_crc32c(b, b->block_size - 4U)).
 * Both the writer and reader in that v1.3 implementation used this span.
 */
static void write_legacy_v13_record(openfs_block_device_t*v,const openfs_superblock_t*s){
    uint8_t b[4096U];memset(b,0,sizeof(b));memcpy(b,OPENFS_JOURNAL_MAGIC,5U);b[5]=OPENFS_JOURNAL_BEGIN;p64(b+8U,1U);p64(b+16U,1U);p32(b+24U,0U);p32(b+28U,openfs_crc32c(b,sizeof(b)-4U));assert(v->write(v->context,s->journal_start,1U,b)==OPENFS_IO_OK);
}
int main(void){
    disk_t d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,(size_t)d.bc);assert(d.b);
    openfs_block_device_t v=dev(&d);uint8_t uuid[16]={0xC1U};openfs_superblock_t s;
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    write_legacy_v13_record(&v,&s);
    openfs_journal_t j;
    /* Exact v1.3 legacy record is rejected by the hardened full-record reader. */
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_CORRUPT);
    assert(openfs_journal_replay(&v,&s,NULL,NULL)==OPENFS_JOURNAL_INVALID_ARGUMENT);
    free(d.b);return 0;
}
