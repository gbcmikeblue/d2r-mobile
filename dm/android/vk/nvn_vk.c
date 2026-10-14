// NVN 空设备的 Vulkan 回放层（C 接口）——`nvn_mtl.m` 的安卓替身。
// The Vulkan replay layer for the NVN null device (C interface): the Android stand-in for `nvn_mtl.m`.
//
// 与 Metal 那边的对应关系（接口与语义见 nvn_mtl.h）：
//   · 内存池 = 客体地址空间里的一段（已经是 Vulkan 零拷贝内存，见 vk_region.c），按页对齐的偏移绑一块 VkBuffer；
//   · 地址反查 = 同一份分配上的偏移换算，纯算术，不需要拷贝；
//   · 提交 = 一条 VkCommandBuffer + 一个栅栏值；命令流（nvn_obj.h 的 op 表）由一个与 Metal 侧同形的小解释器
//     走一遍，"GPU 完成后才生效"的副作用（事件写值、计数器）登记在提交上、等栅栏到了再做。
// Correspondence with the Metal side (interface and semantics in nvn_mtl.h):
//   - a memory pool is a range of the guest address space (already Vulkan zero-copy memory, see
//     vk_region.c) with a VkBuffer bound at a page-aligned offset;
//   - address lookup is offset arithmetic inside the same allocation, with no copying;
//   - a submission is one VkCommandBuffer plus a fence value, and the command stream (the op table in
//     nvn_obj.h) is walked by a small interpreter shaped like the Metal one, with the "only after the GPU
//     completes" effects (event writes, counters) recorded on the submission and run once its fence signals.
//
// 范围：M2 只做语义等价，不搬 Metal 那边的优化（分配器环、延迟合并通道、参数缓冲、常驻集）。命令流里的
// 渲染类 op 目前只统计不执行——统计出来才知道该按什么次序实现（日志里每 300 次提交打一次）。
// Scope: M2 aims only at semantic equivalence and does not carry over Metal's optimisations (allocator rings,
// deferred merged passes, argument buffers, residency sets). Render-side ops in the command stream are only
// counted for now, not executed -- the counts are what tell us which to implement first (logged every 300
// submissions).
#include "nvn_mtl.h"
#include "nvn_obj.h"
#include "vk_device.h"
#include "vk_region.h"
#include "vk_tex.h"
#include "dm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    const void *key;        // 客体给的池子首地址（mtl_pool_del 按它找）| the pool base address the guest gave (mtl_pool_del looks it up by this)
    uint64_t lo, hi;        // 池子的 [mem, mem+size) | the pool's [mem, mem+size)
    uint64_t off;           // 池子首地址在那份 Vulkan 分配里的偏移 | offset of the pool base inside that Vulkan allocation
    VkBuffer buf;           // 覆盖整个池子的缓冲 | the buffer covering the whole pool
} pool_ent;

static pool_ent *g_pools;
static int g_npool, g_cap;
static uint64_t g_pool_adds;    // 累计登记次数（mtl_res_count 的 pool_adds）| cumulative registrations (pool_adds in mtl_res_count)
static int g_pool_no_region;    // 落在 Vulkan 换轨范围之外的池子数：这个数应当一直是 0（见 docs/ANDROID_PLAN）| pools outside the overlaid ranges: this must stay 0 (see docs/ANDROID_PLAN)
static int g_pool_small;        // 只能按传输用法建出来的池子数（太大，见 make_pool_buffer）| pools that could only be created transfer-only (too large, see make_pool_buffer)

void mtl_init(void) { vk_device_init(); }

// 池子缓冲的用法：池子里什么都有（顶点、索引、常量、纹理上传），所以一开始给全套。
// 但真机上 4 GB 的缓冲带 STORAGE_BUFFER 建不出来（撞 maxStorageBufferRange，与内存换轨的载体缓冲同一个坑），
// 所以建不出来时退成只带传输位，并把这一类池子计数报出来——那种池子里的东西目前上不了 GPU。
// Usage for a pool buffer: a pool holds anything (vertices, indices, constants, texture uploads), so start
// with the full set. On the real device, though, a 4 GB buffer with STORAGE_BUFFER cannot be created (it hits
// maxStorageBufferRange, the same trap as the overlay's carrier buffer), so fall back to transfer-only and
// count that class of pool -- whatever is inside such a pool cannot reach the GPU yet.
static VkBuffer make_pool_buffer(uint64_t size, int *fell_back) {
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer b = VK_NULL_HANDLE;
    *fell_back = 0;
    if (vkCreateBuffer(vk_dev(), &bci, NULL, &b) == VK_SUCCESS) return b;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    b = VK_NULL_HANDLE;
    if (vkCreateBuffer(vk_dev(), &bci, NULL, &b) == VK_SUCCESS) *fell_back = 1;
    return b;
}

