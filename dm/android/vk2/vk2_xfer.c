// 新地基 N2 之二：上传窗口与回读窗口的实现（规格与两处措辞差异见 vk2_xfer.h 的文件头）。
// N2 part two: the upload and readback windows (spec and the two deliberate deviations: see vk2_xfer.h).
#define _GNU_SOURCE 1
#include "vk2_xfer.h"

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
void vk2_xfer_set_log(void (*fn)(const char *fmt, ...)) { g_log = fn ? fn : def_log; }
#define XLOG(...) g_log(__VA_ARGS__)

static uint64_t now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// ---------------------------------------------------------------- 状态
static vk2_dev  g_dev;
static int      g_ready;
static uint64_t g_ring_bytes, g_rb_bytes;
static VkBuffer g_ring, g_rb;
static VkDeviceMemory g_ring_mem, g_rb_mem;
static void    *g_ring_ptr, *g_rb_ptr;

// N4：环按槽分 —— `g_bank[i]` 是第 i 段的**环形分配器**（一帧里可以录很多次上传）。缺省 1 段，
// 那时它的 offset = 0、bytes = 整个环 ⇒ 与 N2/N3 的单环一模一样（N2/N3 的探针读数因此不变）。
// N4: the ring split per slot. Each bank is an allocator for one frame's uploads; with one bank (the default) it
// covers the whole ring, which is exactly the N2/N3 single-window behaviour.
typedef struct {
    uint64_t off;         // 这一段在环里的起始偏移 | start offset inside the ring
    uint64_t bytes;       // 容量 | capacity
    uint64_t used;        // 本帧已用（对齐后）| used this frame (aligned up)
    int      open;        // 被一个在飞的窗口占着吗 | held by an in-flight window
    uint64_t windows, moved, ring_full;
} xbank;
static xbank    g_bank[VK2_BANKS_MAX];
static int      g_nbank = 1;
static VkCommandPool g_cpool;
static VkCommandBuffer g_cb;
static VkFence  g_cb_fence;           // 本模块自己的提交（回读拷贝）| this module's own submission fence
// N4-b：初始同步用自己的一套（与回读那条路分开 —— 回读可能同时从前端线程发出来）
static VkCommandPool   g_scpool;
static VkCommandBuffer g_scb;
static VkFence         g_scf;
static VkFence  g_producer;           // 回读要等谁 | whose fence the readback must wait for
static vk2_xfer_stats g_st;

// ---------------------------------------------------------------- G4 的前置自检（不许 vkQueueWaitIdle）
// 本文件里**只准等定向栅栏**（纪律，§9.5 / §9.6）。为了让它成为编译期就挡得住的东西，这里把
// `vkQueueWaitIdle` 这个名字在本编译单元里宏成"计数 + 报红"的包装：以后谁手滑写上一句，
// 编译会走这条路（计数不会静默为 0），而不是真的去等队列空闲。
// Only targeted fences are allowed in this file. To make that a compile-time trap, the name vkQueueWaitIdle is
// macro'd inside this translation unit into a counter + a red log, so a slip cannot silently wait for the queue.
static VkResult vk2_idle_forbidden(VkQueue q, int line);
#define vkQueueWaitIdle(q) vk2_idle_forbidden((q), __LINE__)

static VkResult vk2_idle_forbidden(VkQueue q, int line) {
    (void)q;
    g_st.idle_calls++;
    XLOG("【红】vk2_xfer 里出现了 vkQueueWaitIdle（第 %d 行）：G4 要求这个计数恒为 0 —— 只准等定向栅栏",
         line);
    return VK_ERROR_DEVICE_LOST;
}

// ---------------------------------------------------------------- 小工具
static uint32_t pick_type(uint32_t bits, VkMemoryPropertyFlags need) {
    uint32_t best = UINT32_MAX; VkDeviceSize bestsz = 0;
    for (uint32_t i = 0; i < g_dev.mp.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        if ((g_dev.mp.memoryTypes[i].propertyFlags & need) != need) continue;
        VkDeviceSize sz = g_dev.mp.memoryHeaps[g_dev.mp.memoryTypes[i].heapIndex].size;
        if (best == UINT32_MAX || sz > bestsz) { best = i; bestsz = sz; }
    }
    return best;
}

