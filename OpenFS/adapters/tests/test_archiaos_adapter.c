#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "openfs_archiaos_adapter.h"

typedef struct { uint8_t blocks[2][16]; int flushes; } storage_t;
static openfs_io_result_t rd(void*c,uint64_t first,uint32_t count,void*b){storage_t*s=c;if(first>=2U||(uint64_t)count>2U-first)return OPENFS_IO_OUT_OF_RANGE;memcpy(b,&s->blocks[first][0],(size_t)count*16U);return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t first,uint32_t count,const void*b){storage_t*s=c;if(first>=2U||(uint64_t)count>2U-first)return OPENFS_IO_OUT_OF_RANGE;memcpy(&s->blocks[first][0],b,(size_t)count*16U);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){((storage_t*)c)->flushes++;return OPENFS_IO_OK;}

int main(void)
{
    storage_t s={0};openfs_archiaos_storage_t hooks={&s,16U,2U,rd,wr,fl};openfs_archiaos_adapter_t a;
    assert(openfs_archiaos_adapter_init(&a,&hooks)==1);
    uint8_t x[16];memset(x,0x7a,sizeof(x));assert(a.device.write(a.device.context,1U,1U,x)==OPENFS_IO_OK);
    memset(x,0,sizeof(x));assert(a.device.read(a.device.context,1U,1U,x)==OPENFS_IO_OK);assert(x[0]==0x7aU);
    assert(a.device.flush(a.device.context)==OPENFS_IO_OK&&s.flushes==1);
    assert(a.device.read(a.device.context,2U,1U,x)==OPENFS_IO_OUT_OF_RANGE);
    return 0;
}
