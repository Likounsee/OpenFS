#include "openfs/runtime.h"
#include <stdlib.h>
#include <string.h>

static _Thread_local openfs_runtime_t *tls_runtime;
static _Thread_local unsigned tls_runtime_depth;

int openfs_runtime_enter(openfs_runtime_t *r)
{
    if(r==NULL)return 0;
    if(tls_runtime==r){++tls_runtime_depth;return 1;}
    if(tls_runtime!=NULL)return 0;
    if(openfs_mutex_lock(&r->lifecycle_lock,OPENFS_LOCK_RANK_HANDLE)!=OPENFS_LOCK_OK)return 0;
    int ok=r->initialized&&r->accepting;
    if(ok){if(r->active_users==UINT64_MAX)ok=0;else ++r->active_users;}
    (void)openfs_mutex_unlock(&r->lifecycle_lock);
    if(!ok)return 0;
    tls_runtime=r;tls_runtime_depth=1U;return 1;
}

void openfs_runtime_leave(openfs_runtime_t *r)
{
    if(r==NULL||tls_runtime!=r||tls_runtime_depth==0U)return;
    --tls_runtime_depth;
    if(tls_runtime_depth!=0U)return;
    tls_runtime=NULL;
    if(openfs_mutex_lock(&r->lifecycle_lock,OPENFS_LOCK_RANK_HANDLE)!=OPENFS_LOCK_OK)return;
    if(r->active_users!=0U)--r->active_users;
    (void)openfs_mutex_unlock(&r->lifecycle_lock);
}

