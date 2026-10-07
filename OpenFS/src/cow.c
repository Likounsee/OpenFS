#include "openfs/cow.h"
#include <stdlib.h>
#include "openfs/runtime.h"
#include "openfs/allocator.h"
#include "openfs/extent.h"
#include "openfs/inode_alloc.h"
#include "openfs/time.h"

static openfs_cow_result_t lock_cow(const openfs_superblock_t *sb)
{
    if(sb==NULL||sb->runtime==NULL)return OPENFS_COW_OK;
    if(!openfs_runtime_enter(sb->runtime))return OPENFS_COW_IO_ERROR;
    if(openfs_mutex_lock(&sb->runtime->allocation_lock,OPENFS_LOCK_RANK_ALLOCATION)!=OPENFS_LOCK_OK){
        openfs_runtime_leave(sb->runtime);
        return OPENFS_COW_IO_ERROR;
    }
    return OPENFS_COW_OK;
}

static void unlock_cow(const openfs_superblock_t *sb)
{
    if(sb==NULL||sb->runtime==NULL)return;
    (void)openfs_mutex_unlock(&sb->runtime->allocation_lock);
    openfs_runtime_leave(sb->runtime);
}

static openfs_cow_result_t validate(const openfs_block_device_t *d,const openfs_superblock_t *sb,uint64_t block)
{
    if(!openfs_block_device_is_valid(d)||sb==NULL)return OPENFS_COW_INVALID_ARGUMENT;
    if((sb->feature_flags&OPENFS_FEATURE_COW)==0U)return OPENFS_COW_UNSUPPORTED;
    if(sb->block_size!=d->block_size||sb->refcount_blocks==0U||sb->refcount_start>UINT64_MAX-sb->refcount_blocks||
       sb->refcount_start+sb->refcount_blocks>sb->data_start||sb->data_start>UINT64_MAX-sb->data_blocks||
       sb->data_start+sb->data_blocks>d->block_count)return OPENFS_COW_CORRUPT;
    if(block<sb->data_start||block>=sb->data_start+sb->data_blocks)return OPENFS_COW_OUT_OF_RANGE;
    return OPENFS_COW_OK;
}

static openfs_cow_result_t entry_location(const openfs_block_device_t *d,const openfs_superblock_t *sb,
    uint64_t block,uint64_t *table_block,uint32_t *offset)
{
    openfs_cow_result_t r=validate(d,sb,block);if(r!=OPENFS_COW_OK)return r;
    uint64_t index=block-sb->data_start;
    uint64_t entries_per_block=(uint64_t)d->block_size/2U;
    if(entries_per_block==0U)return OPENFS_COW_CORRUPT;
    uint64_t tb=index/entries_per_block;
    if(tb>=sb->refcount_blocks)return OPENFS_COW_CORRUPT;
    *table_block=sb->refcount_start+tb;
    *offset=(uint32_t)((index%entries_per_block)*2U);
    return OPENFS_COW_OK;
}

static uint16_t load16(const uint8_t *p){return (uint16_t)p[0]|((uint16_t)p[1]<<8U);}
static void store16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);}

static openfs_cow_result_t refcount_get_locked(openfs_block_device_t *d,const openfs_superblock_t *sb,uint64_t block,uint16_t *out)
{
    uint64_t table=0U;uint32_t off=0U;
    openfs_cow_result_t r=entry_location(d,sb,block,&table,&off);
    if(r!=OPENFS_COW_OK)return r;
    uint8_t *buf=(uint8_t*)malloc(d->block_size);
    if(buf==NULL)return OPENFS_COW_IO_ERROR;
    if(d->read(d->context,table,1U,buf)!=OPENFS_IO_OK){free(buf);return OPENFS_COW_IO_ERROR;}
    *out=load16(buf+off);free(buf);return OPENFS_COW_OK;
}

