#include "openfs/cow.h"
#include <stdlib.h>
#include "openfs/runtime.h"

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

openfs_cow_result_t openfs_cow_refcount_get(openfs_block_device_t *d,const openfs_superblock_t *sb,uint64_t block,uint16_t *out)
{
    if(out==NULL)return OPENFS_COW_INVALID_ARGUMENT;
    openfs_cow_result_t lr=lock_cow(sb);if(lr!=OPENFS_COW_OK)return lr;
    uint64_t table=0U;uint32_t off=0U;openfs_cow_result_t r=entry_location(d,sb,block,&table,&off);
    if(r==OPENFS_COW_OK){
        uint8_t *buf=(uint8_t*)malloc(d->block_size);
        if(buf==NULL)r=OPENFS_COW_IO_ERROR;
        else if(d->read(d->context,table,1U,buf)!=OPENFS_IO_OK)r=OPENFS_COW_IO_ERROR;
        else *out=load16(buf+off);
        free(buf);
    }
    unlock_cow(sb);return r;
}

openfs_cow_result_t openfs_cow_refcount_set(openfs_block_device_t *d,const openfs_superblock_t *sb,uint64_t block,uint16_t value)
{
    openfs_cow_result_t lr=lock_cow(sb);if(lr!=OPENFS_COW_OK)return lr;
    uint64_t table=0U;uint32_t off=0U;openfs_cow_result_t r=entry_location(d,sb,block,&table,&off);
    if(r==OPENFS_COW_OK){
        uint8_t *buf=(uint8_t*)malloc(d->block_size);
        if(buf==NULL)r=OPENFS_COW_IO_ERROR;
        else if(d->read(d->context,table,1U,buf)!=OPENFS_IO_OK)r=OPENFS_COW_IO_ERROR;
        else{store16(buf+off,value);r=d->write(d->context,table,1U,buf)==OPENFS_IO_OK?OPENFS_COW_OK:OPENFS_COW_IO_ERROR;if(r==OPENFS_COW_OK&&d->flush(d->context)!=OPENFS_IO_OK)r=OPENFS_COW_IO_ERROR;}
        free(buf);
    }
    unlock_cow(sb);return r;
}

openfs_cow_result_t openfs_cow_refcount_inc(openfs_block_device_t *d,const openfs_superblock_t *sb,uint64_t block,uint16_t *out)
{
    uint16_t current=0U;openfs_cow_result_t r=openfs_cow_refcount_get(d,sb,block,&current);
    if(r!=OPENFS_COW_OK)return r;
    if(current==OPENFS_COW_MAX_REFCOUNT)return OPENFS_COW_OVERFLOW;
    r=openfs_cow_refcount_set(d,sb,block,(uint16_t)(current+1U));
    if(out!=NULL&&r==OPENFS_COW_OK)*out=(uint16_t)(current+1U);
    return r;
}

openfs_cow_result_t openfs_cow_refcount_dec(openfs_block_device_t *d,const openfs_superblock_t *sb,uint64_t block,uint16_t *out)
{
    uint16_t current=0U;openfs_cow_result_t r=openfs_cow_refcount_get(d,sb,block,&current);
    if(r!=OPENFS_COW_OK)return r;
    if(current==0U)return OPENFS_COW_CORRUPT;
    current--;r=openfs_cow_refcount_set(d,sb,block,current);
    if(out!=NULL&&r==OPENFS_COW_OK)*out=current;
    return r;
}
