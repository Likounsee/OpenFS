#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/format.h"
#include "openfs/fsck.h"

typedef struct { uint8_t *bytes; uint32_t block_size; uint64_t block_count; unsigned flushes; unsigned writes; } disk_t;

static openfs_io_result_t rd(void *c,uint64_t f,uint32_t n,void *b){disk_t*d=c;if(n==0U||f>=d->block_count||(uint64_t)n>d->block_count-f)return OPENFS_IO_OUT_OF_RANGE;uint64_t o=f*d->block_size,l=(uint64_t)n*d->block_size;memcpy(b,d->bytes+o,(size_t)l);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void *c,uint64_t f,uint32_t n,const void *b){disk_t*d=c;if(n==0U||f>=d->block_count||(uint64_t)n>d->block_count-f)return OPENFS_IO_OUT_OF_RANGE;d->writes++;uint64_t o=f*d->block_size,l=(uint64_t)n*d->block_size;memcpy(d->bytes+o,b,(size_t)l);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void *c){((disk_t*)c)->flushes++;return OPENFS_IO_OK;}
static openfs_block_device_t dev(disk_t*d){return (openfs_block_device_t){d,d->block_size,d->block_count,rd,wr,fl};}

static void format_remount(void){
 disk_t d={.block_size=4096U,.block_count=128U}; d.bytes=calloc((size_t)d.block_count,d.block_size); assert(d.bytes);
 openfs_block_device_t v=dev(&d); const uint8_t u[16]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
 assert(openfs_format(&v,u)==OPENFS_FORMAT_OK); assert(d.flushes==1U); assert(d.writes<=16U);
 openfs_superblock_t s; assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
 assert(s.block_size==4096U&&s.total_blocks==128U&&s.root_inode==1U); assert((s.feature_flags&OPENFS_FEATURE_EXTENT_TREE)!=0U); assert(memcmp(s.uuid,u,16U)==0);
 assert(memcmp(d.bytes+((d.block_count-1U)*d.block_size),d.bytes,d.block_size)==0); free(d.bytes);
}
static void fast_vs_full_zero(void){
 disk_t d={.block_size=4096U,.block_count=128U}; d.bytes=malloc((size_t)d.block_count*d.block_size); assert(d.bytes);
 memset(d.bytes,0xA5,(size_t)d.block_count*d.block_size);
 openfs_block_device_t v=dev(&d); const uint8_t u[16]={1U};
 assert(openfs_format_ex(&v,u,OPENFS_FORMAT_FLAG_NONE)==OPENFS_FORMAT_OK);
 openfs_superblock_t s; assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
 uint64_t errors=UINT64_MAX; assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK); assert(errors==0U);
 assert(d.bytes[(size_t)s.data_start*d.block_size]==0xA5);
 memset(d.bytes,0xA5,(size_t)d.block_count*d.block_size);
 assert(openfs_format_ex(&v,u,OPENFS_FORMAT_FLAG_FULL_ZERO)==OPENFS_FORMAT_OK);
 assert(d.bytes[(size_t)s.data_start*d.block_size]==0U);
 free(d.bytes);
}
static void checksum(void){
 disk_t d={.block_size=4096U,.block_count=64U}; d.bytes=calloc((size_t)d.block_count,d.block_size); assert(d.bytes);
 openfs_block_device_t v=dev(&d); const uint8_t u[16]={0}; assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);
 d.bytes[32]^=0x80U; openfs_superblock_t s; assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_CORRUPT); free(d.bytes);
}
static void layout_validation(void){
 disk_t d={.block_size=4096U,.block_count=64U}; d.bytes=calloc((size_t)d.block_count,d.block_size); assert(d.bytes);
 openfs_block_device_t v=dev(&d); openfs_superblock_t s={0}; s.version_major=OPENFS_FORMAT_VERSION_MAJOR; s.version_minor=0U; s.block_size=4096U; s.total_blocks=64U; s.metadata_start=2U; s.metadata_blocks=61U; s.inode_bitmap_start=3U; s.inode_bitmap_blocks=1U; s.inode_table_start=4U; s.inode_table_blocks=20U; s.journal_start=25U; s.journal_blocks=38U; s.root_inode=1U;
 assert(openfs_validate_superblock(&v,&s)==OPENFS_FORMAT_CORRUPT); free(d.bytes);
}
static void feature_validation(void){
 disk_t d={.block_size=4096U,.block_count=64U}; d.bytes=calloc((size_t)d.block_count,d.block_size); assert(d.bytes);
 openfs_block_device_t v=dev(&d); openfs_superblock_t s={0}; s.version_major=OPENFS_FORMAT_VERSION_MAJOR; s.version_minor=2U; s.feature_flags=OPENFS_FEATURE_EXTENT_TREE; s.block_size=4096U; s.total_blocks=64U;
 assert(openfs_validate_superblock(&v,&s)==OPENFS_FORMAT_CORRUPT); s.version_minor=OPENFS_FORMAT_VERSION_MINOR; s.feature_flags=(1ULL<<63); assert(openfs_validate_superblock(&v,&s)==OPENFS_FORMAT_CORRUPT); free(d.bytes);
}
static void block_bitmap_capacity(void){disk_t d={.block_size=4096U,.block_count=40000U}; d.bytes=calloc((size_t)d.block_count,d.block_size); assert(d.bytes); openfs_block_device_t v=dev(&d);openfs_superblock_t s={0};s.version_major=OPENFS_FORMAT_VERSION_MAJOR;s.version_minor=OPENFS_FORMAT_VERSION_MINOR;s.block_size=4096U;s.total_blocks=40000U;s.metadata_start=2U;s.metadata_blocks=39997U;s.block_bitmap_start=3U;s.block_bitmap_blocks=1U;s.inode_bitmap_start=4U;s.inode_bitmap_blocks=1U;s.inode_table_start=5U;s.inode_table_blocks=1000U;s.journal_start=1005U;s.journal_blocks=6227U;s.data_start=7232U;s.data_blocks=32767U;s.root_inode=1U;assert(openfs_validate_superblock(&v,&s)==OPENFS_FORMAT_CORRUPT);free(d.bytes);}
static void layout_boundary_format(void){
 disk_t d={.block_size=4096U,.block_count=2986U};d.bytes=calloc((size_t)d.block_count,d.block_size);assert(d.bytes);
 openfs_block_device_t v=dev(&d);const uint8_t u[16]={1U};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);
 openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);assert(s.data_blocks>=8U);free(d.bytes);
}
static void geometry(void){
 disk_t d={.block_size=3000U,.block_count=64U}; d.bytes=calloc((size_t)d.block_count,d.block_size); assert(d.bytes);
 openfs_block_device_t v=dev(&d); const uint8_t u[16]={0}; assert(openfs_format(&v,u)==OPENFS_FORMAT_UNSUPPORTED_DEVICE); free(d.bytes);
}
int main(void){format_remount();fast_vs_full_zero();checksum();layout_validation();feature_validation();block_bitmap_capacity();layout_boundary_format();geometry();return 0;}
