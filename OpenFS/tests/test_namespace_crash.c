#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "openfs/file.h"
#include "openfs/fsck.h"
#include "openfs/format.h"
#include "openfs/link.h"
#include "openfs/mount.h"
#include "openfs/path.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define BLOCK_SIZE 4096U
#define BLOCK_COUNT 256U
#define CRASH_EXIT_CODE 137
#define SNAP_PATHS 12U
#define SNAP_CONTENT 64U

typedef enum {
    CUT_N1 = 0,
    CUT_N2,
    CUT_N3,
    CUT_N4
} namespace_cut_t;

typedef enum {
    OP_CREATE = 0,
    OP_MKDIR,
    OP_LINK,
    OP_UNLINK,
    OP_SYMLINK,
    OP_RENAME_REPLACE,
    OP_RENAME_NEW
} namespace_op_t;

typedef struct {
    FILE *file;
    uint32_t block_size;
    uint64_t block_count;
    uint64_t journal_start;
    uint64_t journal_blocks;
    namespace_cut_t cut;
    unsigned flush_count;
    int armed;
    int commit_seen;
    int final_write_seen;
} crash_disk_t;

typedef struct {
    const char *path;
    int expect_exists;
    uint64_t inode;
    uint64_t parent;
    uint64_t size;
    uint64_t blocks;
    uint64_t link_count;
    uint32_t type;
    int is_symlink;
    char symlink_target[OPENFS_PATH_MAX];
    uint8_t content[SNAP_CONTENT];
    size_t content_len;
} snapshot_entry_t;

typedef struct {
    const char *name;
    namespace_op_t op;
    const char *paths[SNAP_PATHS];
    size_t path_count;
} scenario_t;

static void crash_now(void)
{
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), CRASH_EXIT_CODE);
#else
    _exit(CRASH_EXIT_CODE);
#endif
}

static int persist(crash_disk_t *d)
{
    if (fflush(d->file) != 0) return 0;
#if defined(_WIN32)
    return _commit(_fileno(d->file)) == 0;
#else
    return fsync(fileno(d->file)) == 0;
#endif
}

static int seek_block(crash_disk_t *d, uint64_t block)
{
    uint64_t off;
    if (d == NULL || block >= d->block_count) return 0;
    off = block * (uint64_t)d->block_size;
    if (off > (uint64_t)LONG_MAX) return 0;
    return fseek(d->file, (long)off, SEEK_SET) == 0;
}

static int is_journal_block(const crash_disk_t *d, uint64_t block)
{
    return block >= d->journal_start &&
           block < d->journal_start + d->journal_blocks;
}

static openfs_io_result_t disk_read(void *ctx, uint64_t first, uint32_t count, void *out)
{
    crash_disk_t *d = ctx;
    size_t bytes;
    if (d == NULL || out == NULL || count == 0U ||
        first >= d->block_count || (uint64_t)count > d->block_count - first)
        return OPENFS_IO_OUT_OF_RANGE;
    if (!seek_block(d, first)) return OPENFS_IO_IO_ERROR;
    bytes = (size_t)((uint64_t)count * d->block_size);
    return fread(out, 1U, bytes, d->file) == bytes ? OPENFS_IO_OK : OPENFS_IO_IO_ERROR;
}

