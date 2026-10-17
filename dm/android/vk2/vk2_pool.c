// 新地基（Vulkan 原生的显式实现）N1 的实现：**影子内存 + 分块后备资源 + 地址反查**。
// 规格与判据：docs/rewrite/ANDROID_REWRITE.zh-CN.md §9.2 / §9.3 / §9.8（判据 **G3**）；文件头自述见 vk2_pool.h。
//
// 三条本文件要顶住的性质（写在这里，改代码前先看一眼）：
//   ① **客体池永远是普通内存**：本文件从不对池内存做 `mprotect` / `mmap` / 换轨，也不要求页对齐。
//      它对池内存做的唯一一件事是**读 /proc/self/maps 确认这段是本进程可读写的普通内存**（`pool_mem_ok`）。
//      ⇒ 「池在哪儿」与「GPU 用哪块」解耦，AHB 那条路的原子语义问题不存在。
//   ② **块数按池大小算，块大小是常数**（960 MB，N0 真机读数）：4 GB 池 ⇒ 5 块（4×960 + 256）。
//      ⇒ 4 GB 池也能建出带 `STORAGE_BUFFER` 的**全功能**缓冲，不再有旧路的 `g_pool_small`。
//   ③ **地址反查跨块就返回 0**，并把"跨块请求的长度分布"记成账（§9.3）：这是"块大小该定多少"的复核依据。
//
// ---------------------------------------------------------------------------------------------------------------
// N1 of the new foundation: shadow memory + chunked backing store + address lookup. Three properties this file
// holds up: (1) a guest pool is always ordinary memory -- the only thing done to it is reading /proc/self/maps to
// confirm it is read-write ordinary memory of this process, so "where the pool lives" and "which piece the GPU
// uses" are decoupled and the AHB atomic-semantics problem cannot arise; (2) the chunk count follows the pool
// size while the chunk size is a constant (960 MB, the N0 on-device reading), so a 4 GB pool still gets
// full-featured STORAGE_BUFFER backing and the old road's g_pool_small cannot appear; (3) an address lookup that
// crosses a chunk boundary returns 0 and lands in a length histogram, which is what re-checks the chunk size.
#define _GNU_SOURCE 1
#include "vk2_pool.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// 跨块请求长度分布的档位上界（最后一档 = 无上界）| bucket edges of the cross-chunk length distribution
const uint64_t vk2_xlen_edge[VK2_XLEN_BUCKETS] = {
    4ull * 1024ull,          // <4 KiB
    64ull * 1024ull,         // 4~64 KiB
    1024ull * 1024ull,       // 64 KiB~1 MiB
    16ull * 1024ull * 1024ull, // 1~16 MiB
    UINT64_MAX               // ≥16 MiB
};
static const char *const xlen_name[VK2_XLEN_BUCKETS] = { "<4 KiB", "4~64 KiB", "64 KiB~1 MiB", "1~16 MiB", "≥16 MiB" };

// ---------------------------------------------------------------- 日志
static void def_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
static vk2_log_fn g_log = def_log;
void vk2_pool_set_log(vk2_log_fn fn) { g_log = fn ? fn : def_log; }
void vk2_pool_shutdown_log(void) { g_log = def_log; }
#define V2LOG(...) g_log(__VA_ARGS__)

// ---------------------------------------------------------------- 状态
static vk2_dev  g_dev;
static int      g_ready;
static uint64_t g_chunk;                 // 实际生效的块大小 | the chunk size actually in use
static vk2_pool *g_pools;
static int      g_npool;
static vk2_pool_stats g_st;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
// N4：后备预算（0 = 一块都不建；缺省 = 不限）。见 vk2_pool.h 的说明 | N4: the backing budget (0 = census only)
static uint64_t g_backing_budget = UINT64_MAX;
static uint64_t g_backing_bytes;         // 已经建出来的后备资源总字节 | backing built so far