// 建一块 host-visible 缓冲并常驻映射 | create a host-visible buffer and map it once
static int host_buf(uint64_t bytes, VkBuffer *buf, VkDeviceMemory *mem, void **ptr) {
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof bci);
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_dev.dev, &bci, NULL, buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_dev.dev, *buf, &mr);
    uint32_t mi = pick_type(mr.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mi == UINT32_MAX) return 0;
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof mai);
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mi;
    if (vkAllocateMemory(g_dev.dev, &mai, NULL, mem) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(g_dev.dev, *buf, *mem, 0) != VK_SUCCESS) return 0;
    if (vkMapMemory(g_dev.dev, *mem, 0, bytes, 0, ptr) != VK_SUCCESS) return 0;
    return 1;
}

static int submit_wait(VkCommandBuffer cb, VkFence fe, const char *what) {
    VkSubmitInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VkResult r = vkQueueSubmit(g_dev.q, 1, &si, fe);
    if (r != VK_SUCCESS) { XLOG("【红】%s 提交失败：VkResult %d", what, (int)r); return 0; }
    r = vkWaitForFences(g_dev.dev, 1, &fe, VK_TRUE, (uint64_t)VK2_XFER_TIMEOUT_MS * 1000000ull);
    if (r != VK_SUCCESS) {
        g_st.fence_timeouts++;
        XLOG("【红】%s 等定向栅栏超时（%u ms）：VkResult %d —— **如实报失败，不重试、不放宽**",
             what, (unsigned)VK2_XFER_TIMEOUT_MS, (int)r);
        return 0;
    }
    return 1;
}

int  vk2_xfer_ready(void) { return g_ready; }
uint64_t vk2_xfer_ring_bytes(void) { return g_ring_bytes; }

// ---------------------------------------------------------------- N4：环按槽分
// 把环均分成 n 段。**有任何一段被占着就拒绝**（改动会把在飞窗口的环内容挪走）。缺省 = 1 段（整个环）。
// Split the ring into n banks; refused while any bank is held. One bank by default (the whole ring).
int vk2_xfer_set_banks(int n) {
    if (!g_ready) { XLOG("【红】vk2_xfer_set_banks：模块没就绪"); return 0; }
    if (n < 1) n = 1;
    if (n > VK2_BANKS_MAX) n = VK2_BANKS_MAX;
    for (int i = 0; i < g_nbank; i++) {
        if (g_bank[i].open) {
            XLOG("【红】vk2_xfer_set_banks(%d)：第 %d 段还被一个在飞窗口占着 ⇒ 拒绝改（先在回收时交还它）",
                 n, i);
            return 0;
        }
    }
    // 均分并**向下取到页**：每段的容量与起始偏移都必须页对齐（与上面"切片按页对齐"是同一条理由）
    uint64_t gran = vk2_dirty_gran_default();
    uint64_t per = (g_ring_bytes / (uint64_t)n) / gran * gran;
    if (!per) per = gran;
    memset(g_bank, 0, sizeof g_bank);
    for (int i = 0; i < n; i++) {
        g_bank[i].off = per * (uint64_t)i;
        // 最后一段吃到环尾，避免整除不了时丢字节 | the last bank takes the remainder
        g_bank[i].bytes = (i == n - 1) ? (g_ring_bytes - g_bank[i].off) : per;
        if (g_bank[i].off >= g_ring_bytes) { g_bank[i].bytes = 0; }
    }
    g_nbank = n;
    g_st.banks = (uint64_t)n;
    XLOG("暂存环按槽分：**%d 段**（每段 %llu KB；总环 %llu MB）；一段是一个环形分配器（一帧可多次上传）",
         n, (unsigned long long)(g_bank[0].bytes >> 10), (unsigned long long)(g_ring_bytes >> 20));
    return n;
}
int vk2_xfer_banks(void) { return g_nbank; }
uint64_t vk2_xfer_bank_bytes(int bank) {
    if (bank < 0 || bank >= g_nbank) return 0;
    return g_bank[bank].bytes;
}
void vk2_xfer_bank_stats_get(int bank, vk2_bank_stats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (bank < 0 || bank >= g_nbank) return;
    out->bytes = g_bank[bank].bytes;
    out->used = g_bank[bank].used;
    out->windows = g_bank[bank].windows;
    out->moved = g_bank[bank].moved;
    out->ring_full = g_bank[bank].ring_full;
    out->open = g_bank[bank].open;
}
int vk2_xfer_window_open_bank(int bank) {
    if (bank < 0 || bank >= g_nbank) return 0;
    return g_bank[bank].open;
}

