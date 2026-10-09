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

typedef struct {
    uint64_t block;
    uint32_t issue;
    unsigned calls;
} scrub_report_t;

static void record_scrub_issue(void *context, uint64_t block, uint32_t issue)
{
    scrub_report_t *report = (scrub_report_t *)context;
    report->block = block;
    report->issue = issue;
    report->calls++;
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

    /* Detailed scrub reports the exact block for a data checksum mismatch. */
    setup(&d, &v, &s);
    openfs_mount_t report_mount = { 0 };
    assert(openfs_mount(&report_mount, &v) == OPENFS_MOUNT_OK);
    uint64_t report_ino = 0U;
    assert(openfs_path_create(&v, &report_mount.superblock, "/scrub-report",
                              OPENFS_INODE_MODE_REGULAR, &report_ino) == OPENFS_PATH_OK);
    uint64_t report_inode_count = report_mount.superblock.inode_table_blocks *
                                  (uint64_t)report_mount.superblock.block_size /
                                  OPENFS_INODE_SIZE;
    openfs_inode_t report_inode;
    assert(openfs_inode_read(&v, report_mount.superblock.inode_table_start,
                             report_ino, report_inode_count,
                             &report_inode) == OPENFS_INODE_OK);
    uint8_t report_payload[BS];
    memset(report_payload, 0xC3, sizeof(report_payload));
    assert(openfs_file_write(&v, &report_mount.superblock, &report_inode, 0U,
                             report_payload, sizeof(report_payload)) == OPENFS_FILE_OK);
    uint64_t data_block = 0U;
    assert(openfs_file_map_block_device(&v, &report_mount.superblock,
                                        &report_inode, 0U, &data_block) == OPENFS_FILE_OK);
    d.bytes[data_block * (uint64_t)BS] ^= 0x01U;
    scrub_report_t report = { UINT64_MAX, 0U, 0U };
    assert(openfs_scrub_ex(&v, &report_mount.superblock, &errors,
                           record_scrub_issue, &report) == OPENFS_SCRUB_CORRUPT);
    assert(errors == 1U);
    assert(report.calls == 1U);
    assert(report.block == data_block);
    assert(report.issue == OPENFS_SCRUB_ISSUE_CHECKSUM);
    assert(openfs_unmount(&report_mount) == OPENFS_MOUNT_OK);
    cleanup(&d);

    /* Continue after a checksum-table I/O error and report later corruption. */
    setup(&d, &v, &s);
    openfs_mount_t multi_mount = { 0 };
    assert(openfs_mount(&multi_mount, &v) == OPENFS_MOUNT_OK);
    uint64_t multi_ino = 0U;
    assert(openfs_path_create(&v, &multi_mount.superblock, "/scrub-multiple-issues",
                              OPENFS_INODE_MODE_REGULAR, &multi_ino) == OPENFS_PATH_OK);
    uint64_t multi_inode_count = multi_mount.superblock.inode_table_blocks *
                                 (uint64_t)multi_mount.superblock.block_size /
                                 OPENFS_INODE_SIZE;
    openfs_inode_t multi_inode;
    assert(openfs_inode_read(&v, multi_mount.superblock.inode_table_start,
                             multi_ino, multi_inode_count, &multi_inode) == OPENFS_INODE_OK);
    assert(openfs_file_write(&v, &multi_mount.superblock, &multi_inode, 0U,
                             report_payload, sizeof(report_payload)) == OPENFS_FILE_OK);
    assert(openfs_file_write(&v, &multi_mount.superblock, &multi_inode, BS,
                             report_payload, sizeof(report_payload)) == OPENFS_FILE_OK);
    uint64_t second_data_block = 0U;
    assert(openfs_file_map_block_device(&v, &multi_mount.superblock, &multi_inode,
                                        1U, &second_data_block) == OPENFS_FILE_OK);
    d.bytes[second_data_block * (uint64_t)BS] ^= 0x80U;
    d.fail_block = (uint32_t)multi_mount.superblock.data_checksum_start;
    d.fail_read_match = 2U; /* fail first checksum lookup after the scan read */
    d.matching_reads = 0U;
    d.fail_enabled = 1;
    report = (scrub_report_t){ UINT64_MAX, 0U, 0U };
    assert(openfs_scrub_ex(&v, &multi_mount.superblock, &errors,
                           record_scrub_issue, &report) == OPENFS_SCRUB_IO_ERROR);
    assert(errors == 2U);
    assert(report.calls == 2U);
    assert(report.block == second_data_block);
    assert(report.issue == OPENFS_SCRUB_ISSUE_CHECKSUM);
    d.fail_enabled = 0;
    assert(openfs_unmount(&multi_mount) == OPENFS_MOUNT_OK);
    cleanup(&d);
    /* Checksum-table I/O errors identify the data block being verified. */
    setup(&d, &v, &s);
    openfs_mount_t io_mount = { 0 };
    assert(openfs_mount(&io_mount, &v) == OPENFS_MOUNT_OK);
    uint64_t io_ino = 0U;
    assert(openfs_path_create(&v, &io_mount.superblock, "/scrub-report-io",
                              OPENFS_INODE_MODE_REGULAR, &io_ino) == OPENFS_PATH_OK);
    uint64_t io_inode_count = io_mount.superblock.inode_table_blocks *
                              (uint64_t)io_mount.superblock.block_size /
                              OPENFS_INODE_SIZE;
    openfs_inode_t io_inode;
    assert(openfs_inode_read(&v, io_mount.superblock.inode_table_start,
                             io_ino, io_inode_count, &io_inode) == OPENFS_INODE_OK);
    assert(openfs_file_write(&v, &io_mount.superblock, &io_inode, 0U,
                             report_payload, sizeof(report_payload)) == OPENFS_FILE_OK);
    uint64_t io_data_block = 0U;
    assert(openfs_file_map_block_device(&v, &io_mount.superblock, &io_inode,
                                        0U, &io_data_block) == OPENFS_FILE_OK);
    d.fail_block = (uint32_t)io_mount.superblock.data_checksum_start;
    /* The raw scan reads the checksum table once. Skip checksum lookups for
       earlier allocated blocks so the injected failure targets this file. */
    d.fail_read_match = 2U;
    for (uint64_t b = io_mount.superblock.data_start; b < io_data_block; ++b) {
        if (b == io_mount.superblock.metadata_root_block) continue;
        int used = 0;
        assert(openfs_bitmap_test(&v, io_mount.superblock.block_bitmap_start,
                                  io_mount.superblock.block_bitmap_blocks,
                                  b, &used) == OPENFS_BITMAP_OK);
        if (used) d.fail_read_match++;
    }
    d.matching_reads = 0U;
    d.fail_enabled = 1;
    report = (scrub_report_t){ UINT64_MAX, 0U, 0U };
    assert(openfs_scrub_ex(&v, &io_mount.superblock, &errors,
                           record_scrub_issue, &report) == OPENFS_SCRUB_IO_ERROR);
    assert(errors == 1U);
    assert(report.calls == 1U);
    assert(report.block == io_data_block);
    assert(report.issue == OPENFS_SCRUB_ISSUE_READ);
    d.fail_enabled = 0;
    assert(openfs_unmount(&io_mount) == OPENFS_MOUNT_OK);
    cleanup(&d);

    return 0;
}
