#include "openfs/mount.h"

#include <stdlib.h>
#include <string.h>
#include "openfs/crc32c.h"
#include "openfs/journal.h"

#define OPENFS_CHECKSUM_OFFSET 4088U

static uint16_t get16(const uint8_t *p){return (uint16_t)p[0]|((uint16_t)p[1]<<8U);}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8U)|((uint32_t)p[2]<<16U)|((uint32_t)p[3]<<24U);}
static uint64_t get64(const uint8_t *p){uint64_t v=0U;for(unsigned k=0U;k<8U;++k)v|=(uint64_t)p[k]<<(8U*k);return v;}
static int same_layout(const openfs_superblock_t *a,const openfs_superblock_t *b)
{
    return a->version_major==b->version_major && a->version_minor==b->version_minor &&
        a->feature_flags==b->feature_flags && a->block_size==b->block_size &&
        a->total_blocks==b->total_blocks && a->metadata_start==b->metadata_start &&
        a->metadata_blocks==b->metadata_blocks && a->block_bitmap_start==b->block_bitmap_start &&
        a->block_bitmap_blocks==b->block_bitmap_blocks && a->inode_bitmap_start==b->inode_bitmap_start &&
        a->inode_bitmap_blocks==b->inode_bitmap_blocks && a->inode_table_start==b->inode_table_start &&
        a->inode_table_blocks==b->inode_table_blocks && a->journal_start==b->journal_start &&
        a->journal_blocks==b->journal_blocks && a->data_start==b->data_start &&
        a->data_blocks==b->data_blocks && a->root_inode==b->root_inode &&
        memcmp(a->uuid,b->uuid,sizeof(a->uuid))==0;
}

static openfs_format_result_t read_at(openfs_block_device_t *d,uint64_t block,openfs_superblock_t *out)
{
    if (block >= d->block_count) return OPENFS_FORMAT_CORRUPT;
    uint8_t *raw=malloc(d->block_size);
    if(raw==NULL)return OPENFS_FORMAT_IO_ERROR;
    if(d->read(d->context,block,1U,raw)!=OPENFS_IO_OK){free(raw);return OPENFS_FORMAT_IO_ERROR;}
    if(memcmp(raw,"OPENFS\0\0",8U)!=0||get32(raw+24U)!=OPENFS_SUPERBLOCK_SIZE){free(raw);return OPENFS_FORMAT_CORRUPT;}
    uint32_t stored=get32(raw+OPENFS_CHECKSUM_OFFSET);
    uint8_t *copy=malloc(OPENFS_SUPERBLOCK_SIZE);
    if(copy==NULL){free(raw);return OPENFS_FORMAT_IO_ERROR;}
    memcpy(copy,raw,OPENFS_SUPERBLOCK_SIZE);
    copy[OPENFS_CHECKSUM_OFFSET]=copy[OPENFS_CHECKSUM_OFFSET+1U]=copy[OPENFS_CHECKSUM_OFFSET+2U]=copy[OPENFS_CHECKSUM_OFFSET+3U]=0U;
    if(stored!=openfs_crc32c(copy,OPENFS_CHECKSUM_OFFSET)){free(copy);free(raw);return OPENFS_FORMAT_CORRUPT;}
    free(copy);
    out->version_major=get16(raw+8U);out->version_minor=get16(raw+10U);out->feature_flags=get64(raw+12U);
    out->block_size=get32(raw+20U);out->total_blocks=get64(raw+28U);out->metadata_start=get64(raw+36U);out->metadata_blocks=get64(raw+44U);
    out->block_bitmap_start=get64(raw+52U);out->block_bitmap_blocks=get64(raw+60U);out->inode_bitmap_start=get64(raw+68U);out->inode_bitmap_blocks=get64(raw+76U);
    out->inode_table_start=get64(raw+84U);out->inode_table_blocks=get64(raw+92U);out->journal_start=get64(raw+100U);out->journal_blocks=get64(raw+108U);
    out->data_start=get64(raw+116U);out->data_blocks=get64(raw+124U);out->root_inode=get64(raw+132U);out->generation=get64(raw+140U);
    memcpy(out->uuid,raw+148U,16U);free(raw);
    return openfs_validate_superblock(d,out);
}