static openfs_io_result_t disk_write(void *ctx, uint64_t first, uint32_t count, const void *in)
{
    crash_disk_t *d = ctx;
    size_t bytes;
    const uint8_t *raw;

    if (d == NULL || in == NULL || count == 0U ||
        first >= d->block_count || (uint64_t)count > d->block_count - first)
        return OPENFS_IO_OUT_OF_RANGE;
    if (!seek_block(d, first)) return OPENFS_IO_IO_ERROR;

    bytes = (size_t)((uint64_t)count * d->block_size);
    if (fwrite(in, 1U, bytes, d->file) != bytes) return OPENFS_IO_IO_ERROR;
    if (!persist(d)) return OPENFS_IO_IO_ERROR;

    if (!d->armed || count != 1U) return OPENFS_IO_OK;

    raw = in;
    if (is_journal_block(d, first) &&
        memcmp(raw, OPENFS_JOURNAL_MAGIC, 5U) == 0) {
        if (raw[5U] == OPENFS_JOURNAL_DATA && d->cut == CUT_N1)
            crash_now();
        if (raw[5U] == OPENFS_JOURNAL_COMMIT)
            d->commit_seen = 1;
        return OPENFS_IO_OK;
    }

    if (d->commit_seen && d->cut == CUT_N3 && !d->final_write_seen) {
        d->final_write_seen = 1;
        crash_now();
    }

    return OPENFS_IO_OK;
}

static openfs_io_result_t disk_flush(void *ctx)
{
    crash_disk_t *d = ctx;
    if (d == NULL) return OPENFS_IO_INVALID_ARGUMENT;
    if (!persist(d)) return OPENFS_IO_IO_ERROR;

    if (!d->armed) return OPENFS_IO_OK;
    d->flush_count++;

    if (d->cut == CUT_N2 && d->flush_count == 1U)
        crash_now();
    if (d->cut == CUT_N4 && d->flush_count == 2U)
        crash_now();

    return OPENFS_IO_OK;
}

static int open_disk(crash_disk_t *d, const char *path, const char *mode)
{
    memset(d, 0, sizeof(*d));
    d->file = fopen(path, mode);
    d->block_size = BLOCK_SIZE;
    d->block_count = BLOCK_COUNT;
    return d->file != NULL;
}

static int create_disk(crash_disk_t *d, const char *path)
{
    if (!open_disk(d, path, "w+b")) return 0;
    if (fseek(d->file, (long)(BLOCK_SIZE * (uint64_t)BLOCK_COUNT - 1U), SEEK_SET) != 0)
        return 0;
    if (fputc(0, d->file) == EOF) return 0;
    return persist(d);
}

static void close_disk(crash_disk_t *d)
{
    if (d != NULL && d->file != NULL) {
        (void)fflush(d->file);
        (void)fclose(d->file);
        d->file = NULL;
    }
}

static openfs_block_device_t make_device(crash_disk_t *d)
{
    openfs_block_device_t v = {
        d, d->block_size, d->block_count, disk_read, disk_write, disk_flush
    };
    return v;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    uint8_t buffer[8192];
    size_t n;

    if (in == NULL) return 0;
    out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return 0;
    }

    while ((n = fread(buffer, 1U, sizeof(buffer), in)) != 0U) {
        if (fwrite(buffer, 1U, n, out) != n) {
            fclose(in);
            fclose(out);
            return 0;
        }
    }

    if (ferror(in) != 0) {
        fclose(in);
        fclose(out);
        return 0;
    }

    if (fflush(out) != 0) {
        fclose(in);
        fclose(out);
        return 0;
    }

    fclose(in);
    fclose(out);
    return 1;
}

static int create_file(openfs_block_device_t *d, const openfs_superblock_t *s,
                       const char *path, const char *content)
{
    uint64_t ino = 0U;
    openfs_inode_t inode;

    if (openfs_path_create(d, s, path, OPENFS_INODE_MODE_REGULAR, &ino) != OPENFS_PATH_OK)
        return 0;
    if (openfs_inode_read(d, s->inode_table_start, ino,
                          (s->inode_table_blocks * s->block_size) / OPENFS_INODE_SIZE,
                          &inode) != OPENFS_INODE_OK)
        return 0;
    if (openfs_file_write(d, s, &inode, 0U, content, strlen(content)) != OPENFS_FILE_OK)
        return 0;
    return 1;
}

