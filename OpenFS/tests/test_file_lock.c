#define _POSIX_C_SOURCE 200809L
#include "openfs/file_lock.h"
#include "openfs/mount.h"
#include "openfs/format.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

/*
 * These regression tests must execute in Release builds too.  The standard
 * assert macro removes its expression when NDEBUG is defined, which would
 * otherwise skip formatting, mounting, thread creation, and the actual checks.
 */
#undef assert
#define assert(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "check failed: %s (%s:%d)\n", \
                #expression, __FILE__, __LINE__); \
        abort(); \
    } \
} while (0)

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

static atomic_int lock_race_start;
typedef struct { openfs_file_handle_t *handle; unsigned failures; } lock_race_context_t;
#if defined(_WIN32)
static unsigned __stdcall lock_race_worker(void *arg)
#else
static void *lock_race_worker(void *arg)
#endif
{
    lock_race_context_t *ctx=(lock_race_context_t *)arg;
    while(atomic_load_explicit(&lock_race_start,memory_order_acquire)==0){
#if defined(_WIN32)
        Sleep(0);
#else
        sched_yield();
#endif
    }
    for(unsigned i=0U;i<20000U;i++){
        uint32_t conflict=0U;
        if(openfs_file_lock_test(ctx->handle,300U,20U,&conflict)!=OPENFS_FILE_LOCK_OK)ctx->failures++;
    }
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

typedef struct { openfs_runtime_t *runtime; atomic_int started; int result; } shutdown_context_t;
#if defined(_WIN32)
static unsigned __stdcall shutdown_worker(void *arg)
#else
static void *shutdown_worker(void *arg)
#endif
{
    shutdown_context_t *ctx=(shutdown_context_t *)arg;
    atomic_store_explicit(&ctx->started,1,memory_order_release);
    ctx->result=openfs_runtime_shutdown_if_unused(ctx->runtime);
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

typedef struct { openfs_file_handle_t *handle; atomic_int started; openfs_file_lock_result_t result; } blocking_lock_context_t;
#if defined(_WIN32)
static unsigned __stdcall blocking_lock_worker(void *arg)
#else
static void *blocking_lock_worker(void *arg)
#endif
{
    blocking_lock_context_t *ctx=(blocking_lock_context_t *)arg;
    atomic_store_explicit(&ctx->started,1,memory_order_release);
    ctx->result=openfs_file_lock(ctx->handle,700U,25U,OPENFS_FILE_LOCK_EXCLUSIVE,OPENFS_FILE_LOCK_BLOCK);
    if(ctx->result==OPENFS_FILE_LOCK_OK){
        if(openfs_file_unlock(ctx->handle,700U,25U)!=OPENFS_FILE_LOCK_OK)ctx->result=OPENFS_FILE_LOCK_IO_ERROR;
    }
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

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

    /* Invalid requests must not publish a lock or mutate the registry. */
    assert(openfs_file_lock(a,0U,10U,0U,0U)==OPENFS_FILE_LOCK_INVALID_ARGUMENT);
    assert(openfs_file_lock(a,0U,10U,OPENFS_FILE_LOCK_SHARED,~OPENFS_FILE_LOCK_BLOCK)==OPENFS_FILE_LOCK_INVALID_ARGUMENT);
    uint32_t invalid_conflict=123U;
    assert(openfs_file_lock_test(a,0U,10U,NULL)==OPENFS_FILE_LOCK_INVALID_ARGUMENT);
    assert(openfs_file_lock_test(a,0U,10U,&invalid_conflict)==OPENFS_FILE_LOCK_OK);
    assert(invalid_conflict==0U);

    assert(openfs_file_lock(a,0U,100U,OPENFS_FILE_LOCK_SHARED,0U)==OPENFS_FILE_LOCK_OK);
    assert(openfs_file_lock(b,50U,10U,OPENFS_FILE_LOCK_SHARED,0U)==OPENFS_FILE_LOCK_OK);
    assert(openfs_file_lock(b,50U,10U,OPENFS_FILE_LOCK_EXCLUSIVE,0U)==OPENFS_FILE_LOCK_CONFLICT);
    uint32_t conflict=0U;
    assert(openfs_file_lock_test(b,50U,10U,&conflict)==OPENFS_FILE_LOCK_CONFLICT);
    assert(conflict==OPENFS_FILE_LOCK_SHARED);
    /* A successful query must clear a previously populated output value. */
    conflict=123U;
    assert(openfs_file_lock_test(b,500U,10U,&conflict)==OPENFS_FILE_LOCK_OK);
    assert(conflict==0U);
    assert(openfs_file_lock(b,200U,10U,OPENFS_FILE_LOCK_EXCLUSIVE,0U)==OPENFS_FILE_LOCK_OK);

    assert(openfs_file_unlock(a,0U,100U)==OPENFS_FILE_LOCK_OK);
    assert(openfs_file_unlock(b,50U,10U)==OPENFS_FILE_LOCK_OK);
    assert(openfs_file_lock(b,50U,10U,OPENFS_FILE_LOCK_EXCLUSIVE,0U)==OPENFS_FILE_LOCK_OK);
    assert(openfs_file_unlock(b,50U,10U)==OPENFS_FILE_LOCK_OK);
    assert(openfs_file_unlock(b,50U,10U)==OPENFS_FILE_LOCK_NOT_FOUND);

    /* A blocking lock must wait for a conflicting owner and then proceed. */
    assert(openfs_file_lock(a,700U,25U,OPENFS_FILE_LOCK_EXCLUSIVE,0U)==OPENFS_FILE_LOCK_OK);
    blocking_lock_context_t blocking={b,0,OPENFS_FILE_LOCK_IO_ERROR};
#if defined(_WIN32)
    uintptr_t blocking_thread=_beginthreadex(NULL,0U,blocking_lock_worker,&blocking,0U,NULL);
    assert(blocking_thread!=0U);
    while(atomic_load_explicit(&blocking.started,memory_order_acquire)==0)Sleep(0);
    assert(openfs_file_unlock(a,700U,25U)==OPENFS_FILE_LOCK_OK);
    assert(WaitForSingleObject((HANDLE)blocking_thread,60000U)==WAIT_OBJECT_0);
    CloseHandle((HANDLE)blocking_thread);
#else
    pthread_t blocking_thread;
    assert(pthread_create(&blocking_thread,NULL,blocking_lock_worker,&blocking)==0);
    while(atomic_load_explicit(&blocking.started,memory_order_acquire)==0)sched_yield();
    assert(openfs_file_unlock(a,700U,25U)==OPENFS_FILE_LOCK_OK);
    assert(pthread_join(blocking_thread,NULL)==0);
#endif
    assert(blocking.result==OPENFS_FILE_LOCK_OK);

    /*
     * Shutdown closes runtime admission before waiting for active operations.
     * A blocking lock waiter must notice that transition and leave its runtime
     * pin, otherwise shutdown cannot drain active users. Since this test still
     * has open handles, shutdown must then refuse teardown and reopen admission.
     */
    assert(openfs_file_lock(a,700U,25U,OPENFS_FILE_LOCK_EXCLUSIVE,0U)==OPENFS_FILE_LOCK_OK);
    blocking_lock_context_t shutdown_waiter={b,0,OPENFS_FILE_LOCK_IO_ERROR};
#if defined(_WIN32)
    uintptr_t shutdown_waiter_thread=_beginthreadex(NULL,0U,blocking_lock_worker,&shutdown_waiter,0U,NULL);
    assert(shutdown_waiter_thread!=0U);
    while(atomic_load_explicit(&shutdown_waiter.started,memory_order_acquire)==0)Sleep(0);
    Sleep(10);
    /* Keep shutdown in its drain phase until the test observes closed admission. */
    assert(openfs_runtime_enter(m.superblock.runtime)==1);
    shutdown_context_t shutdown={m.superblock.runtime,0,-1};
    uintptr_t shutdown_thread=_beginthreadex(NULL,0U,shutdown_worker,&shutdown,0U,NULL);
    assert(shutdown_thread!=0U);
    while(atomic_load_explicit(&shutdown.started,memory_order_acquire)==0)Sleep(0);
    while(openfs_runtime_is_accepting(m.superblock.runtime))Sleep(0);
    assert(WaitForSingleObject((HANDLE)shutdown_waiter_thread,60000U)==WAIT_OBJECT_0);
    openfs_runtime_leave(m.superblock.runtime);
    assert(WaitForSingleObject((HANDLE)shutdown_thread,60000U)==WAIT_OBJECT_0);
    CloseHandle((HANDLE)shutdown_waiter_thread);
    CloseHandle((HANDLE)shutdown_thread);
#else
    pthread_t shutdown_waiter_thread;
    assert(pthread_create(&shutdown_waiter_thread,NULL,blocking_lock_worker,&shutdown_waiter)==0);
    while(atomic_load_explicit(&shutdown_waiter.started,memory_order_acquire)==0)sched_yield();
    struct timespec delay={0,10000000L};
    (void)nanosleep(&delay,NULL);
    /* Keep shutdown in its drain phase until the test observes closed admission. */
    assert(openfs_runtime_enter(m.superblock.runtime)==1);
    shutdown_context_t shutdown={m.superblock.runtime,0,-1};
    pthread_t shutdown_thread;
    assert(pthread_create(&shutdown_thread,NULL,shutdown_worker,&shutdown)==0);
    while(atomic_load_explicit(&shutdown.started,memory_order_acquire)==0)sched_yield();
    while(openfs_runtime_is_accepting(m.superblock.runtime))sched_yield();
    assert(pthread_join(shutdown_waiter_thread,NULL)==0);
    openfs_runtime_leave(m.superblock.runtime);
    assert(pthread_join(shutdown_thread,NULL)==0);
#endif
    assert(shutdown_waiter.result==OPENFS_FILE_LOCK_CLOSED);
    assert(shutdown.result==0);
    assert(openfs_runtime_is_accepting(m.superblock.runtime)==1);
    assert(openfs_file_unlock(a,700U,25U)==OPENFS_FILE_LOCK_OK);

    assert(openfs_fd_dup(b,&dup)==OPENFS_FD_OK&&dup==b);
    assert(openfs_file_lock(b,300U,20U,OPENFS_FILE_LOCK_EXCLUSIVE,0U)==OPENFS_FILE_LOCK_OK);
    lock_race_context_t race={dup,0U};
    atomic_init(&lock_race_start,0);
#if defined(_WIN32)
    uintptr_t race_thread=_beginthreadex(NULL,0U,lock_race_worker,&race,0U,NULL);
    assert(race_thread!=0U);
    atomic_store_explicit(&lock_race_start,1,memory_order_release);
    assert(openfs_fd_close(b)==OPENFS_FD_OK);
    assert(WaitForSingleObject((HANDLE)race_thread,60000U)==WAIT_OBJECT_0);
    CloseHandle((HANDLE)race_thread);
#else
    pthread_t race_thread;
    assert(pthread_create(&race_thread,NULL,lock_race_worker,&race)==0);
    atomic_store_explicit(&lock_race_start,1,memory_order_release);
    assert(openfs_fd_close(b)==OPENFS_FD_OK);
    assert(pthread_join(race_thread,NULL)==0);
#endif
    assert(race.failures==0U);
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
