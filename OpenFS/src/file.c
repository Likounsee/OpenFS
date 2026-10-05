#include "openfs/file.h"
#include "openfs/bitmap.h"
#include "openfs/time.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (b > UINT64_MAX - a) {
        return 1;
    }
    *out = a + b;
    return 0;
}

static int mul_overflow_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a != 0U && b > UINT64_MAX / a) {
        return 1;
    }
    *out = a * b;
    return 0;
}

static uint64_t ceil_div_u64(uint64_t value, uint64_t divisor)
{
    return value == 0U ? 0U : 1U + (value - 1U) / divisor;
}

static openfs_file_result_t inode_table_count(
    const openfs_superblock_t *sb,
    uint64_t *count)
{
    uint64_t bytes = 0U;
    if (sb == NULL || count == NULL ||
        mul_overflow_u64(sb->inode_table_blocks, sb->block_size, &bytes)) {
        return OPENFS_FILE_CORRUPT;
    }
    *count = bytes / OPENFS_INODE_SIZE;
    return *count == 0U ? OPENFS_FILE_CORRUPT : OPENFS_FILE_OK;
}

static openfs_file_result_t load_all_extents(const openfs_block_device_t*,const openfs_superblock_t*,const openfs_inode_t*,openfs_extent_t**,uint32_t*);
static int validate_extent_set(const openfs_superblock_t*,const openfs_extent_t*,uint32_t);

static openfs_file_result_t validate_file(
    const openfs_block_device_t *device,
    const openfs_superblock_t *sb,
    const openfs_inode_t *inode)
{
    if (!openfs_block_device_is_valid(device) || sb == NULL || inode == NULL) {
        return OPENFS_FILE_INVALID_ARGUMENT;
    }
    if (sb->block_size == 0U || sb->block_size != device->block_size ||
        sb->data_blocks == 0U || sb->data_start > UINT64_MAX - sb->data_blocks ||
        sb->data_start + sb->data_blocks > device->block_count) {
        return OPENFS_FILE_CORRUPT;
    }
    uint32_t type = inode->mode & 0170000U;
    if (type != OPENFS_INODE_MODE_REGULAR &&
        type != OPENFS_INODE_MODE_DIRECTORY &&
        type != OPENFS_INODE_MODE_SYMLINK) {
        return OPENFS_FILE_INVALID_ARGUMENT;
    }
    if (inode->extent_count > OPENFS_INODE_INLINE_EXTENT_MAX &&
        (inode->flags & OPENFS_INODE_FLAG_EXTENT_TREE) == 0U) {
        return OPENFS_FILE_CORRUPT;
    }
    if ((inode->flags & OPENFS_INODE_FLAG_INLINE_DATA) != 0U) {
        if ((inode->flags & (OPENFS_INODE_FLAG_HAS_EXTENTS | OPENFS_INODE_FLAG_EXTENT_TREE)) != 0U ||
            (inode->mode & OPENFS_INODE_TYPE_MASK) != OPENFS_INODE_MODE_SYMLINK ||
            inode->blocks != 0U || inode->extent_count != 0U ||
            inode->size > sizeof(inode->inline_data)) {
            return OPENFS_FILE_CORRUPT;
        }
    } else {
        uint64_t required_blocks = inode->size == 0U ? 0U :
            1U + (inode->size - 1U) / (uint64_t)sb->block_size;
        if (required_blocks != inode->blocks) {
            return OPENFS_FILE_CORRUPT;
        }
    }
    if ((inode->blocks == 0U && inode->extent_count != 0U) ||
        (inode->blocks != 0U && inode->extent_count == 0U) ||
        ((inode->blocks == 0U) != ((inode->flags & OPENFS_INODE_FLAG_HAS_EXTENTS) == 0U))) {
        return OPENFS_FILE_CORRUPT;
    }
    if ((inode->flags & OPENFS_INODE_FLAG_EXTENT_TREE) != 0U) {
        uint64_t root = openfs_inode_get_extent_tree_root(inode);
        if (inode->extent_count <= OPENFS_INODE_TREE_INLINE_EXTENT_MAX || root == 0U ||
            root < sb->data_start || root >= sb->data_start + sb->data_blocks) {
            return OPENFS_FILE_CORRUPT;
        }
    }
    if ((inode->flags & OPENFS_INODE_FLAG_INLINE_DATA) == 0U) {
        openfs_extent_t *extents = NULL; uint32_t extent_count = 0U;
        openfs_file_result_t er = load_all_extents(device,sb,inode,&extents,&extent_count);
        if (er != OPENFS_FILE_OK) return er;
        int valid = validate_extent_set(sb,extents,extent_count);
        uint64_t logical_end = extent_count == 0U ? 0U : extents[extent_count-1U].logical_start + extents[extent_count-1U].block_count;
        if (!valid || logical_end != inode->blocks) { free(extents); return OPENFS_FILE_CORRUPT; }
        free(extents);
    }
    return OPENFS_FILE_OK;
}

openfs_file_result_t openfs_file_map_block(const openfs_inode_t *inode,uint64_t logical_block,uint64_t *physical_block)
{
    if(inode==NULL||physical_block==NULL||inode->blocks==0U||logical_block>=inode->blocks)return OPENFS_FILE_OUT_OF_RANGE;
    if(inode->extent_count>OPENFS_INODE_INLINE_EXTENT_MAX)return OPENFS_FILE_OUT_OF_RANGE;
    uint64_t previous=0U;
    for(uint32_t n=0U;n<inode->extent_count;n++){openfs_extent_t e;if(openfs_inode_get_extent(inode,n,&e)!=OPENFS_EXTENT_OK)return OPENFS_FILE_CORRUPT;if(e.block_count==0U||e.logical_start>UINT64_MAX-e.block_count||e.physical_start>UINT64_MAX-e.block_count)return OPENFS_FILE_CORRUPT;uint64_t end=e.logical_start+e.block_count;if(n>0U&&e.logical_start!=previous)return OPENFS_FILE_CORRUPT;for(uint32_t p=0U;p<n;p++){openfs_extent_t prior;if(openfs_inode_get_extent(inode,p,&prior)!=OPENFS_EXTENT_OK||prior.block_count==0U||prior.physical_start>UINT64_MAX-prior.block_count)return OPENFS_FILE_CORRUPT;uint64_t prior_end=prior.physical_start+prior.block_count;if(e.physical_start<prior_end&&prior.physical_start<e.physical_start+e.block_count)return OPENFS_FILE_CORRUPT;}if(logical_block>=e.logical_start&&logical_block<end){uint64_t delta=logical_block-e.logical_start;if(e.physical_start>UINT64_MAX-delta)return OPENFS_FILE_CORRUPT;*physical_block=e.physical_start+delta;return OPENFS_FILE_OK;}previous=end;}
    return OPENFS_FILE_OUT_OF_RANGE;
}

