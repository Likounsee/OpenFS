#ifndef OPENFS_DATA_CHECKSUM_H
#define OPENFS_DATA_CHECKSUM_H
#include <stdint.h>
#include "openfs/block_device.h"
#include "openfs/format.h"
#include "openfs/transaction.h"
#ifdef __cplusplus
extern "C" {
#endif
uint32_t openfs_data_checksum(const void *, uint32_t);
int openfs_data_checksum_get(const openfs_block_device_t *, const openfs_superblock_t *, uint64_t, uint32_t *);
int openfs_data_checksum_set(const openfs_block_device_t *, const openfs_superblock_t *, uint64_t, uint32_t);
int openfs_data_checksum_set_tx(openfs_transaction_t *, const openfs_superblock_t *, uint64_t, uint32_t);
#ifdef __cplusplus
}
#endif
#endif