int      vk2_pool_ready(void) { return g_ready; }
uint64_t vk2_pool_chunk_bytes(void) { return g_chunk; }
void     vk2_pool_set_backing_budget(uint64_t bytes) {
    pthread_mutex_lock(&g_mu);
    g_backing_budget = bytes;
    pthread_mutex_unlock(&g_mu);
}
uint64_t vk2_pool_backing_bytes(void) {
    uint64_t n;
    pthread_mutex_lock(&g_mu);
    n = g_backing_bytes;
    pthread_mutex_unlock(&g_mu);
    return n;
}
int      vk2_pool_count(void) { int n; pthread_mutex_lock(&g_mu); n = g_npool; pthread_mutex_unlock(&g_mu); return n; }
const vk2_pool *vk2_pool_at(int i) {
    const vk2_pool *p = NULL;
    pthread_mutex_lock(&g_mu);
    if (i >= 0 && i < g_npool) p = &g_pools[i];
    pthread_mutex_unlock(&g_mu);
    return p;
}
// N4-k：`VkBuffer` → **块的字节数**（找不到 = 0，表示"不是池里的块"）。
// 为什么要有它：`nvn_vk.c` 的 `desc_write` 要把 UBO 描述符的范围从 `VK_WHOLE_SIZE` 收到
// `min(maxUniformBufferRange, 块剩余字节)`（`DM_UBO_RANGE=1`）；"块剩余多少"只有这一层知道。
uint64_t vk2_buf_bytes(VkBuffer buf) {
    if (buf == VK_NULL_HANDLE) return 0;
    uint64_t out = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool && !out; i++)
        for (int k = 0; k < g_pools[i].nchunk; k++)
            if (g_pools[i].chunk[k].buf == buf) { out = (uint64_t)g_pools[i].chunk[k].size; break; }
    pthread_mutex_unlock(&g_mu);
    return out;
}
vk2_dirty *vk2_pool_dirty(int i) {
    vk2_dirty *d = NULL;
    pthread_mutex_lock(&g_mu);
    if (i >= 0 && i < g_npool) d = &g_pools[i].dirty;
    pthread_mutex_unlock(&g_mu);
    return d;
}
void *vk2_pool_base(int i) {
    void *b = NULL;
    pthread_mutex_lock(&g_mu);
    if (i >= 0 && i < g_npool) b = (void *)g_pools[i].lo;
    pthread_mutex_unlock(&g_mu);
    return b;
}
uint64_t vk2_pool_bytes(int i) {
    uint64_t n = 0;
    pthread_mutex_lock(&g_mu);
    if (i >= 0 && i < g_npool) n = g_pools[i].hi - g_pools[i].lo;
    pthread_mutex_unlock(&g_mu);
    return n;
}

// 按需求位挑一个内存类型，并列时选堆最大的那个（与 N0 探针同一套规则）
// Pick a memory type by property bits, preferring the largest heap on a tie (the same rule as N0's probe)
static uint32_t find_type(const vk2_dev *v, uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags avoid) {
    uint32_t best = UINT32_MAX; VkDeviceSize bestsz = 0;
    for (uint32_t i = 0; i < v->mp.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        VkMemoryPropertyFlags f = v->mp.memoryTypes[i].propertyFlags;
        if ((f & need) != need) continue;
        if (avoid && (f & avoid)) continue;
        VkDeviceSize sz = v->mp.memoryHeaps[v->mp.memoryTypes[i].heapIndex].size;
        if (best == UINT32_MAX || sz > bestsz) { best = i; bestsz = sz; }
    }
    return best;
}

// ---------------------------------------------------------------- 池前提：这段地址是本进程可读写的普通内存吗
// 新地基里池**唯一**的前提就是这个（旧路的前提是"落在我们铺过的换轨区间里"）。判法：`/proc/self/maps` 里
// 必须有一段 `rw` 的映射把 `[lo, hi)` **整段**盖住（有洞或不可读写都不算）。不成立 ⇒ 一块都不建，计 no_region。
// The single precondition a pool has in the new foundation (the old road required "inside the overlaid ranges"):
// some rw mapping in /proc/self/maps must cover [lo, hi) entirely; otherwise nothing is built and no_region counts.
// 判失败时**把这份账打出来**：读了多少行、解析出多少行、第一行落在 `lo` 之上的映射是哪一行、
// 最后一行是什么。"它不是普通内存"这句话本身要能自证，不能只给一句结论。
// On failure, print the accounting: how many lines were read and parsed, which mapping is the first one above lo,
// and what the last line was. "This is not ordinary memory" has to self-prove, not just assert.
static void maps_dump_around(uintptr_t lo, uintptr_t hi) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { V2LOG("    maps：打不开 /proc/self/maps"); return; }
    char line[512], last[512] = "", first_above[512] = "", first_cover[512] = "";
    int total = 0, parsed = 0, ncover = 0;
    while (fgets(line, sizeof line, f)) {
        total++;
        unsigned long long s = 0, e = 0;
        char perms[8] = "";
        if (sscanf(line, "%llx-%llx %7s", &s, &e, perms) != 3) continue;
        parsed++;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        snprintf(last, sizeof last, "%s", line);
        if (!first_above[0] && (uintptr_t)e > lo) snprintf(first_above, sizeof first_above, "%s", line);
        if ((uintptr_t)e > lo && (uintptr_t)s < hi) {
            if (!first_cover[0]) snprintf(first_cover, sizeof first_cover, "%s", line);
            if (ncover++ < 6) V2LOG("    maps[%d]（与本池范围有交叠）：%s", ncover, line);
        }
    }
    fclose(f);
    V2LOG("    maps：共 %d 行、解析出 %d 行；与本池范围 [%p, %p) 有交叠的 %d 行",
          total, parsed, (void *)lo, (void *)hi, ncover);
    V2LOG("    maps：第一行结束位置超过池首的映射：%s", first_above[0] ? first_above : "（没有——整份表都在池首之下）");
    if (!first_cover[0]) V2LOG("    maps：最后一行：%s", last);
}