static openfs_file_result_t validate_physical_block(
    const openfs_superblock_t *sb,
    uint64_t physical)
{
    if (sb==NULL||sb->data_blocks==0U||sb->data_start > UINT64_MAX - sb->data_blocks) {
        return OPENFS_FILE_CORRUPT;
    }
    return (physical >= sb->data_start &&
            physical < sb->data_start + sb->data_blocks)
        ? OPENFS_FILE_OK
        : OPENFS_FILE_CORRUPT;
}

static openfs_file_result_t validate_allocated_block(
    const openfs_block_device_t *device,
    const openfs_superblock_t *sb,
    uint64_t physical)
{
    openfs_file_result_t r = validate_physical_block(sb, physical);
    if (r != OPENFS_FILE_OK) return r;
    if (sb->block_bitmap_blocks == 0U ||
        sb->block_bitmap_start >= device->block_count ||
        sb->block_bitmap_blocks > device->block_count - sb->block_bitmap_start) {
        return OPENFS_FILE_CORRUPT;
    }
    int allocated = 0;
    openfs_bitmap_result_t br = openfs_bitmap_test(
        device, sb->block_bitmap_start, sb->block_bitmap_blocks, physical, &allocated);
    if (br != OPENFS_BITMAP_OK) {
        return br == OPENFS_BITMAP_IO_ERROR ? OPENFS_FILE_IO_ERROR : OPENFS_FILE_CORRUPT;
    }
    return allocated ? OPENFS_FILE_OK : OPENFS_FILE_CORRUPT;
}

static openfs_file_result_t write_inode(
    openfs_block_device_t *device,
    const openfs_superblock_t *sb,
    const openfs_inode_t *inode)
{
    uint64_t count = 0U;
    openfs_file_result_t r = inode_table_count(sb, &count);
    if (r != OPENFS_FILE_OK) {
        return r;
    }
    openfs_inode_result_t ir=openfs_inode_write(device, sb->inode_table_start, count, inode);return ir==OPENFS_INODE_OK?OPENFS_FILE_OK:(ir==OPENFS_INODE_CORRUPT?OPENFS_FILE_CORRUPT:OPENFS_FILE_IO_ERROR);
}

static uint32_t inline_extent_count(const openfs_inode_t *inode)
{
    return (inode != NULL && (inode->flags & OPENFS_INODE_FLAG_EXTENT_TREE) != 0U)
        ? OPENFS_INODE_TREE_INLINE_EXTENT_MAX : OPENFS_INODE_INLINE_EXTENT_MAX;
}
static openfs_file_result_t load_all_extents(const openfs_block_device_t *d,const openfs_superblock_t *sb,const openfs_inode_t *inode,openfs_extent_t **out,uint32_t *count)
{
    if(d==NULL||inode==NULL||out==NULL||count==NULL)return OPENFS_FILE_INVALID_ARGUMENT;
    if(inode->extent_count>OPENFS_INODE_INLINE_EXTENT_MAX&&(inode->flags&OPENFS_INODE_FLAG_EXTENT_TREE)==0U)return OPENFS_FILE_CORRUPT;
    uint32_t n=inode->extent_count;if(n==0U){*out=NULL;*count=0U;return OPENFS_FILE_OK;}
    #if SIZE_MAX < UINT32_MAX
    if(n > SIZE_MAX/sizeof(openfs_extent_t))return OPENFS_FILE_CORRUPT;
    #endif
    openfs_extent_t*a=calloc((size_t)n,sizeof(*a));if(a==NULL)return OPENFS_FILE_IO_ERROR;
    uint32_t inline_max=inline_extent_count(inode);uint32_t inline_n=n<inline_max?n:inline_max;
    for(uint32_t i=0U;i<inline_n;i++){openfs_extent_result_t er=openfs_inode_get_extent(inode,i,&a[i]);if(er!=OPENFS_EXTENT_OK){free(a);return OPENFS_FILE_CORRUPT;}}
    for(uint32_t i=inline_max;i<n;i++){openfs_extent_result_t er=openfs_extent_tree_read(d,sb,inode,i-inline_max,&a[i]);if(er!=OPENFS_EXTENT_OK){free(a);return er==OPENFS_EXTENT_IO_ERROR?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;}}
    *out=a;*count=n;return OPENFS_FILE_OK;
}
static int validate_extent_set(const openfs_superblock_t *sb,const openfs_extent_t *a,uint32_t n)
{
    if(sb==NULL||(n!=0U&&a==NULL)||sb->data_blocks==0U||
       sb->data_start>UINT64_MAX-sb->data_blocks)return 0;
    uint64_t data_end=sb->data_start+sb->data_blocks;
    uint64_t logical_end=0U;
    for(uint32_t i=0U;i<n;i++){
        if(a[i].block_count==0U||
           a[i].logical_start>UINT64_MAX-a[i].block_count||
           a[i].physical_start>UINT64_MAX-a[i].block_count)return 0;
        if(i==0U){
            if(a[i].logical_start!=0U)return 0;
        }else if(a[i].logical_start!=logical_end){
            return 0;
        }
        uint64_t physical_end=a[i].physical_start+a[i].block_count;
        for(uint32_t p=0U;p<i;p++){
            uint64_t prior_end=a[p].physical_start+a[p].block_count;
            if(a[i].physical_start<prior_end&&a[p].physical_start<physical_end)return 0;
        }
        if(a[i].physical_start<sb->data_start||physical_end>data_end)return 0;
        logical_end=a[i].logical_start+a[i].block_count;
    }
    return 1;
}

static int physical_extents_overlap(const openfs_extent_t *a,const openfs_extent_t *b)
{
    uint64_t ae=a->physical_start+a->block_count,be=b->physical_start+b->block_count;
    return a->physical_start<be&&b->physical_start<ae;
}

static int extent_contains_block(const openfs_extent_t *e,uint64_t block)
{
    return e != NULL && e->block_count != 0U && e->physical_start <= block && block - e->physical_start < e->block_count;
}

