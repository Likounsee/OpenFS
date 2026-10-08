#include "openfs/fsck.h"
#include "openfs/bitmap.h"
#include "openfs/extent.h"
#include "openfs/file.h"
#include "openfs/dir.h"
#include "openfs/crc32c.h"
#include "openfs/journal.h"
#include "openfs/path.h"
#include "openfs/xattr.h"
#include "openfs/inode.h"
#include "openfs/runtime.h"
#include "openfs/cow.h"
#include "openfs/metadata_root.h"
#include "openfs/data_checksum.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define OPENFS_FSCK_MAX_REF_BYTES (64U * 1024U * 1024U)
#define OPENFS_FSCK_MAX_DIR_REFS (8U * 1024U * 1024U)
static uint16_t sb_get16(const uint8_t*p){return (uint16_t)p[0]|((uint16_t)p[1]<<8U);}
static uint32_t sb_get32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8U)|((uint32_t)p[2]<<16U)|((uint32_t)p[3]<<24U);}
static uint64_t sb_get64(const uint8_t*p){uint64_t v=0U;for(unsigned k=0U;k<8U;k++)v|=(uint64_t)p[k]<<(8U*k);return v;}
static openfs_format_result_t fsck_read_superblock_at(openfs_block_device_t*d,uint64_t block,openfs_superblock_t*out){
    if(d==NULL||out==NULL||block>=d->block_count||d->block_size<OPENFS_SUPERBLOCK_SIZE)return OPENFS_FORMAT_CORRUPT;
    uint8_t*raw=malloc(d->block_size);if(raw==NULL)return OPENFS_FORMAT_IO_ERROR;
    if(d->read(d->context,block,1U,raw)!=OPENFS_IO_OK){free(raw);return OPENFS_FORMAT_IO_ERROR;}
    if(memcmp(raw,"OPENFS\0\0",8U)!=0||sb_get32(raw+24U)!=OPENFS_SUPERBLOCK_SIZE){free(raw);return OPENFS_FORMAT_CORRUPT;}
    uint32_t stored=sb_get32(raw+4088U);uint8_t copy[OPENFS_SUPERBLOCK_SIZE];
    memcpy(copy,raw,OPENFS_SUPERBLOCK_SIZE);copy[4088U]=copy[4089U]=copy[4090U]=copy[4091U]=0U;
    if(stored!=openfs_crc32c(copy,4088U)){free(raw);return OPENFS_FORMAT_CORRUPT;}
    for(size_t i=212U;i<4088U;i++)if(raw[i]!=0U){free(raw);return OPENFS_FORMAT_CORRUPT;}
    memset(out,0,sizeof(*out));
    out->version_major=sb_get16(raw+8U);out->version_minor=sb_get16(raw+10U);out->feature_flags=sb_get64(raw+12U);
    out->block_size=sb_get32(raw+20U);out->total_blocks=sb_get64(raw+28U);out->metadata_start=sb_get64(raw+36U);out->metadata_blocks=sb_get64(raw+44U);
    out->block_bitmap_start=sb_get64(raw+52U);out->block_bitmap_blocks=sb_get64(raw+60U);out->inode_bitmap_start=sb_get64(raw+68U);out->inode_bitmap_blocks=sb_get64(raw+76U);
    out->inode_table_start=sb_get64(raw+84U);out->inode_table_blocks=sb_get64(raw+92U);out->journal_start=sb_get64(raw+100U);out->journal_blocks=sb_get64(raw+108U);
    out->data_start=sb_get64(raw+116U);out->data_blocks=sb_get64(raw+124U);out->data_checksum_start=sb_get64(raw+196U);out->data_checksum_blocks=sb_get64(raw+204U);out->root_inode=sb_get64(raw+132U);out->generation=sb_get64(raw+140U);out->refcount_start=sb_get64(raw+164U);out->refcount_blocks=sb_get64(raw+172U);out->metadata_root_block=sb_get64(raw+180U);out->metadata_root_generation=sb_get64(raw+188U);
    memcpy(out->uuid,raw+148U,16U);free(raw);
    return openfs_validate_superblock(d,out);
}
static int fsck_same_superblock_layout(const openfs_superblock_t*a,const openfs_superblock_t*b){
    return a!=NULL&&b!=NULL&&a->version_major==b->version_major&&a->version_minor==b->version_minor&&
        a->feature_flags==b->feature_flags&&a->block_size==b->block_size&&a->total_blocks==b->total_blocks&&
        a->metadata_start==b->metadata_start&&a->metadata_blocks==b->metadata_blocks&&
        a->block_bitmap_start==b->block_bitmap_start&&a->block_bitmap_blocks==b->block_bitmap_blocks&&
        a->inode_bitmap_start==b->inode_bitmap_start&&a->inode_bitmap_blocks==b->inode_bitmap_blocks&&
        a->inode_table_start==b->inode_table_start&&a->inode_table_blocks==b->inode_table_blocks&&
        a->journal_start==b->journal_start&&a->journal_blocks==b->journal_blocks&&a->refcount_start==b->refcount_start&&a->refcount_blocks==b->refcount_blocks&&
        a->data_start==b->data_start&&a->data_blocks==b->data_blocks&&a->root_inode==b->root_inode&&a->metadata_root_block==b->metadata_root_block&&a->metadata_root_generation==b->metadata_root_generation&&
        memcmp(a->uuid,b->uuid,sizeof(a->uuid))==0;
}

typedef struct {
    uint8_t *data;
    uint64_t bytes;
    uint64_t bits;
} fsck_bitmap_snapshot_t;

static int fsck_bitmap_snapshot_load(const openfs_block_device_t *d,
    uint64_t start, uint64_t blocks, fsck_bitmap_snapshot_t *snapshot)
{
    if(d==NULL||snapshot==NULL||blocks==0U||d->block_size==0U||
       blocks>UINT64_MAX/d->block_size)return 0;
    uint64_t bytes=blocks*(uint64_t)d->block_size;
    if(bytes>SIZE_MAX||blocks>UINT32_MAX||bytes>UINT64_MAX/8U)return 0;
    uint8_t *data=(uint8_t *)malloc((size_t)bytes);
    if(data==NULL)return 0;
    if(blocks>UINT32_MAX)return 0;if(d->read(d->context,start,(uint32_t)blocks,data)!=OPENFS_IO_OK){
        free(data);return 0;
    }
    snapshot->data=data;
    snapshot->bytes=bytes;
    snapshot->bits=bytes*8U;
    return 1;
}

