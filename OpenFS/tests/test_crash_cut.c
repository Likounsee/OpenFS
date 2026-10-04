#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "openfs/fsck.h"
#include "openfs/format.h"
#include "openfs/journal.h"
#include "openfs/mount.h"
#include "openfs/transaction.h"

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#else
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define BLOCK_SIZE 4096U
#define BLOCK_COUNT 256U
#define CRASH_EXIT_CODE 137

typedef enum crash_cut {
    CUT_A = 0,
    CUT_B,
    CUT_C,
    CUT_D
} crash_cut_t;

typedef struct crash_disk {
    FILE *file;
    uint32_t block_size;
    uint64_t block_count;
    uint64_t journal_start;
    uint64_t journal_blocks;
    uint64_t target_block;
    crash_cut_t cut;
    unsigned flush_count;
    int armed;
} crash_disk_t;

static void die_now(void)
{
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), CRASH_EXIT_CODE);
#else
    _exit(CRASH_EXIT_CODE);
#endif
}

static int persist(crash_disk_t *d)
{
    return fflush(d->file) == 0;
}

static int seek_block(crash_disk_t *d, uint64_t block)
{
    if (block >= d->block_count) return 0;
    uint64_t off = block * (uint64_t)d->block_size;
    if (off > (uint64_t)LONG_MAX) return 0;
    return fseek(d->file, (long)off, SEEK_SET) == 0;
}

static openfs_io_result_t disk_read(void *ctx, uint64_t first, uint32_t count, void *out)
{
    crash_disk_t *d = ctx;
    if (d == NULL || out == NULL || count == 0U ||
        first >= d->block_count || (uint64_t)count > d->block_count - first)
        return OPENFS_IO_OUT_OF_RANGE;
    if (!seek_block(d, first)) return OPENFS_IO_IO_ERROR;
    size_t bytes = (size_t)((uint64_t)count * d->block_size);
    return fread(out, 1U, bytes, d->file) == bytes ? OPENFS_IO_OK : OPENFS_IO_IO_ERROR;
}

static int is_journal_block(const crash_disk_t *d, uint64_t block)
{
    return block >= d->journal_start &&
           block < d->journal_start + d->journal_blocks;
}

static openfs_io_result_t disk_write(void *ctx, uint64_t first, uint32_t count, const void *in)
{
    crash_disk_t *d = ctx;
    if (d == NULL || in == NULL || count == 0U ||
        first >= d->block_count || (uint64_t)count > d->block_count - first)
        return OPENFS_IO_OUT_OF_RANGE;
    if (!seek_block(d, first)) return OPENFS_IO_IO_ERROR;

    size_t bytes = (size_t)((uint64_t)count * d->block_size);
    if (fwrite(in, 1U, bytes, d->file) != bytes) return OPENFS_IO_IO_ERROR;
    if (!persist(d)) return OPENFS_IO_IO_ERROR;

    if (!d->armed || count != 1U) return OPENFS_IO_OK;

    if (d->cut == CUT_A && is_journal_block(d, first)) {
        const uint8_t *raw = in;
        if (memcmp(raw, OPENFS_JOURNAL_MAGIC, 5U) == 0 &&
            raw[5U] == OPENFS_JOURNAL_DATA)
            die_now();
    }

    if (d->cut == CUT_C && first == d->target_block)
        die_now();

    return OPENFS_IO_OK;
}

static openfs_io_result_t disk_flush(void *ctx)
{
    crash_disk_t *d = ctx;
    if (d == NULL) return OPENFS_IO_INVALID_ARGUMENT;
    if (!persist(d)) return OPENFS_IO_IO_ERROR;
    d->flush_count++;
    if (!d->armed) return OPENFS_IO_OK;

    if ((d->cut == CUT_B && d->flush_count == 1U) ||
        (d->cut == CUT_D && d->flush_count == 2U))
        die_now();

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
    if (fseek(d->file, (long)(BLOCK_SIZE * (uint64_t)BLOCK_COUNT - 1U), SEEK_SET) != 0) return 0;
    if (fputc(0, d->file) == EOF) return 0;
    return persist(d);
}

