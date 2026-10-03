#include "openfs/dir.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/crc32c.h"
#include "openfs/time.h"

#define DIR_MAGIC "ODIR1"

static size_t name_length(const char *name)
{
    if (name == NULL) return SIZE_MAX;
    size_t n = 0U;
    while (n <= OPENFS_DIR_NAME_MAX && name[n] != '\0') ++n;
    return n > OPENFS_DIR_NAME_MAX ? SIZE_MAX : n;
}

static openfs_dir_result_t validate_entry(const openfs_dir_entry_t *entry)
{
    if (entry == NULL || entry->inode_number == 0U || entry->generation == 0U ||
        (entry->type != 1U && entry->type != 2U && entry->type != 3U)) {
        return OPENFS_DIR_INVALID_ARGUMENT;
    }
    return OPENFS_DIR_OK;
}

static openfs_dir_result_t validate_dir(const openfs_inode_t *inode, const char *name)
{
    if (inode == NULL || name == NULL) return OPENFS_DIR_INVALID_ARGUMENT;
    if ((inode->mode & 0170000U) != OPENFS_INODE_MODE_DIRECTORY) return OPENFS_DIR_INVALID_ARGUMENT;
    if (name[0] == '\0' || strchr(name, '/') != NULL) return OPENFS_DIR_INVALID_ARGUMENT;
    if (name_length(name) == SIZE_MAX) return OPENFS_DIR_NAME_TOO_LONG;
    return OPENFS_DIR_OK;
}

static void encode_entry(uint8_t *raw, const char *name, const openfs_dir_entry_t *entry)
{
    memset(raw, 0, OPENFS_DIR_ENTRY_SIZE);
    memcpy(raw, DIR_MAGIC, 5U);
    for (unsigned k = 0U; k < 8U; ++k) {
        raw[8U + k] = (uint8_t)(entry->inode_number >> (8U * k));
        raw[16U + k] = (uint8_t)(entry->generation >> (8U * k));
    }
    raw[6U] = entry->type;
    size_t len = strlen(name);
    raw[7U] = (uint8_t)len;
    memcpy(raw + 24U, name, len);
    uint32_t crc = openfs_crc32c(raw, 252U);
    raw[252U] = (uint8_t)crc;
    raw[253U] = (uint8_t)(crc >> 8U);
    raw[254U] = (uint8_t)(crc >> 16U);
    raw[255U] = (uint8_t)(crc >> 24U);
}

static int decode_entry(const uint8_t *raw, openfs_dir_entry_t *entry, char *name)
{
    if (memcmp(raw, DIR_MAGIC, 5U) != 0) return 0;
    uint32_t stored = (uint32_t)raw[252U] |
        ((uint32_t)raw[253U] << 8U) |
        ((uint32_t)raw[254U] << 16U) |
        ((uint32_t)raw[255U] << 24U);
    if (stored != openfs_crc32c(raw, 252U)) return -1;
    size_t len = raw[7U];
    if (len == 0U || len > OPENFS_DIR_NAME_MAX) return -1;
    entry->inode_number = 0U;
    entry->generation = 0U;
    for (unsigned k = 0U; k < 8U; ++k) {
        entry->inode_number |= (uint64_t)raw[8U + k] << (8U * k);
        entry->generation |= (uint64_t)raw[16U + k] << (8U * k);
    }
    entry->type = raw[6U];
    if (entry->generation == 0U || entry->inode_number == 0U ||
        (entry->type != 1U && entry->type != 2U && entry->type != 3U)) return -1;
    memcpy(name, raw + 24U, len);
    name[len] = '\0';
    return 1;
}

openfs_dir_result_t openfs_dir_lookup(
    const openfs_block_device_t *d,
    const openfs_superblock_t *sb,
    const openfs_inode_t *dir,
    const char *name,
    openfs_dir_entry_t *out)
{
    openfs_dir_result_t vr = validate_dir(dir, name);
    if (!openfs_block_device_is_valid(d) || sb == NULL || sb->block_size != d->block_size) return OPENFS_DIR_INVALID_ARGUMENT;
    if (dir != NULL && dir->size % OPENFS_DIR_ENTRY_SIZE != 0U) return OPENFS_DIR_CORRUPT;
    if (vr != OPENFS_DIR_OK || out == NULL) return vr != OPENFS_DIR_OK ? vr : OPENFS_DIR_INVALID_ARGUMENT;
    size_t len = name_length(name);
    uint64_t entries = dir->size / OPENFS_DIR_ENTRY_SIZE;
    uint8_t raw[OPENFS_DIR_ENTRY_SIZE];
    char current[OPENFS_DIR_NAME_MAX + 1U];

    for (uint64_t n = 0U; n < entries; ++n) {
        size_t got = 0U;
        openfs_file_result_t r = openfs_file_read(d, sb, dir, n * OPENFS_DIR_ENTRY_SIZE, raw, sizeof(raw), &got);
        if (r != OPENFS_FILE_OK || got != sizeof(raw)) return OPENFS_DIR_IO_ERROR;
        int decoded = decode_entry(raw, out, current);
        if (decoded < 0) return OPENFS_DIR_CORRUPT;
        if (decoded == 1 && strlen(current) == len && memcmp(current, name, len) == 0) return OPENFS_DIR_OK;
    }
    return OPENFS_DIR_NOT_FOUND;
}

