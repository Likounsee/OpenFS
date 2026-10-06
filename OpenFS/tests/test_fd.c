#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/mount.h"
#include "openfs/fsck.h"
#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif
#include "openfs/fd.h"
#include "openfs/path.h"
#include "openfs/format.h"
typedef struct{uint8_t*b;uint32_t bs;uint64_t n;}disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*out){disk_t*d=c;if(f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(out,d->b+(size_t)(f*d->bs),(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*in){disk_t*d=c;if(f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),in,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}
typedef struct {openfs_block_device_t*dev;openfs_superblock_t*sb;unsigned id;unsigned failures;} create_ctx_t;
#if defined(_WIN32)
static unsigned __stdcall create_worker(void*arg)
#else
static void* create_worker(void*arg)
#endif
{
    create_ctx_t*c=(create_ctx_t*)arg;
    char path[96];
    for(unsigned n=0U;n<32U;n++){(void)snprintf(path,sizeof(path),"/concurrent-%u-%u",c->id,n);uint64_t ino=0U;if(openfs_path_create(c->dev,c->sb,path,OPENFS_INODE_MODE_REGULAR|0644U,&ino)!=OPENFS_PATH_OK)c->failures++;}
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}
static void concurrency_namespace_test(openfs_block_device_t*dev)
{
    openfs_mount_t m;assert(openfs_mount(&m,dev)==OPENFS_MOUNT_OK);
    create_ctx_t ctx[4];memset(ctx,0,sizeof(ctx));
#if defined(_WIN32)
    HANDLE threads[4];
    for(unsigned i=0U;i<4U;i++){ctx[i].dev=dev;ctx[i].sb=&m.superblock;ctx[i].id=i;uintptr_t h=_beginthreadex(NULL,0U,create_worker,&ctx[i],0U,NULL);assert(h!=0U);threads[i]=(HANDLE)h;}
    assert(WaitForMultipleObjects(4,threads,TRUE,60000U)==WAIT_OBJECT_0);
    for(unsigned i=0U;i<4U;i++)CloseHandle(threads[i]);
#else
    pthread_t threads[4];
    for(unsigned i=0U;i<4U;i++){ctx[i].dev=dev;ctx[i].sb=&m.superblock;ctx[i].id=i;assert(pthread_create(&threads[i],NULL,create_worker,&ctx[i])==0);}
    for(unsigned i=0U;i<4U;i++)assert(pthread_join(threads[i],NULL)==0);
#endif
    for(unsigned i=0U;i<4U;i++)assert(ctx[i].failures==0U);
    openfs_file_handle_t *h1=NULL,*h2=NULL;assert(openfs_fd_open(dev,&m.superblock,"/concurrent-0-0",OPENFS_FD_RDWR,0,&h1)==OPENFS_FD_OK);assert(openfs_fd_open(dev,&m.superblock,"/concurrent-0-0",OPENFS_FD_RDWR|OPENFS_FD_APPEND,0,&h2)==OPENFS_FD_OK);assert(openfs_fd_write(h1,"X",1U)==OPENFS_FD_OK);assert(openfs_fd_write(h2,"Y",1U)==OPENFS_FD_OK);int64_t p=0;char v[4]={0};size_t got2=0U;assert(openfs_fd_seek(h1,0,0,&p)==OPENFS_FD_OK);assert(openfs_fd_read(h1,v,sizeof(v),&got2)==OPENFS_FD_OK&&got2==2U&&memcmp(v,"XY",2U)==0);assert(openfs_fd_close(h1)==OPENFS_FD_OK&&openfs_fd_close(h2)==OPENFS_FD_OK);
    uint64_t errors=0U;assert(openfs_fsck(dev,&m.superblock,&errors)==OPENFS_FSCK_OK&&errors==0U);
    assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
}

int main(void){
 disk_t d={0};d.bs=4096U;d.n=1024U;d.b=calloc((size_t)d.n,d.bs);assert(d.b);
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
 concurrency_namespace_test(&dev);
 assert(openfs_fd_open(&dev,&sb,"/missing",OPENFS_FD_RDONLY,0,&r)==OPENFS_FD_NOT_FOUND);
 assert(openfs_fd_open(&dev,&sb,"/fd-test",OPENFS_FD_CREAT|OPENFS_FD_EXCL|OPENFS_FD_RDWR,0,&r)==OPENFS_FD_EXISTS);
 free(d.b);return 0;
}