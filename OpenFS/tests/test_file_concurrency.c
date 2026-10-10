#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

#undef assert
#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed: %s (%s:%d)\\n", #condition, __FILE__, __LINE__); \
        abort(); \
    } \
} while (0)
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
#include <sched.h>
#endif

#define WORKERS 8U
#define ROUNDS 8U
#define BLOCK_SIZE 4096U

typedef struct {
    uint8_t *bytes;
    uint32_t block_size;
    uint64_t block_count;
    atomic_int gate_enabled;
    atomic_int gate_entered;
    atomic_int gate_release;
} disk_t;

static openfs_io_result_t read_blocks(void *ctx,uint64_t first,uint32_t count,void *buffer)
{
    disk_t *d=(disk_t *)ctx;
    if(count==0U||first>=d->block_count||(uint64_t)count>d->block_count-first)return OPENFS_IO_OUT_OF_RANGE;
    if(atomic_load_explicit(&d->gate_enabled,memory_order_acquire)!=0 &&
       atomic_exchange_explicit(&d->gate_entered,1,memory_order_acq_rel)==0){
        while(atomic_load_explicit(&d->gate_release,memory_order_acquire)==0){
#if defined(_WIN32)
            Sleep(0);
#else
            sched_yield();
#endif
        }
    }
    memcpy(buffer,d->bytes+(size_t)(first*d->block_size),(size_t)((uint64_t)count*d->block_size));
    return OPENFS_IO_OK;
}

static openfs_io_result_t write_blocks(void *ctx,uint64_t first,uint32_t count,const void *buffer)
{
    disk_t *d=(disk_t *)ctx;
    if(count==0U||first>=d->block_count||(uint64_t)count>d->block_count-first)return OPENFS_IO_OUT_OF_RANGE;
    if(atomic_load_explicit(&d->gate_enabled,memory_order_acquire)!=0 &&
       atomic_exchange_explicit(&d->gate_entered,1,memory_order_acq_rel)==0){
        while(atomic_load_explicit(&d->gate_release,memory_order_acquire)==0){
#if defined(_WIN32)
            Sleep(0);
#else
            sched_yield();
#endif
        }
    }
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
    size_t bytes_read;
    openfs_file_result_t result;
} gated_read_context_t;

typedef struct {
    openfs_mount_t *mount;
    atomic_int done;
    openfs_mount_result_t result;
} unmount_context_t;

typedef struct {
    openfs_block_device_t *device;
    openfs_superblock_t *superblock;
    openfs_inode_t inode;
    openfs_file_result_t result;
} gated_write_context_t;


