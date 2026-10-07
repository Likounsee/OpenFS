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
        assert(openfs_mutex_lock(&r->lifecycle_lock,OPENFS_LOCK_RANK_LIFECYCLE)==OPENFS_LOCK_OK);
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
    assert(openfs_sync(&m)==OPENFS_MOUNT_INVALID_ARGUMENT);
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
 concurrent_unmount_runtime_lock_regression();
 return 0;
}
