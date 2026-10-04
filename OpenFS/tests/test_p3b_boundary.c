#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/format.h"
#include "openfs/journal.h"
#include "openfs/path.h"
#include "openfs/dir.h"
#include "openfs/link.h"
#include "openfs/extent.h"
#include "openfs/fsck.h"

typedef struct { uint8_t *b; uint32_t bs; uint64_t bc; } disk_t;

static openfs_io_result_t rd(void *ctx,uint64_t first,uint32_t count,void *out){
    disk_t *d=(disk_t *)ctx;
    if(count==0U||first>=d->bc||(uint64_t)count>d->bc-first)return OPENFS_IO_OUT_OF_RANGE;
    memcpy(out,d->b+(size_t)(first*d->bs),(size_t)((uint64_t)count*d->bs));
    return OPENFS_IO_OK;
}
static openfs_io_result_t wr(void *ctx,uint64_t first,uint32_t count,const void *in){
    disk_t *d=(disk_t *)ctx;
    if(count==0U||first>=d->bc||(uint64_t)count>d->bc-first)return OPENFS_IO_OUT_OF_RANGE;
    memcpy(d->b+(size_t)(first*d->bs),in,(size_t)((uint64_t)count*d->bs));
    return OPENFS_IO_OK;
}
static openfs_io_result_t fl(void *ctx){(void)ctx;return OPENFS_IO_OK;}

static openfs_block_device_t dev(disk_t *d){
    openfs_block_device_t v={d,d->bs,d->bc,rd,wr,fl};
    return v;
}

static void setup(disk_t *d,openfs_block_device_t *v,openfs_superblock_t *s,uint64_t blocks){
    memset(d,0,sizeof(*d));d->bs=4096U;d->bc=blocks;
    d->b=calloc((size_t)d->bs,(size_t)d->bc);assert(d->b!=NULL);
    *v=dev(d);
    uint8_t uuid[16]={0};
    assert(openfs_format(v,uuid)==OPENFS_FORMAT_OK);
    assert(openfs_read_superblock(v,s)==OPENFS_FORMAT_OK);
}

static void make_name(char *path,char fill){
    path[0]='/';
    memset(path+1,fill,OPENFS_DIR_NAME_MAX);
    path[OPENFS_DIR_NAME_MAX+1U]='\0';
}

static void journal_capacity_and_limits(void){
    disk_t d;openfs_block_device_t v;openfs_superblock_t s;setup(&d,&v,&s,128U);
    openfs_journal_t j;
    uint64_t tx=0U;
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);

    /* UINT64_MAX is a terminal txid: the following begin must be refused. */
    j.transaction_id=UINT64_MAX-1U;
    assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);
    assert(tx==UINT64_MAX);
    assert(j.transaction_id==UINT64_MAX);
    assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);
    assert(j.sequence!=0U);
    assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_FULL);
    assert(j.transaction_id==UINT64_MAX);

    /* Reset only the in-memory boundary for the independent sequence test. */
    j.transaction_id=0U;
    assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);
    assert(tx==1U);

    /* sequence UINT64_MAX is also a hard refusal boundary. */
    j.sequence=UINT64_MAX-1U;
    assert(openfs_journal_write(&j,&v,tx,"x",1U)==OPENFS_JOURNAL_OK);
    assert(j.sequence==UINT64_MAX);
    assert(openfs_journal_write(&j,&v,tx,"y",1U)==OPENFS_JOURNAL_FULL);
    assert(j.sequence==UINT64_MAX);
    assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_FULL);
    j.active_transaction_id=0U;
    assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);

    /* Fill the journal exactly: BEGIN + DATA records + COMMIT. */
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);
    while(j.next_record+1U<j.journal_blocks)
        assert(openfs_journal_write(&j,&v,tx,"x",1U)==OPENFS_JOURNAL_OK);
    assert(j.next_record+1U==j.journal_blocks);
    assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
    assert(j.next_record==j.journal_blocks);
    /* A committed full journal still requires checkpoint before another BEGIN. */
    assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_INVALID_ARGUMENT);
    assert(j.next_record==j.journal_blocks);
    assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);
    assert(j.next_record==0U);
    free(d.b);
}