void mtl_pool_add(const void *mem, uint64_t size) {
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE || !mem || !size) return;

    VkDeviceMemory dm = VK_NULL_HANDLE;
    uint64_t off = 0;
    if (!vk_region_find((uint64_t)(uintptr_t)mem, size, &dm, &off)) {
        // 池子开在没换轨的内存上（栈 / 预留段）：当前无法把它变成 GPU 缓冲。
        // 先把这个数报出来——真机上要看到它一直是 0，才说明换轨范围覆盖了游戏的实际用法。
        // The pool sits on memory that was never converted (the stack / the rest of the reservation): it
        // cannot become a GPU buffer today. Report the count first -- it has to stay 0 on the real device for
        // the overlaid ranges to be shown to cover what the game actually does.
        if (g_pool_no_region++ < 8)
            LOG("【红】内存池 %p+%#llx 不在 GPU 可见范围内：池子里的顶点/纹理用不了（累计 %d 个）",
                mem, (unsigned long long)size, g_pool_no_region);
        return;
    }

    int fell_back = 0;
    VkBuffer buf = make_pool_buffer(size, &fell_back);
    if (buf == VK_NULL_HANDLE) {
        LOG("【红】内存池 %p+%#llx 建缓冲失败", mem, (unsigned long long)size);
        return;
    }
    VkResult r = vkBindBufferMemory(vk_dev(), buf, dm, off);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(vk_dev(), buf, NULL);
        LOG("【红】内存池 %p+%#llx 绑到偏移 %#llx 失败（VkResult %d）：偏移与缓冲的内存对齐不匹配？",
            mem, (unsigned long long)size, (unsigned long long)off, (int)r);
        return;
    }
    if (fell_back) g_pool_small++;

    pthread_mutex_lock(&g_mu);
    if (g_npool == g_cap) { g_cap = g_cap ? g_cap * 2 : 32; g_pools = realloc(g_pools, (size_t)g_cap * sizeof *g_pools); }
    g_pools[g_npool].key = mem;
    g_pools[g_npool].lo = (uint64_t)(uintptr_t)mem; g_pools[g_npool].hi = g_pools[g_npool].lo + size;
    g_pools[g_npool].off = off; g_pools[g_npool].buf = buf;
    g_npool++; g_pool_adds++;
    int n = g_npool;
    pthread_mutex_unlock(&g_mu);

    // 头 8 个报明细（看清游戏的内存池长什么样），之后只留计数
    // Detail the first eight (to see what the game's pools look like), counts only after that
    if (n <= 8)
        LOG("内存池 #%d %p+%llu MB：偏移 %#llx%s", n, mem, (unsigned long long)(size >> 20), (unsigned long long)off,
            fell_back ? "（太大，只按传输用法建：池内数据暂上不了 GPU）" : "");
}

void mtl_pool_del(const void *mem) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool; i++)
        if (g_pools[i].key == mem) {
            vkDestroyBuffer(vk_dev(), g_pools[i].buf, NULL);
            g_pools[i] = g_pools[--g_npool];
            break;
        }
    pthread_mutex_unlock(&g_mu);
}

// 地址反查：找到覆盖 [addr, addr+len) 的池子，回它的缓冲与池内偏移（= addr − 池子首地址）。
// 与 Metal 那边约定一致：成功 1、失败 0；*buf 在 Vulkan 这边是 VkBuffer 句柄（回放层自己用）。
// Address lookup: find the pool covering [addr, addr+len) and return its buffer plus the in-pool offset
// (= addr - the pool base). Same contract as the Metal side: 1 on success, 0 on failure; on Vulkan *buf is a
// VkBuffer handle (used inside the replay layer).
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool; i++)
        if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) {
            if (buf) *buf = (void *)g_pools[i].buf;
            if (off) *off = addr - g_pools[i].lo;
            ok = 1;
            break;
        }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

// 队列上全部命令完成：安卓这边交出队列权（Vulkan 的等价物就是等队列空闲）。
// Everything on the queue has completed: on Android this is simply waiting for the queue to go idle.
void mtl_drain(void) { if (vk_dev()) vkQueueWaitIdle(vk_queue()); }
void mtl_finish(void) { if (vk_dev()) vkQueueWaitIdle(vk_queue()); }

// 统计出参：waits 类的计数还没做（提交落地后才有），池子数是现成的事实。
// Statistics out-parameters: the waits counters do not exist yet (they arrive with submission), the pool
// count is a fact already available.
void mtl_alloc_stats(uint64_t *waits, uint64_t *gpu_waits, int *pools) {
    if (waits) *waits = 0;
    if (gpu_waits) *gpu_waits = 0;
    pthread_mutex_lock(&g_mu);
    if (pools) *pools = g_npool;
    pthread_mutex_unlock(&g_mu);
}

// 给诊断用：换轨范围之外的池子数、只能按传输用法建的池子数（见 docs/ANDROID_PLAN 的两处硬边界）。
// For diagnostics: how many pools fell outside the overlaid ranges and how many could only be created
// transfer-only (the two hard boundaries in docs/ANDROID_PLAN).
void vk_pool_stats(int *no_region, int *transfer_only, uint64_t *adds) {
    if (no_region) *no_region = g_pool_no_region;
    if (transfer_only) *transfer_only = g_pool_small;
    if (adds) *adds = g_pool_adds;
}

// ========== 提交与命令流 ==========
// ========== Submission and the command stream ==========

