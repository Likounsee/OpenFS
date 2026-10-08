#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/format.h"
#include "openfs/mount.h"
#include "openfs/path.h"
#include "openfs/file.h"
#include "openfs/inode.h"
#include "openfs/fsck.h"
#include "openfs/cow.h"

typedef struct { uint8_t *b; uint32_t bs; uint64_t bc; } disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*o){disk_t*d=c;if(!d||!o||!n||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(o,d->b+(size_t)(f*d->bs),(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*i){disk_t*d=c;if(!d||!i||!n||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),i,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}
int main(void){disk_t d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);openfs_block_device_t dev={&d,d.bs,d.bc,rd,wr,fl};uint8_t uuid[16]={0x42};assert(openfs_format(&dev,uuid)==OPENFS_FORMAT_OK);openfs_mount_t m={0};assert(openfs_mount(&m,&dev)==OPENFS_MOUNT_OK);assert((m.superblock.feature_flags&OPENFS_FEATURE_COW)!=0U);uint64_t ino=0;assert(openfs_path_create(&dev,&m.superblock,"/f",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);uint64_t ic=(m.superblock.inode_table_blocks*(uint64_t)m.superblock.block_size)/OPENFS_INODE_SIZE;openfs_inode_t in;assert(openfs_inode_read(&dev,m.superblock.inode_table_start,ino,ic,&in)==OPENFS_INODE_OK);uint8_t data[4096];memset(data,0xA7,sizeof(data));assert(openfs_file_write(&dev,&m.superblock,&in,0,sizeof(data))==OPENFS_FILE_OK);assert(openfs_inode_read(&dev,m.superblock.inode_table_start,ino,ic,&in)==OPENFS_INODE_OK);openfs_extent_t e;assert(openfs_inode_get_extent(&in,0,&e)==OPENFS_EXTENT_OK);uint16_t refs=0;assert(openfs_cow_refcount_get(&dev,&m.superblock,e.physical_start,&refs)==OPENFS_COW_OK&&refs==1U);uint64_t idx=e.physical_start-m.superblock.data_start,epb=m.superblock.block_size/2U,tb=m.superblock.refcount_start+idx/epb;uint32_t off=(uint32_t)((idx%epb)*2U);uint8_t raw[4096];assert(dev.read(dev.context,tb,1,raw)==OPENFS_IO_OK);raw[off]=0;raw[off+1]=0;assert(dev.write(dev.context,tb,1,raw)==OPENFS_IO_OK);assert(dev.flush(dev.context)==OPENFS_IO_OK);uint64_t errors=0;assert(openfs_fsck(&dev,&m.superblock,&errors)==OPENFS_FSCK_CORRUPT&&errors>0);assert(openfs_fsck_repair_cow_refcounts(&dev,&m.superblock,&errors)==OPENFS_FSCK_OK&&errors==0);assert(openfs_cow_refcount_get(&dev,&m.superblock,e.physical_start,&refs)==OPENFS_COW_OK&&refs==1U);assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);free(d.b);return 0;}
