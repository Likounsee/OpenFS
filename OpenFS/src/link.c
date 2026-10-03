#include "openfs/link.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "openfs/time.h"
static openfs_path_result_t icount(const openfs_superblock_t*s,uint64_t*n){if(s==NULL||s->block_size==0U||s->inode_table_blocks>UINT64_MAX/s->block_size)return OPENFS_PATH_CORRUPT;*n=(s->inode_table_blocks*s->block_size)/OPENFS_INODE_SIZE;return *n?OPENFS_PATH_OK:OPENFS_PATH_CORRUPT;}
static openfs_path_result_t ri(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t n,openfs_inode_t*i){uint64_t c=0U;if(icount(s,&c)!=OPENFS_PATH_OK)return OPENFS_PATH_CORRUPT;return openfs_inode_read(d,s->inode_table_start,n,c,i)==OPENFS_INODE_OK?OPENFS_PATH_OK:OPENFS_PATH_IO_ERROR;}
openfs_path_result_t openfs_link(openfs_block_device_t*d,const openfs_superblock_t*s,const char*oldp,const char*newp){uint64_t ino=0U;if(openfs_path_lookup(d,s,oldp,&ino)!=OPENFS_PATH_OK)return OPENFS_PATH_NOT_FOUND;openfs_inode_t in;if(ri(d,s,ino,&in)!=OPENFS_PATH_OK||(in.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_DIRECTORY)return OPENFS_PATH_INVALID_ARGUMENT;char pp[OPENFS_PATH_MAX],nn[OPENFS_DIR_NAME_MAX+1U];size_t l=strlen(newp),slash=l;while(slash>0U&&newp[slash-1U]!='/')--slash;if(l==0U||l>=OPENFS_PATH_MAX||slash==0U)return OPENFS_PATH_INVALID_ARGUMENT;memcpy(nn,newp+slash,l-slash);nn[l-slash]='\0';if(l-slash==0U||l-slash>OPENFS_DIR_NAME_MAX)return OPENFS_PATH_NAME_TOO_LONG;if(slash==1U){pp[0]='/';pp[1]='\0';}else{memcpy(pp,newp,slash-1U);pp[slash-1U]='\0';}uint64_t parent=0U;if(openfs_path_lookup(d,s,pp,&parent)!=OPENFS_PATH_OK)return OPENFS_PATH_NOT_FOUND;openfs_inode_t pd;if(ri(d,s,parent,&pd)!=OPENFS_PATH_OK||(pd.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)return OPENFS_PATH_NOT_DIRECTORY;openfs_dir_entry_t ex;if(openfs_dir_lookup(d,s,&pd,nn,&ex)==OPENFS_DIR_OK)return OPENFS_PATH_EXISTS;if(in.link_count==UINT64_MAX)return OPENFS_PATH_CORRUPT;openfs_dir_entry_t e={ino,in.generation,(uint8_t)((in.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_SYMLINK?3U:1U)};if(openfs_dir_add(d,s,&pd,nn,&e)!=OPENFS_DIR_OK)return OPENFS_PATH_IO_ERROR;in.link_count++;uint64_t now=openfs_time_now_ns();if(now!=UINT64_MAX)in.ctime_ns=now;if(openfs_inode_write(d,s->inode_table_start,(s->inode_table_blocks*s->block_size)/OPENFS_INODE_SIZE,&in)!=OPENFS_INODE_OK){(void)openfs_dir_remove(d,s,&pd,nn);return OPENFS_PATH_IO_ERROR;}return OPENFS_PATH_OK;}
openfs_path_result_t openfs_symlink(openfs_block_device_t*d,const openfs_superblock_t*s,const char*target,const char*linkp){uint64_t ino=0U;openfs_path_result_t r=openfs_path_create(d,s,linkp,OPENFS_INODE_MODE_SYMLINK,&ino);if(r!=OPENFS_PATH_OK)return r;openfs_inode_t in;if(ri(d,s,ino,&in)!=OPENFS_PATH_OK){(void)openfs_path_unlink(d,s,linkp);return OPENFS_PATH_IO_ERROR;}size_t len=strlen(target);if(len>UINT64_MAX-1U){return OPENFS_PATH_INVALID_ARGUMENT;}if(len<=sizeof(in.inline_data)){memset(in.inline_data,0,sizeof(in.inline_data));memcpy(in.inline_data,target,len);in.size=len;in.flags|=OPENFS_INODE_FLAG_INLINE_DATA;uint64_t count=0U;if(icount(s,&count)!=OPENFS_PATH_OK||openfs_inode_write(d,s->inode_table_start,count,&in)!=OPENFS_INODE_OK){(void)openfs_path_unlink(d,s,linkp);return OPENFS_PATH_IO_ERROR;}}else if(openfs_file_write(d,s,&in,0U,target,len)!=OPENFS_FILE_OK){(void)openfs_path_unlink(d,s,linkp);return OPENFS_PATH_IO_ERROR;}return OPENFS_PATH_OK;}
openfs_path_result_t openfs_readlink(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,char*out,uint64_t cap){if(out==NULL||cap==0U)return OPENFS_PATH_INVALID_ARGUMENT;uint64_t ino=0U;if(openfs_path_lookup(d,s,p,&ino)!=OPENFS_PATH_OK)return OPENFS_PATH_NOT_FOUND;openfs_inode_t in;if(ri(d,s,ino,&in)!=OPENFS_PATH_OK)return OPENFS_PATH_IO_ERROR;if((in.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_SYMLINK)return OPENFS_PATH_INVALID_ARGUMENT;if(in.size==UINT64_MAX||in.size+1U>cap)return OPENFS_PATH_NO_SPACE;if((in.flags&OPENFS_INODE_FLAG_INLINE_DATA)!=0U){memcpy(out,in.inline_data,(size_t)in.size);out[in.size]='\0';return OPENFS_PATH_OK;}size_t got=0U;if(openfs_file_read(d,s,&in,0U,out,(size_t)in.size,&got)!=OPENFS_FILE_OK||got!=in.size)return OPENFS_PATH_IO_ERROR;out[in.size]='\0';return OPENFS_PATH_OK;}
openfs_path_result_t openfs_resolve_symlink(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,char*out,uint64_t cap,uint32_t depth){
if(d==NULL||s==NULL||p==NULL||out==NULL||cap==0U)return OPENFS_PATH_INVALID_ARGUMENT;
if(depth>40U)return OPENFS_PATH_CORRUPT;
uint64_t ino=0U;if(openfs_path_lookup(d,s,p,&ino)!=OPENFS_PATH_OK)return OPENFS_PATH_NOT_FOUND;
openfs_inode_t in;if(ri(d,s,ino,&in)!=OPENFS_PATH_OK)return OPENFS_PATH_IO_ERROR;
if((in.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_SYMLINK){size_t n=strlen(p);if((uint64_t)n+1U>cap)return OPENFS_PATH_NO_SPACE;memcpy(out,p,n+1U);return OPENFS_PATH_OK;}
uint64_t target_cap64=in.size==UINT64_MAX?0U:in.size+1U;if(target_cap64==0U||target_cap64>(uint64_t)SIZE_MAX)return OPENFS_PATH_NO_SPACE;char *target=malloc((size_t)target_cap64);if(target==NULL)return OPENFS_PATH_NO_SPACE;openfs_path_result_t r=openfs_readlink(d,s,p,target,target_cap64);if(r!=OPENFS_PATH_OK){free(target);return r;}
size_t tl=strlen(target);if(target[0]=='/'){if((uint64_t)tl+1U>cap){free(target);return OPENFS_PATH_NO_SPACE;}r=openfs_resolve_symlink(d,s,target,out,cap,depth+1U);free(target);return r;}
size_t pl=strlen(p);size_t cut=pl;while(cut>0U&&p[cut-1U]!='/')--cut;if(cut==0U){free(target);return OPENFS_PATH_INVALID_ARGUMENT;}if(pl>SIZE_MAX-tl-2U){free(target);return OPENFS_PATH_NO_SPACE;}size_t needed=pl+tl+2U;if((uint64_t)needed>cap){free(target);return OPENFS_PATH_NO_SPACE;}char *combined=malloc(needed);if(combined==NULL){free(target);return OPENFS_PATH_NO_SPACE;}size_t used=0U;if(cut==1U){combined[0]='/';used=1U;}else{memcpy(combined,p,cut);used=cut;}memcpy(combined+used,target,tl+1U);free(target);r=openfs_resolve_symlink(d,s,combined,out,cap,depth+1U);free(combined);return r;
}

static openfs_path_result_t link_parent_access(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t uid,uint32_t gid)
{
    size_t l=strlen(p),slash=l;while(slash>0U&&p[slash-1U]!='/')--slash;
    if(l==0U||l>=OPENFS_PATH_MAX||slash==0U)return OPENFS_PATH_INVALID_ARGUMENT;
    char pp[OPENFS_PATH_MAX];
    if(slash==1U){pp[0]='/';pp[1]='\0';}else{memcpy(pp,p,slash-1U);pp[slash-1U]='\0';}
    return openfs_path_check_access(d,s,pp,uid,gid,3U);
}
openfs_path_result_t openfs_link_as(openfs_block_device_t*d,const openfs_superblock_t*s,const char*a,const char*b,uint32_t uid,uint32_t gid)
{
    openfs_path_result_t r=link_parent_access(d,s,b,uid,gid);if(r!=OPENFS_PATH_OK)return r;return openfs_link(d,s,a,b);
}
openfs_path_result_t openfs_symlink_as(openfs_block_device_t*d,const openfs_superblock_t*s,const char*a,const char*b,uint32_t uid,uint32_t gid)
{
    openfs_path_result_t r=link_parent_access(d,s,b,uid,gid);if(r!=OPENFS_PATH_OK)return r;return openfs_symlink(d,s,a,b);
}

openfs_path_result_t openfs_link_tx(openfs_transaction_t*t,const openfs_superblock_t*s,const char*a,const char*b){openfs_block_device_t*d=openfs_transaction_device(t);if(d==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_path_result_t r=openfs_link(d,s,a,b);if(r!=OPENFS_PATH_OK)t->failed=1;return r;}
openfs_path_result_t openfs_symlink_tx(openfs_transaction_t*t,const openfs_superblock_t*s,const char*a,const char*b){openfs_block_device_t*d=openfs_transaction_device(t);if(d==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_path_result_t r=openfs_symlink(d,s,a,b);if(r!=OPENFS_PATH_OK)t->failed=1;return r;}