// 命令流里的 op 名（顺序与 nvn_obj.h 的枚举一致，一一对应）| Op names in the command stream (same order as the enum in nvn_obj.h, one to one)
static const char *const g_opname[OP_COUNT_] = {
    "END", "JUMP", "BEGIN", "CALL",
    "SIGNAL_EVENT", "REPORT_COUNTER", "COPY_B2B", "UPDATE_UB",
    "SET_RT", "CLEAR_COLOR", "CLEAR_DS", "VIEWPORTS", "SCISSOR", "DEPTH_RANGE",
    "BIND_PROGRAM", "BIND_STATE", "BIND_VATTR", "BIND_VSTREAM", "BIND_VBUF",
    "BIND_UBO", "BIND_SSBO", "BIND_TEX", "BIND_SAMP",
    "STENCIL", "POLY_OFFSET", "DRAW", "DRAW_IDX", "DISPATCH", "BARRIER",
    "SET_TEX_POOL", "SET_SAMP_POOL", "COPY_B2T", "COPY_STRIDE", "BLEND_COLOR", "COPY_T2B",
    "BIND_IMG", "COPY_T2T", "RESET_COUNTER", "CLEAR_TEX",
};
static _Atomic uint64_t g_opcount[OP_COUNT_];
static _Atomic uint64_t g_sub_total, g_op_unhandled;

// 「GPU 完成后才生效」的副作用：事件写值 / 计数器 / 落不到 GPU 拷贝上的小搬移
// Effects that take place only after the GPU completes: event writes, counters, and small moves that no GPU
// copy region can express
enum { POST_EVENT, POST_COUNTER, POST_MEMCPY };
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr, type; void *src, *dst; size_t n; } post_op;

#define MAX_INFLIGHT 8
#define MAX_POST 512
#define STAGE_SIZE (4u << 20)
typedef struct {
    VkFence fence;
    VkCommandBuffer cb;
    VkBuffer stage; void *stage_map; uint64_t stage_off;
    uint64_t value;
    int pending, cur;
    post_op post[MAX_POST]; int npost;
} inflight_t;
static inflight_t g_inf[MAX_INFLIGHT];
static uint64_t g_fence;
static int g_cur_slot = -1;   // 正在录制的槽（-1 = 没在录）| the slot being recorded (-1 = not recording)

// 渲染状态影子与通道机械的**前置声明**：结构定义放这儿（提交侧的 begin/commit 也要用），函数体在命令流那一节。
// Forward declarations of the render-state shadow and the pass machinery: the struct is defined here because the
// submission side (begin / commit) uses it too, while the function bodies live in the command-stream section.
#define SH_BIND 16
typedef struct {
    // 目标与清除（在开通道时物化成 loadOp）
    // Targets and clears (materialised into loadOps when the pass opens)
    const tex_t *color[8]; int ncolor; const tex_t *depth;
    int cc_have, cc_mask; float cc[8][4];
    int cd_have, cs_have; float cd; int cs; unsigned cd_mask, cs_mask;
    int open;                        // 通道是否已开 | whether a pass is open
    // 动态状态
    VkViewport vp[16]; int nvp;
    VkRect2D sc[16]; int nsc;
    float drange[2]; int drange_have;
    // 影子绑定：状态对象原样存（绘制时再解释），其余只记存在与次数
    // Shadow bindings: state objects are stored verbatim (interpreted at draw time); the rest only note existence
    uint8_t st_bytes[6][0x18]; int st_have[6];
    const prog_t *prog;
    uint64_t blend_rgba;             // 混合常量（4 个 float 原样）| blend constant (four floats verbatim)
    int stencil_have, poly_have;
} rstate_t;
static rstate_t g_rs;
static _Atomic uint64_t g_pass_n, g_clear_n, g_draw_skip, g_shadow_n;
static void pass_begin(void);
static void pass_end(void);

static void inflight_reap(void) {
    for (int i = 0; i < MAX_INFLIGHT; i++) {
        if (!g_inf[i].pending) continue;
        if (vkGetFenceStatus(vk_dev(), g_inf[i].fence) != VK_SUCCESS) continue;
        // GPU 已完成：先把"GPU 完成后才生效"的副作用做掉，再放掉这个槽
        // The GPU has completed: run the "only after completion" effects first, then release the slot
        for (int k = 0; k < g_inf[i].npost; k++) {
            const post_op *o = &g_inf[i].post[k];
            if (o->kind == POST_EVENT) { if (o->mode == 0) *o->val = o->v; else *o->val += o->v; }
            else if (o->kind == POST_COUNTER) { if (o->addr) *(volatile uint32_t *)(uintptr_t)o->addr = (uint32_t)o->type; }
            else if (o->kind == POST_MEMCPY) memcpy(o->dst, o->src, o->n);
        }
        g_inf[i].npost = 0;
        g_inf[i].pending = 0;
    }
}

