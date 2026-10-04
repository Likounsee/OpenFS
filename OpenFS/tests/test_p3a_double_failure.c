#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "openfs/allocator.h"
#include "openfs/bitmap.h"
#include "openfs/file.h"
#include "openfs/fsck.h"
#include "openfs/format.h"
#include "openfs/inode.h"
#include "openfs/inode_alloc.h"
#include "openfs/link.h"
#include "openfs/mount.h"
#include "openfs/path.h"

#define BS 4096U
#define BC 256U

typedef struct {
    uint8_t *bytes;
    uint32_t block_size;
    uint64_t block_count;
    uint64_t fail_block;
    unsigned skip_writes;
    int fail_enabled;
    int partial_once;
    size_t partial_bytes;
    int fail_flush;
} disk_t;

static openfs_io_result_t rd(void *ctx, uint64_t first, uint32_t count, void *out)
{
    disk_t *d = ctx;
    if (d == NULL || out == NULL || count == 0U ||
        first >= d->block_count || (uint64_t)count > d->block_count - first)
        return OPENFS_IO_OUT_OF_RANGE;
    memcpy(out, d->bytes + (size_t)(first * d->block_size),
           (size_t)((uint64_t)count * d->block_size));
    return OPENFS_IO_OK;
}

static openfs_io_result_t wr(void *ctx, uint64_t first, uint32_t count,
                             const void *in)
{
    disk_t *d = ctx;
    size_t bytes;
    if (d == NULL || in == NULL || count == 0U ||
        first >= d->block_count || (uint64_t)count > d->block_count - first)
        return OPENFS_IO_OUT_OF_RANGE;

    bytes = (size_t)((uint64_t)count * d->block_size);
    if (d->fail_enabled && first == d->fail_block) {
        if (d->skip_writes != 0U) {
            d->skip_writes--;
        } else {
            if (d->partial_once) {
                size_t partial = d->partial_bytes;
                if (partial == 0U || partial >= bytes) partial = bytes / 2U;
                memcpy(d->bytes + (size_t)(first * d->block_size), in, partial);
                d->partial_once = 0;
            }
            return OPENFS_IO_IO_ERROR;
        }
    }

    memcpy(d->bytes + (size_t)(first * d->block_size), in, bytes);
    return OPENFS_IO_OK;
}

static openfs_io_result_t fl(void *ctx)
{
    disk_t *d = ctx;
    return d != NULL && !d->fail_flush ? OPENFS_IO_OK : OPENFS_IO_IO_ERROR;
}

static openfs_block_device_t dev(disk_t *d)
{
    openfs_block_device_t v = { d, d->block_size, d->block_count, rd, wr, fl };
    return v;
}

static void setup(disk_t *d, openfs_block_device_t *v, openfs_superblock_t *s)
{
    uint8_t uuid[16] = { 0x50U, 0x33U, 0x41U };
    memset(d, 0, sizeof(*d));
    d->block_size = BS;
    d->block_count = BC;
    d->bytes = calloc((size_t)BC, BS);
    assert(d->bytes != NULL);
    *v = dev(d);
    assert(openfs_format(v, uuid) == OPENFS_FORMAT_OK);
    assert(openfs_read_superblock(v, s) == OPENFS_FORMAT_OK);
}

static uint64_t inode_count(const openfs_superblock_t *s)
{
    return (s->inode_table_blocks * (uint64_t)s->block_size) / OPENFS_INODE_SIZE;
}

static uint64_t inode_block(const openfs_superblock_t *s, uint64_t ino)
{
    uint64_t off = (ino - 1U) * (uint64_t)OPENFS_INODE_SIZE;
    return s->inode_table_start + off / s->block_size;
}

static uint64_t file_block(openfs_block_device_t *v, const openfs_superblock_t *s,
                           const openfs_inode_t *inode, uint64_t logical)
{
    uint64_t physical = 0U;
    assert(openfs_file_map_block_device(v, s, inode, logical, &physical) == OPENFS_FILE_OK);
    return physical;
}

