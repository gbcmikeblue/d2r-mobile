// 安卓兼容层：把宿主里用到的 macOS / Apple 专有 API 映射到 POSIX / Android 等价物。
// 由构建命令以 -include 强制包含（见 CMakeLists.txt），因此宿主源码本体暂时不用改。
// Android compatibility layer: map the macOS / Apple-specific APIs used by the host onto
// POSIX / Android equivalents. Force-included by the build (see CMakeLists.txt), so the
// host sources themselves need no change for now.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#pragma once

#include <time.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>

// ---- 平台判断宏 / platform-selection macros ----
// 宿主的平台判断只有 Mac / iOS 两支：安卓先统一走"非 iOS"这一支（编译风险最小）。
// 注意 TARGET_OS_IPHONE 在个别文件里是**当表达式用的**（如 hle_fs.c 的默认开关），
// 所以这里必须无条件定义，而不是只放在 TargetConditionals.h 里。
// The host only branches Mac / iOS; Android takes the "not iOS" side first (lowest build risk).
// Note that a few files use TARGET_OS_IPHONE as an *expression* (e.g. a default toggle in
// hle_fs.c), so it must be defined here unconditionally, not only in TargetConditionals.h.
#ifndef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 0
#endif
#ifndef TARGET_OS_OSX
#define TARGET_OS_OSX 0
#endif
#ifndef TARGET_OS_MAC
#define TARGET_OS_MAC 0
#endif
#define TARGET_OS_ANDROID 1

// ---- 宏层面的等价物 / macro-level equivalents ----

// macOS 的 st_mtimespec 是 POSIX 的 st_mtim / macOS st_mtimespec is POSIX st_mtim
#define st_mtimespec st_mtim

// macOS 的 CLOCK_UPTIME_RAW（不含睡眠的单调时钟）/ macOS CLOCK_UPTIME_RAW (monotonic, excluding sleep)
#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#endif

// BSD 的 getdtablesize() / BSD getdtablesize()
#ifndef getdtablesize
#define getdtablesize() ((int)sysconf(_SC_OPEN_MAX))
#endif

// macOS 的 xattr 选项与"属性不存在"错误码
// macOS xattr options and the "attribute not found" errno
#ifndef XATTR_NOFOLLOW
#define XATTR_NOFOLLOW 0x0001
#endif
#ifndef XATTR_CREATE
#define XATTR_CREATE 0x0002
#endif
#ifndef XATTR_REPLACE
#define XATTR_REPLACE 0x0004
#endif
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

// macOS 的原子交换重命名 / macOS atomic-swap rename
#ifndef RENAME_SWAP
#define RENAME_SWAP 0x0002
#endif

// macOS 的 F_GETPATH（fcntl 取路径）：安卓没有；定义成一个不会被识别、fcntl 会返回失败的值，
// 调用方按"取不到路径"处理（待办：改成 /proc/self/fd 读链接）。
// macOS F_GETPATH (fcntl to fetch a path): absent on Android. Define a value fcntl will reject so
// the caller takes its "no path" path (follow-up: implement via /proc/self/fd readlink).
#ifndef F_GETPATH
#define F_GETPATH 9999
#endif

// ---- 函数层面的等价物 / function-level equivalents ----

// macOS 的 clock_gettime_nsec_np(clk)：直接返回纳秒 / macOS clock_gettime_nsec_np(clk): nanoseconds as a value
static inline uint64_t clock_gettime_nsec_np(clockid_t clk) {
    struct timespec ts;
    if (clock_gettime(clk, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// macOS 的 pthread_get_stacksize_np / pthread_get_stackaddr_np → pthread_getattr_np
static inline int dm_stack_attr(pthread_t t, void **addr, size_t *size) {
    pthread_attr_t a;
    if (pthread_getattr_np(t, &a) != 0) return -1;
    if (pthread_attr_getstack(&a, addr, size) != 0) { pthread_attr_destroy(&a); return -1; }
    pthread_attr_destroy(&a);
    return 0;
}
static inline size_t pthread_get_stacksize_np(pthread_t t) {
    void *addr; size_t size;
    return dm_stack_attr(t, &addr, &size) == 0 ? size : 0;
}
static inline void *pthread_get_stackaddr_np(pthread_t t) {
    void *addr; size_t size;
    if (dm_stack_attr(t, &addr, &size) != 0) return NULL;
    return (char *)addr + size;   // macOS 返回栈顶（高地址）/ macOS returns the stack top (high address)
}

// mach_absolute_time()：宿主用它做单调计时。跳板汇编 nvn_entry.S 会**直接 bl 这个符号**，
// 所以这里必须是真符号、不能是内联（实现在 dm_android_compat.c）。
// mach_absolute_time(): monotonic timing used by the host. The thunk assembly in nvn_entry.S does a
// direct `bl` to this symbol, so it must be a real symbol rather than an inline (defined in
// dm_android_compat.c).
uint64_t mach_absolute_time(void);

// macOS 的 renamex_np 原子交换：安卓没有等价物，退化为"两次普通改名"（非原子）。
// 待办：M1 之后按存档事务的实际需求换成更稳的做法。
// macOS renamex_np atomic swap: no Android equivalent; degraded to two plain renames (not atomic).
// Follow-up: replace with something sturdier once the save transaction's needs are known.
static inline int renamex_np(const char *from, const char *to, unsigned int flags) {
    if (flags & RENAME_SWAP) {
        char tmp[4096];
        snprintf(tmp, sizeof tmp, "%s.dmswap", to);
        if (rename(to, tmp) != 0) return -1;
        if (rename(from, to) != 0) { rename(tmp, to); return -1; }
        if (rename(tmp, from) != 0) return -1;
        return 0;
    }
    return rename(from, to);
}
