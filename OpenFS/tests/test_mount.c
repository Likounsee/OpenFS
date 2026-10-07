#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/mount.h"
#include "openfs/crc32c.h"
#include "openfs/fsck.h"
#include "openfs/file.h"
#include "openfs/inode_alloc.h"
#include "openfs/path.h"
#include "openfs/fd.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

typedef struct {uint8_t *bytes;uint32_t block_size;uint64_t block_count;unsigned flushes;uint64_t partial_block;size_t partial_bytes;size_t partial_next_bytes;int partial_enabled;int partial_once;int partial_change;} disk_t;
static openfs_io_result_t rd(void*c,uint64_t f,uint32_t n,void*b){disk_t*d=c;if(n==0U||f>=d->block_count||(uint64_t)n>d->block_count-f)return OPENFS_IO_OUT_OF_RANGE;memcpy(b,d->bytes+(size_t)(f*d->block_size),(size_t)((uint64_t)n*d->block_size));return OPENFS_IO_OK;}
static openfs_io_result_t wr(void*c,uint64_t f,uint32_t n,const void*b){disk_t*d=c;if(n==0U||f>=d->block_count||(uint64_t)n>d->block_count-f)return OPENFS_IO_OUT_OF_RANGE;size_t bytes=(size_t)((uint64_t)n*d->block_size);if(d->partial_enabled&&f==d->partial_block){size_t copy=d->partial_bytes!=0U&&d->partial_bytes<bytes?d->partial_bytes:bytes;memcpy(d->bytes+(size_t)(f*d->block_size),b,copy);if(d->partial_once){if(d->partial_change)d->partial_bytes=d->partial_next_bytes;else d->partial_enabled=0;}return OPENFS_IO_IO_ERROR;}memcpy(d->bytes+(size_t)(f*d->block_size),b,bytes);return OPENFS_IO_OK;}
static openfs_io_result_t fl(void*c){((disk_t*)c)->flushes++;return OPENFS_IO_OK;} static openfs_journal_result_t replay_probe(void*c,uint64_t tx,const uint8_t*p,uint32_t n){unsigned *hits=c;(void)tx;(void)p;if(n>=24U&&memcmp(p,"OJBD1",5U)==0)(*hits)++;return OPENFS_JOURNAL_OK;}

