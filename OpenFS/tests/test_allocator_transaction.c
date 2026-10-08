#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/allocator.h"
#include "openfs/cow.h"
#include "openfs/format.h"
#include "openfs/mount.h"
#include "openfs/bitmap.h"
#include "openfs/file.h"
#include "openfs/inode_alloc.h"

typedef struct {
    uint8_t *bytes;
    uint32_t bs;
    uint64_t blocks;
    uint64_t fail_block;
    int fail_enabled;
} disk_t;

static openfs_io_result_t rd(void *ctx,uint64_t first,uint32_t count,void *out)
{
    disk_t *d=(disk_t *)ctx;
    if(d==NULL||out==NULL||count==0U||first>=d->blocks||(uint64_t)count>d->blocks-first)return OPENFS_IO_OUT_OF_RANGE;
    memcpy(out,d->bytes+(size_t)(first*d->bs),(size_t)count*d->bs);
    return OPENFS_IO_OK;
}

static openfs_io_result_t wr(void *ctx,uint64_t first,uint32_t count,const void *in)
{
    disk_t *d=(disk_t *)ctx;
    if(d==NULL||in==NULL||count==0U||first>=d->blocks||(uint64_t)count>d->blocks-first)return OPENFS_IO_OUT_OF_RANGE;
    if(d->fail_enabled&&count==1U&&first==d->fail_block)return OPENFS_IO_IO_ERROR;
    memcpy(d->bytes+(size_t)(first*d->bs),in,(size_t)count*d->bs);
    return OPENFS_IO_OK;
}

static openfs_io_result_t fl(void *ctx){(void)ctx;return OPENFS_IO_OK;}

int main(void)
{
    disk_t disk={0};
    disk.bs=4096U;
    disk.blocks=128U;
    disk.bytes=(uint8_t *)calloc((size_t)disk.bs,disk.blocks);
    assert(disk.bytes!=NULL);
    openfs_block_device_t dev={&disk,disk.bs,disk.blocks,rd,wr,fl};
    uint8_t uuid[16]={0};
    assert(openfs_format(&dev,uuid)==OPENFS_FORMAT_OK);

    openfs_mount_t mount={0};
    assert(openfs_mount(&mount,&dev)==OPENFS_MOUNT_OK);
    /* Transaction-aware allocation must not create a nested WAL transaction. */
    openfs_journal_t tx_journal;
    assert(openfs_journal_open(&tx_journal,&dev,&mount.superblock)==OPENFS_JOURNAL_OK);
    openfs_transaction_t tx;
    assert(openfs_transaction_begin(&tx,&dev,&tx_journal)==OPENFS_TRANSACTION_OK);
    uint64_t tx_block=0U;
    assert(openfs_alloc_block_tx(&tx,&mount.superblock,&tx_block)==OPENFS_ALLOC_OK);
    if((mount.superblock.feature_flags&OPENFS_FEATURE_COW)!=0U){uint16_t refs=99U;assert(openfs_cow_refcount_get(&dev,&mount.superblock,tx_block,&refs)==OPENFS_COW_OK&&refs==0U);}
    int tx_used=0;
    assert(openfs_bitmap_test(&dev,mount.superblock.block_bitmap_start,mount.superblock.block_bitmap_blocks,tx_block,&tx_used)==OPENFS_BITMAP_OK&&tx_used==0);
    assert(openfs_transaction_abort(&tx)==OPENFS_TRANSACTION_OK);
    assert(openfs_bitmap_test(&dev,mount.superblock.block_bitmap_start,mount.superblock.block_bitmap_blocks,tx_block,&tx_used)==OPENFS_BITMAP_OK&&tx_used==0);
    assert(openfs_transaction_begin(&tx,&dev,&tx_journal)==OPENFS_TRANSACTION_OK);
    assert(openfs_alloc_block_tx(&tx,&mount.superblock,&tx_block)==OPENFS_ALLOC_OK);
    assert(openfs_transaction_commit(&tx)==OPENFS_TRANSACTION_OK);
    if((mount.superblock.feature_flags&OPENFS_FEATURE_COW)!=0U){uint16_t refs=0U;assert(openfs_cow_refcount_get(&dev,&mount.superblock,tx_block,&refs)==OPENFS_COW_OK&&refs==1U);}
    assert(openfs_bitmap_test(&dev,mount.superblock.block_bitmap_start,mount.superblock.block_bitmap_blocks,tx_block,&tx_used)==OPENFS_BITMAP_OK&&tx_used==1);
    assert(openfs_transaction_begin(&tx,&dev,&tx_journal)==OPENFS_TRANSACTION_OK);
    assert(openfs_free_block_tx(&tx,&mount.superblock,tx_block)==OPENFS_ALLOC_OK);
    assert(openfs_transaction_commit(&tx)==OPENFS_TRANSACTION_OK);
    if((mount.superblock.feature_flags&OPENFS_FEATURE_COW)!=0U){uint16_t refs=99U;assert(openfs_cow_refcount_get(&dev,&mount.superblock,tx_block,&refs)==OPENFS_COW_OK&&refs==0U);}
    assert(openfs_bitmap_test(&dev,mount.superblock.block_bitmap_start,mount.superblock.block_bitmap_blocks,tx_block,&tx_used)==OPENFS_BITMAP_OK&&tx_used==0);
    assert(openfs_unmount(&mount)==OPENFS_MOUNT_OK);
    free(disk.bytes);
    return 0;
}
