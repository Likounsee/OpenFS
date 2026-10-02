#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/file.h"
#include "openfs/format.h"

typedef struct {
    uint8_t *bytes;
    uint32_t block_size;
    uint64_t block_count;
    unsigned flushes;
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
    disk_t d;openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
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
    disk_t d;openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
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
static void map_bounds(void){
    openfs_inode_t i=new_file();uint64_t p=0U;
    openfs_extent_t e={0U,42U,2U};assert(openfs_inode_set_extent(&i,0U,&e)==OPENFS_EXTENT_OK);i.blocks=2U;
    assert(openfs_file_map_block(&i,0U,&p)==OPENFS_FILE_OK&&p==42U);
    assert(openfs_file_map_block(&i,1U,&p)==OPENFS_FILE_OK&&p==43U);
    assert(openfs_file_map_block(&i,2U,&p)==OPENFS_FILE_OUT_OF_RANGE);
}
int main(void){basic_rw();multi_block_and_truncate();map_bounds();return 0;}
