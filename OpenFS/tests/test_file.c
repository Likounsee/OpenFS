#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/file.h"
#include "openfs/format.h"
#include "openfs/bitmap.h"

typedef struct {
    uint8_t *bytes;
    uint32_t block_size;
    uint64_t block_count;
    unsigned flushes;
    uint64_t fail_block;
    int fail_block_enabled;
} disk_t;

static openfs_io_result_t rd(void *ctx,uint64_t first,uint32_t count,void *buffer){
    disk_t*d=ctx;
    if(count==0U||first>=d->block_count||(uint64_t)count>d->block_count-first)return OPENFS_IO_OUT_OF_RANGE;
    memcpy(buffer,d->bytes+(size_t)(first*d->block_size),(size_t)((uint64_t)count*d->block_size));
    return OPENFS_IO_OK;
}
static openfs_io_result_t wr(void *ctx,uint64_t first,uint32_t count,const void *buffer){
    disk_t*d=ctx;
    if(count==0U||first>=d->block_count||(uint64_t)count>d->block_count-first)return OPENFS_IO_OUT_OF_RANGE;
    if(d->fail_block_enabled&&first==d->fail_block)return OPENFS_IO_IO_ERROR;
    memcpy(d->bytes+(size_t)(first*d->block_size),buffer,(size_t)((uint64_t)count*d->block_size));
    return OPENFS_IO_OK;
}
static openfs_io_result_t fl(void *ctx){((disk_t*)ctx)->flushes++;return OPENFS_IO_OK;}
static openfs_block_device_t dev(disk_t*d){return (openfs_block_device_t){d,d->block_size,d->block_count,rd,wr,fl};}

static void setup(disk_t*d,openfs_block_device_t*v,openfs_superblock_t*sb){
    d->block_size=4096U;d->block_count=128U;
    d->bytes=calloc((size_t)d->block_count,d->block_size);assert(d->bytes);
    *v=dev(d);
    uint8_t uuid[16]={0};
    assert(openfs_format(v,uuid)==OPENFS_FORMAT_OK);
    assert(openfs_read_superblock(v,sb)==OPENFS_FORMAT_OK);
}
static openfs_inode_t new_file(void){
    openfs_inode_t i;memset(&i,0,sizeof(i));
    i.inode_number=2U;i.generation=1U;i.mode=OPENFS_INODE_MODE_REGULAR;i.link_count=1U;
    return i;
}

