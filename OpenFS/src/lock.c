#define _XOPEN_SOURCE 700
#include "openfs/lock.h"
#include "openfs/runtime.h"
#include <stdint.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef struct { CRITICAL_SECTION native; DWORD owner; unsigned depth; } mutex_impl_t;
typedef struct { SRWLOCK native; DWORD writer; unsigned write_depth; } rwlock_impl_t;
static DWORD lock_thread_id(void){return GetCurrentThreadId();}
static mutex_impl_t *mi(openfs_mutex_t*m){return (mutex_impl_t*)m;}
static struct openfs_runtime *mutex_runtime(openfs_mutex_t*m){return (struct openfs_runtime *)(uintptr_t)m->storage[15];}
static rwlock_impl_t *ri(openfs_rwlock_t*l){return (rwlock_impl_t*)l;}
#else
#include <pthread.h>
#include <errno.h>
typedef struct { pthread_mutex_t native; pthread_t owner; unsigned depth; int owned; } mutex_impl_t;
typedef struct { pthread_rwlock_t native; pthread_t writer; unsigned write_depth; int writer_owned; } rwlock_impl_t;
static int same_thread(pthread_t a,pthread_t b){return pthread_equal(a,b)!=0;}
static mutex_impl_t *mi(openfs_mutex_t*m){return (mutex_impl_t*)m;}
static struct openfs_runtime *mutex_runtime(openfs_mutex_t*m){return (struct openfs_runtime *)(uintptr_t)m->storage[15];}
static rwlock_impl_t *ri(openfs_rwlock_t*l){return (rwlock_impl_t*)l;}
#endif
_Static_assert(sizeof(mutex_impl_t)<=sizeof(openfs_mutex_t), "openfs_mutex_t storage is too small");
_Static_assert(sizeof(rwlock_impl_t)<=sizeof(openfs_rwlock_t), "openfs_rwlock_t storage is too small");
#define OPENFS_LOCK_STACK_MAX 32U
typedef struct { const void *object; openfs_lock_rank_t rank; } lock_entry_t;
static _Thread_local lock_entry_t lock_stack[OPENFS_LOCK_STACK_MAX];
static _Thread_local unsigned lock_depth;
#if defined(_WIN32)
/* SRWLOCK is not recursive. Track recursive writer ownership per thread so
   other threads never inspect writer metadata without owning the native lock. */
