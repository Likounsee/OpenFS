#include <fuse/fuse.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "openfs_windows_adapter.h"
#include "openfs/mount.h"
#include "openfs/path.h"
#include "openfs/file.h"
#include "openfs/inode.h"
#include "openfs/dir.h"

typedef struct {
    openfs_windows_adapter_t adapter;
    openfs_mount_t mount;
} openfs_winfsp_context_t;

static openfs_winfsp_context_t *ctx_from_fuse(void)
{
    struct fuse_context *fc = fuse_get_context();
    return fc != NULL ? (openfs_winfsp_context_t *)fc->private_data : NULL;
}

static int map_path_result(openfs_path_result_t r)
{
    switch (r) {
    case OPENFS_PATH_OK: return 0;
    case OPENFS_PATH_NOT_FOUND: return -ENOENT;
    case OPENFS_PATH_NOT_DIRECTORY: return -ENOTDIR;
    case OPENFS_PATH_EXISTS: return -EEXIST;
    case OPENFS_PATH_ACCESS_DENIED: return -EACCES;
    case OPENFS_PATH_NAME_TOO_LONG: return -ENAMETOOLONG;
    case OPENFS_PATH_NO_SPACE: return -ENOSPC;
    case OPENFS_PATH_CORRUPT: return -EIO;
    default: return -EIO;
    }
}

static int lookup_inode(openfs_winfsp_context_t *ctx, const char *path,
    openfs_inode_t *inode)
{
    if (ctx == NULL || path == NULL || inode == NULL) return -EINVAL;

    uint64_t ino = 0U;
    openfs_path_result_t r = openfs_path_lookup_follow(
        openfs_windows_adapter_device(&ctx->adapter),
        &ctx->mount.superblock, path, &ino);
    if (r != OPENFS_PATH_OK) return map_path_result(r);

    openfs_inode_result_t ir = openfs_inode_read(
        openfs_windows_adapter_device(&ctx->adapter),
        ctx->mount.superblock.inode_table_start,
        ino,
        ctx->mount.superblock.inode_table_blocks *
            ctx->mount.superblock.block_size / OPENFS_INODE_SIZE,
        inode);
    if (ir != OPENFS_INODE_OK)
        return ir == OPENFS_INODE_ACCESS_DENIED ? -EACCES : -EIO;
    return 0;
}

static void fill_stat(const openfs_inode_t *inode,
    const openfs_superblock_t *sb, struct fuse_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = (fuse_ino_t)inode->inode_number;
    st->st_mode = (fuse_mode_t)(inode->mode & OPENFS_INODE_PERMISSION_MASK);
    st->st_mode |= (fuse_mode_t)(inode->mode & OPENFS_INODE_TYPE_MASK);
    st->st_nlink = (fuse_nlink_t)(inode->link_count == 0U ? 1U : inode->link_count);
    st->st_uid = (fuse_uid_t)inode->uid;
    st->st_gid = (fuse_gid_t)inode->gid;
    st->st_size = (fuse_off_t)inode->size;
    st->st_blksize = (fuse_blksize_t)sb->block_size;
    st->st_blocks = (fuse_blkcnt_t)inode->blocks;
    st->st_atim.tv_sec = (int64_t)(inode->atime_ns / 1000000000ULL);
    st->st_atim.tv_nsec = (int64_t)(inode->atime_ns % 1000000000ULL);
    st->st_mtim.tv_sec = (int64_t)(inode->mtime_ns / 1000000000ULL);
    st->st_mtim.tv_nsec = (int64_t)(inode->mtime_ns % 1000000000ULL);
    st->st_ctim.tv_sec = (int64_t)(inode->ctime_ns / 1000000000ULL);
    st->st_ctim.tv_nsec = (int64_t)(inode->ctime_ns % 1000000000ULL);
    st->st_birthtim = st->st_ctim;
}

static int openfs_getattr(const char *path, struct fuse_stat *st)
{
    openfs_winfsp_context_t *ctx = ctx_from_fuse();
    if (ctx == NULL || st == NULL) return -EIO;

    openfs_inode_t inode;
    int r = lookup_inode(ctx, path, &inode);
    if (r != 0) return r;
    fill_stat(&inode, &ctx->mount.superblock, st);
    return 0;
}