static int prepare_base(const char *path, namespace_op_t op)
{
    crash_disk_t d;
    openfs_block_device_t v;
    openfs_superblock_t s;
    uint8_t uuid[16] = { 0x4EU, 0x53U, 0x50U, 0x32U };
    uint64_t ino = 0U;

    if (!create_disk(&d, path)) return 0;
    v = make_device(&d);
    if (openfs_format(&v, uuid) != OPENFS_FORMAT_OK) {
        close_disk(&d);
        return 0;
    }
    if (openfs_read_superblock(&v, &s) != OPENFS_FORMAT_OK) {
        close_disk(&d);
        return 0;
    }

    if (openfs_path_mkdir(&v, &s, "/home", &ino) != OPENFS_PATH_OK ||
        openfs_path_mkdir(&v, &s, "/home/test", &ino) != OPENFS_PATH_OK ||
        openfs_path_mkdir(&v, &s, "/home/test/dir", &ino) != OPENFS_PATH_OK)
        goto fail;

    if (!create_file(&v, &s, "/home/test/src", "SRC") ||
        !create_file(&v, &s, "/home/test/dst", "DST") ||
        !create_file(&v, &s, "/home/test/dir/file", "DIR") ||
        !create_file(&v, &s, "/home/test/target", "TARGET"))
        goto fail;

    if (op == OP_UNLINK) {
        /* Keep src as the sole link so unlink has an observable deletion. */
    }

    if (d.file == NULL || !persist(&d)) goto fail;
    close_disk(&d);
    return 1;

fail:
    close_disk(&d);
    return 0;
}

static int apply_operation(crash_disk_t *d, namespace_op_t op)
{
    openfs_block_device_t v = make_device(d);
    openfs_superblock_t s;
    openfs_journal_t j;
    openfs_transaction_t t;
    uint64_t ino = 0U;
    openfs_path_result_t pr;

    if (openfs_read_superblock(&v, &s) != OPENFS_FORMAT_OK) return 0;
    d->journal_start = s.journal_start;
    d->journal_blocks = s.journal_blocks;
    if (openfs_journal_open(&j, &v, &s) != OPENFS_JOURNAL_OK) return 0;
    if (openfs_transaction_begin(&t, &v, &j) != OPENFS_TRANSACTION_OK) return 0;

    switch (op) {
    case OP_CREATE:
        pr = openfs_path_create_tx(&t, &s, "/home/test/new", OPENFS_INODE_MODE_REGULAR, &ino);
        break;
    case OP_MKDIR:
        pr = openfs_path_mkdir_tx(&t, &s, "/home/test/newdir", &ino);
        break;
    case OP_LINK:
        pr = openfs_link_tx(&t, &s, "/home/test/src", "/home/test/alias");
        break;
    case OP_UNLINK:
        pr = openfs_path_unlink_tx(&t, &s, "/home/test/src");
        break;
    case OP_SYMLINK:
        pr = openfs_symlink_tx(&t, &s, "/home/test/target", "/home/test/symlink");
        break;
    case OP_RENAME_REPLACE:
        pr = openfs_path_rename_tx(&t, &s, "/home/test/src", "/home/test/dst");
        break;
    case OP_RENAME_NEW:
        pr = openfs_path_rename_tx(&t, &s, "/home/test/src", "/home/test/renamed");
        break;
    default:
        pr = OPENFS_PATH_INVALID_ARGUMENT;
        break;
    }

    if (pr != OPENFS_PATH_OK) {
        (void)openfs_transaction_abort(&t);
        return 0;
    }

    return openfs_transaction_commit(&t) == OPENFS_TRANSACTION_OK;
}

static int run_worker(namespace_op_t op, namespace_cut_t cut, const char *path, int crash)
{
    crash_disk_t d;

    if (!open_disk(&d, path, "r+b")) return 0;
    d.cut = cut;
    d.armed = crash;

    if (apply_operation(&d, op)) {
        close_disk(&d);
        return crash ? 0 : 1;
    }

    close_disk(&d);
    return crash ? 0 : 0;
}