static openfs_file_result_t store_all_extents(openfs_block_device_t*d,const openfs_superblock_t*sb,openfs_inode_t*inode,const openfs_extent_t*a,uint32_t n)
{
    if(d==NULL||sb==NULL||inode==NULL||(n!=0U&&a==NULL))return OPENFS_FILE_INVALID_ARGUMENT;
    if(!validate_extent_set(sb,a,n))return OPENFS_FILE_CORRUPT;
    uint32_t cap=openfs_extent_tree_capacity(d->block_size);uint64_t oldroot=openfs_inode_get_extent_tree_root(inode);
    if(oldroot!=0U && (oldroot<sb->data_start || oldroot>=sb->data_start+sb->data_blocks))return OPENFS_FILE_CORRUPT;
    if(oldroot!=0U){for(uint32_t i=0U;i<n;i++)if(extent_contains_block(&a[i],oldroot))return OPENFS_FILE_CORRUPT;}
    if(n>OPENFS_INODE_TREE_INLINE_EXTENT_MAX){
        if(cap==0U||n-OPENFS_INODE_TREE_INLINE_EXTENT_MAX>cap)return OPENFS_FILE_TOO_MANY_EXTENTS;
        uint64_t root=oldroot;int newroot=0;
        if(root==0U){if(openfs_alloc_block(d,sb,&root)!=OPENFS_ALLOC_OK)return OPENFS_FILE_NO_SPACE;newroot=1;}
        for(uint32_t i=0U;i<n;i++)if(extent_contains_block(&a[i],root)){if(newroot&&openfs_free_block(d,sb,root)!=OPENFS_ALLOC_OK)return OPENFS_FILE_CORRUPT;return OPENFS_FILE_CORRUPT;}
        openfs_inode_t tmp=*inode;if(openfs_inode_set_extent_tree_root(&tmp,root)!=OPENFS_EXTENT_OK){
            if(newroot&&openfs_free_block(d,sb,root)!=OPENFS_ALLOC_OK)return OPENFS_FILE_CORRUPT;
            return OPENFS_FILE_CORRUPT;
        }
        memset(tmp.reserved,0U,112U);
        tmp.extent_count=0U;
        for(uint32_t i=0U;i<OPENFS_INODE_TREE_INLINE_EXTENT_MAX;i++)if(openfs_inode_set_extent(&tmp,i,&a[i])!=OPENFS_EXTENT_OK){
            if(newroot&&openfs_free_block(d,sb,root)!=OPENFS_ALLOC_OK)return OPENFS_FILE_CORRUPT;
            return OPENFS_FILE_CORRUPT;
        }
        tmp.extent_count=n;tmp.flags|=OPENFS_INODE_FLAG_EXTENT_TREE|OPENFS_INODE_FLAG_HAS_EXTENTS;
        uint8_t *old_tree = NULL;
        if (!newroot) {
            old_tree = malloc(d->block_size);
            if (old_tree == NULL ||
                d->read(d->context, root, 1U, old_tree) != OPENFS_IO_OK) {
                free(old_tree);
                return OPENFS_FILE_IO_ERROR;
            }
        }
        openfs_extent_result_t tree_result =
            openfs_extent_tree_write(d,sb,&tmp,a+OPENFS_INODE_TREE_INLINE_EXTENT_MAX,
                                     n-OPENFS_INODE_TREE_INLINE_EXTENT_MAX);
        if (tree_result != OPENFS_EXTENT_OK) {
            int restored = newroot ||
                d->write(d->context, root, 1U, old_tree) == OPENFS_IO_OK;
            if (newroot) {
                if (openfs_free_block(d,sb,root) != OPENFS_ALLOC_OK) restored = 0;
            } else if (d->flush(d->context) != OPENFS_IO_OK) {
                restored = 0;
            }
            free(old_tree);
            return restored
                ? (tree_result == OPENFS_EXTENT_IO_ERROR
                    ? OPENFS_FILE_IO_ERROR : OPENFS_FILE_CORRUPT)
                : OPENFS_FILE_CORRUPT;
        }
        free(old_tree);
        *inode=tmp;return OPENFS_FILE_OK;
    }
    memset(inode->reserved,0U,120U);inode->extent_count=0U;
    for(uint32_t i=0U;i<n;i++)if(openfs_inode_set_extent(inode,i,&a[i])!=OPENFS_EXTENT_OK)return OPENFS_FILE_CORRUPT;
    inode->extent_count=n;
    /*
     * Once the extent set fits inline again, the tree root is no longer part
     * of the inode's live metadata.  Do not rewrite the old root with an
     * empty tree: the caller must free the old root only after the new inode
     * image has been durably published.
     */
    inode->flags&=~OPENFS_INODE_FLAG_EXTENT_TREE;
    if(n==0U)inode->flags&=~OPENFS_INODE_FLAG_HAS_EXTENTS;
    else inode->flags|=OPENFS_INODE_FLAG_HAS_EXTENTS;
    return OPENFS_FILE_OK;
}
static openfs_file_result_t map_block_on_disk(const openfs_block_device_t*d,const openfs_superblock_t*sb,const openfs_inode_t*inode,uint64_t logical,uint64_t*physical)
{
    if(!openfs_block_device_is_valid(d)||d->block_size==0U||sb==NULL||inode==NULL||physical==NULL)return OPENFS_FILE_INVALID_ARGUMENT;
    uint64_t root=openfs_inode_get_extent_tree_root(inode);uint32_t inline_max=inline_extent_count(inode);uint32_t inline_n=inode->extent_count<inline_max?inode->extent_count:inline_max;uint64_t previous=0U;
    for(uint32_t n=0U;n<inline_n;n++){openfs_extent_t e;if(openfs_inode_get_extent(inode,n,&e)!=OPENFS_EXTENT_OK)return OPENFS_FILE_CORRUPT;uint64_t end=0U;if(add_overflow_u64(e.logical_start,e.block_count,&end))return OPENFS_FILE_CORRUPT;if(e.physical_start>UINT64_MAX-e.block_count)return OPENFS_FILE_CORRUPT;if(root!=0U&&extent_contains_block(&e,root))return OPENFS_FILE_CORRUPT;for(uint32_t p=0U;p<n;p++){openfs_extent_t prior;if(openfs_inode_get_extent(inode,p,&prior)!=OPENFS_EXTENT_OK||physical_extents_overlap(&prior,&e))return OPENFS_FILE_CORRUPT;}if(e.logical_start!=previous)return OPENFS_FILE_CORRUPT;if(logical>=e.logical_start&&logical<end){uint64_t delta=logical-e.logical_start;if(e.physical_start>UINT64_MAX-delta)return OPENFS_FILE_CORRUPT;*physical=e.physical_start+delta;return OPENFS_FILE_OK;}previous=end;}
    if(inode->extent_count<=inline_max)return OPENFS_FILE_OUT_OF_RANGE;
    for(uint32_t n=inline_max;n<inode->extent_count;n++){openfs_extent_t e;openfs_extent_result_t er=openfs_extent_tree_read(d,sb,inode,n-inline_max,&e);if(er!=OPENFS_EXTENT_OK)return er==OPENFS_EXTENT_IO_ERROR?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;uint64_t end=0U;if(add_overflow_u64(e.logical_start,e.block_count,&end))return OPENFS_FILE_CORRUPT;if(e.physical_start>UINT64_MAX-e.block_count||e.logical_start!=previous)return OPENFS_FILE_CORRUPT;if(root!=0U&&extent_contains_block(&e,root))return OPENFS_FILE_CORRUPT;for(uint32_t p=0U;p<inline_n;p++){openfs_extent_t prior;if(openfs_inode_get_extent(inode,p,&prior)!=OPENFS_EXTENT_OK||physical_extents_overlap(&prior,&e))return OPENFS_FILE_CORRUPT;}for(uint32_t p=inline_max;p<n;p++){openfs_extent_t prior;openfs_extent_result_t per=openfs_extent_tree_read(d,sb,inode,p-inline_max,&prior);if(per!=OPENFS_EXTENT_OK)return per==OPENFS_EXTENT_IO_ERROR?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;if(physical_extents_overlap(&prior,&e))return OPENFS_FILE_CORRUPT;}if(logical>=e.logical_start&&logical<end){uint64_t delta=logical-e.logical_start;if(e.physical_start>UINT64_MAX-delta)return OPENFS_FILE_CORRUPT;*physical=e.physical_start+delta;return OPENFS_FILE_OK;}previous=end;}
    return OPENFS_FILE_OUT_OF_RANGE;
}
openfs_file_result_t openfs_file_map_block_device(const openfs_block_device_t*d,const openfs_superblock_t*sb,const openfs_inode_t*i,uint64_t logical,uint64_t*p){openfs_file_result_t r=map_block_on_disk(d,sb,i,logical,p);if(r!=OPENFS_FILE_OK)return r;r=validate_allocated_block(d,sb,*p);if(r!=OPENFS_FILE_OK)return r;return OPENFS_FILE_OK;}

