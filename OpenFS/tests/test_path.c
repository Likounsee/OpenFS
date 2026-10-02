#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/path.h"
typedef struct{uint8_t*b;uint32_t bs;uint64_t bc;}D;
static openfs_io_result_t r(void*c,uint64_t f,uint32_t n,void*x){D*d=c;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(x,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t w(void*c,uint64_t f,uint32_t n,const void*x){D*d=c;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),x,(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t f(void*c){(void)c;return OPENFS_IO_OK;}
int main(void){D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={0};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);uint64_t n=0,m=0,x=0;assert(openfs_path_mkdir(&v,&s,"/home",&n)==OPENFS_PATH_OK);assert(openfs_path_mkdir(&v,&s,"/home/test",&m)==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/home/./test/../test",&x)==OPENFS_PATH_OK&&x==m);assert(openfs_path_create(&v,&s,"/home/test/file",OPENFS_INODE_MODE_REGULAR,&x)==OPENFS_PATH_OK);uint64_t q=0;assert(openfs_path_lookup(&v,&s,"/home/test/file",&q)==OPENFS_PATH_OK&&q==x);assert(openfs_path_rename(&v,&s,"/home/test/file","/home/test/newfile")==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/home/test/newfile",&q)==OPENFS_PATH_OK&&q==x);assert(openfs_path_unlink(&v,&s,"/home/test/newfile")==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/home/test/newfile",&q)==OPENFS_PATH_NOT_FOUND);free(d.b);return 0;}