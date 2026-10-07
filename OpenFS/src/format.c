#include "openfs/format.h"
#include <stdlib.h>
#include <string.h>
#include "openfs/crc32c.h"
#include "openfs/inode.h"
#include "openfs/time.h"

#define OPENFS_CHECKSUM_OFFSET 4088U

static uint16_t get16(const uint8_t*p){return (uint16_t)p[0]|((uint16_t)p[1]<<8U);}
static uint32_t get32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8U)|((uint32_t)p[2]<<16U)|((uint32_t)p[3]<<24U);}
static uint64_t get64(const uint8_t*p){uint64_t v=0U;for(unsigned i=0U;i<8U;i++)v|=(uint64_t)p[i]<<(8U*i);return v;}
static void put16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);}
static void put32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);p[2]=(uint8_t)(v>>16U);p[3]=(uint8_t)(v>>24U);}
static void put64(uint8_t*p,uint64_t v){for(unsigned i=0U;i<8U;i++)p[i]=(uint8_t)(v>>(8U*i));}
static int pow2(uint32_t v){return v!=0U&&(v&(v-1U))==0U;}
static int addov(uint64_t a,uint64_t b,uint64_t*out){if(b>UINT64_MAX-a)return 1;*out=a+b;return 0;}
static int mulov(uint64_t a,uint64_t b,uint64_t*out){if(a!=0U&&b>UINT64_MAX/a)return 1;*out=a*b;return 0;}
static int ceildiv(uint64_t a,uint64_t b,uint64_t*out){if(b==0U||a>UINT64_MAX-(b-1U))return 1;*out=(a+b-1U)/b;return 0;}
static openfs_format_result_t validation_corrupt(const char **reason,const char *text){if(reason!=NULL)*reason=text;return OPENFS_FORMAT_CORRUPT;}

static int calculate_layout(uint64_t total,uint32_t bs,uint64_t*bb,uint64_t*ib,uint64_t*it,uint64_t*jb){
    /*
     * Keep a sane fixed inode density instead of consuming all remaining
     * metadata space as an inode table. 256-byte inodes at 16 KiB/inode
     * is a conservative general-purpose filesystem density.
     */
    enum { OPENFS_BYTES_PER_INODE = 16384U };
    if(total<64U||bb==NULL||ib==NULL||it==NULL||jb==NULL)return 0;
    uint64_t bits=(uint64_t)bs*8U;
    if(ceildiv(total,bits,bb)||*bb==0U)return 0;

    uint64_t journal=total/16U;
    if(journal<8U)journal=8U;
    {
        const uint64_t max_journal_blocks=(UINT64_C(256)*1024U*1024U)/bs;
        if(max_journal_blocks>=8U&&journal>max_journal_blocks)journal=max_journal_blocks;
    }

    if((uint64_t)bs<OPENFS_INODE_SIZE)return 0;
    uint64_t total_bytes=0U;
    uint64_t desired_inodes=0U,desired_inode_bytes=0U;
    if(mulov(total,(uint64_t)bs,&total_bytes))return 0;
    if(ceildiv(total_bytes,OPENFS_BYTES_PER_INODE,&desired_inodes)||desired_inodes==0U)return 0;
    if(mulov(desired_inodes,(uint64_t)OPENFS_INODE_SIZE,&desired_inode_bytes))return 0;
    if(ceildiv(desired_inode_bytes,(uint64_t)bs,it)||*it<4U)*it=4U;

    uint64_t inode_table_bytes=0U,inode_count=0U;
    if(mulov(*it,(uint64_t)bs,&inode_table_bytes))return 0;
    inode_count=inode_table_bytes/OPENFS_INODE_SIZE;
    if(inode_count==0U||ceildiv(inode_count,bits,ib)||*ib==0U)return 0;

    *jb=journal;

    uint64_t metadata=total-3U;
    if(*bb>metadata||journal>metadata-*bb)return 0;
    if(*ib>metadata-*bb-journal)return 0;
    if(*it>metadata-*bb-journal-*ib)return 0;

    uint64_t data=metadata-1U-*bb-*ib-*it-journal;
    return data>=8U;
}

