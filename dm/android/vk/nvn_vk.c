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
#include "vk_pipe.h"
#include "vk_region.h"
#include "vk_shader.h"
#include "vk_tex.h"
#include "vk_png.h"
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
// 等队列空闲也要外部同步（见 vk_device.h）：这个调用随时可能从前端线程发出来。
// Everything on the queue has completed: on Android this is simply waiting for the queue to go idle. That wait needs
// the external synchronisation too (see vk_device.h), since it can come from the front-end thread at any moment.
void mtl_drain(void) { if (vk_dev()) { vk_queue_note("客体：等队列空闲（drain）"); vk_queue_lock(); vkQueueWaitIdle(vk_queue()); vk_queue_unlock(); } }
void mtl_finish(void) { if (vk_dev()) { vk_queue_note("客体：等队列空闲（finish）"); vk_queue_lock(); vkQueueWaitIdle(vk_queue()); vk_queue_unlock(); } }

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
    // 渲染目标各自的 view（SET_RT 载荷里带着）。不是可有可无：限定了 mip/层的 view 才是游戏真正要画的那一层，
    // 拿底视图去当附件会画到第 0 级上去——真机上"新建子视图 0 张"就是这个缺口的表现。
    // Each render target's own view (carried in the SET_RT payload). Not optional: a view that limits the mip range
    // or the layers is the one the game means to draw into, and using the base view as the attachment would draw into
    // level 0 instead -- the device's "0 sub-views created" is exactly this gap.
    view_t cview[8]; uint8_t has_cview[8]; view_t dview; uint8_t has_dview;
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
    // 混合状态**按目标分存**：nvnCommandBufferBindBlendState 只传状态对象、目标号在对象里面（blend_st.target），
    // 所以不能像别的状态那样只留最后一份 | the blend state is stored **per target**: BindBlendState passes only the
    // state object and the target number lives inside it (blend_st.target), so unlike the other states a single last
    // copy will not do
    blend_st blend[8]; uint8_t blend_have;
    // ---- 顶点输入与资源绑定（M2-6d-3）：绑定类 op 占命令流七成（实测前五名合计 70.5%），
    //      所以这里一律"只存不解释"，把解释推迟到绘制时（与苹果侧的影子状态同一套做法）。
    // ---- vertex input and resource bindings (M2-6d-3): bind ops are 70% of the stream (the measured top five are
    //      70.5% together), so everything here is stored rather than interpreted, deferring that to draw time (the
    //      same shape as Apple's shadow state).
    va_st va[16]; uint16_t va_have;               // 位图：哪几号属性被绑过 | bitmap: which attribute numbers were bound
    vs_st stream[16]; uint16_t stream_have;
    NVNbufferRange vbuf[16]; uint16_t vbuf_have;
    struct { uint64_t addr, size; } ubo[6][16], ssbo[6][128];
    uint16_t ubo_have[6];                         // 16 位位图 | a 16-bit bitmap
    uint64_t ssbo_have[6][2];                     // 128 位用两个 64 位字 | 128 bits take two 64-bit words
    uint64_t tex[6][128], samp[6][128], img[6][32];
    uint64_t tex_have[6][2], samp_have[6][2];     // 128 位用两个 64 位字 | 128 bits take two 64-bit words
    uint32_t img_have[6];
    dpool_t texpool, samppool; int have_texpool, have_samppool;
    // 模板的值掩码 / 写掩码 / 参考值（按面，默认 0xff，与苹果侧一致；哪一项由 p_stencil.which 决定）
    // Stencil value mask / write mask / reference (per face, defaulting to 0xff as on Apple; p_stencil.which picks one)
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    float po_factor, po_units, po_clamp;          // 多边形偏移的三个值 | the three polygon offset values
} rstate_t;
// 模板掩码的默认值：NVN / GL 的默认是全通，苹果侧同样按 0xff 起步（nvn_mtl.m 的状态复位）
// Stencil mask defaults: NVN / GL default to all-ones, and the Apple side starts at 0xff too (its state reset)
static rstate_t g_rs = { .st_vmask = { 0xff, 0xff }, .st_wmask = { 0xff, 0xff } };

// NVN 的默认状态：逐项与苹果侧的 state_reset（nvn_mtl.m「绘制状态」一节，能跑的那份实现）对齐，
// 在第一次 mtl_exec 时生效——苹果侧也正是这么做的，见其 mtl_exec 开头那句 state_reset()。
// 为什么必须显式给：gfx_state_fill 是**无条件**读这份影子的，零初始化于是把"游戏没设过的状态"变成
// "写掩码 0（一个通道都不写）"与"深度范围 [0,0]"，接下来绘制就会静默地一个像素都不落——真机上
// "34 万次绘制、目标读出全零、零报错"正是这个样子。NVN 对这些状态有默认值，游戏有权不设。
// NVN's default state, item for item with the Apple side's state_reset (the "draw state" section of the working
// implementation in nvn_mtl.m), applied at the first mtl_exec -- which is exactly where Apple does it too (the
// state_reset() at the head of its mtl_exec). Why it must be explicit: gfx_state_fill reads this shadow
// **unconditionally**, so zero-initialisation turns "a state the game never set" into "write mask 0 (not one
// channel written)" and "depth range [0,0]", after which the draws land nothing at all, silently -- which is what
// "348k draws, all-zero targets, zero errors" looks like on the device. NVN gives these states defaults and the
// game may rely on them.
static void state_defaults(void) {
    // 混合：默认关（blend_have 不置位就是关，与苹果侧 color.blend_en = 0 同一回事），系数按 NVN 默认给全
    // Blend: off by default (leaving blend_have clear means off, the same as Apple's color.blend_en = 0), with the
    // factors filled in per NVN's defaults
    blend_st bl = { 0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO,
                    NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD };
    for (uint64_t i = 0; i < 8; i++) { bl.target = i; g_rs.blend[i] = bl; }
    color_st co = { 0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS };
    memcpy(g_rs.st_bytes[1], &co, sizeof co);
    // 写掩码：NVN 默认全开。这一条是"一个像素都不落"的头号成因——零初始化的掩码 0 会让每一次绘制都写不出东西
    // Write mask: all channels on by NVN's default. This one is the chief cause of "not a pixel lands": a
    // zero-initialised mask of 0 makes every draw write nothing
    chmask_st cm = { 0xffffffffu };
    memcpy(g_rs.st_bytes[2], &cm, sizeof cm);
    poly_st po = { NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0 };
    memcpy(g_rs.st_bytes[4], &po, sizeof po);
    ds_st ds = { 0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                 NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP,
                 NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP };
    memcpy(g_rs.st_bytes[5], &ds, sizeof ds);
    // 深度范围：NVN 的 SetViewports 只带 x/y/宽/高（四个 float），近远另有 SetDepthRange，默认 [0,1]。
    // 这里必须置成"已有值"：否则 OP_VIEWPORTS 会去读载荷里那两个从来没被填过的字（读到的是 [0,0]）。
    // Depth range: NVN's SetViewports carries only x/y/width/height (four floats) while near/far come from
    // SetDepthRange and default to [0,1]. This has to be marked as set here: otherwise OP_VIEWPORTS reads the two
    // words in its payload that are never filled in (and gets [0,0]).
    g_rs.drange[0] = 0.0f; g_rs.drange[1] = 1.0f; g_rs.drange_have = 1;
}
static _Atomic uint64_t g_pass_n, g_clear_n, g_draw_skip, g_shadow_n;

// 绘制按"有没有渲染目标"分开计数 + 最近 32 个 op 的流水 + 最近一次 SET_RT。
// 为什么：真机上近半数绘制写着"目标 0 个"（于是被跳过），而游戏那些目标因此一直是零 —— 究竟是"游戏真的没设目标"还是
// "我们把它丢了"，光看计数分不出来，只能把**这次绘制前面的那条 op 序列**拿出来看。
// Draws counted by "has render targets or not", plus a ring of the last 32 ops and the last SET_RT. Why: on the device
// nearly half the draws say "0 targets" and are skipped, which is why the game's targets stay zero -- whether the game
// really set no targets or we dropped them cannot be told from counts alone, only from **the op sequence in front of
// that draw**.
static _Atomic uint64_t g_draw_nc0, g_draw_ncN;
static uint8_t g_opring[32];
static _Atomic unsigned g_opring_i;
static uint64_t g_last_rt[8];
static int g_last_rt_n;   // 最近一次 SET_RT 报了几个目标（-1 = 从没报过）| how many targets the last SET_RT carried (-1 = never)
static int g_last_rt_seen;

// 渲染目标使用计数：哪张纹理被当渲染目标用过、用了多少次。回答的是"游戏到底往哪张纹理画"——而这件事只能靠数
// （真机上出图纹理全白、游戏自己的目标全零，光看名字猜不出来）。表很小：超过就并到"其他"。
// Render-target usage counts: which textures serve as render targets, and how often. It answers "which texture does the
// game draw into", which only counting can settle (on the device the present texture is all white while the game's own
// targets are all zero, and the names alone will not tell you). The table is small: anything past it goes to "other".
static struct { uint64_t ser, n; } g_rt_use[48];
static int g_rt_n;
static _Atomic uint64_t g_rt_other;

static void rt_use_bump(uint64_t ser) {
    if (!ser) return;
    for (int i = 0; i < g_rt_n; i++) if (g_rt_use[i].ser == ser) { g_rt_use[i].n++; return; }
    if (g_rt_n < 48) { g_rt_use[g_rt_n].ser = ser; g_rt_use[g_rt_n].n = 1; g_rt_n++; }
    else atomic_fetch_add(&g_rt_other, 1);
}
static void pass_begin(void);
static void pass_end(void);
static void desc_slot_reset(int slot);   // 定义在描述符那一节（在途槽回收时重置它的池）| defined in the descriptor section (resets a slot's pool when it is reaped)

// ========== GPU 计时（M2-6d 的收尾指标）==========
// ========== GPU timing (M2-6d's closing metric) ==========
// 安卓侧的 mtl_gpu_ms 原本是空桩（直接 return 0），所以日志里的 "GPU 0.00 ms" **从来不是测量结果**。
// 这里把它变成真测量：每次通道开始 / 结束各写一个时间戳（vkCmdWriteTimestamp），栅栏过了之后读回，
// 取"最后一次结束 − 第一次开始"作为这次提交的 GPU 跨度，累进按帧的环——与苹果侧同一套语义
// （那边的 GPUStartTime/GPUEndTime 也是按帧号回查、未全部完成时返回 -1）。
// On Android mtl_gpu_ms used to be a stub returning 0, so the "GPU 0.00 ms" in the log was **never a measurement**.
// This makes it real: a timestamp at the start and the end of every pass (vkCmdWriteTimestamp), read back once the
// fence has signalled, with "last end minus first start" as the submission's GPU span accumulated into a per-frame
// ring -- the same semantics as Apple's (whose GPUStartTime/GPUEndTime are also looked up by frame number and
// return -1 while incomplete).
// 这两个计数在"提交"那一段的统计行里要用，而它们属于描述符那一节（定义在后），所以在这里先定下来。
// These two counters are used by the periodic stats line in the submission section while they belong to the
// descriptor section (defined further down), so they are declared here first.
static _Atomic uint64_t g_desc_draws, g_desc_tex_ok, g_desc_tex_blank, g_desc_tex_nosamp, g_desc_tex_emptysamp;

#define GPU_RING 1024        // 与苹果侧同一个环大小 | the same ring size as Apple
#define GPU_PASS_MAX 64      // 一次提交最多记这么多通道；超了就不再写时间戳（那一次的量会偏小） | at most this many passes per submission; beyond it no more timestamps are written (that submission reads low)
typedef struct { _Atomic uint64_t frame, ns, pending, t0, t1; } gpu_rec;
static gpu_rec g_gpu[GPU_RING];
static _Atomic uint64_t g_cur_frame;
static VkQueryPool g_tsq;
static int g_ts_told;
static uint8_t g_slot_npass[MAX_INFLIGHT];
static uint64_t g_slot_frame[MAX_INFLIGHT];

void mtl_set_frame(uint64_t f) { atomic_store(&g_cur_frame, f); }

static int gpu_pool(void) {
    if (g_tsq) return 1;
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return 0;
    if (!vk_ts_bits()) {
        static int said;
        if (!said++) LOG("【黄】本队列族不支持时间戳查询（有效位数 0）：GPU 时间这一项测不了，仍报 0");
        return 0;
    }
    VkQueryPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = MAX_INFLIGHT * GPU_PASS_MAX * 2 };
    if (vkCreateQueryPool(vk_dev(), &ci, NULL, &g_tsq) != VK_SUCCESS) { LOG("【红】建时间戳查询池失败"); return 0; }
    LOG("【绿】GPU 计时就绪：时间戳 %d 位有效、每刻 %.2f ns、%d 槽 × %d 通道",
        vk_ts_bits(), vk_ts_period_ns(), MAX_INFLIGHT, GPU_PASS_MAX);
    return 1;
}

// 一个槽开始录制：清掉这一槽的通道计数，并在命令缓冲里复位它的查询区间（复位是命令，GPU 执行时才生效）
// A slot starts recording: clear this slot's pass count and reset its query range in the command buffer (the reset
// is a command and only takes effect when the GPU executes it)
static void gpu_slot_reset(int slot, VkCommandBuffer cb) {
    g_slot_npass[slot] = 0;
    if (slot < 0 || !gpu_pool()) return;
    vkCmdResetQueryPool(cb, g_tsq, (uint32_t)(slot * GPU_PASS_MAX * 2), (uint32_t)(GPU_PASS_MAX * 2));
}

// 通道起止各写一个时间戳。只有**真的开成**通道时才写（见调用点），所以起止必然配对。
// One timestamp at each end of a pass. Written only when the pass **really opened** (see the call sites), so the
// pairs always match.
static void gpu_pass_ts(int slot, VkCommandBuffer cb, int end) {
    if (!g_tsq || slot < 0 || slot >= MAX_INFLIGHT || g_slot_npass[slot] >= GPU_PASS_MAX) return;
    uint32_t q = (uint32_t)(slot * GPU_PASS_MAX * 2 + g_slot_npass[slot] * 2 + (end ? 1 : 0));
    vkCmdWriteTimestamp(cb, end ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_tsq, q);
    if (end) g_slot_npass[slot]++;
}

static void gpu_tag_begin(uint64_t f) {
    gpu_rec *g = &g_gpu[f % GPU_RING];
    if (atomic_load(&g->frame) != f) {
        atomic_store(&g->frame, f); atomic_store(&g->ns, 0); atomic_store(&g->pending, 0);
        atomic_store(&g->t0, UINT64_MAX); atomic_store(&g->t1, 0);
    }
    atomic_fetch_add(&g->pending, 1);
}

static void gpu_tag_done(uint64_t f, uint64_t t0, uint64_t t1) {
    gpu_rec *g = &g_gpu[f % GPU_RING];
    if (atomic_load(&g->frame) != f) return;
    if (t1 > t0) {
        atomic_fetch_add(&g->ns, t1 - t0);
        uint64_t v;
        for (v = atomic_load(&g->t0); t0 < v && !atomic_compare_exchange_weak(&g->t0, &v, t0);) {}
        for (v = atomic_load(&g->t1); t1 > v && !atomic_compare_exchange_weak(&g->t1, &v, t1);) {}
    }
    atomic_fetch_sub(&g->pending, 1);
}

// 提交时记下这次提交属于哪一帧：完成是滞后的，回查只能靠帧号（与苹果侧一样）。
// Record which frame a submission belongs to at commit time: completion is late, so the lookup can only go by
// frame number (the same as on Apple).
static void gpu_note_frame(int slot, uint64_t f) {
    g_slot_frame[slot] = f;
    if (f) gpu_tag_begin(f);
}

