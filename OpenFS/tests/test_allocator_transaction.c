#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/allocator.h"
#include "openfs/cow.h"
#include "openfs/format.h"
#include "openfs/mount.h"
#include "openfs/bitmap.h"
#include "openfs/file.h"
#include "openfs/inode_alloc.h"
#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

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


#define ALLOC_RACE_THREADS 4U
#define ALLOC_RACE_PER_THREAD 4U

typedef struct {
    openfs_block_device_t *device;
    openfs_superblock_t *superblock;
    openfs_alloc_result_t results[ALLOC_RACE_PER_THREAD];
    uint64_t blocks[ALLOC_RACE_PER_THREAD];
} allocation_race_context_t;

#if defined(_WIN32)
static unsigned __stdcall allocation_race_worker(void *arg)
#else
static void *allocation_race_worker(void *arg)
#endif
{
    allocation_race_context_t *ctx=(allocation_race_context_t *)arg;
    for(unsigned i=0U;i<ALLOC_RACE_PER_THREAD;i++) {
        ctx->blocks[i]=UINT64_MAX;
        ctx->results[i]=openfs_alloc_block(ctx->device,ctx->superblock,&ctx->blocks[i]);
    }
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

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
    /* Transaction-aware allocation must not create a nested WAL transaction. */
    openfs_journal_t tx_journal;
    assert(openfs_journal_open(&tx_journal,&dev,&mount.superblock)==OPENFS_JOURNAL_OK);
    openfs_transaction_t tx;
    assert(openfs_transaction_begin(&tx,&dev,&tx_journal)==OPENFS_TRANSACTION_OK);
    uint64_t tx_block=0U;
    assert(openfs_alloc_block_tx(&tx,&mount.superblock,&tx_block)==OPENFS_ALLOC_OK);
    if((mount.superblock.feature_flags&OPENFS_FEATURE_COW)!=0U){uint16_t refs=99U;assert(openfs_cow_refcount_get(&dev,&mount.superblock,tx_block,&refs)==OPENFS_COW_OK&&refs==0U);}
    int tx_used=0;
    assert(openfs_bitmap_test(&dev,mount.superblock.block_bitmap_start,mount.superblock.block_bitmap_blocks,tx_block,&tx_used)==OPENFS_BITMAP_OK&&tx_used==0);
    assert(openfs_transaction_abort(&tx)==OPENFS_TRANSACTION_OK);
    assert(openfs_bitmap_test(&dev,mount.superblock.block_bitmap_start,mount.superblock.block_bitmap_blocks,tx_block,&tx_used)==OPENFS_BITMAP_OK&&tx_used==0);
    assert(openfs_transaction_begin(&tx,&dev,&tx_journal)==OPENFS_TRANSACTION_OK);
    assert(openfs_alloc_block_tx(&tx,&mount.superblock,&tx_block)==OPENFS_ALLOC_OK);
    assert(openfs_transaction_commit(&tx)==OPENFS_TRANSACTION_OK);
    if((mount.superblock.feature_flags&OPENFS_FEATURE_COW)!=0U){uint16_t refs=0U;assert(openfs_cow_refcount_get(&dev,&mount.superblock,tx_block,&refs)==OPENFS_COW_OK&&refs==1U);}
    assert(openfs_bitmap_test(&dev,mount.superblock.block_bitmap_start,mount.superblock.block_bitmap_blocks,tx_block,&tx_used)==OPENFS_BITMAP_OK&&tx_used==1);
    if((mount.superblock.feature_flags&OPENFS_FEATURE_COW)!=0U){
        /* A free bitmap bit must never let a stale shared refcount be silently decremented. */
        assert(openfs_cow_refcount_set(&dev,&mount.superblock,tx_block,2U)==OPENFS_COW_OK);
        assert(openfs_bitmap_set(&dev,mount.superblock.block_bitmap_start,mount.superblock.block_bitmap_blocks,tx_block,0)==OPENFS_BITMAP_OK);
        assert(openfs_transaction_begin(&tx,&dev,&tx_journal)==OPENFS_TRANSACTION_OK);
        uint16_t stale_refs=0U;
        assert(openfs_cow_refcount_inc_tx(&tx,&mount.superblock,tx_block,&stale_refs)==OPENFS_COW_CORRUPT);
        assert(openfs_cow_refcount_dec_tx(&tx,&mount.superblock,tx_block,&stale_refs)==OPENFS_COW_CORRUPT);
        assert(openfs_free_block_tx(&tx,&mount.superblock,tx_block)==OPENFS_ALLOC_CORRUPT);
        assert(openfs_transaction_abort(&tx)==OPENFS_TRANSACTION_OK);
        uint16_t refs=0U;
        assert(openfs_cow_refcount_get(&dev,&mount.superblock,tx_block,&refs)==OPENFS_COW_OK&&refs==2U);
        assert(openfs_bitmap_test(&dev,mount.superblock.block_bitmap_start,mount.superblock.block_bitmap_blocks,tx_block,&tx_used)==OPENFS_BITMAP_OK&&tx_used==0);
        assert(openfs_bitmap_set(&dev,mount.superblock.block_bitmap_start,mount.superblock.block_bitmap_blocks,tx_block,1)==OPENFS_BITMAP_OK);
        assert(openfs_cow_refcount_set(&dev,&mount.superblock,tx_block,1U)==OPENFS_COW_OK);
    }
    assert(openfs_transaction_begin(&tx,&dev,&tx_journal)==OPENFS_TRANSACTION_OK);
    assert(openfs_free_block_tx(&tx,&mount.superblock,tx_block)==OPENFS_ALLOC_OK);
    assert(openfs_transaction_commit(&tx)==OPENFS_TRANSACTION_OK);
    if((mount.superblock.feature_flags&OPENFS_FEATURE_COW)!=0U){uint16_t refs=99U;assert(openfs_cow_refcount_get(&dev,&mount.superblock,tx_block,&refs)==OPENFS_COW_OK&&refs==0U);}
    assert(openfs_bitmap_test(&dev,mount.superblock.block_bitmap_start,mount.superblock.block_bitmap_blocks,tx_block,&tx_used)==OPENFS_BITMAP_OK&&tx_used==0);

    /* Contend the runtime allocation + transaction locks from real threads.
     * Every successful reservation must be unique and safely releasable. */
    allocation_race_context_t allocation_contexts[ALLOC_RACE_THREADS];
#if defined(_WIN32)
    HANDLE allocation_threads[ALLOC_RACE_THREADS];
    for(unsigned i=0U;i<ALLOC_RACE_THREADS;i++) {
        memset(&allocation_contexts[i],0,sizeof(allocation_contexts[i]));
        allocation_contexts[i].device=&dev;
        allocation_contexts[i].superblock=&mount.superblock;
        uintptr_t thread=_beginthreadex(NULL,0U,allocation_race_worker,&allocation_contexts[i],0U,NULL);
        assert(thread!=0U);
        allocation_threads[i]=(HANDLE)thread;
    }
    assert(WaitForMultipleObjects(ALLOC_RACE_THREADS,allocation_threads,TRUE,60000U)==WAIT_OBJECT_0);
    for(unsigned i=0U;i<ALLOC_RACE_THREADS;i++)CloseHandle(allocation_threads[i]);
#else
    pthread_t allocation_threads[ALLOC_RACE_THREADS];
    for(unsigned i=0U;i<ALLOC_RACE_THREADS;i++) {
        memset(&allocation_contexts[i],0,sizeof(allocation_contexts[i]));
        allocation_contexts[i].device=&dev;
        allocation_contexts[i].superblock=&mount.superblock;
        assert(pthread_create(&allocation_threads[i],NULL,allocation_race_worker,&allocation_contexts[i])==0);
    }
    for(unsigned i=0U;i<ALLOC_RACE_THREADS;i++)assert(pthread_join(allocation_threads[i],NULL)==0);
#endif
    for(unsigned t=0U;t<ALLOC_RACE_THREADS;t++) {
        for(unsigned i=0U;i<ALLOC_RACE_PER_THREAD;i++) {
            assert(allocation_contexts[t].results[i]==OPENFS_ALLOC_OK);
            assert(allocation_contexts[t].blocks[i]>=mount.superblock.data_start);
            assert(allocation_contexts[t].blocks[i]<mount.superblock.data_start+mount.superblock.data_blocks);
            for(unsigned pt=0U;pt<=t;pt++) {
                unsigned limit=pt==t?i:ALLOC_RACE_PER_THREAD;
                for(unsigned pi=0U;pi<limit;pi++)
                    assert(allocation_contexts[t].blocks[i]!=allocation_contexts[pt].blocks[pi]);
            }
        }
    }
    for(unsigned t=0U;t<ALLOC_RACE_THREADS;t++)
        for(unsigned i=0U;i<ALLOC_RACE_PER_THREAD;i++)
            assert(openfs_free_block(&dev,&mount.superblock,allocation_contexts[t].blocks[i])==OPENFS_ALLOC_OK);

    assert(openfs_unmount(&mount)==OPENFS_MOUNT_OK);
    free(disk.bytes);
    return 0;
}