static int recovery_oracle(disk_t *d)
{
    openfs_block_device_t v = dev(d);
    openfs_mount_t m;
    openfs_mount_result_t mr = openfs_mount(&m, &v);
    if (mr != OPENFS_MOUNT_OK)
        return 1; /* Explicit corruption is an acceptable terminal state. */

    {
        uint64_t errors = 0U;
        if (openfs_fsck(&v, &m.superblock, &errors) != OPENFS_FSCK_OK || errors != 0U) {
            (void)openfs_unmount(&m);
            return 0;
        }
    }

    if (openfs_unmount(&m) != OPENFS_MOUNT_OK)
        return 0;

    if (openfs_mount(&m, &v) != OPENFS_MOUNT_OK)
        return 0;

    {
        uint64_t errors = 0U;
        if (openfs_fsck(&v, &m.superblock, &errors) != OPENFS_FSCK_OK || errors != 0U) {
            (void)openfs_unmount(&m);
            return 0;
        }
    }

    return openfs_unmount(&m) == OPENFS_MOUNT_OK;
}

static int write_rollback_write_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t ino = 0U;
    openfs_inode_t inode;
    uint8_t seed[BS], update[BS];
    memset(seed, 0x31, sizeof(seed));
    memset(update, 0xE7, sizeof(update));
    setup(&d, &v, &s);

    assert(openfs_path_create(&v, &s, "/double-write", OPENFS_INODE_MODE_REGULAR, &ino) == OPENFS_PATH_OK);
    assert(openfs_inode_read(&v, s.inode_table_start, ino, inode_count(&s), &inode) == OPENFS_INODE_OK);
    assert(openfs_file_write(&v, &s, &inode, 0U, seed, sizeof(seed)) == OPENFS_FILE_OK);

    uint64_t physical = file_block(&v, &s, &inode, 0U);
    uint8_t *before = malloc((size_t)d.block_count * d.block_size);
    assert(before != NULL);
    memcpy(before, d.bytes, (size_t)d.block_count * d.block_size);

    d.fail_block = physical;
    d.fail_enabled = 1;
    d.partial_once = 1;
    d.partial_bytes = 512U;

    assert(openfs_file_write(&v, &s, &inode, 0U, update, sizeof(update)) == OPENFS_FILE_CORRUPT);
    assert(memcmp(before, d.bytes, (size_t)d.block_count * d.block_size) != 0);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(before);
    free(d.bytes);
    return 0;
}

static int write_rollback_flush_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t ino = 0U;
    openfs_inode_t inode;
    uint8_t seed[BS], update[BS];
    memset(seed, 0x41, sizeof(seed));
    memset(update, 0xB2, sizeof(update));
    setup(&d, &v, &s);

    assert(openfs_path_create(&v, &s, "/double-flush", OPENFS_INODE_MODE_REGULAR, &ino) == OPENFS_PATH_OK);
    assert(openfs_inode_read(&v, s.inode_table_start, ino, inode_count(&s), &inode) == OPENFS_INODE_OK);
    assert(openfs_file_write(&v, &s, &inode, 0U, seed, sizeof(seed)) == OPENFS_FILE_OK);

    d.fail_flush = 1;
    assert(openfs_file_write(&v, &s, &inode, 0U, update, sizeof(update)) == OPENFS_FILE_CORRUPT);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(d.bytes);
    return 0;
}

static int allocator_rollback_write_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t block = 0U;
    setup(&d, &v, &s);

    uint8_t *before = malloc((size_t)d.block_count * d.block_size);
    assert(before != NULL);
    memcpy(before, d.bytes, (size_t)d.block_count * d.block_size);

    d.fail_block = s.block_bitmap_start;
    d.fail_enabled = 1;
    d.partial_once = 1;
    d.partial_bytes = 256U;

    assert(openfs_alloc_block(&v, &s, &block) == OPENFS_ALLOC_CORRUPT);
    assert(memcmp(before, d.bytes, (size_t)d.block_count * d.block_size) != 0);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(before);
    free(d.bytes);
    return 0;
}