static void successive_transactions(void){
    disk_t d;openfs_block_device_t v;openfs_superblock_t s;setup(&d,&v,&s,256U);
    openfs_journal_t j;uint64_t tx=0U;
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    for(uint64_t expected=1U;expected<=3U;expected++){
        assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);
        assert(tx==expected);
        assert(j.transaction_id==expected);
        assert(j.sequence==expected*3U-2U);
        assert(openfs_journal_write(&j,&v,tx,"x",1U)==OPENFS_JOURNAL_OK);
        assert(j.sequence==expected*3U-1U);
        assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
        assert(j.sequence==expected*3U);
        assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);
        assert(j.next_record==0U);
    }
    /* Checkpointed transactions keep the monotone counters within one journal lifetime. */
    assert(j.transaction_id==3U);
    assert(j.sequence==9U);

    /* Before checkpoint, a remount must recover the committed journal counters. */
    assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);
    assert(tx==4U);
    assert(openfs_journal_write(&j,&v,tx,"x",1U)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
    openfs_journal_t reopened;
    assert(openfs_journal_open(&reopened,&v,&s)==OPENFS_JOURNAL_OK);
    assert(reopened.transaction_id==4U);
    assert(reopened.sequence==12U);
    assert(reopened.next_record==3U);
    assert(openfs_journal_checkpoint(&reopened,&v)==OPENFS_JOURNAL_OK);
    assert(reopened.next_record==0U);
    free(d.b);
}