// 栅栏过了：把这一槽的时间戳读回来，换算成纳秒累进那一帧
// The fence has signalled: read this slot's timestamps back and add them to that frame in nanoseconds
static void gpu_slot_readback(int slot) {
    uint64_t f = g_slot_frame[slot];
    int n = g_slot_npass[slot];
    g_slot_npass[slot] = 0;
    g_slot_frame[slot] = 0;
    if (!f) return;
    uint64_t t0 = 0, t1 = 0;
    if (g_tsq && n > 0) {
        static uint64_t res[GPU_PASS_MAX * 2];
        VkResult r = vkGetQueryPoolResults(vk_dev(), g_tsq, (uint32_t)(slot * GPU_PASS_MAX * 2), (uint32_t)(n * 2),
                                           sizeof res, res, sizeof(uint64_t),
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (r == VK_SUCCESS) {
            for (int i = 0; i < n; i++) {
                uint64_t a = res[i * 2], b = res[i * 2 + 1];
                if (!a || !b) continue;
                if (!t0 || a < t0) t0 = a;
                if (b > t1) t1 = b;
            }
            double per = vk_ts_period_ns();
            t0 = (uint64_t)((double)t0 * per);
            t1 = (uint64_t)((double)t1 * per);
        } else if (g_ts_told++ < 3) {
            LOG("【黄】读时间戳失败 VkResult %d（这一槽的 GPU 时间按 0 记）", (int)r);
        }
    }
    gpu_tag_done(f, t0, t1);
}

double mtl_gpu_ms(uint64_t f) {
    gpu_rec *g = &g_gpu[f % GPU_RING];
    if (atomic_load(&g->frame) != f || atomic_load(&g->pending)) return -1;
    return (double)atomic_load(&g->ns) / 1e6;
}

int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) {
    gpu_rec *g = &g_gpu[f % GPU_RING];
    if (atomic_load(&g->frame) != f || atomic_load(&g->pending)) return 0;
    uint64_t a = atomic_load(&g->t0), b = atomic_load(&g->t1);
    if (t0) *t0 = a;
    if (t1) *t1 = b;
    return b >= a && b ? 1 : 0;
}

// ========== 出图与截图（M2 验收的仪器）==========
// ========== Present and screenshots (M2's acceptance instrument) ==========
// 还没有窗口与交换链（那是 M3），但客体出图时**仍然会调** mtl_present——nvnQueuePresentTexture 会把交换链纹理交出来，
// 所以这里是"游戏真正要显示哪一张画面"的**权威来源**，不用去猜哪张贴图是最终输出。这也是这一步能落地的原因：
// 与其从几百张渲染目标里挑一张，不如用游戏自己交出来的那张。
// There is no window or swapchain yet (that is M3), but the guest **still calls** mtl_present when it presents --
// nvnQueuePresentTexture hands over the swapchain texture -- so this is the **authoritative** answer to "which
// picture does the game mean to show", with no need to guess which texture is the final output. That is also why this
// step can land at all: rather than picking one out of hundreds of render targets, take the one the game itself
// hands over.

static _Atomic uint64_t g_present_n;
static struct { uint64_t frame; char path[512]; } g_shotq[8];
static int g_nshotq;
static pthread_mutex_t g_shotq_mu = PTHREAD_MUTEX_INITIALIZER;

// diag.c 提供强定义（DM_TICK_HOLD=E:SHOT 靠它解冻）；图形单测不链宿主时用这个空定义
// diag.c provides the strong definition (DM_TICK_HOLD=E:SHOT unfreezes on it); this empty one serves graphics unit
// tests that do not link the host
__attribute__((weak)) void dm_shot_written(const char *path, int ok) { (void)path; (void)ok; }

// 截图的核心，按**序号**：抓像素、算亮度统计、写 PNG、报一行。出图那一支与"同尺寸全部截取"共用它。
// The core of a screenshot, by **serial**: grab pixels, compute luminance statistics, write the PNG, report one line.
// The present path and "grab every same-size texture" both go through it.
// `save_blank`：整片只有一种值时要不要照样落盘（出图那一支要——它得证明"这里确实是空白"；扫描不要——扫描靠
// 统计行说话，落一张 5.7 MB 的纯色图没有意义）。
// `save_blank`: whether to write the file even when the whole picture is a single value (the present path wants it --
// it has to prove "this really is blank"; a scan does not -- the statistics line says it, and writing a 5.7 MB
// single-colour file proves nothing).
static int shot_grab_ser(uint64_t serial, uint32_t w, uint32_t h, const char *path, int save_blank) {
    if (!w || !h) { LOG("【红】截图 %s：尺寸 %ux%u", path, w, h); return 0; }
    uint8_t *buf = malloc((size_t)w * h * 4);
    if (!buf) { LOG("【红】截图 %s：%ux%u 的像素缓冲分配不出", path, w, h); return 0; }
    uint32_t gw = 0, gh = 0;
    int ok = vk_tex_grab_ser(serial, 0, buf, (size_t)w * 4, &gw, &gh);
    if (ok) {
        // 亮度统计：白 / 黑 / 有内容从此是**日志里的数**，不必把图拉下来才知道（上一轮的"全白"正是这样看出来的）
        // Luminance statistics: white / black / has content become **numbers in the log** instead of something you
        // learn only after pulling the file down (last round's "all white" was read exactly that way)
        uint64_t sum = 0;
        uint8_t lo = 255, hi = 0;
        size_t np = (size_t)gw * gh;
        for (size_t i = 0; i < np; i++) {
            uint8_t l = (uint8_t)(((unsigned)buf[i * 4] + buf[i * 4 + 1] + buf[i * 4 + 2]) / 3);
            sum += l;
            if (l < lo) lo = l;
            if (l > hi) hi = l;
        }
        double mean = np ? (double)sum / (double)np : 0.0;
        if (!path || (lo == hi && !save_blank)) {
            // 整片一个值就是"没有画面"的**数**：扫描时只报不写
            // A single value across the whole picture is "no picture" as a **number**: a scan reports it without writing
            LOG("[vk] 扫描 序号 %llu %ux%u：亮度 均值 %.1f、最小 %u、最大 %u%s", (unsigned long long)serial, gw, gh, mean, lo, hi,
                path ? "（整片一个值，不落盘）" : "（只统计）");
        } else {
            ok = vk_png_write(path, buf, gw, gh);
            LOG("截图序号 %llu %ux%u → %s %s；亮度 均值 %.1f、最小 %u、最大 %u", (unsigned long long)serial, gw, gh, path,
                ok ? "成功" : "失败", mean, lo, hi);
        }
    } else {
        LOG("截图序号 %llu %ux%u → %s 失败", (unsigned long long)serial, w, h, path);
        // 读不回来就把"这个尺寸还有哪些已知贴图"列出来（只列一次：逐张截取时后面每张都失败，会刷屏）
        // If the readback fails, list which known textures share this size (once only: when grabbing every texture of
        // a size, the later ones all fail and would flood the log)
        static int dumped;
        if (!dumped++) vk_tex_dump_size(w, h);
    }
    free(buf);
    if (path) dm_shot_written(path, ok);
    return ok;
}

// 出图那一支（tex_t 入口）：等到 GPU 做完，把这张纹理从驱动侧读回，写成 PNG。
// The present path (the tex_t entry): wait for the GPU to finish, read this texture back from the driver side and write
// it as a PNG.
static int shot_take(const tex_t *t, const char *path) {
    if (!t) { LOG("【红】截图 %s：出图纹理为空", path); return 0; }
    // 出图那张贴图**按需建**——与苹果侧一致（那边是 `id<MTLTexture> src = tex_mtl(t)`，建不出就报红"出图贴图建不出
    // MTLTexture"）。这一条我先前搞错了：要求"入口必须已存在"等于把"此前没人问过它"误当成"游戏没画过它"。
    // The present texture is built **on demand**, the same as Apple (there it is `id<MTLTexture> src = tex_mtl(t)`,
    // which reports "cannot build an MTLTexture for the present texture" on failure). I had this wrong before:
    // requiring a pre-existing entry mistakes "nobody asked for it yet" for "the game never drew into it".
    if (!vk_tex_view(t, NULL)) {
        LOG("【红】截图 %s：出图贴图（序号 %llu）建不出图像", path, (unsigned long long)t->serial);
        return 0;
    }
    return shot_grab_ser(t->serial, t->d.width, t->d.height, path, 1);
}

// 扫描：把**所有**已知贴图各读一次、报一行统计，只有"不是整片一个值"的才落盘。与"同尺寸"那条相比，它不预设画面
// 在哪个尺寸上——真机实测 1600×900 那 11 张全是空的（0 或 255），画面可能根本不在这个尺寸。
// A scan: read **every** known texture once and report a statistics line for each, writing a file only for those that
// are not a single value throughout. Unlike the same-size path it does not presuppose which size the picture lives at
// -- on the device all eleven 1600x900 textures are empty (0 or 255), and the picture may not be at that size at all.
static void shot_scan(uint64_t frame, const char *prefix) {
    enum { SCAN_MAX = 192 };
    static uint64_t ser[SCAN_MAX];
    static uint32_t ws[SCAN_MAX], hs[SCAN_MAX];
    int n = vk_tex_list(ser, ws, hs, SCAN_MAX);
    // 只扫"够大"的：画面不可能是一张 16×24 的精灵图，而每读一张都要分配一次缓冲并等一次队列（实测约 0.45 秒），
    // 192 张会把整轮跑满。这个门槛是**语义**上的，不是性能上的取巧。
    // Only "big enough" ones: a picture cannot be a 16x24 sprite, and every read allocates a buffer and waits for the
    // queue (measured at about 0.45 s), so 192 of them would eat the whole run. The threshold is a **semantic** one, not
    // a performance trick.
    int big = 0;
    for (int i = 0; i < n; i++) if (ws[i] >= 256 && hs[i] >= 256) big++;
    LOG("[vk] 扫描（出图第 %llu 次）：已知贴图 %d 张，其中 ≥256×256 的 %d 张，逐张读一次，只有有内容的落盘到 %s",
        (unsigned long long)frame, n, big, prefix ? prefix : "（不落盘）");
    for (int i = 0; i < n; i++) {
        if (ws[i] < 256 || hs[i] < 256) continue;
        char path[512];
        // 文件名带帧号：多帧扫描时同一张贴图会被看好几次，不带帧号就互相覆盖了
        // The name carries the frame number: a multi-frame scan looks at the same texture several times, and without it
        // they would overwrite one another
        if (prefix) snprintf(path, sizeof path, "%s%llu-%llu.png", prefix, (unsigned long long)frame, (unsigned long long)ser[i]);
        shot_grab_ser(ser[i], ws[i], hs[i], prefix ? path : NULL, 0);
    }
}

// 同尺寸的全部已知贴图逐张截出来（DM_NVN_SHOT_ALL）：这是找"游戏把画面画进了哪一张"的仪器——
// 出图纹理可能根本不是画面所在（真机上它就是全白），那时唯一的办法是把同尺寸的每张都看一眼。
// Grab every known texture of the same size (DM_NVN_SHOT_ALL): the instrument for finding "which texture did the game
// draw the picture into" -- the present texture may not hold the picture at all (on the device it is all white), and
// then the only way is to look at each same-size texture.
static void shot_all_same_size(const tex_t *t, const char *prefix) {
    if (!t) return;
    uint32_t w = t->d.width, h = t->d.height;
    uint64_t ser[24];
    int n = vk_tex_same_size_serials(w, h, ser, 24);
    LOG("[vk] 同尺寸（%ux%u）共 %d 张：逐张截到 %s<序号>.png", w, h, n, prefix);
    for (int i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s%llu.png", prefix, (unsigned long long)ser[i]);
        shot_grab_ser(ser[i], w, h, path, 1);
    }
}

// 出图。安卓这边没有出图队列（M3 才有真的交换链），所以只做三件事：数帧、按请求截图、把票号报 0。
// Presenting. There is no present queue on Android (a real swapchain arrives in M3), so this does three things: count
// frames, take requested screenshots, and report a ticket of 0.
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s; (void)crop;
    uint64_t n = atomic_fetch_add(&g_present_n, 1) + 1;

    // DM_GFX_PROBE=<序号>：每次出图后把这张贴图清成一个已知颜色（蓝）—— 这是"已知好的写入 → 读回 → 上屏"的对照实验。
    // 放在出图之后、截图之前，所以同一帧里截的图/查看器抓的就是它：屏幕变蓝 = 抓取与上屏那条链路是通的，问题在游戏
    // 自己的绘制上；还是黑的 = 我们的写入/布局/读回本身有问题（那么"目标全零"那条结论也要重审）。
    // DM_GFX_PROBE=<serial>: after each present, clear that texture to a known colour (blue) -- the control experiment
    // for "a known-good write -> read back -> present". It runs after the present but before any screenshot, so a
    // screenshot or viewer grab in the same frame captures it: a blue screen means the grab-and-present chain works and
    // the trouble is in the game's own drawing, while a black screen means our write, layout or readback is at fault
    // (and "the targets are all zero" has to be revisited).
    static long probe_ser = -2;
    if (probe_ser == -2) {
        const char *e = getenv("DM_GFX_PROBE");
        probe_ser = (e && *e) ? atol(e) : -1;
        if (probe_ser > 0) LOG("[vk] 对照实验已开启：出图后把序号 %ld 清成蓝色（DM_GFX_PROBE 去掉即关）", probe_ser);
    }
    if (probe_ser > 0) vk_tex_probe_clear((uint64_t)probe_ser, 0.0f, 0.6f, 1.0f, 1.0f);

    // DM_NVN_SHOT=帧号[,帧号…]:路径（路径里的 %d 换成帧号）——与苹果侧同名同义，脚本与经验可以直接搬过来
    // DM_NVN_SHOT=<frame>[,<frame>...]:<path> (%d in the path becomes the frame number) -- the same name and meaning
    // as on Apple, so scripts and know-how carry over
    static long frames[16]; static int nf = -1; static const char *shot_path;
    if (nf < 0) {
        nf = 0;
        const char *e = getenv("DM_NVN_SHOT"), *c = e ? strchr(e, ':') : NULL;
        if (c) {
            shot_path = c + 1;
            for (const char *q = e; q < c && nf < 16; q = strchr(q, ',') ? strchr(q, ',') + 1 : c) frames[nf++] = atol(q);
            LOG("[vk] 截图已排程：出图帧 %s 共 %d 个 → %s", e, nf, shot_path);
        }
    }
    for (int i = 0; i < nf; i++)
        if ((long)n == frames[i]) { char path[512]; snprintf(path, sizeof path, shot_path, (int)n); shot_take(t, path); }

    // DM_NVN_SHOT_ALL=帧号:路径前缀 —— 该帧把"与出图纹理同尺寸"的每张已知贴图各截一张（找画面在哪一张）
    // DM_NVN_SHOT_ALL=<frame>:<path prefix> -- at that frame, grab every known texture of the same size as the present
    // texture (to find which one holds the picture)
    static long all_at = -1; static const char *all_prefix;
    if (all_at < 0) {
        const char *e = getenv("DM_NVN_SHOT_ALL"), *c = e ? strchr(e, ':') : NULL;
        all_at = (c && c > e) ? atol(e) : 0;
        if (c) { all_prefix = c + 1; LOG("[vk] 同尺寸全部截取已排程：出图帧 %ld → %s", all_at, all_prefix); }
    }
    if (all_at > 0 && (long)n == all_at) shot_all_same_size(t, all_prefix);

    // DM_NVN_SHOT_SCAN=帧号[:路径前缀] —— 该帧扫一遍所有已知贴图（报统计，只有有内容的落盘）。不预设尺寸。
    // DM_NVN_SHOT_SCAN=<frame>[:<path prefix>] -- at that frame, scan every known texture (report statistics, write
    // only the ones with content). No size presupposed.
    static long sc[8]; static int nsc = -1; static const char *sc_prefix;
    if (nsc < 0) {
        nsc = 0;
        const char *e = getenv("DM_NVN_SHOT_SCAN");
        if (e) {
            const char *c = strchr(e, ':');
            const char *end = c ? c : e + strlen(e);
            if (c) sc_prefix = c + 1;
            for (const char *q = e; q < end && nsc < 8; q = strchr(q, ',') ? strchr(q, ',') + 1 : end) sc[nsc++] = atol(q);
            if (nsc) LOG("[vk] 扫描已排程：出图帧 %s 共 %d 个 → %s", e, nsc, sc_prefix ? sc_prefix : "（不落盘）");
        }
    }
    for (int i = 0; i < nsc; i++) if ((long)n == sc[i]) shot_scan(n, sc_prefix);

    pthread_mutex_lock(&g_shotq_mu);
    for (int i = 0; i < g_nshotq;) if (n >= g_shotq[i].frame) { shot_take(t, g_shotq[i].path); g_shotq[i] = g_shotq[--g_nshotq]; } else i++;
    pthread_mutex_unlock(&g_shotq_mu);

    // 前三次与每 300 次报一行：这也是日志里的"出图帧号"，截图时间点按它对齐
    // The first three and then every 300th: this is the present frame number in the log, which screenshot timings
    // line up against
    if (n <= 3 || n % 300 == 0)
        LOG("[vk] 出图第 %llu 次：纹理序号 %llu %ux%u 格式 %#x（没有交换链，只做统计与截图）", (unsigned long long)n,
            (unsigned long long)(t ? t->serial : 0), t ? t->d.width : 0, t ? t->d.height : 0, t ? t->d.format : 0);
    return 0;   // 没有出图队列：票号恒 0，后端那条 mtl_present_wait 等待即空转 | no present queue: the ticket is always 0, so the back end's mtl_present_wait returns immediately
}