static int decode_dir_entry(const uint8_t *raw, openfs_dir_entry_t *entry,
    char *name)
{
    if (raw == NULL || entry == NULL || name == NULL) return -EINVAL;

    int empty = 1;
    for (size_t i = 0U; i < OPENFS_DIR_ENTRY_SIZE; ++i) {
        if (raw[i] != 0U) {
            empty = 0;
            break;
        }
    }
    if (empty) return 0;
    if (memcmp(raw, "ODIR1", 5U) != 0) return -EIO;

    uint32_t stored = (uint32_t)raw[252U] |
        ((uint32_t)raw[253U] << 8U) |
        ((uint32_t)raw[254U] << 16U) |
        ((uint32_t)raw[255U] << 24U);
    uint32_t crc = openfs_crc32c(raw, 252U);
    if (stored != crc) return -EIO;

    size_t len = raw[7U];
    if (len == 0U || len > OPENFS_DIR_NAME_MAX) return -EIO;

    entry->inode_number = 0U;
    entry->generation = 0U;
    for (unsigned k = 0U; k < 8U; ++k) {
        entry->inode_number |= (uint64_t)raw[8U + k] << (8U * k);
        entry->generation |= (uint64_t)raw[16U + k] << (8U * k);
    }
    entry->type = raw[6U];
    if (entry->inode_number == 0U || entry->generation == 0U ||
        (entry->type != 1U && entry->type != 2U && entry->type != 3U))
        return -EIO;

    for (size_t i = 24U + len; i < 252U; ++i)
        if (raw[i] != 0U) return -EIO;

    if (memchr(raw + 24U, '\0', len) != NULL)
        return -EIO;

    memcpy(name, raw + 24U, len);
    name[len] = '\0';
    if (memchr(name, '/', len) != NULL) return -EIO;
    return 1;
}

static int openfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
    fuse_off_t off, struct fuse_file_info *fi)
{
    (void)fi;
    openfs_winfsp_context_t *ctx = ctx_from_fuse();
    if (ctx == NULL) return -EIO;

    openfs_inode_t dir;
    int r = lookup_inode(ctx, path, &dir);
    if (r != 0) return r;
    if ((dir.mode & OPENFS_INODE_TYPE_MASK) != OPENFS_INODE_MODE_DIRECTORY)
        return -ENOTDIR;
    if (dir.size % OPENFS_DIR_ENTRY_SIZE != 0U)
        return -EIO;

    if (off <= 0) {
        if (filler(buf, ".", NULL, 1) != 0) return 0;
        if (filler(buf, "..", NULL, 2) != 0) return 0;
    }

    uint64_t entries = dir.size / OPENFS_DIR_ENTRY_SIZE;
    uint64_t start = off > 2 ? (uint64_t)off - 2U : 0U;
    uint8_t raw[OPENFS_DIR_ENTRY_SIZE];
    char name[OPENFS_DIR_NAME_MAX + 1U];

    for (uint64_t n = start; n < entries; ++n) {
        size_t got = 0U;
        openfs_file_result_t fr = openfs_file_read(
            openfs_windows_adapter_device(&ctx->adapter),
            &ctx->mount.superblock, &dir,
            n * OPENFS_DIR_ENTRY_SIZE, raw, sizeof(raw), &got);
        if (fr != OPENFS_FILE_OK || got != sizeof(raw)) return -EIO;

        openfs_dir_entry_t entry;
        int decoded = decode_dir_entry(raw, &entry, name);
        if (decoded < 0) return -EIO;
        if (decoded == 0) continue;

        openfs_inode_t child;
        openfs_inode_result_t ir = openfs_inode_read(
            openfs_windows_adapter_device(&ctx->adapter),
            ctx->mount.superblock.inode_table_start,
            entry.inode_number,
            ctx->mount.superblock.inode_table_blocks *
                ctx->mount.superblock.block_size / OPENFS_INODE_SIZE,
            &child);
        if (ir != OPENFS_INODE_OK) return -EIO;

        struct fuse_stat st;
        fill_stat(&child, &ctx->mount.superblock, &st);
        if (filler(buf, name, &st, (fuse_off_t)(n + 3U)) != 0)
            break;
    }
    return 0;
}

