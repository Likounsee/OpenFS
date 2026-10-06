#include "openfs/runtime.h"
#include <stdlib.h>
#include <string.h>

typedef struct openfs_handle_entry {
    const void *device;
    uint64_t inode_number;
    uint64_t generation;
    uint64_t references;
    struct openfs_handle_entry *next;
} openfs_handle_entry_t;

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

int openfs_runtime_init(openfs_runtime_t*r){if(r==NULL)return 0;memset(r,0,sizeof(*r));if(openfs_mutex_init(&r->directory_lock)!=OPENFS_LOCK_OK)goto fail0;if(openfs_mutex_init(&r->inode_lock)!=OPENFS_LOCK_OK)goto fail1;if(openfs_mutex_init(&r->allocation_lock)!=OPENFS_LOCK_OK)goto fail2;if(openfs_mutex_init(&r->journal_lock)!=OPENFS_LOCK_OK)goto fail3;if(openfs_mutex_init(&r->handle_registry_lock)!=OPENFS_LOCK_OK)goto fail4;r->initialized=1;return 1;fail4:(void)openfs_mutex_destroy(&r->journal_lock);fail3:(void)openfs_mutex_destroy(&r->allocation_lock);fail2:(void)openfs_mutex_destroy(&r->inode_lock);fail1:(void)openfs_mutex_destroy(&r->directory_lock);fail0:return 0;}
void openfs_runtime_destroy(openfs_runtime_t*r){if(r==NULL||!r->initialized)return;openfs_handle_entry_t *e=entries(r);while(e!=NULL){openfs_handle_entry_t*n=e->next;free(e);e=n;}r->open_handles=NULL;(void)openfs_mutex_destroy(&r->handle_registry_lock);(void)openfs_mutex_destroy(&r->journal_lock);(void)openfs_mutex_destroy(&r->allocation_lock);(void)openfs_mutex_destroy(&r->inode_lock);(void)openfs_mutex_destroy(&r->directory_lock);r->initialized=0;}
