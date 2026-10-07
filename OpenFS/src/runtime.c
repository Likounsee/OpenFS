#include "openfs/runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static _Thread_local openfs_runtime_t *tls_runtime;
static _Thread_local unsigned tls_runtime_depth;

static atomic_flag runtime_lifecycle_guard = ATOMIC_FLAG_INIT;

typedef struct runtime_registry_entry {
    openfs_runtime_t *runtime;
    struct runtime_registry_entry *next;
} runtime_registry_entry_t;

static runtime_registry_entry_t *runtime_registry;

static void runtime_lifecycle_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&runtime_lifecycle_guard, memory_order_acquire)) {
    }
}

static void runtime_lifecycle_unlock(void)
{
    atomic_flag_clear_explicit(&runtime_lifecycle_guard, memory_order_release);
}

static int runtime_registry_contains(openfs_runtime_t *r)
{
    for (runtime_registry_entry_t *e = runtime_registry; e != NULL; e = e->next) {
        if (e->runtime == r) return 1;
    }
    return 0;
}

static void runtime_registry_remove(openfs_runtime_t *r)
{
    runtime_registry_entry_t **pp = &runtime_registry;
    while (*pp != NULL) {
        if ((*pp)->runtime == r) {
            runtime_registry_entry_t *old = *pp;
            *pp = old->next;
            free(old);
            return;
        }
        pp = &(*pp)->next;
    }
}

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

