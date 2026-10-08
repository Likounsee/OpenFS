#ifndef OPENFS_CRC32C_H
#define OPENFS_CRC32C_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t openfs_crc32c_update(uint32_t state, const void *data, size_t length);
uint32_t openfs_crc32c_finalize(uint32_t state);
uint32_t openfs_crc32c(const void *data, size_t length);
#ifdef __cplusplus
}
#endif
#endif
