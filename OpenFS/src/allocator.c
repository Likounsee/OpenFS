#include "openfs/allocator.h"
#include "openfs/bitmap.h"
#include "openfs/runtime.h"
#include "openfs/cow.h"
#include "openfs/data_checksum.h"
#include "openfs/transaction.h"
#include <stdlib.h>
static openfs_alloc_result_t set_block(openfs_block_device_t*d,const openfs_superblock_t*sb,uint64_t block,int value){
    if(d==NULL||sb==NULL)return OPENFS_ALLOC_INVALID_ARGUMENT;
    if(sb->block_size!=d->block_size||sb->data_blocks==0U||sb->data_start>UINT64_MAX-sb->data_blocks||sb->data_start+sb->data_blocks>d->block_count||sb->block_bitmap_start>=d->block_count||sb->block_bitmap_blocks==0U||sb->block_bitmap_blocks>d->block_count-sb->block_bitmap_start)return OPENFS_ALLOC_CORRUPT;
    if(block<sb->data_start||block>=sb->data_start+sb->data_blocks)return OPENFS_ALLOC_INVALID_ARGUMENT;
    int used=0;
    if(openfs_bitmap_test(d,sb->block_bitmap_start,sb->block_bitmap_blocks,block,&used)!=OPENFS_BITMAP_OK)return OPENFS_ALLOC_IO_ERROR;
    if(value&&used)return OPENFS_ALLOC_CORRUPT;
    if(!value&&!used)return OPENFS_ALLOC_CORRUPT;
    if(openfs_bitmap_set(d,sb->block_bitmap_start,sb->block_bitmap_blocks,block,value)!=OPENFS_BITMAP_OK){
        int rollback_ok=openfs_bitmap_set(d,sb->block_bitmap_start,sb->block_bitmap_blocks,block,used)==OPENFS_BITMAP_OK;
        if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
        return rollback_ok?OPENFS_ALLOC_IO_ERROR:OPENFS_ALLOC_CORRUPT;
    }
    if(d->flush(d->context)==OPENFS_IO_OK)return OPENFS_ALLOC_OK;
    if(openfs_bitmap_set(d,sb->block_bitmap_start,sb->block_bitmap_blocks,block,used)!=OPENFS_BITMAP_OK||d->flush(d->context)!=OPENFS_IO_OK)return OPENFS_ALLOC_CORRUPT;
    return OPENFS_ALLOC_IO_ERROR;
}
static openfs_alloc_result_t alloc_block_tx_unlocked(openfs_transaction_t *t,
    const openfs_superblock_t *sb, uint64_t *out)
{
    openfs_block_device_t *d = openfs_transaction_device(t);
    if (d == NULL || sb == NULL || out == NULL)
        return OPENFS_ALLOC_INVALID_ARGUMENT;
    if (sb->block_size != d->block_size || sb->data_blocks == 0U ||
        sb->data_start > UINT64_MAX - sb->data_blocks ||
        sb->data_start + sb->data_blocks > d->block_count ||
        sb->block_bitmap_blocks == 0U ||
        sb->block_bitmap_start >= d->block_count ||
        sb->block_bitmap_blocks > d->block_count - sb->block_bitmap_start)
        return OPENFS_ALLOC_CORRUPT;

    uint64_t end = sb->data_start + sb->data_blocks;
    for (uint64_t b = sb->data_start; b < end; ++b) {
        int used = 0;
        if (openfs_bitmap_test(d, sb->block_bitmap_start,
                sb->block_bitmap_blocks, b, &used) != OPENFS_BITMAP_OK)
            return OPENFS_ALLOC_IO_ERROR;
        if (used)
            continue;

        /* Once the bitmap has been staged, every later failure must make the
         * transaction uncommittable: otherwise a caller could commit a
         * reservation without its matching refcount/checksum metadata. */
        if (openfs_bitmap_set(d, sb->block_bitmap_start,
                sb->block_bitmap_blocks, b, 1) != OPENFS_BITMAP_OK) {
            t->failed = 1;
            return OPENFS_ALLOC_IO_ERROR;
        }
        if ((sb->feature_flags & OPENFS_FEATURE_COW) != 0U &&
            openfs_cow_refcount_set_tx(t, sb, b, 1U) != OPENFS_COW_OK) {
            t->failed = 1;
            return OPENFS_ALLOC_CORRUPT;
        }
        if ((sb->feature_flags & OPENFS_FEATURE_DATA_CHECKSUM) != 0U) {
            uint8_t *zero = (uint8_t *)calloc(1U, d->block_size);
            if (zero == NULL ||
                openfs_data_checksum_set(d, sb, b,
                    openfs_data_checksum(zero, d->block_size)) != 0) {
                free(zero);
                t->failed = 1;
                return OPENFS_ALLOC_IO_ERROR;
            }
            free(zero);
        }
        *out = b;
        return OPENFS_ALLOC_OK;
    }
    return OPENFS_ALLOC_OUT_OF_SPACE;
}
static openfs_alloc_result_t alloc_block_unlocked(openfs_block_device_t*d,const openfs_superblock_t*sb,uint64_t*out){
    if(d==NULL||sb==NULL||out==NULL)return OPENFS_ALLOC_INVALID_ARGUMENT;
    if(sb->block_size!=d->block_size||sb->data_blocks==0U||sb->data_start>UINT64_MAX-sb->data_blocks||sb->data_start+sb->data_blocks>d->block_count||sb->block_bitmap_blocks==0U||sb->block_bitmap_start>=d->block_count||sb->block_bitmap_blocks>d->block_count-sb->block_bitmap_start)return OPENFS_ALLOC_CORRUPT;
    const uint64_t data_end=sb->data_start+sb->data_blocks;
    for(uint64_t b=sb->data_start;b<data_end;++b){
        int used=0;
        openfs_bitmap_result_t br=openfs_bitmap_test(d,sb->block_bitmap_start,sb->block_bitmap_blocks,b,&used);if(br!=OPENFS_BITMAP_OK)return br==OPENFS_BITMAP_OUT_OF_RANGE?OPENFS_ALLOC_CORRUPT:OPENFS_ALLOC_IO_ERROR;
        if(!used){
            if(openfs_bitmap_set(d,sb->block_bitmap_start,sb->block_bitmap_blocks,b,1)!=OPENFS_BITMAP_OK){
                int rollback_ok=openfs_bitmap_set(d,sb->block_bitmap_start,sb->block_bitmap_blocks,b,0)==OPENFS_BITMAP_OK;
                if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
                return rollback_ok?OPENFS_ALLOC_IO_ERROR:OPENFS_ALLOC_CORRUPT;
            }
            if(d->flush(d->context)!=OPENFS_IO_OK){
                int rollback_ok=openfs_bitmap_set(d,sb->block_bitmap_start,sb->block_bitmap_blocks,b,0)==OPENFS_BITMAP_OK;
                if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
                return rollback_ok?OPENFS_ALLOC_IO_ERROR:OPENFS_ALLOC_CORRUPT;
            }
            if((sb->feature_flags&OPENFS_FEATURE_COW)!=0U){
                openfs_cow_result_t cr=openfs_cow_refcount_set(d,sb,b,1U);
                if(cr!=OPENFS_COW_OK){
                    int rollback_ok=1;
                    if(openfs_cow_refcount_set(d,sb,b,0U)!=OPENFS_COW_OK)rollback_ok=0;
                    if(openfs_bitmap_set(d,sb->block_bitmap_start,sb->block_bitmap_blocks,b,0)!=OPENFS_BITMAP_OK)rollback_ok=0;
                    if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
                    return rollback_ok?OPENFS_ALLOC_IO_ERROR:OPENFS_ALLOC_CORRUPT;
                }
            }
            if((sb->feature_flags&OPENFS_FEATURE_DATA_CHECKSUM)!=0U){uint8_t*z=calloc(1U,d->block_size);if(z==NULL||openfs_data_checksum_set(d,sb,b,openfs_data_checksum(z,d->block_size))!=0){free(z);if((sb->feature_flags&OPENFS_FEATURE_COW)!=0U)(void)openfs_cow_refcount_set(d,sb,b,0U);(void)openfs_bitmap_set(d,sb->block_bitmap_start,sb->block_bitmap_blocks,b,0);(void)d->flush(d->context);return OPENFS_ALLOC_IO_ERROR;}free(z);}
            *out=b; return OPENFS_ALLOC_OK;
        }
    }
    return OPENFS_ALLOC_OUT_OF_SPACE;
}
static openfs_alloc_result_t free_block_unlocked(openfs_block_device_t*d,const openfs_superblock_t*sb,uint64_t block)
{
    if((sb->feature_flags&OPENFS_FEATURE_COW)==0U)return set_block(d,sb,block,0);
    uint16_t refs=0U;
    openfs_cow_result_t cr=openfs_cow_refcount_get(d,sb,block,&refs);
    if(cr!=OPENFS_COW_OK)return cr==OPENFS_COW_UNSUPPORTED?set_block(d,sb,block,0):OPENFS_ALLOC_CORRUPT;
    if(refs==0U)return OPENFS_ALLOC_CORRUPT;
    if(refs>1U){
        cr=openfs_cow_refcount_dec(d,sb,block,NULL);
        return cr==OPENFS_COW_OK?OPENFS_ALLOC_OK:OPENFS_ALLOC_CORRUPT;
    }
    cr=openfs_cow_refcount_set(d,sb,block,0U);
    if(cr!=OPENFS_COW_OK)return OPENFS_ALLOC_CORRUPT;
    openfs_alloc_result_t r=set_block(d,sb,block,0);
    if(r!=OPENFS_ALLOC_OK){
        if(openfs_cow_refcount_set(d,sb,block,1U)!=OPENFS_COW_OK)return OPENFS_ALLOC_CORRUPT;
        return r;
    }
    return OPENFS_ALLOC_OK;
}

