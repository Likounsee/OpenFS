#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/transaction.h"
#include "openfs/format.h"
typedef struct{uint8_t*b;uint32_t bs;uint64_t bc;}D;
static openfs_io_result_t r(void*c,uint64_t f,uint32_t n,void*x){D*d=c;if(n==0U||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(x,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t w(void*c,uint64_t f,uint32_t n,const void*x){D*d=c;if(n==0U||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),x,(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}
int main(void){D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bc,d.bs);assert(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};uint8_t uuid[16]={9U};assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);uint64_t target=s.data_start;uint8_t a[4096],b[4096],readback[4096];memset(a,0xA5U,sizeof(a));memset(b,0x5AU,sizeof(b));openfs_transaction_t t;assert(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);openfs_block_device_t*td=openfs_transaction_device(&t);assert(td!=NULL);assert(td->write(td->context,target,1U,a)==OPENFS_IO_OK);assert(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))!=0);assert(td->read(td->context,target,1U,readback)==OPENFS_IO_OK&&memcmp(readback,a,sizeof(a))==0);assert(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_OK);assert(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))==0);assert(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);td=openfs_transaction_device(&t);assert(td->write(td->context,target,1U,b)==OPENFS_IO_OK);assert(openfs_transaction_abort(&t)==OPENFS_TRANSACTION_OK);assert(memcmp(d.b+(size_t)(target*d.bs),a,sizeof(a))==0);free(d.b);return 0;}
