#include "openfs/file.h"

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

static openfs_file_result_t validate_file(
    const openfs_block_device_t *device,
    const openfs_superblock_t *sb,
    const openfs_inode_t *inode)
{
    if (!openfs_block_device_is_valid(device) || sb == NULL || inode == NULL) {
        return OPENFS_FILE_INVALID_ARGUMENT;
    }
    if (sb->block_size != device->block_size ||
        sb->data_start > UINT64_MAX - sb->data_blocks ||
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
    return OPENFS_FILE_OK;
}

openfs_file_result_t openfs_file_map_block(const openfs_inode_t *inode,uint64_t logical_block,uint64_t *physical_block)
{
    if(inode==NULL||physical_block==NULL)return OPENFS_FILE_INVALID_ARGUMENT;
    if(inode->extent_count>OPENFS_INODE_INLINE_EXTENT_MAX)return OPENFS_FILE_OUT_OF_RANGE;
    uint64_t previous=0U;
    for(uint32_t n=0U;n<inode->extent_count;n++){openfs_extent_t e;if(openfs_inode_get_extent(inode,n,&e)!=OPENFS_EXTENT_OK)return OPENFS_FILE_CORRUPT;uint64_t end=0U;if(add_overflow_u64(e.logical_start,e.block_count,&end))return OPENFS_FILE_CORRUPT;if(n>0U&&e.logical_start<previous)return OPENFS_FILE_CORRUPT;if(logical_block>=e.logical_start&&logical_block<end){uint64_t delta=logical_block-e.logical_start;if(e.physical_start>UINT64_MAX-delta)return OPENFS_FILE_CORRUPT;*physical_block=e.physical_start+delta;return OPENFS_FILE_OK;}previous=end;}
    return OPENFS_FILE_OUT_OF_RANGE;
}

static openfs_file_result_t validate_physical_block(
    const openfs_superblock_t *sb,
    uint64_t physical)
{
    if (sb->data_start > UINT64_MAX - sb->data_blocks) {
        return OPENFS_FILE_CORRUPT;
    }
    return (physical >= sb->data_start &&
            physical < sb->data_start + sb->data_blocks)
        ? OPENFS_FILE_OK
        : OPENFS_FILE_CORRUPT;
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
    return openfs_inode_write(device, sb->inode_table_start, count, inode) ==
        OPENFS_INODE_OK ? OPENFS_FILE_OK : OPENFS_FILE_IO_ERROR;
}

static openfs_file_result_t load_all_extents(const openfs_block_device_t *d,const openfs_inode_t *inode,openfs_extent_t **out,uint32_t *count)
{
    if(d==NULL||inode==NULL||out==NULL||count==NULL)return OPENFS_FILE_INVALID_ARGUMENT;
    if(inode->extent_count>OPENFS_INODE_INLINE_EXTENT_MAX&&(inode->flags&OPENFS_INODE_FLAG_EXTENT_TREE)==0U)return OPENFS_FILE_CORRUPT;
    uint32_t n=inode->extent_count;if(n==0U){*out=NULL;*count=0U;return OPENFS_FILE_OK;}
    if((uint64_t)n>SIZE_MAX/sizeof(openfs_extent_t))return OPENFS_FILE_CORRUPT;
    openfs_extent_t*a=calloc((size_t)n,sizeof(*a));if(a==NULL)return OPENFS_FILE_IO_ERROR;
    uint32_t inline_n=n<OPENFS_INODE_INLINE_EXTENT_MAX?n:OPENFS_INODE_INLINE_EXTENT_MAX;
    for(uint32_t i=0U;i<inline_n;i++)if(openfs_inode_get_extent(inode,i,&a[i])!=OPENFS_EXTENT_OK){free(a);return OPENFS_FILE_CORRUPT;}
    for(uint32_t i=OPENFS_INODE_INLINE_EXTENT_MAX;i<n;i++)if(openfs_extent_tree_read(d,inode,i-OPENFS_INODE_INLINE_EXTENT_MAX,&a[i])!=OPENFS_EXTENT_OK){free(a);return OPENFS_FILE_CORRUPT;}
    *out=a;*count=n;return OPENFS_FILE_OK;
}
static openfs_file_result_t store_all_extents(openfs_block_device_t*d,const openfs_superblock_t*sb,openfs_inode_t*inode,const openfs_extent_t*a,uint32_t n)
{
    if(d==NULL||sb==NULL||inode==NULL||(n!=0U&&a==NULL))return OPENFS_FILE_INVALID_ARGUMENT;
    uint32_t cap=openfs_extent_tree_capacity(d->block_size);uint64_t oldroot=openfs_inode_get_extent_tree_root(inode);
    if(n>OPENFS_INODE_INLINE_EXTENT_MAX){
        if(cap==0U||n-OPENFS_INODE_INLINE_EXTENT_MAX>cap)return OPENFS_FILE_TOO_MANY_EXTENTS;
        uint64_t root=oldroot;int newroot=0;
        if(root==0U){if(openfs_alloc_block(d,sb,&root)!=OPENFS_ALLOC_OK)return OPENFS_FILE_NO_SPACE;newroot=1;}
        openfs_inode_t tmp=*inode;if(openfs_inode_set_extent_tree_root(&tmp,root)!=OPENFS_EXTENT_OK){if(newroot)(void)openfs_free_block(d,sb,root);return OPENFS_FILE_CORRUPT;}
        memset(tmp.reserved,0U,120U);
        for(uint32_t i=0U;i<OPENFS_INODE_INLINE_EXTENT_MAX;i++)if(openfs_inode_set_extent(&tmp,i,&a[i])!=OPENFS_EXTENT_OK){if(newroot)(void)openfs_free_block(d,sb,root);return OPENFS_FILE_CORRUPT;}
        tmp.extent_count=n;tmp.flags|=OPENFS_INODE_FLAG_EXTENT_TREE;
        if(openfs_extent_tree_write(d,&tmp,a+OPENFS_INODE_INLINE_EXTENT_MAX,n-OPENFS_INODE_INLINE_EXTENT_MAX)!=OPENFS_EXTENT_OK){if(newroot)(void)openfs_free_block(d,sb,root);return OPENFS_FILE_IO_ERROR;}
        *inode=tmp;return OPENFS_FILE_OK;
    }
    memset(inode->reserved,0U,120U);inode->extent_count=n;
    for(uint32_t i=0U;i<n;i++)if(openfs_inode_set_extent(inode,i,&a[i])!=OPENFS_EXTENT_OK)return OPENFS_FILE_CORRUPT;
    if(oldroot!=0U){
        inode->flags|=OPENFS_INODE_FLAG_EXTENT_TREE;inode->flags|=OPENFS_INODE_FLAG_HAS_EXTENTS;
        if(openfs_inode_set_extent_tree_root(inode,oldroot)!=OPENFS_EXTENT_OK)return OPENFS_FILE_CORRUPT;
        if(openfs_extent_tree_write(d,inode,NULL,0U)!=OPENFS_EXTENT_OK)return OPENFS_FILE_IO_ERROR;
    }else if(n==0U){
        inode->flags&=~OPENFS_INODE_FLAG_EXTENT_TREE;inode->flags&=~OPENFS_INODE_FLAG_HAS_EXTENTS;
    }
    return OPENFS_FILE_OK;
}
static openfs_file_result_t map_block_on_disk(const openfs_block_device_t*d,const openfs_superblock_t*sb,const openfs_inode_t*inode,uint64_t logical,uint64_t*physical)
{
    if(!openfs_block_device_is_valid(d)||sb==NULL||inode==NULL||physical==NULL)return OPENFS_FILE_INVALID_ARGUMENT;
    uint32_t inline_n=inode->extent_count<OPENFS_INODE_INLINE_EXTENT_MAX?inode->extent_count:OPENFS_INODE_INLINE_EXTENT_MAX;uint64_t previous=0U;
    for(uint32_t n=0U;n<inline_n;n++){openfs_extent_t e;if(openfs_inode_get_extent(inode,n,&e)!=OPENFS_EXTENT_OK)return OPENFS_FILE_CORRUPT;uint64_t end=0U;if(add_overflow_u64(e.logical_start,e.block_count,&end))return OPENFS_FILE_CORRUPT;if(n>0U&&e.logical_start<previous)return OPENFS_FILE_CORRUPT;if(logical>=e.logical_start&&logical<end){uint64_t delta=logical-e.logical_start;if(e.physical_start>UINT64_MAX-delta)return OPENFS_FILE_CORRUPT;*physical=e.physical_start+delta;return OPENFS_FILE_OK;}previous=end;}
    if(inode->extent_count<=OPENFS_INODE_INLINE_EXTENT_MAX)return OPENFS_FILE_OUT_OF_RANGE;
    for(uint32_t n=OPENFS_INODE_INLINE_EXTENT_MAX;n<inode->extent_count;n++){openfs_extent_t e;if(openfs_extent_tree_read(d,inode,n-OPENFS_INODE_INLINE_EXTENT_MAX,&e)!=OPENFS_EXTENT_OK)return OPENFS_FILE_CORRUPT;uint64_t end=0U;if(add_overflow_u64(e.logical_start,e.block_count,&end))return OPENFS_FILE_CORRUPT;if(e.logical_start<previous)return OPENFS_FILE_CORRUPT;if(logical>=e.logical_start&&logical<end){uint64_t delta=logical-e.logical_start;if(e.physical_start>UINT64_MAX-delta)return OPENFS_FILE_CORRUPT;*physical=e.physical_start+delta;return OPENFS_FILE_OK;}previous=end;}
    return OPENFS_FILE_OUT_OF_RANGE;
}
openfs_file_result_t openfs_file_map_block_device(const openfs_block_device_t*d,const openfs_superblock_t*sb,const openfs_inode_t*i,uint64_t logical,uint64_t*p){return map_block_on_disk(d,sb,i,logical,p);}

static openfs_file_result_t append_block_to_inode(openfs_block_device_t *d,const openfs_superblock_t *sb,openfs_inode_t *inode,uint64_t physical)
{
    openfs_inode_t original=*inode;openfs_extent_t *a=NULL;uint32_t n=0U;
    openfs_file_result_t r=load_all_extents(d,inode,&a,&n);if(r!=OPENFS_FILE_OK)return r;
    if(n!=0U){openfs_extent_t *last=&a[n-1U];uint64_t le=0U,pe=0U;if(add_overflow_u64(last->logical_start,last->block_count,&le)||add_overflow_u64(last->physical_start,last->block_count,&pe)){free(a);return OPENFS_FILE_CORRUPT;}if(le==inode->blocks&&pe==physical){last->block_count++;r=store_all_extents(d,sb,inode,a,n);free(a);if(r!=OPENFS_FILE_OK)*inode=original;return r;}}
    uint64_t max=(uint64_t)OPENFS_INODE_INLINE_EXTENT_MAX+(uint64_t)openfs_extent_tree_capacity(d->block_size);
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
            (void)openfs_free_block(device, sb, physical);
            return r;
        }
        inode->blocks++;
    }
    return OPENFS_FILE_OK;
}

