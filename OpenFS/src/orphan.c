#include "openfs/orphan.h"
#include "openfs/bitmap.h"
#include "openfs/file.h"
#include "openfs/inode.h"
#include "openfs/inode_alloc.h"

#include <stdint.h>
#include <stdlib.h>

static int inode_count(const openfs_superblock_t *s,uint64_t *out)
{
    if(s==NULL||out==NULL||s->block_size==0U||s->inode_table_blocks>UINT64_MAX/s->block_size)return 0;
    uint64_t bytes=s->inode_table_blocks*(uint64_t)s->block_size;
    if(bytes<OPENFS_INODE_SIZE)return 0;
    *out=bytes/OPENFS_INODE_SIZE;
    return *out!=0U;
}

openfs_orphan_result_t openfs_orphan_reclaim(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t ino)
{
    if(!openfs_block_device_is_valid(d)||s==NULL||ino==0U)return OPENFS_ORPHAN_INVALID_ARGUMENT;
    uint64_t count=0U;if(!inode_count(s,&count)||ino>count)return OPENFS_ORPHAN_CORRUPT;
    int used=0;if(openfs_bitmap_test(d,s->inode_bitmap_start,s->inode_bitmap_blocks,ino-1U,&used)!=OPENFS_BITMAP_OK)return OPENFS_ORPHAN_IO_ERROR;
    if(!used)return OPENFS_ORPHAN_NOT_ORPHAN;
    openfs_inode_t inode;openfs_inode_result_t ir=openfs_inode_read(d,s->inode_table_start,ino,count,&inode);
    if(ir!=OPENFS_INODE_OK)return ir==OPENFS_INODE_CORRUPT?OPENFS_ORPHAN_CORRUPT:OPENFS_ORPHAN_IO_ERROR;
    if((inode.flags&OPENFS_INODE_FLAG_ORPHAN)==0U||inode.link_count!=0U)return OPENFS_ORPHAN_NOT_ORPHAN;
    openfs_file_result_t fr=openfs_file_truncate(d,s,&inode,0U);
    if(fr!=OPENFS_FILE_OK)return fr==OPENFS_FILE_CORRUPT?OPENFS_ORPHAN_CORRUPT:OPENFS_ORPHAN_IO_ERROR;
    inode.mode=OPENFS_INODE_MODE_FREE;inode.link_count=0U;inode.parent_inode=0U;inode.flags=0U;inode.extent_count=0U;inode.blocks=0U;inode.size=0U;inode.uid=0U;inode.gid=0U;inode.atime_ns=0U;inode.mtime_ns=0U;inode.ctime_ns=0U;memset(inode.inline_data,0,sizeof(inode.inline_data));memset(inode.reserved,0,sizeof(inode.reserved));
    if(openfs_inode_write(d,s->inode_table_start,count,&inode)!=OPENFS_INODE_OK)return OPENFS_ORPHAN_IO_ERROR;
    if(openfs_inode_free(d,s,ino)!=OPENFS_INODE_ALLOC_OK)return OPENFS_ORPHAN_IO_ERROR;
    return d->flush(d->context)==OPENFS_IO_OK?OPENFS_ORPHAN_OK:OPENFS_ORPHAN_IO_ERROR;
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