openfs_dir_result_t openfs_dir_add(
    openfs_block_device_t *d,
    const openfs_superblock_t *sb,
    openfs_inode_t *dir,
    const char *name,
    const openfs_dir_entry_t *entry)
{
    openfs_dir_result_t vr = validate_dir(dir, name);
    if (!openfs_block_device_is_valid(d) || sb == NULL || sb->block_size != d->block_size) return OPENFS_DIR_INVALID_ARGUMENT;
    if (dir != NULL && dir->size % OPENFS_DIR_ENTRY_SIZE != 0U) return OPENFS_DIR_CORRUPT;
    if (vr != OPENFS_DIR_OK) return vr;
    if (validate_entry(entry) != OPENFS_DIR_OK) return OPENFS_DIR_INVALID_ARGUMENT;
    openfs_dir_entry_t found;
    openfs_dir_result_t lookup = openfs_dir_lookup(d, sb, dir, name, &found);
    if (lookup == OPENFS_DIR_OK) return OPENFS_DIR_EXISTS;
    if (lookup != OPENFS_DIR_NOT_FOUND) return lookup;

    uint8_t raw[OPENFS_DIR_ENTRY_SIZE];
    encode_entry(raw, name, entry);

    /* Reuse a deleted slot before growing the directory. */
    uint64_t entries = dir->size / OPENFS_DIR_ENTRY_SIZE;
    uint8_t existing[OPENFS_DIR_ENTRY_SIZE];
    for (uint64_t n = 0U; n < entries; ++n) {
        size_t got = 0U;
        openfs_file_result_t rr = openfs_file_read(
            d, sb, dir, n * OPENFS_DIR_ENTRY_SIZE, existing, sizeof(existing), &got);
        if (rr != OPENFS_FILE_OK || got != sizeof(existing)) {
            return OPENFS_DIR_IO_ERROR;
        }
        if (memcmp(existing, "\0\0\0\0\0", 5U) == 0) {
            return openfs_file_write(
                d, sb, dir, n * OPENFS_DIR_ENTRY_SIZE, raw, sizeof(raw)) == OPENFS_FILE_OK
                ? OPENFS_DIR_OK : OPENFS_DIR_IO_ERROR;
        }
    }

    uint64_t offset = dir->size;
    if (offset > UINT64_MAX - OPENFS_DIR_ENTRY_SIZE) return OPENFS_DIR_NO_SPACE;
    return openfs_file_write(d, sb, dir, offset, raw, sizeof(raw)) == OPENFS_FILE_OK
        ? OPENFS_DIR_OK : OPENFS_DIR_IO_ERROR;
}

static openfs_inode_result_t write_inode_for_dir_rollback(
    openfs_block_device_t *d,
    const openfs_superblock_t *sb,
    const openfs_inode_t *dir)
{
    if (d == NULL || sb == NULL || dir == NULL ||
        d->block_size == 0U ||
        sb->inode_table_blocks > UINT64_MAX / d->block_size) {
        return OPENFS_INODE_INVALID_ARGUMENT;
    }
    uint64_t count = (sb->inode_table_blocks * (uint64_t)d->block_size) /
        OPENFS_INODE_SIZE;
    if (count == 0U) {
        return OPENFS_INODE_CORRUPT;
    }
    return openfs_inode_write(d, sb->inode_table_start, count, dir);
}

