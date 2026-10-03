#ifndef OPENFS_ARCHIAOS_ADAPTER_H
#define OPENFS_ARCHIAOS_ADAPTER_H

#include <stdint.h>
#include "openfs/block_device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef openfs_io_result_t (*openfs_archiaos_read_fn)(void *context,uint64_t first_block,uint32_t count,void *buffer);
typedef openfs_io_result_t (*openfs_archiaos_write_fn)(void *context,uint64_t first_block,uint32_t count,const void *buffer);
typedef openfs_io_result_t (*openfs_archiaos_flush_fn)(void *context);

typedef struct {
    void *context;
    uint32_t block_size;
    uint64_t block_count;
    openfs_archiaos_read_fn read;
    openfs_archiaos_write_fn write;
    openfs_archiaos_flush_fn flush;
} openfs_archiaos_storage_t;

typedef struct {
    openfs_block_device_t device;
    openfs_archiaos_storage_t storage;
} openfs_archiaos_adapter_t;

int openfs_archiaos_adapter_init(
    openfs_archiaos_adapter_t *adapter,
    const openfs_archiaos_storage_t *storage);

openfs_block_device_t *openfs_archiaos_adapter_device(
    openfs_archiaos_adapter_t *adapter);

#ifdef __cplusplus
}
#endif
#endif
