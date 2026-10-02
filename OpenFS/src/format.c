#include "openfs/format.h"
#include <stdlib.h>
#include <string.h>
#include "openfs/bitmap.h"
#include "openfs/crc32c.h"
#include "openfs/inode.h"

#define OPENFS_CHECKSUM_OFFSET 4088U
#define OPENFS_BACKUP_OFFSET(total) ((total) - 1U)

static uint16_t get_u16(const uint8_t *p){return (uint16_t)p[0]|((uint16_t)p[1]<<8U);}
static uint32_t get_u32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8U)|((uint32_t)p[2]<<16U)|((uint32_t)p[3]<<24U);}
static uint64_t get_u64(const uint8_t *p){uint64_t v=0U;for(unsigned i=0U;i<8U;++i)v|=(uint64_t)p[i]<<(8U*i);return v;}
static void put_u16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);}
static void put_u32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);p[2]=(uint8_t)(v>>16U);p[3]=(uint8_t)(v>>24U);}
static void put_u64(uint8_t*p,uint64_t v){for(unsigned i=0U;i<8U;++i)p[i]=(uint8_t)(v>>(8U*i));}
static int power_of_two(uint32_t v){return v!=0U&&(v&(v-1U))==0U;}
static int add_overflow(uint64_t a,uint64_t b,uint64_t*out){if(b>UINT64_MAX-a)return 1;*out=a+b;return 0;}
static int mul_overflow(uint64_t a,uint64_t b,uint64_t*out){if(a!=0U&&b>UINT64_MAX/a)return 1;*out=a*b;return 0;}
static int div_ceil(uint64_t a,uint64_t b,uint64_t*out){if(b==0U||a>UINT64_MAX-(b-1U))return 1;*out=(a+(b-1U))/b;return 0;}

