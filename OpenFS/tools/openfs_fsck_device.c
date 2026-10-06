#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "openfs_windows_adapter.h"
#include "openfs/format.h"
#include "openfs/fsck.h"

typedef struct fsck_progress_state {
    ULONGLONG start_ms;
    ULONGLONG sample_ms;
    uint64_t sample_done;
    uint64_t sample_total;
    int sample_ready;
} fsck_progress_state_t;

static void format_duration(double seconds,wchar_t *out,size_t out_count)
{
    if(out==NULL||out_count==0U)return;
    if(seconds<0.0||seconds>86400000.0){wcsncpy(out,L"--:--",out_count-1U);out[out_count-1U]=L'\\0';return;}
    unsigned long long total=(unsigned long long)(seconds+0.5);
    unsigned long long hours=total/3600ULL;
    unsigned long long minutes=(total%3600ULL)/60ULL;
    unsigned long long secs=total%60ULL;
    if(hours>0ULL)swprintf(out,out_count,L"%llu:%02llu:%02llu",hours,minutes,secs);
    else swprintf(out,out_count,L"%02llu:%02llu",minutes,secs);
}

static void print_fsck_progress(void *context,uint64_t done,uint64_t total,const char *stage)
{
    fsck_progress_state_t *state=(fsck_progress_state_t *)context;
    if(state==NULL)return;
    if(total==0U)total=1U;
    ULONGLONG now=GetTickCount64();
    if(state->start_ms==0U)state->start_ms=now;
    ULONGLONG elapsed_ms=now-state->start_ms;
    if(!state->sample_ready&&elapsed_ms>=10000ULL){
        state->sample_ready=1;
        state->sample_ms=now;
        state->sample_done=done;
        state->sample_total=total;
    }
    wchar_t eta[32]=L"--:--";
    if(state->sample_ready&&done>state->sample_done&&total>done){
        ULONGLONG sample_elapsed=now-state->sample_ms;
        uint64_t progress_delta=done-state->sample_done;
        if(sample_elapsed>0U&&progress_delta>0U){
            double remaining=(double)(total-done)*(double)sample_elapsed/(double)progress_delta/1000.0;
            format_duration(remaining,eta,sizeof(eta)/sizeof(eta[0]));
        }
    }
    uint64_t percent=done>=total?100U:(done*100U)/total;
    unsigned width=40U;
    unsigned filled=(unsigned)((percent*width)/100U);
    fwprintf(stdout,L"\\rFSCK [");
    for(unsigned i=0U;i<width;i++)fputwc(i<filled?L'#':L'-',stdout);
    fwprintf(stdout,L"] %3llu%%  %hs  ETA %ls",
             (unsigned long long)percent,stage!=NULL?stage:"",eta);
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
    fsck_progress_state_t progress_state={0};
    openfs_fsck_result_t result=openfs_fsck_with_progress(device,&sb,&errors,print_fsck_progress,&progress_state);
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
