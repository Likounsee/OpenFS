#include "openfs/path.h"
#include "openfs/bitmap.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>
static openfs_path_result_t inode_count(const openfs_superblock_t *s,uint64_t *n){if(s->block_size!=0U&&s->inode_table_blocks>UINT64_MAX/s->block_size)return OPENFS_PATH_CORRUPT;*n=(s->inode_table_blocks*(uint64_t)s->block_size)/OPENFS_INODE_SIZE;return *n?OPENFS_PATH_OK:OPENFS_PATH_CORRUPT;}
static openfs_path_result_t read_inode(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t n,openfs_inode_t*i){uint64_t c=0U;if(inode_count(s,&c)!=OPENFS_PATH_OK)return OPENFS_PATH_CORRUPT;return openfs_inode_read(d,s->inode_table_start,n,c,i)==OPENFS_INODE_OK?OPENFS_PATH_OK:OPENFS_PATH_IO_ERROR;}
static openfs_path_result_t split_last(const char*p,char *parent,size_t ps,char *name,size_t ns){size_t len=strlen(p);if(len==0U||len>=ps)return OPENFS_PATH_INVALID_ARGUMENT;while(len>1U&&p[len-1U]=='/')--len;if(p[0]!='/')return OPENFS_PATH_INVALID_ARGUMENT;size_t slash=len;while(slash>0U&&p[slash-1U]!='/')--slash;if(slash==0U)return OPENFS_PATH_INVALID_ARGUMENT;size_t nl=len-slash;if(nl==0U||nl>=ns)return OPENFS_PATH_NAME_TOO_LONG;memcpy(name,p+slash,nl);name[nl]='\0';if(strcmp(name,".")==0||strcmp(name,"..")==0)return OPENFS_PATH_INVALID_ARGUMENT;if(slash==1U){parent[0]='/';parent[1]='\0';}else{if(slash>=ps)return OPENFS_PATH_INVALID_ARGUMENT;memcpy(parent,p,slash-1U);parent[slash-1U]='\0';}return OPENFS_PATH_OK;}
openfs_path_result_t openfs_path_lookup(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint64_t*out){if(d==NULL||s==NULL||p==NULL||out==NULL||p[0]!='/')return OPENFS_PATH_INVALID_ARGUMENT;if(strcmp(p,"/")==0){*out=s->root_inode;return OPENFS_PATH_OK;}char buf[1024];size_t len=strlen(p);if(len>=sizeof(buf))return OPENFS_PATH_NAME_TOO_LONG;memcpy(buf,p,len+1U);uint64_t cur=s->root_inode;char *part=strtok(buf,"/");while(part){if(part[0]=='\0')return OPENFS_PATH_INVALID_ARGUMENT;if(strcmp(part,".")==0){part=strtok(NULL,"/");continue;}openfs_inode_t dir;if(read_inode(d,s,cur,&dir)!=OPENFS_PATH_OK)return OPENFS_PATH_IO_ERROR;if((dir.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)return OPENFS_PATH_NOT_DIRECTORY;if(strcmp(part,"..")==0){if(dir.parent_inode==0U)return OPENFS_PATH_CORRUPT;cur=dir.parent_inode;part=strtok(NULL,"/");continue;}openfs_dir_entry_t e;if(openfs_dir_lookup(d,s,&dir,part,&e)!=OPENFS_DIR_OK)return OPENFS_PATH_NOT_FOUND;cur=e.inode_number;part=strtok(NULL,"/");}*out=cur;return OPENFS_PATH_OK;}
openfs_path_result_t openfs_path_create(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t mode,uint64_t*out){if(d==NULL||s==NULL||p==NULL||out==NULL)return OPENFS_PATH_INVALID_ARGUMENT;char pp[1024],name[OPENFS_DIR_NAME_MAX+1U];openfs_path_result_t r=split_last(p,pp,sizeof(pp),name,sizeof(name));if(r!=OPENFS_PATH_OK)return r;uint64_t parent=0U;if(openfs_path_lookup(d,s,pp,&parent)!=OPENFS_PATH_OK)return OPENFS_PATH_NOT_FOUND;openfs_inode_t pi;if(read_inode(d,s,parent,&pi)!=OPENFS_PATH_OK||(pi.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)return OPENFS_PATH_NOT_DIRECTORY;openfs_dir_entry_t exists;if(openfs_dir_lookup(d,s,&pi,name,&exists)==OPENFS_DIR_OK)return OPENFS_PATH_EXISTS;uint64_t ino=0U;if(openfs_inode_alloc(d,s,parent,mode,&ino)!=OPENFS_INODE_ALLOC_OK)return OPENFS_PATH_NO_SPACE;openfs_dir_entry_t e={ino,1U,(uint8_t)(mode==OPENFS_INODE_MODE_DIRECTORY?2U:1U)};if(openfs_dir_add(d,s,&pi,name,&e)!=OPENFS_DIR_OK){openfs_inode_t dead;if(read_inode(d,s,ino,&dead)==OPENFS_PATH_OK){dead.mode=OPENFS_INODE_MODE_FREE;dead.link_count=0U;(void)openfs_inode_write(d,s->inode_table_start,(s->inode_table_blocks*(uint64_t)s->block_size)/OPENFS_INODE_SIZE,&dead);(void)openfs_inode_free(d,s,ino);}return OPENFS_PATH_IO_ERROR;}if(out)*out=ino;return OPENFS_PATH_OK;}
openfs_path_result_t openfs_path_mkdir(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint64_t*out){return openfs_path_create(d,s,p,OPENFS_INODE_MODE_DIRECTORY,out);}
openfs_path_result_t openfs_path_unlink(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p){char pp[1024],name[OPENFS_DIR_NAME_MAX+1U];openfs_path_result_t r=split_last(p,pp,sizeof(pp),name,sizeof(name));if(r!=OPENFS_PATH_OK)return r;uint64_t parent=0U;if(openfs_path_lookup(d,s,pp,&parent)!=OPENFS_PATH_OK)return OPENFS_PATH_NOT_FOUND;openfs_inode_t pi;if(read_inode(d,s,parent,&pi)!=OPENFS_PATH_OK||pi.mode!=OPENFS_INODE_MODE_DIRECTORY)return OPENFS_PATH_NOT_DIRECTORY;openfs_dir_entry_t e;if(openfs_dir_lookup(d,s,&pi,name,&e)!=OPENFS_DIR_OK)return OPENFS_PATH_NOT_FOUND;openfs_inode_t target;if(read_inode(d,s,e.inode_number,&target)!=OPENFS_PATH_OK)return OPENFS_PATH_IO_ERROR;if((target.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_DIRECTORY&&target.size!=0U)return OPENFS_PATH_INVALID_ARGUMENT;if(openfs_dir_remove(d,s,&pi,name)!=OPENFS_DIR_OK)return OPENFS_PATH_IO_ERROR;if(target.link_count==0U)return OPENFS_PATH_CORRUPT;if(target.link_count==1U){if(openfs_file_truncate(d,s,&target,0U)!=OPENFS_FILE_OK)return OPENFS_PATH_IO_ERROR;target.mode=OPENFS_INODE_MODE_FREE;target.link_count=0U;}else{target.link_count--;}uint64_t c=0U;if(inode_count(s,&c)!=OPENFS_PATH_OK)return OPENFS_PATH_CORRUPT;if(openfs_inode_write(d,s->inode_table_start,c,&target)!=OPENFS_INODE_OK)return OPENFS_PATH_IO_ERROR;if(target.mode==OPENFS_INODE_MODE_FREE&&(openfs_bitmap_set(d,s->inode_bitmap_start,s->inode_bitmap_blocks,e.inode_number-1U,0)!=OPENFS_BITMAP_OK))return OPENFS_PATH_IO_ERROR;return OPENFS_PATH_OK;}
openfs_path_result_t openfs_path_rename(openfs_block_device_t*d,const openfs_superblock_t*s,const char*oldp,const char*newp){
char op[1024],on[OPENFS_DIR_NAME_MAX+1U],np[1024],nn[OPENFS_DIR_NAME_MAX+1U];
if(split_last(oldp,op,sizeof(op),on,sizeof(on))!=OPENFS_PATH_OK||split_last(newp,np,sizeof(np),nn,sizeof(nn))!=OPENFS_PATH_OK)return OPENFS_PATH_INVALID_ARGUMENT;
uint64_t oldparent=0U,newparent=0U;
if(openfs_path_lookup(d,s,op,&oldparent)!=OPENFS_PATH_OK)return OPENFS_PATH_NOT_FOUND;
if(openfs_path_lookup(d,s,np,&newparent)!=OPENFS_PATH_OK)return OPENFS_PATH_NOT_FOUND;
if(oldparent==0U||newparent==0U)return OPENFS_PATH_CORRUPT;
openfs_inode_t odir,ndir;
if(read_inode(d,s,oldparent,&odir)!=OPENFS_PATH_OK||read_inode(d,s,newparent,&ndir)!=OPENFS_PATH_OK)return OPENFS_PATH_IO_ERROR;
if((odir.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY||(ndir.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)return OPENFS_PATH_NOT_DIRECTORY;
openfs_dir_entry_t e,exists;
if(openfs_dir_lookup(d,s,&odir,on,&e)!=OPENFS_DIR_OK)return OPENFS_PATH_NOT_FOUND;
if(openfs_dir_lookup(d,s,&ndir,nn,&exists)==OPENFS_DIR_OK)return OPENFS_PATH_EXISTS;
openfs_inode_t target;
if(read_inode(d,s,e.inode_number,&target)!=OPENFS_PATH_OK)return OPENFS_PATH_IO_ERROR;
if(target.mode==OPENFS_INODE_MODE_DIRECTORY&&newparent!=oldparent){
uint64_t cur=newparent;
while(cur!=s->root_inode){
if(cur==e.inode_number)return OPENFS_PATH_INVALID_ARGUMENT;
openfs_inode_t ci;if(read_inode(d,s,cur,&ci)!=OPENFS_PATH_OK)return OPENFS_PATH_IO_ERROR;
if(ci.parent_inode==0U||ci.parent_inode==cur)return OPENFS_PATH_CORRUPT;
cur=ci.parent_inode;
}
}
if(openfs_dir_remove(d,s,&odir,on)!=OPENFS_DIR_OK)return OPENFS_PATH_IO_ERROR;
if(openfs_dir_add(d,s,&ndir,nn,&e)!=OPENFS_DIR_OK){(void)openfs_dir_add(d,s,&odir,on,&e);return OPENFS_PATH_IO_ERROR;}
target.parent_inode=newparent;
uint64_t count=0U;if(inode_count(s,&count)!=OPENFS_PATH_OK||openfs_inode_write(d,s->inode_table_start,count,&target)!=OPENFS_INODE_OK)return OPENFS_PATH_IO_ERROR;
return OPENFS_PATH_OK;
}

openfs_path_result_t openfs_path_chmod(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t permissions){uint64_t ino=0U,count=0U;if(openfs_path_lookup(d,s,p,&ino)!=OPENFS_PATH_OK)return OPENFS_PATH_NOT_FOUND;openfs_inode_t in;if(read_inode(d,s,ino,&in)!=OPENFS_PATH_OK)return OPENFS_PATH_IO_ERROR;if(inode_count(s,&count)!=OPENFS_PATH_OK)return OPENFS_PATH_CORRUPT;in.mode=(in.mode&0170000U)|(permissions&07777U);return openfs_inode_write(d,s->inode_table_start,count,&in)==OPENFS_INODE_OK?OPENFS_PATH_OK:OPENFS_PATH_IO_ERROR;}
openfs_path_result_t openfs_path_set_times(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint64_t atime_ns,uint64_t mtime_ns){uint64_t ino=0U,count=0U;if(openfs_path_lookup(d,s,p,&ino)!=OPENFS_PATH_OK)return OPENFS_PATH_NOT_FOUND;openfs_inode_t in;if(read_inode(d,s,ino,&in)!=OPENFS_PATH_OK)return OPENFS_PATH_IO_ERROR;if(inode_count(s,&count)!=OPENFS_PATH_OK)return OPENFS_PATH_CORRUPT;in.atime_ns=atime_ns;in.mtime_ns=mtime_ns;return openfs_inode_write(d,s->inode_table_start,count,&in)==OPENFS_INODE_OK?OPENFS_PATH_OK:OPENFS_PATH_IO_ERROR;}
