// 新地基 N2 之一：区间级脏记账的实现（规格与语义边界见 vk2_dirty.h 的文件头）。
// N2 part one: the implementation of interval-level dirty accounting (see vk2_dirty.h for the spec).
#include "vk2_dirty.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void def_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
static void (*g_log)(const char *fmt, ...) = def_log;
void vk2_dirty_set_log(void (*fn)(const char *fmt, ...)) { g_log = fn ? fn : def_log; }
#define DLOG(...) g_log(__VA_ARGS__)

static uint64_t now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

uint32_t vk2_dirty_gran_default(void) { return VK2_DIRTY_GRAN_DEFAULT; }

uint64_t vk2_dirty_bitmap_bytes(uint64_t bytes, uint32_t gran) {
    if (!gran) gran = VK2_DIRTY_GRAN_DEFAULT;
    uint64_t pages = (bytes + gran - 1) / gran;
    uint64_t words = (pages + 63) / 64;
    return words * 8 * 2;   // 两张位图（cpu + gpu）| two bitmaps
}

int vk2_dirty_init(vk2_dirty *d, uint64_t bytes, uint32_t gran) {
    if (!d || !bytes) return 0;
    memset(d, 0, sizeof *d);
    if (!gran) gran = VK2_DIRTY_GRAN_DEFAULT;
    d->gran = gran;
    d->bytes = bytes;
    d->pages = (bytes + gran - 1) / gran;
    d->words = (d->pages + 63) / 64;
    d->cpu = calloc((size_t)d->words, sizeof *d->cpu);
    d->gpu = calloc((size_t)d->words, sizeof *d->gpu);
    if (!d->cpu || !d->gpu) { free(d->cpu); free(d->gpu); d->cpu = d->gpu = NULL; return 0; }
    d->st.gran_asked = gran;
    return 1;
}

void vk2_dirty_fini(vk2_dirty *d) {
    if (!d) return;
    free(d->cpu); free(d->gpu);
    d->cpu = d->gpu = NULL; d->pages = d->words = 0; d->bytes = 0;
}

// 把 [off, len) 向外扩到页边界（并夹进 [0, bytes)），回第一个与最后一个页号（含）。
// Round [off, len) outward to page boundaries, clamp to [0, bytes), return the first and last page (inclusive).
static int clip_pages(const vk2_dirty *d, uint64_t off, uint64_t len, uint64_t *p0, uint64_t *p1, int *clamped) {
    if (!d->pages) return 0;
    if (len == 0) len = 1;
    uint64_t lo = off, hi = off + len;              // 溢出检查：hi < lo 视为越界到底
    if (hi < lo) { hi = d->bytes; *clamped = 1; }
    if (lo >= d->bytes) { *clamped = 1; return 0; }
    if (hi > d->bytes) { hi = d->bytes; *clamped = 1; }
    *p0 = lo / d->gran;
    *p1 = (hi - 1) / d->gran;
    if (*p1 >= d->pages) *p1 = d->pages - 1;
    return 1;
}

static void bits_set(uint64_t *m, uint64_t p0, uint64_t p1) {
    uint64_t w0 = p0 >> 6, w1 = p1 >> 6;
    if (w0 == w1) {
        uint64_t lo = p0 & 63, hi = p1 & 63;
        uint64_t mask = (hi == 63) ? ~0ull << lo : (((~0ull << lo) & (~0ull >> (63 - hi))));
        m[w0] |= mask;
        return;
    }
    m[w0] |= ~0ull << (p0 & 63);
    for (uint64_t w = w0 + 1; w < w1; w++) m[w] = ~0ull;
    uint64_t hi = p1 & 63;
    m[w1] |= (hi == 63) ? ~0ull : ((1ull << (hi + 1)) - 1);
}