static int openfs_open(const char *path, struct fuse_file_info *fi)
{
    openfs_winfsp_context_t *ctx = ctx_from_fuse();
    if (ctx == NULL || fi == NULL) return -EIO;

    if ((fi->flags & (O_WRONLY | O_RDWR | O_TRUNC)) != 0)
        return -EROFS;

    openfs_inode_t inode;
    int r = lookup_inode(ctx, path, &inode);
    if (r != 0) return r;
    if ((inode.mode & OPENFS_INODE_TYPE_MASK) == OPENFS_INODE_MODE_DIRECTORY)
        return -EISDIR;

    fi->fh = inode.inode_number;
    fi->direct_io = 0;
    return 0;
}

static int openfs_opendir(const char *path, struct fuse_file_info *fi)
{
    openfs_winfsp_context_t *ctx = ctx_from_fuse();
    if (ctx == NULL || fi == NULL) return -EIO;

    openfs_inode_t inode;
    int r = lookup_inode(ctx, path, &inode);
    if (r != 0) return r;
    if ((inode.mode & OPENFS_INODE_TYPE_MASK) != OPENFS_INODE_MODE_DIRECTORY)
        return -ENOTDIR;

    fi->fh = inode.inode_number;
    return 0;
}

static int openfs_read(const char *path, char *buf, size_t size,
    fuse_off_t off, struct fuse_file_info *fi)
{
    openfs_winfsp_context_t *ctx = ctx_from_fuse();
    if (ctx == NULL || buf == NULL || fi == NULL || off < 0) return -EINVAL;

    openfs_inode_t inode;
    if (fi->fh != 0U) {
        openfs_inode_result_t ir = openfs_inode_read(
            openfs_windows_adapter_device(&ctx->adapter),
            ctx->mount.superblock.inode_table_start, fi->fh,
            ctx->mount.superblock.inode_table_blocks *
                ctx->mount.superblock.block_size / OPENFS_INODE_SIZE,
            &inode);
        if (ir != OPENFS_INODE_OK) return -EIO;
    } else {
        int r = lookup_inode(ctx, path, &inode);
        if (r != 0) return r;
    }

    size_t got = 0U;
    openfs_file_result_t r = openfs_file_read(
        openfs_windows_adapter_device(&ctx->adapter),
        &ctx->mount.superblock, &inode, (uint64_t)off, buf, size, &got);
    if (r != OPENFS_FILE_OK) return -EIO;
    return (int)got;
}

static int openfs_statfs(const char *path, struct fuse_statvfs *st)
{
    (void)path;
    openfs_winfsp_context_t *ctx = ctx_from_fuse();
    if (ctx == NULL || st == NULL) return -EIO;

    memset(st, 0, sizeof(*st));
    st->f_bsize = ctx->mount.superblock.block_size;
    st->f_frsize = ctx->mount.superblock.block_size;
    st->f_blocks = ctx->mount.superblock.data_blocks;
    st->f_bfree = ctx->mount.superblock.data_blocks;
    st->f_bavail = ctx->mount.superblock.data_blocks;
    st->f_files = ctx->mount.superblock.inode_table_blocks *
        ctx->mount.superblock.block_size / OPENFS_INODE_SIZE;
    st->f_ffree = st->f_files;
    st->f_favail = st->f_files;
    st->f_namemax = OPENFS_DIR_NAME_MAX;
    return 0;
}

static int openfs_access(const char *path, int mask)
{
    openfs_winfsp_context_t *ctx = ctx_from_fuse();
    if (ctx == NULL) return -EIO;
    if ((mask & (W_OK | X_OK)) != 0) return -EROFS;
    openfs_inode_t inode;
    return lookup_inode(ctx, path, &inode);
}

static int openfs_readlink(const char *path, char *buf, size_t size)
{
    openfs_winfsp_context_t *ctx = ctx_from_fuse();
    if (ctx == NULL || buf == NULL || size == 0U) return -EINVAL;

    openfs_inode_t inode;
    int r = lookup_inode(ctx, path, &inode);
    if (r != 0) return r;
    if ((inode.mode & OPENFS_INODE_TYPE_MASK) != OPENFS_INODE_MODE_SYMLINK)
        return -EINVAL;
    if (inode.size >= sizeof(inode.inline_data) || inode.size + 1U > size)
        return -ENAMETOOLONG;

    memcpy(buf, inode.inline_data, (size_t)inode.size);
    buf[inode.size] = '\0';
    return 0;
}

