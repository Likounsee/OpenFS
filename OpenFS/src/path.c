#include "openfs/path.h"
#include "openfs/bitmap.h"
#include "openfs/crc32c.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "openfs/time.h"
static int rollback_allocated_inode(openfs_block_device_t *d,const openfs_superblock_t *s,uint64_t ino);
static int restore_unlinked_inode_storage(openfs_block_device_t *d,const openfs_superblock_t *s,const openfs_inode_t *original)
{
    if (d == NULL || s == NULL || original == NULL || s->data_start > UINT64_MAX-s->data_blocks) return 0;
    uint64_t data_end=s->data_start+s->data_blocks;
    for (uint64_t logical = 0U; logical < original->blocks; ++logical) {
        uint64_t physical = 0U;
        if (openfs_file_map_block_device(d, s, original, logical, &physical) != OPENFS_FILE_OK || physical < s->data_start || physical >= data_end ||
            openfs_bitmap_set(d, s->block_bitmap_start, s->block_bitmap_blocks, physical, 1) != OPENFS_BITMAP_OK) {
            return 0;
        }
    }
    uint64_t root = openfs_inode_get_extent_tree_root(original);
    if (root != 0U && (root < s->data_start || root >= data_end ||
        openfs_bitmap_set(d, s->block_bitmap_start, s->block_bitmap_blocks, root, 1) != OPENFS_BITMAP_OK)) {
        return 0;
    }
    return d->flush(d->context) == OPENFS_IO_OK;
}