static void basic_rw(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    openfs_inode_t i=new_file();
    const char msg[]="hello OpenFS";
    assert(openfs_file_write(&v,&sb,&i,123U,msg,sizeof(msg))==OPENFS_FILE_OK);
    assert(i.size==123U+sizeof(msg));assert(i.blocks==1U);
    char out[32]={0};size_t got=0U;
    assert(openfs_file_read(&v,&sb,&i,123U,out,sizeof(msg),&got)==OPENFS_FILE_OK);
    assert(got==sizeof(msg)&&memcmp(out,msg,sizeof(msg))==0);
    uint8_t gap[123]={0};assert(openfs_file_read(&v,&sb,&i,0U,gap,sizeof(gap),&got)==OPENFS_FILE_OK);
    assert(got==sizeof(gap));for(size_t n=0;n<sizeof(gap);++n)assert(gap[n]==0U);
    free(d.bytes);
}
static void multi_block_and_truncate(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    openfs_inode_t i=new_file();
    size_t len=4096U*2U+17U;uint8_t *src=malloc(len);uint8_t *dst=malloc(len);assert(src&&dst);
    for(size_t n=0;n<len;++n)src[n]=(uint8_t)(n*17U);
    assert(openfs_file_write(&v,&sb,&i,0U,src,len)==OPENFS_FILE_OK);
    assert(i.size==len&&i.blocks==3U);
    assert(openfs_file_read(&v,&sb,&i,0U,dst,len,&len)==OPENFS_FILE_OK);
    assert(memcmp(src,dst,len)==0);
    assert(openfs_file_truncate(&v,&sb,&i,4096U+1U)==OPENFS_FILE_OK);
    assert(i.size==4097U&&i.blocks==2U);
    assert(openfs_file_truncate(&v,&sb,&i,0U)==OPENFS_FILE_OK);
    assert(i.size==0U&&i.blocks==0U&&i.extent_count==0U);
    free(src);free(dst);free(d.bytes);
}
static void truncate_zero_failure_rolls_back(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    openfs_inode_t i=new_file();
    const uint64_t before_blocks=i.blocks,before_size=i.size,before_extents=i.extent_count;
    d.fail_block=sb.data_start;
    d.fail_block_enabled=1;
    assert(openfs_file_truncate(&v,&sb,&i,8192U)==OPENFS_FILE_IO_ERROR);
    assert(i.blocks==before_blocks&&i.size==before_size&&i.extent_count==before_extents);
    d.fail_block_enabled=0;
    uint64_t allocated=0U;
    assert(openfs_alloc_block(&v,&sb,&allocated)==OPENFS_ALLOC_OK);
    assert(allocated==sb.data_start);
    assert(openfs_free_block(&v,&sb,allocated)==OPENFS_ALLOC_OK);
    free(d.bytes);
}
static void truncate_shrink_inode_write_failure_keeps_blocks(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    openfs_inode_t i=new_file();uint8_t data[4096U*3U];memset(data,0xA5U,sizeof(data));
    assert(openfs_file_write(&v,&sb,&i,0U,data,sizeof(data))==OPENFS_FILE_OK);
    openfs_inode_t before=i;openfs_extent_t last={0};assert(openfs_inode_get_extent(&i,i.extent_count-1U,&last)==OPENFS_EXTENT_OK);
    uint64_t inode_block=sb.inode_table_start+(((i.inode_number-1U)*(uint64_t)OPENFS_INODE_SIZE)/sb.block_size);
    d.fail_block=inode_block;d.fail_block_enabled=1;
    assert(openfs_file_truncate(&v,&sb,&i,4096U)==OPENFS_FILE_IO_ERROR);
    d.fail_block_enabled=0;assert(memcmp(&i,&before,sizeof(i))==0);
    uint64_t allocated=0U;assert(openfs_alloc_block(&v,&sb,&allocated)==OPENFS_ALLOC_OK);
    assert(allocated!=last.physical_start+last.block_count-1U);
    assert(openfs_free_block(&v,&sb,allocated)==OPENFS_ALLOC_OK);
    free(d.bytes);
}
static void extent_limit_rollback(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    openfs_inode_t i=new_file();uint8_t block[4096U];memset(block,0xC3U,sizeof(block));
    for(uint64_t n=0U;n<10U;n++) assert(openfs_bitmap_set(&v,sb.block_bitmap_start,sb.block_bitmap_blocks,sb.data_start+n,1)==OPENFS_BITMAP_OK);
    for(uint32_t n=0U;n<OPENFS_EXTENT_MAX;n++){
        openfs_extent_t e={n,sb.data_start+(uint64_t)n*2U,1U};
        assert(openfs_inode_set_extent(&i,n,&e)==OPENFS_EXTENT_OK);
        assert(openfs_bitmap_set(&v,sb.block_bitmap_start,sb.block_bitmap_blocks,e.physical_start,1)==OPENFS_BITMAP_OK);
    }
    i.blocks=OPENFS_EXTENT_MAX;i.size=(uint64_t)OPENFS_EXTENT_MAX*4096U;
    openfs_inode_t before=i;
    assert(openfs_file_write(&v,&sb,&i,(uint64_t)OPENFS_EXTENT_MAX*4096U,block,sizeof(block))==OPENFS_FILE_OK);assert(i.extent_count==OPENFS_EXTENT_MAX+1U);assert((i.flags&OPENFS_INODE_FLAG_EXTENT_TREE)!=0U);
    assert(memcmp(&i,&before,sizeof(i))!=0);
    assert(i.extent_count==OPENFS_EXTENT_MAX+1U);
    uint64_t mapped=0U;assert(openfs_file_map_block_device(&v,&sb,&i,(uint64_t)OPENFS_EXTENT_MAX*4096U/4096U,&mapped)==OPENFS_FILE_OK);
    assert(mapped==sb.data_start+10U);
    for(uint32_t n=0U;n<OPENFS_EXTENT_MAX;n++){
        uint64_t physical=0U;assert(openfs_file_map_block_device(&v,&sb,&i,n,&physical)==OPENFS_FILE_OK);
        assert(openfs_free_block(&v,&sb,physical)==OPENFS_ALLOC_OK);
    }
    free(d.bytes);
}
static void credential_io(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    openfs_inode_t i=new_file();i.uid=1000U;i.gid=2000U;i.mode=OPENFS_INODE_MODE_REGULAR|0640U;
    const char msg[]="secure";
    assert(openfs_file_write_as(&v,&sb,&i,1000U,2000U,0U,msg,sizeof(msg))==OPENFS_FILE_OK);
    char out[16]={0};size_t got=0U;
    assert(openfs_file_read_as(&v,&sb,&i,1000U,2000U,0U,out,sizeof(msg),&got)==OPENFS_FILE_OK);
    assert(got==sizeof(msg)&&memcmp(out,msg,sizeof(msg))==0);
    assert(openfs_file_read_as(&v,&sb,&i,3000U,3000U,0U,out,sizeof(msg),&got)==OPENFS_FILE_ACCESS_DENIED);
    assert(openfs_file_write_as(&v,&sb,&i,3000U,3000U,0U,msg,sizeof(msg))==OPENFS_FILE_ACCESS_DENIED);
    assert(openfs_file_write_as(&v,&sb,&i,3000U,2000U,0U,msg,sizeof(msg))==OPENFS_FILE_ACCESS_DENIED);
    assert(openfs_file_truncate_as(&v,&sb,&i,1000U,2000U,0U)==OPENFS_FILE_OK);
    free(d.bytes);
}
static void map_bounds(void){
    openfs_inode_t i=new_file();uint64_t p=0U;
    openfs_extent_t e={0U,42U,2U};assert(openfs_inode_set_extent(&i,0U,&e)==OPENFS_EXTENT_OK);i.blocks=2U;
    assert(openfs_file_map_block(&i,0U,&p)==OPENFS_FILE_OK&&p==42U);
    assert(openfs_file_map_block(&i,1U,&p)==OPENFS_FILE_OK&&p==43U);
    assert(openfs_file_map_block(&i,2U,&p)==OPENFS_FILE_OUT_OF_RANGE);
    openfs_extent_t e2={2U,10U,1U};
    assert(openfs_inode_set_extent(&i,1U,&e2)==OPENFS_EXTENT_OK);i.blocks=3U;
    assert(openfs_file_map_block(&i,2U,&p)==OPENFS_FILE_OK&&p==10U);
}
int main(void){basic_rw();credential_io();multi_block_and_truncate();truncate_zero_failure_rolls_back();truncate_shrink_inode_write_failure_keeps_blocks();extent_limit_rollback();map_bounds();return 0;}
