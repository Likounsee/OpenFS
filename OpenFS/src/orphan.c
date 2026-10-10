#include "openfs/orphan.h"
#include "openfs/allocator.h"
#include "openfs/bitmap.h"
#include "openfs/file.h"
#include "openfs/inode.h"
#include "openfs/inode_alloc.h"
#include "openfs/lock.h"
#include "openfs/runtime.h"
#include "openfs/transaction.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int inode_count(const openfs_superblock_t *s,uint64_t *out)
{
    if(s==NULL||out==NULL||s->block_size==0U||s->inode_table_blocks>UINT64_MAX/s->block_size)return 0;
    uint64_t bytes=s->inode_table_blocks*(uint64_t)s->block_size;
    if(bytes<OPENFS_INODE_SIZE)return 0;
    *out=bytes/OPENFS_INODE_SIZE;
    return *out!=0U;
}

static void clear_orphan_inode(openfs_inode_t *inode)
{
    inode->mode=OPENFS_INODE_MODE_FREE;
    inode->link_count=0U;
    inode->parent_inode=0U;
    inode->flags=0U;
    inode->extent_count=0U;
    inode->blocks=0U;
    inode->size=0U;
    inode->uid=0U;
    inode->gid=0U;
    inode->atime_ns=0U;
    inode->mtime_ns=0U;
    inode->ctime_ns=0U;
    memset(inode->inline_data,0,sizeof(inode->inline_data));
    memset(inode->reserved,0,sizeof(inode->reserved));
}

static openfs_orphan_result_t map_file_result(openfs_file_result_t r)
{
    if(r==OPENFS_FILE_CORRUPT)return OPENFS_ORPHAN_CORRUPT;
    if(r==OPENFS_FILE_OK)return OPENFS_ORPHAN_OK;
    return OPENFS_ORPHAN_IO_ERROR;
}

static openfs_orphan_result_t map_alloc_result(openfs_alloc_result_t r)
{
    if(r==OPENFS_ALLOC_OK)return OPENFS_ORPHAN_OK;
    if(r==OPENFS_ALLOC_CORRUPT)return OPENFS_ORPHAN_CORRUPT;
    return OPENFS_ORPHAN_IO_ERROR;
}

static openfs_orphan_result_t map_transaction_result(openfs_transaction_result_t r)
{
    if(r==OPENFS_TRANSACTION_CORRUPT)return OPENFS_ORPHAN_CORRUPT;
    if(r==OPENFS_TRANSACTION_INVALID_ARGUMENT)return OPENFS_ORPHAN_INVALID_ARGUMENT;
    return OPENFS_ORPHAN_IO_ERROR;
}

/*
 * A mounted filesystem must retire an orphan in one journal transaction.
 * This keeps data/refcounts, the inode contents, its inode-bitmap bit and any
 * xattr block in the same commit, so a crash after COMMIT can be replayed.
 */