static openfs_cow_result_t refcount_set_locked(openfs_block_device_t *d,const openfs_superblock_t *sb,uint64_t block,uint16_t value)
{
    uint64_t table=0U;uint32_t off=0U;
    openfs_cow_result_t r=entry_location(d,sb,block,&table,&off);
    if(r!=OPENFS_COW_OK)return r;
    uint8_t *buf=(uint8_t*)malloc(d->block_size);
    if(buf==NULL)return OPENFS_COW_IO_ERROR;
    if(d->read(d->context,table,1U,buf)!=OPENFS_IO_OK){free(buf);return OPENFS_COW_IO_ERROR;}
    store16(buf+off,value);
    r=d->write(d->context,table,1U,buf)==OPENFS_IO_OK?OPENFS_COW_OK:OPENFS_COW_IO_ERROR;
    if(r==OPENFS_COW_OK&&d->flush(d->context)!=OPENFS_IO_OK)r=OPENFS_COW_IO_ERROR;
    free(buf);return r;
}

openfs_cow_result_t openfs_cow_refcount_get(openfs_block_device_t *d,const openfs_superblock_t *sb,uint64_t block,uint16_t *out)
{
    if(out==NULL)return OPENFS_COW_INVALID_ARGUMENT;
    openfs_cow_result_t lr=lock_cow(sb);if(lr!=OPENFS_COW_OK)return lr;
    openfs_cow_result_t r=refcount_get_locked(d,sb,block,out);
    unlock_cow(sb);return r;
}

openfs_cow_result_t openfs_cow_refcount_set(openfs_block_device_t *d,const openfs_superblock_t *sb,uint64_t block,uint16_t value)
{
    openfs_cow_result_t lr=lock_cow(sb);if(lr!=OPENFS_COW_OK)return lr;
    openfs_cow_result_t r=refcount_set_locked(d,sb,block,value);
    unlock_cow(sb);return r;
}

openfs_cow_result_t openfs_cow_refcount_inc(openfs_block_device_t *d,const openfs_superblock_t *sb,uint64_t block,uint16_t *out)
{
    openfs_cow_result_t lr=lock_cow(sb);if(lr!=OPENFS_COW_OK)return lr;
    uint16_t current=0U;
    openfs_cow_result_t r=refcount_get_locked(d,sb,block,&current);
    if(r==OPENFS_COW_OK){
        if(current==OPENFS_COW_MAX_REFCOUNT)r=OPENFS_COW_OVERFLOW;
        else{
            uint16_t next=(uint16_t)(current+1U);
            r=refcount_set_locked(d,sb,block,next);
            if(out!=NULL&&r==OPENFS_COW_OK)*out=next;
        }
    }
    unlock_cow(sb);return r;
}

openfs_cow_result_t openfs_cow_refcount_dec(openfs_block_device_t *d,const openfs_superblock_t *sb,uint64_t block,uint16_t *out)
{
    openfs_cow_result_t lr=lock_cow(sb);if(lr!=OPENFS_COW_OK)return lr;
    uint16_t current=0U;
    openfs_cow_result_t r=refcount_get_locked(d,sb,block,&current);
    if(r==OPENFS_COW_OK){
        if(current==0U)r=OPENFS_COW_CORRUPT;
        else{
            uint16_t next=(uint16_t)(current-1U);
            r=refcount_set_locked(d,sb,block,next);
            if(out!=NULL&&r==OPENFS_COW_OK)*out=next;
        }
    }
    unlock_cow(sb);return r;
}

static openfs_cow_result_t cow_lock_inode(const openfs_superblock_t *sb)
{
    if(sb==NULL||sb->runtime==NULL)return OPENFS_COW_OK;
    if(!openfs_runtime_enter(sb->runtime))return OPENFS_COW_IO_ERROR;
    if(openfs_mutex_lock(&sb->runtime->inode_lock,OPENFS_LOCK_RANK_INODE)!=OPENFS_LOCK_OK){
        openfs_runtime_leave(sb->runtime);
        return OPENFS_COW_IO_ERROR;
    }
    return OPENFS_COW_OK;
}