int vk2_xfer_init(const vk2_dev *d, uint64_t ring_bytes, uint64_t rb_bytes) {
    if (!d || !d->dev || !d->q || !d->phys) { XLOG("【红】vk2_xfer_init：设备事实不全"); return 0; }
    vk2_xfer_fini();
    g_dev = *d;
    g_ring_bytes = ring_bytes ? ring_bytes : VK2_RING_BYTES_DEFAULT;
    g_rb_bytes = rb_bytes ? rb_bytes : VK2_RB_BYTES_DEFAULT;
    // 环的总量对齐到**页**（脏位图粒度）：切片与清位都按页走，环也页对齐才不会有"半页"的边角
    // （N0 读数：optimalBufferCopyOffsetAlignment = 64，页对齐当然满足它）。
    g_ring_bytes = align_up(g_ring_bytes, vk2_dirty_gran_default());
    g_rb_bytes = align_up(g_rb_bytes, 64);
    if (!host_buf(g_ring_bytes, &g_ring, &g_ring_mem, &g_ring_ptr)) {
        XLOG("【红】暂存环 %llu 字节建不出来（host-visible + 常驻映射）", (unsigned long long)g_ring_bytes);
        vk2_xfer_fini(); return 0;
    }
    if (!host_buf(g_rb_bytes, &g_rb, &g_rb_mem, &g_rb_ptr)) {
        XLOG("【红】回读暂存 %llu 字节建不出来", (unsigned long long)g_rb_bytes);
        vk2_xfer_fini(); return 0;
    }
    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    // 这条 **必须** 有（N0 踩过：不加时"某一条之后 vkQueueSubmit 开始返回 -3"，看着像驱动的锅）
    // Required (N0's bug: without it vkQueueSubmit started returning -3 after some point).
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_dev.qfi;
    if (vkCreateCommandPool(g_dev.dev, &pci, NULL, &g_cpool) != VK_SUCCESS) {
        XLOG("【红】命令池建不出来"); vk2_xfer_fini(); return 0;
    }
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof cbai);
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_cpool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g_dev.dev, &cbai, &g_cb) != VK_SUCCESS) {
        XLOG("【红】命令缓冲拿不到"); vk2_xfer_fini(); return 0;
    }
    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof fci);
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(g_dev.dev, &fci, NULL, &g_cb_fence) != VK_SUCCESS) {
        XLOG("【红】栅栏建不出来"); vk2_xfer_fini(); return 0;
    }
    // N4：缺省 **1 段**（= N2/N3 的单环行为）；回放层接管时再按槽数 set_banks()
    memset(g_bank, 0, sizeof g_bank);
    g_nbank = 1;
    g_bank[0].off = 0; g_bank[0].bytes = g_ring_bytes;
    g_st.banks = 1;
    g_ready = 1;
    XLOG("搬运窗口就绪：暂存环 %llu MB（%d 段）、回读暂存 %llu MB、超时 %u ms、**只等定向栅栏**",
         (unsigned long long)(g_ring_bytes >> 20), g_nbank, (unsigned long long)(g_rb_bytes >> 20),
         (unsigned)VK2_XFER_TIMEOUT_MS);
    return 1;
}

