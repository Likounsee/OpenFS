#include "OpenFSWritePlan.h"
#include <assert.h>
#include <stdio.h>

static void
test_multi_extent_partial_range(void)
{
    const OPENFS_EXTENT extents[] = {
        { 0, 100, 2 },
        { 2, 200, 3 }
    };
    ULONGLONG blocks[3] = { 0, 0, 0 };
    ULONG required = 0;

    assert(OpenFsBuildWriteBlockPlan(extents, 2, 5u * 4096u,
        4096u, 4090u, 8198u, 50, 900, 1000,
        blocks, 3, &required) == STATUS_SUCCESS);
    assert(required == 3);
    assert(blocks[0] == 100);
    assert(blocks[1] == 101);
    assert(blocks[2] == 200);
}

static void
test_empty_write_is_noop(void)
{
    ULONG required = 99;
    assert(OpenFsBuildWriteBlockPlan(NULL, 0, 0, 4096,
        MAXULONGLONG, 0, 10, 90, 100, NULL, 0,
        &required) == STATUS_SUCCESS);
    assert(required == 0);
}

static void
test_rejects_eof_extension(void)
{
    const OPENFS_EXTENT extents[] = { { 0, 100, 1 } };
    ULONGLONG blocks[2];
    ULONG required = 0;
    assert(OpenFsBuildWriteBlockPlan(extents, 1, 4096, 4096,
        4095, 2, 50, 900, 1000, blocks, 2,
        &required) == STATUS_INVALID_PARAMETER);
}

static void
test_rejects_sparse_hole(void)
{
    const OPENFS_EXTENT extents[] = { { 0, 100, 1 }, { 2, 200, 1 } };
    ULONGLONG blocks[2];
    ULONG required = 0;
    assert(OpenFsBuildWriteBlockPlan(extents, 2, 3u * 4096u, 4096,
        4096, 4096, 50, 900, 1000, blocks, 2,
        &required) == STATUS_NOT_FOUND);
    assert(required == 1);
}

static void
test_capacity_preflight(void)
{
    const OPENFS_EXTENT extents[] = { { 0, 100, 4 } };
    ULONGLONG blocks[1];
    ULONG required = 0;
    assert(OpenFsBuildWriteBlockPlan(extents, 1, 4u * 4096u, 4096,
        0, 3u * 4096u, 50, 900, 1000, blocks, 1,
        &required) == STATUS_BUFFER_TOO_SMALL);
    assert(required == 3);
}

static void
test_rejects_aliased_physical_blocks(void)
{
    const OPENFS_EXTENT extents[] = { { 0, 100, 1 }, { 1, 100, 1 } };
    ULONGLONG blocks[2];
    ULONG required = 0;
    assert(OpenFsBuildWriteBlockPlan(extents, 2, 2u * 4096u, 4096,
        0, 2u * 4096u, 50, 900, 1000, blocks, 2,
        &required) == STATUS_DISK_CORRUPT_ERROR);
}

int
main(void)
{
    test_multi_extent_partial_range();
    test_empty_write_is_noop();
    test_rejects_eof_extension();
    test_rejects_sparse_hole();
    test_capacity_preflight();
    test_rejects_aliased_physical_blocks();
    puts("OpenFS write-plan tests passed");
    return 0;
}