static void close_disk(crash_disk_t *d)
{
    if (d->file != NULL) {
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

static int worker(crash_cut_t cut, const char *path)
{
    crash_disk_t d;
    if (!create_disk(&d, path)) return 2;

    openfs_block_device_t v = make_device(&d);
    uint8_t uuid[16] = { 0x42U, 0x50U, 0x32U };
    if (openfs_format(&v, uuid) != OPENFS_FORMAT_OK) return 3;

    openfs_superblock_t s;
    if (openfs_read_superblock(&v, &s) != OPENFS_FORMAT_OK) return 4;

    openfs_journal_t j;
    if (openfs_journal_open(&j, &v, &s) != OPENFS_JOURNAL_OK) return 5;

    openfs_transaction_t t;
    if (openfs_transaction_begin(&t, &v, &j) != OPENFS_TRANSACTION_OK) return 6;

    d.journal_start = s.journal_start;
    d.journal_blocks = s.journal_blocks;
    d.target_block = s.data_start + 8U;
    d.cut = cut;
    d.armed = 1;

    uint8_t pattern[BLOCK_SIZE];
    memset(pattern, 0xA7U, sizeof(pattern));
    openfs_block_device_t *td = openfs_transaction_device(&t);
    if (td == NULL || td->write(td->context, d.target_block, 1U, pattern) != OPENFS_IO_OK)
        return 7;

    (void)openfs_transaction_commit(&t);
    return 8;
}

static int run_worker(const char *self, crash_cut_t cut, const char *path)
{
#if defined(_WIN32)
    char command[1024];
    if (snprintf(command, sizeof(command), "\"%s\" worker %d \"%s\"", self, (int)cut, path) < 0)
        return 0;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0U;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == CRASH_EXIT_CODE;
#else
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        char cut_arg[16];
        snprintf(cut_arg, sizeof(cut_arg), "%d", (int)cut);
        execl(self, self, "worker", cut_arg, path, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == CRASH_EXIT_CODE;
#endif
}

static int verify_recovery(crash_cut_t cut, const char *path)
{
    crash_disk_t d;
    if (!open_disk(&d, path, "r+b")) return 0;
    openfs_block_device_t v = make_device(&d);

    openfs_mount_t m;
    openfs_mount_result_t mount_result = openfs_mount(&m, &v);
    if (mount_result != OPENFS_MOUNT_OK) {
        close_disk(&d);
        return 0;
    }

    uint8_t raw[BLOCK_SIZE];
    if (disk_read(&d, m.superblock.data_start + 8U, 1U, raw) != OPENFS_IO_OK) {
        (void)openfs_unmount(&m);
        close_disk(&d);
        return 0;
    }

    uint8_t expected[BLOCK_SIZE];
    memset(expected, 0xA7U, sizeof(expected));
    int mutation_present = memcmp(raw, expected, sizeof(raw)) == 0;
    int expected_present = cut != CUT_A;

    uint64_t errors = 0U;
    int fsck_ok = openfs_fsck(&v, &m.superblock, &errors) == OPENFS_FSCK_OK && errors == 0U;

    openfs_journal_t j;
    int journal_clean = openfs_journal_open(&j, &v, &m.superblock) == OPENFS_JOURNAL_OK &&
                        j.next_record == 0U &&
                        j.active_transaction_id == 0U &&
                        j.commit_record_written == 0U;

    int stable_remount = openfs_unmount(&m) == OPENFS_MOUNT_OK;
    if (stable_remount) {
        openfs_mount_t second;
        stable_remount = openfs_mount(&second, &v) == OPENFS_MOUNT_OK;
        if (stable_remount) {
            uint64_t second_errors = 0U;
            stable_remount = openfs_fsck(&v, &second.superblock, &second_errors) == OPENFS_FSCK_OK &&
                             second_errors == 0U;
            if (stable_remount) stable_remount = openfs_unmount(&second) == OPENFS_MOUNT_OK;
        }
    }

    int ok = mutation_present == expected_present && fsck_ok && journal_clean && stable_remount;
    fprintf(stderr, "crash-cut %d: mount=%d mutation=%d expected=%d fsck=%d journal=%d remount=%d\\n", (int)cut, (int)mount_result, mutation_present, expected_present, fsck_ok, journal_clean, stable_remount);
    close_disk(&d);
    return ok;
}

static int crash_cut_test(crash_cut_t cut, const char *self)
{
    char path[512];
#if defined(_WIN32)
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    if (snprintf(path, sizeof(path), "openfs-crash-cut-%lu-%d.img", pid, (int)cut) < 0)
        return 0;

    int crashed = run_worker(self, cut, path);
    int recovered = crashed && verify_recovery(cut, path);
    (void)remove(path);
    return recovered;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "worker") == 0) {
        char *end = NULL;
        long value = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || value < CUT_A || value > CUT_D) return 1;
        return worker((crash_cut_t)value, argv[3]);
    }

    if (argc != 1) return 1;

    assert(crash_cut_test(CUT_A, argv[0]));
    assert(crash_cut_test(CUT_B, argv[0]));
    assert(crash_cut_test(CUT_C, argv[0]));
    assert(crash_cut_test(CUT_D, argv[0]));
    return 0;
}
