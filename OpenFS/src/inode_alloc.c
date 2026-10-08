#include "openfs/inode_alloc.h"

#include <limits.h>
#include <string.h>
#include "openfs/bitmap.h"
#include "openfs/time.h"
#include "openfs/runtime.h"
#include "openfs/transaction.h"

static openfs_inode_alloc_result_t inode_count(
    const openfs_superblock_t *sb,
    uint64_t *count)
{
    if (sb == NULL || count == NULL) return OPENFS_INODE_ALLOC_INVALID_ARGUMENT;
    if (sb->block_size != 0U &&
        sb->inode_table_blocks > UINT64_MAX / sb->block_size) {
        return OPENFS_INODE_ALLOC_CORRUPT;
    }
    uint64_t bytes=sb->inode_table_blocks*(uint64_t)sb->block_size;if(bytes<OPENFS_INODE_SIZE||bytes%OPENFS_INODE_SIZE!=0U)return OPENFS_INODE_ALLOC_CORRUPT;*count=bytes/OPENFS_INODE_SIZE;return *count==0U?OPENFS_INODE_ALLOC_CORRUPT:OPENFS_INODE_ALLOC_OK;
}

static openfs_inode_alloc_result_t valid(
    const openfs_block_device_t *d,
    const openfs_superblock_t *sb)
{
    if (!openfs_block_device_is_valid(d) || sb == NULL) {
        return OPENFS_INODE_ALLOC_INVALID_ARGUMENT;
    }
    if (sb->block_size != d->block_size ||
        sb->inode_bitmap_blocks == 0U || sb->inode_table_blocks == 0U ||
        sb->inode_bitmap_start >= d->block_count || sb->inode_bitmap_blocks > d->block_count - sb->inode_bitmap_start ||
        sb->inode_table_start >= d->block_count || sb->inode_table_blocks > d->block_count - sb->inode_table_start) {
        return OPENFS_INODE_ALLOC_CORRUPT;
    }
    return OPENFS_INODE_ALLOC_OK;
}

