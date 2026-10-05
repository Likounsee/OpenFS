#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "openfs/format.h"
#include "openfs/mount.h"
#include "openfs/fsck.h"
#include "openfs/inode.h"
#include "openfs/file.h"
#include "openfs/path.h"
#include "openfs/link.h"

typedef struct { uint8_t *b; uint32_t bs; uint64_t bc; } disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*o){disk_t*d=c;if(!d||!o||!n||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(o,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*i){disk_t*d=c;if(!d||!i||!n||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),(const uint8_t*)i,(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}

static void check_fsck(openfs_block_device_t*v,const openfs_superblock_t*s){
    uint64_t errors=0U;assert(openfs_fsck(v,s,&errors)==OPENFS_FSCK_OK);assert(errors==0U);
}
static void remount_and_check(openfs_block_device_t*v){
    openfs_mount_t m;assert(openfs_mount(&m,v)==OPENFS_MOUNT_OK);check_fsck(v,&m.superblock);assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
}

int main(void){
    disk_t d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,(size_t)d.bc);assert(d.b);
    openfs_block_device_t v={&d,d.bs,d.bc,rd,wr,fl};uint8_t uuid[16]={0x5AU};
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    for(unsigned i=0U;i<24U;i++){
        openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
        char dir[32],file[64],renamed[64],hard[64],sym[64],target[64];
        snprintf(dir,sizeof(dir),"/d%u",i);snprintf(file,sizeof(file),"%s/f",dir);snprintf(renamed,sizeof(renamed),"%s/r",dir);snprintf(hard,sizeof(hard),"%s/h",dir);snprintf(sym,sizeof(sym),"%s/s",dir);snprintf(target,sizeof(target),"%s/r",dir);
        uint64_t ino=0U;assert(openfs_path_mkdir(&v,&s,dir,&ino)==OPENFS_PATH_OK);
        openfs_path_result_t create_result=openfs_path_create(&v,&s,file,OPENFS_INODE_MODE_REGULAR|0644U,&ino);if(create_result!=OPENFS_PATH_OK){fprintf(stderr,"cycle %u file-create result=%d\\n",i,(int)create_result);check_fsck(&v,&s);}assert(create_result==OPENFS_PATH_OK);
        uint64_t inode_count=(s.inode_table_blocks*(uint64_t)s.block_size)/OPENFS_INODE_SIZE;openfs_inode_t node;assert(openfs_inode_read(&v,s.inode_table_start,ino,inode_count,&node)==OPENFS_INODE_OK);
        uint8_t wbuf[1024U],rbuf[1024U];for(size_t n=0U;n<sizeof(wbuf);n++)wbuf[n]=(uint8_t)(n+i);
        assert(openfs_file_write(&v,&s,&node,0U,wbuf,sizeof(wbuf))==OPENFS_FILE_OK);
        memset(rbuf,0,sizeof(rbuf));size_t got=0U;assert(openfs_file_read(&v,&s,&node,0U,rbuf,sizeof(rbuf),&got)==OPENFS_FILE_OK&&got==sizeof(rbuf)&&memcmp(wbuf,rbuf,sizeof(wbuf))==0);
        assert(openfs_file_truncate(&v,&s,&node,73U)==OPENFS_FILE_OK);assert(node.size==73U);
        assert(openfs_path_rename(&v,&s,file,renamed)==OPENFS_PATH_OK);
        assert(openfs_link(&v,&s,renamed,hard)==OPENFS_PATH_OK);
        assert(openfs_symlink(&v,&s,target,sym)==OPENFS_PATH_OK);
        char linkbuf[96];assert(openfs_readlink(&v,&s,sym,linkbuf,sizeof(linkbuf))==OPENFS_PATH_OK&&strcmp(linkbuf,target)==0);
        uint64_t resolved=0U;assert(openfs_path_lookup_follow(&v,&s,sym,&resolved)==OPENFS_PATH_OK&&resolved==ino);
        assert(openfs_path_unlink(&v,&s,sym)==OPENFS_PATH_OK);
        assert(openfs_path_unlink(&v,&s,hard)==OPENFS_PATH_OK);
        assert(openfs_path_unlink(&v,&s,renamed)==OPENFS_PATH_OK);
        assert(openfs_path_unlink(&v,&s,dir)==OPENFS_PATH_OK);
        if((i%4U)==3U)remount_and_check(&v);
    }
    remount_and_check(&v);
    free(d.b);return 0;
}