static openfs_file_result_t append_block_to_inode(openfs_block_device_t *d,const openfs_superblock_t *sb,openfs_inode_t *inode,uint64_t physical)
{
    openfs_inode_t original=*inode;openfs_extent_t *a=NULL;uint32_t n=0U;
    openfs_file_result_t r=load_all_extents(d,sb,inode,&a,&n);if(r!=OPENFS_FILE_OK)return r;
    if(n!=0U){openfs_extent_t *last=&a[n-1U];uint64_t le=0U,pe=0U;if(add_overflow_u64(last->logical_start,last->block_count,&le)||add_overflow_u64(last->physical_start,last->block_count,&pe)){free(a);return OPENFS_FILE_CORRUPT;}if(le==inode->blocks&&pe==physical){last->block_count++;r=store_all_extents(d,sb,inode,a,n);free(a);if(r!=OPENFS_FILE_OK)*inode=original;return r;}}
    uint64_t max=(uint64_t)OPENFS_INODE_TREE_INLINE_EXTENT_MAX+(uint64_t)openfs_extent_tree_capacity(d->block_size);
    if((uint64_t)n>=max){free(a);return OPENFS_FILE_TOO_MANY_EXTENTS;}
    if(n==UINT32_MAX){free(a);return OPENFS_FILE_TOO_MANY_EXTENTS;}
    openfs_extent_t *b=realloc(a,(size_t)(n+1U)*sizeof(*b));if(b==NULL){free(a);return OPENFS_FILE_IO_ERROR;}a=b;a[n]=(openfs_extent_t){inode->blocks,physical,1U};n++;
    r=store_all_extents(d,sb,inode,a,n);free(a);if(r!=OPENFS_FILE_OK)*inode=original;return r;
}

static openfs_file_result_t allocate_blocks(
    openfs_block_device_t *device,
    const openfs_superblock_t *sb,    openfs_inode_t *inode,
    uint64_t target_blocks)
{
    while (inode->blocks < target_blocks) {        uint64_t physical = 0U;
        openfs_alloc_result_t ar = openfs_alloc_block(device, sb, &physical);
        if (ar == OPENFS_ALLOC_OUT_OF_SPACE) {
            return OPENFS_FILE_NO_SPACE;
        }
        if (ar != OPENFS_ALLOC_OK) {
            return ar == OPENFS_ALLOC_CORRUPT ? OPENFS_FILE_CORRUPT : OPENFS_FILE_IO_ERROR;
        }
        openfs_file_result_t r = append_block_to_inode(device, sb, inode, physical);
        if (r != OPENFS_FILE_OK) {
            if (openfs_free_block(device, sb, physical) != OPENFS_ALLOC_OK) {
                return OPENFS_FILE_CORRUPT;
            }
            return r;
        }
        inode->blocks++;
    }
    return OPENFS_FILE_OK;
}

static openfs_file_result_t rollback_blocks(openfs_block_device_t *device,const openfs_superblock_t *sb,openfs_inode_t *inode,uint64_t target_blocks,uint64_t original_root)
{
    uint64_t rollback_root=openfs_inode_get_extent_tree_root(inode);
    while(inode->blocks>target_blocks){
        openfs_extent_t*a=NULL;uint32_t n=0U;openfs_file_result_t r=load_all_extents(device,sb,inode,&a,&n);if(r!=OPENFS_FILE_OK)return r;
        if(n==0U){free(a);return OPENFS_FILE_CORRUPT;}
        openfs_extent_t*last=&a[n-1U];if(last->block_count==0U){free(a);return OPENFS_FILE_CORRUPT;}
        uint64_t physical=0U;
        if(last->physical_start>UINT64_MAX-(last->block_count-1U)){
            free(a);
            return OPENFS_FILE_CORRUPT;
        }
        physical=last->physical_start+last->block_count-1U;
        if(validate_physical_block(sb,physical)!=OPENFS_FILE_OK||
           openfs_free_block(device,sb,physical)!=OPENFS_ALLOC_OK){free(a);return OPENFS_FILE_CORRUPT;}
        last->block_count--;inode->blocks--;
        if(last->block_count==0U)n--;
        r=store_all_extents(device,sb,inode,a,n);free(a);if(r!=OPENFS_FILE_OK)return r;
    }
    if(inode->blocks==0U){inode->extent_count=0U;inode->flags&=~OPENFS_INODE_FLAG_HAS_EXTENTS;inode->flags&=~OPENFS_INODE_FLAG_EXTENT_TREE;}
    if(original_root==0U&&rollback_root!=0U){
        if(openfs_free_block(device,sb,rollback_root)!=OPENFS_ALLOC_OK)return OPENFS_FILE_CORRUPT;
    }
    return OPENFS_FILE_OK;
}

