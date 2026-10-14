// BSD 的 sysctl 在安卓上没有：给宿主提供最小替身。
// 已覆盖：hw.memsize / hw.ncpu（走 sysinfo 与 sysconf）；KERN_PROC_PID 只保证**类型**齐全，
// 调用返回失败，调用方（main.c 的 proc_start_mono_ns）会按"取不到就退化"处理。
// 待办：可从 /proc/self/stat 的第 22 字段算真实启动时间。
// BSD sysctl does not exist on Android: minimal stand-ins for the host.
// Covered: hw.memsize / hw.ncpu (via sysinfo and sysconf). KERN_PROC_PID only provides the *types*;
// the call fails and the caller (proc_start_mono_ns in main.c) degrades as designed.
// Follow-up: real process start time can be computed from field 22 of /proc/self/stat.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/sysinfo.h>

#define CTL_KERN 1
#define CTL_HW 6
#define HW_MEMSIZE 24
#define HW_NCPU 3
#define KERN_PROC 14
#define KERN_PROC_PID 1

struct extern_proc {
    struct { long tv_sec; long tv_usec; } p_starttime;   // 与 macOS 同名成员 / same member name as macOS
};
struct kinfo_proc {
    struct extern_proc kp_proc;
    int kp_pid;
};

static inline int dm_sysinfo_shim(uint64_t *total_bytes, int *ncpu) {
    struct sysinfo s;
    if (sysinfo(&s) == 0) {
        if (total_bytes) *total_bytes = (uint64_t)s.totalram * (uint64_t)s.mem_unit;
    } else if (total_bytes) {
        return -1;
    }
    if (ncpu) *ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    return 0;
}

static inline int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, const void *newp, size_t newlen) {
    (void)newp; (void)newlen;
    if (!name || !oldp || !oldlenp) return -1;
    if (strcmp(name, "hw.memsize") == 0) {
        uint64_t total = 0;
        if (dm_sysinfo_shim(&total, NULL) != 0 || *oldlenp < sizeof(uint64_t)) return -1;
        *(uint64_t *)oldp = total; *oldlenp = sizeof(uint64_t);
        return 0;
    }
    if (strcmp(name, "hw.ncpu") == 0) {
        int ncpu = 0;
        if (dm_sysinfo_shim(NULL, &ncpu) != 0 || *oldlenp < sizeof(int)) return -1;
        *(int *)oldp = ncpu; *oldlenp = sizeof(int);
        return 0;
    }
    return -1;
}

static inline int sysctl(const int *name, unsigned namelen, void *oldp, size_t *oldlenp, const void *newp, size_t newlen) {
    (void)newp; (void)newlen;
    if (!name || namelen < 2 || !oldp || !oldlenp) return -1;
    if (name[0] == CTL_HW && name[1] == HW_MEMSIZE) {
        uint64_t total = 0;
        if (dm_sysinfo_shim(&total, NULL) != 0 || *oldlenp < sizeof(uint64_t)) return -1;
        *(uint64_t *)oldp = total; *oldlenp = sizeof(uint64_t);
        return 0;
    }
    if (name[0] == CTL_HW && name[1] == HW_NCPU) {
        int ncpu = 0;
        if (dm_sysinfo_shim(NULL, &ncpu) != 0 || *oldlenp < sizeof(int)) return -1;
        *(int *)oldp = ncpu; *oldlenp = sizeof(int);
        return 0;
    }
    // KERN_PROC_PID：暂时不实现，让调用方退化 / KERN_PROC_PID: not implemented yet; let the caller degrade
    return -1;
}
