#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/dir.h"
#include "openfs/fd.h"
#include "openfs/format.h"
#include "openfs/fsck.h"
#include "openfs/inode.h"
#include "openfs/mount.h"
#include "openfs/orphan.h"
#include "openfs/path.h"

typedef struct { uint8_t *b; uint32_t bs; uint64_t n; } disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*out){disk_t*d=(disk_t*)c;if(f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(out,d->b+(size_t)(f*d->bs),(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*in){disk_t*d=(disk_t*)c;if(f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),in,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}

static uint64_t inode_count(const openfs_superblock_t*s){return (s->inode_table_blocks*(uint64_t)s->block_size)/OPENFS_INODE_SIZE;}

static void test_open_unlink_close(openfs_block_device_t *dev,openfs_superblock_t *sb)
{
    openfs_mount_t m;
    assert(openfs_mount(&m,dev)==OPENFS_MOUNT_OK);
    openfs_file_handle_t *h=NULL;
    assert(openfs_fd_open(dev,&m.superblock,"/unlink-open",OPENFS_FD_CREAT|OPENFS_FD_RDWR,0644U,&h)==OPENFS_FD_OK);
    assert(openfs_fd_write(h,"orphan-data",11U)==OPENFS_FD_OK);
    openfs_inode_t st;assert(openfs_fd_stat(h,&st)==OPENFS_FD_OK);uint64_t ino=st.inode_number;uint64_t generation=st.generation;
    assert(openfs_path_unlink(dev,&m.superblock,"/unlink-open")==OPENFS_PATH_OK);
    uint64_t missing=0U;
    assert(openfs_path_lookup(dev,&m.superblock,"/unlink-open",&missing)==OPENFS_PATH_NOT_FOUND);
    int64_t pos=0;size_t got=0;char data[32]={0};
    assert(openfs_fd_seek(h,0,0,&pos)==OPENFS_FD_OK);
    assert(openfs_fd_read(h,data,sizeof(data),&got)==OPENFS_FD_OK&&got==11U&&memcmp(data,"orphan-data",11U)==0);
    assert(openfs_runtime_handle_count(&m.runtime,dev,ino,generation)==1U);
    assert(openfs_unmount(&m)==OPENFS_MOUNT_IO_ERROR);
    assert(openfs_fd_close(h)==OPENFS_FD_OK);
    uint64_t errors=0U;
    assert(openfs_fsck(dev,&m.superblock,&errors)==OPENFS_FSCK_OK&&errors==0U);
    assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
    *sb=m.superblock;
}

static void test_mount_recovery(openfs_block_device_t *dev,openfs_superblock_t *sb)
{
    uint64_t ino=0U;
    assert(openfs_path_create(dev,sb,"/crash-orphan",OPENFS_INODE_MODE_REGULAR|0644U,&ino)==OPENFS_PATH_OK);
    openfs_inode_t parent,target;
    assert(openfs_inode_read(dev,sb->inode_table_start,sb->root_inode,inode_count(sb),&parent)==OPENFS_INODE_OK);
    assert(openfs_inode_read(dev,sb->inode_table_start,ino,inode_count(sb),&target)==OPENFS_INODE_OK);
    assert(openfs_dir_remove(dev,sb,&parent,"crash-orphan")==OPENFS_DIR_OK);
    target.link_count=0U;
    target.flags|=OPENFS_INODE_FLAG_ORPHAN;
    assert(openfs_inode_write(dev,sb->inode_table_start,inode_count(sb),&target)==OPENFS_INODE_OK);
    assert(dev->flush(dev->context)==OPENFS_IO_OK);
    openfs_mount_t m;
    assert(openfs_mount(&m,dev)==OPENFS_MOUNT_OK);
    int used=1;
    assert(openfs_bitmap_test(dev,m.superblock.inode_bitmap_start,m.superblock.inode_bitmap_blocks,ino-1U,&used)==OPENFS_BITMAP_OK&&used==0);
    uint64_t errors=0U;
    assert(openfs_fsck(dev,&m.superblock,&errors)==OPENFS_FSCK_OK&&errors==0U);
    assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
}

int main(void)
{
    disk_t d={0};d.bs=4096U;d.n=512U;d.b=(uint8_t*)calloc((size_t)d.n,d.bs);assert(d.b);
    openfs_block_device_t dev={&d,d.bs,d.n,rd,wr,fl};
    uint8_t uuid[16]={0};openfs_superblock_t sb;
    assert(openfs_format(&dev,uuid)==OPENFS_FORMAT_OK);
    assert(openfs_read_superblock(&dev,&sb)==OPENFS_FORMAT_OK);
    test_open_unlink_close(&dev,&sb);
    assert(openfs_read_superblock(&dev,&sb)==OPENFS_FORMAT_OK);
    test_mount_recovery(&dev,&sb);
    free(d.b);
    return 0;
}