static openfs_file_result_t zero_block(
    openfs_block_device_t *device,
    uint64_t physical)
{
    if(device==NULL||!openfs_block_device_is_valid(device)||physical>=device->block_count)return OPENFS_FILE_INVALID_ARGUMENT;
    uint8_t *zero = calloc(1U, device->block_size);
    if (zero == NULL) {
        return OPENFS_FILE_IO_ERROR;
    }
    openfs_io_result_t io = device->write(device->context, physical, 1U, zero);
    free(zero);
    return io == OPENFS_IO_OK ? OPENFS_FILE_OK : OPENFS_FILE_IO_ERROR;
}

openfs_file_result_t openfs_file_truncate(
    openfs_block_device_t *device,
    const openfs_superblock_t *sb,
    openfs_inode_t *inode,
    uint64_t new_size)
{
    openfs_file_result_t r = validate_file(device, sb, inode);
    if (r != OPENFS_FILE_OK) return r;

    uint64_t old_size=inode->size;
    uint64_t old_blocks=ceil_div_u64(old_size,device->block_size);
    uint64_t new_blocks=ceil_div_u64(new_size,device->block_size);
    if(new_blocks>UINT64_MAX/device->block_size||new_blocks>sb->data_blocks)return OPENFS_FILE_OUT_OF_RANGE;

    openfs_inode_t original=*inode;
    uint8_t *tail_backup=NULL;
    uint64_t tail_physical=0U;
    int tail_saved=0;
    if(new_size>old_size&&old_size%device->block_size!=0U){
        if(map_block_on_disk(device,sb,&original,old_size/device->block_size,&tail_physical)!=OPENFS_FILE_OK||
           validate_physical_block(sb,tail_physical)!=OPENFS_FILE_OK)return OPENFS_FILE_CORRUPT;
        tail_backup=malloc(device->block_size);
        if(tail_backup==NULL)return OPENFS_FILE_IO_ERROR;
        if(device->read(device->context,tail_physical,1U,tail_backup)!=OPENFS_IO_OK){free(tail_backup);return OPENFS_FILE_IO_ERROR;}
        tail_saved=1;
    }

    if(new_blocks>inode->blocks){
        uint64_t before=inode->blocks;
        r=allocate_blocks(device,sb,inode,new_blocks);
        if(r!=OPENFS_FILE_OK){
            int ok=rollback_blocks(device,sb,inode,before,openfs_inode_get_extent_tree_root(&original))==OPENFS_FILE_OK;
            *inode=original;
            if(tail_saved&&device->write(device->context,tail_physical,1U,tail_backup)!=OPENFS_IO_OK)ok=0;
            free(tail_backup);
            return ok?r:OPENFS_FILE_CORRUPT;
        }
        for(uint64_t b=before;b<inode->blocks;b++){
            uint64_t physical=0U;
            if(map_block_on_disk(device,sb,inode,b,&physical)!=OPENFS_FILE_OK||
               zero_block(device,physical)!=OPENFS_FILE_OK){
                int ok=rollback_blocks(device,sb,inode,before,openfs_inode_get_extent_tree_root(&original))==OPENFS_FILE_OK;
                *inode=original;
                if(tail_saved&&device->write(device->context,tail_physical,1U,tail_backup)!=OPENFS_IO_OK)ok=0;
                free(tail_backup);
                return ok?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;
            }
        }
    }else if(new_blocks<inode->blocks){
        openfs_inode_t reduced=*inode;
        uint64_t removed=inode->blocks-new_blocks;
        if(removed>SIZE_MAX/sizeof(uint64_t)){free(tail_backup);return OPENFS_FILE_OUT_OF_RANGE;}
        uint64_t *freed=removed==0U?NULL:calloc((size_t)removed,sizeof(uint64_t));
        if(removed!=0U&&freed==NULL){free(tail_backup);return OPENFS_FILE_IO_ERROR;}
        for(uint64_t n=0U;n<removed;n++){
            if(map_block_on_disk(device,sb,&original,new_blocks+n,&freed[n])!=OPENFS_FILE_OK){free(freed);free(tail_backup);return OPENFS_FILE_CORRUPT;}
        }
        openfs_extent_t *extents=NULL;uint32_t extent_count=0U;
        r=load_all_extents(device,sb,&reduced,&extents,&extent_count);
        if(r!=OPENFS_FILE_OK){free(freed);free(tail_backup);return r;}
        while(reduced.blocks>new_blocks){
            if(extent_count==0U){free(extents);free(freed);free(tail_backup);return OPENFS_FILE_CORRUPT;}
            openfs_extent_t *last=&extents[extent_count-1U];
            uint64_t keep=new_blocks>last->logical_start?new_blocks-last->logical_start:0U;
            if(keep>last->block_count){free(extents);free(freed);free(tail_backup);return OPENFS_FILE_CORRUPT;}
            reduced.blocks-=last->block_count-keep;
            if(keep==0U)extent_count--;else last->block_count=keep;
        }
        uint64_t old_root=openfs_inode_get_extent_tree_root(&original);
        uint8_t *old_tree_block=NULL;
        if(old_root!=0U){
            old_tree_block=malloc(device->block_size);
            if(old_tree_block==NULL||device->read(device->context,old_root,1U,old_tree_block)!=OPENFS_IO_OK){
                free(old_tree_block);free(extents);free(freed);free(tail_backup);return OPENFS_FILE_IO_ERROR;
            }
        }
        r=store_all_extents(device,sb,&reduced,extents,extent_count);free(extents);
        if(r!=OPENFS_FILE_OK){
            int ok=1;
            if(old_tree_block!=NULL&&device->write(device->context,old_root,1U,old_tree_block)!=OPENFS_IO_OK)ok=0;
            if(ok&&device->flush(device->context)!=OPENFS_IO_OK)ok=0;
            free(old_tree_block);free(freed);free(tail_backup);*inode=original;
            return ok?r:OPENFS_FILE_CORRUPT;
        }
        reduced.size=new_size;uint64_t now=openfs_time_now_ns();if(now!=UINT64_MAX){reduced.mtime_ns=now;reduced.ctime_ns=now;}
        r=write_inode(device,sb,&reduced);
        if(r!=OPENFS_FILE_OK){
            int ok=1;
            if(old_tree_block!=NULL){if(device->write(device->context,old_root,1U,old_tree_block)!=OPENFS_IO_OK)ok=0;else if(device->flush(device->context)!=OPENFS_IO_OK)ok=0;}
            free(old_tree_block);free(freed);free(tail_backup);*inode=original;
            return ok?r:OPENFS_FILE_CORRUPT;
        }
        /*
         * Publish the reduced inode before releasing any blocks.  free_block()
         * flushes bitmap changes, so doing it first could leave the old inode
         * durable while its blocks were already reusable after a crash.
         */
        if(device->flush(device->context)!=OPENFS_IO_OK){
            int ok=1;
            if(old_tree_block!=NULL&&device->write(device->context,old_root,1U,old_tree_block)!=OPENFS_IO_OK)ok=0;
            if(write_inode(device,sb,&original)!=OPENFS_FILE_OK)ok=0;
            if(device->flush(device->context)!=OPENFS_IO_OK)ok=0;
            free(old_tree_block);free(freed);free(tail_backup);*inode=original;
            return ok?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;
        }
        int root_freed=0;
        for(uint64_t n=0U;n<removed;n++){
            if(openfs_free_block(device,sb,freed[n])!=OPENFS_ALLOC_OK){
                int ok=1;
                if(old_tree_block!=NULL&&device->write(device->context,old_root,1U,old_tree_block)!=OPENFS_IO_OK)ok=0;
                for(uint64_t k=0U;k<removed;k++)if(openfs_bitmap_set(device,sb->block_bitmap_start,sb->block_bitmap_blocks,freed[k],1)!=OPENFS_BITMAP_OK)ok=0;
                if(write_inode(device,sb,&original)!=OPENFS_FILE_OK)ok=0;
                if(device->flush(device->context)!=OPENFS_IO_OK)ok=0;
                free(old_tree_block);free(freed);free(tail_backup);*inode=original;
                return ok?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;
            }
        }
        uint64_t new_root=openfs_inode_get_extent_tree_root(&reduced);
        if(old_root!=0U&&new_root!=old_root){
            if(openfs_free_block(device,sb,old_root)!=OPENFS_ALLOC_OK){
                int ok=1;
                if(old_tree_block!=NULL&&device->write(device->context,old_root,1U,old_tree_block)!=OPENFS_IO_OK)ok=0;
                for(uint64_t k=0U;k<removed;k++)if(openfs_bitmap_set(device,sb->block_bitmap_start,sb->block_bitmap_blocks,freed[k],1)!=OPENFS_BITMAP_OK)ok=0;
                if(write_inode(device,sb,&original)!=OPENFS_FILE_OK)ok=0;
                if(device->flush(device->context)!=OPENFS_IO_OK)ok=0;
                free(old_tree_block);free(freed);free(tail_backup);*inode=original;
                return ok?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;
            }
            root_freed=1;
        }
        *inode=reduced;
        if(device->flush(device->context)!=OPENFS_IO_OK){
            int ok=1;
            for(uint64_t k=0U;k<removed;k++)if(openfs_bitmap_set(device,sb->block_bitmap_start,sb->block_bitmap_blocks,freed[k],1)!=OPENFS_BITMAP_OK)ok=0;
            if(root_freed&&openfs_bitmap_set(device,sb->block_bitmap_start,sb->block_bitmap_blocks,old_root,1)!=OPENFS_BITMAP_OK)ok=0;
            if(old_root!=0U&&new_root==old_root&&old_tree_block!=NULL&&device->write(device->context,old_root,1U,old_tree_block)!=OPENFS_IO_OK)ok=0;
            if(write_inode(device,sb,&original)!=OPENFS_FILE_OK)ok=0;
            if(device->flush(device->context)!=OPENFS_IO_OK)ok=0;
            *inode=original;
            free(old_tree_block);free(freed);free(tail_backup);
            return ok?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;
        }
        free(old_tree_block);free(freed);free(tail_backup);return OPENFS_FILE_OK;
    }

    if(tail_saved){
        uint8_t *block=malloc(device->block_size);
        if(block==NULL){free(tail_backup);return OPENFS_FILE_IO_ERROR;}
        memcpy(block,tail_backup,device->block_size);
        uint32_t from=(uint32_t)(old_size%device->block_size);
        uint32_t to=(new_blocks>old_blocks)?device->block_size:(uint32_t)(new_size%device->block_size);
        memset(block+from,0,(size_t)(to-from));
        if(device->write(device->context,tail_physical,1U,block)!=OPENFS_IO_OK){
            int ok=device->write(device->context,tail_physical,1U,tail_backup)==OPENFS_IO_OK;
            if(device->flush(device->context)!=OPENFS_IO_OK)ok=0;
            free(block);free(tail_backup);return ok?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;
        }
        free(block);
    }

    inode->size=new_size;
    uint64_t now=openfs_time_now_ns();
    if(now!=UINT64_MAX){inode->mtime_ns=now;inode->ctime_ns=now;}
    r=write_inode(device,sb,inode);
    if(r!=OPENFS_FILE_OK){
        int ok=1;
        if(new_blocks>original.blocks&&rollback_blocks(device,sb,inode,original.blocks,openfs_inode_get_extent_tree_root(&original))!=OPENFS_FILE_OK)ok=0;
        if(tail_saved&&device->write(device->context,tail_physical,1U,tail_backup)!=OPENFS_IO_OK)ok=0;
        *inode=original;free(tail_backup);
        return ok?r:OPENFS_FILE_CORRUPT;
    }
    if(device->flush(device->context)!=OPENFS_IO_OK){
        int ok=1;
        if(new_blocks>original.blocks&&rollback_blocks(device,sb,inode,original.blocks,openfs_inode_get_extent_tree_root(&original))!=OPENFS_FILE_OK)ok=0;
        if(tail_saved&&device->write(device->context,tail_physical,1U,tail_backup)!=OPENFS_IO_OK)ok=0;
        *inode=original;
        if(write_inode(device,sb,&original)!=OPENFS_FILE_OK)ok=0;
        if(device->flush(device->context)!=OPENFS_IO_OK)ok=0;
        free(tail_backup);
        return ok?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;
    }
    free(tail_backup);
    return OPENFS_FILE_OK;
}