static int pool_mem_ok(uintptr_t lo, uintptr_t hi, const char **why) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { if (why) *why = "/proc/self/maps 读不到"; return 0; }
    char line[512];
    uintptr_t need = lo;
    int ok = 0;
    const char *bad = "没有整段覆盖它的可读写映射";
    while (fgets(line, sizeof line, f)) {
        unsigned long long s = 0, e = 0;
        char perms[8] = "";
        if (sscanf(line, "%llx-%llx %7s", &s, &e, perms) != 3) continue;
        if (e <= need) continue;                 // 这段在需要的起点之前 | entirely before what we still need
        if (s > need) { bad = "中间有洞（池跨了未映射的地址）"; break; }
        if (perms[0] != 'r' || perms[1] != 'w') { bad = "这段映射不是可读写（rw）的"; break; }
        need = (uintptr_t)e;
        if (need >= hi) { ok = 1; break; }
    }
    fclose(f);
    if (!ok && why) *why = bad;
    return ok;
}

// ---------------------------------------------------------------- 建一块后备资源
// 建缓冲（先全功能、失败退传输位并计数）→ 取需求 → 挑 DEVICE_LOCAL → 分配 → 绑定。
// 失败时把这一步建出来的东西全收回，返回 0（调用方决定要不要记 pool_fail）。
// Create the buffer (full usage first, falling back to transfer-only and counting), query requirements, pick
// DEVICE_LOCAL, allocate, bind. On failure everything created here is released and 0 is returned.
static int chunk_make(vk2_chunk *c, uint64_t size) {
    c->small = 0; c->buf = VK_NULL_HANDLE; c->mem = VK_NULL_HANDLE; c->size = (size_t)size;
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof bci);
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = (VkDeviceSize)size;
    bci.usage = VK2_FULL_USAGE;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(g_dev.dev, &bci, NULL, &buf);
    if (r != VK_SUCCESS) {
        // 全功能建不出来 ⇒ 退成只带传输位，**并计数**（旧路的 g_pool_small 就是这个）。G3 要求它为 0。
        // Full usage failed: fall back to transfer-only and **count it** (the old road's g_pool_small). G3 says 0.
        buf = VK_NULL_HANDLE;
        bci.usage = VK2_MIN_USAGE;
        r = vkCreateBuffer(g_dev.dev, &bci, NULL, &buf);
        if (r != VK_SUCCESS) {
            V2LOG("【红】块 %llu MB 建缓冲失败（全功能与传输位都失败：VkResult %d）",
                  (unsigned long long)(size >> 20), (int)r);
            return 0;
        }
        c->small = 1;
        g_st.small++;
        V2LOG("【黄】块 %llu MB 只能按传输用法建出（全功能失败）⇒ 计到 small 上（G3 判据要求它为 0）",
              (unsigned long long)(size >> 20));
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_dev.dev, buf, &mr);
    uint32_t mi = find_type(&g_dev, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (mi == UINT32_MAX) mi = find_type(&g_dev, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (mi == UINT32_MAX) {
        vkDestroyBuffer(g_dev.dev, buf, NULL);
        V2LOG("【红】块 %llu MB 找不到 DEVICE_LOCAL 内存类型（内存类型位 %#x）",
              (unsigned long long)(size >> 20), (unsigned)mr.memoryTypeBits);
        return 0;
    }
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof mai);
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mi;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    r = vkAllocateMemory(g_dev.dev, &mai, NULL, &mem);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(g_dev.dev, buf, NULL);
        V2LOG("【红】块 %llu MB 分配失败（类型 %u，要对 %llu 字节）：VkResult %d",
              (unsigned long long)(size >> 20), mi, (unsigned long long)mr.size, (int)r);
        return 0;
    }
    r = vkBindBufferMemory(g_dev.dev, buf, mem, 0);
    if (r != VK_SUCCESS) {
        vkFreeMemory(g_dev.dev, mem, NULL);
        vkDestroyBuffer(g_dev.dev, buf, NULL);
        V2LOG("【红】块 %llu MB 绑定失败：VkResult %d", (unsigned long long)(size >> 20), (int)r);
        return 0;
    }
    c->buf = buf; c->mem = mem; c->size = (size_t)size;
    return 1;
}

static void chunk_free(vk2_chunk *c) {
    if (c->mem) vkFreeMemory(g_dev.dev, c->mem, NULL);
    if (c->buf) vkDestroyBuffer(g_dev.dev, c->buf, NULL);
    c->mem = VK_NULL_HANDLE; c->buf = VK_NULL_HANDLE; c->size = 0; c->small = 0;
}

