#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/extent.h"
#include "openfs/allocator.h"
#include "openfs/inode_alloc.h"
#include "openfs/bitmap.h"
typedef struct{uint8_t*b;uint32_t bs;uint64_t bc;}ED;
static openfs_io_result_t er( void*c,uint64_t b,uint32_t n,void*out){ED*d=c;if(b>=d->bc||(uint64_t)n>d->bc-b)return OPENFS_IO_OUT_OF_RANGE;memcpy(out,d->b+(size_t)b*d->bs,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t ew( void*c,uint64_t b,uint32_t n,const void*in){ED*d=c;if(b>=d->bc||(uint64_t)n>d->bc-b)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)b*d->bs,in,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t ef(void*c){(void)c;return OPENFS_IO_OK;}
static void extent_tree_root_collision_regression(void)
{
    ED d={0};d.bs=4096U;d.bc=512U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);
    openfs_block_device_t v={&d,d.bs,d.bc,er,ew,ef};uint8_t uuid[16]={0};openfs_superblock_t s;
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    uint64_t ino=0U;assert(openfs_inode_alloc(&v,&s,s.root_inode,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_INODE_ALLOC_OK);
    openfs_inode_t i;uint64_t inode_count=(s.inode_table_blocks*(uint64_t)s.block_size)/OPENFS_INODE_SIZE;
    assert(openfs_inode_read(&v,s.inode_table_start,ino,inode_count,&i)==OPENFS_INODE_OK);
    uint64_t b0=0U,b1=0U,b2=0U,b3=0U;
    assert(openfs_alloc_block(&v,&s,&b0)==OPENFS_ALLOC_OK);assert(openfs_alloc_block(&v,&s,&b1)==OPENFS_ALLOC_OK);
    assert(openfs_alloc_block(&v,&s,&b2)==OPENFS_ALLOC_OK);assert(openfs_alloc_block(&v,&s,&b3)==OPENFS_ALLOC_OK);
    openfs_extent_t e0={0U,b0,1U},e1={1U,b1,1U},e2={2U,b2,1U},e3={3U,b3,1U};
    assert(openfs_inode_set_extent(&i,0U,&e0)==OPENFS_EXTENT_OK);i.extent_count=1U;i.size=s.block_size;i.blocks=1U;i.flags|=OPENFS_INODE_FLAG_HAS_EXTENTS;
    assert(openfs_inode_write(&v,s.inode_table_start,inode_count,&i)==OPENFS_INODE_OK);
    assert(openfs_inode_set_extent(&i,1U,&e1)==OPENFS_EXTENT_OK);assert(openfs_inode_set_extent(&i,2U,&e2)==OPENFS_EXTENT_OK);assert(openfs_inode_set_extent(&i,3U,&e3)==OPENFS_EXTENT_OK);
    i.extent_count=5U;assert(openfs_inode_set_extent_tree_root(&i,b0)==OPENFS_EXTENT_OK);
    uint8_t *before=malloc(s.block_size),*after=malloc(s.block_size);assert(before&&after);memset(before,0xA5,s.block_size);
    assert(v.write(v.context,b0,1U,before)==OPENFS_IO_OK);
    openfs_extent_t tree_extent={4U,s.data_start+10U,1U};
    assert(openfs_extent_tree_write(&v,&s,&i,NULL,0U)==OPENFS_EXTENT_INVALID_ARGUMENT);
    assert(openfs_extent_tree_write(&v,&s,&i,&tree_extent,1U)==OPENFS_EXTENT_CORRUPT);
    assert(v.read(v.context,b0,1U,after)==OPENFS_IO_OK&&memcmp(before,after,s.block_size)==0);
    free(before);free(after);free(d.b);
}
static void extent_tree_unallocated_root_regression(void)
{
    ED d={0};d.bs=4096U;d.bc=512U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);
    openfs_block_device_t v={&d,d.bs,d.bc,er,ew,ef};uint8_t uuid[16]={0};openfs_superblock_t s;
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    uint64_t ino=0U;assert(openfs_inode_alloc(&v,&s,s.root_inode,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_INODE_ALLOC_OK);
    uint64_t count=(s.inode_table_blocks*(uint64_t)s.block_size)/OPENFS_INODE_SIZE;openfs_inode_t i;
    assert(openfs_inode_read(&v,s.inode_table_start,ino,count,&i)==OPENFS_INODE_OK);
    uint64_t blocks[6];for(unsigned n=0U;n<6U;n++)assert(openfs_alloc_block(&v,&s,&blocks[n])==OPENFS_ALLOC_OK);
    for(unsigned n=0U;n<4U;n++){openfs_extent_t e={n,blocks[n+1U],1U};assert(openfs_inode_set_extent(&i,n,&e)==OPENFS_EXTENT_OK);}
    i.extent_count=5U;i.blocks=5U;i.size=5U*s.block_size;i.flags|=OPENFS_INODE_FLAG_HAS_EXTENTS;
    assert(openfs_inode_set_extent_tree_root(&i,blocks[0])==OPENFS_EXTENT_OK);
    openfs_extent_t tree={4U,blocks[5],1U};
    assert(openfs_extent_tree_write(&v,&s,&i,&tree,1U)==OPENFS_EXTENT_OK);
    openfs_extent_t out={0};assert(openfs_extent_tree_read(&v,&s,&i,0U,&out)==OPENFS_EXTENT_OK);
    assert(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,blocks[0],0)==OPENFS_BITMAP_OK);
    assert(openfs_extent_tree_read(&v,&s,&i,0U,&out)==OPENFS_EXTENT_CORRUPT);
    free(d.b);
}

static void extent_overlap_regression(void){openfs_inode_t i;memset(&i,0,sizeof(i));i.inode_number=1U;i.generation=1U;i.mode=OPENFS_INODE_MODE_REGULAR;i.link_count=1U;openfs_extent_t a={0U,100U,2U},logical_overlap={1U,110U,1U},physical_overlap={2U,101U,1U},adjacent={2U,102U,1U};assert(openfs_inode_set_extent(&i,0U,&a)==OPENFS_EXTENT_OK);assert(openfs_inode_set_extent(&i,1U,&logical_overlap)==OPENFS_EXTENT_CORRUPT);assert(i.extent_count==1U);assert(openfs_inode_set_extent(&i,1U,&physical_overlap)==OPENFS_EXTENT_CORRUPT);assert(i.extent_count==1U);assert(openfs_inode_set_extent(&i,1U,&adjacent)==OPENFS_EXTENT_OK);assert(i.extent_count==2U);}int main(void){extent_tree_unallocated_root_regression();extent_overlap_regression();extent_tree_root_collision_regression();openfs_inode_t i;memset(&i,0,sizeof(i));i.inode_number=1U;i.generation=1U;i.mode=OPENFS_INODE_MODE_REGULAR;i.link_count=1U;openfs_extent_t e={4U,100U,8U},o={0};assert(openfs_inode_set_extent(&i,0U,&e)==OPENFS_EXTENT_OK);assert(i.extent_count==1U);assert(openfs_inode_get_extent(&i,0U,&o)==OPENFS_EXTENT_OK);assert(memcmp(&e,&o,sizeof(e))==0);openfs_extent_t e2={12U,108U,4U};assert(openfs_inode_set_extent(&i,1U,&e2)==OPENFS_EXTENT_OK);assert(i.extent_count==2U);assert(openfs_inode_get_extent(&i,1U,&o)==OPENFS_EXTENT_OK&&o.physical_start==108U);assert(openfs_inode_set_extent(&i,5U,&e)==OPENFS_EXTENT_OUT_OF_RANGE);assert(openfs_inode_get_extent(&i,5U,&o)==OPENFS_EXTENT_OUT_OF_RANGE);assert(i.extent_count==2U);i.flags=OPENFS_INODE_FLAG_EXTENT_TREE;assert(openfs_inode_set_extent_tree_root(&i,8U)==OPENFS_EXTENT_OK);assert(openfs_inode_set_extent_tree_root(&i,0U)==OPENFS_EXTENT_INVALID_ARGUMENT);return 0;}
