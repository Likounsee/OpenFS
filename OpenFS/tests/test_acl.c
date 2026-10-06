#include <assert.h>
#include <stdlib.h>
#include "openfs/acl.h"
#include "openfs/path.h"
#include "openfs/fsck.h"
typedef struct{unsigned char*b;unsigned bs;unsigned long long bc;}D;
static openfs_io_result_t r(void*c,uint64_t f,uint32_t n,void*x){D*d=c;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(x,d->b+(size_t)f*d->bs,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t w(void*c,uint64_t f,uint32_t n,const void*x){D*d=c;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)f*d->bs,x,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}
int main(void){D d={0};d.bs=4096U;d.bc=512U;d.b=calloc(d.bs,d.bc);assert(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,fl};uint8_t u[16]={0};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);uint64_t dir=0,file=0;assert(openfs_path_mkdir(&v,&s,"/d",&dir)==OPENFS_PATH_OK);openfs_acl_entry_t def[]={{OPENFS_ACL_USER_OBJ,0,7},{OPENFS_ACL_GROUP_OBJ,0,5},{OPENFS_ACL_OTHER,0,0},{OPENFS_ACL_MASK,0,5}};assert(openfs_acl_set(&v,&s,dir,def,4,1)==OPENFS_ACL_OK);assert(openfs_path_create_as(&v,&s,"/d/f",OPENFS_INODE_MODE_REGULAR|0600U,1000U,2000U,&file)==OPENFS_PATH_OK);assert(openfs_acl_inherit(&v,&s,dir,file,0600U,0)==OPENFS_ACL_OK);assert(openfs_acl_check_access(&v,&s,&(openfs_inode_t){0},0,0,0)==OPENFS_ACL_OK);uint64_t errors=0;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK);free(d.b);return 0;}
