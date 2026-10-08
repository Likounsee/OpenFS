#include "openfs/fsck.h"
#include "openfs/bitmap.h"
#include "openfs/runtime.h"
#include <stdlib.h>
#include <string.h>

static openfs_fsck_result_t repair_bitmap_tail(openfs_block_device_t *d,
    uint64_t start, uint64_t blocks, uint64_t valid_bits)
{
    if(d==NULL||blocks==0U||d->block_size==0U)return OPENFS_FSCK_INVALID_ARGUMENT;
    if(start>=d->block_count||blocks>d->block_count-start)return OPENFS_FSCK_CORRUPT;
    if(blocks>UINT64_MAX/d->block_size)return OPENFS_FSCK_CORRUPT;

    uint64_t bytes=blocks*(uint64_t)d->block_size;
    if(bytes>UINT64_MAX/8U)return OPENFS_FSCK_CORRUPT;
    uint64_t capacity=bytes*8U;
    if(valid_bits>=capacity)return OPENFS_FSCK_OK;

    uint64_t bits_per_block=(uint64_t)d->block_size*8U;
    uint64_t first=valid_bits/bits_per_block;
    uint8_t *buf=(uint8_t *)malloc(d->block_size);
    uint8_t *original=(uint8_t *)malloc(d->block_size);
    if(buf==NULL||original==NULL){
        free(buf);free(original);
        return OPENFS_FSCK_IO_ERROR;
    }

    for(uint64_t block=first;block<blocks;block++){
        if(d->read(d->context,start+block,1U,buf)!=OPENFS_IO_OK){
            free(buf);free(original);
            return OPENFS_FSCK_IO_ERROR;
        }
        memcpy(original,buf,d->block_size);
        uint64_t begin_bit=block==first?valid_bits%bits_per_block:0U;
        for(uint64_t bit=begin_bit;bit<bits_per_block;bit++)
            buf[bit/8U]&=(uint8_t)~(uint8_t)(1U<<(bit%8U));

        if(memcmp(buf,original,d->block_size)==0)continue;
        if(d->write(d->context,start+block,1U,buf)!=OPENFS_IO_OK){
            free(buf);free(original);
            return OPENFS_FSCK_IO_ERROR;
        }
        if(d->flush(d->context)!=OPENFS_IO_OK){
            int restored=d->write(d->context,start+block,1U,original)==OPENFS_IO_OK;
            if(restored)restored=d->flush(d->context)==OPENFS_IO_OK;
            free(buf);free(original);
            return restored?OPENFS_FSCK_IO_ERROR:OPENFS_FSCK_CORRUPT;
        }
    }

    free(buf);free(original);
    return OPENFS_FSCK_OK;
}

static openfs_fsck_result_t repair_lock(const openfs_superblock_t *sb)
{
    if(sb==NULL||sb->runtime==NULL)return OPENFS_FSCK_OK;
    if(!openfs_runtime_enter(sb->runtime))return OPENFS_FSCK_IO_ERROR;
    if(openfs_mutex_lock(&sb->runtime->allocation_lock,OPENFS_LOCK_RANK_ALLOCATION)!=OPENFS_LOCK_OK){
        openfs_runtime_leave(sb->runtime);
        return OPENFS_FSCK_IO_ERROR;
    }
    return OPENFS_FSCK_OK;
}

static void repair_unlock(const openfs_superblock_t *sb)
{
    if(sb==NULL||sb->runtime==NULL)return;
    (void)openfs_mutex_unlock(&sb->runtime->allocation_lock);
    openfs_runtime_leave(sb->runtime);
}

openfs_fsck_result_t openfs_fsck_repair_bitmap_tails(
    openfs_block_device_t *d,const openfs_superblock_t *sb,uint64_t *errors)
{
    if(!openfs_block_device_is_valid(d)||sb==NULL||errors==NULL)
        return OPENFS_FSCK_INVALID_ARGUMENT;
    if(openfs_validate_superblock(d,sb)!=OPENFS_FORMAT_OK)
        return OPENFS_FSCK_CORRUPT;

    uint64_t inode_bytes=0U,inode_count=0U;
    if(sb->inode_table_blocks>UINT64_MAX/sb->block_size)
        return OPENFS_FSCK_CORRUPT;
    inode_bytes=sb->inode_table_blocks*(uint64_t)sb->block_size;
    inode_count=inode_bytes/OPENFS_INODE_SIZE;
    if(inode_count==0U)return OPENFS_FSCK_CORRUPT;

    openfs_fsck_result_t lr=repair_lock(sb);
    if(lr!=OPENFS_FSCK_OK)return lr;

    openfs_fsck_result_t r=repair_bitmap_tail(
        d,sb->block_bitmap_start,sb->block_bitmap_blocks,sb->total_blocks);
    if(r==OPENFS_FSCK_OK)
        r=repair_bitmap_tail(
            d,sb->inode_bitmap_start,sb->inode_bitmap_blocks,inode_count);

    repair_unlock(sb);
    if(r!=OPENFS_FSCK_OK)return r;

    return openfs_fsck(d,sb,errors);
}
