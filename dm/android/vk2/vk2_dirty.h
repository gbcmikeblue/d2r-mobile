// 新地基 N2 之一：**区间级脏记账**（规格：docs/rewrite/ANDROID_REWRITE.zh-CN.md §9.4）。
//
// 新地基里"写完即可见"（Metal 的假设 2）不存在：CPU 写的是**影子**（普通内存），GPU 用的是**另一份**
// 后备资源（`vk2_pool.h`）。所以必须有人记得"哪一段的 CPU 副本与 GPU 副本不一致"，这份账就是本文件。
//
// 三态（每页两位，两张 1 bit 位图）：
//   · 两位都 0 = **CLEAN**：影子与后备一致（不需要搬）；
//   · cpu = 1 = **CPU_DIRTY**：CPU 写过影子 ⇒ 上传窗口要把它搬上去（搬完置 CLEAN）；
//   · gpu = 1 = **GPU_DIRTY**：GPU 写过（渲染目标 / 计算输出）⇒ **回读之前影像是过期的**，
//     谁要读这段就必须先 `vk2_readback`（读完置 CLEAN）。
//
// 粒度 = **4 KiB**（N0 的真机读数：`vk2_probe` 量到 4 KiB = 本机页大小、4 GB 池的位图 131072 字节、
// "标记 + 并 run"扫一遍 0.9~2.7 ms）。**三态要两位**，所以本实现是 CPU/GPU 各一张 1 bit 位图
// ⇒ 4 GB 池 = 2 × 131072 = **262144 字节**（N2 的探针复核这张表，并把扫描耗时重新量一遍）。
//
// **半页竞态（本文件要说清的语义边界）**：同一页既被 GPU 写过（gpu=1）又被 CPU **部分**写过时，
// 整页上传影子会把 GPU 写在那一页里、CPU 没碰过的那几个字节盖掉。N2 只报计数（`hazard_halfpage`）；
// **N4-c 给了对策**（两条，见 `vk2_xfer.h` 与 `vk2_xfer.c`）：① 槽回收时把那一次提交直写过的段**整页**回读
// 进影子；② 帧开头上传**跳过**还带着 GPU 脏的页（`up_skip_gpu`），不拿旧影子盖掉在飞的 GPU 写。
// "在绑定点先回读该页"在时序上做不到（那一刻这次 GPU 写还没发生），所以不是那条路。
//
// ---------------------------------------------------------------------------------------------------------------
// N2 part one: interval-level dirty accounting (spec: section 9.4). "Write is immediately visible" (Metal's
// assumption 2) does not exist in the new foundation -- the CPU writes the shadow while the GPU uses a separate
// backing store -- so somebody has to remember which ranges disagree. Three states, two one-bit bitmaps per page:
// both zero = CLEAN, cpu = the CPU wrote the shadow (the upload window must push it), gpu = the GPU wrote (any
// reader of the shadow is stale until it calls vk2_readback). Granularity is 4 KiB (N0's device reading), and
// because three states need two bits the 4 GB pool costs 262144 bytes of bitmap instead of N0's 131072.
// Half-page hazard: a page the GPU wrote and the CPU then wrote *partially* cannot be uploaded whole without
// clobbering the GPU's bytes; this round only counts it (hazard_halfpage + a yellow log) and leaves the decision
// to N4's binding points.
#ifndef DM_VK2_DIRTY_H
#define DM_VK2_DIRTY_H

#include <stddef.h>
#include <stdint.h>

#define VK2_DIRTY_CLEAN 0
#define VK2_DIRTY_CPU 1
#define VK2_DIRTY_GPU 2

typedef struct {
    uint64_t mark_cpu, mark_gpu, clean;      // 记账调用次数 | accounting calls
    uint64_t clean_gpu;                      // N4-c：只清 GPU 位（往返型回读之后，页上有 CPU 脏）| GPU-bit-only cleans
    uint64_t scans, runs_seen;               // 扫描次数 / 扫出来的 run 数 | scans / runs produced
    uint64_t scan_pages;                     // 累计扫过的页数（按池大小计）| pages walked, cumulative
    uint64_t scan_us, last_scan_us;          // 扫描累计 / 最近一次耗时（µs）| scan time, cumulative / last
    uint64_t hazard_halfpage;                // 半页竞态（同时被 GPU 写、又被 CPU 部分写）| half-page hazard
    uint64_t clamp;                          // 越界被夹到 [0, bytes) 的记账次数 | out-of-range clamp count
    uint64_t gran_asked;                     // 调用方请求的粒度（复核用）| requested granularity
} vk2_dirty_stats;