typedef struct {
    openfs_mount_t *mount;
    int result;
} unmount_worker_context_t;
#if defined(_WIN32)
static unsigned __stdcall unmount_worker(void *arg)
#else
static void *unmount_worker(void *arg)
#endif
{
    unmount_worker_context_t *ctx=(unmount_worker_context_t *)arg;
    ctx->result=(int)openfs_unmount(ctx->mount);
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

typedef struct {
    openfs_runtime_t *runtime;
} runtime_destroy_worker_context_t;

#if defined(_WIN32)
static unsigned __stdcall runtime_destroy_worker(void *arg)
#else
static void *runtime_destroy_worker(void *arg)
#endif
{
    runtime_destroy_worker_context_t *ctx=(runtime_destroy_worker_context_t *)arg;
    openfs_runtime_destroy(ctx->runtime);
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

static void unmount_open_handle_regression(void)
{
    disk_t d={.block_size=4096U,.block_count=128U};
    d.bytes=calloc((size_t)d.block_count,d.block_size);assert(d.bytes);
    openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};
    uint8_t uuid[16]={0x52U};
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_mount_t m;assert(openfs_mount(&m,&v)==OPENFS_MOUNT_OK);
    openfs_file_handle_t *h=NULL;
    assert(openfs_fd_open(&v,&m.superblock,"/held",OPENFS_FD_CREAT|OPENFS_FD_RDWR,OPENFS_INODE_MODE_REGULAR|0644U,&h)==OPENFS_FD_OK);
    assert(openfs_unmount(&m)==OPENFS_MOUNT_IO_ERROR);
    assert(openfs_fd_close(h)==OPENFS_FD_OK);
    assert(openfs_fd_close(h)==OPENFS_FD_CLOSED);
    assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
    free(d.bytes);
}

static void runtime_shutdown_handle_admission_regression(void)
{
    openfs_runtime_t runtime;
    int device_marker=0;
    assert(openfs_runtime_init(&runtime)==1);
    assert(openfs_runtime_handle_acquire(&runtime,&device_marker,1U,1U)==1);
    openfs_runtime_destroy(&runtime);
    assert(runtime.initialized!=0&&runtime.accepting!=0);
    assert(openfs_runtime_shutdown_if_unused(&runtime)==0);
    assert(runtime.initialized!=0&&runtime.accepting!=0);
    assert(openfs_runtime_handle_count_all(&runtime)==1U);
    assert(openfs_runtime_handle_release(&runtime,&device_marker,1U,1U)==1);
    assert(openfs_runtime_shutdown_if_unused(&runtime)==1);
    assert(runtime.initialized==0&&runtime.accepting==0);
}

static void concurrent_runtime_destroy_regression(void)
{
    openfs_runtime_t runtime;
    assert(openfs_runtime_init(&runtime)==1);
    assert(openfs_runtime_enter(&runtime)==1);

    runtime_destroy_worker_context_t ctx={&runtime};
#if defined(_WIN32)
    HANDLE a=(HANDLE)_beginthreadex(NULL,0U,runtime_destroy_worker,&ctx,0U,NULL);
    HANDLE b=(HANDLE)_beginthreadex(NULL,0U,runtime_destroy_worker,&ctx,0U,NULL);
    assert(a!=NULL&&b!=NULL);
#else
    pthread_t a,b;
    assert(pthread_create(&a,NULL,runtime_destroy_worker,&ctx)==0);
    assert(pthread_create(&b,NULL,runtime_destroy_worker,&ctx)==0);
#endif

    for(unsigned i=0U;i<100000U;i++){
        assert(openfs_mutex_lock(&runtime.lifecycle_lock,OPENFS_LOCK_RANK_HANDLE)==OPENFS_LOCK_OK);
        int accepting=runtime.accepting;
        assert(openfs_mutex_unlock(&runtime.lifecycle_lock)==OPENFS_LOCK_OK);
        if(!accepting)break;
#if defined(_WIN32)
        Sleep(0);
#else
        sched_yield();
#endif
        assert(i+1U<100000U);
    }

    openfs_runtime_leave(&runtime);
#if defined(_WIN32)
    assert(WaitForSingleObject(a,60000U)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(b,60000U)==WAIT_OBJECT_0);
    CloseHandle(a);CloseHandle(b);
#else
    assert(pthread_join(a,NULL)==0);
    assert(pthread_join(b,NULL)==0);
#endif
    assert(runtime.initialized==0);
    assert(runtime.accepting==0);
}

static void concurrent_double_unmount_regression(void)
{
    disk_t d={.block_size=4096U,.block_count=128U};
    d.bytes=calloc((size_t)d.block_count,d.block_size);assert(d.bytes);
    openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};
    uint8_t uuid[16]={0x51U};
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_mount_t m;assert(openfs_mount(&m,&v)==OPENFS_MOUNT_OK);

    unmount_worker_context_t a={&m,(int)OPENFS_MOUNT_IO_ERROR};
    unmount_worker_context_t b={&m,(int)OPENFS_MOUNT_IO_ERROR};
#if defined(_WIN32)
    HANDLE ta=(HANDLE)_beginthreadex(NULL,0U,unmount_worker,&a,0U,NULL);
    HANDLE tb=(HANDLE)_beginthreadex(NULL,0U,unmount_worker,&b,0U,NULL);
    assert(ta!=NULL&&tb!=NULL);
    HANDLE handles[2]={ta,tb};
    assert(WaitForMultipleObjects(2,handles,TRUE,60000U)==WAIT_OBJECT_0);
    CloseHandle(ta);CloseHandle(tb);
#else
    pthread_t ta,tb;
    assert(pthread_create(&ta,NULL,unmount_worker,&a)==0);
    assert(pthread_create(&tb,NULL,unmount_worker,&b)==0);
    assert(pthread_join(ta,NULL)==0);
    assert(pthread_join(tb,NULL)==0);
#endif
    assert((a.result==OPENFS_MOUNT_OK&&b.result==OPENFS_MOUNT_INVALID_ARGUMENT) ||
           (b.result==OPENFS_MOUNT_OK&&a.result==OPENFS_MOUNT_INVALID_ARGUMENT));
    assert(openfs_unmount(&m)==OPENFS_MOUNT_INVALID_ARGUMENT);
    free(d.bytes);
}

