#include "openfs/dir.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/crc32c.h"
#include "openfs/data_checksum.h"
#include "openfs/time.h"
#include "openfs/runtime.h"

#define DIR_MAGIC "ODIR1"

static size_t name_length(const char *name)
{
    if (name == NULL) return SIZE_MAX;
    size_t n = 0U;
    while (n <= OPENFS_DIR_NAME_MAX && name[n] != '\0') ++n;
    return n > OPENFS_DIR_NAME_MAX ? SIZE_MAX : n;
}

static openfs_dir_result_t map_file_result(openfs_file_result_t r)
{
    switch (r) {
    case OPENFS_FILE_OK: return OPENFS_DIR_OK;
    case OPENFS_FILE_NO_SPACE: return OPENFS_DIR_NO_SPACE;
    case OPENFS_FILE_OUT_OF_RANGE: return OPENFS_DIR_NO_SPACE;
    case OPENFS_FILE_CORRUPT: return OPENFS_DIR_CORRUPT;
    default: return OPENFS_DIR_IO_ERROR;
    }
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
    if (inode == NULL || name == NULL || inode->size > UINT64_MAX) return OPENFS_DIR_INVALID_ARGUMENT;
    if (inode->size > UINT64_MAX - (OPENFS_DIR_ENTRY_SIZE - 1U)) return OPENFS_DIR_CORRUPT;
    if ((inode->mode & 0170000U) != OPENFS_INODE_MODE_DIRECTORY) return OPENFS_DIR_INVALID_ARGUMENT;
    if (name[0] == '\0' || strchr(name, '/') != NULL) return OPENFS_DIR_INVALID_ARGUMENT;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return OPENFS_DIR_INVALID_ARGUMENT;
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
    if (raw == NULL || entry == NULL || name == NULL) return -1;
    int empty = 1;
    for (size_t z = 0U; z < OPENFS_DIR_ENTRY_SIZE; ++z) {
        if (raw[z] != 0U) { empty = 0; break; }
    }
    if (empty) return 0;
    if (memcmp(raw, DIR_MAGIC, 5U) != 0) return -1;
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
    for (size_t z = 24U + len; z < 252U; ++z) if (raw[z] != 0U) return -1;
    if (memchr(raw + 24U, '\0', len) != NULL ||
        (len == 1U && raw[24U] == '.') ||
        (len == 2U && raw[24U] == '.' && raw[25U] == '.')) return -1;
    memcpy(name, raw + 24U, len);
    name[len] = '\0';
    if (memchr(name, '/', len) != NULL) return -1;
    return 1;
}

static openfs_dir_result_t dir_lookup_unlocked(
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
        if (n > UINT64_MAX / OPENFS_DIR_ENTRY_SIZE) return OPENFS_DIR_CORRUPT;
        openfs_file_result_t r = openfs_file_read(d, sb, dir, n * OPENFS_DIR_ENTRY_SIZE, raw, sizeof(raw), &got);
        if (r != OPENFS_FILE_OK || got != sizeof(raw)) {
            return r == OPENFS_FILE_CORRUPT ? OPENFS_DIR_CORRUPT : OPENFS_DIR_IO_ERROR;
        }
        int decoded = decode_entry(raw, out, current);
        if (decoded < 0) return OPENFS_DIR_CORRUPT;
        if (decoded == 1 && strlen(current) == len && memcmp(current, name, len) == 0) return OPENFS_DIR_OK;
    }
    return OPENFS_DIR_NOT_FOUND;
}

static openfs_dir_result_t dir_add_unlocked(
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
        if (n > UINT64_MAX / OPENFS_DIR_ENTRY_SIZE) return OPENFS_DIR_CORRUPT;
        openfs_file_result_t rr = openfs_file_read(
            d, sb, dir, n * OPENFS_DIR_ENTRY_SIZE, existing, sizeof(existing), &got);
        if (rr != OPENFS_FILE_OK || got != sizeof(existing)) {
            return rr == OPENFS_FILE_CORRUPT ? OPENFS_DIR_CORRUPT : OPENFS_DIR_IO_ERROR;
        }
        int empty = 1;
        for (size_t z = 0U; z < sizeof(existing); ++z) {
            if (existing[z] != 0U) { empty = 0; break; }
        }
        if (empty) {
            return map_file_result(openfs_file_write(
                d, sb, dir, n * OPENFS_DIR_ENTRY_SIZE, raw, sizeof(raw)));
        }
    }

    uint64_t offset = dir->size;
    if (offset > UINT64_MAX - OPENFS_DIR_ENTRY_SIZE) return OPENFS_DIR_NO_SPACE;
    return map_file_result(openfs_file_write(d, sb, dir, offset, raw, sizeof(raw)));
}

