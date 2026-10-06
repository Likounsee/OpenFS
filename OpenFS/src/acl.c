#include "openfs/acl.h"
#include <stdlib.h>
#include <string.h>
#define ACL_MAGIC "OACL1\0\0\0"
#define ACL_HEADER 16U
#define ACL_RECORD 8U
#define ACL_MAX 512U
static uint16_t g16(const uint8_t*p){return (uint16_t)p[0]|((uint16_t)p[1]<<8U);}
static uint32_t g32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8U)|((uint32_t)p[2]<<16U)|((uint32_t)p[3]<<24U);}
static void p16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);}
static void p32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);p[2]=(uint8_t)(v>>16U);p[3]=(uint8_t)(v>>24U);}
static const char*name_for(int def){return def?"system.posix_acl_default":"system.posix_acl_access";}
static int tag_valid(uint16_t t){return t==OPENFS_ACL_USER_OBJ||t==OPENFS_ACL_USER||t==OPENFS_ACL_GROUP_OBJ||t==OPENFS_ACL_GROUP||t==OPENFS_ACL_MASK||t==OPENFS_ACL_OTHER;}
static openfs_acl_result_t validate(const openfs_acl_entry_t*e,uint32_t n){
    if(e==NULL||n==0U||n>ACL_MAX)return OPENFS_ACL_INVALID_ARGUMENT;
    uint32_t counts[6]={0};int named=0;
    for(uint32_t i=0;i<n;i++){if(!tag_valid(e[i].tag)||e[i].permissions>7U)return OPENFS_ACL_INVALID_ARGUMENT;unsigned k=0U;switch(e[i].tag){case OPENFS_ACL_USER_OBJ:k=0;break;case OPENFS_ACL_USER:k=1;named=1;break;case OPENFS_ACL_GROUP_OBJ:k=2;break;case OPENFS_ACL_GROUP:k=3;named=1;break;case OPENFS_ACL_MASK:k=4;break;default:k=5;break;}counts[k]++;if((e[i].tag==OPENFS_ACL_USER||e[i].tag==OPENFS_ACL_GROUP)&&e[i].id==UINT32_MAX)return OPENFS_ACL_INVALID_ARGUMENT;}
    }
    if(counts[0]!=1U||counts[2]!=1U||counts[5]!=1U)return OPENFS_ACL_INVALID_ARGUMENT;
    if(named&&counts[4]!=1U)return OPENFS_ACL_INVALID_ARGUMENT;
    if(!named&&counts[4]>1U)return OPENFS_ACL_INVALID_ARGUMENT;
    for(uint32_t i=0;i<n;i++)for(uint32_t j=0;j<i;j++)if(e[i].tag==e[j].tag&&e[i].id==e[j].id)return OPENFS_ACL_INVALID_ARGUMENT;
    return OPENFS_ACL_OK;
}
static openfs_acl_result_t load(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t ino,openfs_acl_entry_t*e,uint32_t*count,int def){
    if(e==NULL||count==NULL)return OPENFS_ACL_INVALID_ARGUMENT;*count=0U;size_t cap=(size_t)ACL_MAX*sizeof(*e);uint8_t*buf=malloc(cap);if(buf==NULL)return OPENFS_ACL_IO_ERROR;size_t got=0;openfs_xattr_result_t xr=openfs_xattr_get(d,s,ino,name_for(def),buf,cap,&got);if(xr==OPENFS_XATTR_NOT_FOUND){free(buf);return OPENFS_ACL_NOT_FOUND;}if(xr!=OPENFS_XATTR_OK){free(buf);return xr==OPENFS_XATTR_IO_ERROR?OPENFS_ACL_IO_ERROR:OPENFS_ACL_CORRUPT;}if(got<ACL_HEADER||memcmp(buf,ACL_MAGIC,8U)!=0||g16(buf+8U)!=1U){free(buf);return OPENFS_ACL_CORRUPT;}uint32_t n=g32(buf+12U);if(n==0U||n>ACL_MAX||ACL_HEADER+(size_t)n*ACL_RECORD!=got){free(buf);return OPENFS_ACL_CORRUPT;}for(uint32_t i=0;i<n;i++){size_t p=ACL_HEADER+(size_t)i*ACL_RECORD;e[i].tag=g16(buf+p);e[i].id=g32(buf+p+2U);e[i].permissions=g16(buf+p+6U);}free(buf);return validate(e,n)==OPENFS_ACL_OK?(*count=n,OPENFS_ACL_OK):OPENFS_ACL_CORRUPT;
}
static openfs_acl_result_t store(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t ino,const openfs_acl_entry_t*e,uint32_t n,int def){
    openfs_acl_result_t v=validate(e,n);if(v!=OPENFS_ACL_OK)return v;size_t len=ACL_HEADER+(size_t)n*ACL_RECORD;uint8_t*buf=calloc(1U,len);if(buf==NULL)return OPENFS_ACL_IO_ERROR;memcpy(buf,ACL_MAGIC,8U);p16(buf+8U,1U);p32(buf+12U,n);for(uint32_t i=0;i<n;i++){size_t p=ACL_HEADER+(size_t)i*ACL_RECORD;p16(buf+p,e[i].tag);p32(buf+p+2U,e[i].id);p16(buf+p+6U,e[i].permissions);}openfs_xattr_result_t xr=openfs_xattr_set(d,s,ino,name_for(def),buf,len,0U);free(buf);if(xr==OPENFS_XATTR_OK)return OPENFS_ACL_OK;if(xr==OPENFS_XATTR_NO_SPACE)return OPENFS_ACL_NO_SPACE;return xr==OPENFS_XATTR_IO_ERROR?OPENFS_ACL_IO_ERROR:OPENFS_ACL_CORRUPT;
}
openfs_acl_result_t openfs_acl_set(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t ino,const openfs_acl_entry_t*e,uint32_t n,int def){return store(d,s,ino,e,n,def);}
openfs_acl_result_t openfs_acl_get(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t ino,openfs_acl_entry_t*e,uint32_t*n,int def){return load(d,s,ino,e,n,def);}
openfs_acl_result_t openfs_acl_remove(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t ino,int def){openfs_xattr_result_t r=openfs_xattr_remove(d,s,ino,name_for(def));if(r==OPENFS_XATTR_OK)return OPENFS_ACL_OK;if(r==OPENFS_XATTR_NOT_FOUND)return OPENFS_ACL_NOT_FOUND;return r==OPENFS_XATTR_IO_ERROR?OPENFS_ACL_IO_ERROR:OPENFS_ACL_CORRUPT;}
static uint16_t mask_for(const openfs_acl_entry_t*e,uint32_t n){for(uint32_t i=0;i<n;i++)if(e[i].tag==OPENFS_ACL_MASK)return e[i].permissions;return 7U;}
openfs_acl_result_t openfs_acl_check_access(openfs_block_device_t*d,const openfs_superblock_t*s,const openfs_inode_t*in,uint32_t uid,uint32_t gid,uint8_t req){
    if(in==NULL||req>7U)return OPENFS_ACL_INVALID_ARGUMENT;if(uid==0U)return OPENFS_ACL_OK;openfs_acl_entry_t e[ACL_MAX];uint32_t n=0U;openfs_acl_result_t r=load(d,s,in->inode_number,e,&n,0);if(r==OPENFS_ACL_NOT_FOUND){openfs_inode_result_t ir=openfs_inode_check_access(in,uid,gid,req);return ir==OPENFS_INODE_OK?OPENFS_ACL_OK:OPENFS_ACL_ACCESS_DENIED;}if(r!=OPENFS_ACL_OK)return r;uint16_t mask=mask_for(e,n),perms=0U;int matched=0;
    if(uid==in->uid){for(uint32_t i=0;i<n;i++)if(e[i].tag==OPENFS_ACL_USER_OBJ){perms=e[i].permissions;matched=1;break;}}
    else{for(uint32_t i=0;i<n;i++)if(e[i].tag==OPENFS_ACL_USER&&e[i].id==uid){perms=e[i].permissions&mask;matched=1;break;}}
    if(!matched){uint16_t gp=0;int gm=0;for(uint32_t i=0;i<n;i++){if(e[i].tag==OPENFS_ACL_GROUP_OBJ){gp|=e[i].permissions;gm=1;}if(e[i].tag==OPENFS_ACL_GROUP&&e[i].id==gid){gp|=e[i].permissions;gm=1;}}if(gm){perms=gp&mask;matched=1;}}
    if(!matched)for(uint32_t i=0;i<n;i++)if(e[i].tag==OPENFS_ACL_OTHER){perms=e[i].permissions;matched=1;break;}
    return matched&&((perms&req)==req)?OPENFS_ACL_OK:OPENFS_ACL_ACCESS_DENIED;
}
static void inherit_adjust(openfs_acl_entry_t*e,uint32_t n,uint32_t mode){
    uint16_t owner=(uint16_t)((mode>>6U)&7U),group=(uint16_t)((mode>>3U)&7U),other=(uint16_t)(mode&7U);
    for(uint32_t i=0;i<n;i++){if(e[i].tag==OPENFS_ACL_USER_OBJ)e[i].permissions=owner;else if(e[i].tag==OPENFS_ACL_OTHER)e[i].permissions=other;else if(e[i].tag==OPENFS_ACL_MASK||e[i].tag==OPENFS_ACL_GROUP_OBJ||e[i].tag==OPENFS_ACL_GROUP)e[i].permissions=(uint16_t)(e[i].permissions&group);else if(e[i].tag==OPENFS_ACL_USER)e[i].permissions=e[i].permissions;}
}
openfs_acl_result_t openfs_acl_inherit(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t parent,uint64_t child,uint32_t mode,int child_is_dir){
    openfs_acl_entry_t e[ACL_MAX],original[ACL_MAX];uint32_t n=0;openfs_acl_result_t r=load(d,s,parent,e,&n,1);if(r==OPENFS_ACL_NOT_FOUND)return OPENFS_ACL_OK;if(r!=OPENFS_ACL_OK)return r;memcpy(original,e,(size_t)n*sizeof(*e));inherit_adjust(e,n,mode);r=store(d,s,child,e,n,0);if(r!=OPENFS_ACL_OK)return r;if(child_is_dir)return store(d,s,child,original,n,1);return OPENFS_ACL_OK;
}
