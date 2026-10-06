#ifndef OPENFS_RUNTIME_H
#define OPENFS_RUNTIME_H
#include "openfs/lock.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct openfs_runtime {
    openfs_mutex_t directory_lock;
    openfs_mutex_t inode_lock;
    openfs_mutex_t allocation_lock;
    openfs_mutex_t journal_lock;
    int initialized;
} openfs_runtime_t;
int openfs_runtime_init(openfs_runtime_t *);
void openfs_runtime_destroy(openfs_runtime_t *);
#ifdef __cplusplus
}
#endif
#endif