static void stale_superblock_is_rejected_after_unmount(void)
{
    disk_t d={.block_size=4096U,.block_count=128U};
    d.bytes=calloc((size_t)d.block_count,d.block_size);assert(d.bytes);
    openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};
    uint8_t uuid[16]={0x46U};
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_mount_t m;assert(openfs_mount(&m,&v)==OPENFS_MOUNT_OK);
    openfs_superblock_t stale=m.superblock;
    openfs_inode_t inode;memset(&inode,0,sizeof(inode));inode.inode_number=1U;inode.generation=1U;inode.mode=OPENFS_INODE_MODE_REGULAR;
    assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
    assert(stale.runtime!=NULL);
    uint8_t byte=0U;size_t got=0U;
    assert(openfs_file_read(&v,&stale,&inode,0U,&byte,1U,&got)==OPENFS_FILE_IO_ERROR);
    free(d.bytes);
}

static void stale_runtime_admission_is_rejected(void)
{
    openfs_runtime_t runtime;
    assert(openfs_runtime_init(&runtime)==1);
    assert(openfs_runtime_enter(&runtime)==1);
    openfs_runtime_leave(&runtime);
    openfs_runtime_destroy(&runtime);
    assert(openfs_runtime_enter(&runtime)==0);
}

static void runtime_admission_unmount_barrier_regression(void)
{
    disk_t d={.block_size=4096U,.block_count=128U};
    d.bytes=calloc((size_t)d.block_count,d.block_size);assert(d.bytes);
    openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};uint8_t uuid[16]={0x45U};
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_mount_t m;assert(openfs_mount(&m,&v)==OPENFS_MOUNT_OK);
    openfs_runtime_t *r=&m.runtime;
    assert(openfs_runtime_enter(r)==1);
    unmount_worker_context_t ctx={&m,(int)OPENFS_MOUNT_IO_ERROR};
#if defined(_WIN32)
    uintptr_t thread=_beginthreadex(NULL,0U,unmount_worker,&ctx,0U,NULL);
    assert(thread!=0U);
#else
    pthread_t thread;assert(pthread_create(&thread,NULL,unmount_worker,&ctx)==0);
#endif
    /* The worker must close runtime admission before waiting for our active pin. */
    for(unsigned i=0U;i<100000U;i++){
        assert(openfs_mutex_lock(&r->lifecycle_lock,OPENFS_LOCK_RANK_HANDLE)==OPENFS_LOCK_OK);
        int accepting=r->accepting;
        assert(openfs_mutex_unlock(&r->lifecycle_lock)==OPENFS_LOCK_OK);
        if(!accepting)break;
#if defined(_WIN32)
        Sleep(0);
#else
        sched_yield();
#endif
        assert(i+1U<100000U);
    }
    openfs_runtime_leave(r);
#if defined(_WIN32)
    assert(WaitForSingleObject((HANDLE)thread,60000U)==WAIT_OBJECT_0);CloseHandle((HANDLE)thread);
#else
    assert(pthread_join(thread,NULL)==0);
#endif
    assert(ctx.result==OPENFS_MOUNT_OK);
    assert(r->initialized==0&&r->accepting==0);
    free(d.bytes);
}

static void concurrent_unmount_runtime_lock_regression(void)
{
    disk_t d={.block_size=4096U,.block_count=128U};
    d.bytes=calloc((size_t)d.block_count,d.block_size);assert(d.bytes);
    openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};uint8_t uuid[16]={0x44U};
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_mount_t m;assert(openfs_mount(&m,&v)==OPENFS_MOUNT_OK);
    openfs_runtime_t *r=&m.runtime;
    assert(openfs_runtime_enter(r)==1);
    assert(openfs_mutex_lock(&r->inode_lock,OPENFS_LOCK_RANK_INODE)==OPENFS_LOCK_OK);
    assert(openfs_mutex_unlock(&r->inode_lock)==OPENFS_LOCK_OK);
    openfs_runtime_leave(r);
    assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);
    assert(r->initialized==0&&r->accepting==0);
    free(d.bytes);
}

