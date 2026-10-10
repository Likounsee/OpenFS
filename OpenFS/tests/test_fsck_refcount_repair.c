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
#include "openfs/bitmap.h"

typedef struct { uint8_t *b; uint32_t bs; uint64_t bc; uint64_t fail_block; int fail_enabled; uint64_t fail_read_block; int fail_read_enabled; } disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*o){disk_t*d=c;if(!d||!o||!n||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;if(d->fail_read_enabled&&f<=d->fail_read_block&&d->fail_read_block-f<n)return OPENFS_IO_IO_ERROR;memcpy(o,d->b+(size_t)(f*d->bs),(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*i){disk_t*d=c;if(!d||!i||!n||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;if(d->fail_enabled&&f==d->fail_block)return OPENFS_IO_IO_ERROR;memcpy(d->b+(size_t)(f*d->bs),i,(size_t)n*d->bs);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){(void)c;return OPENFS_IO_OK;}
static void reject_mismatched_runtime_device(void){disk_t d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);openfs_block_device_t dev={&d,d.bs,d.bc,rd,wr,fl};uint8_t uuid[16]={0x31};assert(openfs_format(&dev,uuid)==OPENFS_FORMAT_OK);openfs_mount_t m={0};assert(openfs_mount(&m,&dev)==OPENFS_MOUNT_OK);openfs_block_device_t alias=dev;uint64_t tail=m.superblock.total_blocks;assert(openfs_bitmap_set(&dev,m.superblock.block_bitmap_start,m.superblock.block_bitmap_blocks,tail,1)==OPENFS_BITMAP_OK);int set=0;assert(openfs_bitmap_test(&dev,m.superblock.block_bitmap_start,m.superblock.block_bitmap_blocks,tail,&set)==OPENFS_BITMAP_OK&&set);uint64_t errors=0;assert(openfs_fsck_repair_bitmap_tails(&alias,&m.superblock,&errors)==OPENFS_FSCK_IO_ERROR);assert(openfs_bitmap_test(&dev,m.superblock.block_bitmap_start,m.superblock.block_bitmap_blocks,tail,&set)==OPENFS_BITMAP_OK&&set);assert(openfs_bitmap_set(&dev,m.superblock.block_bitmap_start,m.superblock.block_bitmap_blocks,tail,0)==OPENFS_BITMAP_OK);assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);free(d.b);}
int main(void){reject_mismatched_runtime_device();disk_t d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);openfs_block_device_t dev={&d,d.bs,d.bc,rd,wr,fl};uint8_t uuid[16]={0x42};assert(openfs_format(&dev,uuid)==OPENFS_FORMAT_OK);openfs_mount_t m={0};assert(openfs_mount(&m,&dev)==OPENFS_MOUNT_OK);assert((m.superblock.feature_flags&OPENFS_FEATURE_COW)!=0U);uint64_t ino=0;assert(openfs_path_create(&dev,&m.superblock,"/f",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);uint64_t ic=(m.superblock.inode_table_blocks*(uint64_t)m.superblock.block_size)/OPENFS_INODE_SIZE;openfs_inode_t in;assert(openfs_inode_read(&dev,m.superblock.inode_table_start,ino,ic,&in)==OPENFS_INODE_OK);uint8_t data[4096];memset(data,0xA7,sizeof(data));assert(openfs_file_write(&dev,&m.superblock,&in,0,data,sizeof(data))==OPENFS_FILE_OK);assert(openfs_inode_read(&dev,m.superblock.inode_table_start,ino,ic,&in)==OPENFS_INODE_OK);openfs_extent_t e;assert(openfs_inode_get_extent(&in,0,&e)==OPENFS_EXTENT_OK);uint16_t refs=0;assert(openfs_cow_refcount_get(&dev,&m.superblock,e.physical_start,&refs)==OPENFS_COW_OK&&refs==1U);uint64_t idx=e.physical_start-m.superblock.data_start,epb=m.superblock.block_size/2U,tb=m.superblock.refcount_start+idx/epb;uint32_t off=(uint32_t)((idx%epb)*2U);uint8_t raw[4096];assert(dev.read(dev.context,tb,1,raw)==OPENFS_IO_OK);raw[off]=0;raw[off+1]=0;assert(dev.write(dev.context,tb,1,raw)==OPENFS_IO_OK);assert(dev.flush(dev.context)==OPENFS_IO_OK);uint64_t errors=0;d.fail_read_block=m.superblock.inode_table_start;d.fail_read_enabled=1;assert(openfs_fsck_repair_cow_refcounts(&dev,&m.superblock,&errors)==OPENFS_FSCK_IO_ERROR);d.fail_read_enabled=0;assert(openfs_fsck(&dev,&m.superblock,&errors)==OPENFS_FSCK_CORRUPT&&errors>0);d.fail_enabled=1;d.fail_block=m.superblock.journal_start;assert(openfs_fsck_repair_cow_refcounts(&dev,&m.superblock,&errors)!=OPENFS_FSCK_OK);assert(openfs_cow_refcount_get(&dev,&m.superblock,e.physical_start,&refs)==OPENFS_COW_OK&&refs==0U);d.fail_enabled=0;assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);assert(openfs_mount(&m,&dev)==OPENFS_MOUNT_OK);errors=0;assert(openfs_fsck_repair_cow_refcounts(&dev,&m.superblock,&errors)==OPENFS_FSCK_OK&&errors==0);assert(openfs_cow_refcount_get(&dev,&m.superblock,e.physical_start,&refs)==OPENFS_COW_OK&&refs==1U);assert(openfs_inode_read(&dev,m.superblock.inode_table_start,ino,ic,&in)==OPENFS_INODE_OK);
    /* Refcount repair must reject bitmap/ownership disagreement without publishing a partial repair. */
    assert(openfs_cow_refcount_set(&dev,&m.superblock,e.physical_start,2U)==OPENFS_COW_OK);
    assert(openfs_bitmap_set(&dev,m.superblock.block_bitmap_start,m.superblock.block_bitmap_blocks,e.physical_start,0)==OPENFS_BITMAP_OK);
    errors=0;assert(openfs_fsck_repair_cow_refcounts(&dev,&m.superblock,&errors)==OPENFS_FSCK_CORRUPT);
    assert(openfs_cow_refcount_get(&dev,&m.superblock,e.physical_start,&refs)==OPENFS_COW_OK&&refs==2U);
    assert(openfs_bitmap_set(&dev,m.superblock.block_bitmap_start,m.superblock.block_bitmap_blocks,e.physical_start,1)==OPENFS_BITMAP_OK);
    assert(openfs_cow_refcount_set(&dev,&m.superblock,e.physical_start,1U)==OPENFS_COW_OK);
    /* An allocated block with no inode-derived owner is equally unsafe to repair
     * automatically: preserve it and require explicit orphan recovery. */
    uint64_t orphan_block = 0U;
    int orphan_found = 0;
    for (uint64_t candidate = m.superblock.data_start;
         candidate < m.superblock.data_start + m.superblock.data_blocks;
         candidate++) {
        int candidate_used = 0;
        if (candidate == m.superblock.metadata_root_block) continue;
        assert(openfs_bitmap_test(&dev, m.superblock.block_bitmap_start,
                                  m.superblock.block_bitmap_blocks,
                                  candidate, &candidate_used) == OPENFS_BITMAP_OK);
        if (!candidate_used) {
            orphan_block = candidate;
            orphan_found = 1;
            break;
        }
    }
    assert(orphan_found);
    assert(openfs_bitmap_set(&dev, m.superblock.block_bitmap_start,
                             m.superblock.block_bitmap_blocks,
                             orphan_block, 1) == OPENFS_BITMAP_OK);
    assert(openfs_cow_refcount_set(&dev, &m.superblock, orphan_block, 1U) == OPENFS_COW_OK);
    errors = 0U;
    assert(openfs_fsck_repair_cow_refcounts(&dev, &m.superblock, &errors) == OPENFS_FSCK_CORRUPT);
    assert(openfs_cow_refcount_get(&dev, &m.superblock, orphan_block, &refs) == OPENFS_COW_OK && refs == 1U);
    int orphan_used = 0;
    assert(openfs_bitmap_test(&dev, m.superblock.block_bitmap_start,
                              m.superblock.block_bitmap_blocks,
                              orphan_block, &orphan_used) == OPENFS_BITMAP_OK && orphan_used);
    assert(openfs_cow_refcount_set(&dev, &m.superblock, orphan_block, 0U) == OPENFS_COW_OK);
    assert(openfs_bitmap_set(&dev, m.superblock.block_bitmap_start,
                             m.superblock.block_bitmap_blocks,
                             orphan_block, 0) == OPENFS_BITMAP_OK);
    openfs_extent_t saved=e;openfs_extent_t invalid=e;invalid.physical_start=m.superblock.data_start+m.superblock.data_blocks+10U;assert(openfs_inode_set_extent(&in,0U,&invalid)==OPENFS_EXTENT_OK);assert(openfs_inode_write(&dev,m.superblock.inode_table_start,ino,&in)==OPENFS_INODE_OK);errors=0;assert(openfs_fsck_repair_cow_refcounts(&dev,&m.superblock,&errors)==OPENFS_FSCK_CORRUPT);assert(openfs_inode_read(&dev,m.superblock.inode_table_start,ino,ic,&in)==OPENFS_INODE_OK);assert(openfs_inode_set_extent(&in,0U,&saved)==OPENFS_EXTENT_OK);assert(openfs_inode_write(&dev,m.superblock.inode_table_start,ino,&in)==OPENFS_INODE_OK);assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);free(d.b);return 0;}