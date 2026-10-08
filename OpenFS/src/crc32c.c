#include "openfs/crc32c.h"

uint32_t openfs_crc32c_update(uint32_t state, const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = state;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint32_t)bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0x82F63B78) & mask);
        }
    }
    return crc;
}

uint32_t openfs_crc32c_finalize(uint32_t state) {
    return ~state;
}

uint32_t openfs_crc32c(const void *data, size_t length) {
    return openfs_crc32c_finalize(openfs_crc32c_update(UINT32_MAX, data, length));
}
