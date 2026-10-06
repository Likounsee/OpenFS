#include <assert.h>
#include <stdio.h>
#include "openfs/lock.h"

int main(void)
{
    openfs_mutex_t mutex;
    assert(openfs_mutex_init(&mutex)==OPENFS_LOCK_OK);
    assert(openfs_mutex_lock(&mutex,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
    assert(openfs_mutex_trylock(&mutex,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
    assert(openfs_mutex_unlock(&mutex)==OPENFS_LOCK_OK);
    assert(openfs_mutex_unlock(&mutex)==OPENFS_LOCK_OK);
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
