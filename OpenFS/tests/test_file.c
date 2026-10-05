#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/file.h"
#include "openfs/format.h"
#include "openfs/bitmap.h"
#include "openfs/fsck.h"
#include "openfs/path.h"
#include "openfs/mount.h"
#include "openfs/allocator.h"
#include "openfs/crc32c.h"

typedef struct {
    uint8_t *bytes;
    uint32_t block_size;
    uint64_t block_count;
    unsigned flushes;
    uint64_t fail_block;
    int fail_block_enabled;
    unsigned fail_after_writes;
    int fail_once;
    int fail_after_write;
    size_t partial_write_bytes;
    int fail_flush;
    int fail_flush_once;
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
    if(d->fail_block_enabled&&first==d->fail_block){if(d->fail_after_writes!=0U&&--d->fail_after_writes!=0U){}else{if(d->fail_once)d->fail_block_enabled=0;if(d->fail_after_write){size_t bytes=(size_t)((uint64_t)count*d->block_size);if(d->partial_write_bytes!=0U&&d->partial_write_bytes<bytes)bytes=d->partial_write_bytes;memcpy(d->bytes+(size_t)(first*d->block_size),buffer,bytes);d->fail_after_write=0;return OPENFS_IO_IO_ERROR;}return OPENFS_IO_IO_ERROR;}}
    memcpy(d->bytes+(size_t)(first*d->block_size),buffer,(size_t)((uint64_t)count*d->block_size));
    return OPENFS_IO_OK;
}
static openfs_io_result_t fl(void *ctx){disk_t*d=ctx;d->flushes++;if(d->fail_flush){if(d->fail_flush_once)d->fail_flush=0;return OPENFS_IO_IO_ERROR;}return OPENFS_IO_OK;}
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
    i.inode_number=2U;i.generation=1U;i.parent_inode=1U;i.mode=OPENFS_INODE_MODE_REGULAR;i.link_count=1U;
    return i;
}