static void backup_superblock_extent_tree_regression(void)
{
    disk_t d={.block_size=4096U,.block_count=512U};
    d.bytes=calloc((size_t)d.block_count,d.block_size);assert(d.bytes);
    openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};
    uint8_t uuid[16]={9U};
    assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_mount_t mounted;
    assert(openfs_mount(&mounted,&v)==OPENFS_MOUNT_OK);
    uint64_t ino_no=0U;
    assert(openfs_path_create(&v,&mounted.superblock,"/backup-extent-tree",OPENFS_INODE_MODE_REGULAR|0644U,&ino_no)==OPENFS_PATH_OK);
    uint64_t inode_count=(mounted.superblock.inode_table_blocks*(uint64_t)d.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t inode;
    assert(openfs_inode_read(&v,mounted.superblock.inode_table_start,ino_no,inode_count,&inode)==OPENFS_INODE_OK);

    const uint64_t count=(uint64_t)OPENFS_INODE_TREE_INLINE_EXTENT_MAX+2U;
    const size_t bytes=(size_t)(count*(uint64_t)d.block_size);
    uint8_t *data=malloc(bytes);assert(data);
    for(size_t i=0U;i<bytes;i++)data[i]=(uint8_t)(i*13U+7U);

    uint64_t spacers[OPENFS_INODE_TREE_INLINE_EXTENT_MAX]={0U};
    for(uint64_t n=0U;n<count;n++){
        assert(openfs_file_write(&v,&mounted.superblock,&inode,n*(uint64_t)d.block_size,
            data+(size_t)(n*d.block_size),d.block_size)==OPENFS_FILE_OK);
        assert(openfs_inode_read(&v,mounted.superblock.inode_table_start,ino_no,inode_count,&inode)==OPENFS_INODE_OK);
        if(n<OPENFS_INODE_TREE_INLINE_EXTENT_MAX)
            assert(openfs_alloc_block(&v,&mounted.superblock,&spacers[n])==OPENFS_ALLOC_OK);
    }
    assert(inode.extent_count>OPENFS_INODE_TREE_INLINE_EXTENT_MAX);
    assert(openfs_inode_get_extent_tree_root(&inode)!=0U);
    for(uint32_t n=0U;n<OPENFS_INODE_TREE_INLINE_EXTENT_MAX;n++)
        assert(openfs_free_block(&v,&mounted.superblock,spacers[n])==OPENFS_ALLOC_OK);
    assert(openfs_unmount(&mounted)==OPENFS_MOUNT_OK);

    d.bytes[0]^=0x5AU;
    openfs_mount_t fallback;
    assert(openfs_mount(&fallback,&v)==OPENFS_MOUNT_OK);
    openfs_inode_t recovered;
    assert(openfs_inode_read(&v,fallback.superblock.inode_table_start,ino_no,inode_count,&recovered)==OPENFS_INODE_OK);
    assert(openfs_inode_get_extent_tree_root(&recovered)!=0U);
    assert(recovered.size==bytes);
    assert(recovered.blocks==count);
    uint8_t *readback=malloc(bytes);assert(readback);
    size_t got=0U;
    assert(openfs_file_read(&v,&fallback.superblock,&recovered,0U,readback,bytes,&got)==OPENFS_FILE_OK);
    assert(got==bytes&&memcmp(readback,data,bytes)==0);
    d.bytes[0]^=0x5AU; /* restore the intentionally corrupted primary copy before fsck */

    uint8_t marker=0xE7U;
    uint64_t write_offset=(count-1U)*(uint64_t)d.block_size+123U;
    assert(openfs_file_write(&v,&fallback.superblock,&recovered,write_offset,&marker,1U)==OPENFS_FILE_OK);
    data[write_offset]=marker;
    assert(openfs_unmount(&fallback)==OPENFS_MOUNT_OK);

    openfs_mount_t remounted;
    assert(openfs_mount(&remounted,&v)==OPENFS_MOUNT_OK);
    assert(openfs_inode_read(&v,remounted.superblock.inode_table_start,ino_no,inode_count,&recovered)==OPENFS_INODE_OK);
    assert(recovered.size==bytes&&recovered.blocks==count);
    got=0U;
    assert(openfs_file_read(&v,&remounted.superblock,&recovered,write_offset,readback+write_offset,1U,&got)==OPENFS_FILE_OK);
    assert(got==1U&&readback[write_offset]==marker);
    uint64_t fsck_errors=0U;assert(openfs_fsck(&v,&remounted.superblock,&fsck_errors)==OPENFS_FSCK_OK&&fsck_errors==0U);
    assert(openfs_unmount(&remounted)==OPENFS_MOUNT_OK);
    free(readback);free(data);free(d.bytes);
}

