#include "openfs/fsck.h"
#include "openfs/bitmap.h"
#include "openfs/extent.h"
#include <limits.h>
#include <stdlib.h>
static openfs_fsck_result_t icount(const openfs_superblock_t*s,uint64_t*n){if(s==NULL||s->block_size==0U||s->inode_table_blocks>UINT64_MAX/s->block_size)return OPENFS_FSCK_CORRUPT;*n=(s->inode_table_blocks*s->block_size)/OPENFS_INODE_SIZE;return *n?OPENFS_FSCK_OK:OPENFS_FSCK_CORRUPT;}
static int add(uint64_t a,uint64_t b,uint64_t*o){if(b>UINT64_MAX-a)return 0;*o=a+b;return 1;}
static int ref_mark(uint8_t*refs,uint64_t index){uint64_t byte=index/8U;if(byte>SIZE_MAX)return 0;refs[(size_t)byte]|=(uint8_t)(1U<<(index%8U));return 1;}
static int ref_test(const uint8_t*refs,uint64_t index){uint64_t byte=index/8U;if(byte>SIZE_MAX)return 0;return (refs[(size_t)byte]&(uint8_t)(1U<<(index%8U)))!=0U;}
openfs_fsck_result_t openfs_fsck(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t*errors){
if(errors)*errors=0U;if(!openfs_block_device_is_valid(d)||s==NULL||errors==NULL)return OPENFS_FSCK_INVALID_ARGUMENT;if(openfs_validate_superblock(d,s)!=OPENFS_FORMAT_OK)return OPENFS_FSCK_CORRUPT;
uint64_t count=0U;if(icount(s,&count)!=OPENFS_FSCK_OK)return OPENFS_FSCK_CORRUPT;
uint64_t ref_bytes64=0U;if(!add(s->data_blocks,7U,&ref_bytes64))return OPENFS_FSCK_CORRUPT;ref_bytes64/=8U;if(ref_bytes64>SIZE_MAX)return OPENFS_FSCK_CORRUPT;
uint8_t*refs=calloc(1U,(size_t)ref_bytes64);if(refs==NULL&&ref_bytes64!=0U)return OPENFS_FSCK_IO_ERROR;
uint64_t bad=0U;openfs_fsck_result_t result=OPENFS_FSCK_OK;
for(uint64_t n=1U;n<=count;n++){int used=0;if(openfs_bitmap_test(d,s->inode_bitmap_start,s->inode_bitmap_blocks,n-1U,&used)!=OPENFS_BITMAP_OK){result=OPENFS_FSCK_IO_ERROR;goto done;}openfs_inode_t in;if(openfs_inode_read(d,s->inode_table_start,n,count,&in)!=OPENFS_INODE_OK){if(used)bad++;continue;}
if(used){if(in.mode==OPENFS_INODE_MODE_FREE||in.link_count==0U||in.inode_number!=n)bad++;if(in.mode==OPENFS_INODE_MODE_FREE&&in.blocks!=0U)bad++;
for(uint32_t i=0U;i<in.extent_count;i++){openfs_extent_t e;if(openfs_inode_get_extent(&in,i,&e)!=OPENFS_EXTENT_OK||e.block_count==0U){bad++;continue;}uint64_t pe=0U,data_end=0U;if(!add(s->data_start,s->data_blocks,&data_end)||!add(e.physical_start,e.block_count,&pe)||e.physical_start<s->data_start||pe>data_end){bad++;}else{for(uint64_t b=0U;b<e.block_count;b++){uint64_t physical=e.physical_start+b,rel=physical-s->data_start;int allocated=0;if(openfs_bitmap_test(d,s->block_bitmap_start,s->block_bitmap_blocks,physical,&allocated)!=OPENFS_BITMAP_OK){result=OPENFS_FSCK_IO_ERROR;goto done;}if(!allocated)bad++;if(ref_test(refs,rel))bad++;else if(!ref_mark(refs,rel)){result=OPENFS_FSCK_CORRUPT;goto done;}}}if(i>0U){openfs_extent_t prev;uint64_t le=0U,pe_prev=0U;if(openfs_inode_get_extent(&in,i-1U,&prev)!=OPENFS_EXTENT_OK||!add(prev.logical_start,prev.block_count,&le)||!add(prev.physical_start,prev.block_count,&pe_prev)||e.logical_start<le||e.physical_start<pe_prev)bad++;}}}
else if(in.mode!=OPENFS_INODE_MODE_FREE||in.link_count!=0U)bad++;}
for(uint64_t b=0U;b<s->data_start;b++){int set=0;if(openfs_bitmap_test(d,s->block_bitmap_start,s->block_bitmap_blocks,b,&set)!=OPENFS_BITMAP_OK){result=OPENFS_FSCK_IO_ERROR;goto done;}if(!set)bad++;}
if(s->data_start>UINT64_MAX-s->data_blocks){result=OPENFS_FSCK_CORRUPT;goto done;}uint64_t data_end=s->data_start+s->data_blocks;for(uint64_t b=s->data_start;b<data_end;b++){int set=0;if(openfs_bitmap_test(d,s->block_bitmap_start,s->block_bitmap_blocks,b,&set)!=OPENFS_BITMAP_OK){result=OPENFS_FSCK_IO_ERROR;goto done;}if(set&&!ref_test(refs,b-s->data_start))bad++;}
for(uint64_t b=data_end;b<d->block_count;b++){int set=0;if(openfs_bitmap_test(d,s->block_bitmap_start,s->block_bitmap_blocks,b,&set)!=OPENFS_BITMAP_OK){result=OPENFS_FSCK_IO_ERROR;goto done;}if(b==d->block_count-1U){if(!set)bad++;}else if(set)bad++;}
uint64_t inode_cap=s->inode_bitmap_blocks*(uint64_t)d->block_size*8U;if(inode_cap>count){for(uint64_t bit=count;bit<inode_cap;bit++){int set=0;if(openfs_bitmap_test(d,s->inode_bitmap_start,s->inode_bitmap_blocks,bit,&set)!=OPENFS_BITMAP_OK){result=OPENFS_FSCK_IO_ERROR;goto done;}if(set)bad++;}}
done:free(refs);*errors=bad;return result!=OPENFS_FSCK_OK?result:(bad==0U?OPENFS_FSCK_OK:OPENFS_FSCK_CORRUPT);}
