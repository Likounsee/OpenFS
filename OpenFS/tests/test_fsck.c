#include <assert.h>
#include <stdio.h>
#define TEST_ASSERT(expr) do { if(!(expr)) { fprintf(stderr, "test assertion failed: %s\n", #expr); abort(); } } while(0)
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/fsck.h"
#include "openfs/bitmap.h"
#include "openfs/file.h"
#include "openfs/extent.h"
#include "openfs/path.h"
#include "openfs/dir.h"
#include "openfs/crc32c.h"
#include "openfs/link.h"
#include "openfs/cow.h"
typedef struct{uint8_t*b;uint32_t bs;uint64_t bc;}D;
static openfs_io_result_t r(void*c,uint64_t f,uint32_t n,void*x){D*d=c;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(x,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t w(void*c,uint64_t f,uint32_t n,const void*x){D*d=c;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),x,(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t f(void*c){(void)c;return OPENFS_IO_OK;}
static void patch_parent(openfs_block_device_t *v,const openfs_superblock_t *s,uint64_t ino,uint64_t parent){
    uint64_t off=(ino-1U)*(uint64_t)OPENFS_INODE_SIZE,blk=s->inode_table_start+off/s->block_size;uint32_t within=(uint32_t)(off%s->block_size);uint8_t raw[4096U];TEST_ASSERT(v->read(v->context,blk,1U,raw)==OPENFS_IO_OK);
    for(unsigned k=0;k<8U;k++)raw[within+32U+k]=(uint8_t)(parent>>(8U*k));memset(raw+within+224U,0,4U);uint32_t crc=openfs_crc32c(raw+within,224U);
    raw[within+224U]=(uint8_t)crc;raw[within+225U]=(uint8_t)(crc>>8U);raw[within+226U]=(uint8_t)(crc>>16U);raw[within+227U]=(uint8_t)(crc>>24U);TEST_ASSERT(v->write(v->context,blk,1U,raw)==OPENFS_IO_OK);
}
static void patch_flags(openfs_block_device_t *v,const openfs_superblock_t *s,uint64_t ino,uint32_t flags){
    uint64_t off=(ino-1U)*(uint64_t)OPENFS_INODE_SIZE,blk=s->inode_table_start+off/s->block_size;uint32_t within=(uint32_t)(off%s->block_size);uint8_t raw[4096U];TEST_ASSERT(v->read(v->context,blk,1U,raw)==OPENFS_IO_OK);
    raw[within+76U]=(uint8_t)flags;raw[within+77U]=(uint8_t)(flags>>8U);raw[within+78U]=(uint8_t)(flags>>16U);raw[within+79U]=(uint8_t)(flags>>24U);memset(raw+within+224U,0,4U);uint32_t crc=openfs_crc32c(raw+within,224U);
    raw[within+224U]=(uint8_t)crc;raw[within+225U]=(uint8_t)(crc>>8U);raw[within+226U]=(uint8_t)(crc>>16U);raw[within+227U]=(uint8_t)(crc>>24U);TEST_ASSERT(v->write(v->context,blk,1U,raw)==OPENFS_IO_OK);
}
static void patch_link_count(openfs_block_device_t *v,const openfs_superblock_t *s,uint64_t ino,uint64_t links){
    uint64_t off=(ino-1U)*(uint64_t)OPENFS_INODE_SIZE,blk=s->inode_table_start+off/s->block_size;uint32_t within=(uint32_t)(off%s->block_size);uint8_t raw[4096U];TEST_ASSERT(v->read(v->context,blk,1U,raw)==OPENFS_IO_OK);
    for(unsigned k=0;k<8U;k++)raw[within+40U+k]=(uint8_t)(links>>(8U*k));memset(raw+within+224U,0,4U);uint32_t crc=openfs_crc32c(raw+within,224U);
    raw[within+224U]=(uint8_t)crc;raw[within+225U]=(uint8_t)(crc>>8U);raw[within+226U]=(uint8_t)(crc>>16U);raw[within+227U]=(uint8_t)(crc>>24U);TEST_ASSERT(v->write(v->context,blk,1U,raw)==OPENFS_IO_OK);
}
static void disconnected_directory_cycle(void){
    D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);TEST_ASSERT(d.b);
    openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={77U};
    TEST_ASSERT(openfs_format(&v,u)==OPENFS_FORMAT_OK);
    openfs_superblock_t s;TEST_ASSERT(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    uint64_t a=0U,b=0U,ic=(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE;
    TEST_ASSERT(openfs_path_mkdir(&v,&s,"/a",&a)==OPENFS_PATH_OK);
    TEST_ASSERT(openfs_path_mkdir(&v,&s,"/b",&b)==OPENFS_PATH_OK);
    openfs_inode_t root,ai,bi;
    TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,s.root_inode,ic,&root)==OPENFS_INODE_OK);
    TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,a,ic,&ai)==OPENFS_INODE_OK);
    TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,b,ic,&bi)==OPENFS_INODE_OK);
    openfs_dir_entry_t ae={a,ai.generation,2U},be={b,bi.generation,2U};
    TEST_ASSERT(openfs_dir_remove(&v,&s,&root,"a")==OPENFS_DIR_OK);
    TEST_ASSERT(openfs_dir_remove(&v,&s,&root,"b")==OPENFS_DIR_OK);
    TEST_ASSERT(openfs_dir_add(&v,&s,&ai,"b",&be)==OPENFS_DIR_OK);
    TEST_ASSERT(openfs_dir_add(&v,&s,&bi,"a",&ae)==OPENFS_DIR_OK);
    patch_parent(&v,&s,a,b);
    patch_parent(&v,&s,b,a);
    uint64_t errors=0U;
    TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);
    free(d.b);
}

