#ifndef OPENFS_TIME_H
#define OPENFS_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns UTC nanoseconds from the C runtime clock, or UINT64_MAX on failure/overflow. */
uint64_t openfs_time_now_ns(void);

#ifdef __cplusplus
}
#endif
#endif
