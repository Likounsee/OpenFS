#include "openfs/metadata_cow.h"
#include "openfs/allocator.h"
#include "openfs/cow.h"
#include "openfs/crc32c.h"
#include <stdlib.h>
#include <string.h>
static uint16_t g16(const uint8_t*p){return (uint16_t)p[0]|((uint16_t)p[1]<<8U);}
static uint32_t g32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8U)|((uint32_t)p[2]<<16U)|((uint32_t)p[3]<<24U);}
static uint64_t g64(const uint8_t*p){uint64_t v=0U;for(unsigned i=0U;i<8U;i++)v|=(uint64_t)p[i]<<(8U*i);return v;}
static void p16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);}
static void p32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);p[2]=(uint8_t)(v>>16U);p[3]=(uint8_t)(v>>24U);}
static void p64(uint8_t*p,uint64_t v){for(unsigned i=0U;i<8U;i++)p[i]=(uint8_t)(v>>(8U*i));}
static int type_ok(openfs_metadata_cow_type_t t){return t>=OPENFS_METADATA_COW_TYPE_METADATA_ROOT&&t<=OPENFS_METADATA_COW_TYPE_SNAPSHOT_CATALOG;}
static openfs_metadata_cow_result_t geometry(const openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t b){
 if(!openfs_block_device_is_valid(d)||s==NULL)return OPENFS_METADATA_COW_INVALID_ARGUMENT;
 if((s->feature_flags&OPENFS_FEATURE_COW)==0U)return OPENFS_METADATA_COW_UNSUPPORTED;
 if(s->block_size!=d->block_size||s->block_size<OPENFS_METADATA_COW_HEADER_SIZE||s->data_blocks==0U||s->data_start>UINT64_MAX-s->data_blocks||s->data_start+s->data_blocks>d->block_count)return OPENFS_METADATA_COW_CORRUPT;
 return b<s->data_start||b>=s->data_start+s->data_blocks?OPENFS_METADATA_COW_OUT_OF_SPACE:OPENFS_METADATA_COW_OK;
}
static void encode(uint8_t*b,openfs_metadata_cow_type_t t,uint64_t id,uint64_t gen,uint32_t flags,uint32_t payload_crc){
 memset(b,0,OPENFS_METADATA_COW_HEADER_SIZE);memcpy(b,OPENFS_METADATA_COW_MAGIC,5U);p16(b+8U,(uint16_t)OPENFS_METADATA_COW_VERSION);p16(b+10U,(uint16_t)t);p32(b+12U,flags);p64(b+16U,id);p64(b+24U,gen);p32(b+32U,payload_crc);p32(b+36U,0U);p32(b+36U,openfs_crc32c(b,36U));
}
static openfs_metadata_cow_result_t decode(const uint8_t*b,uint32_t bs,openfs_metadata_cow_header_t*out){
 if(b==NULL||out==NULL||bs<OPENFS_METADATA_COW_HEADER_SIZE)return OPENFS_METADATA_COW_INVALID_ARGUMENT;
 if(memcmp(b,OPENFS_METADATA_COW_MAGIC,5U)!=0||b[5U]!=0U||b[6U]!=0U||b[7U]!=0U)return OPENFS_METADATA_COW_CORRUPT;
 uint32_t stored=g32(b+36U);uint8_t h[36U];memcpy(h,b,36U);if(stored!=openfs_crc32c(h,36U))return OPENFS_METADATA_COW_CORRUPT;
 uint16_t ver=g16(b+8U),type=g16(b+10U);if(ver!=OPENFS_METADATA_COW_VERSION||!type_ok((openfs_metadata_cow_type_t)type))return OPENFS_METADATA_COW_CORRUPT;
 out->version=ver;out->type=type;out->flags=g32(b+12U);out->logical_id=g64(b+16U);out->generation=g64(b+24U);out->payload_crc32c=g32(b+32U);out->header_crc32c=stored;return OPENFS_METADATA_COW_OK;
}
openfs_metadata_cow_result_t openfs_metadata_cow_validate_block(const openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t block,openfs_metadata_cow_header_t*out){
 openfs_metadata_cow_result_t r=geometry(d,s,block);if(r!=OPENFS_METADATA_COW_OK)return r;uint8_t*b=malloc(d->block_size);if(b==NULL)return OPENFS_METADATA_COW_IO_ERROR;
 if(d->read(d->context,block,1U,b)!=OPENFS_IO_OK){free(b);return OPENFS_METADATA_COW_IO_ERROR;}r=decode(b,d->block_size,out);
 if(r==OPENFS_METADATA_COW_OK&&openfs_crc32c(b+OPENFS_METADATA_COW_HEADER_SIZE,d->block_size-OPENFS_METADATA_COW_HEADER_SIZE)!=out->payload_crc32c)r=OPENFS_METADATA_COW_CORRUPT;
 free(b);return r;
}
static openfs_metadata_cow_result_t write_verified_block(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t block,openfs_metadata_cow_type_t type,uint64_t id,uint64_t gen,uint32_t flags,const uint8_t*payload){
 openfs_metadata_cow_result_t vr=geometry(d,s,block);if(vr!=OPENFS_METADATA_COW_OK)return vr;if(!type_ok(type)||gen==0U)return OPENFS_METADATA_COW_INVALID_ARGUMENT;
 uint16_t refs=0U;openfs_cow_result_t cr=openfs_cow_refcount_get(d,s,block,&refs);if(cr!=OPENFS_COW_OK)return cr==OPENFS_COW_CORRUPT?OPENFS_METADATA_COW_CORRUPT:OPENFS_METADATA_COW_IO_ERROR;if(refs!=1U)return refs==0U?OPENFS_METADATA_COW_CORRUPT:OPENFS_METADATA_COW_OVERFLOW;
 uint8_t*next=calloc(1U,d->block_size);uint8_t*old=malloc(d->block_size);if(next==NULL||old==NULL){free(next);free(old);return OPENFS_METADATA_COW_IO_ERROR;}
 if(d->read(d->context,block,1U,old)!=OPENFS_IO_OK){free(next);free(old);return OPENFS_METADATA_COW_IO_ERROR;}
 if(payload!=NULL)memcpy(next+OPENFS_METADATA_COW_HEADER_SIZE,payload,d->block_size-OPENFS_METADATA_COW_HEADER_SIZE);
 encode(next,type,id,gen,flags,openfs_crc32c(next+OPENFS_METADATA_COW_HEADER_SIZE,d->block_size-OPENFS_METADATA_COW_HEADER_SIZE));
 int ok=d->write(d->context,block,1U,next)==OPENFS_IO_OK;if(ok)ok=d->flush(d->context)==OPENFS_IO_OK;
 if(!ok){int restored=d->write(d->context,block,1U,old)==OPENFS_IO_OK;if(restored)restored=d->flush(d->context)==OPENFS_IO_OK;free(next);free(old);return restored?OPENFS_METADATA_COW_IO_ERROR:OPENFS_METADATA_COW_CORRUPT;}
 free(next);free(old);return OPENFS_METADATA_COW_OK;
}
openfs_metadata_cow_result_t openfs_metadata_cow_initialize_block(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t block,openfs_metadata_cow_type_t type,uint64_t id,uint64_t gen,uint32_t flags,const uint8_t*payload){return write_verified_block(d,s,block,type,id,gen,flags,payload);}
openfs_metadata_cow_result_t openfs_metadata_cow_write_payload(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t block,openfs_metadata_cow_type_t type,uint64_t id,uint64_t gen,uint32_t flags,const uint8_t*payload){
 openfs_metadata_cow_header_t h;openfs_metadata_cow_result_t r=openfs_metadata_cow_validate_block(d,s,block,&h);if(r!=OPENFS_METADATA_COW_OK)return r;if(h.type!=(uint16_t)type||h.logical_id!=id)return OPENFS_METADATA_COW_CORRUPT;return write_verified_block(d,s,block,type,id,gen,flags,payload);
}
openfs_metadata_cow_result_t openfs_metadata_cow_alloc(openfs_block_device_t*d,const openfs_superblock_t*s,openfs_metadata_cow_type_t type,uint64_t id,uint64_t gen,uint32_t flags,uint64_t*out){
 if(out==NULL||!type_ok(type)||gen==0U)return OPENFS_METADATA_COW_INVALID_ARGUMENT;uint64_t b=0U;openfs_alloc_result_t ar=openfs_alloc_block(d,s,&b);
 if(ar!=OPENFS_ALLOC_OK)return ar==OPENFS_ALLOC_OUT_OF_SPACE?OPENFS_METADATA_COW_OUT_OF_SPACE:(ar==OPENFS_ALLOC_CORRUPT?OPENFS_METADATA_COW_CORRUPT:OPENFS_METADATA_COW_IO_ERROR);
 openfs_metadata_cow_result_t r=openfs_metadata_cow_initialize_block(d,s,b,type,id,gen,flags,NULL);if(r!=OPENFS_METADATA_COW_OK){if(openfs_free_block(d,s,b)!=OPENFS_ALLOC_OK)return OPENFS_METADATA_COW_CORRUPT;return r;}*out=b;return OPENFS_METADATA_COW_OK;
}
openfs_metadata_cow_result_t openfs_metadata_cow_clone(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t source,openfs_metadata_cow_type_t type,uint64_t id,uint64_t gen,uint64_t*out){
 if(out==NULL||!type_ok(type)||gen==0U)return OPENFS_METADATA_COW_INVALID_ARGUMENT;openfs_metadata_cow_header_t h;openfs_metadata_cow_result_t r=openfs_metadata_cow_validate_block(d,s,source,&h);if(r!=OPENFS_METADATA_COW_OK)return r;if(h.type!=(uint16_t)type)return OPENFS_METADATA_COW_CORRUPT;
 uint8_t*b=malloc(d->block_size);if(b==NULL)return OPENFS_METADATA_COW_IO_ERROR;if(d->read(d->context,source,1U,b)!=OPENFS_IO_OK){free(b);return OPENFS_METADATA_COW_IO_ERROR;}
 uint64_t nb=0U;openfs_alloc_result_t ar=openfs_alloc_block(d,s,&nb);if(ar!=OPENFS_ALLOC_OK){free(b);return ar==OPENFS_ALLOC_OUT_OF_SPACE?OPENFS_METADATA_COW_OUT_OF_SPACE:(ar==OPENFS_ALLOC_CORRUPT?OPENFS_METADATA_COW_CORRUPT:OPENFS_METADATA_COW_IO_ERROR);}
 encode(b,type,id,gen,h.flags,openfs_crc32c(b+OPENFS_METADATA_COW_HEADER_SIZE,d->block_size-OPENFS_METADATA_COW_HEADER_SIZE));
 int ok=d->write(d->context,nb,1U,b)==OPENFS_IO_OK;if(ok)ok=d->flush(d->context)==OPENFS_IO_OK;free(b);if(!ok){if(openfs_free_block(d,s,nb)!=OPENFS_ALLOC_OK)return OPENFS_METADATA_COW_CORRUPT;return OPENFS_METADATA_COW_IO_ERROR;}*out=nb;return OPENFS_METADATA_COW_OK;
}
static openfs_metadata_cow_result_t map_alloc_result(openfs_alloc_result_t ar){return ar==OPENFS_ALLOC_OK?OPENFS_METADATA_COW_OK:(ar==OPENFS_ALLOC_OUT_OF_SPACE?OPENFS_METADATA_COW_OUT_OF_SPACE:(ar==OPENFS_ALLOC_CORRUPT?OPENFS_METADATA_COW_CORRUPT:OPENFS_METADATA_COW_IO_ERROR));}
static openfs_metadata_cow_result_t opaque_clone(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t source,uint64_t*out){
 openfs_metadata_cow_result_t gr=geometry(d,s,source);if(gr!=OPENFS_METADATA_COW_OK)return gr;uint16_t refs=0U;openfs_cow_result_t cr=openfs_cow_refcount_get(d,s,source,&refs);if(cr!=OPENFS_COW_OK)return cr==OPENFS_COW_CORRUPT?OPENFS_METADATA_COW_CORRUPT:OPENFS_METADATA_COW_IO_ERROR;if(refs==0U)return OPENFS_METADATA_COW_CORRUPT;
 uint8_t*b=malloc(d->block_size);if(b==NULL)return OPENFS_METADATA_COW_IO_ERROR;if(d->read(d->context,source,1U,b)!=OPENFS_IO_OK){free(b);return OPENFS_METADATA_COW_IO_ERROR;}uint64_t target=0U;openfs_alloc_result_t ar=openfs_alloc_block(d,s,&target);if(ar!=OPENFS_ALLOC_OK){free(b);return map_alloc_result(ar);}int ok=d->write(d->context,target,1U,b)==OPENFS_IO_OK;if(ok)ok=d->flush(d->context)==OPENFS_IO_OK;free(b);if(!ok){if(openfs_free_block(d,s,target)!=OPENFS_ALLOC_OK)return OPENFS_METADATA_COW_CORRUPT;return OPENFS_METADATA_COW_IO_ERROR;}*out=target;return OPENFS_METADATA_COW_OK;
}
openfs_metadata_cow_result_t openfs_metadata_cow_clone_opaque(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t source,uint64_t*out){if(out==NULL)return OPENFS_METADATA_COW_INVALID_ARGUMENT;return opaque_clone(d,s,source,out);}
openfs_metadata_cow_result_t openfs_metadata_cow_copy_before_write_opaque(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t source,uint64_t*out){if(out==NULL)return OPENFS_METADATA_COW_INVALID_ARGUMENT;openfs_metadata_cow_result_t gr=geometry(d,s,source);if(gr!=OPENFS_METADATA_COW_OK)return gr;uint16_t refs=0U;openfs_cow_result_t cr=openfs_cow_refcount_get(d,s,source,&refs);if(cr!=OPENFS_COW_OK)return cr==OPENFS_COW_CORRUPT?OPENFS_METADATA_COW_CORRUPT:OPENFS_METADATA_COW_IO_ERROR;if(refs==0U)return OPENFS_METADATA_COW_CORRUPT;if(refs==1U){*out=source;return OPENFS_METADATA_COW_OK;}return opaque_clone(d,s,source,out);}
openfs_metadata_cow_result_t openfs_metadata_cow_acquire_opaque(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t block,uint16_t*out){openfs_metadata_cow_result_t gr=geometry(d,s,block);if(gr!=OPENFS_METADATA_COW_OK)return gr;openfs_cow_result_t cr=openfs_cow_refcount_inc(d,s,block,out);if(cr==OPENFS_COW_OVERFLOW)return OPENFS_METADATA_COW_OVERFLOW;if(cr==OPENFS_COW_CORRUPT)return OPENFS_METADATA_COW_CORRUPT;return cr==OPENFS_COW_OK?OPENFS_METADATA_COW_OK:OPENFS_METADATA_COW_IO_ERROR;}
openfs_metadata_cow_result_t openfs_metadata_cow_release_opaque(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t block,uint16_t*out){openfs_metadata_cow_result_t gr=geometry(d,s,block);if(gr!=OPENFS_METADATA_COW_OK)return gr;uint16_t refs=0U;openfs_cow_result_t cr=openfs_cow_refcount_get(d,s,block,&refs);if(cr!=OPENFS_COW_OK)return cr==OPENFS_COW_CORRUPT?OPENFS_METADATA_COW_CORRUPT:OPENFS_METADATA_COW_IO_ERROR;if(refs==0U)return OPENFS_METADATA_COW_CORRUPT;openfs_alloc_result_t ar=openfs_free_block(d,s,block);if(ar==OPENFS_ALLOC_CORRUPT)return OPENFS_METADATA_COW_CORRUPT;if(ar!=OPENFS_ALLOC_OK)return OPENFS_METADATA_COW_IO_ERROR;if(out!=NULL)*out=refs==1U?0U:(uint16_t)(refs-1U);return OPENFS_METADATA_COW_OK;}
openfs_metadata_cow_result_t openfs_metadata_cow_acquire(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t block,uint16_t*out_refs){
 openfs_metadata_cow_header_t h;openfs_metadata_cow_result_t r=openfs_metadata_cow_validate_block(d,s,block,&h);if(r!=OPENFS_METADATA_COW_OK)return r;openfs_cow_result_t cr=openfs_cow_refcount_inc(d,s,block,out_refs);
 if(cr==OPENFS_COW_OVERFLOW)return OPENFS_METADATA_COW_OVERFLOW;if(cr==OPENFS_COW_CORRUPT)return OPENFS_METADATA_COW_CORRUPT;return cr==OPENFS_COW_OK?OPENFS_METADATA_COW_OK:OPENFS_METADATA_COW_IO_ERROR;
}
openfs_metadata_cow_result_t openfs_metadata_cow_release(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t block,uint16_t*out_refs){
 openfs_metadata_cow_header_t h;openfs_metadata_cow_result_t r=openfs_metadata_cow_validate_block(d,s,block,&h);if(r!=OPENFS_METADATA_COW_OK)return r;uint16_t refs=0U;openfs_cow_result_t cr=openfs_cow_refcount_get(d,s,block,&refs);
 if(cr!=OPENFS_COW_OK)return cr==OPENFS_COW_CORRUPT?OPENFS_METADATA_COW_CORRUPT:OPENFS_METADATA_COW_IO_ERROR;if(refs==0U)return OPENFS_METADATA_COW_CORRUPT;
 openfs_alloc_result_t ar=openfs_free_block(d,s,block);if(ar==OPENFS_ALLOC_CORRUPT)return OPENFS_METADATA_COW_CORRUPT;if(ar!=OPENFS_ALLOC_OK)return OPENFS_METADATA_COW_IO_ERROR;
 if(out_refs!=NULL)*out_refs=refs==1U?0U:(uint16_t)(refs-1U);return OPENFS_METADATA_COW_OK;
}
openfs_metadata_cow_result_t openfs_metadata_cow_copy_before_write(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t source,openfs_metadata_cow_type_t type,uint64_t id,uint64_t gen,uint64_t*out){
 if(out==NULL||!type_ok(type)||gen==0U)return OPENFS_METADATA_COW_INVALID_ARGUMENT;openfs_metadata_cow_header_t h;openfs_metadata_cow_result_t r=openfs_metadata_cow_validate_block(d,s,source,&h);if(r!=OPENFS_METADATA_COW_OK)return r;
 if(h.type!=(uint16_t)type||h.logical_id!=id)return OPENFS_METADATA_COW_CORRUPT;uint16_t refs=0U;openfs_cow_result_t cr=openfs_cow_refcount_get(d,s,source,&refs);
 if(cr!=OPENFS_COW_OK)return cr==OPENFS_COW_CORRUPT?OPENFS_METADATA_COW_CORRUPT:OPENFS_METADATA_COW_IO_ERROR;if(refs==0U)return OPENFS_METADATA_COW_CORRUPT;if(refs==1U){*out=source;return OPENFS_METADATA_COW_OK;}
 return openfs_metadata_cow_clone(d,s,source,type,id,gen,out);
}
