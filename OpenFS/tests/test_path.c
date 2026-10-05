#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/path.h"
#include "openfs/link.h"
#include "openfs/bitmap.h"
#include "openfs/fsck.h"
#include "openfs/file.h"
#include "openfs/mount.h"
#include <stdio.h>
typedef struct{uint8_t*b;uint32_t bs;uint64_t bc;uint64_t fail_read_block;uint64_t fail_write_block;uint64_t arm_block;openfs_superblock_t *mutate_sb_after_write;uint64_t mutate_after_write_block;uint64_t partial_write_block;uint32_t partial_write_bytes;int mutate_after_write;int partial_write_once;int fail_read_enabled;int fail_write_enabled;int arm_on_write;int armed;int fail_next_read;int fail_write_count;int fail_next_armed_write;int fail_flush;int fail_flush_once;uint32_t flush_calls;uint32_t fail_flush_call;}D;
static openfs_io_result_t r(void*c,uint64_t f,uint32_t n,void*x){D*d=c;if(d->fail_next_read&&d->armed){d->fail_next_read=0;return OPENFS_IO_IO_ERROR;}if(d->fail_read_enabled&&f==d->fail_read_block)return OPENFS_IO_IO_ERROR;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(x,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t w(void*c,uint64_t f,uint32_t n,const void*x){D*d=c;if(d->fail_write_enabled&&f==d->fail_write_block){if(d->fail_write_count>0){if(--d->fail_write_count==0)d->fail_write_enabled=0;}return OPENFS_IO_IO_ERROR;}if(d->arm_on_write&&f==d->arm_block){d->armed=1;}if(d->armed&&d->fail_next_armed_write){d->fail_next_armed_write=0;return OPENFS_IO_IO_ERROR;}if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;if(d->partial_write_once&&f==d->partial_write_block){uint32_t bytes=d->partial_write_bytes<d->bs?d->partial_write_bytes:d->bs;memcpy(d->b+(size_t)(f*d->bs),x,bytes);d->partial_write_once=0;return OPENFS_IO_IO_ERROR;}memcpy(d->b+(size_t)(f*d->bs),x,(size_t)((uint64_t)n*d->bs));if(d->mutate_after_write&&d->mutate_sb_after_write&&f==d->mutate_after_write_block){d->mutate_after_write=0;d->mutate_sb_after_write->inode_table_blocks=0U;}return OPENFS_IO_OK;}
static openfs_io_result_t f(void*c){D*d=c;d->flush_calls++;if(d->fail_flush_call!=0U&&d->flush_calls==d->fail_flush_call){d->fail_flush_call=0U;return OPENFS_IO_IO_ERROR;}if(d->fail_flush){if(d->fail_flush_once)d->fail_flush=0;return OPENFS_IO_IO_ERROR;}return OPENFS_IO_OK;}
static void create_as_growth_rollback(void)
{
 D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);
 openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={0};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);
 openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
 uint64_t parent=0U,ic=(s.inode_table_blocks*(uint64_t)s.block_size)/OPENFS_INODE_SIZE;
 assert(openfs_path_mkdir(&v,&s,"/grow",&parent)==OPENFS_PATH_OK);
 for(unsigned i=0U;i<16U;i++){char p[32];(void)snprintf(p,sizeof(p),"/grow/f%u",i);uint64_t ino=0U;assert(openfs_path_create(&v,&s,p,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);}
 openfs_inode_t before;assert(openfs_inode_read(&v,s.inode_table_start,parent,ic,&before)==OPENFS_INODE_OK);
 assert(before.size==16U*OPENFS_DIR_ENTRY_SIZE&&before.blocks==1U);
 uint64_t parent_block=0U;assert(openfs_file_map_block_device(&v,&s,&before,0U,&parent_block)==OPENFS_FILE_OK);uint64_t next_data=0U;for(uint64_t b=s.data_start;b<s.data_start+s.data_blocks;b++){int used=1;assert(openfs_bitmap_test(&v,s.block_bitmap_start,s.block_bitmap_blocks,b,&used)==OPENFS_BITMAP_OK);if(!used){next_data=b;break;}}assert(next_data!=0U);
 d.arm_on_write=1;d.arm_block=next_data;d.fail_next_armed_write=1;
 uint64_t ino=0U;openfs_path_result_t growth_result=openfs_path_create_as(&v,&s,"/grow/rollback",OPENFS_INODE_MODE_REGULAR,1000U,1000U,&ino);assert(growth_result==OPENFS_PATH_IO_ERROR);
 d.arm_on_write=0;d.armed=0;
 assert(openfs_path_lookup(&v,&s,"/grow/rollback",&ino)==OPENFS_PATH_NOT_FOUND);
 d.arm_on_write=1;d.arm_block=next_data;d.fail_next_armed_write=1;assert(openfs_path_mkdir_as(&v,&s,"/grow/mkdir-rollback",1000U,1000U,&ino)==OPENFS_PATH_IO_ERROR);d.arm_on_write=0;d.armed=0;assert(openfs_path_lookup(&v,&s,"/grow/mkdir-rollback",&ino)==OPENFS_PATH_NOT_FOUND);
 openfs_inode_t after;assert(openfs_inode_read(&v,s.inode_table_start,parent,ic,&after)==OPENFS_INODE_OK);
 assert(after.size==before.size&&after.blocks==before.blocks);
 uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);
 free(d.b);
}