static void encode(const openfs_superblock_t*sb,uint8_t*b){
    memset(b,0,OPENFS_SUPERBLOCK_SIZE);memcpy(b,"OPENFS\0\0",8U);
    put16(b+8U,sb->version_major);put16(b+10U,sb->version_minor);put64(b+12U,sb->feature_flags);
    put32(b+20U,sb->block_size);put32(b+24U,OPENFS_SUPERBLOCK_SIZE);put64(b+28U,sb->total_blocks);
    put64(b+36U,sb->metadata_start);put64(b+44U,sb->metadata_blocks);
    put64(b+52U,sb->block_bitmap_start);put64(b+60U,sb->block_bitmap_blocks);
    put64(b+68U,sb->inode_bitmap_start);put64(b+76U,sb->inode_bitmap_blocks);
    put64(b+84U,sb->inode_table_start);put64(b+92U,sb->inode_table_blocks);
    put64(b+100U,sb->journal_start);put64(b+108U,sb->journal_blocks);
    put64(b+116U,sb->data_start);put64(b+124U,sb->data_blocks);
    put64(b+132U,sb->root_inode);put64(b+140U,sb->generation);memcpy(b+148U,sb->uuid,16U);
    put32(b+OPENFS_CHECKSUM_OFFSET,0U);put32(b+OPENFS_CHECKSUM_OFFSET,openfs_crc32c(b,OPENFS_CHECKSUM_OFFSET));
}

static openfs_format_result_t decode(const uint8_t*b,openfs_superblock_t*sb){
    if(memcmp(b,"OPENFS\0\0",8U)!=0||get32(b+24U)!=OPENFS_SUPERBLOCK_SIZE)return OPENFS_FORMAT_CORRUPT;
    uint32_t stored=get32(b+OPENFS_CHECKSUM_OFFSET);uint8_t copy[OPENFS_SUPERBLOCK_SIZE];memcpy(copy,b,sizeof(copy));put32(copy+OPENFS_CHECKSUM_OFFSET,0U);
    if(stored!=openfs_crc32c(copy,OPENFS_CHECKSUM_OFFSET))return OPENFS_FORMAT_CORRUPT;
    sb->version_major=get16(b+8U);sb->version_minor=get16(b+10U);sb->feature_flags=get64(b+12U);sb->block_size=get32(b+20U);sb->total_blocks=get64(b+28U);
    sb->metadata_start=get64(b+36U);sb->metadata_blocks=get64(b+44U);sb->block_bitmap_start=get64(b+52U);sb->block_bitmap_blocks=get64(b+60U);
    sb->inode_bitmap_start=get64(b+68U);sb->inode_bitmap_blocks=get64(b+76U);sb->inode_table_start=get64(b+84U);sb->inode_table_blocks=get64(b+92U);
    sb->journal_start=get64(b+100U);sb->journal_blocks=get64(b+108U);sb->data_start=get64(b+116U);sb->data_blocks=get64(b+124U);
    sb->root_inode=get64(b+132U);sb->generation=get64(b+140U);memcpy(sb->uuid,b+148U,16U);sb->runtime=NULL;return OPENFS_FORMAT_OK;
}