static int fsck_bitmap_snapshot_test(const fsck_bitmap_snapshot_t *snapshot,
    uint64_t bit,int *out)
{
    if(snapshot==NULL||snapshot->data==NULL||out==NULL||
       bit>=snapshot->bits)return 0;
    uint64_t byte=bit/8U;
    if(byte>=snapshot->bytes)return 0;
    *out=(snapshot->data[(size_t)byte]&(uint8_t)(1U<<(bit%8U)))!=0U;
    return 1;
}

static openfs_fsck_result_t icount(const openfs_superblock_t*s,uint64_t*n){if(s==NULL||s->block_size==0U||s->inode_table_blocks>UINT64_MAX/s->block_size)return OPENFS_FSCK_CORRUPT;*n=(s->inode_table_blocks*s->block_size)/OPENFS_INODE_SIZE;return *n?OPENFS_FSCK_OK:OPENFS_FSCK_CORRUPT;}
static int add(uint64_t a,uint64_t b,uint64_t*o){if(b>UINT64_MAX-a)return 0;*o=a+b;return 1;}
static void fsck_record_bad(uint64_t *bad,openfs_fsck_diagnostic_t *diag,const char *stage,const char *reason,uint64_t index,uint64_t total)
{
    if(bad==NULL)return;
    (*bad)++;
    if(diag!=NULL){
        diag->count++;
        if(diag->stage==NULL){
            diag->stage=stage;
            diag->reason=reason;
            diag->index=index;
            diag->total=total;
        }
    }
}

#define OPENFS_FSCK_IO_CHUNK (64U * 1024U * 1024U)

#define OPENFS_FSCK_CACHE_SLOTS 16U

typedef struct openfs_fsck_io_cache_slot {
    uint8_t *buffer;
    uint64_t start_block;
    uint32_t block_count;
    uint64_t last_use;
    int valid;
} openfs_fsck_io_cache_slot_t;

typedef struct openfs_fsck_io_cache {
    openfs_block_device_t *device;
    openfs_fsck_io_cache_slot_t slots[OPENFS_FSCK_CACHE_SLOTS];
    uint64_t use_counter;
} openfs_fsck_io_cache_t;

static openfs_io_result_t fsck_cached_read(void *context,uint64_t block,uint32_t count,void *buffer)
{
    openfs_fsck_io_cache_t *cache=(openfs_fsck_io_cache_t *)context;
    if(cache==NULL||cache->device==NULL||buffer==NULL||count==0U)return OPENFS_IO_INVALID_ARGUMENT;
    uint64_t chunk_blocks=(uint64_t)OPENFS_FSCK_IO_CHUNK/cache->device->block_size;
    if(chunk_blocks==0U||chunk_blocks>UINT32_MAX||count>(uint32_t)chunk_blocks)
        return cache->device->read(cache->device->context,block,count,buffer);
    uint64_t end=0U;
    if(!add(block,count,&end)||end>cache->device->block_count)return OPENFS_IO_OUT_OF_RANGE;
    uint64_t aligned=block-(block%chunk_blocks);
    uint64_t chunk_end=0U;
    if(!add(aligned,chunk_blocks,&chunk_end))return OPENFS_IO_OUT_OF_RANGE;
    if(chunk_end>cache->device->block_count)chunk_end=cache->device->block_count;
    uint32_t loaded=(uint32_t)(chunk_end-aligned);
    size_t selected=0U;
    int found=-1;
    for(size_t i=0U;i<OPENFS_FSCK_CACHE_SLOTS;i++){
        if(cache->slots[i].valid&&cache->slots[i].start_block==aligned&&cache->slots[i].block_count==loaded){
            found=(int)i;
            break;
        }
    }
    if(found<0){
        uint64_t oldest=UINT64_MAX;
        for(size_t i=0U;i<OPENFS_FSCK_CACHE_SLOTS;i++){
            if(!cache->slots[i].valid){selected=i;break;}
            if(cache->slots[i].last_use<oldest){oldest=cache->slots[i].last_use;selected=i;}
        }
        openfs_io_result_t r=cache->device->read(cache->device->context,aligned,loaded,cache->slots[selected].buffer);
        if(r!=OPENFS_IO_OK){cache->slots[selected].valid=0;return r;}
        cache->slots[selected].start_block=aligned;
        cache->slots[selected].block_count=loaded;
        cache->slots[selected].valid=1;
        found=(int)selected;
    }
    cache->slots[found].last_use=++cache->use_counter;
    memcpy(buffer,
           cache->slots[found].buffer+(size_t)(block-aligned)*(size_t)cache->device->block_size,
           (size_t)count*(size_t)cache->device->block_size);
    return OPENFS_IO_OK;
}
static openfs_io_result_t fsck_cached_write(void *context,uint64_t block,uint32_t count,const void *buffer)
{
    openfs_fsck_io_cache_t *cache=(openfs_fsck_io_cache_t *)context;
    if(cache==NULL||cache->device==NULL)return OPENFS_IO_INVALID_ARGUMENT;
    for(size_t i=0U;i<OPENFS_FSCK_CACHE_SLOTS;i++)cache->slots[i].valid=0;
    return cache->device->write(cache->device->context,block,count,buffer);
}
static openfs_io_result_t fsck_cached_flush(void *context)
{
    openfs_fsck_io_cache_t *cache=(openfs_fsck_io_cache_t *)context;
    if(cache==NULL||cache->device==NULL)return OPENFS_IO_INVALID_ARGUMENT;
    return cache->device->flush(cache->device->context);
}