openfs_dir_result_t openfs_dir_remove(
    openfs_block_device_t *d,
    const openfs_superblock_t *sb,
    openfs_inode_t *dir,
    const char *name)
{
    openfs_dir_result_t vr = validate_dir(dir, name);
    if (vr != OPENFS_DIR_OK) return vr;
    if (!openfs_block_device_is_valid(d) || sb == NULL || sb->block_size != d->block_size) return OPENFS_DIR_INVALID_ARGUMENT;
    if (dir->size % OPENFS_DIR_ENTRY_SIZE != 0U) return OPENFS_DIR_CORRUPT;
    uint64_t entries = dir->size / OPENFS_DIR_ENTRY_SIZE;
    uint8_t raw[OPENFS_DIR_ENTRY_SIZE];
    char current[OPENFS_DIR_NAME_MAX + 1U];
    openfs_dir_entry_t found;

    for (uint64_t n = 0U; n < entries; ++n) {
        size_t got = 0U;
        if (openfs_file_read(d, sb, dir, n * OPENFS_DIR_ENTRY_SIZE, raw, sizeof(raw), &got) != OPENFS_FILE_OK || got != sizeof(raw)) {
            return OPENFS_DIR_IO_ERROR;
        }
        int decoded = decode_entry(raw, &found, current);
        if (decoded < 0) return OPENFS_DIR_CORRUPT;
        if (decoded == 1 && strcmp(current, name) == 0) {
            memset(raw, 0, sizeof(raw));
            uint64_t physical = 0U;
            if (openfs_file_map_block_device(d, sb, dir, (n * OPENFS_DIR_ENTRY_SIZE) / d->block_size, &physical) != OPENFS_FILE_OK) return OPENFS_DIR_CORRUPT;
            uint32_t within = (uint32_t)((n * OPENFS_DIR_ENTRY_SIZE) % d->block_size);
            uint8_t *block = (uint8_t *)malloc(d->block_size);
            uint8_t *original_block = (uint8_t *)malloc(d->block_size);
            if (block == NULL || original_block == NULL) { free(block); free(original_block); return OPENFS_DIR_IO_ERROR; }
            if (d->read(d->context, physical, 1U, block) != OPENFS_IO_OK) { free(block); free(original_block); return OPENFS_DIR_IO_ERROR; }
            memcpy(original_block, block, d->block_size);
            memset(block + within, 0, OPENFS_DIR_ENTRY_SIZE);
            openfs_io_result_t io = d->write(d->context, physical, 1U, block);
            if (io != OPENFS_IO_OK) { free(block); free(original_block); return OPENFS_DIR_IO_ERROR; }
            uint64_t inode_bytes=0U;
            if (d->block_size!=0U && sb->inode_table_blocks>UINT64_MAX/d->block_size) {
                int rollback_ok=d->write(d->context, physical, 1U, original_block)==OPENFS_IO_OK;
                if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
                free(block); free(original_block); return OPENFS_DIR_CORRUPT;
            }
            inode_bytes=sb->inode_table_blocks*(uint64_t)d->block_size;
            uint64_t inode_count=inode_bytes/OPENFS_INODE_SIZE;
            if (inode_count==0U) {
                (void)d->write(d->context, physical, 1U, original_block);
                free(block); free(original_block); return OPENFS_DIR_CORRUPT;
            }
            uint64_t now=openfs_time_now_ns();
            uint64_t old_mtime=dir->mtime_ns, old_ctime=dir->ctime_ns;
            if(now!=UINT64_MAX){dir->mtime_ns=now;dir->ctime_ns=now;}
            openfs_inode_result_t ir=openfs_inode_write(d,sb->inode_table_start,inode_count,dir);
            if(ir!=OPENFS_INODE_OK){
                dir->mtime_ns=old_mtime;dir->ctime_ns=old_ctime;
                int rollback_ok=d->write(d->context, physical, 1U, original_block)==OPENFS_IO_OK;
                if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
                free(block); free(original_block);
                return rollback_ok?OPENFS_DIR_IO_ERROR:OPENFS_DIR_CORRUPT;
            }
            if (d->flush(d->context) == OPENFS_IO_OK) {
                free(block);
                free(original_block);
                return OPENFS_DIR_OK;
            }

            /*
             * The directory block and inode metadata have both been published
             * before the durability barrier.  A failed flush must not leave a
             * successful-looking direct unlink partially persisted.  Restore
             * both pieces and require a successful rollback flush; otherwise
             * report corruption because the previous state is no longer known
             * to be durable.
             */
            int rollback_ok = 1;
            if (d->write(d->context, physical, 1U, original_block) != OPENFS_IO_OK) {
                rollback_ok = 0;
            }
            /* Restore timestamps and persist the original inode metadata. */
            dir->mtime_ns = old_mtime;
            dir->ctime_ns = old_ctime;
            if (write_inode_for_dir_rollback(d, sb, dir) != OPENFS_INODE_OK) {
                rollback_ok = 0;
            }
            if (d->flush(d->context) != OPENFS_IO_OK) {
                rollback_ok = 0;
            }
            free(block);
            free(original_block);
            return rollback_ok ? OPENFS_DIR_IO_ERROR : OPENFS_DIR_CORRUPT;
        }
    }
    return OPENFS_DIR_NOT_FOUND;
}