// ---------- 给 app 的心跳与报告用的一行计数 ----------
// app 那边**就地声明**这两个（不去包含宿主的头，理由见 main_apk.c 里那段说明）。这一行是"进程被杀之前最后留下的现场"：
// 每秒一条，所以哪怕后来被 SIGKILL，文件里也能看出"最后一次出图是多少、之后有没有再动"。
// ---------- One line of counters for the app's heartbeat and report ----------
// The app declares these two locally (rather than including the host's headers, for the reason given in main_apk.c). This
// line is "the scene left behind before the process is killed": one per second, so even a later SIGKILL shows in the file
// how many presents happened and whether anything moved after that.
uint64_t vk_present_count(void) { return atomic_load(&g_present_n); }

int vk_diag_line(char *buf, size_t cap) {
    if (!buf || cap < 64) return 0;
    uint64_t nlay = 0, nl2 = 0, ncpso = 0, gpso = 0, nfail = 0;
    vk_pipe_stats(&nlay, &nl2, &ncpso, &gpso, &nfail);
    uint64_t tot = 0;
    for (int t = 0; t < g_rt_n; t++) tot += g_rt_use[t].n;
    return snprintf(buf, cap,
                    "出图 %llu；绘制 有目标 %llu / 无目标 %llu；T2T %llu；渲染目标 %d 张（前几名累计 %llu）；图形管线 %llu（失败 %llu）；未执行 op %llu",
                    (unsigned long long)atomic_load(&g_present_n),
                    (unsigned long long)atomic_load(&g_draw_ncN), (unsigned long long)atomic_load(&g_draw_nc0),
                    (unsigned long long)vk_tex_t2t_count(), g_rt_n, (unsigned long long)tot,
                    (unsigned long long)gpso, (unsigned long long)nfail, (unsigned long long)atomic_load(&g_op_unhandled));
}

// 按请求截图（DM_NVN_SHOT_UI / DM_TICK_HOLD=E:SHOT 用）：在当前出图帧之后第 k 帧截图。与苹果侧同签名。
// Screenshot on request (used by DM_NVN_SHOT_UI and DM_TICK_HOLD=E:SHOT): the kth frame after the current present.
// The same signature as Apple's.
void dm_gfx_shot_at(long k, const char *path) {
    pthread_mutex_lock(&g_shotq_mu);
    if (g_nshotq < 8) {
        g_shotq[g_nshotq].frame = atomic_load(&g_present_n) + (uint64_t)(k > 0 ? k : 0);
        snprintf(g_shotq[g_nshotq].path, sizeof g_shotq[0].path, "%s", path);
        g_nshotq++;
    } else LOG("【红】截图请求超过 8 个，丢弃 %s", path);
    pthread_mutex_unlock(&g_shotq_mu);
}

// 客体建窗口时逐个报过来的交换链贴图（nvnWindowInitialize）：**存储归属**的登记，不是记账——见 vk_tex.h 的说明。
// The swapchain textures the guest reports one by one when it builds a window (nvnWindowInitialize): a registration of
// **storage ownership** rather than bookkeeping -- see the note in vk_tex.h.
void mtl_xv_window_tex(uint64_t serial) {
    vk_tex_mark_window_ser(serial);
    LOG("[vk] 登记窗口贴图：序号 %llu", (unsigned long long)serial);   // 与出图时报的序号对着看 | compare against the serial reported at present
}

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
        // 槽已回收（栅栏过了）：这个槽的描述符池可以整池重置——池里所有集都被 GPU 用完且再也不用了
        // The slot is reaped (fence signalled): this slot's descriptor pool can be reset whole -- every set in it
        // has been consumed by the GPU and never will be again
        desc_slot_reset(i);
        gpu_slot_readback(i);   // 这一槽的时间戳也读回来（同一时刻：GPU 已经做完） | read this slot's timestamps too (same moment: the GPU is done)
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

// 看门狗（无进展）时调用：把图形侧所有阻塞等待点的状态写进日志——谁在等谁一眼可见。
// 与苹果侧同名同义（nvn_mtl.m 的 mtl_wait_state_dump）；安卓侧此前**没有注册**，所以"客体停住"时
// 日志里只有心跳，看不出它卡在哪（这正是第一次遇到时抓不到现场的原因）。
// Called by the watchdog (no progress): writes the state of every blocking wait point on the graphics side into the
// log so who waits for whom is visible at a glance. Same name and meaning as Apple's (mtl_wait_state_dump in
// nvn_mtl.m); the Android side **never registered** one, which is why "the guest stopped" left only heartbeats in the
// log with no sign of where it stopped (and why the first encounter had no scene at all).
void mtl_wait_state_dump(void) {
    int taken = 0;
    char slots[256]; int o = 0;
    char qst[96];   // 队列锁现在在谁手里、它打算干什么（见 vk_device.h 的 vk_queue_note）| who holds the queue lock now and what for (see vk_queue_note in vk_device.h)
    vk_queue_lock_state(qst, sizeof qst);
    for (int i = 0; i < MAX_INFLIGHT; i++) {
        if (g_inf[i].pending) taken++;
        o += snprintf(slots + o, sizeof slots - o, "%s%llu", i ? " " : "",
                      (unsigned long long)(g_inf[i].pending ? g_inf[i].value : 0));
    }
    LOG("[vk] 等待点：已提交到栅栏 %llu、在飞槽 %d/%d（各槽的栅栏值 %s）、正在录制的槽 %d、通道开着 %d、"
        "提交 %llu 次、出图 %llu 次、绘制计数 %llu（有目标 %llu / 无目标 %llu）；队列锁 %s；窗口队列 %s",
        (unsigned long long)g_fence, taken, MAX_INFLIGHT, slots, g_cur_slot, g_rs.open ? 1 : 0,
        (unsigned long long)atomic_load(&g_sub_total), (unsigned long long)atomic_load(&g_present_n),
        (unsigned long long)atomic_load(&g_draw_skip),
        (unsigned long long)atomic_load(&g_draw_ncN), (unsigned long long)atomic_load(&g_draw_nc0),
        qst, vk_queue_win() != vk_queue() ? "自己的（与客体分开）" : "与客体共用");
}

static int vk_sub_ready(void) {
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return 0;
    if (!g_inf[0].fence) {
        // 注册"等待点转储"（与苹果侧在设备初始化时注册同一件事）
        // Register the wait-point dump (the same thing Apple registers during device initialisation)
        { void crash_set_wait_dump(void (*fn)(void)); void mtl_wait_state_dump(void); crash_set_wait_dump(mtl_wait_state_dump); }
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
    gpu_slot_reset(i, g_inf[i].cb);   // 这一槽的查询区间复位（时间戳必须在同一命令缓冲里先复位再写） | reset this slot's query range (timestamps must be reset before being written, in the same command buffer)
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
    // 客体线程的提交与前端线程的上屏/读回共用同一条队列，提交这一步必须串行化（见 vk_device.h）
    // The guest thread's submits and the front end's present and readbacks share one queue, so this submit has to be
    // serialised (see vk_device.h)
    vk_queue_note("客体：提交命令缓冲");
    vk_queue_lock();
    r = vkQueueSubmit(vk_queue(), 1, &si, in->fence);
    vk_queue_unlock();
    if (r != VK_SUCCESS) { LOG("【红】提交失败 VkResult %d", (int)r); return g_fence; }
    in->pending = 1;
    in->value = ++g_fence;
    gpu_note_frame(i, atomic_load(&g_cur_frame));   // 这次提交属于哪一帧（完成是滞后的，回查靠帧号） | which frame this submission belongs to (completion is late; the lookup goes by frame number)
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
        LOG("[vk] 本窗口 %llu 条；开通道 %llu 次、清除 %llu 次、影子绑定 %llu 条、绘制跳过 %llu 条；未执行累计 %llu；片元绑定已挪到 set 1 共 %u 条、真下发 %llu 次（贴图解析成功 %llu）",
            (unsigned long long)total, (unsigned long long)atomic_load(&g_pass_n), (unsigned long long)atomic_load(&g_clear_n),
            (unsigned long long)atomic_load(&g_shadow_n), (unsigned long long)atomic_load(&g_draw_skip),
            (unsigned long long)atomic_load(&g_op_unhandled), vk_shader_set_moved(),
            (unsigned long long)atomic_load(&g_desc_draws), (unsigned long long)atomic_load(&g_desc_tex_ok));
        // 渲染目标使用前 5 与 T2T 总数：单独一行（上面的行已接近长度上限，见下一段的说明）
        // The top five render targets and the T2T total on their own line (the line above is already near the length
        // limit, see the note below)
        {
            char rl[320]; int o2 = 0;
            uint64_t tops[5] = { 0, 0, 0, 0, 0 };
            uint64_t tot = 0;
            for (int t = 0; t < g_rt_n; t++) tot += g_rt_use[t].n;
            for (int k = 0; k < 5; k++) {
                int best = -1;
                for (int t = 0; t < g_rt_n; t++) if (g_rt_use[t].n && (best < 0 || g_rt_use[t].n > g_rt_use[best].n)) best = t;
                if (best < 0 || !g_rt_use[best].n) break;
                o2 += snprintf(rl + o2, sizeof rl - o2, " 序号 %llu=%llu", (unsigned long long)g_rt_use[best].ser, (unsigned long long)g_rt_use[best].n);
                tops[k] = g_rt_use[best].ser;
                g_rt_use[best].n = 0;          // 取走后清零，下一次窗口重新数 | cleared after taking it, so the next window counts afresh
            }
            LOG("[vk] 渲染目标前 5（共 %d 张、%llu 次，另 %llu 次超表）：%s", g_rt_n, (unsigned long long)tot,
                (unsigned long long)atomic_load(&g_rt_other), o2 ? rl : "（无）");
            LOG("[vk] T2T 累计 %llu 次", (unsigned long long)vk_tex_t2t_count());
            // 绘制按有无渲染目标分开：无目标的那些**被跳过**，所以"有目标"才是真正下发的绘制
            // Draws split by whether they have render targets: the ones without are **skipped**, so "with targets" is
            // what really goes out
            LOG("[vk] 绘制：有目标 %llu、无目标 %llu（无目标者被跳过）",
                (unsigned long long)atomic_load(&g_draw_ncN), (unsigned long long)atomic_load(&g_draw_nc0));
            // 前 5 名的身份逐条问出来：序号、尺寸、格式、是不是窗口贴图 —— "这个序号到底是什么"不看名字猜不出来
            // Ask each of the top five for its identity: serial, size, format, whether it is a window texture --
            // "what is this serial" cannot be guessed from the name
            for (int k = 0; k < 5; k++) if (tops[k]) vk_tex_dump_ser(tops[k]);
        }
        // 贴图单独一行：上面的行已经接近宿主的长度上限，再挂两项就会被截断（实测过）
        // Textures get their own line: the line above is already close to the host's length limit and two more
        // fields would be cut off (measured)
        uint32_t pok = 0, pbad = 0;
        vk_shader_texpair_stats(&pok, &pbad);
        LOG("[vk] 贴图绑定：配上 %llu、写空白 %llu、空槽采样器 %llu、采样器号查不到 %llu；槽解码 成 %u/败 %u",
            (unsigned long long)atomic_load(&g_desc_tex_ok), (unsigned long long)atomic_load(&g_desc_tex_blank),
            (unsigned long long)atomic_load(&g_desc_tex_emptysamp), (unsigned long long)atomic_load(&g_desc_tex_nosamp),
            pok, pbad);
        uint64_t vb = 0, vh = 0, vm = 0;
        vk_tex_view_stats(&vb, &vh, &vm);
        LOG("[vk] 视图：底视图 %llu 次、命中子视图 %llu 次、新建子视图 %llu 张",
            (unsigned long long)vb, (unsigned long long)vh, (unsigned long long)vm);
        // 判据单独一行：宿主的日志有长度上限，挂在别的行上会被截断（这一条踩过）| the judgment gets its own line:
        // the host's log has a length limit and appending to another line truncates it (learned the hard way)
        LOG("[vk] ReadTexels 读到 GPU 写过的贴图 %llu 次（0 = 从 VkImage 读回那套暂时还只是工具）",
            (unsigned long long)vk_tex_reads_gpu_written());
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
    gpu_pass_ts(g_cur_slot, g_inf[g_cur_slot].cb, 1);   // 结束时间戳要在收通道之前写（之后写就不在这段里了）| the end timestamp goes in before the pass is closed (afterwards it would be outside the pass)
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
// ========== 贴图注册表：贴图号 → (tex_t, view) ==========
// ========== Texture registry: texture id -> (tex_t, view) ==========
// 为什么要有这一层：SPIR-V 里的贴图 binding 与 NVN 的贴图槽**不是一回事**——客体常常是把一个**句柄**（低 20 位是
// 贴图号，高位是采样器号）放进自己的常量里，着色器拿这个句柄去取贴图。所以"binding → 哪张贴图"这条路上必须先有
// 一张"贴图号 → 贴图对象"的表。宿主在游戏注册贴图时调 mtl_reg_tex（hle_nvn.c 的
// nvnTexturePoolRegisterTexture），这里把它记下来——与苹果侧 g_treg 同一个做法（按值存副本：游戏会按值拷贝
// NVNtexture，存指针会失效）。
// Why this layer exists: a texture binding in the SPIR-V is **not** an NVN texture slot -- the guest often puts a
// **handle** (low 20 bits = texture id, the high bits = sampler id) into its own constants and the shader fetches the
// texture through that handle. So the path "binding -> which texture" needs a "texture id -> texture object" table
// first. The host calls mtl_reg_tex when the game registers a texture (nvnTexturePoolRegisterTexture in hle_nvn.c) and
// this records it -- the same shape as Apple's g_treg (a copy by value: the game copies NVNtexture by value and
// storing a pointer would go stale).
#define TREG_TAB (1 << 14)
typedef struct { int used, tid, has_v; tex_t t; view_t v; } treg_e;
static treg_e g_treg[TREG_TAB];
static _Atomic uint64_t g_treg_n;

void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id <= 0 || !t) return;
    unsigned h = ((unsigned)id * 2654435761u) & (TREG_TAB - 1);
    for (unsigned k = 0; k < TREG_TAB; k++) {
        treg_e *e = &g_treg[(h + k) & (TREG_TAB - 1)];
        if (!e->used) {
            e->used = 1; e->tid = id; e->t = *t;
            if (v) { e->v = *v; e->has_v = 1; }
            atomic_fetch_add(&g_treg_n, 1);
            return;
        }
        if (e->tid == id) {   // 同一个号重新注册：按值刷新（游戏会重建对象）| re-registration of one id: refresh by value (the game rebuilds objects)
            e->t = *t;
            if (v) { e->v = *v; e->has_v = 1; }
            return;
        }
    }
    static int said;
    if (!said++) LOG("【红】贴图注册表满了（%d 项）：后面的贴图句柄解析不出来", TREG_TAB);
}