static _Thread_local const void *rwlock_writer_stack[OPENFS_LOCK_STACK_MAX];
static _Thread_local unsigned rwlock_writer_depth;
static int rwlock_writer_is_top(const void *object){return rwlock_writer_depth!=0U&&rwlock_writer_stack[rwlock_writer_depth-1U]==object;}
static int rwlock_writer_contains(const void *object){for(unsigned i=0U;i<rwlock_writer_depth;i++)if(rwlock_writer_stack[i]==object)return 1;return 0;}
static int rwlock_writer_push(const void *object){if(rwlock_writer_depth>=OPENFS_LOCK_STACK_MAX)return 0;rwlock_writer_stack[rwlock_writer_depth++]=object;return 1;}
static int rwlock_writer_pop(const void *object){if(!rwlock_writer_is_top(object))return 0;rwlock_writer_stack[--rwlock_writer_depth]=NULL;return 1;}
#endif
static int rank_valid(openfs_lock_rank_t rank){switch(rank){case OPENFS_LOCK_RANK_MOUNT:case OPENFS_LOCK_RANK_DIRECTORY:case OPENFS_LOCK_RANK_HANDLE:case OPENFS_LOCK_RANK_INODE:case OPENFS_LOCK_RANK_REGISTRY:case OPENFS_LOCK_RANK_ALLOCATION:case OPENFS_LOCK_RANK_TRANSACTION:case OPENFS_LOCK_RANK_JOURNAL:case OPENFS_LOCK_RANK_CHECKSUM:return 1;default:return 0;}}
static openfs_lock_result_t rank_enter(const void *object,openfs_lock_rank_t rank){if(object==NULL||!rank_valid(rank))return OPENFS_LOCK_INVALID_ARGUMENT;if(lock_depth!=0U&&rank<lock_stack[lock_depth-1U].rank)return OPENFS_LOCK_DEADLOCK;if(lock_depth>=OPENFS_LOCK_STACK_MAX)return OPENFS_LOCK_DEADLOCK;lock_stack[lock_depth].object=object;lock_stack[lock_depth].rank=rank;++lock_depth;return OPENFS_LOCK_OK;}
static void rank_cancel(void){if(lock_depth!=0U)--lock_depth;}
static int rank_is_top(const void *object){return lock_depth!=0U&&lock_stack[lock_depth-1U].object==object;}
static int rank_contains(const void *object){for(unsigned i=0U;i<lock_depth;i++)if(lock_stack[i].object==object)return 1;return 0;}
static openfs_lock_result_t rank_leave(const void *object){if(!rank_is_top(object))return OPENFS_LOCK_DEADLOCK;--lock_depth;return OPENFS_LOCK_OK;}
openfs_lock_result_t openfs_mutex_init(openfs_mutex_t*m){if(m==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;memset(m,0,sizeof(*m));
#if defined(_WIN32)
InitializeCriticalSection(&mi(m)->native);mi(m)->owner=0;mi(m)->depth=0;return OPENFS_LOCK_OK;
#else
pthread_mutexattr_t a;if(pthread_mutexattr_init(&a)!=0)return OPENFS_LOCK_ERROR;if(pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE)!=0){pthread_mutexattr_destroy(&a);return OPENFS_LOCK_ERROR;}int r=pthread_mutex_init(&mi(m)->native,&a);pthread_mutexattr_destroy(&a);return r==0?OPENFS_LOCK_OK:OPENFS_LOCK_ERROR;
#endif
}
openfs_lock_result_t openfs_mutex_destroy(openfs_mutex_t*m){if(m==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
#if defined(_WIN32)
if(mi(m)->depth!=0U)return OPENFS_LOCK_ERROR;DeleteCriticalSection(&mi(m)->native);mi(m)->owner=0;mi(m)->depth=0;return OPENFS_LOCK_OK;
#else
if(mi(m)->owned||mi(m)->depth!=0U)return OPENFS_LOCK_ERROR;return pthread_mutex_destroy(&mi(m)->native)==0?OPENFS_LOCK_OK:OPENFS_LOCK_ERROR;
#endif
}
openfs_lock_result_t openfs_mutex_lock(openfs_mutex_t*m,openfs_lock_rank_t rank){if(m==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;struct openfs_runtime *rt=mutex_runtime(m);if(rt!=NULL&&!openfs_runtime_enter(rt))return OPENFS_LOCK_ERROR;if(rank_contains(m)&&!rank_is_top(m)){if(rt!=NULL)openfs_runtime_leave(rt);return OPENFS_LOCK_DEADLOCK;}openfs_lock_result_t r=rank_enter(m,rank);if(r!=OPENFS_LOCK_OK){if(rt!=NULL)openfs_runtime_leave(rt);return r;}
#if defined(_WIN32)
EnterCriticalSection(&mi(m)->native);mi(m)->owner=lock_thread_id();mi(m)->depth++;return OPENFS_LOCK_OK;
#else
if(pthread_mutex_lock(&mi(m)->native)!=0){rank_cancel();if(rt!=NULL)openfs_runtime_leave(rt);return OPENFS_LOCK_ERROR;}mi(m)->owner=pthread_self();mi(m)->owned=1;mi(m)->depth++;return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_mutex_trylock(openfs_mutex_t*m,openfs_lock_rank_t rank){if(m==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;struct openfs_runtime *rt=mutex_runtime(m);if(rt!=NULL&&!openfs_runtime_enter(rt))return OPENFS_LOCK_ERROR;if(rank_contains(m)&&!rank_is_top(m)){if(rt!=NULL)openfs_runtime_leave(rt);return OPENFS_LOCK_DEADLOCK;}openfs_lock_result_t r=rank_enter(m,rank);if(r!=OPENFS_LOCK_OK){if(rt!=NULL)openfs_runtime_leave(rt);return r;}
#if defined(_WIN32)
if(!TryEnterCriticalSection(&mi(m)->native)){rank_cancel();if(rt!=NULL)openfs_runtime_leave(rt);return OPENFS_LOCK_ERROR;}mi(m)->owner=lock_thread_id();mi(m)->depth++;return OPENFS_LOCK_OK;
#else
int e=pthread_mutex_trylock(&mi(m)->native);if(e!=0){rank_cancel();if(rt!=NULL)openfs_runtime_leave(rt);return OPENFS_LOCK_ERROR;}(void)e;mi(m)->owner=pthread_self();mi(m)->owned=1;mi(m)->depth++;return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_mutex_unlock(openfs_mutex_t*m){if(m==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;struct openfs_runtime *rt=mutex_runtime(m);
/* The rank stack is thread-local. Check it before reading owner/depth, which
   another thread may be changing while it owns the native mutex. */
if(!rank_is_top(m))return OPENFS_LOCK_DEADLOCK;
#if defined(_WIN32)
if(mi(m)->owner!=lock_thread_id()||mi(m)->depth==0U)return OPENFS_LOCK_ERROR;if(--mi(m)->depth==0U)mi(m)->owner=0;LeaveCriticalSection(&mi(m)->native);if(rank_leave(m)!=OPENFS_LOCK_OK)return OPENFS_LOCK_DEADLOCK;if(rt!=NULL)openfs_runtime_leave(rt);return OPENFS_LOCK_OK;
#else
if(!mi(m)->owned||mi(m)->depth==0U||!same_thread(mi(m)->owner,pthread_self()))return OPENFS_LOCK_ERROR;if(--mi(m)->depth==0U)mi(m)->owned=0;if(pthread_mutex_unlock(&mi(m)->native)!=0)return OPENFS_LOCK_ERROR;if(rank_leave(m)!=OPENFS_LOCK_OK)return OPENFS_LOCK_DEADLOCK;if(rt!=NULL)openfs_runtime_leave(rt);return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_rwlock_init(openfs_rwlock_t*l){if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
#if defined(_WIN32)
InitializeSRWLock(&ri(l)->native);ri(l)->writer=0;ri(l)->write_depth=0;return OPENFS_LOCK_OK;
#else
return pthread_rwlock_init(&ri(l)->native,NULL)==0?OPENFS_LOCK_OK:OPENFS_LOCK_ERROR;
#endif
}
openfs_lock_result_t openfs_rwlock_destroy(openfs_rwlock_t*l){if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
#if defined(_WIN32)
if(ri(l)->write_depth!=0U)return OPENFS_LOCK_ERROR;ri(l)->writer=0;ri(l)->write_depth=0;return OPENFS_LOCK_OK;
#else
return pthread_rwlock_destroy(&ri(l)->native)==0?OPENFS_LOCK_OK:OPENFS_LOCK_ERROR;
#endif
}
openfs_lock_result_t openfs_rwlock_read_lock(openfs_rwlock_t*l,openfs_lock_rank_t rank){
    if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
    if(rank_contains(l))return OPENFS_LOCK_DEADLOCK;
    openfs_lock_result_t r=rank_enter(l,rank);
    if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
    AcquireSRWLockShared(&ri(l)->native);
    return OPENFS_LOCK_OK;
#else
    if(pthread_rwlock_rdlock(&ri(l)->native)!=0){rank_cancel();return OPENFS_LOCK_ERROR;}
    return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_rwlock_write_lock(openfs_rwlock_t*l,openfs_lock_rank_t rank){
    if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
#if defined(_WIN32)
    int recursive=(rank_is_top(l)&&rwlock_writer_is_top(l));
    if(rwlock_writer_contains(l)&&!recursive)return OPENFS_LOCK_DEADLOCK;
#else
    int recursive=0;
#endif
    if(rank_contains(l)&&!recursive)return OPENFS_LOCK_DEADLOCK;
    openfs_lock_result_t r=rank_enter(l,rank);
    if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
    if(recursive){
        if(!rwlock_writer_push(l)){rank_cancel();return OPENFS_LOCK_DEADLOCK;}
        ri(l)->write_depth++;
        return OPENFS_LOCK_OK;
    }
    AcquireSRWLockExclusive(&ri(l)->native);
    if(!rwlock_writer_push(l)){ReleaseSRWLockExclusive(&ri(l)->native);rank_cancel();return OPENFS_LOCK_DEADLOCK;}
    ri(l)->writer=lock_thread_id();ri(l)->write_depth=1U;return OPENFS_LOCK_OK;
#else
    if(pthread_rwlock_wrlock(&ri(l)->native)!=0){rank_cancel();return OPENFS_LOCK_ERROR;}
    ri(l)->writer=pthread_self();ri(l)->writer_owned=1;ri(l)->write_depth++;return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_rwlock_try_read_lock(openfs_rwlock_t*l,openfs_lock_rank_t rank){
    if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
    if(rank_contains(l))return OPENFS_LOCK_DEADLOCK;
    openfs_lock_result_t r=rank_enter(l,rank);
    if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
    if(!TryAcquireSRWLockShared(&ri(l)->native)){rank_cancel();return OPENFS_LOCK_ERROR;}return OPENFS_LOCK_OK;
#else
    if(pthread_rwlock_tryrdlock(&ri(l)->native)!=0){rank_cancel();return OPENFS_LOCK_ERROR;}return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_rwlock_try_write_lock(openfs_rwlock_t*l,openfs_lock_rank_t rank){
    if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
#if defined(_WIN32)
    int recursive=(rank_is_top(l)&&rwlock_writer_is_top(l));
    if(rwlock_writer_contains(l)&&!recursive)return OPENFS_LOCK_DEADLOCK;
#else
    int recursive=0;
#endif
    if(rank_contains(l)&&!recursive)return OPENFS_LOCK_DEADLOCK;
    openfs_lock_result_t r=rank_enter(l,rank);
    if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
    if(recursive){
        if(!rwlock_writer_push(l)){rank_cancel();return OPENFS_LOCK_DEADLOCK;}
        ri(l)->write_depth++;
        return OPENFS_LOCK_OK;
    }
    if(!TryAcquireSRWLockExclusive(&ri(l)->native)){rank_cancel();return OPENFS_LOCK_ERROR;}
    if(!rwlock_writer_push(l)){ReleaseSRWLockExclusive(&ri(l)->native);rank_cancel();return OPENFS_LOCK_DEADLOCK;}
    ri(l)->writer=lock_thread_id();ri(l)->write_depth=1U;return OPENFS_LOCK_OK;
#else
    if(pthread_rwlock_trywrlock(&ri(l)->native)!=0){rank_cancel();return OPENFS_LOCK_ERROR;}
    ri(l)->writer=pthread_self();ri(l)->writer_owned=1;ri(l)->write_depth++;return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_rwlock_unlock(openfs_rwlock_t*l){if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;if(!rank_is_top(l))return OPENFS_LOCK_DEADLOCK;
#if defined(_WIN32)
if(rwlock_writer_is_top(l)){
    if(ri(l)->write_depth==0U)return OPENFS_LOCK_ERROR;
    if(!rwlock_writer_pop(l))return OPENFS_LOCK_ERROR;
    ri(l)->write_depth--;
    if(ri(l)->write_depth==0U){ri(l)->writer=0;ReleaseSRWLockExclusive(&ri(l)->native);}
}else ReleaseSRWLockShared(&ri(l)->native);
#else
if(ri(l)->writer_owned&&ri(l)->write_depth!=0U&&same_thread(ri(l)->writer,pthread_self())){ri(l)->write_depth--;if(ri(l)->write_depth==0U)ri(l)->writer_owned=0;}if(pthread_rwlock_unlock(&ri(l)->native)!=0)return OPENFS_LOCK_ERROR;
#endif
if(rank_leave(l)!=OPENFS_LOCK_OK)return OPENFS_LOCK_DEADLOCK;return OPENFS_LOCK_OK;}