static int read_snapshot_entry(openfs_block_device_t *d, const openfs_superblock_t *s,
                               snapshot_entry_t *e)
{
    uint64_t ino = 0U;
    openfs_inode_t inode;
    openfs_path_result_t r;
    size_t got = 0U;

    memset(e->symlink_target, 0, sizeof(e->symlink_target));
    e->content_len = 0U;

    r = openfs_path_lookup(d, s, e->path, &ino);
    if (r == OPENFS_PATH_NOT_FOUND) {
        e->expect_exists = 0;
        return 1;
    }
    if (r != OPENFS_PATH_OK) return 0;

    if (openfs_inode_read(d, s->inode_table_start, ino,
                          (s->inode_table_blocks * s->block_size) / OPENFS_INODE_SIZE,
                          &inode) != OPENFS_INODE_OK)
        return 0;

    e->expect_exists = 1;
    e->inode = ino;
    e->parent = inode.parent_inode;
    e->size = inode.size;
    e->blocks = inode.blocks;
    e->link_count = inode.link_count;
    e->type = inode.mode & OPENFS_INODE_TYPE_MASK;
    e->is_symlink = e->type == OPENFS_INODE_MODE_SYMLINK;

    if (e->is_symlink) {
        if (openfs_readlink(d, s, e->path, e->symlink_target,
                            sizeof(e->symlink_target)) != OPENFS_PATH_OK)
            return 0;
    } else if (e->type == OPENFS_INODE_MODE_REGULAR && inode.size != 0U) {
        size_t want = inode.size < SNAP_CONTENT ? (size_t)inode.size : SNAP_CONTENT;
        if (openfs_file_read(d, s, &inode, 0U, e->content, want, &got) != OPENFS_FILE_OK)
            return 0;
        e->content_len = got;
    }

    return 1;
}

static int snapshot_paths(openfs_block_device_t *d, const openfs_superblock_t *s,
                          const scenario_t *scenario, snapshot_entry_t *out)
{
    size_t i;
    for (i = 0U; i < scenario->path_count; ++i) {
        out[i].path = scenario->paths[i];
        if (!read_snapshot_entry(d, s, &out[i])) return 0;
    }
    return 1;
}

static int snapshot_equal(const snapshot_entry_t *a, const snapshot_entry_t *b)
{
    if (a->expect_exists != b->expect_exists) return 0;
    if (!a->expect_exists) return 1;
    if (a->parent != b->parent ||
        a->size != b->size ||
        a->blocks != b->blocks ||
        a->link_count != b->link_count ||
        a->type != b->type ||
        a->is_symlink != b->is_symlink)
        return 0;
    if (a->is_symlink && strcmp(a->symlink_target, b->symlink_target) != 0)
        return 0;
    if (a->content_len != b->content_len ||
        memcmp(a->content, b->content, a->content_len) != 0)
        return 0;
    return 1;
}

static int verify_image(const char *path, const scenario_t *scenario,
                        const snapshot_entry_t *expected)
{
    crash_disk_t d;
    openfs_block_device_t v;
    openfs_mount_t m;
    snapshot_entry_t actual[SNAP_PATHS];
    uint64_t errors = 0U;
    size_t i;

    if (!open_disk(&d, path, "r+b")) return 0;
    v = make_device(&d);

    if (openfs_mount(&m, &v) != OPENFS_MOUNT_OK) {
        close_disk(&d);
        return 0;
    }

    if (openfs_fsck(&v, &m.superblock, &errors) != OPENFS_FSCK_OK || errors != 0U) {
        (void)openfs_unmount(&m);
        close_disk(&d);
        return 0;
    }

    if (!snapshot_paths(&v, &m.superblock, scenario, actual)) {
        (void)openfs_unmount(&m);
        close_disk(&d);
        return 0;
    }

    for (i = 0U; i < scenario->path_count; ++i) {
        if (!snapshot_equal(&expected[i], &actual[i])) {
            (void)openfs_unmount(&m);
            close_disk(&d);
            return 0;
        }
    }

    if (openfs_unmount(&m) != OPENFS_MOUNT_OK) {
        close_disk(&d);
        return 0;
    }

    if (openfs_mount(&m, &v) != OPENFS_MOUNT_OK) {
        close_disk(&d);
        return 0;
    }
    errors = 0U;
    if (openfs_fsck(&v, &m.superblock, &errors) != OPENFS_FSCK_OK || errors != 0U) {
        (void)openfs_unmount(&m);
        close_disk(&d);
        return 0;
    }
    if (!snapshot_paths(&v, &m.superblock, scenario, actual)) {
        (void)openfs_unmount(&m);
        close_disk(&d);
        return 0;
    }
    for (i = 0U; i < scenario->path_count; ++i) {
        if (!snapshot_equal(&expected[i], &actual[i])) {
            (void)openfs_unmount(&m);
            close_disk(&d);
            return 0;
        }
    }

    if (openfs_unmount(&m) != OPENFS_MOUNT_OK) {
        close_disk(&d);
        return 0;
    }

    close_disk(&d);
    return 1;
}