static int openfs_read_only_write(const char *path, const char *buf,
    size_t size, fuse_off_t off, struct fuse_file_info *fi)
{
    (void)path; (void)buf; (void)size; (void)off; (void)fi;
    return -EROFS;
}

static int openfs_read_only_simple(const char *path)
{
    (void)path;
    return -EROFS;
}

static int openfs_init(struct fuse_conn_info *conn)
{
    if (conn != NULL) {
        conn->want |= FSP_FUSE_CAP_READ_ONLY;
        conn->want |= FUSE_CAP_BIG_WRITES;
    }
    return 0;
}

static struct fuse_operations openfs_operations = {
    .getattr = openfs_getattr,
    .readlink = openfs_readlink,
    .open = openfs_open,
    .read = openfs_read,
    .write = openfs_read_only_write,
    .statfs = openfs_statfs,
    .flush = NULL,
    .release = NULL,
    .fsync = NULL,
    .opendir = openfs_opendir,
    .readdir = openfs_readdir,
    .releasedir = NULL,
    .access = openfs_access,
    .create = NULL,
    .mkdir = NULL,
    .unlink = openfs_read_only_simple,
    .rmdir = openfs_read_only_simple,
    .rename = NULL,
    .chmod = NULL,
    .chown = NULL,
    .truncate = NULL,
    .ftruncate = NULL,
    .utimens = NULL,
    .init = openfs_init
};

static void usage(const wchar_t *program)
{
    fwprintf(stderr,
        L"Usage: %ls <device> <drive> [block_size]\n"
        L"Example: %ls \\\\.\\PhysicalDrive2 X: 65536\n",
        program, program);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3 || argc > 4) {
        usage(argv[0]);
        return 2;
    }

    wchar_t *end = NULL;
    unsigned long block_size = 65536UL;
    if (argc == 4) {
        block_size = wcstoul(argv[3], &end, 10);
        if (end == argv[3] || *end != L'\0' || block_size < 4096UL ||
            block_size > 65536UL ||
            (block_size & (block_size - 1UL)) != 0UL) {
            fwprintf(stderr, L"Invalid block size: %ls\n", argv[3]);
            return 2;
        }
    }

    if (wcslen(argv[2]) != 2U ||
        (argv[2][0] < L'A' || argv[2][0] > L'Z') ||
        argv[2][1] != L':') {
        fwprintf(stderr, L"Invalid drive mount point: %ls\n", argv[2]);
        return 2;
    }

    openfs_winfsp_context_t context;
    memset(&context, 0, sizeof(context));

    openfs_windows_adapter_result_t ar =
        openfs_windows_adapter_open(&context.adapter, argv[1],
            (uint32_t)block_size, 1);
    if (ar != OPENFS_WINDOWS_ADAPTER_OK) {
        fwprintf(stderr, L"Cannot open device '%ls' for OpenFS mount (adapter=%d, WindowsError=%lu).\n",
            argv[1], (int)ar,
            (unsigned long)openfs_windows_adapter_last_error(&context.adapter));
        return 1;
    }

    openfs_mount_result_t mr =
        openfs_mount(&context.mount,
            openfs_windows_adapter_device(&context.adapter));
    if (mr != OPENFS_MOUNT_OK) {
        fwprintf(stderr, L"Cannot mount OpenFS device (result=%d, WindowsError=%lu).\n",
            (int)mr,
            (unsigned long)openfs_windows_adapter_last_error(&context.adapter));
        openfs_windows_adapter_close(&context.adapter);
        return 1;
    }

    char drive[4];
    drive[0] = (char)argv[2][0];
    drive[1] = ':';
    drive[2] = '\0';

    char *fuse_argv[4];
    fuse_argv[0] = (char *)"openfs-mount-device";
    fuse_argv[1] = drive;
    fuse_argv[2] = (char *)"-f";
    fuse_argv[3] = NULL;

    printf("OpenFS mounted read-only on %s. Press Ctrl+C to unmount.\n", drive);
    fflush(stdout);

    int result = fuse_main(3, fuse_argv, &openfs_operations, &context);

    (void)openfs_unmount(&context.mount);
    (void)openfs_windows_adapter_close(&context.adapter);
    return result;
}
