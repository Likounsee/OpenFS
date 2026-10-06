#include "openfs/fd.h"
#include "openfs/path.h"
#include "openfs/runtime.h"
#include "openfs/acl.h"
#include "openfs/orphan.h"
#include "openfs/file_lock.h"
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static openfs_fd_result_t map_path(openfs_path_result_t r){switch(r){case OPENFS_PATH_OK:return OPENFS_FD_OK;case OPENFS_PATH_NOT_FOUND:return OPENFS_FD_NOT_FOUND;case OPENFS_PATH_EXISTS:return OPENFS_FD_EXISTS;case OPENFS_PATH_ACCESS_DENIED:return OPENFS_FD_ACCESS_DENIED;case OPENFS_PATH_CORRUPT:return OPENFS_FD_CORRUPT;default:return OPENFS_FD_IO_ERROR;}}
static openfs_fd_result_t inode_count(const openfs_superblock_t*s,uint64_t*out){if(s==NULL||out==NULL||s->block_size==0U)return OPENFS_FD_CORRUPT;if(s->inode_table_blocks>UINT64_MAX/s->block_size)return OPENFS_FD_CORRUPT;*out=(s->inode_table_blocks*s->block_size)/OPENFS_INODE_SIZE;return *out==0U?OPENFS_FD_CORRUPT:OPENFS_FD_OK;}
static openfs_fd_result_t load_inode(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t ino,openfs_inode_t*out){uint64_t count=0U;if(inode_count(s,&count)!=OPENFS_FD_OK)return OPENFS_FD_CORRUPT;openfs_inode_result_t r=openfs_inode_read(d,s->inode_table_start,ino,count,out);if(r==OPENFS_INODE_OK)return OPENFS_FD_OK;return r==OPENFS_INODE_CORRUPT?OPENFS_FD_CORRUPT:OPENFS_FD_IO_ERROR;}
static int can_read(uint32_t f){return (f&OPENFS_FD_ACCESS_MASK)!=OPENFS_FD_WRONLY;}
static int can_write(uint32_t f){uint32_t a=f&OPENFS_FD_ACCESS_MASK;return a==OPENFS_FD_WRONLY||a==OPENFS_FD_RDWR;}
static openfs_fd_result_t validate_flags(uint32_t f){uint32_t a=f&OPENFS_FD_ACCESS_MASK;if(a>OPENFS_FD_RDWR)return OPENFS_FD_INVALID_ARGUMENT;if((f&OPENFS_FD_EXCL)!=0U&&(f&OPENFS_FD_CREAT)==0U)return OPENFS_FD_INVALID_ARGUMENT;if((f&OPENFS_FD_TRUNC)!=0U&&!can_write(f))return OPENFS_FD_ACCESS_DENIED;return OPENFS_FD_OK;}
static openfs_fd_result_t handle_lock(openfs_file_handle_t*h){if(h==NULL||!h->lock_initialized)return OPENFS_FD_INVALID_ARGUMENT;return openfs_mutex_lock(&h->lock,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK?OPENFS_FD_OK:OPENFS_FD_IO_ERROR;}
static void destroy_handle(openfs_file_handle_t*h){if(h==NULL)return;if(h->lock_initialized){(void)openfs_mutex_destroy(&h->lock);h->lock_initialized=0;}free(h);}
static openfs_fd_result_t open_common(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t flags,uint32_t mode,uint32_t uid,uint32_t gid,int credentials,openfs_file_handle_t**out){
if(out==NULL||!openfs_block_device_is_valid(d)||s==NULL||p==NULL)return OPENFS_FD_INVALID_ARGUMENT;*out=NULL;openfs_fd_result_t vr=validate_flags(flags);if(vr!=OPENFS_FD_OK)return vr;
uint32_t create_mode=(mode&OPENFS_INODE_TYPE_MASK)==0U?(OPENFS_INODE_MODE_REGULAR|(mode&OPENFS_INODE_PERMISSION_MASK)):mode;uint64_t ino=0U;openfs_path_result_t lr=credentials?openfs_path_lookup_as(d,s,p,uid,gid,&ino):openfs_path_lookup(d,s,p,&ino);
if(lr==OPENFS_PATH_NOT_FOUND&&(flags&OPENFS_FD_CREAT)!=0U){openfs_path_result_t cr=credentials?openfs_path_create_as(d,s,p,create_mode,uid,gid,&ino):openfs_path_create(d,s,p,create_mode,&ino);if(cr!=OPENFS_PATH_OK){fprintf(stderr,"fd create path result=%d\\n",(int)cr);return map_path(cr);}}
else if(lr!=OPENFS_PATH_OK)return map_path(lr);
else if((flags&(OPENFS_FD_EXCL|OPENFS_FD_CREAT))==(OPENFS_FD_EXCL|OPENFS_FD_CREAT))return OPENFS_FD_EXISTS;
openfs_inode_t inode;openfs_fd_result_t ir=load_inode(d,s,ino,&inode);if(ir!=OPENFS_FD_OK){fprintf(stderr,"fd load inode result=%d\\n",(int)ir);return ir;}if((inode.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_FREE)return OPENFS_FD_NOT_FOUND;
if(credentials){uint8_t need=can_write(flags)?(can_read(flags)?6U:2U):4U;openfs_acl_result_t ar=openfs_acl_check_access(d,s,&inode,uid,gid,need);if(ar!=OPENFS_ACL_OK)return ar==OPENFS_ACL_ACCESS_DENIED?OPENFS_FD_ACCESS_DENIED:OPENFS_FD_CORRUPT;}
openfs_file_handle_t*h=calloc(1,sizeof(*h));if(h==NULL)return OPENFS_FD_IO_ERROR;h->device=d;h->superblock=*s;h->inode=inode;h->flags=flags;h->references=1U;
if(openfs_mutex_init(&h->lock)!=OPENFS_LOCK_OK){fprintf(stderr,"fd handle mutex init failed\\n");free(h);return OPENFS_FD_IO_ERROR;}h->lock_initialized=1;
if(s->runtime!=NULL&&s->runtime->initialized&&!openfs_runtime_handle_acquire(s->runtime,d,inode.inode_number,inode.generation)){fprintf(stderr,"fd registry acquire failed ino=%llu gen=%llu\\n",(unsigned long long)inode.inode_number,(unsigned long long)inode.generation);destroy_handle(h);return OPENFS_FD_IO_ERROR;}
if((flags&OPENFS_FD_TRUNC)!=0U){openfs_file_result_t fr=openfs_file_truncate(d,&h->superblock,&h->inode,0U);if(fr!=OPENFS_FILE_OK){if(s->runtime!=NULL&&s->runtime->initialized)(void)openfs_runtime_handle_release(s->runtime,d,inode.inode_number,inode.generation);destroy_handle(h);return fr==OPENFS_FILE_ACCESS_DENIED?OPENFS_FD_ACCESS_DENIED:fr==OPENFS_FILE_CORRUPT?OPENFS_FD_CORRUPT:OPENFS_FD_IO_ERROR;}}
*out=h;return OPENFS_FD_OK;}
openfs_fd_result_t openfs_fd_open(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t flags,uint32_t mode,openfs_file_handle_t**out){return open_common(d,s,p,flags,mode,0U,0U,0,out);}
openfs_fd_result_t openfs_fd_open_as(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t flags,uint32_t mode,uint32_t uid,uint32_t gid,openfs_file_handle_t**out){return open_common(d,s,p,flags,mode,uid,gid,1,out);}
openfs_fd_result_t openfs_fd_retain(openfs_file_handle_t*h){openfs_fd_result_t r=handle_lock(h);if(r!=OPENFS_FD_OK)return r;if(h->closed||h->references==0U){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_CLOSED;}if(h->references==UINT32_MAX){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_IO_ERROR;}h->references++;(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_OK;}
openfs_fd_result_t openfs_fd_close(openfs_file_handle_t*h){if(h==NULL||!h->lock_initialized)return OPENFS_FD_INVALID_ARGUMENT;if(openfs_mutex_lock(&h->lock,OPENFS_LOCK_RANK_INODE)!=OPENFS_LOCK_OK)return OPENFS_FD_IO_ERROR;if(h->closed||h->references==0U){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_CLOSED;}h->references--;if(h->references!=0U){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_OK;}h->closed=1;
    openfs_runtime_t *runtime=h->superblock.runtime;
    openfs_block_device_t *device=h->device;
    uint64_t inode_number=h->inode.inode_number;
    uint64_t generation=h->inode.generation;
    (void)openfs_mutex_unlock(&h->lock);
    openfs_file_lock_release_all(h);
    (void)openfs_mutex_destroy(&h->lock);h->lock_initialized=0;
    int registry_released=1;
    if(runtime!=NULL&&runtime->initialized)registry_released=openfs_runtime_handle_release(runtime,device,inode_number,generation);
    if(registry_released&&runtime!=NULL&&runtime->initialized){
        openfs_inode_t current;
        uint64_t count=0U;
        if(inode_count(&h->superblock,&count)==OPENFS_FD_OK&&
           openfs_inode_read(device,h->superblock.inode_table_start,inode_number,count,&current)==OPENFS_INODE_OK&&
           (current.flags&OPENFS_INODE_FLAG_ORPHAN)!=0U&&current.generation==generation){
            (void)openfs_orphan_reclaim(device,&h->superblock,inode_number);
        }
    }
    free(h);return OPENFS_FD_OK;}
openfs_fd_result_t openfs_fd_dup(openfs_file_handle_t*h,openfs_file_handle_t**out){if(out==NULL)return OPENFS_FD_INVALID_ARGUMENT;*out=NULL;openfs_fd_result_t r=handle_lock(h);if(r!=OPENFS_FD_OK)return r;if(h->closed||h->references==0U){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_CLOSED;}if(h->references==UINT32_MAX){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_IO_ERROR;}h->references++;*out=h;(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_OK;}
openfs_fd_result_t openfs_fd_read(openfs_file_handle_t*h,void*b,size_t n,size_t*got){if(got==NULL)return OPENFS_FD_INVALID_ARGUMENT;*got=0U;openfs_fd_result_t r=handle_lock(h);if(r!=OPENFS_FD_OK)return r;if(h->closed||h->references==0U){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_CLOSED;}if(!can_read(h->flags)){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_ACCESS_DENIED;}openfs_file_result_t fr=openfs_file_read(h->device,&h->superblock,&h->inode,h->offset,b,n,got);if(fr==OPENFS_FILE_OK)h->offset+=(uint64_t)*got;(void)openfs_mutex_unlock(&h->lock);return fr==OPENFS_FILE_OK?OPENFS_FD_OK:fr==OPENFS_FILE_ACCESS_DENIED?OPENFS_FD_ACCESS_DENIED:fr==OPENFS_FILE_CORRUPT?OPENFS_FD_CORRUPT:OPENFS_FD_IO_ERROR;}
openfs_fd_result_t openfs_fd_write(openfs_file_handle_t*h,const void*b,size_t n){openfs_fd_result_t r=handle_lock(h);if(r!=OPENFS_FD_OK)return r;if(h->closed||h->references==0U){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_CLOSED;}if(!can_write(h->flags)){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_ACCESS_DENIED;}uint64_t off=(h->flags&OPENFS_FD_APPEND)!=0U?h->inode.size:h->offset;openfs_file_result_t fr=openfs_file_write(h->device,&h->superblock,&h->inode,off,b,n);if(fr==OPENFS_FILE_OK)h->offset=off+(uint64_t)n;(void)openfs_mutex_unlock(&h->lock);return fr==OPENFS_FILE_OK?OPENFS_FD_OK:fr==OPENFS_FILE_CORRUPT?OPENFS_FD_CORRUPT:fr==OPENFS_FILE_ACCESS_DENIED?OPENFS_FD_ACCESS_DENIED:OPENFS_FD_IO_ERROR;}
openfs_fd_result_t openfs_fd_seek(openfs_file_handle_t*h,int64_t off,int whence,int64_t*out){if(out==NULL)return OPENFS_FD_INVALID_ARGUMENT;openfs_fd_result_t r=handle_lock(h);if(r!=OPENFS_FD_OK)return r;if(h->closed||h->references==0U){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_CLOSED;}uint64_t base=whence==0?0U:whence==1?h->offset:whence==2?h->inode.size:UINT64_MAX;if(base==UINT64_MAX||(off<0&&((uint64_t)(-(off+1))+1U)>base)||(off>=0&&(uint64_t)off>UINT64_MAX-base)){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_BAD_SEEK;}uint64_t next=off<0?base-((uint64_t)(-(off+1))+1U):base+(uint64_t)off;if(next>INT64_MAX){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_BAD_SEEK;}h->offset=next;*out=(int64_t)next;(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_OK;}
openfs_fd_result_t openfs_fd_truncate(openfs_file_handle_t*h,uint64_t size){openfs_fd_result_t r=handle_lock(h);if(r!=OPENFS_FD_OK)return r;if(h->closed||h->references==0U){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_CLOSED;}if(!can_write(h->flags)){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_ACCESS_DENIED;}openfs_file_result_t fr=openfs_file_truncate(h->device,&h->superblock,&h->inode,size);if(fr==OPENFS_FILE_OK&&h->offset>size)h->offset=size;(void)openfs_mutex_unlock(&h->lock);return fr==OPENFS_FILE_OK?OPENFS_FD_OK:fr==OPENFS_FILE_CORRUPT?OPENFS_FD_CORRUPT:OPENFS_FD_IO_ERROR;}
openfs_fd_result_t openfs_fd_stat(openfs_file_handle_t*h,openfs_inode_t*out){if(out==NULL)return OPENFS_FD_INVALID_ARGUMENT;openfs_fd_result_t r=handle_lock(h);if(r!=OPENFS_FD_OK)return r;if(h->closed||h->references==0U){(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_CLOSED;}*out=h->inode;(void)openfs_mutex_unlock(&h->lock);return OPENFS_FD_OK;}
uint64_t openfs_fd_inode_number(const openfs_file_handle_t*h){return h==NULL?0U:h->inode.inode_number;}
