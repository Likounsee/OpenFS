#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/format.h"
#include "openfs/journal.h"
#include "openfs/transaction.h"

#define BS 4096U
#define BC 256U

typedef struct { uint8_t *bytes; uint64_t js, jb; int saw_commit, saw_publish, saw_checkpoint, bad; } disk_t;
static int isj(const disk_t *d,uint64_t b){return b>=d->js&&b<d->js+d->jb;}
static openfs_io_result_t rd(void *c,uint64_t b,uint32_t n,void *o){disk_t*d=c;if(!d||!o||!n||b>=BC||(uint64_t)n>BC-b)return OPENFS_IO_OUT_OF_RANGE;memcpy(o,d->bytes+(size_t)b*BS,(size_t)n*BS);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void *c,uint64_t b,uint32_t n,const void *in){disk_t*d=c;if(!d||!in||!n||b>=BC||(uint64_t)n>BC-b)return OPENFS_IO_OUT_OF_RANGE;const uint8_t*x=in;int j=isj(d,b)&&n==1U;if(j&&memcmp(x,OPENFS_JOURNAL_MAGIC,5U)==0&&x[5]==OPENFS_JOURNAL_COMMIT)d->saw_commit=1;if(j&&d->saw_publish){int z=1;for(uint32_t i=0;i<BS;i++)if(x[i]){z=0;break;}if(z)d->saw_checkpoint=1;}if(!j&&d->saw_commit)d->saw_publish=1;if(!j&&!d->saw_commit)d->bad=1;if(d->saw_checkpoint&&!j)d->bad=1;memcpy(d->bytes+(size_t)b*BS,x,(size_t)n*BS);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void *c){(void)c;return OPENFS_IO_OK;}
int main(void){disk_t d={0};d.bytes=calloc(BC,BS);assert(d.bytes);openfs_block_device_t v={&d,BS,BC,rd,wr,fl};uint8_t u[16]={0x44U};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);d.js=s.journal_start;d.jb=s.journal_blocks;openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);openfs_transaction_t t;assert(openfs_transaction_begin(&t,&v,&j)==OPENFS_TRANSACTION_OK);openfs_block_device_t*td=openfs_transaction_device(&t);assert(td);uint8_t payload[BS];memset(payload,0xA5,sizeof(payload));assert(td->write(td->context,s.data_start+3U,1U,payload)==OPENFS_IO_OK);assert(openfs_transaction_commit(&t)==OPENFS_TRANSACTION_OK);assert(!d.bad&&d.saw_commit&&d.saw_publish&&d.saw_checkpoint);free(d.bytes);return 0;}
