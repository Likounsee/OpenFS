#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "openfs_windows_adapter.h"
#include "openfs/format.h"
#include "openfs/fsck.h"

static void print_fsck_progress(void *context,uint64_t done,uint64_t total,const char *stage)
{
    (void)context;
    if(total==0U)total=1U;
    uint64_t percent=done>=total?100U:(done*100U)/total;
    unsigned width=40U;
    unsigned filled=(unsigned)((percent*width)/100U);
    fwprintf(stdout,L"\rFSCK [");
    for(unsigned i=0U;i<width;i++)fputwc(i<filled?L'#':L'-',stdout);
    fwprintf(stdout,L"] %3llu%%  %hs", (unsigned long long)percent, stage!=NULL?stage:"");
    fflush(stdout);
}

int wmain(int argc,wchar_t **argv)
{
    if(argc!=2){
        fwprintf(stderr,L"Usage: %s <device>\n",argv[0]);
        return 2;
    }

    openfs_windows_adapter_t adapter;
    memset(&adapter,0,sizeof(adapter));
    adapter.handle=INVALID_HANDLE_VALUE;

    openfs_windows_adapter_result_t ar=
        openfs_windows_adapter_open(&adapter,argv[1],65536U,0);
    if(ar!=OPENFS_WINDOWS_ADAPTER_OK){
        fwprintf(stderr,L"Cannot open device '%s' read-only (adapter error %d, Windows error %lu).\n",
                 argv[1],(int)ar,
                 (unsigned long)openfs_windows_adapter_last_error(&adapter));
        return 3;
    }

    openfs_block_device_t *device=openfs_windows_adapter_device(&adapter);
    openfs_superblock_t sb;
    openfs_format_result_t sr=openfs_read_superblock(device,&sb);
    if(sr!=OPENFS_FORMAT_OK){
        fwprintf(stderr,L"OpenFS superblock read failed: result=%d WindowsError=%lu.\n",
                 (int)sr,
                 (unsigned long)openfs_windows_adapter_last_error(&adapter));
        openfs_windows_adapter_close(&adapter);
        return 4;
    }

    uint64_t inode_count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
    wprintf(L"Device: %s\n",argv[1]);
    wprintf(L"Size: %llu bytes\n",
            (unsigned long long)(device->block_count*(uint64_t)device->block_size));
    wprintf(L"Block size: %u bytes\n",device->block_size);
    wprintf(L"Blocks: %llu\n",(unsigned long long)device->block_count);
    wprintf(L"Inode table blocks: %llu\n",(unsigned long long)sb.inode_table_blocks);
    wprintf(L"Inode count: %llu\n",(unsigned long long)inode_count);
    wprintf(L"Journal blocks: %llu\n",(unsigned long long)sb.journal_blocks);
    wprintf(L"Data blocks: %llu\n",(unsigned long long)sb.data_blocks);

    uint64_t errors=0U;
    openfs_fsck_result_t result=openfs_fsck_with_progress(device,&sb,&errors,print_fsck_progress,NULL);
wprintf(L"\n");
    if(result!=OPENFS_FSCK_OK||errors!=0U){
        fwprintf(stderr,L"OpenFS fsck failed: result=%d errors=%llu WindowsError=%lu.\n",
                 (int)result,(unsigned long long)errors,
                 (unsigned long)openfs_windows_adapter_last_error(&adapter));
        openfs_windows_adapter_close(&adapter);
        return 5;
    }

    wprintf(L"OpenFS fsck passed.\n");
    return openfs_windows_adapter_close(&adapter)==OPENFS_WINDOWS_ADAPTER_OK?0:6;
}