static int reference_snapshot(const char *path, const scenario_t *scenario,
                              snapshot_entry_t *snapshot)
{
    crash_disk_t d;
    openfs_block_device_t v;
    openfs_superblock_t s;

    if (!open_disk(&d, path, "r+b")) return 0;
    v = make_device(&d);
    if (openfs_read_superblock(&v, &s) != OPENFS_FORMAT_OK ||
        !snapshot_paths(&v, &s, scenario, snapshot)) {
        close_disk(&d);
        return 0;
    }

    close_disk(&d);
    return 1;
}

static int run_child(const char *self, namespace_op_t op,
                     namespace_cut_t cut, const char *path, int crash)
{
#if defined(_WIN32)
    char command[1024];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 0U;

    if (snprintf(command, sizeof(command), "\"%s\" worker %d %d %d \"%s\"",
                 self, (int)op, (int)cut, crash, path) < 0)
        return 0;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return 0;
    (void)WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, &code)) code = 0U;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return crash ? code == CRASH_EXIT_CODE : code == 0U;
#else
    pid_t pid = fork();
    int status = 0;
    if (pid < 0) return 0;
    if (pid == 0) {
        crash_disk_t d;
        if (!open_disk(&d, path, "r+b")) _exit(2);
        d.cut = cut;
        d.armed = crash;
        if (!apply_operation(&d, op)) _exit(3);
        close_disk(&d);
        _exit(0);
    }
    if (waitpid(pid, &status, 0) != pid) return 0;
    if (crash) return WIFEXITED(status) && WEXITSTATUS(status) == CRASH_EXIT_CODE;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

