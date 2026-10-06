#include "openfs/lock.h"
#include <stdint.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef struct { CRITICAL_SECTION native; DWORD owner; unsigned depth; } mutex_impl_t;
typedef struct { SRWLOCK native; DWORD writer; unsigned write_depth; } rwlock_impl_t;
static DWORD lock_thread_id(void){return GetCurrentThreadId();}
static mutex_impl_t *mi(openfs_mutex_t*m){return (mutex_impl_t*)m;}
static rwlock_impl_t *ri(openfs_rwlock_t*l){return (rwlock_impl_t*)l;}
#else
#include <pthread.h>
#include <errno.h>
typedef struct { pthread_mutex_t native; pthread_t owner; unsigned depth; int owned; } mutex_impl_t;
typedef struct { pthread_rwlock_t native; pthread_t writer; unsigned write_depth; int writer_owned; } rwlock_impl_t;
static int same_thread(pthread_t a,pthread_t b){return pthread_equal(a,b)!=0;}
static mutex_impl_t *mi(openfs_mutex_t*m){return (mutex_impl_t*)m;}
static rwlock_impl_t *ri(openfs_rwlock_t*l){return (rwlock_impl_t*)l;}
#endif
_Static_assert(sizeof(mutex_impl_t)<=sizeof(openfs_mutex_t), "openfs_mutex_t storage is too small");
_Static_assert(sizeof(rwlock_impl_t)<=sizeof(openfs_rwlock_t), "openfs_rwlock_t storage is too small");
#define OPENFS_LOCK_STACK_MAX 32U
typedef struct { const void *object; openfs_lock_rank_t rank; } lock_entry_t;
static _Thread_local lock_entry_t lock_stack[OPENFS_LOCK_STACK_MAX];
static _Thread_local unsigned lock_depth;
static openfs_lock_result_t rank_enter(const void *object,openfs_lock_rank_t rank){if(object==NULL||rank==OPENFS_LOCK_RANK_NONE)return OPENFS_LOCK_INVALID_ARGUMENT;if(lock_depth!=0U&&rank<lock_stack[lock_depth-1U].rank)return OPENFS_LOCK_DEADLOCK;if(lock_depth>=OPENFS_LOCK_STACK_MAX)return OPENFS_LOCK_DEADLOCK;lock_stack[lock_depth].object=object;lock_stack[lock_depth].rank=rank;++lock_depth;return OPENFS_LOCK_OK;}
static void rank_cancel(void){if(lock_depth!=0U)--lock_depth;}
static int rank_is_top(const void *object){return lock_depth!=0U&&lock_stack[lock_depth-1U].object==object;}\nstatic openfs_lock_result_t rank_leave(const void *object){if(!rank_is_top(object))return OPENFS_LOCK_DEADLOCK;--lock_depth;return OPENFS_LOCK_OK;}
openfs_lock_result_t openfs_mutex_init(openfs_mutex_t*m){if(m==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
#if defined(_WIN32)
InitializeCriticalSection(&mi(m)->native);mi(m)->owner=0;mi(m)->depth=0;return OPENFS_LOCK_OK;
#else
pthread_mutexattr_t a;if(pthread_mutexattr_init(&a)!=0)return OPENFS_LOCK_ERROR;if(pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE)!=0){pthread_mutexattr_destroy(&a);return OPENFS_LOCK_ERROR;}int r=pthread_mutex_init(&mi(m)->native,&a);pthread_mutexattr_destroy(&a);return r==0?OPENFS_LOCK_OK:OPENFS_LOCK_ERROR;
#endif
}
openfs_lock_result_t openfs_mutex_destroy(openfs_mutex_t*m){if(m==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
#if defined(_WIN32)
DeleteCriticalSection(&mi(m)->native);mi(m)->owner=0;mi(m)->depth=0;return OPENFS_LOCK_OK;
#else
return pthread_mutex_destroy(&mi(m)->native)==0?OPENFS_LOCK_OK:OPENFS_LOCK_ERROR;
#endif
}
openfs_lock_result_t openfs_mutex_lock(openfs_mutex_t*m,openfs_lock_rank_t rank){if(m==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;openfs_lock_result_t r=rank_enter(m,rank);if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
EnterCriticalSection(&mi(m)->native);mi(m)->owner=lock_thread_id();mi(m)->depth++;return OPENFS_LOCK_OK;
#else
if(pthread_mutex_lock(&mi(m)->native)!=0){rank_cancel();return OPENFS_LOCK_ERROR;}mi(m)->owner=pthread_self();mi(m)->owned=1;mi(m)->depth++;return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_mutex_trylock(openfs_mutex_t*m,openfs_lock_rank_t rank){if(m==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;openfs_lock_result_t r=rank_enter(rank);if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
if(!TryEnterCriticalSection(&mi(m)->native)){rank_cancel();return OPENFS_LOCK_ERROR;}mi(m)->owner=lock_thread_id();mi(m)->depth++;return OPENFS_LOCK_OK;
#else
int e=pthread_mutex_trylock(&mi(m)->native);if(e!=0){rank_cancel();return OPENFS_LOCK_ERROR;}(void)e;mi(m)->owner=pthread_self();mi(m)->owned=1;mi(m)->depth++;return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_mutex_unlock(openfs_mutex_t*m){if(m==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
#if defined(_WIN32)
if(mi(m)->owner!=lock_thread_id()||mi(m)->depth==0U)return OPENFS_LOCK_ERROR;if(!rank_is_top(m))return OPENFS_LOCK_DEADLOCK;if(--mi(m)->depth==0U){mi(m)->owner=0;LeaveCriticalSection(&mi(m)->native);if(rank_leave(m)!=OPENFS_LOCK_OK)return OPENFS_LOCK_DEADLOCK;}else{LeaveCriticalSection(&mi(m)->native);}return OPENFS_LOCK_OK;
#else
if(!mi(m)->owned||mi(m)->depth==0U||!same_thread(mi(m)->owner,pthread_self()))return OPENFS_LOCK_ERROR;if(!rank_is_top(m))return OPENFS_LOCK_DEADLOCK;if(--mi(m)->depth==0U){mi(m)->owned=0;if(pthread_mutex_unlock(&mi(m)->native)!=0)return OPENFS_LOCK_ERROR;if(rank_leave(m)!=OPENFS_LOCK_OK)return OPENFS_LOCK_DEADLOCK;}else{if(pthread_mutex_unlock(&mi(m)->native)!=0)return OPENFS_LOCK_ERROR;}return OPENFS_LOCK_OK;
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
ri(l)->writer=0;ri(l)->write_depth=0;return OPENFS_LOCK_OK;
#else
return pthread_rwlock_destroy(&ri(l)->native)==0?OPENFS_LOCK_OK:OPENFS_LOCK_ERROR;
#endif
}
openfs_lock_result_t openfs_rwlock_read_lock(openfs_rwlock_t*l,openfs_lock_rank_t rank){if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;if(lock_depth!=0U&&lock_stack[lock_depth-1U].object==l)return OPENFS_LOCK_DEADLOCK;openfs_lock_result_t r=rank_enter(l,rank);if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
AcquireSRWLockShared(&ri(l)->native);return OPENFS_LOCK_OK;
#else
if(pthread_rwlock_rdlock(&ri(l)->native)!=0){rank_cancel();return OPENFS_LOCK_ERROR;}return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_rwlock_write_lock(openfs_rwlock_t*l,openfs_lock_rank_t rank){if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;if(lock_depth!=0U&&lock_stack[lock_depth-1U].object==l)return OPENFS_LOCK_DEADLOCK;openfs_lock_result_t r=rank_enter(l,rank);if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
AcquireSRWLockExclusive(&ri(l)->native);ri(l)->writer=lock_thread_id();ri(l)->write_depth++;return OPENFS_LOCK_OK;
#else
if(pthread_rwlock_wrlock(&ri(l)->native)!=0){rank_cancel();return OPENFS_LOCK_ERROR;}ri(l)->writer=pthread_self();ri(l)->writer_owned=1;ri(l)->write_depth++;return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_rwlock_try_read_lock(openfs_rwlock_t*l,openfs_lock_rank_t rank){if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;if(lock_depth!=0U&&lock_stack[lock_depth-1U].object==l)return OPENFS_LOCK_DEADLOCK;openfs_lock_result_t r=rank_enter(l,rank);if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
if(!TryAcquireSRWLockShared(&ri(l)->native)){rank_cancel();return OPENFS_LOCK_ERROR;}return OPENFS_LOCK_OK;
#else
if(pthread_rwlock_tryrdlock(&ri(l)->native)!=0){rank_cancel();return OPENFS_LOCK_ERROR;}return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_rwlock_try_write_lock(openfs_rwlock_t*l,openfs_lock_rank_t rank){if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;if(lock_depth!=0U&&lock_stack[lock_depth-1U].object==l)return OPENFS_LOCK_DEADLOCK;openfs_lock_result_t r=rank_enter(l,rank);if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
if(!TryAcquireSRWLockExclusive(&ri(l)->native)){rank_cancel();return OPENFS_LOCK_ERROR;}ri(l)->writer=lock_thread_id();ri(l)->write_depth++;return OPENFS_LOCK_OK;
#else
if(pthread_rwlock_trywrlock(&ri(l)->native)!=0){rank_cancel();return OPENFS_LOCK_ERROR;}ri(l)->writer=pthread_self();ri(l)->writer_owned=1;ri(l)->write_depth++;return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_rwlock_unlock(openfs_rwlock_t*l){if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;if(!rank_is_top(l))return OPENFS_LOCK_DEADLOCK;
#if defined(_WIN32)
if(ri(l)->write_depth!=0U&&ri(l)->writer==lock_thread_id()){ri(l)->write_depth--;if(ri(l)->write_depth==0U)ri(l)->writer=0;ReleaseSRWLockExclusive(&ri(l)->native);}else ReleaseSRWLockShared(&ri(l)->native);
#else
if(ri(l)->writer_owned&&ri(l)->write_depth!=0U&&same_thread(ri(l)->writer,pthread_self())){ri(l)->write_depth--;if(ri(l)->write_depth==0U)ri(l)->writer_owned=0;}if(pthread_rwlock_unlock(&ri(l)->native)!=0)return OPENFS_LOCK_ERROR;
#endif
if(rank_leave(l)!=OPENFS_LOCK_OK)return OPENFS_LOCK_DEADLOCK;return OPENFS_LOCK_OK;}