// ---------------------------------------------------------------- 生命周期
int vk2_pool_init(const vk2_dev *d, uint64_t chunk_bytes) {
    if (!d || !d->dev || !d->phys) { V2LOG("【红】vk2_pool_init：设备句柄为空"); return 0; }
    pthread_mutex_lock(&g_mu);
    g_dev = *d;
    g_ready = 0;
    // 块大小的自检：不能超过"纸面上限取整再减一档"（N0 读数的规则），并对齐到档位网格
    // Self-check of the chunk size: never above "paper ceiling floored to a notch, minus one notch", and snap
    // to the notch grid.
    uint64_t cap = g_dev.m3.maxMemoryAllocationSize;
    if (cap == 0 || (g_dev.props.limits.maxStorageBufferRange &&
                     g_dev.props.limits.maxStorageBufferRange < cap))
        cap = g_dev.props.limits.maxStorageBufferRange;
    if (cap == 0) {
        pthread_mutex_unlock(&g_mu);
        V2LOG("【红】vk2_pool_init：maxMemoryAllocationSize 与 maxStorageBufferRange 都是 0（设备结构没填全？）");
        return 0;
    }
    uint64_t capf = (cap / VK2_NOTCH_BYTES) * VK2_NOTCH_BYTES;
    uint64_t max_ok = capf > VK2_NOTCH_BYTES ? capf - VK2_NOTCH_BYTES : capf;
    uint64_t want = chunk_bytes ? chunk_bytes : VK2_CHUNK_BYTES;
    want = (want / VK2_NOTCH_BYTES) * VK2_NOTCH_BYTES;
    if (!want) want = VK2_NOTCH_BYTES;
    if (want > max_ok) {
        V2LOG("【黄】块大小请求 %llu MB 超过「纸面上限取整再减一档」的 %llu MB ⇒ 夹到 %llu MB",
              (unsigned long long)(chunk_bytes >> 20), (unsigned long long)(max_ok >> 20),
              (unsigned long long)(max_ok >> 20));
        want = max_ok;
    }
    g_chunk = want;
    g_backing_bytes = 0;
    g_backing_budget = UINT64_MAX;     // 新的一轮：预算复位成"不限"，调用方（N4 的选路点/探针）再给
    if (!g_pools) {
        g_pools = calloc(VK2_MAX_POOLS, sizeof *g_pools);
        if (!g_pools) {
            pthread_mutex_unlock(&g_mu);
            V2LOG("【红】vk2_pool_init：池表分配失败（%d 项）", VK2_MAX_POOLS);
            return 0;
        }
    }
    g_ready = 1;
    pthread_mutex_unlock(&g_mu);
    V2LOG("池模块就绪：块 %llu MB、纸面上限 %llu MB；池永远是普通内存，GPU 用另一份分块后备资源",
          (unsigned long long)(g_chunk >> 20), (unsigned long long)(cap >> 20));
    return 1;
}

void vk2_pool_fini(void) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool; i++) {
        for (int k = 0; k < g_pools[i].nchunk; k++) chunk_free(&g_pools[i].chunk[k]);
        free(g_pools[i].chunk);
        vk2_dirty_fini(&g_pools[i].dirty);
    }
    free(g_pools);
    g_pools = NULL; g_npool = 0; g_ready = 0;
    g_backing_bytes = 0;
    g_backing_budget = UINT64_MAX;
    memset(&g_st, 0, sizeof g_st);
    pthread_mutex_unlock(&g_mu);
}