static void bits_clr(uint64_t *m, uint64_t p0, uint64_t p1) {
    uint64_t w0 = p0 >> 6, w1 = p1 >> 6;
    if (w0 == w1) {
        uint64_t lo = p0 & 63, hi = p1 & 63;
        uint64_t mask = (hi == 63) ? ~0ull << lo : (((~0ull << lo) & (~0ull >> (63 - hi))));
        m[w0] &= ~mask;
        return;
    }
    m[w0] &= ~(~0ull << (p0 & 63));
    for (uint64_t w = w0 + 1; w < w1; w++) m[w] = 0;
    uint64_t hi = p1 & 63;
    m[w1] &= ~((hi == 63) ? ~0ull : ((1ull << (hi + 1)) - 1));
}

static int bits_any(const uint64_t *m, uint64_t p0, uint64_t p1) {
    uint64_t w0 = p0 >> 6, w1 = p1 >> 6;
    if (w0 == w1) {
        uint64_t lo = p0 & 63, hi = p1 & 63;
        uint64_t mask = (hi == 63) ? ~0ull << lo : (((~0ull << lo) & (~0ull >> (63 - hi))));
        return (m[w0] & mask) != 0;
    }
    if (m[w0] & (~0ull << (p0 & 63))) return 1;
    for (uint64_t w = w0 + 1; w < w1; w++) if (m[w]) return 1;
    uint64_t hi = p1 & 63;
    uint64_t mask = (hi == 63) ? ~0ull : ((1ull << (hi + 1)) - 1);
    return (m[w1] & mask) != 0;
}

void vk2_dirty_mark_cpu(vk2_dirty *d, uint64_t off, uint64_t len) {
    if (!d || !d->pages) return;
    uint64_t p0 = 0, p1 = 0; int cl = 0;
    if (!clip_pages(d, off, len, &p0, &p1, &cl)) return;
    if (cl) d->st.clamp++;
    // 半页竞态：这一页已经被 GPU 写过，而 CPU 只写了它的一部分 ⇒ 整页上传会盖掉 GPU 那部分。
    // 本轮**只报计数不猜**（真正的处理在 N4 的绑定点决定）| half-page hazard: counted, not guessed at.
    int hazard = 0;
    if (!(d->gran == 1) && bits_any(d->gpu, p0, p1)) {
        uint64_t lo = p0 * d->gran, hi = (p1 + 1) * d->gran;
        int full = (off <= lo) && (off + (len ? len : 1) >= hi);
        if (!full) hazard = 1;
    }
    bits_set(d->cpu, p0, p1);
    d->st.mark_cpu++;
    if (hazard) {
        d->st.hazard_halfpage++;
        // 前 8 条报明细，之后只留计数 —— 一轮里这类事件可以上万次，不节流会把读数刷没
        // Detail the first eight, counts only after that: this can happen tens of thousands of times per run.
        if (d->st.hazard_halfpage <= 8) {
            uint64_t n = d->st.hazard_halfpage;
            DLOG("【黄】脏记账：这段（池内 %#llx+%llu）既有 GPU 写过的页、又被 CPU **部分**写过 ⇒ 整页上传会"
                 "盖掉 GPU 那几个字节（半页竞态累计 %llu 次；处理留给 N4 的绑定点）",
                 (unsigned long long)off, (unsigned long long)len, (unsigned long long)n);
        }
    }
}

void vk2_dirty_mark_gpu(vk2_dirty *d, uint64_t off, uint64_t len) {
    if (!d || !d->pages) return;
    uint64_t p0 = 0, p1 = 0; int cl = 0;
    if (!clip_pages(d, off, len, &p0, &p1, &cl)) return;
    if (cl) d->st.clamp++;
    bits_set(d->gpu, p0, p1);
    d->st.mark_gpu++;
}

void vk2_dirty_clean(vk2_dirty *d, uint64_t off, uint64_t len) {
    if (!d || !d->pages) return;
    uint64_t p0 = 0, p1 = 0; int cl = 0;
    if (!clip_pages(d, off, len, &p0, &p1, &cl)) return;
    if (cl) d->st.clamp++;
    bits_clr(d->cpu, p0, p1);
    bits_clr(d->gpu, p0, p1);
    d->st.clean++;
}