static int restore_directory_data_block(openfs_block_device_t *d,const openfs_superblock_t *sb,uint64_t physical,const uint8_t *original)
{
    int ok=d->write(d->context,physical,1U,original)==OPENFS_IO_OK;
    if((sb->feature_flags&OPENFS_FEATURE_DATA_CHECKSUM)!=0U &&
       openfs_data_checksum_set(d,sb,physical,openfs_data_checksum(original,d->block_size))!=0)ok=0;
    return ok;
}

static openfs_dir_result_t dir_remove_unlocked(
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
        openfs_file_result_t fr=openfs_file_read(d,sb,dir,n*OPENFS_DIR_ENTRY_SIZE,raw,sizeof(raw),&got);if(fr!=OPENFS_FILE_OK||got!=sizeof(raw))return fr==OPENFS_FILE_CORRUPT?OPENFS_DIR_CORRUPT:OPENFS_DIR_IO_ERROR;
        int decoded = decode_entry(raw, &found, current);
        if (decoded < 0) return OPENFS_DIR_CORRUPT;
        if (decoded == 1 && strcmp(current, name) == 0) {
            memset(raw, 0, sizeof(raw));
            uint64_t physical = 0U;
            if (d->block_size == 0U || n > UINT64_MAX / OPENFS_DIR_ENTRY_SIZE || openfs_file_map_block_device(d, sb, dir, (n * OPENFS_DIR_ENTRY_SIZE) / d->block_size, &physical) != OPENFS_FILE_OK) return OPENFS_DIR_CORRUPT;
            uint32_t within = (uint32_t)((n * OPENFS_DIR_ENTRY_SIZE) % d->block_size);
            if ((uint64_t)within + OPENFS_DIR_ENTRY_SIZE > d->block_size) return OPENFS_DIR_CORRUPT;
            uint8_t *block = (uint8_t *)malloc(d->block_size);
            uint8_t *original_block = (uint8_t *)malloc(d->block_size);
            if (block == NULL || original_block == NULL) { free(block); free(original_block); return OPENFS_DIR_IO_ERROR; }
            if (d->read(d->context, physical, 1U, block) != OPENFS_IO_OK) { free(block); free(original_block); return OPENFS_DIR_IO_ERROR; }
            memcpy(original_block, block, d->block_size);
            memset(block + within, 0, OPENFS_DIR_ENTRY_SIZE);
            openfs_io_result_t io = d->write(d->context, physical, 1U, block);
            if(io==OPENFS_IO_OK && (sb->feature_flags&OPENFS_FEATURE_DATA_CHECKSUM)!=0U &&
               openfs_data_checksum_set(d,sb,physical,openfs_data_checksum(block,d->block_size))!=0)io=OPENFS_IO_IO_ERROR;
            if (io != OPENFS_IO_OK) {
                int rollback_ok=restore_directory_data_block(d,sb,physical,original_block);
                if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
                free(block); free(original_block);
                return rollback_ok?OPENFS_DIR_IO_ERROR:OPENFS_DIR_CORRUPT;
            }
            uint64_t inode_bytes=0U;
            if (d->block_size!=0U && sb->inode_table_blocks>UINT64_MAX/d->block_size) {
                int rollback_ok=restore_directory_data_block(d,sb,physical,original_block);
                if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
                free(block); free(original_block); return rollback_ok?OPENFS_DIR_IO_ERROR:OPENFS_DIR_CORRUPT;
            }
            inode_bytes=sb->inode_table_blocks*(uint64_t)d->block_size;
            uint64_t inode_count=inode_bytes/OPENFS_INODE_SIZE;
            if (inode_count==0U || dir->inode_number==0U || dir->inode_number>inode_count ||
                dir->inode_number-1U>UINT64_MAX/OPENFS_INODE_SIZE) {
                int rollback_ok=restore_directory_data_block(d,sb,physical,original_block);
                if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
                free(block); free(original_block); return rollback_ok?OPENFS_DIR_CORRUPT:OPENFS_DIR_CORRUPT;
            }
            uint64_t inode_offset=(dir->inode_number-1U)*(uint64_t)OPENFS_INODE_SIZE;
            uint64_t inode_block=sb->inode_table_start+inode_offset/d->block_size;
            if (inode_block>=d->block_count ||
                (inode_offset%d->block_size)+OPENFS_INODE_SIZE>d->block_size) {
                int rollback_ok=restore_directory_data_block(d,sb,physical,original_block);
                if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
                free(block); free(original_block); return rollback_ok?OPENFS_DIR_CORRUPT:OPENFS_DIR_CORRUPT;
            }
            uint8_t *original_inode_block=(uint8_t *)malloc(d->block_size);
            if (original_inode_block==NULL ||
                d->read(d->context,inode_block,1U,original_inode_block)!=OPENFS_IO_OK) {
                free(original_inode_block);
                int rollback_ok=restore_directory_data_block(d,sb,physical,original_block);
                if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
                free(block); free(original_block); return rollback_ok?OPENFS_DIR_IO_ERROR:OPENFS_DIR_CORRUPT;
            }
            uint64_t now=openfs_time_now_ns();
            uint64_t old_mtime=dir->mtime_ns, old_ctime=dir->ctime_ns;
            if(now!=UINT64_MAX){dir->mtime_ns=now;dir->ctime_ns=now;}
            openfs_inode_result_t ir=openfs_inode_write(d,sb->inode_table_start,inode_count,dir);
            if(ir!=OPENFS_INODE_OK){
                dir->mtime_ns=old_mtime;dir->ctime_ns=old_ctime;
                int rollback_ok=restore_directory_data_block(d,sb,physical,original_block);
                if(d->write(d->context,inode_block,1U,original_inode_block)!=OPENFS_IO_OK)rollback_ok=0;
                if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;
                free(original_inode_block);
                free(block); free(original_block);
                return rollback_ok?OPENFS_DIR_IO_ERROR:OPENFS_DIR_CORRUPT;
            }
            if (d->flush(d->context) == OPENFS_IO_OK) {
                free(original_inode_block);
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
            if (!restore_directory_data_block(d,sb,physical,original_block)) {
                rollback_ok = 0;
            }
            dir->mtime_ns = old_mtime;
            dir->ctime_ns = old_ctime;
            if (d->write(d->context,inode_block,1U,original_inode_block)!=OPENFS_IO_OK) {
                rollback_ok = 0;
            }
            if (d->flush(d->context) != OPENFS_IO_OK) {
                rollback_ok = 0;
            }
            free(original_inode_block);
            free(block);
            free(original_block);
            return rollback_ok ? OPENFS_DIR_IO_ERROR : OPENFS_DIR_CORRUPT;
        }
    }
    return OPENFS_DIR_NOT_FOUND;
}

