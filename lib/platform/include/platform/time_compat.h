/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Small compatibility helpers for the Phase 1 platform.clock rewire.
 * Prefer clock_now_* directly in new code; these helpers keep existing
 * time_t/timespec call sites mechanical while routing through platform.clock.
 */

#ifndef ZCL_PLATFORM_TIME_COMPAT_H
#define ZCL_PLATFORM_TIME_COMPAT_H

#include "platform/clock.h"

#include <stdint.h>
#include <time.h>

static inline int64_t platform_time_wall_unix(void)
{
    return clock_now_wall_ms() / 1000;
}

static inline time_t platform_time_wall_time_t(void)
{
    return (time_t)platform_time_wall_unix();
}

static inline int platform_time_monotonic_timespec(struct timespec *ts)
{
    if (!ts) return -1;
    int64_t ns = clock_now_monotonic_ns();
    ts->tv_sec = (time_t)(ns / 1000000000LL);
    ts->tv_nsec = (long)(ns % 1000000000LL);
    return 0;
}

static inline int platform_time_realtime_timespec(struct timespec *ts)
{
    if (!ts) return -1;
    int64_t ms = clock_now_wall_ms();
    ts->tv_sec = (time_t)(ms / 1000LL);
    ts->tv_nsec = (long)((ms % 1000LL) * 1000000LL);
    return 0;
}

#endif /* ZCL_PLATFORM_TIME_COMPAT_H */
