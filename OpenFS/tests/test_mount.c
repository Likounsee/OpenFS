#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/mount.h"
#include "openfs/crc32c.h"

typedef struct {uint8_t *bytes;uint32_t block_size;uint64_t block_count;unsigned flushes;} disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*b){disk_t*d=c;if(n==0U||f>=d->block_count||(uint64_t)n>d->block_count-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(b,d->bytes+(size_t)(f*d->block_size),(size_t)((uint64_t)n*d->block_size));return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*b){disk_t*d=c;if(n==0U||f>=d->block_count||(uint64_t)n>d->block_count-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->bytes+(size_t)(f*d->block_size),b,(size_t)((uint64_t)n*d->block_size));return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){((disk_t*)c)->flushes++;return OPENFS_IO_OK;} static openfs_journal_result_t replay_probe(void*c,uint64_t tx,const uint8_t*p,uint32_t n){unsigned *hits=c;(void)tx;(void)p;if(n>=24U&&memcmp(p,"OJBD1",5U)==0)(*hits)++;return OPENFS_JOURNAL_OK;}
int main(void){
 disk_t d={.block_size=4096U,.block_count=128U};d.bytes=calloc((size_t)d.block_count,d.block_size);assert(d.bytes);
 openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};uint8_t uuid[16]={7U};
 assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
 openfs_mount_t m;assert(openfs_mount(&m,&v)==OPENFS_MOUNT_OK&&m.mounted);
 assert(m.superblock.root_inode==1U);
 uint64_t target=m.superblock.data_start;uint8_t pattern[4096];for(size_t i=0U;i<sizeof(pattern);++i)pattern[i]=(uint8_t)(i^0x5AU);openfs_journal_t j;uint64_t tx=0U;assert(openfs_journal_open(&j,&v,&m.superblock)==OPENFS_JOURNAL_OK);assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_write_block(&j,&v,tx,target,pattern)==OPENFS_JOURNAL_OK);assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);memset(d.bytes+(size_t)(target*d.block_size),0U,d.block_size);unsigned replay_hits=0U;assert(openfs_journal_replay(&v,&m.superblock,replay_probe,&replay_hits)==OPENFS_JOURNAL_OK&&replay_hits==2U);assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);openfs_mount_t replayed;assert(openfs_mount(&replayed,&v)==OPENFS_MOUNT_OK);assert(memcmp(d.bytes+(size_t)(target*d.block_size),pattern,sizeof(pattern))==0);openfs_superblock_t stable=replayed.superblock;assert(openfs_unmount(&replayed)==OPENFS_MOUNT_OK);uint8_t *backup=d.bytes+(size_t)((d.block_count-1U)*d.block_size);backup[148U]^=0xA5U;backup[4088U]=backup[4089U]=backup[4090U]=backup[4091U]=0U;uint32_t crc=openfs_crc32c(backup,4088U);backup[4088U]=(uint8_t)crc;backup[4089U]=(uint8_t)(crc>>8U);backup[4090U]=(uint8_t)(crc>>16U);backup[4091U]=(uint8_t)(crc>>24U);openfs_mount_t mismatched;assert(openfs_mount(&mismatched,&v)==OPENFS_MOUNT_CORRUPT);backup[148U]^=0xA5U;crc=openfs_crc32c(backup,4088U);backup[4088U]=(uint8_t)crc;backup[4089U]=(uint8_t)(crc>>8U);backup[4090U]=(uint8_t)(crc>>16U);backup[4091U]=(uint8_t)(crc>>24U);openfs_journal_t malformed;assert(openfs_journal_open(&malformed,&v,&stable)==OPENFS_JOURNAL_OK);assert(openfs_journal_begin(&malformed,&v,&tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_write(&malformed,&v,tx,"bad",3U)==OPENFS_JOURNAL_OK);assert(openfs_journal_commit(&malformed,&v,tx)==OPENFS_JOURNAL_OK);openfs_mount_t rejected;assert(openfs_mount(&rejected,&v)==OPENFS_MOUNT_CORRUPT);assert(openfs_journal_checkpoint(&malformed,&v)==OPENFS_JOURNAL_OK);d.bytes[64]^=0x55U;
 openfs_mount_t fallback;assert(openfs_mount(&fallback,&v)==OPENFS_MOUNT_OK);
 assert(fallback.superblock.root_inode==1U&&memcmp(fallback.superblock.uuid,uuid,16U)==0);
 assert(openfs_unmount(&fallback)==OPENFS_MOUNT_OK);
 free(d.bytes);return 0;
}
