#include "openfs_windows_adapter.h"

#ifdef _WIN32
#include <limits.h>

static openfs_io_result_t windows_read(void *context,uint64_t first,uint32_t count,void *buffer)
{
    openfs_windows_adapter_t *a=context;
    if(a==NULL||buffer==NULL||count==0U)return OPENFS_IO_INVALID_ARGUMENT;
    if(first>=a->device.block_count||(uint64_t)count>a->device.block_count-first)return OPENFS_IO_OUT_OF_RANGE;
    uint64_t bytes64=(uint64_t)count*a->device.block_size;
    uint64_t offset64=first*(uint64_t)a->device.block_size;
    if(bytes64>UINT32_MAX||offset64>INT64_MAX-bytes64)return OPENFS_IO_OUT_OF_RANGE;
    OVERLAPPED ov={0};ov.Offset=(DWORD)offset64;ov.OffsetHigh=(DWORD)(offset64>>32U);
    DWORD done=0U;
    if(!ReadFile(a->handle,buffer,(DWORD)bytes64,&done,&ov)||done!=(DWORD)bytes64)return OPENFS_IO_IO_ERROR;
    return OPENFS_IO_OK;
}

static openfs_io_result_t windows_write(void *context,uint64_t first,uint32_t count,const void *buffer)
{
    openfs_windows_adapter_t *a=context;
    if(a==NULL||buffer==NULL||count==0U)return OPENFS_IO_INVALID_ARGUMENT;
    if(!a->writable)return OPENFS_IO_READ_ONLY;
    if(first>=a->device.block_count||(uint64_t)count>a->device.block_count-first)return OPENFS_IO_OUT_OF_RANGE;
    uint64_t bytes64=(uint64_t)count*a->device.block_size;
    uint64_t offset64=first*(uint64_t)a->device.block_size;
    if(bytes64>UINT32_MAX||offset64>INT64_MAX-bytes64)return OPENFS_IO_OUT_OF_RANGE;
    OVERLAPPED ov={0};ov.Offset=(DWORD)offset64;ov.OffsetHigh=(DWORD)(offset64>>32U);
    DWORD done=0U;
    if(!WriteFile(a->handle,buffer,(DWORD)bytes64,&done,&ov)||done!=(DWORD)bytes64)return OPENFS_IO_IO_ERROR;
    return OPENFS_IO_OK;
}

static openfs_io_result_t windows_flush(void *context)
{
    openfs_windows_adapter_t *a=context;
    if(a==NULL)return OPENFS_IO_INVALID_ARGUMENT;
    return FlushFileBuffers(a->handle)?OPENFS_IO_OK:OPENFS_IO_IO_ERROR;
}

openfs_windows_adapter_result_t openfs_windows_adapter_open(
    openfs_windows_adapter_t *adapter,const wchar_t *path,uint32_t block_size,int writable)
{
    if(adapter==NULL||path==NULL||block_size==0U)return OPENFS_WINDOWS_ADAPTER_INVALID_ARGUMENT;
    DWORD access=writable?(GENERIC_READ|GENERIC_WRITE):GENERIC_READ;
    HANDLE h=CreateFileW(path,access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED,NULL);
    if(h==INVALID_HANDLE_VALUE)return OPENFS_WINDOWS_ADAPTER_IO_ERROR;
    LARGE_INTEGER size;
    if(!GetFileSizeEx(h,&size)||size.QuadPart<0){
        CloseHandle(h);return OPENFS_WINDOWS_ADAPTER_IO_ERROR;
    }
    uint64_t bytes=(uint64_t)size.QuadPart;
    if(bytes<(uint64_t)block_size||bytes%(uint64_t)block_size!=0U){
        CloseHandle(h);return OPENFS_WINDOWS_ADAPTER_UNSUPPORTED;
    }
    adapter->handle=h;adapter->writable=writable?1:0;adapter->device.context=adapter;
    adapter->device.block_size=block_size;adapter->device.block_count=bytes/block_size;
    adapter->device.read=windows_read;adapter->device.write=windows_write;adapter->device.flush=windows_flush;
    return OPENFS_WINDOWS_ADAPTER_OK;
}

openfs_windows_adapter_result_t openfs_windows_adapter_close(openfs_windows_adapter_t *adapter)
{
    if(adapter==NULL||adapter->handle==INVALID_HANDLE_VALUE)return OPENFS_WINDOWS_ADAPTER_INVALID_ARGUMENT;
    BOOL ok=CloseHandle(adapter->handle);adapter->handle=INVALID_HANDLE_VALUE;adapter->device.context=NULL;
    return ok?OPENFS_WINDOWS_ADAPTER_OK:OPENFS_WINDOWS_ADAPTER_IO_ERROR;
}

openfs_block_device_t *openfs_windows_adapter_device(openfs_windows_adapter_t *adapter)
{
    return adapter==NULL?NULL:&adapter->device;
}
#else
/* The implementation is intentionally inert on non-Windows hosts. */
#endif
