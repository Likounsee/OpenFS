#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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

#define WORKERS 8U

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
    openfs_path_result_t result;
} worker_context_t;

#if defined(_WIN32)
static unsigned __stdcall create_worker(void *arg)
#else
static void *create_worker(void *arg)
#endif
{
    worker_context_t *ctx=(worker_context_t *)arg;
    char path[96];
    uint64_t inode=0U;
    (void)snprintf(path,sizeof(path),"/collision");
    ctx->result=openfs_path_create(ctx->device,ctx->superblock,path,
                                   OPENFS_INODE_MODE_REGULAR|0644U,&inode);
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

#if defined(_WIN32)
static unsigned __stdcall rename_worker(void *arg)
#else
static void *rename_worker(void *arg)
#endif
{
    worker_context_t *ctx=(worker_context_t *)arg;
    char source[96];
    (void)snprintf(source,sizeof(source),"/rename-source-%u",ctx->id);
    ctx->result=openfs_path_rename(ctx->device,ctx->superblock,source,"/rename-destination");
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

static int run_create_collision(openfs_block_device_t *device,openfs_superblock_t *sb)
{
    worker_context_t contexts[WORKERS];
#if defined(_WIN32)
    HANDLE threads[WORKERS];
    for(unsigned i=0U;i<WORKERS;i++){
        contexts[i]=(worker_context_t){device,sb,i,OPENFS_PATH_IO_ERROR};
        uintptr_t thread=_beginthreadex(NULL,0U,create_worker,&contexts[i],0U,NULL);
        assert(thread!=0U);
        threads[i]=(HANDLE)thread;
    }
    assert(WaitForMultipleObjects(WORKERS,threads,TRUE,60000U)==WAIT_OBJECT_0);
    for(unsigned i=0U;i<WORKERS;i++)CloseHandle(threads[i]);
#else
    pthread_t threads[WORKERS];
    for(unsigned i=0U;i<WORKERS;i++){
        contexts[i]=(worker_context_t){device,sb,i,OPENFS_PATH_IO_ERROR};
        assert(pthread_create(&threads[i],NULL,create_worker,&contexts[i])==0);
    }
    for(unsigned i=0U;i<WORKERS;i++)assert(pthread_join(threads[i],NULL)==0);
#endif

    unsigned successes=0U,exists=0U;
    for(unsigned i=0U;i<WORKERS;i++){
        if(contexts[i].result==OPENFS_PATH_OK)successes++;
        else if(contexts[i].result==OPENFS_PATH_EXISTS)exists++;
        else return 0;
    }
    if(successes!=1U||exists!=WORKERS-1U)return 0;

    uint64_t inode=0U;
    if(openfs_path_lookup(device,sb,"/collision",&inode)!=OPENFS_PATH_OK||inode==0U)return 0;
    if(openfs_path_unlink(device,sb,"/collision")!=OPENFS_PATH_OK)return 0;
    return openfs_path_lookup(device,sb,"/collision",&inode)==OPENFS_PATH_NOT_FOUND;
}

static int run_rename_collision(openfs_block_device_t *device,openfs_superblock_t *sb)
{
    for(unsigned i=0U;i<WORKERS;i++){
        char source[96];
        uint64_t inode=0U;
        (void)snprintf(source,sizeof(source),"/rename-source-%u",i);
        if(openfs_path_create(device,sb,source,OPENFS_INODE_MODE_REGULAR|0644U,&inode)!=OPENFS_PATH_OK)return 0;
    }

    worker_context_t contexts[WORKERS];
#if defined(_WIN32)
    HANDLE threads[WORKERS];
    for(unsigned i=0U;i<WORKERS;i++){
        contexts[i]=(worker_context_t){device,sb,i,OPENFS_PATH_IO_ERROR};
        uintptr_t thread=_beginthreadex(NULL,0U,rename_worker,&contexts[i],0U,NULL);
        assert(thread!=0U);
        threads[i]=(HANDLE)thread;
    }
    assert(WaitForMultipleObjects(WORKERS,threads,TRUE,60000U)==WAIT_OBJECT_0);
    for(unsigned i=0U;i<WORKERS;i++)CloseHandle(threads[i]);
#else
    pthread_t threads[WORKERS];
    for(unsigned i=0U;i<WORKERS;i++){
        contexts[i]=(worker_context_t){device,sb,i,OPENFS_PATH_IO_ERROR};
        assert(pthread_create(&threads[i],NULL,rename_worker,&contexts[i])==0);
    }
    for(unsigned i=0U;i<WORKERS;i++)assert(pthread_join(threads[i],NULL)==0);
#endif

    for(unsigned i=0U;i<WORKERS;i++){
        if(contexts[i].result!=OPENFS_PATH_OK)return 0;
    }

    uint64_t destination=0U;
    if(openfs_path_lookup(device,sb,"/rename-destination",&destination)!=OPENFS_PATH_OK)return 0;

    for(unsigned i=0U;i<WORKERS;i++){
        char source[96];
        uint64_t inode=0U;
        (void)snprintf(source,sizeof(source),"/rename-source-%u",i);
        if(openfs_path_lookup(device,sb,source,&inode)!=OPENFS_PATH_NOT_FOUND)return 0;
    }

    return openfs_path_unlink(device,sb,"/rename-destination")==OPENFS_PATH_OK;
}

int main(void)
{
    disk_t disk={.block_size=4096U,.block_count=1024U};
    disk.bytes=calloc((size_t)disk.block_count,disk.block_size);
    assert(disk.bytes!=NULL);

    openfs_block_device_t device={&disk,disk.block_size,disk.block_count,read_blocks,write_blocks,flush_blocks};
    uint8_t uuid[16]={0x91U,0xC3U};
    assert(openfs_format(&device,uuid)==OPENFS_FORMAT_OK);

    openfs_mount_t mount;
    assert(openfs_mount(&mount,&device)==OPENFS_MOUNT_OK);
    assert(run_create_collision(&device,&mount.superblock));
    assert(run_rename_collision(&device,&mount.superblock));

    uint64_t errors=0U;
    assert(openfs_fsck(&device,&mount.superblock,&errors)==OPENFS_FSCK_OK);
    assert(errors==0U);
    assert(openfs_unmount(&mount)==OPENFS_MOUNT_OK);
    free(disk.bytes);
    return 0;
}
