#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/format.h"
#include "openfs/metadata_root.h"
#include "openfs/metadata_cow.h"
#include "openfs/cow.h"
typedef struct {uint8_t*b;uint32_t bs;uint64_t n;} disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*out){disk_t*d=c;if(n==0U||f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(out,d->b+f*d->bs,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*in){disk_t*d=c;if(n==0U||f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+f*d->bs,in,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}
int main(void){disk_t x={.bs=4096U,.n=128U};x.b=calloc((size_t)x.bs,x.n);assert(x.b);openfs_block_device_t d={&x,x.bs,x.n,rd,wr,fl};uint8_t u[16]={0};assert(openfs_format(&d,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&d,&s)==OPENFS_FORMAT_OK);assert((s.feature_flags&OPENFS_FEATURE_METADATA_ROOT)!=0U);assert(s.version_minor==5U);openfs_metadata_root_t r;assert(openfs_metadata_root_read(&d,&s,s.metadata_root_block,&r)==OPENFS_METADATA_ROOT_OK);assert(r.generation==1U);uint16_t refs=0U;assert(openfs_cow_refcount_get(&d,&s,s.metadata_root_block,&refs)==OPENFS_COW_OK&&refs==1U);uint64_t next=0U;assert(openfs_metadata_root_update_generation(&d,&s,s.metadata_root_block,2U,&next)==OPENFS_METADATA_ROOT_OK&&next!=s.metadata_root_block);assert(openfs_metadata_root_read(&d,&s,next,&r)==OPENFS_METADATA_ROOT_OK&&r.generation==2U);assert(openfs_metadata_root_read(&d,&s,s.metadata_root_block,&r)==OPENFS_METADATA_ROOT_OK&&r.generation==1U);assert(openfs_metadata_cow_release(&d,&s,next,NULL)==OPENFS_METADATA_COW_OK);free(x.b);return 0;}
