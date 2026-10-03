#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/path.h"
#include "openfs/link.h"
#include "openfs/bitmap.h"
#include "openfs/fsck.h"
#include "openfs/file.h"
#include <stdio.h>
typedef struct{uint8_t*b;uint32_t bs;uint64_t bc;uint64_t fail_read_block;uint64_t fail_write_block;uint64_t arm_block;int fail_read_enabled;int fail_write_enabled;int arm_on_write;int armed;int fail_next_read;int fail_write_count;int fail_next_armed_write;int fail_flush;int fail_flush_once;}D;
static openfs_io_result_t r(void*c,uint64_t f,uint32_t n,void*x){D*d=c;if(d->fail_next_read&&d->armed){d->fail_next_read=0;return OPENFS_IO_IO_ERROR;}if(d->fail_read_enabled&&f==d->fail_read_block)return OPENFS_IO_IO_ERROR;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(x,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t w(void*c,uint64_t f,uint32_t n,const void*x){D*d=c;if(d->fail_write_enabled&&f==d->fail_write_block){if(d->fail_write_count>0){if(--d->fail_write_count==0)d->fail_write_enabled=0;}return OPENFS_IO_IO_ERROR;}if(d->arm_on_write&&f==d->arm_block)d->armed=1;if(d->armed&&d->fail_next_armed_write){d->fail_next_armed_write=0;return OPENFS_IO_IO_ERROR;}if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),x,(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t f(void*c){D*d=c;if(d->fail_flush){if(d->fail_flush_once)d->fail_flush=0;return OPENFS_IO_IO_ERROR;}return OPENFS_IO_OK;}
static void long_unlink_path(void){
 D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={0};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
 char path[OPENFS_PATH_MAX];size_t used=1U;path[0]='/';path[1]='\0';for(unsigned level=0U;level<6U;level++){char name[201];memset(name,(int)('a'+level),200U);name[200]='\0';assert(used+201U<sizeof(path));memcpy(path+used,name,200U);used+=200U;path[used++]='/';path[used]='\0';uint64_t ino=0U;assert(openfs_path_mkdir(&v,&s,path,&ino)==OPENFS_PATH_OK);}path[used-1U]='x';path[used]='\0';uint64_t ino=0U;assert(openfs_path_create(&v,&s,path,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);assert(openfs_path_unlink(&v,&s,path)==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,path,&ino)==OPENFS_PATH_NOT_FOUND);free(d.b);
}
int main(void){long_unlink_path();D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={0};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);uint64_t n=0,m=0,x=0,q=0;assert(openfs_path_create(&v,&s,"/home",OPENFS_INODE_MODE_DIRECTORY|0755U,&n)==OPENFS_PATH_OK);assert(openfs_path_create(&v,&s,"/home/invalid-mode",0x10000U,&x)==OPENFS_PATH_INVALID_ARGUMENT);for(unsigned i=0U;i<14U;i++){char filler[32];(void)snprintf(filler,sizeof(filler),"/home/f%u",i);uint64_t filler_ino=0U;assert(openfs_path_create(&v,&s,filler,OPENFS_INODE_MODE_REGULAR,&filler_ino)==OPENFS_PATH_OK);}{
    openfs_inode_t home_inode;
    assert(openfs_inode_read(&v,s.inode_table_start,n,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&home_inode)==OPENFS_INODE_OK);
    uint64_t home_block=0U;assert(openfs_file_map_block_device(&v,&s,&home_inode,0U,&home_block)==OPENFS_FILE_OK);
    d.arm_on_write=0;d.fail_write_block=home_block;d.fail_write_enabled=1;d.fail_write_count=1;
    uint64_t create_fail_ino=0U;assert(openfs_path_create(&v,&s,"/home/create-write-fail",OPENFS_INODE_MODE_REGULAR,&create_fail_ino)==OPENFS_PATH_IO_ERROR);
    d.fail_write_enabled=0;
    assert(openfs_path_lookup(&v,&s,"/home/create-write-fail",&x)==OPENFS_PATH_NOT_FOUND);
    {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
}
d.fail_read_block=0U;d.fail_read_enabled=0;d.arm_on_write=1;d.arm_block=s.inode_table_start+1U;d.fail_next_read=1;uint64_t failed_ino=0U;assert(openfs_path_create(&v,&s,"/home/readfail",OPENFS_INODE_MODE_REGULAR,&failed_ino)==OPENFS_PATH_IO_ERROR);d.fail_read_enabled=0;assert(openfs_path_lookup(&v,&s,"/home/readfail",&x)==OPENFS_PATH_NOT_FOUND);assert(openfs_path_mkdir(&v,&s,"/home/test",&m)==OPENFS_PATH_OK);assert(openfs_path_chmod(&v,&s,"/home/test",0755U)==OPENFS_PATH_OK);assert(openfs_path_rename(&v,&s,"/home/test","/home/test")==OPENFS_PATH_EXISTS);
assert(openfs_path_rename(&v,&s,"/home/test","/home/renamed")==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/home/renamed",&m)==OPENFS_PATH_OK);{
    uint64_t target_block=s.inode_table_start+((m-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
    d.fail_write_block=target_block; d.fail_write_enabled=1; d.fail_write_count=1;
    assert(openfs_path_rename(&v,&s,"/home/renamed","/home/failed")==OPENFS_PATH_IO_ERROR);
    d.fail_write_enabled=0;
    assert(openfs_path_lookup(&v,&s,"/home/renamed",&q)==OPENFS_PATH_OK&&q==m);
    assert(openfs_path_lookup(&v,&s,"/home/failed",&q)==OPENFS_PATH_NOT_FOUND);
}
d.fail_flush=1;d.fail_flush_once=1;assert(openfs_path_rename(&v,&s,"/home/renamed","/home/rename-flush-fail")==OPENFS_PATH_IO_ERROR);assert(openfs_path_lookup(&v,&s,"/home/renamed",&q)==OPENFS_PATH_OK&&q==m);assert(openfs_path_lookup(&v,&s,"/home/rename-flush-fail",&q)==OPENFS_PATH_NOT_FOUND);{uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
assert(openfs_path_rename(&v,&s,"/home/renamed","/home/test")==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/..",&x)==OPENFS_PATH_OK&&x==s.root_inode);assert(openfs_path_lookup(&v,&s,"///home//test//",&x)==OPENFS_PATH_OK&&x==m);assert(openfs_path_lookup(&v,&s,"/home/./test/../test",&x)==OPENFS_PATH_OK&&x==m);assert(openfs_path_create(&v,&s,"/home/test/file",OPENFS_INODE_MODE_REGULAR,&x)==OPENFS_PATH_OK);assert(openfs_path_mkdir(&v,&s,"/home/test/nonempty",&q)==OPENFS_PATH_OK);assert(openfs_path_create(&v,&s,"/home/test/nonempty/file",OPENFS_INODE_MODE_REGULAR,&q)==OPENFS_PATH_OK);assert(openfs_path_unlink(&v,&s,"/home/test/nonempty")==OPENFS_PATH_NOT_EMPTY);assert(openfs_path_unlink(&v,&s,"/home/test/nonempty/file")==OPENFS_PATH_OK);assert(openfs_path_unlink(&v,&s,"/home/test/nonempty")==OPENFS_PATH_OK);
assert(openfs_path_chmod(&v,&s,"/home/test",0777U)==OPENFS_PATH_OK);
assert(openfs_path_create_as(&v,&s,"/home/test/asuser",OPENFS_INODE_MODE_REGULAR,1000U,1000U,&q)==OPENFS_PATH_OK);
{openfs_inode_t as_i;assert(openfs_inode_read(&v,s.inode_table_start,q,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&as_i)==OPENFS_INODE_OK);assert(as_i.uid==1000U&&as_i.gid==1000U);}
assert(openfs_path_unlink_as(&v,&s,"/home/test/asuser",1000U,1000U)==OPENFS_PATH_OK);
assert(openfs_path_create_as(&v,&s,"/home/test/denied",OPENFS_INODE_MODE_REGULAR,1000U,1000U,&q)==OPENFS_PATH_OK);
assert(openfs_path_chmod(&v,&s,"/home/test",0555U)==OPENFS_PATH_OK);
assert(openfs_path_unlink_as(&v,&s,"/home/test/denied",1000U,1000U)==OPENFS_PATH_ACCESS_DENIED);
assert(openfs_path_create_as(&v,&s,"/home/test/blocked",OPENFS_INODE_MODE_REGULAR,1000U,1000U,&q)==OPENFS_PATH_ACCESS_DENIED);assert(openfs_path_mkdir(&v,&s,"/home/private",&q)==OPENFS_PATH_OK);assert(openfs_path_chmod(&v,&s,"/home/private",0700U)==OPENFS_PATH_OK);assert(openfs_path_create_as(&v,&s,"/home/private/hidden",OPENFS_INODE_MODE_REGULAR,1000U,1000U,&q)==OPENFS_PATH_ACCESS_DENIED);
assert(openfs_path_chmod_as(&v,&s,"/home/test/file",0600U,1000U,1000U)==OPENFS_PATH_ACCESS_DENIED);
assert(openfs_path_chmod_as(&v,&s,"/home/test/file",0600U,0U,0U)==OPENFS_PATH_OK);
assert(openfs_path_set_times_as(&v,&s,"/home/test/file",1000U,1000U,33U,44U)==OPENFS_PATH_ACCESS_DENIED);
assert(openfs_path_set_times_as(&v,&s,"/home/test/file",0U,0U,33U,44U)==OPENFS_PATH_OK);assert(openfs_path_mkdir(&v,&s,"/home/sticky",&q)==OPENFS_PATH_OK);assert(openfs_path_chmod(&v,&s,"/home/sticky",01777U)==OPENFS_PATH_OK);assert(openfs_path_create(&v,&s,"/home/sticky/file",OPENFS_INODE_MODE_REGULAR,&q)==OPENFS_PATH_OK);{openfs_inode_t si;assert(openfs_inode_read(&v,s.inode_table_start,q,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&si)==OPENFS_INODE_OK);si.uid=2000U;assert(openfs_inode_write(&v,s.inode_table_start,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&si)==OPENFS_INODE_OK);}assert(openfs_path_unlink_as(&v,&s,"/home/sticky/file",1000U,1000U)==OPENFS_PATH_ACCESS_DENIED);assert(openfs_path_unlink_as(&v,&s,"/home/sticky/file",2000U,2000U)==OPENFS_PATH_OK);
assert(openfs_path_chmod(&v,&s,"/home/test",0777U)==OPENFS_PATH_OK);assert(openfs_path_chmod(&v,&s,"/home/test/file",0644U)==OPENFS_PATH_OK);assert(openfs_path_check_access(&v,&s,"/home/test/file",0U,0U,4U)==OPENFS_PATH_OK);assert(openfs_path_check_access(&v,&s,"/home/test/file",1U,1U,2U)==OPENFS_PATH_ACCESS_DENIED);assert(openfs_path_check_access(&v,&s,"/home/test/file",0U,0U,8U)==OPENFS_PATH_INVALID_ARGUMENT);assert(openfs_path_check_access(&v,&s,"/home/test/file",1000U,1000U,1U)==OPENFS_PATH_ACCESS_DENIED);assert(openfs_path_set_times(&v,&s,"/home/test/file",11U,22U)==OPENFS_PATH_OK);openfs_inode_t fi;uint64_t ic=(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE;assert(openfs_inode_read(&v,s.inode_table_start,x,ic,&fi)==OPENFS_INODE_OK&&fi.mode==(OPENFS_INODE_MODE_REGULAR|0644U)&&fi.atime_ns==11U&&fi.mtime_ns==22U);assert(openfs_path_lookup(&v,&s,"/home/test/file",&q)==OPENFS_PATH_OK&&q==x);{
    openfs_inode_t before_link;
    assert(openfs_inode_read(&v,s.inode_table_start,x,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&before_link)==OPENFS_INODE_OK);
    uint64_t target_block=s.inode_table_start+((x-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
    d.fail_write_block=target_block; d.fail_write_enabled=1; d.fail_write_count=1;
    assert(openfs_link(&v,&s,"/home/test/file","/home/test/link-fail")==OPENFS_PATH_IO_ERROR);
    d.fail_write_enabled=0;
    assert(openfs_path_lookup(&v,&s,"/home/test/link-fail",&q)==OPENFS_PATH_NOT_FOUND);{
    uint64_t unlink_fail_ino=0U;
    assert(openfs_path_create(&v,&s,"/home/test/unlink-fail",OPENFS_INODE_MODE_REGULAR,&unlink_fail_ino)==OPENFS_PATH_OK);
    openfs_inode_t unlink_fail_inode;
    assert(openfs_inode_read(&v,s.inode_table_start,unlink_fail_ino,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&unlink_fail_inode)==OPENFS_INODE_OK);
    uint8_t unlink_payload[4096];memset(unlink_payload,0x6BU,sizeof(unlink_payload));
    assert(openfs_file_write(&v,&s,&unlink_fail_inode,0U,unlink_payload,sizeof(unlink_payload))==OPENFS_FILE_OK);
    uint64_t target_block=s.inode_table_start+((unlink_fail_ino-1U)*(uint64_t)OPENFS_INODE_SIZE)/s.block_size;
    d.fail_write_block=target_block; d.fail_write_enabled=1; d.fail_write_count=1;
    assert(openfs_path_unlink(&v,&s,"/home/test/unlink-fail")==OPENFS_PATH_IO_ERROR);
    d.fail_write_enabled=0;
    assert(openfs_path_lookup(&v,&s,"/home/test/unlink-fail",&q)==OPENFS_PATH_OK&&q==unlink_fail_ino);
    {openfs_inode_t restored;uint8_t readback[4096];size_t got=0U;
        assert(openfs_inode_read(&v,s.inode_table_start,unlink_fail_ino,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&restored)==OPENFS_INODE_OK);
        assert(restored.size==sizeof(unlink_payload)&&restored.blocks==1U);
        assert(openfs_file_read(&v,&s,&restored,0U,readback,sizeof(readback),&got)==OPENFS_FILE_OK&&got==sizeof(readback)&&memcmp(readback,unlink_payload,sizeof(readback))==0);
    }
    {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
}
{
    uint64_t free_fail_ino=0U;
    assert(openfs_path_create(&v,&s,"/home/test/inode-free-fail",OPENFS_INODE_MODE_REGULAR,&free_fail_ino)==OPENFS_PATH_OK);
    d.fail_write_block=s.inode_bitmap_start; d.fail_write_enabled=1; d.fail_write_count=1;
    assert(openfs_path_unlink(&v,&s,"/home/test/inode-free-fail")==OPENFS_PATH_IO_ERROR);
    d.fail_write_enabled=0;
    assert(openfs_path_lookup(&v,&s,"/home/test/inode-free-fail",&q)==OPENFS_PATH_OK&&q==free_fail_ino);
    {openfs_inode_t restored;assert(openfs_inode_read(&v,s.inode_table_start,free_fail_ino,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&restored)==OPENFS_INODE_OK);assert(restored.mode==OPENFS_INODE_MODE_REGULAR&&restored.link_count==1U);}
    {uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}
}

    openfs_inode_t after_link;
    assert(openfs_inode_read(&v,s.inode_table_start,x,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&after_link)==OPENFS_INODE_OK);
    assert(after_link.link_count==before_link.link_count);
}
assert(openfs_symlink(&v,&s,"/home/test","/alias")==OPENFS_PATH_OK);{
    openfs_inode_t home_test_inode;assert(openfs_inode_read(&v,s.inode_table_start,m,(s.inode_table_blocks*s.block_size)/OPENFS_INODE_SIZE,&home_test_inode)==OPENFS_INODE_OK);
    uint64_t home_test_block=0U;assert(openfs_file_map_block_device(&v,&s,&home_test_inode,0U,&home_test_block)==OPENFS_FILE_OK);
    d.arm_on_write=1;d.arm_block=home_test_block;d.armed=0;d.fail_next_armed_write=1;
    assert(openfs_symlink(&v,&s,"/home/test","/home/test/symlink-rollback")==OPENFS_PATH_IO_ERROR);
    d.arm_on_write=0;d.armed=0;
    assert(openfs_path_lookup(&v,&s,"/home/test/symlink-rollback",&q)==OPENFS_PATH_NOT_FOUND);
}
uint64_t through=0U;assert(openfs_path_create(&v,&s,"/alias/throughlink",OPENFS_INODE_MODE_REGULAR,&through)==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/home/test/throughlink",&q)==OPENFS_PATH_OK&&q==through);assert(openfs_path_rename(&v,&s,"/home/test/file","/home/test/newfile")==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/home/test/newfile",&q)==OPENFS_PATH_OK&&q==x);assert(openfs_path_unlink(&v,&s,"/home/test/newfile")==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/home/test/newfile",&q)==OPENFS_PATH_NOT_FOUND);assert(openfs_path_create(&v,&s,"/home/test/unlink-flush",OPENFS_INODE_MODE_REGULAR,&q)==OPENFS_PATH_OK);assert(openfs_link(&v,&s,"/home/test/unlink-flush","/home/test/unlink-flush-link")==OPENFS_PATH_OK);d.fail_flush=1;d.fail_flush_once=1;assert(openfs_path_unlink(&v,&s,"/home/test/unlink-flush")==OPENFS_PATH_IO_ERROR);assert(openfs_path_lookup(&v,&s,"/home/test/unlink-flush",&q)==OPENFS_PATH_OK);assert(openfs_path_lookup(&v,&s,"/home/test/unlink-flush-link",&q)==OPENFS_PATH_OK);{uint64_t errors=0U;assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_OK&&errors==0U);}uint64_t recycle=0U;assert(openfs_path_create(&v,&s,"/home/test/recycle",OPENFS_INODE_MODE_REGULAR,&recycle)==OPENFS_PATH_OK);openfs_inode_t recycle_inode;assert(openfs_inode_read(&v,s.inode_table_start,recycle,ic,&recycle_inode)==OPENFS_INODE_OK);uint64_t recycle_generation=recycle_inode.generation;assert(openfs_path_unlink(&v,&s,"/home/test/recycle")==OPENFS_PATH_OK);uint64_t reused=0U;assert(openfs_path_create(&v,&s,"/home/test/reused",OPENFS_INODE_MODE_REGULAR,&reused)==OPENFS_PATH_OK&&reused==recycle);openfs_inode_t reused_inode;assert(openfs_inode_read(&v,s.inode_table_start,reused,ic,&reused_inode)==OPENFS_INODE_OK&&reused_inode.generation==recycle_generation+1U);openfs_dir_entry_t reused_entry;openfs_inode_t parent_inode;assert(openfs_inode_read(&v,s.inode_table_start,m,ic,&parent_inode)==OPENFS_INODE_OK);assert(openfs_dir_lookup(&v,&s,&parent_inode,"reused",&reused_entry)==OPENFS_DIR_OK&&reused_entry.generation==reused_inode.generation);openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);openfs_transaction_t tx;assert(openfs_transaction_begin(&tx,&v,&j)==OPENFS_TRANSACTION_OK);uint64_t atomic_ino=0U;assert(openfs_path_mkdir_tx(&tx,&s,"/atomic",&atomic_ino)==OPENFS_PATH_OK);assert(openfs_transaction_commit(&tx)==OPENFS_TRANSACTION_OK);assert(openfs_path_lookup(&v,&s,"/atomic",&q)==OPENFS_PATH_OK&&q==atomic_ino);assert(openfs_transaction_begin(&tx,&v,&j)==OPENFS_TRANSACTION_OK);assert(openfs_path_mkdir_tx(&tx,&s,"/aborted",&q)==OPENFS_PATH_OK);assert(openfs_transaction_abort(&tx)==OPENFS_TRANSACTION_OK);assert(openfs_path_lookup(&v,&s,"/aborted",&q)==OPENFS_PATH_NOT_FOUND);free(d.b);return 0;}