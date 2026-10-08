#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/file.h"
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
    uint64_t inode_number=0U;
    assert(openfs_inode_alloc(&dev,&mount.superblock,mount.superblock.root_inode,
                              OPENFS_INODE_MODE_REGULAR|0644U,&inode_number)==OPENFS_INODE_ALLOC_OK);
    openfs_inode_t inode;
    uint64_t inode_count=(mount.superblock.inode_table_blocks*(uint64_t)mount.superblock.block_size)/OPENFS_INODE_SIZE;
    assert(openfs_inode_read(&dev,mount.superblock.inode_table_start,inode_number,inode_count,&inode)==OPENFS_INODE_OK);

    uint8_t payload[4096];
    memset(payload,0x5a,sizeof(payload));
    disk.fail_block=mount.superblock.inode_table_start;
    disk.fail_enabled=1;
    assert(openfs_file_write(&dev,&mount.superblock,&inode,0U,payload,sizeof(payload))==OPENFS_FILE_IO_ERROR);
    disk.fail_enabled=0;
    assert(openfs_unmount(&mount)==OPENFS_MOUNT_OK);

    openfs_mount_t recovered={0};
    assert(openfs_mount(&recovered,&dev)==OPENFS_MOUNT_OK);
    assert(openfs_inode_read(&dev,recovered.superblock.inode_table_start,inode_number,
                             (recovered.superblock.inode_table_blocks*(uint64_t)recovered.superblock.block_size)/OPENFS_INODE_SIZE,
                             &inode)==OPENFS_INODE_OK);
    assert(inode.size==sizeof(payload));
    uint8_t got[4096]={0};
    size_t got_len=0U;
    assert(openfs_file_read(&dev,&recovered.superblock,&inode,0U,got,sizeof(got),&got_len)==OPENFS_FILE_OK);
    assert(got_len==sizeof(got));
    assert(memcmp(got,payload,sizeof(got))==0);
    assert(openfs_unmount(&recovered)==OPENFS_MOUNT_OK);
    free(disk.bytes);
    return 0;
}
