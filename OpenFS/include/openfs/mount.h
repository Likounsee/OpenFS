#ifndef OPENFS_MOUNT_H
#define OPENFS_MOUNT_H

#include "openfs/format.h"
#include "openfs/journal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OPENFS_MOUNT_OK = 0,
    OPENFS_MOUNT_INVALID_ARGUMENT = 1,
    OPENFS_MOUNT_CORRUPT = 2,
    OPENFS_MOUNT_IO_ERROR = 3
} openfs_mount_result_t;

typedef struct {
    openfs_block_device_t *device;
    openfs_superblock_t superblock;
    int mounted;
    openfs_journal_t journal;
} openfs_mount_t;

openfs_mount_result_t openfs_mount(openfs_mount_t *, openfs_block_device_t *);
openfs_mount_result_t openfs_sync(openfs_mount_t *);
openfs_mount_result_t openfs_unmount(openfs_mount_t *);

#ifdef __cplusplus
}
#endif
#endif
