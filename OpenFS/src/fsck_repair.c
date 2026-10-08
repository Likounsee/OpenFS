#include "openfs/fsck.h"
#include "openfs/runtime.h"
#include "openfs/transaction.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t start;
    uint64_t blocks;
    uint8_t *original;
    uint8_t *patched;
    uint8_t *changed;
} bitmap_repair_plan_t;

static openfs_fsck_result_t build_plan(openfs_block_device_t *d,uint64_t start,uint64_t blocks,uint64_t valid_bits,bitmap_repair_plan_t *p){
    if(d==NULL||p==NULL||blocks==0U||d->block_size==0U)return OPENFS_FSCK_INVALID_ARGUMENT;
    if(start>=d->block_count||blocks>d->block_count-start||blocks>SIZE_MAX/d->block_size)return OPENFS_FSCK_CORRUPT;
    uint64_t bytes=blocks*(uint64_t)d->block_size;
    if(bytes>SIZE_MAX||bytes>UINT64_MAX/8U)return OPENFS_FSCK_CORRUPT;
    uint64_t capacity=bytes*8U;
    memset(p,0,sizeof(*p));p->start=start;p->blocks=blocks;
    p->original=(uint8_t*)malloc((size_t)bytes);
    p->patched=(uint8_t*)malloc((size_t)bytes);
    p->changed=(uint8_t*)calloc((size_t)blocks,1U);
    if(p->original==NULL||p->patched==NULL||p->changed==NULL)return OPENFS_FSCK_IO_ERROR;
    if(blocks>UINT32_MAX||d->read(d->context,start,(uint32_t)blocks,p->original)!=OPENFS_IO_OK)return OPENFS_FSCK_IO_ERROR;
    memcpy(p->patched,p->original,(size_t)bytes);
    if(valid_bits<capacity){
        uint64_t bits_per_block=(uint64_t)d->block_size*8U;
        uint64_t first=valid_bits/bits_per_block;
        for(uint64_t b=first;b<blocks;b++){
            uint64_t begin=b==first?valid_bits%bits_per_block:0U;
            for(uint64_t bit=begin;bit<bits_per_block;bit++)
                p->patched[(size_t)(b*d->block_size+bit/8U)]&=(uint8_t)~(uint8_t)(1U<<(bit%8U));
            p->changed[b]=(uint8_t)(memcmp(p->patched+(size_t)(b*d->block_size),p->original+(size_t)(b*d->block_size),d->block_size)!=0U);
        }
    }
    return OPENFS_FSCK_OK;
}
static void free_plan(bitmap_repair_plan_t *p){if(p){free(p->original);free(p->patched);free(p->changed);memset(p,0,sizeof(*p));}}
static openfs_fsck_result_t write_plan(openfs_block_device_t *d,const bitmap_repair_plan_t *p){
    for(uint64_t b=0U;b<p->blocks;b++)if(p->changed[b]){
        if(d->write(d->context,p->start+b,1U,p->patched+(size_t)(b*d->block_size))!=OPENFS_IO_OK)return OPENFS_FSCK_IO_ERROR;
    }
    if(d->flush(d->context)!=OPENFS_IO_OK)return OPENFS_FSCK_IO_ERROR;
    return OPENFS_FSCK_OK;
}
static openfs_fsck_result_t rollback_plans(openfs_block_device_t *d,bitmap_repair_plan_t *a,bitmap_repair_plan_t *b){
    int ok=1;
    bitmap_repair_plan_t *plans[2]={a,b};
    for(unsigned i=0U;i<2U;i++)for(uint64_t n=0U;n<plans[i]->blocks;n++)if(plans[i]->changed[n]){
        if(d->write(d->context,plans[i]->start+n,1U,plans[i]->original+(size_t)(n*d->block_size))!=OPENFS_IO_OK)ok=0;
    }
    if(d->flush(d->context)!=OPENFS_IO_OK)ok=0;
    return ok?OPENFS_FSCK_IO_ERROR:OPENFS_FSCK_CORRUPT;
}
static openfs_fsck_result_t repair_lock(const openfs_superblock_t *sb){
    if(sb==NULL||sb->runtime==NULL)return OPENFS_FSCK_OK;
    if(!openfs_runtime_enter(sb->runtime))return OPENFS_FSCK_IO_ERROR;
    if(openfs_mutex_lock(&sb->runtime->allocation_lock,OPENFS_LOCK_RANK_ALLOCATION)!=OPENFS_LOCK_OK){openfs_runtime_leave(sb->runtime);return OPENFS_FSCK_IO_ERROR;}
    return OPENFS_FSCK_OK;
}
static void repair_unlock(const openfs_superblock_t *sb){if(sb==NULL||sb->runtime==NULL)return;(void)openfs_mutex_unlock(&sb->runtime->allocation_lock);openfs_runtime_leave(sb->runtime);}