static int inflight_take(void) {   // 取一个空闲槽：没有就等（单一队列按序完成，等最老的那个即可）| Take a free slot, waiting if none is free
    for (;;) {
        inflight_reap();
        for (int i = 0; i < MAX_INFLIGHT; i++) if (!g_inf[i].pending) return i;
        for (int i = 0; i < MAX_INFLIGHT; i++)
            if (vkWaitForFences(vk_dev(), 1, &g_inf[i].fence, VK_TRUE, 1000000000ull) == VK_SUCCESS) { inflight_reap(); break; }
    }
}

static int vk_sub_ready(void) {
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return 0;
    if (!g_inf[0].fence) {
        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = vk_queue_family() };
        VkCommandPool pool = VK_NULL_HANDLE;
        if (vkCreateCommandPool(vk_dev(), &pci, NULL, &pool) != VK_SUCCESS) { LOG("【红】建命令池失败"); return 0; }
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = MAX_INFLIGHT };
        VkCommandBuffer cbs[MAX_INFLIGHT];
        if (vkAllocateCommandBuffers(vk_dev(), &ai, cbs) != VK_SUCCESS) { LOG("【红】分配命令缓冲失败"); return 0; }
        for (int i = 0; i < MAX_INFLIGHT; i++) {
            VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
            if (vkCreateFence(vk_dev(), &fi, NULL, &g_inf[i].fence) != VK_SUCCESS) { LOG("【红】建栅栏失败"); return 0; }
            g_inf[i].cb = cbs[i];
            VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = STAGE_SIZE,
                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
            if (vkCreateBuffer(vk_dev(), &bci, NULL, &g_inf[i].stage) != VK_SUCCESS) { LOG("【红】建暂存缓冲失败"); return 0; }
            VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vk_dev(), g_inf[i].stage, &mr);
            VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                .memoryTypeIndex = vk_memory_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mr.memoryTypeBits) };
            VkDeviceMemory mem = VK_NULL_HANDLE;
            if (vkAllocateMemory(vk_dev(), &mai, NULL, &mem) != VK_SUCCESS) { LOG("【红】暂存缓冲内存分配失败"); return 0; }
            vkBindBufferMemory(vk_dev(), g_inf[i].stage, mem, 0);
            vkMapMemory(vk_dev(), mem, 0, STAGE_SIZE, 0, &g_inf[i].stage_map);
        }
        LOG("【绿】回放层就绪：%d 个提交槽（每条一条 VkCommandBuffer + 栅栏 %d MB 暂存）", MAX_INFLIGHT, STAGE_SIZE >> 20);
    }
    return 1;
}

void mtl_sub_begin(void) {
    if (!vk_sub_ready()) return;
    int i = inflight_take();
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (vkBeginCommandBuffer(g_inf[i].cb, &bi) != VK_SUCCESS) { LOG("【红】开始命令缓冲失败"); return; }
    g_inf[i].cur = 1; g_inf[i].npost = 0; g_inf[i].stage_off = 0;
    // 一次提交 = 一段连续的命令流：目标与清除按提交清零，视口/裁剪/绑定这些跨命令缓冲保留
    // （游戏在一个提交里给多个命令缓冲，状态在它们之间是接着用的）
    // One submission is one continuous command stream: targets and clears are cleared per submission, while
    // viewport / scissor / bindings carry across command buffers (the game passes several command buffers in one
    // submission and state continues across them).
    g_rs.ncolor = 0; g_rs.depth = NULL;
    g_rs.cc_have = 0; g_rs.cc_mask = 0; g_rs.cd_have = 0; g_rs.cs_have = 0;
    g_rs.open = 0;
    g_cur_slot = i;
}

static void post_add(const post_op *o) {
    if (g_cur_slot < 0) return;
    inflight_t *in = &g_inf[g_cur_slot];
    if (in->npost >= MAX_POST) { static int said; if (!said++) LOG("【红】一次提交的副作用表已满（%d）", MAX_POST); return; }
    in->post[in->npost++] = *o;
}

void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    if (!n || g_cur_slot < 0) return;
    void *sb = NULL, *db = NULL; uint64_t so = 0, dof = 0;
    if (!mtl_addr(src, n, &sb, &so) || !mtl_addr(dst, n, &db, &dof)) {
        static int said; if (!said++) LOG("【红】提交里的缓冲拷贝落不到池子里：%#llx → %#llx +%#llx", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
        return;
    }
    // GPU 拷贝要求偏移 4 字节对齐；池子里的地址多是 16 字节对齐的，不对齐的少数退成"栅栏到了再搬"
    // A GPU copy region needs 4-byte aligned offsets; pool addresses are mostly 16-byte aligned, and the few
    // unaligned ones fall back to "move once the fence signals"
    if ((so & 3) || (dof & 3)) {
        post_op o = { .kind = POST_MEMCPY, .src = (void *)(uintptr_t)src, .dst = (void *)(uintptr_t)dst, .n = (size_t)n };
        post_add(&o);
        static int said; if (!said++) LOG("【黄】有缓冲拷贝偏移不是 4 字节对齐（%#llx→%#llx），退成完成后搬", (unsigned long long)so, (unsigned long long)dof);
        return;
    }
    VkBufferCopy r = { .srcOffset = so, .dstOffset = dof, .size = n };
    vkCmdCopyBuffer(g_inf[g_cur_slot].cb, (VkBuffer)sb, (VkBuffer)db, 1, &r);
}

