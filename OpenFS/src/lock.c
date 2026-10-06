#include "openfs/lock.h"
#include <stdint.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
struct openfs_mutex { CRITICAL_SECTION native; DWORD owner; unsigned depth; };
struct openfs_rwlock { SRWLOCK native; DWORD writer; unsigned write_depth; };
static DWORD lock_thread_id(void){return GetCurrentThreadId();}
#else
#include <pthread.h>
#include <errno.h>
struct openfs_mutex { pthread_mutex_t native; pthread_t owner; unsigned depth; int owned; };
struct openfs_rwlock { pthread_rwlock_t native; pthread_t writer; unsigned write_depth; int writer_owned; };
static int same_thread(pthread_t a,pthread_t b){return pthread_equal(a,b)!=0;}
#endif
#define OPENFS_LOCK_STACK_MAX 32U
static _Thread_local openfs_lock_rank_t lock_stack[OPENFS_LOCK_STACK_MAX];
static _Thread_local unsigned lock_depth;
static openfs_lock_result_t rank_enter(openfs_lock_rank_t rank){if(rank==OPENFS_LOCK_RANK_NONE)return OPENFS_LOCK_INVALID_ARGUMENT;if(lock_depth!=0U&&rank<lock_stack[lock_depth-1U])return OPENFS_LOCK_DEADLOCK;if(lock_depth>=OPENFS_LOCK_STACK_MAX)return OPENFS_LOCK_DEADLOCK;lock_stack[lock_depth++]=rank;return OPENFS_LOCK_OK;}
static void rank_cancel(void){if(lock_depth!=0U)--lock_depth;}
static void rank_leave(void){if(lock_depth!=0U)--lock_depth;}
openfs_lock_result_t openfs_mutex_init(openfs_mutex_t*m){if(m==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
#if defined(_WIN32)
InitializeCriticalSection(&m->native);m->owner=0;m->depth=0;return OPENFS_LOCK_OK;
#else
pthread_mutexattr_t a;if(pthread_mutexattr_init(&a)!=0)return OPENFS_LOCK_ERROR;if(pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE)!=0){pthread_mutexattr_destroy(&a);return OPENFS_LOCK_ERROR;}int r=pthread_mutex_init(&m->native,&a);pthread_mutexattr_destroy(&a);return r==0?OPENFS_LOCK_OK:OPENFS_LOCK_ERROR;
#endif
}
openfs_lock_result_t openfs_mutex_destroy(openfs_mutex_t*m){if(m==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
#if defined(_WIN32)
DeleteCriticalSection(&m->native);m->owner=0;m->depth=0;return OPENFS_LOCK_OK;
#else
return pthread_mutex_destroy(&m->native)==0?OPENFS_LOCK_OK:OPENFS_LOCK_ERROR;
#endif
}
openfs_lock_result_t openfs_mutex_lock(openfs_mutex_t*m,openfs_lock_rank_t rank){if(m==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;openfs_lock_result_t r=rank_enter(rank);if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
EnterCriticalSection(&m->native);m->owner=lock_thread_id();m->depth++;return OPENFS_LOCK_OK;
#else
if(pthread_mutex_lock(&m->native)!=0){rank_cancel();return OPENFS_LOCK_ERROR;}m->owner=pthread_self();m->owned=1;m->depth++;return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_mutex_trylock(openfs_mutex_t*m,openfs_lock_rank_t rank){if(m==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;openfs_lock_result_t r=rank_enter(rank);if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
if(!TryEnterCriticalSection(&m->native)){rank_cancel();return OPENFS_LOCK_ERROR;}m->owner=lock_thread_id();m->depth++;return OPENFS_LOCK_OK;
#else
int e=pthread_mutex_trylock(&m->native);if(e!=0){rank_cancel();return OPENFS_LOCK_ERROR;}(void)e;m->owner=pthread_self();m->owned=1;m->depth++;return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_mutex_unlock(openfs_mutex_t*m){if(m==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
#if defined(_WIN32)
if(m->owner!=lock_thread_id()||m->depth==0U)return OPENFS_LOCK_ERROR;if(--m->depth==0U)m->owner=0;LeaveCriticalSection(&m->native);rank_leave();return OPENFS_LOCK_OK;
#else
if(!m->owned||m->depth==0U||!same_thread(m->owner,pthread_self()))return OPENFS_LOCK_ERROR;if(--m->depth==0U)m->owned=0;if(pthread_mutex_unlock(&m->native)!=0)return OPENFS_LOCK_ERROR;rank_leave();return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_rwlock_init(openfs_rwlock_t*l){if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
#if defined(_WIN32)
InitializeSRWLock(&l->native);l->writer=0;l->write_depth=0;return OPENFS_LOCK_OK;
#else
return pthread_rwlock_init(&l->native,NULL)==0?OPENFS_LOCK_OK:OPENFS_LOCK_ERROR;
#endif
}
openfs_lock_result_t openfs_rwlock_destroy(openfs_rwlock_t*l){if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
#if defined(_WIN32)
l->writer=0;l->write_depth=0;return OPENFS_LOCK_OK;
#else
return pthread_rwlock_destroy(&l->native)==0?OPENFS_LOCK_OK:OPENFS_LOCK_ERROR;
#endif
}
openfs_lock_result_t openfs_rwlock_read_lock(openfs_rwlock_t*l,openfs_lock_rank_t rank){if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;openfs_lock_result_t r=rank_enter(rank);if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
AcquireSRWLockShared(&l->native);return OPENFS_LOCK_OK;
#else
if(pthread_rwlock_rdlock(&l->native)!=0){rank_cancel();return OPENFS_LOCK_ERROR;}return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_rwlock_write_lock(openfs_rwlock_t*l,openfs_lock_rank_t rank){if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;openfs_lock_result_t r=rank_enter(rank);if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
AcquireSRWLockExclusive(&l->native);l->writer=lock_thread_id();l->write_depth++;return OPENFS_LOCK_OK;
#else
if(pthread_rwlock_wrlock(&l->native)!=0){rank_cancel();return OPENFS_LOCK_ERROR;}l->writer=pthread_self();l->writer_owned=1;l->write_depth++;return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_rwlock_try_read_lock(openfs_rwlock_t*l,openfs_lock_rank_t rank){if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;openfs_lock_result_t r=rank_enter(rank);if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
if(!TryAcquireSRWLockShared(&l->native)){rank_cancel();return OPENFS_LOCK_ERROR;}return OPENFS_LOCK_OK;
#else
if(pthread_rwlock_tryrdlock(&l->native)!=0){rank_cancel();return OPENFS_LOCK_ERROR;}return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_rwlock_try_write_lock(openfs_rwlock_t*l,openfs_lock_rank_t rank){if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;openfs_lock_result_t r=rank_enter(rank);if(r!=OPENFS_LOCK_OK)return r;
#if defined(_WIN32)
if(!TryAcquireSRWLockExclusive(&l->native)){rank_cancel();return OPENFS_LOCK_ERROR;}l->writer=lock_thread_id();l->write_depth++;return OPENFS_LOCK_OK;
#else
if(pthread_rwlock_trywrlock(&l->native)!=0){rank_cancel();return OPENFS_LOCK_ERROR;}l->writer=pthread_self();l->writer_owned=1;l->write_depth++;return OPENFS_LOCK_OK;
#endif
}
openfs_lock_result_t openfs_rwlock_unlock(openfs_rwlock_t*l){if(l==NULL)return OPENFS_LOCK_INVALID_ARGUMENT;
#if defined(_WIN32)
if(l->write_depth!=0U&&l->writer==lock_thread_id()){l->write_depth--;if(l->write_depth==0U)l->writer=0;ReleaseSRWLockExclusive(&l->native);}else ReleaseSRWLockShared(&l->native);
#else
if(l->writer_owned&&l->write_depth!=0U&&same_thread(l->writer,pthread_self())){l->write_depth--;if(l->write_depth==0U)l->writer_owned=0;}if(pthread_rwlock_unlock(&l->native)!=0)return OPENFS_LOCK_ERROR;
#endif
rank_leave();return OPENFS_LOCK_OK;}
