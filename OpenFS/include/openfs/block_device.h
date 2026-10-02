#ifndef OPENFS_BLOCK_DEVICE_H
#define OPENFS_BLOCK_DEVICE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum openfs_io_result {
    OPENFS_IO_OK = 0,
    OPENFS_IO_INVALID_ARGUMENT = 1,
    OPENFS_IO_OUT_OF_RANGE = 2,
    OPENFS_IO_IO_ERROR = 3,
    OPENFS_IO_READ_ONLY = 4
} openfs_io_result_t;
typedef struct openfs_block_device {
    void *context;
    uint32_t block_size;
    uint64_t block_count;
    openfs_io_result_t (*read)(void *, uint64_t, uint32_t, void *);
    openfs_io_result_t (*write)(void *, uint64_t, uint32_t, const void *);
    openfs_io_result_t (*flush)(void *);
} openfs_block_device_t;
static inline int openfs_block_device_is_valid(const openfs_block_device_t *d) {
    return d != NULL && d->context != NULL && d->block_size != 0U &&
           d->block_count != 0U && d->read != NULL && d->write != NULL &&
           d->flush != NULL;
}
#ifdef __cplusplus
}
#endif
#endif
