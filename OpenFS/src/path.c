#include "openfs/path.h"
#include "openfs/bitmap.h"
#include "openfs/crc32c.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "openfs/time.h"
static int rollback_allocated_inode(openfs_block_device_t *d,const openfs_superblock_t *s,uint64_t ino);
static int restore_unlinked_inode_storage(openfs_block_device_t *d,const openfs_superblock_t *s,const openfs_inode_t *original,const uint8_t *root_backup)
{
    if (d == NULL || s == NULL || original == NULL || s->data_start > UINT64_MAX-s->data_blocks) return 0;
    uint64_t root = openfs_inode_get_extent_tree_root(original);
    if (root != 0U) {
        if (root < s->data_start || root >= s->data_start+s->data_blocks || root_backup == NULL ||
            d->write(d->context,root,1U,root_backup) != OPENFS_IO_OK) return 0;
    }
    uint64_t data_end=s->data_start+s->data_blocks;
    for (uint64_t logical = 0U; logical < original->blocks; ++logical) {
        uint64_t physical = 0U;
        if (openfs_file_map_block_device(d, s, original, logical, &physical) != OPENFS_FILE_OK || physical < s->data_start || physical >= data_end ||
            openfs_bitmap_set(d, s->block_bitmap_start, s->block_bitmap_blocks, physical, 1) != OPENFS_BITMAP_OK) {
            return 0;
        }
    }
    if (root != 0U && (root < s->data_start || root >= data_end ||
        openfs_bitmap_set(d, s->block_bitmap_start, s->block_bitmap_blocks, root, 1) != OPENFS_BITMAP_OK)) {
        return 0;
    }
    return d->flush(d->context) == OPENFS_IO_OK;
}

