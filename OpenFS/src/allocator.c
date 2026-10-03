#include "openfs/allocator.h"
#include "openfs/bitmap.h"
static openfs_alloc_result_t set_block(openfs_block_device_t*d,const openfs_superblock_t*sb,uint64_t block,int value){
    if(d==NULL||sb==NULL)return OPENFS_ALLOC_INVALID_ARGUMENT;
    if(sb->block_size!=d->block_size||sb->data_start>UINT64_MAX-sb->data_blocks||sb->data_start+sb->data_blocks>d->block_count||sb->block_bitmap_blocks==0U)return OPENFS_ALLOC_CORRUPT;
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
    return d->flush(d->context)==OPENFS_IO_OK?OPENFS_ALLOC_OK:OPENFS_ALLOC_IO_ERROR;
}
openfs_alloc_result_t openfs_alloc_block(openfs_block_device_t*d,const openfs_superblock_t*sb,uint64_t*out){
    if(d==NULL||sb==NULL||out==NULL)return OPENFS_ALLOC_INVALID_ARGUMENT;
    if(sb->block_size!=d->block_size||sb->data_start>UINT64_MAX-sb->data_blocks||sb->data_start+sb->data_blocks>d->block_count||sb->block_bitmap_blocks==0U)return OPENFS_ALLOC_CORRUPT;
    const uint64_t data_end=sb->data_start+sb->data_blocks;
    for(uint64_t b=sb->data_start;b<data_end;++b){
        int used=0;
        if(openfs_bitmap_test(d,sb->block_bitmap_start,sb->block_bitmap_blocks,b,&used)!=OPENFS_BITMAP_OK)return OPENFS_ALLOC_IO_ERROR;
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
            *out=b; return OPENFS_ALLOC_OK;
        }
    }
    return OPENFS_ALLOC_OUT_OF_SPACE;
}
openfs_alloc_result_t openfs_free_block(openfs_block_device_t*d,const openfs_superblock_t*sb,uint64_t block){return set_block(d,sb,block,0);}
