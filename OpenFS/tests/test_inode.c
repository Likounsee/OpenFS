#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/bitmap.h"
#include "openfs/format.h"
#include "openfs/inode.h"
#include "openfs/extent.h"

typedef struct { uint8_t *bytes; uint32_t block_size; uint64_t block_count; } disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*b){disk_t*d=c;if(n==0U||f>=d->block_count||(uint64_t)n>d->block_count-f)return OPENFS_IO_OUT_OF_RANGE;uint64_t o=f*d->block_size,l=(uint64_t)n*d->block_size;memcpy(b,d->bytes+(size_t)o,(size_t)l);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*b){disk_t*d=c;if(n==0U||f>=d->block_count||(uint64_t)n>d->block_count-f)return OPENFS_IO_OUT_OF_RANGE;uint64_t o=f*d->block_size,l=(uint64_t)n*d->block_size;memcpy(d->bytes+(size_t)o,b,(size_t)l);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}
int main(void){
 disk_t d={.block_size=4096U,.block_count=128U};d.bytes=calloc((size_t)d.block_count,d.block_size);assert(d.bytes);
 openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};uint8_t uuid[16]={1U};
 assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
 openfs_superblock_t sb;assert(openfs_read_superblock(&v,&sb)==OPENFS_FORMAT_OK);
 int used=0;assert(openfs_bitmap_test(&v,sb.inode_bitmap_start,sb.inode_bitmap_blocks,0U,&used)==OPENFS_BITMAP_OK);assert(used==1);
 uint64_t inode_count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
 openfs_inode_t root;assert(openfs_inode_read(&v,sb.inode_table_start,1U,inode_count,&root)==OPENFS_INODE_OK);
 assert(root.inode_number==1U&&root.parent_inode==1U&&root.link_count==1U&&root.mode==OPENFS_INODE_MODE_DIRECTORY);
 root.size=4096U;root.generation=2U;assert(openfs_inode_write(&v,sb.inode_table_start,inode_count,&root)==OPENFS_INODE_OK);
 memset(&root,0,sizeof(root));assert(openfs_inode_read(&v,sb.inode_table_start,1U,inode_count,&root)==OPENFS_INODE_OK);assert(root.size==4096U&&root.generation==2U);
 assert(openfs_bitmap_set(&v,sb.inode_bitmap_start,sb.inode_bitmap_blocks,1U,1)==OPENFS_BITMAP_OK);
 assert(openfs_bitmap_test(&v,sb.inode_bitmap_start,sb.inode_bitmap_blocks,1U,&used)==OPENFS_BITMAP_OK&&used==1);openfs_inode_t many=root;memset(many.inline_data,0,sizeof(many.inline_data));memset(many.reserved,0,sizeof(many.reserved));many.inode_number=2U;many.generation=1U;many.parent_inode=1U;many.link_count=1U;many.mode=OPENFS_INODE_MODE_REGULAR;many.flags=OPENFS_INODE_FLAG_HAS_EXTENTS;many.extent_count=OPENFS_EXTENT_MAX;for(uint32_t k=0U;k<OPENFS_EXTENT_MAX;k++){openfs_extent_t e={((uint64_t)k)*100U,((uint64_t)k)*100U+50U,25U};assert(openfs_inode_set_extent(&many,k,&e)==OPENFS_EXTENT_OK);}assert(openfs_inode_write(&v,sb.inode_table_start,inode_count,&many)==OPENFS_INODE_OK);memset(&many,0,sizeof(many));assert(openfs_inode_read(&v,sb.inode_table_start,2U,inode_count,&many)==OPENFS_INODE_OK);assert(many.extent_count==OPENFS_EXTENT_MAX);for(uint32_t k=0U;k<OPENFS_EXTENT_MAX;k++){openfs_extent_t e={0};assert(openfs_inode_get_extent(&many,k,&e)==OPENFS_EXTENT_OK&&e.physical_start==((uint64_t)k)*100U+50U&&e.block_count==25U);}
 free(d.bytes);return 0;
}
