#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/fsck.h"
#include "openfs/bitmap.h"
#include "openfs/format.h"
#include "openfs/mount.h"

typedef struct { uint8_t *data; uint32_t block_size; uint64_t blocks; uint64_t fail_block; unsigned fail_count; } repair_device_t;

static openfs_io_result_t rd(void *ctx,uint64_t block,uint32_t count,void *buffer){
    repair_device_t *d=(repair_device_t *)ctx;
    if(block>=d->blocks||(uint64_t)count>d->blocks-block)return OPENFS_IO_OUT_OF_RANGE;
    memcpy(buffer,d->data+(size_t)(block*d->block_size),(size_t)count*d->block_size);
    return OPENFS_IO_OK;
}
static openfs_io_result_t wr(void *ctx,uint64_t block,uint32_t count,const void *buffer){
    repair_device_t *d=(repair_device_t *)ctx;
    if(block>=d->blocks||(uint64_t)count>d->blocks-block)return OPENFS_IO_OUT_OF_RANGE;
    if(d->fail_count!=0U && block==d->fail_block){d->fail_count--;return OPENFS_IO_IO_ERROR;}
    memcpy(d->data+(size_t)(block*d->block_size),buffer,(size_t)count*d->block_size);
    return OPENFS_IO_OK;
}
static openfs_io_result_t fl(void *ctx){(void)ctx;return OPENFS_IO_OK;}

int main(void){
    repair_device_t mem={0};
    mem.block_size=4096U;
    mem.blocks=256U;
    mem.data=(uint8_t *)calloc((size_t)mem.block_size,mem.blocks);
    assert(mem.data!=NULL);
    openfs_block_device_t d={&mem,mem.block_size,mem.blocks,rd,wr,fl};
    uint8_t uuid[16]={0x52U};
    assert(openfs_format(&d,uuid)==OPENFS_FORMAT_OK);

    openfs_superblock_t sb;
    assert(openfs_read_superblock(&d,&sb)==OPENFS_FORMAT_OK);
    uint64_t inode_count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
    uint64_t block_capacity=sb.block_bitmap_blocks*(uint64_t)sb.block_size*8U;
    uint64_t inode_capacity=sb.inode_bitmap_blocks*(uint64_t)sb.block_size*8U;
    assert(block_capacity>sb.total_blocks);
    assert(inode_capacity>inode_count);

    uint64_t errors=0U;
    assert(openfs_fsck(&d,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    /* Repair is idempotent: a clean second pass must make no on-disk change. */
    uint8_t *snapshot=(uint8_t *)malloc((size_t)mem.block_size*mem.blocks); assert(snapshot!=NULL); memcpy(snapshot,mem.data,(size_t)mem.block_size*mem.blocks);
    assert(openfs_fsck_repair_bitmap_tails(&d,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    assert(memcmp(snapshot,mem.data,(size_t)mem.block_size*mem.blocks)==0); free(snapshot);

    uint64_t stray_block=sb.total_blocks;
    uint64_t stray_inode=inode_count;
    assert(openfs_bitmap_set(&d,sb.block_bitmap_start,sb.block_bitmap_blocks,stray_block,1)==OPENFS_BITMAP_OK);
    assert(openfs_bitmap_set(&d,sb.inode_bitmap_start,sb.inode_bitmap_blocks,stray_inode,1)==OPENFS_BITMAP_OK);
    assert(openfs_fsck(&d,&sb,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);

    assert(openfs_fsck_repair_bitmap_tails(&d,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    int set=1;
    assert(openfs_bitmap_test(&d,sb.block_bitmap_start,sb.block_bitmap_blocks,stray_block,&set)==OPENFS_BITMAP_OK&&!set);
    assert(openfs_bitmap_test(&d,sb.inode_bitmap_start,sb.inode_bitmap_blocks,stray_inode,&set)==OPENFS_BITMAP_OK&&!set);
    assert(openfs_fsck(&d,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);

    assert(openfs_bitmap_set(&d,sb.inode_bitmap_start,sb.inode_bitmap_blocks,0U,0)==OPENFS_BITMAP_OK);
    assert(openfs_fsck_repair_bitmap_tails(&d,&sb,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);
    assert(openfs_bitmap_set(&d,sb.inode_bitmap_start,sb.inode_bitmap_blocks,0U,1)==OPENFS_BITMAP_OK);

    /* Mounted repairs must use the WAL: fail the second DATA record and verify that neither bitmap is partially repaired. */
    openfs_mount_t mounted;
    assert(openfs_mount(&mounted,&d)==OPENFS_MOUNT_OK);
    assert(openfs_bitmap_set(&d,mounted.superblock.block_bitmap_start,mounted.superblock.block_bitmap_blocks,stray_block,1)==OPENFS_BITMAP_OK);
    assert(openfs_bitmap_set(&d,mounted.superblock.inode_bitmap_start,mounted.superblock.inode_bitmap_blocks,stray_inode,1)==OPENFS_BITMAP_OK);
    int block_set=0,inode_set=0;
    assert(openfs_bitmap_test(&d,mounted.superblock.block_bitmap_start,mounted.superblock.block_bitmap_blocks,stray_block,&block_set)==OPENFS_BITMAP_OK&&block_set);
    assert(openfs_bitmap_test(&d,mounted.superblock.inode_bitmap_start,mounted.superblock.inode_bitmap_blocks,stray_inode,&inode_set)==OPENFS_BITMAP_OK&&inode_set);
    mem.fail_block=mounted.superblock.journal_start+2U;mem.fail_count=1U;
    assert(openfs_fsck_repair_bitmap_tails(&d,&mounted.superblock,&errors)==OPENFS_FSCK_IO_ERROR);
    mem.fail_count=0U;
    assert(openfs_bitmap_test(&d,mounted.superblock.block_bitmap_start,mounted.superblock.block_bitmap_blocks,stray_block,&block_set)==OPENFS_BITMAP_OK&&block_set);
    assert(openfs_bitmap_test(&d,mounted.superblock.inode_bitmap_start,mounted.superblock.inode_bitmap_blocks,stray_inode,&inode_set)==OPENFS_BITMAP_OK&&inode_set);
    assert(openfs_fsck_repair_bitmap_tails(&d,&mounted.superblock,&errors)==OPENFS_FSCK_OK&&errors==0U);
    assert(openfs_unmount(&mounted)==OPENFS_MOUNT_OK);

    free(mem.data);
    return 0;
}
