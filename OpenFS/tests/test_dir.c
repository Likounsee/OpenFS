#include <assert.h>
#include <stdio.h>
#define TEST_ASSERT(expr) do { if(!(expr)) { fprintf(stderr, "test assertion failed: %s\n", #expr); abort(); } } while(0)
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/dir.h"
#include "openfs/inode_alloc.h"
#include "openfs/path.h"
#include "openfs/fsck.h"
#include "openfs/crc32c.h"

typedef struct {uint8_t *bytes;uint32_t block_size;uint64_t block_count;int fail_flush;int fail_flush_once;uint64_t fail_block;int fail_block_enabled;int fail_after_write;int fail_once;} disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*b){disk_t*d=c;if(n==0U||f>=d->block_count||(uint64_t)n>d->block_count-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(b,d->bytes+(size_t)(f*d->block_size),(size_t)((uint64_t)n*d->block_size));return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*b){disk_t*d=c;if(n==0U||f>=d->block_count||(uint64_t)n>d->block_count-f)return OPENFS_IO_OUT_OF_RANGE;if(d->fail_block_enabled&&f==d->fail_block){if(d->fail_once)d->fail_block_enabled=0;if(d->fail_after_write){memcpy(d->bytes+(size_t)(f*d->block_size),b,(size_t)((uint64_t)n*d->block_size));d->fail_after_write=0;return OPENFS_IO_IO_ERROR;}return OPENFS_IO_IO_ERROR;}memcpy(d->bytes+(size_t)(f*d->block_size),b,(size_t)((uint64_t)n*d->block_size));return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){disk_t*d=c;if(d->fail_flush){if(d->fail_flush_once)d->fail_flush=0;return OPENFS_IO_IO_ERROR;}return OPENFS_IO_OK;}
static void unlink_partial_write_rolls_back_directory(void)
{
    disk_t d={.block_size=4096U,.block_count=128U};
    d.bytes=calloc((size_t)d.block_count,d.block_size);TEST_ASSERT(d.bytes);
    openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};
    uint8_t uuid[16]={1U};TEST_ASSERT(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_superblock_t sb;TEST_ASSERT(openfs_read_superblock(&v,&sb)==OPENFS_FORMAT_OK);
    uint64_t ino=0U;TEST_ASSERT(openfs_path_create(&v,&sb,"/partial-dir",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);
    uint64_t count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t root;TEST_ASSERT(openfs_inode_read(&v,sb.inode_table_start,sb.root_inode,count,&root)==OPENFS_INODE_OK);
    uint64_t physical=0U;TEST_ASSERT(openfs_file_map_block_device(&v,&sb,&root,0U,&physical)==OPENFS_FILE_OK);
    uint8_t *saved=malloc(d.block_size);TEST_ASSERT(saved);memcpy(saved,d.bytes+(size_t)(physical*d.block_size),d.block_size);
    d.fail_block=physical;d.fail_block_enabled=1;d.fail_after_write=1;d.fail_once=1;
    TEST_ASSERT(openfs_dir_remove(&v,&sb,&root,"partial-dir")==OPENFS_DIR_IO_ERROR);
    TEST_ASSERT(memcmp(saved,d.bytes+(size_t)(physical*d.block_size),d.block_size)==0);
    openfs_dir_entry_t found={0};TEST_ASSERT(openfs_dir_lookup(&v,&sb,&root,"partial-dir",&found)==OPENFS_DIR_OK&&found.inode_number==ino);
    uint64_t errors=0U;TEST_ASSERT(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    free(saved);free(d.bytes);
}
static void unlink_inode_partial_write_rolls_back_both(void)
{
    disk_t d={.block_size=4096U,.block_count=128U};
    d.bytes=calloc((size_t)d.block_count,d.block_size);TEST_ASSERT(d.bytes);
    openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};
    uint8_t uuid[16]={2U};TEST_ASSERT(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_superblock_t sb;TEST_ASSERT(openfs_read_superblock(&v,&sb)==OPENFS_FORMAT_OK);
    uint64_t ino=0U;TEST_ASSERT(openfs_path_create(&v,&sb,"/partial-inode",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);
    uint64_t count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t root;TEST_ASSERT(openfs_inode_read(&v,sb.inode_table_start,sb.root_inode,count,&root)==OPENFS_INODE_OK);
    uint64_t dir_physical=0U;TEST_ASSERT(openfs_file_map_block_device(&v,&sb,&root,0U,&dir_physical)==OPENFS_FILE_OK);
    uint64_t inode_offset=(sb.root_inode-1U)*(uint64_t)OPENFS_INODE_SIZE;
    uint64_t inode_block=sb.inode_table_start+inode_offset/sb.block_size;
    uint8_t *saved_dir=malloc(d.block_size),*saved_inode=malloc(d.block_size);TEST_ASSERT(saved_dir&&saved_inode);
    memcpy(saved_dir,d.bytes+(size_t)(dir_physical*d.block_size),d.block_size);
    memcpy(saved_inode,d.bytes+(size_t)(inode_block*d.block_size),d.block_size);
    d.fail_block=inode_block;d.fail_block_enabled=1;d.fail_after_write=1;d.fail_once=1;
    TEST_ASSERT(openfs_dir_remove(&v,&sb,&root,"partial-inode")==OPENFS_DIR_IO_ERROR);
    TEST_ASSERT(memcmp(saved_dir,d.bytes+(size_t)(dir_physical*d.block_size),d.block_size)==0);
    TEST_ASSERT(memcmp(saved_inode,d.bytes+(size_t)(inode_block*d.block_size),d.block_size)==0);
    openfs_dir_entry_t found={0};TEST_ASSERT(openfs_dir_lookup(&v,&sb,&root,"partial-inode",&found)==OPENFS_DIR_OK&&found.inode_number==ino);
    uint64_t errors=0U;TEST_ASSERT(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK&&errors==0U);
    free(saved_dir);free(saved_inode);free(d.bytes);
}
static void unlink_flush_failure_rolls_back(void)
{
    disk_t d={.block_size=4096U,.block_count=128U};
    d.bytes=calloc((size_t)d.block_count,d.block_size);
    TEST_ASSERT(d.bytes);
    openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};
    uint8_t uuid[16]={0};
    TEST_ASSERT(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_superblock_t sb;
    TEST_ASSERT(openfs_read_superblock(&v,&sb)==OPENFS_FORMAT_OK);
    uint64_t ino=0U;
    TEST_ASSERT(openfs_path_create(&v,&sb,"/flush-unlink",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);
    uint64_t count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t root;
    TEST_ASSERT(openfs_inode_read(&v,sb.inode_table_start,sb.root_inode,count,&root)==OPENFS_INODE_OK);

    d.fail_flush=1;
    d.fail_flush_once=1;
    TEST_ASSERT(openfs_dir_remove(&v,&sb,&root,"flush-unlink")==OPENFS_DIR_IO_ERROR);

    openfs_dir_entry_t found={0};
    TEST_ASSERT(openfs_dir_lookup(&v,&sb,&root,"flush-unlink",&found)==OPENFS_DIR_OK);
    TEST_ASSERT(found.inode_number==ino);
    uint64_t errors=0U;
    TEST_ASSERT(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_OK);
    TEST_ASSERT(errors==0U);
    free(d.bytes);
}

int main(void){
    unlink_partial_write_rolls_back_directory();
    unlink_inode_partial_write_rolls_back_both();
    unlink_flush_failure_rolls_back();
 disk_t d={.block_size=4096U,.block_count=128U};d.bytes=calloc((size_t)d.block_count,d.block_size);TEST_ASSERT(d.bytes);
 openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};uint8_t uuid[16]={0};TEST_ASSERT(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
 openfs_superblock_t sb;TEST_ASSERT(openfs_read_superblock(&v,&sb)==OPENFS_FORMAT_OK);
 uint64_t n=0U;TEST_ASSERT(openfs_inode_alloc(&v,&sb,1U,OPENFS_INODE_MODE_REGULAR,&n)==OPENFS_INODE_ALLOC_OK);TEST_ASSERT(n==2U);
 openfs_inode_t root;uint64_t count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
 TEST_ASSERT(openfs_inode_read(&v,sb.inode_table_start,1U,count,&root)==OPENFS_INODE_OK);
 openfs_dir_entry_t entry={2U,1U,1U};TEST_ASSERT(openfs_dir_add(&v,&sb,&root,"hello",&entry)==OPENFS_DIR_OK);
 openfs_dir_entry_t found={0};TEST_ASSERT(openfs_dir_lookup(&v,&sb,&root,"hello",&found)==OPENFS_DIR_OK);TEST_ASSERT(found.inode_number==2U);openfs_inode_t aliased_free;TEST_ASSERT(openfs_inode_read(&v,sb.inode_table_start,2U,count,&aliased_free)==OPENFS_INODE_OK);aliased_free.mode=OPENFS_INODE_MODE_FREE;aliased_free.link_count=0U;TEST_ASSERT(openfs_inode_write(&v,sb.inode_table_start,count,&aliased_free)==OPENFS_INODE_OK);uint64_t aliased_ino=0U;TEST_ASSERT(openfs_path_lookup(&v,&sb,"/hello",&aliased_ino)==OPENFS_PATH_CORRUPT);aliased_free.mode=OPENFS_INODE_MODE_REGULAR;aliased_free.link_count=1U;TEST_ASSERT(openfs_inode_write(&v,sb.inode_table_start,count,&aliased_free)==OPENFS_INODE_OK);
 TEST_ASSERT(openfs_dir_add(&v,&sb,&root,"hello",&entry)==OPENFS_DIR_EXISTS);
 TEST_ASSERT(openfs_dir_remove(&v,&sb,&root,"hello")==OPENFS_DIR_OK);
 TEST_ASSERT(openfs_dir_lookup(&v,&sb,&root,"hello",&found)==OPENFS_DIR_NOT_FOUND);
 openfs_inode_t child;TEST_ASSERT(openfs_inode_read(&v,sb.inode_table_start,2U,count,&child)==OPENFS_INODE_OK);
 child.link_count=0U;child.mode=OPENFS_INODE_MODE_FREE;TEST_ASSERT(openfs_inode_write(&v,sb.inode_table_start,count,&child)==OPENFS_INODE_OK);
 TEST_ASSERT(openfs_inode_free(&v,&sb,2U)==OPENFS_INODE_ALLOC_OK);
 TEST_ASSERT(openfs_inode_alloc(&v,&sb,1U,OPENFS_INODE_MODE_REGULAR,&n)==OPENFS_INODE_ALLOC_OK);
 openfs_inode_t new_child;TEST_ASSERT(openfs_inode_read(&v,sb.inode_table_start,n,count,&new_child)==OPENFS_INODE_OK);
 entry.inode_number=n;entry.generation=new_child.generation;TEST_ASSERT(openfs_dir_add(&v,&sb,&root,"hello",&entry)==OPENFS_DIR_OK);
 uint64_t root_block=0U;TEST_ASSERT(openfs_file_map_block_device(&v,&sb,&root,0U,&root_block)==OPENFS_FILE_OK);
 uint8_t raw[OPENFS_DIR_ENTRY_SIZE];size_t got=0U;TEST_ASSERT(openfs_file_read(&v,&sb,&root,0U,raw,sizeof(raw),&got)==OPENFS_FILE_OK&&got==sizeof(raw));
 raw[7U]=1U;raw[24U]='.';memset(raw+25U,0U,227U);uint32_t crc=openfs_crc32c(raw,252U);raw[252U]=(uint8_t)crc;raw[253U]=(uint8_t)(crc>>8U);raw[254U]=(uint8_t)(crc>>16U);raw[255U]=(uint8_t)(crc>>24U);
 TEST_ASSERT(openfs_file_write(&v,&sb,&root,0U,raw,sizeof(raw))==OPENFS_FILE_OK);
 TEST_ASSERT(openfs_dir_lookup(&v,&sb,&root,"hello",&found)==OPENFS_DIR_CORRUPT);
 raw[0U]='X';crc=openfs_crc32c(raw,252U);raw[252U]=(uint8_t)crc;raw[253U]=(uint8_t)(crc>>8U);raw[254U]=(uint8_t)(crc>>16U);raw[255U]=(uint8_t)(crc>>24U);TEST_ASSERT(openfs_file_write(&v,&sb,&root,0U,raw,sizeof(raw))==OPENFS_FILE_OK);TEST_ASSERT(openfs_dir_lookup(&v,&sb,&root,"hello",&found)==OPENFS_DIR_CORRUPT);
 uint64_t errors=0U;TEST_ASSERT(openfs_fsck(&v,&sb,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);
 free(d.bytes);return 0;
}