static int test_case(const char *self, const scenario_t *scenario,
                     namespace_cut_t cut, unsigned serial)
{
    char base[512];
    char reference[512];
    char crashed[512];
    snapshot_entry_t expected[SNAP_PATHS];
    snapshot_entry_t initial[SNAP_PATHS];
    int ok = 0;

    if (snprintf(base, sizeof(base), "openfs-ns-%u-base.img", serial) < 0 ||
        snprintf(reference, sizeof(reference), "openfs-ns-%u-ref.img", serial) < 0 ||
        snprintf(crashed, sizeof(crashed), "openfs-ns-%u-crash.img", serial) < 0)
        return 0;

    if (!prepare_base(base, scenario->op)) {
        fprintf(stderr, "namespace stage prepare failed: %s cut=%d\\n", scenario->name, (int)cut);
        goto cleanup;
    }
    {
        crash_disk_t base_disk;
        openfs_block_device_t base_device;
        openfs_superblock_t base_superblock;
        if (!open_disk(&base_disk, base, "r+b")) {
            fprintf(stderr, "namespace stage base reopen failed: %s cut=%d\\n", scenario->name, (int)cut);
            goto cleanup;
        }
        base_device = make_device(&base_disk);
        if (openfs_read_superblock(&base_device, &base_superblock) != OPENFS_FORMAT_OK ||
            !snapshot_paths(&base_device, &base_superblock, scenario, initial)) {
            close_disk(&base_disk);
            fprintf(stderr, "namespace stage initial snapshot failed: %s cut=%d\\n", scenario->name, (int)cut);
            goto cleanup;
        }
        close_disk(&base_disk);
    }

    if (!copy_file(base, reference) || !copy_file(base, crashed)) {
        fprintf(stderr, "namespace stage copy failed: %s cut=%d\\n", scenario->name, (int)cut);
        goto cleanup;
    }

    if (!run_child(self, scenario->op, cut, reference, 0)) {
        fprintf(stderr, "namespace stage reference operation failed: %s cut=%d\\n", scenario->name, (int)cut);
        goto cleanup;
    }
    if (!reference_snapshot(reference, scenario, expected)) {
        fprintf(stderr, "namespace stage reference snapshot failed: %s cut=%d\\n", scenario->name, (int)cut);
        goto cleanup;
    }

    if (!run_child(self, scenario->op, cut, crashed, 1)) {
        fprintf(stderr, "namespace stage crash child failed: %s cut=%d\\n", scenario->name, (int)cut);
        goto cleanup;
    }

    if (cut == CUT_N1) memcpy(expected, initial, sizeof(expected));
    ok = verify_image(crashed, scenario, expected);
    if (!ok) fprintf(stderr, "namespace stage recovery oracle failed: %s cut=%d\\n", scenario->name, (int)cut);

cleanup:
    (void)remove(base);
    (void)remove(reference);
    (void)remove(crashed);
    return ok;
}

static const scenario_t scenarios[] = {
    {
        "create", OP_CREATE,
        {"/home/test/src", "/home/test/dst", "/home/test/new", "/home/test/dir"},
        4U
    },
    {
        "mkdir", OP_MKDIR,
        {"/home/test/src", "/home/test/dir", "/home/test/newdir"},
        3U
    },
    {
        "link", OP_LINK,
        {"/home/test/src", "/home/test/alias", "/home/test/dst"},
        3U
    },
    {
        "unlink", OP_UNLINK,
        {"/home/test/src", "/home/test/dst", "/home/test/dir/file"},
        3U
    },
    {
        "symlink", OP_SYMLINK,
        {"/home/test/target", "/home/test/symlink", "/home/test/src"},
        3U
    },
    {
        "rename-replace", OP_RENAME_REPLACE,
        {"/home/test/src", "/home/test/dst", "/home/test/dir/file"},
        3U
    },
    {
        "rename-new", OP_RENAME_NEW,
        {"/home/test/src", "/home/test/renamed", "/home/test/dst"},
        3U
    }
};

int main(int argc, char **argv)
{
    size_t s;
    unsigned serial = 0U;

    if (argc == 6 && strcmp(argv[1], "worker") == 0) {
        char *end1 = NULL;
        char *end2 = NULL;
        char *end3 = NULL;
        long op = strtol(argv[2], &end1, 10);
        long cut = strtol(argv[3], &end2, 10);
        long crash = strtol(argv[4], &end3, 10);
        if (end1 == argv[2] || *end1 != '\0' ||
            end2 == argv[3] || *end2 != '\0' ||
            end3 == argv[4] || *end3 != '\0')
            return 1;
        {
            crash_disk_t d;
            if (!open_disk(&d, argv[5], "r+b")) return 2;
            d.cut = (namespace_cut_t)cut;
            d.armed = crash != 0;
            if (!apply_operation(&d, (namespace_op_t)op)) return 3;
            close_disk(&d);
        }
        return 0;
    }

    if (argc != 1) return 1;

    for (s = 0U; s < sizeof(scenarios) / sizeof(scenarios[0]); ++s) {
        for (int cut = 0; cut < 4; ++cut) {
            assert(test_case(argv[0], &scenarios[s], (namespace_cut_t)cut, serial++));
        }
    }

    return 0;
}