static void cow_unlock_inode(const openfs_superblock_t *sb)
{
    if(sb==NULL||sb->runtime==NULL)return;
    (void)openfs_mutex_unlock(&sb->runtime->inode_lock);
    openfs_runtime_leave(sb->runtime);
}

static openfs_cow_result_t cow_load_extents(const openfs_block_device_t *d,const openfs_superblock_t *sb,
    const openfs_inode_t *inode,openfs_extent_t **out,uint32_t *count)
{
    if(out==NULL||count==NULL||inode==NULL)return OPENFS_COW_INVALID_ARGUMENT;
    *out=NULL;*count=0U;
    uint32_t n=inode->extent_count;
    if(n==0U)return OPENFS_COW_OK;
    if(n>UINT32_MAX/sizeof(openfs_extent_t))return OPENFS_COW_OUT_OF_RANGE;
    openfs_extent_t *ext=(openfs_extent_t*)calloc(n,sizeof(*ext));
    if(ext==NULL)return OPENFS_COW_IO_ERROR;
    uint32_t inline_count=n<OPENFS_INODE_TREE_INLINE_EXTENT_MAX?n:OPENFS_INODE_TREE_INLINE_EXTENT_MAX;
    for(uint32_t i=0U;i<inline_count;i++){
        if(openfs_inode_get_extent(inode,i,&ext[i])!=OPENFS_EXTENT_OK){free(ext);return OPENFS_COW_CORRUPT;}
    }
    if(n>OPENFS_INODE_TREE_INLINE_EXTENT_MAX){
        if((inode->flags&OPENFS_INODE_FLAG_EXTENT_TREE)==0U){free(ext);return OPENFS_COW_CORRUPT;}
        for(uint32_t i=OPENFS_INODE_TREE_INLINE_EXTENT_MAX;i<n;i++){
            if(openfs_extent_tree_read(d,sb,inode,i-OPENFS_INODE_TREE_INLINE_EXTENT_MAX,&ext[i])!=OPENFS_EXTENT_OK){free(ext);return OPENFS_COW_CORRUPT;}
        }
    }
    *out=ext;*count=n;return OPENFS_COW_OK;
}

