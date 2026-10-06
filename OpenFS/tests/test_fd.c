#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/fd.h"
#include "openfs/format.h"
typedef struct{uint8_t*b;uint32_t bs;uint64_t n;}disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*out){disk_t*d=c;if(f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(out,d->b+(size_t)(f*d->bs),(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*in){disk_t*d=c;if(f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),in,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}
int main(void){
 disk_t d={0};d.bs=4096U;d.n=128U;d.b=calloc((size_t)d.n,d.bs);assert(d.b);
 openfs_block_device_t dev={&d,d.bs,d.n,rd,wr,fl};openfs_superblock_t sb;uint8_t uuid[16]={0};
 assert(openfs_format(&dev,uuid)==OPENFS_FORMAT_OK);assert(openfs_read_superblock(&dev,&sb)==OPENFS_FORMAT_OK);
 openfs_file_handle_t*h=NULL;assert(openfs_fd_open(&dev,&sb,"/fd-test",OPENFS_FD_CREAT|OPENFS_FD_RDWR,OPENFS_INODE_MODE_REGULAR|0644,&h)==OPENFS_FD_OK);
 const char hello[]="hello OpenFS";assert(openfs_fd_write(h,hello,sizeof(hello)-1U)==OPENFS_FD_OK);
 int64_t pos=-1;assert(openfs_fd_seek(h,0,0,&pos)==OPENFS_FD_OK&&pos==0);
 char out[32]={0};size_t got=0;assert(openfs_fd_read(h,out,sizeof(out),&got)==OPENFS_FD_OK&&got==sizeof(hello)-1U&&memcmp(out,hello,got)==0);
 openfs_file_handle_t*dup=NULL;assert(openfs_fd_dup(h,&dup)==OPENFS_FD_OK);assert(dup==h);
 assert(openfs_fd_seek(dup,0,2,&pos)==OPENFS_FD_OK&&pos==(int64_t)(sizeof(hello)-1U));
 const char tail[]="!";assert(openfs_fd_write(dup,tail,1U)==OPENFS_FD_OK);
 assert(openfs_fd_close(h)==OPENFS_FD_OK);
 assert(openfs_fd_seek(dup,0,0,&pos)==OPENFS_FD_OK);
 memset(out,0,sizeof(out));assert(openfs_fd_read(dup,out,sizeof(out),&got)==OPENFS_FD_OK&&got==sizeof(hello));assert(memcmp(out,"hello OpenFS!",sizeof(hello)-1U)==0);
 assert(openfs_fd_truncate(dup,5U)==OPENFS_FD_OK);assert(openfs_fd_close(dup)==OPENFS_FD_OK);
 openfs_file_handle_t*r=NULL;assert(openfs_fd_open(&dev,&sb,"/fd-test",OPENFS_FD_RDONLY,0,&r)==OPENFS_FD_OK);
 assert(openfs_fd_read(r,out,sizeof(out),&got)==OPENFS_FD_OK&&got==5U);assert(memcmp(out,"hello",5U)==0);assert(openfs_fd_close(r)==OPENFS_FD_OK);
 assert(openfs_fd_open(&dev,&sb,"/missing",OPENFS_FD_RDONLY,0,&r)==OPENFS_FD_NOT_FOUND);
 assert(openfs_fd_open(&dev,&sb,"/fd-test",OPENFS_FD_CREAT|OPENFS_FD_EXCL|OPENFS_FD_RDWR,0,&r)==OPENFS_FD_EXISTS);
 free(d.b);return 0;
}