// ---------------------------------------------------------------- 池登记（§9.2）
int vk2_pool_add(void *mem, uint64_t size, const char *tag) {
    if (!g_ready || !mem || !size) return 0;
    // 先掩掉最高字节的标记（这台机的 malloc 返回 `0xb400…` 这样的 TBI 指针，见 vk2_ptr_mask）：
    // 之后所有的比较与偏移都基于**真地址**，`*off` 交给 GPU 的偏移才不会掺进标记位。
    // Mask the high tag byte first (malloc on this device hands back TBI pointers like 0xb400..), so every
    // comparison and every offset handed to the GPU works on the real address.
    uintptr_t raw = (uintptr_t)mem;
    uintptr_t lo = vk2_ptr_mask(raw), hi = lo + size;
    if (hi < lo) { V2LOG("【红】池 %p+%llu 地址溢出", mem, (unsigned long long)size); return 0; }
    int was_tagged = (raw != lo);

    pthread_mutex_lock(&g_mu);
    if (was_tagged) {
        g_st.tagged++;
        V2LOG("【黄】池 %p 的地址最高一个字节带标记（TBI），掩掉之后 = %p，照样登记（累计 %llu 次）",
              mem, (void *)lo, (unsigned long long)g_st.tagged);
    }

    // 旧路在这里问的是"这段在不在换轨区间里"，新地基只问"这段是不是本进程可读写的普通内存"。
    // The old road asked "is this inside the overlaid ranges"; the new one only asks "is this ordinary rw memory".
    const char *why = NULL;
    if (!pool_mem_ok(lo, hi, &why)) {
        g_st.no_region++;
        uint64_t n = g_st.no_region;
        pthread_mutex_unlock(&g_mu);
        V2LOG("【红】池 %p+%llu 不是本进程可读写的普通内存（%s）⇒ 一块都不建（no_region 累计 %llu）",
              mem, (unsigned long long)size, why ? why : "", (unsigned long long)n);
        maps_dump_around(lo, hi);
        return 0;
    }

    for (int i = 0; i < g_npool; i++) {   // 重叠是调用方的 bug：只报计数（vk2_addr 仍是"第一个覆盖者胜"）
        if (lo < g_pools[i].hi && g_pools[i].lo < hi) {
            g_st.overlap++;
            V2LOG("【黄】池 %p+%llu 与已登记的 %p+%llu 地址区间重叠（累计 %llu 次；vk2_addr 取第一个覆盖的池）",
                  mem, (unsigned long long)size, (void *)g_pools[i].lo,
                  (unsigned long long)(g_pools[i].hi - g_pools[i].lo), (unsigned long long)g_st.overlap);
            break;
        }
    }

    // 块数按池大小算：4 GB ÷ 960 MB ⇒ 5 块（最后一块 = 256 MB）| chunk count from the pool size
    uint64_t nb = (size + g_chunk - 1) / g_chunk;
    if (nb > VK2_MAX_CHUNKS) {
        V2LOG("【黄】池 %p+%llu 需要 %llu 块，超过上限 %d ⇒ 只建前 %d 块（超出部分的地址反查会 miss）",
              mem, (unsigned long long)size, (unsigned long long)nb, VK2_MAX_CHUNKS, VK2_MAX_CHUNKS);
        nb = VK2_MAX_CHUNKS;
    }
    vk2_chunk *cs = calloc((size_t)nb, sizeof *cs);
    if (!cs) { g_st.pool_fail++; pthread_mutex_unlock(&g_mu); return 0; }
    uint64_t built = 0, built_bytes = 0, skipped = 0, skip_bytes = 0;
    for (uint64_t k = 0; k < nb; k++) {
        uint64_t remain = size - k * g_chunk;
        uint64_t sz = remain < g_chunk ? remain : g_chunk;
        // N4：**后备预算** —— 建到预算为止，剩下的块不建。**池照样登记**（反查对缺掉的块 miss，计数看得见），
        // 这与"一块都建不出来 ⇒ 整个池失败"是两回事：前者是调用方主动限的，后者是设备/驱动建不出来。
        // N4's backing budget: build up to the budget, skip the rest -- but still register the pool.
        uint64_t room = (g_backing_budget == UINT64_MAX)
                        ? UINT64_MAX
                        : (g_backing_bytes < g_backing_budget ? g_backing_budget - g_backing_bytes : 0);
        if (sz > room) {
            skipped = nb - k;
            skip_bytes = size - k * g_chunk;
            break;
        }
        g_st.chunk_try++;
        if (!chunk_make(&cs[k], sz)) break;      // 失败点名在上面那条日志里 | the failure is named in the log
        cs[k].off = k * g_chunk;
        g_backing_bytes += sz;
        built_bytes += sz;
        g_st.chunk_ok++;
        built++;
    }
    g_st.chunk_skipped += skipped;
    g_st.skipped_bytes += skip_bytes;
    g_st.backing_bytes = g_backing_bytes;
    if (!built && !skipped) {
        free(cs);
        g_st.pool_fail++;
        pthread_mutex_unlock(&g_mu);
        V2LOG("【红】池 %p+%llu **一块都没建出来**（pool_fail 累计 %llu）——池里的东西上不了 GPU",
              mem, (unsigned long long)size, (unsigned long long)g_st.pool_fail);
        return 0;
    }

    if (g_npool >= VK2_MAX_POOLS) {
        for (uint64_t k = 0; k < built; k++) chunk_free(&cs[k]);
        free(cs);
        g_backing_bytes -= built_bytes;          // 建了又退掉，账还回去 | built then dropped: return the bytes
        g_st.backing_bytes = g_backing_bytes;
        pthread_mutex_unlock(&g_mu);
        V2LOG("【红】池表已满（%d），池 %p 未登记", VK2_MAX_POOLS, mem);
        return 0;
    }
    vk2_pool *pool = &g_pools[g_npool];
    memset(pool, 0, sizeof *pool);
    // N2：这块池的脏位图（4 KiB 起步）。**没有脏记账的池就用不了**（上传窗口无从知道该搬哪一段），
    // 所以位图建不出来时宁可整个池登记失败，也不要留下一个"能反查但永远搬不上去"的池。
    // N2: the pool's dirty bitmap. A pool without dirty accounting is unusable (the upload window would not know
    // what to move), so failing to allocate the bitmap fails the whole registration.
    if (!vk2_dirty_init(&pool->dirty, size, 0)) {
        for (uint64_t k = 0; k < built; k++) chunk_free(&cs[k]);
        free(cs);
        g_backing_bytes -= built_bytes;          // 同上：账还回去 | same: return the bytes
        g_st.backing_bytes = g_backing_bytes;
        g_st.dirty_fail++;
        uint64_t n = g_st.dirty_fail;
        pthread_mutex_unlock(&g_mu);
        V2LOG("【红】池 %p+%llu 的脏位图建不出来（%llu 字节，粒度 %u）⇒ 整个池登记失败（dirty_fail 累计 %llu）",
              mem, (unsigned long long)size,
              (unsigned long long)vk2_dirty_bitmap_bytes(size, vk2_dirty_gran_default()),
              vk2_dirty_gran_default(), (unsigned long long)n);
        return 0;
    }
    pool->lo = lo; pool->hi = hi;
    pool->chunk = cs; pool->nchunk = (int)built;
    pool->mode = VK2_MODE_SHADOW;
    pool->seq = ++g_st.pool_adds;
    if (tag && *tag) snprintf(pool->tag, sizeof pool->tag, "%s", tag);
    g_npool++;
    g_st.pools = (uint64_t)g_npool;
    g_st.bytes += size;
    int n = g_npool;
    uint64_t nsmall = 0;
    for (int i = 0; i < pool->nchunk; i++) if (pool->chunk[i].small) nsmall++;
    pthread_mutex_unlock(&g_mu);

    // 头 8 个报明细（看清池子长什么样），之后只留计数 | detail the first eight, counts only after that
    if (n <= 8) {
        if (size >= 1024 * 1024)
            V2LOG("池 #%d %p+%llu MB ⇒ %d 块 × %llu MB%s（模式 SHADOW，%s）",
                  n, mem, (unsigned long long)(size >> 20), (int)built, (unsigned long long)(g_chunk >> 20),
                  nsmall ? "【黄】其中有只带传输位的块" : "", tag && *tag ? tag : "无名字");
        else
            V2LOG("池 #%d %p+%llu 字节 ⇒ %d 块（块 %llu MB，只有头一块用得上）%s（模式 SHADOW，%s）",
                  n, mem, (unsigned long long)size, (int)built, (unsigned long long)(g_chunk >> 20),
                  nsmall ? "【黄】其中有只带传输位的块" : "", tag && *tag ? tag : "无名字");
    }
    return 1;
}

