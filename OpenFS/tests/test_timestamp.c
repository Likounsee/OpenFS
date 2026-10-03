#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/format.h"
#include "openfs/path.h"
#include "openfs/file.h"
#include "openfs/time.h"

typedef struct { uint8_t *bytes; uint32_t block_size; uint64_t block_count; } disk_t;
static openfs_io_result_t rd(void *c,uint64_t f,uint32_t n,void *b){disk_t*d=c;if(n==0U||f>=d->block_count||(uint64_t)n>d->block_count-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(b,d->bytes+(size_t)(f*d->block_size),(size_t)((uint64_t)n*d->block_size));return OPENFS_IO_OK;}
static openfs_io_result_t wr(void *c,uint64_t f,uint32_t n,const void *b){disk_t*d=c;if(n==0U||f>=d->block_count||(uint64_t)n>d->block_count-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->bytes+(size_t)(f*d->block_size),b,(size_t)((uint64_t)n*d->block_size));return OPENFS_IO_OK;}
static openfs_io_result_t fl(void *c){(void)c;return OPENFS_IO_OK;}

int main(void)
{
    disk_t d={0};d.block_size=4096U;d.block_count=256U;d.bytes=calloc((size_t)d.block_count,d.block_size);assert(d.bytes);
    openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};uint8_t uuid[16]={0};
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    uint64_t ic=(s.inode_table_blocks*(uint64_t)s.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t root;assert(openfs_inode_read(&v,s.inode_table_start,s.root_inode,ic,&root)==OPENFS_INODE_OK);
    assert(root.atime_ns!=0U&&root.mtime_ns!=0U&&root.ctime_ns!=0U);
    assert(openfs_inode_check_access(&root,0U,0U,7U)==OPENFS_INODE_OK);
    uint64_t ino=0U;assert(openfs_path_create_as(&v,&s,"/ts",OPENFS_INODE_MODE_REGULAR|0600U,1000U,1000U,&ino)==OPENFS_PATH_OK);
    openfs_inode_t file;assert(openfs_inode_read(&v,s.inode_table_start,ino,ic,&file)==OPENFS_INODE_OK);
    assert(file.uid==1000U&&file.gid==1000U&&file.ctime_ns!=0U&&file.mtime_ns!=0U);
    uint64_t old_mtime=file.mtime_ns,old_ctime=file.ctime_ns;
    const char data[]="timestamp";
    assert(openfs_file_write(&v,&s,&file,0U,data,sizeof(data))==OPENFS_FILE_OK);
    assert(file.mtime_ns>=old_mtime&&file.ctime_ns>=old_ctime);
    old_ctime=file.ctime_ns;
    assert(openfs_path_chmod_as(&v,&s,"/ts",0640U,0U,0U)==OPENFS_PATH_OK);
    assert(openfs_inode_read(&v,s.inode_table_start,ino,ic,&file)==OPENFS_INODE_OK);
    assert((file.mode&OPENFS_INODE_PERMISSION_MASK)==0640U&&file.ctime_ns>=old_ctime);
    old_ctime=file.ctime_ns;
    assert(openfs_path_set_times_as(&v,&s,"/ts",0U,0U,111U,222U)==OPENFS_PATH_OK);
    assert(openfs_inode_read(&v,s.inode_table_start,ino,ic,&file)==OPENFS_INODE_OK);
    assert(file.atime_ns==111U&&file.mtime_ns==222U&&file.ctime_ns>=old_ctime);
    assert(openfs_time_now_ns()!=UINT64_MAX);
    free(d.bytes);return 0;
}