static int ref_mark(uint16_t*refs,uint64_t count,uint64_t index){if(refs==NULL||index>=count)return 0;if(refs[(size_t)index]==UINT16_MAX)return 0;refs[(size_t)index]++;return 1;}
static uint16_t ref_count(const uint16_t*refs,uint64_t count,uint64_t index){if(refs==NULL||index>=count)return 0U;return refs[(size_t)index];}
static int ref_test(const uint16_t*refs,uint64_t count,uint64_t index){return ref_count(refs,count,index)!=0U;}
static int decode_dir_entry(const uint8_t*r,uint64_t*ino,uint64_t*gen,uint8_t*type){
if(memcmp(r,"ODIR1",5U)!=0)return 0;
uint32_t stored=(uint32_t)r[252U]|((uint32_t)r[253U]<<8U)|((uint32_t)r[254U]<<16U)|((uint32_t)r[255U]<<24U);
if(stored!=openfs_crc32c(r,252U))return -1;
uint32_t len=r[7U];if(len==0U||len>OPENFS_DIR_NAME_MAX)return -1;
for(uint32_t i=0U;i<len;i++){if(r[24U+i]=='/'||r[24U+i]=='\0')return -1;}
if((len==1U&&r[24U]=='.')||(len==2U&&r[24U]=='.'&&r[25U]=='.'))return -1;
*ino=0U;*gen=0U;for(unsigned i=0U;i<8U;i++){*ino|=(uint64_t)r[8U+i]<<(8U*i);*gen|=(uint64_t)r[16U+i]<<(8U*i);}*type=r[6U];for(uint32_t i=len+24U;i<252U;i++)if(r[i]!=0U)return -1;return 1;}
static int same_dir_name(const uint8_t*a,const uint8_t*b){uint32_t la=a[7U],lb=b[7U];return la==lb&&memcmp(a+24U,b+24U,la)==0;}
static openfs_journal_result_t validate_journal_data(void *ctx,uint64_t tx,const uint8_t *data,uint32_t len)
{
    (void)tx;
    const openfs_superblock_t *s=ctx;
    if(s==NULL||data==NULL||len<24U||memcmp(data,"OJBD1",5U)!=0)return OPENFS_JOURNAL_CORRUPT;
    uint64_t target=0U;for(unsigned k=0U;k<8U;k++)target|=(uint64_t)data[8U+k]<<(8U*k);
    uint32_t offset=(uint32_t)data[16U]|((uint32_t)data[17U]<<8U)|((uint32_t)data[18U]<<16U)|((uint32_t)data[19U]<<24U);
    uint32_t count=(uint32_t)data[20U]|((uint32_t)data[21U]<<8U)|((uint32_t)data[22U]<<16U)|((uint32_t)data[23U]<<24U);
    if(target>=s->total_blocks||target==0U||target==s->total_blocks-1U||count==0U||count!=len-24U||
       offset>=s->block_size||count>(uint32_t)((uint64_t)s->block_size-offset)||
       (target>=s->journal_start&&target-s->journal_start<s->journal_blocks))return OPENFS_JOURNAL_CORRUPT;
    return OPENFS_JOURNAL_OK;
}

static int validate_symlink_payload(openfs_block_device_t*d,const openfs_superblock_t*s,const openfs_inode_t*in)
{
    if(d==NULL||s==NULL||in==NULL||in->size==0U||in->size>=OPENFS_PATH_MAX)return 0;
    if((in->flags&OPENFS_INODE_FLAG_INLINE_DATA)!=0U){
        for(uint64_t n=0U;n<in->size;n++)if(in->inline_data[n]=='\0')return 0;
        return 1;
    }
    uint8_t *target=malloc((size_t)in->size);
    if(target==NULL)return -1;
    size_t got=0U;
    openfs_file_result_t fr=openfs_file_read(d,s,in,0U,target,(size_t)in->size,&got);
    if(fr!=OPENFS_FILE_OK||got!=in->size){free(target);return fr==OPENFS_FILE_IO_ERROR?-1:0;}
    for(uint64_t n=0U;n<in->size;n++)if(target[n]=='\0'){free(target);return 0;}
    free(target);
    return 1;
}

