#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "openfs_windows_adapter.h"
#include "openfs/format.h"
#include "openfs/fsck.h"

static int parse_block_size(const wchar_t *s,uint32_t *out)
{
    wchar_t *end=NULL;
    unsigned long long value;
    if(s==NULL||out==NULL||*s==L'\0')return 0;
    value=wcstoull(s,&end,10);
    if(end==s||*end!=L'\0'||value>UINT32_MAX)return 0;
    *out=(uint32_t)value;
    return 1;
}

static void make_uuid(uint8_t uuid[16])
{
    LARGE_INTEGER counter;
    uint64_t a,b;
    QueryPerformanceCounter(&counter);
    a=(uint64_t)counter.QuadPart^(uint64_t)GetTickCount64()^((uint64_t)GetCurrentProcessId()<<32U);
    b=((uint64_t)GetCurrentThreadId()<<32U)^(uint64_t)GetTickCount64()^a;
    memcpy(uuid,&a,8U);
    memcpy(uuid+8U,&b,8U);
    uuid[6]=(uint8_t)((uuid[6]&0x0fU)|0x40U);
    uuid[8]=(uint8_t)((uuid[8]&0x3fU)|0x80U);
}

int wmain(int argc,wchar_t **argv)
{
    openfs_windows_adapter_t adapter;
    openfs_block_device_t *device;
    openfs_superblock_t superblock;
    uint8_t uuid[16];
    uint64_t checked=0U;
    uint32_t block_size=65536U;
    openfs_format_result_t format_result;
    openfs_windows_adapter_result_t adapter_result;

    if(argc<2||argc>3){
        fwprintf(stderr,L"Usage: %s <device> [block_size]\n",argv[0]);
        return 2;
    }
    if(argc==3&&!parse_block_size(argv[2],&block_size)){
        fwprintf(stderr,L"Invalid block size.\n");
        return 2;
    }
    if(block_size<OPENFS_MIN_BLOCK_SIZE||block_size>OPENFS_MAX_BLOCK_SIZE||
       (block_size&(block_size-1U))!=0U){
        fwprintf(stderr,L"Block size must be a power of two from %u to %u.\n",
                 OPENFS_MIN_BLOCK_SIZE,OPENFS_MAX_BLOCK_SIZE);
        return 2;
    }

    memset(&adapter,0,sizeof(adapter));
    adapter.handle=INVALID_HANDLE_VALUE;
    adapter_result=openfs_windows_adapter_open(&adapter,argv[1],block_size,1);
    if(adapter_result!=OPENFS_WINDOWS_ADAPTER_OK){
        fwprintf(stderr,L"Cannot open device '%s' for writing (adapter error %d).\n",
                 argv[1],(int)adapter_result);
        return 3;
    }

    device=openfs_windows_adapter_device(&adapter);
    if(device==NULL){
        fwprintf(stderr,L"Invalid OpenFS device adapter.\n");
        openfs_windows_adapter_close(&adapter);
        return 3;
    }

    wprintf(L"Formatting: %s\n",argv[1]);
    wprintf(L"Size: %llu bytes\n",
            (unsigned long long)(device->block_count*(uint64_t)device->block_size));
    wprintf(L"Block size: %u bytes\n",device->block_size);
    wprintf(L"Blocks: %llu\n",(unsigned long long)device->block_count);

    make_uuid(uuid);
    format_result=openfs_format(device,uuid);
    if(format_result!=OPENFS_FORMAT_OK){
        fwprintf(stderr,L"OpenFS format failed: %d (Windows error %lu).\n",(int)format_result,
                 (unsigned long)openfs_windows_adapter_last_error(&adapter));
        openfs_windows_adapter_close(&adapter);
        return 4;
    }

    if(openfs_read_superblock(device,&superblock)!=OPENFS_FORMAT_OK||
       superblock.total_blocks!=device->block_count||
       superblock.block_size!=device->block_size||
       memcmp(superblock.uuid,uuid,sizeof(uuid))!=0){
        fwprintf(stderr,L"OpenFS format verification failed.\n");
        openfs_windows_adapter_close(&adapter);
        return 5;
    }

    if(openfs_fsck(device,&superblock,&checked)!=OPENFS_FSCK_OK){
        fwprintf(stderr,L"OpenFS fsck verification failed.\n");
        openfs_windows_adapter_close(&adapter);
        return 5;
    }

    if(openfs_windows_adapter_close(&adapter)!=OPENFS_WINDOWS_ADAPTER_OK){
        fwprintf(stderr,L"OpenFS device close failed.\n");
        return 6;
    }

    wprintf(L"OpenFS device formatted successfully.\n");
    return 0;
}
