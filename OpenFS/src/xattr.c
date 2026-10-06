#include "openfs/xattr.h"
#include "openfs/allocator.h"
#include "openfs/bitmap.h"
#include "openfs/crc32c.h"
#include "openfs/runtime.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define X_MAGIC "OXAT1\0\0\0"
#define X_HEADER 32U
#define X_NAME_MAX 255U
#define X_VALUE_MAX 65535U

static uint16_t g16(const uint8_t*p){return (uint16_t)p[0]|((uint16_t)p[1]<<8U);}
static uint32_t g32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8U)|((uint32_t)p[2]<<16U)|((uint32_t)p[3]<<24U);}
static uint64_t g64(const uint8_t*p){uint64_t v=0U;for(unsigned n=0;n<8U;n++)v|=(uint64_t)p[n]<<(8U*n);return v;}
static void p16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);}
static void p32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);p[2]=(uint8_t)(v>>16U);p[3]=(uint8_t)(v>>24U);}
static void p64(uint8_t*p,uint64_t v){for(unsigned n=0;n<8U;n++)p[n]=(uint8_t)(v>>(8U*n));}

static openfs_xattr_result_t validate_name(const char*n){
    if(n==NULL||n[0]=='\0')return OPENFS_XATTR_INVALID_ARGUMENT;
    size_t len=strlen(n);if(len>X_NAME_MAX)return OPENFS_XATTR_NAME_TOO_LONG;
    for(size_t i=0;i<len;i++)if((unsigned char)n[i]<0x20U||n[i]=='/'||n[i]=='\0')return OPENFS_XATTR_INVALID_ARGUMENT;
    return OPENFS_XATTR_OK;
}
static openfs_xattr_result_t load_inode(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t ino,openfs_inode_t*out){
    if(!openfs_block_device_is_valid(d)||s==NULL||out==NULL||ino==0U)return OPENFS_XATTR_INVALID_ARGUMENT;
    uint64_t count=(s->inode_table_blocks*(uint64_t)s->block_size)/OPENFS_INODE_SIZE;
    if(count==0U)return OPENFS_XATTR_CORRUPT;
    return openfs_inode_read(d,s->inode_table_start,ino,count,out)==OPENFS_INODE_OK?OPENFS_XATTR_OK:OPENFS_XATTR_CORRUPT;
}
static int valid_xblock(const openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t b){
    return d!=NULL&&s!=NULL&&b>=s->data_start&&s->data_blocks>0U&&s->data_start<=UINT64_MAX-s->data_blocks&&b<s->data_start+s->data_blocks&&b<d->block_count;
}
static openfs_xattr_result_t decode_block(const uint8_t*b,uint32_t bs,uint64_t generation){
    if(b==NULL||bs<X_HEADER||memcmp(b,X_MAGIC,8U)!=0||g16(b+8U)!=1U||g64(b+12U)!=generation)return OPENFS_XATTR_CORRUPT;
    uint32_t used=g32(b+20U),count=g32(b+24U),stored=g32(b+28U);
    if(used<X_HEADER||used>bs){return OPENFS_XATTR_CORRUPT;}
    uint8_t *tmp=malloc(bs);if(tmp==NULL)return OPENFS_XATTR_IO_ERROR;
    memcpy(tmp,b,bs);p32(tmp+28U,0U);uint32_t crc=openfs_crc32c(tmp,bs);free(tmp);
    if(crc!=stored)return OPENFS_XATTR_CORRUPT;
    size_t pos=X_HEADER;for(uint32_t n=0;n<count;n++){if(pos+8U>used)return OPENFS_XATTR_CORRUPT;uint16_t nl=g16(b+pos);uint32_t vl=g32(b+pos+2U);pos+=8U;if(nl==0U||nl>X_NAME_MAX||vl>X_VALUE_MAX||pos+(size_t)nl+(size_t)vl>used)return OPENFS_XATTR_CORRUPT;pos+=(size_t)nl+(size_t)vl;}
    return pos==used?OPENFS_XATTR_OK:OPENFS_XATTR_CORRUPT;
}
static int find_record(const uint8_t*b,uint32_t bs,const char*name,size_t *record_start,size_t *record_end,uint32_t *value_len){
    uint32_t count=g32(b+24U),used=g32(b+20U);size_t pos=X_HEADER;size_t nl=strlen(name);
    for(uint32_t n=0;n<count;n++){size_t start=pos;uint16_t nn=g16(b+pos);uint32_t vl=g32(b+pos+2U);pos+=8U;if(nn==nl&&memcmp(b+pos,name,nl)==0){if(record_start)*record_start=start;if(record_end)*record_end=pos+(size_t)nn+(size_t)vl;if(value_len)*value_len=vl;return 1;}pos+=(size_t)nn+(size_t)vl;}
    (void)bs;return 0;
}
static openfs_xattr_result_t build_block(uint8_t*b,uint32_t bs,uint64_t generation,const char*replace_name,const void*replace_value,size_t replace_len,const char*remove_name,int remove_mode){
    uint8_t *src=calloc(1U,bs);if(src==NULL)return OPENFS_XATTR_IO_ERROR;
    memcpy(src,X_MAGIC,8U);p16(src+8U,1U);p64(src+12U,generation);p32(src+20U,X_HEADER);p32(src+24U,0U);p32(src+28U,0U);
    uint32_t old_count=g32(b+24U);size_t pos=X_HEADER,out=X_HEADER;uint32_t count=0U;int replaced=0;
    for(uint32_t n=0;n<old_count;n++){
        if(pos+8U>bs){free(src);return OPENFS_XATTR_CORRUPT;}
        uint16_t nl=g16(b+pos);uint32_t vl=g32(b+pos+2U);size_t start=pos;pos+=8U;
        if(nl==0U||pos+(size_t)nl+(size_t)vl>g32(b+20U)){free(src);return OPENFS_XATTR_CORRUPT;}
        int same=replace_name!=NULL&&strlen(replace_name)==nl&&memcmp(b+pos,replace_name,nl)==0;
        int remove=remove_name!=NULL&&strlen(remove_name)==nl&&memcmp(b+pos,remove_name,nl)==0;
        if(same){if(replace_len>UINT32_MAX){free(src);return OPENFS_XATTR_INVALID_ARGUMENT;}if(out+8U+nl+replace_len>bs){free(src);return OPENFS_XATTR_NO_SPACE;}p16(src+out,(uint16_t)nl);p32(src+out+2U,(uint32_t)replace_len);memcpy(src+out+8U,b+pos,nl);memcpy(src+out+8U+nl,replace_value,replace_len);out+=8U+nl+replace_len;count++;replaced=1;}
        else if(!remove){size_t rec=8U+(size_t)nl+(size_t)vl;if(out+rec>bs){free(src);return OPENFS_XATTR_NO_SPACE;}memcpy(src+out,b+start,rec);out+=rec;count++;}
    }
    if(replace_name!=NULL&&!replaced){size_t nl=strlen(replace_name);if(out+8U+nl+replace_len>bs){free(src);return OPENFS_XATTR_NO_SPACE;}p16(src+out,(uint16_t)nl);p32(src+out+2U,(uint32_t)replace_len);memcpy(src+out+8U,replace_name,nl);memcpy(src+out+8U+nl,replace_value,replace_len);out+=8U+nl+replace_len;count++;}
    if(remove_mode&&count==0U){p32(src+20U,X_HEADER);}
    p32(src+20U,(uint32_t)out);p32(src+24U,count);p32(src+28U,0U);p32(src+28U,openfs_crc32c(src,bs));memcpy(b,src,bs);free(src);return OPENFS_XATTR_OK;
}
static openfs_xattr_result_t lock_inode(const openfs_superblock_t*s){
    if(s!=NULL&&s->runtime!=NULL&&s->runtime->initialized&&openfs_mutex_lock(&s->runtime->inode_lock,OPENFS_LOCK_RANK_INODE)!=OPENFS_LOCK_OK)return OPENFS_XATTR_IO_ERROR;
    return OPENFS_XATTR_OK;
}
static void unlock_inode(const openfs_superblock_t*s){if(s!=NULL&&s->runtime!=NULL&&s->runtime->initialized)(void)openfs_mutex_unlock(&s->runtime->inode_lock);}

