#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/fsck.h"
#include "openfs/bitmap.h"
#include "openfs/format.h"

typedef struct { uint8_t *bytes; uint32_t bs; uint64_t bc; uint64_t fail_block; int fail_enabled; int fail_once; int fail_flush; } disk_t;
static openfs_io_result_t rd(void*c,uint64_t b,uint32_t n,void*out){disk_t*d=c;if(!d||!out||b>=d->bc||(uint64_t)n>d->bc-b)return OPENFS_IO_OUT_OF_RANGE;memcpy(out,d->bytes+(size_t)(b*d->bs),(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t b,uint32_t n,const void*in){disk_t*d=c;if(!d||!in||b>=d->bc||(uint64_t)n>d->bc-b)return OPENFS_IO_OUT_OF_RANGE;if(d->fail_enabled&&b==d->fail_block){if(d->fail_once){d->fail_once=0;d->fail_enabled=0;}return OPENFS_IO_IO_ERROR;}memcpy(d->bytes+(size_t)(b*d->bs),in,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){disk_t*d=c;return d&&!d->fail_flush?OPENFS_IO_OK:OPENFS_IO_IO_ERROR;}
static openfs_block_device_t dev(disk_t*d){openfs_block_device_t v={d,d->bs,d->bc,rd,wr,fl};return v;}
static void setup(disk_t*d,openfs_block_device_t*v,openfs_superblock_t*s){uint8_t u[16]={0x71U};memset(d,0,sizeof(*d));d->bs=4096U;d->bc=256U;d->bytes=calloc((size_t)d->bs,d->bc);assert(d->bytes);*v=dev(d);assert(openfs_format(v,u)==OPENFS_FORMAT_OK);assert(openfs_read_superblock(v,s)==OPENFS_FORMAT_OK);}
static uint64_t inode_count(const openfs_superblock_t*s){return s->inode_table_blocks*(uint64_t)s->block_size/OPENFS_INODE_SIZE;}
int main(void){
    disk_t d;openfs_block_device_t v;openfs_superblock_t s;setup(&d,&v,&s);
    uint64_t stray_block=s.total_blocks,stray_inode=inode_count(&s),errors=0;
    assert(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,stray_block,1)==OPENFS_BITMAP_OK);
    assert(openfs_bitmap_set(&v,s.inode_bitmap_start,s.inode_bitmap_blocks,stray_inode,1)==OPENFS_BITMAP_OK);
    size_t bytes=(size_t)d.bs*d.bc;uint8_t*before=malloc(bytes);assert(before);memcpy(before,d.bytes,bytes);
    d.fail_enabled=1;d.fail_once=1;d.fail_block=s.inode_bitmap_start;
    assert(openfs_fsck_repair_bitmap_tails(&v,&s,&errors)==OPENFS_FSCK_IO_ERROR);
    assert(memcmp(before,d.bytes,bytes)==0);
    free(before);

    d.fail_enabled=1;d.fail_once=0;
    assert(openfs_fsck_repair_bitmap_tails(&v,&s,&errors)==OPENFS_FSCK_CORRUPT);
    free(d.bytes);return 0;
}