openfs_file_result_t openfs_file_read(    const openfs_block_device_t *device,
    const openfs_superblock_t *sb,
    const openfs_inode_t *inode,
    uint64_t offset,
    void *buffer,
    size_t length,
    size_t *bytes_read)
{
    if (bytes_read == NULL) {
        return OPENFS_FILE_INVALID_ARGUMENT;
    }
    *bytes_read = 0U;
    openfs_file_result_t r = validate_file(device, sb, inode);
    if (r != OPENFS_FILE_OK || (length != 0U && buffer == NULL)) {
        return r != OPENFS_FILE_OK ? r : OPENFS_FILE_INVALID_ARGUMENT;
    }
    if (offset > inode->size) {
        return OPENFS_FILE_OUT_OF_RANGE;
    }
    uint64_t available = inode->size - offset;
    size_t wanted = length;
    if ((uint64_t)wanted > available) {
        wanted = (size_t)available;
    }
    uint8_t *block = malloc(device->block_size);
    if (block == NULL && wanted != 0U) {
        return OPENFS_FILE_IO_ERROR;
    }

    size_t done = 0U;
    while (done < wanted) {
        uint64_t absolute = offset + (uint64_t)done;
        uint64_t logical = absolute / device->block_size;
        uint32_t within = (uint32_t)(absolute % device->block_size);
        uint64_t physical = 0U;
        r = map_block_on_disk(device, sb, inode, logical, &physical);
        if (r != OPENFS_FILE_OK) {
            free(block);
            return r;
        }
        r = validate_allocated_block(device, sb, physical);
        if (r != OPENFS_FILE_OK) {
            free(block);
            return r;
        }
        if (device->read(device->context, physical, 1U, block) != OPENFS_IO_OK) {
            free(block);
            return OPENFS_FILE_IO_ERROR;
        }
        size_t chunk = device->block_size - within;
        if (chunk > wanted - done) {
            chunk = wanted - done;
        }
        memcpy((uint8_t *)buffer + done, block + within, chunk);
        done += chunk;
    }
    free(block);
    *bytes_read = done;
    return OPENFS_FILE_OK;
}

