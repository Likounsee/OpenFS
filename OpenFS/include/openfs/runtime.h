#ifndef OPENFS_RUNTIME_H
#define OPENFS_RUNTIME_H
#include "openfs/lock.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct openfs_runtime {
    openfs_mutex_t directory_lock;
    openfs_mutex_t inode_lock;
    openfs_mutex_t allocation_lock;
    openfs_mutex_t journal_lock;
    openfs_mutex_t handle_registry_lock;
    openfs_mutex_t file_lock_registry_lock;
    void *open_handles;
    void *file_locks;
    int initialized;
} openfs_runtime_t;
int openfs_runtime_init(openfs_runtime_t *);
void openfs_runtime_destroy(openfs_runtime_t *);
int openfs_runtime_handle_acquire(openfs_runtime_t *, const void *, uint64_t, uint64_t);
int openfs_runtime_handle_release(openfs_runtime_t *, const void *, uint64_t, uint64_t);
uint64_t openfs_runtime_handle_count(openfs_runtime_t *, const void *, uint64_t, uint64_t);
uint64_t openfs_runtime_handle_count_all(openfs_runtime_t *);
#ifdef __cplusplus
}
#endif
#endif