static uint8_t inode_dir_type(uint32_t mode){
switch(mode&OPENFS_INODE_TYPE_MASK){case OPENFS_INODE_MODE_REGULAR:return 1U;case OPENFS_INODE_MODE_DIRECTORY:return 2U;case OPENFS_INODE_MODE_SYMLINK:return 3U;default:return 0U;}}
#define FSCK_PROGRESS(done,total,stage) do { if(progress!=NULL) progress(progress_context,(done),(total),(stage)); } while(0)
static openfs_fsck_result_t fsck_core(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t*errors,openfs_fsck_diagnostic_t *diagnostic_out,openfs_fsck_progress_callback_t progress,void *progress_context){
openfs_fsck_diagnostic_t diagnostic={0};
FSCK_PROGRESS(0U,100U,"initialisation");
FSCK_PROGRESS(1U,100U,"lecture des superblocs");
if (errors != NULL) *errors = 0U;
if(!openfs_block_device_is_valid(d)||s==NULL||errors==NULL)return OPENFS_FSCK_INVALID_ARGUMENT;
if(openfs_validate_superblock(d,s)!=OPENFS_FORMAT_OK)return OPENFS_FSCK_CORRUPT;
openfs_superblock_t primary_copy={0},backup_copy={0};
openfs_format_result_t primary_result=fsck_read_superblock_at(d,0U,&primary_copy);
openfs_format_result_t backup_result=fsck_read_superblock_at(d,d->block_count-1U,&backup_copy);
uint64_t superblock_errors=0U;
if(primary_result==OPENFS_FORMAT_IO_ERROR||backup_result==OPENFS_FORMAT_IO_ERROR)return OPENFS_FSCK_IO_ERROR;
if(primary_result!=OPENFS_FORMAT_OK)superblock_errors++;
if(backup_result!=OPENFS_FORMAT_OK)superblock_errors++;
if(primary_result==OPENFS_FORMAT_OK&&backup_result==OPENFS_FORMAT_OK&&!fsck_same_superblock_layout(&primary_copy,&backup_copy))superblock_errors++;
openfs_journal_t journal;FSCK_PROGRESS(3U,100U,"ouverture du journal");openfs_journal_result_t jr=openfs_journal_open(&journal,d,s);
if(jr!=OPENFS_JOURNAL_OK)return jr==OPENFS_JOURNAL_IO_ERROR?OPENFS_FSCK_IO_ERROR:OPENFS_FSCK_CORRUPT;
FSCK_PROGRESS(5U,100U,"validation du journal");jr=openfs_journal_replay(d,s,validate_journal_data,(void *)s);
if(jr!=OPENFS_JOURNAL_OK)return jr==OPENFS_JOURNAL_IO_ERROR?OPENFS_FSCK_IO_ERROR:OPENFS_FSCK_CORRUPT;
FSCK_PROGRESS(8U,100U,"initialisation du cache I/O");openfs_fsck_io_cache_t io_cache={0};
for(size_t i=0U;i<OPENFS_FSCK_CACHE_SLOTS;i++){
    io_cache.slots[i].buffer=malloc(OPENFS_FSCK_IO_CHUNK);
    if(io_cache.slots[i].buffer==NULL){
        for(size_t j=0U;j<i;j++)free(io_cache.slots[j].buffer);
        return OPENFS_FSCK_IO_ERROR;
    }
}
io_cache.device=d;
openfs_block_device_t cached_device=*d;
cached_device.context=&io_cache;
cached_device.read=fsck_cached_read;
cached_device.write=fsck_cached_write;
cached_device.flush=fsck_cached_flush;
d=&cached_device;
fsck_bitmap_snapshot_t inode_bitmap_snapshot={0};
fsck_bitmap_snapshot_t block_bitmap_snapshot={0};
if(!fsck_bitmap_snapshot_load(d,s->inode_bitmap_start,s->inode_bitmap_blocks,&inode_bitmap_snapshot)||!fsck_bitmap_snapshot_load(d,s->block_bitmap_start,s->block_bitmap_blocks,&block_bitmap_snapshot)){free(inode_bitmap_snapshot.data);free(block_bitmap_snapshot.data);for(size_t i=0U;i<OPENFS_FSCK_CACHE_SLOTS;i++)free(io_cache.slots[i].buffer);return OPENFS_FSCK_IO_ERROR;}
uint64_t count=0U;if(icount(s,&count)!=OPENFS_FSCK_OK){free(inode_bitmap_snapshot.data);
free(block_bitmap_snapshot.data);
for(size_t i=0U;i<OPENFS_FSCK_CACHE_SLOTS;i++)free(io_cache.slots[i].buffer);return OPENFS_FSCK_CORRUPT;}
uint64_t ref_bytes64=0U;if(s->data_blocks>SIZE_MAX/sizeof(uint16_t)){free(inode_bitmap_snapshot.data);free(block_bitmap_snapshot.data);for(size_t i=0U;i<OPENFS_FSCK_CACHE_SLOTS;i++)free(io_cache.slots[i].buffer);return OPENFS_FSCK_CORRUPT;}ref_bytes64=s->data_blocks*(uint64_t)sizeof(uint16_t);if(ref_bytes64>OPENFS_FSCK_MAX_REF_BYTES){free(inode_bitmap_snapshot.data);free(block_bitmap_snapshot.data);for(size_t i=0U;i<OPENFS_FSCK_CACHE_SLOTS;i++)free(io_cache.slots[i].buffer);return OPENFS_FSCK_IO_ERROR;}
uint16_t*refs=(uint16_t*)calloc(1U,(size_t)ref_bytes64);if(refs==NULL&&ref_bytes64!=0U){free(inode_bitmap_snapshot.data);free(block_bitmap_snapshot.data);for(size_t i=0U;i<OPENFS_FSCK_CACHE_SLOTS;i++)free(io_cache.slots[i].buffer);return OPENFS_FSCK_IO_ERROR;}
if(count==UINT64_MAX||count+1U>SIZE_MAX/sizeof(uint64_t)||count+1U>SIZE_MAX){free(refs);free(inode_bitmap_snapshot.data);free(block_bitmap_snapshot.data);for(size_t i=0U;i<OPENFS_FSCK_CACHE_SLOTS;i++)free(io_cache.slots[i].buffer);return OPENFS_FSCK_CORRUPT;}
if(count+1U>OPENFS_FSCK_MAX_DIR_REFS){free(refs);free(inode_bitmap_snapshot.data);free(block_bitmap_snapshot.data);for(size_t i=0U;i<OPENFS_FSCK_CACHE_SLOTS;i++)free(io_cache.slots[i].buffer);return OPENFS_FSCK_IO_ERROR;}
uint64_t*dir_refs=calloc((size_t)(count+1U),sizeof(*dir_refs));if(dir_refs==NULL){free(refs);free(inode_bitmap_snapshot.data);free(block_bitmap_snapshot.data);for(size_t i=0U;i<OPENFS_FSCK_CACHE_SLOTS;i++)free(io_cache.slots[i].buffer);return OPENFS_FSCK_IO_ERROR;}
uint64_t bad=superblock_errors;if(superblock_errors!=0U){diagnostic.stage="superblock";diagnostic.reason="superblock incohérent ou invalide";diagnostic.index=0U;diagnostic.total=2U;diagnostic.count=superblock_errors;}
openfs_fsck_result_t result=OPENFS_FSCK_OK;
FSCK_PROGRESS(10U,100U,"préparation de la validation");
if((s->feature_flags&OPENFS_FEATURE_METADATA_ROOT)!=0U){
    openfs_metadata_root_t mr;openfs_metadata_root_result_t mrr=openfs_metadata_root_read(d,s,s->metadata_root_block,&mr);
    if(mrr!=OPENFS_METADATA_ROOT_OK||mr.generation!=s->metadata_root_generation){bad++;if(diagnostic.stage==NULL){diagnostic.stage="metadata root";diagnostic.reason="metadata root invalide ou incohérente";diagnostic.index=s->metadata_root_block;diagnostic.total=s->data_blocks;diagnostic.count=1U;}}
    int root_set=0;if(!fsck_bitmap_snapshot_test(&block_bitmap_snapshot,s->metadata_root_block,&root_set)){result=OPENFS_FSCK_IO_ERROR;goto done;}
    if(!root_set)bad++;else if(!ref_mark(refs,s->data_blocks,s->metadata_root_block-s->data_start)){result=OPENFS_FSCK_CORRUPT;goto done;}
}
openfs_inode_t root;
if(openfs_inode_read(d,s->inode_table_start,s->root_inode,count,&root)!=OPENFS_INODE_OK){
bad++;
}else if(root.inode_number!=s->root_inode||(root.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY||root.parent_inode!=s->root_inode||root.link_count==0U){
bad++;
}
int root_allocated=0;
if(!fsck_bitmap_snapshot_test(&inode_bitmap_snapshot,s->root_inode-1U,&root_allocated)){
result=OPENFS_FSCK_IO_ERROR;
goto done;
}
if(!root_allocated){bad++;if(diagnostic.stage==NULL){diagnostic.stage="inode bitmap";diagnostic.reason="inode racine marqué libre";diagnostic.index=s->root_inode;diagnostic.total=count;diagnostic.count=1U;}}
uint64_t inode_section_bad=bad;
for(uint64_t n=1U;n<=count;n++){if(progress!=NULL&&(n==1U||(n%4096U)==0U||n==count))FSCK_PROGRESS(10U+(count==0U?35U:(35U*n)/count),100U,"inode validation");int used=0;if(!fsck_bitmap_snapshot_test(&inode_bitmap_snapshot,n-1U,&used)){result=OPENFS_FSCK_IO_ERROR;goto done;}if(!used){continue;}openfs_inode_t in;openfs_inode_result_t ir=openfs_inode_read(d,s->inode_table_start,n,count,&in);if(ir!=OPENFS_INODE_OK){if(ir==OPENFS_INODE_IO_ERROR){result=OPENFS_FSCK_IO_ERROR;goto done;}bad++;if(diagnostic.stage==NULL){diagnostic.stage="inode validation";diagnostic.reason="inode invalide ou corrompu";diagnostic.index=n;diagnostic.total=count;diagnostic.count=1U;}continue;}
if(in.mode==OPENFS_INODE_MODE_FREE||in.link_count==0U||in.inode_number!=n||in.generation==0U||in.parent_inode==0U){bad++;if(diagnostic.stage==NULL){diagnostic.stage="inode validation";diagnostic.reason="métadonnées d'inode invalides";diagnostic.index=n;diagnostic.total=count;diagnostic.count=1U;}}if(in.mode==OPENFS_INODE_MODE_FREE&&in.blocks!=0U){bad++;if(diagnostic.stage==NULL){diagnostic.stage="inode validation";diagnostic.reason="inode libre avec des blocs attribués";diagnostic.index=n;diagnostic.total=count;diagnostic.count=1U;}}
uint64_t extent_total=0U;uint64_t previous_logical_end=0U;
uint32_t inline_n=(in.flags&OPENFS_INODE_FLAG_EXTENT_TREE)!=0U ? (in.extent_count<OPENFS_INODE_TREE_INLINE_EXTENT_MAX?in.extent_count:OPENFS_INODE_TREE_INLINE_EXTENT_MAX) : (in.extent_count<OPENFS_INODE_INLINE_EXTENT_MAX?in.extent_count:OPENFS_INODE_INLINE_EXTENT_MAX);
if((in.flags&OPENFS_INODE_FLAG_EXTENT_TREE)!=0U){
    uint64_t root_block=openfs_inode_get_extent_tree_root(&in);
    uint64_t data_end=0U;
    if(root_block==0U||!add(s->data_start,s->data_blocks,&data_end)||root_block<s->data_start||root_block>=data_end)bad++;
    else{
        int root_bitmap_allocated=0;
        if(!fsck_bitmap_snapshot_test(&block_bitmap_snapshot,root_block,&root_bitmap_allocated)){result=OPENFS_FSCK_IO_ERROR;goto done;}
        if(!root_bitmap_allocated)bad++;
        uint64_t rel=root_block-s->data_start;
        if(ref_test(refs,s->data_blocks,rel))bad++;
        else if(!ref_mark(refs,s->data_blocks,rel)){result=OPENFS_FSCK_CORRUPT;goto done;}
    }
    if(in.extent_count<OPENFS_INODE_TREE_INLINE_EXTENT_MAX+1U)bad++;
}else if(in.extent_count>OPENFS_INODE_INLINE_EXTENT_MAX)bad++;
for(uint32_t i=0U;i<in.extent_count;i++){
    openfs_extent_t e;openfs_extent_result_t er;
    if(i<inline_n)er=openfs_inode_get_extent(&in,i,&e);
    else er=openfs_extent_tree_read(d,s,&in,i-inline_n,&e);
    if(er!=OPENFS_EXTENT_OK||e.block_count==0U){if(er==OPENFS_EXTENT_IO_ERROR){result=OPENFS_FSCK_IO_ERROR;goto done;}bad++;continue;}
    uint64_t pe=0U,data_end=0U,logical_end=0U;
    if(!add(extent_total,e.block_count,&extent_total)||!add(s->data_start,s->data_blocks,&data_end)||
       !add(e.physical_start,e.block_count,&pe)||!add(e.logical_start,e.block_count,&logical_end)||
       e.physical_start<s->data_start||pe>data_end){bad++;}
    else{
        if(i>0U&&e.logical_start<previous_logical_end)bad++;
        for(uint64_t b=0U;b<e.block_count;b++){
            uint64_t physical=e.physical_start+b,rel=physical-s->data_start;int allocated=0;
            if(!fsck_bitmap_snapshot_test(&block_bitmap_snapshot,physical,&allocated)){result=OPENFS_FSCK_IO_ERROR;goto done;}
            if(!allocated)bad++;
            if(allocated&&(s->feature_flags&OPENFS_FEATURE_DATA_CHECKSUM)!=0U){uint8_t *cb=(uint8_t*)malloc(d->block_size);uint32_t expected=0U;if(cb==NULL){result=OPENFS_FSCK_IO_ERROR;goto done;}if(d->read(d->context,physical,1U,cb)!=OPENFS_IO_OK||openfs_data_checksum_get(d,s,physical,&expected)!=0){free(cb);result=OPENFS_FSCK_IO_ERROR;goto done;}if(expected!=openfs_data_checksum(cb,d->block_size))bad++;free(cb);}
            if(ref_test(refs,s->data_blocks,rel)){
        if((s->feature_flags&OPENFS_FEATURE_COW)!=0U){uint16_t rc=0U;if(openfs_cow_refcount_get(d,s,physical,&rc)!=OPENFS_COW_OK)bad++;}
        if((s->feature_flags&OPENFS_FEATURE_COW)!=0U&&!ref_mark(refs,s->data_blocks,rel)){result=OPENFS_FSCK_CORRUPT;goto done;}
    }else{
        if((s->feature_flags&OPENFS_FEATURE_COW)!=0U){uint16_t rc=0U;if(openfs_cow_refcount_get(d,s,physical,&rc)!=OPENFS_COW_OK||rc==0U)bad++;}
        if(!ref_mark(refs,s->data_blocks,rel)){result=OPENFS_FSCK_CORRUPT;goto done;}
    }
        }
    }
    previous_logical_end=logical_end;
}
if(extent_total!=in.blocks)bad++;
        uint64_t xattr_block=openfs_inode_get_xattr_block(&in);
        if(xattr_block!=0U){int xattr_allocated=0;uint64_t xattr_end=0U;if(!add(s->data_start,s->data_blocks,&xattr_end)||xattr_block<s->data_start||xattr_block>=xattr_end)bad++;else if(!fsck_bitmap_snapshot_test(&block_bitmap_snapshot,xattr_block,&xattr_allocated)){result=OPENFS_FSCK_IO_ERROR;goto done;}else{if(!xattr_allocated)bad++;uint64_t xrel=xattr_block-s->data_start;if(ref_test(refs,s->data_blocks,xrel)){
            if((s->feature_flags&OPENFS_FEATURE_COW)!=0U){uint16_t rc=0U;if(openfs_cow_refcount_get(d,s,xattr_block,&rc)!=OPENFS_COW_OK)bad++;}
            if((s->feature_flags&OPENFS_FEATURE_COW)!=0U&&!ref_mark(refs,s->data_blocks,xrel)){result=OPENFS_FSCK_CORRUPT;goto done;}
        }else{
            if((s->feature_flags&OPENFS_FEATURE_COW)!=0U){uint16_t rc=0U;if(openfs_cow_refcount_get(d,s,xattr_block,&rc)!=OPENFS_COW_OK||rc==0U)bad++;}
            if(!ref_mark(refs,s->data_blocks,xrel)){result=OPENFS_FSCK_CORRUPT;goto done;}
        }openfs_xattr_result_t xr=openfs_xattr_validate_inode(d,s,&in);if(xr==OPENFS_XATTR_IO_ERROR){result=OPENFS_FSCK_IO_ERROR;goto done;}if(xr!=OPENFS_XATTR_OK)bad++;}}
        if((in.flags&~(OPENFS_INODE_FLAG_INLINE_DATA|OPENFS_INODE_FLAG_HAS_EXTENTS|OPENFS_INODE_FLAG_EXTENT_TREE|OPENFS_INODE_FLAG_ORPHAN))!=0U)bad++;
        if((in.blocks==0U&&((in.flags&OPENFS_INODE_FLAG_HAS_EXTENTS)!=0U))||(in.blocks!=0U&&((in.flags&OPENFS_INODE_FLAG_HAS_EXTENTS)==0U)))bad++;
        if((in.flags&OPENFS_INODE_FLAG_INLINE_DATA)!=0U){
            if((in.flags&OPENFS_INODE_FLAG_HAS_EXTENTS)!=0U||(in.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_SYMLINK||in.blocks!=0U||in.extent_count!=0U||in.size>sizeof(in.inline_data))bad++;
        }else{
            uint64_t required=in.size==0U?0U:1U+(in.size-1U)/(uint64_t)d->block_size;
            if(in.blocks>required)bad++;
        }
        if((in.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_SYMLINK){
            int symlink_result=validate_symlink_payload(d,s,&in);
            if(symlink_result<0){result=OPENFS_FSCK_IO_ERROR;goto done;}
            if(symlink_result==0)bad++;
        }
}
if(bad>inode_section_bad&&diagnostic.stage==NULL){diagnostic.stage="inode validation";diagnostic.reason="une ou plusieurs incohérences d'inode";diagnostic.index=0U;diagnostic.total=count;diagnostic.count=bad-inode_section_bad;}
for(uint64_t n=1U;n<=count;n++){
if(progress!=NULL&&(n==1U||(n%4096U)==0U||n==count))FSCK_PROGRESS(45U+(count==0U?20U:(20U*n)/count),100U,"directory validation");
openfs_inode_t in;if(openfs_inode_read(d,s->inode_table_start,n,count,&in)!=OPENFS_INODE_OK)continue;
if((in.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)continue;
if(in.size%OPENFS_DIR_ENTRY_SIZE!=0U){bad++;continue;}
uint64_t entries=in.size/OPENFS_DIR_ENTRY_SIZE;uint8_t raw[OPENFS_DIR_ENTRY_SIZE];
for(uint64_t e=0U;e<entries;e++){
size_t got=0U;if(e>UINT64_MAX/OPENFS_DIR_ENTRY_SIZE){bad++;continue;}openfs_file_result_t fr=openfs_file_read(d,s,&in,e*OPENFS_DIR_ENTRY_SIZE,raw,sizeof(raw),&got);if(fr!=OPENFS_FILE_OK||got!=sizeof(raw)){if(fr==OPENFS_FILE_IO_ERROR){result=OPENFS_FSCK_IO_ERROR;goto done;}bad++;continue;}
uint64_t target_ino=0U,generation=0U;uint8_t type=0U;int decoded=decode_dir_entry(raw,&target_ino,&generation,&type);
if(decoded==0){int empty=1;for(size_t z=0U;z<sizeof(raw);z++){if(raw[z]!=0U){empty=0;break;}}if(!empty)bad++;continue;}
if(decoded<0){bad++;continue;}
for(uint64_t prior=0U;prior<e;prior++){uint8_t prev[OPENFS_DIR_ENTRY_SIZE];size_t prev_got=0U;openfs_file_result_t pfr=openfs_file_read(d,s,&in,prior*OPENFS_DIR_ENTRY_SIZE,prev,sizeof(prev),&prev_got);if(pfr!=OPENFS_FILE_OK||prev_got!=sizeof(prev)){if(pfr==OPENFS_FILE_IO_ERROR){result=OPENFS_FSCK_IO_ERROR;goto done;}bad++;break;}uint64_t pino=0U,pgen=0U;uint8_t ptype=0U;int pd=decode_dir_entry(prev,&pino,&pgen,&ptype);if(pd==1&&same_dir_name(raw,prev)){bad++;break;}}
if(target_ino==0U||target_ino>count||target_ino==s->root_inode||generation==0U){bad++;continue;}
openfs_inode_t target;if(openfs_inode_read(d,s->inode_table_start,target_ino,count,&target)!=OPENFS_INODE_OK){bad++;continue;}
if(target.generation!=generation||type!=inode_dir_type(target.mode)){bad++;continue;}
if((target.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_DIRECTORY&&target.parent_inode!=n)bad++;
if(dir_refs[target_ino]==UINT64_MAX)bad++;else dir_refs[target_ino]++;
}}
if(bad>inode_section_bad&&diagnostic.stage==NULL){diagnostic.stage="directory validation";diagnostic.reason="une ou plusieurs incohérences de répertoire";diagnostic.index=0U;diagnostic.total=count;diagnostic.count=bad-inode_section_bad;}
uint8_t*reachable=calloc((size_t)(count+1U),1U);
uint64_t*queue=calloc((size_t)(count+1U),sizeof(*queue));
if(reachable==NULL||queue==NULL){free(queue);free(reachable);result=OPENFS_FSCK_IO_ERROR;goto done;}
uint64_t head=0U,tail=0U;
FSCK_PROGRESS(65U,100U,"validation de la cohérence et de la portée");
if(s->root_inode==0U||s->root_inode>count){bad++;}else{
    reachable[s->root_inode]=1U;queue[tail++]=s->root_inode;
}
while(head<tail){
    uint64_t dir_ino=queue[head++];
    if(progress!=NULL&&(head==1U||(head%256U)==0U||head==tail))FSCK_PROGRESS(65U+(count==0U?20U:(20U*head)/count),100U,"reachability validation");
    openfs_inode_t dir_inode;
    if(openfs_inode_read(d,s->inode_table_start,dir_ino,count,&dir_inode)!=OPENFS_INODE_OK){
        bad++;continue;
    }
    if((dir_inode.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY){
        bad++;continue;
    }
    if(dir_inode.size%OPENFS_DIR_ENTRY_SIZE!=0U){
        bad++;continue;
    }
    uint64_t entries=dir_inode.size/OPENFS_DIR_ENTRY_SIZE;
    uint8_t raw[OPENFS_DIR_ENTRY_SIZE];
    for(uint64_t e=0U;e<entries;e++){
        size_t got=0U;
        if(e>UINT64_MAX/OPENFS_DIR_ENTRY_SIZE){
            bad++;continue;
        }
        openfs_file_result_t fr=openfs_file_read(d,s,&dir_inode,e*OPENFS_DIR_ENTRY_SIZE,raw,sizeof(raw),&got);
        if(fr!=OPENFS_FILE_OK||got!=sizeof(raw)){
            if(fr==OPENFS_FILE_IO_ERROR){free(queue);free(reachable);result=OPENFS_FSCK_IO_ERROR;goto done;}
            bad++;continue;
        }
        uint64_t target_ino=0U,generation=0U;uint8_t type=0U;
        int decoded=decode_dir_entry(raw,&target_ino,&generation,&type);
        if(decoded!=1)continue;
        if(target_ino==0U||target_ino>count){bad++;continue;}
        openfs_inode_t target;
        if(openfs_inode_read(d,s->inode_table_start,target_ino,count,&target)!=OPENFS_INODE_OK){
            bad++;continue;
        }
        if(target.generation!=generation||type!=inode_dir_type(target.mode)){
            bad++;continue;
        }
        if((target.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_DIRECTORY){
            if(target.parent_inode!=dir_ino){
                bad++;
            }
            if(reachable[target_ino]){
                bad++;
                continue;
            }
        }
        if(!reachable[target_ino]){
            reachable[target_ino]=1U;
            if((target.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_DIRECTORY){
                if(tail>=count+1U){bad++;continue;}
                queue[tail++]=target_ino;
            }
        }
    }
}
for(uint64_t n=1U;n<=count;n++){
    int used=0;
    if(!fsck_bitmap_snapshot_test(&inode_bitmap_snapshot,n-1U,&used)){
        free(queue);free(reachable);result=OPENFS_FSCK_IO_ERROR;goto done;
    }
    if(used&&!reachable[n]){
        openfs_inode_t orphan_inode;
        if(openfs_inode_read(d,s->inode_table_start,n,count,&orphan_inode)==OPENFS_INODE_OK&&
           (orphan_inode.flags&OPENFS_INODE_FLAG_ORPHAN)!=0U&&orphan_inode.link_count==0U){}
        else bad++;
    }
    if(!used&&reachable[n])bad++;
}
free(queue);free(reachable);
for(uint64_t n=1U;n<=count;n++){
int used=0;if(!fsck_bitmap_snapshot_test(&inode_bitmap_snapshot,n-1U,&used)){result=OPENFS_FSCK_IO_ERROR;goto done;}
if(!used){if(dir_refs[n]!=0U)bad++;continue;}
openfs_inode_t in;if(openfs_inode_read(d,s->inode_table_start,n,count,&in)!=OPENFS_INODE_OK)continue;
if(n==s->root_inode){if(dir_refs[n]!=0U||in.link_count!=1U)bad++;}
else if((in.flags&OPENFS_INODE_FLAG_ORPHAN)!=0U){if(dir_refs[n]!=0U||in.link_count!=0U)bad++;}
else if(dir_refs[n]!=in.link_count||dir_refs[n]==0U)bad++;
}
FSCK_PROGRESS(85U,100U,"validation des bitmaps et des blocs");
for(uint64_t b=0U;b<s->data_start;b++){if(progress!=NULL&&(b==0U||(b%4096U)==0U||b+1U==s->data_start))FSCK_PROGRESS(85U+(s->data_start==0U?0U:(5U*b)/s->data_start),100U,"bitmap validation");int set=0;if(!fsck_bitmap_snapshot_test(&block_bitmap_snapshot,b,&set)){result=OPENFS_FSCK_IO_ERROR;goto done;}if(!set)bad++;}
if(s->data_start>UINT64_MAX-s->data_blocks){result=OPENFS_FSCK_CORRUPT;goto done;}uint64_t data_end=s->data_start+s->data_blocks;for(uint64_t b=s->data_start;b<data_end;b++){if(progress!=NULL&&(b==s->data_start||(b%4096U)==0U||b+1U==data_end))FSCK_PROGRESS(90U+(s->data_blocks==0U?0U:(5U*(b-s->data_start))/s->data_blocks),100U,"bitmap validation");int set=0;if(!fsck_bitmap_snapshot_test(&block_bitmap_snapshot,b,&set)){result=OPENFS_FSCK_IO_ERROR;goto done;}if(set&&!ref_test(refs,s->data_blocks,b-s->data_start))bad++;}
for(uint64_t b=data_end;b<d->block_count;b++){int set=0;if(!fsck_bitmap_snapshot_test(&block_bitmap_snapshot,b,&set)){result=OPENFS_FSCK_IO_ERROR;goto done;}if(b==d->block_count-1U){if(!set)bad++;}else if(set)bad++;}
if((s->feature_flags&OPENFS_FEATURE_COW)!=0U){
    for(uint64_t b=s->data_start;b<data_end;b++){
        int allocated=0;
        if(!fsck_bitmap_snapshot_test(&block_bitmap_snapshot,b,&allocated)){result=OPENFS_FSCK_IO_ERROR;goto done;}
        uint16_t refs_count=0U;
        openfs_cow_result_t cow_result=openfs_cow_refcount_get(d,s,b,&refs_count);if(cow_result!=OPENFS_COW_OK){result=cow_result==OPENFS_COW_IO_ERROR?OPENFS_FSCK_IO_ERROR:OPENFS_FSCK_CORRUPT;if(diagnostic.stage==NULL){diagnostic.stage="CoW refcount validation";diagnostic.reason="échec de lecture du compteur CoW";diagnostic.index=b;diagnostic.total=data_end;diagnostic.count=1U;}goto done;}
        uint16_t counted_refs=ref_count(refs,s->data_blocks,b-s->data_start);
        if(!allocated){
            if(refs_count!=0U||counted_refs!=0U){
                bad++;
                if(diagnostic.stage==NULL){diagnostic.stage="CoW refcount validation";diagnostic.reason="bloc libre avec des références CoW";diagnostic.index=b;diagnostic.total=data_end;diagnostic.count=1U;}
            }
        }else if(refs_count!=counted_refs){
            bad++;
            if(diagnostic.stage==NULL){diagnostic.stage="CoW refcount validation";diagnostic.reason="compteur CoW différent du nombre de références";diagnostic.index=b;diagnostic.total=data_end;diagnostic.count=1U;}
        }
    }
}
uint64_t block_bitmap_bits=0U;if(s->block_bitmap_blocks>UINT64_MAX/d->block_size||((block_bitmap_bits=s->block_bitmap_blocks*(uint64_t)d->block_size)>UINT64_MAX/8U)){result=OPENFS_FSCK_CORRUPT;goto done;}block_bitmap_bits*=8U;if(block_bitmap_bits>d->block_count){for(uint64_t bit=d->block_count;bit<block_bitmap_bits;bit++){int set=0;if(!fsck_bitmap_snapshot_test(&block_bitmap_snapshot,bit,&set)){result=OPENFS_FSCK_IO_ERROR;goto done;}if(set)bad++;}}
if(s->inode_bitmap_blocks>UINT64_MAX/d->block_size){result=OPENFS_FSCK_CORRUPT;goto done;}
uint64_t inode_bitmap_bytes=s->inode_bitmap_blocks*(uint64_t)d->block_size;if(inode_bitmap_bytes>UINT64_MAX/8U){result=OPENFS_FSCK_CORRUPT;goto done;}
uint64_t inode_cap=inode_bitmap_bytes*8U;if(inode_cap>count){
for(uint64_t bit=count;bit<inode_cap;bit++){int set=0;if(!fsck_bitmap_snapshot_test(&inode_bitmap_snapshot,bit,&set)){result=OPENFS_FSCK_IO_ERROR;goto done;}if(set)bad++;}}
FSCK_PROGRESS(99U,100U,"finalisation");
done:;
    free(dir_refs);
    free(refs);
    free(inode_bitmap_snapshot.data);
    free(block_bitmap_snapshot.data);
    for(size_t i=0U;i<OPENFS_FSCK_CACHE_SLOTS;i++)free(io_cache.slots[i].buffer);
    FSCK_PROGRESS(100U,100U,"terminé");
    *errors=bad;
    if(diagnostic_out!=NULL)*diagnostic_out=diagnostic;
    result=result!=OPENFS_FSCK_OK?result:(bad==0U?OPENFS_FSCK_OK:OPENFS_FSCK_CORRUPT);
    return result;
}


openfs_fsck_result_t openfs_fsck_with_progress_and_diagnostics(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t*errors,openfs_fsck_diagnostic_t*diagnostic_out,openfs_fsck_progress_callback_t progress,void*progress_context){
    if(s==NULL||s->runtime==NULL)return fsck_core(d,s,errors,diagnostic_out,progress,progress_context);
    if(!openfs_runtime_enter(s->runtime))return OPENFS_FSCK_IO_ERROR;
    openfs_fsck_result_t r=fsck_core(d,s,errors,diagnostic_out,progress,progress_context);
    openfs_runtime_leave(s->runtime);return r;
}
openfs_fsck_result_t openfs_fsck(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t*errors){
    return openfs_fsck_with_progress(d,s,errors,NULL,NULL);
}
openfs_fsck_result_t openfs_fsck_with_progress(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t*errors,openfs_fsck_progress_callback_t progress,void *progress_context){
    return openfs_fsck_with_progress_and_diagnostics(d,s,errors,NULL,progress,progress_context);
}