static int rollback_created_entry(openfs_block_device_t *d,const openfs_superblock_t *s,uint64_t parent,const char *path,uint64_t ino);
static openfs_path_result_t map_file_result(openfs_file_result_t r)
{
    switch (r) {
    case OPENFS_FILE_OK: return OPENFS_PATH_OK;
    case OPENFS_FILE_NO_SPACE: return OPENFS_PATH_NO_SPACE;
    case OPENFS_FILE_OUT_OF_RANGE: return OPENFS_PATH_NO_SPACE;
    case OPENFS_FILE_CORRUPT: return OPENFS_PATH_CORRUPT;
    case OPENFS_FILE_ACCESS_DENIED: return OPENFS_PATH_ACCESS_DENIED;
    default: return OPENFS_PATH_IO_ERROR;
    }
}
static openfs_path_result_t map_dir_result(openfs_dir_result_t r){switch(r){case OPENFS_DIR_OK:return OPENFS_PATH_OK;case OPENFS_DIR_NOT_FOUND:return OPENFS_PATH_NOT_FOUND;case OPENFS_DIR_EXISTS:return OPENFS_PATH_EXISTS;case OPENFS_DIR_NO_SPACE:return OPENFS_PATH_NO_SPACE;case OPENFS_DIR_NAME_TOO_LONG:return OPENFS_PATH_NAME_TOO_LONG;case OPENFS_DIR_IO_ERROR:return OPENFS_PATH_IO_ERROR;case OPENFS_DIR_CORRUPT:return OPENFS_PATH_CORRUPT;default:return OPENFS_PATH_INVALID_ARGUMENT;}}
static openfs_path_result_t inode_count(const openfs_superblock_t *s,uint64_t *n){if(s==NULL||n==NULL||s->block_size==0U||s->inode_table_blocks>UINT64_MAX/s->block_size)return OPENFS_PATH_CORRUPT;uint64_t bytes=s->inode_table_blocks*(uint64_t)s->block_size;if(bytes<OPENFS_INODE_SIZE)return OPENFS_PATH_CORRUPT;*n=bytes/OPENFS_INODE_SIZE;return *n?OPENFS_PATH_OK:OPENFS_PATH_CORRUPT;}
static openfs_path_result_t read_inode(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t n,openfs_inode_t*i){if(!openfs_block_device_is_valid(d)||s==NULL||i==NULL)return OPENFS_PATH_INVALID_ARGUMENT;uint64_t c=0U;if(inode_count(s,&c)!=OPENFS_PATH_OK||n==0U||n>c)return OPENFS_PATH_CORRUPT;openfs_inode_result_t r=openfs_inode_read(d,s->inode_table_start,n,c,i);return r==OPENFS_INODE_OK?OPENFS_PATH_OK:(r==OPENFS_INODE_CORRUPT?OPENFS_PATH_CORRUPT:OPENFS_PATH_IO_ERROR);}
static openfs_path_result_t split_last(const char*p,char *parent,size_t ps,char *name,size_t ns){if(p==NULL||parent==NULL||name==NULL||ps<2U||ns<2U)return OPENFS_PATH_INVALID_ARGUMENT;size_t len=strlen(p);if(len==0U||len>=ps)return OPENFS_PATH_INVALID_ARGUMENT;while(len>1U&&p[len-1U]=='/')--len;if(p[0]!='/')return OPENFS_PATH_INVALID_ARGUMENT;size_t slash=len;while(slash>0U&&p[slash-1U]!='/')--slash;if(slash==0U)return OPENFS_PATH_INVALID_ARGUMENT;size_t nl=len-slash;if(nl==0U||nl>=ns)return OPENFS_PATH_NAME_TOO_LONG;memcpy(name,p+slash,nl);name[nl]='\0';if(strcmp(name,".")==0||strcmp(name,"..")==0)return OPENFS_PATH_INVALID_ARGUMENT;if(slash==1U){parent[0]='/';parent[1]='\0';}else{if(slash>=ps)return OPENFS_PATH_INVALID_ARGUMENT;memcpy(parent,p,slash-1U);parent[slash-1U]='\0';}return OPENFS_PATH_OK;}
openfs_path_result_t openfs_path_lookup(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint64_t*out){if(d==NULL||s==NULL||p==NULL||out==NULL||p[0]!='/')return OPENFS_PATH_INVALID_ARGUMENT;if(strcmp(p,"/")==0){openfs_inode_t root;openfs_path_result_t rr=read_inode(d,s,s->root_inode,&root);if(rr!=OPENFS_PATH_OK)return rr;if((root.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)return OPENFS_PATH_CORRUPT;*out=s->root_inode;return OPENFS_PATH_OK;}char buf[OPENFS_PATH_MAX];size_t len=strlen(p);if(len>=OPENFS_PATH_MAX)return OPENFS_PATH_NAME_TOO_LONG;memcpy(buf,p,len+1U);uint64_t cur=s->root_inode;char *part=strtok(buf,"/");while(part){if(part[0]=='\0')return OPENFS_PATH_INVALID_ARGUMENT;if(strcmp(part,".")==0){part=strtok(NULL,"/");continue;}openfs_inode_t dir;openfs_path_result_t dir_result=read_inode(d,s,cur,&dir);if(dir_result!=OPENFS_PATH_OK)return dir_result;if((dir.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)return OPENFS_PATH_NOT_DIRECTORY;if(strcmp(part,"..")==0){if(dir.parent_inode==0U)return OPENFS_PATH_CORRUPT;uint64_t parent_count=0U;if(inode_count(s,&parent_count)!=OPENFS_PATH_OK||dir.parent_inode>parent_count)return OPENFS_PATH_CORRUPT;openfs_inode_t parent;openfs_path_result_t parent_result=read_inode(d,s,dir.parent_inode,&parent);if(parent_result!=OPENFS_PATH_OK)return parent_result;if((parent.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)return OPENFS_PATH_CORRUPT;cur=dir.parent_inode;part=strtok(NULL,"/");continue;}openfs_dir_entry_t e;openfs_dir_result_t dr=openfs_dir_lookup(d,s,&dir,part,&e);if(dr!=OPENFS_DIR_OK)return map_dir_result(dr);openfs_inode_t target;openfs_path_result_t tr=read_inode(d,s,e.inode_number,&target);if(tr!=OPENFS_PATH_OK)return tr;if(target.generation!=e.generation)return OPENFS_PATH_CORRUPT;uint8_t expected=(target.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_DIRECTORY?2U:((target.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_SYMLINK?3U:1U);if(expected!=e.type)return OPENFS_PATH_CORRUPT;cur=e.inode_number;part=strtok(NULL,"/");}*out=cur;return OPENFS_PATH_OK;}
openfs_path_result_t openfs_path_lookup_as(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t uid,uint32_t gid,uint64_t*out)
{
    if(d==NULL||s==NULL||p==NULL||out==NULL||p[0]!='/')return OPENFS_PATH_INVALID_ARGUMENT;
    size_t len=strlen(p);if(len>=OPENFS_PATH_MAX)return OPENFS_PATH_NAME_TOO_LONG;
    if(strcmp(p,"/")==0){openfs_inode_t root;openfs_path_result_t rr=read_inode(d,s,s->root_inode,&root);if(rr!=OPENFS_PATH_OK)return rr;if((root.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)return OPENFS_PATH_CORRUPT;openfs_inode_result_t ar=openfs_inode_check_access(&root,uid,gid,1U);if(ar==OPENFS_INODE_ACCESS_DENIED)return OPENFS_PATH_ACCESS_DENIED;if(ar!=OPENFS_INODE_OK)return OPENFS_PATH_CORRUPT;*out=s->root_inode;return OPENFS_PATH_OK;}
    char buf[OPENFS_PATH_MAX];memcpy(buf,p,len+1U);uint64_t cur=s->root_inode;char *part=strtok(buf,"/");
    while(part){
        openfs_inode_t dir;openfs_path_result_t dir_result=read_inode(d,s,cur,&dir);if(dir_result!=OPENFS_PATH_OK)return dir_result;
        if((dir.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)return OPENFS_PATH_NOT_DIRECTORY;
        openfs_inode_result_t ar=openfs_inode_check_access(&dir,uid,gid,1U);
        if(ar==OPENFS_INODE_ACCESS_DENIED)return OPENFS_PATH_ACCESS_DENIED;
        if(ar!=OPENFS_INODE_OK)return OPENFS_PATH_CORRUPT;
        if(strcmp(part,".")==0){part=strtok(NULL,"/");continue;}
        if(strcmp(part,"..")==0){if(dir.parent_inode==0U)return OPENFS_PATH_CORRUPT;uint64_t parent_count=0U;if(inode_count(s,&parent_count)!=OPENFS_PATH_OK||dir.parent_inode>parent_count)return OPENFS_PATH_CORRUPT;openfs_inode_t parent;openfs_path_result_t parent_result=read_inode(d,s,dir.parent_inode,&parent);if(parent_result!=OPENFS_PATH_OK)return parent_result;if((parent.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)return OPENFS_PATH_CORRUPT;cur=dir.parent_inode;part=strtok(NULL,"/");continue;}
        openfs_dir_entry_t e;openfs_dir_result_t dr=openfs_dir_lookup(d,s,&dir,part,&e);if(dr!=OPENFS_DIR_OK)return map_dir_result(dr);
        openfs_inode_t target;openfs_path_result_t target_result=read_inode(d,s,e.inode_number,&target);if(target_result!=OPENFS_PATH_OK)return target_result;if(target.generation!=e.generation)return OPENFS_PATH_CORRUPT;uint8_t expected=(target.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_DIRECTORY?2U:((target.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_SYMLINK?3U:1U);if(expected!=e.type)return OPENFS_PATH_CORRUPT;
        cur=e.inode_number;part=strtok(NULL,"/");
    }
    *out=cur;return OPENFS_PATH_OK;
}
static openfs_path_result_t read_symlink_target(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t ino,char*out,size_t cap){if(!openfs_block_device_is_valid(d)||s==NULL||out==NULL||cap==0U)return OPENFS_PATH_INVALID_ARGUMENT;uint64_t c=0U;if(inode_count(s,&c)!=OPENFS_PATH_OK)return OPENFS_PATH_CORRUPT;openfs_inode_t in;openfs_inode_result_t ir=openfs_inode_read(d,s->inode_table_start,ino,c,&in);if(ir!=OPENFS_INODE_OK)return ir==OPENFS_INODE_CORRUPT?OPENFS_PATH_CORRUPT:OPENFS_PATH_IO_ERROR;if((in.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_SYMLINK)return OPENFS_PATH_INVALID_ARGUMENT;if(in.size==UINT64_MAX||in.size>(uint64_t)SIZE_MAX||in.size+1U>(uint64_t)cap)return OPENFS_PATH_NO_SPACE;if((in.flags&OPENFS_INODE_FLAG_INLINE_DATA)!=0U){memcpy(out,in.inline_data,(size_t)in.size);out[in.size]='\0';return OPENFS_PATH_OK;}size_t got=0U;openfs_file_result_t fr=openfs_file_read(d,s,&in,0U,out,(size_t)in.size,&got);if(fr!=OPENFS_FILE_OK||got!=in.size)return map_file_result(fr);out[in.size]='\0';return OPENFS_PATH_OK;}
openfs_path_result_t openfs_path_lookup_follow(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint64_t*out){if(d==NULL||s==NULL||p==NULL||out==NULL||p[0]!='/')return OPENFS_PATH_INVALID_ARGUMENT;size_t plen=strlen(p);if(plen>=OPENFS_PATH_MAX)return OPENFS_PATH_NAME_TOO_LONG;char pending[OPENFS_PATH_MAX];memcpy(pending,p,plen+1U);for(uint32_t depth=0U;depth<=40U;depth++){size_t len=strlen(pending);char scan[OPENFS_PATH_MAX];memcpy(scan,pending,len+1U);size_t pos=1U;while(pos<=len){while(pos<len&&scan[pos]=='/')pos++;if(pos>=len)break;size_t end=pos;while(end<len&&scan[end]!='/')end++;size_t prefix_len=end;char prefix[OPENFS_PATH_MAX];if(prefix_len>=sizeof(prefix))return OPENFS_PATH_NAME_TOO_LONG;memcpy(prefix,scan,prefix_len);prefix[prefix_len]='\0';uint64_t ino=0U;openfs_path_result_t r=openfs_path_lookup(d,s,prefix,&ino);if(r!=OPENFS_PATH_OK)return r;openfs_inode_t in;openfs_path_result_t in_result=read_inode(d,s,ino,&in);if(in_result!=OPENFS_PATH_OK)return in_result;if((in.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_SYMLINK){if(depth==40U)return OPENFS_PATH_CORRUPT;char target[4096];r=read_symlink_target(d,s,ino,target,sizeof(target));if(r!=OPENFS_PATH_OK)return r;size_t suffix_len=len-end;size_t target_len=strlen(target);size_t parent_len=pos>1U?pos-1U:1U;char replacement[OPENFS_PATH_MAX];size_t used=0U;if(target[0]=='/'){if(target_len>=sizeof(replacement))return OPENFS_PATH_NAME_TOO_LONG;memcpy(replacement,target,target_len);used=target_len;}else{if(parent_len+target_len+1U>=sizeof(replacement))return OPENFS_PATH_NAME_TOO_LONG;if(parent_len==1U){replacement[0]='/';used=1U;}else{memcpy(replacement,scan,parent_len);used=parent_len;}if(used>1U&&replacement[used-1U]!='/')replacement[used++]='/';memcpy(replacement+used,target,target_len);used+=target_len;}if(suffix_len>0U){if(used>0U&&replacement[used-1U]!='/')replacement[used++]='/';if(used+suffix_len>=sizeof(replacement))return OPENFS_PATH_NAME_TOO_LONG;memcpy(replacement+used,scan+end,suffix_len);used+=suffix_len;}replacement[used]='\0';memcpy(pending,replacement,used+1U);goto next_symlink;}pos=end;}return openfs_path_lookup(d,s,pending,out);next_symlink:;}
return OPENFS_PATH_CORRUPT;}
openfs_path_result_t openfs_path_create(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t mode,uint64_t*out){if(d==NULL||s==NULL||p==NULL||out==NULL)return OPENFS_PATH_INVALID_ARGUMENT;char pp[OPENFS_PATH_MAX],name[OPENFS_DIR_NAME_MAX+1U];openfs_path_result_t r=split_last(p,pp,sizeof(pp),name,sizeof(name));if(r!=OPENFS_PATH_OK)return r;uint64_t parent=0U;openfs_path_result_t parent_result=openfs_path_lookup_follow(d,s,pp,&parent);if(parent_result!=OPENFS_PATH_OK)return parent_result;openfs_inode_t pi;openfs_path_result_t pi_result=read_inode(d,s,parent,&pi);if(pi_result!=OPENFS_PATH_OK)return pi_result;if((pi.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)return OPENFS_PATH_NOT_DIRECTORY;openfs_dir_entry_t exists;openfs_dir_result_t dr=openfs_dir_lookup(d,s,&pi,name,&exists);if(dr==OPENFS_DIR_OK)return OPENFS_PATH_EXISTS;if(dr!=OPENFS_DIR_NOT_FOUND)return map_dir_result(dr);uint64_t ino=0U;openfs_inode_alloc_result_t ar=openfs_inode_alloc(d,s,parent,mode,&ino);if(ar!=OPENFS_INODE_ALLOC_OK){if(ar==OPENFS_INODE_ALLOC_INVALID_ARGUMENT)return OPENFS_PATH_INVALID_ARGUMENT;if(ar==OPENFS_INODE_ALLOC_CORRUPT)return OPENFS_PATH_CORRUPT;if(ar==OPENFS_INODE_ALLOC_IO_ERROR)return OPENFS_PATH_IO_ERROR;return OPENFS_PATH_NO_SPACE;}openfs_inode_t allocated_inode;openfs_path_result_t allocated_result=read_inode(d,s,ino,&allocated_inode);if(allocated_result!=OPENFS_PATH_OK){if(!rollback_allocated_inode(d,s,ino))return OPENFS_PATH_CORRUPT;return allocated_result;}uint8_t entry_type=(mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_DIRECTORY?2U:((mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_SYMLINK?3U:1U);openfs_dir_entry_t e={ino,allocated_inode.generation,entry_type};openfs_dir_result_t create_dr=openfs_dir_add(d,s,&pi,name,&e);if(create_dr!=OPENFS_DIR_OK){if(!rollback_allocated_inode(d,s,ino))return OPENFS_PATH_CORRUPT;return map_dir_result(create_dr);}if(out)*out=ino;return OPENFS_PATH_OK;}
openfs_path_result_t openfs_path_mkdir(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint64_t*out){return openfs_path_create(d,s,p,OPENFS_INODE_MODE_DIRECTORY,out);}
openfs_path_result_t openfs_path_unlink(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p){char pp[OPENFS_PATH_MAX],name[OPENFS_DIR_NAME_MAX+1U];openfs_path_result_t r=split_last(p,pp,sizeof(pp),name,sizeof(name));if(r!=OPENFS_PATH_OK)return r;uint64_t parent=0U;openfs_path_result_t parent_lookup=openfs_path_lookup(d,s,pp,&parent);if(parent_lookup!=OPENFS_PATH_OK)return parent_lookup;openfs_inode_t pi;openfs_path_result_t pir=read_inode(d,s,parent,&pi);if(pir!=OPENFS_PATH_OK)return pir;if((pi.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)return OPENFS_PATH_NOT_DIRECTORY;openfs_dir_entry_t e;openfs_dir_result_t dr=openfs_dir_lookup(d,s,&pi,name,&e);if(dr!=OPENFS_DIR_OK)return map_dir_result(dr);openfs_inode_t target;openfs_path_result_t target_result=read_inode(d,s,e.inode_number,&target);if(target_result!=OPENFS_PATH_OK)return target_result;if(target.generation!=e.generation)return OPENFS_PATH_CORRUPT;uint8_t expected=(target.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_DIRECTORY?2U:((target.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_SYMLINK?3U:1U);if(expected!=e.type)return OPENFS_PATH_CORRUPT;
openfs_inode_t original_target=target;if((target.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_DIRECTORY){if(target.size%OPENFS_DIR_ENTRY_SIZE!=0U)return OPENFS_PATH_CORRUPT;uint64_t entries=target.size/OPENFS_DIR_ENTRY_SIZE;uint8_t raw[OPENFS_DIR_ENTRY_SIZE];for(uint64_t n=0U;n<entries;n++){if(n>UINT64_MAX/OPENFS_DIR_ENTRY_SIZE)return OPENFS_PATH_CORRUPT;size_t got=0U;openfs_file_result_t fr=openfs_file_read(d,s,&target,n*OPENFS_DIR_ENTRY_SIZE,raw,sizeof(raw),&got);if(fr!=OPENFS_FILE_OK||got!=sizeof(raw))return map_file_result(fr);int nonzero=0;for(size_t z=0U;z<sizeof(raw);z++)if(raw[z]!=0U){nonzero=1;break;}if(!nonzero)continue;if(memcmp(raw,"ODIR1",5U)!=0)return OPENFS_PATH_CORRUPT;uint32_t stored=(uint32_t)raw[252U]|((uint32_t)raw[253U]<<8U)|((uint32_t)raw[254U]<<16U)|((uint32_t)raw[255U]<<24U);if(stored!=openfs_crc32c(raw,252U))return OPENFS_PATH_CORRUPT;size_t nl=raw[7U];if(nl==0U||nl>OPENFS_DIR_NAME_MAX)return OPENFS_PATH_CORRUPT;return OPENFS_PATH_NOT_EMPTY;}}openfs_dir_result_t rr=openfs_dir_remove(d,s,&pi,name);if(rr!=OPENFS_DIR_OK)return map_dir_result(rr);
if(target.link_count==0U)return OPENFS_PATH_CORRUPT;
if(target.link_count==1U){
    openfs_file_result_t truncate_result=openfs_file_truncate(d,s,&target,0U);
    if(truncate_result!=OPENFS_FILE_OK){
        openfs_dir_entry_t rollback_entry=e;
        if(openfs_dir_add(d,s,&pi,name,&rollback_entry)!=OPENFS_DIR_OK)return OPENFS_PATH_CORRUPT;
        return map_file_result(truncate_result);
    }
    target.mode=OPENFS_INODE_MODE_FREE;
    target.link_count=0U;
    target.parent_inode=0U;
    target.uid=0U;
    target.gid=0U;
    target.atime_ns=0U;
    target.mtime_ns=0U;
    target.ctime_ns=0U;
}else{
    target.link_count--;
}uint64_t c=0U;if(inode_count(s,&c)!=OPENFS_PATH_OK)return OPENFS_PATH_CORRUPT;
if(openfs_inode_write(d,s->inode_table_start,c,&target)!=OPENFS_INODE_OK){
    int rollback_ok=1;
    if(original_target.link_count==1U){
        if(!restore_unlinked_inode_storage(d,s,&original_target))rollback_ok=0;
    }
    if(openfs_inode_write(d,s->inode_table_start,c,&original_target)!=OPENFS_INODE_OK)rollback_ok=0;
    if(openfs_dir_add(d,s,&pi,name,&e)!=OPENFS_DIR_OK)rollback_ok=0;
    if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
    return rollback_ok?OPENFS_PATH_IO_ERROR:OPENFS_PATH_CORRUPT;
}
if (target.mode == OPENFS_INODE_MODE_FREE &&
    openfs_inode_free(d, s, e.inode_number) != OPENFS_INODE_ALLOC_OK) {
    int rollback_ok = 1;
    if (original_target.link_count == 1U) {
        if (!restore_unlinked_inode_storage(d, s, &original_target)) rollback_ok = 0;
    }
    if (openfs_inode_write(d, s->inode_table_start, c, &original_target) != OPENFS_INODE_OK) {
        rollback_ok = 0;
    }
    if (openfs_dir_add(d, s, &pi, name, &e) != OPENFS_DIR_OK) {
        rollback_ok = 0;
    }
    if (d->flush(d->context) != OPENFS_IO_OK) {
        rollback_ok = 0;
    }
    return rollback_ok ? OPENFS_PATH_IO_ERROR : OPENFS_PATH_CORRUPT;
}
if(d->flush(d->context)!=OPENFS_IO_OK){
    int rollback_ok=1;
    if(target.mode==OPENFS_INODE_MODE_FREE){
        if(openfs_bitmap_set(d,s->inode_bitmap_start,s->inode_bitmap_blocks,e.inode_number-1U,1)!=OPENFS_BITMAP_OK)rollback_ok=0;
        if(!restore_unlinked_inode_storage(d,s,&original_target))rollback_ok=0;
    }
    if(openfs_inode_write(d,s->inode_table_start,c,&original_target)!=OPENFS_INODE_OK)rollback_ok=0;
    if(openfs_dir_add(d,s,&pi,name,&e)!=OPENFS_DIR_OK)rollback_ok=0;
    if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
    return rollback_ok?OPENFS_PATH_IO_ERROR:OPENFS_PATH_CORRUPT;
}
return OPENFS_PATH_OK;}
openfs_path_result_t openfs_path_rename(openfs_block_device_t*d,const openfs_superblock_t*s,const char*oldp,const char*newp){
char op[OPENFS_PATH_MAX],on[OPENFS_DIR_NAME_MAX+1U],np[OPENFS_PATH_MAX],nn[OPENFS_DIR_NAME_MAX+1U];
if(split_last(oldp,op,sizeof(op),on,sizeof(on))!=OPENFS_PATH_OK||split_last(newp,np,sizeof(np),nn,sizeof(nn))!=OPENFS_PATH_OK)return OPENFS_PATH_INVALID_ARGUMENT;
uint64_t oldparent=0U,newparent=0U;
openfs_path_result_t lr=openfs_path_lookup_follow(d,s,op,&oldparent);if(lr!=OPENFS_PATH_OK)return lr;
lr=openfs_path_lookup_follow(d,s,np,&newparent);if(lr!=OPENFS_PATH_OK)return lr;
if(oldparent==0U||newparent==0U)return OPENFS_PATH_CORRUPT;
openfs_inode_t odir,ndir;
openfs_path_result_t odir_result=read_inode(d,s,oldparent,&odir);if(odir_result!=OPENFS_PATH_OK)return odir_result;openfs_path_result_t ndir_result=read_inode(d,s,newparent,&ndir);if(ndir_result!=OPENFS_PATH_OK)return ndir_result;
if((odir.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY||(ndir.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_DIRECTORY)return OPENFS_PATH_NOT_DIRECTORY;
openfs_dir_entry_t e,exists;
openfs_dir_result_t dr=openfs_dir_lookup(d,s,&odir,on,&e);if(dr!=OPENFS_DIR_OK)return map_dir_result(dr);
dr=openfs_dir_lookup(d,s,&ndir,nn,&exists);if(dr==OPENFS_DIR_OK)return OPENFS_PATH_EXISTS;if(dr!=OPENFS_DIR_NOT_FOUND)return map_dir_result(dr);
openfs_inode_t target;
openfs_path_result_t target_result=read_inode(d,s,e.inode_number,&target);if(target_result!=OPENFS_PATH_OK)return target_result;
openfs_inode_t original_target=target;
if((target.mode&OPENFS_INODE_TYPE_MASK)==OPENFS_INODE_MODE_DIRECTORY&&newparent!=oldparent){
uint64_t cur=newparent,steps=0U,max_steps=0U;if(inode_count(s,&max_steps)!=OPENFS_PATH_OK)return OPENFS_PATH_CORRUPT;
while(cur!=s->root_inode){
if(++steps>max_steps)return OPENFS_PATH_CORRUPT;
if(cur==e.inode_number)return OPENFS_PATH_INVALID_ARGUMENT;
openfs_inode_t ci;openfs_path_result_t ci_result=read_inode(d,s,cur,&ci);if(ci_result!=OPENFS_PATH_OK)return ci_result;
if(ci.parent_inode==0U||ci.parent_inode==cur)return OPENFS_PATH_CORRUPT;
cur=ci.parent_inode;
}
}
openfs_dir_result_t remove_result=openfs_dir_remove(d,s,&odir,on);if(remove_result!=OPENFS_DIR_OK)return map_dir_result(remove_result);
openfs_dir_result_t add_result=openfs_dir_add(d,s,&ndir,nn,&e);if(add_result!=OPENFS_DIR_OK){
    if(openfs_dir_add(d,s,&odir,on,&e)!=OPENFS_DIR_OK)return OPENFS_PATH_CORRUPT;
    return map_dir_result(add_result);
}
target.parent_inode=newparent;
uint64_t count=0U;
openfs_path_result_t count_result=inode_count(s,&count);openfs_inode_result_t write_result=count_result==OPENFS_PATH_OK?openfs_inode_write(d,s->inode_table_start,count,&target):OPENFS_INODE_CORRUPT;
if(count_result!=OPENFS_PATH_OK||write_result!=OPENFS_INODE_OK){
    int rollback_ok=1;
    if(openfs_dir_remove(d,s,&ndir,nn)!=OPENFS_DIR_OK)rollback_ok=0;
    if(openfs_dir_add(d,s,&odir,on,&e)!=OPENFS_DIR_OK)rollback_ok=0;
    if(openfs_inode_write(d,s->inode_table_start,count,&original_target)!=OPENFS_INODE_OK) rollback_ok=0;
    if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
    if(!rollback_ok)return OPENFS_PATH_CORRUPT;
    if(count_result!=OPENFS_PATH_OK)return count_result;
    return write_result==OPENFS_INODE_CORRUPT?OPENFS_PATH_CORRUPT:OPENFS_PATH_IO_ERROR;
}
if(d->flush(d->context)!=OPENFS_IO_OK){
    int rollback_ok=1;
    if(openfs_dir_remove(d,s,&ndir,nn)!=OPENFS_DIR_OK)rollback_ok=0;
    if(openfs_dir_add(d,s,&odir,on,&e)!=OPENFS_DIR_OK)rollback_ok=0;
    if(openfs_inode_write(d,s->inode_table_start,count,&original_target)!=OPENFS_INODE_OK)rollback_ok=0;
    if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
    return rollback_ok?OPENFS_PATH_IO_ERROR:OPENFS_PATH_CORRUPT;
}
return OPENFS_PATH_OK;
}

openfs_path_result_t openfs_path_chmod(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t permissions){if(!openfs_block_device_is_valid(d)||s==NULL||p==NULL)return OPENFS_PATH_INVALID_ARGUMENT;if((permissions&~OPENFS_INODE_PERMISSION_MASK)!=0U)return OPENFS_PATH_INVALID_ARGUMENT;uint64_t ino=0U,count=0U;openfs_path_result_t lr=openfs_path_lookup(d,s,p,&ino);if(lr!=OPENFS_PATH_OK)return lr;openfs_inode_t in;openfs_path_result_t inode_result=read_inode(d,s,ino,&in);if(inode_result!=OPENFS_PATH_OK)return inode_result;openfs_inode_t original=in;openfs_path_result_t count_result=inode_count(s,&count);if(count_result!=OPENFS_PATH_OK)return count_result;in.mode=(in.mode&OPENFS_INODE_TYPE_MASK)|(permissions&OPENFS_INODE_PERMISSION_MASK);uint64_t now=openfs_time_now_ns();if(now!=UINT64_MAX)in.ctime_ns=now;openfs_inode_result_t write_result=openfs_inode_write(d,s->inode_table_start,count,&in);if(write_result!=OPENFS_INODE_OK)return write_result==OPENFS_INODE_CORRUPT?OPENFS_PATH_CORRUPT:OPENFS_PATH_IO_ERROR;if(d->flush(d->context)==OPENFS_IO_OK)return OPENFS_PATH_OK;return openfs_inode_write(d,s->inode_table_start,count,&original)==OPENFS_INODE_OK&&d->flush(d->context)==OPENFS_IO_OK?OPENFS_PATH_IO_ERROR:OPENFS_PATH_CORRUPT;}
openfs_path_result_t openfs_path_set_times(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint64_t atime_ns,uint64_t mtime_ns){if(!openfs_block_device_is_valid(d)||s==NULL||p==NULL)return OPENFS_PATH_INVALID_ARGUMENT;uint64_t ino=0U,count=0U;openfs_path_result_t lr=openfs_path_lookup(d,s,p,&ino);if(lr!=OPENFS_PATH_OK)return lr;openfs_inode_t in;openfs_path_result_t inode_result=read_inode(d,s,ino,&in);if(inode_result!=OPENFS_PATH_OK)return inode_result;openfs_inode_t original=in;openfs_path_result_t count_result=inode_count(s,&count);if(count_result!=OPENFS_PATH_OK)return count_result;in.atime_ns=atime_ns;in.mtime_ns=mtime_ns;uint64_t now=openfs_time_now_ns();if(now!=UINT64_MAX)in.ctime_ns=now;openfs_inode_result_t write_result=openfs_inode_write(d,s->inode_table_start,count,&in);if(write_result!=OPENFS_INODE_OK)return write_result==OPENFS_INODE_CORRUPT?OPENFS_PATH_CORRUPT:OPENFS_PATH_IO_ERROR;if(d->flush(d->context)==OPENFS_IO_OK)return OPENFS_PATH_OK;return openfs_inode_write(d,s->inode_table_start,count,&original)==OPENFS_INODE_OK&&d->flush(d->context)==OPENFS_IO_OK?OPENFS_PATH_IO_ERROR:OPENFS_PATH_CORRUPT;}
openfs_path_result_t openfs_path_check_access(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t uid,uint32_t gid,uint8_t requested){if(!openfs_block_device_is_valid(d)||s==NULL||p==NULL)return OPENFS_PATH_INVALID_ARGUMENT;uint64_t ino=0U;if(requested>7U)return OPENFS_PATH_INVALID_ARGUMENT;openfs_path_result_t lr=openfs_path_lookup(d,s,p,&ino);if(lr!=OPENFS_PATH_OK)return lr;openfs_inode_t in;openfs_path_result_t ir=read_inode(d,s,ino,&in);if(ir!=OPENFS_PATH_OK)return ir;openfs_inode_result_t r=openfs_inode_check_access(&in,uid,gid,requested);return r==OPENFS_INODE_OK?OPENFS_PATH_OK:(r==OPENFS_INODE_ACCESS_DENIED?OPENFS_PATH_ACCESS_DENIED:OPENFS_PATH_CORRUPT);}


static openfs_path_result_t require_access(openfs_block_device_t *d,const openfs_superblock_t *s,uint64_t ino,uint32_t uid,uint32_t gid,uint8_t requested)
{
    openfs_inode_t in;
    openfs_path_result_t in_result=read_inode(d,s,ino,&in);if(in_result!=OPENFS_PATH_OK)return in_result;
    openfs_inode_result_t ar=openfs_inode_check_access(&in,uid,gid,requested);
    return ar==OPENFS_INODE_OK?OPENFS_PATH_OK:(ar==OPENFS_INODE_ACCESS_DENIED?OPENFS_PATH_ACCESS_DENIED:OPENFS_PATH_CORRUPT);
}
static openfs_path_result_t parent_access(openfs_block_device_t *d,const openfs_superblock_t *s,const char *p,uint32_t uid,uint32_t gid,uint64_t *parent)
{
    char pp[OPENFS_PATH_MAX],name[OPENFS_DIR_NAME_MAX+1U];
    openfs_path_result_t r=split_last(p,pp,sizeof(pp),name,sizeof(name));
    if(r!=OPENFS_PATH_OK)return r;
    r=openfs_path_lookup_as(d,s,pp,uid,gid,parent);
    if(r!=OPENFS_PATH_OK)return r;
    return require_access(d,s,*parent,uid,gid,3U);
}
static int rollback_allocated_inode(openfs_block_device_t *d,const openfs_superblock_t *s,uint64_t ino)
{
    uint64_t count=0U;
    if(inode_count(s,&count)!=OPENFS_PATH_OK)return 0;
    openfs_inode_t in;
    if(read_inode(d,s,ino,&in)!=OPENFS_PATH_OK)return 0;
    in.mode=OPENFS_INODE_MODE_FREE;
    in.link_count=0U;
    in.parent_inode=0U;
    in.uid=0U;
    in.gid=0U;
    in.atime_ns=0U;
    in.mtime_ns=0U;
    in.ctime_ns=0U;
    in.size=0U;
    in.blocks=0U;
    in.extent_count=0U;
    in.flags=0U;
    memset(in.inline_data,0,sizeof(in.inline_data));
    memset(in.reserved,0,sizeof(in.reserved));
    if(openfs_inode_write(d,s->inode_table_start,count,&in)!=OPENFS_INODE_OK)return 0;
    return openfs_inode_free(d,s,ino)==OPENFS_INODE_ALLOC_OK &&
           d->flush(d->context)==OPENFS_IO_OK;
}

static int rollback_created_entry(openfs_block_device_t *d,const openfs_superblock_t *s,uint64_t parent,const char *path,uint64_t ino)
{
    char pp[OPENFS_PATH_MAX],name[OPENFS_DIR_NAME_MAX+1U];
    openfs_inode_t pi;
    if(split_last(path,pp,sizeof(pp),name,sizeof(name))!=OPENFS_PATH_OK||
       read_inode(d,s,parent,&pi)!=OPENFS_PATH_OK||
       openfs_dir_remove(d,s,&pi,name)!=OPENFS_DIR_OK)return 0;
    uint64_t count=0U;
    if(inode_count(s,&count)!=OPENFS_PATH_OK)return 0;
    openfs_inode_t in;
    if(read_inode(d,s,ino,&in)!=OPENFS_PATH_OK)return 0;
    in.mode=OPENFS_INODE_MODE_FREE;
    in.link_count=0U;
    in.parent_inode=0U;
    in.uid=0U;
    in.gid=0U;
    in.atime_ns=0U;
    in.mtime_ns=0U;
    in.ctime_ns=0U;
    in.size=0U;
    in.blocks=0U;
    in.extent_count=0U;
    in.flags=0U;
    memset(in.inline_data,0,sizeof(in.inline_data));
    memset(in.reserved,0,sizeof(in.reserved));
    if(openfs_inode_write(d,s->inode_table_start,count,&in)!=OPENFS_INODE_OK)return 0;
    if(openfs_inode_free(d,s,ino)!=OPENFS_INODE_ALLOC_OK)return 0;
    return d->flush(d->context)==OPENFS_IO_OK;
}
openfs_path_result_t openfs_path_create_as(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t mode,uint32_t uid,uint32_t gid,uint64_t*out)
{
    uint64_t parent=0U;openfs_path_result_t r=parent_access(d,s,p,uid,gid,&parent);
    if(r!=OPENFS_PATH_OK)return r;
    r=openfs_path_create(d,s,p,mode,out);
    if(r!=OPENFS_PATH_OK)return r;
    uint64_t count=0U;
    if(inode_count(s,&count)!=OPENFS_PATH_OK){if(!rollback_created_entry(d,s,parent,p,*out))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_CORRUPT;}
    openfs_inode_t in;
    if(read_inode(d,s,*out,&in)!=OPENFS_PATH_OK){if(!rollback_created_entry(d,s,parent,p,*out))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_IO_ERROR;}
    in.uid=uid;in.gid=gid;
    uint64_t now=openfs_time_now_ns();if(now!=UINT64_MAX)in.ctime_ns=now;
    if(openfs_inode_write(d,s->inode_table_start,count,&in)!=OPENFS_INODE_OK){if(!rollback_created_entry(d,s,parent,p,*out))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_IO_ERROR;}
    if(d->flush(d->context)!=OPENFS_IO_OK){if(!rollback_created_entry(d,s,parent,p,*out))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_IO_ERROR;}
    return OPENFS_PATH_OK;
}
openfs_path_result_t openfs_path_mkdir_as(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t uid,uint32_t gid,uint64_t*out)
{
    uint64_t parent=0U;openfs_path_result_t r=parent_access(d,s,p,uid,gid,&parent);
    if(r!=OPENFS_PATH_OK)return r;
    r=openfs_path_mkdir(d,s,p,out);
    if(r!=OPENFS_PATH_OK)return r;
    uint64_t count=0U;
    if(inode_count(s,&count)!=OPENFS_PATH_OK){if(!rollback_created_entry(d,s,parent,p,*out))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_CORRUPT;}
    openfs_inode_t in;
    if(read_inode(d,s,*out,&in)!=OPENFS_PATH_OK){if(!rollback_created_entry(d,s,parent,p,*out))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_IO_ERROR;}
    in.uid=uid;in.gid=gid;
    uint64_t now=openfs_time_now_ns();if(now!=UINT64_MAX)in.ctime_ns=now;
    if(openfs_inode_write(d,s->inode_table_start,count,&in)!=OPENFS_INODE_OK){if(!rollback_created_entry(d,s,parent,p,*out))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_IO_ERROR;}
    if(d->flush(d->context)!=OPENFS_IO_OK){if(!rollback_created_entry(d,s,parent,p,*out))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_IO_ERROR;}
    return OPENFS_PATH_OK;
}
static openfs_path_result_t sticky_allowed(const openfs_inode_t *parent,const openfs_inode_t *target,uint32_t uid)
{
    if((parent->mode&01000U)==0U)return OPENFS_PATH_OK;
    if(uid==0U||uid==parent->uid||uid==target->uid)return OPENFS_PATH_OK;
    return OPENFS_PATH_ACCESS_DENIED;
}
openfs_path_result_t openfs_path_unlink_as(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t uid,uint32_t gid)
{
    uint64_t parent=0U;openfs_path_result_t r=parent_access(d,s,p,uid,gid,&parent);if(r!=OPENFS_PATH_OK)return r;
    char pp[OPENFS_PATH_MAX],name[OPENFS_DIR_NAME_MAX+1U];r=split_last(p,pp,sizeof(pp),name,sizeof(name));if(r!=OPENFS_PATH_OK)return r;
    uint64_t target_ino=0U;r=openfs_path_lookup_as(d,s,p,uid,gid,&target_ino);if(r!=OPENFS_PATH_OK)return r;
    openfs_inode_t parent_i,target_i;openfs_path_result_t parent_result=read_inode(d,s,parent,&parent_i);if(parent_result!=OPENFS_PATH_OK)return parent_result;openfs_path_result_t target_result=read_inode(d,s,target_ino,&target_i);if(target_result!=OPENFS_PATH_OK)return target_result;
    r=sticky_allowed(&parent_i,&target_i,uid);if(r!=OPENFS_PATH_OK)return r;
    return openfs_path_unlink(d,s,p);
}
openfs_path_result_t openfs_path_rename_as(openfs_block_device_t*d,const openfs_superblock_t*s,const char*a,const char*b,uint32_t uid,uint32_t gid)
{
    uint64_t oldparent=0U,newparent=0U;openfs_path_result_t r=parent_access(d,s,a,uid,gid,&oldparent);if(r!=OPENFS_PATH_OK)return r;
    r=parent_access(d,s,b,uid,gid,&newparent);if(r!=OPENFS_PATH_OK)return r;
    uint64_t src_ino=0U;openfs_path_result_t sr=openfs_path_lookup_as(d,s,a,uid,gid,&src_ino);if(sr!=OPENFS_PATH_OK)return sr;
    openfs_inode_t old_parent_i,src_i;openfs_path_result_t old_parent_result=read_inode(d,s,oldparent,&old_parent_i);if(old_parent_result!=OPENFS_PATH_OK)return old_parent_result;openfs_path_result_t src_result=read_inode(d,s,src_ino,&src_i);if(src_result!=OPENFS_PATH_OK)return src_result;
    r=sticky_allowed(&old_parent_i,&src_i,uid);if(r!=OPENFS_PATH_OK)return r;
    uint64_t dst_ino=0U;
    if(openfs_path_lookup_as(d,s,b,uid,gid,&dst_ino)==OPENFS_PATH_OK){
        openfs_inode_t new_parent_i,dst_i;openfs_path_result_t new_parent_result=read_inode(d,s,newparent,&new_parent_i);if(new_parent_result!=OPENFS_PATH_OK)return new_parent_result;openfs_path_result_t dst_result=read_inode(d,s,dst_ino,&dst_i);if(dst_result!=OPENFS_PATH_OK)return dst_result;
        r=sticky_allowed(&new_parent_i,&dst_i,uid);if(r!=OPENFS_PATH_OK)return r;
    }
    return openfs_path_rename(d,s,a,b);
}

openfs_path_result_t openfs_path_chmod_as(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t permissions,uint32_t uid,uint32_t gid)
{
    uint64_t ino=0U;openfs_path_result_t lr=openfs_path_lookup(d,s,p,&ino);if(lr!=OPENFS_PATH_OK)return lr;
    openfs_inode_t in;openfs_path_result_t ir=read_inode(d,s,ino,&in);if(ir!=OPENFS_PATH_OK)return ir;
    if(uid!=in.uid && uid!=0U)return OPENFS_PATH_ACCESS_DENIED;
    (void)gid;
    if((permissions&~OPENFS_INODE_PERMISSION_MASK)!=0U)return OPENFS_PATH_INVALID_ARGUMENT;
    return openfs_path_chmod(d,s,p,permissions);
}
openfs_path_result_t openfs_path_set_times_as(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t uid,uint32_t gid,uint64_t atime_ns,uint64_t mtime_ns)
{
    uint64_t ino=0U;openfs_path_result_t lr=openfs_path_lookup(d,s,p,&ino);if(lr!=OPENFS_PATH_OK)return lr;
    openfs_inode_t in;if(read_inode(d,s,ino,&in)!=OPENFS_PATH_OK)return OPENFS_PATH_IO_ERROR;
    if(uid!=in.uid && uid!=0U)return OPENFS_PATH_ACCESS_DENIED;
    (void)gid;
    return openfs_path_set_times(d,s,p,atime_ns,mtime_ns);
}
openfs_path_result_t openfs_path_create_tx(openfs_transaction_t*t,const openfs_superblock_t*s,const char*p,uint32_t mode,uint64_t*out){if(t==NULL||s==NULL||p==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_block_device_t*d=openfs_transaction_device(t);if(d==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_path_result_t r=openfs_path_create(d,s,p,mode,out);if(r!=OPENFS_PATH_OK)t->failed=1;return r;}
openfs_path_result_t openfs_path_mkdir_tx(openfs_transaction_t*t,const openfs_superblock_t*s,const char*p,uint64_t*out){if(t==NULL||s==NULL||p==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_block_device_t*d=openfs_transaction_device(t);if(d==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_path_result_t r=openfs_path_mkdir(d,s,p,out);if(r!=OPENFS_PATH_OK)t->failed=1;return r;}
openfs_path_result_t openfs_path_unlink_tx(openfs_transaction_t*t,const openfs_superblock_t*s,const char*p){if(t==NULL||s==NULL||p==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_block_device_t*d=openfs_transaction_device(t);if(d==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_path_result_t r=openfs_path_unlink(d,s,p);if(r!=OPENFS_PATH_OK)t->failed=1;return r;}
openfs_path_result_t openfs_path_rename_tx(openfs_transaction_t*t,const openfs_superblock_t*s,const char*a,const char*b){if(t==NULL||s==NULL||a==NULL||b==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_block_device_t*d=openfs_transaction_device(t);if(d==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_path_result_t r=openfs_path_rename(d,s,a,b);if(r!=OPENFS_PATH_OK)t->failed=1;return r;}
