#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "openfs_windows_adapter.h"
#include "openfs/format.h"
#include "openfs/inode.h"
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
    uint32_t format_flags=OPENFS_FORMAT_FLAG_NONE;
    int run_fsck=0;
    openfs_format_result_t format_result;
    openfs_windows_adapter_result_t adapter_result;

    if(argc<2||argc>4){
        fwprintf(stderr,L"Usage: %s <device> [block_size] [--full-zero] [--fsck]\n",argv[0]);
        return 2;
    }
    for(int i=2;i<argc;i++){
        if(wcscmp(argv[i],L"--full-zero")==0){format_flags|=OPENFS_FORMAT_FLAG_FULL_ZERO;continue;}
        if(wcscmp(argv[i],L"--fsck")==0){run_fsck=1;continue;}
        if(i!=2||!parse_block_size(argv[i],&block_size)){
            fwprintf(stderr,L"Invalid block size or option.\n");
            return 2;
        }
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
    wprintf(L"Format mode: %s\n",(format_flags&OPENFS_FORMAT_FLAG_FULL_ZERO)!=0U?L"full zero":L"fast");

    make_uuid(uuid);
    format_result=openfs_format_ex(device,uuid,format_flags);
    if(format_result!=OPENFS_FORMAT_OK){
        fwprintf(stderr,L"OpenFS format failed: %d (Windows error %lu).\n",(int)format_result,
                 (unsigned long)openfs_windows_adapter_last_error(&adapter));
        if(format_result==OPENFS_FORMAT_CORRUPT){
            openfs_superblock_t diagnostic={0};
            openfs_format_result_t diagnostic_result=openfs_prepare_superblock(device,uuid,&diagnostic);
            if(diagnostic_result==OPENFS_FORMAT_CORRUPT){
                const char *reason=openfs_validate_superblock_reason(device,&diagnostic);
                if(reason!=NULL)fprintf(stderr,"OpenFS validation diagnostic: %s\\n",reason);
            }
        }
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

    uint64_t inode_count=(superblock.inode_table_blocks*(uint64_t)superblock.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t root;
    if(openfs_inode_read(device,superblock.inode_table_start,superblock.root_inode,inode_count,&root)!=OPENFS_INODE_OK||
       root.inode_number!=superblock.root_inode||
       (root.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY){
        fwprintf(stderr,L"OpenFS root inode verification failed.\n");
        openfs_windows_adapter_close(&adapter);
        return 5;
    }
    if(run_fsck){
        openfs_fsck_result_t fsck_result=openfs_fsck(device,&superblock,&checked);
        if(fsck_result!=OPENFS_FSCK_OK||checked!=0U){
            fwprintf(stderr,L"OpenFS fsck verification failed: result=%d errors=%llu WindowsError=%lu.\n",(int)fsck_result,(unsigned long long)checked,(unsigned long)openfs_windows_adapter_last_error(&adapter));
            openfs_windows_adapter_close(&adapter);
            return 5;
        }
    }

    if(openfs_windows_adapter_close(&adapter)!=OPENFS_WINDOWS_ADAPTER_OK){
        fwprintf(stderr,L"OpenFS device close failed.\n");
        return 6;
    }

    wprintf(L"OpenFS device formatted successfully.\n");
    return 0;
}