void vk2_xfer_fini(void) {
    if (g_dev.dev) {
        if (g_ring_ptr) vkUnmapMemory(g_dev.dev, g_ring_mem);
        if (g_rb_ptr) vkUnmapMemory(g_dev.dev, g_rb_mem);
        if (g_cb_fence) vkDestroyFence(g_dev.dev, g_cb_fence, NULL);
        if (g_scf) vkDestroyFence(g_dev.dev, g_scf, NULL);
        if (g_cpool) vkDestroyCommandPool(g_dev.dev, g_cpool, NULL);
        if (g_scpool) vkDestroyCommandPool(g_dev.dev, g_scpool, NULL);
        if (g_ring) vkDestroyBuffer(g_dev.dev, g_ring, NULL);
        if (g_rb) vkDestroyBuffer(g_dev.dev, g_rb, NULL);
        if (g_ring_mem) vkFreeMemory(g_dev.dev, g_ring_mem, NULL);
        if (g_rb_mem) vkFreeMemory(g_dev.dev, g_rb_mem, NULL);
    }
    memset(&g_dev, 0, sizeof g_dev);
    g_ring = g_rb = VK_NULL_HANDLE; g_ring_mem = g_rb_mem = VK_NULL_HANDLE;
    g_ring_ptr = g_rb_ptr = NULL; g_cpool = VK_NULL_HANDLE; g_cb = VK_NULL_HANDLE; g_cb_fence = VK_NULL_HANDLE;
    g_scpool = VK_NULL_HANDLE; g_scb = VK_NULL_HANDLE; g_scf = VK_NULL_HANDLE;
    g_ready = 0; g_producer = VK_NULL_HANDLE;
    memset(g_bank, 0, sizeof g_bank);
    g_nbank = 1;
}

// ---------------------------------------------------------------- 屏障（§9.4）
// 上传的每个 run 后面一条：`TRANSFER_WRITE → 各种 READ`。目标侧先**保守给全覆盖**
// （`ALL_COMMANDS` + `MEMORY_READ|MEMORY_WRITE`）——"这段接下来会被当什么用"的收窄留到 N4 有真实绑定点时做。
// One barrier after every uploaded run: TRANSFER_WRITE to reads. The destination side is deliberately
// conservative (ALL_COMMANDS + MEMORY_READ|MEMORY_WRITE); narrowing it belongs to N4, when there are real bindings.
static void barrier_after_upload(VkCommandBuffer cb, VkBuffer buf, uint64_t off, uint64_t size) {
    VkBufferMemoryBarrier bb;
    memset(&bb, 0, sizeof bb);
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = buf; bb.offset = off; bb.size = size;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
    g_st.barriers++;
}

// ---------------------------------------------------------------- 上传窗口（§9.5）
// 回调里做的事：把这段 run 从影子 memcpy 进暂存环，录一条 `vkCmdCopyBuffer(环 → 后备)`，插屏障，置 CLEAN。
// 一个 run 跨块时在这里被拆成多条拷贝（`split_runs` 计数）。
// The run callback: memcpy the run from the shadow into the ring, record vkCmdCopyBuffer(ring to chunk), insert
// the barrier and mark the pages CLEAN. A run crossing a chunk boundary is split here.
typedef struct {
    vk2_pool *pool;
    VkCommandBuffer cb;
    xbank   *bank;         // N4：搬进哪一段（每段一个环形分配器）| which bank to move into
    uint64_t moved;        // 本窗口搬的字节数 | bytes moved in this window
    int      stop;         // 环装不下 ⇒ 提前收尾 | ring full: stop, the rest stays dirty for the next window
} up_ctx;

