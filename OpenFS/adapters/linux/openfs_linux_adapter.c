#define _POSIX_C_SOURCE 200809L
#include "openfs_linux_adapter.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

static openfs_io_result_t linux_read(void *context,uint64_t first,uint32_t count,void *buffer)
{
    openfs_linux_adapter_t *a=context;
    if(a==NULL||buffer==NULL||count==0U)return OPENFS_IO_INVALID_ARGUMENT;
    if(first>=a->device.block_count||(uint64_t)count>a->device.block_count-first)return OPENFS_IO_OUT_OF_RANGE;
    uint64_t bytes64=(uint64_t)count*a->device.block_size;
    uint64_t offset64=first*(uint64_t)a->device.block_size;
    off_t offset=(off_t)offset64;
    if(bytes64>SIZE_MAX||offset<0||(uint64_t)offset!=offset64||bytes64>(uint64_t)LLONG_MAX-offset64)return OPENFS_IO_OUT_OF_RANGE;
    size_t done=0U;
    uint8_t *dst=buffer;
    while(done<(size_t)bytes64){
        ssize_t n=pread(a->fd,dst+done,(size_t)bytes64-done,offset+(off_t)done);
        if(n<=0)return OPENFS_IO_IO_ERROR;
        done+=(size_t)n;
    }
    return OPENFS_IO_OK;
}

static openfs_io_result_t linux_write(void *context,uint64_t first,uint32_t count,const void *buffer)
{
    openfs_linux_adapter_t *a=context;
    if(a==NULL||buffer==NULL||count==0U)return OPENFS_IO_INVALID_ARGUMENT;
    if(!a->writable)return OPENFS_IO_READ_ONLY;
    if(first>=a->device.block_count||(uint64_t)count>a->device.block_count-first)return OPENFS_IO_OUT_OF_RANGE;
    uint64_t bytes64=(uint64_t)count*a->device.block_size;
    uint64_t offset64=first*(uint64_t)a->device.block_size;
    off_t offset=(off_t)offset64;
    if(bytes64>SIZE_MAX||offset<0||(uint64_t)offset!=offset64)return OPENFS_IO_OUT_OF_RANGE;
    size_t done=0U;
    const uint8_t *src=buffer;
    while(done<(size_t)bytes64){
        ssize_t n=pwrite(a->fd,src+done,(size_t)bytes64-done,offset+(off_t)done);
        if(n<=0)return OPENFS_IO_IO_ERROR;
        done+=(size_t)n;
    }
    return OPENFS_IO_OK;
}

static openfs_io_result_t linux_flush(void *context)
{
    openfs_linux_adapter_t *a=context;
    if(a==NULL)return OPENFS_IO_INVALID_ARGUMENT;
    return fsync(a->fd)==0?OPENFS_IO_OK:OPENFS_IO_IO_ERROR;
}

openfs_linux_adapter_result_t openfs_linux_adapter_open(
    openfs_linux_adapter_t *adapter,const char *path,uint32_t block_size,int writable)
{
    if(adapter==NULL||path==NULL||block_size==0U)return OPENFS_LINUX_ADAPTER_INVALID_ARGUMENT;
    int flags=writable?O_RDWR:O_RDONLY;
    int fd=open(path,flags);
    if(fd<0)return OPENFS_LINUX_ADAPTER_IO_ERROR;
    struct stat st;
    if(fstat(fd,&st)!=0||st.st_size<0){
        close(fd);return OPENFS_LINUX_ADAPTER_IO_ERROR;
    }
    uint64_t size=(uint64_t)st.st_size;
    if(size<(uint64_t)block_size||size%(uint64_t)block_size!=0U){
        close(fd);return OPENFS_LINUX_ADAPTER_UNSUPPORTED;
    }
    adapter->fd=fd;adapter->writable=writable?1:0;
    adapter->device.context=adapter;adapter->device.block_size=block_size;
    adapter->device.block_count=size/block_size;adapter->device.read=linux_read;
    adapter->device.write=linux_write;adapter->device.flush=linux_flush;
    return OPENFS_LINUX_ADAPTER_OK;
}

openfs_linux_adapter_result_t openfs_linux_adapter_close(openfs_linux_adapter_t *adapter)
{
    if(adapter==NULL||adapter->fd<0)return OPENFS_LINUX_ADAPTER_INVALID_ARGUMENT;
    int r=close(adapter->fd);adapter->fd=-1;adapter->device.context=NULL;
    return r==0?OPENFS_LINUX_ADAPTER_OK:OPENFS_LINUX_ADAPTER_IO_ERROR;
}

openfs_block_device_t *openfs_linux_adapter_device(openfs_linux_adapter_t *adapter)
{
    return adapter==NULL?NULL:&adapter->device;
}
