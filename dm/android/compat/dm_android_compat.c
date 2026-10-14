// 安卓兼容层里"必须是真符号"的那几个函数（汇编要直接调用，内联不行）。
// 其余兼容物都是头文件里的宏与 static inline，见 dm_android_compat.h。
// The few compat-layer functions that must be real symbols, because the assembly calls them
// directly and an inline will not do. Everything else is a macro or a static inline in
// dm_android_compat.h.
#include "dm_android_compat.h"

// 与 Apple 的语义一致：单调、纳秒、不随系统时间调整而跳变。
// Matches Apple's semantics: monotonic, nanoseconds, unaffected by wall-clock adjustments.
uint64_t mach_absolute_time(void) {
    return clock_gettime_nsec_np(CLOCK_MONOTONIC);
}
