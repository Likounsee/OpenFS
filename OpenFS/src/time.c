#include "openfs/time.h"

#include <stdint.h>
#include <time.h>

uint64_t openfs_time_now_ns(void)
{
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return UINT64_MAX;
    if (ts.tv_sec < 0 || (uint64_t)ts.tv_sec > UINT64_MAX / UINT64_C(1000000000)) return UINT64_MAX;
    uint64_t seconds = (uint64_t)ts.tv_sec;
    uint64_t nanos = ts.tv_nsec < 0 ? 0U : (uint64_t)ts.tv_nsec;
    if (seconds > (UINT64_MAX - nanos) / UINT64_C(1000000000)) return UINT64_MAX;
    return seconds * UINT64_C(1000000000) + nanos;
}
