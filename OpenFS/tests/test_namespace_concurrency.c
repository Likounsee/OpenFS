#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "openfs/format.h"
#include "openfs/mount.h"
#include "openfs/path.h"
#include "openfs/fsck.h"
#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

typedef struct {
    uint8_t *bytes;
    uint32_t block_size;
    uint64_t block_count;
} disk_t;

static openfs_io_result_t read_blocks(void *ctx,uint64_t first,uint32_t count,void *buffer)
{
    disk_t *d=(disk_t *)ctx;
    if(count==0U||first>=d->block_count||(uint64_t)count>d->block_count-first)return OPENFS_IO_OUT_OF_RANGE;
    memcpy(buffer,d->bytes+(size_t)(first*d->block_size),(size_t)((uint64_t)count*d->block_size));
    return OPENFS_IO_OK;
}

static openfs_io_result_t write_blocks(void *ctx,uint64_t first,uint32_t count,const void *buffer)
{
    disk_t *d=(disk_t *)ctx;
    if(count==0U||first>=d->block_count||(uint64_t)count>d->block_count-first)return OPENFS_IO_OUT_OF_RANGE;
    memcpy(d->bytes+(size_t)(first*d->block_size),buffer,(size_t)((uint64_t)count*d->block_size));
    return OPENFS_IO_OK;
}

static openfs_io_result_t flush_blocks(void *ctx)
{
    (void)ctx;
    return OPENFS_IO_OK;
}

typedef struct {
    openfs_block_device_t *device;
    openfs_superblock_t *superblock;
    unsigned id;
    unsigned failures;
} worker_context_t;

#if defined(_WIN32)
static unsigned __stdcall namespace_worker(void *arg)
#else
static void *namespace_worker(void *arg)
#endif
{
    worker_context_t *ctx=(worker_context_t *)arg;
    for(unsigned n=0U;n<16U;n++){
        char first[96];
        char second[96];
        uint64_t inode=0U;
        (void)snprintf(first,sizeof(first),"/race-%u-%u",ctx->id,n);
        (void)snprintf(second,sizeof(second),"/race-renamed-%u-%u",ctx->id,n);
        if(openfs_path_create(ctx->device,ctx->superblock,first,OPENFS_INODE_MODE_REGULAR|0644U,&inode)!=OPENFS_PATH_OK){
            ctx->failures++;
            continue;
        }
        if(openfs_path_rename(ctx->device,ctx->superblock,first,second)!=OPENFS_PATH_OK)ctx->failures++;
        if(openfs_path_unlink(ctx->device,ctx->superblock,second)!=OPENFS_PATH_OK)ctx->failures++;
    }
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

int main(void)
{
    disk_t disk={.block_size=4096U,.block_count=512U};
    disk.bytes=calloc((size_t)disk.block_count,disk.block_size);
    assert(disk.bytes!=NULL);

    openfs_block_device_t device={&disk,disk.block_size,disk.block_count,read_blocks,write_blocks,flush_blocks};
    uint8_t uuid[16]={0x52U};
    assert(openfs_format(&device,uuid)==OPENFS_FORMAT_OK);

    openfs_mount_t mount;
    assert(openfs_mount(&mount,&device)==OPENFS_MOUNT_OK);

    worker_context_t contexts[4];
#if defined(_WIN32)
    HANDLE threads[4];
    for(unsigned i=0U;i<4U;i++){
        contexts[i]=(worker_context_t){&device,&mount.superblock,i,0U};
        uintptr_t thread=_beginthreadex(NULL,0U,namespace_worker,&contexts[i],0U,NULL);
        assert(thread!=0U);
        threads[i]=(HANDLE)thread;
    }
    assert(WaitForMultipleObjects(4U,threads,TRUE,60000U)==WAIT_OBJECT_0);
    for(unsigned i=0U;i<4U;i++)CloseHandle(threads[i]);
#else
    pthread_t threads[4];
    for(unsigned i=0U;i<4U;i++){
        contexts[i]=(worker_context_t){&device,&mount.superblock,i,0U};
        assert(pthread_create(&threads[i],NULL,namespace_worker,&contexts[i])==0);
    }
    for(unsigned i=0U;i<4U;i++)assert(pthread_join(threads[i],NULL)==0);
#endif

    for(unsigned i=0U;i<4U;i++)assert(contexts[i].failures==0U);

    for(unsigned i=0U;i<4U;i++)
        for(unsigned n=0U;n<16U;n++){
            char second[96];
            uint64_t inode=0U;
            (void)snprintf(second,sizeof(second),"/race-renamed-%u-%u",i,n);
            assert(openfs_path_lookup(&device,&mount.superblock,second,&inode)==OPENFS_PATH_NOT_FOUND);
        }

    uint64_t errors=0U;
    assert(openfs_fsck(&device,&mount.superblock,&errors)==OPENFS_FSCK_OK);
    assert(errors==0U);
    assert(openfs_unmount(&mount)==OPENFS_MOUNT_OK);
    free(disk.bytes);
    return 0;
}
