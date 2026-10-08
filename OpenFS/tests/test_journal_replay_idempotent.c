#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "openfs/format.h"
#include "openfs/journal.h"

#define BS 4096U
#define BC 256U

typedef struct { uint8_t *bytes; uint32_t bs; uint64_t bc; } disk_t;
static openfs_io_result_t rd(void *ctx,uint64_t first,uint32_t count,void*out){disk_t*d=(disk_t*)ctx;if(count==0U||first>=d->bc||(uint64_t)count>d->bc-first)return OPENFS_IO_OUT_OF_RANGE;memcpy(out,d->bytes+(size_t)(first*d->bs),(size_t)count*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void *ctx,uint64_t first,uint32_t count,const void*in){disk_t*d=(disk_t*)ctx;if(count==0U||first>=d->bc||(uint64_t)count>d->bc-first)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->bytes+(size_t)(first*d->bs),in,(size_t)count*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void *ctx){(void)ctx;return OPENFS_IO_OK;}
static openfs_journal_result_t apply(void *ctx,uint64_t tx,const uint8_t*data,uint32_t len){disk_t*d=(disk_t*)ctx;(void)tx;if(data==NULL||len!=1024U)return OPENFS_JOURNAL_CORRUPT;uint64_t target=0U;for(uint64_t i=0U;i<d->bc;i++){if(i==d->bc-1U)continue;if(memcmp(d->bytes+(size_t)(i*d->bs),data,len)==0){target=i;break;}}if(target==0U)return OPENFS_JOURNAL_CORRUPT;memcpy(d->bytes+(size_t)(target*d->bs),data,len);return OPENFS_JOURNAL_OK;}
int main(void){disk_t d={0};d.bs=BS;d.bc=BC;d.bytes=(uint8_t*)calloc((size_t)d.bc,d.bs);assert(d.bytes);openfs_block_device_t v={&d,d.bs,d.bc,rd,wr,fl};uint8_t uuid[16]={0x52U,0x50U,0x4CU};assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);uint64_t tx=0U,target=s.data_start+12U;d.fail_enabled=0;assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);uint8_t block[1024U];memset(block,0xD3U,sizeof(block));assert(openfs_journal_write(&j,&v,tx,block,sizeof(block))==OPENFS_JOURNAL_OK);assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_replay(&v,&s,apply,&d)==OPENFS_JOURNAL_OK);assert(memcmp(d.bytes+(size_t)(target*d.bs),block,sizeof(block))==0);uint8_t snapshot[1024U];memcpy(snapshot,d.bytes+(size_t)(target*d.bs),sizeof(snapshot));assert(openfs_journal_replay(&v,&s,apply,&d)==OPENFS_JOURNAL_OK);assert(memcmp(d.bytes+(size_t)(target*d.bs),snapshot,sizeof(snapshot))==0);free(d.bytes);return 0;}
