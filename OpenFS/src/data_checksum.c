#include "openfs/data_checksum.h"
#include "openfs/crc32c.h"
#include "openfs/bitmap.h"
#include <stdlib.h>
#include <string.h>
uint32_t openfs_data_checksum(const void *data,uint32_t length){return openfs_crc32c(data,length);}
static int locate(const openfs_superblock_t*s,uint64_t b,uint64_t*tb,uint32_t*off){if(s==NULL||tb==NULL||off==NULL||b<s->data_start||b>=s->data_start+s->data_blocks||s->data_checksum_blocks==0U)return 0;uint64_t idx=b-s->data_start,epb=(uint64_t)s->block_size/4U;if(epb==0U)return 0;*tb=s->data_checksum_start+idx/epb;*off=(uint32_t)((idx%epb)*4U);return *tb<s->data_checksum_start+s->data_checksum_blocks;}
static uint32_t get32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8U)|((uint32_t)p[2]<<16U)|((uint32_t)p[3]<<24U);} static void put32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);p[2]=(uint8_t)(v>>16U);p[3]=(uint8_t)(v>>24U);}
int openfs_data_checksum_get(const openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t b,uint32_t*out){if(!openfs_block_device_is_valid(d)||out==NULL)return -1;uint64_t tb;uint32_t off;if(!locate(s,b,&tb,&off))return -1;uint8_t*p=malloc(d->block_size);if(p==NULL)return -1;int ok=d->read(d->context,tb,1U,p)==OPENFS_IO_OK; if(ok)*out=get32(p+off);free(p);return ok?0:-1;}
int openfs_data_checksum_set(const openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t b,uint32_t v){if(!openfs_block_device_is_valid(d))return -1;uint64_t tb;uint32_t off;if(!locate(s,b,&tb,&off))return -1;uint8_t*p=malloc(d->block_size);if(p==NULL)return -1;if(d->read(d->context,tb,1U,p)!=OPENFS_IO_OK){free(p);return -1;}put32(p+off,v);int ok=d->write(d->context,tb,1U,p)==OPENFS_IO_OK;free(p);return ok?0:-1;}
int openfs_data_checksum_set_tx(openfs_transaction_t*t,const openfs_superblock_t*s,uint64_t b,uint32_t v){openfs_block_device_t*d=openfs_transaction_device(t);if(d==NULL)return -1;return openfs_data_checksum_set(d,s,b,v);}