static void write_allocation_failure_rolls_back_partial_allocations(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    uint64_t ino=0U;assert(openfs_path_create(&v,&sb,"/write-allocation-rollback",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);
    uint64_t inode_count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t i;assert(openfs_inode_read(&v,sb.inode_table_start,ino,inode_count,&i)==OPENFS_INODE_OK);
    size_t bitmap_bytes=(size_t)sb.block_bitmap_blocks*sb.block_size;
    uint8_t *bitmap_before=malloc(bitmap_bytes);assert(bitmap_before);
    memcpy(bitmap_before,d.bytes+(size_t)(sb.block_bitmap_start*d.block_size),bitmap_bytes);
    openfs_inode_t before=i;
    uint64_t bitmap_block=sb.block_bitmap_start;
    d.fail_block=bitmap_block;d.fail_block_enabled=1;d.fail_after_writes=2U;d.fail_once=1;
    uint8_t data[8192U];memset(data,0xA6U,sizeof(data));
    assert(openfs_file_write(&v,&sb,&i,0U,data,sizeof(data))==OPENFS_FILE_IO_ERROR);
    assert(memcmp(&i,&before,sizeof(i))==0);
    assert(memcmp(bitmap_before,d.bytes+(size_t)(sb.block_bitmap_start*d.block_size),bitmap_bytes)==0);
    uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    free(bitmap_before);free(d.bytes);
}

static void write_flush_failure_rolls_back_media(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    uint64_t ino=0U;assert(openfs_path_create(&v,&sb,"/write-flush-rollback",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);
    uint64_t inode_count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t i;assert(openfs_inode_read(&v,sb.inode_table_start,ino,inode_count,&i)==OPENFS_INODE_OK);
    uint8_t initial[4096U],update[5000U];memset(initial,0x31U,sizeof(initial));memset(update,0xE7U,sizeof(update));
    assert(openfs_file_write(&v,&sb,&i,0U,initial,sizeof(initial))==OPENFS_FILE_OK);
    openfs_inode_t before=i;
    uint64_t inode_block=sb.inode_table_start+((i.inode_number-1U)*(uint64_t)OPENFS_INODE_SIZE)/sb.block_size;
    uint8_t *before_inode=malloc(sb.block_size);uint8_t *before_data=malloc(sb.block_size);
    assert(before_inode&&before_data);
    assert(v.read(v.context,inode_block,1U,before_inode)==OPENFS_IO_OK);
    uint64_t physical=0U;assert(openfs_file_map_block_device(&v,&sb,&i,0U,&physical)==OPENFS_FILE_OK);
    assert(v.read(v.context,physical,1U,before_data)==OPENFS_IO_OK);
    d.fail_flush=1;d.fail_flush_once=1;
    assert(openfs_file_write(&v,&sb,&i,100U,update,sizeof(update))==OPENFS_FILE_IO_ERROR);
    assert(memcmp(&i,&before,sizeof(i))==0);
    assert(memcmp(d.bytes+(size_t)(inode_block*d.block_size),before_inode,sb.block_size)==0);
    assert(memcmp(d.bytes+(size_t)(physical*d.block_size),before_data,sb.block_size)==0);
    openfs_inode_t persisted;assert(openfs_inode_read(&v,sb.inode_table_start,i.inode_number,inode_count,&persisted)==OPENFS_INODE_OK);
    assert(memcmp(&persisted,&before,sizeof(before))==0);
    uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    openfs_mount_t m;assert(openfs_mount(&m,&v)==OPENFS_MOUNT_OK);assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
    free(before_inode);free(before_data);free(d.bytes);
}

static void truncate_tree_shrink_releases_root(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    uint64_t ino=0U;assert(openfs_path_create(&v,&sb,"/tree-shrink",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);
    uint64_t ic=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t i;assert(openfs_inode_read(&v,sb.inode_table_start,ino,ic,&i)==OPENFS_INODE_OK);
    uint64_t physical[10];for(unsigned n=0U;n<10U;n++)assert(openfs_alloc_block(&v,&sb,&physical[n])==OPENFS_ALLOC_OK);
    for(unsigned n=1U;n<10U;n+=2U)assert(openfs_free_block(&v,&sb,physical[n])==OPENFS_ALLOC_OK);
    memset(i.inline_data,0,sizeof(i.inline_data));memset(i.reserved,0,sizeof(i.reserved));i.flags=0U;i.extent_count=0U;i.blocks=5U;i.size=5U*4096U;
    for(uint32_t n=0U;n<5U;n++){openfs_extent_t e={n,physical[n*2U],1U};assert(openfs_inode_set_extent(&i,n,&e)==OPENFS_EXTENT_OK);}
    assert(openfs_inode_write(&v,sb.inode_table_start,ic,&i)==OPENFS_INODE_OK);
    uint8_t value=0x4DU;assert(openfs_file_write(&v,&sb,&i,5U*4096U,&value,1U)==OPENFS_FILE_OK);
    assert((i.flags&OPENFS_INODE_FLAG_EXTENT_TREE)!=0U);
    uint64_t root=openfs_inode_get_extent_tree_root(&i);assert(root!=0U);
    int used=0;assert(openfs_bitmap_test(&v,sb.block_bitmap_start,sb.block_bitmap_blocks,root,&used)==OPENFS_BITMAP_OK&&used);
    assert(openfs_file_truncate(&v,&sb,&i,4096U)==OPENFS_FILE_OK);
    assert((i.flags&OPENFS_INODE_FLAG_EXTENT_TREE)==0U);
    assert(openfs_inode_get_extent_tree_root(&i)==0U);
    assert(openfs_bitmap_test(&v,sb.block_bitmap_start,sb.block_bitmap_blocks,root,&used)==OPENFS_BITMAP_OK&&!used);
    uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    free(d.bytes);
}

static void truncate_tree_shrink_free_failure_restores_root(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    uint64_t ino=0U;assert(openfs_path_create(&v,&sb,"/tree-shrink-free-failure",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);
    uint64_t ic=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t i;assert(openfs_inode_read(&v,sb.inode_table_start,ino,ic,&i)==OPENFS_INODE_OK);
    uint64_t physical[10];for(unsigned n=0U;n<10U;n++)assert(openfs_alloc_block(&v,&sb,&physical[n])==OPENFS_ALLOC_OK);
    for(unsigned n=1U;n<10U;n+=2U)assert(openfs_free_block(&v,&sb,physical[n])==OPENFS_ALLOC_OK);
    memset(i.inline_data,0,sizeof(i.inline_data));memset(i.reserved,0,sizeof(i.reserved));i.flags=0U;i.extent_count=0U;i.blocks=5U;i.size=5U*4096U;
    for(uint32_t n=0U;n<5U;n++){openfs_extent_t e={n,physical[n*2U],1U};assert(openfs_inode_set_extent(&i,n,&e)==OPENFS_EXTENT_OK);}
    assert(openfs_inode_write(&v,sb.inode_table_start,ic,&i)==OPENFS_INODE_OK);
    uint8_t value=0x5AU;assert(openfs_file_write(&v,&sb,&i,5U*4096U,&value,1U)==OPENFS_FILE_OK);
    assert((i.flags&OPENFS_INODE_FLAG_EXTENT_TREE)!=0U);
    uint64_t root=openfs_inode_get_extent_tree_root(&i);assert(root!=0U);
    uint8_t *root_before=malloc(sb.block_size);assert(root_before);
    assert(v.read(v.context,root,1U,root_before)==OPENFS_IO_OK);
    openfs_extent_t removed_extent;assert(openfs_inode_get_extent(&i,1U,&removed_extent)==OPENFS_EXTENT_OK);
    uint64_t bitmap_block=sb.block_bitmap_start+(removed_extent.physical_start/8U)/sb.block_size;
    d.fail_block=bitmap_block;d.fail_block_enabled=1;d.fail_once=1;
    assert(openfs_file_truncate(&v,&sb,&i,4096U)==OPENFS_FILE_IO_ERROR);
    d.fail_block_enabled=0;
    openfs_inode_t persisted;assert(openfs_inode_read(&v,sb.inode_table_start,ino,ic,&persisted)==OPENFS_INODE_OK);
    assert(memcmp(&persisted,&i,sizeof(i))==0);
    assert(memcmp(d.bytes+(size_t)(root*d.block_size),root_before,sb.block_size)==0);
    uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    free(root_before);free(d.bytes);
}

static void sparse_write_zeroes_intermediate_blocks(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    openfs_inode_t i=new_file();uint8_t one=0x5AU,tail=0xA7U;uint64_t poisoned[3];
    assert(openfs_file_write(&v,&sb,&i,0U,&one,1U)==OPENFS_FILE_OK);
    for(unsigned n=0U;n<3U;n++){assert(openfs_alloc_block(&v,&sb,&poisoned[n])==OPENFS_ALLOC_OK);assert(poisoned[n]==sb.data_start+1U+n);memset(d.bytes+(size_t)(poisoned[n]*d.block_size),0xCCU,d.block_size);}
    for(unsigned n=0U;n<3U;n++)assert(openfs_free_block(&v,&sb,poisoned[n])==OPENFS_ALLOC_OK);
    assert(openfs_file_write(&v,&sb,&i,4U*4096U,&tail,1U)==OPENFS_FILE_OK);
    uint8_t *gap=calloc(3U,4096U);assert(gap);size_t got=0U;assert(openfs_file_read(&v,&sb,&i,4096U,gap,3U*4096U,&got)==OPENFS_FILE_OK&&got==3U*4096U);
    for(size_t n=0;n<3U*4096U;n++)assert(gap[n]==0U);
    free(gap);free(d.bytes);
}
static void file_read_rejects_unallocated_extent(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    openfs_inode_t i=new_file();uint8_t value=0x5AU,out=0U;size_t got=0U;
    assert(openfs_file_write(&v,&sb,&i,0U,&value,1U)==OPENFS_FILE_OK);
    openfs_extent_t e;assert(openfs_inode_get_extent(&i,0U,&e)==OPENFS_EXTENT_OK);
    assert(openfs_bitmap_set(&v,sb.block_bitmap_start,sb.block_bitmap_blocks,e.physical_start,0)==OPENFS_BITMAP_OK);
    assert(openfs_file_read(&v,&sb,&i,0U,&out,1U,&got)==OPENFS_FILE_CORRUPT&&got==0U);
    assert(openfs_bitmap_set(&v,sb.block_bitmap_start,sb.block_bitmap_blocks,e.physical_start,1)==OPENFS_BITMAP_OK);
    uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    free(d.bytes);
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
static void extent_tree_large_file(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    openfs_inode_t i=new_file();uint8_t block[4096U];memset(block,0xC3U,sizeof(block));
    for(uint64_t n=0U;n<10U;n++) assert(openfs_bitmap_set(&v,sb.block_bitmap_start,sb.block_bitmap_blocks,sb.data_start+n,1)==OPENFS_BITMAP_OK);
    for(uint32_t n=0U;n<OPENFS_INODE_TREE_INLINE_EXTENT_MAX;n++){
        openfs_extent_t e={n,sb.data_start+(uint64_t)n*2U,1U};
        assert(openfs_inode_set_extent(&i,n,&e)==OPENFS_EXTENT_OK);
        assert(openfs_bitmap_set(&v,sb.block_bitmap_start,sb.block_bitmap_blocks,e.physical_start,1)==OPENFS_BITMAP_OK);
    }
    i.blocks=OPENFS_INODE_TREE_INLINE_EXTENT_MAX;i.size=(uint64_t)OPENFS_INODE_TREE_INLINE_EXTENT_MAX*4096U;
    openfs_inode_t before=i;
    assert(openfs_file_write(&v,&sb,&i,(uint64_t)OPENFS_INODE_TREE_INLINE_EXTENT_MAX*4096U,block,sizeof(block))==OPENFS_FILE_OK);assert(i.extent_count==OPENFS_INODE_TREE_INLINE_EXTENT_MAX+1U);assert((i.flags&OPENFS_INODE_FLAG_EXTENT_TREE)!=0U);assert((i.flags&OPENFS_INODE_FLAG_HAS_EXTENTS)!=0U);
    assert(memcmp(&i,&before,sizeof(i))!=0);
    assert(i.extent_count==OPENFS_INODE_TREE_INLINE_EXTENT_MAX+1U);
    uint64_t mapped=0U;assert(openfs_file_map_block_device(&v,&sb,&i,(uint64_t)OPENFS_INODE_TREE_INLINE_EXTENT_MAX,&mapped)==OPENFS_FILE_OK);
    assert(mapped==sb.data_start+10U);
    uint8_t out[4096U]={0};size_t got=0U;assert(openfs_file_read(&v,&sb,&i,(uint64_t)OPENFS_INODE_TREE_INLINE_EXTENT_MAX*4096U,out,sizeof(out),&got)==OPENFS_FILE_OK&&got==sizeof(out)&&out[0]==0xC3U);
    uint64_t tree_root=openfs_inode_get_extent_tree_root(&i);assert(tree_root<d.block_count);
    uint8_t *tree_saved=malloc(d.block_size);uint8_t *tree_corrupt=malloc(d.block_size);assert(tree_saved&&tree_corrupt);
    memcpy(tree_saved,d.bytes+(size_t)(tree_root*d.block_size),d.block_size);memcpy(tree_corrupt,tree_saved,d.block_size);
    uint64_t overlap_root=tree_root;for(unsigned k=0;k<8U;k++)tree_corrupt[OPENFS_EXTENT_TREE_HEADER_SIZE+8U+k]=(uint8_t)(overlap_root>>(8U*k));
    tree_corrupt[24U]=tree_corrupt[25U]=tree_corrupt[26U]=tree_corrupt[27U]=0U;
    uint32_t tree_crc=openfs_crc32c(tree_corrupt,d.block_size-4U);
    tree_corrupt[24U]=(uint8_t)tree_crc;tree_corrupt[25U]=(uint8_t)(tree_crc>>8U);tree_corrupt[26U]=(uint8_t)(tree_crc>>16U);tree_corrupt[27U]=(uint8_t)(tree_crc>>24U);
    memcpy(d.bytes+(size_t)(tree_root*d.block_size),tree_corrupt,d.block_size);
    assert(openfs_file_read(&v,&sb,&i,(uint64_t)OPENFS_INODE_TREE_INLINE_EXTENT_MAX*4096U,out,sizeof(out),&got)==OPENFS_FILE_CORRUPT);
    memcpy(d.bytes+(size_t)(tree_root*d.block_size),tree_saved,d.block_size);
    openfs_inode_t malformed=i;malformed.blocks++;malformed.size=malformed.blocks*4096U;assert(openfs_inode_write(&v,sb.inode_table_start,(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE,&malformed)==OPENFS_INODE_OK);openfs_inode_t crc_valid_large;assert(openfs_inode_read(&v,sb.inode_table_start,i.inode_number,(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE,&crc_valid_large)==OPENFS_INODE_OK);assert(openfs_file_read(&v,&sb,&crc_valid_large,(uint64_t)OPENFS_INODE_TREE_INLINE_EXTENT_MAX*4096U,out,sizeof(out),&got)==OPENFS_FILE_CORRUPT);
    malformed=i;malformed.blocks--;malformed.size=malformed.blocks*4096U;assert(openfs_inode_write(&v,sb.inode_table_start,(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE,&malformed)==OPENFS_INODE_OK);openfs_inode_t crc_valid_small;assert(openfs_inode_read(&v,sb.inode_table_start,i.inode_number,(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE,&crc_valid_small)==OPENFS_INODE_OK);assert(openfs_file_read(&v,&sb,&crc_valid_small,(uint64_t)OPENFS_INODE_TREE_INLINE_EXTENT_MAX*4096U,out,sizeof(out),&got)==OPENFS_FILE_CORRUPT);assert(openfs_inode_write(&v,sb.inode_table_start,(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE,&i)==OPENFS_INODE_OK);
    free(tree_saved);free(tree_corrupt);
uint8_t saved_tree=d.bytes[(size_t)(tree_root*d.block_size)];d.bytes[(size_t)(tree_root*d.block_size)]^=0x5AU;assert(openfs_file_read(&v,&sb,&i,(uint64_t)OPENFS_INODE_TREE_INLINE_EXTENT_MAX*4096U,out,sizeof(out),&got)==OPENFS_FILE_CORRUPT);d.bytes[(size_t)(tree_root*d.block_size)]=saved_tree;
    assert(openfs_file_truncate(&v,&sb,&i,3U*4096U)==OPENFS_FILE_OK);assert(i.blocks==3U&&i.size==3U*4096U);assert((i.flags&OPENFS_INODE_FLAG_EXTENT_TREE)==0U);assert((i.flags&OPENFS_INODE_FLAG_HAS_EXTENTS)!=0U);
    for(uint32_t n=0U;n<3U;n++){
        uint64_t physical=0U;assert(openfs_file_map_block_device(&v,&sb,&i,n,&physical)==OPENFS_FILE_OK);
        assert(openfs_free_block(&v,&sb,physical)==OPENFS_ALLOC_OK);
    }
    free(d.bytes);
}
static void truncate_grow_flush_failure_rolls_back(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    uint64_t inode_number=0U;assert(openfs_path_create(&v,&sb,"/truncate_grow_flush_failure_rolls_back",OPENFS_INODE_MODE_REGULAR,&inode_number)==OPENFS_PATH_OK);uint64_t inode_count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;openfs_inode_t i;assert(openfs_inode_read(&v,sb.inode_table_start,inode_number,inode_count,&i)==OPENFS_INODE_OK);uint8_t data[4096U];memset(data,0x31U,sizeof(data));
    assert(openfs_file_write(&v,&sb,&i,0U,data,sizeof(data))==OPENFS_FILE_OK);
    openfs_inode_t before=i;d.fail_flush=1;d.fail_flush_once=1;
    assert(openfs_file_truncate(&v,&sb,&i,8192U)==OPENFS_FILE_IO_ERROR);
    assert(memcmp(&i,&before,sizeof(i))==0);
    uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    free(d.bytes);
}
static void truncate_grow_partial_tail_inode_failure_restores_data(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    uint64_t inode_number=0U;assert(openfs_path_create(&v,&sb,"/truncate_grow_partial_tail_inode_failure_restores_data",OPENFS_INODE_MODE_REGULAR,&inode_number)==OPENFS_PATH_OK);uint64_t inode_count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;openfs_inode_t i;assert(openfs_inode_read(&v,sb.inode_table_start,inode_number,inode_count,&i)==OPENFS_INODE_OK);uint8_t initial[100U];memset(initial,0xD4U,sizeof(initial));
    assert(openfs_file_write(&v,&sb,&i,0U,initial,sizeof(initial))==OPENFS_FILE_OK);
    openfs_inode_t before=i;uint64_t physical=0U;assert(openfs_file_map_block_device(&v,&sb,&i,0U,&physical)==OPENFS_FILE_OK);
    uint8_t *saved=malloc(d.block_size);assert(saved);memcpy(saved,d.bytes+(size_t)(physical*d.block_size),d.block_size);
    uint64_t inode_block=sb.inode_table_start+(((i.inode_number-1U)*(uint64_t)OPENFS_INODE_SIZE)/sb.block_size);
    d.fail_block=inode_block;d.fail_block_enabled=1;d.fail_once=1;
    assert(openfs_file_truncate(&v,&sb,&i,5000U)==OPENFS_FILE_IO_ERROR);
    assert(memcmp(&i,&before,sizeof(i))==0);
    assert(memcmp(saved,d.bytes+(size_t)(physical*d.block_size),d.block_size)==0);
    uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    free(saved);free(d.bytes);
}
static void truncate_shrink_flush_failure_rolls_back(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    uint64_t inode_number=0U;assert(openfs_path_create(&v,&sb,"/truncate_shrink_flush_failure_rolls_back",OPENFS_INODE_MODE_REGULAR,&inode_number)==OPENFS_PATH_OK);uint64_t inode_count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;openfs_inode_t i;assert(openfs_inode_read(&v,sb.inode_table_start,inode_number,inode_count,&i)==OPENFS_INODE_OK);uint8_t data[12288U];memset(data,0x44U,sizeof(data));
    assert(openfs_file_write(&v,&sb,&i,0U,data,sizeof(data))==OPENFS_FILE_OK);
    openfs_inode_t before=i;d.fail_flush=1;d.fail_flush_once=1;
    assert(openfs_file_truncate(&v,&sb,&i,4096U)==OPENFS_FILE_IO_ERROR);
    assert(memcmp(&i,&before,sizeof(i))==0);
    uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    free(d.bytes);
}
static void truncate_tree_inode_write_failure_is_persistent_atomic(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    uint64_t inode_number=0U,filler_number=0U;assert(openfs_path_create(&v,&sb,"/truncate-tree",OPENFS_INODE_MODE_REGULAR,&inode_number)==OPENFS_PATH_OK);assert(openfs_path_create(&v,&sb,"/truncate-filler",OPENFS_INODE_MODE_REGULAR,&filler_number)==OPENFS_PATH_OK);
    uint64_t inode_count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;openfs_inode_t i,filler;assert(openfs_inode_read(&v,sb.inode_table_start,inode_number,inode_count,&i)==OPENFS_INODE_OK);assert(openfs_inode_read(&v,sb.inode_table_start,filler_number,inode_count,&filler)==OPENFS_INODE_OK);
    uint8_t a[4096U],b[4096U];memset(a,0x5AU,sizeof(a));memset(b,0x27U,sizeof(b));
    for(uint64_t n=0U;n<OPENFS_EXTENT_MAX;n++){assert(openfs_file_write(&v,&sb,&i,n*4096U,a,sizeof(a))==OPENFS_FILE_OK);assert(openfs_inode_read(&v,sb.inode_table_start,inode_number,inode_count,&i)==OPENFS_INODE_OK);assert(openfs_file_write(&v,&sb,&filler,n*4096U,b,sizeof(b))==OPENFS_FILE_OK);assert(openfs_inode_read(&v,sb.inode_table_start,filler_number,inode_count,&filler)==OPENFS_INODE_OK);}
    assert(i.extent_count==OPENFS_EXTENT_MAX);uint64_t baseline_errors=0U;assert(openfs_fsck(&v,&sb,&baseline_errors)==OPENFS_FSCK_OK&&baseline_errors==0U);
    openfs_inode_t before=i;uint64_t inode_block=sb.inode_table_start+(((i.inode_number-1U)*(uint64_t)OPENFS_INODE_SIZE)/sb.block_size);size_t bitmap_bytes=(size_t)sb.block_bitmap_blocks*sb.block_size;uint8_t *bitmap_snapshot=malloc(bitmap_bytes);uint8_t *inode_snapshot=malloc(sb.block_size);assert(bitmap_snapshot&&inode_snapshot);memcpy(bitmap_snapshot,d.bytes+(size_t)(sb.block_bitmap_start*d.block_size),bitmap_bytes);memcpy(inode_snapshot,d.bytes+(size_t)(inode_block*d.block_size),sb.block_size);
    d.fail_block=inode_block;d.fail_block_enabled=1;d.fail_once=1;assert(openfs_file_truncate(&v,&sb,&i,4096U)==OPENFS_FILE_IO_ERROR);assert(memcmp(&i,&before,sizeof(i))==0);assert(memcmp(bitmap_snapshot,d.bytes+(size_t)(sb.block_bitmap_start*d.block_size),bitmap_bytes)==0);assert(memcmp(inode_snapshot,d.bytes+(size_t)(inode_block*d.block_size),sb.block_size)==0);
    uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);free(bitmap_snapshot);free(inode_snapshot);free(d.bytes);
}
static void shrink_preserves_live_extent_tree_root(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    uint64_t ino=0U;assert(openfs_path_create(&v,&sb,"/shrink-tree-root",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);
    uint64_t ic=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t i;assert(openfs_inode_read(&v,sb.inode_table_start,ino,ic,&i)==OPENFS_INODE_OK);
    uint64_t root=0U,blocks[6];assert(openfs_alloc_block(&v,&sb,&root)==OPENFS_ALLOC_OK);
    for(unsigned n=0U;n<6U;n++)assert(openfs_alloc_block(&v,&sb,&blocks[n])==OPENFS_ALLOC_OK);
    i.blocks=6U;i.size=6U*sb.block_size;i.extent_count=0U;
    for(uint32_t n=0U;n<4U;n++){openfs_extent_t e={n,blocks[n],1U};assert(openfs_inode_set_extent(&i,n,&e)==OPENFS_EXTENT_OK);}
    assert(openfs_inode_set_extent_tree_root(&i,root)==OPENFS_EXTENT_OK);i.extent_count=6U;
    openfs_extent_t tree_gap[2]={{5U,blocks[4],1U},{6U,blocks[5],1U}};
    assert(openfs_extent_tree_write(&v,&sb,&i,tree_gap,2U)==OPENFS_EXTENT_CORRUPT);
    openfs_extent_t tree[2]={{4U,blocks[4],1U},{5U,blocks[5],1U}};
    assert(openfs_extent_tree_write(&v,&sb,&i,&tree[0],2U)==OPENFS_EXTENT_OK);
    assert(openfs_inode_write(&v,sb.inode_table_start,ic,&i)==OPENFS_INODE_OK);
    assert(openfs_file_truncate(&v,&sb,&i,5U*sb.block_size)==OPENFS_FILE_OK);
    assert(openfs_inode_get_extent_tree_root(&i)==root&&i.extent_count==5U&&i.blocks==5U);
    int used=0;assert(openfs_bitmap_test(&v,sb.block_bitmap_start,sb.block_bitmap_blocks,root,&used)==OPENFS_BITMAP_OK&&used);
    assert(openfs_file_map_block_device(&v,&sb,&i,4U,&blocks[0])==OPENFS_FILE_OK);
    assert(blocks[0]==blocks[4]);
    uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    free(d.bytes);
}

static void new_extent_tree_root_partial_write_rolls_back(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    uint64_t ino=0U;assert(openfs_path_create(&v,&sb,"/tree-new-root-partial",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);
    uint64_t ic=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t i;assert(openfs_inode_read(&v,sb.inode_table_start,ino,ic,&i)==OPENFS_INODE_OK);
    uint8_t data[4096U];memset(data,0xA1U,sizeof(data));
    for(uint32_t n=0U;n<OPENFS_INODE_TREE_INLINE_EXTENT_MAX;n++){
        assert(openfs_file_write(&v,&sb,&i,(uint64_t)n*4096U,data,sizeof(data))==OPENFS_FILE_OK);
        assert(openfs_inode_read(&v,sb.inode_table_start,ino,ic,&i)==OPENFS_INODE_OK);
    }
    uint64_t expected_root=0U;assert(openfs_alloc_block(&v,&sb,&expected_root)==OPENFS_ALLOC_OK);
    assert(openfs_free_block(&v,&sb,expected_root)==OPENFS_ALLOC_OK);
    d.fail_block=expected_root;d.fail_block_enabled=1;d.fail_after_write=1;d.fail_once=1;
    assert(openfs_file_write(&v,&sb,&i,(uint64_t)OPENFS_INODE_TREE_INLINE_EXTENT_MAX*4096U,data,sizeof(data))==OPENFS_FILE_IO_ERROR);
    d.fail_block_enabled=0;
    openfs_inode_t persisted;assert(openfs_inode_read(&v,sb.inode_table_start,ino,ic,&persisted)==OPENFS_INODE_OK);
    assert(persisted.blocks==OPENFS_INODE_TREE_INLINE_EXTENT_MAX);
    assert((persisted.flags&OPENFS_INODE_FLAG_EXTENT_TREE)==0U);
    int used=0;assert(openfs_bitmap_test(&v,sb.block_bitmap_start,sb.block_bitmap_blocks,expected_root,&used)==OPENFS_BITMAP_OK&&!used);
    uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    free(d.bytes);
}
static void existing_extent_tree_write_failure_restores_root(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    uint64_t ino=0U;assert(openfs_path_create(&v,&sb,"/tree-existing-root",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);
    uint64_t ic=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t i;assert(openfs_inode_read(&v,sb.inode_table_start,ino,ic,&i)==OPENFS_INODE_OK);
    uint64_t physical[8];
    for(unsigned n=0U;n<8U;n++)assert(openfs_alloc_block(&v,&sb,&physical[n])==OPENFS_ALLOC_OK);
    for(unsigned n=1U;n<8U;n+=2U)assert(openfs_free_block(&v,&sb,physical[n])==OPENFS_ALLOC_OK);
    memset(i.reserved,0,sizeof(i.reserved));i.flags=0U;i.extent_count=0U;i.blocks=4U;i.size=4U*4096U;
    for(uint32_t n=0U;n<4U;n++){openfs_extent_t e={n,physical[n*2U],1U};assert(openfs_inode_set_extent(&i,n,&e)==OPENFS_EXTENT_OK);}
    assert(openfs_inode_write(&v,sb.inode_table_start,ic,&i)==OPENFS_INODE_OK);
    uint8_t data[4096U];memset(data,0x71U,sizeof(data));
    assert(openfs_file_write(&v,&sb,&i,4U*4096U,data,sizeof(data))==OPENFS_FILE_OK);
    assert((i.flags&OPENFS_INODE_FLAG_EXTENT_TREE)!=0U);
    uint64_t root=openfs_inode_get_extent_tree_root(&i);assert(root!=0U);
    uint8_t *before=malloc(d.block_size);assert(before);
    memcpy(before,d.bytes+(size_t)(root*d.block_size),d.block_size);
    uint64_t inode_block=sb.inode_table_start+((ino-1U)*(uint64_t)OPENFS_INODE_SIZE)/sb.block_size;uint8_t *inode_before=malloc(sb.block_size);size_t bitmap_bytes=(size_t)sb.block_bitmap_blocks*sb.block_size;uint8_t *bitmap_before=malloc(bitmap_bytes);assert(inode_before&&bitmap_before);memcpy(inode_before,d.bytes+(size_t)(inode_block*d.block_size),sb.block_size);memcpy(bitmap_before,d.bytes+(size_t)(sb.block_bitmap_start*d.block_size),bitmap_bytes);
    d.fail_block=root;d.fail_block_enabled=1;d.fail_once=1;d.fail_after_write=1;d.partial_write_bytes=2048U;
    assert(openfs_file_write(&v,&sb,&i,5U*4096U,data,sizeof(data))==OPENFS_FILE_IO_ERROR);
    d.fail_block_enabled=0;
    openfs_inode_t persisted;assert(openfs_inode_read(&v,sb.inode_table_start,ino,ic,&persisted)==OPENFS_INODE_OK);
    assert(persisted.blocks==i.blocks&&persisted.extent_count==i.extent_count);
    assert(memcmp(before,d.bytes+(size_t)(root*d.block_size),d.block_size)==0);assert(memcmp(inode_before,d.bytes+(size_t)(inode_block*d.block_size),sb.block_size)==0);assert(memcmp(bitmap_before,d.bytes+(size_t)(sb.block_bitmap_start*d.block_size),bitmap_bytes)==0);
    openfs_mount_t m;assert(openfs_mount(&m,&v)==OPENFS_MOUNT_OK);openfs_inode_t remounted;assert(openfs_inode_read(&v,sb.inode_table_start,ino,ic,&remounted)==OPENFS_INODE_OK);uint8_t out[4096U]={0};size_t got=0U;assert(openfs_file_read(&v,&sb,&remounted,4U*4096U,out,sizeof(out),&got)==OPENFS_FILE_OK&&got==sizeof(out)&&out[0]==0x71U);assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);free(inode_before);free(bitmap_before);
    uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    free(before);free(d.bytes);
}

static void extent_tree_partial_write_rollback_failure_is_corruption(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    uint64_t ino=0U;assert(openfs_path_create(&v,&sb,"/tree-rollback-failure",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);
    uint64_t ic=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;openfs_inode_t i;assert(openfs_inode_read(&v,sb.inode_table_start,ino,ic,&i)==OPENFS_INODE_OK);
    uint8_t data[4096U];memset(data,0x66U,sizeof(data));uint64_t spacers[OPENFS_INODE_TREE_INLINE_EXTENT_MAX]={0U};uint64_t physical[OPENFS_INODE_TREE_INLINE_EXTENT_MAX]={0U};
    for(uint32_t n=0U;n<OPENFS_INODE_TREE_INLINE_EXTENT_MAX;n++){assert(openfs_file_write(&v,&sb,&i,(uint64_t)n*4096U,data,sizeof(data))==OPENFS_FILE_OK);assert(openfs_inode_read(&v,sb.inode_table_start,ino,ic,&i)==OPENFS_INODE_OK);assert(openfs_file_map_block_device(&v,&sb,&i,n,&physical[n])==OPENFS_FILE_OK);if(n>0U)assert(physical[n]!=physical[n-1U]+1U);{assert(openfs_alloc_block(&v,&sb,&spacers[n])==OPENFS_ALLOC_OK);assert(spacers[n]!=physical[n]);}}
    uint64_t root=openfs_inode_get_extent_tree_root(&i);assert(root==0U);
    openfs_file_result_t tree_grow_result=openfs_file_write(&v,&sb,&i,(uint64_t)OPENFS_INODE_TREE_INLINE_EXTENT_MAX*4096U,data,sizeof(data));assert(tree_grow_result==OPENFS_FILE_OK);openfs_inode_t tree_persisted;assert(openfs_inode_read(&v,sb.inode_table_start,ino,ic,&tree_persisted)==OPENFS_INODE_OK);root=openfs_inode_get_extent_tree_root(&i);uint64_t persisted_root=openfs_inode_get_extent_tree_root(&tree_persisted);assert(root!=0U&&persisted_root==root);
    d.fail_block=root;d.fail_block_enabled=1;d.fail_after_write=1;d.partial_write_bytes=1024U;
    assert(openfs_file_write(&v,&sb,&i,(uint64_t)(OPENFS_INODE_TREE_INLINE_EXTENT_MAX+1U)*4096U,data,sizeof(data))==OPENFS_FILE_CORRUPT);
    d.fail_block_enabled=0;
    uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);for(uint32_t n=0U;n<OPENFS_INODE_TREE_INLINE_EXTENT_MAX;n++)assert(openfs_free_block(&v,&sb,spacers[n])==OPENFS_ALLOC_OK);free(d.bytes);
}
static void write_extent_tree_root_rollback_releases_metadata(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    uint64_t inode_number=0U,filler_number=0U;assert(openfs_path_create(&v,&sb,"/tree-rollback",OPENFS_INODE_MODE_REGULAR,&inode_number)==OPENFS_PATH_OK);assert(openfs_path_create(&v,&sb,"/tree-filler",OPENFS_INODE_MODE_REGULAR,&filler_number)==OPENFS_PATH_OK);
    uint64_t inode_count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;openfs_inode_t i,filler;assert(openfs_inode_read(&v,sb.inode_table_start,inode_number,inode_count,&i)==OPENFS_INODE_OK);assert(openfs_inode_read(&v,sb.inode_table_start,filler_number,inode_count,&filler)==OPENFS_INODE_OK);
    uint8_t a[4096U],b[4096U];memset(a,0x6CU,sizeof(a));memset(b,0x19U,sizeof(b));
    for(uint64_t n=0U;n<OPENFS_EXTENT_MAX;n++){assert(openfs_file_write(&v,&sb,&i,n*4096U,a,sizeof(a))==OPENFS_FILE_OK);assert(openfs_inode_read(&v,sb.inode_table_start,inode_number,inode_count,&i)==OPENFS_INODE_OK);assert(openfs_file_write(&v,&sb,&filler,n*4096U,b,sizeof(b))==OPENFS_FILE_OK);assert(openfs_inode_read(&v,sb.inode_table_start,filler_number,inode_count,&filler)==OPENFS_INODE_OK);}
    assert(i.extent_count==OPENFS_EXTENT_MAX);uint64_t baseline_errors=0U;assert(openfs_fsck(&v,&sb,&baseline_errors)==OPENFS_FSCK_OK&&baseline_errors==0U);
    openfs_inode_t before=i;uint64_t inode_block=sb.inode_table_start+(((i.inode_number-1U)*(uint64_t)OPENFS_INODE_SIZE)/sb.block_size);size_t bitmap_bytes=(size_t)sb.block_bitmap_blocks*sb.block_size;uint8_t *bitmap_snapshot=malloc(bitmap_bytes);uint8_t *inode_snapshot=malloc(sb.block_size);assert(bitmap_snapshot&&inode_snapshot);memcpy(bitmap_snapshot,d.bytes+(size_t)(sb.block_bitmap_start*d.block_size),bitmap_bytes);memcpy(inode_snapshot,d.bytes+(size_t)(inode_block*d.block_size),sb.block_size);
    d.fail_block=inode_block;d.fail_block_enabled=1;d.fail_once=1;assert(openfs_file_write(&v,&sb,&i,(uint64_t)OPENFS_EXTENT_MAX*4096U,a,sizeof(a))==OPENFS_FILE_IO_ERROR);assert(memcmp(&i,&before,sizeof(i))==0);assert(memcmp(bitmap_snapshot,d.bytes+(size_t)(sb.block_bitmap_start*d.block_size),bitmap_bytes)==0);assert(memcmp(inode_snapshot,d.bytes+(size_t)(inode_block*d.block_size),sb.block_size)==0);
    uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);free(bitmap_snapshot);free(inode_snapshot);free(d.bytes);
}
static void truncate_shrink_free_failure_rolls_back_persisted_state(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    uint64_t inode_number=0U;assert(openfs_path_create(&v,&sb,"/shrink-free-failure",OPENFS_INODE_MODE_REGULAR,&inode_number)==OPENFS_PATH_OK);uint64_t inode_count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;openfs_inode_t i;assert(openfs_inode_read(&v,sb.inode_table_start,inode_number,inode_count,&i)==OPENFS_INODE_OK);
    uint8_t data[12288U];memset(data,0xA7U,sizeof(data));assert(openfs_file_write(&v,&sb,&i,0U,data,sizeof(data))==OPENFS_FILE_OK);assert(openfs_inode_read(&v,sb.inode_table_start,inode_number,inode_count,&i)==OPENFS_INODE_OK);
    openfs_inode_t before=i;uint64_t removed=0U;assert(openfs_file_map_block_device(&v,&sb,&i,2U,&removed)==OPENFS_FILE_OK);uint64_t bitmap_block=sb.block_bitmap_start+(removed/8U)/sb.block_size;
    d.fail_block=bitmap_block;d.fail_block_enabled=1;d.fail_after_writes=1U;d.fail_once=1;assert(openfs_file_truncate(&v,&sb,&i,4096U)==OPENFS_FILE_IO_ERROR);
    openfs_inode_t persisted;assert(openfs_inode_read(&v,sb.inode_table_start,inode_number,inode_count,&persisted)==OPENFS_INODE_OK);assert(memcmp(&persisted,&before,sizeof(before))==0);openfs_mount_t m;assert(openfs_mount(&m,&v)==OPENFS_MOUNT_OK);openfs_inode_t remounted;assert(openfs_inode_read(&v,sb.inode_table_start,inode_number,inode_count,&remounted)==OPENFS_INODE_OK);uint8_t out[12288U]={0};size_t got=0U;assert(openfs_file_read(&v,&sb,&remounted,0U,out,sizeof(out),&got)==OPENFS_FILE_OK&&got==sizeof(out)&&out[0]==0xA7U&&out[8192U]==0xA7U);assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);uint64_t errors=0U;assert(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);free(d.bytes);
}
static void partial_existing_write_rolls_back(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    openfs_inode_t i=new_file();uint8_t initial[8192U],replacement[8192U];memset(initial,0x11U,sizeof(initial));memset(replacement,0xE2U,sizeof(replacement));
    assert(openfs_file_write(&v,&sb,&i,0U,initial,sizeof(initial))==OPENFS_FILE_OK);
    openfs_inode_t before=i;uint64_t first=0U,second=0U;assert(openfs_file_map_block_device(&v,&sb,&i,0U,&first)==OPENFS_FILE_OK);assert(openfs_file_map_block_device(&v,&sb,&i,1U,&second)==OPENFS_FILE_OK);
    uint8_t *saved1=malloc(4096U),*saved2=malloc(4096U);assert(saved1&&saved2);memcpy(saved1,d.bytes+(size_t)(first*d.block_size),4096U);memcpy(saved2,d.bytes+(size_t)(second*d.block_size),4096U);
    d.fail_block=second;d.fail_block_enabled=1;d.fail_once=1;d.fail_after_write=1;
    assert(openfs_file_write(&v,&sb,&i,0U,replacement,sizeof(replacement))==OPENFS_FILE_IO_ERROR);
    assert(memcmp(&i,&before,sizeof(i))==0);
    assert(memcmp(saved1,d.bytes+(size_t)(first*d.block_size),4096U)==0);
    assert(memcmp(saved2,d.bytes+(size_t)(second*d.block_size),4096U)==0);
    free(saved1);free(saved2);free(d.bytes);
}
static void partial_write_rollback_failure_is_corruption(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    openfs_inode_t i=new_file();uint8_t initial[8192U],replacement[8192U];memset(initial,0x21U,sizeof(initial));memset(replacement,0xD3U,sizeof(replacement));
    assert(openfs_file_write(&v,&sb,&i,0U,initial,sizeof(initial))==OPENFS_FILE_OK);
    uint64_t second=0U;assert(openfs_file_map_block_device(&v,&sb,&i,1U,&second)==OPENFS_FILE_OK);
    d.fail_block=second;d.fail_block_enabled=1;
    assert(openfs_file_write(&v,&sb,&i,0U,replacement,sizeof(replacement))==OPENFS_FILE_CORRUPT);
    d.fail_block_enabled=0;
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
static void overlapping_physical_extents_are_rejected(void){disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);openfs_inode_t i=new_file();openfs_extent_t a={0,sb.data_start,2U},b={2U,sb.data_start+1U,1U};assert(openfs_inode_set_extent(&i,0U,&a)==OPENFS_EXTENT_OK);assert(openfs_inode_set_extent(&i,1U,&b)==OPENFS_EXTENT_CORRUPT);i.extent_count=2U;i.blocks=3U;uint64_t p=0U;assert(openfs_file_map_block(&i,2U,&p)==OPENFS_FILE_CORRUPT);free(d.bytes);}
static void tree_physical_overlap_with_nonlast_inline_is_rejected(void){disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);openfs_inode_t i=new_file();i.generation=1U;memset(i.reserved,0,sizeof(i.reserved));uint64_t root=0U;assert(openfs_alloc_block(&v,&sb,&root)==OPENFS_ALLOC_OK);assert(openfs_inode_set_extent_tree_root(&i,root)==OPENFS_EXTENT_OK);i.extent_count=0U;i.blocks=OPENFS_INODE_TREE_INLINE_EXTENT_MAX;for(uint32_t n=0U;n<OPENFS_INODE_TREE_INLINE_EXTENT_MAX;n++){openfs_extent_t e={n,sb.data_start+10U+n,1U};assert(openfs_inode_set_extent(&i,n,&e)==OPENFS_EXTENT_OK);}i.extent_count=OPENFS_INODE_TREE_INLINE_EXTENT_MAX+1U;openfs_extent_t tree={OPENFS_INODE_TREE_INLINE_EXTENT_MAX,sb.data_start-1U,1U};assert(openfs_extent_tree_write(&v,&sb,&i,&tree,1U)==OPENFS_EXTENT_CORRUPT);tree.physical_start=sb.data_start+sb.data_blocks;assert(openfs_extent_tree_write(&v,&sb,&i,&tree,1U)==OPENFS_EXTENT_CORRUPT);tree.physical_start=sb.journal_start;assert(openfs_extent_tree_write(&v,&sb,&i,&tree,1U)==OPENFS_EXTENT_CORRUPT);tree.physical_start=sb.data_start+10U;assert(openfs_extent_tree_write(&v,&sb,&i,&tree,1U)==OPENFS_EXTENT_CORRUPT);assert(openfs_free_block(&v,&sb,root)==OPENFS_ALLOC_OK);free(d.bytes);}
static void nonmonotonic_nonoverlapping_physical_extents_are_valid(void){disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);openfs_inode_t i=new_file();openfs_extent_t a={0U,sb.data_start+20U,1U},b={1U,sb.data_start+22U,1U},c={2U,sb.data_start+21U,1U};assert(openfs_inode_set_extent(&i,0U,&a)==OPENFS_EXTENT_OK);assert(openfs_inode_set_extent(&i,1U,&b)==OPENFS_EXTENT_OK);assert(openfs_inode_set_extent(&i,2U,&c)==OPENFS_EXTENT_OK);i.blocks=3U;uint64_t p=0U;assert(openfs_file_map_block(&i,0U,&p)==OPENFS_FILE_OK&&p==a.physical_start);assert(openfs_file_map_block(&i,1U,&p)==OPENFS_FILE_OK&&p==b.physical_start);assert(openfs_file_map_block(&i,2U,&p)==OPENFS_FILE_OK&&p==c.physical_start);free(d.bytes);}
static void extent_overlap_boundaries_and_fragmented_tree_mapping(void){disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);openfs_inode_t i=new_file();openfs_extent_t a={0U,sb.data_start+20U,2U};assert(openfs_inode_set_extent(&i,0U,&a)==OPENFS_EXTENT_OK);openfs_extent_t adjacent={2U,sb.data_start+22U,1U};assert(openfs_inode_set_extent(&i,1U,&adjacent)==OPENFS_EXTENT_OK);openfs_extent_t same={3U,sb.data_start+22U,1U};assert(openfs_inode_set_extent(&i,2U,&same)==OPENFS_EXTENT_CORRUPT);openfs_extent_t contained={3U,sb.data_start+20U,2U};assert(openfs_inode_set_extent(&i,2U,&contained)==OPENFS_EXTENT_CORRUPT);openfs_extent_t contains={3U,sb.data_start+19U,5U};assert(openfs_inode_set_extent(&i,2U,&contains)==OPENFS_EXTENT_CORRUPT);openfs_extent_t end_overlap={3U,sb.data_start+22U,2U};assert(openfs_inode_set_extent(&i,2U,&end_overlap)==OPENFS_EXTENT_CORRUPT);openfs_extent_t one_end={3U,sb.data_start+23U,1U};assert(openfs_inode_set_extent(&i,2U,&one_end)==OPENFS_EXTENT_OK);openfs_extent_t bad_phys={4U,UINT64_MAX,1U};assert(openfs_inode_set_extent(&i,3U,&bad_phys)==OPENFS_EXTENT_CORRUPT);openfs_extent_t bad_log={UINT64_MAX,sb.data_start+24U,1U};assert(openfs_inode_set_extent(&i,3U,&bad_log)==OPENFS_EXTENT_CORRUPT);i.extent_count=3U;i.blocks=4U;uint64_t p=0U;assert(openfs_file_map_block(&i,0U,&p)==OPENFS_FILE_OK&&p==a.physical_start);assert(openfs_file_map_block(&i,2U,&p)==OPENFS_FILE_OK&&p==adjacent.physical_start);assert(openfs_file_map_block(&i,3U,&p)==OPENFS_FILE_OK&&p==one_end.physical_start);memset(i.reserved,0,sizeof(i.reserved));i.extent_count=0U;i.flags=OPENFS_INODE_FLAG_HAS_EXTENTS;uint64_t root=0U;assert(openfs_alloc_block(&v,&sb,&root)==OPENFS_ALLOC_OK);assert(openfs_inode_set_extent_tree_root(&i,root)==OPENFS_EXTENT_OK);openfs_extent_t in[4]={{0U,sb.data_start+2U,1U},{1U,sb.data_start+6U,1U},{2U,sb.data_start+10U,1U},{3U,sb.data_start+4U,1U}};for(uint32_t n=0U;n<4U;n++)assert(openfs_inode_set_extent(&i,n,&in[n])==OPENFS_EXTENT_OK);i.extent_count=6U;i.blocks=6U;openfs_extent_t tree[2]={{4U,sb.data_start+12U,1U},{5U,sb.data_start+8U,1U}};assert(openfs_extent_tree_write(&v,&sb,&i,tree,2U)==OPENFS_EXTENT_OK);uint64_t expected[6]={sb.data_start+2U,sb.data_start+6U,sb.data_start+10U,sb.data_start+4U,sb.data_start+12U,sb.data_start+8U};for(uint64_t n=0U;n<6U;n++){assert(openfs_file_map_block_device(&v,&sb,&i,n,&p)==OPENFS_FILE_OK&&p==expected[n]);}assert(openfs_free_block(&v,&sb,root)==OPENFS_ALLOC_OK);free(d.bytes);}

static void extent_tree_root_outside_data_area_is_rejected(void){
    disk_t d={0};openfs_block_device_t v;openfs_superblock_t sb;setup(&d,&v,&sb);
    openfs_inode_t i=new_file();i.generation=1U;i.flags=OPENFS_INODE_FLAG_EXTENT_TREE|OPENFS_INODE_FLAG_HAS_EXTENTS;
    uint64_t root=sb.journal_start;
    assert(root<sb.data_start);
    assert(openfs_inode_set_extent_tree_root(&i,root)==OPENFS_EXTENT_OK);
    i.extent_count=0U;
    i.blocks=OPENFS_INODE_TREE_INLINE_EXTENT_MAX+1U;
    i.size=i.blocks*(uint64_t)sb.block_size;
    for(uint32_t n=0U;n<OPENFS_INODE_TREE_INLINE_EXTENT_MAX;n++){
        openfs_extent_t e={n,sb.data_start+10U+n,1U};
        assert(openfs_inode_set_extent(&i,n,&e)==OPENFS_EXTENT_OK);
    }
    i.extent_count=OPENFS_INODE_TREE_INLINE_EXTENT_MAX+1U;
    openfs_extent_t tree={OPENFS_INODE_TREE_INLINE_EXTENT_MAX,sb.journal_start,1U};
    assert(openfs_extent_tree_write(&v,&sb,&i,&tree,1U)==OPENFS_EXTENT_CORRUPT);
    tree.physical_start=sb.data_start+20U;
    uint8_t *raw=calloc(1U,sb.block_size);assert(raw);
    memcpy(raw,OPENFS_EXTENT_TREE_MAGIC,8U);
    raw[10]=1U; raw[12]=(uint8_t)openfs_extent_tree_capacity(sb.block_size);
    raw[13]=(uint8_t)(openfs_extent_tree_capacity(sb.block_size)>>8U);
    for(unsigned k=0U;k<8U;k++)raw[16U+k]=(uint8_t)(i.generation>>(8U*k));
    for(unsigned k=0U;k<8U;k++)raw[32U+k]=(uint8_t)(tree.logical_start>>(8U*k));
    for(unsigned k=0U;k<8U;k++)raw[40U+k]=(uint8_t)(tree.physical_start>>(8U*k));
    for(unsigned k=0U;k<8U;k++)raw[48U+k]=(uint8_t)(tree.block_count>>(8U*k));
    assert(v.write(v.context,root,1U,raw)==OPENFS_IO_OK);
    raw[24U]=raw[25U]=raw[26U]=raw[27U]=0U;
    uint32_t crc=openfs_crc32c(raw,sb.block_size);
    raw[24U]=(uint8_t)crc;raw[25U]=(uint8_t)(crc>>8U);raw[26U]=(uint8_t)(crc>>16U);raw[27U]=(uint8_t)(crc>>24U);
    assert(v.write(v.context,root,1U,raw)==OPENFS_IO_OK);
    free(raw);
    openfs_extent_t out={0};
    assert(openfs_extent_tree_read(&v,&sb,&i,0U,&out)==OPENFS_EXTENT_CORRUPT);
    assert(openfs_extent_tree_write(&v,&sb,&i,&tree,1U)==OPENFS_EXTENT_CORRUPT);
    uint8_t value=0xA5U;
    assert(openfs_file_read(&v,&sb,&i,0U,&value,1U,NULL)==OPENFS_FILE_INVALID_ARGUMENT);
    size_t got=0U;
    assert(openfs_file_read(&v,&sb,&i,0U,&value,1U,&got)==OPENFS_FILE_CORRUPT);
    assert(openfs_file_write(&v,&sb,&i,0U,&value,1U)==OPENFS_FILE_CORRUPT);
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
int main(void){
    file_read_rejects_unallocated_extent();
    new_extent_tree_root_partial_write_rolls_back();extent_tree_partial_write_rollback_failure_is_corruption();
    sparse_write_zeroes_intermediate_blocks();
    partial_write_rollback_failure_is_corruption();
shrink_preserves_live_extent_tree_root();existing_extent_tree_write_failure_restores_root();write_allocation_failure_rolls_back_partial_allocations();truncate_tree_shrink_releases_root();truncate_tree_shrink_free_failure_restores_root();write_flush_failure_rolls_back_media();truncate_grow_flush_failure_rolls_back();truncate_shrink_flush_failure_rolls_back();basic_rw();write_extent_tree_root_rollback_releases_metadata();truncate_grow_partial_tail_inode_failure_restores_data();truncate_tree_inode_write_failure_is_persistent_atomic();truncate_shrink_free_failure_rolls_back_persisted_state();partial_existing_write_rolls_back();credential_io();multi_block_and_truncate();truncate_zero_failure_rolls_back();truncate_shrink_inode_write_failure_keeps_blocks();extent_tree_large_file();overlapping_physical_extents_are_rejected();tree_physical_overlap_with_nonlast_inline_is_rejected();nonmonotonic_nonoverlapping_physical_extents_are_valid();extent_overlap_boundaries_and_fragmented_tree_mapping();extent_tree_root_outside_data_area_is_rejected();map_bounds();return 0;}