openfs_fsck_result_t openfs_fsck_repair_bitmap_tails(openfs_block_device_t *d,const openfs_superblock_t *sb,uint64_t *errors){
    if(!openfs_block_device_is_valid(d)||sb==NULL||errors==NULL)return OPENFS_FSCK_INVALID_ARGUMENT;
    if(openfs_validate_superblock(d,sb)!=OPENFS_FORMAT_OK)return OPENFS_FSCK_CORRUPT;
    if(sb->inode_table_blocks>UINT64_MAX/sb->block_size)return OPENFS_FSCK_CORRUPT;
    uint64_t inode_count=(sb->inode_table_blocks*(uint64_t)sb->block_size)/OPENFS_INODE_SIZE;
    if(inode_count==0U)return OPENFS_FSCK_CORRUPT;
    openfs_fsck_result_t lr=repair_lock(sb);if(lr!=OPENFS_FSCK_OK)return lr;
    bitmap_repair_plan_t block_plan={0},inode_plan={0};
    openfs_fsck_result_t r=build_plan(d,sb->block_bitmap_start,sb->block_bitmap_blocks,sb->total_blocks,&block_plan);
    if(r==OPENFS_FSCK_OK)r=build_plan(d,sb->inode_bitmap_start,sb->inode_bitmap_blocks,inode_count,&inode_plan);
    if(r==OPENFS_FSCK_OK && sb->runtime!=NULL && sb->runtime->journal!=NULL && sb->runtime->device==d){
        if(openfs_mutex_lock(&sb->runtime->transaction_lock,OPENFS_LOCK_RANK_TRANSACTION)!=OPENFS_LOCK_OK){
            r=OPENFS_FSCK_IO_ERROR;
        }else{
            openfs_transaction_t tx;
            openfs_transaction_result_t tr=openfs_transaction_begin(&tx,d,sb->runtime->journal);
            if(tr==OPENFS_TRANSACTION_OK){
                openfs_block_device_t *td=openfs_transaction_device(&tx);
                r=write_plan(td,&block_plan);
                if(r==OPENFS_FSCK_OK)r=write_plan(td,&inode_plan);
                if(r==OPENFS_FSCK_OK){
                    tr=openfs_transaction_commit(&tx);
                    if(tr!=OPENFS_TRANSACTION_OK)r=(tx.recovery_required||tr==OPENFS_TRANSACTION_CORRUPT)?OPENFS_FSCK_CORRUPT:OPENFS_FSCK_IO_ERROR;
                }else{
                    openfs_transaction_result_t ar=openfs_transaction_abort(&tx);
                    if(ar==OPENFS_TRANSACTION_CORRUPT)r=OPENFS_FSCK_CORRUPT;
                }
            }else{
                r=(tr==OPENFS_TRANSACTION_CORRUPT)?OPENFS_FSCK_CORRUPT:OPENFS_FSCK_IO_ERROR;
            }
            (void)openfs_mutex_unlock(&sb->runtime->transaction_lock);
        }
    }else{
        if(r==OPENFS_FSCK_OK)r=write_plan(d,&block_plan);
        if(r==OPENFS_FSCK_OK)r=write_plan(d,&inode_plan);
    }
    if(r!=OPENFS_FSCK_OK){
        if(block_plan.changed!=NULL||inode_plan.changed!=NULL){
            openfs_fsck_result_t rr=rollback_plans(d,&block_plan,&inode_plan);
            if(rr==OPENFS_FSCK_CORRUPT)r=rr;
        }
        free_plan(&block_plan);free_plan(&inode_plan);repair_unlock(sb);return r;
    }
    free_plan(&block_plan);free_plan(&inode_plan);repair_unlock(sb);
    return openfs_fsck(d,sb,errors);
}
