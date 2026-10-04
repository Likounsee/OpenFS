#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/format.h"
#include "openfs/journal.h"
#include "openfs/path.h"
#include "openfs/dir.h"
#include "openfs/extent.h"

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

static void setup(disk_t *d,openfs_block_device_t *v,openfs_superblock_t *s){
    memset(d,0,sizeof(*d));d->bs=4096U;d->bc=128U;
    d->b=calloc((size_t)d->bs,(size_t)d->bc);assert(d->b!=NULL);
    *v=dev(d);
    uint8_t uuid[16]={0};
    assert(openfs_format(v,uuid)==OPENFS_FORMAT_OK);
    assert(openfs_read_superblock(v,s)==OPENFS_FORMAT_OK);
}

static void journal_capacity_and_limits(void){
    disk_t d;openfs_block_device_t v;openfs_superblock_t s;setup(&d,&v,&s);
    openfs_journal_t j;
    uint64_t tx=0U;
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);

    /* The transaction-id limit is a refusal boundary, not a wrap boundary. */
    j.transaction_id=UINT64_MAX-1U;
    assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);
    assert(tx==UINT64_MAX);
    assert(j.transaction_id==UINT64_MAX);
    assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);
    assert(j.sequence!=0U);
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

    /* Leave exactly one journal slot: DATA needs one record, a block write needs two. */
    assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);
    while(j.next_record+1U<j.journal_blocks)
        assert(openfs_journal_write(&j,&v,tx,"x",1U)==OPENFS_JOURNAL_OK);
    uint64_t before=j.next_record;
    uint8_t block[4096]={0};
    assert(openfs_journal_write_block(&j,&v,tx,s.data_start,block)==OPENFS_JOURNAL_FULL);
    assert(j.next_record==before);
    assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);
    assert(openfs_journal_checkpoint(&j,&v)==OPENFS_JOURNAL_OK);
    free(d.b);
}

static void name_and_path_boundaries(void){
    disk_t d;openfs_block_device_t v;openfs_superblock_t s;setup(&d,&v,&s);
    char name[OPENFS_DIR_NAME_MAX+2U];
    memset(name,'n',sizeof(name));name[OPENFS_DIR_NAME_MAX]='\0';
    char path[OPENFS_PATH_MAX+2U];
    path[0]='/';memset(path+1,'a',OPENFS_PATH_MAX-1U);path[OPENFS_PATH_MAX]='\0';
    uint64_t ino=0U;

    assert(openfs_path_create(&v,&s,path,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_NAME_TOO_LONG);

    path[0]='/';
    size_t used=1U;
    for(unsigned i=0U;i<2047U;i++){path[used++]='a';path[used++]='/';}
    path[used]='\0';
    assert(strlen(path)==OPENFS_PATH_MAX-1U);
    assert(openfs_path_lookup(&v,&s,path,&ino)==OPENFS_PATH_NOT_FOUND);

    path[OPENFS_PATH_MAX-1U]='a';path[OPENFS_PATH_MAX]='\0';
    assert(strlen(path)==OPENFS_PATH_MAX);
    assert(openfs_path_lookup(&v,&s,path,&ino)==OPENFS_PATH_NAME_TOO_LONG);

    char p[OPENFS_DIR_NAME_MAX+4U]="/";
    memcpy(p+1,name,OPENFS_DIR_NAME_MAX+1U);
    p[OPENFS_DIR_NAME_MAX+2U]='\0';
    assert(strlen(name)==OPENFS_DIR_NAME_MAX);
    assert(openfs_path_create(&v,&s,p,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);

    name[OPENFS_DIR_NAME_MAX]='x';name[OPENFS_DIR_NAME_MAX+1U]='\0';
    memcpy(p+1,name,OPENFS_DIR_NAME_MAX+1U);p[OPENFS_DIR_NAME_MAX+2U]='\0';
    assert(openfs_path_create(&v,&s,p,OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_NAME_TOO_LONG);
    free(d.b);
}

static void extent_boundary(void){
    disk_t d;openfs_block_device_t v;openfs_superblock_t s;setup(&d,&v,&s);
    assert(openfs_extent_tree_capacity(4096U)==169U);
    assert(openfs_extent_tree_capacity(4096U-1U)==168U);

    openfs_inode_t inode;memset(&inode,0,sizeof(inode));
    inode.inode_number=2U;inode.generation=1U;inode.mode=OPENFS_INODE_MODE_REGULAR;
    inode.link_count=1U;inode.flags=OPENFS_INODE_FLAG_EXTENT_TREE|OPENFS_INODE_FLAG_HAS_EXTENTS;
    inode.extent_count=OPENFS_INODE_TREE_INLINE_EXTENT_MAX+1U;
    assert(openfs_inode_set_extent_tree_root(&inode,s.data_start)==OPENFS_EXTENT_OK);

    openfs_extent_t extents[169];
    for(uint32_t i=0U;i<169U;i++){
        extents[i].logical_start=(uint64_t)i+1U;
        extents[i].physical_start=s.data_start+1U+(uint64_t)i;
        extents[i].block_count=1U;
    }
    /* A count above the single-leaf capacity must be rejected before I/O. */
    assert(openfs_extent_tree_write(&v,&inode,extents,170U)==OPENFS_EXTENT_CORRUPT);
    free(d.b);
}

int main(void){
    journal_capacity_and_limits();
    name_and_path_boundaries();
    extent_boundary();
    return 0;
}
