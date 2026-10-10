#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "openfs/dir.h"
#include "openfs/bitmap.h"
#include "openfs/fd.h"
#include "openfs/format.h"
#include "openfs/fsck.h"
#include "openfs/inode.h"
#include "openfs/mount.h"
#include "openfs/orphan.h"
#include "openfs/xattr.h"
#include "openfs/file.h"
#include "openfs/extent.h"
#include "openfs/cow.h"
#include "openfs/path.h"

typedef struct {
    uint8_t *b;
    uint32_t bs;
    uint64_t n;
    uint64_t journal_start;
    uint64_t journal_blocks;
    int fail_home_after_commit;
    int commit_seen;
} disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*out){disk_t*d=(disk_t*)c;if(f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(out,d->b+(size_t)(f*d->bs),(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*in){
    disk_t*d=(disk_t*)c;
    if(f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;
    int in_journal=f>=d->journal_start&&f<d->journal_start+d->journal_blocks;
    const uint8_t*raw=(const uint8_t*)in;
    if(d->fail_home_after_commit&&d->commit_seen&&!in_journal){
        d->fail_home_after_commit=0;
        return OPENFS_IO_IO_ERROR;
    }
    memcpy(d->b+(size_t)(f*d->bs),in,(size_t)n*d->bs);
    if(d->fail_home_after_commit&&in_journal&&n==1U&&memcmp(raw,OPENFS_JOURNAL_MAGIC,5U)==0&&raw[5U]==OPENFS_JOURNAL_COMMIT)
        d->commit_seen=1;
    return OPENFS_IO_OK;
}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}

static uint64_t inode_count(const openfs_superblock_t*s){return (s->inode_table_blocks*(uint64_t)s->block_size)/OPENFS_INODE_SIZE;}

static void test_open_unlink_close(openfs_block_device_t *dev,openfs_superblock_t *sb)
{
    openfs_mount_t m;
    assert(openfs_mount(&m,dev)==OPENFS_MOUNT_OK);
    openfs_file_handle_t *h=NULL;
    {openfs_fd_result_t fr=openfs_fd_open(dev,&m.superblock,"/unlink-open",OPENFS_FD_CREAT|OPENFS_FD_RDWR,0644U,&h);if(fr!=OPENFS_FD_OK)fprintf(stderr,"orphan fd open result=%d\\n",(int)fr);assert(fr==OPENFS_FD_OK);}
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
    openfs_inode_t target;
    assert(openfs_inode_read(dev,sb->inode_table_start,ino,inode_count(sb),&target)==OPENFS_INODE_OK);
    assert(openfs_file_write(dev,sb,&target,0U,"orphan-data",11U)==OPENFS_FILE_OK);
    assert(openfs_xattr_set(dev,sb,ino,"user.recovery","orphan-value",12U,OPENFS_XATTR_CREATE)==OPENFS_XATTR_OK);
    openfs_inode_t parent;
    assert(openfs_inode_read(dev,sb->inode_table_start,sb->root_inode,inode_count(sb),&parent)==OPENFS_INODE_OK);
    assert(openfs_inode_read(dev,sb->inode_table_start,ino,inode_count(sb),&target)==OPENFS_INODE_OK);
    uint64_t xattr_block=openfs_inode_get_xattr_block(&target);
    assert(xattr_block!=0U);
    openfs_extent_t data_extent;
    assert(openfs_inode_get_extent(&target,0U,&data_extent)==OPENFS_EXTENT_OK);
    uint64_t data_block=data_extent.physical_start;
    assert(data_block!=0U);
    assert(openfs_dir_remove(dev,sb,&parent,"crash-orphan")==OPENFS_DIR_OK);
    target.link_count=0U;
    target.flags|=OPENFS_INODE_FLAG_ORPHAN;
    assert(openfs_inode_write(dev,sb->inode_table_start,inode_count(sb),&target)==OPENFS_INODE_OK);
    assert(dev->flush(dev->context)==OPENFS_IO_OK);

    disk_t *disk=(disk_t*)dev->context;
    disk->journal_start=sb->journal_start;
    disk->journal_blocks=sb->journal_blocks;
    disk->fail_home_after_commit=1;
    disk->commit_seen=0;
    openfs_mount_t failed_mount;
    assert(openfs_mount(&failed_mount,dev)==OPENFS_MOUNT_IO_ERROR);
    assert(disk->commit_seen==1&&disk->fail_home_after_commit==0);

    /* The committed orphan-retirement transaction must be replayable in full. */
    openfs_mount_t m;
    assert(openfs_mount(&m,dev)==OPENFS_MOUNT_OK);
    int used=1;
    assert(openfs_bitmap_test(dev,m.superblock.inode_bitmap_start,m.superblock.inode_bitmap_blocks,ino-1U,&used)==OPENFS_BITMAP_OK&&used==0);
    assert(openfs_bitmap_test(dev,m.superblock.block_bitmap_start,m.superblock.block_bitmap_blocks,data_block,&used)==OPENFS_BITMAP_OK&&used==0);
    assert(openfs_bitmap_test(dev,m.superblock.block_bitmap_start,m.superblock.block_bitmap_blocks,xattr_block,&used)==OPENFS_BITMAP_OK&&used==0);
    uint16_t refs=1U;
    assert(openfs_cow_refcount_get(dev,&m.superblock,data_block,&refs)==OPENFS_COW_OK&&refs==0U);
    refs=1U;
    assert(openfs_cow_refcount_get(dev,&m.superblock,xattr_block,&refs)==OPENFS_COW_OK&&refs==0U);
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
