#include <assert.h>
#include <string.h>
#include "openfs/extent.h"
int main(void){openfs_inode_t i;memset(&i,0,sizeof(i));i.inode_number=1U;i.generation=1U;i.mode=OPENFS_INODE_MODE_REGULAR;i.link_count=1U;openfs_extent_t e={4U,100U,8U},o={0};assert(openfs_inode_set_extent(&i,0U,&e)==OPENFS_EXTENT_OK);assert(i.extent_count==1U);assert(openfs_inode_get_extent(&i,0U,&o)==OPENFS_EXTENT_OK);assert(memcmp(&e,&o,sizeof(e))==0);openfs_extent_t e2={12U,108U,4U};assert(openfs_inode_set_extent(&i,1U,&e2)==OPENFS_EXTENT_OK);assert(i.extent_count==2U);assert(openfs_inode_get_extent(&i,1U,&o)==OPENFS_EXTENT_OK&&o.physical_start==108U);assert(openfs_inode_set_extent(&i,5U,&e)==OPENFS_EXTENT_OUT_OF_RANGE);assert(openfs_inode_get_extent(&i,5U,&o)==OPENFS_EXTENT_OUT_OF_RANGE);return 0;}
