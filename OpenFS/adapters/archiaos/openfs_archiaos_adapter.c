#include "openfs_archiaos_adapter.h"

#include <string.h>

static openfs_io_result_t archiaos_read(void *context,uint64_t first,uint32_t count,void *buffer)
{
    openfs_archiaos_adapter_t *a=context;
    if(a==NULL||a->storage.context==NULL||a->storage.read==NULL||a->storage.block_size==0U||a->storage.block_count==0U||buffer==NULL||count==0U)return OPENFS_IO_INVALID_ARGUMENT;
    if(first>=a->storage.block_count||(uint64_t)count>a->storage.block_count-first)return OPENFS_IO_OUT_OF_RANGE;
    return a->storage.read(a->storage.context,first,count,buffer);
}

static openfs_io_result_t archiaos_write(void *context,uint64_t first,uint32_t count,const void *buffer)
{
    openfs_archiaos_adapter_t *a=context;
    if(a==NULL||a->storage.context==NULL||a->storage.write==NULL||a->storage.block_size==0U||a->storage.block_count==0U||buffer==NULL||count==0U)return OPENFS_IO_INVALID_ARGUMENT;
    if(first>=a->storage.block_count||(uint64_t)count>a->storage.block_count-first)return OPENFS_IO_OUT_OF_RANGE;
    return a->storage.write(a->storage.context,first,count,buffer);
}

static openfs_io_result_t archiaos_flush(void *context)
{
    openfs_archiaos_adapter_t *a=context;
    if(a==NULL||a->storage.flush==NULL)return OPENFS_IO_INVALID_ARGUMENT;
    return a->storage.flush(a->storage.context);
}

int openfs_archiaos_adapter_init(openfs_archiaos_adapter_t *adapter,const openfs_archiaos_storage_t *storage)
{
    if(adapter==NULL||storage==NULL||storage->context==NULL||storage->block_size==0U||storage->block_count==0U||
       storage->read==NULL||storage->write==NULL||storage->flush==NULL)return 0;
    memset(adapter,0,sizeof(*adapter));adapter->storage=*storage;
    adapter->device.context=adapter;adapter->device.block_size=storage->block_size;adapter->device.block_count=storage->block_count;
    adapter->device.read=archiaos_read;adapter->device.write=archiaos_write;adapter->device.flush=archiaos_flush;
    return 1;
}

openfs_block_device_t *openfs_archiaos_adapter_device(openfs_archiaos_adapter_t *adapter)
{
    return adapter==NULL?NULL:&adapter->device;
}
