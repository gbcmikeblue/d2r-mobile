// Apple 的 malloc zone 接口在安卓上没有：给最小替身（只服务于诊断统计）。
// Apple's malloc-zone API does not exist on Android: minimal stand-ins (diagnostics only).
#pragma once

#include <malloc.h>
#include <stdint.h>
#include <stddef.h>

typedef struct malloc_zone_t { int dm_placeholder; } malloc_zone_t;

typedef struct {
    size_t size_in_use;
    size_t max_size_in_use;
    size_t size_allocated;
} malloc_statistics_t;

static inline malloc_zone_t *malloc_default_zone(void) { return NULL; }
static inline size_t malloc_size(const void *p) { (void)p; return 0; }
static inline size_t malloc_good_size(size_t n) { return n; }
static inline void malloc_zone_statistics(malloc_zone_t *z, malloc_statistics_t *s) {
    (void)z;
    if (s) { s->size_in_use = 0; s->max_size_in_use = 0; s->size_allocated = 0; }
}
static inline void malloc_zone_pressure_relief(malloc_zone_t *z, size_t goal) { (void)z; (void)goal; }
