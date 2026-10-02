#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/fsck.h"
typedef struct{uint8_t*b;uint32_t bs;uint64_t bc;}D;
static openfs_io_result_t r(void*c,uint64_t f,uint32_t n,void*x){D*d=c;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(x,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t w(void*c,uint64_t f,uint32_t n,const void*x){D*d=c;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),x,(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t f(void*c){(void)c;return OPENFS_IO_OK;}
int main(void){D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={0};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);uint64_t errors=99;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);uint8_t *raw=d.b+(size_t)(s.inode_table_start*d.bs);raw[216]='X';errors=0;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);free(d.b);return 0;}