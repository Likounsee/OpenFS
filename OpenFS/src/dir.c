#include "openfs/dir.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "openfs/crc32c.h"

#define DIR_MAGIC "ODIR1"

static size_t name_length(const char *name)
{
    if (name == NULL) return SIZE_MAX;
    size_t n = 0U;
    while (n <= OPENFS_DIR_NAME_MAX && name[n] != '\0') ++n;
    return n > OPENFS_DIR_NAME_MAX ? SIZE_MAX : n;
}

static openfs_dir_result_t validate_dir(const openfs_inode_t *inode, const char *name)
{
    if (inode == NULL || name == NULL) return OPENFS_DIR_INVALID_ARGUMENT;
    if (inode->mode != OPENFS_INODE_MODE_DIRECTORY) return OPENFS_DIR_INVALID_ARGUMENT;
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
    if (vr != OPENFS_DIR_OK || entry == NULL || entry->inode_number == 0U) {
        return vr != OPENFS_DIR_OK ? vr : OPENFS_DIR_INVALID_ARGUMENT;
    }
    openfs_dir_entry_t found;
    openfs_dir_result_t lookup = openfs_dir_lookup(d, sb, dir, name, &found);
    if (lookup == OPENFS_DIR_OK) return OPENFS_DIR_EXISTS;
    if (lookup != OPENFS_DIR_NOT_FOUND) return lookup;

    uint8_t raw[OPENFS_DIR_ENTRY_SIZE];
    encode_entry(raw, name, entry);
    uint64_t offset = dir->size;
    if (offset > UINT64_MAX - OPENFS_DIR_ENTRY_SIZE) return OPENFS_DIR_NO_SPACE;
    return openfs_file_write(d, sb, dir, offset, raw, sizeof(raw)) == OPENFS_FILE_OK
        ? OPENFS_DIR_OK : OPENFS_DIR_IO_ERROR;
}

openfs_dir_result_t openfs_dir_remove(
    openfs_block_device_t *d,
    const openfs_superblock_t *sb,
    openfs_inode_t *dir,
    const char *name)
{
    openfs_dir_result_t vr = validate_dir(dir, name);
    if (vr != OPENFS_DIR_OK) return vr;
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
            if (openfs_file_map_block(dir, (n * OPENFS_DIR_ENTRY_SIZE) / d->block_size, &physical) != OPENFS_FILE_OK) return OPENFS_DIR_CORRUPT;
            uint32_t within = (uint32_t)((n * OPENFS_DIR_ENTRY_SIZE) % d->block_size);
            uint8_t *block = (uint8_t *)malloc(d->block_size);
            if (block == NULL) return OPENFS_DIR_IO_ERROR;
            if (d->read(d->context, physical, 1U, block) != OPENFS_IO_OK) { free(block); return OPENFS_DIR_IO_ERROR; }
            memset(block + within, 0, OPENFS_DIR_ENTRY_SIZE);
            openfs_io_result_t io = d->write(d->context, physical, 1U, block);
            free(block);
            if (io != OPENFS_IO_OK) return OPENFS_DIR_IO_ERROR;
            return d->flush(d->context) == OPENFS_IO_OK ? OPENFS_DIR_OK : OPENFS_DIR_IO_ERROR;
        }
    }
    return OPENFS_DIR_NOT_FOUND;
}