static openfs_format_result_t validate_superblock_ex(const openfs_block_device_t*d,const openfs_superblock_t*sb,const char **reason){
    if(!openfs_block_device_is_valid(d)||sb==NULL)return OPENFS_FORMAT_INVALID_ARGUMENT;
    if(sb->version_major!=OPENFS_FORMAT_VERSION_MAJOR||sb->version_minor>OPENFS_FORMAT_VERSION_MINOR)return validation_corrupt(reason,"invalid format version");
    if((sb->feature_flags&~OPENFS_FEATURE_EXTENT_TREE)!=0U)return validation_corrupt(reason,"unsupported feature flags");
    if((sb->feature_flags&OPENFS_FEATURE_EXTENT_TREE)!=0U&&sb->version_minor<3U)return validation_corrupt(reason,"extent-tree feature requires format minor version 3");
    if(sb->block_size<OPENFS_MIN_BLOCK_SIZE||sb->block_size>OPENFS_MAX_BLOCK_SIZE||!pow2(sb->block_size)||sb->block_size!=d->block_size)return OPENFS_FORMAT_UNSUPPORTED_DEVICE;
if(sb->block_size%OPENFS_INODE_SIZE!=0U)return validation_corrupt(reason,"block size is not a multiple of inode size");
    if(sb->total_blocks!=d->block_count||sb->total_blocks<64U||sb->root_inode==0U||sb->generation==0U)return validation_corrupt(reason,"invalid device geometry or root/generation fields");
    if(sb->metadata_start!=2U||sb->metadata_blocks!=sb->total_blocks-3U||sb->metadata_blocks==0U||sb->block_bitmap_start!=3U||sb->block_bitmap_blocks==0U||sb->inode_bitmap_blocks==0U||sb->inode_table_blocks==0U)return validation_corrupt(reason,"invalid metadata or bitmap/table geometry");
    uint64_t end=0U;
    if(addov(sb->block_bitmap_start,sb->block_bitmap_blocks,&end)||end!=sb->inode_bitmap_start)return validation_corrupt(reason,"block bitmap does not end at inode bitmap");
    if(addov(sb->inode_bitmap_start,sb->inode_bitmap_blocks,&end)||end!=sb->inode_table_start)return validation_corrupt(reason,"inode bitmap does not end at inode table");
    if(addov(sb->inode_table_start,sb->inode_table_blocks,&end)||end!=sb->journal_start)return validation_corrupt(reason,"inode table does not end at journal");
    if(addov(sb->journal_start,sb->journal_blocks,&end)||end!=sb->data_start)return validation_corrupt(reason,"journal does not end at data region");
    if(addov(sb->data_start,sb->data_blocks,&end)||end!=sb->total_blocks-1U)return validation_corrupt(reason,"data region does not end immediately before backup superblock");
    if(sb->journal_blocks<8U||sb->data_blocks<8U||sb->journal_start==0U||sb->data_start==0U)return validation_corrupt(reason,"journal or data region is too small or starts at block zero");
    uint64_t meta_end=0U; if(addov(sb->metadata_start,sb->metadata_blocks,&meta_end)||meta_end!=sb->total_blocks-1U)return validation_corrupt(reason,"metadata region does not end immediately before backup superblock");
    uint64_t inode_bytes=0U;if(mulov(sb->inode_table_blocks,sb->block_size,&inode_bytes)||inode_bytes<OPENFS_INODE_SIZE)return validation_corrupt(reason,"inode table byte size is invalid");
    uint64_t inode_count=inode_bytes/OPENFS_INODE_SIZE;if(inode_bytes%OPENFS_INODE_SIZE!=0U)return validation_corrupt(reason,"inode table is not an integral number of inodes");
    uint64_t inode_cap=0U;if(mulov(sb->inode_bitmap_blocks,(uint64_t)sb->block_size*8U,&inode_cap)||inode_count==0U||inode_count>inode_cap||sb->root_inode>inode_count)return validation_corrupt(reason,"inode bitmap cannot represent the inode table");
    uint64_t block_cap=0U;if(mulov(sb->block_bitmap_blocks,(uint64_t)sb->block_size*8U,&block_cap)||sb->total_blocks>block_cap)return validation_corrupt(reason,"block bitmap cannot represent the device");
    return OPENFS_FORMAT_OK;
}

