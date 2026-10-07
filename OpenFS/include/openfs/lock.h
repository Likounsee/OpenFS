#ifndef OPENFS_LOCK_H
#define OPENFS_LOCK_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
typedef enum { OPENFS_LOCK_OK=0, OPENFS_LOCK_INVALID_ARGUMENT=1, OPENFS_LOCK_DEADLOCK=2, OPENFS_LOCK_ERROR=3 } openfs_lock_result_t;
typedef enum { OPENFS_LOCK_RANK_NONE=0, OPENFS_LOCK_RANK_LIFECYCLE=6, OPENFS_LOCK_RANK_MOUNT=5, OPENFS_LOCK_RANK_DIRECTORY=10, OPENFS_LOCK_RANK_HANDLE=15, OPENFS_LOCK_RANK_INODE=20, OPENFS_LOCK_RANK_ALLOCATION=30, OPENFS_LOCK_RANK_JOURNAL=40 } openfs_lock_rank_t;
typedef struct { uintptr_t storage[16]; } openfs_mutex_t;
typedef struct { uintptr_t storage[16]; } openfs_rwlock_t;
openfs_lock_result_t openfs_mutex_init(openfs_mutex_t *);
openfs_lock_result_t openfs_mutex_destroy(openfs_mutex_t *);
openfs_lock_result_t openfs_mutex_lock(openfs_mutex_t *, openfs_lock_rank_t);
openfs_lock_result_t openfs_mutex_trylock(openfs_mutex_t *, openfs_lock_rank_t);
openfs_lock_result_t openfs_mutex_unlock(openfs_mutex_t *);
openfs_lock_result_t openfs_rwlock_init(openfs_rwlock_t *);
openfs_lock_result_t openfs_rwlock_destroy(openfs_rwlock_t *);
openfs_lock_result_t openfs_rwlock_read_lock(openfs_rwlock_t *, openfs_lock_rank_t);
openfs_lock_result_t openfs_rwlock_write_lock(openfs_rwlock_t *, openfs_lock_rank_t);
openfs_lock_result_t openfs_rwlock_try_read_lock(openfs_rwlock_t *, openfs_lock_rank_t);
openfs_lock_result_t openfs_rwlock_try_write_lock(openfs_rwlock_t *, openfs_lock_rank_t);
openfs_lock_result_t openfs_rwlock_unlock(openfs_rwlock_t *);
#ifdef __cplusplus
}
#endif
#endif