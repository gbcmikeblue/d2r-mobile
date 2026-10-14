// 安卓上没有 mach_time：用单调时钟替身（mach_absolute_time 在 dm_android_compat.h 里）。
// No mach_time on Android: a monotonic-clock stand-in (mach_absolute_time lives in dm_android_compat.h).
#pragma once

#include <stdint.h>
#include "../dm_android_compat.h"

typedef struct { uint32_t numer; uint32_t denom; } mach_timebase_info_data_t;

static inline int mach_timebase_info(mach_timebase_info_data_t *tb) {
    if (tb) { tb->numer = 1; tb->denom = 1; }   // 我们的 mach_absolute_time 已按纳秒返回 | ours already returns nanoseconds
    return 0;
}
