#include "openfs/journal.h"
#include "openfs/crc32c.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
static void p32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);p[2]=(uint8_t)(v>>16U);p[3]=(uint8_t)(v>>24U);}
static void p64(uint8_t*p,uint64_t v){for(unsigned i=0;i<8U;i++)p[i]=(uint8_t)(v>>(8U*i));}
static uint32_t g32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8U)|((uint32_t)p[2]<<16U)|((uint32_t)p[3]<<24U);}
static uint64_t g64(const uint8_t*p){uint64_t v=0U;for(unsigned i=0;i<8U;i++)v|=(uint64_t)p[i]<<(8U*i);return v;}
static int range(const openfs_block_device_t*d,const openfs_superblock_t*s){return openfs_block_device_is_valid(d)&&s!=NULL&&s->block_size==d->block_size&&s->journal_blocks>0U&&s->journal_start<d->block_count&&s->journal_blocks<=d->block_count-s->journal_start;}
static openfs_journal_result_t put(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t slot,uint64_t tx,uint64_t seq,uint32_t type,const void*data,uint32_t len){if(d->block_size<OPENFS_JOURNAL_HEADER_SIZE||len>d->block_size-OPENFS_JOURNAL_HEADER_SIZE)return OPENFS_JOURNAL_INVALID_ARGUMENT;if(slot>=s->journal_blocks)return OPENFS_JOURNAL_FULL;uint8_t*b=calloc(1U,d->block_size);if(!b)return OPENFS_JOURNAL_IO_ERROR;memcpy(b,OPENFS_JOURNAL_MAGIC,5U);b[5]=(uint8_t)type;p64(b+8U,tx);p64(b+16U,seq);p32(b+24U,len);if(len!=0U)memcpy(b+OPENFS_JOURNAL_HEADER_SIZE,data,len);p32(b+28U,openfs_crc32c(b,d->block_size-4U));openfs_io_result_t io=d->write(d->context,s->journal_start+slot,1U,b);free(b);return io==OPENFS_IO_OK?OPENFS_JOURNAL_OK:OPENFS_JOURNAL_IO_ERROR;}
openfs_journal_result_t openfs_journal_open(openfs_journal_t*j,const openfs_block_device_t*d,const openfs_superblock_t*s){if(j==NULL||!range(d,s))return OPENFS_JOURNAL_INVALID_ARGUMENT;j->transaction_id=0U;j->sequence=0U;j->active_transaction_id=0U;j->next_record=0U;j->journal_start=s->journal_start;j->journal_blocks=s->journal_blocks;j->block_size=s->block_size;uint8_t*b=malloc(d->block_size);if(!b)return OPENFS_JOURNAL_IO_ERROR;for(uint64_t n=0U;n<s->journal_blocks;n++){if(d->read(d->context,s->journal_start+n,1U,b)!=OPENFS_IO_OK){free(b);return OPENFS_JOURNAL_IO_ERROR;}if(memcmp(b,OPENFS_JOURNAL_MAGIC,5U)!=0)break;uint32_t stored=g32(b+28U);p32(b+28U,0U);int ok=stored==openfs_crc32c(b,d->block_size-4U);p32(b+28U,stored);if(!ok){free(b);return OPENFS_JOURNAL_CORRUPT;}uint64_t tx=g64(b+8U),seq=g64(b+16U);if(tx>j->transaction_id)j->transaction_id=tx;if(seq>j->sequence)j->sequence=seq;j->next_record=n+1U;}free(b);return OPENFS_JOURNAL_OK;}
openfs_journal_result_t openfs_journal_begin(openfs_journal_t*j,openfs_block_device_t*d,uint64_t*tx){if(j==NULL||tx==NULL||!openfs_block_device_is_valid(d))return OPENFS_JOURNAL_INVALID_ARGUMENT;if(j->next_record>=j->journal_blocks)return OPENFS_JOURNAL_FULL;j->transaction_id++;if(j->transaction_id==0U)j->transaction_id=1U;openfs_superblock_t s={0};s.block_size=j->block_size;s.journal_start=j->journal_start;s.journal_blocks=j->journal_blocks;openfs_journal_result_t r=put(d,&s,j->next_record,j->transaction_id,++j->sequence,OPENFS_JOURNAL_BEGIN,NULL,0U);if(r==OPENFS_JOURNAL_OK){j->next_record++;*tx=j->transaction_id;}return r;}
openfs_journal_result_t openfs_journal_write(openfs_journal_t*j,openfs_block_device_t*d,uint64_t tx,const void*data,uint32_t len){if(j==NULL||data==NULL||!openfs_block_device_is_valid(d))return OPENFS_JOURNAL_INVALID_ARGUMENT;if(j->next_record>=j->journal_blocks)return OPENFS_JOURNAL_FULL;openfs_superblock_t s={0};s.block_size=j->block_size;s.journal_start=j->journal_start;s.journal_blocks=j->journal_blocks;openfs_journal_result_t r=put(d,&s,j->next_record,tx,++j->sequence,OPENFS_JOURNAL_DATA,data,len);if(r==OPENFS_JOURNAL_OK)j->next_record++;return r;}
openfs_journal_result_t openfs_journal_commit(openfs_journal_t*j,openfs_block_device_t*d,uint64_t tx){if(j==NULL||!openfs_block_device_is_valid(d))return OPENFS_JOURNAL_INVALID_ARGUMENT;if(j->next_record>=j->journal_blocks)return OPENFS_JOURNAL_FULL;openfs_superblock_t s={0};s.block_size=j->block_size;s.journal_start=j->journal_start;s.journal_blocks=j->journal_blocks;openfs_journal_result_t r=put(d,&s,j->next_record,tx,++j->sequence,OPENFS_JOURNAL_COMMIT,NULL,0U);if(r==OPENFS_JOURNAL_OK){j->next_record++;r=d->flush(d->context)==OPENFS_IO_OK?OPENFS_JOURNAL_OK:OPENFS_JOURNAL_IO_ERROR;}return r;}
openfs_journal_result_t openfs_journal_replay(const openfs_block_device_t*d,const openfs_superblock_t*s,openfs_journal_replay_fn cb,void*ctx){
if(!range(d,s)||cb==NULL)return OPENFS_JOURNAL_INVALID_ARGUMENT;
uint8_t*b=malloc(d->block_size);if(!b)return OPENFS_JOURNAL_IO_ERROR;
uint64_t*commits=calloc((size_t)s->journal_blocks,sizeof(*commits));if(!commits){free(b);return OPENFS_JOURNAL_IO_ERROR;}
uint64_t commit_count=0U;
for(uint64_t n=0U;n<s->journal_blocks;n++){
if(d->read(d->context,s->journal_start+n,1U,b)!=OPENFS_IO_OK){free(commits);free(b);return OPENFS_JOURNAL_IO_ERROR;}
if(memcmp(b,OPENFS_JOURNAL_MAGIC,5U)!=0)continue;
uint32_t stored=g32(b+28U);p32(b+28U,0U);if(stored!=openfs_crc32c(b,d->block_size-4U)){free(commits);free(b);return OPENFS_JOURNAL_CORRUPT;}p32(b+28U,stored);
uint32_t len=g32(b+24U);if(len>d->block_size-OPENFS_JOURNAL_HEADER_SIZE){free(commits);free(b);return OPENFS_JOURNAL_CORRUPT;}
if(b[5]==OPENFS_JOURNAL_COMMIT&&commit_count<s->journal_blocks)commits[commit_count++]=g64(b+8U);
}
for(uint64_t n=0U;n<s->journal_blocks;n++){
if(d->read(d->context,s->journal_start+n,1U,b)!=OPENFS_IO_OK){free(commits);free(b);return OPENFS_JOURNAL_IO_ERROR;}
if(memcmp(b,OPENFS_JOURNAL_MAGIC,5U)!=0)continue;
uint32_t stored=g32(b+28U);p32(b+28U,0U);if(stored!=openfs_crc32c(b,d->block_size-4U)){free(commits);free(b);return OPENFS_JOURNAL_CORRUPT;}p32(b+28U,stored);
uint32_t len=g32(b+24U);if(b[5]!=OPENFS_JOURNAL_DATA)continue;
uint64_t tx=g64(b+8U);int committed=0;for(uint64_t i=0U;i<commit_count;i++)if(commits[i]==tx){committed=1;break;}if(!committed)continue;
openfs_journal_result_t r=cb(ctx,tx,b+OPENFS_JOURNAL_HEADER_SIZE,len);if(r!=OPENFS_JOURNAL_OK){free(commits);free(b);return r;}
}
free(commits);free(b);return OPENFS_JOURNAL_OK;}
