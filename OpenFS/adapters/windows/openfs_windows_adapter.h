#ifndef OPENFS_WINDOWS_ADAPTER_H
#define OPENFS_WINDOWS_ADAPTER_H

#include <stdint.h>
#include "openfs/block_device.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OPENFS_WINDOWS_ADAPTER_OK = 0,
    OPENFS_WINDOWS_ADAPTER_INVALID_ARGUMENT = 1,
    OPENFS_WINDOWS_ADAPTER_IO_ERROR = 2,
    OPENFS_WINDOWS_ADAPTER_READ_ONLY = 3,
    OPENFS_WINDOWS_ADAPTER_UNSUPPORTED = 4
} openfs_windows_adapter_result_t;

typedef struct {
#ifdef _WIN32
    HANDLE handle;
#endif
    int writable;
#ifdef _WIN32
    DWORD last_error;
#endif
    openfs_block_device_t device;
} openfs_windows_adapter_t;

openfs_windows_adapter_result_t openfs_windows_adapter_open(
    openfs_windows_adapter_t *adapter,
    const wchar_t *path,
    uint32_t block_size,
    int writable);

openfs_windows_adapter_result_t openfs_windows_adapter_close(
    openfs_windows_adapter_t *adapter);

openfs_block_device_t *openfs_windows_adapter_device(
    openfs_windows_adapter_t *adapter);

DWORD openfs_windows_adapter_last_error(const openfs_windows_adapter_t *adapter);

#ifdef __cplusplus
}
#endif
#endif