static openfs_journal_result_t replay_block(void *ctx,uint64_t tx,const uint8_t *data,uint32_t len){(void)tx;openfs_mount_t*m=(openfs_mount_t*)ctx;if(m==NULL||data==NULL)return OPENFS_JOURNAL_INVALID_ARGUMENT;if(len<24U||memcmp(data,"OJBD1",5U)!=0)return OPENFS_JOURNAL_CORRUPT;uint64_t target=get64(data+8U),offset=(uint64_t)get32(data+16U);uint32_t count=get32(data+20U);if(target>=m->device->block_count||count==0U||offset>=(uint64_t)m->device->block_size||count>(uint32_t)((uint64_t)m->device->block_size-offset)||count>len-24U)return OPENFS_JOURNAL_CORRUPT;uint8_t*b=malloc(m->device->block_size);if(b==NULL)return OPENFS_JOURNAL_IO_ERROR;if(m->device->read(m->device->context,target,1U,b)!=OPENFS_IO_OK){free(b);return OPENFS_JOURNAL_IO_ERROR;}memcpy(b+(size_t)offset,data+24U,count);openfs_io_result_t io=m->device->write(m->device->context,target,1U,b);free(b);return io==OPENFS_IO_OK?OPENFS_JOURNAL_OK:OPENFS_JOURNAL_IO_ERROR;}

openfs_mount_result_t openfs_mount(openfs_mount_t *mount,openfs_block_device_t *device)
{
    if(mount==NULL||!openfs_block_device_is_valid(device))return OPENFS_MOUNT_INVALID_ARGUMENT;
    memset(mount,0,sizeof(*mount));
    openfs_superblock_t primary,backup;
    openfs_format_result_t pr=read_at(device,0U,&primary);
    openfs_format_result_t br=read_at(device,device->block_count-1U,&backup);
    if(pr!=OPENFS_FORMAT_OK&&br!=OPENFS_FORMAT_OK){
        return (pr==OPENFS_FORMAT_IO_ERROR||br==OPENFS_FORMAT_IO_ERROR)?OPENFS_MOUNT_IO_ERROR:OPENFS_MOUNT_CORRUPT;
    }
    if(pr==OPENFS_FORMAT_OK&&br==OPENFS_FORMAT_OK){
        if(!same_layout(&primary,&backup))return OPENFS_MOUNT_CORRUPT;
        mount->superblock=(backup.generation>primary.generation)?backup:primary;
    }else{
        mount->superblock=(pr==OPENFS_FORMAT_OK)?primary:backup;
    }
    mount->device=device;
    openfs_journal_result_t jr=openfs_journal_open(&mount->journal,device,&mount->superblock);
    if(jr!=OPENFS_JOURNAL_OK)return jr==OPENFS_JOURNAL_IO_ERROR?OPENFS_MOUNT_IO_ERROR:OPENFS_MOUNT_CORRUPT;
    jr=openfs_journal_replay(device,&mount->superblock,replay_block,mount);
    if(jr!=OPENFS_JOURNAL_OK)return jr==OPENFS_JOURNAL_IO_ERROR?OPENFS_MOUNT_IO_ERROR:OPENFS_MOUNT_CORRUPT;
    if(device->flush(device->context)!=OPENFS_IO_OK)return OPENFS_MOUNT_IO_ERROR;
    if(openfs_journal_checkpoint(&mount->journal,device)!=OPENFS_JOURNAL_OK)return OPENFS_MOUNT_IO_ERROR;
    mount->mounted=1;
    return OPENFS_MOUNT_OK;
}
openfs_mount_result_t openfs_sync(openfs_mount_t *mount)
{
    if(mount==NULL||!mount->mounted||mount->device==NULL)return OPENFS_MOUNT_INVALID_ARGUMENT;
    return mount->device->flush(mount->device->context)==OPENFS_IO_OK?OPENFS_MOUNT_OK:OPENFS_MOUNT_IO_ERROR;
}
openfs_mount_result_t openfs_unmount(openfs_mount_t *mount)
{
    if(mount==NULL||!mount->mounted)return OPENFS_MOUNT_INVALID_ARGUMENT;
    openfs_mount_result_t r=openfs_sync(mount);
    if(r!=OPENFS_MOUNT_OK)return r;
    mount->mounted=0;mount->device=NULL;memset(&mount->superblock,0,sizeof(mount->superblock));
    return OPENFS_MOUNT_OK;
}
