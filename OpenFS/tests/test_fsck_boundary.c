#include <assert.h>
#include <stdio.h>
#define TEST_ASSERT(expr) do { if(!(expr)) { fprintf(stderr, "test assertion failed: %s\n", #expr); abort(); } } while(0)
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/fsck.h"
#include "openfs/format.h"
#include "openfs/bitmap.h"
#include "openfs/path.h"
#include "openfs/file.h"
#include "openfs/dir.h"
#include "openfs/inode.h"
#include "openfs/crc32c.h"
#include "openfs/journal.h"

typedef struct { uint8_t *b; uint32_t bs; uint64_t bc; } disk_t;

static openfs_io_result_t rd(void *c,uint64_t f,uint32_t n,void *o){disk_t*d=c;if(!d||!o||!n||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(o,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t wr(void *c,uint64_t f,uint32_t n,const void *i){disk_t*d=c;if(!d||!i||!n||f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),i,(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t fl(void *c){(void)c;return OPENFS_IO_OK;}
static openfs_block_device_t dev(disk_t*d){openfs_block_device_t v={d,d->bs,d->bc,rd,wr,fl};return v;}

static void setup(disk_t*d,openfs_block_device_t*v,openfs_superblock_t*s){
    memset(d,0,sizeof(*d));d->bs=4096U;d->bc=256U;d->b=calloc((size_t)d->bs,(size_t)d->bc);TEST_ASSERT(d->b);
    *v=dev(d);uint8_t u[16]={0xB9U};TEST_ASSERT(openfs_format(v,u)==OPENFS_FORMAT_OK);TEST_ASSERT(openfs_read_superblock(v,s)==OPENFS_FORMAT_OK);
}
static uint64_t inode_count(const openfs_superblock_t*s){return (s->inode_table_blocks*(uint64_t)s->block_size)/OPENFS_INODE_SIZE;}
static void patch_inode_u64(openfs_block_device_t*v,const openfs_superblock_t*s,uint64_t ino,uint32_t off,uint64_t value){
    uint64_t pos=(ino-1U)*(uint64_t)OPENFS_INODE_SIZE,blk=s->inode_table_start+pos/s->block_size;uint32_t w=(uint32_t)(pos%s->block_size);uint8_t raw[4096U];
    TEST_ASSERT(v->read(v->context,blk,1U,raw)==OPENFS_IO_OK);for(unsigned k=0;k<8U;k++)raw[w+off+k]=(uint8_t)(value>>(8U*k));
    memset(raw+w+224U,0,4U);uint32_t c=openfs_crc32c(raw+w,224U);for(unsigned k=0;k<4U;k++)raw[w+224U+k]=(uint8_t)(c>>(8U*k));TEST_ASSERT(v->write(v->context,blk,1U,raw)==OPENFS_IO_OK);
}
static void patch_inode_u32(openfs_block_device_t*v,const openfs_superblock_t*s,uint64_t ino,uint32_t off,uint32_t value){patch_inode_u64(v,s,ino,off,(uint64_t)value);}
static void patch_extent(openfs_block_device_t*v,const openfs_superblock_t*s,uint64_t ino,uint32_t index,uint64_t logical,uint64_t physical,uint64_t blocks){
    uint64_t pos=(ino-1U)*(uint64_t)OPENFS_INODE_SIZE,blk=s->inode_table_start+pos/s->block_size;uint32_t w=(uint32_t)(pos%s->block_size)+96U+index*24U;uint8_t raw[4096U];
    TEST_ASSERT(v->read(v->context,blk,1U,raw)==OPENFS_IO_OK);
    for(unsigned k=0;k<8;k++)raw[w+k]=(uint8_t)(logical>>(8U*k));
    for(unsigned k=0;k<8;k++)raw[w+8U+k]=(uint8_t)(physical>>(8U*k));
    for(unsigned k=0;k<8;k++)raw[w+16U+k]=(uint8_t)(blocks>>(8U*k));
    uint32_t base=(uint32_t)(pos%s->block_size);memset(raw+base+224U,0,4U);uint32_t c=openfs_crc32c(raw+base,224U);for(unsigned k=0;k<4;k++)raw[base+224U+k]=(uint8_t)(c>>(8U*k));
    TEST_ASSERT(v->write(v->context,blk,1U,raw)==OPENFS_IO_OK);
}
static void patch_dir_entry(openfs_block_device_t*v,const openfs_superblock_t*s,uint64_t dir,uint64_t entry,uint64_t ino,uint64_t gen){
    uint64_t ic=inode_count(s);openfs_inode_t d;TEST_ASSERT(openfs_inode_read(v,s->inode_table_start,dir,ic,&d)==OPENFS_INODE_OK);
    uint8_t raw[OPENFS_DIR_ENTRY_SIZE];size_t got=0;uint64_t off=entry*(uint64_t)OPENFS_DIR_ENTRY_SIZE;TEST_ASSERT(openfs_file_read(v,s,&d,off,raw,sizeof(raw),&got)==OPENFS_FILE_OK&&got==sizeof(raw));
    for(unsigned k=0;k<8;k++)raw[8U+k]=(uint8_t)(ino>>(8U*k));for(unsigned k=0;k<8;k++)raw[16U+k]=(uint8_t)(gen>>(8U*k));
    uint32_t c=openfs_crc32c(raw,252U);for(unsigned k=0;k<4;k++)raw[252U+k]=(uint8_t)(c>>(8U*k));
    TEST_ASSERT(openfs_file_write(v,s,&d,off,raw,sizeof(raw))==OPENFS_FILE_OK);
}
static void patch_journal_record(openfs_block_device_t*v,const openfs_superblock_t*s,uint64_t slot,uint64_t tx,uint64_t seq,uint32_t len){
    uint8_t raw[4096U];memset(raw,0,sizeof(raw));memcpy(raw,OPENFS_JOURNAL_MAGIC,5U);raw[5U]=OPENFS_JOURNAL_DATA;
    for(unsigned k=0;k<8;k++)raw[8U+k]=(uint8_t)(tx>>(8U*k));for(unsigned k=0;k<8;k++)raw[16U+k]=(uint8_t)(seq>>(8U*k));for(unsigned k=0;k<4;k++)raw[24U+k]=(uint8_t)(len>>(8U*k));
    memset(raw+28U,0,4U);uint32_t c=openfs_crc32c(raw,4096U);for(unsigned k=0;k<4;k++)raw[28U+k]=(uint8_t)(c>>(8U*k));TEST_ASSERT(v->write(v->context,s->journal_start+slot,1U,raw)==OPENFS_IO_OK);
}
static void patch_sb_u64(openfs_block_device_t*v,const openfs_superblock_t*s,uint32_t off,uint64_t value){
    uint64_t blocks[2]={0U,v->block_count-1U};for(unsigned n=0;n<2;n++){uint8_t raw[4096U];TEST_ASSERT(v->read(v->context,blocks[n],1U,raw)==OPENFS_IO_OK);for(unsigned k=0;k<8;k++)raw[off+k]=(uint8_t)(value>>(8U*k));memset(raw+4088U,0,4U);uint32_t c=openfs_crc32c(raw,4088U);for(unsigned k=0;k<4;k++)raw[4088U+k]=(uint8_t)(c>>(8U*k));TEST_ASSERT(v->write(v->context,blocks[n],1U,raw)==OPENFS_IO_OK);}
}
static int corrupt_case(unsigned kind){
    disk_t d;openfs_block_device_t v;openfs_superblock_t s;setup(&d,&v,&s);
    uint64_t ic=inode_count(&s),ino=0;TEST_ASSERT(openfs_path_create(&v,&s,"/f",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);
    uint8_t one[4096U];memset(one,0x5A,sizeof(one));openfs_inode_t fi;TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,ino,ic,&fi)==OPENFS_INODE_OK);TEST_ASSERT(openfs_file_write(&v,&s,&fi,0U,one,sizeof(one))==OPENFS_FILE_OK);TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,ino,ic,&fi)==OPENFS_INODE_OK);
    switch(kind){
    case 0: patch_sb_u64(&v,&s,28U,d.bc+1U); s.total_blocks=d.bc+1U; break; /* block count */
    case 1: patch_sb_u64(&v,&s,92U,0U); s.inode_table_blocks=0U; break; /* inode count geometry */
    case 2: patch_inode_u64(&v,&s,ino,0U,0U); break; /* inode number 0 */
    case 3: patch_inode_u64(&v,&s,ino,0U,ic+1U); break; /* inode number out of range */
    case 4: patch_inode_u64(&v,&s,ino,8U,0U); break; /* generation zero */
    case 5: patch_inode_u64(&v,&s,ino,40U,0U); break; /* link count zero */
    case 6: patch_inode_u64(&v,&s,ino,40U,2U); break; /* link count mismatch */
    case 7: {openfs_extent_t e;TEST_ASSERT(openfs_inode_get_extent(&fi,0U,&e)==OPENFS_EXTENT_OK);patch_extent(&v,&s,ino,0U,e.logical_start,d.bc,1U);break;} /* extent offset */
    case 8: {openfs_extent_t e;TEST_ASSERT(openfs_inode_get_extent(&fi,0U,&e)==OPENFS_EXTENT_OK);patch_extent(&v,&s,ino,0U,e.logical_start,e.physical_start,0U);break;} /* extent length zero */
    case 9: {TEST_ASSERT(openfs_path_create(&v,&s,"/two",OPENFS_INODE_MODE_REGULAR,&ino)==OPENFS_PATH_OK);openfs_inode_t x;TEST_ASSERT(openfs_inode_read(&v,s.inode_table_start,ino,ic,&x)==OPENFS_INODE_OK);TEST_ASSERT(openfs_file_write(&v,&s,&x,0U,one,sizeof(one))==OPENFS_FILE_OK);TEST_ASSERT(openfs_file_write(&v,&s,&x,4096U,one,sizeof(one))==OPENFS_FILE_OK);openfs_inode_read(&v,s.inode_table_start,ino,ic,&x);openfs_extent_t e0;TEST_ASSERT(openfs_inode_get_extent(&x,0U,&e0)==OPENFS_EXTENT_OK);patch_extent(&v,&s,ino,0U,e0.logical_start,e0.physical_start,1U);patch_inode_u32(&v,&s,ino,88U,2U);patch_extent(&v,&s,ino,1U,1U,e0.physical_start,1U);break;} /* overlap */
    case 10: patch_inode_u32(&v,&s,ino,88U,0U); break; /* extent count */
    case 11: patch_dir_entry(&v,&s,s.root_inode,0U,ic+1U,1U); break; /* inode reference */
    case 12: patch_dir_entry(&v,&s,s.root_inode,0U,ino,999U); break; /* generation reference */
    case 13: {uint64_t child=0;TEST_ASSERT(openfs_path_mkdir(&v,&s,"/child",&child)==OPENFS_PATH_OK);patch_inode_u64(&v,&s,child,32U,999U);break;} /* parent */
    case 14: patch_sb_u64(&v,&s,100U,d.bc); s.journal_start=d.bc; break; /* journal start out of range */
    case 15: patch_sb_u64(&v,&s,108U,d.bc); s.journal_blocks=d.bc; break; /* journal length out of range */
    case 16: patch_journal_record(&v,&s,0U,1U,1U,(uint32_t)(v.block_size-OPENFS_JOURNAL_HEADER_SIZE+1U)); break; /* record too large */
    case 17: patch_journal_record(&v,&s,0U,0U,1U,0U); break; /* txid invalid */
    case 18: patch_journal_record(&v,&s,0U,1U,0U,0U); break; /* sequence invalid */
    case 19: {int used=0;TEST_ASSERT(openfs_bitmap_test(&v,s.inode_bitmap_start,s.inode_bitmap_blocks,ino-1U,&used)==OPENFS_BITMAP_OK);TEST_ASSERT(used);TEST_ASSERT(openfs_bitmap_set(&v,s.inode_bitmap_start,s.inode_bitmap_blocks,ino-1U,0)==OPENFS_BITMAP_OK);break;} /* inode bitmap */
    case 20: {openfs_extent_t e;TEST_ASSERT(openfs_inode_get_extent(&fi,0U,&e)==OPENFS_EXTENT_OK);TEST_ASSERT(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,e.physical_start,0)==OPENFS_BITMAP_OK);break;} /* block bitmap */
    case 21: {uint64_t bit=d.bc;TEST_ASSERT(openfs_bitmap_set(&v,s.block_bitmap_start,s.block_bitmap_blocks,bit,1)==OPENFS_BITMAP_OK);break;} /* bitmap bit beyond device */
    default: TEST_ASSERT(0);
    }
    uint64_t errors=0;openfs_fsck_result_t r=openfs_fsck(&v,&s,&errors);int ok=(r!=OPENFS_FSCK_OK);free(d.b);return ok;
}
int main(void){for(unsigned k=0;k<=21;k++){if(!corrupt_case(k)){fprintf(stderr,"fsck boundary case %u failed\\n",k);return 1;}}return 0;}
