#include "openfs/extent.h"
#include "openfs/format.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/crc32c.h"

static uint8_t *slot(openfs_inode_t *inode, uint32_t index)
{
    return inode->reserved + (size_t)index * OPENFS_EXTENT_RECORD_SIZE;
}

static uint64_t load64(const uint8_t *p)
{
    uint64_t v = 0U;
    for (unsigned k = 0U; k < 8U; ++k) v |= (uint64_t)p[k] << (8U * k);
    return v;
}

static void store16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8U);
}

static uint16_t load16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8U);
}

static void store32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8U);
    p[2] = (uint8_t)(v >> 16U);
    p[3] = (uint8_t)(v >> 24U);
}

static void store64(uint8_t *p, uint64_t v)
{
    for (unsigned k = 0U; k < 8U; ++k) p[k] = (uint8_t)(v >> (8U * k));
}

static void encode_extent(uint8_t *p, const openfs_extent_t *e)
{
    store64(p, e->logical_start);
    store64(p + 8U, e->physical_start);
    store64(p + 16U, e->block_count);
}

static int decode_extent(const uint8_t *p, openfs_extent_t *e)
{
    e->logical_start = load64(p);
    e->physical_start = load64(p + 8U);
    e->block_count = load64(p + 16U);
    if (e->block_count == 0U ||
        e->logical_start > UINT64_MAX - e->block_count ||
        e->physical_start > UINT64_MAX - e->block_count) {
        return 0;
    }
    return 1;
}

static int extent_overlap(const openfs_extent_t *a,const openfs_extent_t *b)
{
    uint64_t al=a->logical_start+a->block_count,bl=b->logical_start+b->block_count;
    uint64_t ap=a->physical_start+a->block_count,bp=b->physical_start+b->block_count;
    return (a->logical_start<bl&&b->logical_start<al)||(a->physical_start<bp&&b->physical_start<ap);
}
static int validate_extent_order(const openfs_extent_t *e, uint32_t count)
{
    uint64_t previous_end=0U;
    for(uint32_t n=0U;n<count;n++){
        if(e[n].block_count==0U||e[n].logical_start>UINT64_MAX-e[n].block_count||
           e[n].physical_start>UINT64_MAX-e[n].block_count)return 0;
        if(n!=0U&&e[n].logical_start!=previous_end)return 0;
        for(uint32_t p=0U;p<n;p++)if(extent_overlap(&e[p],&e[n]))return 0;
        previous_end=e[n].logical_start+e[n].block_count;
    }
    return 1;
}

openfs_extent_result_t openfs_inode_get_extent(
    const openfs_inode_t *inode,
    uint32_t index,
    openfs_extent_t *out)
{
    if (inode == NULL || out == NULL) return OPENFS_EXTENT_INVALID_ARGUMENT;
    if (index >= inode->extent_count || index >= OPENFS_EXTENT_MAX || ((inode->flags & OPENFS_INODE_FLAG_EXTENT_TREE) != 0U && index >= OPENFS_INODE_TREE_INLINE_EXTENT_MAX)) {
        return OPENFS_EXTENT_OUT_OF_RANGE;
    }
    const uint8_t *p = slot((openfs_inode_t *)inode, index);
    if (!decode_extent(p, out)) return OPENFS_EXTENT_CORRUPT;
    return OPENFS_EXTENT_OK;
}

openfs_extent_result_t openfs_inode_set_extent(openfs_inode_t *inode,uint32_t index,const openfs_extent_t *extent)
{
    if(inode==NULL||extent==NULL)return OPENFS_EXTENT_INVALID_ARGUMENT;
    uint32_t inline_limit=(inode->flags&OPENFS_INODE_FLAG_EXTENT_TREE)!=0U?OPENFS_INODE_TREE_INLINE_EXTENT_MAX:OPENFS_EXTENT_MAX;
    if(index>=inline_limit||index>inode->extent_count||extent->block_count==0U)return OPENFS_EXTENT_OUT_OF_RANGE;
    if(extent->logical_start>UINT64_MAX-extent->block_count||
       extent->physical_start>UINT64_MAX-extent->block_count)return OPENFS_EXTENT_CORRUPT;
    uint8_t old_record[OPENFS_EXTENT_RECORD_SIZE];uint32_t old_flags=inode->flags,old_count=inode->extent_count;
    uint8_t *p=slot(inode,index);memcpy(old_record,p,sizeof(old_record));encode_extent(p,extent);
    uint32_t stored_count=old_count<inline_limit?old_count:inline_limit;
    for(uint32_t n=0U;n<stored_count;n++){
        if(n==index)continue;
        openfs_extent_t other;
        if(!decode_extent(slot(inode,n),&other)||
           other.block_count==0U||
           other.logical_start>UINT64_MAX-other.block_count||
           other.physical_start>UINT64_MAX-other.block_count||
           extent_overlap(extent,&other)){
            memcpy(p,old_record,sizeof(old_record));inode->flags=old_flags;inode->extent_count=old_count;return OPENFS_EXTENT_CORRUPT;
        }
    }
    if(index>=inode->extent_count)inode->extent_count=index+1U;
    inode->flags|=OPENFS_INODE_FLAG_HAS_EXTENTS;
    return OPENFS_EXTENT_OK;
}

