#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/mount.h"
#include "openfs/path.h"
#include "openfs/file.h"
#include "openfs/cow.h"
#include "openfs/inode_alloc.h"
#include "openfs/fsck.h"

typedef struct { uint8_t *bytes; uint32_t bs; uint64_t n; } disk_t;
static openfs_io_result_t rd(void*c,uint64_t b,uint32_t n,void*out){disk_t*d=(disk_t*)c;if(n==0U||b>=d->n||(uint64_t)n>d->n-b)return OPENFS_IO_OUT_OF_RANGE;memcpy(out,d->bytes+(size_t)b*d->bs,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t b,uint32_t n,const void*in){disk_t*d=(disk_t*)c;if(n==0U||b>=d->n||(uint64_t)n>d->n-b)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->bytes+(size_t)b*d->bs,in,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}

int main(void)
{
    disk_t d={.bs=4096U,.n=512U};d.bytes=calloc((size_t)d.bs,d.n);assert(d.bytes);
    openfs_block_device_t v={&d,d.bs,d.n,rd,wr,fl};uint8_t uuid[16]={0x91U};
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_mount_t m;assert(openfs_mount(&m,&v)==OPENFS_MOUNT_OK);
    uint64_t source_ino=0U;assert(openfs_path_create(&v,&m.superblock,"/source",OPENFS_INODE_MODE_REGULAR|0644U,&source_ino)==OPENFS_PATH_OK);
    uint64_t count=(m.superblock.inode_table_blocks*(uint64_t)m.superblock.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t source;assert(openfs_inode_read(&v,m.superblock.inode_table_start,source_ino,count,&source)==OPENFS_INODE_OK);
    uint8_t a[4096];memset(a,'A',sizeof(a));assert(openfs_file_write(&v,&m.superblock,&source,0U,a,sizeof(a))==OPENFS_FILE_OK);
    assert(openfs_inode_write(&v,m.superblock.inode_table_start,count,&source)==OPENFS_INODE_OK);
    openfs_inode_t source_after;assert(openfs_inode_read(&v,m.superblock.inode_table_start,source_ino,count,&source_after)==OPENFS_INODE_OK);
    openfs_extent_t source_extent;assert(openfs_inode_get_extent(&source_after,0U,&source_extent)==OPENFS_EXTENT_OK);
    uint16_t refs=0U;assert(openfs_cow_refcount_get(&v,&m.superblock,source_extent.physical_start,&refs)==OPENFS_COW_OK&&refs==1U);

    uint64_t clone_ino=0U;assert(openfs_cow_clone_inode(&v,&m.superblock,&source_after,m.superblock.root_inode,&clone_ino)==OPENFS_COW_OK);
    openfs_inode_t clone;assert(openfs_inode_read(&v,m.superblock.inode_table_start,clone_ino,count,&clone)==OPENFS_INODE_OK);
    assert(openfs_cow_refcount_get(&v,&m.superblock,source_extent.physical_start,&refs)==OPENFS_COW_OK&&refs==2U);

    uint8_t b[4096];memset(b,'B',sizeof(b));assert(openfs_file_write(&v,&m.superblock,&clone,0U,b,sizeof(b))==OPENFS_FILE_OK);
    openfs_inode_t source_check;assert(openfs_inode_read(&v,m.superblock.inode_table_start,source_ino,count,&source_check)==OPENFS_INODE_OK);
    openfs_inode_t clone_check;assert(openfs_inode_read(&v,m.superblock.inode_table_start,clone_ino,count,&clone_check)==OPENFS_INODE_OK);
    openfs_extent_t clone_extent;assert(openfs_inode_get_extent(&clone_check,0U,&clone_extent)==OPENFS_EXTENT_OK);
    assert(clone_extent.physical_start!=source_extent.physical_start);
    assert(openfs_cow_refcount_get(&v,&m.superblock,source_extent.physical_start,&refs)==OPENFS_COW_OK&&refs==1U);
    assert(openfs_cow_refcount_get(&v,&m.superblock,clone_extent.physical_start,&refs)==OPENFS_COW_OK&&refs==1U);

    uint8_t out[4096];size_t got=0U;assert(openfs_file_read(&v,&m.superblock,&source_check,0U,out,sizeof(out),&got)==OPENFS_FILE_OK&&got==sizeof(out)&&out[0]=='A');
    assert(openfs_file_read(&v,&m.superblock,&clone_check,0U,out,sizeof(out),&got)==OPENFS_FILE_OK&&got==sizeof(out)&&out[0]=='B');

    assert(openfs_free_block(&v,&m.superblock,clone_extent.physical_start)==OPENFS_ALLOC_OK);
    clone_check.flags=0U;clone_check.extent_count=0U;clone_check.blocks=0U;clone_check.link_count=0U;clone_check.size=0U;clone_check.mode=OPENFS_INODE_MODE_FREE;
    assert(openfs_inode_write(&v,m.superblock.inode_table_start,count,&clone_check)==OPENFS_INODE_OK);
    assert(openfs_inode_free(&v,&m.superblock,clone_ino)==OPENFS_INODE_ALLOC_OK);
    uint64_t errors=0U;assert(openfs_fsck(&v,&m.superblock,&errors)==OPENFS_FSCK_OK&&errors==0U);
    assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
    free(d.bytes);return 0;
}