openfs_format_result_t openfs_validate_superblock(const openfs_block_device_t*d,const openfs_superblock_t*sb){return validate_superblock_ex(d,sb,NULL);}
const char *openfs_validate_superblock_reason(const openfs_block_device_t*d,const openfs_superblock_t*sb){const char *reason=NULL;openfs_format_result_t r=validate_superblock_ex(d,sb,&reason);return r==OPENFS_FORMAT_CORRUPT?reason:NULL;}

openfs_validation_code_t openfs_validate_superblock_code(const openfs_block_device_t*d,const openfs_superblock_t*sb){
    const char *reason=NULL;
    openfs_format_result_t r=validate_superblock_ex(d,sb,&reason);
    if(r==OPENFS_FORMAT_OK)return OPENFS_VALIDATION_OK;
    if(r==OPENFS_FORMAT_INVALID_ARGUMENT)return OPENFS_VALIDATION_INVALID_ARGUMENT;
    if(reason==NULL)return OPENFS_VALIDATION_GEOMETRY;
    if(strcmp(reason,"invalid format version")==0)return OPENFS_VALIDATION_FORMAT_VERSION;
    if(strcmp(reason,"unsupported feature flags")==0)return OPENFS_VALIDATION_FEATURE_FLAGS;
    if(strcmp(reason,"extent-tree feature requires format minor version 3")==0)return OPENFS_VALIDATION_EXTENT_VERSION;
    if(strcmp(reason,"block size is not a multiple of inode size")==0)return OPENFS_VALIDATION_BLOCK_SIZE;
    if(strcmp(reason,"invalid device geometry or root/generation fields")==0)return OPENFS_VALIDATION_GEOMETRY;
    if(strcmp(reason,"invalid metadata or bitmap/table geometry")==0)return OPENFS_VALIDATION_METADATA_GEOMETRY;
    if(strcmp(reason,"block bitmap does not end at inode bitmap")==0)return OPENFS_VALIDATION_BLOCK_BITMAP_CHAIN;
    if(strcmp(reason,"inode bitmap does not end at inode table")==0)return OPENFS_VALIDATION_INODE_BITMAP_CHAIN;
    if(strcmp(reason,"inode table does not end at journal")==0)return OPENFS_VALIDATION_INODE_TABLE_CHAIN;
    if(strcmp(reason,"journal does not end at data region")==0)return OPENFS_VALIDATION_JOURNAL_CHAIN;
    if(strcmp(reason,"data region does not end immediately before backup superblock")==0)return OPENFS_VALIDATION_DATA_END;
    if(strcmp(reason,"journal or data region is too small or starts at block zero")==0)return OPENFS_VALIDATION_REGION_SIZE;
    if(strcmp(reason,"metadata region does not end immediately before backup superblock")==0)return OPENFS_VALIDATION_METADATA_END;
    if(strcmp(reason,"inode table byte size is invalid")==0)return OPENFS_VALIDATION_INODE_TABLE_SIZE;
    if(strcmp(reason,"inode table is not an integral number of inodes")==0)return OPENFS_VALIDATION_INODE_COUNT;
    if(strcmp(reason,"inode bitmap cannot represent the inode table")==0)return OPENFS_VALIDATION_INODE_BITMAP_CAPACITY;
    if(strcmp(reason,"block bitmap cannot represent the device")==0)return OPENFS_VALIDATION_BLOCK_BITMAP_CAPACITY;
    return OPENFS_VALIDATION_GEOMETRY;
}