typedef struct openfs_retired_handle_entry {
    void *handle;
    struct openfs_retired_handle_entry *next;
} openfs_retired_handle_entry_t;
static openfs_handle_entry_t *entries(openfs_runtime_t *r){return (openfs_handle_entry_t *)r->open_handles;}
int openfs_runtime_handle_acquire(openfs_runtime_t*r,const void*d,uint64_t ino,uint64_t generation){
    if(r==NULL||d==NULL||ino==0U||generation==0U)return 0;
    if(!openfs_runtime_enter(r))return 0;
    if(openfs_mutex_lock(&r->handle_registry_lock,OPENFS_LOCK_RANK_REGISTRY)!=OPENFS_LOCK_OK){openfs_runtime_leave(r);return 0;}
    openfs_handle_entry_t *e=entries(r);
    while(e!=NULL){
        if(e->device==d&&e->inode_number==ino&&e->generation==generation){
            if(e->references==UINT64_MAX){(void)openfs_mutex_unlock(&r->handle_registry_lock);openfs_runtime_leave(r);return 0;}
            e->references++;
            (void)openfs_mutex_unlock(&r->handle_registry_lock);
            openfs_runtime_leave(r);
            return 1;
        }
        e=e->next;
    }
    e=(openfs_handle_entry_t*)calloc(1,sizeof(*e));
    if(e==NULL){(void)openfs_mutex_unlock(&r->handle_registry_lock);openfs_runtime_leave(r);return 0;}
    e->device=d;e->inode_number=ino;e->generation=generation;e->references=1U;e->next=entries(r);r->open_handles=e;
    (void)openfs_mutex_unlock(&r->handle_registry_lock);
    openfs_runtime_leave(r);
    return 1;
}
int openfs_runtime_handle_release(openfs_runtime_t*r,const void*d,uint64_t ino,uint64_t generation){
    if(r==NULL||d==NULL||ino==0U||generation==0U)return 0;
    if(!openfs_runtime_enter(r))return 0;
    if(openfs_mutex_lock(&r->handle_registry_lock,OPENFS_LOCK_RANK_REGISTRY)!=OPENFS_LOCK_OK){openfs_runtime_leave(r);return 0;}
    openfs_handle_entry_t **pp=(openfs_handle_entry_t**)&r->open_handles;
    while(*pp!=NULL){
        openfs_handle_entry_t *e=*pp;
        if(e->device==d&&e->inode_number==ino&&e->generation==generation){
            if(e->references==0U){(void)openfs_mutex_unlock(&r->handle_registry_lock);openfs_runtime_leave(r);return 0;}
            e->references--;
            if(e->references==0U){*pp=e->next;free(e);}
            (void)openfs_mutex_unlock(&r->handle_registry_lock);
            openfs_runtime_leave(r);
            return 1;
        }
        pp=&e->next;
    }
    (void)openfs_mutex_unlock(&r->handle_registry_lock);
    openfs_runtime_leave(r);
    return 0;
}
uint64_t openfs_runtime_handle_count(openfs_runtime_t*r,const void*d,uint64_t ino,uint64_t generation){
    if(r==NULL||d==NULL)return 0U;
    if(!openfs_runtime_enter(r))return UINT64_MAX;
    if(openfs_mutex_lock(&r->handle_registry_lock,OPENFS_LOCK_RANK_REGISTRY)!=OPENFS_LOCK_OK){openfs_runtime_leave(r);return UINT64_MAX;}
    uint64_t count=0U;for(openfs_handle_entry_t*e=entries(r);e!=NULL;e=e->next)if(e->device==d&&e->inode_number==ino&&e->generation==generation){count=e->references;break;}
    (void)openfs_mutex_unlock(&r->handle_registry_lock);openfs_runtime_leave(r);return count;
}
uint64_t openfs_runtime_handle_count_all(openfs_runtime_t*r){
    if(r==NULL)return 0U;
    if(!openfs_runtime_enter(r))return UINT64_MAX;
    if(openfs_mutex_lock(&r->handle_registry_lock,OPENFS_LOCK_RANK_REGISTRY)!=OPENFS_LOCK_OK){openfs_runtime_leave(r);return UINT64_MAX;}
    uint64_t count=0U;
    for(openfs_handle_entry_t*e=entries(r);e!=NULL;e=e->next){
        if(UINT64_MAX-count<e->references){count=UINT64_MAX;break;}
        count+=e->references;
    }
    (void)openfs_mutex_unlock(&r->handle_registry_lock);
    openfs_runtime_leave(r);
    return count;
}
int openfs_runtime_retire_handle(openfs_runtime_t*r,void*handle)
{
    if(r==NULL||handle==NULL)return 0;
    if(!openfs_runtime_enter(r))return 0;
    if(openfs_mutex_lock(&r->handle_registry_lock,OPENFS_LOCK_RANK_REGISTRY)!=OPENFS_LOCK_OK){
        openfs_runtime_leave(r);
        return 0;
    }
    openfs_retired_handle_entry_t *entry=(openfs_retired_handle_entry_t*)calloc(1U,sizeof(*entry));
    if(entry==NULL){
        (void)openfs_mutex_unlock(&r->handle_registry_lock);
        openfs_runtime_leave(r);
        return 0;
    }
    entry->handle=handle;
    entry->next=(openfs_retired_handle_entry_t*)r->retired_handles;
    r->retired_handles=entry;
    (void)openfs_mutex_unlock(&r->handle_registry_lock);
    openfs_runtime_leave(r);
    return 1;
}
int openfs_runtime_init(openfs_runtime_t*r)
{
    if(r==NULL)return 0;
    runtime_lifecycle_lock();
    if(runtime_registry_contains(r)){
        runtime_lifecycle_unlock();
        return 0;
    }
    runtime_registry_entry_t *entry=(runtime_registry_entry_t*)calloc(1U,sizeof(*entry));
    if(entry==NULL){
        runtime_lifecycle_unlock();
        return 0;
    }
    entry->runtime=r;
    memset(r,0,sizeof(*r));
    if(openfs_mutex_init(&r->lifecycle_lock)!=OPENFS_LOCK_OK)goto fail0;
    if(openfs_mutex_init(&r->directory_lock)!=OPENFS_LOCK_OK)goto fail1;
    if(openfs_mutex_init(&r->inode_lock)!=OPENFS_LOCK_OK)goto fail2;
    if(openfs_mutex_init(&r->allocation_lock)!=OPENFS_LOCK_OK)goto fail3;
    if(openfs_mutex_init(&r->journal_lock)!=OPENFS_LOCK_OK)goto fail4;
    if(openfs_mutex_init(&r->handle_registry_lock)!=OPENFS_LOCK_OK)goto fail5;
    if(openfs_mutex_init(&r->file_lock_registry_lock)!=OPENFS_LOCK_OK)goto fail6;
    bind_mutex(&r->directory_lock,r);bind_mutex(&r->inode_lock,r);bind_mutex(&r->allocation_lock,r);bind_mutex(&r->journal_lock,r);bind_mutex(&r->handle_registry_lock,r);bind_mutex(&r->file_lock_registry_lock,r);
    r->accepting=1;r->initialized=1;r->destroying=0;
    entry->next=runtime_registry;
    runtime_registry=entry;
    runtime_lifecycle_unlock();
    return 1;
fail6:(void)openfs_mutex_destroy(&r->handle_registry_lock);
fail5:(void)openfs_mutex_destroy(&r->journal_lock);
fail4:(void)openfs_mutex_destroy(&r->allocation_lock);
fail3:(void)openfs_mutex_destroy(&r->inode_lock);
fail2:(void)openfs_mutex_destroy(&r->directory_lock);
fail1:(void)openfs_mutex_destroy(&r->lifecycle_lock);
fail0:
    free(entry);
    memset(r,0,sizeof(*r));
    runtime_lifecycle_unlock();
    return 0;
}
static int runtime_shutdown_internal(openfs_runtime_t*r,int require_unused)
{
    if(r==NULL||tls_runtime==r)return 0;
    runtime_lifecycle_lock();
    if(!runtime_registry_contains(r)){
        runtime_lifecycle_unlock();
        return 0;
    }
    if(openfs_mutex_lock(&r->lifecycle_lock,OPENFS_LOCK_RANK_HANDLE)!=OPENFS_LOCK_OK){
        runtime_lifecycle_unlock();
        return 0;
    }
    if(!r->initialized||r->destroying){
        (void)openfs_mutex_unlock(&r->lifecycle_lock);
        runtime_lifecycle_unlock();
        return 0;
    }
    r->destroying=1;
    r->accepting=0;
    (void)openfs_mutex_unlock(&r->lifecycle_lock);

    for(;;){
        if(openfs_mutex_lock(&r->lifecycle_lock,OPENFS_LOCK_RANK_HANDLE)!=OPENFS_LOCK_OK){
            runtime_lifecycle_unlock();
            return 0;
        }
        uint64_t active=r->active_users;
        (void)openfs_mutex_unlock(&r->lifecycle_lock);
        if(active==0U)break;
    }

    if(require_unused){
        if(openfs_mutex_lock(&r->handle_registry_lock,OPENFS_LOCK_RANK_REGISTRY)!=OPENFS_LOCK_OK){
            runtime_lifecycle_unlock();
            return 0;
        }
        uint64_t handles=0U;
        for(openfs_handle_entry_t*e=entries(r);e!=NULL;e=e->next){
            if(UINT64_MAX-handles<e->references){handles=UINT64_MAX;break;}
            handles+=e->references;
        }
        (void)openfs_mutex_unlock(&r->handle_registry_lock);
        if(handles!=0U){
            if(openfs_mutex_lock(&r->lifecycle_lock,OPENFS_LOCK_RANK_HANDLE)==OPENFS_LOCK_OK){
                r->destroying=0;
                r->accepting=1;
                (void)openfs_mutex_unlock(&r->lifecycle_lock);
            }
            runtime_lifecycle_unlock();
            return 0;
        }
    }

    if(openfs_mutex_lock(&r->lifecycle_lock,OPENFS_LOCK_RANK_HANDLE)!=OPENFS_LOCK_OK){
        runtime_lifecycle_unlock();
        return 0;
    }
    r->initialized=0;
    (void)openfs_mutex_unlock(&r->lifecycle_lock);

    unbind_mutex(&r->directory_lock);unbind_mutex(&r->inode_lock);unbind_mutex(&r->allocation_lock);unbind_mutex(&r->journal_lock);unbind_mutex(&r->handle_registry_lock);unbind_mutex(&r->file_lock_registry_lock);
    openfs_handle_entry_t *e=entries(r);while(e!=NULL){openfs_handle_entry_t*n=e->next;free(e);e=n;}
    r->open_handles=NULL;r->file_locks=NULL;
    openfs_retired_handle_entry_t *retired=(openfs_retired_handle_entry_t*)r->retired_handles;
    while(retired!=NULL){
        openfs_retired_handle_entry_t *next=retired->next;
        free(retired->handle);
        free(retired);
        retired=next;
    }
    r->retired_handles=NULL;
    (void)openfs_mutex_destroy(&r->file_lock_registry_lock);
    (void)openfs_mutex_destroy(&r->handle_registry_lock);
    (void)openfs_mutex_destroy(&r->journal_lock);
    (void)openfs_mutex_destroy(&r->allocation_lock);
    (void)openfs_mutex_destroy(&r->inode_lock);
    (void)openfs_mutex_destroy(&r->directory_lock);
    (void)openfs_mutex_destroy(&r->lifecycle_lock);
    runtime_registry_remove(r);
    runtime_lifecycle_unlock();
    return 1;
}

int openfs_runtime_shutdown_if_unused(openfs_runtime_t*r)
{
    return runtime_shutdown_internal(r,1);
}

void openfs_runtime_destroy(openfs_runtime_t*r)
{
    (void)runtime_shutdown_internal(r,0);
}
