#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "openfs/format.h"
#include "openfs/scrub.h"

#define BS 4096U
#define BC 256U

typedef struct {
    uint8_t *bytes;
    uint32_t fail_block;
    int fail_enabled;
} disk_t;

static openfs_io_result_t rd(void *ctx, uint64_t first, uint32_t count, void *out)
{
    disk_t *d = (disk_t *)ctx;
    if (count == 0U || first >= BC || (uint64_t)count > BC - first)
        return OPENFS_IO_OUT_OF_RANGE;
    if (d->fail_enabled && first <= d->fail_block && d->fail_block < first + count)
        return OPENFS_IO_IO_ERROR;
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
    d.fail_block = s.inode_table_start;
    d.fail_enabled = 1;
    assert(openfs_scrub(&v, &s, &errors) == OPENFS_SCRUB_IO_ERROR);
    assert(errors == 1U);
    cleanup(&d);

    return 0;
}