openfs_cow_result_t openfs_cow_clone_inode(openfs_block_device_t *d,const openfs_superblock_t *sb,
    const openfs_inode_t *source,uint64_t parent,uint64_t *new_inode_number)
{
    if(!openfs_block_device_is_valid(d)||sb==NULL||source==NULL||new_inode_number==NULL||parent==0U)return OPENFS_COW_INVALID_ARGUMENT;
    if((sb->feature_flags&OPENFS_FEATURE_COW)==0U)return OPENFS_COW_UNSUPPORTED;
    if((source->mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_REGULAR)return OPENFS_COW_INVALID_ARGUMENT;
    if(source->link_count==0U||source->generation==0U)return OPENFS_COW_CORRUPT;
    if(openfs_inode_get_xattr_block(source)!=0U)return OPENFS_COW_INVALID_ARGUMENT;

    openfs_cow_result_t lr=cow_lock_inode(sb);if(lr!=OPENFS_COW_OK)return lr;
    openfs_extent_t *ext=NULL;uint32_t count=0U;
    openfs_cow_result_t r=cow_load_extents(d,sb,source,&ext,&count);
    if(r!=OPENFS_COW_OK){cow_unlock_inode(sb);return r;}

    uint64_t new_ino=0U;
    openfs_inode_alloc_result_t iar=openfs_inode_alloc(d,sb,parent,source->mode,&new_ino);
    if(iar!=OPENFS_INODE_ALLOC_OK){free(ext);cow_unlock_inode(sb);return iar==OPENFS_INODE_ALLOC_OUT_OF_SPACE?OPENFS_COW_OUT_OF_RANGE:OPENFS_COW_IO_ERROR;}

    openfs_inode_t created;
    uint64_t inode_count=(sb->inode_table_blocks*(uint64_t)sb->block_size)/OPENFS_INODE_SIZE;
    openfs_inode_result_t ir=openfs_inode_read(d,sb->inode_table_start,new_ino,inode_count,&created);
    if(ir!=OPENFS_INODE_OK){free(ext);cow_unlock_inode(sb);return OPENFS_COW_CORRUPT;}
    uint64_t new_generation=created.generation;
    created=*source;
    created.inode_number=new_ino;
    created.generation=new_generation;

    uint64_t incremented_blocks=0U;
    for(uint32_t i=0U;i<count;i++){
        for(uint64_t n=0U;n<ext[i].block_count;n++){
            uint64_t block=ext[i].physical_start+n;
            uint16_t refs=0U;
            if(incremented_blocks==UINT64_MAX){r=OPENFS_COW_OUT_OF_RANGE;break;}
            if(openfs_cow_refcount_inc(d,sb,block,&refs)!=OPENFS_COW_OK){r=OPENFS_COW_CORRUPT;break;}
            incremented_blocks++;
        }
        if(r!=OPENFS_COW_OK)break;
    }

    uint64_t new_root=0U;
    if(r==OPENFS_COW_OK&&count>OPENFS_INODE_TREE_INLINE_EXTENT_MAX){
        openfs_alloc_result_t ar=openfs_alloc_block(d,sb,&new_root);
        if(ar!=OPENFS_ALLOC_OK)r=ar==OPENFS_ALLOC_OUT_OF_SPACE?OPENFS_COW_OUT_OF_RANGE:OPENFS_COW_IO_ERROR;
        else if(openfs_inode_set_extent_tree_root(&created,new_root)!=OPENFS_EXTENT_OK)r=OPENFS_COW_CORRUPT;
        else if(openfs_extent_tree_write(d,sb,&created,ext+OPENFS_INODE_TREE_INLINE_EXTENT_MAX,count-OPENFS_INODE_TREE_INLINE_EXTENT_MAX)!=OPENFS_EXTENT_OK)r=OPENFS_COW_IO_ERROR;
    }else if(r==OPENFS_COW_OK){
        created.flags=source->flags;
        created.extent_count=source->extent_count;
    }

    if(r==OPENFS_COW_OK){
        uint64_t now=openfs_time_now_ns();
        created.parent_inode=parent;
        created.link_count=1U;
        created.blocks=source->blocks;
        created.size=source->size;
        created.uid=source->uid;created.gid=source->gid;
        created.atime_ns=source->atime_ns;created.mtime_ns=source->mtime_ns;
        created.ctime_ns=now==UINT64_MAX?source->ctime_ns:now;
        if(openfs_inode_write(d,sb->inode_table_start,inode_count,&created)!=OPENFS_INODE_OK)r=OPENFS_COW_IO_ERROR;
        else if(d->flush(d->context)!=OPENFS_IO_OK)r=OPENFS_COW_IO_ERROR;
    }

    if(r!=OPENFS_COW_OK){
        if(new_root!=0U)(void)openfs_free_block(d,sb,new_root);
        uint64_t remaining=incremented_blocks;
        for(uint32_t i=0U;i<count&&remaining!=0U;i++){
            uint64_t take=ext[i].block_count<remaining?ext[i].block_count:remaining;
            for(uint64_t n=0U;n<take;n++)(void)openfs_cow_refcount_dec(d,sb,ext[i].physical_start+n,NULL);
            remaining-=take;
        }
        created.mode=OPENFS_INODE_MODE_FREE;created.link_count=0U;
        (void)openfs_inode_write(d,sb->inode_table_start,inode_count,&created);
        (void)openfs_inode_free(d,sb,new_ino);
    }
    free(ext);
    cow_unlock_inode(sb);
    if(r==OPENFS_COW_OK)*new_inode_number=new_ino;
    return r;
}