#if defined(_WIN32)
static unsigned __stdcall gated_file_reader(void *arg)
#else
static void *gated_file_reader(void *arg)
#endif
{
    gated_read_context_t *ctx=(gated_read_context_t *)arg;
    uint8_t block[BLOCK_SIZE];
    ctx->result=openfs_file_read(ctx->device,ctx->superblock,&ctx->inode,0U,
                                 block,sizeof(block),&ctx->bytes_read);
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

#if defined(_WIN32)
static unsigned __stdcall gated_file_writer(void *arg)
#else
static void *gated_file_writer(void *arg)
#endif
{
    gated_write_context_t *ctx=(gated_write_context_t *)arg;
    uint8_t block[BLOCK_SIZE];
    memset(block,0xD7,sizeof(block));
    ctx->result=openfs_file_write(ctx->device,ctx->superblock,&ctx->inode,
                                  0U,block,sizeof(block));
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

#if defined(_WIN32)
static unsigned __stdcall unmount_worker(void *arg)
#else
static void *unmount_worker(void *arg)
#endif
{
    unmount_context_t *ctx=(unmount_context_t *)arg;
    ctx->result=openfs_unmount(ctx->mount);
    atomic_store_explicit(&ctx->done,1,memory_order_release);
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
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
        if(openfs_file_write(ctx->device,ctx->superblock,&ctx->inode,
                             ((uint64_t)ctx->id*(uint64_t)BLOCK_SIZE),block,sizeof(block))!=OPENFS_FILE_OK){
            ctx->failures++;
            continue;
        }
        uint8_t readback[BLOCK_SIZE];
        size_t got=0U;
        if(openfs_file_read(ctx->device,ctx->superblock,&ctx->inode,
                            ((uint64_t)ctx->id*(uint64_t)BLOCK_SIZE),readback,sizeof(readback),&got)!=OPENFS_FILE_OK ||
           got!=sizeof(readback) || memcmp(readback,block,sizeof(block))!=0)ctx->failures++;
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
    atomic_init(&disk.gate_enabled,0);
    atomic_init(&disk.gate_entered,0);
    atomic_init(&disk.gate_release,0);
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

    for(unsigned teardown_cycle=0U;teardown_cycle<100U;teardown_cycle++){
    /* Repeat the deterministic read/unmount race 100 times. Every cycle
     * proves admission closure, waits for the pinned read, remounts, and runs
     * FSCK before the next cycle begins. */
    gated_read_context_t gated={.device=&device,.superblock=&mount.superblock,
                                .inode=final_inode,.bytes_read=0U,
                                .result=OPENFS_FILE_INVALID_ARGUMENT};
    unmount_context_t unmount_ctx={.mount=&mount,.result=OPENFS_MOUNT_IO_ERROR};
    atomic_init(&unmount_ctx.done,0);
    atomic_store_explicit(&disk.gate_entered,0,memory_order_relaxed);
    atomic_store_explicit(&disk.gate_release,0,memory_order_relaxed);
    atomic_store_explicit(&disk.gate_enabled,1,memory_order_release);
#if defined(_WIN32)
    uintptr_t read_handle=_beginthreadex(NULL,0U,gated_file_reader,&gated,0U,NULL);
    assert(read_handle!=0U);
    HANDLE read_thread=(HANDLE)read_handle;
#else
    pthread_t read_thread;
    assert(pthread_create(&read_thread,NULL,gated_file_reader,&gated)==0);
#endif
    for(unsigned spin=0U;spin<10000000U &&
        atomic_load_explicit(&disk.gate_entered,memory_order_acquire)==0;spin++){
#if defined(_WIN32)
        Sleep(0);
#else
        sched_yield();
#endif
    }
    assert(atomic_load_explicit(&disk.gate_entered,memory_order_acquire)!=0);
#if defined(_WIN32)
    uintptr_t unmount_handle=_beginthreadex(NULL,0U,unmount_worker,&unmount_ctx,0U,NULL);
    assert(unmount_handle!=0U);
    HANDLE unmount_thread=(HANDLE)unmount_handle;
#else
    pthread_t unmount_thread;
    assert(pthread_create(&unmount_thread,NULL,unmount_worker,&unmount_ctx)==0);
#endif
    int stale_admission_accepted=1;
    for(unsigned spin=0U;spin<10000000U && stale_admission_accepted!=0;spin++){
        /* enter() checks the runtime registry before touching its lifecycle
         * mutex, so this remains safe even if unmount wins the race. */
        stale_admission_accepted=openfs_runtime_enter(&mount.runtime);
        if(stale_admission_accepted!=0)openfs_runtime_leave(&mount.runtime);
#if defined(_WIN32)
        Sleep(0);
#else
        sched_yield();
#endif
    }
    assert(stale_admission_accepted==0);
    assert(atomic_load_explicit(&unmount_ctx.done,memory_order_acquire)==0);
    atomic_store_explicit(&disk.gate_release,1,memory_order_release);
#if defined(_WIN32)
    assert(WaitForSingleObject(read_thread,60000U)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(unmount_thread,60000U)==WAIT_OBJECT_0);
    CloseHandle(read_thread);
    CloseHandle(unmount_thread);
#else
    assert(pthread_join(read_thread,NULL)==0);
    assert(pthread_join(unmount_thread,NULL)==0);
#endif
    atomic_store_explicit(&disk.gate_enabled,0,memory_order_release);
    assert(gated.result==OPENFS_FILE_OK && gated.bytes_read==BLOCK_SIZE);
    assert(unmount_ctx.result==OPENFS_MOUNT_OK);
    assert(atomic_load_explicit(&unmount_ctx.done,memory_order_acquire)!=0);

    /* A clean remount after each contested teardown verifies on-disk integrity. */
    assert(openfs_mount(&mount,&device)==OPENFS_MOUNT_OK);
    errors=UINT64_MAX;
    assert(openfs_fsck(&device,&mount.superblock,&errors)==OPENFS_FSCK_OK);
    assert(errors==0U);

    }

    /* Repeat the in-flight write/unmount barrier to exercise scheduling
     * variations; every iteration remounts and validates disk integrity. */
    for(unsigned write_cycle=0U;write_cycle<32U;write_cycle++){
    /* Also hold an in-flight write inside a block-device callback while
     * unmount closes admission. Teardown must wait for the writer's pin. */
    atomic_store_explicit(&disk.gate_entered,0,memory_order_release);
    atomic_store_explicit(&disk.gate_release,0,memory_order_release);
    atomic_store_explicit(&disk.gate_enabled,1,memory_order_release);
    gated_write_context_t gated_write={&device,&mount.superblock,final_inode,OPENFS_FILE_IO_ERROR};
#if defined(_WIN32)
    uintptr_t write_thread=_beginthreadex(NULL,0U,gated_file_writer,&gated_write,0U,NULL);
    assert(write_thread!=0U);
#else
    pthread_t write_thread;
    assert(pthread_create(&write_thread,NULL,gated_file_writer,&gated_write)==0);
#endif
    for(unsigned spin=0U;spin<10000000U && atomic_load_explicit(&disk.gate_entered,memory_order_acquire)==0;spin++){
#if defined(_WIN32)
        Sleep(0);
#else
        sched_yield();
#endif
    }
    assert(atomic_load_explicit(&disk.gate_entered,memory_order_acquire)!=0);
    unmount_context_t write_unmount={&mount,0,OPENFS_MOUNT_IO_ERROR};
#if defined(_WIN32)
    uintptr_t write_unmount_thread=_beginthreadex(NULL,0U,unmount_worker,&write_unmount,0U,NULL);
    assert(write_unmount_thread!=0U);
#else
    pthread_t write_unmount_thread;
    assert(pthread_create(&write_unmount_thread,NULL,unmount_worker,&write_unmount)==0);
#endif
    int write_admission_accepted=1;
    for(unsigned spin=0U;spin<10000000U && write_admission_accepted!=0;spin++){
        write_admission_accepted=openfs_runtime_enter(&mount.runtime);
        if(write_admission_accepted!=0)openfs_runtime_leave(&mount.runtime);
#if defined(_WIN32)
        Sleep(0);
#else
        sched_yield();
#endif
    }
    assert(write_admission_accepted==0);
    assert(atomic_load_explicit(&write_unmount.done,memory_order_acquire)==0);
    atomic_store_explicit(&disk.gate_release,1,memory_order_release);
#if defined(_WIN32)
    assert(WaitForSingleObject((HANDLE)write_thread,60000U)==WAIT_OBJECT_0);
    assert(WaitForSingleObject((HANDLE)write_unmount_thread,60000U)==WAIT_OBJECT_0);
    CloseHandle((HANDLE)write_thread);
    CloseHandle((HANDLE)write_unmount_thread);
#else
    assert(pthread_join(write_thread,NULL)==0);
    assert(pthread_join(write_unmount_thread,NULL)==0);
#endif
    atomic_store_explicit(&disk.gate_enabled,0,memory_order_release);
    assert(gated_write.result==OPENFS_FILE_OK);
    assert(write_unmount.result==OPENFS_MOUNT_OK);
    assert(atomic_load_explicit(&write_unmount.done,memory_order_acquire)!=0);
    assert(openfs_mount(&mount,&device)==OPENFS_MOUNT_OK);
    errors=UINT64_MAX;
    assert(openfs_fsck(&device,&mount.superblock,&errors)==OPENFS_FSCK_OK);
    assert(errors==0U);

    }

    assert(openfs_unmount(&mount)==OPENFS_MOUNT_OK);
    free(disk.bytes);
    return 0;
}