openfs_format_result_t openfs_prepare_superblock(openfs_block_device_t*d,const uint8_t uuid[16],openfs_superblock_t*out){
    if(!openfs_block_device_is_valid(d)||uuid==NULL||out==NULL)return OPENFS_FORMAT_INVALID_ARGUMENT;
    if(d->block_size<OPENFS_SUPERBLOCK_SIZE||d->block_size>OPENFS_MAX_BLOCK_SIZE||!pow2(d->block_size)||d->block_count<64U)return OPENFS_FORMAT_UNSUPPORTED_DEVICE;
    uint64_t bb=0U,ib=0U,it=0U,jb=0U;if(!calculate_layout(d->block_count,d->block_size,&bb,&ib,&it,&jb))return OPENFS_FORMAT_TOO_SMALL;
    uint64_t metadata=d->block_count-3U;if(bb+ib>metadata||it>metadata-bb-ib||jb>metadata-bb-ib-it)return OPENFS_FORMAT_TOO_SMALL;
    uint64_t data=metadata-1U-bb-ib-it-jb-rb;if(data<8U)return OPENFS_FORMAT_TOO_SMALL;
    memset(out,0,sizeof(*out));out->version_major=OPENFS_FORMAT_VERSION_MAJOR;out->version_minor=OPENFS_FORMAT_VERSION_MINOR;out->feature_flags=OPENFS_FEATURE_EXTENT_TREE;out->block_size=d->block_size;out->total_blocks=d->block_count;
    out->metadata_start=2U;out->metadata_blocks=metadata;out->block_bitmap_start=3U;out->block_bitmap_blocks=bb;out->inode_bitmap_start=3U+bb;out->inode_bitmap_blocks=ib;out->inode_table_start=out->inode_bitmap_start+ib;out->inode_table_blocks=it;out->journal_start=out->inode_table_start+it;out->journal_blocks=jb;out->data_start=out->journal_start+jb;out->data_blocks=data;out->root_inode=1U;out->generation=1U;memcpy(out->uuid,uuid,16U);
    return validate_superblock_ex(d,out,NULL);
}

openfs_format_result_t openfs_read_superblock(openfs_block_device_t*d,openfs_superblock_t*out){
    if(!openfs_block_device_is_valid(d)||out==NULL)return OPENFS_FORMAT_INVALID_ARGUMENT;
    if(d->block_size<OPENFS_SUPERBLOCK_SIZE||d->block_size>OPENFS_MAX_BLOCK_SIZE)return OPENFS_FORMAT_UNSUPPORTED_DEVICE;
    uint8_t*b=malloc(d->block_size);if(b==NULL)return OPENFS_FORMAT_IO_ERROR;
    if(d->read(d->context,0U,1U,b)!=OPENFS_IO_OK){free(b);return OPENFS_FORMAT_IO_ERROR;}
    openfs_format_result_t r=decode(b,out);free(b);return r==OPENFS_FORMAT_OK?openfs_validate_superblock(d,out):r;
}