static int calculate_layout(uint64_t total,uint32_t block_size,uint64_t*block_bitmap_blocks,uint64_t*inode_bitmap_blocks,uint64_t*inode_table_blocks,uint64_t*journal_blocks){
    if(total<64U||block_size==0U||block_bitmap_blocks==NULL||inode_bitmap_blocks==NULL||inode_table_blocks==NULL||journal_blocks==NULL)return 0;
    const uint64_t metadata=total-3U;
    uint64_t bb=0U;
    uint64_t bits=(uint64_t)block_size*8U;
    if(div_ceil(total,bits,&bb)||bb==0U)return 0;
    uint64_t journal=total/16U;if(journal<8U)journal=8U;
    uint64_t data=total/4U;if(data<8U)data=8U;
    if(bb+1U+journal+data+4U>metadata)return 0;
    uint64_t it=metadata-bb-1U-journal-data;
    if(it<4U)return 0;
    for(unsigned i=0U;i<16U;++i){
        uint64_t inode_capacity=(uint64_t)block_size*8U;
        uint64_t ib=0U;
        uint64_t inode_count=(it*(uint64_t)block_size)/OPENFS_INODE_SIZE;
        if(div_ceil(inode_count,inode_capacity,&ib)||ib==0U)return 0;
        uint64_t fixed=bb+ib+journal+data;
        if(fixed>=metadata)return 0;
        uint64_t next_it=metadata-fixed;
        if(next_it==it){*block_bitmap_blocks=bb;*inode_bitmap_blocks=ib;*inode_table_blocks=it;*journal_blocks=journal;return 1;}
        it=next_it;
    }
    return 0;
}
static void encode(const openfs_superblock_t*sb,uint8_t*buf){
    memset(buf,0,OPENFS_SUPERBLOCK_SIZE); memcpy(buf,"OPENFS\0\0",8U);
    put_u16(buf+8U,sb->version_major);put_u16(buf+10U,sb->version_minor);put_u64(buf+12U,sb->feature_flags);
    put_u32(buf+20U,sb->block_size);put_u32(buf+24U,OPENFS_SUPERBLOCK_SIZE);put_u64(buf+28U,sb->total_blocks);
    put_u64(buf+36U,sb->metadata_start);put_u64(buf+44U,sb->metadata_blocks);
    put_u64(buf+52U,sb->block_bitmap_start);put_u64(buf+60U,sb->block_bitmap_blocks);
    put_u64(buf+68U,sb->inode_bitmap_start);put_u64(buf+76U,sb->inode_bitmap_blocks);
    put_u64(buf+84U,sb->inode_table_start);put_u64(buf+92U,sb->inode_table_blocks);
    put_u64(buf+100U,sb->journal_start);put_u64(buf+108U,sb->journal_blocks);
    put_u64(buf+116U,sb->data_start);put_u64(buf+124U,sb->data_blocks);
    put_u64(buf+132U,sb->root_inode);put_u64(buf+140U,sb->generation);memcpy(buf+148U,sb->uuid,16U);
    put_u32(buf+OPENFS_CHECKSUM_OFFSET,0U);put_u32(buf+OPENFS_CHECKSUM_OFFSET,openfs_crc32c(buf,OPENFS_CHECKSUM_OFFSET));
}
static openfs_format_result_t decode(const uint8_t*buf,openfs_superblock_t*sb){
    if(memcmp(buf,"OPENFS\0\0",8U)!=0||get_u32(buf+24U)!=OPENFS_SUPERBLOCK_SIZE)return OPENFS_FORMAT_CORRUPT;
    uint32_t stored=get_u32(buf+OPENFS_CHECKSUM_OFFSET);uint8_t copy[OPENFS_SUPERBLOCK_SIZE];memcpy(copy,buf,sizeof(copy));
    put_u32(copy+OPENFS_CHECKSUM_OFFSET,0U);if(stored!=openfs_crc32c(copy,OPENFS_CHECKSUM_OFFSET))return OPENFS_FORMAT_CORRUPT;
    sb->version_major=get_u16(buf+8U);sb->version_minor=get_u16(buf+10U);sb->feature_flags=get_u64(buf+12U);sb->block_size=get_u32(buf+20U);
    sb->total_blocks=get_u64(buf+28U);sb->metadata_start=get_u64(buf+36U);sb->metadata_blocks=get_u64(buf+44U);
    sb->block_bitmap_start=get_u64(buf+52U);sb->block_bitmap_blocks=get_u64(buf+60U);
    sb->inode_bitmap_start=get_u64(buf+68U);sb->inode_bitmap_blocks=get_u64(buf+76U);
    sb->inode_table_startopenfs_format_result_t openfs_validate_superblock(const openfs_block_device_t*d,const openfs_superblock_t*sb){
    if(!openfs_block_device_is_valid(d)||sb==NULL)return OPENFS_FORMAT_INVALID_ARGUMENT;
    if(sb->version_major!=OPENFS_FORMAT_VERSION_MAJOR||sb->version_minor>OPENFS_FORMAT_VERSION_MINOR)return OPENFS_FORMAT_CORRUPT;
    if(sb->block_size<OPENFS_MIN_BLOCK_SIZE||sb->block_size>OPENFS_MAX_BLOCK_SIZE||!power_of_two(sb->block_size)||sb->block_size!=d->block_size)return OPENFS_FORMAT_UNSUPPORTED_DEVICE;
    if(sb->total_blocks!=d->block_count||sb->total_blocks<64U||sb->root_inode==0U)return OPENFS_FORMAT_CORRUPT;
    if(sb->metadata_start!=2U||sb->metadata_blocks!=sb->total_blocks-3U||sb->block_bitmap_start!=3U||sb->block_bitmap_blocks==0U)return OPENFS_FORMAT_CORRUPT;
    if(sb->inode_bitmap_start!=sb->block_bitmap_start+sb->block_bitmap_blocks||sb->inode_bitmap_blocks==0U)return OPENFS_FORMAT_CORRUPT;
    if(sb->inode_table_start!=sb->inode_bitmap_start+sb->inode_bitmap_blocks||sb->inode_table_blocks==0U)return OPENFS_FORMAT_CORRUPT;
    if(sb->journal_start!=sb->inode_table_start+sb->inode_table_blocks||sb->journal_blocks<8U)return OPENFS_FORMAT_CORRUPT;
    if(sb->data_start!=sb->journal_start+sb->journal_blocks||sb->data_blocks<8U)return OPENFS_FORMAT_CORRUPT;
    const uint64_t ranges[][2]={{sb->metadata_start,sb->metadata_blocks},{sb->block_bitmap_start,sb->block_bitmap_blocks},{sb->inode_bitmap_start,sb->inode_bitmap_blocks},{sb->inode_table_start,sb->inode_table_blocks},{sb->journal_start,sb->journal_blocks},{sb->data_start,sb->data_blocks}};
    for(size_t i=0U;i<sizeof(ranges)/sizeof(ranges[0]);++i){uint64_t end=0U;if(add_overflow(ranges[i][0],ranges[i][1],&end)||ranges[i][0]<2U||end>sb->total_blocks-1U)return OPENFS_FORMAT_CORRUPT;}
    uint64_t metadata_end=0U,block_bitmap_end=0U,inode_bitmap_end=0U,table_end=0U,journal_end=0U,data_end=0U,inode_bytes=0U,inode_count=0U;
    if(add_overflow(sb->metadata_start,sb->metadata_blocks,&metadata_end)||add_overflow(sb->block_bitmap_start,sb->block_bitmap_blocks,&block_bitmap_end)||add_overflow(sb->inode_bitmap_start,sb->inode_bitmap_blocks,&inode_bitmap_end)||add_overflow(sb->inode_table_start,sb->inode_table_blocks,&table_end)||add_overflow(sb->journal_start,sb->journal_blocks,&journal_end)||add_overflow(sb->data_start,sb->data_blocks,&data_end)||mul_overflow(sb->inode_table_blocks,sb->block_size,&inode_bytes)||inode_bytes<OPENFS_INODE_SIZE)return OPENFS_FORMAT_CORRUPT;
    inode_count=inode_bytes/OPENFS_INODE_SIZE;
    uint64_t block_bitmap_capacity=0U; if(mul_overflow(sb->block_bitmap_blocks,(uint64_t)sb->block_size*8U,&block_bitmap_capacity))return OPENFS_FORMAT_CORRUPT;
    uint64_t inode_bitmap_capacity=0U; if(mul_overflow(sb->inode_bitmap_blocks,(uint64_t)sb->block_size*8U,&inode_bitmap_capacity))return OPENFS_FORMAT_CORRUPT;
    if(metadata_end!=sb->total_blocks-1U||block_bitmap_end!=sb->inode_bitmap_start||inode_bitmap_end!=sb->inode_table_start||table_end!=sb->journal_start||journal_end!=sb->data_start||data_end!=sb->total_blocks-1U||inode_count==0U||inode_count>inode_bitmap_capacity||sb->root_inode>inode_count||sb->data_blocks>block_bitmap_capacity)return OPENFS_FORMAT_CORRUPT;
    return OPENFS_FORMAT_OK;
}

openfs_format_result_t openfs_read_superblock(openfs_block_device_t*d,openfs_superblock_t*out){
    if(!openfs_block_device_is_valid(d)||out==NULL)return OPENFS_FORMAT_INVALID_ARGUMENT;
    if(d->block_size<OPENFS_SUPERBLOCK_SIZE)return OPENFS_FORMAT_UNSUPPORTED_DEVICE;
    uint8_t*b=(uint8_t*)malloc(d->block_size);if(b==NULL)return OPENFS_FORMAT_IO_ERROR;
    if(d->read(d->context,0U,1U,b)!=OPENFS_IO_OK){free(b);return OPENFS_FORMAT_IO_ERROR;}
    openfs_format_result_t r=decode(b,out);free(b);if(r!=OPENFS_FORMAT_OK)return r;return openfs_validate_superblock(d,out);
}
openfs_format_result_t openfs_format(openfs_block_device_t*d,const uint8_t uuid[16]){
    if(!openfs_block_device_is_valid(d)||uuid==NULL)return OPENFS_FORMAT_INVALID_ARGUMENT;
    if(d->block_size<OPENFS_SUPERBLOCK_SIZE||d->block_size>OPENFS_MAX_BLOCK_SIZE||!power_of_two(d->block_size)||d->block_count<64U)return OPENFS_FORMAT_UNSUPPORTED_DEVICE;
    uint64_t bb=0U,ib=0U,it=0U,jb=0U;
    if(!calculate_layout(d->block_count,d->block_size,&bb,&ib,&it,&jb))return OPENFS_FORMAT_TOO_SMALL;
    uint64_t data_blocks=0U;uint64_t metadata= d->block_count-3U;
    if(bb>metadata||ib>metadata-bb||it>metadata-bb-ib||jb>metadata-bb-ib-it-1U)return OPENFS_FORMAT_TOO_SMALL;
    data_blocks=metadata-1U-bb-ib-it-jb;if(data_blocks<8U)return OPENFS_FORMAT_TOO_SMALL;
    openfs_superblock_t sb;memset(&sb,0,sizeof(sb));sb.version_major=OPENFS_FORMAT_VERSION_MAJOR;sb.version_minor=OPENFS_FORMAT_VERSION_MINOR;
    sb.block_size=d->block_size;sb.total_blocks=d->block_count;sb.metadata_start=2U;sb.metadata_blocks=metadata;
    sb.block_bitmap_start=3U;sb.block_bitmap_blocks=bb;sb.inode_bitmap_start=3U+bb;sb.inode_bitmap_blocks=ib;
    sb.inode_table_start=sb.inode_bitmap_start+sb.inode_bitmap_blocks;sb.inode_table_blocks=it;
    sb.journal_start=sb.inode_table_start+it;sb.journal_blocks=jb;sb.data_start=sb.journal_start+jb;sb.data_blocks=data_blocks;
    sb.root_inode=1U;sb.generation=1U;memcpy(sb.uuid,uuid,16U);
    if(openfs_validate_superblock(d,&sb)!=OPENFS_FORMAT_OK)return OPENFS_FORMAT_CORRUPT;
    uint8_t*zero=(uint8_t*)calloc(1U,d->block_size);if(zero==NULL)return OPENFS_FORMAT_IO_ERROR;
    for(uint64_t block=2U;block<d->block_count-1U;++block){if(d->write(d->context,block,1U,zero)!=OPENFS_IO_OK){free(zero);return OPENFS_FORMAT_IO_ERROR;}}
    free(zero);
    if(openfs_bitmap_set(d,sb.block_bitmap_start,sb.block_bitmap_blocks,0U,1)!=OPENFS_BITMAP_OK)return OPENFS_FORMAT_IO_ERROR;
    for(uint64_t b=0U;b<sb.data_start;++b){if(openfs_bitmap_set(d,sb.block_bitmap_start,sb.block_bitmap_blocks,b,1)!=OPENFS_BITMAP_OK)return OPENFS_FORMAT_IO_ERROR;}
    if(openfs_bitmap_set(d,sb.block_bitmap_start,sb.block_bitmap_blocks,d->block_count-1U,1)!=OPENFS_BITMAP_OK)return OPENFS_FORMAT_IO_ERROR;
    if(openfs_bitmap_set(d,sb.inode_bitmap_start,sb.inode_bitmap_blocks,0U,1)!=OPENFS_BITMAP_OK)return OPENFS_FORMAT_IO_ERROR;
    openfs_inode_t root;memset(&root,0,sizeof(root));root.inode_number=1U;root.generation=1U;root.parent_inode=1U;root.link_count=1U;root.mode=OPENFS_INODE_MODE_DIRECTORY;
    uint64_t inode_count=(sb.inode_table_blocks*(uint64_t)sb.block_size)/OPENFS_INODE_SIZE;
    if(openfs_inode_write(d,sb.inode_table_start,inode_count,&root)!=OPENFS_INODE_OK)return OPENFS_FORMAT_IO_ERROR;
    uint8_t*buffer=(uint8_t*)calloc(1U,d->block_size);if(buffer==NULL)return OPENFS_FORMAT_IO_ERROR;encode(&sb,buffer);
    uint64_t backup=d->block_count-1U;int ok=d->write(d->context,0U,1U,buffer)==OPENFS_IO_OK&&d->write(d->context,backup,1U,buffer)==OPENFS_IO_OK&&d->flush(d->context)==OPENFS_IO_OK;
    free(buffer);return ok?OPENFS_FORMAT_OK:OPENFS_FORMAT_IO_ERROR;
}
