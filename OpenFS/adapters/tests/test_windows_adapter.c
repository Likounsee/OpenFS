#include <assert.h>
#include <stdint.h>
#include <wchar.h>
#include <string.h>
#include <windows.h>
#include "openfs_windows_adapter.h"

int wmain(void)
{
    wchar_t path[MAX_PATH];
    wchar_t temp[MAX_PATH];
    assert(GetTempPathW(MAX_PATH,temp)>0U);
    assert(GetTempFileNameW(temp,L"ofs",0,path)!=0U);
    HANDLE h=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,0,NULL,TRUNCATE_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    assert(h!=INVALID_HANDLE_VALUE);
    LARGE_INTEGER size;size.QuadPart=8192;
    assert(SetFilePointerEx(h,size,NULL,FILE_BEGIN));
    assert(SetEndOfFile(h));
    CloseHandle(h);

    openfs_windows_adapter_t a;
    assert(openfs_windows_adapter_open(&a,path,3000U,1)==OPENFS_WINDOWS_ADAPTER_INVALID_ARGUMENT);assert(openfs_windows_adapter_open(&a,path,4096U,1)==OPENFS_WINDOWS_ADAPTER_OK);
    uint8_t w[4096],r[4096];memset(w,0x5AU,sizeof(w));memset(r,0,sizeof(r));
    assert(a.device.write(a.device.context,1U,1U,w)==OPENFS_IO_OK);
    assert(a.device.flush(a.device.context)==OPENFS_IO_OK);
    assert(a.device.read(a.device.context,1U,1U,r)==OPENFS_IO_OK);
    assert(memcmp(w,r,sizeof(w))==0);
    assert(a.device.read(a.device.context,2U,1U,r)==OPENFS_IO_OUT_OF_RANGE);
    assert(openfs_windows_adapter_close(&a)==OPENFS_WINDOWS_ADAPTER_OK);

    assert(openfs_windows_adapter_open(&a,path,4096U,0)==OPENFS_WINDOWS_ADAPTER_OK);
    assert(a.device.write(a.device.context,0U,1U,w)==OPENFS_IO_READ_ONLY);
    assert(openfs_windows_adapter_close(&a)==OPENFS_WINDOWS_ADAPTER_OK);
    DeleteFileW(path);
    return 0;
}
