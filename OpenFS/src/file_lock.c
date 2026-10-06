#include "openfs/file_lock.h"
#include "openfs/runtime.h"
#include "openfs/lock.h"
#include <limits.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif
typedef struct openfs_file_lock_entry {
    const void *device;
    uint64_t inode_number;
    uint64_t generation;
    const openfs_file_handle_t *owner;
    uint64_t start;
    uint64_t length;
    uint32_t type;
    struct openfs_file_lock_entry *next;
} openfs_file_lock_entry_t;
static openfs_file_lock_entry_t *entries(openfs_runtime_t *r){return (openfs_file_lock_entry_t *)r->file_locks;}
static void range_end(uint64_t start,uint64_t length,uint64_t *end){
    if(length==0U||UINT64_MAX-start<length)*end=UINT64_MAX;else *end=start+length;
}
static int overlaps(uint64_t a_start,uint64_t a_len,uint64_t b_start,uint64_t b_len){
    uint64_t ae=0U,be=0U;range_end(a_start,a_len,&ae);range_end(b_start,b_len,&be);
    return a_start<be&&b_start<ae;
}
static int conflicts(const openfs_file_lock_entry_t *e,const openfs_file_handle_t *h,uint64_t start,uint64_t length,uint32_t type){
    if(e->device!=h->device||e->inode_number!=h->inode.inode_number||e->generation!=h->inode.generation||e->owner==h)return 0;
    return overlaps(e->start,e->length,start,length)&&(e->type==OPENFS_FILE_LOCK_EXCLUSIVE||type==OPENFS_FILE_LOCK_EXCLUSIVE);
}
static int handle_open(const openfs_file_handle_t *h){
    return h!=NULL&&h->lock_initialized&&!h->closed&&h->references!=0U&&h->superblock.runtime!=NULL&&h->superblock.runtime->initialized;
}
static openfs_file_lock_result_t registry_lock(openfs_file_handle_t *h){
    if(!handle_open(h))return OPENFS_FILE_LOCK_CLOSED;
    return openfs_mutex_lock(&h->superblock.runtime->file_lock_registry_lock,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK?OPENFS_FILE_LOCK_OK:OPENFS_FILE_LOCK_IO_ERROR;
}
static void registry_unlock(openfs_file_handle_t *h){(void)openfs_mutex_unlock(&h->superblock.runtime->file_lock_registry_lock);}
openfs_file_lock_result_t openfs_file_lock(openfs_file_handle_t *h,uint64_t start,uint64_t length,uint32_t type,uint32_t flags){
    if(!handle_open(h))return OPENFS_FILE_LOCK_CLOSED;
    if(type!=OPENFS_FILE_LOCK_SHARED&&type!=OPENFS_FILE_LOCK_EXCLUSIVE)return OPENFS_FILE_LOCK_INVALID_ARGUMENT;
    if((flags&~OPENFS_FILE_LOCK_BLOCK)!=0U)return OPENFS_FILE_LOCK_INVALID_ARGUMENT;
    for(;;){
        openfs_file_lock_result_t lr=registry_lock(h);if(lr!=OPENFS_FILE_LOCK_OK)return lr;
        int conflict=0;
        for(openfs_file_lock_entry_t *e=entries(h->superblock.runtime);e!=NULL;e=e->next)if(conflicts(e,h,start,length,type)){conflict=1;break;}
        if(!conflict){
            openfs_file_lock_entry_t *e=calloc(1,sizeof(*e));
            if(e==NULL){registry_unlock(h);return OPENFS_FILE_LOCK_IO_ERROR;}
            e->device=h->device;e->inode_number=h->inode.inode_number;e->generation=h->inode.generation;
            e->owner=h;e->start=start;e->length=length;e->type=type;e->next=entries(h->superblock.runtime);
            h->superblock.runtime->file_locks=e;registry_unlock(h);return OPENFS_FILE_LOCK_OK;
        }
        registry_unlock(h);
        if((flags&OPENFS_FILE_LOCK_BLOCK)==0U)return OPENFS_FILE_LOCK_CONFLICT;
#ifdef _WIN32
        Sleep(1);
#else
        sched_yield();
#endif
    }
}
openfs_file_lock_result_t openfs_file_unlock(openfs_file_handle_t *h,uint64_t start,uint64_t length){
    if(!handle_open(h))return OPENFS_FILE_LOCK_CLOSED;
    openfs_file_lock_result_t lr=registry_lock(h);if(lr!=OPENFS_FILE_LOCK_OK)return lr;
    openfs_file_lock_entry_t **pp=(openfs_file_lock_entry_t **)&h->superblock.runtime->file_locks;
    while(*pp!=NULL){
        openfs_file_lock_entry_t *e=*pp;
        if(e->owner==h&&e->device==h->device&&e->inode_number==h->inode.inode_number&&e->generation==h->inode.generation&&e->start==start&&e->length==length){
            *pp=e->next;free(e);registry_unlock(h);return OPENFS_FILE_LOCK_OK;
        }
        pp=&e->next;
    }
    registry_unlock(h);return OPENFS_FILE_LOCK_NOT_FOUND;
}
openfs_file_lock_result_t openfs_file_lock_test(openfs_file_handle_t *h,uint64_t start,uint64_t length,uint32_t *conflict_type){
    if(conflict_type==NULL)return OPENFS_FILE_LOCK_INVALID_ARGUMENT;*conflict_type=0U;
    if(!handle_open(h))return OPENFS_FILE_LOCK_CLOSED;
    openfs_file_lock_result_t lr=registry_lock(h);if(lr!=OPENFS_FILE_LOCK_OK)return lr;
    for(openfs_file_lock_entry_t *e=entries(h->superblock.runtime);e!=NULL;e=e->next){
        if(e->device==h->device&&e->inode_number==h->inode.inode_number&&e->generation==h->inode.generation&&e->owner!=h&&overlaps(e->start,e->length,start,length)){
            *conflict_type=e->type;registry_unlock(h);return OPENFS_FILE_LOCK_CONFLICT;
        }
    }
    registry_unlock(h);return OPENFS_FILE_LOCK_OK;
}
void openfs_file_lock_release_all(openfs_file_handle_t *h){
    if(!handle_open(h))return;
    if(registry_lock(h)!=OPENFS_FILE_LOCK_OK)return;
    openfs_file_lock_entry_t **pp=(openfs_file_lock_entry_t **)&h->superblock.runtime->file_locks;
    while(*pp!=NULL){openfs_file_lock_entry_t *e=*pp;if(e->owner==h){*pp=e->next;free(e);continue;}pp=&e->next;}
    registry_unlock(h);
}
