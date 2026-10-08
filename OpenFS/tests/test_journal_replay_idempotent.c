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
static openfs_journal_result_t apply(void *ctx,uint64_t tx,const uint8_t*data,uint32_t len){disk_t*d=(disk_t*)ctx;(void)tx;if(len!=BS+24U||memcmp(data,"OJBD1",5U)!=0)return OPENFS_JOURNAL_CORRUPT;uint64_t target=(uint64_t)data[8]|((uint64_t)data[9]<<8U)|((uint64_t)data[10]<<16U)|((uint64_t)data[11]<<24U)|((uint64_t)data[12]<<32U)|((uint64_t)data[13]<<40U)|((uint64_t)data[14]<<48U)|((uint64_t)data[15]<<56U);uint32_t offset=(uint32_t)data[16]|((uint32_t)data[17]<<8U)|((uint32_t)data[18]<<16U)|((uint32_t)data[19]<<24U);uint32_t count=(uint32_t)data[20]|((uint32_t)data[21]<<8U)|((uint32_t)data[22]<<16U)|((uint32_t)data[23]<<24U);if(target>=d->bc||offset!=0U||count!=BS)return OPENFS_JOURNAL_CORRUPT;memcpy(d->bytes+(size_t)(target*d->bs),data+24U,BS);return OPENFS_JOURNAL_OK;}
int main(void){disk_t d={0};d.bs=BS;d.bc=BC;d.bytes=(uint8_t*)calloc((size_t)d.bc,d.bs);assert(d.bytes);openfs_block_device_t v={&d,d.bs,d.bc,rd,wr,fl};uint8_t uuid[16]={0x52U,0x50U,0x4CU};assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);uint64_t tx=0U,target=s.data_start+12U;assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);uint8_t block[BS],payload[BS+24U];memset(block,0xD3U,sizeof(block));memset(payload,0,sizeof(payload));memcpy(payload,"OJBD1",5U);for(unsigned i=0U;i<8U;i++)payload[8U+i]=(uint8_t)(target>>(8U*i));payload[16U]=0U;payload[17U]=0U;payload[18U]=0U;payload[19U]=0U;payload[20U]=(uint8_t)BS;payload[21U]=(uint8_t)(BS>>8U);payload[22U]=(uint8_t)(BS>>16U);payload[23U]=(uint8_t)(BS>>24U);memcpy(payload+24U,block,BS);assert(openfs_journal_write(&j,&v,tx,payload,sizeof(payload))==OPENFS_JOURNAL_OK);assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);assert(memcmp(d.bytes+(size_t)(target*d.bs),block,BS)!=0);assert(openfs_journal_replay(&v,&s,apply,&d)==OPENFS_JOURNAL_OK);assert(memcmp(d.bytes+(size_t)(target*d.bs),block,BS)==0);uint8_t snapshot[BS];memcpy(snapshot,d.bytes+(size_t)(target*d.bs),BS);assert(openfs_journal_replay(&v,&s,apply,&d)==OPENFS_JOURNAL_OK);assert(memcmp(d.bytes+(size_t)(target*d.bs),snapshot,BS)==0);free(d.bytes);return 0;}