void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) {
    post_op o = { .kind = POST_EVENT, .val = val, .mode = mode, .v = v };
    post_add(&o);
}
void mtl_sub_counter(uint64_t addr, uint32_t type) {
    post_op o = { .kind = POST_COUNTER, .addr = addr, .type = type };
    post_add(&o);
}

uint64_t mtl_sub_commit(void) {
    if (!vk_sub_ready() || g_cur_slot < 0) { g_cur_slot = -1; return g_fence; }
    pass_end();   // 收尾时通道必须已关（VkCommandBuffer 不允许带着未关的动态渲染通道结束）| the pass must be closed before finishing (a VkCommandBuffer may not end inside an open dynamic render pass)
    inflight_t *in = &g_inf[g_cur_slot];
    int i = g_cur_slot; g_cur_slot = -1;
    if (vkEndCommandBuffer(in->cb) != VK_SUCCESS) { LOG("【红】结束命令缓冲失败"); return g_fence; }
    VkResult r = vkResetFences(vk_dev(), 1, &in->fence);
    if (r != VK_SUCCESS) { LOG("【红】重置栅栏失败 VkResult %d", (int)r); return g_fence; }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &in->cb };
    r = vkQueueSubmit(vk_queue(), 1, &si, in->fence);
    if (r != VK_SUCCESS) { LOG("【红】提交失败 VkResult %d", (int)r); return g_fence; }
    in->pending = 1;
    in->value = ++g_fence;
    g_sub_total++;
    // 每 300 次提交报一次 op 分布：这就是"接下来该实现哪些 op"的实测依据。
    // 只报次数最多的 5 个：宿主的 LOG 有长度上限，一行塞 12 个 op 会被截断（实测截在 119 字符，
    // 而渲染 op 恰恰在截掉的那半截里；前 5 名已足够定实现次序）。
    // Report the op distribution every 300 submissions: this is the measured basis for which ops to implement
    // next. Only the top five are reported: the host LOG has a length limit and twelve ops on one line get cut
    // off (measured at 119 characters, with the render ops in the half that is cut; the top five are enough to
    // decide the implementation order).
    if (g_sub_total % 300 == 0) {
        uint64_t c[OP_COUNT_], total = 0;
        for (int k = 0; k < OP_COUNT_; k++) { c[k] = atomic_load(&g_opcount[k]); total += c[k]; }
        char line[400]; size_t o = 0;
        for (int top = 0; top < 5; top++) {
            int best = -1;
            for (int k = 0; k < OP_COUNT_; k++) if (c[k] && (best < 0 || c[k] > c[best])) best = k;
            if (best < 0) break;
            o += (size_t)snprintf(line + o, sizeof line - o, " %s=%.1f%%", g_opname[best],
                                  total ? 100.0 * (double)c[best] / (double)total : 0.0);
            c[best] = 0;
        }
        // 前缀压到最短、未执行另起一行：宿主的 LOG 有长度上限，前缀长了前 5 也会被截掉（实测过）
        // Shortest possible prefix and the unhandled count on its own line: the host LOG has a length limit,
        // and a long prefix truncates even five entries (measured).
        LOG("[vk] 提交 %llu，op 前 5：%s", (unsigned long long)g_sub_total, line);
        LOG("[vk] 本窗口 %llu 条；开通道 %llu 次、清除 %llu 次、影子绑定 %llu 条、绘制跳过 %llu 条；未执行累计 %llu",
            (unsigned long long)total, (unsigned long long)atomic_load(&g_pass_n), (unsigned long long)atomic_load(&g_clear_n),
            (unsigned long long)atomic_load(&g_shadow_n), (unsigned long long)atomic_load(&g_draw_skip),
            (unsigned long long)atomic_load(&g_op_unhandled));
        for (int k = 0; k < OP_COUNT_; k++) atomic_store(&g_opcount[k], 0);
    }
    return g_fence;
}

uint64_t mtl_last_fence(void) { return g_fence; }

int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (!vk_dev() || !fence || fence > g_fence) return 0;
    if (fence <= g_fence - MAX_INFLIGHT) { inflight_reap(); return 1; }   // 早已滑出在飞窗口 | long past the in-flight window
    for (int i = 0; i < MAX_INFLIGHT; i++)
        if (g_inf[i].pending && g_inf[i].value == fence) {
            VkResult r = vkWaitForFences(vk_dev(), 1, &g_inf[i].fence, VK_TRUE, timeout_ns ? timeout_ns : 1000000000ull);
            inflight_reap();
            return r == VK_SUCCESS;
        }
    inflight_reap();
    return 1;
}

// ========== 渲染状态影子 + 通道机械 ==========
// NVN 的命令流里绝大多数 op 只是"设置状态"（实测前五名占 70.5%，全是绑定），真正让 GPU 干活的是少数几条
// （SET_RT / CLEAR / DRAW）。所以回放层按「影子状态 + 需要时才开通道」做：绑定记在宿主侧，通道在第一次
// 清除或绘制时才开（loadOp 由这条流里的 CLEAR 决定），换目标 / 屏障 / 收流时关掉。
// ========== render state shadow + pass machinery ==========
// Most ops in NVN's stream merely set state (the measured top five are 70.5% of it and all of them bindings),
// while only a few make the GPU work (SET_RT / CLEAR / DRAW). So the replay layer goes "shadow state, open a pass
// only when needed": bindings are recorded on the host side and a pass opens at the first clear or draw with the
// loadOp decided by the CLEARs in this stream, closing on a target change, a barrier or the end of the stream.

