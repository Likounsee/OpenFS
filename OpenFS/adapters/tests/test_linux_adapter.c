#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "openfs_linux_adapter.h"

int main(void)
{
    char path[]="/tmp/openfs-adapter-XXXXXX";
    int seed=mkstemp(path);assert(seed>=0);
    uint8_t zero[4096]={0};
    for(unsigned i=0U;i<4U;i++)assert(write(seed,zero,sizeof(zero))==(ssize_t)sizeof(zero));
    assert(fsync(seed)==0);assert(close(seed)==0);

    openfs_linux_adapter_t a={0};
    assert(openfs_linux_adapter_open(&a,path,4096U,1)==OPENFS_LINUX_ADAPTER_OK);
    assert(a.device.block_count==4U);
    uint8_t block[4096];memset(block,0x5a,sizeof(block));
    assert(a.device.write(a.device.context,1U,1U,block)==OPENFS_IO_OK);
    assert(a.device.flush(a.device.context)==OPENFS_IO_OK);
    memset(block,0,sizeof(block));
    assert(a.device.read(a.device.context,1U,1U,block)==OPENFS_IO_OK);
    assert(block[0]==0x5aU&&block[4095]==0x5aU);
    assert(a.device.read(a.device.context,4U,1U,block)==OPENFS_IO_OUT_OF_RANGE);
    assert(openfs_linux_adapter_close(&a)==OPENFS_LINUX_ADAPTER_OK);

    assert(openfs_linux_adapter_open(&a,path,4096U,0)==OPENFS_LINUX_ADAPTER_OK);
    assert(a.device.write(a.device.context,0U,1U,block)==OPENFS_IO_READ_ONLY);
    assert(openfs_linux_adapter_close(&a)==OPENFS_LINUX_ADAPTER_OK);
    assert(unlink(path)==0);
    return 0;
}
