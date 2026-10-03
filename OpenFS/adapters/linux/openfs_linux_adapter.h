#ifndef OPENFS_LINUX_ADAPTER_H
#define OPENFS_LINUX_ADAPTER_H

#include <stdint.h>
#include "openfs/block_device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OPENFS_LINUX_ADAPTER_OK = 0,
    OPENFS_LINUX_ADAPTER_INVALID_ARGUMENT = 1,
    OPENFS_LINUX_ADAPTER_IO_ERROR = 2,
    OPENFS_LINUX_ADAPTER_READ_ONLY = 3,
    OPENFS_LINUX_ADAPTER_UNSUPPORTED = 4
} openfs_linux_adapter_result_t;

typedef struct {
    int fd;
    int writable;
    openfs_block_device_t device;
} openfs_linux_adapter_t;

openfs_linux_adapter_result_t openfs_linux_adapter_open(
    openfs_linux_adapter_t *adapter,
    const char *path,
    uint32_t block_size,
    int writable);

openfs_linux_adapter_result_t openfs_linux_adapter_close(
    openfs_linux_adapter_t *adapter);

openfs_block_device_t *openfs_linux_adapter_device(
    openfs_linux_adapter_t *adapter);

#ifdef __cplusplus
}
#endif
#endif
