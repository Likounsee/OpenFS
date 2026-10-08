#include "openfs_windows_adapter.h"
#include "openfs/format.h"

#ifdef _WIN32
#include <stdint.h>
#include <winioctl.h>

static int query_device_size(HANDLE handle,uint64_t *bytes)
{
    if(bytes==NULL)return 0;
    GET_LENGTH_INFORMATION info;
    DWORD returned=0U;
    if(DeviceIoControl(handle,IOCTL_DISK_GET_LENGTH_INFO,NULL,0U,&info,(DWORD)sizeof(info),&returned,NULL)
       && returned>=sizeof(info) && info.Length.QuadPart>=0){
        *bytes=(uint64_t)info.Length.QuadPart;
        return 1;
    }
    LARGE_INTEGER size;
    if(GetFileSizeEx(handle,&size)&&size.QuadPart>=0){
        *bytes=(uint64_t)size.QuadPart;
        return 1;
    }
    return 0;
}

static openfs_io_result_t transfer(openfs_windows_adapter_t *adapter,uint64_t offset64,void *buffer,DWORD bytes,int write)
{
    if(bytes==0U||offset64>INT64_MAX)return OPENFS_IO_OUT_OF_RANGE;
    adapter->last_error=ERROR_SUCCESS;
    OVERLAPPED ov={0};
    ov.Offset=(DWORD)offset64;
    ov.OffsetHigh=(DWORD)(offset64>>32U);
    ov.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(ov.hEvent==NULL){adapter->last_error=GetLastError();return OPENFS_IO_IO_ERROR;}
    DWORD done=0U;
    BOOL ok=write?WriteFile(adapter->handle,buffer,bytes,NULL,&ov):ReadFile(adapter->handle,buffer,bytes,NULL,&ov);
    if(!ok){
        DWORD error=GetLastError();
        if(error!=ERROR_IO_PENDING){adapter->last_error=error;CloseHandle(ov.hEvent);return OPENFS_IO_IO_ERROR;}
        ok=GetOverlappedResult(adapter->handle,&ov,&done,TRUE);
        if(!ok)adapter->last_error=GetLastError();
    }else{
        ok=GetOverlappedResult(adapter->handle,&ov,&done,TRUE);
        if(!ok)adapter->last_error=GetLastError();
    }
    CloseHandle(ov.hEvent);
    if(ok&&done==bytes)return OPENFS_IO_OK;
    if(ok)adapter->last_error=ERROR_WRITE_FAULT;
    return OPENFS_IO_IO_ERROR;
}

static openfs_io_result_t windows_read(void *context,uint64_t first,uint32_t count,void *buffer)
{
    openfs_windows_adapter_t *a=context;
    if(a==NULL||buffer==NULL||count==0U)return OPENFS_IO_INVALID_ARGUMENT;
    if(first>=a->device.block_count||(uint64_t)count>a->device.block_count-first)return OPENFS_IO_OUT_OF_RANGE;
    if(first>UINT64_MAX/(uint64_t)a->device.block_size)return OPENFS_IO_OUT_OF_RANGE;
    uint64_t bytes64=(uint64_t)count*a->device.block_size;
    if(bytes64>UINT32_MAX||first>(UINT64_MAX-(uint64_t)bytes64)/(uint64_t)a->device.block_size)return OPENFS_IO_OUT_OF_RANGE;
    uint64_t offset=first*(uint64_t)a->device.block_size;if(offset>INT64_MAX-(uint64_t)bytes64)return OPENFS_IO_OUT_OF_RANGE;return transfer(a,offset,buffer,(DWORD)bytes64,0);
}

static openfs_io_result_t windows_write(void *context,uint64_t first,uint32_t count,const void *buffer)
{
    openfs_windows_adapter_t *a=context;
    if(a==NULL||buffer==NULL||count==0U)return OPENFS_IO_INVALID_ARGUMENT;
    if(!a->writable)return OPENFS_IO_READ_ONLY;
    if(first>=a->device.block_count||(uint64_t)count>a->device.block_count-first)return OPENFS_IO_OUT_OF_RANGE;
    if(first>UINT64_MAX/(uint64_t)a->device.block_size)return OPENFS_IO_OUT_OF_RANGE;
    uint64_t bytes64=(uint64_t)count*a->device.block_size;
    if(bytes64>UINT32_MAX||first>(UINT64_MAX-(uint64_t)bytes64)/(uint64_t)a->device.block_size)return OPENFS_IO_OUT_OF_RANGE;
    uint64_t offset=first*(uint64_t)a->device.block_size;if(offset>INT64_MAX-(uint64_t)bytes64)return OPENFS_IO_OUT_OF_RANGE;return transfer(a,offset,(void *)buffer,(DWORD)bytes64,1);
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
    if(adapter==NULL||path==NULL||block_size<OPENFS_MIN_BLOCK_SIZE||block_size>OPENFS_MAX_BLOCK_SIZE||(block_size&(block_size-1U))!=0U)return OPENFS_WINDOWS_ADAPTER_INVALID_ARGUMENT;
    DWORD access=writable?(GENERIC_READ|GENERIC_WRITE):GENERIC_READ;
    HANDLE h=CreateFileW(path,access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED,NULL);
    if(h==INVALID_HANDLE_VALUE)return OPENFS_WINDOWS_ADAPTER_IO_ERROR;
    uint64_t bytes=0U;
    if(!query_device_size(h,&bytes)){
        CloseHandle(h);return OPENFS_WINDOWS_ADAPTER_IO_ERROR;
    }
    if(bytes<(uint64_t)block_size){
        CloseHandle(h);return OPENFS_WINDOWS_ADAPTER_UNSUPPORTED;
    }
    adapter->handle=h;adapter->writable=writable?1:0;adapter->last_error=ERROR_SUCCESS;adapter->device.context=adapter;
    adapter->device.block_size=block_size;adapter->device.block_count=bytes/(uint64_t)block_size;
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

DWORD openfs_windows_adapter_last_error(const openfs_windows_adapter_t *adapter)
{
    return adapter==NULL?ERROR_INVALID_PARAMETER:adapter->last_error;
}
#else
/* The implementation is intentionally inert on non-Windows hosts. */
#endif
