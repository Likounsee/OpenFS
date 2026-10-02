#include "openfs/extent.h"
#include <limits.h>
static uint8_t *slot(openfs_inode_t*i,uint32_t n){return i->reserved+(size_t)n*24U;}
openfs_extent_result_t openfs_inode_get_extent(const openfs_inode_t*i,uint32_t n,openfs_extent_t*out){
 if(i==NULL||out==NULL)return OPENFS_EXTENT_INVALID_ARGUMENT;if(n>=i->extent_count||n>=OPENFS_EXTENT_MAX)return OPENFS_EXTENT_OUT_OF_RANGE;
 const uint8_t*p=slot((openfs_inode_t*)i,n);uint64_t a=0,b=0,c=0;for(unsigned k=0;k<8;k++){a|=(uint64_t)p[k]<<(8*k);b|=(uint64_t)p[8+k]<<(8*k);c|=(uint64_t)p[16+k]<<(8*k);}
 if(c==0||a>UINT64_MAX-c||b>UINT64_MAX-c)return OPENFS_EXTENT_CORRUPT;out->logical_start=a;out->physical_start=b;out->block_count=c;return OPENFS_EXTENT_OK;
}
openfs_extent_result_t openfs_inode_set_extent(openfs_inode_t*i,uint32_t n,const openfs_extent_t*e){
 if(i==NULL||e==NULL)return OPENFS_EXTENT_INVALID_ARGUMENT;if(n>=OPENFS_EXTENT_MAX||e->block_count==0U)return OPENFS_EXTENT_OUT_OF_RANGE;
 if(e->logical_start>UINT64_MAX-e->block_count||e->physical_start>UINT64_MAX-e->block_count)return OPENFS_EXTENT_CORRUPT;
 uint8_t*p=slot(i,n);for(unsigned k=0;k<8;k++){p[k]=(uint8_t)(e->logical_start>>(8*k));p[8+k]=(uint8_t)(e->physical_start>>(8*k));p[16+k]=(uint8_t)(e->block_count>>(8*k));}
 if(n>=i->extent_count)i->extent_count=n+1U;i->flags|=OPENFS_INODE_FLAG_HAS_EXTENTS;return OPENFS_EXTENT_OK;
}
