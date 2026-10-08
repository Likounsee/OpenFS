#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/bitmap.h"
#include "openfs/format.h"
#include "openfs/inode.h"
#include "openfs/inode_alloc.h"
#include "openfs/mount.h"

typedef struct {
    uint8_t *bytes;
    uint32_t bs;
    uint64_t blocks;
    uint64_t fail_block;
    int fail_enabled;
} disk_t;

static openfs_io_result_t rd(void *ctx,uint64_t first,uint32_t count,void *out)
{
    disk_t *d=(disk_t *)ctx;
    if(d==NULL||out==NULL||count==0U||first>=d->blocks||(uint64_t)count>d->blocks-first)return OPENFS_IO_OUT_OF_RANGE;
    memcpy(out,d->bytes+(size_t)(first*d->bs),(size_t)count*d->bs);
    return OPENFS_IO_OK;
}

static openfs_io_result_t wr(void *ctx,uint64_t first,uint32_t count,const void *in)
{
    disk_t *d=(disk_t *)ctx;
    if(d==NULL||in==NULL||count==0U||first>=d->blocks||(uint64_t)count>d->blocks-first)return OPENFS_IO_OUT_OF_RANGE;
    if(d->fail_enabled&&count==1U&&first==d->fail_block)return OPENFS_IO_IO_ERROR;
    memcpy(d->bytes+(size_t)(first*d->bs),in,(size_t)count*d->bs);
    return OPENFS_IO_OK;
}

static openfs_io_result_t fl(void *ctx){(void)ctx;return OPENFS_IO_OK;}

int main(void)
{
    disk_t disk={0};
    disk.bs=4096U;
    disk.blocks=128U;
    disk.bytes=(uint8_t *)calloc((size_t)disk.bs,disk.blocks);
    assert(disk.bytes!=NULL);
    openfs_block_device_t dev={&disk,disk.bs,disk.blocks,rd,wr,fl};
    uint8_t uuid[16]={0};
    assert(openfs_format(&dev,uuid)==OPENFS_FORMAT_OK);

    openfs_mount_t mount={0};
    assert(openfs_mount(&mount,&dev)==OPENFS_MOUNT_OK);
    uint64_t bitmap_block=mount.superblock.inode_bitmap_start;
    disk.fail_block=bitmap_block;
    disk.fail_enabled=1;

    uint64_t ino=0U;
    assert(openfs_inode_alloc(&dev,&mount.superblock,mount.superblock.root_inode,
                              OPENFS_INODE_MODE_REGULAR|0644U,&ino)==OPENFS_INODE_ALLOC_IO_ERROR);
    assert(ino==0U);
    disk.fail_enabled=0;
    assert(openfs_unmount(&mount)==OPENFS_MOUNT_OK);

    openfs_mount_t recovered={0};
    assert(openfs_mount(&recovered,&dev)==OPENFS_MOUNT_OK);
    int used=0;
    assert(openfs_bitmap_test(&dev,recovered.superblock.inode_bitmap_start,
                              recovered.superblock.inode_bitmap_blocks,1U,&used)==OPENFS_BITMAP_OK);
    assert(used==1);
    openfs_inode_t inode;
    assert(openfs_inode_read(&dev,recovered.superblock.inode_table_start,2U,
                             (recovered.superblock.inode_table_blocks*(uint64_t)recovered.superblock.block_size)/OPENFS_INODE_SIZE,
                             &inode)==OPENFS_INODE_OK);
    assert(inode.inode_number==2U);
    assert((inode.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_REGULAR);
    assert(inode.parent_inode==recovered.superblock.root_inode);
    assert(openfs_unmount(&recovered)==OPENFS_MOUNT_OK);
    free(disk.bytes);
    return 0;
}