static void name_and_path_boundaries(void){
    disk_t d;openfs_block_device_t v;openfs_superblock_t s;setup(&d,&v,&s,128U);
    char name[OPENFS_DIR_NAME_MAX+2U];
    memset(name,'n',sizeof(name));name[OPENFS_DIR_NAME_MAX]='\0';
    char path[OPENFS_PATH_MAX+2U];
    uint64_t ino=0U;
    size_t used=1U;
    path[0]='/';

    /* Build 15 maximal components so the final component isolates PATH_MAX. */
    for(unsigned i=0U;i<17U;i++){
        memset(path+used,'d',OPENFS_DIR_NAME_MAX);
        used+=OPENFS_DIR_NAME_MAX;
        path[used++]='/';
        path[used]='\0';
        assert(strlen(path)<OPENFS_PATH_MAX);
        assert(openfs_path_mkdir(&v,&s,path,&ino)==OPENFS_PATH_OK);
    }
    memset(path+used,'e',OPENFS_DIR_NAME_MAX-9U);
    used+=OPENFS_DIR_NAME_MAX-9U;
    path[used]='\0';
    assert(used==OPENFS_PATH_MAX-1U);
    assert(openfs_path_create(&v,&s,path,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);

    path[used++]='e';
    path[used]='\0';
    assert(used==OPENFS_PATH_MAX);
    assert(openfs_path_create(&v,&s,path,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_INVALID_ARGUMENT);

    path[used++]='e';
    path[used]='\0';
    assert(used==OPENFS_PATH_MAX+1U);
    assert(openfs_path_create(&v,&s,path,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_INVALID_ARGUMENT);

    char p[OPENFS_DIR_NAME_MAX+4U]="/";
    memcpy(p+1,name,OPENFS_DIR_NAME_MAX+1U);
    p[OPENFS_DIR_NAME_MAX+2U]='\0';
    assert(strlen(name)==OPENFS_DIR_NAME_MAX);
    assert(openfs_path_create(&v,&s,p,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);

    name[OPENFS_DIR_NAME_MAX]='x';name[OPENFS_DIR_NAME_MAX+1U]='\0';
    memcpy(p+1,name,OPENFS_DIR_NAME_MAX+1U);p[OPENFS_DIR_NAME_MAX+2U]='\0';
    assert(openfs_path_create(&v,&s,p,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_NAME_TOO_LONG);

    /* Restore a valid source path after the explicit +1 name rejection. */
    name[OPENFS_DIR_NAME_MAX]='\0';
    memcpy(p+1,name,OPENFS_DIR_NAME_MAX+1U);p[OPENFS_DIR_NAME_MAX+2U]='\0';

    char mkdir_path[OPENFS_DIR_NAME_MAX+2U];make_name(mkdir_path,'m');
    assert(openfs_path_mkdir(&v,&s,mkdir_path,&ino)==OPENFS_PATH_OK);
    char link_path[OPENFS_DIR_NAME_MAX+2U];make_name(link_path,'l');
    assert(openfs_link(&v,&s,p,link_path)==OPENFS_PATH_OK);
    char symlink_path[OPENFS_DIR_NAME_MAX+2U];make_name(symlink_path,'s');
    assert(openfs_symlink(&v,&s,p,symlink_path)==OPENFS_PATH_OK);
    char renamed_path[OPENFS_DIR_NAME_MAX+2U];make_name(renamed_path,'r');
    assert(openfs_path_rename(&v,&s,p,renamed_path)==OPENFS_PATH_OK);

    char too_long[OPENFS_DIR_NAME_MAX+3U];make_name(too_long,'t');
    too_long[OPENFS_DIR_NAME_MAX+1U]='t';too_long[OPENFS_DIR_NAME_MAX+2U]='\0';
    assert(openfs_path_mkdir(&v,&s,too_long,&ino)==OPENFS_PATH_NAME_TOO_LONG);
    free(d.b);
}

static void inode_generation_boundary(void){
    disk_t d;openfs_block_device_t v;openfs_superblock_t s;setup(&d,&v,&s,128U);
    uint64_t first=0U, second=0U;
    assert(openfs_path_create(&v,&s,"/generation-a",OPENFS_INODE_MODE_REGULAR,&first)==OPENFS_PATH_OK);
    openfs_inode_t a;assert(openfs_inode_read(&v,s.inode_table_start,first,64U,&a)==OPENFS_INODE_OK);
    assert(a.generation==1U);
    assert(openfs_path_unlink(&v,&s,"/generation-a")==OPENFS_PATH_OK);
    assert(openfs_path_lookup(&v,&s,"/generation-a",&second)==OPENFS_PATH_NOT_FOUND);
    assert(openfs_path_create(&v,&s,"/generation-b",OPENFS_INODE_MODE_REGULAR,&second)==OPENFS_PATH_OK);
    assert(second==first);
    openfs_inode_t b;assert(openfs_inode_read(&v,s.inode_table_start,second,64U,&b)==OPENFS_INODE_OK);
    assert(b.generation==2U);

    /* An old inode+generation pair must not resolve to the reused inode. */
    openfs_inode_t root;
    assert(openfs_inode_read(&v,s.inode_table_start,s.root_inode,64U,&root)==OPENFS_INODE_OK);
    openfs_dir_entry_t stale={first,a.generation,1U};
    assert(openfs_dir_add(&v,&s,&root,"stale-generation",&stale)==OPENFS_DIR_OK);
    assert(openfs_path_lookup(&v,&s,"/stale-generation",&second)==OPENFS_PATH_CORRUPT);

    uint64_t errors=0U;
    assert(openfs_fsck(&v,&s,&errors)==OPENFS_FSCK_CORRUPT);
    assert(errors!=0U);
    free(d.b);
}

static void extent_boundary(void){
    disk_t d;openfs_block_device_t v;openfs_superblock_t s;setup(&d,&v,&s,256U);
    assert(openfs_extent_tree_capacity(4096U)==169U);
    assert(openfs_extent_tree_capacity(4096U-1U)==169U);

    openfs_inode_t inode;memset(&inode,0,sizeof(inode));
    inode.inode_number=2U;inode.generation=1U;inode.mode=OPENFS_INODE_MODE_REGULAR;
    inode.link_count=1U;inode.flags=OPENFS_INODE_FLAG_EXTENT_TREE|OPENFS_INODE_FLAG_HAS_EXTENTS;
    inode.extent_count=0U;
    inode.blocks=173U;
    for(uint32_t i=0U;i<OPENFS_INODE_TREE_INLINE_EXTENT_MAX;i++){
        openfs_extent_t e={(uint64_t)i,s.data_start+1U+(uint64_t)i,1U};
        assert(openfs_inode_set_extent(&inode,i,&e)==OPENFS_EXTENT_OK);
    }
    assert(openfs_inode_set_extent_tree_root(&inode,s.data_start)==OPENFS_EXTENT_OK);
    inode.extent_count=OPENFS_INODE_TREE_INLINE_EXTENT_MAX+169U;

    openfs_extent_t extents[169];
    for(uint32_t i=0U;i<169U;i++){
        extents[i].logical_start=4U+(uint64_t)i;
        extents[i].physical_start=s.data_start+5U+(uint64_t)i;
        extents[i].block_count=1U;
    }
    assert(openfs_extent_tree_write(&v,&inode,extents,169U)==OPENFS_EXTENT_OK);
    openfs_extent_t last;
    assert(openfs_extent_tree_read(&v,&inode,168U,&last)==OPENFS_EXTENT_OK);
    assert(last.logical_start==172U);
    assert(last.physical_start==s.data_start+173U);

    /* Capacity + 1 must be rejected before the tree can be published. */
    assert(openfs_extent_tree_write(&v,&inode,extents,170U)==OPENFS_EXTENT_CORRUPT);
    free(d.b);
}

int main(void){
    journal_capacity_and_limits();
    successive_transactions();
    name_and_path_boundaries();
    extent_boundary();
    inode_generation_boundary();
    return 0;
}