static int upload_run(uint64_t pool_off, uint64_t len, void *ud) {
    up_ctx *c = (up_ctx *)ud;
    xbank *b = c->bank;
    uint64_t chunk_bytes = vk2_pool_chunk_bytes();
    uint8_t *shadow = (uint8_t *)(uintptr_t)c->pool->lo;
    uint64_t ncopy = 0;                                          // 这一条 run 拆成了几条拷贝
    while (len) {
        uint64_t k = pool_off / chunk_bytes;
        if (k >= (uint64_t)c->pool->nchunk) break;                // 没有后备（块数被上限截过）
        const vk2_chunk *ch = &c->pool->chunk[k];
        uint64_t rel = pool_off - ch->off;
        uint64_t take = ch->size - rel;                           // 块内到边界的余量
        if (take > len) take = len;
        // 暂存环的余量也要算进来：**一条 run 可能比环还大**（大范围拷贝 / 整张贴图），
        // 那就把它按环的大小拆成几条拷贝 —— 环装不下的那部分留给下一个窗口（那些页仍然是脏的）。
        // The ring's remaining space counts too: a single run may be larger than the ring, in which case it is
        // split into ring-sized copies and the rest stays dirty for the next window.
        uint64_t avail = (b->used < b->bytes) ? (b->bytes - b->used) : 0;
        if (avail < 64) {                                         // 这一段用光了（连一条最小拷贝都放不下）
            b->ring_full++;
            g_st.ring_full++;
            uint64_t used = b->used, rb = b->bytes;
            c->stop = 1;
            if (b->ring_full <= 4)
                XLOG("【黄】暂存环这一段用光了（本窗口已用 %llu / %llu MB）⇒ 本窗口收尾：已搬的置 CLEAN，"
                     "剩下的仍是脏、下一个窗口继续", (unsigned long long)(used >> 20),
                     (unsigned long long)(rb >> 20));
            break;
        }
        // 切片必须**按脏位图的粒度（页）对齐**：搬完要 `vk2_dirty_clean(pool_off, take)`，而清位是按页的
        // ⇒ 切片只要落在页中间，尾部那不足一页的字节就会被**误标 CLEAN 却从没上传**（影子与后备不一致，
        // 而且记账看不出来）。N4-b 的探针把这条抓出来了：初始同步搬 7.99 MB 而不是 8 MB，回读头尾对不上。
        // The slice must be aligned to the dirty bitmap's granularity: cleaning is per page, so a slice ending
        // inside a page would mark the tail CLEAN without ever uploading it.
        uint64_t gran = vk2_dirty_gran_default();
        uint64_t cap = take;
        if (cap > avail) cap = avail;
        cap = (cap / gran) * gran;
        if (cap == 0) { b->ring_full++; g_st.ring_full++; c->stop = 1; break; }  // 余量不足一页
        take = cap;
        memcpy((uint8_t *)g_ring_ptr + b->off + b->used, shadow + pool_off, (size_t)take);
        VkBufferCopy cp;
        memset(&cp, 0, sizeof cp);
        cp.srcOffset = b->off + b->used; cp.dstOffset = rel; cp.size = take;
        vkCmdCopyBuffer(c->cb, g_ring, ch->buf, 1, &cp);
        g_st.copies++;
        barrier_after_upload(c->cb, ch->buf, rel, take);
        vk2_dirty_clean(&c->pool->dirty, pool_off, take);         // 搬完就是 CLEAN（影子与后备一致）
        b->used += align_up(take, 64);
        c->moved += take;
        ncopy++;
        pool_off += take;
        len -= take;
    }
    if (ncopy > 1) g_st.split_runs += ncopy - 1;                  // 多出来的拷贝（跨块 / 超出暂存环）
    return c->stop ? 1 : 0;
}