static void combined_corruption_cases(void){
    D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);TEST_ASSERT(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={31U};TEST_ASSERT(openfs_format(&v,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;TEST_ASSERT(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    uint64_t ic=(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,ino_a=0U,ino_b=0U;TEST_ASSERT(openfs_path_create(&v,&s,"/a",OPENFS_INODE_MODE_REGULAR,&ino_a)==OPENFS_PATH_OK);TEST_ASSERT(openfs_path_create(&v,&s,"/b",OPENFS_INODE_MODE_REGULAR,&ino_b)==OPENFS_PATH_OK);
    openfs_inode_t a,b;TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,ino_a,ic,&a)==OPENFS_INODE_OK);TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,ino_b,ic,&b)==OPENFS_INODE_OK);uint8_t data[4096U];memset(data,0x3DU,sizeof(data));TEST_ASSERT(openfs_file_write(&v,&s,&a,0U,data,sizeof(data))==OPENFS_FILE_OK);TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,ino_a,ic,&a)==OPENFS_INODE_OK);
    openfs_extent_t ex;TEST_ASSERT(openfs_inode_get_extent(&a,0U,&ex)==OPENFS_EXTENT_OK);b.blocks=1U;b.size=4096U;b.extent_count=0U;TEST_ASSERT(openfs_inode_set_extent(&b,0U,&ex)==OPENFS_EXTENT_OK);TEST_ASSERT(openfs_inode_write(&v,s.inode_table_start,ic,&b)==OPENFS_INODE_OK);patch_link_count(&v,&s,ino_b,0U);
    TEST_ASSERT(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,ex.physical_start,0)==OPENFS_BITMAP_OK);uint64_t errors=0U;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);uint8_t bad_journal[4096U]={0};bad_journal[0]='X';TEST_ASSERT(v.write(v.context,s.journal_start,1U,bad_journal)==OPENFS_IO_OK);
    errors=0U;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT);free(d.b);
}
static void cow_refcount_exact_ownership_cases(void){
    D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);TEST_ASSERT(d.b);
    openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t uuid[16]={91U};
    TEST_ASSERT(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_superblock_t s;TEST_ASSERT(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    uint64_t ic=(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,ino=0U,clone_ino=0U;
    TEST_ASSERT(openfs_path_create(&v,&s,"/cow",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);
    openfs_inode_t in;TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,ino,ic,&in)==OPENFS_INODE_OK);
    uint8_t data[4096U]={0xA5U};TEST_ASSERT(openfs_file_write(&v,&s,&in,0U,data,sizeof(data))==OPENFS_FILE_OK);
    TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,ino,ic,&in)==OPENFS_INODE_OK);
    openfs_extent_t ex;TEST_ASSERT(openfs_inode_get_extent(&in,0U,&ex)==OPENFS_EXTENT_OK);
    uint16_t rc=0U;uint64_t errors=0U;
    TEST_ASSERT(openfs_cow_refcount_get(&v,&s,ex.physical_start,&rc)==OPENFS_COW_OK&&rc==1U);
    TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);
    TEST_ASSERT(openfs_path_clone(&v,&s,"/cow","/cow-clone",&clone_ino)==OPENFS_PATH_OK);
    TEST_ASSERT(openfs_cow_refcount_get(&v,&s,ex.physical_start,&rc)==OPENFS_COW_OK&&rc==2U);
    TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);
    TEST_ASSERT(openfs_cow_refcount_set(&v,&s,ex.physical_start,1U)==OPENFS_COW_OK);
    TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);
    TEST_ASSERT(openfs_cow_refcount_set(&v,&s,ex.physical_start,2U)==OPENFS_COW_OK);
    TEST_ASSERT(openfs_cow_refcount_set(&v,&s,ex.physical_start,0U)==OPENFS_COW_OK);
    TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);
    TEST_ASSERT(openfs_cow_refcount_set(&v,&s,ex.physical_start,1U)==OPENFS_COW_OK);
    TEST_ASSERT(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,ex.physical_start,0)==OPENFS_BITMAP_OK);
    TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);
    TEST_ASSERT(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,ex.physical_start,1)==OPENFS_BITMAP_OK);
    TEST_ASSERT(openfs_cow_refcount_set(&v,&s,ex.physical_start,0U)==OPENFS_COW_OK);
    TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);
    TEST_ASSERT(openfs_cow_refcount_set(&v,&s,ex.physical_start,OPENFS_COW_MAX_REFCOUNT)==OPENFS_COW_OK);
    TEST_ASSERT(openfs_cow_refcount_inc(&v,&s,ex.physical_start,&rc)==OPENFS_COW_OVERFLOW);
    TEST_ASSERT(openfs_cow_refcount_get(&v,&s,ex.physical_start,&rc)==OPENFS_COW_OK&&rc==OPENFS_COW_MAX_REFCOUNT);
    free(d.b);
}