static int inode_rollback_write_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t ino = 0U;
    setup(&d, &v, &s);

    assert(openfs_inode_alloc(&v, &s, s.root_inode, OPENFS_INODE_MODE_REGULAR, &ino) == OPENFS_INODE_ALLOC_OK);

    /* Free it cleanly first so the scenario starts from a durable baseline. */
    {
        openfs_inode_t i;
        assert(openfs_inode_read(&v, s.inode_table_start, ino, inode_count(&s), &i) == OPENFS_INODE_OK);
        i.mode = OPENFS_INODE_MODE_FREE;
        i.link_count = 0U;
        i.parent_inode = 0U;
        i.size = 0U;
        i.blocks = 0U;
        i.extent_count = 0U;
        i.flags = 0U;
        assert(openfs_inode_write(&v, s.inode_table_start, inode_count(&s), &i) == OPENFS_INODE_OK);
        assert(openfs_inode_free(&v, &s, ino) == OPENFS_INODE_ALLOC_OK);
    }

    d.fail_block = inode_block(&s, ino);
    d.fail_enabled = 1;
    d.partial_once = 1;
    d.partial_bytes = 64U;

    assert(openfs_inode_alloc(&v, &s, s.root_inode, OPENFS_INODE_MODE_REGULAR, &ino) == OPENFS_INODE_ALLOC_CORRUPT);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(d.bytes);
    return 0;
}

static int create_rollback_write_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t ino = 0U;
    setup(&d, &v, &s);

    /*
     * Creation writes the inode once.  The _as wrapper then writes uid/gid.
     * Fail that second write and keep failing the same block during rollback.
     */
    d.fail_block = inode_block(&s, 2U);
    d.fail_enabled = 1;
    d.skip_writes = 1U;
    d.partial_once = 1;
    d.partial_bytes = 64U;

    assert(openfs_path_create_as(&v, &s, "/create-double-write",
                                 OPENFS_INODE_MODE_REGULAR, 1000U, 1000U, &ino)
           == OPENFS_PATH_CORRUPT);
    d.fail_enabled = 0;
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(d.bytes);
    return 0;
}

static int unlink_rollback_write_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t ino = 0U;
    setup(&d, &v, &s);

    assert(openfs_path_create(&v, &s, "/unlink-double", OPENFS_INODE_MODE_REGULAR, &ino) == OPENFS_PATH_OK);
    d.fail_block = inode_block(&s, ino);
    d.fail_enabled = 1;
    d.partial_once = 1;
    d.partial_bytes = 64U;

    assert(openfs_path_unlink(&v, &s, "/unlink-double") == OPENFS_PATH_CORRUPT);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(d.bytes);
    return 0;
}

static int link_rollback_write_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t ino = 0U;
    setup(&d, &v, &s);

    assert(openfs_path_create(&v, &s, "/link-source", OPENFS_INODE_MODE_REGULAR, &ino) == OPENFS_PATH_OK);
    d.fail_block = inode_block(&s, ino);
    d.fail_enabled = 1;
    d.partial_once = 1;
    d.partial_bytes = 64U;

    assert(openfs_link(&v, &s, "/link-source", "/link-alias") == OPENFS_PATH_CORRUPT);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(d.bytes);
    return 0;
}

static int symlink_rollback_write_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t target = 0U;
    setup(&d, &v, &s);

    assert(openfs_path_create(&v, &s, "/symlink-target", OPENFS_INODE_MODE_REGULAR, &target) == OPENFS_PATH_OK);
    uint64_t symlink_ino = 0U;

    /*
     * The symlink path first creates the inode and then persists its target.
     * Skip that creation write so the second write to the inode block is the
     * primary failure; every subsequent write to the same block fails.
     */
    uint64_t candidate = inode_block(&s, target + 1U);
    d.fail_block = candidate;
    d.fail_enabled = 1;
    d.skip_writes = 1U;
    d.partial_once = 1;
    d.partial_bytes = 64U;

    assert(openfs_symlink(&v, &s, "/symlink-target", "/symlink-double") == OPENFS_PATH_CORRUPT);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    (void)symlink_ino;
    free(d.bytes);
    return 0;
}

static int rename_replace_rollback_write_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t src = 0U, dst = 0U;
    setup(&d, &v, &s);

    assert(openfs_path_create(&v, &s, "/rename-src", OPENFS_INODE_MODE_REGULAR, &src) == OPENFS_PATH_OK);
    assert(openfs_path_create(&v, &s, "/rename-dst", OPENFS_INODE_MODE_REGULAR, &dst) == OPENFS_PATH_OK);

    openfs_inode_t source_dir;
    assert(openfs_inode_read(&v, s.inode_table_start, s.root_inode,
                             inode_count(&s), &source_dir) == OPENFS_INODE_OK);
    uint64_t dir_block = file_block(&v, &s, &source_dir, 0U);

    /*
     * Destination removal succeeds. Source removal is the first failed
     * mutation. Rollback then attempts to restore the same directory block.
     */
    d.fail_block = dir_block;
    d.fail_enabled = 1;
    d.skip_writes = 1U;
    d.partial_once = 1;
    d.partial_bytes = 128U;

    assert(openfs_path_rename(&v, &s, "/rename-src", "/rename-dst") == OPENFS_PATH_CORRUPT);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(d.bytes);
    return 0;
}

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "openfs/allocator.h"
#include "openfs/bitmap.h"
#include "openfs/file.h"
#include "openfs/fsck.h"
#include "openfs/format.h"
#include "openfs/inode.h"
#include "openfs/inode_alloc.h"
#include "openfs/link.h"
#include "openfs/mount.h"
#include "openfs/path.h"

#define BS 4096U
#define BC 256U

typedef struct {
    uint8_t *bytes;
    uint32_t block_size;
    uint64_t block_count;
    uint64_t fail_block;
    unsigned skip_writes;
    int fail_enabled;
    int partial_once;
    size_t partial_bytes;
    int fail_flush;
} disk_t;

static openfs_io_result_t rd(void *ctx, uint64_t first, uint32_t count, void *out)
{
    disk_t *d = ctx;
    if (d == NULL || out == NULL || count == 0U ||
        first >= d->block_count || (uint64_t)count > d->block_count - first)
        return OPENFS_IO_OUT_OF_RANGE;
    memcpy(out, d->bytes + (size_t)(first * d->block_size),
           (size_t)((uint64_t)count * d->block_size));
    return OPENFS_IO_OK;
}

static openfs_io_result_t wr(void *ctx, uint64_t first, uint32_t count,
                             const void *in)
{
    disk_t *d = ctx;
    size_t bytes;
    if (d == NULL || in == NULL || count == 0U ||
        first >= d->block_count || (uint64_t)count > d->block_count - first)
        return OPENFS_IO_OUT_OF_RANGE;

    bytes = (size_t)((uint64_t)count * d->block_size);
    if (d->fail_enabled && first == d->fail_block) {
        if (d->skip_writes != 0U) {
            d->skip_writes--;
        } else {
            if (d->partial_once) {
                size_t partial = d->partial_bytes;
                if (partial == 0U || partial >= bytes) partial = bytes / 2U;
                memcpy(d->bytes + (size_t)(first * d->block_size), in, partial);
                d->partial_once = 0;
            }
            return OPENFS_IO_IO_ERROR;
        }
    }

    memcpy(d->bytes + (size_t)(first * d->block_size), in, bytes);
    return OPENFS_IO_OK;
}

static openfs_io_result_t fl(void *ctx)
{
    disk_t *d = ctx;
    return d != NULL && !d->fail_flush ? OPENFS_IO_OK : OPENFS_IO_IO_ERROR;
}

static openfs_block_device_t dev(disk_t *d)
{
    openfs_block_device_t v = { d, d->block_size, d->block_count, rd, wr, fl };
    return v;
}

static void setup(disk_t *d, openfs_block_device_t *v, openfs_superblock_t *s)
{
    uint8_t uuid[16] = { 0x50U, 0x33U, 0x41U };
    memset(d, 0, sizeof(*d));
    d->block_size = BS;
    d->block_count = BC;
    d->bytes = calloc((size_t)BC, BS);
    assert(d->bytes != NULL);
    *v = dev(d);
    assert(openfs_format(v, uuid) == OPENFS_FORMAT_OK);
    assert(openfs_read_superblock(v, s) == OPENFS_FORMAT_OK);
}

static uint64_t inode_count(const openfs_superblock_t *s)
{
    return (s->inode_table_blocks * (uint64_t)s->block_size) / OPENFS_INODE_SIZE;
}

static uint64_t inode_block(const openfs_superblock_t *s, uint64_t ino)
{
    uint64_t off = (ino - 1U) * (uint64_t)OPENFS_INODE_SIZE;
    return s->inode_table_start + off / s->block_size;
}

static uint64_t file_block(openfs_block_device_t *v, const openfs_superblock_t *s,
                           const openfs_inode_t *inode, uint64_t logical)
{
    uint64_t physical = 0U;
    assert(openfs_file_map_block_device(v, s, inode, logical, &physical) == OPENFS_FILE_OK);
    return physical;
}

static int recovery_oracle(disk_t *d)
{
    openfs_block_device_t v = dev(d);
    openfs_mount_t m;
    openfs_mount_result_t mr = openfs_mount(&m, &v);
    if (mr != OPENFS_MOUNT_OK)
        return 1; /* Explicit corruption is an acceptable terminal state. */

    {
        uint64_t errors = 0U;
        if (openfs_fsck(&v, &m.superblock, &errors) != OPENFS_FSCK_OK || errors != 0U) {
            (void)openfs_unmount(&m);
            return 0;
        }
    }

    if (openfs_unmount(&m) != OPENFS_MOUNT_OK)
        return 0;

    if (openfs_mount(&m, &v) != OPENFS_MOUNT_OK)
        return 0;

    {
        uint64_t errors = 0U;
        if (openfs_fsck(&v, &m.superblock, &errors) != OPENFS_FSCK_OK || errors != 0U) {
            (void)openfs_unmount(&m);
            return 0;
        }
    }

    return openfs_unmount(&m) == OPENFS_MOUNT_OK;
}

static int write_rollback_write_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t ino = 0U;
    openfs_inode_t inode;
    uint8_t seed[BS], update[BS];
    memset(seed, 0x31, sizeof(seed));
    memset(update, 0xE7, sizeof(update));
    setup(&d, &v, &s);

    assert(openfs_path_create(&v, &s, "/double-write", OPENFS_INODE_MODE_REGULAR, &ino) == OPENFS_PATH_OK);
    assert(openfs_inode_read(&v, s.inode_table_start, ino, inode_count(&s), &inode) == OPENFS_INODE_OK);
    assert(openfs_file_write(&v, &s, &inode, 0U, seed, sizeof(seed)) == OPENFS_FILE_OK);

    uint64_t physical = file_block(&v, &s, &inode, 0U);
    uint8_t *before = malloc((size_t)d.block_count * d.block_size);
    assert(before != NULL);
    memcpy(before, d.bytes, (size_t)d.block_count * d.block_size);

    d.fail_block = physical;
    d.fail_enabled = 1;
    d.partial_once = 1;
    d.partial_bytes = 512U;

    assert(openfs_file_write(&v, &s, &inode, 0U, update, sizeof(update)) == OPENFS_FILE_CORRUPT);
    assert(memcmp(before, d.bytes, (size_t)d.block_count * d.block_size) != 0);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(before);
    free(d.bytes);
    return 0;
}

static int write_rollback_flush_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t ino = 0U;
    openfs_inode_t inode;
    uint8_t seed[BS], update[BS];
    memset(seed, 0x41, sizeof(seed));
    memset(update, 0xB2, sizeof(update));
    setup(&d, &v, &s);

    assert(openfs_path_create(&v, &s, "/double-flush", OPENFS_INODE_MODE_REGULAR, &ino) == OPENFS_PATH_OK);
    assert(openfs_inode_read(&v, s.inode_table_start, ino, inode_count(&s), &inode) == OPENFS_INODE_OK);
    assert(openfs_file_write(&v, &s, &inode, 0U, seed, sizeof(seed)) == OPENFS_FILE_OK);

    d.fail_flush = 1;
    assert(openfs_file_write(&v, &s, &inode, 0U, update, sizeof(update)) == OPENFS_FILE_CORRUPT);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(d.bytes);
    return 0;
}

static int allocator_rollback_write_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t block = 0U;
    setup(&d, &v, &s);

    uint8_t *before = malloc((size_t)d.block_count * d.block_size);
    assert(before != NULL);
    memcpy(before, d.bytes, (size_t)d.block_count * d.block_size);

    d.fail_block = s.block_bitmap_start;
    d.fail_enabled = 1;
    d.partial_once = 1;
    d.partial_bytes = 256U;

    assert(openfs_alloc_block(&v, &s, &block) == OPENFS_ALLOC_CORRUPT);
    assert(memcmp(before, d.bytes, (size_t)d.block_count * d.block_size) != 0);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(before);
    free(d.bytes);
    return 0;
}

static int inode_rollback_write_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t ino = 0U;
    setup(&d, &v, &s);

    assert(openfs_inode_alloc(&v, &s, s.root_inode, OPENFS_INODE_MODE_REGULAR, &ino) == OPENFS_INODE_ALLOC_OK);

    /* Free it cleanly first so the scenario starts from a durable baseline. */
    {
        openfs_inode_t i;
        assert(openfs_inode_read(&v, s.inode_table_start, ino, inode_count(&s), &i) == OPENFS_INODE_OK);
        i.mode = OPENFS_INODE_MODE_FREE;
        i.link_count = 0U;
        i.parent_inode = 0U;
        i.size = 0U;
        i.blocks = 0U;
        i.extent_count = 0U;
        i.flags = 0U;
        assert(openfs_inode_write(&v, s.inode_table_start, inode_count(&s), &i) == OPENFS_INODE_OK);
        assert(openfs_inode_free(&v, &s, ino) == OPENFS_INODE_ALLOC_OK);
    }

    d.fail_block = inode_block(&s, ino);
    d.fail_enabled = 1;
    d.partial_once = 1;
    d.partial_bytes = 64U;

    assert(openfs_inode_alloc(&v, &s, s.root_inode, OPENFS_INODE_MODE_REGULAR, &ino) == OPENFS_INODE_ALLOC_CORRUPT);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(d.bytes);
    return 0;
}

static int create_rollback_flush_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t ino = 0U;
    setup(&d, &v, &s);

    d.fail_flush = 1;
    assert(openfs_path_create_as(&v, &s, "/create-double-flush",
                                 OPENFS_INODE_MODE_REGULAR, 1000U, 1000U, &ino)
           == OPENFS_PATH_CORRUPT);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(d.bytes);
    return 0;
}

static int unlink_rollback_write_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t ino = 0U;
    setup(&d, &v, &s);

    assert(openfs_path_create(&v, &s, "/unlink-double", OPENFS_INODE_MODE_REGULAR, &ino) == OPENFS_PATH_OK);
    d.fail_block = inode_block(&s, ino);
    d.fail_enabled = 1;
    d.partial_once = 1;
    d.partial_bytes = 64U;

    assert(openfs_path_unlink(&v, &s, "/unlink-double") == OPENFS_PATH_CORRUPT);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(d.bytes);
    return 0;
}

static int link_rollback_write_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t ino = 0U;
    setup(&d, &v, &s);

    assert(openfs_path_create(&v, &s, "/link-source", OPENFS_INODE_MODE_REGULAR, &ino) == OPENFS_PATH_OK);
    d.fail_block = inode_block(&s, ino);
    d.fail_enabled = 1;
    d.partial_once = 1;
    d.partial_bytes = 64U;

    assert(openfs_link(&v, &s, "/link-source", "/link-alias") == OPENFS_PATH_CORRUPT);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(d.bytes);
    return 0;
}

static int symlink_rollback_write_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t target = 0U;
    setup(&d, &v, &s);

    assert(openfs_path_create(&v, &s, "/symlink-target", OPENFS_INODE_MODE_REGULAR, &target) == OPENFS_PATH_OK);
    uint64_t symlink_ino = 0U;

    /*
     * The symlink path first creates the inode and then persists its target.
     * Skip that creation write so the second write to the inode block is the
     * primary failure; every subsequent write to the same block fails.
     */
    uint64_t candidate = inode_block(&s, target + 1U);
    d.fail_block = candidate;
    d.fail_enabled = 1;
    d.skip_writes = 1U;
    d.partial_once = 1;
    d.partial_bytes = 64U;

    assert(openfs_symlink(&v, &s, "/symlink-target", "/symlink-double") == OPENFS_PATH_CORRUPT);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    (void)symlink_ino;
    free(d.bytes);
    return 0;
}

static int rename_replace_rollback_write_failure(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    uint64_t src = 0U, dst = 0U;
    setup(&d, &v, &s);

    assert(openfs_path_create(&v, &s, "/rename-src", OPENFS_INODE_MODE_REGULAR, &src) == OPENFS_PATH_OK);
    assert(openfs_path_create(&v, &s, "/rename-dst", OPENFS_INODE_MODE_REGULAR, &dst) == OPENFS_PATH_OK);

    openfs_inode_t source_dir;
    assert(openfs_inode_read(&v, s.inode_table_start, s.root_inode,
                             inode_count(&s), &source_dir) == OPENFS_INODE_OK);
    uint64_t dir_block = file_block(&v, &s, &source_dir, 0U);

    /*
     * Destination removal succeeds. Source removal is the first failed
     * mutation. Rollback then attempts to restore the same directory block.
     */
    d.fail_block = dir_block;
    d.fail_enabled = 1;
    d.skip_writes = 1U;
    d.partial_once = 1;
    d.partial_bytes = 128U;

    assert(openfs_path_rename(&v, &s, "/rename-src", "/rename-dst") == OPENFS_PATH_CORRUPT);
    d.fail_enabled = 0;
    d.fail_flush = 0;
    assert(recovery_oracle(&d));

    free(d.bytes);
    return 0;
}

static int transaction_abort_classification(void)
{
    disk_t d; openfs_block_device_t v; openfs_superblock_t s;
    openfs_journal_t j;
    openfs_transaction_t t;
    uint64_t ino = 0U;

    setup(&d, &v, &s);
    assert(openfs_journal_open(&j, &v, &s) == OPENFS_JOURNAL_OK);
    assert(openfs_transaction_begin(&t, &v, &j) == OPENFS_TRANSACTION_OK);

    /*
     * Make a namespace mutation fail with an explicit corruption result, then
     * verify the transaction is poisoned and cannot be committed as OK.
     */
    assert(openfs_path_create_tx(&t, &s, "/tx-double", OPENFS_INODE_MODE_REGULAR, &ino) == OPENFS_PATH_OK);

    d.fail_flush = 1;
    assert(openfs_path_unlink_tx(&t, &s, "/tx-double") == OPENFS_PATH_CORRUPT);
    assert(t.failed == 1);

    assert(openfs_transaction_commit(&t) == OPENFS_TRANSACTION_IO_ERROR);
    d.fail_flush = 0;
    assert(openfs_transaction_abort(&t) == OPENFS_TRANSACTION_OK ||
           openfs_transaction_abort(&t) == OPENFS_TRANSACTION_CORRUPT);

    free(d.bytes);
    return 0;
}

int main(void)
{
    assert(write_rollback_write_failure() == 0);
    assert(write_rollback_flush_failure() == 0);
    assert(allocator_rollback_write_failure() == 0);
    assert(inode_rollback_write_failure() == 0);
    assert(create_rollback_write_failure() == 0);
    assert(unlink_rollback_write_failure() == 0);
    assert(link_rollback_write_failure() == 0);
    assert(symlink_rollback_write_failure() == 0);
    assert(rename_replace_rollback_write_failure() == 0);
    assert(transaction_abort_classification() == 0);
    return 0;
}