void vk2_pool_del(void *mem) {
    uintptr_t want = vk2_ptr_mask((uintptr_t)mem);   // 带标记的指针也要能删掉自己的池 | a tagged pointer still deletes
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool; i++) {
        if (g_pools[i].lo != want) continue;
        uint64_t sz = g_pools[i].hi - g_pools[i].lo;
        uint64_t rel = 0;
        for (int k = 0; k < g_pools[i].nchunk; k++) {
            rel += g_pools[i].chunk[k].size;     // chunk_free 会把 size 清零，先求和 | sum before chunk_free zeroes it
            chunk_free(&g_pools[i].chunk[k]);
        }
        free(g_pools[i].chunk);
        vk2_dirty_fini(&g_pools[i].dirty);
        g_backing_bytes -= rel;
        g_st.backing_bytes = g_backing_bytes;
        g_st.bytes -= sz;
        g_pools[i] = g_pools[--g_npool];
        g_st.pools = (uint64_t)g_npool;
        break;
    }
    pthread_mutex_unlock(&g_mu);
}

// ---------------------------------------------------------------- 地址反查（§9.3）
int vk2_addr(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off) {
    if (!g_ready) return 0;
    if (len == 0) len = 1;                        // 保守：0 字节按 1 字节 | len == 0 is treated as one byte
    addr = vk2_ptr_mask((uintptr_t)addr);         // 与登记时同一套掩码 | the same mask as at registration
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    g_st.lookups++;
    for (int i = 0; i < g_npool; i++) {
        const vk2_pool *p = &g_pools[i];
        if (addr < p->lo || addr + len > p->hi) continue;
        uint64_t k = (addr - p->lo) / g_chunk;
        if (k >= (uint64_t)p->nchunk) {           // 这段没有后备（块数被上限截过）| no backing for this range
            g_st.misses++;
            pthread_mutex_unlock(&g_mu);
            return 0;
        }
        const vk2_chunk *c = &p->chunk[k];
        uint64_t rel = addr - p->lo - c->off;
        if (rel + len > c->size) {
            // **请求跨了块边界**：返回 0，记长度分布，并报一条日志（前 16 条带明细）——§9.3 的契约。
            // A request crossing a chunk boundary: return 0, record the length histogram, log (details for the
            // first 16). This is exactly section 9.3's contract.
            int b = 0;
            while (b < VK2_XLEN_BUCKETS - 1 && len > vk2_xlen_edge[b]) b++;
            g_st.cross++;
            g_st.cross_hist[b]++;
            if (g_st.cross <= 16)
                V2LOG("【黄】地址反查：这次访问跨了块边界 —— 池 %p 内偏移 %llu、长度 %llu B（%s）"
                      "落在块 %llu（%llu MB）的末尾 %llu B 内；块大小该定多少看这份长度分布",
                      (void *)p->lo, (unsigned long long)(addr - p->lo), (unsigned long long)len,
                      xlen_name[b], (unsigned long long)k, (unsigned long long)(c->size >> 20),
                      (unsigned long long)(c->size - rel));
            pthread_mutex_unlock(&g_mu);
            return 0;
        }
        if (buf) *buf = c->buf;
        if (off) *off = rel;
        g_st.hits++;
        ok = 1;
        break;
    }
    if (!ok) g_st.misses++;   // 没落在任何池里 | outside every pool
    pthread_mutex_unlock(&g_mu);
    return ok;
}