uint64_t openfs_inode_get_extent_tree_root(const openfs_inode_t *inode)
{
    if (inode == NULL || (inode->flags & OPENFS_INODE_FLAG_EXTENT_TREE) == 0U) return 0U;
    return load64(inode->reserved + 112U);
}

openfs_extent_result_t openfs_inode_set_extent_tree_root(openfs_inode_t *inode, uint64_t root)
{
    if (inode == NULL || root == 0U) return OPENFS_EXTENT_INVALID_ARGUMENT;
    store64(inode->reserved + 112U, root);
    inode->flags |= OPENFS_INODE_FLAG_EXTENT_TREE;
    inode->flags |= OPENFS_INODE_FLAG_HAS_EXTENTS;
    return OPENFS_EXTENT_OK;
}

uint32_t openfs_extent_tree_capacity(uint32_t block_size)
{
    if (block_size < OPENFS_EXTENT_TREE_HEADER_SIZE + OPENFS_EXTENT_TREE_RECORD_SIZE) return 0U;
    uint64_t cap = (block_size - OPENFS_EXTENT_TREE_HEADER_SIZE) / OPENFS_EXTENT_TREE_RECORD_SIZE;
    return cap > UINT32_MAX ? UINT32_MAX : (uint32_t)cap;
}

openfs_extent_result_t openfs_extent_tree_read(
    const openfs_block_device_t *device,
    const openfs_superblock_t *sb,
    const openfs_inode_t *inode,
    uint32_t index,
    openfs_extent_t *out)
{
    if (!openfs_block_device_is_valid(device) || sb == NULL || device->block_size==0U || sb->block_size != device->block_size || inode == NULL || out == NULL) {
        return OPENFS_EXTENT_INVALID_ARGUMENT;
    }
    uint64_t root = openfs_inode_get_extent_tree_root(inode);
    if (sb->data_blocks == 0U || sb->data_start > UINT64_MAX - sb->data_blocks || sb->data_start + sb->data_blocks > device->block_count || root < sb->data_start || root >= sb->data_start + sb->data_blocks) return OPENFS_EXTENT_CORRUPT;
    if (inode->extent_count < OPENFS_INODE_TREE_INLINE_EXTENT_MAX + 1U ||
        (inode->flags & OPENFS_INODE_FLAG_EXTENT_TREE) == 0U) return OPENFS_EXTENT_OUT_OF_RANGE;
    uint32_t cap = openfs_extent_tree_capacity(device->block_size);
    uint64_t tree_count64 = inode->extent_count - OPENFS_INODE_TREE_INLINE_EXTENT_MAX;
    if (cap == 0U || tree_count64 > UINT16_MAX || index >= tree_count64 || index >= cap) return OPENFS_EXTENT_OUT_OF_RANGE;

    uint8_t *block = malloc(device->block_size);
    if (block == NULL) return OPENFS_EXTENT_IO_ERROR;
    if (device->read(device->context, root, 1U, block) != OPENFS_IO_OK) {
        free(block);
        return OPENFS_EXTENT_IO_ERROR;
    }
    if (inode->generation == 0U || memcmp(block, OPENFS_EXTENT_TREE_MAGIC, 8U) != 0 ||
        load16(block + 8U) != 0U ||
        load16(block + 10U) != (uint16_t)tree_count64 ||
        load16(block + 12U) != (uint16_t)cap ||
        load64(block + 16U) != inode->generation) {
        free(block);
        return OPENFS_EXTENT_CORRUPT;
    }
    uint32_t stored = (uint32_t)block[24U] | ((uint32_t)block[25U] << 8U) |
        ((uint32_t)block[26U] << 16U) | ((uint32_t)block[27U] << 24U);
    memset(block + 24U, 0, 4U);
    if (stored != openfs_crc32c(block, device->block_size)) {
        free(block);
        return OPENFS_EXTENT_CORRUPT;
    }
    uint16_t entries = load16(block + 10U);
    if (index >= entries) {
        free(block);
        return OPENFS_EXTENT_OUT_OF_RANGE;
    }
    if (!decode_extent(block + OPENFS_EXTENT_TREE_HEADER_SIZE +
        (size_t)index * OPENFS_EXTENT_TREE_RECORD_SIZE, out)) {
        free(block);
        return OPENFS_EXTENT_CORRUPT;
    }
    free(block);
    return OPENFS_EXTENT_OK;
}