static void bind_mutex(openfs_mutex_t *m,openfs_runtime_t *r){m->storage[15]=(uintptr_t)r;}
static void unbind_mutex(openfs_mutex_t *m){m->storage[15]=(uintptr_t)0U;}
typedef struct openfs_handle_entry {
    const void *device;
    uint64_t inode_number;
    uint64_t generation;
    uint64_t references;
    struct openfs_handle_entry *next;
} openfs_handle_entry_t;
typedef struct openfs_file_lock_runtime_entry {
    void *entry;
    struct openfs_file_lock_runtime_entry *next;
} openfs_file_lock_runtime_entry_t;
static openfs_handle_entry_t *entries(openfs_runtime_t *r){return (openfs_handle_entry_t *)r->open_handles;}
int openfs_runtime_handle_acquire(openfs_runtime_t*r,const void*d,uint64_t ino,uint64_t generation){
    if(r==NULL||!r->initialized||d==NULL||ino==0U||generation==0U)return 0;
    if(openfs_mutex_lock(&r->handle_registry_lock,OPENFS_LOCK_RANK_INODE)!=OPENFS_LOCK_OK)return 0;
    openfs_handle_entry_t *e=entries(r);
    while(e!=NULL){if(e->device==d&&e->inode_number==ino&&e->generation==generation){if(e->references==UINT64_MAX){(void)openfs_mutex_unlock(&r->handle_registry_lock);return 0;}e->references++;(void)openfs_mutex_unlock(&r->handle_registry_lock);return 1;}e=e->next;}
    e=(openfs_handle_entry_t*)calloc(1,sizeof(*e));
    if(e==NULL){(void)openfs_mutex_unlock(&r->handle_registry_lock);return 0;}
    e->device=d;e->inode_number=ino;e->generation=generation;e->references=1U;e->next=entries(r);r->open_handles=e;
    (void)openfs_mutex_unlock(&r->handle_registry_lock);return 1;
}
int openfs_runtime_handle_release(openfs_runtime_t*r,const void*d,uint64_t ino,uint64_t generation){
    if(r==NULL||!r->initialized||d==NULL||ino==0U||generation==0U)return 0;
    if(openfs_mutex_lock(&r->handle_registry_lock,OPENFS_LOCK_RANK_INODE)!=OPENFS_LOCK_OK)return 0;
    openfs_handle_entry_t **pp=(openfs_handle_entry_t**)&r->open_handles;
    while(*pp!=NULL){openfs_handle_entry_t *e=*pp;if(e->device==d&&e->inode_number==ino&&e->generation==generation){if(e->references==0U){(void)openfs_mutex_unlock(&r->handle_registry_lock);return 0;}e->references--;if(e->references==0U){*pp=e->next;free(e);} (void)openfs_mutex_unlock(&r->handle_registry_lock);return 1;}pp=&e->next;}
    (void)openfs_mutex_unlock(&r->handle_registry_lock);return 0;
}
uint64_t openfs_runtime_handle_count(openfs_runtime_t*r,const void*d,uint64_t ino,uint64_t generation){
    if(r==NULL||!r->initialized||d==NULL)return 0U;
    if(openfs_mutex_lock(&r->handle_registry_lock,OPENFS_LOCK_RANK_INODE)!=OPENFS_LOCK_OK)return UINT64_MAX;
    uint64_t count=0U;for(openfs_handle_entry_t*e=entries(r);e!=NULL;e=e->next)if(e->device==d&&e->inode_number==ino&&e->generation==generation){count=e->references;break;}
    (void)openfs_mutex_unlock(&r->handle_registry_lock);return count;
}
uint64_t openfs_runtime_handle_count_all(openfs_runtime_t*r){
    if(r==NULL||!r->initialized)return 0U;
    if(openfs_mutex_lock(&r->handle_registry_lock,OPENFS_LOCK_RANK_INODE)!=OPENFS_LOCK_OK)return UINT64_MAX;
    uint64_t count=0U;for(openfs_handle_entry_t*e=entries(r);e!=NULL;e=e->next){if(UINT64_MAX-count<e->references){count=UINT64_MAX;break;}count+=e->references;}
    (void)openfs_mutex_unlock(&r->handle_registry_lock);return count;
}
int openfs_runtime_init(openfs_runtime_t*r){
    if(r==NULL)return 0;memset(r,0,sizeof(*r));
    if(openfs_mutex_init(&r->lifecycle_lock)!=OPENFS_LOCK_OK)goto fail0;
    if(openfs_mutex_init(&r->directory_lock)!=OPENFS_LOCK_OK)goto fail1;
    if(openfs_mutex_init(&r->inode_lock)!=OPENFS_LOCK_OK)goto fail2;
    if(openfs_mutex_init(&r->allocation_lock)!=OPENFS_LOCK_OK)goto fail3;
    if(openfs_mutex_init(&r->journal_lock)!=OPENFS_LOCK_OK)goto fail4;
    if(openfs_mutex_init(&r->handle_registry_lock)!=OPENFS_LOCK_OK)goto fail5;
    if(openfs_mutex_init(&r->file_lock_registry_lock)!=OPENFS_LOCK_OK)goto fail6;
    bind_mutex(&r->directory_lock,r);bind_mutex(&r->inode_lock,r);bind_mutex(&r->allocation_lock,r);bind_mutex(&r->journal_lock,r);bind_mutex(&r->handle_registry_lock,r);bind_mutex(&r->file_lock_registry_lock,r);
    r->accepting=1;r->initialized=1;return 1;
fail6:(void)openfs_mutex_destroy(&r->handle_registry_lock);
fail5:(void)openfs_mutex_destroy(&r->journal_lock);
fail4:(void)openfs_mutex_destroy(&r->allocation_lock);
fail3:(void)openfs_mutex_destroy(&r->inode_lock);
fail2:(void)openfs_mutex_destroy(&r->directory_lock);
fail1:(void)openfs_mutex_destroy(&r->lifecycle_lock);
fail0:return 0;
}
void openfs_runtime_destroy(openfs_runtime_t*r){
    if(r==NULL||!r->initialized)return;
    if(openfs_mutex_lock(&r->lifecycle_lock,OPENFS_LOCK_RANK_HANDLE)!=OPENFS_LOCK_OK)return;
    r->accepting=0;
    (void)openfs_mutex_unlock(&r->lifecycle_lock);
    for(;;){
        if(openfs_mutex_lock(&r->lifecycle_lock,OPENFS_LOCK_RANK_HANDLE)!=OPENFS_LOCK_OK)return;
        uint64_t active=r->active_users;
        (void)openfs_mutex_unlock(&r->lifecycle_lock);
        if(active==0U)break;
    }
    r->initialized=0;
    unbind_mutex(&r->directory_lock);unbind_mutex(&r->inode_lock);unbind_mutex(&r->allocation_lock);unbind_mutex(&r->journal_lock);unbind_mutex(&r->handle_registry_lock);unbind_mutex(&r->file_lock_registry_lock);
    openfs_handle_entry_t *e=entries(r);while(e!=NULL){openfs_handle_entry_t*n=e->next;free(e);e=n;}
    r->open_handles=NULL;r->file_locks=NULL;
    (void)openfs_mutex_destroy(&r->file_lock_registry_lock);
    (void)openfs_mutex_destroy(&r->handle_registry_lock);
    (void)openfs_mutex_destroy(&r->journal_lock);
    (void)openfs_mutex_destroy(&r->allocation_lock);
    (void)openfs_mutex_destroy(&r->inode_lock);
    (void)openfs_mutex_destroy(&r->directory_lock);
    (void)openfs_mutex_destroy(&r->lifecycle_lock);
    r->initialized=0;
}