// 一个池搬一遍（搬进 `bank` 那一段）。`fresh` = 这一段本帧还没被用过（used 从 0 起算）。
// Move one pool into the given bank; fresh means this bank has not been used yet this frame.
static uint64_t upload_pool(int bank, VkCommandBuffer cb, int pool_index) {
    vk2_pool *pool = (vk2_pool *)(uintptr_t)vk2_pool_at(pool_index);
    if (!pool) { XLOG("【红】vk2_upload：池号 %d 不存在", pool_index); return 0; }
    xbank *b = &g_bank[bank];
    uint64_t t0 = now_us();
    up_ctx c;
    memset(&c, 0, sizeof c);
    c.pool = pool; c.cb = cb; c.bank = b;
    uint64_t runs = vk2_dirty_scan(&pool->dirty, VK2_DIRTY_CPU, 0, pool->hi - pool->lo, upload_run, &c);
    if (c.moved) {
        g_st.runs += runs;
        g_st.upload_bytes += c.moved;
        g_st.upload_us += now_us() - t0;
        b->moved += c.moved;
    } else if (runs && c.stop) {
        // 一字节都没搬、却已经收尾：分两种情形 —— **这一段本帧早就用满了**（前面搬过了，剩下的留给下一个
        // 窗口/下一帧，报【黄】）与**第一条 run 就超过整段容量**（那才是"环配小了"，报【红】）。
        // 混成一条【红】会把"用满了"读成"配置错"（N4-b 第一次真机轮就吃了这个亏：日志里几百条红看起来
        // 像环配得荒唐，其实是一帧要搬的量超过了每段 16 MB）。
        if (b->used)
            XLOG("【黄】暂存环这一段本帧已经用满（%llu / %llu MB）⇒ 这一个池这次没搬：剩下的仍是脏，"
                 "下一个窗口 / 下一帧继续", (unsigned long long)(b->used >> 20),
                 (unsigned long long)(b->bytes >> 20));
        else
            XLOG("【红】暂存环这一段 %llu 字节连一条 run 都装不下 ⇒ 环配小了（%d 段）",
                 (unsigned long long)b->bytes, g_nbank);
    }
    return c.moved;
}

// 旧签名（N2 起就在用）：**单环语义** —— 上一个窗口没收尾就拒绝，窗口记在**第 0 段**上。
// The original entry point: single-ring semantics on bank 0 (an unfinished window is refused).
uint64_t vk2_upload(VkCommandBuffer cb, uint64_t frame, int pool_index) {
    (void)frame;
    if (!g_ready || !cb) return 0;
    if (g_bank[0].open) {
        XLOG("【红】上一个上传窗口还没 `vk2_xfer_window_done()`：这一次拒绝（否则会盖掉在飞窗口的环内容）");
        return 0;
    }
    g_bank[0].used = 0;
    uint64_t moved = upload_pool(0, cb, pool_index);
    if (moved) { g_bank[0].open = 1; g_bank[0].windows++; g_st.windows++; }
    return moved;
}

// N4 的入口：走指定那一段；第一次调用开窗口，之后往同一段里追加（一帧里可以搬好几个池）。
// The N4 entry point: into a named bank, opening the window on first use and appending after that.
uint64_t vk2_upload_into(int bank, VkCommandBuffer cb, uint64_t frame, int pool_index) {
    (void)frame;
    if (!g_ready || !cb) return 0;
    if (bank < 0 || bank >= g_nbank) { XLOG("【红】vk2_upload_into：段号 %d 越界（现在 %d 段）", bank, g_nbank); return 0; }
    xbank *b = &g_bank[bank];
    int fresh = !b->open;
    if (fresh) { b->used = 0; b->open = 1; }
    uint64_t moved = upload_pool(bank, cb, pool_index);
    if (moved && fresh) { b->windows++; g_st.windows++; }
    else if (!moved && fresh) b->open = 0;   // 没搬东西就别占着这一段（否则要等一次没意义的交还）
    return moved;
}

// 把**所有**池搬进这一段（帧开头的上传就是它）。返回这一帧搬上去的字节数（11.3 的第一个字段）。
uint64_t vk2_upload_all_into(int bank, VkCommandBuffer cb, uint64_t frame) {
    if (!g_ready || !cb || bank < 0 || bank >= g_nbank) return 0;
    uint64_t total = 0;
    int n = vk2_pool_count();
    for (int i = 0; i < n; i++) total += vk2_upload_into(bank, cb, frame, i);
    return total;
}