static int rollback_created_entry(openfs_block_device_t *d,const openfs_superblock_t *s,uint64_t parent,const char *path,uint64_t ino,const openfs_inode_t *original_parent)
{
    char pp[OPENFS_PATH_MAX],name[OPENFS_DIR_NAME_MAX+1U];
    if(original_parent==NULL||split_last(path,pp,sizeof(pp),name,sizeof(name))!=OPENFS_PATH_OK)return 0;
    openfs_inode_t pi=*original_parent;
    uint64_t count=0U;
    if(inode_count(s,&count)!=OPENFS_PATH_OK)return 0;
    if(read_inode(d,s,parent,&pi)!=OPENFS_PATH_OK)return 0;
    if(openfs_dir_remove(d,s,&pi,name)!=OPENFS_DIR_OK)return 0;
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
    if(!restore_directory_state(d,s,&pi,original_parent,count))return 0;
    return d->flush(d->context)==OPENFS_IO_OK;
}
openfs_path_result_t openfs_path_create_as(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t mode,uint32_t uid,uint32_t gid,uint64_t*out)
{
    uint64_t parent=0U;openfs_path_result_t r=parent_access(d,s,p,uid,gid,&parent);
    if(r!=OPENFS_PATH_OK)return r;
    openfs_inode_t original_parent;if(read_inode(d,s,parent,&original_parent)!=OPENFS_PATH_OK)return OPENFS_PATH_CORRUPT;
    r=openfs_path_create(d,s,p,mode,out);
    if(r!=OPENFS_PATH_OK)return r;
    uint64_t count=0U;
    if(inode_count(s,&count)!=OPENFS_PATH_OK){if(!rollback_created_entry(d,s,parent,p,*out,&original_parent))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_CORRUPT;}
    openfs_inode_t in;
    if(read_inode(d,s,*out,&in)!=OPENFS_PATH_OK){if(!rollback_created_entry(d,s,parent,p,*out,&original_parent))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_IO_ERROR;}
    in.uid=uid;in.gid=gid;
    uint64_t now=openfs_time_now_ns();if(now!=UINT64_MAX)in.ctime_ns=now;
    if(openfs_inode_write(d,s->inode_table_start,count,&in)!=OPENFS_INODE_OK){if(!rollback_created_entry(d,s,parent,p,*out,&original_parent))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_IO_ERROR;}
    if(d->flush(d->context)!=OPENFS_IO_OK){if(!rollback_created_entry(d,s,parent,p,*out,&original_parent))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_IO_ERROR;}
    return OPENFS_PATH_OK;
}
openfs_path_result_t openfs_path_mkdir_as(openfs_block_device_t*d,const openfs_superblock_t*s,const char*p,uint32_t uid,uint32_t gid,uint64_t*out)
{
    uint64_t parent=0U;openfs_path_result_t r=parent_access(d,s,p,uid,gid,&parent);
    if(r!=OPENFS_PATH_OK)return r;
    openfs_inode_t original_parent;if(read_inode(d,s,parent,&original_parent)!=OPENFS_PATH_OK)return OPENFS_PATH_CORRUPT;
    r=openfs_path_mkdir(d,s,p,out);
    if(r!=OPENFS_PATH_OK)return r;
    uint64_t count=0U;
    if(inode_count(s,&count)!=OPENFS_PATH_OK){if(!rollback_created_entry(d,s,parent,p,*out,&original_parent))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_CORRUPT;}
    openfs_inode_t in;
    if(read_inode(d,s,*out,&in)!=OPENFS_PATH_OK){if(!rollback_created_entry(d,s,parent,p,*out,&original_parent))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_IO_ERROR;}
    in.uid=uid;in.gid=gid;
    uint64_t now=openfs_time_now_ns();if(now!=UINT64_MAX)in.ctime_ns=now;
    if(openfs_inode_write(d,s->inode_table_start,count,&in)!=OPENFS_INODE_OK){if(!rollback_created_entry(d,s,parent,p,*out,&original_parent))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_IO_ERROR;}
    if(d->flush(d->context)!=OPENFS_IO_OK){if(!rollback_created_entry(d,s,parent,p,*out,&original_parent))return OPENFS_PATH_CORRUPT;return OPENFS_PATH_IO_ERROR;}
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
    openfs_inode_t in;openfs_path_result_t ir=read_inode(d,s,ino,&in);if(ir!=OPENFS_PATH_OK)return ir;
    if(uid!=in.uid && uid!=0U)return OPENFS_PATH_ACCESS_DENIED;
    (void)gid;
    return openfs_path_set_times(d,s,p,atime_ns,mtime_ns);
}
openfs_path_result_t openfs_path_create_tx(openfs_transaction_t*t,const openfs_superblock_t*s,const char*p,uint32_t mode,uint64_t*out){if(t==NULL||s==NULL||p==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_block_device_t*d=openfs_transaction_device(t);if(d==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_path_result_t r=openfs_path_create(d,s,p,mode,out);if(r!=OPENFS_PATH_OK)t->failed=1;return r;}
openfs_path_result_t openfs_path_mkdir_tx(openfs_transaction_t*t,const openfs_superblock_t*s,const char*p,uint64_t*out){if(t==NULL||s==NULL||p==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_block_device_t*d=openfs_transaction_device(t);if(d==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_path_result_t r=openfs_path_mkdir(d,s,p,out);if(r!=OPENFS_PATH_OK)t->failed=1;return r;}
openfs_path_result_t openfs_path_unlink_tx(openfs_transaction_t*t,const openfs_superblock_t*s,const char*p){if(t==NULL||s==NULL||p==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_block_device_t*d=openfs_transaction_device(t);if(d==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_path_result_t r=openfs_path_unlink(d,s,p);if(r!=OPENFS_PATH_OK)t->failed=1;return r;}
openfs_path_result_t openfs_path_rename_tx(openfs_transaction_t*t,const openfs_superblock_t*s,const char*a,const char*b){if(t==NULL||s==NULL||a==NULL||b==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_block_device_t*d=openfs_transaction_device(t);if(d==NULL)return OPENFS_PATH_INVALID_ARGUMENT;openfs_path_result_t r=openfs_path_rename(d,s,a,b);if(r!=OPENFS_PATH_OK)t->failed=1;return r;}
