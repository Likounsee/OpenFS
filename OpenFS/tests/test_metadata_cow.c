#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/metadata_cow.h"
#include "openfs/format.h"
#include "openfs/cow.h"
typedef struct {uint8_t *b;uint32_t bs;uint64_t n;uint64_t fail_data_write;uint64_t fail_flush;} disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*out){disk_t*d=c;if(n==0U||f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(out,d->b+f*d->bs,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*in){disk_t*d=c;if(n==0U||f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;if(d->fail_data_write!=0U&&f>=2U){d->fail_data_write--;return OPENFS_IO_IO_ERROR;}memcpy(d->b+f*d->bs,in,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){disk_t*d=c;if(d->fail_flush!=0U){d->fail_flush--;return OPENFS_IO_IO_ERROR;}return OPENFS_IO_OK;}
static void setup(disk_t*d,openfs_block_device_t*dev,openfs_superblock_t*s){memset(d,0,sizeof(*d));d->bs=4096U;d->n=128U;d->b=calloc((size_t)d->bs,d->n);assert(d->b);*dev=(openfs_block_device_t){d,d->bs,d->n,rd,wr,fl};uint8_t u[16]={0};assert(openfs_format(dev,u)==OPENFS_FORMAT_OK);assert(openfs_read_superblock(dev,s)==OPENFS_FORMAT_OK);}
int main(void){
 disk_t x;openfs_block_device_t d;openfs_superblock_t s;setup(&x,&d,&s);
 uint64_t block=0U;assert(openfs_metadata_cow_alloc(&d,&s,OPENFS_METADATA_COW_TYPE_DIRECTORY,42U,1U,7U,&block)==OPENFS_METADATA_COW_OK);
 openfs_metadata_cow_header_t h;assert(openfs_metadata_cow_validate_block(&d,&s,block,&h)==OPENFS_METADATA_COW_OK);assert(h.type==OPENFS_METADATA_COW_TYPE_DIRECTORY&&h.logical_id==42U&&h.generation==1U&&h.flags==7U);
 uint8_t *raw=x.b+block*x.bs;raw[100]^=0x5a;assert(openfs_metadata_cow_validate_block(&d,&s,block,&h)==OPENFS_METADATA_COW_CORRUPT);raw[100]^=0x5a;
 uint16_t refs=0U;assert(openfs_metadata_cow_acquire(&d,&s,block,&refs)==OPENFS_METADATA_COW_OK&&refs==2U);
 uint64_t clone=0U;assert(openfs_metadata_cow_copy_before_write(&d,&s,block,OPENFS_METADATA_COW_TYPE_DIRECTORY,42U,2U,&clone)==OPENFS_METADATA_COW_OK&&clone!=block);
 assert(openfs_cow_refcount_get(&d,&s,block,&refs)==OPENFS_COW_OK&&refs==2U);assert(openfs_cow_refcount_get(&d,&s,clone,&refs)==OPENFS_COW_OK&&refs==1U);
 assert(openfs_metadata_cow_release(&d,&s,clone,&refs)==OPENFS_METADATA_COW_OK&&refs==0U);
 assert(openfs_metadata_cow_release(&d,&s,block,&refs)==OPENFS_METADATA_COW_OK&&refs==1U);
 assert(openfs_metadata_cow_release(&d,&s,block,&refs)==OPENFS_METADATA_COW_OK&&refs==0U);
 uint64_t failed=0U;assert(openfs_metadata_cow_alloc(&d,&s,OPENFS_METADATA_COW_TYPE_XATTR,9U,1U,0U,&failed)==OPENFS_METADATA_COW_OK);x.fail_data_write=1U;
 uint64_t clone_fail=0U;assert(openfs_metadata_cow_acquire(&d,&s,failed,NULL)==OPENFS_METADATA_COW_OK);assert(openfs_metadata_cow_clone(&d,&s,failed,OPENFS_METADATA_COW_TYPE_XATTR,9U,2U,&clone_fail)==OPENFS_METADATA_COW_IO_ERROR);assert(openfs_metadata_cow_validate_block(&d,&s,failed,&h)==OPENFS_METADATA_COW_OK);
 assert(openfs_metadata_cow_release(&d,&s,failed,NULL)==OPENFS_METADATA_COW_OK);assert(openfs_metadata_cow_release(&d,&s,failed,NULL)==OPENFS_METADATA_COW_OK);
 free(x.b);return 0;
}