static openfs_alloc_result_t lock_allocation(const openfs_superblock_t *sb)
{
    if(sb==NULL||sb->runtime==NULL)return OPENFS_ALLOC_OK;
    if(!openfs_runtime_enter(sb->runtime))return OPENFS_ALLOC_IO_ERROR;
    if(openfs_mutex_lock(&sb->runtime->allocation_lock,OPENFS_LOCK_RANK_ALLOCATION)!=OPENFS_LOCK_OK){openfs_runtime_leave(sb->runtime);return OPENFS_ALLOC_IO_ERROR;}
    return OPENFS_ALLOC_OK;
}
static void unlock_allocation(const openfs_superblock_t *sb){
    if(sb==NULL||sb->runtime==NULL)return;
    (void)openfs_mutex_unlock(&sb->runtime->allocation_lock);
    openfs_runtime_leave(sb->runtime);
}
static int transaction_available(const openfs_block_device_t*d,const openfs_superblock_t*sb)
{
    return sb!=NULL&&sb->runtime!=NULL&&sb->runtime->journal!=NULL&&sb->runtime->device==d;
}
static openfs_alloc_result_t map_transaction_result(openfs_transaction_result_t r)
{
    if(r==OPENFS_TRANSACTION_CORRUPT)return OPENFS_ALLOC_CORRUPT;
    if(r==OPENFS_TRANSACTION_OK)return OPENFS_ALLOC_OK;
    return OPENFS_ALLOC_IO_ERROR;
}
openfs_alloc_result_t openfs_alloc_block_tx(openfs_transaction_t*t,const openfs_superblock_t*sb,uint64_t*out)
{
    if(t==NULL||!t->active||t->base==NULL||sb==NULL||out==NULL)return OPENFS_ALLOC_INVALID_ARGUMENT;
    openfs_block_device_t *td=openfs_transaction_device(t);
    if(td==NULL)return OPENFS_ALLOC_INVALID_ARGUMENT;
    return alloc_block_tx_unlocked(t,sb,out);
}
openfs_alloc_result_t openfs_free_block_tx(openfs_transaction_t*t,const openfs_superblock_t*sb,uint64_t block)
{
    if(t==NULL||!t->active||t->base==NULL||sb==NULL)return OPENFS_ALLOC_INVALID_ARGUMENT;
    openfs_block_device_t *td=openfs_transaction_device(t); if(td==NULL)return OPENFS_ALLOC_INVALID_ARGUMENT;
    if((sb->feature_flags&OPENFS_FEATURE_COW)==0U)return set_block(td,sb,block,0);
    uint16_t refs=0U; openfs_cow_result_t cr=openfs_cow_refcount_get_tx(t,sb,block,&refs);
    if(cr!=OPENFS_COW_OK||refs==0U)return OPENFS_ALLOC_CORRUPT;
    if(refs>1U)return openfs_cow_refcount_dec_tx(t,sb,block,NULL)==OPENFS_COW_OK?OPENFS_ALLOC_OK:OPENFS_ALLOC_CORRUPT;
    if(openfs_cow_refcount_set_tx(t,sb,block,0U)!=OPENFS_COW_OK)return OPENFS_ALLOC_CORRUPT;
    return openfs_bitmap_set(td,sb->block_bitmap_start,sb->block_bitmap_blocks,block,0)==OPENFS_BITMAP_OK?OPENFS_ALLOC_OK:OPENFS_ALLOC_CORRUPT;
}