// 句柄 → 注册项；低 20 位是贴图号（与苹果侧 h_ser 同一套取法）
// handle -> registry entry; the low 20 bits are the texture id (the same extraction as Apple's h_ser)
static const treg_e *treg_of(uint32_t handle) {
    int tid = (int)(handle & 0xfffffu);
    if (!tid) return NULL;
    unsigned h = ((unsigned)tid * 2654435761u) & (TREG_TAB - 1);
    for (unsigned k = 0; k < TREG_TAB; k++) {
        const treg_e *e = &g_treg[(h + k) & (TREG_TAB - 1)];
        if (!e->used) return NULL;
        if (e->tid == tid) return e;
    }
    return NULL;
}

// ========== 采样器注册表：采样器号 → 参数 ==========
// ========== Sampler registry: sampler id -> parameters ==========
// 宿主在游戏建采样器时调 mtl_reg_samp（hle_nvn.c 的 nvnSamplerPoolRegisterSampler，传的就是采样器对象里的
// `samp_par`），安卓侧原本是**空桩**。绑定时拿到的句柄高 20 位是采样器号（`samp[阶段][槽] >> 20`，
// 与苹果侧同一套取法），所以需要这张表把号换回参数，再交给纹理层已有的 vk_samp_of 按参数内容建/取 VkSampler。
// The host calls mtl_reg_samp when the game builds a sampler (nvnSamplerPoolRegisterSampler in hle_nvn.c, passing the
// `samp_par` inside the sampler object), and on Android it was a **stub**. A bound handle carries the sampler id in
// its high 20 bits (`samp[stage][slot] >> 20`, the same extraction as Apple), so this table turns the id back into
// parameters and hands them to the texture layer's existing vk_samp_of, which caches a VkSampler by parameter content.
#define SREG_TAB 4096
typedef struct { int used, id; samp_par p; } sreg_e;
static sreg_e g_sreg[SREG_TAB];
static _Atomic uint64_t g_sreg_n;

void mtl_reg_samp(int id, const samp_par *p) {
    if (id < 0 || !p) return;
    unsigned h = ((unsigned)id * 2654435761u) & (SREG_TAB - 1);
    for (unsigned k = 0; k < SREG_TAB; k++) {
        sreg_e *e = &g_sreg[(h + k) & (SREG_TAB - 1)];
        if (!e->used) { e->used = 1; e->id = id; e->p = *p; atomic_fetch_add(&g_sreg_n, 1); return; }
        if (e->id == id) { e->p = *p; return; }   // 同号重注册：按值刷新 | re-registration of one id: refresh by value
    }
    static int said;
    if (!said++) LOG("【红】采样器注册表满了（%d 项）：后面的采样器取不到参数", SREG_TAB);
}

static VkSampler samp_of_id(uint32_t sid) {
    unsigned h = (sid * 2654435761u) & (SREG_TAB - 1);
    for (unsigned k = 0; k < SREG_TAB; k++) {
        const sreg_e *e = &g_sreg[(h + k) & (SREG_TAB - 1)];
        if (!e->used) return VK_NULL_HANDLE;
        if ((unsigned)e->id == sid) {
            samp_t t;
            memset(&t, 0, sizeof t);
            t.p = e->p;            // vk_samp_of 只读 p（按参数内容缓存）| vk_samp_of reads only p (cached by content)
            return vk_samp_of(&t);
        }
    }
    return VK_NULL_HANDLE;
}

// ========== 描述符写入（M2-6d-3 的 B 步）==========
// ========== Descriptor writing (step B of M2-6d-3) ==========
// 三件事决定了这里的形状：
//   ① **池按在途槽开**：描述符集在 GPU 用完之前不能改写，而回放层本来就有"在途槽 + 栅栏"这套（见 g_inf），
//      所以每个槽一个池，槽被回收（栅栏已过）时整池重置——不需要任何额外的同步；
//   ② **缓冲走动态偏移**：客体数据就在零拷贝池里，`mtl_addr` 给出(缓冲, 偏移)，所以描述符里存基址、
//      偏移走 dynamicOffsets（与苹果侧的 setBuffer:offset: 同一个意思）。动态偏移数组必须按**布局顺序**
//      （binding 升序）填，所以顺序来自 vk_pipe_binds 的同一张表，不在两边各算一次；
//   ③ **缓冲地址为 0 或不认识 → 写空白**：写空描述符是必须的（未写的描述符内容不确定），而游戏没绑时
//      NVN 自己也读 0，所以空白是忠实的回退。
// Three facts shape this:
//   1. **A pool per in-flight slot**: a descriptor set cannot be rewritten until the GPU is done with it, and the
//      replay layer already has in-flight slots with fences (see g_inf), so each slot owns a pool that is reset
//      whole once the slot is reaped (fence signalled) -- no extra synchronisation needed;
//   2. **Buffers go through dynamic offsets**: the guest data already sits in zero-copy pools and `mtl_addr`
//      returns (buffer, offset), so the descriptor holds the base and the offset travels in dynamicOffsets (the
//      same meaning as setBuffer:offset: on Apple). The dynamic offset array must be filled in **layout order**
//      (ascending binding), so that order comes from the same vk_pipe_binds table rather than being computed twice;
//   3. **A zero or unrecognised buffer address writes a blank**: writing something is mandatory (an unwritten
//      descriptor is undefined), and NVN itself reads 0 when the game bound nothing, so a blank is a faithful
//      fallback.
#define DESC_POOL_SETS 512
#define DESC_POOLS 2                      // 一个槽最多两池：满了再开一池，回收时一起销毁 | at most two pools per slot: a second one when full, both destroyed on reap
static VkDescriptorPool g_dpool[MAX_INFLIGHT][DESC_POOLS];
static uint8_t g_dpool_n[MAX_INFLIGHT];
static _Atomic uint64_t g_desc_sets, g_desc_blank, g_desc_skip;
// 按**阶段**给 BIND_UBO 计数（含"地址为 0"的那些）：问的是"游戏到底往哪个阶段绑过常量"，
// 因为写空白的现场只能看出"这个槽没有地址"，看不出"是绑到了别的阶段、还是根本没绑"。
// Count BIND_UBO by **stage** (including the ones whose address is 0): the question is "which stage did the game bind
// constants for at all", because a blank's situation shows only "this slot has no address" and not whether it was
// bound under another stage or never bound.
static _Atomic uint64_t g_ubo_binds[6], g_ubo_zero[6], g_ubo_out;
static _Atomic uint64_t g_ssbo_binds[6];   // 同上，按阶段给 BIND_SSBO 计数（存储缓冲写空白的现场要看这个）| same for BIND_SSBO (needed by the storage-buffer blank's situation)

static void desc_slot_reset(int slot) {
    for (int k = 0; k < DESC_POOLS; k++)
        if (g_dpool[slot][k]) { vkDestroyDescriptorPool(vk_dev(), g_dpool[slot][k], NULL); g_dpool[slot][k] = VK_NULL_HANDLE; }
    g_dpool_n[slot] = 0;
}

static VkDescriptorPool desc_pool_at(int slot, int idx) {
    if (g_dpool[slot][idx]) return g_dpool[slot][idx];
    if (idx >= DESC_POOLS) return VK_NULL_HANDLE;
    VkDescriptorPoolSize ps[4] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, DESC_POOL_SETS },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, DESC_POOL_SETS },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, DESC_POOL_SETS },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 128 },
    };
    VkDescriptorPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = DESC_POOL_SETS, .poolSizeCount = 4, .pPoolSizes = ps };
    VkDescriptorPool p = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(vk_dev(), &ci, NULL, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (g_dpool_n[slot] < idx + 1) g_dpool_n[slot] = (uint8_t)(idx + 1);
    return (g_dpool[slot][idx] = p);
}

static VkDescriptorSet desc_alloc(int slot, VkDescriptorSetLayout lay) {
    for (int idx = 0; idx < DESC_POOLS; idx++) {
        VkDescriptorPool p = desc_pool_at(slot, idx);
        if (!p) break;
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = p, .descriptorSetCount = 1, .pSetLayouts = &lay };
        VkDescriptorSet s = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(vk_dev(), &ai, &s) == VK_SUCCESS) return s;
    }
    return VK_NULL_HANDLE;
}

// 按 binding 找 uniform 缓冲的客体 c 槽（来自 res.json 的 cbufs）
// Find the guest c slot of a uniform buffer by binding (from res.json's cbufs)
static int desc_cbuf_slot(const vk_shader *sh, uint32_t binding) {
    for (uint32_t i = 0; i < sh->ncbuf; i++) if (sh->cbuf[i].binding == binding) return (int)sh->cbuf[i].slot;
    return -1;
}
static const vk_sbuf_ref *desc_sbuf_ref(const vk_shader *sh, uint32_t binding) {
    for (uint32_t i = 0; i < sh->nsbuf; i++) if (sh->sbuf[i].binding == binding) return &sh->sbuf[i];
    return NULL;
}
static const vk_tex_ref *desc_tex_ref(const vk_shader *sh, uint32_t binding) {
    for (uint32_t i = 0; i < sh->ntex; i++) if (sh->tex[i].binding == binding) return &sh->tex[i];
    return NULL;
}
// 声明了这条 (set, binding) 的着色器。**set 必须一起比**：同一个号在 set 0（顶点空间）与 set 1（片元空间）
// 是两个互不相干的绑定，只按号找会拿到另一个阶段的反射。
// The shader that declares this (set, binding). **The set must be compared too**: the same number on set 0 (the
// vertex space) and set 1 (the fragment space) are two unrelated bindings, and looking up by number alone would
// fetch the other stage's reflection.
static const vk_shader *desc_stage_of(const vk_shader *const *sh, int n, uint32_t set, uint32_t binding) {
    for (int i = 0; i < n; i++) {
        if (!sh[i]) continue;
        for (uint32_t k = 0; k < sh[i]->nspv; k++)
            if (sh[i]->spv[k].binding == binding && sh[i]->spv[k].set == set) return sh[i];
    }
    return NULL;
}

// ---------- 特殊常量段：support_buffer / c1 / 驱动段 ----------
// 这三类都不是"游戏绑的内存"，而是回放层自己供给着色器的（苹果侧同样自己造：support_buf / zero_bank）。
// support_buffer 的布局与取值照 Ryujinx 的 SupportBuffer（见 nvn_mtl.m 的 support_buf）：render_scale 必须为 1，
// 否则着色器按 0 去缩放，屏幕上会一个像素都不落。
// ---------- Special constant segments: support_buffer, c1 and the driver bank ----------
// None of these three is memory the game binds; the replay layer supplies them to the shader itself (Apple builds its
// own too: support_buf / zero_bank). The layout and values of support_buffer follow Ryujinx's SupportBuffer (see
// support_buf in nvn_mtl.m): render_scale must be 1, or the shader scales by 0 and not a pixel lands.
#define BANK_BYTES (64u << 10)
#define SUPPORT_RENDER_SCALE_OFF 192   // MSL 布局实测（与苹果侧注释同源）：render_scale float4[73]，取 .x | measured MSL layout (same source as Apple's note): render_scale float4[73], take .x
static VkBuffer g_bank[2];             // 0 = support_buffer、1 = 全零（c1 与驱动段）| 0 = support_buffer, 1 = all zero (c1 and the driver bank)
static VkBuffer bank_get(int which) {
    if (g_bank[which]) return g_bank[which];
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BANK_BYTES,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer b = VK_NULL_HANDLE;
    if (vkCreateBuffer(vk_dev(), &bci, NULL, &b) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk_dev(), b, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = vk_memory_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mr.memoryTypeBits) };
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (vkAllocateMemory(vk_dev(), &mai, NULL, &m) != VK_SUCCESS || vkBindBufferMemory(vk_dev(), b, m, 0) != VK_SUCCESS) {
        if (m) vkFreeMemory(vk_dev(), m, NULL);
        vkDestroyBuffer(vk_dev(), b, NULL);
        LOG("【红】特殊常量段建不出来（哪一段 %d）", which);
        return VK_NULL_HANDLE;
    }
    void *p = NULL;
    if (vkMapMemory(vk_dev(), m, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
        memset(p, 0, BANK_BYTES);
        if (!which) for (int i = 0; i < 73; i++) *(float *)((uint8_t *)p + SUPPORT_RENDER_SCALE_OFF + 16 * i) = 1.0f;
        vkUnmapMemory(vk_dev(), m);
    }
    g_bank[which] = b;
    LOG("[vk] 特殊常量段 %s 就绪（%u 字节）：%s", which ? "c1 / 驱动段" : "support_buffer", (unsigned)BANK_BYTES,
        which ? "c1 的真实数据在控制段里，暂以全零代替" : "render_scale = 1");
    return b;
}