openfs_extent_result_t openfs_extent_tree_write(
    const openfs_block_device_t *device,
    const openfs_inode_t *inode,
    const openfs_extent_t *extents,
    uint32_t count)
{
    if (!openfs_block_device_is_valid(device) || sb == NULL || device->block_size==0U || sb->block_size != device->block_size || inode == NULL ||
        (count != 0U && extents == NULL)) return OPENFS_EXTENT_INVALID_ARGUMENT;
    uint64_t root = openfs_inode_get_extent_tree_root(inode);
    uint32_t cap = openfs_extent_tree_capacity(device->block_size);
    if (!extent_tree_root_in_data_area(device, root) || inode->generation==0U || cap == 0U || count > cap || count > UINT16_MAX ||
        inode->extent_count < OPENFS_INODE_TREE_INLINE_EXTENT_MAX + 1U ||
        count != inode->extent_count - OPENFS_INODE_TREE_INLINE_EXTENT_MAX ||
        !validate_extent_order(extents, count)) return OPENFS_EXTENT_CORRUPT;
    openfs_extent_t previous_inline;
    if (openfs_inode_get_extent(inode, OPENFS_INODE_TREE_INLINE_EXTENT_MAX - 1U,
            &previous_inline) != OPENFS_EXTENT_OK ||
        previous_inline.logical_start > UINT64_MAX - previous_inline.block_count ||
        previous_inline.physical_start > UINT64_MAX - previous_inline.block_count ||
        extents[0].logical_start != previous_inline.logical_start + previous_inline.block_count) {
        return OPENFS_EXTENT_CORRUPT;
    }
    for(uint32_t n=0U;n<OPENFS_INODE_TREE_INLINE_EXTENT_MAX;n++){
        openfs_extent_t inline_extent;if(openfs_inode_get_extent(inode,n,&inline_extent)!=OPENFS_EXTENT_OK||
           inline_extent.block_count==0U||inline_extent.logical_start>UINT64_MAX-inline_extent.block_count||
           inline_extent.physical_start>UINT64_MAX-inline_extent.block_count||extent_overlap(&inline_extent,&extents[0]))return OPENFS_EXTENT_CORRUPT;
    }
    for(uint32_t n=0U;n<count;n++){
        if(extents[n].physical_start<sb->data_start || extents[n].physical_start>=sb->data_start+sb->data_blocks ||
           extents[n].block_count>sb->data_start+sb->data_blocks-extents[n].physical_start)return OPENFS_EXTENT_CORRUPT;
        for(uint32_t i=0U;i<OPENFS_INODE_TREE_INLINE_EXTENT_MAX;i++){
            openfs_extent_t inline_extent;
            if(openfs_inode_get_extent(inode,i,&inline_extent)!=OPENFS_EXTENT_OK)return OPENFS_EXTENT_CORRUPT;
            if(extent_overlap(&inline_extent,&extents[n]))return OPENFS_EXTENT_CORRUPT;
        }
        if(root>=extents[n].physical_start&&root<extents[n].physical_start+extents[n].block_count)return OPENFS_EXTENT_CORRUPT;
    }

    uint8_t *block = calloc(1U, device->block_size);
    if (block == NULL) return OPENFS_EXTENT_IO_ERROR;
    memcpy(block, OPENFS_EXTENT_TREE_MAGIC, 8U);
    store16(block + 8U, 0U);
    store16(block + 10U, (uint16_t)count);
    store16(block + 12U, (uint16_t)cap);
    store16(block + 14U, 0U);
    store64(block + 16U, inode->generation);
    for (uint32_t n = 0U; n < count; ++n) {
        encode_extent(block + OPENFS_EXTENT_TREE_HEADER_SIZE +
            (size_t)n * OPENFS_EXTENT_TREE_RECORD_SIZE, &extents[n]);
    }
    store32(block + 24U, 0U);
    store32(block + 24U, openfs_crc32c(block, device->block_size));
    openfs_io_result_t io = device->write(device->context, root, 1U, block);
    free(block);
    return io == OPENFS_IO_OK ? OPENFS_EXTENT_OK : OPENFS_EXTENT_IO_ERROR;
}