openfs_alloc_result_t openfs_alloc_block(openfs_block_device_t*d,const openfs_superblock_t*sb,uint64_t*out)
{
    openfs_transaction_t *owner=openfs_transaction_from_device(d);
    if(owner!=NULL)return openfs_alloc_block_tx(owner,sb,out);
    openfs_alloc_result_t lr=lock_allocation(sb);if(lr!=OPENFS_ALLOC_OK)return lr;
    if(!transaction_available(d,sb)){
        openfs_alloc_result_t r=alloc_block_unlocked(d,sb,out);unlock_allocation(sb);return r;
    }
    if(openfs_mutex_lock(&sb->runtime->transaction_lock,OPENFS_LOCK_RANK_TRANSACTION)!=OPENFS_LOCK_OK){unlock_allocation(sb);return OPENFS_ALLOC_IO_ERROR;}
    openfs_transaction_t tx;
    openfs_transaction_result_t tr=openfs_transaction_begin(&tx,d,sb->runtime->journal);
    if(tr!=OPENFS_TRANSACTION_OK){(void)openfs_mutex_unlock(&sb->runtime->transaction_lock);unlock_allocation(sb);return map_transaction_result(tr);}
    openfs_alloc_result_t r=openfs_alloc_block_tx(&tx,sb,out);
    if(r!=OPENFS_ALLOC_OK){
        openfs_transaction_result_t ar=openfs_transaction_abort(&tx);
        if(ar==OPENFS_TRANSACTION_CORRUPT)r=OPENFS_ALLOC_CORRUPT;
        (void)openfs_mutex_unlock(&sb->runtime->transaction_lock);
        unlock_allocation(sb);
        return r;
    }
    tr=openfs_transaction_commit(&tx);
    if(tr!=OPENFS_TRANSACTION_OK)r=tx.recovery_required?OPENFS_ALLOC_CORRUPT:map_transaction_result(tr);
    (void)openfs_mutex_unlock(&sb->runtime->transaction_lock);
    unlock_allocation(sb);
    return r;
}
openfs_alloc_result_t openfs_free_block(openfs_block_device_t*d,const openfs_superblock_t*sb,uint64_t block)
{
    openfs_transaction_t *owner=openfs_transaction_from_device(d);
    if(owner!=NULL)return openfs_free_block_tx(owner,sb,block);
    openfs_alloc_result_t lr=lock_allocation(sb);if(lr!=OPENFS_ALLOC_OK)return lr;
    if(!transaction_available(d,sb)){
        openfs_alloc_result_t r=free_block_unlocked(d,sb,block);unlock_allocation(sb);return r;
    }
    if(openfs_mutex_lock(&sb->runtime->transaction_lock,OPENFS_LOCK_RANK_TRANSACTION)!=OPENFS_LOCK_OK){unlock_allocation(sb);return OPENFS_ALLOC_IO_ERROR;}
    openfs_transaction_t tx;
    openfs_transaction_result_t tr=openfs_transaction_begin(&tx,d,sb->runtime->journal);
    if(tr!=OPENFS_TRANSACTION_OK){(void)openfs_mutex_unlock(&sb->runtime->transaction_lock);unlock_allocation(sb);return map_transaction_result(tr);}
    openfs_alloc_result_t r=openfs_free_block_tx(&tx,sb,block);
    if(r!=OPENFS_ALLOC_OK){
        openfs_transaction_result_t ar=openfs_transaction_abort(&tx);
        if(ar==OPENFS_TRANSACTION_CORRUPT)r=OPENFS_ALLOC_CORRUPT;
        (void)openfs_mutex_unlock(&sb->runtime->transaction_lock);
        unlock_allocation(sb);
        return r;
    }
    tr=openfs_transaction_commit(&tx);
    if(tr!=OPENFS_TRANSACTION_OK)r=map_transaction_result(tr);
    (void)openfs_mutex_unlock(&sb->runtime->transaction_lock);
    unlock_allocation(sb);
    return r;
}
