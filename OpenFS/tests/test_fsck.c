#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/fsck.h"
#include "openfs/bitmap.h"
#include "openfs/file.h"
#include "openfs/extent.h"
#include "openfs/path.h"
typedef struct{uint8_t*b;uint32_t bs;uint64_t bc;}D;
static openfs_io_result_t r(void*c,uint64_t f,uint32_t n,void*x){D*d=c;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(x,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t w(void*c,uint64_t f,uint32_t n,const void*x){D*d=c;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),x,(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t f(void*c){(void)c;return OPENFS_IO_OK;}
int main(void){D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={0};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);uint64_t errors=99;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);
openfs_inode_t root;
assert(openfs_inode_read(&v,s.inode_table_start,s.root_inode,ic,&root)==OPENFS_INODE_OK);
root.parent_inode=2U;
assert(openfs_inode_write(&v,s.inode_table_start,ic,&root)==OPENFS_INODE_OK);
errors=0;
assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);
root.parent_inode=s.root_inode;
assert(openfs_inode_write(&v,s.inode_table_start,ic,&root)==OPENFS_INODE_OK);uint64_t ino=0U;assert(openfs_path_create(&v,&s,"/f",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);uint64_t ic=(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE;openfs_inode_t fi;assert(openfs_inode_read(&v,s.inode_table_start,ino,ic,&fi)==OPENFS_INODE_OK);assert(openfs_file_write(&v,&s,&fi,0U,"x",1U)==OPENFS_FILE_OK);openfs_extent_t ex;assert(openfs_inode_get_extent(&fi,0U,&ex)==OPENFS_EXTENT_OK);assert(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,ex.physical_start,0)==OPENFS_BITMAP_OK);errors=0;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);assert(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,ex.physical_start,1)==OPENFS_BITMAP_OK);uint64_t leaked=ex.physical_start+1U;assert(leaked<s.data_start+s.data_blocks);assert(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,leaked,1)==OPENFS_BITMAP_OK);errors=0;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);assert(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,leaked,0)==OPENFS_BITMAP_OK);uint64_t ino2=0U;assert(openfs_path_create(&v,&s,"/g",OPENFS_INODE_MODE_REGULAR,&ino2)==OPENFS_PATH_OK);openfs_inode_t gi;assert(openfs_inode_read(&v,s.inode_table_start,ino2,ic,&gi)==OPENFS_INODE_OK);openfs_extent_t dup={0U,ex.physical_start,1U};assert(openfs_inode_set_extent(&gi,0U,&dup)==OPENFS_EXTENT_OK);gi.size=1U;gi.blocks=1U;assert(openfs_inode_write(&v,s.inode_table_start,ic,&gi)==OPENFS_INODE_OK);errors=0;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);uint8_t *raw=d.b+(size_t)(s.inode_table_start*d.bs);raw[216]='X';errors=0;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT&&errors>0U);free(d.b);return 0;}