static void long_unlink_path(void){
 D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={0};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
 char path[OPENFS_PATH_MAX];size_t used=1U;path[0]='/';path[1]='\0';for(unsigned level=0U;level<6U;level++){char name[201];memset(name,(int)('a'+level),200U);name[200]='\0';assert(used+201U<sizeof(path));memcpy(path+used,name,200U);used+=200U;path[used++]='/';path[used]='\0';uint64_t ino=0U;assert(openfs_path_mkdir(&v,&s,path,&ino)==OPENFS_PATH_OK);}path[used-1U]='x';path[used]='\0';uint64_t ino=0U;assert(openfs_path_create(&v,&s,path,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);assert(openfs_path_unlink(&v,&s,path)==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,path,&ino)==OPENFS_PATH_NOT_FOUND);free(d.b);
}
int main(void){create_as_growth_rollback();long_unlink_path();D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={0};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);uint64_t n=0,m=0,x=0,q=0;assert(openfs_path_create(&v,&s,"/home",OPENFS_INODE_MODE_DIRECTORY|0755U,&n)==OPENFS_PATH_OK);assert(openfs_path_create(&v,&s,"/home/invalid-mode",0x10000U,&x)==OPENFS_PATH_INVALID_ARGUMENT);for(unsigned i=0U;i<14U;i++){char filler[32];(void)snprintf(filler,sizeof(filler),"/home/f%u",i);uint64_t filler_ino=0U;assert(openfs_path_create(&v,&s,filler,OPENFS_INODE_MODE_REGULAR,&filler_ino)==OPENFS_PATH_OK);openfs_inode_t filler_inode;assert(openfs_inode_read(&v,s.inode_table_start,filler_ino,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&filler_inode)==OPENFS_INODE_OK);assert(openfs_file_truncate(&v,&s,&filler_inode,s.block_size)==OPENFS_FILE_OK);}{
    openfs_inode_t home_inode;
    assert(openfs_inode_read(&v,s.inode_table_start,n,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&home_inode)==OPENFS_INODE_OK);
    uint64_t home_block=0U;assert(openfs_file_map_block_device(&v,&s,&home_inode,0U,&home_block)==OPENFS_FILE_OK);
    d.arm_on_write=0;d.fail_write_block=home_block;d.fail_write_enabled=1;d.fail_write_count=1;
    uint64_t create_fail_ino=0U;assert(openfs_path_create(&v,&s,"/home/create-write-fail",OPENFS_INODE_MODE_REGULAR,&create_fail_ino)==OPENFS_PATH_IO_ERROR);
    d.fail_write_enabled=0;
    assert(openfs_path_lookup(&v,&s,"/home/create-write-fail",&x)==OPENFS_PATH_NOT_FOUND);
    {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
}
assert(openfs_path_mkdir(&v,&s,"/home/test",&m)==OPENFS_PATH_OK);d.fail_read_block=0U;d.fail_read_enabled=0;d.arm_on_write=1;d.arm_block=s.inode_table_start+1U;d.fail_next_read=1;uint64_t failed_ino=0U;assert(openfs_path_create(&v,&s,"/home/readfail",OPENFS_INODE_MODE_REGULAR,&failed_ino)==OPENFS_PATH_IO_ERROR);d.fail_read_enabled=0;assert(openfs_path_lookup(&v,&s,"/home/readfail",&x)==OPENFS_PATH_NOT_FOUND);{
    uint64_t late_validate_ino=0U;
    uint64_t original_inode_table_blocks=s.inode_table_blocks;
    assert(openfs_path_create(&v,&s,"/home/test/late-validate",OPENFS_INODE_MODE_REGULAR,&late_validate_ino)==OPENFS_PATH_OK);
    s.inode_table_blocks=0U;
    assert(openfs_path_unlink(&v,&s,"/home/test/late-validate")==OPENFS_PATH_CORRUPT);
    s.inode_table_blocks=original_inode_table_blocks;
    assert(openfs_path_lookup(&v,&s,"/home/test/late-validate",&q)==OPENFS_PATH_OK&&q==late_validate_ino);
    {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
}
assert(openfs_path_chmod(&v,&s,"/home/test",0755U)==OPENFS_PATH_OK);assert(openfs_path_rename(&v,&s,"/home/test","/home/test")==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/home/test",&q)==OPENFS_PATH_OK&&q==m);
{
    uint64_t src=0U,dst=0U;
    assert(openfs_path_create(&v,&s,"/home/test/replace-src",OPENFS_INODE_MODE_REGULAR,&src)==OPENFS_PATH_OK);
    assert(openfs_path_create(&v,&s,"/home/test/replace-dst",OPENFS_INODE_MODE_REGULAR,&dst)==OPENFS_PATH_OK);
    assert(openfs_path_rename(&v,&s,"/home/test/replace-src","/home/test/replace-dst")==OPENFS_PATH_OK);
    assert(openfs_path_lookup(&v,&s,"/home/test/replace-dst",&q)==OPENFS_PATH_OK&&q==src);
    assert(openfs_path_lookup(&v,&s,"/home/test/replace-src",&q)==OPENFS_PATH_NOT_FOUND);
    openfs_inode_t replaced_dst;assert(openfs_inode_read(&v,s.inode_table_start,dst,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&replaced_dst)==OPENFS_INODE_OK);
    assert(replaced_dst.mode==OPENFS_INODE_MODE_FREE&&replaced_dst.link_count==0U);
    {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
}
{
    uint64_t a=0U,b=0U,q=0U;
    assert(openfs_path_create(&v,&s,"/home/test/r-file-symlink-src",OPENFS_INODE_MODE_REGULAR,&a)==OPENFS_PATH_OK);
    assert(openfs_symlink(&v,&s,"/home/test","/home/test/r-file-symlink-dst")==OPENFS_PATH_OK);
    assert(openfs_path_rename(&v,&s,"/home/test/r-file-symlink-src","/home/test/r-file-symlink-dst")==OPENFS_PATH_OK);
    assert(openfs_path_lookup(&v,&s,"/home/test/r-file-symlink-dst",&q)==OPENFS_PATH_OK&&q==a);
    assert(openfs_path_lookup(&v,&s,"/home/test/r-file-symlink-src",&q)==OPENFS_PATH_NOT_FOUND);
    assert(openfs_path_create(&v,&s,"/home/test/r-symlink-file-dst",OPENFS_INODE_MODE_REGULAR,&b)==OPENFS_PATH_OK);
    assert(openfs_symlink(&v,&s,"/home/test","/home/test/r-symlink-file-src")==OPENFS_PATH_OK);
    assert(openfs_path_rename(&v,&s,"/home/test/r-symlink-file-src","/home/test/r-symlink-file-dst")==OPENFS_PATH_OK);
    assert(openfs_path_lookup(&v,&s,"/home/test/r-symlink-file-dst",&q)==OPENFS_PATH_OK&&q!=b);
    assert(openfs_symlink(&v,&s,"/home/test","/home/test/r-symlink-symlink-src")==OPENFS_PATH_OK);
    assert(openfs_symlink(&v,&s,"/home","/home/test/r-symlink-symlink-dst")==OPENFS_PATH_OK);
    assert(openfs_path_rename(&v,&s,"/home/test/r-symlink-symlink-src","/home/test/r-symlink-symlink-dst")==OPENFS_PATH_OK);
    assert(openfs_path_create(&v,&s,"/home/test/r-hard-src",OPENFS_INODE_MODE_REGULAR,&a)==OPENFS_PATH_OK);
    assert(openfs_link(&v,&s,"/home/test/r-hard-src","/home/test/r-hard-alias")==OPENFS_PATH_OK);
    assert(openfs_path_create(&v,&s,"/home/test/r-hard-dst",OPENFS_INODE_MODE_REGULAR,&b)==OPENFS_PATH_OK);
    {openfs_inode_t hi;assert(openfs_inode_read(&v,s.inode_table_start,a,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&hi)==OPENFS_INODE_OK&&hi.link_count==2U);
     assert(openfs_path_rename(&v,&s,"/home/test/r-hard-src","/home/test/r-hard-dst")==OPENFS_PATH_OK);
     assert(openfs_path_lookup(&v,&s,"/home/test/r-hard-dst",&q)==OPENFS_PATH_OK&&q==a);
     assert(openfs_path_lookup(&v,&s,"/home/test/r-hard-src",&q)==OPENFS_PATH_NOT_FOUND);
     assert(openfs_path_lookup(&v,&s,"/home/test/r-hard-alias",&q)==OPENFS_PATH_OK&&q==a);
     assert(openfs_inode_read(&v,s.inode_table_start,a,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&hi)==OPENFS_INODE_OK&&hi.link_count==2U);}
    assert(openfs_path_mkdir(&v,&s,"/home/test/r-dir-src",&a)==OPENFS_PATH_OK);
    assert(openfs_path_mkdir(&v,&s,"/home/test/r-dir-dst",&b)==OPENFS_PATH_OK);
    assert(openfs_path_rename(&v,&s,"/home/test/r-dir-src","/home/test/r-dir-dst")==OPENFS_PATH_OK);
    assert(openfs_path_lookup(&v,&s,"/home/test/r-dir-dst",&q)==OPENFS_PATH_OK&&q==a);
    assert(openfs_path_lookup(&v,&s,"/home/test/r-dir-src",&q)==OPENFS_PATH_NOT_FOUND);
    assert(openfs_path_mkdir(&v,&s,"/home/test/r-dir-nonempty",&b)==OPENFS_PATH_OK);
    assert(openfs_path_create(&v,&s,"/home/test/r-dir-nonempty/file",OPENFS_INODE_MODE_REGULAR,&q)==OPENFS_PATH_OK);
    assert(openfs_path_mkdir(&v,&s,"/home/test/r-dir-src2",&a)==OPENFS_PATH_OK);
    assert(openfs_path_rename(&v,&s,"/home/test/r-dir-src2","/home/test/r-dir-nonempty")==OPENFS_PATH_NOT_EMPTY);
    assert(openfs_path_rename(&v,&s,"/home/test/r-dir-src2","/home/test/r-hard-alias")==OPENFS_PATH_NOT_DIRECTORY);
    assert(openfs_path_rename(&v,&s,"/home/test/r-file-symlink-dst","/home/test/r-dir-src2")==OPENFS_PATH_NOT_DIRECTORY);
    assert(openfs_path_mkdir(&v,&s,"/home/r-replace-parent",&a)==OPENFS_PATH_OK);
    assert(openfs_path_mkdir(&v,&s,"/home/r-replace-parent2",&b)==OPENFS_PATH_OK);
    assert(openfs_path_create(&v,&s,"/home/r-replace-parent/src",OPENFS_INODE_MODE_REGULAR,&a)==OPENFS_PATH_OK);
    assert(openfs_path_create(&v,&s,"/home/r-replace-parent2/dst",OPENFS_INODE_MODE_REGULAR,&b)==OPENFS_PATH_OK);
    assert(openfs_path_rename(&v,&s,"/home/r-replace-parent/src","/home/r-replace-parent2/dst")==OPENFS_PATH_OK);
    assert(openfs_path_lookup(&v,&s,"/home/r-replace-parent/src",&q)==OPENFS_PATH_NOT_FOUND);
    assert(openfs_path_lookup(&v,&s,"/home/r-replace-parent2/dst",&q)==OPENFS_PATH_OK&&q==a);
    {openfs_mount_t remount;assert(openfs_mount(&remount,&v)==OPENFS_MOUNT_OK);assert(openfs_path_lookup(&v,&remount.superblock,"/home/r-replace-parent2/dst",&q)==OPENFS_PATH_OK&&q==a);uint64_t errors=0U;assert(openfs_fsck(&v,&remount.superblock,&errors)==OPENFS_FSCK_OK&&errors==0U);assert(openfs_unmount(&remount)==OPENFS_MOUNT_OK);}
}
{
    uint64_t src=0U,dst=0U;assert(openfs_path_create(&v,&s,"/home/test/rb-src",OPENFS_INODE_MODE_REGULAR,&src)==OPENFS_PATH_OK);assert(openfs_path_create(&v,&s,"/home/test/rb-dst",OPENFS_INODE_MODE_REGULAR,&dst)==OPENFS_PATH_OK);
    openfs_inode_t di;assert(openfs_inode_read(&v,s.inode_table_start,dst,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&di)==OPENFS_INODE_OK);uint8_t payload[4096];memset(payload,0x5AU,sizeof(payload));assert(openfs_file_write(&v,&s,&di,0U,payload,sizeof(payload))==OPENFS_FILE_OK);
    assert(openfs_inode_read(&v,s.inode_table_start,dst,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&di)==OPENFS_INODE_OK);uint64_t db=0U;assert(openfs_file_map_block_device(&v,&s,&di,0U,&db)==OPENFS_FILE_OK);
    uint64_t parent_ino=0U;assert(openfs_path_lookup(&v,&s,"/home/test",&parent_ino)==OPENFS_PATH_OK);openfs_inode_t parent;assert(openfs_inode_read(&v,s.inode_table_start,parent_ino,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&parent)==OPENFS_INODE_OK);uint64_t pb=0U;assert(openfs_file_map_block_device(&v,&s,&parent,0U,&pb)==OPENFS_FILE_OK);
    uint64_t dib=s.inode_table_start+((dst-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;uint8_t *dbefore=malloc(s.block_size),*dafter=malloc(s.block_size),*pbefore=malloc(s.block_size),*pafter=malloc(s.block_size),*ibefore=malloc(s.block_size),*iafter=malloc(s.block_size);assert(dbefore&&dafter&&pbefore&&pafter&&ibefore&&iafter);
    assert(v.read(v.context,db,1U,dbefore)==OPENFS_IO_OK);assert(v.read(v.context,pb,1U,pbefore)==OPENFS_IO_OK);assert(v.read(v.context,dib,1U,ibefore)==OPENFS_IO_OK);
    size_t bb=(size_t)(s.block_bitmap_blocks*s.block_size),ib=(size_t)(s.inode_bitmap_blocks*s.block_size);uint8_t *bbefore=malloc(bb),*bafter=malloc(bb),*ibitmap_before=malloc(ib),*ibitmap_after=malloc(ib);assert(bbefore&&bafter&&ibitmap_before&&ibitmap_after);memcpy(bbefore,d.b+(size_t)(s.block_bitmap_start*s.block_size),bb);memcpy(ibitmap_before,d.b+(size_t)(s.inode_bitmap_start*s.block_size),ib);
    d.fail_write_block=dib;d.fail_write_enabled=1;d.fail_write_count=1;assert(openfs_path_rename(&v,&s,"/home/test/rb-src","/home/test/rb-dst")==OPENFS_PATH_IO_ERROR);d.fail_write_enabled=0;
    assert(openfs_path_lookup(&v,&s,"/home/test/rb-src",&q)==OPENFS_PATH_OK&&q==src);assert(openfs_path_lookup(&v,&s,"/home/test/rb-dst",&q)==OPENFS_PATH_OK&&q==dst);
    assert(v.read(v.context,db,1U,dafter)==OPENFS_IO_OK&&memcmp(dbefore,dafter,s.block_size)==0);assert(v.read(v.context,pb,1U,pafter)==OPENFS_IO_OK&&memcmp(pbefore,pafter,s.block_size)==0);assert(v.read(v.context,dib,1U,iafter)==OPENFS_IO_OK&&memcmp(ibefore,iafter,s.block_size)==0);
    memcpy(bafter,d.b+(size_t)(s.block_bitmap_start*s.block_size),bb);memcpy(ibitmap_after,d.b+(size_t)(s.inode_bitmap_start*s.block_size),ib);assert(memcmp(bbefore,bafter,bb)==0&&memcmp(ibitmap_before,ibitmap_after,ib)==0);
    {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
    free(dbefore);free(dafter);free(pbefore);free(pafter);free(ibefore);free(iafter);free(bbefore);free(bafter);free(ibitmap_before);free(ibitmap_after);
}
assert(openfs_path_rename(&v,&s,"/home/test","/home/renamed")==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/home/renamed",&m)==OPENFS_PATH_OK);{
    openfs_inode_t rename_parent_before,rename_parent_after;
    assert(openfs_inode_read(&v,s.inode_table_start,n,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&rename_parent_before)==OPENFS_INODE_OK);
    uint64_t target_block=s.inode_table_start+((m-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
    d.fail_write_block=target_block; d.fail_write_enabled=1; d.fail_write_count=1;
    assert(openfs_path_rename(&v,&s,"/home/renamed","/home/failed")==OPENFS_PATH_IO_ERROR);
    d.fail_write_enabled=0;
    assert(openfs_path_lookup(&v,&s,"/home/renamed",&q)==OPENFS_PATH_OK&&q==m);
    assert(openfs_path_lookup(&v,&s,"/home/failed",&q)==OPENFS_PATH_NOT_FOUND);
    assert(openfs_inode_read(&v,s.inode_table_start,n,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&rename_parent_after)==OPENFS_INODE_OK);
    assert(rename_parent_after.mtime_ns==rename_parent_before.mtime_ns&&rename_parent_after.ctime_ns==rename_parent_before.ctime_ns);
}
{
    openfs_inode_t rename_parent_before,rename_parent_after;
    assert(openfs_inode_read(&v,s.inode_table_start,n,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&rename_parent_before)==OPENFS_INODE_OK);
    d.fail_flush=1;d.fail_flush_once=1;
    assert(openfs_path_rename(&v,&s,"/home/renamed","/home/rename-flush-fail")==OPENFS_PATH_IO_ERROR);
    assert(openfs_path_lookup(&v,&s,"/home/renamed",&q)==OPENFS_PATH_OK&&q==m);
    assert(openfs_path_lookup(&v,&s,"/home/rename-flush-fail",&q)==OPENFS_PATH_NOT_FOUND);
    assert(openfs_inode_read(&v,s.inode_table_start,n,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&rename_parent_after)==OPENFS_INODE_OK);
    assert(rename_parent_after.mtime_ns==rename_parent_before.mtime_ns&&rename_parent_after.ctime_ns==rename_parent_before.ctime_ns);
}{uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
assert(openfs_path_rename(&v,&s,"/home/renamed","/home/test")==OPENFS_PATH_OK);
assert(openfs_path_mkdir(&v,&s,"/home/other",&q)==OPENFS_PATH_OK);
assert(openfs_path_create(&v,&s,"/home/test/cross-file",OPENFS_INODE_MODE_REGULAR,&q)==OPENFS_PATH_OK);
assert(openfs_path_rename(&v,&s,"/home/test/cross-file","/home/other/cross-file")==OPENFS_PATH_OK);
assert(openfs_path_lookup(&v,&s,"/home/other/cross-file",&q)==OPENFS_PATH_OK);
assert(openfs_path_lookup(&v,&s,"/home/test/cross-file",&q)==OPENFS_PATH_NOT_FOUND);
assert(openfs_path_rename(&v,&s,"/home/other/cross-file","/home/test/cross-file")==OPENFS_PATH_OK);
assert(openfs_path_mkdir(&v,&s,"/home/test/movedir",&q)==OPENFS_PATH_OK);
assert(openfs_path_create(&v,&s,"/home/test/movedir/file",OPENFS_INODE_MODE_REGULAR,&x)==OPENFS_PATH_OK);
assert(openfs_path_rename(&v,&s,"/home/test/movedir","/home/other/movedir")==OPENFS_PATH_OK);
assert(openfs_path_lookup(&v,&s,"/home/other/movedir/file",&q)==OPENFS_PATH_OK&&q==x);
assert(openfs_path_rename(&v,&s,"/home/other/movedir","/home/other/movedir/sub")==OPENFS_PATH_INVALID_ARGUMENT);
assert(openfs_path_rename(&v,&s,"/home/other/movedir","/home/test/movedir")==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/..",&x)==OPENFS_PATH_OK&&x==s.root_inode);assert(openfs_path_lookup(&v,&s,"///home//test//",&x)==OPENFS_PATH_OK&&x==m);assert(openfs_path_lookup(&v,&s,"/home/./test/../test",&x)==OPENFS_PATH_OK&&x==m);assert(openfs_path_create(&v,&s,"/home/test/file",OPENFS_INODE_MODE_REGULAR,&x)==OPENFS_PATH_OK);assert(openfs_path_mkdir(&v,&s,"/home/test/symlink-parent",&q)==OPENFS_PATH_OK);assert(openfs_path_create(&v,&s,"/home/test/symlink-parent/file",OPENFS_INODE_MODE_REGULAR,&x)==OPENFS_PATH_OK);assert(openfs_symlink(&v,&s,"symlink-parent","/home/test/symlink-parent-link")==OPENFS_PATH_OK);assert(openfs_path_unlink(&v,&s,"/home/test/symlink-parent-link/file")==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/home/test/symlink-parent/file",&q)==OPENFS_PATH_NOT_FOUND);assert(openfs_path_mkdir(&v,&s,"/home/test/nonempty",&q)==OPENFS_PATH_OK);assert(openfs_path_create(&v,&s,"/home/test/nonempty/file",OPENFS_INODE_MODE_REGULAR,&q)==OPENFS_PATH_OK);assert(openfs_path_unlink(&v,&s,"/home/test/nonempty")==OPENFS_PATH_NOT_EMPTY);assert(openfs_path_unlink(&v,&s,"/home/test/nonempty/file")==OPENFS_PATH_OK);assert(openfs_path_unlink(&v,&s,"/home/test/nonempty")==OPENFS_PATH_OK);
assert(openfs_path_chmod(&v,&s,"/home/test",0777U)==OPENFS_PATH_OK);
assert(openfs_path_create_as(&v,&s,"/home/test/asuser",OPENFS_INODE_MODE_REGULAR,1000U,1000U,&q)==OPENFS_PATH_OK);
{openfs_inode_t as_i;assert(openfs_inode_read(&v,s.inode_table_start,q,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&as_i)==OPENFS_INODE_OK);assert(as_i.uid==1000U&&as_i.gid==1000U);}
assert(openfs_path_unlink_as(&v,&s,"/home/test/asuser",1000U,1000U)==OPENFS_PATH_OK);
{
    uint64_t inode_count_value=(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE;
    uint64_t candidate=0U;
    for(uint64_t n=2U;n<=inode_count_value;n++){
        int used=0;
        assert(openfs_bitmap_test(&v,s.inode_bitmap_start,s.inode_bitmap_blocks,n-1U,&used)==OPENFS_BITMAP_OK);
        if(!used){candidate=n;break;}
    }
    assert(candidate!=0U);
    uint64_t inode_block=s.inode_table_start+((candidate-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
    d.fail_write_block=inode_block;d.fail_write_enabled=1;d.fail_write_count=2;
    assert(openfs_path_create_as(&v,&s,"/home/test/as-create-write-fail",OPENFS_INODE_MODE_REGULAR,1000U,1000U,&q)==OPENFS_PATH_IO_ERROR);
    d.fail_write_enabled=0;
    assert(openfs_path_lookup(&v,&s,"/home/test/as-create-write-fail",&x)==OPENFS_PATH_NOT_FOUND);
    {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
}
{
    uint64_t flush_failure=d.flush_calls+3U;
    d.fail_flush_call=(uint32_t)flush_failure;
    assert(openfs_path_mkdir_as(&v,&s,"/home/test/as-mkdir-flush-fail",1000U,1000U,&q)==OPENFS_PATH_IO_ERROR);
    assert(openfs_path_lookup(&v,&s,"/home/test/as-mkdir-flush-fail",&x)==OPENFS_PATH_NOT_FOUND);
    {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
}
assert(openfs_path_create_as(&v,&s,"/home/test/denied",OPENFS_INODE_MODE_REGULAR,1000U,1000U,&q)==OPENFS_PATH_OK);
assert(openfs_path_chmod(&v,&s,"/home/test",0555U)==OPENFS_PATH_OK);
assert(openfs_path_unlink_as(&v,&s,"/home/test/denied",1000U,1000U)==OPENFS_PATH_ACCESS_DENIED);
assert(openfs_path_create_as(&v,&s,"/home/test/blocked",OPENFS_INODE_MODE_REGULAR,1000U,1000U,&q)==OPENFS_PATH_ACCESS_DENIED);assert(openfs_path_mkdir(&v,&s,"/home/private",&q)==OPENFS_PATH_OK);assert(openfs_path_chmod(&v,&s,"/home/private",0700U)==OPENFS_PATH_OK);assert(openfs_path_create_as(&v,&s,"/home/private/hidden",OPENFS_INODE_MODE_REGULAR,1000U,1000U,&q)==OPENFS_PATH_ACCESS_DENIED);
assert(openfs_path_chmod_as(&v,&s,"/home/test/file",0600U,1000U,1000U)==OPENFS_PATH_ACCESS_DENIED);
assert(openfs_path_chmod_as(&v,&s,"/home/test/file",0600U,0U,0U)==OPENFS_PATH_OK);
assert(openfs_path_set_times_as(&v,&s,"/home/test/file",1000U,1000U,33U,44U)==OPENFS_PATH_ACCESS_DENIED);
assert(openfs_path_set_times_as(&v,&s,"/home/test/file",0U,0U,33U,44U)==OPENFS_PATH_OK);assert(openfs_path_unlink(&v,&s,"/home/f0")==OPENFS_PATH_OK);assert(openfs_path_mkdir(&v,&s,"/home/sticky",&q)==OPENFS_PATH_OK);assert(openfs_path_chmod(&v,&s,"/home/sticky",01777U)==OPENFS_PATH_OK);assert(openfs_path_create(&v,&s,"/home/sticky/file",OPENFS_INODE_MODE_REGULAR,&q)==OPENFS_PATH_OK);{openfs_inode_t si;assert(openfs_inode_read(&v,s.inode_table_start,q,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&si)==OPENFS_INODE_OK);si.uid=2000U;assert(openfs_inode_write(&v,s.inode_table_start,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&si)==OPENFS_INODE_OK);}assert(openfs_path_unlink_as(&v,&s,"/home/sticky/file",1000U,1000U)==OPENFS_PATH_ACCESS_DENIED);assert(openfs_path_unlink_as(&v,&s,"/home/sticky/file",2000U,2000U)==OPENFS_PATH_OK);
assert(openfs_path_chmod(&v,&s,"/home/test",0777U)==OPENFS_PATH_OK);assert(openfs_path_chmod(&v,&s,"/home/test/file",0644U)==OPENFS_PATH_OK);assert(openfs_path_check_access(&v,&s,"/home/test/file",0U,0U,4U)==OPENFS_PATH_OK);assert(openfs_path_check_access(&v,&s,"/home/test/file",1U,1U,2U)==OPENFS_PATH_ACCESS_DENIED);assert(openfs_path_check_access(&v,&s,"/home/test/file",0U,0U,8U)==OPENFS_PATH_INVALID_ARGUMENT);assert(openfs_path_check_access(&v,&s,"/home/test/file",1000U,1000U,1U)==OPENFS_PATH_ACCESS_DENIED);assert(openfs_path_set_times(&v,&s,"/home/test/file",11U,22U)==OPENFS_PATH_OK);openfs_inode_t fi;uint64_t ic=(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE;assert(openfs_inode_read(&v,s.inode_table_start,x,ic,&fi)==OPENFS_INODE_OK&&fi.mode==(OPENFS_INODE_MODE_REGULAR|0644U)&&fi.atime_ns==11U&&fi.mtime_ns==22U);assert(openfs_path_lookup(&v,&s,"/home/test/file",&q)==OPENFS_PATH_OK&&q==x);{
    openfs_inode_t before_link, before_link_parent;
    assert(openfs_inode_read(&v,s.inode_table_start,x,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&before_link)==OPENFS_INODE_OK);
    assert(openfs_inode_read(&v,s.inode_table_start,m,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&before_link_parent)==OPENFS_INODE_OK);
    uint64_t target_block=s.inode_table_start+((x-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
    d.fail_write_block=target_block; d.fail_write_enabled=1; d.fail_write_count=1;
    assert(openfs_link(&v,&s,"/home/test/file","/home/test/link-fail")==OPENFS_PATH_IO_ERROR);
    d.fail_write_enabled=0;
    assert(openfs_path_lookup(&v,&s,"/home/test/link-fail",&q)==OPENFS_PATH_NOT_FOUND);
    openfs_inode_t after_link;
    assert(openfs_inode_read(&v,s.inode_table_start,x,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&after_link)==OPENFS_INODE_OK);
    assert(after_link.link_count==before_link.link_count);
    openfs_inode_t after_link_parent;
    assert(openfs_inode_read(&v,s.inode_table_start,m,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&after_link_parent)==OPENFS_INODE_OK);
    assert(after_link_parent.mtime_ns==before_link_parent.mtime_ns&&after_link_parent.ctime_ns==before_link_parent.ctime_ns);{
    uint64_t unlink_fail_ino=0U;
    uint64_t unlink_parent_block=s.inode_table_start+((m-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
    unsigned unlink_filler=0U;char unlink_path[64];
    for(;;){
        (void)snprintf(unlink_path,sizeof(unlink_path),"/home/test/unlink-fail-%u",unlink_filler++);
        assert(openfs_path_create(&v,&s,unlink_path,OPENFS_INODE_MODE_REGULAR,&unlink_fail_ino)==OPENFS_PATH_OK);
        uint64_t unlink_target_block=s.inode_table_start+((unlink_fail_ino-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
        if(unlink_target_block!=unlink_parent_block)break;
    }
    openfs_inode_t unlink_fail_inode, unlink_fail_parent_before;
    assert(openfs_inode_read(&v,s.inode_table_start,unlink_fail_ino,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&unlink_fail_inode)==OPENFS_INODE_OK);
    assert(openfs_inode_read(&v,s.inode_table_start,m,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&unlink_fail_parent_before)==OPENFS_INODE_OK);
    uint8_t unlink_payload[4096];memset(unlink_payload,0x6BU,sizeof(unlink_payload));
    assert(openfs_file_write(&v,&s,&unlink_fail_inode,0U,unlink_payload,sizeof(unlink_payload))==OPENFS_FILE_OK);
    uint64_t target_block=s.inode_table_start+((unlink_fail_ino-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
    d.fail_write_block=target_block; d.fail_write_enabled=1; d.fail_write_count=1;
    assert(openfs_path_unlink(&v,&s,unlink_path)==OPENFS_PATH_IO_ERROR);
    d.fail_write_enabled=0;
    assert(openfs_path_lookup(&v,&s,unlink_path,&q)==OPENFS_PATH_OK&&q==unlink_fail_ino);
    {openfs_inode_t restored;uint8_t readback[4096];size_t got=0U;
        assert(openfs_inode_read(&v,s.inode_table_start,unlink_fail_ino,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&restored)==OPENFS_INODE_OK);
        assert(restored.size==sizeof(unlink_payload)&&restored.blocks==1U);
        assert(openfs_file_read(&v,&s,&restored,0U,readback,sizeof(readback),&got)==OPENFS_FILE_OK&&got==sizeof(readback)&&memcmp(readback,unlink_payload,sizeof(readback))==0);
    }
    {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
    {openfs_inode_t unlink_fail_parent_after;assert(openfs_inode_read(&v,s.inode_table_start,m,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&unlink_fail_parent_after)==OPENFS_INODE_OK);assert(unlink_fail_parent_after.mtime_ns==unlink_fail_parent_before.mtime_ns&&unlink_fail_parent_after.ctime_ns==unlink_fail_parent_before.ctime_ns);}
}
{
    uint64_t rold=0U,rnew=0U,rsrc=0U,rdst=0U;
    assert(openfs_path_mkdir(&v,&s,"/home/test/rename-partial-old",&rold)==OPENFS_PATH_OK);
    assert(openfs_path_mkdir(&v,&s,"/home/test/rename-partial-new",&rnew)==OPENFS_PATH_OK);
    for(unsigned i=0U;i<16U;i++){char p[96];(void)snprintf(p,sizeof(p),"/home/test/rename-partial-old/f%u",i);uint64_t ino=0U;assert(openfs_path_create(&v,&s,p,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);}
    assert(openfs_path_create(&v,&s,"/home/test/rename-partial-old/src",OPENFS_INODE_MODE_REGULAR,&rsrc)==OPENFS_PATH_OK);
    assert(openfs_path_create(&v,&s,"/home/test/rename-partial-new/dst",OPENFS_INODE_MODE_REGULAR,&rdst)==OPENFS_PATH_OK);
    openfs_inode_t oi,ni;assert(openfs_inode_read(&v,s.inode_table_start,rold,ic,&oi)==OPENFS_INODE_OK);assert(openfs_inode_read(&v,s.inode_table_start,rnew,ic,&ni)==OPENFS_INODE_OK);
    uint64_t ob=0U,nb=0U;assert(openfs_file_map_block_device(&v,&s,&oi,1U,&ob)==OPENFS_FILE_OK);assert(openfs_file_map_block_device(&v,&s,&ni,0U,&nb)==OPENFS_FILE_OK);
    uint64_t old_inode_block=s.inode_table_start+((rsrc-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
    uint64_t new_inode_block=s.inode_table_start+((rdst-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
    size_t block_bitmap_bytes=(size_t)(s.block_bitmap_blocks*s.block_size),inode_bitmap_bytes=(size_t)(s.inode_bitmap_blocks*s.block_size);
    uint8_t *obefore=malloc(s.block_size),*oafter=malloc(s.block_size),*nbefore=malloc(s.block_size),*nafter=malloc(s.block_size),*old_ibefore=malloc(s.block_size),*old_iafter=malloc(s.block_size),*new_ibefore=malloc(s.block_size),*new_iafter=malloc(s.block_size),*bbefore=malloc(block_bitmap_bytes),*bafter=malloc(block_bitmap_bytes),*ibitmap_before=malloc(inode_bitmap_bytes),*ibitmap_after=malloc(inode_bitmap_bytes);
    assert(obefore&&oafter&&nbefore&&nafter&&old_ibefore&&old_iafter&&new_ibefore&&new_iafter&&bbefore&&bafter&&ibitmap_before&&ibitmap_after);
    assert(v.read(v.context,ob,1U,obefore)==OPENFS_IO_OK);assert(v.read(v.context,nb,1U,nbefore)==OPENFS_IO_OK);
    assert(v.read(v.context,old_inode_block,1U,old_ibefore)==OPENFS_IO_OK);assert(v.read(v.context,new_inode_block,1U,new_ibefore)==OPENFS_IO_OK);
    memcpy(bbefore,d.b+(size_t)(s.block_bitmap_start*s.block_size),block_bitmap_bytes);
    memcpy(ibitmap_before,d.b+(size_t)(s.inode_bitmap_start*s.block_size),inode_bitmap_bytes);
    d.partial_write_block=ob;d.partial_write_bytes=17U;d.partial_write_once=1;
    assert(openfs_path_rename(&v,&s,"/home/test/rename-partial-old/src","/home/test/rename-partial-new/dst")==OPENFS_PATH_IO_ERROR);
    assert(v.read(v.context,ob,1U,oafter)==OPENFS_IO_OK&&memcmp(obefore,oafter,s.block_size)==0);
    assert(v.read(v.context,nb,1U,nafter)==OPENFS_IO_OK&&memcmp(nbefore,nafter,s.block_size)==0);
    assert(v.read(v.context,old_inode_block,1U,old_iafter)==OPENFS_IO_OK&&memcmp(old_ibefore,old_iafter,s.block_size)==0);
    assert(v.read(v.context,new_inode_block,1U,new_iafter)==OPENFS_IO_OK&&memcmp(new_ibefore,new_iafter,s.block_size)==0);
    memcpy(bafter,d.b+(size_t)(s.block_bitmap_start*s.block_size),block_bitmap_bytes);
    memcpy(ibitmap_after,d.b+(size_t)(s.inode_bitmap_start*s.block_size),inode_bitmap_bytes);
    assert(memcmp(bbefore,bafter,block_bitmap_bytes)==0&&memcmp(ibitmap_before,ibitmap_after,inode_bitmap_bytes)==0);
    assert(openfs_path_lookup(&v,&s,"/home/test/rename-partial-old/src",&q)==OPENFS_PATH_OK&&q==rsrc);
    assert(openfs_path_lookup(&v,&s,"/home/test/rename-partial-new/dst",&q)==OPENFS_PATH_OK&&q==rdst);
    {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
    {openfs_mount_t remount;assert(openfs_mount(&remount,&v)==OPENFS_MOUNT_OK);assert(openfs_path_lookup(&v,&remount.superblock,"/home/test/rename-partial-old/src",&q)==OPENFS_PATH_OK&&q==rsrc);assert(openfs_path_lookup(&v,&remount.superblock,"/home/test/rename-partial-new/dst",&q)==OPENFS_PATH_OK&&q==rdst);uint64_t errors=0U;assert(openfs_fsck(&v,&remount.superblock,&errors)==OPENFS_FSCK_OK&&errors==0U);assert(openfs_unmount(&remount)==OPENFS_MOUNT_OK);}
    free(obefore);free(oafter);free(nbefore);free(nafter);free(old_ibefore);free(old_iafter);free(new_ibefore);free(new_iafter);free(bbefore);free(bafter);free(ibitmap_before);free(ibitmap_after);
}
{
    uint64_t rsrc=0U;
    for(unsigned i=0U;i<32U;i++){char p[96];(void)snprintf(p,sizeof(p),"/home/test/rename-rollback-filler-%u",i);uint64_t ino=0U;assert(openfs_path_create(&v,&s,p,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);}
    assert(openfs_path_mkdir(&v,&s,"/home/test/rename-rollback-dir",&rsrc)==OPENFS_PATH_OK);
    uint64_t parent_for_rollback=0U;assert(openfs_path_lookup(&v,&s,"/home/test",&parent_for_rollback)==OPENFS_PATH_OK);
    uint64_t source_inode_block=s.inode_table_start+((rsrc-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
    uint64_t parent_inode_block=s.inode_table_start+((parent_for_rollback-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
    assert(source_inode_block!=parent_inode_block);
    d.fail_write_block=source_inode_block;d.fail_write_enabled=1;d.fail_write_count=2;
    assert(openfs_path_rename(&v,&s,"/home/test/rename-rollback-dir","/home/test/rename-rollback-dir-moved")==OPENFS_PATH_CORRUPT);
    d.fail_write_enabled=0;
    assert(openfs_path_lookup(&v,&s,"/home/test/rename-rollback-dir",&q)==OPENFS_PATH_OK&&q==rsrc);
    assert(openfs_path_lookup(&v,&s,"/home/test/rename-rollback-dir-moved",&q)==OPENFS_PATH_NOT_FOUND);
    {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
    {openfs_mount_t remount;assert(openfs_mount(&remount,&v)==OPENFS_MOUNT_OK);assert(openfs_path_lookup(&v,&remount.superblock,"/home/test/rename-rollback-dir",&q)==OPENFS_PATH_OK&&q==rsrc);assert(openfs_unmount(&remount)==OPENFS_MOUNT_OK);}
}

{
    {
        uint64_t tree_ino=0U;
        assert(openfs_path_create(&v,&s,"/home/test/tree-unlink",OPENFS_INODE_MODE_REGULAR,&tree_ino)==OPENFS_PATH_OK);
        openfs_inode_t tree_inode;
        assert(openfs_inode_read(&v,s.inode_table_start,tree_ino,ic,&tree_inode)==OPENFS_INODE_OK);
        uint8_t tree_data[4096U];memset(tree_data,0xA5U,sizeof(tree_data));
        for(unsigned n=0U;n<5U;n++){
            assert(openfs_file_write(&v,&s,&tree_inode,(uint64_t)n*s.block_size,tree_data,sizeof(tree_data))==OPENFS_FILE_OK);
            assert(openfs_inode_read(&v,s.inode_table_start,tree_ino,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&tree_inode)==OPENFS_INODE_OK);
            if(n<4U){char filler[64];(void)snprintf(filler,sizeof(filler),"/home/test/tree-filler-%u",n);uint64_t filler_ino=0U;assert(openfs_path_create(&v,&s,filler,OPENFS_INODE_MODE_REGULAR,&filler_ino)==OPENFS_PATH_OK);openfs_inode_t filler_inode;assert(openfs_inode_read(&v,s.inode_table_start,filler_ino,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&filler_inode)==OPENFS_INODE_OK);assert(openfs_file_truncate(&v,&s,&filler_inode,s.block_size)==OPENFS_FILE_OK);}
        }
        assert(tree_inode.extent_count>OPENFS_INODE_TREE_INLINE_EXTENT_MAX);
        uint64_t tree_root=openfs_inode_get_extent_tree_root(&tree_inode);assert(tree_root!=0U);
        uint8_t *root_before=malloc(s.block_size);uint8_t *root_after=malloc(s.block_size);assert(root_before&&root_after);
        assert(v.read(v.context,tree_root,1U,root_before)==OPENFS_IO_OK);
        uint64_t tree_inode_block=s.inode_table_start+((tree_ino-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
        d.fail_write_block=tree_inode_block;d.fail_write_enabled=1;d.fail_write_count=1;
        assert(openfs_path_unlink(&v,&s,"/home/test/tree-unlink")==OPENFS_PATH_IO_ERROR);
        d.fail_write_enabled=0;
        assert(openfs_path_lookup(&v,&s,"/home/test/tree-unlink",&q)==OPENFS_PATH_OK&&q==tree_ino);
        assert(v.read(v.context,tree_root,1U,root_after)==OPENFS_IO_OK&&memcmp(root_before,root_after,s.block_size)==0);
        {openfs_inode_t restored;assert(openfs_inode_read(&v,s.inode_table_start,tree_ino,ic,&restored)==OPENFS_INODE_OK);assert(restored.blocks==5U&&restored.extent_count>OPENFS_INODE_TREE_INLINE_EXTENT_MAX);}
        {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
        free(root_before);free(root_after);
    }
    {
        uint64_t tree_src=0U,tree_dst=0U;
        assert(openfs_path_create(&v,&s,"/home/test/tree-rename-src",OPENFS_INODE_MODE_REGULAR,&tree_src)==OPENFS_PATH_OK);
        assert(openfs_path_create(&v,&s,"/home/test/tree-rename-dst",OPENFS_INODE_MODE_REGULAR,&tree_dst)==OPENFS_PATH_OK);
        openfs_inode_t tree_dest_inode;assert(openfs_inode_read(&v,s.inode_table_start,tree_dst,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&tree_dest_inode)==OPENFS_INODE_OK);
        uint8_t tree_data2[4096U];memset(tree_data2,0x3CU,sizeof(tree_data2));
        for(unsigned n=0U;n<5U;n++){
            assert(openfs_file_write(&v,&s,&tree_dest_inode,(uint64_t)n*s.block_size,tree_data2,sizeof(tree_data2))==OPENFS_FILE_OK);
            assert(openfs_inode_read(&v,s.inode_table_start,tree_dst,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&tree_dest_inode)==OPENFS_INODE_OK);
            if(n<4U){char filler[64];(void)snprintf(filler,sizeof(filler),"/home/test/tree-rename-filler-%u",n);uint64_t filler_ino=0U;assert(openfs_path_create(&v,&s,filler,OPENFS_INODE_MODE_REGULAR,&filler_ino)==OPENFS_PATH_OK);openfs_inode_t filler_inode;assert(openfs_inode_read(&v,s.inode_table_start,filler_ino,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&filler_inode)==OPENFS_INODE_OK);assert(openfs_file_truncate(&v,&s,&filler_inode,s.block_size)==OPENFS_FILE_OK);}
        }
        assert(tree_dest_inode.extent_count>OPENFS_INODE_TREE_INLINE_EXTENT_MAX);
        uint64_t tree_dest_root=openfs_inode_get_extent_tree_root(&tree_dest_inode);assert(tree_dest_root!=0U);
        uint64_t tree_dest_inode_block=s.inode_table_start+((tree_dst-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
        uint8_t *tree_root_before=malloc(s.block_size);uint8_t *tree_root_after=malloc(s.block_size);assert(tree_root_before&&tree_root_after);assert(v.read(v.context,tree_dest_root,1U,tree_root_before)==OPENFS_IO_OK);
        d.fail_write_block=tree_dest_inode_block;d.fail_write_enabled=1;d.fail_write_count=1;
        assert(openfs_path_rename(&v,&s,"/home/test/tree-rename-src","/home/test/tree-rename-dst")==OPENFS_PATH_IO_ERROR);d.fail_write_enabled=0;
        assert(openfs_path_lookup(&v,&s,"/home/test/tree-rename-src",&q)==OPENFS_PATH_OK&&q==tree_src);assert(openfs_path_lookup(&v,&s,"/home/test/tree-rename-dst",&q)==OPENFS_PATH_OK&&q==tree_dst);
        assert(v.read(v.context,tree_dest_root,1U,tree_root_after)==OPENFS_IO_OK&&memcmp(tree_root_before,tree_root_after,s.block_size)==0);
        {openfs_inode_t restored;assert(openfs_inode_read(&v,s.inode_table_start,tree_dst,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&restored)==OPENFS_INODE_OK);assert(restored.blocks==5U&&restored.extent_count>OPENFS_INODE_TREE_INLINE_EXTENT_MAX);}
        {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
        free(tree_root_before);free(tree_root_after);
    }
    {
        uint64_t partial_dir=0U;assert(openfs_path_mkdir(&v,&s,"/home/test/partial-dir",&partial_dir)==OPENFS_PATH_OK);
        for(unsigned i=0U;i<16U;i++){char p[64];(void)snprintf(p,sizeof(p),"/home/test/partial-dir/f%u",i);uint64_t ino=0U;assert(openfs_path_create(&v,&s,p,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);}
        uint64_t partial_ino=0U;assert(openfs_path_create(&v,&s,"/home/test/partial-dir/partial-unlink",OPENFS_INODE_MODE_REGULAR,&partial_ino)==OPENFS_PATH_OK);
        openfs_inode_t partial_parent;assert(openfs_inode_read(&v,s.inode_table_start,partial_dir,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&partial_parent)==OPENFS_INODE_OK);
        uint64_t partial_block=0U;assert(openfs_file_map_block_device(&v,&s,&partial_parent,1U,&partial_block)==OPENFS_FILE_OK);
        uint8_t *before=malloc(s.block_size);uint8_t *after=malloc(s.block_size);assert(before&&after);
        assert(v.read(v.context,partial_block,1U,before)==OPENFS_IO_OK);
        d.partial_write_block=partial_block;d.partial_write_bytes=17U;d.partial_write_once=1;
        assert(openfs_path_unlink(&v,&s,"/home/test/partial-dir/partial-unlink")==OPENFS_PATH_IO_ERROR);
        assert(v.read(v.context,partial_block,1U,after)==OPENFS_IO_OK&&memcmp(before,after,s.block_size)==0);
        assert(openfs_path_lookup(&v,&s,"/home/test/partial-dir/partial-unlink",&q)==OPENFS_PATH_OK&&q==partial_ino);
        {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
        free(before);free(after);
    }
    uint64_t free_fail_ino=0U;
    assert(openfs_path_create(&v,&s,"/home/test/inode-free-fail",OPENFS_INODE_MODE_REGULAR,&free_fail_ino)==OPENFS_PATH_OK);
    d.fail_write_block=s.inode_bitmap_start; d.fail_write_enabled=1; d.fail_write_count=1;
    assert(openfs_path_unlink(&v,&s,"/home/test/inode-free-fail")==OPENFS_PATH_IO_ERROR);
    d.fail_write_enabled=0;
    assert(openfs_path_lookup(&v,&s,"/home/test/inode-free-fail",&q)==OPENFS_PATH_OK&&q==free_fail_ino);
    {openfs_inode_t restored;assert(openfs_inode_read(&v,s.inode_table_start,free_fail_ino,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&restored)==OPENFS_INODE_OK);assert(restored.mode==OPENFS_INODE_MODE_REGULAR&&restored.link_count==1U);}
    {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
}

}
{
    uint64_t full_link_dir=0U;
    assert(openfs_path_mkdir(&v,&s,"/home/test/full-link-dir",&full_link_dir)==OPENFS_PATH_OK);
    for(unsigned i=0U;i<16U;i++){char p[64];(void)snprintf(p,sizeof(p),"/home/test/full-link-dir/f%u",i);uint64_t ino=0U;assert(openfs_path_create(&v,&s,p,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);}
    openfs_inode_t full_before;
    assert(openfs_inode_read(&v,s.inode_table_start,full_link_dir,ic,&full_before)==OPENFS_INODE_OK&&full_before.size==s.block_size&&full_before.blocks==1U);
    size_t bitmap_bytes=(size_t)(s.block_bitmap_blocks*s.block_size);
    uint8_t *bitmap_before=malloc(bitmap_bytes);uint8_t *bitmap_after=malloc(bitmap_bytes);assert(bitmap_before&&bitmap_after);
    memcpy(bitmap_before,d.b+(size_t)(s.block_bitmap_start*s.block_size),bitmap_bytes);
    uint64_t target_block=s.inode_table_start+((x-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
    d.fail_write_block=target_block;d.fail_write_enabled=1;d.fail_write_count=1;
    assert(openfs_link(&v,&s,"/home/test/file","/home/test/full-link-dir/alias")==OPENFS_PATH_IO_ERROR);
    d.fail_write_enabled=0;
    openfs_inode_t full_after;
    assert(openfs_inode_read(&v,s.inode_table_start,full_link_dir,ic,&full_after)==OPENFS_INODE_OK);
    assert(full_after.size==full_before.size&&full_after.blocks==full_before.blocks);
    memcpy(bitmap_after,d.b+(size_t)(s.block_bitmap_start*s.block_size),bitmap_bytes);
    assert(memcmp(bitmap_before,bitmap_after,bitmap_bytes)==0);
    assert(openfs_path_lookup(&v,&s,"/home/test/full-link-dir/alias",&q)==OPENFS_PATH_NOT_FOUND);
    {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
    free(bitmap_before);free(bitmap_after);
}
{
    uint64_t full_rename_dir=0U,rename_source=0U;
    assert(openfs_path_mkdir(&v,&s,"/home/test/full-rename-dir",&full_rename_dir)==OPENFS_PATH_OK);
    for(unsigned i=0U;i<16U;i++){char p[64];(void)snprintf(p,sizeof(p),"/home/test/full-rename-dir/f%u",i);uint64_t ino=0U;assert(openfs_path_create(&v,&s,p,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);}
    assert(openfs_path_create(&v,&s,"/home/test/rename-source",OPENFS_INODE_MODE_REGULAR,&rename_source)==OPENFS_PATH_OK);
    openfs_inode_t full_before;
    assert(openfs_inode_read(&v,s.inode_table_start,full_rename_dir,ic,&full_before)==OPENFS_INODE_OK&&full_before.size==s.block_size&&full_before.blocks==1U);
    size_t bitmap_bytes=(size_t)(s.block_bitmap_blocks*s.block_size);
    uint8_t *bitmap_before=malloc(bitmap_bytes);uint8_t *bitmap_after=malloc(bitmap_bytes);assert(bitmap_before&&bitmap_after);
    memcpy(bitmap_before,d.b+(size_t)(s.block_bitmap_start*s.block_size),bitmap_bytes);
    d.fail_write_block=s.block_bitmap_start;d.fail_write_enabled=1;d.fail_write_count=1;
    assert(openfs_path_rename(&v,&s,"/home/test/rename-source","/home/test/full-rename-dir/moved")==OPENFS_PATH_IO_ERROR);
    d.fail_write_enabled=0;
    openfs_inode_t full_after;
    assert(openfs_inode_read(&v,s.inode_table_start,full_rename_dir,ic,&full_after)==OPENFS_INODE_OK);
    assert(full_after.size==full_before.size&&full_after.blocks==full_before.blocks);
    memcpy(bitmap_after,d.b+(size_t)(s.block_bitmap_start*s.block_size),bitmap_bytes);
    assert(memcmp(bitmap_before,bitmap_after,bitmap_bytes)==0);
    assert(openfs_path_lookup(&v,&s,"/home/test/rename-source",&q)==OPENFS_PATH_OK&&q==rename_source);
    assert(openfs_path_lookup(&v,&s,"/home/test/full-rename-dir/moved",&q)==OPENFS_PATH_NOT_FOUND);
    {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
    free(bitmap_before);free(bitmap_after);
}
assert(openfs_symlink(&v,&s,"/home/test","/alias")==OPENFS_PATH_OK);{
    openfs_inode_t home_test_inode;assert(openfs_inode_read(&v,s.inode_table_start,m,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&home_test_inode)==OPENFS_INODE_OK);
    uint64_t home_test_block=0U;assert(openfs_file_map_block_device(&v,&s,&home_test_inode,0U,&home_test_block)==OPENFS_FILE_OK);
    d.fail_flush=1;d.fail_flush_once=1;
    assert(openfs_symlink(&v,&s,"/home/test","/home/test/symlink-rollback")==OPENFS_PATH_IO_ERROR);
    d.fail_flush=0;
    assert(openfs_path_lookup(&v,&s,"/home/test/symlink-rollback",&q)==OPENFS_PATH_NOT_FOUND);
}
uint64_t through=0U;assert(openfs_path_create(&v,&s,"/alias/throughlink",OPENFS_INODE_MODE_REGULAR,&through)==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/home/test/throughlink",&q)==OPENFS_PATH_OK&&q==through);assert(openfs_path_rename(&v,&s,"/home/test/file","/home/test/newfile")==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/home/test/newfile",&q)==OPENFS_PATH_OK&&q==x);assert(openfs_path_unlink(&v,&s,"/home/test/newfile")==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/home/test/newfile",&q)==OPENFS_PATH_NOT_FOUND);assert(openfs_path_create(&v,&s,"/home/test/unlink-flush",OPENFS_INODE_MODE_REGULAR,&q)==OPENFS_PATH_OK);assert(openfs_link(&v,&s,"/home/test/unlink-flush","/home/test/unlink-flush-link")==OPENFS_PATH_OK);d.fail_flush=1;d.fail_flush_once=1;assert(openfs_path_unlink(&v,&s,"/home/test/unlink-flush")==OPENFS_PATH_IO_ERROR);assert(openfs_path_lookup(&v,&s,"/home/test/unlink-flush",&q)==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/home/test/unlink-flush-link",&q)==OPENFS_PATH_OK);{uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}uint64_t recycle=0U;assert(openfs_path_create(&v,&s,"/home/test/recycle",OPENFS_INODE_MODE_REGULAR,&recycle)==OPENFS_PATH_OK);openfs_inode_t recycle_inode;assert(openfs_inode_read(&v,s.inode_table_start,recycle,ic,&recycle_inode)==OPENFS_INODE_OK);uint64_t recycle_generation=recycle_inode.generation;assert(openfs_path_unlink(&v,&s,"/home/test/recycle")==OPENFS_PATH_OK);uint64_t reused=0U;assert(openfs_path_create(&v,&s,"/home/test/reused",OPENFS_INODE_MODE_REGULAR,&reused)==OPENFS_PATH_OK&&reused==recycle);openfs_inode_t reused_inode;assert(openfs_inode_read(&v,s.inode_table_start,reused,ic,&reused_inode)==OPENFS_INODE_OK&&reused_inode.generation==recycle_generation+1U);openfs_dir_entry_t reused_entry;openfs_inode_t parent_inode;assert(openfs_inode_read(&v,s.inode_table_start,m,ic,&parent_inode)==OPENFS_INODE_OK);assert(openfs_dir_lookup(&v,&s,&parent_inode,"reused",&reused_entry)==OPENFS_DIR_OK&&reused_entry.generation==reused_inode.generation);openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);openfs_transaction_t tx;assert(openfs_transaction_begin(&tx,&v,&j)==OPENFS_TRANSACTION_OK);uint64_t atomic_ino=0U;assert(openfs_path_mkdir_tx(&tx,&s,"/atomic",&atomic_ino)==OPENFS_PATH_OK);assert(openfs_transaction_commit(&tx)==OPENFS_TRANSACTION_OK);assert(openfs_path_lookup(&v,&s,"/atomic",&q)==OPENFS_PATH_OK&&q==atomic_ino);assert(openfs_transaction_begin(&tx,&v,&j)==OPENFS_TRANSACTION_OK);assert(openfs_path_mkdir_tx(&tx,&s,"/aborted",&q)==OPENFS_PATH_OK);assert(openfs_transaction_abort(&tx)==OPENFS_TRANSACTION_OK);assert(openfs_path_lookup(&v,&s,"/aborted",&q)==OPENFS_PATH_NOT_FOUND);
{
    uint64_t tx_src=0U,tx_dst=0U;
    assert(openfs_path_create(&v,&s,"/home/test/tx-rename-src",OPENFS_INODE_MODE_REGULAR,&tx_src)==OPENFS_PATH_OK);
    assert(openfs_path_create(&v,&s,"/home/test/tx-rename-dst",OPENFS_INODE_MODE_REGULAR,&tx_dst)==OPENFS_PATH_OK);
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    assert(openfs_transaction_begin(&tx,&v,&j)==OPENFS_TRANSACTION_OK);
    assert(openfs_path_rename_tx(&tx,&s,"/home/test/tx-rename-src","/home/test/tx-rename-dst-new")==OPENFS_PATH_OK);
    openfs_block_device_t *txdev=openfs_transaction_device(&tx);assert(txdev!=NULL);
    assert(openfs_path_lookup(txdev,&s,"/home/test/tx-rename-src",&q)==OPENFS_PATH_NOT_FOUND);
    assert(openfs_path_lookup(txdev,&s,"/home/test/tx-rename-dst-new",&q)==OPENFS_PATH_OK&&q==tx_src);
    assert(openfs_path_lookup(txdev,&s,"/home/test/tx-rename-dst",&q)==OPENFS_PATH_OK&&q==tx_dst);
    assert(openfs_transaction_abort(&tx)==OPENFS_TRANSACTION_OK);
    assert(openfs_path_lookup(&v,&s,"/home/test/tx-rename-src",&q)==OPENFS_PATH_OK&&q==tx_src);
    assert(openfs_path_lookup(&v,&s,"/home/test/tx-rename-dst",&q)==OPENFS_PATH_OK&&q==tx_dst);
    {openfs_mount_t remount;assert(openfs_mount(&remount,&v)==OPENFS_MOUNT_OK);assert(openfs_path_lookup(&v,&remount.superblock,"/home/test/tx-rename-src",&q)==OPENFS_PATH_OK&&q==tx_src);assert(openfs_path_lookup(&v,&remount.superblock,"/home/test/tx-rename-dst",&q)==OPENFS_PATH_OK&&q==tx_dst);assert(openfs_unmount(&remount)==OPENFS_MOUNT_OK);}
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    assert(openfs_transaction_begin(&tx,&v,&j)==OPENFS_TRANSACTION_OK);
    assert(openfs_path_rename_tx(&tx,&s,"/home/test/tx-rename-src","/home/test/tx-rename-committed")==OPENFS_PATH_OK);
    assert(openfs_transaction_commit(&tx)==OPENFS_TRANSACTION_OK);
    assert(openfs_transaction_begin(&tx,&v,&j)==OPENFS_TRANSACTION_OK);
    assert(openfs_path_rename_tx(&tx,&s,"/home/test/tx-rename-committed","/home/test/tx-rename-commit-fail")==OPENFS_PATH_OK);
    openfs_inode_t tx_parent_inode;assert(openfs_inode_read(&v,s.inode_table_start,m,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&tx_parent_inode)==OPENFS_INODE_OK);
    txdev=openfs_transaction_device(&tx);assert(txdev!=NULL);
    uint64_t tx_parent_block=0U;int tx_parent_block_found=0;
    for(uint64_t logical=0U;logical<tx_parent_inode.blocks&&!tx_parent_block_found;logical++){
        assert(openfs_file_map_block_device(&v,&s,&tx_parent_inode,logical,&tx_parent_block)==OPENFS_FILE_OK);
        uint8_t *block=malloc(s.block_size);assert(block!=NULL);
        assert(txdev->read(txdev->context,tx_parent_block,1U,block)==OPENFS_IO_OK);
        uint64_t entries=s.block_size/OPENFS_DIR_ENTRY_SIZE;
        for(uint64_t slot=0U;slot<entries;slot++){
            const uint8_t *raw=block+(size_t)(slot*OPENFS_DIR_ENTRY_SIZE);
            if(memcmp(raw,"ODIR1",5U)==0&&raw[7U]==21U&&memcmp(raw+24U,"tx-rename-commit-fail",21U)==0){tx_parent_block_found=1;break;}
        }
        free(block);
    }
    assert(tx_parent_block_found);
    d.fail_write_block=tx_parent_block;d.fail_write_enabled=1;d.fail_write_count=1;
    assert(openfs_transaction_commit(&tx)==OPENFS_TRANSACTION_IO_ERROR);
    d.fail_write_enabled=0;
    assert(openfs_transaction_abort(&tx)==OPENFS_TRANSACTION_CORRUPT);
    {openfs_mount_t remount;assert(openfs_mount(&remount,&v)==OPENFS_MOUNT_OK);assert(openfs_path_lookup(&v,&remount.superblock,"/home/test/tx-rename-dst",&q)==OPENFS_PATH_OK&&q==tx_dst);assert(openfs_path_lookup(&v,&remount.superblock,"/home/test/tx-rename-commit-fail",&q)==OPENFS_PATH_OK&&q==tx_src);uint64_t errors=0U;assert(openfs_fsck(&v,&remount.superblock,&errors)==OPENFS_FSCK_OK&&errors==0U);assert(openfs_unmount(&remount)==OPENFS_MOUNT_OK);}
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    assert(openfs_transaction_begin(&tx,&v,&j)==OPENFS_TRANSACTION_OK);
    assert(openfs_path_rename_tx(&tx,&s,"/home/test/tx-rename-commit-fail","/home/test/tx-rename-flush-fail")==OPENFS_PATH_OK);
    d.fail_flush=1;d.fail_flush_once=1;
    assert(openfs_transaction_commit(&tx)==OPENFS_TRANSACTION_IO_ERROR);
    assert(openfs_transaction_abort(&tx)==OPENFS_TRANSACTION_OK);
    d.fail_flush=0;
    {openfs_mount_t remount;assert(openfs_mount(&remount,&v)==OPENFS_MOUNT_OK);assert(openfs_path_lookup(&v,&remount.superblock,"/home/test/tx-rename-commit-fail",&q)==OPENFS_PATH_NOT_FOUND);assert(openfs_path_lookup(&v,&remount.superblock,"/home/test/tx-rename-flush-fail",&q)==OPENFS_PATH_OK&&q==tx_src);assert(openfs_unmount(&remount)==OPENFS_MOUNT_OK);}
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    assert(openfs_transaction_begin(&tx,&v,&j)==OPENFS_TRANSACTION_OK);
    assert(openfs_path_rename_tx(&tx,&s,"/home/test/tx-rename-flush-fail","/home/test/tx-rename-checkpoint-fail")==OPENFS_PATH_OK);
    d.flush_calls=0U;d.fail_flush=0;d.fail_flush_once=0;d.fail_flush_call=3U;
    assert(openfs_transaction_commit(&tx)==OPENFS_TRANSACTION_IO_ERROR);
    assert(tx.active==1&&tx.failed==1&&tx.commit_started==1&&tx.committed==1&&tx.recovery_required==1&&tx.pending==NULL&&tx.pending_count==0U);
    assert(j.active_transaction_id==0U&&j.commit_record_written==1U);
    assert(openfs_transaction_abort(&tx)==OPENFS_TRANSACTION_CORRUPT);
    assert(tx.active==0&&tx.pending==NULL&&tx.pending_count==0U);
    d.fail_flush_call=0U;
    {openfs_mount_t remount;assert(openfs_mount(&remount,&v)==OPENFS_MOUNT_OK);assert(openfs_path_lookup(&v,&remount.superblock,"/home/test/tx-rename-flush-fail",&q)==OPENFS_PATH_NOT_FOUND);assert(openfs_path_lookup(&v,&remount.superblock,"/home/test/tx-rename-checkpoint-fail",&q)==OPENFS_PATH_OK&&q==tx_src);uint64_t errors=0U;assert(openfs_fsck(&v,&remount.superblock,&errors)==OPENFS_FSCK_OK&&errors==0U);assert(openfs_unmount(&remount)==OPENFS_MOUNT_OK);}
    uint64_t tx_partial_src=0U;
    assert(openfs_path_create(&v,&s,"/home/test/tx-partial-src",OPENFS_INODE_MODE_REGULAR,&tx_partial_src)==OPENFS_PATH_OK);
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    assert(openfs_transaction_begin(&tx,&v,&j)==OPENFS_TRANSACTION_OK);
    assert(openfs_path_rename_tx(&tx,&s,"/home/test/tx-partial-src","/home/test/tx-partial-dst")==OPENFS_PATH_OK);
    uint64_t commit_slot=s.journal_start+j.next_record;uint64_t sequence_before_failed_commit=j.sequence;uint8_t *commit_before=malloc(s.block_size);uint8_t *commit_after=malloc(s.block_size);assert(commit_before&&commit_after);
    assert(v.read(v.context,commit_slot,1U,commit_before)==OPENFS_IO_OK);
    d.partial_write_block=commit_slot;d.partial_write_bytes=17U;d.partial_write_once=1;
    assert(openfs_transaction_commit(&tx)==OPENFS_TRANSACTION_IO_ERROR);
    assert(tx.active==1&&tx.failed==1&&tx.commit_started==1&&tx.committed==0&&tx.recovery_required==0&&tx.pending==NULL&&tx.pending_count==0U);
    assert(v.read(v.context,commit_slot,1U,commit_after)==OPENFS_IO_OK&&memcmp(commit_before,commit_after,s.block_size)==0);
    assert(j.active_transaction_id==tx.txid&&j.commit_record_written==0U);
    assert(j.sequence==sequence_before_failed_commit);
    assert(openfs_transaction_abort(&tx)==OPENFS_TRANSACTION_OK);
    assert(tx.active==0&&tx.pending==NULL&&tx.pending_count==0U);
    assert(openfs_path_lookup(&v,&s,"/home/test/tx-partial-src",&q)==OPENFS_PATH_OK&&q==tx_partial_src);
    assert(openfs_path_lookup(&v,&s,"/home/test/tx-partial-dst",&q)==OPENFS_PATH_NOT_FOUND);
    free(commit_before);free(commit_after);
    {openfs_mount_t remount;assert(openfs_mount(&remount,&v)==OPENFS_MOUNT_OK);assert(openfs_path_lookup(&v,&remount.superblock,"/home/test/tx-partial-src",&q)==OPENFS_PATH_OK&&q==tx_partial_src);assert(openfs_path_lookup(&v,&remount.superblock,"/home/test/tx-partial-dst",&q)==OPENFS_PATH_NOT_FOUND);uint64_t errors=0U;assert(openfs_fsck(&v,&remount.superblock,&errors)==OPENFS_FSCK_OK&&errors==0U);assert(openfs_unmount(&remount)==OPENFS_MOUNT_OK);}


    assert(openfs_path_lookup(&v,&s,"/home/test/tx-rename-src",&q)==OPENFS_PATH_NOT_FOUND);
    assert(openfs_path_lookup(&v,&s,"/home/test/tx-rename-dst",&q)==OPENFS_PATH_OK&&q==tx_dst);
    {openfs_mount_t remount;assert(openfs_mount(&remount,&v)==OPENFS_MOUNT_OK);assert(openfs_path_lookup(&v,&remount.superblock,"/home/test/tx-rename-dst",&q)==OPENFS_PATH_OK&&q==tx_dst);assert(openfs_path_lookup(&v,&remount.superblock,"/home/test/tx-rename-src",&q)==OPENFS_PATH_NOT_FOUND);assert(openfs_unmount(&remount)==OPENFS_MOUNT_OK);}
}
free(d.b);return 0;}