#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/xattr.h"
#include "openfs/fsck.h"
#include "openfs/path.h"
typedef struct{uint8_t*b;uint32_t bs;uint64_t bc;}D;
static openfs_io_result_t r(void*c,uint64_t f,uint32_t n,void*x){D*d=c;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(x,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t w(void*c,uint64_t f,uint32_t n,const void*x){D*d=c;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),x,(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t f(void*c){(void)c;return OPENFS_IO_OK;}
int main(void){D d={0};d.bs=4096U;d.bc=512U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={0};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);uint64_t ino=0U;assert(openfs_path_create(&v,&s,"/x",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);const char a[]="hello";assert(openfs_xattr_set(&v,&s,ino,"user.test",a,sizeof(a),0)==OPENFS_XATTR_OK);char out[16];size_t got=0;assert(openfs_xattr_get(&v,&s,ino,"user.test",out,sizeof(out),&got)==OPENFS_XATTR_OK&&got==sizeof(a)&&memcmp(out,a,sizeof(a))==0);const char b[]="world";assert(openfs_xattr_set(&v,&s,ino,"user.test",b,sizeof(b),OPENFS_XATTR_REPLACE)==OPENFS_XATTR_OK);assert(openfs_xattr_get(&v,&s,ino,"user.test",out,sizeof(out),&got)==OPENFS_XATTR_OK&&memcmp(out,b,sizeof(b))==0);char names[64];size_t used=0;assert(openfs_xattr_list(&v,&s,ino,names,sizeof(names),&used)==OPENFS_XATTR_OK&&strcmp(names,"user.test")==0);assert(openfs_xattr_remove(&v,&s,ino,"user.test")==OPENFS_XATTR_OK);assert(openfs_xattr_get(&v,&s,ino,"user.test",out,sizeof(out),&got)==OPENFS_XATTR_NOT_FOUND);uint64_t errors=0;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);free(d.b);return 0;}
