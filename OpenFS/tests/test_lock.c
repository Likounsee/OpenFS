#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif
#include "openfs/lock.h"

typedef struct {
    openfs_mutex_t *mutex;
    uint64_t *counter;
} worker_context_t;

typedef struct {
    openfs_mutex_t *mutex;
    openfs_lock_result_t result;
} wrong_unlock_context_t;

typedef struct {
    openfs_rwlock_t *lock;
    uint64_t *counter;
} rw_worker_context_t;
typedef struct {
    openfs_mutex_t *contended;
    openfs_mutex_t *lower_rank;
    openfs_lock_result_t try_result;
    openfs_lock_result_t lower_lock_result;
} trylock_context_t;

#if defined(_WIN32)
static unsigned __stdcall worker(void *arg)
#else
static void *worker(void *arg)
#endif
{
    worker_context_t *ctx=(worker_context_t *)arg;
    for(unsigned i=0U;i<10000U;i++){
        assert(openfs_mutex_lock(ctx->mutex,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
        (*ctx->counter)++;
        assert(openfs_mutex_unlock(ctx->mutex)==OPENFS_LOCK_OK);
    }
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

#if defined(_WIN32)
static unsigned __stdcall wrong_unlock_worker(void *arg)
#else
static void *wrong_unlock_worker(void *arg)
#endif
{
    wrong_unlock_context_t *ctx=(wrong_unlock_context_t *)arg;
    ctx->result=openfs_mutex_unlock(ctx->mutex);
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

#if defined(_WIN32)
static unsigned __stdcall failed_trylock_worker(void *arg)
#else
static void *failed_trylock_worker(void *arg)
#endif
{
    trylock_context_t *ctx=(trylock_context_t *)arg;
    ctx->try_result=openfs_mutex_trylock(ctx->contended,OPENFS_LOCK_RANK_INODE);
    ctx->lower_lock_result=openfs_mutex_lock(ctx->lower_rank,OPENFS_LOCK_RANK_DIRECTORY);
    if(ctx->lower_lock_result==OPENFS_LOCK_OK)
        (void)openfs_mutex_unlock(ctx->lower_rank);
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

#if defined(_WIN32)
static unsigned __stdcall rw_worker(void *arg)
#else
static void *rw_worker(void *arg)
#endif
{
    rw_worker_context_t *ctx=(rw_worker_context_t *)arg;
    for(unsigned i=0U;i<10000U;i++){
        assert(openfs_rwlock_write_lock(ctx->lock,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
        (*ctx->counter)++;
        assert(openfs_rwlock_unlock(ctx->lock)==OPENFS_LOCK_OK);
    }
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

int main(void)
{
    openfs_mutex_t mutex;
    assert(openfs_mutex_init(&mutex)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&mutex,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
    assert(openfs_mutex_destroy(&mutex)==OPENFS_LOCK_ERROR);
    assert(openfs_mutex_trylock(&mutex,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
    wrong_unlock_context_t wrong_unlock={&mutex,OPENFS_LOCK_OK};
#if defined(_WIN32)
    uintptr_t wrong_handle=_beginthreadex(NULL,0U,wrong_unlock_worker,&wrong_unlock,0U,NULL);
    assert(wrong_handle!=0U);
    assert(WaitForSingleObject((HANDLE)wrong_handle,60000U)==WAIT_OBJECT_0);
    CloseHandle((HANDLE)wrong_handle);
#else
    pthread_t wrong_thread;
    assert(pthread_create(&wrong_thread,NULL,wrong_unlock_worker,&wrong_unlock)==0);
    assert(pthread_join(wrong_thread,NULL)==0);
#endif
    assert(wrong_unlock.result==OPENFS_LOCK_DEADLOCK);
    assert(openfs_mutex_unlock(&mutex)==OPENFS_LOCK_OK);
    assert(openfs_mutex_unlock(&mutex)==OPENFS_LOCK_OK);

    /*
     * A failed try-lock must roll back its thread-local rank entry. Otherwise
     * a later, lower-ranked acquisition on the same thread is falsely rejected.
     */
    openfs_mutex_t lower_after_try;
    assert(openfs_mutex_init(&lower_after_try)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&mutex,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
    trylock_context_t try_ctx={&mutex,&lower_after_try,OPENFS_LOCK_OK,OPENFS_LOCK_ERROR};
#if defined(_WIN32)
    uintptr_t try_handle=_beginthreadex(NULL,0U,failed_trylock_worker,&try_ctx,0U,NULL);
    assert(try_handle!=0U);
    assert(WaitForSingleObject((HANDLE)try_handle,60000U)==WAIT_OBJECT_0);
    CloseHandle((HANDLE)try_handle);
#else
    pthread_t try_thread;
    assert(pthread_create(&try_thread,NULL,failed_trylock_worker,&try_ctx)==0);
    assert(pthread_join(try_thread,NULL)==0);
#endif
    assert(try_ctx.try_result==OPENFS_LOCK_ERROR);
    assert(try_ctx.lower_lock_result==OPENFS_LOCK_OK);
    assert(openfs_mutex_unlock(&mutex)==OPENFS_LOCK_OK);
    assert(openfs_mutex_destroy(&lower_after_try)==OPENFS_LOCK_OK);

    /*
     * The per-thread rank stack has a fixed capacity. The acquisition beyond
     * that limit must fail without taking the native mutex or corrupting the
     * stack, and all successful recursive acquisitions must remain releasable.
     */
    openfs_mutex_t stack_limit;
    assert(openfs_mutex_init(&stack_limit)==OPENFS_LOCK_OK);
    for(unsigned i=0U;i<32U;i++)
        assert(openfs_mutex_lock(&stack_limit,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&stack_limit,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_DEADLOCK);
    for(unsigned i=0U;i<32U;i++)
        assert(openfs_mutex_unlock(&stack_limit)==OPENFS_LOCK_OK);
    assert(openfs_mutex_destroy(&stack_limit)==OPENFS_LOCK_OK);

    /* Lock ordering must reject a lower-ranked lock while a higher-ranked
       lock is held, preventing the most common lock-order deadlock. */
    openfs_mutex_t journal;
    assert(openfs_mutex_init(&journal)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&journal,OPENFS_LOCK_RANK_JOURNAL)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&mutex,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_DEADLOCK);
    assert(openfs_mutex_unlock(&journal)==OPENFS_LOCK_OK);

    /* A file-handle lock is below the inode hierarchy level, so an append operation may safely acquire the inode lock while retaining its handle lock. */
    openfs_mutex_t handle_order;
    assert(openfs_mutex_init(&handle_order)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&handle_order,OPENFS_LOCK_RANK_HANDLE)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&mutex,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
    assert(openfs_mutex_unlock(&mutex)==OPENFS_LOCK_OK);
    assert(openfs_mutex_unlock(&handle_order)==OPENFS_LOCK_OK);
    assert(openfs_mutex_destroy(&handle_order)==OPENFS_LOCK_OK);

    /* Registry locks sit above inode locks but below allocation/journal. */
    openfs_mutex_t registry_order;
    assert(openfs_mutex_init(&registry_order)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&mutex,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&registry_order,OPENFS_LOCK_RANK_REGISTRY)==OPENFS_LOCK_OK);
    assert(openfs_mutex_unlock(&registry_order)==OPENFS_LOCK_OK);
    assert(openfs_mutex_unlock(&mutex)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&registry_order,OPENFS_LOCK_RANK_REGISTRY)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&mutex,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_DEADLOCK);
    assert(openfs_mutex_unlock(&registry_order)==OPENFS_LOCK_OK);
    assert(openfs_mutex_destroy(&registry_order)==OPENFS_LOCK_OK);

    openfs_mutex_t ordered;
    assert(openfs_mutex_init(&ordered)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&ordered,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
    assert(openfs_mutex_unlock(&ordered)==OPENFS_LOCK_OK);
    assert(openfs_mutex_destroy(&ordered)==OPENFS_LOCK_OK);

    openfs_mutex_t low,high;
    assert(openfs_mutex_init(&low)==OPENFS_LOCK_OK&&openfs_mutex_init(&high)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&low,OPENFS_LOCK_RANK_DIRECTORY)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&high,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
    assert(openfs_mutex_unlock(&low)==OPENFS_LOCK_DEADLOCK);
    assert(openfs_mutex_unlock(&high)==OPENFS_LOCK_OK);
    assert(openfs_mutex_unlock(&low)==OPENFS_LOCK_OK);
    assert(openfs_mutex_destroy(&high)==OPENFS_LOCK_OK&&openfs_mutex_destroy(&low)==OPENFS_LOCK_OK);

    uint64_t counter=0U;
    worker_context_t ctx={&mutex,&counter};
#if defined(_WIN32)
    HANDLE threads[8];
    for(unsigned i=0U;i<8U;i++){
        uintptr_t h=_beginthreadex(NULL,0U,worker,&ctx,0U,NULL);
        assert(h!=0U);
        threads[i]=(HANDLE)h;
    }
    assert(WaitForMultipleObjects(8U,threads,TRUE,60000U)==WAIT_OBJECT_0);
    for(unsigned i=0U;i<8U;i++)CloseHandle(threads[i]);
#else
    pthread_t threads[8];
    for(unsigned i=0U;i<8U;i++)assert(pthread_create(&threads[i],NULL,worker,&ctx)==0);
    for(unsigned i=0U;i<8U;i++)assert(pthread_join(threads[i],NULL)==0);
#endif
    assert(counter==80000U);

    assert(openfs_mutex_destroy(&journal)==OPENFS_LOCK_OK);
    assert(openfs_mutex_destroy(&mutex)==OPENFS_LOCK_OK);

    openfs_rwlock_t lock;
    assert(openfs_rwlock_init(&lock)==OPENFS_LOCK_OK);
    assert(openfs_rwlock_read_lock(&lock,OPENFS_LOCK_RANK_DIRECTORY)==OPENFS_LOCK_OK);
    assert(openfs_rwlock_unlock(&lock)==OPENFS_LOCK_OK);
    assert(openfs_rwlock_write_lock(&lock,OPENFS_LOCK_RANK_DIRECTORY)==OPENFS_LOCK_OK);
#if defined(_WIN32)
    assert(openfs_rwlock_destroy(&lock)==OPENFS_LOCK_ERROR);
    assert(openfs_rwlock_write_lock(&lock,OPENFS_LOCK_RANK_DIRECTORY)==OPENFS_LOCK_OK);
    assert(openfs_rwlock_try_write_lock(&lock,OPENFS_LOCK_RANK_DIRECTORY)==OPENFS_LOCK_OK);
    assert(openfs_rwlock_unlock(&lock)==OPENFS_LOCK_OK);
    assert(openfs_rwlock_unlock(&lock)==OPENFS_LOCK_OK);
    assert(openfs_rwlock_unlock(&lock)==OPENFS_LOCK_OK);
#else
    assert(openfs_rwlock_unlock(&lock)==OPENFS_LOCK_OK);
#endif
    assert(openfs_rwlock_destroy(&lock)==OPENFS_LOCK_OK);

    /* Re-entering a rwlock beneath another held lock must fail on every
       platform instead of depending on native recursive-lock behavior. */
    openfs_rwlock_t low_rank;
    openfs_mutex_t high_rank;
    assert(openfs_rwlock_init(&low_rank)==OPENFS_LOCK_OK);
    assert(openfs_mutex_init(&high_rank)==OPENFS_LOCK_OK);
    assert(openfs_rwlock_write_lock(&low_rank,OPENFS_LOCK_RANK_DIRECTORY)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&high_rank,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
    assert(openfs_rwlock_try_write_lock(&low_rank,OPENFS_LOCK_RANK_REGISTRY)==OPENFS_LOCK_DEADLOCK);
    assert(openfs_rwlock_try_read_lock(&low_rank,OPENFS_LOCK_RANK_REGISTRY)==OPENFS_LOCK_DEADLOCK);
    assert(openfs_rwlock_write_lock(&low_rank,OPENFS_LOCK_RANK_REGISTRY)==OPENFS_LOCK_DEADLOCK);
    assert(openfs_rwlock_read_lock(&low_rank,OPENFS_LOCK_RANK_REGISTRY)==OPENFS_LOCK_DEADLOCK);
    assert(openfs_mutex_unlock(&high_rank)==OPENFS_LOCK_OK);
    assert(openfs_rwlock_unlock(&low_rank)==OPENFS_LOCK_OK);
    assert(openfs_mutex_destroy(&high_rank)==OPENFS_LOCK_OK);
    assert(openfs_rwlock_destroy(&low_rank)==OPENFS_LOCK_OK);

    /* Contended writer acquisitions must serialize updates on every platform. */
    assert(openfs_rwlock_init(&lock)==OPENFS_LOCK_OK);
    uint64_t rw_counter=0U;
    rw_worker_context_t rw_ctx={&lock,&rw_counter};
#if defined(_WIN32)
    HANDLE rw_threads[8];
    for(unsigned i=0U;i<8U;i++){
        uintptr_t h=_beginthreadex(NULL,0U,rw_worker,&rw_ctx,0U,NULL);
        assert(h!=0U);
        rw_threads[i]=(HANDLE)h;
    }
    assert(WaitForMultipleObjects(8U,rw_threads,TRUE,60000U)==WAIT_OBJECT_0);
    for(unsigned i=0U;i<8U;i++)CloseHandle(rw_threads[i]);
#else
    pthread_t rw_threads[8];
    for(unsigned i=0U;i<8U;i++)assert(pthread_create(&rw_threads[i],NULL,rw_worker,&rw_ctx)==0);
    for(unsigned i=0U;i<8U;i++)assert(pthread_join(rw_threads[i],NULL)==0);
#endif
    assert(rw_counter==80000U);
    assert(openfs_rwlock_destroy(&lock)==OPENFS_LOCK_OK);
    puts("openfs lock tests passed");
    return 0;
}
