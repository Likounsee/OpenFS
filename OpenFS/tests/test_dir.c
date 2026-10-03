#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/dir.h"
#include "openfs/inode_alloc.h"
#include "openfs/path.h"
#include "openfs/fsck.h"

typedef struct {uint8_t *bytes;uint32_t block_size;uint64_t block_count;int fail_flush;int fail_flush_once;} disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*b){disk_t*d=c;if(n==0U||f>=d->block_count||(uint64_t)n>d->block_count-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(b,d->bytes+(size_t)(f*d->block_size),(size_t)((uint64_t)n*d->block_size));return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*b){disk_t*d=c;if(n==0U||f>=d->block_count||(uint64_t)n>d->block_count-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->bytes+(size_t)(f*d->block_size),b,(size_t)((uint64_t)n*d->block_size));return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){disk_t*d=c;if(d->fail_flush){if(d->fail_flush_once)d->fail_flush=0;return OPENFS_IO_IO_ERROR;}return OPENFS_IO_OK;}
static void unlink_flush_failure_rolls_back(void)
{
    disk_t d={.block_size=4096U,.block_count=128U};
    d.bytes=calloc((size_t)d.block_count,d.block_size);
    assert(d.bytes);
    openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};
    uint8_t uuid[16]={0};
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_superblock_t sb;
    assert(openfs_read_superblock(&v,&sb)==OPENFS_FORMAT_OK);
    uint64_t ino=0U;
    assert(openfs_path_create(&v,&sb,"/flush-unlink",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);
    uint64_t count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t root;
    assert(openfs_inode_read(&v,sb.inode_table_start,sb.root_inode,count,&root)==OPENFS_INODE_OK);

    d.fail_flush=1;
    d.fail_flush_once=1;
    assert(openfs_dir_remove(&v,&sb,&root,"flush-unlink")==OPENFS_DIR_IO_ERROR);

    openfs_dir_entry_t found={0};
    assert(openfs_dir_lookup(&v,&sb,&root,"flush-unlink",&found)==OPENFS_DIR_OK);
    assert(found.inode_number==ino);
    uint64_t errors=0U;
    assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK);
    assert(errors==0U);
    free(d.bytes);
}

int main(void){
    unlink_flush_failure_rolls_back();
 disk_t d={.block_size=4096U,.block_count=128U};d.bytes=calloc((size_t)d.block_count,d.block_size);assert(d.bytes);
 openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};uint8_t uuid[16]={0};assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
 openfs_superblock_t sb;assert(openfs_read_superblock(&v,&sb)==OPENFS_FORMAT_OK);
 uint64_t n=0U;assert(openfs_inode_alloc(&v,&sb,1U,OPENFS_INODE_MODE_REGULAR,&n)==OPENFS_INODE_ALLOC_OK);assert(n==2U);
 openfs_inode_t root;uint64_t count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
 assert(openfs_inode_read(&v,sb.inode_table_start,1U,count,&root)==OPENFS_INODE_OK);
 openfs_dir_entry_t entry={2U,1U,1U};assert(openfs_dir_add(&v,&sb,&root,"hello",&entry)==OPENFS_DIR_OK);
 openfs_dir_entry_t found={0};assert(openfs_dir_lookup(&v,&sb,&root,"hello",&found)==OPENFS_DIR_OK);assert(found.inode_number==2U);
 assert(openfs_dir_add(&v,&sb,&root,"hello",&entry)==OPENFS_DIR_EXISTS);
 assert(openfs_dir_remove(&v,&sb,&root,"hello")==OPENFS_DIR_OK);
 assert(openfs_dir_lookup(&v,&sb,&root,"hello",&found)==OPENFS_DIR_NOT_FOUND);
 openfs_inode_t child;assert(openfs_inode_read(&v,sb.inode_table_start,2U,count,&child)==OPENFS_INODE_OK);
 child.link_count=0U;child.mode=OPENFS_INODE_MODE_FREE;assert(openfs_inode_write(&v,sb.inode_table_start,count,&child)==OPENFS_INODE_OK);
 assert(openfs_inode_free(&v,&sb,2U)==OPENFS_INODE_ALLOC_OK);
 free(d.bytes);return 0;
}
