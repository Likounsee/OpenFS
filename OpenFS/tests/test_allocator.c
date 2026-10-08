#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/allocator.h"
#include "openfs/format.h"
#include "openfs/bitmap.h"
typedef struct {uint8_t*b;uint32_t bs;uint64_t n;} disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*out){disk_t*d=c;if(n==0U||f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(out,d->b+f*d->bs,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*in){disk_t*d=c;if(n==0U||f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+f*d->bs,in,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}
int main(void){disk_t x={.bs=4096U,.n=128U};x.b=calloc((size_t)x.bs,x.n);assert(x.b);openfs_block_device_t d={&x,x.bs,x.n,rd,wr,fl};uint8_t u[16]={0};assert(openfs_format(&d,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&d,&s)==OPENFS_FORMAT_OK);uint64_t a=0,b=0;assert(openfs_alloc_block(&d,&s,&a)==OPENFS_ALLOC_OK);assert(a>=s.data_start&&a<s.data_start+s.data_blocks);assert(openfs_alloc_block(&d,&s,&b)==OPENFS_ALLOC_OK&&b!=a);assert(openfs_free_block(&d,&s,a)==OPENFS_ALLOC_OK);uint64_t c=0;assert(openfs_alloc_block(&d,&s,&c)==OPENFS_ALLOC_OK&&c==a);assert(openfs_free_block(&d,&s,b)==OPENFS_ALLOC_OK);int bit=0;assert(openfs_bitmap_set(&d,s.block_bitmap_start,s.block_bitmap_blocks,0U,2)==OPENFS_BITMAP_INVALID_ARGUMENT);assert(openfs_bitmap_test(&d,s.block_bitmap_start,s.block_bitmap_blocks,UINT64_MAX,&bit)==OPENFS_BITMAP_OUT_OF_RANGE);assert(openfs_bitmap_test(&d,d.block_count-1U,2U,0U,&bit)==OPENFS_BITMAP_OUT_OF_RANGE);free(x.b);return 0;}
