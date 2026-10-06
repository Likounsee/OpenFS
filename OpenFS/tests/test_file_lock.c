#include "openfs/file_lock.h"
#include <assert.h>
#include <stdio.h>
int main(void){
    uint32_t conflict=0U;
    assert(openfs_file_lock_test(NULL,0U,1U,&conflict)==OPENFS_FILE_LOCK_CLOSED);
    assert(openfs_file_lock(NULL,0U,1U,OPENFS_FILE_LOCK_SHARED,0U)==OPENFS_FILE_LOCK_CLOSED);
    assert(openfs_file_unlock(NULL,0U,1U)==OPENFS_FILE_LOCK_CLOSED);
    puts("file lock API test passed");
    return 0;
}
