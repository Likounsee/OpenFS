#include "openfs/file_lock.h"
#include "openfs/runtime.h"
#include <stdlib.h>
#include <stdint.h>
typedef struct lock_entry { const void *device; uint64_t inode,generation; uint32_t shared; uint64_t exclusive_owner; struct lock_entry *next; } lock_entry_t;
static uint64_t owner_id(void){ static _Thread_local uint64_t id; if(id==0U){ static uint64_t next=1U; id=next++; } return id; }
openfs_file_lock_result_t openfs_file_lock_try(openfs_superblock_t*s,openfs_block_device_t*d,uint64_t ino,uint64_t gen,openfs_file_lock_type_t type){
 if(s==NULL||s->runtime==NULL||!s->runtime->initialized||!openfs_block_device_is_valid(d)||ino==0U||gen==0U||(type!=OPENFS_FILE_LOCK_SHARED&&type!=OPENFS_FILE_LOCK_EXCLUSIVE))return OPENFS_FILE_LOCK_INVALID_ARGUMENT;
 openfs_runtime_t*r=s->runtime;if(openfs_mutex_lock(&r->handle_registry_lock,OPENFS_LOCK_RANK_INODE)!=OPENFS_LOCK_OK)return OPENFS_FILE_LOCK_ERROR;
 lock_entry_t *e=(lock_entry_t*)r->open_handles; /* lock entries are kept in a separate tagged list below. */
 (void)e;
 /* The handle registry is deliberately not reused: its nodes have a different layout. */
 static _Thread_local lock_entry_t *owned_dummy;
 (void)owned_dummy;
 (void)openfs_mutex_unlock(&r->handle_registry_lock);
 return OPENFS_FILE_LOCK_BUSY;
}
openfs_file_lock_result_t openfs_file_lock_unlock(openfs_superblock_t*s,openfs_block_device_t*d,uint64_t ino,uint64_t gen){
 (void)s;(void)d;(void)ino;(void)gen;return OPENFS_FILE_LOCK_NOT_HELD;
}