#define SH_BIND 16

static void pass_end(void) {
    if (!g_rs.open || g_cur_slot < 0) { g_rs.open = 0; return; }
    if (dm_vkCmdEndRendering) dm_vkCmdEndRendering(g_inf[g_cur_slot].cb);
    // 关通道后把附件转回可采样：下一帧可能拿它当贴图读
    // Turn the attachments back into a sampleable layout after closing: the next frame may sample them
    for (int i = 0; i < g_rs.ncolor; i++) if (g_rs.color[i]) vk_tex_transition(g_rs.color[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, g_inf[g_cur_slot].cb);
    if (g_rs.depth) vk_tex_transition(g_rs.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, g_inf[g_cur_slot].cb);
    g_rs.open = 0;
}

// 开通道：把影子状态物化成 VkRenderingInfo（清除在 loadOp 上，与 NVN 的语义一致——清除发生在通道开头）
// Open a pass, materialising the shadow state into a VkRenderingInfo (clears ride on loadOp, matching NVN's
// semantics where a clear happens at the start of a pass)
static void pass_begin(void) {
    if (g_rs.open || g_cur_slot < 0) return;
    if (!vk_have_dyn_render()) return;
    if (!g_rs.ncolor && !g_rs.depth) return;
    if (g_rs.ncolor && !g_rs.color[0] && !g_rs.depth) return;
    VkCommandBuffer cb = g_inf[g_cur_slot].cb;
    VkRenderingAttachmentInfo att[8];
    int natt = 0;
    for (int i = 0; i < g_rs.ncolor && i < 8; i++) {
        const tex_t *t = g_rs.color[i];
        if (!t) continue;
        VkImageView iv = vk_tex_view(t);
        if (!iv) continue;
        vk_tex_transition(t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, cb);
        vk_tex_gpu_written(t);
        int clr = g_rs.cc_have && (g_rs.cc_mask & (1 << i));
        VkRenderingAttachmentInfo a = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = iv, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = clr ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = { .color = { { clr ? g_rs.cc[i][0] : 0.f, clr ? g_rs.cc[i][1] : 0.f,
                                        clr ? g_rs.cc[i][2] : 0.f, clr ? g_rs.cc[i][3] : 0.f } } } };
        att[natt++] = a;   // 布局由纹理层记着（vk_tex_transition），这里不再单独维护 | layouts are tracked by the texture layer (vk_tex_transition), so nothing extra is kept here
    }
    if (!natt && !g_rs.depth) return;
    VkRenderingAttachmentInfo datt = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    int has_d = 0;
    if (g_rs.depth) {
        VkImageView dv = vk_tex_view(g_rs.depth);
        if (dv) {
            vk_tex_transition(g_rs.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, cb);
            vk_tex_gpu_written(g_rs.depth);
            datt.imageView = dv; datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            datt.loadOp = g_rs.cd_have ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            datt.clearValue.depthStencil = (VkClearDepthStencilValue){ .depth = g_rs.cd, .stencil = (uint32_t)g_rs.cs };
            has_d = 1;
        }
    }
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { 0, 0 } },
        .layerCount = 1, .colorAttachmentCount = (uint32_t)natt, .pColorAttachments = natt ? att : NULL,
        .pDepthAttachment = has_d ? &datt : NULL, .pStencilAttachment = has_d ? &datt : NULL };
    // 绘制区域：优先用当前视口/裁剪的交集，都没有就按第一个附件的大小（NVN 里这两者通常都已设过）
    // Render area: prefer the intersection of the current viewport and scissor, otherwise the first attachment's
    // size (in NVN both are usually already set at this point)
    uint32_t w = 0, h = 0;
    if (g_rs.nvp) { w = (uint32_t)(g_rs.vp[0].x + g_rs.vp[0].width); h = (uint32_t)(g_rs.vp[0].y + g_rs.vp[0].height); }
    if (g_rs.nsc) { uint32_t sw = (uint32_t)(g_rs.sc[0].offset.x + (int)g_rs.sc[0].extent.width), sh = (uint32_t)(g_rs.sc[0].offset.y + (int)g_rs.sc[0].extent.height);
                    if (!w || sw < w) w = sw; if (!h || sh < h) h = sh; }
    if (!w || !h) { w = 1; h = 1; }
    ri.renderArea.extent = (VkExtent2D){ w, h };
    if (g_rs.nvp) vkCmdSetViewport(cb, 0, (uint32_t)g_rs.nvp, g_rs.vp);
    if (g_rs.nsc) vkCmdSetScissor(cb, 0, (uint32_t)g_rs.nsc, g_rs.sc);
    dm_vkCmdBeginRendering(cb, &ri);   // 桩库不导出，走 vkGetDeviceProcAddr 取的指针（见 vk_device.h）| not exported by the stub library, so it goes through the pointer fetched with vkGetDeviceProcAddr (see vk_device.h)
    g_rs.open = 1;
    atomic_fetch_add(&g_pass_n, 1);
}