static openfs_dir_result_t lock_directory_runtime(const openfs_superblock_t *s){
    if(s==NULL||s->runtime==NULL)return OPENFS_DIR_OK;
    if(!openfs_runtime_enter(s->runtime))return OPENFS_DIR_IO_ERROR;
    if(openfs_mutex_lock(&s->runtime->directory_lock,OPENFS_LOCK_RANK_DIRECTORY)!=OPENFS_LOCK_OK){openfs_runtime_leave(s->runtime);return OPENFS_DIR_IO_ERROR;}
    return OPENFS_DIR_OK;
}
static void unlock_directory_runtime(const openfs_superblock_t *s){if(s==NULL||s->runtime==NULL)return;(void)openfs_mutex_unlock(&s->runtime->directory_lock);openfs_runtime_leave(s->runtime);}
openfs_dir_result_t openfs_dir_lookup(const openfs_block_device_t*d,const openfs_superblock_t*s,const openfs_inode_t*i,const char*n,openfs_dir_entry_t*e)
{
    openfs_dir_result_t lr=lock_directory_runtime(s);if(lr!=OPENFS_DIR_OK)return lr;
    if(s==NULL||s->runtime==NULL)return dir_lookup_unlocked(d,s,i,n,e);
    openfs_dir_result_t r=dir_lookup_unlocked(d,s,i,n,e);unlock_directory_runtime(s);return r;
}
openfs_dir_result_t openfs_dir_add(openfs_block_device_t*d,const openfs_superblock_t*s,openfs_inode_t*i,const char*n,const openfs_dir_entry_t*e)
{
    openfs_dir_result_t lr=lock_directory_runtime(s);if(lr!=OPENFS_DIR_OK)return lr;
    if(s==NULL||s->runtime==NULL)return dir_add_unlocked(d,s,i,n,e);
    openfs_dir_result_t r=dir_add_unlocked(d,s,i,n,e);unlock_directory_runtime(s);return r;
}
openfs_dir_result_t openfs_dir_remove(openfs_block_device_t*d,const openfs_superblock_t*s,openfs_inode_t*i,const char*n)
{
    openfs_dir_result_t lr=lock_directory_runtime(s);if(lr!=OPENFS_DIR_OK)return lr;
    if(s==NULL||s->runtime==NULL)return dir_remove_unlocked(d,s,i,n);
    openfs_dir_result_t r=dir_remove_unlocked(d,s,i,n);unlock_directory_runtime(s);return r;
}