// 缓冲类绑定的地址与长度：UBO 直接来自 c 槽，SSBO 先看有没有直接绑（游戏用 BindStorageBuffer），
// 否则按 res.json 的讲法从 c[cb_slot] 的 cb_offset 处读那个地址。
// host_buf 非空时表示用宿主自己的缓冲（上面的特殊常量段），此时 addr/size 无意义。
// Address and length of a buffer binding: a UBO comes straight from its c slot, an SSBO first tries a direct
// binding (the game uses BindStorageBuffer) and otherwise reads the address at c[cb_slot] + cb_offset, as res.json
// says. A non-NULL host_buf means the host's own buffer is used (the special segments above) and addr/size mean nothing.
static int desc_buf_range(const vk_shader *sh, const vk_pipe_bind *b, uint64_t *addr, uint64_t *size, VkBuffer *host_buf) {
    int stg = sh->stage >= 0 && sh->stage < 6 ? sh->stage : 0;
    if (host_buf) *host_buf = VK_NULL_HANDLE;
    if (b->kind == VK_SPV_UBO) {
        // res.json 的槽号是**名字里的号**（名字形如 fp_c3 / fp_c13），NVN 的常量槽是它减 3；名字里的 1 是特殊段
        // c1、2 是驱动段，都不是"游戏绑的 UBO"；一条 cbuf 都没有 = Ryujinx 的 support_buffer。规则与苹果侧同源：
        // dm/gfx/tools/dmsh_build.py 的 reflect()。
        // res.json's slot is the **number in the name** (names like fp_c3 / fp_c13) and the NVN constant slot is that
        // minus 3; a 1 in a name is the special c1 bank and a 2 the driver bank, neither of them a UBO the game binds;
        // no cbuf entry at all means Ryujinx's support_buffer. The rule comes from where Apple gets it: reflect() in
        // dm/gfx/tools/dmsh_build.py.
        int slot = desc_cbuf_slot(sh, b->binding);
        if (slot < 0 || slot == 2 || slot == 1) {
            if (!host_buf) return 0;
            *host_buf = bank_get(slot == 2 ? 1 : (slot == 1 ? 1 : 0));
            if (!*host_buf) return 0;
            *addr = 0; *size = BANK_BYTES;
            return 1;
        }
        int nvn = slot - 3;   // 名字里的号 → NVN 常量槽 | the number in the name -> the NVN constant slot
        if (nvn < 0 || nvn >= 16) return 0;
        *addr = g_rs.ubo[stg][nvn].addr;
        *size = g_rs.ubo[stg][nvn].size;
        return *addr != 0;
    }
    const vk_sbuf_ref *r = desc_sbuf_ref(sh, b->binding);
    if (r) {
        // 存储缓冲的 NVN 槽号**不能直接用 res.json 的 slot**（那是 Ryujinx 自己的编号，与游戏绑的号不是一套）。
        // 与苹果侧同源的做法是从 cb_offset 反推：存储缓冲地址表在各阶段的固定基址（顶点 0x110、像素 0x510、
        // 计算 0x310，每槽 0x10 字节），所以 NVN 槽 = (cb_offset × 4 − 基址) ÷ 16。
        // 真机取证：cb_offset 340/344 → 0x550/0x560 → 槽 4/5，而游戏绑的正是 4..7（位图 0xf0）。
        // A storage buffer's NVN slot **cannot use res.json's slot directly** (that is Ryujinx's own numbering, a
        // different space from the index the game binds). The Apple side derives it from cb_offset instead: the storage
        // buffer address table sits at a fixed base per stage (vertex 0x110, pixel 0x510, compute 0x310, 0x10 bytes per
        // slot), so NVN slot = (cb_offset x 4 - base) / 16. Device forensics: cb_offset 340/344 -> 0x550/0x560 -> slots
        // 4/5, while the game binds exactly 4..7 (bitmap 0xf0).
        static const uint32_t sb_base[6] = { 0x110, 0x510, 0, 0, 0, 0x310 };   // 0=顶点 1=像素 5=计算（其余阶段未实测）| 0 = vertex, 1 = pixel, 5 = compute (the other stages are unmeasured)
        int slot = r->slot;
        uint32_t off = (uint32_t)r->cb_offset * 4, b0 = stg < 6 ? sb_base[stg] : 0;
        if (b0 && r->cb_slot == 0 && off >= b0 && off < b0 + 0x100 && (off - b0) % 16 == 0) slot = (int)((off - b0) / 16);
        if (slot >= 0 && slot < 128 && g_rs.ssbo[stg][slot].addr) {
            *addr = g_rs.ssbo[stg][slot].addr;
            *size = g_rs.ssbo[stg][slot].size;
            return 1;
        }
        // 地址在常量段里：c[cb_slot] 那段的开头 + cb_offset 处放着一个 64 位地址
        // The address sits in the constant bank: a 64-bit address at the start of c[cb_slot] plus cb_offset
        int cs = r->cb_slot;
        if (cs >= 0 && cs < 16 && g_rs.ubo[stg][cs].addr && r->cb_offset + 8 <= g_rs.ubo[stg][cs].size) {
            uint64_t a = 0;
            memcpy(&a, (const void *)(uintptr_t)(g_rs.ubo[stg][cs].addr + (uint64_t)r->cb_offset), 8);
            if (a) { *addr = a; *size = g_rs.ubo[stg][cs].size - (uint64_t)r->cb_offset; return 1; }
        }
    }
    return 0;
}

// 写一个阶段的全部绑定。返回值是填入的动态偏移个数（-1 = 没法写，调用方跳过这次绘制）。
// 动态偏移与 vk_pipe_binds 的表同序——这是 vkCmdBindDescriptorSets 的要求。
// Write every binding for one set. The return value is how many dynamic offsets were filled in (-1 means it cannot
// be written and the caller skips the draw). The dynamic offsets are in the same order as the vk_pipe_binds table,
// which is what vkCmdBindDescriptorSets requires.
// 贴图句柄来处的取证（一次性，写进日志）。res.json 说"句柄从常量里读"，但编码没定下来
// （全库抽样 1205 条 cbuf_slot 全是 0x30002，而 handle 是 0x101D815A 这类客体地址）。两条路一起试：
//   ① 把 res.json 里那个 handle 当**客体地址**读 4 字节——它可能就是"句柄所在的地址"；
//   ② 扫该阶段绑定的常量段，找"低 20 位是已注册贴图号"的 32 位字——找到了既证明编码、又直接给出该读的值。
// 这是"用真机的数定案"而不是猜（与仓库一贯做法一致）。
// First-hand evidence for where a texture handle comes from (once, into the log). res.json says "the handle is read
// from the constants" but the encoding is not settled (a library-wide sample shows all 1205 cbuf_slot values are
// 0x30002 while handle is a guest address like 0x101D815A). Two routes are tried together:
//   1. read 4 bytes at the handle value treated as a **guest address** -- it may be "the address the handle lives at";
//   2. scan the constants bound for that stage for a 32-bit word whose low 20 bits are a registered texture id --
//      finding one both proves the encoding and hands over the value to read.
// This settles it with the device's numbers rather than guessing (the repository's usual way).
static void desc_tex_probe(const vk_shader *st, const vk_tex_ref *tr) {
    static int done;
    if (!tr || done >= 3) return;
    done++;
    int stg = st->stage >= 0 && st->stage < 6 ? st->stage : 0;
    LOG("【查】贴图句柄：binding %u、cbuf_slot=%d、handle=0x%x、阶段 %d、已注册贴图 %llu 项",
        tr->binding, tr->cbuf_slot, (unsigned)tr->handle, stg, (unsigned long long)atomic_load(&g_treg_n));
    // 该阶段到底绑了什么。早先我只打印了前三个槽就写下"一张都没绑"——那是个**坏样本**：槽号可以远大于 3，
    // 于是"没绑"这个结论可能是错的。这里改成普查（贴图 128 槽、采样器 128 槽、图像 32 槽）。
    // What this stage actually binds. Earlier I printed only the first three slots and concluded "nothing bound" --
    // that was a **bad sample**: slot numbers can be far above 3, so the conclusion could be wrong. This does a full
    // census instead (128 texture slots, 128 sampler slots, 32 image slots).
    int nt = 0, ns = 0, ni = 0;
    uint64_t t0 = 0, s0 = 0, i0 = 0;
    for (int k = 0; k < 128; k++) if (g_rs.tex[stg][k]) { if (!nt) t0 = g_rs.tex[stg][k]; nt++; }
    for (int k = 0; k < 128; k++) if (g_rs.samp[stg][k]) { if (!ns) s0 = g_rs.samp[stg][k]; ns++; }
    for (int k = 0; k < 32; k++) if (g_rs.img[stg][k]) { if (!ni) i0 = g_rs.img[stg][k]; ni++; }
    LOG("【查】阶段 %d 绑了：贴图 %d 张、采样器 %d 个、图像 %d 张；贴图池 %s", stg, nt, ns, ni, g_rs.have_texpool ? "已设" : "未设");
    if (nt) LOG("【查】首个贴图句柄 0x%llx（低 20 位 %u、采样器号 %llu）→ 注册表 %s", (unsigned long long)t0,
                (unsigned)(t0 & 0xfffffu), (unsigned long long)(t0 >> 20), treg_of((uint32_t)t0) ? "命中" : "不在");
    if (ns) LOG("【查】首个采样器句柄 0x%llx（低 20 位 %u）", (unsigned long long)s0, (unsigned)(s0 & 0xfffffu));
    if (ni) LOG("【查】首张图像句柄 0x%llx", (unsigned long long)i0);
    // 判定"贴图槽与采样器槽是否相交"用的现场：把非零采样器槽列出来（最多 8 个）——与上面的贴图槽集合一比即知。
    // desc_tex_probe 打印的贴图槽分布来自"配不上的绑定"那条路，所以这里同时把非零贴图槽也列出来。
    // The situation for deciding whether texture and sampler slots intersect: list the non-zero sampler slots (at
    // most eight). Comparing that set with the texture slots settles it.
    {
        char sb[200];
        size_t so = 0;
        int shown = 0;
        for (int k = 0; k < 128 && shown < 8 && so + 24 < sizeof sb; k++)
            if (g_rs.samp[stg][k]) { so += (size_t)snprintf(sb + so, sizeof sb - so, " %d:0x%llx", k, (unsigned long long)g_rs.samp[stg][k]); shown++; }
        sb[so] = 0;
        LOG("【查】阶段 %d 的非零采样器槽（最多 8 个）：%s", stg, so ? sb : "（没有）");
    }
    uint32_t ha = (uint32_t)tr->handle;
    void *hb = NULL; uint64_t ho = 0;
    if (ha && mtl_addr(ha, 4, &hb, &ho)) {
        uint32_t v = 0;
        memcpy(&v, (const void *)(uintptr_t)ha, 4);
        LOG("【查】handle 当地址可读：0x%x（低 20 位 %u、采样器号 %u）→ 注册表 %s",
            v, v & 0xfffffu, v >> 20, treg_of(v) ? "命中" : "不在");
    } else {
        LOG("【查】handle 不在任何池里：0x%x", ha);
    }
    for (int s = 0; s < 16; s++) {
        uint64_t a = g_rs.ubo[stg][s].addr, sz = g_rs.ubo[stg][s].size;
        if (!a || !sz) continue;
        void *bb = NULL; uint64_t bo = 0;
        if (!mtl_addr(a, 1, &bb, &bo)) { LOG("【查】常量槽 %d 地址 %llx 不在池里", s, (unsigned long long)a); continue; }
        size_t lim = sz > 4096 ? 4096 : (size_t)sz;
        const uint8_t *p = (const uint8_t *)(uintptr_t)a;
        int hits = 0;
        for (size_t k = 0; k + 4 <= lim; k += 4) {
            uint32_t w = 0;
            memcpy(&w, p + k, 4);
            if (!w) continue;
            if (treg_of(w)) {
                if (hits < 4) LOG("【查】常量槽 %d 偏移 %zu：0x%x（贴图号 %u、采样器号 %u）命中注册表", s, k, w, w & 0xfffffu, w >> 20);
                hits++;
            }
        }
        LOG("【查】常量槽 %d：地址 %llx、%llu 字节，扫 %zu 字节、命中 %d 处", s, (unsigned long long)a, (unsigned long long)sz, lim, hits);
    }
}

