#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/xattr.h"
#include "openfs/fsck.h"
#include "openfs/path.h"
#include "openfs/mount.h"
#include "openfs/bitmap.h"
#include "openfs/cow.h"
#include "openfs/journal.h"

typedef struct{
    uint8_t*b;
    uint32_t bs;
    uint64_t bc;
    uint64_t journal_start;
    uint64_t journal_blocks;
    int fail_home_after_commit;
    int commit_seen;
}D;

static openfs_io_result_t r(void*c,uint64_t f,uint32_t n,void*x){
    D*d=c;
    if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;
    memcpy(x,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));
    return OPENFS_IO_OK;
}
static openfs_io_result_t w(void*c,uint64_t f,uint32_t n,const void*x){
    D*d=c;
    if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;
    int in_journal=f>=d->journal_start&&f<d->journal_start+d->journal_blocks;
    const uint8_t*raw=(const uint8_t*)x;
    if(d->fail_home_after_commit&&d->commit_seen&&!in_journal){
        d->fail_home_after_commit=0;
        return OPENFS_IO_IO_ERROR;
    }
    memcpy(d->b+(size_t)(f*d->bs),x,(size_t)((uint64_t)n*d->bs));
    if(d->fail_home_after_commit&&in_journal&&n==1U&&
       memcmp(raw,OPENFS_JOURNAL_MAGIC,5U)==0&&raw[5U]==OPENFS_JOURNAL_COMMIT)
        d->commit_seen=1;
    return OPENFS_IO_OK;
}
static openfs_io_result_t f(void*c){(void)c;return OPENFS_IO_OK;}