openfs_format_result_t openfs_format_ex(openfs_block_device_t*d,const uint8_t uuid[16],uint32_t flags){
    if(!openfs_block_device_is_valid(d)||uuid==NULL||(flags&~OPENFS_FORMAT_FLAG_FULL_ZERO)!=0U)return OPENFS_FORMAT_INVALID_ARGUMENT;
    if(d->block_size<OPENFS_SUPERBLOCK_SIZE||d->block_size>OPENFS_MAX_BLOCK_SIZE||!pow2(d->block_size)||d->block_count<64U)return OPENFS_FORMAT_UNSUPPORTED_DEVICE;
    openfs_superblock_t sb;openfs_format_result_t layout_result=openfs_prepare_superblock(d,uuid,&sb);if(layout_result!=OPENFS_FORMAT_OK)return layout_result;
    enum { OPENFS_FORMAT_BATCH_BLOCKS = 256 };
    size_t zero_bytes=(size_t)OPENFS_FORMAT_BATCH_BLOCKS*(size_t)d->block_size;
    uint8_t*zero=calloc(1U,zero_bytes);if(zero==NULL)return OPENFS_FORMAT_IO_ERROR;
    uint64_t zero_start=sb.journal_start;
    uint64_t zero_end=sb.refcount_start+sb.refcount_blocks;
    if((flags&OPENFS_FORMAT_FLAG_FULL_ZERO)!=0U){zero_start=2U;zero_end=d->block_count-1U;}
    for(uint64_t b=zero_start;b<zero_end;){
        uint64_t remaining=zero_end-b;
        uint32_t count=remaining>OPENFS_FORMAT_BATCH_BLOCKS?OPENFS_FORMAT_BATCH_BLOCKS:(uint32_t)remaining;
        if(d->write(d->context,b,count,zero)!=OPENFS_IO_OK){free(zero);return OPENFS_FORMAT_IO_ERROR;}
        b+=(uint64_t)count;
    }
    free(zero);

    uint8_t*bitmap=malloc(d->block_size);if(bitmap==NULL)return OPENFS_FORMAT_IO_ERROR;
    uint64_t bits_per_block=(uint64_t)d->block_size*8U;
    for(uint64_t n=0U;n<sb.block_bitmap_blocks;n++){
        memset(bitmap,0,d->block_size);
        uint64_t first_bit=n*bits_per_block;
        uint64_t end_bit=first_bit+bits_per_block;
        if(end_bit>sb.total_blocks)end_bit=sb.total_blocks;
        uint64_t used_end=sb.data_start<end_bit?sb.data_start:end_bit;
        if(used_end>first_bit){
            uint64_t used_bits=used_end-first_bit;
            uint64_t full_bytes=used_bits/8U;
            if(full_bytes>(uint64_t)d->block_size)full_bytes=d->block_size;
            memset(bitmap,0xff,(size_t)full_bytes);
            if(full_bytes<(uint64_t)d->block_size&&used_bits%8U!=0U)
                bitmap[full_bytes]=(uint8_t)((1U<<(used_bits%8U))-1U);
        }
        if(d->block_count-1U>=first_bit&&d->block_count-1U<end_bit)
            bitmap[(d->block_count-1U-first_bit)/8U]|=(uint8_t)(1U<<((d->block_count-1U-first_bit)%8U));
        if(d->write(d->context,sb.block_bitmap_start+n,1U,bitmap)!=OPENFS_IO_OK){free(bitmap);return OPENFS_FORMAT_IO_ERROR;}
    }
    free(bitmap);

    bitmap=calloc(1U,d->block_size);if(bitmap==NULL)return OPENFS_FORMAT_IO_ERROR;
    bitmap[0]=1U;
    if(d->write(d->context,sb.inode_bitmap_start,1U,bitmap)!=OPENFS_IO_OK){free(bitmap);return OPENFS_FORMAT_IO_ERROR;}
    free(bitmap);
    openfs_inode_t root;memset(&root,0,sizeof(root));root.inode_number=1U;root.generation=1U;root.parent_inode=1U;root.link_count=1U;root.mode=OPENFS_INODE_MODE_DIRECTORY|0755U;uint64_t now=openfs_time_now_ns();if(now!=UINT64_MAX){root.atime_ns=now;root.mtime_ns=now;root.ctime_ns=now;}
    uint64_t inode_count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
    if(openfs_inode_write(d,sb.inode_table_start,inode_count,&root)!=OPENFS_INODE_OK)return OPENFS_FORMAT_IO_ERROR;
    uint8_t*buf=calloc(1U,d->block_size);if(buf==NULL)return OPENFS_FORMAT_IO_ERROR;encode(&sb,buf);
    int ok=d->write(d->context,0U,1U,buf)==OPENFS_IO_OK&&d->write(d->context,d->block_count-1U,1U,buf)==OPENFS_IO_OK; if(ok)ok=d->flush(d->context)==OPENFS_IO_OK; free(buf);
    return ok?OPENFS_FORMAT_OK:OPENFS_FORMAT_IO_ERROR;
}

openfs_format_result_t openfs_format(openfs_block_device_t*d,const uint8_t uuid[16]){return openfs_format_ex(d,uuid,OPENFS_FORMAT_FLAG_NONE);}