typedef struct {
    uint64_t logical;
    uint64_t physical;
    uint8_t *data;
} openfs_write_backup_t;

static void free_write_backups(openfs_write_backup_t *backups,uint64_t count)
{
    if(backups==NULL)return;
    for(uint64_t n=0U;n<count;n++)free(backups[n].data);
    free(backups);
}

static int restore_write_backups(openfs_block_device_t *device,const openfs_write_backup_t *backups,uint64_t count)
{
    for(uint64_t n=count;n>0U;n--){
        const openfs_write_backup_t *b=&backups[n-1U];
        if(device->write(device->context,b->physical,1U,b->data)!=OPENFS_IO_OK)return 0;
    }
    return 1;
}

openfs_file_result_t openfs_file_write(
    openfs_block_device_t *device,
    const openfs_superblock_t *sb,
    openfs_inode_t *inode,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    openfs_file_result_t r = validate_file(device, sb, inode);
    if (r != OPENFS_FILE_OK || (length != 0U && buffer == NULL)) {
        return r != OPENFS_FILE_OK ? r : OPENFS_FILE_INVALID_ARGUMENT;
    }
    if (length == 0U) {
        return OPENFS_FILE_OK;
    }
    if (offset > UINT64_MAX - (uint64_t)length) {
        return OPENFS_FILE_OUT_OF_RANGE;
    }

    uint64_t end = offset + (uint64_t)length;
    uint64_t target_blocks = ceil_div_u64(end, device->block_size);if(target_blocks>sb->data_blocks)return OPENFS_FILE_OUT_OF_RANGE;
    openfs_inode_t original = *inode;
    uint64_t old_blocks = inode->blocks;
    uint64_t old_size = inode->size;

    uint64_t first_logical = offset / device->block_size;
    uint64_t last_logical = (end - 1U) / device->block_size;
    uint64_t backup_count = 0U;
    if (first_logical < old_blocks) {
        uint64_t last_existing = last_logical < old_blocks ? last_logical : old_blocks - 1U;
        backup_count = last_existing - first_logical + 1U;
    }
    if (backup_count > SIZE_MAX / sizeof(openfs_write_backup_t)) {
        return OPENFS_FILE_OUT_OF_RANGE;
    }

    openfs_write_backup_t *backups = backup_count == 0U ? NULL :
        calloc((size_t)backup_count, sizeof(*backups));
    if (backup_count != 0U && backups == NULL) {
        return OPENFS_FILE_IO_ERROR;
    }

    for (uint64_t n=0U;n<backup_count;n++) {
        backups[n].logical=first_logical+n;
        if(map_block_on_disk(device,sb,&original,backups[n].logical,&backups[n].physical)!=OPENFS_FILE_OK ||
           validate_physical_block(sb,backups[n].physical)!=OPENFS_FILE_OK){
            free_write_backups(backups,backup_count);
            return OPENFS_FILE_CORRUPT;
        }
        backups[n].data=malloc(device->block_size);
        if(backups[n].data==NULL ||
           device->read(device->context,backups[n].physical,1U,backups[n].data)!=OPENFS_IO_OK){
            free_write_backups(backups,backup_count);
            return OPENFS_FILE_IO_ERROR;
        }
    }

    r = allocate_blocks(device, sb, inode, target_blocks);
    if (r != OPENFS_FILE_OK) {
        int rollback_ok = 1;
        if (inode->blocks > old_blocks) {
            if (rollback_blocks(device, sb, inode, old_blocks,
                    openfs_inode_get_extent_tree_root(&original)) != OPENFS_FILE_OK) {
                rollback_ok = 0;
            }
        }
        *inode = original;
        free_write_backups(backups,backup_count);
        return rollback_ok ? r : OPENFS_FILE_CORRUPT;
    }

    if (offset > old_size && first_logical > old_blocks) {
        for (uint64_t b=old_blocks;b<first_logical;b++) {
            uint64_t physical=0U;
            if (map_block_on_disk(device,sb,inode,b,&physical)!=OPENFS_FILE_OK ||
                zero_block(device,physical)!=OPENFS_FILE_OK) {
                int rollback_ok=rollback_blocks(device,sb,inode,old_blocks,
                    openfs_inode_get_extent_tree_root(&original))==OPENFS_FILE_OK;
                *inode=original;
                free_write_backups(backups,backup_count);
                return rollback_ok?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;
            }
        }
    }

    uint8_t *block = malloc(device->block_size);
    if (block == NULL) {
        int rollback_ok = rollback_blocks(device, sb, inode, old_blocks,
            openfs_inode_get_extent_tree_root(&original)) == OPENFS_FILE_OK;
        *inode = original;
        free_write_backups(backups,backup_count);
        return rollback_ok ? OPENFS_FILE_IO_ERROR : OPENFS_FILE_CORRUPT;
    }

    size_t done = 0U;
    while (done < length) {
        uint64_t absolute = offset + (uint64_t)done;
        uint64_t logical = absolute / device->block_size;
        uint32_t within = (uint32_t)(absolute % device->block_size);
        uint64_t physical = 0U;
        r = map_block_on_disk(device, sb, inode, logical, &physical);
        if (r != OPENFS_FILE_OK || validate_physical_block(sb, physical) != OPENFS_FILE_OK) {
            free(block);
            int rollback_ok = rollback_blocks(device, sb, inode, old_blocks,
                openfs_inode_get_extent_tree_root(&original)) == OPENFS_FILE_OK;
            *inode = original;
            free_write_backups(backups,backup_count);
            return rollback_ok ? (r == OPENFS_FILE_OK ? OPENFS_FILE_CORRUPT : r) : OPENFS_FILE_CORRUPT;
        }

        size_t chunk = device->block_size - within;
        if (chunk > length - done) {
            chunk = length - done;
        }

        if (within != 0U || chunk != device->block_size || (offset > old_size && logical == old_size / device->block_size)) {
            if (logical < old_blocks) {
                if (device->read(device->context, physical, 1U, block) != OPENFS_IO_OK) {
                    free(block);
                    int rollback_ok = rollback_blocks(device, sb, inode, old_blocks,
                        openfs_inode_get_extent_tree_root(&original)) == OPENFS_FILE_OK;
                    *inode = original;
                    free_write_backups(backups,backup_count);
                    return rollback_ok ? OPENFS_FILE_IO_ERROR : OPENFS_FILE_CORRUPT;
                }
            } else {
                memset(block, 0, device->block_size);
            }
        }
        if (offset > old_size && logical == old_size / device->block_size && old_size % device->block_size < within) {
            uint32_t gap_from = (uint32_t)(old_size % device->block_size);
            memset(block + gap_from, 0, (size_t)within - gap_from);
        }
        if (within == 0U && chunk == device->block_size) {
            memcpy(block, (const uint8_t *)buffer + done, chunk);
        } else {
            memcpy(block + within, (const uint8_t *)buffer + done, chunk);
        }
        if (device->write(device->context, physical, 1U, block) != OPENFS_IO_OK) {
            int restored=restore_write_backups(device,backups,backup_count);
            int rollback_ok = rollback_blocks(device, sb, inode, old_blocks,
                openfs_inode_get_extent_tree_root(&original)) == OPENFS_FILE_OK;
            *inode = original;
            free(block);
            free_write_backups(backups,backup_count);
            return (restored && rollback_ok) ? OPENFS_FILE_IO_ERROR : OPENFS_FILE_CORRUPT;
        }
        done += chunk;
    }
    free(block);

    if (end > inode->size) {
        inode->size = end;
    }
    uint64_t now = openfs_time_now_ns();
    if (now != UINT64_MAX) {
        inode->mtime_ns = now;
        inode->ctime_ns = now;
    }
    r = write_inode(device, sb, inode);
    if (r != OPENFS_FILE_OK) {
        int restored=restore_write_backups(device,backups,backup_count);
        int rollback_ok = rollback_blocks(device, sb, inode, old_blocks,
            openfs_inode_get_extent_tree_root(&original)) == OPENFS_FILE_OK;
        *inode = original;
        free_write_backups(backups,backup_count);
        return (restored && rollback_ok) ? r : OPENFS_FILE_CORRUPT;
    }
    if (device->flush(device->context) != OPENFS_IO_OK) {
        int ok = 1;
        if (!restore_write_backups(device, backups, backup_count)) ok = 0;
        if (rollback_blocks(device, sb, inode, old_blocks,
                openfs_inode_get_extent_tree_root(&original)) != OPENFS_FILE_OK) ok = 0;
        *inode = original;
        if (write_inode(device, sb, &original) != OPENFS_FILE_OK) ok = 0;
        if (device->flush(device->context) != OPENFS_IO_OK) ok = 0;
        free_write_backups(backups, backup_count);
        return ok ? OPENFS_FILE_IO_ERROR : OPENFS_FILE_CORRUPT;
    }
    free_write_backups(backups, backup_count);
    return OPENFS_FILE_OK;
}

