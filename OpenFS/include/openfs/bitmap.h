#ifndef OPENFS_BITMAP_H
#define OPENFS_BITMAP_H
#include <stdint.h>
#include "openfs/block_device.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { OPENFS_BITMAP_OK=0, OPENFS_BITMAP_INVALID_ARGUMENT=1, OPENFS_BITMAP_OUT_OF_RANGE=2, OPENFS_BITMAP_IO_ERROR=3 } openfs_bitmap_result_t;
openfs_bitmap_result_t openfs_bitmap_test(const openfs_block_device_t *,uint64_t,uint64_t,uint64_t,int *);
openfs_bitmap_result_t openfs_bitmap_set(const openfs_block_device_t *,uint64_t,uint64_t,uint64_t,int);
#ifdef __cplusplus
}
#endif
#endif
