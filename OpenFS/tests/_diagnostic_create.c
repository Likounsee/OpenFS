#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "openfs/format.h"
#include "openfs/mount.h"
#include "openfs/path.h"
typedef struct {uint8_t *b;uint32_t bs;uint64_t n;} D;
static openfs_io_result_t r(void*c,uint64_t f,uint32_t n,void*o){D*d=c;if(n==0U||f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(o,d->b+(size_t)(f*d->bs),(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t w(void*c,uint64_t f,uint32_t n,const void*i){D*d=c;if(n==0U||f>=d->n||(uint64_t)n>d->n-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),i,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t f(void*c){(void)c;return OPENFS_IO_OK;}
int main(void){D x={0};x.bs=4096U;x.n=128U;x.b=calloc((size_t)x.bs,x.n);openfs_block_device_t d={&x,x.bs,x.n,r,w,f};uint8_t u[16]={0x5a};openfs_mount_t m={0};if(openfs_format(&d,u)!=OPENFS_FORMAT_OK)return 2;if(openfs_mount(&m,&d)!=OPENFS_MOUNT_OK)return 3;uint64_t ino=0;openfs_path_result_t pr=openfs_path_create(&d,&m.superblock,"/probe",0100644U,&ino);printf("path_create=%d ino=%llu\n",(int)pr,(unsigned long long)ino);openfs_unmount(&m);free(x.b);return 0;}
