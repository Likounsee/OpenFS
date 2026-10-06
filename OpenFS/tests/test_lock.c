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

int main(void)
{
    openfs_mutex_t mutex;
    assert(openfs_mutex_init(&mutex)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&mutex,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
    assert(openfs_mutex_trylock(&mutex,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
    assert(openfs_mutex_unlock(&mutex)==OPENFS_LOCK_OK);
    assert(openfs_mutex_unlock(&mutex)==OPENFS_LOCK_OK);

    /* Lock ordering must reject a lower-ranked lock while a higher-ranked
       lock is held, preventing the most common lock-order deadlock. */
    openfs_mutex_t journal;
    assert(openfs_mutex_init(&journal)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&journal,OPENFS_LOCK_RANK_JOURNAL)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&mutex,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_DEADLOCK);
    assert(openfs_mutex_unlock(&journal)==OPENFS_LOCK_OK);

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
    assert(openfs_rwlock_unlock(&lock)==OPENFS_LOCK_OK);
    assert(openfs_rwlock_destroy(&lock)==OPENFS_LOCK_OK);
    puts("openfs lock tests passed");
    return 0;
}
