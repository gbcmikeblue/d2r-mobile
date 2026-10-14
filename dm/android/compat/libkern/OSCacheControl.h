// Apple 的缓存维护接口在安卓上没有：用编译器内建替身（安卓上写代码后要清指令缓存）。
// Apple's cache-control API does not exist on Android: use the compiler builtin instead
// (Android requires an instruction-cache flush after writing code).
#pragma once

#include <stddef.h>

static inline void sys_icache_invalidate(void *start, size_t len) {
    __builtin___clear_cache((char *)start, (char *)start + len);
}
static inline void sys_dcache_flush(void *start, size_t len) {
    __builtin___clear_cache((char *)start, (char *)start + len);
}