openfs_xattr_result_t openfs_xattr_get(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t ino,const char*n,void*out,size_t cap,size_t*got){
    if(got==NULL)return OPENFS_XATTR_INVALID_ARGUMENT;*got=0U;openfs_xattr_result_t nr=validate_name(n);if(nr!=OPENFS_XATTR_OK||out==NULL)return nr==OPENFS_XATTR_OK?OPENFS_XATTR_INVALID_ARGUMENT:nr;
    if(lock_inode(s)!=OPENFS_XATTR_OK)return OPENFS_XATTR_IO_ERROR;openfs_inode_t in;openfs_xattr_result_t r=load_inode(d,s,ino,&in);if(r!=OPENFS_XATTR_OK){unlock_inode(s);return r;}uint64_t xb=openfs_inode_get_xattr_block(&in);if(xb==0U){unlock_inode(s);return OPENFS_XATTR_NOT_FOUND;}if(!valid_xblock(d,s,xb)){unlock_inode(s);return OPENFS_XATTR_CORRUPT;}
    uint8_t*b=malloc(d->block_size);if(b==NULL){unlock_inode(s);return OPENFS_XATTR_IO_ERROR;}if(d->read(d->context,xb,1U,b)!=OPENFS_IO_OK){free(b);unlock_inode(s);return OPENFS_XATTR_IO_ERROR;}r=decode_block(b,d->block_size,in.generation);if(r!=OPENFS_XATTR_OK){free(b);unlock_inode(s);return r;}
    size_t rs=0,re=0;uint32_t vl=0;if(!find_record(b,d->block_size,n,&rs,&re,&vl)){free(b);unlock_inode(s);return OPENFS_XATTR_NOT_FOUND;}size_t name_len=strlen(n);if(vl>cap){free(b);unlock_inode(s);return OPENFS_XATTR_NO_SPACE;}memcpy(out,b+rs+8U+name_len,vl);*got=vl;free(b);unlock_inode(s);(void)re;return OPENFS_XATTR_OK;
}
openfs_xattr_result_t openfs_xattr_set(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t ino,const char*n,const void*v,size_t len,uint32_t flags){
    openfs_xattr_result_t nr=validate_name(n);if(nr!=OPENFS_XATTR_OK||v==NULL&&len!=0U||len>X_VALUE_MAX)return nr!=OPENFS_XATTR_OK?nr:OPENFS_XATTR_INVALID_ARGUMENT;
    if(lock_inode(s)!=OPENFS_XATTR_OK)return OPENFS_XATTR_IO_ERROR;openfs_inode_t in;openfs_xattr_result_t r=load_inode(d,s,ino,&in);if(r!=OPENFS_XATTR_OK){unlock_inode(s);return r;}uint64_t xb=openfs_inode_get_xattr_block(&in);uint8_t*b=malloc(d->block_size);if(b==NULL){unlock_inode(s);return OPENFS_XATTR_IO_ERROR;}
    int exists=0;if(xb!=0U){if(!valid_xblock(d,s,xb)){free(b);unlock_inode(s);return OPENFS_XATTR_CORRUPT;}if(d->read(d->context,xb,1U,b)!=OPENFS_IO_OK){free(b);unlock_inode(s);return OPENFS_XATTR_IO_ERROR;}r=decode_block(b,d->block_size,in.generation);if(r!=OPENFS_XATTR_OK){free(b);unlock_inode(s);return r;}exists=find_record(b,d->block_size,n,NULL,NULL,NULL);}
    if((flags&OPENFS_XATTR_CREATE)!=0U&&exists){free(b);unlock_inode(s);return OPENFS_XATTR_EXISTS;}if((flags&OPENFS_XATTR_REPLACE)!=0U&&!exists){free(b);unlock_inode(s);return OPENFS_XATTR_NOT_FOUND;}
    if(xb==0U){r=openfs_alloc_block(d,s,&xb);if(r!=OPENFS_XATTR_OK){free(b);unlock_inode(s);return r==OPENFS_ALLOC_OUT_OF_SPACE?OPENFS_XATTR_NO_SPACE:OPENFS_XATTR_IO_ERROR;}memset(b,0,d->block_size);memcpy(b,X_MAGIC,8U);p16(b+8U,1U);p64(b+12U,in.generation);p32(b+20U,X_HEADER);p32(b+24U,0U);p32(b+28U,0U);}
    r=build_block(b,d->block_size,in.generation,n,v,len,NULL,0);if(r!=OPENFS_XATTR_OK){if(openfs_inode_get_xattr_block(&in)==0U)(void)openfs_free_block(d,s,xb);free(b);unlock_inode(s);return r;}
    if(d->write(d->context,xb,1U,b)!=OPENFS_IO_OK||d->flush(d->context)!=OPENFS_IO_OK){if(openfs_inode_get_xattr_block(&in)==0U)(void)openfs_free_block(d,s,xb);free(b);unlock_inode(s);return OPENFS_XATTR_IO_ERROR;}
    if(openfs_inode_get_xattr_block(&in)==0U){openfs_inode_set_xattr_block(&in,xb);if(openfs_inode_write(d,s->inode_table_start,(s->inode_table_blocks*(uint64_t)s->block_size)/OPENFS_INODE_SIZE,&in)!=OPENFS_INODE_OK){(void)openfs_free_block(d,s,xb);free(b);unlock_inode(s);return OPENFS_XATTR_IO_ERROR;}if(d->flush(d->context)!=OPENFS_IO_OK){free(b);unlock_inode(s);return OPENFS_XATTR_IO_ERROR;}}
    free(b);unlock_inode(s);return OPENFS_XATTR_OK;
}
openfs_xattr_result_t openfs_xattr_remove(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t ino,const char*n){
    openfs_xattr_result_t nr=validate_name(n);if(nr!=OPENFS_XATTR_OK)return nr;if(lock_inode(s)!=OPENFS_XATTR_OK)return OPENFS_XATTR_IO_ERROR;openfs_inode_t in;openfs_xattr_result_t r=load_inode(d,s,ino,&in);if(r!=OPENFS_XATTR_OK){unlock_inode(s);return r;}uint64_t xb=openfs_inode_get_xattr_block(&in);if(xb==0U){unlock_inode(s);return OPENFS_XATTR_NOT_FOUND;}if(!valid_xblock(d,s,xb)){unlock_inode(s);return OPENFS_XATTR_CORRUPT;}uint8_t*b=malloc(d->block_size);if(b==NULL){unlock_inode(s);return OPENFS_XATTR_IO_ERROR;}if(d->read(d->context,xb,1U,b)!=OPENFS_IO_OK){free(b);unlock_inode(s);return OPENFS_XATTR_IO_ERROR;}r=decode_block(b,d->block_size,in.generation);if(r!=OPENFS_XATTR_OK){free(b);unlock_inode(s);return r;}if(!find_record(b,d->block_size,n,NULL,NULL,NULL)){free(b);unlock_inode(s);return OPENFS_XATTR_NOT_FOUND;}r=build_block(b,d->block_size,in.generation,NULL,NULL,0U,n,1);if(r!=OPENFS_XATTR_OK){free(b);unlock_inode(s);return r;}uint32_t count=g32(b+24U);if(count!=0U){if(d->write(d->context,xb,1U,b)!=OPENFS_IO_OK||d->flush(d->context)!=OPENFS_IO_OK){free(b);unlock_inode(s);return OPENFS_XATTR_IO_ERROR;}free(b);unlock_inode(s);return OPENFS_XATTR_OK;}
    openfs_inode_set_xattr_block(&in,0U);if(openfs_inode_write(d,s->inode_table_start,(s->inode_table_blocks*(uint64_t)s->block_size)/OPENFS_INODE_SIZE,&in)!=OPENFS_INODE_OK){free(b);unlock_inode(s);return OPENFS_XATTR_IO_ERROR;}if(d->flush(d->context)!=OPENFS_IO_OK){free(b);unlock_inode(s);return OPENFS_XATTR_IO_ERROR;}r=openfs_free_block(d,s,xb);free(b);unlock_inode(s);return r==OPENFS_ALLOC_OK?OPENFS_XATTR_OK:OPENFS_XATTR_CORRUPT;
}
openfs_xattr_result_t openfs_xattr_list(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t ino,char*out,size_t cap,size_t*used){
    if(used==NULL)return OPENFS_XATTR_INVALID_ARGUMENT;*used=0U;if(lock_inode(s)!=OPENFS_XATTR_OK)return OPENFS_XATTR_IO_ERROR;openfs_inode_t in;openfs_xattr_result_t r=load_inode(d,s,ino,&in);if(r!=OPENFS_XATTR_OK){unlock_inode(s);return r;}uint64_t xb=openfs_inode_get_xattr_block(&in);if(xb==0U){unlock_inode(s);return OPENFS_XATTR_OK;}if(!valid_xblock(d,s,xb)){unlock_inode(s);return OPENFS_XATTR_CORRUPT;}uint8_t*b=malloc(d->block_size);if(b==NULL){unlock_inode(s);return OPENFS_XATTR_IO_ERROR;}if(d->read(d->context,xb,1U,b)!=OPENFS_IO_OK){free(b);unlock_inode(s);return OPENFS_XATTR_IO_ERROR;}r=decode_block(b,d->block_size,in.generation);if(r!=OPENFS_XATTR_OK){free(b);unlock_inode(s);return r;}size_t pos=X_HEADER,outpos=0U;uint32_t count=g32(b+24U);for(uint32_t n=0;n<count;n++){uint16_t nl=g16(b+pos);uint32_t vl=g32(b+pos+2U);(void)vl;pos+=8U;if(outpos+(size_t)nl+1U>cap){free(b);unlock_inode(s);return OPENFS_XATTR_NO_SPACE;}memcpy(out+outpos,b+pos,nl);outpos+=nl;out[outpos++]='\0';pos+=(size_t)nl+(size_t)vl;}*used=outpos;free(b);unlock_inode(s);return OPENFS_XATTR_OK;
}
