#include "openfs/file_lock.h"
#include "openfs/mount.h"
#include "openfs/format.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *data; uint32_t bs; uint64_t blocks; } disk_t;
static openfs_io_result_t rd(void *ctx,uint64_t first,uint32_t count,void *out){
    disk_t *d=ctx;if(first>=d->blocks||(uint64_t)count>d->blocks-first)return OPENFS_IO_OUT_OF_RANGE;
    memcpy(out,d->data+(size_t)(first*d->bs),(size_t)count*d->bs);return OPENFS_IO_OK;
}
static openfs_io_result_t wr(void *ctx,uint64_t first,uint32_t count,const void *in){
    disk_t *d=ctx;if(first>=d->blocks||(uint64_t)count>d->blocks-first)return OPENFS_IO_OUT_OF_RANGE;
    memcpy(d->data+(size_t)(first*d->bs),in,(size_t)count*d->bs);return OPENFS_IO_OK;
}
static openfs_io_result_t fl(void *ctx){(void)ctx;return OPENFS_IO_OK;}

int main(void){
    disk_t d={0};d.bs=4096U;d.blocks=256U;d.data=calloc((size_t)d.blocks,d.bs);assert(d.data);
    openfs_block_device_t dev={&d,d.bs,d.blocks,rd,wr,fl};
    uint8_t uuid[16]={0};openfs_superblock_t sb;
    assert(openfs_format(&dev,uuid)==OPENFS_FORMAT_OK);
    assert(openfs_read_superblock(&dev,&sb)==OPENFS_FORMAT_OK);

    openfs_mount_t m;assert(openfs_mount(&m,&dev)==OPENFS_MOUNT_OK);
    openfs_file_handle_t *a=NULL,*b=NULL,*dup=NULL;
    assert(openfs_fd_open(&dev,&m.superblock,"/lock-test",OPENFS_FD_CREAT|OPENFS_FD_RDWR,OPENFS_INODE_MODE_REGULAR|0644U,&a)==OPENFS_FD_OK);
    assert(openfs_fd_open(&dev,&m.superblock,"/lock-test",OPENFS_FD_RDWR,0U,&b)==OPENFS_FD_OK);

    assert(openfs_file_lock(a,0U,100U,OPENFS_FILE_LOCK_SHARED,0U)==OPENFS_FILE_LOCK_OK);
    assert(openfs_file_lock(b,50U,10U,OPENFS_FILE_LOCK_SHARED,0U)==OPENFS_FILE_LOCK_OK);
    assert(openfs_file_lock(b,50U,10U,OPENFS_FILE_LOCK_EXCLUSIVE,0U)==OPENFS_FILE_LOCK_CONFLICT);
    uint32_t conflict=0U;
    assert(openfs_file_lock_test(b,50U,10U,&conflict)==OPENFS_FILE_LOCK_CONFLICT);
    assert(conflict==OPENFS_FILE_LOCK_SHARED);
    assert(openfs_file_lock(b,200U,10U,OPENFS_FILE_LOCK_EXCLUSIVE,0U)==OPENFS_FILE_LOCK_OK);

    assert(openfs_file_unlock(a,0U,100U)==OPENFS_FILE_LOCK_OK);
    assert(openfs_file_unlock(b,50U,10U)==OPENFS_FILE_LOCK_OK);
    assert(openfs_file_lock(b,50U,10U,OPENFS_FILE_LOCK_EXCLUSIVE,0U)==OPENFS_FILE_LOCK_OK);
    assert(openfs_file_unlock(b,50U,10U)==OPENFS_FILE_LOCK_OK);
    assert(openfs_file_unlock(b,50U,10U)==OPENFS_FILE_LOCK_NOT_FOUND);

    assert(openfs_fd_dup(b,&dup)==OPENFS_FD_OK&&dup==b);
    assert(openfs_file_lock(b,300U,20U,OPENFS_FILE_LOCK_EXCLUSIVE,0U)==OPENFS_FILE_LOCK_OK);
    assert(openfs_fd_close(b)==OPENFS_FD_OK);
    assert(openfs_file_lock_test(dup,300U,20U,&conflict)==OPENFS_FILE_LOCK_OK);
    assert(openfs_fd_close(dup)==OPENFS_FD_OK);
    assert(openfs_file_lock(a,300U,20U,OPENFS_FILE_LOCK_EXCLUSIVE,0U)==OPENFS_FILE_LOCK_OK);
    assert(openfs_file_unlock(a,300U,20U)==OPENFS_FILE_LOCK_OK);
    assert(openfs_fd_close(a)==OPENFS_FD_OK);
    assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
    free(d.data);
    puts("file lock test passed");
    return 0;
}