static void arm_post_commit_home_failure(D*d,const openfs_superblock_t*s){
    d->journal_start=s->journal_start;
    d->journal_blocks=s->journal_blocks;
    d->fail_home_after_commit=1;
    d->commit_seen=0;
}
static void assert_fsck_clean(openfs_block_device_t*v,const openfs_superblock_t*s){
    uint64_t errors=0U;
    assert(openfs_fsck(v,s,&errors)==OPENFS_FSCK_OK&&errors==0U);
}
static void assert_xattr_block_allocated(openfs_block_device_t*v,const openfs_superblock_t*s,uint64_t ino,uint64_t*block_out){
    uint64_t count=(s->inode_table_blocks*(uint64_t)s->block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t inode;
    assert(openfs_inode_read(v,s->inode_table_start,ino,count,&inode)==OPENFS_INODE_OK);
    uint64_t block=openfs_inode_get_xattr_block(&inode);
    assert(block!=0U);
    int used=0;
    assert(openfs_bitmap_test(v,s->block_bitmap_start,s->block_bitmap_blocks,block,&used)==OPENFS_BITMAP_OK&&used==1);
    uint16_t refs=0U;
    assert(openfs_cow_refcount_get(v,s,block,&refs)==OPENFS_COW_OK&&refs==1U);
    *block_out=block;
}
static void test_post_commit_set_recovery(openfs_block_device_t*v,D*d){
    openfs_mount_t m={0};
    assert(openfs_mount(&m,v)==OPENFS_MOUNT_OK);
    uint64_t ino=0U;
    assert(openfs_path_create(v,&m.superblock,"/atomic-xattr",OPENFS_INODE_MODE_REGULAR|0644U,&ino)==OPENFS_PATH_OK);
    const char value[]="set-after-commit";
    arm_post_commit_home_failure(d,&m.superblock);
    assert(openfs_xattr_set(v,&m.superblock,ino,"user.atomic",value,sizeof(value),OPENFS_XATTR_CREATE)==OPENFS_XATTR_IO_ERROR);
    assert(d->commit_seen==1&&d->fail_home_after_commit==0);
    assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
    assert(openfs_mount(&m,v)==OPENFS_MOUNT_OK);

    char out[64]={0};size_t got=0U;
    assert(openfs_xattr_get(v,&m.superblock,ino,"user.atomic",out,sizeof(out),&got)==OPENFS_XATTR_OK);
    assert(got==sizeof(value)&&memcmp(out,value,sizeof(value))==0);
    uint64_t xblock=0U;
    assert_xattr_block_allocated(v,&m.superblock,ino,&xblock);
    assert_fsck_clean(v,&m.superblock);
    assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
}
static void test_post_commit_remove_recovery(openfs_block_device_t*v,D*d){
    openfs_mount_t m={0};
    assert(openfs_mount(&m,v)==OPENFS_MOUNT_OK);
    uint64_t ino=0U;
    assert(openfs_path_lookup(v,&m.superblock,"/atomic-xattr",&ino)==OPENFS_PATH_OK);
    uint64_t xblock=0U;
    assert_xattr_block_allocated(v,&m.superblock,ino,&xblock);

    arm_post_commit_home_failure(d,&m.superblock);
    assert(openfs_xattr_remove(v,&m.superblock,ino,"user.atomic")==OPENFS_XATTR_IO_ERROR);
    assert(d->commit_seen==1&&d->fail_home_after_commit==0);
    assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
    assert(openfs_mount(&m,v)==OPENFS_MOUNT_OK);
    char out[64]={0};size_t got=0U;
    assert(openfs_xattr_get(v,&m.superblock,ino,"user.atomic",out,sizeof(out),&got)==OPENFS_XATTR_NOT_FOUND);
    openfs_inode_t inode;
    uint64_t count=(m.superblock.inode_table_blocks*(uint64_t)m.superblock.block_size)/OPENFS_INODE_SIZE;
    assert(openfs_inode_read(v,m.superblock.inode_table_start,ino,count,&inode)==OPENFS_INODE_OK);
    assert(openfs_inode_get_xattr_block(&inode)==0U);
    int used=1;
    assert(openfs_bitmap_test(v,m.superblock.block_bitmap_start,m.superblock.block_bitmap_blocks,xblock,&used)==OPENFS_BITMAP_OK&&used==0);
    uint16_t refs=1U;
    assert(openfs_cow_refcount_get(v,&m.superblock,xblock,&refs)==OPENFS_COW_OK&&refs==0U);
    assert_fsck_clean(v,&m.superblock);
    assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
}

int main(void){
    D d={0};d.bs=4096U;d.bc=512U;d.b=calloc((size_t)d.bs,(size_t)d.bc);assert(d.b);
    openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};
    uint8_t u[16]={0};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);
    openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);
    uint64_t ino=0U;assert(openfs_path_create(&v,&s,"/x",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);
    const char a[]="hello";assert(openfs_xattr_set(&v,&s,ino,"user.test",a,sizeof(a),0)==OPENFS_XATTR_OK);
    char out[16];size_t got=0;assert(openfs_xattr_get(&v,&s,ino,"user.test",out,sizeof(out),&got)==OPENFS_XATTR_OK&&got==sizeof(a)&&memcmp(out,a,sizeof(a))==0);
    const char b[]="world";assert(openfs_xattr_set(&v,&s,ino,"user.test",b,sizeof(b),OPENFS_XATTR_REPLACE)==OPENFS_XATTR_OK);
    assert(openfs_xattr_get(&v,&s,ino,"user.test",out,sizeof(out),&got)==OPENFS_XATTR_OK&&memcmp(out,b,sizeof(b))==0);
    char names[64];size_t used=0;assert(openfs_xattr_list(&v,&s,ino,names,sizeof(names),&used)==OPENFS_XATTR_OK&&strcmp(names,"user.test")==0);
    assert(openfs_xattr_remove(&v,&s,ino,"user.test")==OPENFS_XATTR_OK);
    assert(openfs_xattr_get(&v,&s,ino,"user.test",out,sizeof(out),&got)==OPENFS_XATTR_NOT_FOUND);
    assert_fsck_clean(&v,&s);
    test_post_commit_set_recovery(&v,&d);
    test_post_commit_remove_recovery(&v,&d);
    free(d.b);
    return 0;
}