// ---------------------------------------------------------------- N4：按客体地址标脏
// 反查池（与 vk2_addr 同一套掩码与遍历），把池内偏移交给脏位图。**只做标记，不搬东西**：
// 搬是上传窗口（vk2_xfer）的事，标记是回放层在"这段要被 GPU 用"的时刻做的。
// Mark by guest address: resolve the pool (the same mask and walk as vk2_addr) and hand the pool-relative offset to
// the dirty bitmap. Marking only -- moving is the upload window's job, and the replay layer marks at "this range is
// about to be used by the GPU" time.
static int touch_at(uint64_t addr, uint64_t len, int gpu) {
    if (!g_ready) return 0;
    if (len == 0) len = 1;
    addr = vk2_ptr_mask((uintptr_t)addr);
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool; i++) {
        vk2_pool *p = &g_pools[i];
        if (addr < p->lo || addr + len > p->hi) continue;
        if (gpu) vk2_dirty_mark_gpu(&p->dirty, addr - p->lo, len);
        else     vk2_dirty_mark_cpu(&p->dirty, addr - p->lo, len);
        ok = 1;
        break;
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int vk2_touch_cpu(uint64_t addr, uint64_t len) { return touch_at(addr, len, 0); }
int vk2_touch_gpu(uint64_t addr, uint64_t len) { return touch_at(addr, len, 1); }

// N4-c：**只查不改** —— 这段客体地址里有没有"GPU 写过、还没被回读回去"的页？半页竞态对策的第一问
// （有 ⇒ 调用方先回读整页并保留 CPU 刚写的那几个字节，然后再标 CPU 脏；见 vk2_xfer.h 的 vk2_hazard_merge）。
// N4-c: read-only query -- are there GPU-dirty (not yet read back) pages in this guest address range? The first
// question the half-page hazard handling asks before it marks a CPU write.
int vk2_dirty_any_gpu(uint64_t addr, uint64_t len) {
    if (!g_ready) return 0;
    addr = vk2_ptr_mask((uintptr_t)addr);
    if (!len) len = 1;
    int any = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool; i++) {
        vk2_pool *p = &g_pools[i];
        if (addr < p->lo || addr + len > p->hi) continue;
        any = vk2_dirty_any(&p->dirty, addr - p->lo, len, VK2_DIRTY_GPU);
        break;
    }
    pthread_mutex_unlock(&g_mu);
    return any;
}

