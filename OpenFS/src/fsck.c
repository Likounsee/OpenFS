#include "openfs/fsck.h"
#include "openfs/bitmap.h"
#include "openfs/extent.h"
#include "openfs/file.h"
#include "openfs/dir.h"
#include "openfs/crc32c.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
static openfs_fsck_result_t icount(const openfs_superblock_t*s,uint64_t*n){if(s==NULL||s->block_size==0U||s->inode_table_blocks>UINT64_MAX/s->block_size)return OPENFS_FSCK_CORRUPT;*n=(s->inode_table_blocks*s->block_size)/OPENFS_INODE_SIZE;return *n?OPENFS_FSCK_OK:OPENFS_FSCK_CORRUPT;}
static int add(uint64_t a,uint64_t b,uint64_t*o){if(b>UINT64_MAX-a)return 0;*o=a+b;return 1;}
static int ref_mark(uint8_t*refs,uint64_t index){uint64_t byte=index/8U;if(byte>SIZE_MAX)return 0;refs[(size_t)byte]|=(uint8_t)(1U<<(index%8U));return 1;}
static int ref_test(const uint8_t*refs,uint64_t index){uint64_t byte=index/8U;if(byte>SIZE_MAX)return 0;return (refs[(size_t)byte]&(uint8_t)(1U<<(index%8U)))!=0U;}
static int decode_dir_entry(const uint8_t*r,uint64_t*ino,uint64_t*gen,uint8_t*type){
if(memcmp(r,"ODIR1",5U)!=0)return 0;
uint32_t stored=(uint32_t)r[252U]|((uint32_t)r[253U]<<8U)|((uint32_t)r[254U]<<16U)|((uint32_t)r[255U]<<24U);
if(stored!=openfs_crc32c(r,252U))return -1;
uint32_t len=r[7U];if(len==0U||len>OPENFS_DIR_NAME_MAX)return -1;
for(uint32_t i=0U;i<len;i++){if(r[24U+i]=='/'||r[24U+i]=='\0')return -1;}
*ino=0U;*gen=0U;for(unsigned i=0U;i<8U;i++){*ino|=(uint64_t)r[8U+i]<<(8U*i);*gen|=(uint64_t)r[16U+i]<<(8U*i);}*type=r[6U];return 1;}
static int same_dir_name(const uint8_t*a,const uint8_t*b){uint32_t la=a[7U],lb=b[7U];return la==lb&&memcmp(a+24U,b+24U,la)==0;}
static uint8_t inode_dir_type(uint32_t mode){
switch(mode&OPENFS_INODE_TYPE_MASK){case OPENFS_INODE_MODE_REGULAR:return 1U;case OPENFS_INODE_MODE_DIRECTORY:return 2U;case OPENFS_INODE_MODE_SYMLINK:return 3U;default:return 0U;}}
openfs_fsck_result_t openfs_fsck(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t*errors){
if (errors != NULL) *errors = 0U;
if(!openfs_block_device_is_valid(d)||s==NULL||errors==NULL)return OPENFS_FSCK_INVALID_ARGUMENT;
if(openfs_validate_superblock(d,s)!=OPENFS_FORMAT_OK)return OPENFS_FSCK_CORRUPT;
uint64_t count=0U;if(icount(s,&count)!=OPENFS_FSCK_OK)return OPENFS_FSCK_CORRUPT;
uint64_t ref_bytes64=0U;if(!add(s->data_blocks,7U,&ref_bytes64))return OPENFS_FSCK_CORRUPT;ref_bytes64/=8U;if(ref_bytes64>SIZE_MAX)return OPENFS_FSCK_CORRUPT;
uint8_t*refs=calloc(1U,(size_t)ref_bytes64);if(refs==NULL&&ref_bytes64!=0U)return OPENFS_FSCK_IO_ERROR;
if(count==UINT64_MAX||count+1U>SIZE_MAX/sizeof(uint64_t)||count+1U>SIZE_MAX)return OPENFS_FSCK_CORRUPT;
uint64_t*dir_refs=calloc((size_t)(count+1U),sizeof(*dir_refs));if(dir_refs==NULL){free(refs);return OPENFS_FSCK_IO_ERROR;}
uint64_t bad=0U;
openfs_fsck_result_t result=OPENFS_FSCK_OK;
openfs_inode_t root;
if(openfs_inode_read(d,s->inode_table_start,s->root_inode,count,&root)!=OPENFS_INODE_OK){
bad++;
}else if(root.inode_number!=s->root_inode||(root.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY||root.parent_inode!=s->root_inode||root.link_count==0U){
bad++;
}
int root_allocated=0;
if(openfs_bitmap_test(d,s->inode_bitmap_start,s->inode_bitmap_blocks,s->root_inode-1U,&root_allocated)!=OPENFS_BITMAP_OK){
result=OPENFS_FSCK_IO_ERROR;
goto done;
}
if(!root_allocated)bad++;
for(uint64_t n=1U;n<=count;n++){int used=0;if(openfs_bitmap_test(d,s->inode_bitmap_start,s->inode_bitmap_blocks,n-1U,&used)!=OPENFS_BITMAP_OK){result=OPENFS_FSCK_IO_ERROR;goto done;}openfs_inode_t in;if(openfs_inode_read(d,s->inode_table_start,n,count,&in)!=OPENFS_INODE_OK){if(used)bad++;continue;}
if(used){if(in.mode==OPENFS_INODE_MODE_FREE||in.link_count==0U||in.inode_number!=n)bad++;if(in.mode==OPENFS_INODE_MODE_FREE&&in.blocks!=0U)bad++;
uint64_t extent_total=0U;uint64_t previous_logical_end=0U;for(uint32_t i=0U;i<in.extent_count;i++){openfs_extent_t e;if(openfs_inode_get_extent(&in,i,&e)!=OPENFS_EXTENT_OK||e.block_count==0U){bad++;continue;}uint64_t pe=0U,data_end=0U,logical_end=0U;if(!add(extent_total,e.block_count,&extent_total)||!add(s->data_start,s->data_blocks,&data_end)||!add(e.physical_start,e.block_count,&pe)||!add(e.logical_start,e.block_count,&logical_end)||e.physical_start<s->data_start||pe>data_end){bad++;}else{if((i==0U&&e.logical_start!=0U)||(i>0U&&e.logical_start!=previous_logical_end))bad++;for(uint64_t b=0U;b<e.block_count;b++){uint64_t physical=e.physical_start+b,rel=physical-s->data_start;int allocated=0;if(openfs_bitmap_test(d,s->block_bitmap_start,s->block_bitmap_blocks,physical,&allocated)!=OPENFS_BITMAP_OK){result=OPENFS_FSCK_IO_ERROR;goto done;}if(!allocated)bad++;if(ref_test(refs,rel))bad++;else if(!ref_mark(refs,rel)){result=OPENFS_FSCK_CORRUPT;goto done;}}}previous_logical_end=logical_end;}if(extent_total!=in.blocks)bad++;if((in.flags&OPENFS_INODE_FLAG_INLINE_DATA)!=0U){if((in.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_SYMLINK||in.blocks!=0U||in.extent_count!=0U||in.size>sizeof(in.inline_data))bad++;}else{uint64_t required=in.size==0U?0U:1U+(in.size-1U)/(uint64_t)d->block_size;if(required!=in.blocks)bad++;}}
else if(in.mode!=OPENFS_INODE_MODE_FREE||in.link_count!=0U)bad++;}
for(uint64_t n=1U;n<=count;n++){
openfs_inode_t in;if(openfs_inode_read(d,s->inode_table_start,n,count,&in)!=OPENFS_INODE_OK)continue;
if((in.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)continue;
if(in.size%OPENFS_DIR_ENTRY_SIZE!=0U){bad++;continue;}
uint64_t entries=in.size/OPENFS_DIR_ENTRY_SIZE;uint8_t raw[OPENFS_DIR_ENTRY_SIZE];
for(uint64_t e=0U;e<entries;e++){
size_t got=0U;if(openfs_file_read(d,s,&in,e*OPENFS_DIR_ENTRY_SIZE,raw,sizeof(raw),&got)!=OPENFS_FILE_OK||got!=sizeof(raw)){result=OPENFS_FSCK_IO_ERROR;goto done;}
uint64_t target_ino=0U,generation=0U;uint8_t type=0U;int decoded=decode_dir_entry(raw,&target_ino,&generation,&type);
if(decoded==0) continue;
if(decoded<0){bad++;continue;}
for(uint64_t prior=0U;prior<e;prior++){uint8_t prev[OPENFS_DIR_ENTRY_SIZE];size_t prev_got=0U;if(openfs_file_read(d,s,&in,prior*OPENFS_DIR_ENTRY_SIZE,prev,sizeof(prev),&prev_got)!=OPENFS_FILE_OK||prev_got!=sizeof(prev)){result=OPENFS_FSCK_IO_ERROR;goto done;}uint64_t pino=0U,pgen=0U;uint8_t ptype=0U;int pd=decode_dir_entry(prev,&pino,&pgen,&ptype);if(pd==1&&same_dir_name(raw,prev)){bad++;break;}}
if(target_ino==0U||target_ino>count||target_ino==s->root_inode||generation==0U){bad++;continue;}
openfs_inode_t target;if(openfs_inode_read(d,s->inode_table_start,target_ino,count,&target)!=OPENFS_INODE_OK){bad++;continue;}
if(target.generation!=generation||type!=inode_dir_type(target.mode)){bad++;continue;}
if((target.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_DIRECTORY&&target.parent_inode!=n)bad++;
if(dir_refs[target_ino]==UINT64_MAX)bad++;else dir_refs[target_ino]++;
}}
for(uint64_t n=1U;n<=count;n++){
int used=0;if(openfs_bitmap_test(d,s->inode_bitmap_start,s->inode_bitmap_blocks,n-1U,&used)!=OPENFS_BITMAP_OK){result=OPENFS_FSCK_IO_ERROR;goto done;}
if(!used)continue;
openfs_inode_t in;if(openfs_inode_read(d,s->inode_table_start,n,count,&in)!=OPENFS_INODE_OK)continue;
if(n==s->root_inode){if(dir_refs[n]!=0U||in.link_count!=1U)bad++;}else if(dir_refs[n]!=in.link_count||dir_refs[n]==0U)bad++;
}
for(uint64_t b=0U;b<s->data_start;b++){int set=0;if(openfs_bitmap_test(d,s->block_bitmap_start,s->block_bitmap_blocks,b,&set)!=OPENFS_BITMAP_OK){result=OPENFS_FSCK_IO_ERROR;goto done;}if(!set)bad++;}
if(s->data_start>UINT64_MAX-s->data_blocks){result=OPENFS_FSCK_CORRUPT;goto done;}uint64_t data_end=s->data_start+s->data_blocks;for(uint64_t b=s->data_start;b<data_end;b++){int set=0;if(openfs_bitmap_test(d,s->block_bitmap_start,s->block_bitmap_blocks,b,&set)!=OPENFS_BITMAP_OK){result=OPENFS_FSCK_IO_ERROR;goto done;}if(set&&!ref_test(refs,b-s->data_start))bad++;}
for(uint64_t b=data_end;b<d->block_count;b++){int set=0;if(openfs_bitmap_test(d,s->block_bitmap_start,s->block_bitmap_blocks,b,&set)!=OPENFS_BITMAP_OK){result=OPENFS_FSCK_IO_ERROR;goto done;}if(b==d->block_count-1U){if(!set)bad++;}else if(set)bad++;}
if(s->inode_bitmap_blocks>UINT64_MAX/d->block_size)return OPENFS_FSCK_CORRUPT;uint64_t inode_bitmap_bytes=s->inode_bitmap_blocks*(uint64_t)d->block_size;if(inode_bitmap_bytes>UINT64_MAX/8U)return OPENFS_FSCK_CORRUPT;uint64_t inode_cap=inode_bitmap_bytes*8U;if(inode_cap>count){for(uint64_t bit=count;bit<inode_cap;bit++){int set=0;if(openfs_bitmap_test(d,s->inode_bitmap_start,s->inode_bitmap_blocks,bit,&set)!=OPENFS_BITMAP_OK){result=OPENFS_FSCK_IO_ERROR;goto done;}if(set)bad++;}}
done:free(dir_refs);free(refs);*errors=bad;return result!=OPENFS_FSCK_OK?result:(bad==0U?OPENFS_FSCK_OK:OPENFS_FSCK_CORRUPT);}
