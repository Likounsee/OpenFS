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
    int tx_used=0;
    assert(openfs_bitmap_test(&dev,mount.superblock.block_bitmap_start,mount.superblock.block_bitmap_blocks,tx_block,&tx_used)==OPENFS_BITMAP_OK&&tx_used==0);
    assert(openfs_transaction_abort(&tx)==OPENFS_TRANSACTION_OK);
    assert(openfs_bitmap_test(&dev,mount.superblock.block_bitmap_start,mount.superblock.block_bitmap_blocks,tx_block,&tx_used)==OPENFS_BITMAP_OK&&tx_used==0);
    assert(openfs_transaction_begin(&tx,&dev,&tx_journal)==OPENFS_TRANSACTION_OK);
    assert(openfs_alloc_block_tx(&tx,&mount.superblock,&tx_block)==OPENFS_ALLOC_OK);
    assert(openfs_transaction_commit(&tx)==OPENFS_TRANSACTION_OK);
    assert(openfs_bitmap_test(&dev,mount.superblock.block_bitmap_start,mount.superblock.block_bitmap_blocks,tx_block,&tx_used)==OPENFS_BITMAP_OK&&tx_used==1);
    assert(openfs_free_block(&dev,&mount.superblock,tx_block)==OPENFS_ALLOC_OK);

    /* File transactions must carry allocation/refcount changes in the same WAL. */
    uint64_t file_ino=0U;
    assert(openfs_inode_alloc(&dev,&mount.superblock,mount.superblock.root_inode,
                              OPENFS_INODE_MODE_REGULAR|0600U,&file_ino)==OPENFS_INODE_ALLOC_OK);
    uint64_t inode_count=(mount.superblock.inode_table_blocks*(uint64_t)mount.superblock.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t before;
    assert(openfs_inode_read(&dev,mount.superblock.inode_table_start,file_ino,inode_count,&before)==OPENFS_INODE_OK);
    assert(before.blocks==0U);
    assert(openfs_transaction_begin(&tx,&dev,&tx_journal)==OPENFS_TRANSACTION_OK);
    uint8_t payload[4096]; memset(payload,0x5a,sizeof(payload));
    openfs_inode_t working=before;
    assert(openfs_file_write_tx(&tx,&mount.superblock,&working,0U,payload,sizeof(payload))==OPENFS_FILE_OK);    assert(working.blocks==1U);
    uint64_t staged_block=0U;
    assert(openfs_file_map_block_device(openfs_transaction_device(&tx),&mount.superblock,
                                        &working,0U,&staged_block)==OPENFS_FILE_OK);
    int staged_used=0;
    assert(openfs_bitmap_test(&dev,mount.superblock.block_bitmap_start,
                              mount.superblock.block_bitmap_blocks,staged_block,
                              &staged_used)==OPENFS_BITMAP_OK&&staged_used==0);
    assert(openfs_transaction_abort(&tx)==OPENFS_TRANSACTION_OK);
    openfs_inode_t after;
    assert(openfs_inode_read(&dev,mount.superblock.inode_table_start,file_ino,inode_count,&after)==OPENFS_INODE_OK);
    assert(after.blocks==0U&&after.size==0U);
    assert(openfs_bitmap_test(&dev,mount.superblock.block_bitmap_start,
                              mount.superblock.block_bitmap_blocks,staged_block,
                              &staged_used)==OPENFS_BITMAP_OK&&staged_used==0);
    assert(openfs_inode_free(&dev,&mount.superblock,file_ino)==OPENFS_INODE_ALLOC_OK);

    uint64_t target=mount.superblock.data_start;
    uint64_t bitmap_block=mount.superblock.block_bitmap_start+
        (target/((uint64_t)mount.superblock.block_size*8U));
    disk.fail_block=bitmap_block;
    disk.fail_enabled=1;

    uint64_t allocated=0U;
    assert(openfs_alloc_block(&dev,&mount.superblock,&allocated)==OPENFS_ALLOC_CORRUPT);
    disk.fail_enabled=0;
    assert(openfs_unmount(&mount)==OPENFS_MOUNT_OK);
    openfs_mount_t recovered={0};
    assert(openfs_mount(&recovered,&dev)==OPENFS_MOUNT_OK);
    int used=0;
    assert(openfs_bitmap_test(&dev,recovered.superblock.block_bitmap_start,
                              recovered.superblock.block_bitmap_blocks,target,&used)==OPENFS_BITMAP_OK);
    assert(used==1);
    uint16_t refs=0U;
    assert(openfs_cow_refcount_get(&dev,&recovered.superblock,target,&refs)==OPENFS_COW_OK);
    assert(refs==1U);
    assert(openfs_unmount(&recovered)==OPENFS_MOUNT_OK);
    free(disk.bytes);
    return 0;
}