static openfs_file_result_t rollback_blocks(openfs_block_device_t *device,const openfs_superblock_t *sb,openfs_inode_t *inode,uint64_t target_blocks)
{
    while(inode->blocks>target_blocks){
        openfs_extent_t*a=NULL;uint32_t n=0U;openfs_file_result_t r=load_all_extents(device,inode,&a,&n);if(r!=OPENFS_FILE_OK)return r;
        if(n==0U){free(a);return OPENFS_FILE_CORRUPT;}
        openfs_extent_t*last=&a[n-1U];if(last->block_count==0U){free(a);return OPENFS_FILE_CORRUPT;}
        uint64_t physical=last->physical_start+last->block_count-1U;
        if(openfs_free_block(device,sb,physical)!=OPENFS_ALLOC_OK){free(a);return OPENFS_FILE_CORRUPT;}
        last->block_count--;inode->blocks--;
        if(last->block_count==0U)n--;
        r=store_all_extents(device,sb,inode,a,n);free(a);if(r!=OPENFS_FILE_OK)return r;
    }
    if(inode->blocks==0U){inode->extent_count=0U;inode->flags&=~OPENFS_INODE_FLAG_HAS_EXTENTS;inode->flags&=~OPENFS_INODE_FLAG_EXTENT_TREE;}
    return OPENFS_FILE_OK;
}

