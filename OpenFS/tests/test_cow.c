#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/format.h"
#include "openfs/allocator.h"
#include "openfs/bitmap.h"
#include "openfs/cow.h"
#include "openfs/fsck.h"

typedef struct { uint8_t *bytes; uint32_t bs; uint64_t n; } disk_t;
static openfs_io_result_t rd(void*c,uint64_t b,uint32_t n,void*out){disk_t*d=(disk_t*)c;if(n==0U||b>=d->n||(uint64_t)n>d->n-b)return OPENFS_IO_OUT_OF_RANGE;memcpy(out,d->bytes+(size_t)b*d->bs,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t b,uint32_t n,const void*in){disk_t*d=(disk_t*)c;if(n==0U||b>=d->n||(uint64_t)n>d->n-b)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->bytes+(size_t)b*d->bs,in,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}

int main(void)
{
    disk_t d={.bs=4096U,.n=256U};d.bytes=calloc((size_t)d.bs,d.n);assert(d.bytes);
    openfs_block_device_t v={&d,d.bs,d.n,rd,wr,fl};uint8_t uuid[16]={0x81U};
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_superblock_t sb;assert(openfs_read_superblock(&v,&sb)==OPENFS_FORMAT_OK);
    assert((sb.feature_flags&OPENFS_FEATURE_COW)!=0U);
    assert(sb.refcount_start>sb.journal_start&&sb.refcount_blocks>0U&&sb.refcount_start+sb.refcount_blocks==sb.data_start);
    uint64_t block=0U;assert(openfs_alloc_block(&v,&sb,&block)==OPENFS_ALLOC_OK);
    uint16_t refs=0U;assert(openfs_cow_refcount_get(&v,&sb,block,&refs)==OPENFS_COW_OK&&refs==1U);
    assert(openfs_cow_refcount_inc(&v,&sb,block,&refs)==OPENFS_COW_OK&&refs==2U);
    assert(openfs_free_block(&v,&sb,block)==OPENFS_ALLOC_OK);
    int used=0;assert(openfs_bitmap_test(&v,sb.block_bitmap_start,sb.block_bitmap_blocks,block,&used)==OPENFS_BITMAP_OK&&used!=0);
    assert(openfs_cow_refcount_get(&v,&sb,block,&refs)==OPENFS_COW_OK&&refs==1U);
    assert(openfs_free_block(&v,&sb,block)==OPENFS_ALLOC_OK);
    assert(openfs_bitmap_test(&v,sb.block_bitmap_start,sb.block_bitmap_blocks,block,&used)==OPENFS_BITMAP_OK&&used==0);
    assert(openfs_cow_refcount_get(&v,&sb,block,&refs)==OPENFS_COW_OK&&refs==0U);
    uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    free(d.bytes);return 0;
}