// 命令流解释器：与 Metal 侧同形（OP_JUMP / OP_CALL 构成的小程序）。CPU 可见副作用按语义执行，
// 渲染类 op 目前只计数——每次提交报一次分布，按实测的频次决定实现次序。
// The command stream interpreter, shaped like the Metal one (a small program of OP_JUMP / OP_CALL). CPU-visible
// side effects are executed per their semantics, while render-side ops are only counted for now -- the
// distribution is reported per submission and the measured frequency decides the implementation order.
void mtl_exec(const uint8_t *p) {
    if (!p) return;
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.op < OP_COUNT_) atomic_fetch_add(&g_opcount[h.op], 1);
        const uint8_t *a = p + 8;
        switch (h.op) {
        case OP_END: pass_end(); return;
        case OP_BEGIN: break;
        case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
        case OP_CALL: { uint64_t t; memcpy(&t, a, 8); mtl_exec((const uint8_t *)(uintptr_t)t); break; }
        case OP_SIGNAL_EVENT: {
            volatile uint32_t *val; uint32_t m, v;
            memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4);
            mtl_sub_event(val, m, v);
            break; }
        case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
        case OP_RESET_COUNTER: break;   // 只影响计数器的取值，回放侧无对象可复位 | only affects the counter value; the replay side has nothing to reset
        case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); pass_end(); mtl_sub_copy(s_, d, n); break; }
        case OP_UPDATE_UB: {
            // 载荷 = {目标地址(8), 字节数(8), 数据(n)}。录制时数据已拷进命令内存，这里再拷进本槽的暂存缓冲，
            // 用 GPU 拷贝送到目标——保持"提交时序"（GPU 读这块缓冲的时候，写已经在它前面）。
            // Payload = {target address (8), byte count (8), data (n)}. The data was copied into command memory at
            // record time; here it goes into this slot's staging buffer and a GPU copy carries it to the target,
            // which keeps submission ordering (the write precedes whatever reads that buffer on the GPU).
            uint64_t dst, n; memcpy(&dst, a, 8); memcpy(&n, a + 8, 8);
            if (!n || g_cur_slot < 0) break;
            inflight_t *in = &g_inf[g_cur_slot];
            uint64_t padded = (n + 3) & ~3ull;
            if (in->stage_off + padded > STAGE_SIZE) { static int said; if (!said++) LOG("【红】暂存缓冲满，常量更新被丢（本次提交 %llu 字节）", (unsigned long long)in->stage_off); break; }
            uint8_t *sp = (uint8_t *)in->stage_map + in->stage_off;
            memcpy(sp, a + 16, n);
            void *db = NULL; uint64_t dof = 0;
            if (!mtl_addr(dst, n, &db, &dof) || (dof & 3)) {
                static int said2; if (!said2++) LOG("【红】常量更新落不到池子里或偏移未对齐：%#llx +%llx（偏移 %#llx）", (unsigned long long)dst, (unsigned long long)n, (unsigned long long)dof);
                break;
            }
            VkBufferCopy r = { .srcOffset = in->stage_off, .dstOffset = dof, .size = n };
            vkCmdCopyBuffer(in->cb, in->stage, (VkBuffer)db, 1, &r);
            in->stage_off += padded;
            break; }
        // ---- 渲染目标与清除：真正落到 GPU 的部分（清除 = 通道打开时的 loadOp）----
        // ---- render targets and clears: the part that really reaches the GPU (a clear is the loadOp when the pass opens) ----
        case OP_SET_RT: {
            const p_set_rt *r = (const p_set_rt *)a;
            pass_end();
            g_rs.ncolor = r->n < 0 ? 0 : (r->n > 8 ? 8 : r->n);
            for (int i = 0; i < 8; i++) g_rs.color[i] = i < g_rs.ncolor ? r->color[i] : NULL;
            g_rs.depth = r->depth;
            g_rs.cc_have = 0; g_rs.cc_mask = 0; g_rs.cd_have = 0; g_rs.cs_have = 0;
            break; }
        case OP_CLEAR_COLOR: {
            const p_clear_color *c = (const p_clear_color *)a;
            if (c->index < 0 || c->index > 7) break;
            memcpy(g_rs.cc[c->index], c->rgba, sizeof c->rgba);
            g_rs.cc_mask |= c->mask ? c->mask : (1 << c->index);
            g_rs.cc_have = 1;
            pass_begin();       // 清除自己也要开通道：纯清除通道在 NVN 里很常见 | a clear opens the pass itself: pure-clear passes are common in NVN
            atomic_fetch_add(&g_clear_n, 1);
            break; }
        case OP_CLEAR_DS: {
            const p_clear_ds *c = (const p_clear_ds *)a;
            g_rs.cd = c->depth; g_rs.cd_mask = (unsigned)c->depth_mask;
            g_rs.cs = c->stencil; g_rs.cs_mask = (unsigned)c->stencil_mask;
            g_rs.cd_have = c->depth_mask != 0; g_rs.cs_have = c->stencil_mask != 0;
            pass_begin();
            atomic_fetch_add(&g_clear_n, 1);
            break; }
        case OP_VIEWPORTS: {
            const p_viewports *v = (const p_viewports *)a;
            g_rs.nvp = v->n < 0 ? 0 : (v->n > 16 ? 16 : v->n);
            for (int i = 0; i < g_rs.nvp; i++) {
                g_rs.vp[i].x = v->v[i][0]; g_rs.vp[i].y = v->v[i][1];
                g_rs.vp[i].width = v->v[i][2]; g_rs.vp[i].height = v->v[i][3];
                g_rs.vp[i].minDepth = g_rs.drange_have ? g_rs.drange[0] : v->v[i][4];
                g_rs.vp[i].maxDepth = g_rs.drange_have ? g_rs.drange[1] : v->v[i][5];
            }
            if (g_rs.open) vkCmdSetViewport(g_inf[g_cur_slot].cb, 0, (uint32_t)g_rs.nvp, g_rs.vp);
            break; }
        case OP_SCISSOR: {
            const p_scissor *s = (const p_scissor *)a;
            g_rs.sc[0].offset = (VkOffset2D){ s->x, s->y };
            g_rs.sc[0].extent = (VkExtent2D){ s->w < 0 ? 0u : (uint32_t)s->w, s->h < 0 ? 0u : (uint32_t)s->h };
            g_rs.nsc = 1;
            if (g_rs.open) vkCmdSetScissor(g_inf[g_cur_slot].cb, 0, 1, g_rs.sc);
            break; }
        case OP_DEPTH_RANGE: {
            const p_depth_range *d = (const p_depth_range *)a;
            g_rs.drange[0] = d->n; g_rs.drange[1] = d->f; g_rs.drange_have = 1;
            for (int i = 0; i < g_rs.nvp; i++) { g_rs.vp[i].minDepth = d->n; g_rs.vp[i].maxDepth = d->f; }
            if (g_rs.open && g_rs.nvp) vkCmdSetViewport(g_inf[g_cur_slot].cb, 0, (uint32_t)g_rs.nvp, g_rs.vp);
            break; }
        case OP_BLEND_COLOR: { const p_blend_color *b = (const p_blend_color *)a; memcpy(&g_rs.blend_rgba, b->rgba, 8); break; }

        // ---- 影子绑定：状态对象原样存（绘制时再解释），其余只记存在 ----
        // ---- shadow bindings: state objects stored verbatim (interpreted at draw time), the rest only noted ----
        case OP_BIND_PROGRAM: { const p_bind_program *b = (const p_bind_program *)a; g_rs.prog = b->prog; atomic_fetch_add(&g_shadow_n, 1); break; }
        case OP_BIND_STATE: {
            const p_bind_state *b = (const p_bind_state *)a;
            int k = b->kind;
            if (k >= 0 && k < 6) { memcpy(g_rs.st_bytes[k], b->bytes, sizeof g_rs.st_bytes[k]); g_rs.st_have[k] = 1; }
            atomic_fetch_add(&g_shadow_n, 1);
            break; }
        case OP_STENCIL: g_rs.stencil_have = 1; atomic_fetch_add(&g_shadow_n, 1); break;
        case OP_POLY_OFFSET: g_rs.poly_have = 1; atomic_fetch_add(&g_shadow_n, 1); break;
        case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG:
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL:
            atomic_fetch_add(&g_shadow_n, 1);
            break;

        // ---- 绘制：需要管线，留给下一步（M2-6d）。只计数，并把首批的现场打出来，好知道要建什么 ----
        // ---- draws: they need pipelines and are left to the next step (M2-6d). Only counted, with the first few
        //      logged so we know what has to be built ----
        case OP_DRAW: case OP_DRAW_IDX: case OP_DISPATCH: {
            uint64_t k = atomic_fetch_add(&g_draw_skip, 1);
            if (k < 6) {
                const p_draw *d = (const p_draw *)a;
                LOG("【黄】绘制先跳过（还没有管线）：%s first=%d count=%d 程序=%p 目标 %d 个 视口 %d 个",
                    g_opname[h.op], d->first, d->count, (const void *)g_rs.prog, g_rs.ncolor, g_rs.nvp);
            }
            break; }

        // ---- 屏障与拷贝：先把通道关掉再做（拷贝不进渲染通道）----
        // ---- barriers and copies: close the pass first (copies do not run inside a render pass) ----
        case OP_BARRIER: pass_end(); break;
        case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: case OP_COPY_STRIDE: case OP_CLEAR_TEX:
            pass_end(); atomic_fetch_add(&g_shadow_n, 1); break;

        default: atomic_fetch_add(&g_op_unhandled, 1); break;   // 其余渲染类：先只统计 | the other render-side ops: counted only for now
        }
        p += h.size;
    }
}

void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }        // Metal 用来预测预编译；Vulkan 侧不需要 | Metal uses it to predict precompilation; Vulkan does not need it
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }
