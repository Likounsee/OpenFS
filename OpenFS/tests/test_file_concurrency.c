#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "openfs/format.h"
#include "openfs/mount.h"
#include "openfs/path.h"
#include "openfs/file.h"
#include "openfs/fsck.h"
#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

#define WORKERS 8U
#define ROUNDS 8U
#define BLOCK_SIZE 4096U

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
    openfs_inode_t inode;
    unsigned id;
    unsigned failures;
} worker_context_t;

#if defined(_WIN32)
static unsigned __stdcall file_worker(void *arg)
#else
static void *file_worker(void *arg)
#endif
{
    worker_context_t *ctx=(worker_context_t *)arg;
    uint8_t block[BLOCK_SIZE];
    for(unsigned round=0U;round<ROUNDS;round++){
        memset(block,(int)(0x40U+ctx->id),sizeof(block));
        openfs_file_result_t write_result=openfs_file_write(ctx->device,ctx->superblock,&ctx->inode,
                             ((uint64_t)ctx->id*(uint64_t)BLOCK_SIZE),block,sizeof(block));
        if(write_result!=OPENFS_FILE_OK){
            fprintf(stderr,"worker %u round %u write result=%d\\n",ctx->id,round,(int)write_result);
            ctx->failures++;
            continue;
        }
        uint8_t readback[BLOCK_SIZE];
        size_t got=0U;
        openfs_file_result_t read_result=openfs_file_read(ctx->device,ctx->superblock,&ctx->inode,
                            ((uint64_t)ctx->id*(uint64_t)BLOCK_SIZE),readback,sizeof(readback),&got);
        if(read_result!=OPENFS_FILE_OK || got!=sizeof(readback) || memcmp(readback,block,sizeof(block))!=0){
            fprintf(stderr,"worker %u round %u read result=%d got=%zu match=%d\\n",ctx->id,round,(int)read_result,got,got==sizeof(readback)&&memcmp(readback,block,sizeof(block))==0);
            ctx->failures++;
        }
    }
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

int main(void)
{
    disk_t disk={.block_size=BLOCK_SIZE,.block_count=2048U};
    disk.bytes=calloc((size_t)disk.block_count,disk.block_size);
    assert(disk.bytes!=NULL);

    openfs_block_device_t device={&disk,disk.block_size,disk.block_count,read_blocks,write_blocks,flush_blocks};
    uint8_t uuid[16]={0xA4U,0x17U};
    assert(openfs_format(&device,uuid)==OPENFS_FORMAT_OK);

    openfs_mount_t mount;
    assert(openfs_mount(&mount,&device)==OPENFS_MOUNT_OK);
    uint64_t inode_number=0U;
    assert(openfs_path_create(&device,&mount.superblock,"/file-race",OPENFS_INODE_MODE_REGULAR|0644U,&inode_number)==OPENFS_PATH_OK);

    uint64_t inode_count=(mount.superblock.inode_table_blocks*(uint64_t)mount.superblock.block_size)/OPENFS_INODE_SIZE;
    worker_context_t contexts[WORKERS];
    for(unsigned i=0U;i<WORKERS;i++){
        contexts[i].device=&device;
        contexts[i].superblock=&mount.superblock;
        contexts[i].id=i;
        contexts[i].failures=0U;
        assert(openfs_inode_read(&device,mount.superblock.inode_table_start,inode_number,inode_count,&contexts[i].inode)==OPENFS_INODE_OK);
    }

#if defined(_WIN32)
    HANDLE threads[WORKERS];
    for(unsigned i=0U;i<WORKERS;i++){
        uintptr_t thread=_beginthreadex(NULL,0U,file_worker,&contexts[i],0U,NULL);
        assert(thread!=0U);
        threads[i]=(HANDLE)thread;
    }
    assert(WaitForMultipleObjects(WORKERS,threads,TRUE,60000U)==WAIT_OBJECT_0);
    for(unsigned i=0U;i<WORKERS;i++)CloseHandle(threads[i]);
#else
    pthread_t threads[WORKERS];
    for(unsigned i=0U;i<WORKERS;i++)assert(pthread_create(&threads[i],NULL,file_worker,&contexts[i])==0);
    for(unsigned i=0U;i<WORKERS;i++)assert(pthread_join(threads[i],NULL)==0);
#endif

    for(unsigned i=0U;i<WORKERS;i++)assert(contexts[i].failures==0U);

    openfs_inode_t final_inode;
    assert(openfs_inode_read(&device,mount.superblock.inode_table_start,inode_number,inode_count,&final_inode)==OPENFS_INODE_OK);
    assert(final_inode.size==(uint64_t)WORKERS*BLOCK_SIZE);

    uint8_t block[BLOCK_SIZE];
    uint8_t expected[BLOCK_SIZE];
    size_t got=0U;
    for(unsigned i=0U;i<WORKERS;i++){
        memset(expected,(int)(0x40U+i),sizeof(expected));
        memset(block,0U,sizeof(block));
        assert(openfs_file_read(&device,&mount.superblock,&final_inode,
                                (uint64_t)i*BLOCK_SIZE,block,sizeof(block),&got)==OPENFS_FILE_OK);
        assert(got==sizeof(block));
        assert(memcmp(block,expected,sizeof(block))==0);
    }

    uint64_t errors=0U;
    assert(openfs_fsck(&device,&mount.superblock,&errors)==OPENFS_FSCK_OK);
    assert(errors==0U);
    assert(openfs_unmount(&mount)==OPENFS_MOUNT_OK);
    free(disk.bytes);
    return 0;
}