int main(void){
 unmount_open_handle_regression();
 runtime_shutdown_handle_admission_regression();
 concurrent_runtime_destroy_regression();
 stale_runtime_admission_is_rejected();
 stale_superblock_is_rejected_after_unmount();
 concurrent_double_unmount_regression();
 concurrent_unmount_runtime_lock_regression();
 backup_superblock_extent_tree_regression();
 disk_t d={.block_size=4096U,.block_count=128U};d.bytes=calloc((size_t)d.block_count,d.block_size);assert(d.bytes);
 openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};uint8_t uuid[16]={7U};
 assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
 openfs_mount_t m;assert(openfs_mount(&m,&v)==OPENFS_MOUNT_OK&&m.mounted);
 assert(m.superblock.root_inode==1U&&m.superblock.runtime==&m.runtime&&m.runtime.initialized!=0);

 uint64_t target=m.superblock.data_start;uint8_t pattern[4096];for(size_t i=0U;i<sizeof(pattern);++i)pattern[i]=(uint8_t)(i^0x5AU);openfs_journal_t j;uint64_t tx=0U;assert(openfs_journal_open(&j,&v,&m.superblock)==OPENFS_JOURNAL_OK);assert(openfs_journal_begin(&j,&v,&tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_write_block(&j,&v,tx,target,pattern)==OPENFS_JOURNAL_OK);assert(openfs_journal_commit(&j,&v,tx)==OPENFS_JOURNAL_OK);memset(d.bytes+(size_t)(target*d.block_size),0U,d.block_size);unsigned replay_hits=0U;assert(openfs_journal_replay(&v,&m.superblock,replay_probe,&replay_hits)==OPENFS_JOURNAL_OK&&replay_hits==2U);assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);assert(m.runtime.initialized==0);openfs_mount_t replayed;assert(openfs_mount(&replayed,&v)==OPENFS_MOUNT_OK);assert(memcmp(d.bytes+(size_t)(target*d.block_size),pattern,sizeof(pattern))==0);openfs_superblock_t stable=replayed.superblock;assert(openfs_unmount(&replayed)==OPENFS_MOUNT_OK);uint8_t *backup=d.bytes+(size_t)((d.block_count-1U)*d.block_size);backup[148U]^=0xA5U;backup[4088U]=backup[4089U]=backup[4090U]=backup[4091U]=0U;uint32_t crc=openfs_crc32c(backup,4088U);backup[4088U]=(uint8_t)crc;backup[4089U]=(uint8_t)(crc>>8U);backup[4090U]=(uint8_t)(crc>>16U);backup[4091U]=(uint8_t)(crc>>24U);openfs_mount_t mismatched;assert(openfs_mount(&mismatched,&v)==OPENFS_MOUNT_CORRUPT);backup[148U]^=0xA5U;crc=openfs_crc32c(backup,4088U);backup[4088U]=(uint8_t)crc;backup[4089U]=(uint8_t)(crc>>8U);backup[4090U]=(uint8_t)(crc>>16U);backup[4091U]=(uint8_t)(crc>>24U);openfs_journal_t malformed;assert(openfs_journal_open(&malformed,&v,&stable)==OPENFS_JOURNAL_OK);assert(openfs_journal_begin(&malformed,&v,&tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_write(&malformed,&v,tx,"bad",3U)==OPENFS_JOURNAL_OK);assert(openfs_journal_commit(&malformed,&v,tx)==OPENFS_JOURNAL_OK);openfs_mount_t rejected;assert(openfs_mount(&rejected,&v)==OPENFS_MOUNT_CORRUPT);assert(openfs_journal_checkpoint(&malformed,&v)==OPENFS_JOURNAL_OK);
openfs_journal_t incomplete;assert(openfs_journal_open(&incomplete,&v,&stable)==OPENFS_JOURNAL_OK);assert(openfs_journal_begin(&incomplete,&v,&tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_write_block(&incomplete,&v,tx,target,pattern)==OPENFS_JOURNAL_OK);memset(d.bytes+(size_t)(target*d.block_size),0xA5U,d.block_size);openfs_mount_t incomplete_mount;assert(openfs_mount(&incomplete_mount,&v)==OPENFS_MOUNT_OK);assert(memcmp(d.bytes+(size_t)(target*d.block_size),pattern,sizeof(pattern))!=0);assert(openfs_unmount(&incomplete_mount)==OPENFS_MOUNT_OK);
d.bytes[(size_t)(stable.inode_table_start*d.block_size)+32U]^=0x55U;openfs_mount_t metadata_mount;assert(openfs_mount(&metadata_mount,&v)==OPENFS_MOUNT_OK);uint64_t fsck_errors=0U;assert(openfs_fsck(&v,&metadata_mount.superblock,&fsck_errors)==OPENFS_FSCK_CORRUPT&&fsck_errors>0U);assert(openfs_unmount(&metadata_mount)==OPENFS_MOUNT_OK);d.bytes[(size_t)(stable.inode_table_start*d.block_size)+32U]^=0x55U;
 openfs_mount_t fallback;assert(openfs_mount(&fallback,&v)==OPENFS_MOUNT_OK);
 assert(fallback.superblock.root_inode==1U&&memcmp(fallback.superblock.uuid,uuid,16U)==0);
 assert(openfs_unmount(&fallback)==OPENFS_MOUNT_OK);uint8_t *generation_backup=d.bytes+(size_t)((d.block_count-1U)*d.block_size);generation_backup[140U]=(uint8_t)(stable.generation+1U);generation_backup[141U]=(uint8_t)((stable.generation+1U)>>8U);generation_backup[142U]=(uint8_t)((stable.generation+1U)>>16U);generation_backup[143U]=(uint8_t)((stable.generation+1U)>>24U);generation_backup[144U]=(uint8_t)((stable.generation+1U)>>32U);generation_backup[145U]=(uint8_t)((stable.generation+1U)>>40U);generation_backup[146U]=(uint8_t)((stable.generation+1U)>>48U);generation_backup[147U]=(uint8_t)((stable.generation+1U)>>56U);generation_backup[4088U]=generation_backup[4089U]=generation_backup[4090U]=generation_backup[4091U]=0U;crc=openfs_crc32c(generation_backup,4088U);generation_backup[4088U]=(uint8_t)crc;generation_backup[4089U]=(uint8_t)(crc>>8U);generation_backup[4090U]=(uint8_t)(crc>>16U);generation_backup[4091U]=(uint8_t)(crc>>24U);openfs_mount_t newer_generation;assert(openfs_mount(&newer_generation,&v)==OPENFS_MOUNT_OK);assert(newer_generation.superblock.generation==stable.generation+1U);assert(openfs_unmount(&newer_generation)==OPENFS_MOUNT_OK);
 openfs_journal_t pending;assert(openfs_journal_open(&pending,&v,&stable)==OPENFS_JOURNAL_OK);assert(openfs_journal_begin(&pending,&v,&tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_write_block(&pending,&v,tx,target,pattern)==OPENFS_JOURNAL_OK);assert(openfs_journal_commit(&pending,&v,tx)==OPENFS_JOURNAL_OK);memset(d.bytes+(size_t)(target*d.block_size),0U,d.block_size);d.partial_block=stable.journal_start+1U;d.partial_bytes=1024U;d.partial_enabled=1;d.partial_once=1;d.partial_change=0;openfs_mount_t checkpoint_io;assert(openfs_mount(&checkpoint_io,&v)==OPENFS_MOUNT_IO_ERROR);d.partial_enabled=0;openfs_journal_t restored;assert(openfs_journal_open(&restored,&v,&stable)==OPENFS_JOURNAL_OK);openfs_mount_t recovered;assert(openfs_mount(&recovered,&v)==OPENFS_MOUNT_OK);assert(memcmp(d.bytes+(size_t)(target*d.block_size),pattern,sizeof(pattern))==0);assert(openfs_unmount(&recovered)==OPENFS_MOUNT_OK);assert(openfs_journal_open(&pending,&v,&stable)==OPENFS_JOURNAL_OK);assert(openfs_journal_begin(&pending,&v,&tx)==OPENFS_JOURNAL_OK);assert(openfs_journal_write_block(&pending,&v,tx,target,pattern)==OPENFS_JOURNAL_OK);assert(openfs_journal_commit(&pending,&v,tx)==OPENFS_JOURNAL_OK);memset(d.bytes+(size_t)(target*d.block_size),0U,d.block_size);uint8_t checkpoint_saved[4096U];assert(v.read(v.context,stable.journal_start+1U,1U,checkpoint_saved)==OPENFS_IO_OK);d.partial_block=stable.journal_start+1U;d.partial_bytes=2048U;d.partial_next_bytes=1024U;d.partial_enabled=1;d.partial_once=1;d.partial_change=1;openfs_mount_t checkpoint_corrupt;assert(openfs_mount(&checkpoint_corrupt,&v)==OPENFS_MOUNT_CORRUPT);d.partial_enabled=0;assert(memcmp(d.bytes+(size_t)((stable.journal_start+1U)*d.block_size),checkpoint_saved,sizeof(checkpoint_saved))!=0);assert(openfs_journal_open(&restored,&v,&stable)==OPENFS_JOURNAL_CORRUPT); free(d.bytes);return 0;
}