int vk2_pool_mark_all_cpu(int i) {
    if (!g_ready) return 0;
    pthread_mutex_lock(&g_mu);
    int ok = 0;
    if (i >= 0 && i < g_npool) {
        vk2_dirty_mark_cpu(&g_pools[i].dirty, 0, g_pools[i].hi - g_pools[i].lo);
        ok = 1;
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

// ---------------------------------------------------------------- 快路（§9.8）：只落降级规则
int vk2_pin_capable(const char **why) {
    if (!g_ready) { if (why) *why = "池模块未就绪"; return 0; }
    VkPhysicalDeviceExternalBufferInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    bi.usage = VK2_FULL_USAGE;
    bi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalBufferProperties bp;
    memset(&bp, 0, sizeof bp);
    bp.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    vkGetPhysicalDeviceExternalBufferProperties(g_dev.phys, &bi, &bp);
    if (!(bp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
        if (why) *why = "不透明 fd（OPAQUE_FD）不支持导出";
        return 0;
    }
    if (why) *why = "不透明 fd 可导出（快路的前提成立）";
    return 1;
}

int vk2_pool_pin(void *mem) {
    if (!g_ready || !mem) return 0;
    const char *why = NULL;
    uintptr_t want = vk2_ptr_mask((uintptr_t)mem);
    pthread_mutex_lock(&g_mu);
    vk2_pool *p = NULL;
    for (int i = 0; i < g_npool; i++) if (g_pools[i].lo == want) { p = &g_pools[i]; break; }
    if (!p) { pthread_mutex_unlock(&g_mu); return 0; }
    g_st.pin_try++;
    int cap = vk2_pin_capable(&why);
    // 降级规则（§9.8）：任何一步失败 ⇒ 该池回落 SHADOW，**报计数不报红**；正确性路径不依赖它。
    // The degradation rule: any failing step drops the pool back to SHADOW, counted and never red; correctness
    // never depends on it.
    p->mode = VK2_MODE_SHADOW;
    g_st.pin_fail++;
    uint64_t nfail = g_st.pin_fail;
    pthread_mutex_unlock(&g_mu);
    if (!cap)
        V2LOG("【黄】池 %p 请求 PINNED ⇒ 回落 SHADOW：%s（pin_fail 累计 %llu；正确性路径本来就只走 SHADOW）",
              mem, why ? why : "", (unsigned long long)nfail);
    else
        V2LOG("【黄】池 %p 请求 PINNED ⇒ 回落 SHADOW：设备的前提成立（%s），但**真正的钉住本轮没实现**"
              "（N1 只落降级规则）⇒ pin_fail 累计 %llu；vk2_addr 不受影响",
              mem, why ? why : "", (unsigned long long)nfail);
    return 0;
}

// ---------------------------------------------------------------- 读数
void vk2_pool_stats_get(vk2_pool_stats *out) {
    if (!out) return;
    pthread_mutex_lock(&g_mu);
    *out = g_st;
    out->pools = (uint64_t)g_npool;
    pthread_mutex_unlock(&g_mu);
}
void vk2_pool_stats_reset(void) {
    pthread_mutex_lock(&g_mu);
    memset(&g_st, 0, sizeof g_st);
    g_st.pools = (uint64_t)g_npool;
    pthread_mutex_unlock(&g_mu);
}

void vk2_cross_hist_text(char *out, size_t cap) {
    if (!out || !cap) return;
    size_t n = 0;
    vk2_pool_stats st;
    vk2_pool_stats_get(&st);
    n += (size_t)snprintf(out + n, cap - n, "跨块请求长度分布（cross 共 %llu，miss %llu）",
                          (unsigned long long)st.cross, (unsigned long long)st.misses);
    for (int b = 0; b < VK2_XLEN_BUCKETS && n < cap; b++)
        n += (size_t)snprintf(out + n, cap - n, " / %s=%llu", xlen_name[b], (unsigned long long)st.cross_hist[b]);
}

// ---------------------------------------------------------------- 开关 DM_GFX2（缺省关）
int vk2_gfx2_enabled(void) {
    const char *v = getenv("DM_GFX2");
    if (!v || !*v) return 0;
    return (strcasecmp(v, "1") == 0 || strcasecmp(v, "on") == 0 ||
            strcasecmp(v, "yes") == 0 || strcasecmp(v, "true") == 0);
}

static int g_gpu_takenover;
void vk2_pool_set_gpu_takenover(int on) { g_gpu_takenover = on ? 1 : 0; }
// N4-c：回放层的贴图侧（`vk_tex.c` 那三处"一次性提交 + 等队列空闲"）要按同一条口径决定走不走它
// ——"GPU 侧接管"是这一件事的**唯一判据**，所以给一个只读的问法（不读环境变量第二次，避免两处口径漂）。
// N4-c: the texture side asks the same question -- "is the GPU side taken over" is the single criterion.
int vk2_gpu_takenover(void) { return g_gpu_takenover; }

const char *vk2_route_line(void) {
    static char line[360];
    uint64_t mb = (g_ready ? g_chunk : VK2_CHUNK_BYTES) >> 20;
    if (vk2_gfx2_enabled())
        snprintf(line, sizeof line,
                 "这次走**新地基**（DM_GFX2=1）：影子内存 + 分块后备资源（块 %llu MB）；"
                 "回放层的接管范围 = %s",
                 (unsigned long long)mb,
                 g_gpu_takenover
                   ? "**GPU 侧已接管**（N4-b：`mtl_addr` 回后备资源 + 帧开头上传；N4-c：提交侧 = `vk2_sub_*`、"
                     "GPU→池的回读、贴图侧不再等队列空闲）"
                   : "只登记池（N4-a：GPU 侧仍走旧路的零拷贝）");
    else
        snprintf(line, sizeof line,
                 "这次走**旧路**（DM_GFX2 未设/关）：mmap 换轨 + 8 槽 + vkQueueWaitIdle；"
                 "新地基（块 %llu MB）尚未接管（N4）", (unsigned long long)mb);
    return line;
}
