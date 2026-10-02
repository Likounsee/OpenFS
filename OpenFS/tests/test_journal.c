#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/journal.h"
typedef struct{uint8_t*b;uint32_t bs;uint64_t bc;}D;
static openfs_io_result_t r(void*c,uint64_t f,uint32_t n,void*x){D*d=c;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(x,d->b+(size_t)(f*d->bs),(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t w(void*c,uint64_t f,uint32_t n,const void*x){D*d=c;if(f>=d->bc||(uint64_t)n>d->bc-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(d->b+(size_t)(f*d->bs),x,(size_t)((uint64_t)n*d->bs));return OPENFS_IO_OK;}
static openfs_io_result_t f(void*c){(void)c;return OPENFS_IO_OK;}
static openfs_journal_result_t cb(void*c,uint64_t tx,const uint8_t*p,uint32_t n){uint32_t *hits=c;assert(tx==1U&&n==3U&&memcmp(p,"abc",3U)==0);(*hits)++;return OPENFS_JOURNAL_OK;}
int main(void){D d={0};d.bs=4096U;d.bc=256U;d.b=calloc((size_t)d.bs,d.bc);assert(d.b);openfs_block_device_t v={&d,d.bs,d.bc,r,w,f};uint8_t u[16]={0};assert(openfs_format(&v,u)==OPENFS_FORMAT_OK);openfs_superblock_t s;assert(openfs_read_superblock(&v,&s)==OPENFS_FORMAT_OK);openfs_journal_t j;assert(openfs_journal_open(&j,&v,&s)==OPENFS_JOURNAL_OK);uint64_t tx=0;assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK&&tx==1U);assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_INVALID_ARGUMENT);assert(openfs_journal_write(&j,&v,2U,"abc",3U)==OPENFS_JOURNAL_INVALID_ARGUMENT);assert(openfs_journal_write(&j,&v,tx,"abc",3U)==OPENFS_JOURNAL_OK);uint32_t hits=0;assert(openfs_journal_replay(&v,&s,cb,&hits)==OPENFS_JOURNAL_OK&&hits==0U);assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_replay(&v,&s,cb,&hits)==OPENFS_JOURNAL_OK&&hits==1U);free(d.b);return 0;}