static openfs_inode_alloc_result_t inode_alloc_unlocked(
    openfs_block_device_t *d,
    const openfs_superblock_t *sb,
    uint64_t parent,
    uint32_t mode,
    uint64_t *out)
{
    openfs_inode_alloc_result_t r = valid(d, sb);
    if (r != OPENFS_INODE_ALLOC_OK || out == NULL || parent == 0U) {
        return r != OPENFS_INODE_ALLOC_OK ? r : OPENFS_INODE_ALLOC_INVALID_ARGUMENT;
    }
    uint32_t type = mode & OPENFS_INODE_TYPE_MASK;
    if ((mode & ~(OPENFS_INODE_TYPE_MASK | OPENFS_INODE_PERMISSION_MASK)) != 0U ||
        (type != OPENFS_INODE_MODE_REGULAR && type != OPENFS_INODE_MODE_DIRECTORY && type != OPENFS_INODE_MODE_SYMLINK)) {
        return OPENFS_INODE_ALLOC_INVALID_ARGUMENT;
    }

    uint64_t count = 0U;
    r = inode_count(sb, &count);
    if (r != OPENFS_INODE_ALLOC_OK) return r;
    if (parent > count) return OPENFS_INODE_ALLOC_CORRUPT;
    if (parent == 0U) return OPENFS_INODE_ALLOC_INVALID_ARGUMENT;
    {
        openfs_inode_t parent_inode;
        openfs_inode_result_t pir=openfs_inode_read(d,sb->inode_table_start,parent,count,&parent_inode);if(pir!=OPENFS_INODE_OK)return pir==OPENFS_INODE_IO_ERROR?OPENFS_INODE_ALLOC_IO_ERROR:OPENFS_INODE_ALLOC_CORRUPT;if((parent_inode.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)return OPENFS_INODE_ALLOC_CORRUPT;
    }

    if (count < 2U) return OPENFS_INODE_ALLOC_OUT_OF_SPACE;
    for (uint64_t n = 2U; n <= count; ++n) {
        int used = 0;
        if (openfs_bitmap_test(d, sb->inode_bitmap_start, sb->inode_bitmap_blocks, n - 1U, &used) != OPENFS_BITMAP_OK) {
            return OPENFS_INODE_ALLOC_IO_ERROR;
        }
        if (used) {
            if (n == count) break;
            continue;
        }

        openfs_inode_t previous;
        openfs_inode_result_t pr=openfs_inode_read(d,sb->inode_table_start,n,count,&previous);if(pr!=OPENFS_INODE_OK)return pr==OPENFS_INODE_IO_ERROR?OPENFS_INODE_ALLOC_IO_ERROR:OPENFS_INODE_ALLOC_CORRUPT;
        uint64_t generation = previous.generation;
        if (generation == 0U) generation = 1U;
        if (openfs_bitmap_set(d, sb->inode_bitmap_start, sb->inode_bitmap_blocks, n - 1U, 1) != OPENFS_BITMAP_OK) {
            int rollback_ok=openfs_bitmap_set(d,sb->inode_bitmap_start,sb->inode_bitmap_blocks,n-1U,0)==OPENFS_BITMAP_OK;
            if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
            return rollback_ok?OPENFS_INODE_ALLOC_IO_ERROR:OPENFS_INODE_ALLOC_CORRUPT;
        }

        openfs_inode_t inode;
        memset(&inode, 0, sizeof(inode));
        inode.inode_number = n;
        inode.generation = generation;
        inode.parent_inode = parent;
        inode.link_count = 1U;
        inode.mode = mode;
        uint64_t now = openfs_time_now_ns();
        if (now != UINT64_MAX) {
            inode.atime_ns = now;
            inode.mtime_ns = now;
            inode.ctime_ns = now;
        }

        if (openfs_inode_write(d, sb->inode_table_start, count, &inode) != OPENFS_INODE_OK) {
            int rollback_ok = 1;
            if (openfs_inode_write(d, sb->inode_table_start, count, &previous) != OPENFS_INODE_OK) {
                rollback_ok = 0;
            }
            if (openfs_bitmap_set(d, sb->inode_bitmap_start, sb->inode_bitmap_blocks, n - 1U, 0) != OPENFS_BITMAP_OK) {
                rollback_ok = 0;
            }
            if (d->flush(d->context) != OPENFS_IO_OK) {
                rollback_ok = 0;
            }
            return rollback_ok ? OPENFS_INODE_ALLOC_IO_ERROR : OPENFS_INODE_ALLOC_CORRUPT;
        }
        if (d->flush(d->context) != OPENFS_IO_OK) {
            int rollback_ok = 1;
            if (openfs_inode_write(d, sb->inode_table_start, count, &previous) != OPENFS_INODE_OK) rollback_ok = 0;
            if (openfs_bitmap_set(d, sb->inode_bitmap_start, sb->inode_bitmap_blocks, n - 1U, 0) != OPENFS_BITMAP_OK) rollback_ok = 0;
            if (d->flush(d->context) != OPENFS_IO_OK) rollback_ok = 0;
            return rollback_ok ? OPENFS_INODE_ALLOC_IO_ERROR : OPENFS_INODE_ALLOC_CORRUPT;
        }
        *out = n;
        return OPENFS_INODE_ALLOC_OK;
    }
    return OPENFS_INODE_ALLOC_OUT_OF_SPACE;
}

static openfs_inode_alloc_result_t inode_free_unlocked(
    openfs_block_device_t *d,
    const openfs_superblock_t *sb,
    uint64_t n)
{
    openfs_inode_alloc_result_t r = valid(d, sb);
    if (r != OPENFS_INODE_ALLOC_OK || n == 0U) {
        return r != OPENFS_INODE_ALLOC_OK ? r : OPENFS_INODE_ALLOC_INVALID_ARGUMENT;
    }

    uint64_t count = 0U;
    r = inode_count(sb, &count);
    if (r != OPENFS_INODE_ALLOC_OK || n > count) {
        return OPENFS_INODE_ALLOC_OUT_OF_SPACE;
    }
    if (n == sb->root_inode) {
        return OPENFS_INODE_ALLOC_CORRUPT;
    }

    int used = 0;
    if (openfs_bitmap_test(d, sb->inode_bitmap_start, sb->inode_bitmap_blocks, n - 1U, &used) != OPENFS_BITMAP_OK) {
        return OPENFS_INODE_ALLOC_IO_ERROR;
    }
    if (!used) return OPENFS_INODE_ALLOC_CORRUPT;

    openfs_inode_t inode;
    openfs_inode_result_t ir=openfs_inode_read(d,sb->inode_table_start,n,count,&inode);if(ir!=OPENFS_INODE_OK)return ir==OPENFS_INODE_IO_ERROR?OPENFS_INODE_ALLOC_IO_ERROR:OPENFS_INODE_ALLOC_CORRUPT;
    if (inode.link_count != 0U || inode.mode != OPENFS_INODE_MODE_FREE) {
        return OPENFS_INODE_ALLOC_CORRUPT;
    }
    openfs_inode_t original = inode;
    inode.parent_inode = 0U;
    inode.uid = 0U;
    inode.gid = 0U;
    inode.atime_ns = 0U;
    inode.mtime_ns = 0U;
    inode.ctime_ns = 0U;
    int original_used = 0;
    if (openfs_bitmap_test(d, sb->inode_bitmap_start, sb->inode_bitmap_blocks,
            n - 1U, &original_used) != OPENFS_BITMAP_OK || !original_used) {
        return OPENFS_INODE_ALLOC_CORRUPT;
    }
    if (inode.generation == UINT64_MAX) inode.generation = 1U; else inode.generation++;
    if (openfs_inode_write(d, sb->inode_table_start, count, &inode) != OPENFS_INODE_OK) {
        int rollback_ok = 1;
        if (openfs_inode_write(d, sb->inode_table_start, count, &original) != OPENFS_INODE_OK) rollback_ok = 0;
        if (d->flush(d->context) != OPENFS_IO_OK) rollback_ok = 0;
        return rollback_ok ? OPENFS_INODE_ALLOC_IO_ERROR : OPENFS_INODE_ALLOC_CORRUPT;
    }
    if (openfs_bitmap_set(d, sb->inode_bitmap_start, sb->inode_bitmap_blocks, n - 1U, 0) != OPENFS_BITMAP_OK) {
        int rollback_ok = 1;
        if (openfs_bitmap_set(d, sb->inode_bitmap_start, sb->inode_bitmap_blocks,
                n - 1U, original_used) != OPENFS_BITMAP_OK) rollback_ok = 0;
        if (openfs_inode_write(d, sb->inode_table_start, count, &original) != OPENFS_INODE_OK) rollback_ok = 0;
        if (d->flush(d->context) != OPENFS_IO_OK) rollback_ok = 0;
        return rollback_ok ? OPENFS_INODE_ALLOC_IO_ERROR : OPENFS_INODE_ALLOC_CORRUPT;
    }
    if (d->flush(d->context) != OPENFS_IO_OK) {
        int rollback_ok = 1;
        if (openfs_bitmap_set(d, sb->inode_bitmap_start, sb->inode_bitmap_blocks, n - 1U, 1) != OPENFS_BITMAP_OK) rollback_ok = 0;
        if (openfs_inode_write(d, sb->inode_table_start, count, &original) != OPENFS_INODE_OK) rollback_ok = 0;
        if (d->flush(d->context) != OPENFS_IO_OK) rollback_ok = 0;
        return rollback_ok ? OPENFS_INODE_ALLOC_IO_ERROR : OPENFS_INODE_ALLOC_CORRUPT;
    }
    return OPENFS_INODE_ALLOC_OK;
}

static int inode_transaction_available(const openfs_block_device_t*d,const openfs_superblock_t*sb)
{
    return sb!=NULL&&sb->runtime!=NULL&&sb->runtime->journal!=NULL&&sb->runtime->device==d;
}
static openfs_inode_alloc_result_t map_transaction_result(openfs_transaction_result_t r)
{
    if(r==OPENFS_TRANSACTION_CORRUPT)return OPENFS_INODE_ALLOC_CORRUPT;
    if(r==OPENFS_TRANSACTION_OK)return OPENFS_INODE_ALLOC_OK;
    return OPENFS_INODE_ALLOC_IO_ERROR;
}
openfs_inode_alloc_result_t openfs_inode_alloc(openfs_block_device_t*d,const openfs_superblock_t*sb,uint64_t parent,uint32_t mode,uint64_t*out){
    if(sb==NULL||sb->runtime==NULL)return inode_alloc_unlocked(d,sb,parent,mode,out);
    if(!openfs_runtime_enter(sb->runtime))return OPENFS_INODE_ALLOC_IO_ERROR;
    if(openfs_mutex_lock(&sb->runtime->allocation_lock,OPENFS_LOCK_RANK_ALLOCATION)!=OPENFS_LOCK_OK){openfs_runtime_leave(sb->runtime);return OPENFS_INODE_ALLOC_IO_ERROR;}
    if(!inode_transaction_available(d,sb)){
        openfs_inode_alloc_result_t r=inode_alloc_unlocked(d,sb,parent,mode,out);
        (void)openfs_mutex_unlock(&sb->runtime->allocation_lock);openfs_runtime_leave(sb->runtime);return r;
    }
    if(openfs_mutex_lock(&sb->runtime->transaction_lock,OPENFS_LOCK_RANK_TRANSACTION)!=OPENFS_LOCK_OK){(void)openfs_mutex_unlock(&sb->runtime->allocation_lock);openfs_runtime_leave(sb->runtime);return OPENFS_INODE_ALLOC_IO_ERROR;}
    openfs_transaction_t tx;
    openfs_transaction_result_t tr=openfs_transaction_begin(&tx,d,sb->runtime->journal);
    if(tr!=OPENFS_TRANSACTION_OK){(void)openfs_mutex_unlock(&sb->runtime->transaction_lock);(void)openfs_mutex_unlock(&sb->runtime->allocation_lock);openfs_runtime_leave(sb->runtime);return map_transaction_result(tr);}
    openfs_inode_alloc_result_t r=inode_alloc_unlocked(openfs_transaction_device(&tx),sb,parent,mode,out);
    if(r!=OPENFS_INODE_ALLOC_OK){
        openfs_transaction_result_t ar=openfs_transaction_abort(&tx);
        if(ar==OPENFS_TRANSACTION_CORRUPT)r=OPENFS_INODE_ALLOC_CORRUPT;
        (void)openfs_mutex_unlock(&sb->runtime->transaction_lock);(void)openfs_mutex_unlock(&sb->runtime->allocation_lock);openfs_runtime_leave(sb->runtime);return r;
    }
    tr=openfs_transaction_commit(&tx);
    if(tr!=OPENFS_TRANSACTION_OK)r=tx.recovery_required?OPENFS_INODE_ALLOC_CORRUPT:map_transaction_result(tr);
    (void)openfs_mutex_unlock(&sb->runtime->transaction_lock);(void)openfs_mutex_unlock(&sb->runtime->allocation_lock);openfs_runtime_leave(sb->runtime);return r;
}
openfs_inode_alloc_result_t openfs_inode_free(openfs_block_device_t*d,const openfs_superblock_t*sb,uint64_t n){
    if(sb==NULL||sb->runtime==NULL)return inode_free_unlocked(d,sb,n);
    if(!openfs_runtime_enter(sb->runtime))return OPENFS_INODE_ALLOC_IO_ERROR;
    if(openfs_mutex_lock(&sb->runtime->allocation_lock,OPENFS_LOCK_RANK_ALLOCATION)!=OPENFS_LOCK_OK){openfs_runtime_leave(sb->runtime);return OPENFS_INODE_ALLOC_IO_ERROR;}
    if(!inode_transaction_available(d,sb)){
        openfs_inode_alloc_result_t r=inode_free_unlocked(d,sb,n);
        (void)openfs_mutex_unlock(&sb->runtime->allocation_lock);openfs_runtime_leave(sb->runtime);return r;
    }
    if(openfs_mutex_lock(&sb->runtime->transaction_lock,OPENFS_LOCK_RANK_TRANSACTION)!=OPENFS_LOCK_OK){(void)openfs_mutex_unlock(&sb->runtime->allocation_lock);openfs_runtime_leave(sb->runtime);return OPENFS_INODE_ALLOC_IO_ERROR;}
    openfs_transaction_t tx;
    openfs_transaction_result_t tr=openfs_transaction_begin(&tx,d,sb->runtime->journal);
    if(tr!=OPENFS_TRANSACTION_OK){(void)openfs_mutex_unlock(&sb->runtime->transaction_lock);(void)openfs_mutex_unlock(&sb->runtime->allocation_lock);openfs_runtime_leave(sb->runtime);return map_transaction_result(tr);}
    openfs_inode_alloc_result_t r=inode_free_unlocked(openfs_transaction_device(&tx),sb,n);
    if(r!=OPENFS_INODE_ALLOC_OK){
        openfs_transaction_result_t ar=openfs_transaction_abort(&tx);
        if(ar==OPENFS_TRANSACTION_CORRUPT)r=OPENFS_INODE_ALLOC_CORRUPT;
        (void)openfs_mutex_unlock(&sb->runtime->transaction_lock);(void)openfs_mutex_unlock(&sb->runtime->allocation_lock);openfs_runtime_leave(sb->runtime);return r;
    }
    tr=openfs_transaction_commit(&tx);
    if(tr!=OPENFS_TRANSACTION_OK)r=map_transaction_result(tr);
    (void)openfs_mutex_unlock(&sb->runtime->transaction_lock);(void)openfs_mutex_unlock(&sb->runtime->allocation_lock);openfs_runtime_leave(sb->runtime);return r;
}