static int desc_write(VkDescriptorSet sets[2], const vk_shader *const *sh, int n,
                      const vk_pipe_bind *b, int nb, uint32_t *dyn) {
    // 一屏最多 64 条绑定，用静态数组（mtl_exec 递归，不能用大栈帧 | see the note on gfx_state_fill）
    static VkDescriptorBufferInfo bi[VK_SPV_MAXB];
    static VkDescriptorImageInfo ii[VK_SPV_MAXB];
    static VkWriteDescriptorSet wr[VK_SPV_MAXB];
    int nw = 0, nd = 0;
    for (int i = 0; i < nb; i++) {
        // 每条绑定写进它自己那个 set（set 0 = 顶点空间、set 1 = 片元空间）。表按 (set, binding) 升序，
        // 所以动态偏移也是按这个顺序落进 dyn[] —— 正是 vkCmdBindDescriptorSets 消费它们的顺序。
        // Every binding goes into its own set (set 0 = the vertex space, set 1 = the fragment space). The table is
        // sorted by (set, binding), so the dynamic offsets land in dyn[] in that order -- which is exactly the order
        // vkCmdBindDescriptorSets consumes them in.
        if (b[i].set > 1 || !sets[b[i].set]) continue;
        const vk_shader *st = desc_stage_of(sh, n, b[i].set, b[i].binding);
        if (!st) continue;
        VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = sets[b[i].set], .dstBinding = b[i].binding, .descriptorCount = b[i].count ? b[i].count : 1 };
        if (b[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || b[i].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
            uint64_t a = 0, sz = 0;
            void *buf = NULL; uint64_t off = 0;
            VkBuffer hb = VK_NULL_HANDLE;
            if (!desc_buf_range(st, &b[i], &a, &sz, &hb) || (!hb && !mtl_addr(a, sz, &buf, &off))) {
                // 空白回退：游戏没绑这一段（NVN 自己读 0），或地址不在任何池里
                // Blank fallback: the game bound no such range (NVN reads 0 itself) or the address is in no pool
                if (atomic_fetch_add(&g_desc_blank, 1) < 5) {
                    // 取证：这条绑定由哪个阶段的着色器声明、那个槽在**各阶段**分别是什么值、各阶段总共绑过多少条 UBO。
                    // 判读法：某另一阶段有值 = 阶段没对上；哪一阶段都没有 = 游戏没绑（或它给的编号不是槽号）。
                    // Forensics: which stage's shader declares this binding, what that slot holds in **each** stage, and
                    // how many UBOs each stage has been bound in total. Reading: a value under another stage means the
                    // stages do not line up; nothing under any stage means the game never bound it (or its index is
                    // not the slot).
                    int sd = desc_cbuf_slot(st, b[i].binding);
                    uint64_t pa[6] = { 0, 0, 0, 0, 0, 0 };
                    if (sd >= 0 && sd < 16) for (int g2 = 0; g2 < 6; g2++) pa[g2] = g_rs.ubo[g2][sd].addr;
                    if (b[i].kind == VK_SPV_SSBO) {
                        // 存储缓冲：res.json 的三个数（Ryujinx 的槽号、地址表所在常量段、偏移）与**游戏实际绑的号**
                        // 是不是一套，就看这一行（UBO 那边已经证过"res.json 的槽号要减 3 才是 NVN 的槽"）。
                        // Storage buffer: this line shows whether res.json's three numbers (Ryujinx's slot, the constant
                        // bank holding the address table, the offset) and **the index the game actually binds** are the
                        // same space (the UBO side already proved that res.json's slot needs a minus 3 to be NVN's slot).
                        const vk_sbuf_ref *r2 = desc_sbuf_ref(st, b[i].binding);
                        LOG("【黄】binding %u（storage buffer）拿不到缓冲地址、声明者阶段 %d：res.json 槽 %d、cb_slot %d、cb_offset %d；已绑 SSBO 位图（阶段 0..5 的低 64 位）%llx/%llx/%llx/%llx/%llx/%llx；BIND_SSBO 各阶段计数 %llu/%llu/%llu/%llu/%llu/%llu",
                            b[i].binding, st->stage, r2 ? r2->slot : -999, r2 ? r2->cb_slot : -999, r2 ? r2->cb_offset : -999,
                            (unsigned long long)g_rs.ssbo_have[0][0], (unsigned long long)g_rs.ssbo_have[1][0],
                            (unsigned long long)g_rs.ssbo_have[2][0], (unsigned long long)g_rs.ssbo_have[3][0],
                            (unsigned long long)g_rs.ssbo_have[4][0], (unsigned long long)g_rs.ssbo_have[5][0],
                            (unsigned long long)atomic_load(&g_ssbo_binds[0]), (unsigned long long)atomic_load(&g_ssbo_binds[1]),
                            (unsigned long long)atomic_load(&g_ssbo_binds[2]), (unsigned long long)atomic_load(&g_ssbo_binds[3]),
                            (unsigned long long)atomic_load(&g_ssbo_binds[4]), (unsigned long long)atomic_load(&g_ssbo_binds[5]));
                    } else
                    LOG("【黄】binding %u（%s）拿不到缓冲地址（地址 %llx、槽解析 %d、声明者阶段 %d）：写空白；同一槽各阶段 %llx/%llx/%llx/%llx/%llx/%llx；已绑上的槽位图（按阶段）%#x/%#x/%#x/%#x/%#x/%#x；BIND_UBO 各阶段计数 %llu/%llu/%llu/%llu/%llu/%llu（地址 0 的 %llu/%llu/%llu/%llu/%llu/%llu）",
                        b[i].binding, vk_spv_kind_name(b[i].kind), (unsigned long long)a, sd, st->stage,
                        (unsigned long long)pa[0], (unsigned long long)pa[1], (unsigned long long)pa[2],
                        (unsigned long long)pa[3], (unsigned long long)pa[4], (unsigned long long)pa[5],
                        (unsigned)g_rs.ubo_have[0], (unsigned)g_rs.ubo_have[1], (unsigned)g_rs.ubo_have[2],
                        (unsigned)g_rs.ubo_have[3], (unsigned)g_rs.ubo_have[4], (unsigned)g_rs.ubo_have[5],
                        (unsigned long long)atomic_load(&g_ubo_binds[0]), (unsigned long long)atomic_load(&g_ubo_binds[1]),
                        (unsigned long long)atomic_load(&g_ubo_binds[2]), (unsigned long long)atomic_load(&g_ubo_binds[3]),
                        (unsigned long long)atomic_load(&g_ubo_binds[4]), (unsigned long long)atomic_load(&g_ubo_binds[5]),
                        (unsigned long long)atomic_load(&g_ubo_zero[0]), (unsigned long long)atomic_load(&g_ubo_zero[1]),
                        (unsigned long long)atomic_load(&g_ubo_zero[2]), (unsigned long long)atomic_load(&g_ubo_zero[3]),
                        (unsigned long long)atomic_load(&g_ubo_zero[4]), (unsigned long long)atomic_load(&g_ubo_zero[5]));
                }
                VkBuffer dum = vk_pipe_dummy_buffer();
                if (!dum) return -1;
                bi[nw] = (VkDescriptorBufferInfo){ dum, 0, VK_WHOLE_SIZE };
            } else if (hb) {
                // 特殊常量段：宿主自己的缓冲，没有动态偏移
                // A special constant segment: the host's own buffer, with no dynamic offset
                bi[nw] = (VkDescriptorBufferInfo){ hb, 0, VK_WHOLE_SIZE };
            } else {
                // 动态偏移之和必须落在 minUniform/StorageBufferOffsetAlignment 的整数倍上，而规范保证这个
                // 界限不超过 256，所以按 256 对齐一定成立；客体自己的缓冲地址本该对齐，没对齐就记一次。
                // The sum of offset and dynamic offset must be a multiple of the alignment limit, which the spec
                // guarantees to be at most 256, so aligning to 256 always works; the guest's own buffer addresses
                // should be aligned and a misaligned one is reported once.
                uint64_t al = off & 255u;
                if (al) { static int said; if (!said++) LOG("【黄】缓冲地址没按 256 对齐（偏移 %llu）：按对齐下取整 + 动态偏移补", (unsigned long long)off); }
                bi[nw] = (VkDescriptorBufferInfo){ (VkBuffer)buf, off - al, VK_WHOLE_SIZE };
                dyn[nd++] = (uint32_t)al;
            }
            w.descriptorType = b[i].type;
            w.pBufferInfo = &bi[nw];
            nw++;
        } else if (b[i].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b[i].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            // 贴图 / 图像：句柄的来处分两种情况（res.json 的 cbuf_slot 与 handle）——
            //   cbuf_slot < 0：句柄是着色器常量里 baked 的那个数，本身就是**贴图号** → 查注册表拿贴图；
            //   cbuf_slot >= 0：句柄在客体的常量里（要从 c[..] 读），**编码还没定下来**（本地全库抽样 1205 条
            //                   全是 0x30002，而 handle 是 0x101D815A 这类客体地址）→ 先写空白，并把现场
            //                   （这一条 + 该阶段绑着的贴图句柄 + 常量段地址）打进日志，用真机的数把它定下来。
            // Textures / images: where the handle comes from has two cases (cbuf_slot and handle in res.json) --
            //   cbuf_slot < 0: the handle is the number baked into the shader's constants and is itself the
            //   **texture id**, so the registry yields the texture;
            //   cbuf_slot >= 0: the handle lives in the guest's constants (it has to be read from c[..]) and the
            //   **encoding is not settled yet** (a library-wide sample of 1205 entries is all 0x30002 while the
            //   handle is a guest address like 0x101D815A) -- a blank is written and the situation (this entry plus
            //   the texture handles bound for that stage and the constant-bank addresses) goes into the log so the
            //   device's numbers can settle it.
            const vk_tex_ref *tr = desc_tex_ref(st, b[i].binding);
            int stg_t = st->stage >= 0 && st->stage < 6 ? st->stage : 0;
            VkImageView texv = VK_NULL_HANDLE;
            VkSampler texs = VK_NULL_HANDLE;
            // 槽由接口描述表配对而来（见 vk_shader.c 那一节）。有了槽，就该阶段"绑在第几号"有了答案：
            // **贴图与采样器都按同一个槽取**——NVN 里这两者就是按槽号配对绑定的（分开绑的两个数组，同一号成对）。
            // The slot comes from the interface table (see the section in vk_shader.c). With a slot there is an
            // answer to "which slot is bound for this stage": **both the texture and the sampler are taken by that
            // same slot** -- in NVN the two are bound paired by slot number (two separate arrays, paired by index).
            if (tr && tr->nvn_slot >= 0 && tr->nvn_slot < 128) {
                uint64_t h = g_rs.tex[stg_t][tr->nvn_slot];
                if (h) {
                    const treg_e *e = treg_of((uint32_t)h);
                    if (e) {
                        // 注册时存下的 view 在这里**第一次被用上**（以前存了却从没读过）：它可能限定 mip/层范围、
                        // 重解释格式或目标类型——也就是"按游戏的意思采样"这一步。
                        // The view stored at registration time is **used here for the first time** (it used to be stored
                        // and never read): it may limit the mip/layer range or reinterpret the format or target, which is
                        // what "sampling the way the game means it" takes.
                        texv = vk_tex_view(&e->t, e->has_v ? &e->v : NULL);
                        // 采样器按**采样器槽**取（不是贴图槽！两套编号：分离贴图在 c2[0x168+8r]、分离采样器在
                        // c2[0x568+8r]，`nvn_sslot` 是从同一个 handle 的第二个字解出来的）。句柄高 20 位是采样器
                        // 池号，但 0xFFF 是"填空槽"的哨兵——那种槽上的绑定本来不会被采样，用默认采样器是对的。
                        // The sampler is taken by its **sampler slot** (not the texture slot! Two numberings: separate
                        // textures at c2[0x168+8r], separate samplers at c2[0x568+8r], and `nvn_sslot` is decoded from
                        // the second word of the same handle). The handle's high 20 bits are the sampler pool id while
                        // 0xFFF is the "slot filled in" sentinel -- a binding on such a slot is never sampled, so a
                        // default sampler is correct there.
                        uint64_t sh_ = (tr->nvn_sslot >= 0 && tr->nvn_sslot < 128) ? g_rs.samp[stg_t][tr->nvn_sslot] : 0;
                        uint32_t sid = (uint32_t)(sh_ >> 20);
                        if (sh_ && sid != 0xfffu) texs = samp_of_id(sid);
                        if (texv) atomic_fetch_add(&g_desc_tex_ok, 1);
                        if (!texs) {
                            if (!sh_ || sid == 0xfffu) atomic_fetch_add(&g_desc_tex_emptysamp, 1);
                            else {
                                static _Atomic uint64_t miss_said;
                                if (atomic_fetch_add(&g_desc_tex_nosamp, 1) < 6 && atomic_fetch_add(&miss_said, 1) < 6)
                                    LOG("【查】采样器槽 %d 的号 %u 在注册表里没有（句柄 0x%llx、注册表 %llu 项）",
                                        tr->nvn_sslot, sid, (unsigned long long)sh_, (unsigned long long)atomic_load(&g_sreg_n));
                            }
                        }
                    }
                }
            }
            if (!texv && tr) desc_tex_probe(st, tr);   // 配不上时取证（前三次）| forensics when unpaired (first three)
            if (!texv) {
                // 用**自己的**计数器：早先和图"拿不到缓冲地址"共用 g_desc_blank，前 8 条被 UBO 的空白占掉，
                // 于是贴图这条诊断在真机上一行都没打出来（查了半天）。
                // Its **own** counter: it used to share g_desc_blank with the "buffer address unavailable" case, so
                // the first eight were taken by UBO blanks and this texture diagnostic never printed a single line on
                // the device (which cost a while to figure out).
                if (atomic_fetch_add(&g_desc_tex_blank, 1) < 8) {
                    int stg = st->stage >= 0 && st->stage < 6 ? st->stage : 0;
                    LOG("【黄】binding %u（%s）贴图写空白：res.json 说 cbuf_slot=%d handle=0x%x；该阶段贴图句柄 %llx/%llx/%llx、常量段 %llx/%llx/%llx/%llx",
                        b[i].binding, vk_spv_kind_name(b[i].kind), tr ? tr->cbuf_slot : -999, tr ? (unsigned)tr->handle : 0u,
                        (unsigned long long)g_rs.tex[stg][0], (unsigned long long)g_rs.tex[stg][1], (unsigned long long)g_rs.tex[stg][2],
                        (unsigned long long)g_rs.ubo[stg][0].addr, (unsigned long long)g_rs.ubo[stg][1].addr,
                        (unsigned long long)g_rs.ubo[stg][2].addr, (unsigned long long)g_rs.ubo[stg][3].addr);
                }
                ii[nw] = (VkDescriptorImageInfo){ vk_pipe_dummy_sampler(), vk_pipe_dummy_view(),
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            } else {
                // 采样器来自**同一个槽**的采样器句柄高位（>> 20）；取不到才退回空白并计数——贴图照常绑上。
                // 注：早先这里写着"res.json 的 samplers 数组"是**错的**，res.json 没有那个数组（已核实）。
                // The sampler comes from the **same slot**'s sampler handle high bits (>> 20); only when that fails
                // does it fall back to a blank (counted), with the texture still bound.
                // Note: an earlier comment here mentioned "res.json's samplers array", which is **wrong** -- res.json
                // has no such array (verified).
                ii[nw] = (VkDescriptorImageInfo){ texs ? texs : vk_pipe_dummy_sampler(), texv,
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                // 计数已在解析那一段做过（空槽 / 取不到号分开计），这里不再重复加 | counted in the resolution block
                // above (empty versus missing, separately); not counted again here
            }
            w.descriptorType = b[i].type;
            w.pImageInfo = &ii[nw];
            nw++;
        } else {
            continue;   // VK_SPV_OTHER 之类：布局里没这条就走不到这里 | kinds such as VK_SPV_OTHER never reach here for a binding that is in the layout
        }
        wr[nw - 1] = w;   // nw 已经加过，所以回填的是刚写的那条 | nw was already incremented, so this fills in the entry just written
    }
    if (nw) vkUpdateDescriptorSets(vk_dev(), (uint32_t)nw, wr, 0, NULL);
    return nd;
}

static void pass_begin(void) {
    if (g_rs.open || g_cur_slot < 0) return;
    if (!vk_have_dyn_render()) return;
    if (!g_rs.ncolor && !g_rs.depth) return;
    if (g_rs.ncolor && !g_rs.color[0] && !g_rs.depth) return;
    VkCommandBuffer cb = g_inf[g_cur_slot].cb;
    VkRenderingAttachmentInfo att[8];
    int natt = 0;
    uint32_t att_w = 0, att_h = 0;   // 附件的有效尺寸（限了 mip 的视图要右移）| the attachment's effective size (a mip-limited view shifts it right)
    for (int i = 0; i < g_rs.ncolor && i < 8; i++) {
        const tex_t *t = g_rs.color[i];
        if (!t) continue;
        // 渲染目标用 SET_RT 载荷里带来的那个 view（影子状态存的 cview）：限定了 mip 的 view 才是游戏真正要画
        // 的那一层，拿底视图当附件会画到第 0 级上去。附件尺寸也随之不同，见下面按 base_level 右移那两行。
        // The render target uses the view carried in the SET_RT payload (the cview kept in the shadow state): a view
        // that limits the mip range is the level the game really draws into, and using the base view as the attachment
        // would draw into level 0. The attachment size shifts along with it (see the two base_level lines below).
        VkImageView iv = vk_tex_view(t, g_rs.has_cview[i] ? &g_rs.cview[i] : NULL);
        if (!iv) {
            // 不再静默：附件建不出图像这件事曾经让"画面到不了出图纹理"藏了很久（截图才把它照出来），
            // 而纹理层只会报一次原因，所以这里也报一次"哪个目标被跳过"。
            // No longer silent: an attachment that cannot produce an image once hid "the picture never reaches the
            // present texture" for a long time (the screenshot is what exposed it), and the texture layer reports the
            // reason only once, so this says which target was skipped.
            static int said;
            if (!said++) LOG("【黄】颜色附件建不出图像（序号 %llu）：这个目标本次跳过", (unsigned long long)t->serial);
            continue;
        }
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
        // 限了 mip/层的视图，附件尺寸不是底图尺寸：有效尺寸 = 底图 >> base_level（层范围不影响二维尺寸）
        // A view that limits mip levels or layers does not have the base image's size: the effective size is the
        // base shifted right by base_level (the layer range does not affect the 2D size)
        uint32_t al = (g_rs.has_cview[i] && g_rs.cview[i].set_levels && g_rs.cview[i].base_level > 0)
                      ? (uint32_t)g_rs.cview[i].base_level : 0;
        uint32_t aw = t->d.width >> (al > 31 ? 0 : al), ah = t->d.height >> (al > 31 ? 0 : al);
        if (!aw) aw = 1;
        if (!ah) ah = 1;
        if (!att_w || aw < att_w) att_w = aw;
        if (!att_h || ah < att_h) att_h = ah;
    }
    if (!natt && !g_rs.depth) return;
    VkRenderingAttachmentInfo datt = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    int has_d = 0;
    if (g_rs.depth) {
        VkImageView dv = vk_tex_view(g_rs.depth, g_rs.has_dview ? &g_rs.dview : NULL);
        if (!dv) {
            static int said;
            if (!said++) LOG("【黄】深度附件建不出图像（序号 %llu）：这次绘制没有深度附件", (unsigned long long)g_rs.depth->serial);
        }
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
    // 渲染区**必须**落在附件里（Vulkan 的要求）。限了 mip 的附件比底图小，而游戏设的视口是按它要画的那一级给的
    // ——两者本该一致；不一致就在这里钳住并报一次，否则会变成静默的 GPU 错误（真机没有校验层，看不见）。
    // The render area **must** lie inside the attachment (a Vulkan requirement). A mip-limited attachment is smaller
    // than the base image while the viewport the game set is for the level it means to draw -- the two should agree,
    // and any disagreement is clamped and reported once here rather than becoming a silent GPU error (the device has
    // no validation layers to show it).
    if (att_w && w > att_w) { static int said; if (!said++) LOG("【黄】渲染区宽 %u 超出附件 %u（限了 mip 的视图）：已按附件钳住", w, att_w); w = att_w; }
    if (att_h && h > att_h) { static int said; if (!said++) LOG("【黄】渲染区高 %u 超出附件 %u（限了 mip 的视图）：已按附件钳住", h, att_h); h = att_h; }
    ri.renderArea.extent = (VkExtent2D){ w, h };
    if (g_rs.nvp) vkCmdSetViewport(cb, 0, (uint32_t)g_rs.nvp, g_rs.vp);
    if (g_rs.nsc) vkCmdSetScissor(cb, 0, (uint32_t)g_rs.nsc, g_rs.sc);
    dm_vkCmdBeginRendering(cb, &ri);
    gpu_pass_ts(g_cur_slot, cb, 0);   // 通道真的开成了，写起始时间戳 | the pass really opened, so write the start timestamp   // 桩库不导出，走 vkGetDeviceProcAddr 取的指针（见 vk_device.h）| not exported by the stub library, so it goes through the pointer fetched with vkGetDeviceProcAddr (see vk_device.h)
    g_rs.open = 1;
    atomic_fetch_add(&g_pass_n, 1);
}

// ========== 影子状态 → 图形管线状态（M2-6d-3 的解释层）==========
// ========== Shadow state -> graphics pipeline state (the interpretation layer of M2-6d-3) ==========
// 与苹果侧的 pso_key_fill 同一套语义：位域对象 → Vulkan 枚举。所有"按面/按目标分存"的状态在这里摊平。
// The same semantics as Apple's pso_key_fill: packed bit-field objects -> Vulkan enums. Every per-face / per-target
// state is flattened here.
static VkBlendFactor vk_bfac(uint32_t f) {
    switch (f) {
        case 0x01: return VK_BLEND_FACTOR_ZERO; case 0x02: return VK_BLEND_FACTOR_ONE;
        case 0x03: return VK_BLEND_FACTOR_SRC_COLOR; case 0x04: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x05: return VK_BLEND_FACTOR_SRC_ALPHA; case 0x06: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x07: return VK_BLEND_FACTOR_DST_ALPHA; case 0x08: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x09: return VK_BLEND_FACTOR_DST_COLOR; case 0x0A: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x0B: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR; case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA; case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}
// NVN 的混合运算 / 比较运算 / 模板运算编号都从 1 起且顺序与 Vulkan 一致，所以减一即可（苹果侧同样这么转）
// NVN's blend / compare / stencil operation numbers start at 1 in the same order as Vulkan, so subtracting one is
// all it takes (Apple converts the same way)
static VkBlendOp vk_bop(uint32_t e) {
    switch (e) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
                 case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; }
    return VK_BLEND_OP_ADD;
}
static VkCompareOp vk_cmp(uint32_t f) {
    if (f < 1 || f > 8) return VK_COMPARE_OP_ALWAYS;
    return (VkCompareOp)(f - 1);
}
static VkStencilOp vk_sop(uint32_t o) {
    if (o < 1 || o > 8) return VK_STENCIL_OP_KEEP;
    return (VkStencilOp)(o - 1);
}
static VkSampleCountFlagBits vk_samples(int n) {
    switch (n) { case 2: return VK_SAMPLE_COUNT_2_BIT; case 4: return VK_SAMPLE_COUNT_4_BIT;
                 case 8: return VK_SAMPLE_COUNT_8_BIT; case 16: return VK_SAMPLE_COUNT_16_BIT; }
    return VK_SAMPLE_COUNT_1_BIT;
}
// 深度/模板附件的格式拆分：组合格式两边都给，纯深度只给深度，S8 只给模板
// Splitting the depth/stencil attachment format: a combined format goes to both, depth-only to depth, S8 to stencil
static void vk_ds_fmt(VkFormat f, VkFormat *df, VkFormat *sf) {
    switch (f) {
        case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
            *df = f; *sf = f; return;
        case VK_FORMAT_S8_UINT: *df = VK_FORMAT_UNDEFINED; *sf = f; return;
        default: *df = f; *sf = VK_FORMAT_UNDEFINED; return;
    }
}

static void gfx_state_fill(vk_gfx_state *out, int topo_prim) {
    memset(out, 0, sizeof *out);
    out->topo = vk_topo_of(topo_prim);
    out->poly = VK_POLYGON_MODE_FILL;
    out->front = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    // 颜色目标：格式按顺序取到第一个空缺为止（NVN 的目标是从 0 开始的连续编号，跳号会错位）
    // Color targets: formats are taken in order until the first gap (NVN numbers its targets contiguously from 0,
    // so skipping one would misalign everything)
    for (int i = 0; i < g_rs.ncolor && i < 8 && g_rs.color[i]; i++) {
        VkFormat f = VK_FORMAT_UNDEFINED;
        if (!vk_fmt_to_vk(g_rs.color[i]->d.format, &f) || !f) {
            static int said;
            if (!said++) LOG("【黄】颜色目标格式 %d 没有 Vulkan 对应物：这条流按不出图处理", g_rs.color[i]->d.format);
            out->color_count = 0;
            break;
        }
        out->color[out->color_count++] = f;
        if (g_rs.color[i]->d.samples > 1) out->samples = (uint32_t)vk_samples(g_rs.color[i]->d.samples);
    }
    if (g_rs.depth) {
        VkFormat f = VK_FORMAT_UNDEFINED;
        if (vk_fmt_to_vk(g_rs.depth->d.format, &f) && f) vk_ds_fmt(f, &out->depth_fmt, &out->stencil_fmt);
        if (g_rs.depth->d.samples > 1) out->samples = (uint32_t)vk_samples(g_rs.depth->d.samples);
    }
    if (!out->samples) out->samples = (uint32_t)VK_SAMPLE_COUNT_1_BIT;

    // 颜色状态：写掩码、逐目标混合、alpha-to-coverage
    color_st co;
    memcpy(&co, g_rs.st_bytes[1], sizeof co);
    chmask_st cm;
    memcpy(&cm, g_rs.st_bytes[2], sizeof cm);
    ms_st ms;
    memcpy(&ms, g_rs.st_bytes[3], sizeof ms);
    for (uint32_t i = 0; i < out->color_count; i++) {
        out->write_mask[i] = (cm.mask >> (4 * i)) & 0xf;
        out->blend_en[i] = g_rs.blend_have & (1u << i) ? 1 : 0;
        out->src_rgb[i] = vk_bfac(g_rs.blend[i].src_rgb); out->dst_rgb[i] = vk_bfac(g_rs.blend[i].dst_rgb);
        out->src_a[i] = vk_bfac(g_rs.blend[i].src_a); out->dst_a[i] = vk_bfac(g_rs.blend[i].dst_a);
        out->eq_rgb[i] = vk_bop(g_rs.blend[i].eq_rgb); out->eq_a[i] = vk_bop(g_rs.blend[i].eq_a);
    }
    out->a2c = ms.a2c;

    // 深度模板
    ds_st ds;
    memcpy(&ds, g_rs.st_bytes[5], sizeof ds);
    out->depth_test = (uint8_t)ds.dt; out->depth_write = (uint8_t)ds.dw; out->depth_op = vk_cmp(ds.df);
    out->stencil_en = (uint8_t)ds.st;
    out->stencil_func[0] = vk_cmp(ds.sf_f); out->stencil_func[1] = vk_cmp(ds.sf_b);
    out->stencil_vmask[0] = g_rs.st_vmask[0]; out->stencil_vmask[1] = g_rs.st_vmask[1];
    out->stencil_wmask[0] = g_rs.st_wmask[0]; out->stencil_wmask[1] = g_rs.st_wmask[1];
    out->stencil_fail[0] = vk_sop(ds.fail_f); out->stencil_zfail[0] = vk_sop(ds.zfail_f); out->stencil_zpass[0] = vk_sop(ds.zpass_f);
    out->stencil_fail[1] = vk_sop(ds.fail_b); out->stencil_zfail[1] = vk_sop(ds.zfail_b); out->stencil_zpass[1] = vk_sop(ds.zpass_b);

    // 光栅：剔除面与 NVNface 同位（NONE/FRONT/BACK/FRONT_AND_BACK = 0/1/2/3），前端面要翻过来（NVN 的 CW=0 与 Vulkan 相反）
    // Rasterization: the cull face shares NVNface's bit values (NONE/FRONT/BACK/FRONT_AND_BACK = 0/1/2/3) while the
    // front face must be flipped (NVN's CW=0 is the opposite of Vulkan's)
    poly_st po;
    memcpy(&po, g_rs.st_bytes[4], sizeof po);
    out->cull = (VkCullModeFlags)(po.cull & 3);
    out->front = po.front ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    out->poly = (VkPolygonMode)(po.mode <= 2 ? po.mode : 2);
    out->bias_en = po.offset_en ? 1 : 0;
    out->bias_factor = g_rs.po_factor; out->bias_units = g_rs.po_units; out->bias_clamp = g_rs.po_clamp;

    // 顶点输入：影子里的属性按 NVN 属性号存，管线按着色器声明的 Location 取（两者同一套编号，见第 9 步）
    // Vertex input: the shadow stores attributes by NVN attribute number and the pipeline looks them up by the
    // Location a shader declares (the same numbering, see step 9)
    for (int k = 0; k < 16; k++) {
        if (!(g_rs.va_have & (1u << k))) continue;
        out->attr[k].valid = 1;
        out->attr[k].nvn_fmt = g_rs.va[k].fmt;
        out->attr[k].offset = g_rs.va[k].offset;
        out->attr[k].stream = g_rs.va[k].stream;
    }
    for (int k = 0; k < 16; k++) {
        if (!(g_rs.stream_have & (1u << k))) continue;
        out->stream[k].stride = g_rs.stream[k].stride;
        out->stream[k].divisor = g_rs.stream[k].divisor;
    }
}

// 命令流解释器：与 Metal 侧同形（OP_JUMP / OP_CALL 构成的小程序）。CPU 可见副作用按语义执行，
// 渲染类 op 目前只计数——每次提交报一次分布，按实测的频次决定实现次序。
// The command stream interpreter, shaped like the Metal one (a small program of OP_JUMP / OP_CALL). CPU-visible
// side effects are executed per their semantics, while render-side ops are only counted for now -- the
// distribution is reported per submission and the measured frequency decides the implementation order.
void mtl_exec(const uint8_t *p) {
    if (!p) return;
    // 第一次进解释器时把 NVN 的默认状态铺上（与苹果侧 mtl_exec 开头那句 state_reset() 同一个位置、同一个意思）。
    // 静态守卫同时挡住 OP_CALL 的递归。
    // Lay down NVN's default state on the first entry into the interpreter (the same place and meaning as the
    // state_reset() at the head of Apple's mtl_exec). The static guard also covers the recursion of OP_CALL.
    static int inited;
    if (!inited) { inited = 1; state_defaults(); }
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.op < OP_COUNT_) atomic_fetch_add(&g_opcount[h.op], 1);
        g_opring[atomic_fetch_add(&g_opring_i, 1) & 31] = (uint8_t)h.op;   // 流水：出问题时把"前面发生了什么"打出来 | the ring: print "what came before" when a draw looks wrong
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
            for (int i = 0; i < 8; i++) {
                g_rs.color[i] = i < g_rs.ncolor ? r->color[i] : NULL;
                if (g_rs.color[i]) rt_use_bump(g_rs.color[i]->serial);   // 谁被当渲染目标用过（见 rt_use_bump）| which textures serve as render targets (see rt_use_bump)
                // 视图一并存下来（见 rstate_t 里那段说明）：渲染目标那一层常常是"底图的某个 mip"
                g_rs.cview[i] = r->cview[i];
                g_rs.has_cview[i] = r->has_cview[i];
            }
            g_rs.depth = r->depth;
            g_rs.dview = r->dview;
            g_rs.has_dview = r->has_dview;
            // 记下最近一次 SET_RT 的目标序号：绘制报"目标 0 个"时，要能一眼看出"到底有没有报过 SET_RT"
            // Record the last SET_RT's target serials: when a draw reports "0 targets" it must be possible to see at a glance
            // whether a SET_RT was ever reported
            g_last_rt_n = (int)g_rs.ncolor;
            g_last_rt_seen = 1;
            for (int i = 0; i < 8; i++) g_last_rt[i] = g_rs.color[i] ? g_rs.color[i]->serial : 0;
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
                // 近远只来自 SetDepthRange（默认 [0,1]，见 state_defaults）：这条载荷的 v[4]/v[5] 从来没被
                // 录进来（NVN 的 SetViewports 只带四个 float），读它们只会得到 [0,0]。
                // near/far come only from SetDepthRange (defaulting to [0,1], see state_defaults): v[4]/v[5] in
                // this payload are never recorded (NVN's SetViewports carries four floats), and reading them only
                // yields [0,0].
                g_rs.vp[i].minDepth = g_rs.drange[0];
                g_rs.vp[i].maxDepth = g_rs.drange[1];
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
            if (k == 0) {   // 混合：目标号在对象内部（blend_st.target），按它分存 | blend: the target number lives inside the object (blend_st.target), so store by it
                blend_st bl;
                memcpy(&bl, b->bytes, sizeof bl);
                uint32_t tg = bl.target < 8 ? bl.target : 0;
                g_rs.blend[tg] = bl;
                g_rs.blend_have |= (uint8_t)(1u << tg);
            }
            atomic_fetch_add(&g_shadow_n, 1);
            break; }
        case OP_STENCIL: {
            const p_stencil *s = (const p_stencil *)a;
            for (int f = 0; f < 2; f++) if (s->face & (1 << f)) {   // NVN_FACE_FRONT = 1、BACK = 2 | NVN_FACE_FRONT = 1, BACK = 2
                if (s->which == 0) g_rs.st_vmask[f] = (uint32_t)s->value_mask;
                else if (s->which == 1) g_rs.st_wmask[f] = (uint32_t)s->mask;
                else g_rs.st_ref[f] = (uint32_t)s->ref;
            }
            g_rs.stencil_have = 1;
            atomic_fetch_add(&g_shadow_n, 1);
            break; }
        case OP_POLY_OFFSET: {
            const p_poly_offset *p = (const p_poly_offset *)a;
            g_rs.po_factor = p->factor; g_rs.po_units = p->units; g_rs.po_clamp = p->clamp;
            g_rs.poly_have = 1;
            atomic_fetch_add(&g_shadow_n, 1);
            break; }
        case OP_BIND_VATTR: {
            const p_bind_vattr *b = (const p_bind_vattr *)a;
            for (int32_t i = 0; i < b->n && i < 16; i++) {
                int k = b->first + i;
                if (k < 0 || k >= 16) continue;
                memcpy(&g_rs.va[k], &b->st[i], sizeof g_rs.va[k]);   // 载荷里是原样的 32 位字，按结构体拷 | the payload carries verbatim 32-bit words, copied as the struct
                g_rs.va_have |= (uint16_t)(1u << k);
            }
            atomic_fetch_add(&g_shadow_n, 1);
            break; }
        case OP_BIND_VSTREAM: {
            const p_bind_vstream *b = (const p_bind_vstream *)a;
            for (int32_t i = 0; i < b->n && i < 16; i++) {
                int k = b->first + i;
                if (k < 0 || k >= 16) continue;
                g_rs.stream[k] = b->st[i];
                g_rs.stream_have |= (uint16_t)(1u << k);
            }
            atomic_fetch_add(&g_shadow_n, 1);
            break; }
        case OP_BIND_VBUF: {
            const p_bind_vbuf *b = (const p_bind_vbuf *)a;
            for (int32_t i = 0; i < b->n && i < 16; i++) {
                int k = b->first + i;
                if (k < 0 || k >= 16) continue;
                g_rs.vbuf[k] = b->r[i];
                g_rs.vbuf_have |= (uint16_t)(1u << k);
            }
            atomic_fetch_add(&g_shadow_n, 1);
            break; }
        case OP_BIND_UBO: case OP_BIND_SSBO: {
            const p_bind_buf *b = (const p_bind_buf *)a;
            int stg = b->stage, idx = b->index;
            // 按阶段计数（见 g_ubo_binds 的说明）：写空白的现场要能分清"绑到别的阶段了"和"根本没绑"。
            // 阶段号超出 0..5 的单独计（若真有这种，也是个发现）。
            // Count by stage (see g_ubo_binds): a blank's situation has to distinguish "bound under another stage" from
            // "never bound". Stage numbers outside 0..5 are counted separately (if any turn up, that is a finding too).
            if (h.op == OP_BIND_UBO) {
                if (stg >= 0 && stg < 6) {
                    atomic_fetch_add(&g_ubo_binds[stg], 1);
                    if (!b->addr) atomic_fetch_add(&g_ubo_zero[stg], 1);
                } else atomic_fetch_add(&g_ubo_out, 1);
            } else if (h.op == OP_BIND_SSBO && stg >= 0 && stg < 6) atomic_fetch_add(&g_ssbo_binds[stg], 1);
            if (stg >= 0 && stg < 6 && idx >= 0) {
                if (h.op == OP_BIND_UBO && idx < 16) {
                    g_rs.ubo[stg][idx].addr = b->addr; g_rs.ubo[stg][idx].size = b->size;
                    g_rs.ubo_have[stg] |= (uint16_t)(1u << idx);
                } else if (h.op == OP_BIND_SSBO && idx < 128) {
                    g_rs.ssbo[stg][idx].addr = b->addr; g_rs.ssbo[stg][idx].size = b->size;
                    g_rs.ssbo_have[stg][idx >> 6] |= 1ull << (idx & 63);
                }
            }
            atomic_fetch_add(&g_shadow_n, 1);
            break; }
        case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: {
            const p_bind_handles *b = (const p_bind_handles *)a;
            int stg = b->stage;
            if (stg >= 0 && stg < 6) {
                for (int32_t i = 0; i < b->n && i < 32; i++) {
                    int k = b->first + i;
                    if (k < 0) continue;
                    if (h.op == OP_BIND_IMG) {
                        if (k >= 32) continue;
                        g_rs.img[stg][k] = b->h[i];
                        g_rs.img_have[stg] |= (uint32_t)(1u << k);
                    } else if (k < 128) {
                        if (h.op == OP_BIND_TEX) {
                            g_rs.tex[stg][k] = b->h[i];
                            g_rs.tex_have[stg][k >> 6] |= 1ull << (k & 63);
                        } else {
                            g_rs.samp[stg][k] = b->h[i];
                            g_rs.samp_have[stg][k >> 6] |= 1ull << (k & 63);
                        }
                    }
                }
            }
            atomic_fetch_add(&g_shadow_n, 1);
            break; }
        case OP_SET_TEX_POOL: { const p_set_pool *p = (const p_set_pool *)a; g_rs.texpool = *p->pool; g_rs.have_texpool = 1; atomic_fetch_add(&g_shadow_n, 1); break; }
        case OP_SET_SAMP_POOL: { const p_set_pool *p = (const p_set_pool *)a; g_rs.samppool = *p->pool; g_rs.have_samppool = 1; atomic_fetch_add(&g_shadow_n, 1); break; }

        // ---- 绘制：M2-6d-3 的第一步——把影子状态解释成管线状态并**真建一条管线**，但还不绘制（描述符写入是下一步）。
        //      这样"影子存储 + 状态解释"就能在真机跑游戏时被验证：管线建不建得出来、属性与格式对不对、拓扑是什么。
        // ---- draws: the first half of M2-6d-3 -- interpret the shadow state into pipeline state and really build a
        //      pipeline, but do not draw yet (writing descriptors is the next step). This makes the shadow storage and
        //      the state interpretation verifiable while the game runs on the device: whether the pipeline builds,
        //      whether attributes and formats line up, and which topology it is.
        case OP_DRAW: case OP_DRAW_IDX: case OP_DISPATCH: {
            const p_draw *d = (const p_draw *)a;   // 两种绘制的载荷都以 prim/first 开头，按 p_draw 读拓扑是安全的 | both draw payloads start with prim/first, so reading the topology through p_draw is safe
            uint64_t k = atomic_fetch_add(&g_draw_skip, 1);
            // 静态而不是栈上：mtl_exec 是递归的（OP_CALL），而它的栈是**客体的栈**——宿主在客体栈上展开
            // 每多一个字节都可能把客体线程推过栈底。下面这些都放静态，且"填完立刻用、中间不再进 mtl_exec"。
            // Static rather than on the stack: mtl_exec recurses (OP_CALL) and its stack is **the guest's stack** --
            // every extra byte of host frame can push a guest thread past the bottom of its stack. Everything below
            // is static, and each one is filled and used immediately with no re-entry into mtl_exec in between.
            static vk_gfx_state stv;
            static vk_pipe_bind binds[VK_SPV_MAXB];
            static uint32_t dyn[VK_SPV_MAXB];
            const vk_shader *shs[2] = { NULL, NULL }; int nsh = 0;
            if (g_rs.prog) for (uint32_t i = 0; i < g_rs.prog->nstage && i < 6; i++) {
                const vk_shader *sh = vk_shader_get(g_rs.prog->sd[i].control);
                if (!sh) continue;
                if (sh->stage == VK_STAGE_VS || sh->stage == VK_STAGE_CS) {
                    if (!shs[0]) { shs[0] = sh; if (!nsh) nsh = 1; }
                } else if (sh->stage == VK_STAGE_PS) { shs[1] = sh; nsh = 2; }
            }
            if (h.op == OP_DISPATCH) {   // 计算派发：图形先通，这一步只计数（计算管线+描述符是同一个套路，随后接）
                if (k < 8) LOG("DISPATCH 第 %llu 次：着色器 %s（计算派发这一步先只计数）",
                               (unsigned long long)(k + 1), shs[0] ? shs[0]->key : "无");
                atomic_fetch_add(&g_shadow_n, 1);
                break;
            }
            gfx_state_fill(&stv, d->prim);
            // 没有渲染目标的绘制：把"前面发生了什么"打出来（只打前三次，够看清形态；打多了会把日志淹掉）
            // A draw with no render targets: print "what came before" (only the first three, enough to see the shape;
            // more would drown the log)
            if (!stv.color_count) {
                uint64_t nc0 = atomic_fetch_add(&g_draw_nc0, 1);
                if (nc0 < 3) {
                    char seq[288]; int so = 0;
                    unsigned wi = atomic_load(&g_opring_i);
                    unsigned span = wi < 16 ? wi : 16;
                    for (unsigned i2 = 0; i2 < span; i2++) {
                        uint8_t op = g_opring[(wi - span + i2) & 31];
                        so += snprintf(seq + so, sizeof seq - so, " %s", op < OP_COUNT_ ? g_opname[op] : "?");
                    }
                    LOG("【查】第 %llu 次绘制没有渲染目标：影子 ncolor=%d、开通道=%d、程序=%s、顶点流掩码 %#x（%d 条）；此前 %u 个 op：%s",
                        (unsigned long long)(k + 1), g_rs.ncolor, g_rs.open ? 1 : 0, g_rs.prog ? "有" : "无",
                        (unsigned)g_rs.stream_have, __builtin_popcount((unsigned)g_rs.stream_have), span, so ? seq : "（无）");
                    if (!g_last_rt_seen) LOG("【查】至今**没有过** SET_RT 类 op");
                    else {
                        char rl[200]; int ro = 0;
                        for (int i2 = 0; i2 < g_last_rt_n && i2 < 8; i2++)
                            ro += snprintf(rl + ro, sizeof rl - ro, " 序号 %llu", (unsigned long long)g_last_rt[i2]);
                        LOG("【查】最近一次 SET_RT：%d 个目标%s", g_last_rt_n, g_last_rt_n ? rl : "（零个）");
                    }
                }
            } else atomic_fetch_add(&g_draw_ncN, 1);
            VkPipeline pl = shs[0] ? vk_pipe_graphics(shs[0], shs[1], &stv) : VK_NULL_HANDLE;
            // 通道：按本文件"渲染状态影子 + 通道机械"一节的讲法，"第一次清除**或绘制**"就该开。先前只有清除会开，
            // 于是 `SET_RT → 屏障 → 绘制` 这类没有清除的流里通道一直是关的，绘制被成片丢掉（日志里的"未下发"）。
            // 开通道的同时要把动态视口/裁剪补一遍：VIEWPORTS / SCISSOR 到达时通道还没开，那一次没往命令缓冲里写。
            // Passes: the machinery described at the head of this file says a pass opens at the first clear **or
            // draw**. Only clears used to open one, so in streams shaped `SET_RT -> barrier -> draws` the pass stayed
            // closed and draws were dropped wholesale (the "not issued" lines in the log). Opening a pass here also
            // has to re-supply the dynamic viewport/scissor: those were never written into the command buffer,
            // because the pass was not open when VIEWPORTS / SCISSOR arrived.
            if (pl && g_cur_slot >= 0 && !g_rs.open && (stv.color_count || g_rs.depth)) {
                pass_begin();
                if (g_rs.open) {
                    if (g_rs.nvp) vkCmdSetViewport(g_inf[g_cur_slot].cb, 0, (uint32_t)g_rs.nvp, g_rs.vp);
                    if (g_rs.nsc) vkCmdSetScissor(g_inf[g_cur_slot].cb, 0, 1, g_rs.sc);
                }
            }
            int wrote = 0;
            if (pl && g_rs.open && g_cur_slot >= 0) {
                VkCommandBuffer cb = g_inf[g_cur_slot].cb;
                // 两个空间都取（set 0 = 顶点、set 1 = 片元），每个绘制固定分配两个集：其中一个没有绑定时
                // 它是个**空布局**（写入侧跳过它，但仍然要绑——管线布局里它有位置）。
                // Both spaces are fetched (set 0 = vertex, set 1 = fragment) and each draw allocates two sets fixed:
                // when one has no bindings it is an **empty layout** (the write side skips it but it is still bound,
                // because the pipeline layout has a slot for it).
                VkDescriptorSetLayout dsl0 = vk_pipe_set_layout_set((const vk_shader *const *)shs, nsh, 0);
                VkDescriptorSetLayout dsl1 = vk_pipe_set_layout_set((const vk_shader *const *)shs, nsh, 1);
                VkPipelineLayout lay = vk_pipe_layout((const vk_shader *const *)shs, nsh);
                VkDescriptorSet sets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
                if (dsl0) sets[0] = desc_alloc(g_cur_slot, dsl0);
                if (dsl1) sets[1] = desc_alloc(g_cur_slot, dsl1);
                int nb = (sets[0] && sets[1]) ? vk_pipe_binds((const vk_shader *const *)shs, nsh, binds, VK_SPV_MAXB) : -1;
                int nd = nb >= 0 ? desc_write(sets, (const vk_shader *const *)shs, nsh, binds, nb, dyn) : -1;
                if (nd >= 0) {
                    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, 2, sets, (uint32_t)nd, dyn);
                    // 顶点流：只绑管线真的用到的那些；其余属性走常量回退，落在 VK_PIPE_DUMMY_BIND 上
                    // Vertex streams: bind only the ones the pipeline really uses; the remaining attributes come
                    // from the constant fallback that lives on VK_PIPE_DUMMY_BIND
                    uint8_t used[16] = { 0 };
                    for (int i2 = 0; i2 < 16; i2++) if (stv.attr[i2].valid) used[stv.attr[i2].stream & 15] = 1;
                    for (int s2 = 0; s2 < 16; s2++) if (used[s2]) {
                        void *bb = NULL; uint64_t bo = 0;
                        if (mtl_addr(g_rs.vbuf[s2].address, g_rs.vbuf[s2].size, &bb, &bo)) {
                            VkBuffer vb = (VkBuffer)bb; VkDeviceSize vo = (VkDeviceSize)bo;
                            vkCmdBindVertexBuffers(cb, (uint32_t)s2, 1, &vb, &vo);
                        }
                    }
                    VkBuffer dum = vk_pipe_dummy_buffer();
                    if (dum) { VkDeviceSize z = 0; vkCmdBindVertexBuffers(cb, VK_PIPE_DUMMY_BIND, 1, &dum, &z); }
                    if (h.op == OP_DRAW_IDX) {
                        const p_draw_idx *di = (const p_draw_idx *)a;
                        void *ib = NULL; uint64_t io = 0;
                        // NVN 的索引类型是 0 = 8 位、1 = 16 位、2 = 32 位（nvn.h 的 NVNindexType）。先前我按
                        // "2/4 字节"猜过一次——真机上是 1，恰好读对了 16 位，但 32 位会被读成 16 位。8 位索引在
                        // Vulkan 核心不支持（要 KHR 扩展），苹果侧同样是报出来不画。
                        // NVN's index type is 0 = 8-bit, 1 = 16-bit, 2 = 32-bit (NVNindexType in nvn.h). I once
                        // guessed it as "2/4 bytes" -- the device says 1, which happened to read correctly as 16-bit,
                        // but 32-bit would have been read as 16-bit. 8-bit indices are not in core Vulkan (they need
                        // a KHR extension) and the Apple side likewise reports them instead of drawing.
                        int it_ok = 1;
                        VkIndexType it = VK_INDEX_TYPE_UINT16;
                        if (di->itype == 2) it = VK_INDEX_TYPE_UINT32;
                        else if (di->itype != 1) {
                            static int said;
                            if (!said++) LOG("【红】索引类型 %d（NVN 的 0=8 位 / 1=16 位 / 2=32 位）这里只支持 16/32 位：这次绘制跳过", di->itype);
                            it_ok = 0;
                        }
                        if (it_ok && mtl_addr(di->ibuf, 4, &ib, &io)) {
                            vkCmdBindIndexBuffer(cb, (VkBuffer)ib, (VkDeviceSize)io, it);
                            vkCmdDrawIndexed(cb, (uint32_t)di->count, di->instances ? (uint32_t)di->instances : 1,
                                             0, di->base_vertex, di->base_instance > 0 ? (uint32_t)di->base_instance : 0);
                            wrote = 1;
                        } else atomic_fetch_add(&g_desc_skip, 1);
                    } else {
                        vkCmdDraw(cb, (uint32_t)d->count, 1, (uint32_t)d->first, 0);
                        wrote = 1;
                    }
                } else atomic_fetch_add(&g_desc_skip, 1);
            } else if (pl) atomic_fetch_add(&g_desc_skip, 1);
            if (wrote) atomic_fetch_add(&g_desc_draws, 1);
            if (k < 8 || (k & 2047) == 2047) {
                uint64_t nlay = 0, nlay2 = 0, ncpso = 0, gpso = 0, nfail = 0;
                vk_pipe_stats(&nlay, &nlay2, &ncpso, &gpso, &nfail);
                LOG("%s 第 %llu 次：顶点 %s 片元 %s → 管线 %s；目标 %u 个、拓扑 %d、属性 %u 条；%s；图形管线累计 %llu 条（失败 %llu）；真下发 %llu 次",
                    g_opname[h.op], (unsigned long long)(k + 1), shs[0] ? shs[0]->key : "无", shs[1] ? shs[1]->key : "无",
                    pl ? "建得成" : "建不出", stv.color_count, (int)stv.topo, shs[0] ? shs[0]->nin : 0,
                    wrote ? "已下发" : "未下发",
                    (unsigned long long)gpso, (unsigned long long)nfail, (unsigned long long)atomic_load(&g_desc_draws));
            }
            break; }

        // ---- 屏障与拷贝：先把通道关掉再做（拷贝不进渲染通道）----
        // ---- barriers and copies: close the pass first (copies do not run inside a render pass) ----
        case OP_BARRIER: pass_end(); break;
        case OP_COPY_T2T: {
            // 贴图 → 贴图拷贝。**画面进"出图纹理"走的就是这条路**：真机上窗口纹理从没被当渲染目标用过，
            // 所以在实现这一步之前它在 Vulkan 侧根本不存在（截图失败的诊断正是这么说的）。
            // Texture-to-texture copy. **This is how the picture reaches the present texture**: on the device the
            // window texture is never used as a render target, so before this step it did not exist on the Vulkan
            // side at all (which is exactly what the failed screenshot's diagnostics said).
            pass_end();
            const p_copy_t2t *c = (const p_copy_t2t *)a;
            if (g_cur_slot < 0 || !vk_tex_copy_t2t(c->src, c->has_sv ? &c->sv : NULL, &c->sr,
                                                   c->dst, c->has_dv ? &c->dv : NULL, &c->dr, g_inf[g_cur_slot].cb))
                atomic_fetch_add(&g_shadow_n, 1);
            break; }
        case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_STRIDE: case OP_CLEAR_TEX:
            pass_end(); atomic_fetch_add(&g_shadow_n, 1); break;

        default: atomic_fetch_add(&g_op_unhandled, 1); break;   // 其余渲染类：先只统计 | the other render-side ops: counted only for now
        }
        p += h.size;
    }
}

void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }        // Metal 用来预测预编译；Vulkan 侧不需要 | Metal uses it to predict precompilation; Vulkan does not need it
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }
