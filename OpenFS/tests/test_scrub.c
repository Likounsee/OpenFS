#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "openfs/format.h"
#include "openfs/scrub.h"
#include "openfs/mount.h"
#include "openfs/path.h"
#include "openfs/inode.h"
#include "openfs/file.h"

#define BS 4096U
#define BC 256U

typedef struct {
    uint8_t *bytes;
    uint32_t fail_block;
    int fail_enabled;
    uint32_t fail_read_match;
    uint32_t matching_reads;
} disk_t;

static openfs_io_result_t rd(void *ctx, uint64_t first, uint32_t count, void *out)
{
    disk_t *d = (disk_t *)ctx;
    if (count == 0U || first >= BC || (uint64_t)count > BC - first)
        return OPENFS_IO_OUT_OF_RANGE;
    if (d->fail_enabled && first <= d->fail_block && d->fail_block < first + count) {
        ++d->matching_reads;
        if (d->fail_read_match == 0U || d->matching_reads == d->fail_read_match)
            return OPENFS_IO_IO_ERROR;
    }
    memcpy(out, d->bytes + first * (uint64_t)BS, (size_t)count * BS);
    return OPENFS_IO_OK;
}

static openfs_io_result_t wr(void *ctx, uint64_t first, uint32_t count, const void *in)
{
    disk_t *d = (disk_t *)ctx;
    if (count == 0U || first >= BC || (uint64_t)count > BC - first)
        return OPENFS_IO_OUT_OF_RANGE;
    memcpy(d->bytes + first * (uint64_t)BS, in, (size_t)count * BS);
    return OPENFS_IO_OK;
}

static openfs_io_result_t fl(void *ctx)
{
    (void)ctx;
    return OPENFS_IO_OK;
}

static openfs_block_device_t device(disk_t *d)
{
    openfs_block_device_t v = { d, BS, BC, rd, wr, fl };
    return v;
}

static void setup(disk_t *d, openfs_block_device_t *v, openfs_superblock_t *s)
{
    memset(d, 0, sizeof(*d));
    d->bytes = (uint8_t *)calloc(BC, BS);
    assert(d->bytes != NULL);
    *v = device(d);
    uint8_t uuid[16] = { 0x53U, 0x43U, 0x52U, 0x55U, 0x42U };
    assert(openfs_format(v, uuid) == OPENFS_FORMAT_OK);
    assert(openfs_read_superblock(v, s) == OPENFS_FORMAT_OK);
}

static void cleanup(disk_t *d)
{
    free(d->bytes);
    d->bytes = NULL;
}

static void corrupt_inode(disk_t *d, const openfs_superblock_t *s)
{
    uint64_t byte_offset = s->inode_table_start * (uint64_t)BS + 32U;
    d->bytes[byte_offset] ^= 0x80U;
}

int main(void)
{
    disk_t d;
    openfs_block_device_t v;
    openfs_superblock_t s;
    uint64_t errors = 0U;

    setup(&d, &v, &s);
    assert(openfs_scrub(&v, &s, &errors) == OPENFS_SCRUB_OK);
    assert(errors == 0U);
    cleanup(&d);

    setup(&d, &v, &s);
    corrupt_inode(&d, &s);
    assert(openfs_scrub(&v, &s, &errors) == OPENFS_SCRUB_CORRUPT);
    assert(errors != 0U);
    cleanup(&d);

    setup(&d, &v, &s);
    d.fail_block = (uint32_t)s.inode_table_start;
    d.fail_enabled = 1;
    assert(openfs_scrub(&v, &s, &errors) == OPENFS_SCRUB_IO_ERROR);
    assert(errors == 1U);
    cleanup(&d);

    /* A checksum-table read failure must be reported as I/O, not corruption. */
    setup(&d, &v, &s);
    openfs_mount_t mount = { 0 };
    assert(openfs_mount(&mount, &v) == OPENFS_MOUNT_OK);
    uint64_t ino = 0U;
    assert(openfs_path_create(&v, &mount.superblock, "/scrub-io",
                              OPENFS_INODE_MODE_REGULAR, &ino) == OPENFS_PATH_OK);
    uint64_t inode_count = mount.superblock.inode_table_blocks *
                           (uint64_t)mount.superblock.block_size /
                           OPENFS_INODE_SIZE;
    openfs_inode_t inode;
    assert(openfs_inode_read(&v, mount.superblock.inode_table_start, ino,
                             inode_count, &inode) == OPENFS_INODE_OK);
    uint8_t payload[BS];
    memset(payload, 0xA7, sizeof(payload));
    assert(openfs_file_write(&v, &mount.superblock, &inode, 0U,
                             payload, sizeof(payload)) == OPENFS_FILE_OK);
    d.fail_block = (uint32_t)mount.superblock.data_checksum_start;
    d.fail_read_match = 2U; /* scan read succeeds; checksum lookup read fails */
    d.matching_reads = 0U;
    d.fail_enabled = 1;
    assert(openfs_scrub(&v, &mount.superblock, &errors) == OPENFS_SCRUB_IO_ERROR);
    assert(errors == 1U);
    d.fail_enabled = 0;
    assert(openfs_unmount(&mount) == OPENFS_MOUNT_OK);
    cleanup(&d);

    return 0;
}