int main(void){cow_refcount_exact_ownership_cases();disconnected_directory_cycle();combined_corruption_cases();D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);TEST_ASSERT(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={0};TEST_ASSERT(openfs_format(&v,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;TEST_ASSERT(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);uint64_t errors=99;uint64_t ic=(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);
{
 uint8_t backup_saved[4096U];uint64_t backup_block=s.total_blocks-1U;
 TEST_ASSERT(v.read(v.context,backup_block,1U,backup_saved)==OPENFS_IO_OK);
 d.b[(size_t)(backup_block*d.bs)+148U]^=0x5AU;
 errors=0U;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);
 TEST_ASSERT(v.write(v.context,backup_block,1U,backup_saved)==OPENFS_IO_OK);
 errors=0U;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);
}
openfs_inode_t root;TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,s.root_inode,ic,&root)==OPENFS_INODE_OK);uint64_t root_off=(s.root_inode-1U)*(uint64_t)OPENFS_INODE_SIZE,root_blk=s.inode_table_start+root_off/s.block_size;uint8_t root_saved[4096U];TEST_ASSERT(v.read(v.context,root_blk,1U,root_saved)==OPENFS_IO_OK);root.parent_inode=2U;patch_parent(&v,&s,s.root_inode,2U);errors=0;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);TEST_ASSERT(v.write(v.context,root_blk,1U,root_saved)==OPENFS_IO_OK);uint64_t ino=0U;TEST_ASSERT(openfs_path_create(&v,&s,"/f",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);openfs_inode_t parent_corrupt;TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,ino,ic,&parent_corrupt)==OPENFS_INODE_OK);parent_corrupt.parent_inode=0U;patch_parent(&v,&s,ino,0U);errors=0;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);parent_corrupt.parent_inode=s.root_inode;patch_parent(&v,&s,ino,s.root_inode);openfs_inode_t fi;openfs_inode_t root_dir;uint8_t dir_raw[OPENFS_DIR_ENTRY_SIZE],dup_raw[OPENFS_DIR_ENTRY_SIZE];size_t dir_got=0U;uint32_t dir_crc=0U;TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,ino,ic,&fi)==OPENFS_INODE_OK);TEST_ASSERT(openfs_file_write(&v,&s,&fi,0U,"x",1U)==OPENFS_FILE_OK);TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,s.root_inode,ic,&root_dir)==OPENFS_INODE_OK);TEST_ASSERT(openfs_file_read(&v,&s,&root_dir,0U,dir_raw,sizeof(dir_raw),&dir_got)==OPENFS_FILE_OK&&dir_got==sizeof(dir_raw));uint8_t saved_gen=dir_raw[16U];dir_raw[16U]=2U;dir_crc=openfs_crc32c(dir_raw,252U);dir_raw[252U]=(uint8_t)dir_crc;dir_raw[253U]=(uint8_t)(dir_crc>>8U);dir_raw[254U]=(uint8_t)(dir_crc>>16U);dir_raw[255U]=(uint8_t)(dir_crc>>24U);TEST_ASSERT(openfs_file_write(&v,&s,&root_dir,0U,dir_raw,sizeof(dir_raw))==OPENFS_FILE_OK);errors=0;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);dir_raw[16U]=saved_gen;dir_crc=openfs_crc32c(dir_raw,252U);dir_raw[252U]=(uint8_t)dir_crc;dir_raw[253U]=(uint8_t)(dir_crc>>8U);dir_raw[254U]=(uint8_t)(dir_crc>>16U);dir_raw[255U]=(uint8_t)(dir_crc>>24U);TEST_ASSERT(openfs_file_write(&v,&s,&root_dir,0U,dir_raw,sizeof(dir_raw))==OPENFS_FILE_OK);uint8_t malformed[OPENFS_DIR_ENTRY_SIZE]={0};malformed[0]='X';TEST_ASSERT(openfs_file_write(&v,&s,&root_dir,0U,malformed,sizeof(malformed))==OPENFS_FILE_OK);errors=0;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);TEST_ASSERT(openfs_file_write(&v,&s,&root_dir,0U,dir_raw,sizeof(dir_raw))==OPENFS_FILE_OK);TEST_ASSERT(openfs_link(&v,&s,"/f","/dup")==OPENFS_PATH_OK);TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,s.root_inode,ic,&root_dir)==OPENFS_INODE_OK);TEST_ASSERT(openfs_file_read(&v,&s,&root_dir,OPENFS_DIR_ENTRY_SIZE,dup_raw,sizeof(dup_raw),&dir_got)==OPENFS_FILE_OK&&dir_got==sizeof(dup_raw));memcpy(dup_raw+24U,dir_raw+24U,dir_raw[7U]);dup_raw[7U]=dir_raw[7U];dir_crc=openfs_crc32c(dup_raw,252U);dup_raw[252U]=(uint8_t)dir_crc;dup_raw[253U]=(uint8_t)(dir_crc>>8U);dup_raw[254U]=(uint8_t)(dir_crc>>16U);dup_raw[255U]=(uint8_t)(dir_crc>>24U);TEST_ASSERT(openfs_file_write(&v,&s,&root_dir,OPENFS_DIR_ENTRY_SIZE,dup_raw,sizeof(dup_raw))==OPENFS_FILE_OK);errors=0;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);dup_raw[7U]=3U;memcpy(dup_raw+24U,"dup",4U);dir_crc=openfs_crc32c(dup_raw,252U);dup_raw[252U]=(uint8_t)dir_crc;dup_raw[253U]=(uint8_t)(dir_crc>>8U);dup_raw[254U]=(uint8_t)(dir_crc>>16U);dup_raw[255U]=(uint8_t)(dir_crc>>24U);TEST_ASSERT(openfs_file_write(&v,&s,&root_dir,OPENFS_DIR_ENTRY_SIZE,dup_raw,sizeof(dup_raw))==OPENFS_FILE_OK);TEST_ASSERT(openfs_path_unlink(&v,&s,"/dup")==OPENFS_PATH_OK);openfs_extent_t ex;TEST_ASSERT(openfs_inode_get_extent(&fi,0U,&ex)==OPENFS_EXTENT_OK);TEST_ASSERT(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,ex.physical_start,0)==OPENFS_BITMAP_OK);errors=0;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);TEST_ASSERT(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,ex.physical_start,1)==OPENFS_BITMAP_OK);uint64_t leaked=ex.physical_start+1U;TEST_ASSERT(leaked<s.data_start+s.data_blocks);TEST_ASSERT(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,leaked,1)==OPENFS_BITMAP_OK);TEST_ASSERT(openfs_cow_refcount_set(&v,&s,leaked,3U)==OPENFS_COW_OK);errors=0;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);TEST_ASSERT(openfs_cow_refcount_set(&v,&s,leaked,0U)==OPENFS_COW_OK);TEST_ASSERT(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,leaked,0)==OPENFS_BITMAP_OK);uint64_t ino2=0U;TEST_ASSERT(openfs_path_create(&v,&s,"/g",OPENFS_INODE_MODE_REGULAR,&ino2)==OPENFS_PATH_OK);openfs_inode_t gi_check;TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,ino2,ic,&gi_check)==OPENFS_INODE_OK);gi_check.blocks=1U;gi_check.size=1U;gi_check.extent_count=0U;openfs_extent_t check_extent={0U,ex.physical_start,1U};TEST_ASSERT(openfs_inode_set_extent(&gi_check,0U,&check_extent)==OPENFS_EXTENT_OK);TEST_ASSERT(openfs_inode_write(&v,s.inode_table_start,ic,&gi_check)==OPENFS_INODE_OK);gi_check.blocks=0U;{uint64_t off=(ino2-1U)*(uint64_t)OPENFS_INODE_SIZE;uint64_t blk=s.inode_table_start+off/s.block_size;uint32_t within=(uint32_t)(off%s.block_size);uint8_t raw[4096U];TEST_ASSERT(v.read(v.context,blk,1U,raw)==OPENFS_IO_OK);memset(raw+within+24U,0,8U);memset(raw+within+224U,0,4U);uint32_t crc=openfs_crc32c(raw+within,224U);raw[within+224U]=(uint8_t)crc;raw[within+225U]=(uint8_t)(crc>>8U);raw[within+226U]=(uint8_t)(crc>>16U);raw[within+227U]=(uint8_t)(crc>>24U);TEST_ASSERT(v.write(v.context,blk,1U,raw)==OPENFS_IO_OK);}errors=0;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);gi_check.blocks=1U;TEST_ASSERT(openfs_inode_write(&v,s.inode_table_start,ic,&gi_check)==OPENFS_INODE_OK);openfs_inode_t rd;TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,s.root_inode,ic,&rd)==OPENFS_INODE_OK);TEST_ASSERT(openfs_dir_remove(&v,&s,&rd,"g")==OPENFS_DIR_OK);errors=0;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);openfs_inode_t gi;TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,ino2,ic,&gi)==OPENFS_INODE_OK);openfs_extent_t dup={0U,ex.physical_start,1U};TEST_ASSERT(openfs_inode_set_extent(&gi,0U,&dup)==OPENFS_EXTENT_OK);gi.size=1U;gi.blocks=1U;TEST_ASSERT(openfs_inode_write(&v,s.inode_table_start,ic,&gi)==OPENFS_INODE_OK);errors=0;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,s.root_inode,ic,&root)==OPENFS_INODE_OK);root.flags=8U;patch_flags(&v,&s,s.root_inode,8U);errors=0;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);root.flags=0U;patch_flags(&v,&s,s.root_inode,0U);uint8_t *raw=d.b+(size_t)(s.inode_table_start*d.bs);raw[216]='X';errors=0;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);uint8_t bad_journal[4096U]={0};bad_journal[17U]=0xA5U;TEST_ASSERT(v.write(v.context,s.journal_start,1U,bad_journal)==OPENFS_IO_OK);errors=0;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT);uint8_t zero_journal[4096U]={0};TEST_ASSERT(v.write(v.context,s.journal_start,1U,zero_journal)==OPENFS_IO_OK);
uint64_t block_bitmap_bits=s.block_bitmap_blocks*(uint64_t)s.block_size*8U;TEST_ASSERT(block_bitmap_bits>s.total_blocks);uint64_t stray=s.total_blocks;TEST_ASSERT(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,stray,1)==OPENFS_BITMAP_OK);errors=0;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);TEST_ASSERT(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,stray,0)==OPENFS_BITMAP_OK);uint64_t inode_bitmap_bits=s.inode_bitmap_blocks*(uint64_t)s.block_size*8U;TEST_ASSERT(inode_bitmap_bits>ic);uint64_t stray_inode=ic;TEST_ASSERT(openfs_bitmap_set(&v,s.inode_bitmap_start,s.inode_bitmap_blocks,stray_inode,1)==OPENFS_BITMAP_OK);errors=0;TEST_ASSERT(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);TEST_ASSERT(openfs_bitmap_set(&v,s.inode_bitmap_start,s.inode_bitmap_blocks,stray_inode,0)==OPENFS_BITMAP_OK);free(d.b);return 0;}