static openfs_orphan_result_t reclaim_with_runtime(
    openfs_block_device_t *d,const openfs_superblock_t *s,uint64_t ino,uint64_t count)
{
    openfs_runtime_t *runtime=s->runtime;
    openfs_orphan_result_t result=OPENFS_ORPHAN_IO_ERROR;
    int runtime_entered=0;
    int inode_locked=0;
    int allocation_locked=0;
    int transaction_locked=0;
    int transaction_started=0;
    openfs_transaction_t transaction={0};

    if(runtime==NULL||runtime->device!=d||runtime->journal==NULL)
        return OPENFS_ORPHAN_IO_ERROR;
    if(openfs_transaction_from_device(d)!=NULL)
        return OPENFS_ORPHAN_INVALID_ARGUMENT;
    if(!openfs_runtime_enter(runtime))
        return OPENFS_ORPHAN_IO_ERROR;
    runtime_entered=1;

    if(openfs_mutex_lock(&runtime->inode_lock,OPENFS_LOCK_RANK_INODE)!=OPENFS_LOCK_OK)
        goto done;
    inode_locked=1;
    if(openfs_mutex_lock(&runtime->allocation_lock,OPENFS_LOCK_RANK_ALLOCATION)!=OPENFS_LOCK_OK)
        goto done;
    allocation_locked=1;
    if(openfs_mutex_lock(&runtime->transaction_lock,OPENFS_LOCK_RANK_TRANSACTION)!=OPENFS_LOCK_OK)
        goto done;
    transaction_locked=1;

    int used=0;
    openfs_bitmap_result_t bitmap_result=openfs_bitmap_test(
        d,s->inode_bitmap_start,s->inode_bitmap_blocks,ino-1U,&used);
    if(bitmap_result!=OPENFS_BITMAP_OK){
        result=bitmap_result==OPENFS_BITMAP_IO_ERROR?OPENFS_ORPHAN_IO_ERROR:OPENFS_ORPHAN_CORRUPT;
        goto done;
    }
    if(!used){
        result=OPENFS_ORPHAN_NOT_ORPHAN;
        goto done;
    }

    openfs_inode_t inode;
    openfs_inode_result_t ir=openfs_inode_read(d,s->inode_table_start,ino,count,&inode);
    if(ir!=OPENFS_INODE_OK){
        result=ir==OPENFS_INODE_CORRUPT?OPENFS_ORPHAN_CORRUPT:OPENFS_ORPHAN_IO_ERROR;
        goto done;
    }
    if((inode.flags&OPENFS_INODE_FLAG_ORPHAN)==0U||inode.link_count!=0U){
        result=OPENFS_ORPHAN_NOT_ORPHAN;
        goto done;
    }

    openfs_transaction_result_t tr=openfs_transaction_begin(&transaction,d,runtime->journal);
    if(tr!=OPENFS_TRANSACTION_OK){
        result=map_transaction_result(tr);
        goto done;
    }
    transaction_started=1;
    openfs_block_device_t *td=openfs_transaction_device(&transaction);
    if(td==NULL){
        result=OPENFS_ORPHAN_INVALID_ARGUMENT;
        goto abort_transaction;
    }

    /* Re-read through the transaction view before making any changes. */
    bitmap_result=openfs_bitmap_test(td,s->inode_bitmap_start,s->inode_bitmap_blocks,ino-1U,&used);
    if(bitmap_result!=OPENFS_BITMAP_OK){
        result=bitmap_result==OPENFS_BITMAP_IO_ERROR?OPENFS_ORPHAN_IO_ERROR:OPENFS_ORPHAN_CORRUPT;
        goto abort_transaction;
    }
    if(!used){
        result=OPENFS_ORPHAN_NOT_ORPHAN;
        goto abort_transaction;
    }
    ir=openfs_inode_read(td,s->inode_table_start,ino,count,&inode);
    if(ir!=OPENFS_INODE_OK){
        result=ir==OPENFS_INODE_CORRUPT?OPENFS_ORPHAN_CORRUPT:OPENFS_ORPHAN_IO_ERROR;
        goto abort_transaction;
    }
    if((inode.flags&OPENFS_INODE_FLAG_ORPHAN)==0U||inode.link_count!=0U){
        result=OPENFS_ORPHAN_NOT_ORPHAN;
        goto abort_transaction;
    }

    uint64_t xattr_block=openfs_inode_get_xattr_block(&inode);
    openfs_file_result_t fr=openfs_file_truncate_tx(&transaction,s,&inode,0U);
    if(fr!=OPENFS_FILE_OK){
        result=map_file_result(fr);
        goto abort_transaction;
    }

    if(xattr_block!=0U){
        openfs_alloc_result_t ar=openfs_free_block(td,s,xattr_block);
        if(ar!=OPENFS_ALLOC_OK){
            result=map_alloc_result(ar);
            goto abort_transaction;
        }
    }

    clear_orphan_inode(&inode);
    inode.generation=inode.generation==UINT64_MAX?1U:inode.generation+1U;
    ir=openfs_inode_write(td,s->inode_table_start,count,&inode);
    if(ir!=OPENFS_INODE_OK){
        result=ir==OPENFS_INODE_CORRUPT?OPENFS_ORPHAN_CORRUPT:OPENFS_ORPHAN_IO_ERROR;
        goto abort_transaction;
    }
    bitmap_result=openfs_bitmap_set(td,s->inode_bitmap_start,s->inode_bitmap_blocks,ino-1U,0);
    if(bitmap_result!=OPENFS_BITMAP_OK){
        result=bitmap_result==OPENFS_BITMAP_IO_ERROR?OPENFS_ORPHAN_IO_ERROR:OPENFS_ORPHAN_CORRUPT;
        goto abort_transaction;
    }

    tr=openfs_transaction_commit(&transaction);
    transaction_started=transaction.active;
    if(tr!=OPENFS_TRANSACTION_OK){
        result=map_transaction_result(tr);
        if(transaction_started&&transaction.active){
            (void)openfs_transaction_abort(&transaction);
            transaction_started=0;
        }
        goto done;
    }
    transaction_started=0;
    result=OPENFS_ORPHAN_OK;
    goto done;

abort_transaction:
    if(transaction_started&&transaction.active){
        openfs_transaction_result_t ar=openfs_transaction_abort(&transaction);
        transaction_started=0;
        if(ar==OPENFS_TRANSACTION_CORRUPT)
            result=OPENFS_ORPHAN_CORRUPT;
        else if(ar!=OPENFS_TRANSACTION_OK&&result==OPENFS_ORPHAN_OK)
            result=OPENFS_ORPHAN_IO_ERROR;
    }

done:
    if(transaction_started&&transaction.active)
        (void)openfs_transaction_abort(&transaction);
    if(transaction_locked)(void)openfs_mutex_unlock(&runtime->transaction_lock);
    if(allocation_locked)(void)openfs_mutex_unlock(&runtime->allocation_lock);
    if(inode_locked)(void)openfs_mutex_unlock(&runtime->inode_lock);
    if(runtime_entered)openfs_runtime_leave(runtime);
    return result;
}