openfs_file_result_t openfs_file_read_as(openfs_block_device_t*d,const openfs_superblock_t*s,const openfs_inode_t*i,uint32_t uid,uint32_t gid,uint64_t off,void*b,size_t len,size_t*got)
{
    openfs_file_result_t r=validate_file(d,s,i);if(r!=OPENFS_FILE_OK)return r;
    openfs_inode_result_t ar=openfs_inode_check_access(i,uid,gid,4U);
    if(ar!=OPENFS_INODE_OK)return ar==OPENFS_INODE_ACCESS_DENIED?OPENFS_FILE_ACCESS_DENIED:OPENFS_FILE_CORRUPT;
    return openfs_file_read(d,s,i,off,b,len,got);
}
openfs_file_result_t openfs_file_write_as(openfs_block_device_t*d,const openfs_superblock_t*s,openfs_inode_t*i,uint32_t uid,uint32_t gid,uint64_t off,const void*b,size_t len)
{
    openfs_file_result_t r=validate_file(d,s,i);if(r!=OPENFS_FILE_OK)return r;
    openfs_inode_result_t ar=openfs_inode_check_access(i,uid,gid,2U);
    if(ar!=OPENFS_INODE_OK)return ar==OPENFS_INODE_ACCESS_DENIED?OPENFS_FILE_ACCESS_DENIED:OPENFS_FILE_CORRUPT;
    return openfs_file_write(d,s,i,off,b,len);
}
openfs_file_result_t openfs_file_truncate_as(openfs_block_device_t*d,const openfs_superblock_t*s,openfs_inode_t*i,uint32_t uid,uint32_t gid,uint64_t size)
{
    openfs_file_result_t r=validate_file(d,s,i);if(r!=OPENFS_FILE_OK)return r;
    openfs_inode_result_t ar=openfs_inode_check_access(i,uid,gid,2U);
    if(ar!=OPENFS_INODE_OK)return ar==OPENFS_INODE_ACCESS_DENIED?OPENFS_FILE_ACCESS_DENIED:OPENFS_FILE_CORRUPT;
    return openfs_file_truncate(d,s,i,size);
}
openfs_file_result_t openfs_file_write_tx(openfs_transaction_t*t,const openfs_superblock_t*s,openfs_inode_t*i,uint64_t o,const void*b,size_t n){if(t==NULL||s==NULL||i==NULL)return OPENFS_FILE_INVALID_ARGUMENT;openfs_block_device_t*d=openfs_transaction_device(t);if(d==NULL)return OPENFS_FILE_INVALID_ARGUMENT;openfs_file_result_t r=openfs_file_write(d,s,i,o,b,n);if(r!=OPENFS_FILE_OK)t->failed=1;return r;}
openfs_file_result_t openfs_file_truncate_tx(openfs_transaction_t*t,const openfs_superblock_t*s,openfs_inode_t*i,uint64_t n){if(t==NULL||s==NULL||i==NULL)return OPENFS_FILE_INVALID_ARGUMENT;openfs_block_device_t*d=openfs_transaction_device(t);if(d==NULL)return OPENFS_FILE_INVALID_ARGUMENT;openfs_file_result_t r=openfs_file_truncate(d,s,i,n);if(r!=OPENFS_FILE_OK)t->failed=1;return r;}