void vk2_xfer_window_done_bank(int bank) {
    if (bank < 0 || bank >= g_nbank) return;
    if (g_bank[bank].open) g_st.ring_wrap++;
    g_bank[bank].open = 0;
    g_bank[bank].used = 0;
}

void vk2_xfer_window_done(void) { vk2_xfer_window_done_bank(0); }

// 只读：第 0 段现在被一个窗口占着吗（N3 用它判断"这个槽能不能录上传窗口"）| read-only: is bank 0 held
int vk2_xfer_window_open(void) { return vk2_xfer_window_open_bank(0); }

// ---------------------------------------------------------------- 回读窗口（§9.5）
void vk2_readback_after(VkFence producer) { g_producer = producer; }

int vk2_readback(VkBuffer src, uint64_t off, void *dst, size_t n) {
    if (!g_ready || !src || !dst || !n) return 0;
    if (!g_producer) {
        g_st.rb_no_fence++;
        XLOG("【黄】回读没指定要等谁的栅栏（`vk2_readback_after`）⇒ **拒绝**：不知道等谁也照读就是静默旧值");
        return 0;
    }
    uint64_t t0 = now_us();
    VkResult r = vkWaitForFences(g_dev.dev, 1, &g_producer, VK_TRUE, (uint64_t)VK2_XFER_TIMEOUT_MS * 1000000ull);
    if (r != VK_SUCCESS) {
        g_st.fence_timeouts++;
        XLOG("【红】回读：等生产者栅栏超时（%u ms）：VkResult %d —— 如实报失败"
             "（没有「多等一会儿再看看」这条路）", (unsigned)VK2_XFER_TIMEOUT_MS, (int)r);
        return 0;
    }
    g_st.rb_wait_us += now_us() - t0;

    uint64_t done = 0;
    while (done < n) {
        uint64_t take = n - done;
        if (take > g_rb_bytes) take = g_rb_bytes;
        vkResetCommandBuffer(g_cb, 0);
        VkCommandBufferBeginInfo bi;
        memset(&bi, 0, sizeof bi);
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(g_cb, &bi) != VK_SUCCESS) return 0;
        VkBufferCopy cp;
        memset(&cp, 0, sizeof cp);
        cp.srcOffset = off + done; cp.dstOffset = 0; cp.size = take;
        vkCmdCopyBuffer(g_cb, src, g_rb, 1, &cp);
        if (vkEndCommandBuffer(g_cb) != VK_SUCCESS) return 0;
        vkResetFences(g_dev.dev, 1, &g_cb_fence);
        if (!submit_wait(g_cb, g_cb_fence, "回读拷贝")) return 0;
        memcpy((uint8_t *)dst + done, g_rb_ptr, (size_t)take);
        done += take;
    }
    g_st.rb_calls++;
    g_st.rb_bytes += n;
    return 1;
}

int vk2_readback_pool(void *pool_mem, uint64_t pool_off, size_t n) {
    uintptr_t base = vk2_ptr_mask((uintptr_t)pool_mem);
    vk2_pool *pool = NULL;
    for (int i = 0; i < vk2_pool_count(); i++) {
        const vk2_pool *p = vk2_pool_at(i);
        if (p && p->lo == base) { pool = (vk2_pool *)(uintptr_t)p; break; }
    }
    if (!pool) { XLOG("【黄】回读：%p 不是一个已登记的池", pool_mem); return 0; }
    if (pool_off + n > pool->hi - pool->lo) { XLOG("【红】回读：池内偏移 %llu+%zu 越出池范围", (unsigned long long)pool_off, n); return 0; }

    uint64_t chunk_bytes = vk2_pool_chunk_bytes();
    uint8_t *shadow = (uint8_t *)base;
    uint64_t left = n, at = pool_off;
    while (left) {
        uint64_t k = at / chunk_bytes;
        if (k >= (uint64_t)pool->nchunk) { XLOG("【红】回读：池内 %llu 没有后备块", (unsigned long long)at); return 0; }
        const vk2_chunk *ch = &pool->chunk[k];
        uint64_t rel = at - ch->off;
        uint64_t take = ch->size - rel;
        if (take > left) take = left;
        if (!vk2_readback(ch->buf, rel, shadow + at, (size_t)take)) return 0;
        // 读回来的就是 GPU 的内容 ⇒ 影子与后备一致，这段置 CLEAN（GPU_DIRTY 也一起清）。
        // **清位只清整页的那一部分**：尾部不足一页的字节没有被整页回读，保持脏（宁可多回读一次，
        // 也不能把"没读全的一页"记成一致 —— 与上传切片同一条理由）。
        uint64_t gran = vk2_dirty_gran_default();
        uint64_t whole = take / gran * gran;
        if (whole) vk2_dirty_clean(&pool->dirty, at, whole);
        at += take;
        left -= take;
    }
    return 1;
}

// ---------------------------------------------------------------- N4-b：初始同步
uint64_t vk2_xfer_initial_sync(int bank) {
    if (!g_ready) return 0;
    if (bank < 0 || bank >= g_nbank) {
        XLOG("【红】vk2_xfer_initial_sync：段号 %d 越界（现在 %d 段）", bank, g_nbank);
        return 0;
    }
    if (!g_scb) {
        VkCommandPoolCreateInfo pci;
        memset(&pci, 0, sizeof pci);
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;   // N0 的第二个坑：必须有
        pci.queueFamilyIndex = g_dev.qfi;
        if (vkCreateCommandPool(g_dev.dev, &pci, NULL, &g_scpool) != VK_SUCCESS) {
            XLOG("【红】初始同步：命令池建不出来"); return 0;
        }
        VkCommandBufferAllocateInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = g_scpool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(g_dev.dev, &ai, &g_scb) != VK_SUCCESS) {
            XLOG("【红】初始同步：命令缓冲拿不到"); return 0;
        }
        VkFenceCreateInfo fi;
        memset(&fi, 0, sizeof fi);
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(g_dev.dev, &fi, NULL, &g_scf) != VK_SUCCESS) {
            XLOG("【红】初始同步：栅栏建不出来"); return 0;
        }
    }
    uint64_t total = 0;
    int rounds = 0;
    for (; rounds < 65536; rounds++) {
        if (g_bank[bank].open) {   // 上一轮没交还（不该发生）：先交还，否则这一轮的 memcpy 会盖在飞窗口
            XLOG("【黄】初始同步：第 %d 段还被占着 ⇒ 先交还（谁没按纪律交还？）", bank);
            vk2_xfer_window_done_bank(bank);
        }
        vkResetCommandBuffer(g_scb, 0);
        VkCommandBufferBeginInfo bi;
        memset(&bi, 0, sizeof bi);
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(g_scb, &bi) != VK_SUCCESS) { XLOG("【红】初始同步：命令缓冲开不了录制"); return total; }
        uint64_t moved = vk2_upload_all_into(bank, g_scb, 0);
        if (vkEndCommandBuffer(g_scb) != VK_SUCCESS) { XLOG("【红】初始同步：命令缓冲收不了尾"); return total; }
        if (!moved) break;                       // 没有可搬的脏页（或整批被拒）⇒ 收工
        vkResetFences(g_dev.dev, 1, &g_scf);
        if (!submit_wait(g_scb, g_scf, "初始同步")) return total;   // 超时如实报失败，不重试
        vk2_xfer_window_done_bank(bank);
        total += moved;
    }
    if (rounds >= 65536) XLOG("【红】初始同步：轮数到上限（%d）还没搬完 ⇒ 剩下的仍是脏（如实报）", rounds);
    g_st.initial_rounds = (uint64_t)rounds;
    g_st.initial_bytes += total;
    return total;
}

void vk2_xfer_stats_get(vk2_xfer_stats *out) { if (out) *out = g_st; }
void vk2_xfer_stats_reset(void) { memset(&g_st, 0, sizeof g_st); }