typedef struct {
    uint64_t      *cpu;   // 1 bit / 页：CPU 写过影子 | one bit per page: the CPU wrote the shadow
    uint64_t      *gpu;   // 1 bit / 页：GPU 写过后备 | one bit per page: the GPU wrote the backing store
    uint64_t       bytes; // 这段区间的字节数（池大小）| bytes covered (the pool size)
    uint64_t       pages;
    uint64_t       words; // 每张位图的字数（64 位字）| 64-bit words per bitmap
    uint32_t       gran;  // 粒度（字节）| granularity in bytes
    vk2_dirty_stats st;
} vk2_dirty;

// 粒度（字节）：4 KiB 起步。**改这里是唯一的改法**（位图大小与 runs 都由它推）| granularity, 4 KiB for now
#define VK2_DIRTY_GRAN_DEFAULT 4096u
uint32_t vk2_dirty_gran_default(void);
// 两张位图合起来的字节数（按给定粒度算，给探针对账用）| combined bitmap bytes for a given size and granularity
uint64_t vk2_dirty_bitmap_bytes(uint64_t bytes, uint32_t gran);

// 初始化：`gran` = 0 用 VK2_DIRTY_GRAN_DEFAULT。返回 0 = 失败（日志由调用方决定）。
int  vk2_dirty_init(vk2_dirty *d, uint64_t bytes, uint32_t gran);
void vk2_dirty_fini(vk2_dirty *d);

// 记账（off / len 会被**向外扩到页边界**；len = 0 按 1 字节；越界会被夹到 [0, bytes) 并计数）。
// Accounting; off/len are rounded outward to page boundaries, len = 0 means one byte, out-of-range is clamped.
void vk2_dirty_mark_cpu(vk2_dirty *d, uint64_t off, uint64_t len);
void vk2_dirty_mark_gpu(vk2_dirty *d, uint64_t off, uint64_t len);
void vk2_dirty_clean(vk2_dirty *d, uint64_t off, uint64_t len);
// N4-c：只清 GPU 那一位（往返型回读之后用，理由见 vk2_dirty.c 里那段说明）| clear only the GPU bit (N4-c)
void vk2_dirty_clean_gpu(vk2_dirty *d, uint64_t off, uint64_t len);

// 查询：这段里有没有 cpu / gpu 脏页（1 = 有）；count 给页数。
// Query: is there any cpu/gpu-dirty page in this range (1 = yes); the page counts are the two helpers.
int      vk2_dirty_any(const vk2_dirty *d, uint64_t off, uint64_t len, int state);
uint64_t vk2_dirty_count(const vk2_dirty *d, int state);

// 扫描：把 [lo, hi) 里状态为 `state` 的**连续页并成 run**，逐个回调（run 的 off 是池内偏移、页对齐）。
// 返回扫出来的 run 数；`cb` 的返回值非 0 ⇒ 提前停。整套扫描的耗时记进统计（`last_scan_us`）。
// Scan: merge consecutive pages of the given state into runs and hand each to cb (offsets are pool-relative and
// page-aligned). Returns the number of runs; a non-zero cb return stops early.
typedef int (*vk2_dirty_run_fn)(uint64_t off, uint64_t len, void *ud);
uint64_t vk2_dirty_scan(vk2_dirty *d, int state, uint64_t lo, uint64_t hi, vk2_dirty_run_fn cb, void *ud);

void vk2_dirty_stats_get(const vk2_dirty *d, vk2_dirty_stats *out);
void vk2_dirty_stats_reset(vk2_dirty *d);
void vk2_dirty_set_log(void (*fn)(const char *fmt, ...));   // 不给就用 stderr | stderr when unset

#endif // DM_VK2_DIRTY_H
