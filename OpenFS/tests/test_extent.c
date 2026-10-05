#include <assert.h>
#include <string.h>
#include "openfs/extent.h"
typedef struct{uint8_t*b;uint32_t bs;uint64_t bc;}ED;
static openfs_io_result_t er( void*c,uint64_t b,uint32_t n,void*out){ED*d=c;if(b>=d->bc||(uint64_t)n>d->bc-b)return OPENFS_IO_OUT_OF_RANGE;memcpy(out,d->b+(size_t)b*d->bs,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t ew( void*c,uint64_t b,uint32_t n,const void*in){ED*d=c;if(b>=d->bc||(uint64_t)n>d->bc-b)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)b*d->bs,in,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t ef(void*c){(void)c;return OPENFS_IO_OK;}
static void extent_tree_root_collision_regression(void)
{
    ED d={0};d.bs=4096U;d.bc=512U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);
    openfs_block_device_t v={&d,d.bs,d.bc,er,ew,ef};uint8_t uuid[16]={0};openfs_superblock_t s;
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    openfs_inode_t i;memset(&i,0,sizeof(i));i.inode_number=7U;i.generation=1U;i.mode=OPENFS_INODE_MODE_REGULAR;i.link_count=1U;
    openfs_extent_t e0={0U,s.data_start,1U},e1={1U,s.data_start+1U,1U},e2={2U,s.data_start+2U,1U},e3={3U,s.data_start+3U,1U};
    assert(openfs_inode_set_extent(&i,0U,&e0)==OPENFS_EXTENT_OK);assert(openfs_inode_set_extent(&i,1U,&e1)==OPENFS_EXTENT_OK);assert(openfs_inode_set_extent(&i,2U,&e2)==OPENFS_EXTENT_OK);assert(openfs_inode_set_extent(&i,3U,&e3)==OPENFS_EXTENT_OK);
    i.extent_count=5U;assert(openfs_inode_set_extent_tree_root(&i,e0.physical_start)==OPENFS_EXTENT_OK);
    openfs_extent_t tree_extent={4U,s.data_start+10U,1U};uint8_t *before=malloc(s.block_size);assert(before);assert(v.read(v.context,e0.physical_start,1U,before)==OPENFS_IO_OK);
    assert(openfs_extent_tree_write(&v,&s,&i,&tree_extent,1U)==OPENFS_EXTENT_CORRUPT);
    uint8_t *after=malloc(s.block_size);assert(after);assert(v.read(v.context,e0.physical_start,1U,after)==OPENFS_IO_OK);assert(memcmp(before,after,s.block_size)==0);
    free(before);free(after);free(d.b);
}
int main(void){extent_tree_root_collision_regression();openfs_inode_t i;memset(&i,0,sizeof(i));i.inode_number=1U;i.generation=1U;i.mode=OPENFS_INODE_MODE_REGULAR;i.link_count=1U;openfs_extent_t e={4U,100U,8U},o={0};assert(openfs_inode_set_extent(&i,0U,&e)==OPENFS_EXTENT_OK);assert(i.extent_count==1U);assert(openfs_inode_get_extent(&i,0U,&o)==OPENFS_EXTENT_OK);assert(memcmp(&e,&o,sizeof(e))==0);openfs_extent_t e2={12U,108U,4U};assert(openfs_inode_set_extent(&i,1U,&e2)==OPENFS_EXTENT_OK);assert(i.extent_count==2U);assert(openfs_inode_get_extent(&i,1U,&o)==OPENFS_EXTENT_OK&&o.physical_start==108U);assert(openfs_inode_set_extent(&i,5U,&e)==OPENFS_EXTENT_OUT_OF_RANGE);assert(openfs_inode_get_extent(&i,5U,&o)==OPENFS_EXTENT_OUT_OF_RANGE);assert(i.extent_count==2U);i.flags=OPENFS_INODE_FLAG_EXTENT_TREE;assert(openfs_inode_set_extent_tree_root(&i,8U)==OPENFS_EXTENT_OK);assert(openfs_inode_set_extent_tree_root(&i,0U)==OPENFS_EXTENT_INVALID_ARGUMENT);return 0;}