static openfs_file_result_t zero_block(
    openfs_block_device_t *device,
    uint64_t physical)
{
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
    if (r != OPENFS_FILE_OK) {
        return r;
    }

    uint64_t old_size = inode->size;
    uint64_t old_blocks = ceil_div_u64(old_size, device->block_size);
    uint64_t new_blocks = ceil_div_u64(new_size, device->block_size);

    if (new_blocks > UINT64_MAX / device->block_size) {
        return OPENFS_FILE_OUT_OF_RANGE;
    }

    if (new_blocks > inode->blocks) {
        openfs_inode_t original = *inode;
        uint64_t before = inode->blocks;
        r = allocate_blocks(device, sb, inode, new_blocks);
        if (r != OPENFS_FILE_OK) {
            while (inode->blocks > before) {
                openfs_extent_t last;
                if (openfs_inode_get_extent(inode, inode->extent_count - 1U, &last) != OPENFS_EXTENT_OK) {
                    return OPENFS_FILE_CORRUPT;
                }
                uint64_t physical = last.physical_start + last.block_count - 1U;
                if (openfs_free_block(device, sb, physical) != OPENFS_ALLOC_OK) {
                    return OPENFS_FILE_CORRUPT;
                }
                last.block_count--;
                inode->blocks--;
                if (last.block_count == 0U) {
                    inode->extent_count--;
                } else {
                    (void)openfs_inode_set_extent(inode, inode->extent_count - 1U, &last);
                }
            }
            return r;
        }
        for (uint64_t b = before; b < inode->blocks; ++b) {
            uint64_t physical = 0U;
            if (map_block_on_disk(device, sb, inode, b, &physical) != OPENFS_FILE_OK ||
                zero_block(device, physical) != OPENFS_FILE_OK) {
                (void)rollback_blocks(device, sb, inode, before);
                *inode = original;
                return OPENFS_FILE_IO_ERROR;
            }
        }
    } else if (new_blocks < inode->blocks) {
        openfs_inode_t original = *inode;
        openfs_inode_t reduced = *inode;
        while (reduced.blocks > new_blocks) {
            openfs_extent_t last;
            if (reduced.extent_count == 0U ||
                openfs_inode_get_extent(&reduced, reduced.extent_count - 1U, &last) != OPENFS_EXTENT_OK) {
                return OPENFS_FILE_CORRUPT;
            }
            uint64_t keep = new_blocks > last.logical_start ? new_blocks - last.logical_start : 0U;
            if (keep > last.block_count) {
                return OPENFS_FILE_CORRUPT;
            }
            reduced.blocks -= last.block_count - keep;
            if (keep == 0U) {
                reduced.extent_count--;
            } else {
                last.block_count = keep;
                if (openfs_inode_set_extent(&reduced, reduced.extent_count - 1U, &last) != OPENFS_EXTENT_OK) {
                    return OPENFS_FILE_CORRUPT;
                }
            }
        }
        reduced.size = new_size;
        r = write_inode(device, sb, &reduced);
        if (r != OPENFS_FILE_OK) {
            *inode = original;
            return r;
        }
        for (uint64_t logical = new_blocks; logical < original.blocks; ++logical) {
            uint64_t physical = 0U;
            if (openfs_file_map_block(&original, logical, &physical) != OPENFS_FILE_OK ||
                openfs_free_block(device, sb, physical) != OPENFS_ALLOC_OK) {
                *inode = reduced;
                return OPENFS_FILE_IO_ERROR;
            }
        }
        *inode = reduced;
        return OPENFS_FILE_OK;
    }

    if (new_size > old_size && old_size % device->block_size != 0U) {
        uint64_t physical = 0U;
        if (map_block_on_disk(device, sb, inode, old_size / device->block_size, &physical) != OPENFS_FILE_OK ||
            validate_physical_block(sb, physical) != OPENFS_FILE_OK) {
            return OPENFS_FILE_CORRUPT;
        }
        uint8_t *block = calloc(1U, device->block_size);
        if (block == NULL) return OPENFS_FILE_IO_ERROR;
        if (device->read(device->context, physical, 1U, block) != OPENFS_IO_OK) {
            free(block);
            return OPENFS_FILE_IO_ERROR;
        }
        uint32_t from = (uint32_t)(old_size % device->block_size);
        uint32_t to = (new_blocks > old_blocks) ? device->block_size : (uint32_t)(new_size % device->block_size);
        memset(block + from, 0, (size_t)(to - from));
        if (device->write(device->context, physical, 1U, block) != OPENFS_IO_OK) {
            free(block);
            return OPENFS_FILE_IO_ERROR;
        }
        free(block);
    }
    inode->size = new_size;
    return write_inode(device, sb, inode);
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
        r = validate_physical_block(sb, physical);
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
    uint64_t target_blocks = ceil_div_u64(end, device->block_size);
    openfs_inode_t original = *inode;
    uint64_t old_blocks = inode->blocks;
    uint64_t old_size = inode->size;
    r = allocate_blocks(device, sb, inode, target_blocks);
    if (r != OPENFS_FILE_OK) {
        return r;
    }

    uint8_t *block = malloc(device->block_size);
    if (block == NULL) {
        (void)rollback_blocks(device, sb, inode, old_blocks);
        *inode = original;
        return OPENFS_FILE_IO_ERROR;
    }

    size_t done = 0U;
    while (done < length) {
        uint64_t absolute = offset + (uint64_t)done;
        uint64_t logical = absolute / device->block_size;
        uint32_t within = (uint32_t)(absolute % device->block_size);
        uint64_t physical = 0U;
        r = openfs_file_map_block(inode, logical, &physical);
        if (r != OPENFS_FILE_OK || validate_physical_block(sb, physical) != OPENFS_FILE_OK) {
            free(block);
            (void)rollback_blocks(device, sb, inode, old_blocks);
            *inode = original;
            return r == OPENFS_FILE_OK ? OPENFS_FILE_CORRUPT : r;
        }

        size_t chunk = device->block_size - within;
        if (chunk > length - done) {
            chunk = length - done;
        }

        if (within != 0U || chunk != device->block_size || (offset > old_size && logical == old_size / device->block_size)) {
            if (logical < old_blocks) {
                if (device->read(device->context, physical, 1U, block) != OPENFS_IO_OK) {
                    free(block);
                    (void)rollback_blocks(device, sb, inode, old_blocks);
                    *inode = original;
                    return OPENFS_FILE_IO_ERROR;
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
            free(block);
            (void)rollback_blocks(device, sb, inode, old_blocks);
            *inode = original;
            return OPENFS_FILE_IO_ERROR;
        }
        done += chunk;
    }
    free(block);

    if (end > inode->size) {
        inode->size = end;
    }
    r = write_inode(device, sb, inode);
    if (r != OPENFS_FILE_OK) {
        (void)rollback_blocks(device, sb, inode, old_blocks);
        *inode = original;
        return r;
    }
    return device->flush(device->context) == OPENFS_IO_OK ? OPENFS_FILE_OK : OPENFS_FILE_IO_ERROR;
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
    if(ar!=OPENFS_INODE_OK)return ar==OPENFS_INODE_ACCESS_DENIED?OPENFS_FILE_OUT_OF_RANGE:OPENFS_FILE_CORRUPT;
    return openfs_file_truncate(d,s,i,size);
}
openfs_file_result_t openfs_file_write_tx(openfs_transaction_t*t,const openfs_superblock_t*s,openfs_inode_t*i,uint64_t o,const void*b,size_t n){openfs_block_device_t*d=openfs_transaction_device(t);if(d==NULL)return OPENFS_FILE_INVALID_ARGUMENT;openfs_file_result_t r=openfs_file_write(d,s,i,o,b,n);if(r!=OPENFS_FILE_OK)t->failed=1;return r;}
openfs_file_result_t openfs_file_truncate_tx(openfs_transaction_t*t,const openfs_superblock_t*s,openfs_inode_t*i,uint64_t n){openfs_block_device_t*d=openfs_transaction_device(t);if(d==NULL)return OPENFS_FILE_INVALID_ARGUMENT;openfs_file_result_t r=openfs_file_truncate(d,s,i,n);if(r!=OPENFS_FILE_OK)t->failed=1;return r;}