// N4-c：**只清 GPU 那一位**（往返型回读之后用）。为什么不能顺手把 CPU 位也清掉：那一页可能同时还有 CPU 脏
// （客体刚写过、还没上传），清掉它就等于把那几个字节永远丢在影子里（而记账看不出来）。两条路分开：
//   · 整段回读进影子且页上没有 CPU 脏 ⇒ `vk2_dirty_clean`（两位都清，影子与后备一致）；
//   · 页上有 CPU 脏 ⇒ 只清 GPU 位（GPU 写过的字节已经回读进影子，CPU 那几个字节留给上传窗口）。
// N4-c: clear only the GPU bit after a round-trip readback. The page may still carry a CPU-dirty mark (the guest wrote
// there and the upload has not happened yet); clearing that would silently drop those bytes.
void vk2_dirty_clean_gpu(vk2_dirty *d, uint64_t off, uint64_t len) {
    if (!d || !d->pages) return;
    uint64_t p0 = 0, p1 = 0; int cl = 0;
    if (!clip_pages(d, off, len, &p0, &p1, &cl)) return;
    if (cl) d->st.clamp++;
    bits_clr(d->gpu, p0, p1);
    d->st.clean_gpu++;
}

int vk2_dirty_any(const vk2_dirty *d, uint64_t off, uint64_t len, int state) {
    if (!d || !d->pages) return 0;
    uint64_t p0 = 0, p1 = 0; int cl = 0;
    if (!clip_pages(d, off, len, &p0, &p1, &cl)) return 0;
    return bits_any(state == VK2_DIRTY_CPU ? d->cpu : d->gpu, p0, p1);
}

uint64_t vk2_dirty_count(const vk2_dirty *d, int state) {
    if (!d || !d->pages) return 0;
    const uint64_t *m = (state == VK2_DIRTY_CPU) ? d->cpu : d->gpu;
    uint64_t n = 0;
    for (uint64_t p = 0; p < d->pages; p++) if (m[p >> 6] & (1ull << (p & 63))) n++;
    return n;
}

// 扫描：逐字找"有置位的字"，再把连续页并成 run。就快在这里 —— 空白页是整字跳过（4 GB 池 = 16384 个字）。
// Scan: skip whole words that are zero and merge consecutive set pages into runs; this is where the speed comes
// from (a 4 GB pool is 16384 words per bitmap).
uint64_t vk2_dirty_scan(vk2_dirty *d, int state, uint64_t lo, uint64_t hi, vk2_dirty_run_fn cb, void *ud) {
    if (!d || !d->pages || !cb) return 0;
    if (hi > d->bytes) hi = d->bytes;
    if (lo >= hi) return 0;
    const uint64_t *m = (state == VK2_DIRTY_CPU) ? d->cpu : d->gpu;
    uint64_t t0 = now_us();
    uint64_t p0 = lo / d->gran, p1 = (hi - 1) / d->gran;
    uint64_t runs = 0, p = p0;
    while (p <= p1) {
        // 整字空白就跳 64 页（4 GB 池只有 16384 个字，所以空闲池的扫描是"读几千个字"的量级）
        // Skip a whole zero word (a 4 GB pool is only 16384 words, so an idle pool costs a few thousand reads).
        if ((p & 63) == 0 && m[p >> 6] == 0) { p += 64; continue; }
        if (!((m[p >> 6] >> (p & 63)) & 1ull)) { p++; continue; }
        uint64_t start = p;
        while (p <= p1 && ((m[p >> 6] >> (p & 63)) & 1ull)) p++;
        runs++;
        if (cb(start * d->gran, (p - start) * d->gran, ud)) break;
    }
    uint64_t dt = now_us() - t0;
    d->st.scans++;
    d->st.runs_seen += runs;
    d->st.scan_pages += d->pages;
    d->st.scan_us += dt;
    d->st.last_scan_us = dt;
    return runs;
}

void vk2_dirty_stats_get(const vk2_dirty *d, vk2_dirty_stats *out) {
    if (!d || !out) return;
    *out = d->st;
}

void vk2_dirty_stats_reset(vk2_dirty *d) {
    if (!d) return;
    memset(&d->st, 0, sizeof d->st);
    d->st.gran_asked = d->gran;
}