/* Compatibility path for callers using an unmounted superblock (no WAL). */
static openfs_orphan_result_t reclaim_without_runtime(
    openfs_block_device_t *d,const openfs_superblock_t *s,uint64_t ino,uint64_t count)
{
    int used=0;
    if(openfs_bitmap_test(d,s->inode_bitmap_start,s->inode_bitmap_blocks,ino-1U,&used)!=OPENFS_BITMAP_OK)
        return OPENFS_ORPHAN_IO_ERROR;
    if(!used)return OPENFS_ORPHAN_NOT_ORPHAN;

    openfs_inode_t inode;
    openfs_inode_result_t ir=openfs_inode_read(d,s->inode_table_start,ino,count,&inode);
    if(ir!=OPENFS_INODE_OK)return ir==OPENFS_INODE_CORRUPT?OPENFS_ORPHAN_CORRUPT:OPENFS_ORPHAN_IO_ERROR;
    if((inode.flags&OPENFS_INODE_FLAG_ORPHAN)==0U||inode.link_count!=0U)
        return OPENFS_ORPHAN_NOT_ORPHAN;

    uint64_t xattr_block=openfs_inode_get_xattr_block(&inode);
    openfs_file_result_t fr=openfs_file_truncate(d,s,&inode,0U);
    if(fr!=OPENFS_FILE_OK)return map_file_result(fr);
    if(xattr_block!=0U){
        openfs_alloc_result_t ar=openfs_free_block(d,s,xattr_block);
        if(ar!=OPENFS_ALLOC_OK)return map_alloc_result(ar);
    }

    clear_orphan_inode(&inode);
    if(openfs_inode_write(d,s->inode_table_start,count,&inode)!=OPENFS_INODE_OK)
        return OPENFS_ORPHAN_IO_ERROR;
    openfs_inode_alloc_result_t free_result=openfs_inode_free(d,s,ino);
    if(free_result==OPENFS_INODE_ALLOC_CORRUPT)return OPENFS_ORPHAN_CORRUPT;
    if(free_result!=OPENFS_INODE_ALLOC_OK)return OPENFS_ORPHAN_IO_ERROR;
    return d->flush(d->context)==OPENFS_IO_OK?OPENFS_ORPHAN_OK:OPENFS_ORPHAN_IO_ERROR;
}

openfs_orphan_result_t openfs_orphan_reclaim(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t ino)
{
    if(!openfs_block_device_is_valid(d)||s==NULL||ino==0U)return OPENFS_ORPHAN_INVALID_ARGUMENT;
    uint64_t count=0U;
    if(!inode_count(s,&count)||ino>count)return OPENFS_ORPHAN_CORRUPT;
    if(ino==s->root_inode)return OPENFS_ORPHAN_CORRUPT;
    if(s->runtime!=NULL){
        if(s->runtime->device!=d||s->runtime->journal==NULL)return OPENFS_ORPHAN_IO_ERROR;
        return reclaim_with_runtime(d,s,ino,count);
    }
    if(openfs_transaction_from_device(d)!=NULL)return OPENFS_ORPHAN_INVALID_ARGUMENT;
    return reclaim_without_runtime(d,s,ino,count);
}

openfs_orphan_result_t openfs_orphan_recover_all(openfs_block_device_t*d,const openfs_superblock_t*s)
{
    if(!openfs_block_device_is_valid(d)||s==NULL)return OPENFS_ORPHAN_INVALID_ARGUMENT;
    uint64_t count=0U;if(!inode_count(s,&count))return OPENFS_ORPHAN_CORRUPT;
    if(s->inode_bitmap_blocks>SIZE_MAX/s->block_size)return OPENFS_ORPHAN_CORRUPT;
    size_t bitmap_bytes=(size_t)(s->inode_bitmap_blocks*(uint64_t)s->block_size);
    uint8_t *bitmap=(uint8_t*)malloc(bitmap_bytes);
    if(bitmap==NULL)return OPENFS_ORPHAN_IO_ERROR;
    if(d->read(d->context,s->inode_bitmap_start,s->inode_bitmap_blocks,bitmap)!=OPENFS_IO_OK){free(bitmap);return OPENFS_ORPHAN_IO_ERROR;}
    for(uint64_t ino=1U;ino<=count;ino++){
        uint64_t bit=ino-1U;
        if(bit/8U>=bitmap_bytes)break;
        if((bitmap[(size_t)(bit/8U)]&(uint8_t)(1U<<(bit%8U)))==0U)continue;
        openfs_inode_t inode;openfs_inode_result_t ir=openfs_inode_read(d,s->inode_table_start,ino,count,&inode);
        if(ir==OPENFS_INODE_IO_ERROR){free(bitmap);return OPENFS_ORPHAN_IO_ERROR;}
        if(ir!=OPENFS_INODE_OK)continue;
        if((inode.flags&OPENFS_INODE_FLAG_ORPHAN)!=0U){
            openfs_orphan_result_t r=openfs_orphan_reclaim(d,s,ino);
            if(r!=OPENFS_ORPHAN_OK){free(bitmap);return r;}
        }
    }
    free(bitmap);
    return OPENFS_ORPHAN_OK;
}
