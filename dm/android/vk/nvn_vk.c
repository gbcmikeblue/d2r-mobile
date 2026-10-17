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
#include "vk_ops.h"
#include "vk_tri_game.h"   // N4-h：把"我们自己的三角形"画进游戏的通道里（缺省关的诊断开关）| N4-h: draw our own triangle into the game's pass (off by default)
#include "vk2_pool.h"      // N4：新地基（Vulkan 原生显式实现）的池模块 —— 选路点与真机池账 | N4: the new pool module
#include "vk2_xfer.h"      // N4-b：上传窗口 —— 帧开头把 CPU 脏页搬进后备资源（GPU 侧从此读的是后备）| the upload window
#include "vk2_sub.h"       // N4-c：提交侧（帧批提交 + 有界在飞 + 定向栅栏）—— 回放层不再用旧路的 8 槽与队列空闲等待

// N4-b：暂存环按**提交槽**分段（一段一个槽 ⇒ 在飞几个槽就能同时各载一个上传窗口，N3 量出来的待办）。
// N4-c：**槽=新地基的 `vk2_sub` 槽**（在飞深度 `VK2_INFLIGHT_DEFAULT` = 3，N3 的读数定下来的）⇒ 段数取 8 是
// 一个上界（VK2_BANKS_MAX = 16 够），真正生效的段数是 `vk2_sub_use_banks()` 按槽数分的那个。
// N4-c: the slots are now vk2_sub's slots (in-flight depth 3, settled by N3's readings), so 8 is only an upper bound
// for the bank count -- what takes effect is what vk2_sub_use_banks() splits for the actual slot count.
#define VK2_UPLOAD_BANKS 8
#include "dm.h"
#include <pthread.h>
#include <stdarg.h>
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
        // texel buffer 的两个位是**缓冲区贴图**要的：VkBufferView 要求缓冲在创建时就带这两个用法位之一，
        // 而缓冲区贴图的存储就是池里的一段（见 vk_tex_bufview）——位不能事后补，所以一开始就给。
        // The two texel-buffer bits are for **buffer textures**: a VkBufferView requires the buffer to have been
        // created with one of them, and a buffer texture's storage is a stretch of the pool (see vk_tex_bufview).
        // The bits cannot be added later, so they go in from the start.
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
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

// ---------------------------------------------------------------------------------------------------------------
// N4-a：新地基（`DM_GFX2=1`）的**选路点与真机池账** —— 缺省关，关着时这一段一次都不执行、旧路一行不变。
//
// 新地基的第一块接管（§9.7 表的 `mtl_pool_add / mtl_pool_del / mtl_addr`）这一步**只做登记与记账**：
// 池内存照旧交给旧路（零拷贝那条路一行不改），新模块**并行**登记一份「影子 + 分块后备资源」，把真机上
// 「游戏实际开了哪些池、各多少字节、**是不是本进程可读写的普通内存**」量出来 —— 后者是新地基的**唯一前提**
// （旧路的前提是"落在我们铺过的换轨区间里"），计数里的 `no_region` 必须为 0 才说明这条前提在真机游戏里成立。
//
// 两件刻意的事：① **GPU 侧一律仍走旧路**（`mtl_addr` 这一步不接管）⇒ 本轮不可能改变画面/崩溃率，
// 读数只回答"账"；② 后备资源由 `DM_GFX2_BACKING_MB` 限（**0 = 一块都不建**，先只量账 —— 池预算按
// 后备资源字节数算，见 ANDROID_PLAN §八·N1，先把"要多少"问清楚再决定"给不给"）。
// N4-a: the route switch plus the real-machine pool ledger, off by default. It registers a shadow pool and its
// chunked backing in parallel with the old road (which keeps drawing), so the reading is the ledger itself: how
// many pools the game opens, how big they are, and whether each one is ordinary readable/writable memory (the new
// foundation's single precondition). The GPU side still goes through the old road, so this step cannot change
// behaviour; DM_GFX2_BACKING_MB caps the backing (0 = census only) because the pool budget is counted in backing
// bytes and "how much is needed" has to be measured before "whether to give it".
static int      g_vk2_up;              // 新地基池模块起来了吗 | the new pool module is up
static int      g_vk2_gpu;             // **N4-b：GPU 侧接管了吗**（`mtl_addr` 回后备资源 + 帧开头上传）| GPU side taken over
static int      g_vk2_sub_ok;          // **N4-c：提交侧接管了吗**（`vk2_sub` 起来了、钩子挂上了）| submission side taken over
static uint64_t g_vk2_banks;           // 实际分了几个环段（= 提交槽数）| banks the ring was actually split into
static uint64_t g_vk2_adds;            // 经新地基登记过的池数（含旧路会拒的那些）| pools registered via the new module
static uint64_t g_vk2_bytes;           // 它们的总字节 | their total bytes
static uint64_t g_vk2_route_logged;    // 选路行只打一次 | log the route line once
// N4-b 的读数（11.3 的"每帧搬运字节数"就从这几笔来）| N4-b readings (the per-frame upload bytes come from here)
static uint64_t g_vk2_frames;          // 走过"帧开头"的次数 | how often the frame head ran
static uint64_t g_vk2_up_frames;       // 其中真的搬了东西的帧数 | frames that actually moved bytes
static uint64_t g_vk2_up_windows;      // 上传窗口数（一段一帧算一个）| upload windows
static uint64_t g_vk2_up_bytes;        // 累计搬上去的字节 | cumulative bytes uploaded
static uint64_t g_vk2_up_max;          // 单帧最多搬了多少 | the most bytes in one frame
static uint64_t g_vk2_touch_hit;       // 触碰命中池的次数（标脏成功）| touches that hit a pool
static uint64_t g_vk2_touch_miss;      // 触碰没落在池里的次数（正常情形，只计数）| touches outside every pool (normal)
static uint64_t g_vk2_gpu_writes;      // GPU 直写后备（UPDATE_UB / COPY_B2B 的目标）的标脏次数 | GPU-direct writes marked
static uint64_t g_vk2_synced_pools;    // 上次初始同步时已登记几个池（多出来的池要再同步一次）| pools covered so far
// N4-c 的读数：往返型（GPU→池）回读、半页竞态、提交侧接管（G4 的第三条与 11.3 缺的三个字段都从这里读）
// N4-c readings: round-trip readbacks, half-page hazards, and the submission side (G4's third criterion).
static uint64_t g_vk2_rb_calls;        // 槽回收时回读的次数 | readbacks in the reclaim hook
static uint64_t g_vk2_rb_bytes;        // 回读的字节 | bytes read back
static uint64_t g_vk2_rb_us;           // 回读花的累计时间 | cumulative readback time
static uint64_t g_vk2_rb_max_us;       // 单次最长（µs）| longest single readback
static uint64_t g_vk2_rb_hook;         // 槽回收钩子被调用的次数 | reclaim-hook calls
static uint64_t g_vk2_rb_deferred;     // 清 GPU 位被延后的段数（别的在飞提交还写同一页）| settle deferred to a later slot
static uint64_t g_vk2_rb_ranges;       // 一次提交里记下的 GPU 直写区间数（累计）| GPU-written intervals recorded
static uint64_t g_vk2_hazard;          // 半页竞态**化解**次数（绑定点上先回读整页）| half-page hazards resolved
static uint64_t g_vk2_hazard_bytes;    // 为化解而回读的字节 | bytes read back to resolve them
static uint64_t g_vk2_begin_fail;      // 取不到提交槽的次数（超时如实报失败）| times no submission slot could be taken
static uint64_t g_vk2_idle_skipped;    // **N4-c：本该等队列空闲、但被定向栅栏/顺序替代掉的次数** | queue-idle waits avoided
// "一帧"的三个新基线字段（11.3）：每帧脏 run 数（上传侧由 vk2_xfer 记账）、每帧提交次数、槽等待率。
// The three new baseline fields (11.3): dirty runs per frame, submissions per frame, and the slot wait rate.
static uint64_t g_vk2_subs;

// N4-c：槽回收钩子（定义在提交那一节：它要用 g_inf[] 的 per-slot 状态）| N4-c: the reclaim hook (defined with submission)
static void vk2_reap_hook(int slot, uint64_t frame, VkFence fence, void *ud);

static void vk2_boot(void) {
    if (g_vk2_up) return;
    if (!vk2_gfx2_enabled()) return;                       // 缺省关：一次都不进来
    VkPhysicalDevice phys = vk_phys();
    if (vk_dev() == VK_NULL_HANDLE || phys == VK_NULL_HANDLE) {
        LOG("【红】N4：设备还没起来，新地基先不登记（DM_GFX2=1）");
        return;
    }
    vk2_dev d;
    memset(&d, 0, sizeof d);
    d.dev = vk_dev(); d.phys = phys; d.q = vk_queue(); d.qfi = vk_queue_family();
    const VkPhysicalDeviceMemoryProperties *mp = vk_mem_props();
    if (mp) d.mp = *mp;
    VkPhysicalDeviceProperties2 p2;
    memset(&p2, 0, sizeof p2);
    d.m3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &d.m3;
    vkGetPhysicalDeviceProperties2(phys, &p2);
    d.props = p2.properties;
    if (!vk2_pool_init(&d, 0)) {
        LOG("【红】N4：新地基池模块起不来（DM_GFX2=1 但 vk2_pool_init 失败）⇒ 后面的池账不会有");
        return;
    }
    // 后备预算：未设 = 不限（与 N1 起的语义一致）；0 = 只登记不建后备（N4-a 的"池账"模式）
    const char *s = getenv("DM_GFX2_BACKING_MB");
    uint64_t mb = (s && *s) ? strtoull(s, NULL, 0) : UINT64_MAX;
    vk2_pool_set_backing_budget(mb == UINT64_MAX ? UINT64_MAX : mb * 1024ull * 1024ull);
    // N4-b：上传窗口（暂存环按提交槽分段）。**它起不来就不接管 GPU 侧** —— 宁可整块退回旧路的零拷贝，
    // 也不要"一半走后备、一半走池缓冲"这种半接管（那会读到未定义的字节）。
    // N4-b: the upload window (ring split per submission slot). If it cannot come up, the GPU side is **not** taken
    // over at all -- a half-takeover would read undefined bytes.
    // 缺省 256 MB = 8 段 × 32 MB（N0 的"暂存环 16 MiB 起步"是**单环**时代的口径；N4-b 的真机轮量到
    // "一帧要搬的量超过每段 16 MB" ⇒ 每段给 32 MB，仍可用 DM_GFX2_RING_MB 逐档试）
    const char *rs = getenv("DM_GFX2_RING_MB");
    uint64_t rmb = (rs && *rs) ? strtoull(rs, NULL, 0) : 256;
    if (!rmb) rmb = 128;
    if (!vk2_xfer_init(&d, rmb * 1024ull * 1024ull, 0)) {
        LOG("【红】N4：上传窗口起不来（DM_GFX2=1 但没有搬运窗口）⇒ **GPU 侧不接管**（整块退回旧路的零拷贝）");
        vk2_pool_fini();
        return;
    }
    int banks = vk2_xfer_set_banks(VK2_UPLOAD_BANKS);
    if (banks != VK2_UPLOAD_BANKS)
        LOG("【黄】N4：暂存环只分到 %d 段（要 %d 段）⇒ 多出来的槽会共用第 0 段",
            banks, (int)VK2_UPLOAD_BANKS);
    // ---- N4-c：提交侧（§9.6 的 `vk2_sub_*` 顶替旧路的 8 槽 + 队列锁 + 全部 `vkQueueWaitIdle`）----
    // 三件事必须一起挂上，缺一个就退回旧路（宁可整块退回，也不要"一半新一半旧"的提交）；
    // 起不来时 g_vk2_sub_ok 保持 0 ⇒ 提交走旧路（GPU 侧的内存接管仍然有效）。
    // N4-c: the submission side. All three pieces are installed together, or the submission stays on the old road
    // (a half-taken-over submission path is worse than either road); the GPU-side memory takeover stays either way.
    if (vk2_sub_init(&d, VK2_INFLIGHT_DEFAULT)) {
        vk2_sub_set_reap_hook(vk2_reap_hook, NULL);               // 槽回收：副作用 + GPU→池回读 + 描述符池
        vk2_sub_set_queue_lock(vk_queue_lock, vk_queue_unlock);   // 与前端上屏/回读共用同一条队列
        vk2_xfer_set_queue_lock(vk_queue_lock, vk_queue_unlock);  // 回读拷贝也在同一条队列上
        int nb = vk2_sub_use_banks();
        if (nb > 0) {
            g_vk2_banks = (uint64_t)nb;
            g_vk2_sub_ok = 1;
        } else {
            LOG("【红】N4-c：环按槽分失败 ⇒ **提交侧仍走旧路**（在飞深度与环段数不匹配会把上传串行化）");
        }
    } else {
        LOG("【红】N4-c：提交模块起不来 ⇒ **提交侧仍走旧路**（GPU 侧的内存接管不受影响）");
    }
    g_vk2_up = 1;
    g_vk2_gpu = 1;
    vk2_pool_set_gpu_takenover(1);   // 只影响路由行的自证措辞（"只登记" vs "GPU 侧已接管"）
    LOG("N4 选路：%s", vk2_route_line());
    LOG("N4 池账开始：后备预算 = %s；块 %llu MB", 
        mb == UINT64_MAX ? "不限" : (mb ? "见 DM_GFX2_BACKING_MB" : "0（只登记不建后备）"),
        (unsigned long long)(vk2_pool_chunk_bytes() >> 20));
    LOG("N4-b **GPU 侧接管**：暂存环 %llu MB / %d 段（按提交槽分）；`mtl_addr` 起回**后备资源**"
        "（块缓冲 + 块内偏移）；帧开头把 CPU 脏页搬上去",
        (unsigned long long)rmb, banks);
    if (g_vk2_sub_ok)
        LOG("N4-c **提交侧接管**：%d 个提交槽（`vk2_sub_*`，一环一段）、**槽满了只等最老那个槽的定向栅栏**、"
            "`mtl_drain/finish` → 等全部在飞槽的栅栏；槽回收时把这次提交 **GPU 直写的段回读进影子**（往返型）",
            vk2_sub_inflight());
    else
        LOG("【黄】N4-c：提交侧**没接上**（回放层继续用旧路 8 槽 + `vkQueueWaitIdle`）");
}

// N4-c：一次提交里记多少个"GPU 直写过后备"的区间（`COPY_B2B` / `UPDATE_UB` 的目标）。超了就把它们并成
// **一个上界**（从最小到最大），回读时多读一点但语义不变 —— 与"节流而不是丢"同一条纪律。
// N4-c: how many GPU-written ranges one submission records; on overflow they are merged into a single bounding
// range (reading a bit more is fine, silently dropping one is not).
#define VK2_GPUWR_MAX 64
// 定义在提交那一节（要用 inflight_t / g_inf / g_cur_slot）：这里先声明，两个触碰包装就都能用它。
// Defined in the submission section (it needs inflight_t / g_inf / g_cur_slot); declared here for the wrappers.
static inline void vk2_gpuwrite_note(uint64_t a, uint64_t n);

// N4-b：标脏的两个小包装（回放层的绑定点与拷贝点用）。关着时一次都不动；命中/未命中都记账
// —— 未命中是**正常情形**（绑定的地址常常落在池外），所以只计数、不报红。
// N4-b: the two marking wrappers the replay layer's binding and copy sites use. Misses are normal, so they count.
//
// N4-c：半页竞态的**对策**（N2 量出的那条：GPU 写过的页被 CPU 部分写，整页上传会把 GPU 写在那页里、
// CPU 没碰过的字节盖掉）。顺序上"在绑定点先回读该页"做不到 —— 那一刻这次要发生的 GPU 写**还没发生**
// （标记是录制期写的，提交还没发），回读只会把还没被写过的后备读进影子。真正在时序上成立的只有两条：
//   ① **槽回收时**（那次提交的栅栏已经 signal）把它直写过的段**整页**回读进影子 —— 这是最早可行的"先回读该页"；
//   ② **帧开头上传时跳过还带着 GPU 脏的页**（`vk2_xfer.c` 的 `up_skip_gpu`）—— 不拿旧影子盖掉在飞的 GPU 写，
//      那几页保持 CPU 脏，等 ① 把 GPU 的字节合回影子之后再上传。
// 本包装只把"这次触碰撞上了 GPU 脏页"记一笔（`g_vk2_hazard`）—— 它不再是"只报计数"，而是上面两条机制的
// 计数入口；真出现"撞上了、而两条机制都没覆盖到"的情形，读数里的 hazard 与 up_skip_gpu 会一起说明。
// N4-c: the half-page countermeasure. Reading the page back *at the binding point* cannot be correct -- the GPU
// write being announced has not happened yet (the mark is recorded, the submission is not), so a readback there would
// bring an unwritten backing store into the shadow. The two orderings that do hold are (1) at slot reap, with the fence
// already signalled, read the submission's GPU-written ranges back page by page, and (2) at the frame head, skip pages
// that are still GPU-dirty instead of pushing stale shadow bytes over an in-flight GPU write.
static inline void vk2_touch_cpu_(uint64_t a, uint64_t n) {
    if (!g_vk2_gpu) return;
    if (vk2_dirty_any_gpu(a, n)) {
        g_vk2_hazard++;
        if (g_vk2_hazard <= 8)
            LOG("【地基】半页竞态：这次触碰（%#llx+%llu）撞上 GPU 写过的页 ⇒ 靠「回收时整页回读」+"
                "「上传跳过 GPU 脏页」两条对策（累计 %llu 次）", (unsigned long long)a, (unsigned long long)n,
                (unsigned long long)g_vk2_hazard);
    }
    if (vk2_touch_cpu(a, n)) g_vk2_touch_hit++; else g_vk2_touch_miss++;
}
static inline void vk2_touch_gpu_(uint64_t a, uint64_t n) {
    if (!g_vk2_gpu) return;
    if (vk2_touch_gpu(a, n)) g_vk2_gpu_writes++;
    vk2_gpuwrite_note(a, n);   // 记进本槽 —— 槽回收时按它把这一段回读进影子（N4-c 的往返型）
}

// 池账一行（自证：池地址、大小、新地基收没收、以及"要多少/给了多少"）
// One ledger line: the pool address, its size, whether the new module took it, and needed-versus-given bytes.
static void vk2_pool_ledger(const void *mem, uint64_t size, int ok) {
    vk2_pool_stats st;
    vk2_pool_stats_get(&st);
    uint64_t need = g_vk2_bytes;                       // 累计"要多少"（按池字节数算）| cumulative "needed"
    LOG("【地基】池账 #%llu：%p+%llu MB ⇒ 新地基%s；累计 %llu 个池 / %llu MB；"
        "后备已建 %llu MB（跳过 %llu 块 / %llu MB）；no_region=%llu small=%llu pool_fail=%llu dirty_fail=%llu",
        (unsigned long long)g_vk2_adds, mem, (unsigned long long)(size >> 20),
        ok ? "登记成功" : "【红】登记失败", (unsigned long long)g_vk2_adds,
        (unsigned long long)(need >> 20), (unsigned long long)(st.backing_bytes >> 20),
        (unsigned long long)st.chunk_skipped, (unsigned long long)(st.skipped_bytes >> 20),
        (unsigned long long)st.no_region, (unsigned long long)st.small,
        (unsigned long long)st.pool_fail, (unsigned long long)st.dirty_fail);
}

void mtl_pool_add(const void *mem, uint64_t size) {
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE || !mem || !size) return;

    // N4-a：新地基**并行**登记（DM_GFX2=1 才动）。放在旧路的区间判断**之前**：旧路拒掉的池（栈/预留段）
    // 也要进账 —— "新地基收不收得下"与"旧路铺没铺到"是两件事，这条正是要量的区别。
    // N4-a: register in parallel, before the old road's range test, so pools the old road rejects are still
    // counted -- "can the new foundation take it" and "did the overlay cover it" are two different questions.
    if (vk2_gfx2_enabled()) {
        vk2_boot();
        if (g_vk2_up) {
            if (!g_vk2_route_logged) { g_vk2_route_logged = 1; LOG("N4 选路自证：%s", vk2_route_line()); }
            int ok = vk2_pool_add((void *)(uintptr_t)mem, size, "guest");
            g_vk2_adds++; g_vk2_bytes += size;
            // N4-b：新池**整段标 CPU_DIRTY** —— 后备资源在第一次上传之前是未定义的，所以"登记时全脏"是
            // "第一帧就把整池搬上去"的机制（否则第一帧里没被触碰过的部分读到的是垃圾）。
            // N4-b: mark the whole new pool CPU_DIRTY -- the backing store is undefined before its first upload.
            if (ok) vk2_pool_mark_all_cpu(vk2_pool_count() - 1);
            if (g_vk2_adds <= 12 || (g_vk2_adds % 32) == 0 || !ok) vk2_pool_ledger(mem, size, ok);
        }
    }

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
    if (g_vk2_up) vk2_pool_del((void *)(uintptr_t)mem);   // N4-a：新地基那边也拆掉（缺省关时 g_vk2_up = 0）
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
    // N4-b：**GPU 侧接管之后，反查给的是后备资源**（块缓冲 + 块内偏移）—— 池自己那份零拷贝缓冲不再被 GPU 读。
    // 跨块 / 不在池内 / 没有后备都返回 0（`vk2_addr` 已经把这些记了账，前 16 条明细打在日志里）。
    // N4-b: after the takeover the lookup hands back the **backing store** (chunk buffer + in-chunk offset); a
    // cross-chunk or unresolved request returns 0 and has already been counted and logged by vk2_addr.
    if (g_vk2_gpu) {
        VkBuffer b = VK_NULL_HANDLE; uint64_t o = 0;
        if (vk2_addr(addr, len, &b, &o)) {
            if (buf) *buf = (void *)(uintptr_t)b;
            if (off) *off = o;
            return 1;
        }
        return 0;
    }
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
// M3-2w：这两处也走账本 —— 它们是客体线程上"等驱动把队列里的活做完"，而驱动内部正是 1 毫秒一睡的轮询
// （「M3-2v」抓到的宿主链 `nanosleep ← usleep ← …` 就是这个形状）。站点名分开 drain / finish，读转储时不混。
// M3-2w: these two go through the ledger as well -- they are "a guest thread waiting for the driver to finish the queue",
// and the driver polls inside with a 1 ms sleep (exactly the shape of the host chain `nanosleep <- usleep <- ...` that
// "M3-2v" caught). The site names split drain / finish so a dump is not ambiguous.
// N4-c：新地基下这两条不再等队列空闲，而是**等所有在飞槽自己的定向栅栏**（`vk2_sub_wait_all()`，带超时、
// 超时如实报失败）。语义上等价（都在等"我这边的活全做完"），但**不等别人**：前端线程的上屏提交与它无关，
// G4 要的 `vkQueueWaitIdle` 计数 0 也正是靠这一行。旧路照旧（缺省关时一行都不变）。
// N4-c: with the new foundation these wait on every in-flight slot's own fence instead of queue idle -- the same
// meaning ("my work is done") without waiting on the front end's presents, and it is what keeps the queue-idle count
// at zero. The old road is untouched while DM_GFX2 is off.
void mtl_drain(void) {
    if (!vk_dev()) return;
    if (g_vk2_sub_ok) {
        vk_queue_note("客体：等所有在飞槽的栅栏（drain）");
        dm_waitled_enter("mtl_drain：等全部在飞槽的定向栅栏（N4-c，不 vkQueueWaitIdle）");
        if (!vk2_sub_wait_all()) LOG("【红】mtl_drain：等在飞槽超时（如实报失败，不重试）");
        dm_waitled_leave();
        return;
    }
    vk_queue_note("客体：等队列空闲（drain）"); vk_queue_lock();
    dm_waitled_enter("mtl_drain：等队列空闲（客体线程上等驱动）");
    vk_idle_wait("mtl_drain：等队列空闲（客体线程上等驱动）");
    dm_waitled_leave(); vk_queue_unlock();
}
void mtl_finish(void) {
    if (!vk_dev()) return;
    if (g_vk2_sub_ok) {
        vk_queue_note("客体：等所有在飞槽的栅栏（finish）");
        dm_waitled_enter("mtl_finish：等全部在飞槽的定向栅栏（N4-c，不 vkQueueWaitIdle）");
        if (!vk2_sub_wait_all()) LOG("【红】mtl_finish：等在飞槽超时（如实报失败，不重试）");
        dm_waitled_leave();
        return;
    }
    vk_queue_note("客体：等队列空闲（finish）"); vk_queue_lock();
    dm_waitled_enter("mtl_finish：等队列空闲（客体线程上等驱动）");
    vk_idle_wait("mtl_finish：等队列空闲（客体线程上等驱动）");
    dm_waitled_leave(); vk_queue_unlock();
}

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
// N4-c：新地基的槽数（`vk2_sub` 的在飞深度）≤ 我们请求的环段数 ≤ 环能分的上限。旧路的 8 槽只在新地基关着时用。
// N4-c: the new foundation's slot count (vk2_sub's in-flight depth) must fit in the banks we ask for, and those in
// the ring's maximum; the old road's 8 slots are only used while the new foundation is off.
_Static_assert(VK2_INFLIGHT_DEFAULT <= VK2_UPLOAD_BANKS && VK2_UPLOAD_BANKS <= VK2_BANKS_MAX,
               "暂存环段数要 ≥ 新地基的槽数、≤ 环能分的上限（N4-c）");
#define MAX_POST 512
#define STAGE_SIZE (4u << 20)
typedef struct {
    VkFence fence;
    VkCommandBuffer cb;
    VkBuffer stage; void *stage_map; uint64_t stage_off;
    uint64_t value;
    int pending, cur;
    // N4-b：这一槽里录过上传窗口吗（回收时要把**这一段**暂存环交还；环的存活期 ≤ 槽的存活期）
    // N4-b: did this slot record an upload window? The ring bank is handed back when the slot is reaped.
    int vk2_up;
    post_op post[MAX_POST]; int npost;
    // N4-c：**这次提交 GPU 直写过后备的区间**（`COPY_B2B` / `UPDATE_UB` 的目标），槽回收时按这些区间把后备
    // 回读进影子 —— 旧路里 GPU 写的就是池内存本身、CPU 后读得到；新地基下写落在后备资源上，不回读就是永远
    // 读到旧值。**按槽记账**（不是"扫全池的 GPU 脏页"）：同时有别的槽在飞、又标了同一页时，扫全池会把还没被
    // GPU 写到的页提前回读 + 置 CLEAN ⇒ 那次写就丢了（N4-c 设计时正是为了绕开这条）。
    // N4-c: the ranges this submission *directly* let the GPU write, read back into the shadow when its slot is
    // reaped. Tracked per slot on purpose: scanning the whole pool would also pick up pages a still-in-flight slot
    // has marked but not written yet, and cleaning those would lose that write.
    struct { uint64_t addr, len; } gpu_wr[VK2_GPUWR_MAX];
    int ngpu_wr, gpu_wr_over;   // gpu_wr_over：区间表满之后合并进来的次数（合并 = 读一个上界，正确但多读）
} inflight_t;
static inflight_t g_inf[MAX_INFLIGHT];
static uint64_t g_fence;
static int g_cur_slot = -1;   // 正在录制的槽（-1 = 没在录）| the slot being recorded (-1 = not recording)

// N4-c：记一笔"这次提交让 GPU 直写后备的区间"（槽回收时按它回读）。表满 ⇒ 并成一个上界（正确、多读一点）。
// N4-c: record a range this submission lets the GPU write directly (the reclaim hook reads it back). On overflow the
// ranges are merged into one bounding range.
static inline void vk2_gpuwrite_note(uint64_t a, uint64_t n) {
    if (!g_vk2_gpu || g_cur_slot < 0 || !g_vk2_sub_ok) return;
    inflight_t *in = &g_inf[g_cur_slot];
    g_vk2_rb_ranges++;
    for (int k = 0; k < in->ngpu_wr; k++) {   // 与已有区间相邻/重叠 ⇒ 合并（同一个目标反复写是常态）
        uint64_t lo = in->gpu_wr[k].addr, hi = lo + in->gpu_wr[k].len;
        if (a + n >= lo && a <= hi) {
            uint64_t nlo = a < lo ? a : lo, nhi = (a + n) > hi ? a + n : hi;
            in->gpu_wr[k].addr = nlo; in->gpu_wr[k].len = nhi - nlo;
            return;
        }
    }
    if (in->ngpu_wr < VK2_GPUWR_MAX) { in->gpu_wr[in->ngpu_wr].addr = a; in->gpu_wr[in->ngpu_wr].len = n; in->ngpu_wr++; return; }
    uint64_t lo = in->gpu_wr[0].addr, hi = lo + in->gpu_wr[0].len;
    for (int k = 1; k < in->ngpu_wr; k++) {
        uint64_t l2 = in->gpu_wr[k].addr, h2 = l2 + in->gpu_wr[k].len;
        if (l2 < lo) lo = l2;
        if (h2 > hi) hi = h2;
    }
    if (a < lo) lo = a;
    if (a + n > hi) hi = a + n;
    in->gpu_wr[0].addr = lo; in->gpu_wr[0].len = hi - lo;
    in->ngpu_wr = 1;
    in->gpu_wr_over++;
    if (in->gpu_wr_over <= 4)
        LOG("【黄】这次提交的 GPU 直写区间超过 %d 条 ⇒ 并成一个上界（回读会多读 %llu 字节，语义不变）",
            VK2_GPUWR_MAX, (unsigned long long)(hi - lo));
}

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
// N4-e：**顶点/几何侧**的读数（光栅化侧已被排除 —— 深度/模板、剔除、混合、写掩码、视口/裁剪全放开之后
// 游戏那 3.9 万次绘制在"已知色底子"上仍一个像素都没落下，见 `DM_RASTER_BRUTE` 的说明）。
// `g_vb_fail` = "管线要用这条顶点流、但 `mtl_addr` 没反查出缓冲（于是**没绑上**）"的次数；
// 那种绘制在 Vulkan 里就是**不带顶点缓冲的绘制**，一个片元都不会产生，而在这之前**一条记录都没有**。
// N4-e: the **vertex/geometry side** counters (the raster side is excluded -- with depth/stencil, culling,
// blending, write mask and viewport/scissor all opened up, the game's 39k draws still left not one pixel on
// the known-colour base). g_vb_fail counts "the pipeline wants this vertex stream but mtl_addr found no
// buffer", i.e. a draw with that stream unbound -- such a draw cannot produce a single fragment, and until
// now there was not one line about it.
static _Atomic uint64_t g_vb_ok, g_vb_fail, g_vb_none;   // 顶点流：绑上 / 没绑上 / 一条都没绑
static _Atomic uint64_t g_draw_v0, g_draw_i0;            // 顶点数 0 / 索引数 0 的绘制
// 按**顶点流号**分账：只报"没绑上"的总数不够用 —— 管线要的是哪一条流决定后果（位置流没绑 = 这个绘制
// 一个片元都不会产生；颜色流没绑可能只是少了点细节）。位置流一般是第 0 条。
// Per **stream index**: the total only is not enough -- which stream is missing decides the consequence
// (an unbound position stream means the draw cannot produce one fragment; an unbound colour stream may only
// lose detail). The position stream is usually index 0.
static _Atomic uint64_t g_vb_fail_s[16];
static int vb_fail_breakdown(char *buf, size_t cap) {   // 「没绑上（按流）」那一段
    int o = 0;
    for (int s = 0; s < 16 && o < (int)cap - 24; s++) {
        uint64_t v = atomic_load(&g_vb_fail_s[s]);
        if (v) o += snprintf(buf + o, cap - (size_t)o, "%s流%d=%llu", o ? "、" : "", s, (unsigned long long)v);
    }
    return o;
}

// ============================================================================
// N4-f：**顶点数据与常量缓冲的只读采样**（诊断，缺省关 `DM_VTX_PROBE=1`）
// ============================================================================
// 要回答的那句话（N4-e 把范围收到这里）：**"游戏那些绘制的顶点，经变换后到底在不在裁剪体里、有没有 NaN/Inf"**。
// 本轮先问最直接的一半 —— **顶点数据本身**与**位置的变换矩阵（常量缓冲）**：
//   `NaN/Inf 命中` 或 `数量级离谱`（|分量| 不在 1e-3~1e6 这个常见世界/局部坐标区间里）就是"一个片元都不产生"
//   的经典形状；两样都正常，问题就落在着色器输出那一侧。
// **为什么现在才做得了**：新地基的池是**本进程可读写的普通内存**（N1 / N4-a 的读数反复验过），而旧路的池也是
// `mmap` 回来的普通内存 ⇒ **两臂都能在 CPU 侧直接读影子里的字节**，不必问 GPU、也不必改任何行为。
// 纪律：**只读**（不改状态、不标脏、不提交）、**缺省关**、一个判据行采一个样本（一轮约 20 个）；
// 与 N4-d（第一部分）同一条教训 —— 采样行的**位置**要挂在既有判据行上（第 ⑱ 条），否则一轮打不出几行。
// Reading: NaN/Inf > 0 or a wild magnitude in the position stream means the vertex side is the cause of "not one
// fragment"; a healthy range moves the question on to the shader's output.
static _Atomic uint64_t g_vtx_samp, g_vtx_nan, g_vtx_inf, g_vtx_unread;   // 顶点采样：次数 / NaN / Inf / 读不了
static _Atomic uint64_t g_ubo_samp, g_ubo_nan, g_ubo_inf;                 // 常量采样：次数 / NaN / Inf

// 采样开关自证（**缺省关**；`DM_VTX_PROBE=0` 或未设都是关）
static int vtx_probe_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_VTX_PROBE");
        v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        if (v)
            LOG("【查】DM_VTX_PROBE=1：判据行处读一次**影子里的顶点数据与常量缓冲**（只读：不标脏、不提交）"
                "——查 NaN / Inf / 数量级；位置属性取 Location 0（缺则取着色器声明的第一条真的绑了的）");
    }
    return v;
}

// 半精度 → 单精度。**NaN 的位要保住**（这个开关就是来问"有没有 NaN"的，把 NaN 折成 0 等于把判据删掉）。
static float vtx_half(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) & 1u, e = (h >> 10) & 0x1fu, m = h & 0x3ffu, b;
    float v;
    if (e == 31) { b = (s << 31) | 0x7f800000u | (m << 13); memcpy(&v, &b, 4); return v; }
    if (e == 0) { if (!m) return s ? -0.0f : 0.0f; v = (float)m * 5.9604644775390625e-8f; return s ? -v : v; }
    b = (s << 31) | ((e + 112u) << 23) | (m << 13); memcpy(&v, &b, 4); return v;
}
static int vtx_is_nan(float v) { uint32_t b; memcpy(&b, &v, 4); return (b & 0x7f800000u) == 0x7f800000u && (b & 0x007fffffu) != 0; }
static int vtx_is_inf(float v) { uint32_t b; memcpy(&b, &v, 4); return (b & 0x7fffffffu) == 0x7f800000u; }

// 按 NVN 属性格式把指针处的分量解成浮点：1 = 浮点/归一化类（**可做数值判断**）、0 = 整数类或认不出的格式。
// 编号与 vk_pipe.c 那张 `g_attr_fmt` 表同一套：分量数 = (f-1)/12 + 1；组内类型 k = (f-1)%12，其中
// 0 UCharUNORM / 1 CharSNORM / 4 Half / 5 UShortUNORM / 6 ShortSNORM / 9 Float 是**浮点系**，其余是整数系。
// 整数系（uint/sint）不做数值判断：它们在 IEEE 里不会出现 NaN/Inf，报了反而误导（第 ⑥ 条同一类"不许越读"）。
static int vtx_decode(const uint8_t *p, uint32_t f, float out[4], int *ncomp) {
    int n = 4, k = -1;
    if (f >= 0x01 && f <= 0x30) { n = (int)((f - 1) / 12) + 1; k = (int)((f - 1) % 12); }
    for (int i = 0; i < 4; i++) out[i] = 0.0f;
    if (ncomp) *ncomp = n;
    switch (k) {
        case 0: for (int i = 0; i < n; i++) out[i] = (float)p[i] / 255.0f; return 1;
        case 1: for (int i = 0; i < n; i++) { float v = (float)(int8_t)p[i] / 127.0f; out[i] = v < -1.0f ? -1.0f : v; } return 1;
        case 4: for (int i = 0; i < n; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); out[i] = vtx_half(h); } return 1;
        case 5: for (int i = 0; i < n; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); out[i] = (float)h / 65535.0f; } return 1;
        case 6: for (int i = 0; i < n; i++) { int16_t h; memcpy(&h, p + 2 * i, 2); float v = (float)h / 32767.0f; out[i] = v < -1.0f ? -1.0f : v; } return 1;
        case 9: for (int i = 0; i < n; i++) { float v; memcpy(&v, p + 4 * i, 4); out[i] = v; } return 1;
        default: return 0;   // 整数类（uint / sint）：不做数值判断 | integer classes: no numeric verdict
    }
}

// 安全拼接（**不**用 `o += snprintf(...)`：截断时 snprintf 返回"本来要写的长度"，`cap - o` 会下溢）
static void vtx_cat(char *b, size_t cap, int *o, const char *fmt, ...) {
    if (!b || !o || *o < 0 || (size_t)*o >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b + *o, cap - (size_t)*o, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    *o += n;
    if ((size_t)*o >= cap) *o = (int)cap - 1;
}

// 判据行处的一次采样：位置属性的前几个顶点 + 顶点阶段第一个常量槽的前 64 字节。**只读**。
static void vtx_sample(const vk_gfx_state *st, const vk_shader *vs, uint64_t draw_no, int nvert) {
    // ---- ① 位置属性：Location 0（缺则取着色器声明的第一条真的绑了的）----
    int loc = -1;
    if (st->attr[0].valid) loc = 0;
    else if (vs) for (uint32_t i = 0; i < vs->nin; i++) {
        uint32_t l = vs->in[i].loc;
        if (l < 16 && st->attr[l].valid) { loc = (int)l; break; }
    }
    if (loc < 0) {
        atomic_fetch_add(&g_vtx_unread, 1);
        LOG("【顶点采样】DRAW 第 %llu 次：**没有可用的位置属性**（Location 0 既没绑、着色器也没声明别的）：读不了",
            (unsigned long long)draw_no);
        return;
    }
    uint32_t f = st->attr[loc].nvn_fmt, off = st->attr[loc].offset;
    uint32_t sidx = st->attr[loc].stream & 15, stride = st->stream[sidx].stride;
    uint64_t addr = g_rs.vbuf[sidx].address, size = g_rs.vbuf[sidx].size;
    int nv = stride ? 4 : 1;          // 步长 0 = NVN 的"常量流"（每个实例一个元素）⇒ 只读一个
    int ncomp = 0, nan_n = 0, inf_n = 0, nz = 0, decoded = 0;
    float lo = 0.0f, hi = 0.0f;
    char vt[360]; int vo = 0;
    vt[0] = 0;

    void *bb = NULL; uint64_t bo = 0;
    uint64_t need = (uint64_t)off + (uint64_t)(nv - 1) * (uint64_t)stride + 16;
    if (!addr) vtx_cat(vt, sizeof vt, &vo, "**流%d 没绑（`vbuf.address` = 0）**", sidx);
    else if (!mtl_addr(addr, need, &bb, &bo)) vtx_cat(vt, sizeof vt, &vo, "**流%d 地址 %#llx 不在池里（读不了）**", sidx, (unsigned long long)addr);
    else {
        const uint8_t *base = (const uint8_t *)(uintptr_t)vk2_ptr_mask((uintptr_t)addr);
        for (int i = 0; i < nv; i++) {
            float c[4];
            if (!vtx_decode(base + off + (uint64_t)i * stride, f, c, &ncomp)) {
                vtx_cat(vt, sizeof vt, &vo, "（格式 %#x 不是浮点/归一化类：不做数值判断）", f);
                break;
            }
            decoded = 1;
            vtx_cat(vt, sizeof vt, &vo, "%s(%g", i ? " " : "", (double)c[0]);
            for (int j = 1; j < ncomp && j < 4; j++) vtx_cat(vt, sizeof vt, &vo, ",%g", (double)c[j]);
            vtx_cat(vt, sizeof vt, &vo, ")");
            for (int j = 0; j < ncomp && j < 4; j++) {
                if (vtx_is_nan(c[j])) nan_n++;
                else if (vtx_is_inf(c[j])) inf_n++;
                else { float a = c[j] < 0 ? -c[j] : c[j]; if (a > 0) { nz++; if (a > hi) hi = a; if (!lo || a < lo) lo = a; } }
            }
        }
    }
    atomic_fetch_add(&g_vtx_samp, 1);
    atomic_fetch_add(&g_vtx_nan, (uint64_t)nan_n);
    atomic_fetch_add(&g_vtx_inf, (uint64_t)inf_n);

    // ---- ② 变换矩阵：顶点阶段**所有**有地址的常量槽，各读前 64 字节（16 个 float）----
    // **为什么不是"第一个非零槽"就够**（第一轮读数给的更正）：第一轮 A 臂 12 个样本里，`常量[0][0]`
    // 的前 4 个是 `-0.00125,0,0,0`（|最大| 2）—— 那**不像**变换矩阵；同一轮另一条样本里
    // `0.766044,0,-0.642788,0`（|最大| 1055）才是旋转/平移那一类 ⇒ **只查一个槽会漏掉真正在做变换的那一段**
    // （第 ⑩ 条的同类："快照取在哪"直接改变结论）。所以这里改成扫全部槽，只有第一个槽额外报前 4 个值。
    char ut[560]; int uo2 = 0;
    ut[0] = 0;
    int nub = 0, u_nan = 0, u_inf = 0; float u_hi = 0.0f;
    for (int i = 0; i < 16; i++) {
        uint64_t a2 = g_rs.ubo[0][i].addr, s2 = g_rs.ubo[0][i].size;
        if (!a2 || !s2) continue;
        nub++;
        vtx_cat(ut, sizeof ut, &uo2, "%s槽%d:", nub > 1 ? "；" : "", i);
        uint64_t n = s2 < 64 ? s2 : 64;
        int nf = (int)(n / 4);
        void *ub = NULL; uint64_t ub_off = 0;
        if (!nf) { vtx_cat(ut, sizeof ut, &uo2, "小(%lluB)", (unsigned long long)s2); continue; }
        if (!mtl_addr(a2, n, &ub, &ub_off)) { vtx_cat(ut, sizeof ut, &uo2, "不在池里"); continue; }
        const uint8_t *up = (const uint8_t *)(uintptr_t)vk2_ptr_mask((uintptr_t)a2);
        int kn = 0, ki = 0; float kh = 0.0f;
        for (int k2 = 0; k2 < nf; k2++) {
            float v;
            memcpy(&v, up + 4 * k2, 4);
            if (nub == 1 && k2 < 4) vtx_cat(ut, sizeof ut, &uo2, k2 ? ",%g" : "%g", (double)v);
            if (vtx_is_nan(v)) kn++;
            else if (vtx_is_inf(v)) ki++;
            else { float aa = v < 0 ? -v : v; if (aa > kh) kh = aa; }
        }
        vtx_cat(ut, sizeof ut, &uo2, "…(NaN %d/Inf %d/|max| %g)", kn, ki, (double)kh);
        u_nan += kn; u_inf += ki;
        if (kh > u_hi) u_hi = kh;
        atomic_fetch_add(&g_ubo_samp, 1);
        atomic_fetch_add(&g_ubo_nan, (uint64_t)kn);
        atomic_fetch_add(&g_ubo_inf, (uint64_t)ki);
    }
    if (!nub) vtx_cat(ut, sizeof ut, &uo2, "顶点阶段的常量槽：**一个都没绑**");

    LOG("【顶点采样】DRAW 第 %llu 次（%d 个顶点、vs=%s）：位置 属性%d 流%u 步长%u 偏移%u 格式%#x vbuf=%#llx/%llu；%s"
        "；NaN %d、Inf %d、非零 %d%s"
        "｜常量 %d 个槽：%s；合计 NaN %d、Inf %d、|最大| %g",
        (unsigned long long)draw_no, nvert, vs ? vs->key : "无", loc, sidx, stride, off, f,
        (unsigned long long)addr, (unsigned long long)size, vo ? vt : "（空）",
        nan_n, inf_n, nz,
        decoded ? (nz ? "" : "（**全零**）") : "",
        nub, uo2 ? ut : "（空）", u_nan, u_inf, (double)u_hi);
}
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
static void desc_frame_stats(int slot);  // N4-l：描述符池的每帧读数（`DM_DESC_STATS=1`，缺省关）| N4-l per-frame descriptor-pool readings

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
// 缓冲区贴图（texel buffer）两条：**绑上**与"注册项有、但视图建不出来"。分开计是为了让"空白降下来了没有"
// 这件事有确定的读数——它们是同一类绑定的两种结局。
// Two counters for buffer textures (texel buffers): **bound** and "the registry has it but no view could be built".
// They are the two outcomes of one kind of binding, and keeping them apart gives "did the blanks go down" a definite
// reading.
static _Atomic uint64_t g_desc_texel_ok, g_desc_texel_fail;

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
// `level`：读第几级 mip。`w`/`h` 是**底图**尺寸，缓冲与行距按该级算（`>> level`）。
// 为什么要带级：真机上"扫一遍第 0 级全零、查看器读第 1 级却有内容"（「M3-2m」），而扫描原来把级**写死成 0**、
// 缓冲还按底图开——于是"画面在第几级"这个问题从来没被问过。读回那一层本来就支持按级（`vk_tex_grab_ser` 里
// `w = iw >> lv`），缺的只是调用侧把级传下来。
// `level`: which mip to read. `w`/`h` are the **base** size and the buffer and pitch follow the level (`>> level`).
// Why the level matters: on the device a level-0 scan reads all zero while the viewer reads content at level 1
// ("M3-2m"), and the scan used to hardcode level 0 with a base-sized buffer -- so "which level holds the picture" was
// never actually asked. The readback layer already supports a level (`w = iw >> lv` in `vk_tex_grab_ser`); what was
// missing is the call site passing it down.
static int shot_grab_ser_lv(uint64_t serial, unsigned level, uint32_t w, uint32_t h, const char *path, int save_blank) {
    if (!w || !h) { LOG("【红】截图 %s：尺寸 %ux%u", path, w, h); return 0; }
    uint32_t lw = level > 31 ? 1u : (w >> level), lh = level > 31 ? 1u : (h >> level);
    if (!lw) lw = 1;
    if (!lh) lh = 1;
    uint8_t *buf = malloc((size_t)lw * lh * 4);
    if (!buf) { LOG("【红】截图 %s：%ux%u（第 %u 级）的像素缓冲分配不出", path, lw, lh, level); return 0; }
    uint32_t gw = 0, gh = 0;
    int content = 0;   // 这一级到底有没有内容（整片一个值 = 没有）；扫描的汇总与返回码都要它 | whether this level has content at all (a single value throughout means none); the scan's summary and the return code both need it
    int ok = vk_tex_grab_ser(serial, level, buf, (size_t)lw * 4, &gw, &gh);
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
        content = lo != hi;
        if (!path || (lo == hi && !save_blank)) {
            // 整片一个值就是"没有画面"的**数**：扫描时只报不写
            // A single value across the whole picture is "no picture" as a **number**: a scan reports it without writing
            LOG("[vk] 扫描 序号 %llu 第 %u 级 %ux%u：亮度 均值 %.1f、最小 %u、最大 %u%s", (unsigned long long)serial, level, gw, gh, mean, lo, hi,
                path ? "（整片一个值，不落盘）" : "（只统计）");
        } else {
            ok = vk_png_write(path, buf, gw, gh);
            LOG("截图序号 %llu 第 %u 级 %ux%u → %s %s；亮度 均值 %.1f、最小 %u、最大 %u", (unsigned long long)serial, level, gw, gh, path,
                ok ? "成功" : "失败", mean, lo, hi);
        }
    } else {
        LOG("截图序号 %llu 第 %u 级 %ux%u → %s 失败", (unsigned long long)serial, level, lw, lh, path);
        // 读不回来就把"这个尺寸还有哪些已知贴图"列出来（只列一次：逐张截取时后面每张都失败，会刷屏）
        // If the readback fails, list which known textures share this size (once only: when grabbing every texture of
        // a size, the later ones all fail and would flood the log)
        static int dumped;
        if (!dumped++) vk_tex_dump_size(w, h);
    }
    free(buf);
    if (path) dm_shot_written(path, ok);
    // 返回值把"读成功"与"有没有内容"分开（扫描要靠它做汇总）：0 = 失败、1 = 读成功但整片一个值、2 = 读成功且有内容。
    // The return separates "read succeeded" from "has content" (the scan needs it for its summary): 0 = failed,
    // 1 = read fine but a single value throughout, 2 = read fine with content.
    return ok ? (content ? 2 : 1) : 0;
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
    return shot_grab_ser_lv(t->serial, 0, t->d.width, t->d.height, path, 1) != 0;
}

// 扫描：把**所有**已知贴图各读一次、报一行统计，只有"不是整片一个值"的才落盘。与"同尺寸"那条相比，它不预设画面
// 在哪个尺寸上——真机实测 1600×900 那 11 张全是空的（0 或 255），画面可能根本不在这个尺寸。
// A scan: read **every** known texture once and report a statistics line for each, writing a file only for those that
// are not a single value throughout. Unlike the same-size path it does not presuppose which size the picture lives at
// -- on the device all eleven 1600x900 textures are empty (0 or 255), and the picture may not be at that size at all.
static void shot_scan(uint64_t frame, const char *prefix) {
    enum { SCAN_MAX = 192, SCAN_MAXLV = 16 };
    static uint64_t ser[SCAN_MAX];
    static uint32_t ws[SCAN_MAX], hs[SCAN_MAX];
    static uint32_t lvs[SCAN_MAX];
    int n = vk_tex_list(ser, ws, hs, lvs, SCAN_MAX);
    // 只扫"够大"的：画面不可能是一张 16×24 的精灵图，而每读一张都要分配一次缓冲并等一次队列（实测约 0.45 秒），
    // 192 张会把整轮跑满。这个门槛是**语义**上的，不是性能上的取巧。
    // Only "big enough" ones: a picture cannot be a 16x24 sprite, and every read allocates a buffer and waits for the
    // queue (measured at about 0.45 s), so 192 of them would eat the whole run. The threshold is a **semantic** one, not
    // a performance trick.
    int big = 0;
    for (int i = 0; i < n; i++) if (ws[i] >= 256 && hs[i] >= 256) big++;
    LOG("[vk] 扫描（出图第 %llu 次）：已知贴图 %d 张，其中 ≥256×256 的 %d 张，**逐张逐级**读一遍，只有有内容的落盘到 %s",
        (unsigned long long)frame, n, big, prefix ? prefix : "（不落盘）");
    int nread = 0, ncontent = 0;
    for (int i = 0; i < n; i++) {
        if (ws[i] < 256 || hs[i] < 256) continue;
        int nl = lvs[i] ? (int)lvs[i] : 1;
        if (nl > SCAN_MAXLV) nl = SCAN_MAXLV;
        for (int lv = 0; lv < nl; lv++) {
            // 每级用**该级自己的尺寸**（`>> lv`），并各自过"够大"这条线：一级 800×450 是画面，二级 400×225 也是，
            // 再往下才没有意义——门槛跟着级走，不然"逐级"就白做了。
            // Each level uses **its own size** (`>> lv`) and passes the "big enough" line on its own: 800x450 at level 1
            // is a picture and so is 400x225 at level 2, and only below that does it stop meaning anything -- the
            // threshold follows the level, or "level by level" would be pointless.
            uint32_t lw = lv > 31 ? 1u : (ws[i] >> lv), lh = lv > 31 ? 1u : (hs[i] >> lv);
            if (lw < 128 || lh < 128) break;
            char path[512];
            // 文件名带帧号与级号：多帧扫描时同一张贴图的同一级会被看好几次，不带就会互相覆盖
            // The name carries the frame number and the level: a multi-frame scan looks at the same level of the same
            // texture several times, and without them they overwrite one another
            if (prefix) snprintf(path, sizeof path, "%s%llu-%llu-l%u.png", prefix, (unsigned long long)frame,
                                 (unsigned long long)ser[i], (unsigned)lv);
            nread++;
            if (shot_grab_ser_lv(ser[i], (unsigned)lv, ws[i], hs[i], prefix ? path : NULL, 0) >= 2) ncontent++;
        }
    }
    LOG("[vk] 扫描（出图第 %llu 次）结束：读了 %d 次（逐张逐级），其中 %d 次拿到了内容", (unsigned long long)frame, nread, ncontent);
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
        shot_grab_ser_lv(ser[i], 0, w, h, path, 1);
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
    int n = snprintf(buf, cap,
                    "出图 %llu；绘制 有目标 %llu / 无目标 %llu；T2T %llu；渲染目标 %d 张（前几名累计 %llu）；图形管线 %llu（失败 %llu）；未执行 op %llu",
                    (unsigned long long)atomic_load(&g_present_n),
                    (unsigned long long)atomic_load(&g_draw_ncN), (unsigned long long)atomic_load(&g_draw_nc0),
                    (unsigned long long)vk_tex_t2t_count(), g_rt_n, (unsigned long long)tot,
                    (unsigned long long)gpso, (unsigned long long)nfail, (unsigned long long)atomic_load(&g_op_unhandled));
    // N4-c：**G4 的第三条判据就挂在这一行上**（每秒一次，截图里也看得见）——
    // 「等队列空闲」那一格在新地基下必须是 0；另一半是"被定向栅栏/提交序替代掉"的次数（自证不是没跑到）。
    // N4-c: G4's third criterion rides on this line (once a second, and it shows in screenshots): the queue-idle
    // count must be zero under the new foundation, and the "avoided" count proves the sites really ran.
    if (g_vk2_gpu && n > 0 && (size_t)n < cap)
        n += snprintf(buf + n, cap - (size_t)n,
                      "；**等队列空闲 %llu**（新地基下必须 0）/ 被定向栅栏替代 %llu；提交侧 %s",
                      (unsigned long long)vk_idle_calls(), (unsigned long long)vk_idle_calls_skipped(),
                      g_vk2_sub_ok ? "新地基（vk2_sub）" : "旧路 8 槽");
    // N4-d 第 0 条：**我方自己的超时**与两条水位线挂在**这一行**上 —— 这一行是**带时间戳**的（宿主每秒一条），
    // 而 `【红】` 那几条（模块自己打的）没有时间戳 ⇒ 只有把计数搬到这里，才能回答"超时发生在停顿窗口里的
    // 哪一刻"（判据：出图冻住 + 提交到 frame 涨、完成到 frame 不涨 ⇒ 队列真的不动）。
    // N4-d item 0: our own timeout counters and the two high-water marks ride this line because it is the timestamped
    // one (one per second from the host); the module's own 【红】 lines carry no timestamp.
    if (g_vk2_gpu && n > 0 && (size_t)n < cap) {
        vk2_sub_stats ss; vk2_sub_stats_get(&ss);
        vk2_xfer_stats xs; vk2_xfer_stats_get(&xs);
        // N4-d 第二部分：**三个水位线之间差 1 的口径**要靠"正在录的 frame"才能对齐（`vk2_sub_begin` 已经把这个
        // frame 记进台账，只是没被报出来）。三个数一起读才有意义：`提交到 X` 是**已经交给队列的最大 frame**、
        // `完成到 Y` 是**已确认完成的最大 frame**、`正在录 Z` 是**下一个要提交的 frame**（`vk2_sub_begin` 拿到
        // 槽之后、`vk2_sub_commit` 之前，此时 Z 还没进队列）。⇒ 稳态下应满足 `Z == X + 1`（下一帧已开始录）、
        // `X - Y == 在飞`；冻结时若 `X` 不涨而 `Y` 也不涨，才是"队列不动"（反过来 `X` 涨而 `Y` 不涨 = 在飞堆积）。
        // N4-d part 2: the "off-by-one" between the three numbers can only be settled by also reporting the frame
        // being recorded right now. In steady state Z == X+1 and X - Y == in-flight.
        n += snprintf(buf + n, cap - (size_t)n,
                      "；**我方超时** 回读拷贝 %llu / 取槽 %llu；**提交到 %llu、完成到 %llu、正在录 %llu**（在飞 %d）"
                      "；ReadTexels 读到 GPU 写过的贴图 %llu 次",
                      (unsigned long long)xs.fence_timeouts, (unsigned long long)ss.timeout_fails,
                      (unsigned long long)g_fence, (unsigned long long)ss.reaped_max,
                      (unsigned long long)vk2_sub_frame(), vk2_sub_pending(),
                      (unsigned long long)vk_tex_reads_gpu_written());
    }
    // N4-e：**顶点/几何侧**那几格（光栅化侧已排除；这几格以前一条读数都没有）。
    // 判读：`没绑上` > 0 ⇒ 有绘制是"管线要用这条流、但缓冲没反查出来 ⇒ 没绑"，那种绘制产不出片元，
    // 与"画了什么都没有"直接对上；`顶点数 0 / 索引数 0` > 0 ⇒ 几何本身是空的（另一种直接成因）。
    if (n > 0 && (size_t)n < cap) {
        // 心跳这一行**有长度上限**（调用方给 640 字节）⇒ 这里只放四个总数；"没绑上**按哪条流**"那一段
        // 放进"每 60 次提交"的判据行（没有这个上限）。**2026-10-16 实测过**：把分账塞进心跳之后，
        // 这一行的尾巴（`查看器 读成 / 跳过`、`出图在推进`）被截掉了 —— 那是把一条既有判据弄丢，
        // 比多报一格更糟（第 ⑩ 条的同类：统计快照的位置会改读数）。
        // The heartbeat line has a length cap (640 bytes from the caller), so only the four totals go here; the
        // "which stream" breakdown goes into the every-60-submissions criterion line, which has no such cap.
        // Measured on 2026-10-16: putting the breakdown into the heartbeat truncated the tail (the viewer's
        // "read/skipped" counters and "present advancing") -- losing an existing criterion is worse than adding one.
        n += snprintf(buf + n, cap - (size_t)n,
                      "；顶点流 绑上 %llu / **没绑上 %llu** / 一条都没 %llu；顶点数 0 的 %llu、索引数 0 的 %llu",
                      (unsigned long long)atomic_load(&g_vb_ok), (unsigned long long)atomic_load(&g_vb_fail),
                      (unsigned long long)atomic_load(&g_vb_none), (unsigned long long)atomic_load(&g_draw_v0),
                      (unsigned long long)atomic_load(&g_draw_i0));
        // N4-f：顶点/常量侧那几格。**只在开了采样时出现**（缺省关 ⇒ 这一行的长度与 N4-e 逐字节相同，
        // "缺省关不改变既有判据"这条口径因此是可核的）。明细在判据行那条【顶点采样】里（心跳放不下）。
        uint64_t vs_n = atomic_load(&g_vtx_samp), us_n = atomic_load(&g_ubo_samp);
        if ((vs_n || us_n) && (size_t)n < cap) {
            int m = snprintf(buf + n, cap - (size_t)n,
                             "；**顶点采样 %llu 次**（NaN %llu / Inf %llu / 读不了 %llu）；常量采样 %llu 次（NaN %llu / Inf %llu）",
                             (unsigned long long)vs_n, (unsigned long long)atomic_load(&g_vtx_nan),
                             (unsigned long long)atomic_load(&g_vtx_inf), (unsigned long long)atomic_load(&g_vtx_unread),
                             (unsigned long long)us_n, (unsigned long long)atomic_load(&g_ubo_nan),
                             (unsigned long long)atomic_load(&g_ubo_inf));
            if (m > 0) n += m;
            if ((size_t)n > cap) n = (int)cap;
        }
        // N4-h：**我们自己的三角形**那一格（插入次数 / 本模块自己建的管线数）。与 N4-f 同一套做法：
        // 包在"只在开关打开时出现"里 ⇒ **缺省关时这一行与上一轮逐字节相同**（可用 `.work/n4e_hb_len.py` 核，
        // 第 ㉓ 条）。明细（第几次插入、那一刻的通道与视口、管线创建结果）在 `vk_tri_game.c` 的【查】行里。
        // N4-h: the "our own triangle" cell (inserts / pipelines built by this module). Same shape as N4-f: it only
        // appears when the switch is on, so with the default off this line is byte-for-byte the previous round's.
        char tgb[192];
        int tgn = vk_tri_game_desc(tgb, sizeof tgb);
        if (tgn > 0 && (size_t)n < cap) {
            int m = snprintf(buf + n, cap - (size_t)n, "；%s", tgb);
            if (m > 0) n += m;
            if ((size_t)n > cap) n = (int)cap;
        }
        // N4-i：**管线缓存键里没有着色器**这件事发生过多少次（只读；`DM_PIPEKEY_PROBE=1` 才增长 ⇒
        // 缺省关时恒 0 ⇒ 这一行与上一轮**逐字节相同**，第 ㉓ 条）。判读：> 0 ⇒ 有绘制拿到的是
        // **别人那一对的管线**（那种绘制的顶点着色器不是它要的 ⇒ 几何整片塌掉、零报错 —— 正是
        // "39k 次绘制一个像素不落、且校验层在 draw 层面一条都不报"的形状）。
        uint64_t km = vk_pipe_keymiss();
        if (km && (size_t)n < cap) {
            int m = snprintf(buf + n, cap - (size_t)n, "；**管线键** 命中但着色器不同 %llu 次", (unsigned long long)km);
            if (m > 0) n += m;
            if ((size_t)n > cap) n = (int)cap;
        }
    }
    return n;
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

// GPU 已完成某一个槽：先把"GPU 完成后才生效"的副作用做掉（旧路与新路的**同一段代码**，N4-c 把提出来共用）。
// The "only after completion" effects of one finished slot (the same code on both roads; N4-c factored it out).
static void slot_post_run(int i) {
    for (int k = 0; k < g_inf[i].npost; k++) {
        const post_op *o = &g_inf[i].post[k];
        if (o->kind == POST_EVENT) { if (o->mode == 0) *o->val = o->v; else *o->val += o->v; }
        else if (o->kind == POST_COUNTER) { if (o->addr) *(volatile uint32_t *)(uintptr_t)o->addr = (uint32_t)o->type; }
        else if (o->kind == POST_MEMCPY) memcpy(o->dst, o->src, o->n);
    }
    g_inf[i].npost = 0;
    g_inf[i].pending = 0;
}

// N4-c：**槽回收钩子**（`vk2_sub` 在"观察到某个槽的栅栏已经 signal"时调它，然后才让那个槽可重用）。
// 三件事，顺序就是它们必须发生的顺序：
//   ① 副作用（事件写值 / 计数器 / 未对齐拷贝的 CPU memcpy）—— 与旧路同一段代码；
//   ② **往返型的回读**：这次提交让 GPU 直写过后备的那几段（`COPY_B2B` / `UPDATE_UB` 的目标），按**这次提交
//      自己的栅栏（已经 signal）**整页回读进影子 —— 旧路里 GPU 写的就是池内存本身、CPU 后读得到；新地基下
//      写落在后备资源上 ⇒ 不回读就永远读到旧值（"出图停住"的第一嫌疑，见 ANDROID_PLAN §八·N4-b）；
//   ③ 描述符池整池重置 + 时间戳读回（与旧路回收时同一件事）。
// **环的交还由 `vk2_sub` 自己做**（`vk2_sub_note_upload` 记着这一槽载着窗口）—— 这里不碰环。
// N4-c: the reclaim hook. Three things in this order: the post-completion effects, the round-trip readback of the
// ranges this submission let the GPU write directly (on its own signalled fence), and the descriptor-pool reset plus
// timestamp readback. Handing the ring bank back is vk2_sub's own job (vk2_sub_note_upload), not ours.
static void vk2_reap_hook(int slot, uint64_t frame, VkFence fence, void *ud) {
    (void)ud;
    if (slot < 0 || slot >= MAX_INFLIGHT) return;
    g_vk2_rb_hook++;
    inflight_t *in = &g_inf[slot];
    slot_post_run(slot);
    in->value = frame;
    if (in->ngpu_wr) {
        uint64_t t0 = now_ns(), bytes = 0;
        int ok = 1, deferred = 0;
        vk2_readback_after(fence);              // 生产者 = 这次提交自己（此刻已经 signal）
        for (int k = 0; k < in->ngpu_wr; k++) {
            bytes += in->gpu_wr[k].len;
            if (!vk2_readback_addr(in->gpu_wr[k].addr, in->gpu_wr[k].len)) ok = 0;
        }
        // 语义（照旧路的读法定的）：**GPU 直写那一段里的字节以 GPU 为准**，其余字节保持影子的（= CPU 的）。
        // 理由：客体是"先把数据写好、再录这条拷贝/更新、最后提交"，而拷贝在提交之后才执行 ⇒ 同一份内存里
        // 也是"拷贝后写、它赢"（旧路的池内存就是 GPU 内存，就是这个次序）。同页上**落在 GPU 写过的区间之外**
        // 的 CPU 字节不受影响 —— 这就是"页内字节级"的那一半（N2 只报计数的半页竞态在这里被拆开了）。
        // Semantics (read off the old road): inside the range the GPU wrote, the GPU's bytes win -- the guest writes
        // its data first, records the copy, and the copy executes after submission, so on a single copy of memory the
        // GPU write is the later one. CPU bytes elsewhere on the page are untouched.
        // 清 GPU 位**只能在没有别的在飞提交还写同一段覆盖到的页时做**：清早了会把那一次写记成 CLEAN，
        // 而它还没被回读 ⇒ 那个写就再也回不来了（下一个回收钩子也不会再碰它）。有人还在写 ⇒ 留给它收尾。
        for (int k = 0; k < in->ngpu_wr; k++) {
            uint64_t lo = in->gpu_wr[k].addr, hi = lo + in->gpu_wr[k].len;
            uint64_t gran = vk2_dirty_gran_default();              // 与脏位图粒度同档（N0 的读数：4 KiB）
            uint64_t p0 = lo / gran * gran, p1 = (hi + gran - 1) / gran * gran;
            int busy = 0;
            for (int j = 0; j < MAX_INFLIGHT && !busy; j++) {
                if (j == slot || !g_inf[j].pending) continue;
                for (int m = 0; m < g_inf[j].ngpu_wr; m++) {
                    uint64_t q0 = g_inf[j].gpu_wr[m].addr, q1 = q0 + g_inf[j].gpu_wr[m].len;
                    if (q1 > p0 && q0 < p1) { busy = 1; break; }
                }
            }
            if (busy) { deferred++; continue; }
            vk2_gpu_settle(lo, hi > lo ? hi - lo : 1);
        }
        uint64_t dt = (now_ns() - t0) / 1000ull;   // µs
        g_vk2_rb_calls++; g_vk2_rb_bytes += bytes; g_vk2_rb_us += dt;
        if (deferred) g_vk2_rb_deferred += (uint64_t)deferred;
        if (dt > g_vk2_rb_max_us) g_vk2_rb_max_us = dt;
        if (!ok && g_vk2_rb_calls <= 8)
            LOG("【黄】槽 %d（frame %llu）：这次提交的 GPU 直写段有 %d 段没回读成（影子可能还是旧值）",
                slot, (unsigned long long)frame, in->ngpu_wr);
        if (g_vk2_rb_calls <= 6 || (g_vk2_rb_calls % 300) == 0)
            LOG("【地基】往返型回读：第 %llu 次（槽 %d / frame %llu）：%d 段 / %llu 字节，耗时 %llu µs"
                "（GPU 写后备→影子；清位延后 %d 段）",
                (unsigned long long)g_vk2_rb_calls, slot, (unsigned long long)frame, in->ngpu_wr,
                (unsigned long long)bytes, (unsigned long long)dt, deferred);
    }
    in->ngpu_wr = 0; in->gpu_wr_over = 0;
    desc_slot_reset(slot);
    gpu_slot_readback(slot);
}

static void inflight_reap(void) {
    if (g_vk2_sub_ok) return;   // N4-c：新地基下槽的回收由 `vk2_sub` 在回收时刻调钩子（栅栏句柄会被重用）
    for (int i = 0; i < MAX_INFLIGHT; i++) {
        if (!g_inf[i].pending) continue;
        if (vkGetFenceStatus(vk_dev(), g_inf[i].fence) != VK_SUCCESS) continue;
        slot_post_run(i);
        // N4-b：这一槽载着上传窗口 ⇒ **这一段暂存环现在才能交还**（环的存活期 ≤ 槽的存活期：
        // 环里的字节是那次拷贝的源，GPU 没做完之前不能被下一帧的 memcpy 盖掉）。
        // N4-b: this slot carried an upload window, so its ring bank is handed back only now.
        if (g_inf[i].vk2_up) { g_inf[i].vk2_up = 0; if (vk2_xfer_ready()) vk2_xfer_window_done_bank(i); }
        // 槽已回收（栅栏过了）：这个槽的描述符池可以整池重置——池里所有集都被 GPU 用完且再也不用了
        // The slot is reaped (fence signalled): this slot's descriptor pool can be reset whole -- every set in it
        // has been consumed by the GPU and never will be again
        desc_slot_reset(i);
        gpu_slot_readback(i);   // 这一槽的时间戳也读回来（同一时刻：GPU 已经做完） | read this slot's timestamps too (same moment: the GPU is done)
    }
}

// M3-2v：等栅栏这一段也记账。「M3-2u」的停住现场是"客体线程卡在宿主侧的一把锁 / futex 上、32 秒不发一次 SVC"，
// 而这条路上最可疑的宿主等待就是这里（8 个槽全满时等最老的那个栅栏；设备丢失 / 驱动卡住时它永远不回来）。
// 记三样：现在有没有人在等、等哪个槽、从什么时候开始；超过阈值另打一行日志（不看转储也能看出"谁在等栅栏"）。
// M3-2v: the fence wait is accounted for too. The scene in "M3-2u" is "the guest thread blocked on a host-side
// lock / futex, no SVC for 32 s", and the most suspicious host wait on this path is this one (with all 8 slots busy it
// waits for the oldest fence; on a lost device or a stuck driver that never returns). It records whether anyone is
// waiting, on which slot, and since when, and logs a line past a threshold.
static _Atomic int g_led_fence_wait; static int g_led_fence_slot; static uint64_t g_led_fence_since;
static int inflight_take(void) {   // 取一个空闲槽：没有就等（单一队列按序完成，等最老的那个即可）| Take a free slot, waiting if none is free
    for (;;) {
        inflight_reap();
        for (int i = 0; i < MAX_INFLIGHT; i++) if (!g_inf[i].pending) return i;
        for (int i = 0; i < MAX_INFLIGHT; i++) {
            uint64_t t0 = now_ns();
            atomic_store(&g_led_fence_wait, 1); g_led_fence_slot = i; g_led_fence_since = t0;
            dm_waitled_enter("inflight_take：槽池满，等最老那个槽的栅栏");
            VkResult wr = vkWaitForFences(vk_dev(), 1, &g_inf[i].fence, VK_TRUE, 1000000000ull);
            dm_waitled_leave();
            atomic_store(&g_led_fence_wait, 0);
            if (now_ns() - t0 > 300000000ull)
                LOG("[vk] 等栅栏：槽 %d 等了 %llu 毫秒（返回 %d，槽的栅栏值 %llu，已提交到栅栏 %llu）", i,
                    (unsigned long long)((now_ns() - t0) / 1000000), (int)wr,
                    (unsigned long long)g_inf[i].value, (unsigned long long)g_fence);
            if (wr == VK_SUCCESS) { inflight_reap(); break; }
        }
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
    // M3-2v：再把读回路径那把锁的账本与"等栅栏"那一段打出来 —— 三样合起来就是完整的"谁在等谁"。
    // M3-2v: then the ledger of the read-back path's lock and the fence-wait stretch -- the three together are the
    // complete "who waits on whom".
    vk_led_line();
    if (atomic_load(&g_led_fence_wait))
        LOG("[vk] 等栅栏：有人在等槽 %d，已等 %llu 毫秒", g_led_fence_slot,
            (unsigned long long)((now_ns() - g_led_fence_since) / 1000000));
    else LOG("[vk] 等栅栏：没人在等");
    // 直接问驱动：每个在飞槽的栅栏**到底到了没有**（0 = 已就绪、1 = 还没到、其他 = 出错）。
    // 这一行把"驱动没做完"与"我们自己的记账没跟上"分开 —— 前者是 GPU / 队列的事，后者是我们的 bug。
    // Ask the driver directly whether each in-flight slot's fence has arrived (0 = signalled, 1 = not yet, other =
    // error). This line separates "the driver never finished" from "our own bookkeeping is behind": the first is the
    // GPU / queue, the second is our bug.
    { char st[200]; int q = 0;
      for (int i = 0; i < MAX_INFLIGHT && q < (int)sizeof st - 24; i++)
          q += snprintf(st + q, sizeof st - q, "%s%d:%llu/%d", i ? " " : "", i,
                        (unsigned long long)(g_inf[i].pending ? g_inf[i].value : 0),
                        g_inf[i].pending ? (int)vkGetFenceStatus(vk_dev(), g_inf[i].fence) : -1);
      LOG("[vk] 槽的栅栏实况（槽:栅栏值/驱动状态，-1 = 槽空闲）：%s", st); }
    // B3：GPU 操作流水也归在"现场"里 —— 看门狗转储、按需转储（dump.trigger）、崩溃现场都走到这个函数，
    // 所以"停住/丢失之前最后几笔成功操作"在任何一份现场里都看得到（见 vk_ops.h 的开头）。
    // B3: the GPU-operation ledger belongs in the scene too -- the watchdog dump, the on-demand dump (dump.trigger) and
    // a crash scene all reach this function, so "the last few successful operations before the stall / loss" is in
    // every scene (see the top of vk_ops.h).
    vk_op_dump("等待点转储");
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

// N4-b：**一帧的开头** —— 把"帧开头这一刻影子已经写好、而 GPU 那边还没有"的字节搬进后备资源。
// 录在命令缓冲的**最前面**：这一帧里所有"读池内存"的命令（顶点/索引/常量/贴图绑定、拷贝）都排在它后面。
// 为什么是帧开头而不是绑定点：NVN 的模型是"CPU 先把内存写好，再提交命令流"，而我们的回放就发生在提交那一刻
// ⇒ 帧开头时影子已经含这一帧的全部 CPU 写。标记（哪一段要搬）来自**上一次绑定/拷贝的触碰**（见 vk2_touch_*）。
// N4-b: the head of a frame -- push the bytes the shadow already has (and the GPU does not) into the backing store,
// recorded before anything else in the command buffer. The replay runs at submission time, so the shadow already
// holds every CPU write of this frame; which ranges to move is what the previous frame's touches marked.
static void vk2_frame_upload(int slot, int bank) {
    if (!g_vk2_gpu || !vk2_xfer_ready() || slot < 0 || slot >= MAX_INFLIGHT) return;
    // N4-b：**初始同步**（含"运行中新开的池"）—— 池刚登记时整池是脏的，而"一帧一段环"的节奏要几百帧才搬完，
    // 那几百帧里游戏读到的是没搬上去的部分。所以第一次提交之前先把脏页整批搬完（一次，~0.4 s 量级）。
    // N4-c：用**最后一段**环（这一刻它不可能是某个在飞槽的段：初始同步发生在第一帧的开头），
    // 新地基下段数是 3（不是旧路的 8）⇒ 段号要按实际分出来的段数取，写死 7 会越界。
    // N4-b: initial sync (including pools opened later): a fresh pool is entirely dirty and one ring's worth per
    // frame would take hundreds of frames, so the whole dirty set is pushed once before the first submission.
    // N4-c: it uses the last bank, which cannot be held at this point (this runs at the very first frame head).
    if (g_vk2_adds && g_vk2_adds != g_vk2_synced_pools) {
        uint64_t t0 = now_ns();
        int sb = (int)(g_vk2_banks ? g_vk2_banks - 1 : (uint64_t)(MAX_INFLIGHT - 1));
        uint64_t total = vk2_xfer_initial_sync(sb);
        g_vk2_synced_pools = g_vk2_adds;
        vk2_xfer_stats xs;
        vk2_xfer_stats_get(&xs);
        LOG("【地基】初始同步：%llu 个池 / 搬 %llu MB、%llu 轮、耗时 %llu ms（让后备资源与影子一次一致）",
            (unsigned long long)g_vk2_adds, (unsigned long long)(total >> 20),
            (unsigned long long)xs.initial_rounds, (unsigned long long)((now_ns() - t0) / 1000000));
    }
    g_vk2_frames++;
    uint64_t moved = vk2_upload_all_into(bank, g_inf[slot].cb, g_fence + 1);
    if (!moved) return;
    // N4-b：回收这个槽时才能把这**一段**环交还；N4-c：交还由 `vk2_sub` 在回收时刻自己做（它记着"这一槽载着窗口"），
    // 旧路仍然由 `inflight_reap` 交还。
    // N4-b: the bank is handed back when the slot is reaped; N4-c leaves that to vk2_sub (which knows the slot carries
    // a window) and the old road keeps doing it in inflight_reap.
    if (g_vk2_sub_ok) vk2_sub_note_upload();
    else g_inf[slot].vk2_up = 1;
    g_vk2_up_frames++; g_vk2_up_windows++; g_vk2_up_bytes += moved;
    if (moved > g_vk2_up_max) g_vk2_up_max = moved;
    if (g_vk2_up_frames <= 6 || (g_vk2_frames % 30) == 0)
        LOG("【地基】帧开头上传：第 %llu 帧（槽 %d / 段 %d）搬 %llu KB；累计 %llu 帧里 %llu 帧搬过、"
            "共 %llu MB（最多一帧 %llu KB）",
            (unsigned long long)g_vk2_frames, slot, bank, (unsigned long long)(moved >> 10),
            (unsigned long long)g_vk2_frames, (unsigned long long)g_vk2_up_frames,
            (unsigned long long)(g_vk2_up_bytes >> 20), (unsigned long long)(g_vk2_up_max >> 10));
}

void mtl_sub_begin(void) {
    if (!vk_sub_ready()) return;
    int i = -1;
    int bank = 0;
    if (g_vk2_sub_ok) {
        // N4-c：**提交侧走新地基** —— 取槽 = `vk2_sub_begin()`（先轮询 `vkGetFenceStatus`、满了只等最老那个槽的
        // 定向栅栏、带超时；**绝不等队列空闲**）。命令缓冲是**它**的（"一个客体提交 = 一次 `vkQueueSubmit`"），
        // 我们只是把它接到 `g_inf[i].cb` 上 —— 下游几十处录命令的地方一行都不用改。
        // 槽回收（副作用 / GPU→池回读 / 描述符池）由它的回收钩子在**它自己看到栅栏 signal 的那一刻**做。
        vk2_slot *s = vk2_sub_begin();
        if (!s) {
            g_vk2_begin_fail++;
            if (g_vk2_begin_fail <= 4)
                LOG("【红】N4-c：取不到提交槽（%d 个槽都在飞且等栅栏超时）⇒ 这次提交被丢（如实报，不重试）",
                    vk2_sub_inflight());
            return;                                  // g_cur_slot 保持 -1：下游录命令的地方都会自己跳过
        }
        i = vk2_sub_slot_id(s);
        if (i < 0 || i >= MAX_INFLIGHT) { LOG("【红】N4-c：槽号 %d 越界（最多 %d）", i, MAX_INFLIGHT); vk2_sub_abort(); return; }
        g_inf[i].cb = s->cb;                          // 把新地基的 cb 接进来（下游只认 g_inf[..].cb）
        bank = vk2_sub_slot_bank(i);
        if (bank < 0) bank = 0;
    } else {
        i = inflight_take();
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        if (vkBeginCommandBuffer(g_inf[i].cb, &bi) != VK_SUCCESS) { LOG("【红】开始命令缓冲失败"); return; }
        bank = i;
    }
    g_inf[i].cur = 1; g_inf[i].npost = 0; g_inf[i].stage_off = 0;
    g_inf[i].ngpu_wr = 0; g_inf[i].gpu_wr_over = 0;
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
    // N4-l：描述符池的每帧读数（`DM_DESC_STATS=1`；关着时一行都不走 ⇒ 既有判据逐字节不变）
    desc_frame_stats(i);
    // N4-b：帧开头把 CPU 脏页搬上去（`DM_GFX2=1` 且 GPU 侧已接管时才动；关着时一行都不执行）
    vk2_frame_upload(i, bank);
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
        // N4-b：退成"栅栏到了 CPU memcpy"，那是**写影子** ⇒ 目标标 CPU 脏（下一帧开头会上传它）
        vk2_touch_cpu_(dst, n);
        static int said; if (!said++) LOG("【黄】有缓冲拷贝偏移不是 4 字节对齐（%#llx→%#llx），退成完成后搬", (unsigned long long)so, (unsigned long long)dof);
        return;
    }
    // N4-b（`COPY_B2B`）：这是一次 **GPU 拷贝** —— 源必须先在**后备**里是最新的（标 CPU 脏 ⇒ 帧开头搬它；
    // 因为旧路的源是池内存本身、CPU 写完 GPU 就能看见，而新地基里"看见"必须显式买回来），
    // 目标是 GPU 直写后备 ⇒ **影子从此过期**（标 GPU 脏：上传不许碰它，谁要读它必须先回读）。
    // N4-b (COPY_B2B): a GPU copy. The source must be up to date in the backing store (marked CPU-dirty so the
    // frame head uploads it), and the destination is written by the GPU, so the shadow goes stale (GPU-dirty).
    vk2_touch_cpu_(src, n);
    vk2_touch_gpu_(dst, n);
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

// N4-c 的读数（**11.3 里最后缺的三个字段的真帧落点**）：每帧提交次数 / 槽等待率 / 每帧脏 run 数，
// 外加往返型回读与半页竞态的账。节流 = 每 60 次提交（+ 第一次）—— 游戏一轮约 100~200 次提交，
// 挂在"每 300 次"上会一行都打不出来（口径：节流要按这一轮实际发生的量级定）。
// N4-c's readings: the three baseline fields 11.3 was still missing, plus the round-trip and half-page accounts.
static void vk2_n4c_report(int slot) {
    vk2_sub_stats ss;
    vk2_sub_stats_get(&ss);
    vk2_xfer_stats xs;
    vk2_xfer_stats_get(&xs);
    uint64_t fr = g_vk2_frames ? g_vk2_frames : 1;
    LOG("【地基】N4-c 提交侧：第 %llu 次提交（帧号 %llu、槽 %d、在飞 %d、每帧提交 %.2f）；槽等待率 %llu‰"
        "（%llu/%llu，轮询回收 %llu）、提交失败 %llu、取不到槽 %llu；**等队列空闲 %llu**（新地基下必须 0）/ 被替代 %llu；"
        "客体按自己的超时等围栏 %llu 次（其中超时 %llu 次）",
        (unsigned long long)g_sub_total, (unsigned long long)g_fence, slot, vk2_sub_pending(),
        (double)g_vk2_subs / (double)fr,
        (unsigned long long)(ss.begins ? 1000ull * ss.waits / ss.begins : 0),
        (unsigned long long)ss.waits, (unsigned long long)ss.begins, (unsigned long long)ss.poll_reaps,
        (unsigned long long)ss.submit_fail, (unsigned long long)g_vk2_begin_fail,
        (unsigned long long)vk_idle_calls(), (unsigned long long)vk_idle_calls_skipped(),
        (unsigned long long)ss.wait1_calls, (unsigned long long)ss.wait1_timeouts);
    LOG("【地基】N4-c 往返型：回读 %llu 次 / %llu KB（单次最长 %llu µs、累计 %llu ms；回收钩子 %llu 次、记下"
        "GPU 直写区间 %llu 段、清位延后 %llu 段）；半页竞态撞上 %llu 次、上传跳过 GPU 脏页 %llu 页 / %llu KB；"
        "每帧脏 run %.2f",
        (unsigned long long)g_vk2_rb_calls, (unsigned long long)(g_vk2_rb_bytes >> 10),
        (unsigned long long)g_vk2_rb_max_us, (unsigned long long)(g_vk2_rb_us / 1000),
        (unsigned long long)g_vk2_rb_hook, (unsigned long long)g_vk2_rb_ranges,
        (unsigned long long)g_vk2_rb_deferred, (unsigned long long)g_vk2_hazard,
        (unsigned long long)xs.up_skip_gpu, (unsigned long long)(xs.up_skip_gpu_bytes >> 10),
        (double)xs.runs / (double)fr);
}

uint64_t mtl_sub_commit(void) {
    if (!vk_sub_ready() || g_cur_slot < 0) { g_cur_slot = -1; return g_fence; }
    pass_end();   // 收尾时通道必须已关（VkCommandBuffer 不允许带着未关的动态渲染通道结束）| the pass must be closed before finishing (a VkCommandBuffer may not end inside an open dynamic render pass)
    inflight_t *in = &g_inf[g_cur_slot];
    int i = g_cur_slot; g_cur_slot = -1;
    if (g_vk2_sub_ok) {
        // N4-c：**一次客体提交 = 一次 `vkQueueSubmit`**（`vk2_sub_commit` 自己收尾 cb、`vkResetFences`、提交并
        // 带上那个槽的定向栅栏；队列锁由它内部用同一把锁拿）。帧号也用它那一套 —— `g_fence` 跟着它走，
        // 客体按 NVN 语义来等的那条路（`mtl_wait`）因此问的是**新地基的 frame**。
        // N4-c: one guest submission is exactly one vkQueueSubmit, done inside vk2_sub_commit (it ends the cb, resets
        // the fence and submits with that slot's own fence, taking the same queue lock). The frame numbering becomes
        // vk2_sub's, so the guest's own fence waits (mtl_wait) ask about a new-foundation frame.
        vk_queue_note("客体：提交命令缓冲");
        dm_waitled_enter("vk：提交命令缓冲");
        int ok = vk2_sub_commit();
        dm_waitled_leave();
        if (!ok) { LOG("【红】N4-c：vk2_sub_commit 没成（收尾 cb / 重置栅栏 / 提交 三者之一失败，细节见上一行）"); return g_fence; }
        vk_op_note("客体：提交命令缓冲");
        g_fence = vk2_sub_last_frame();
        in->pending = 1;
        in->value = g_fence;
        g_vk2_subs++;
        gpu_note_frame(i, atomic_load(&g_cur_frame));
        g_sub_total++;
        // N4-c 的读数**单独一行、按 60 次提交节流**（原来挂在"每 300 次"那个块里 —— 游戏一轮只有 ~200 次提交，
        // 于是那三行一次都没打出来：读数的节流口径要按**这一轮实际会发生的量级**定，见 TOOLS B⁷）。
        if (g_sub_total % 60 == 0 || g_sub_total == 1) vk2_n4c_report(i);
        goto report_ops;
    }
    if (vkEndCommandBuffer(in->cb) != VK_SUCCESS) { LOG("【红】结束命令缓冲失败"); return g_fence; }
    VkResult r = vkResetFences(vk_dev(), 1, &in->fence);
    if (r != VK_SUCCESS) { LOG("【红】重置栅栏失败 VkResult %d", (int)r); return g_fence; }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &in->cb };
    // 客体线程的提交与前端线程的上屏/读回共用同一条队列，提交这一步必须串行化（见 vk_device.h）
    // The guest thread's submits and the front end's present and readbacks share one queue, so this submit has to be
    // serialised (see vk_device.h)
    vk_queue_note("客体：提交命令缓冲");
    vk_queue_lock();
    // M3-2w：这是客体线程**每帧**的提交，也是"等驱动"的头号调用点（驱动内部 poll），进账本。
    // M3-2w: the guest thread's **per-frame** submit, and the number one "waiting for the driver" call (the driver polls
    // inside), so it goes on the books. (Written out by hand rather than through VKW: the return value is used below.)
    dm_waitled_enter("vk：提交命令缓冲");
    r = vkQueueSubmit(vk_queue(), 1, &si, in->fence);
    dm_waitled_leave();
    vk_queue_unlock();
    if (r != VK_SUCCESS) { LOG("【红】提交失败 VkResult %d", (int)r); vk_op_result("vkQueueSubmit（客体：命令缓冲）", r); return g_fence; }
    vk_op_note("客体：提交命令缓冲");   // B3：成功的那几笔进流水，-4 时连同"前序操作"一起打出来 | B3: successful ones go on the ledger for the -4 report
    in->pending = 1;
    in->value = ++g_fence;
    gpu_note_frame(i, atomic_load(&g_cur_frame));   // 这次提交属于哪一帧（完成是滞后的，回查靠帧号） | which frame this submission belongs to (completion is late; the lookup goes by frame number)
    g_sub_total++;
report_ops:
    // N4-d：**这两条判据各占一行、按"每 60 次提交 + 第 1 次"节流**（与 N4-c 的 60 次口径一致），两臂都打。
    // ① `ReadTexels 读到 GPU 写过的贴图 N 次` —— N4-c 的读数里**它一次都没出现**：原来挂在下面"每 300 次提交"
    //    那个块里，而游戏一轮只有 ~100~200 次提交 ⇒ **没有读数**（没有读数 ≠ 是 0，见 TOOLS B⁷）。
    // ② **我方自己的超时**（回读拷贝 / 取槽 / 提交失败 / 取不到槽）与"**提交到哪个 frame、完成到哪个 frame**"——
    //    N4-d 第 0 条要回答的"停顿窗口里那条队列是不是完全不动"就靠这两个水位线（t. 与心跳那一行对照）。
    // N4-d: both criteria get their own line, throttled at every 60th submission plus the first (the same throttle
    // N4-c used), and they print on **both** arms. ① the ReadTexels reading that never appeared in N4-c (it used to
    // ride the every-300 block, and a game round only submits ~100~200 times); ② our own timeout counters plus the
    // submit/complete high-water marks, which is what N4-d item 0 needs to answer "did the queue move at all".
    if (g_sub_total % 60 == 0 || g_sub_total == 1) {
        if (g_vk2_gpu) {
            vk2_sub_stats ss; vk2_sub_stats_get(&ss);
            vk2_xfer_stats xs; vk2_xfer_stats_get(&xs);
            LOG("[vk] 提交 %llu：ReadTexels 读到 GPU 写过的贴图 %llu 次；**我方超时** 回读拷贝 %llu / 取槽 %llu；"
                "提交失败 %llu、取不到槽 %llu；**提交到 frame %llu、完成到 frame %llu、正在录 %llu**（在飞 %d）",
                (unsigned long long)g_sub_total, (unsigned long long)vk_tex_reads_gpu_written(),
                (unsigned long long)xs.fence_timeouts, (unsigned long long)ss.timeout_fails,
                (unsigned long long)ss.submit_fail, (unsigned long long)ss.no_slot_fail,
                (unsigned long long)g_fence, (unsigned long long)ss.reaped_max,
                (unsigned long long)vk2_sub_frame(), vk2_sub_pending());
        } else {
            LOG("[vk] 提交 %llu：ReadTexels 读到 GPU 写过的贴图 %llu 次（0 = 从 VkImage 读回那套暂时还只是工具）",
                (unsigned long long)g_sub_total, (unsigned long long)vk_tex_reads_gpu_written());
        }
    }
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
        // 缓冲区贴图单独一行：它是新补的一种资源（texel buffer），"绑上多少、建不出多少"直接说明它有没有生效
        // Buffer textures get their own line: this is a newly added resource kind (a texel buffer), and "how many
        // bound, how many could not be built" says directly whether it works
        LOG("[vk] 缓冲区贴图（texel buffer）：绑上 %llu 条、建不出视图 %llu 条",
            (unsigned long long)atomic_load(&g_desc_texel_ok), (unsigned long long)atomic_load(&g_desc_texel_fail));
        uint64_t vb = 0, vh = 0, vm = 0;
        vk_tex_view_stats(&vb, &vh, &vm);
        LOG("[vk] 视图：底视图 %llu 次、命中子视图 %llu 次、新建子视图 %llu 张",
            (unsigned long long)vb, (unsigned long long)vh, (unsigned long long)vm);
        // N4-b：**每帧搬运字节数**（11.3 的第一个字段）—— 新地基唯一的代价读数，单独一行。
        // N4-b: bytes uploaded per frame, the first of the six baseline fields, on its own line.
        if (g_vk2_gpu && g_vk2_frames)
            LOG("【地基】上传：%llu 帧里 %llu 帧搬过（%llu 个窗口）、共 %llu MB（最多一帧 %llu KB、"
                "平均每帧 %llu KB）；触碰命中 %llu / 未命中 %llu；GPU 直写后备标脏 %llu 次",
                (unsigned long long)g_vk2_frames, (unsigned long long)g_vk2_up_frames,
                (unsigned long long)g_vk2_up_windows, (unsigned long long)(g_vk2_up_bytes >> 20),
                (unsigned long long)(g_vk2_up_max >> 10),
                (unsigned long long)(g_vk2_up_bytes / (g_vk2_frames ? g_vk2_frames : 1) >> 10),
                (unsigned long long)g_vk2_touch_hit, (unsigned long long)g_vk2_touch_miss,
                (unsigned long long)g_vk2_gpu_writes);
        for (int k = 0; k < OP_COUNT_; k++) atomic_store(&g_opcount[k], 0);
    }
    return g_fence;
}

uint64_t mtl_last_fence(void) { return g_fence; }

// M3-2v：**客体的等栅栏**也记账 —— 这才是「M3-2u」停住现场里那一条（宿主 PC 符号化成 `nanosleep ← … ← mtl_wait`，
// 也就是驱动在 `vkWaitForFences` 里 poll）。它与 `inflight_take` 那条（槽池满时等最老的）不是一回事：
// 这条是客体自己按 NVN 的 fence 语义来等，一等等不到就是"这个提交永远没完成"。
// M3-2v: **the guest's own fence wait** is accounted for too -- that is the one in the "M3-2u" scene (the host PC
// symbolises to `nanosleep <- ... <- mtl_wait`, i.e. the driver polling inside `vkWaitForFences`). It is not the same
// thing as the `inflight_take` wait (which waits for the oldest slot when the pool is full): here the guest waits by
// NVN's own fence semantics, and a wait that never returns means "that submission never completes".
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (!vk_dev() || !fence || fence > g_fence) return 0;
    if (g_vk2_sub_ok) {
        // N4-c：客体按 NVN 的 fence 语义来等 —— 新地基下问的是 `vk2_sub` 的 frame：
        // ① `timeout_ns == 0` 是"看一眼"（`nvnSyncWait` 第一次调用就是这个）⇒ `vk2_sub_poll` **不阻塞**；
        // ② 带超时就等**那个 frame 自己的定向栅栏**（`vk2_sub_wait`，超时如实报失败）。
        // 注意：这里**不再**"滑出在飞窗口就直接当完成" —— 那张表由 `vk2_sub` 的 frame 序号与 `poll` 负责。
        // N4-c: the guest's own NVN fence wait now asks a vk2_sub frame: timeout 0 is a peek (vk2_sub_poll does not
        // block) and a real timeout waits on that frame's own fence (a timeout is reported as failure).
        uint64_t t0 = now_ns();
        int done = vk2_sub_poll(fence);
        if (done == 1) {
            if (timeout_ns && now_ns() - t0 > 300000000ull)
                LOG("[vk] 客体等栅栏：frame %llu 已经完成（等了 %llu 毫秒；新地基的定向栅栏）",
                    (unsigned long long)fence, (unsigned long long)((now_ns() - t0) / 1000000));
            return 1;
        }
        if (!timeout_ns) return 0;
        atomic_store(&g_led_fence_wait, 1); g_led_fence_slot = -1; g_led_fence_since = t0;
        dm_waitled_enter("mtl_wait：客体等围栏（N4-c：frame 自己的定向栅栏，**用客体给的超时**）");
        // **客体给多久就等多久**（不许拿模块的 5 秒上限去截短客体的语义 —— 第一轮真机读数抓到的就是这个：
        // 日志里 5005 毫秒正好等于模块那个上限，而 `nvnSyncWait` 要的是它自己的超时）。
        int ok = vk2_sub_wait_timeout(fence, timeout_ns);
        dm_waitled_leave();
        atomic_store(&g_led_fence_wait, 0);
        if (now_ns() - t0 > 300000000ull)
            LOG("[vk] 客体等栅栏：frame %llu 等了 %llu 毫秒（新地基的定向栅栏；客体给的超时 %llu 毫秒、返回 %d）",
                (unsigned long long)fence, (unsigned long long)((now_ns() - t0) / 1000000),
                (unsigned long long)(timeout_ns / 1000000), ok);
        return ok;
    }
    if (fence <= g_fence - MAX_INFLIGHT) { inflight_reap(); return 1; }   // 早已滑出在飞窗口 | long past the in-flight window
    for (int i = 0; i < MAX_INFLIGHT; i++)
        if (g_inf[i].pending && g_inf[i].value == fence) {
            uint64_t t0 = now_ns();
            atomic_store(&g_led_fence_wait, 1); g_led_fence_slot = i; g_led_fence_since = t0;
            // M3-2w：站点名把 timeout 的两种情形分开 —— `nvnSyncWait` 第一次调用是 `mtl_wait(fence, 0)`（本意"看一眼"），
            // 而 timeout_ns = 0 会回落到 1 秒（下一行），所以这两者在账本里必须是两个名字，否则"是谁在等"又糊了。
            // M3-2w: the site name splits the two timeout shapes -- `nvnSyncWait` first calls `mtl_wait(fence, 0)` (meant
            // as "just look") while timeout_ns = 0 falls back to 1 s (next line), so the ledger must name them apart or
            // "who is waiting" blurs again.
            dm_waitled_enter(timeout_ns ? "mtl_wait：客体等围栏（带超时）" : "mtl_wait：客体等围栏（timeout=0，落到 1 秒）");
            VkResult r = vkWaitForFences(vk_dev(), 1, &g_inf[i].fence, VK_TRUE, timeout_ns ? timeout_ns : 1000000000ull);
            dm_waitled_leave();
            atomic_store(&g_led_fence_wait, 0);
            if (now_ns() - t0 > 300000000ull)
                LOG("[vk] 客体等栅栏：槽 %d（栅栏值 %llu，已提交到 %llu）等了 %llu 毫秒、返回 %d（槽状态 %d、客体给的超时 %llu 毫秒）", i,
                    (unsigned long long)g_inf[i].value, (unsigned long long)g_fence,
                    (unsigned long long)((now_ns() - t0) / 1000000), (int)r, (int)vkGetFenceStatus(vk_dev(), g_inf[i].fence),
                    (unsigned long long)(timeout_ns / 1000000));
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

// ---------- DM_PASS_PROBE=<序号>：**通道级**对照实验（缺省关，诊断用；N4-e）----------
// 要回答的那句话：**"通道里的附件写入，到底有没有落到我们读回来的那张图上"**。
// 为什么非要有它：`DM_GFX_PROBE` 的清是 `vkCmdClearColorImage`（**在通道之外**、直接对图像），
// 它只能证明"我们的命令 → 图像 → 读回"这一段；而游戏那些绘制是**在通道里**写的 —— 一旦"通道挂的附件
// 不是我们读的那张图 / 那一级"（视图、mip、层、存储归属任一处不一致），就会出现"绘制全都下发了、
// 读回仍是零"而**不报任何错**（真机没有校验层，看不见）。这正是 N4-e 的扫描读数（13 次读成功全 0）
// 之后要分的那一半。
// 做法（**只**在这个序号上，其余一律不动）：
//   ① 以它为 `color0` 的通道，`loadOp` **强制**成 CLEAR + 已知色 `(0, 0.6, 1)`（⇒ 亮度 152/136，按格式的伽马）；
//   ② **`DM_PASS_PROBE=<序号>:end`** 时，通道收尾（`pass_end`）再用 `vkCmdClearAttachments` 把附件 0
//      清成同一个已知色 —— 于是这一帧它**最后的状态就是已知色**（后面的通道不会再动它）。
// 判读（两件事分开问，正是 N4-e 要分的那一刀）：
//   · **`:end` 那种**：读到已知色 ⇒ **通道附件写入能到达我们读回的那张图**（附件/视图/布局/存储归属这一层成立）
//     ⇒ 剩下的只能是"游戏那些绘制在附件里产不出片元"。读到 0 ⇒ 这一层不成立，先修这里。
//   · **不带 `:end`**（不清收尾）：通道**从已知色开始、游戏的绘制往上画** ⇒ 最后读到的内容就是"游戏画了什么"：
//     整片等于已知色 ⇒ **一个像素都没落下**；出现别的值 ⇒ **落下过**，读到的就是它写的颜色。
// 只清颜色附件 0，不碰深度/模板；`vkCmdClearAttachments` 是核心 1.0 入口（直接调，不必取设备指针）。
static uint64_t g_pp_serial;       // 0 = 关
static int g_pp_endclear;          // 1 = 通道收尾再补一刀（`:end`）
static int g_pp_armed;             // 这条通道上已经挂了探针
static uint32_t g_pp_w, g_pp_h;    // 补清那一次的矩形（附件有效尺寸）
// DM_RASTER_BRUTE 的另一半：把**动态视口/裁剪**铺满附件（光栅化侧那几项在 vk_pipe.c 里按同一个开关放开）。
// 为什么要它：视口/裁剪是"吃光片元"最直接的一处，而它们是**动态状态**、由客体经 OP_VIEWPORTS / OP_SCISSOR 设，
// 所以只能在记录绘制的这一刻覆盖（`pass_begin` 里那两行只在开通道时补一次，之后客体还能再改）。
// The other half of DM_RASTER_BRUTE: make the **dynamic viewport/scissor** cover the whole attachment (the
// other raster-state items are opened up in vk_pipe.c under the same switch). Viewport/scissor are the most
// direct way to lose every fragment, and they are dynamic state set by the guest via OP_VIEWPORTS/OP_SCISSOR,
// so they can only be overridden where the draw is recorded.
static uint32_t g_pass_w, g_pass_h;   // 本通道的渲染区（附件尺寸），brute 模式下当满屏视口用
// N4-h：本通道**附件自身的有效尺寸**（限了 mip 的视图要右移；与渲染区不是一回事）——"我们自己的三角形"
// 铺满附件时要用它（渲染区可能比附件小），读数里也要把两者都打出来才分得清"没落屏"是哪一种。
// N4-h: the attachment's own effective size for this pass (a mip-limited view shifts right; not the same thing as
// the render area). The diagnostic triangle fills this when covering the attachment, and both are logged.
static uint32_t g_att_w0, g_att_h0;
static uint64_t pass_probe_serial(void) {
    static int once;
    if (!once) {
        once = 1;
        const char *v = getenv("DM_PASS_PROBE");
        g_pp_serial = (v && *v) ? strtoull(v, NULL, 10) : 0;
        if (v && strstr(v, ":end")) g_pp_endclear = 1;
        if (g_pp_serial)
            LOG("【查】DM_PASS_PROBE=%llu%s：以它为 color0 的通道 loadOp 强制 CLEAR + 已知色 (0,0.6,1)%s"
                "（不带 `:end` ⇒ 通道从已知色开始、游戏的绘制往上画：整片仍是已知色 = 一个像素都没落下）",
                (unsigned long long)g_pp_serial, g_pp_endclear ? ":end" : "",
                g_pp_endclear ? "；并在通道收尾用 vkCmdClearAttachments 再补一刀（读到已知色 = 通道写入能到达读回的那张图）" : "");
    }
    return g_pp_serial;
}

static void pass_end(void) {
    if (!g_rs.open || g_cur_slot < 0) { g_rs.open = 0; g_pp_armed = 0; return; }
    gpu_pass_ts(g_cur_slot, g_inf[g_cur_slot].cb, 1);   // 结束时间戳要在收通道之前写（之后写就不在这段里了）| the end timestamp goes in before the pass is closed (afterwards it would be outside the pass)
    if (g_pp_armed && g_pp_endclear && g_pp_w && g_pp_h) {
        VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0,
                                 .clearValue = { .color = { { 0.0f, 0.6f, 1.0f, 1.0f } } } };
        VkClearRect r = { { { 0, 0 }, { g_pp_w, g_pp_h } }, 0, 1 };
        vkCmdClearAttachments(g_inf[g_cur_slot].cb, 1, &ca, 1, &r);
        static int said;
        if (!said++) LOG("【查】DM_PASS_PROBE：已在通道收尾对附件 0（%ux%u）补清成已知色（亮度 152/136）", g_pp_w, g_pp_h);
    }
    g_pp_armed = 0;
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

// ===========================================================================
// N4-l：把"描述符池满 ⇒ 这次绘制被**静默**跳过"变成每帧可读的读数（`DM_DESC_STATS=1`，**缺省关**）。
//
// 为什么先做这一件（N4-l 的第 0 条）：`desc_alloc` 拿不到集时只是返回 `VK_NULL_HANDLE`，调用方把那次绘制
// 跳过、只加一个 `g_desc_skip` —— 而 N4-k 的每一格判据都是"已知色底盘上出现非 152 = 落屏"，**"没画"与"画了
// 但没落下"在读数是同一个样子**。而且配额账本身就不够宽：`poolSize` 是**按类型**给的（每类型 512 个描述符），
// 一个集里有两三个同类型描述符时，512 个集的额度只够 floor(512/3) ≈ 170 个集 —— 真机每帧约 350 次绘制、
// 每次要 2 个集 ⇒ 缺的额度足够吞掉**半个帧**。
// 三个数：本帧（= 这次提交）分配了多少 / 失败多少（= 被静默跳过的绘制数）/ 这个槽的池里还剩多少额度。
// ===========================================================================
static _Atomic uint64_t g_desc_reqs, g_desc_fails, g_desc_poolnew, g_desc_poolkill;
static _Atomic uint64_t g_desc_used[MAX_INFLIGHT];   // 本槽已分配出去的集数（回收时清零）
static int desc_stats_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_DESC_STATS");
        v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        if (v) LOG("【查】DM_DESC_STATS=1：每帧打一行描述符池的账（分配 / **失败 = 被静默跳过的绘制** / 池新建与销毁 / "
                   "本槽剩余额度）—— `desc_alloc` 失败时调用方直接跳过这次绘制，而在\"已知色底盘上判落屏\"的读数里"
                   "\"没画\"与\"画了没落下\"长得一模一样");
    }
    return v;
}
static void desc_frame_stats(int slot) {
    if (!desc_stats_on()) return;
    static uint64_t last_reqs, last_fails;
    uint64_t rq = atomic_load(&g_desc_reqs), fl = atomic_load(&g_desc_fails);
    uint64_t used = (slot >= 0 && slot < MAX_INFLIGHT) ? atomic_load(&g_desc_used[slot]) : 0;
    LOG("【查】描述符池（槽 %d）：本帧分配 %llu / **失败 %llu**（失败 ⇒ 那次绘制被静默跳过）；"
        "累计 %llu / %llu；池 新建 %llu / 销毁 %llu；本槽已用 %llu 集（每池上限 %d 集 × %d 池，"
        "但额度是**按类型**给的 ⇒ 一个集里有 k 个同类型描述符时实际只够 %d/k 集）",
        slot, (unsigned long long)(rq - last_reqs), (unsigned long long)(fl - last_fails),
        (unsigned long long)rq, (unsigned long long)fl,
        (unsigned long long)atomic_load(&g_desc_poolnew), (unsigned long long)atomic_load(&g_desc_poolkill),
        (unsigned long long)used, DESC_POOL_SETS, DESC_POOLS, DESC_POOL_SETS);
    last_reqs = rq; last_fails = fl;
}
// 按**阶段**给 BIND_UBO 计数（含"地址为 0"的那些）：问的是"游戏到底往哪个阶段绑过常量"，
// 因为写空白的现场只能看出"这个槽没有地址"，看不出"是绑到了别的阶段、还是根本没绑"。
// Count BIND_UBO by **stage** (including the ones whose address is 0): the question is "which stage did the game bind
// constants for at all", because a blank's situation shows only "this slot has no address" and not whether it was
// bound under another stage or never bound.
static _Atomic uint64_t g_ubo_binds[6], g_ubo_zero[6], g_ubo_out;
static _Atomic uint64_t g_ssbo_binds[6];   // 同上，按阶段给 BIND_SSBO 计数（存储缓冲写空白的现场要看这个）| same for BIND_SSBO (needed by the storage-buffer blank's situation)

// ===========================================================================
// N4-l：`DM_DESC_POOL_BIG=1`（**缺省关**）—— 把每个槽的描述符池**按类型**放大。
//
// 为什么（N4-l 的第一个读数，`n4l-laya`）：**98 帧里累计 3009 次 `vkAllocateDescriptorSets` 失败、
// 末帧一次就是 791 次** —— 每一次失败都意味着**那一次绘制被静默跳过**（`desc_alloc` 返回 NULL ⇒ 调用方
// `nd = -1` ⇒ 不录这次绘制）。机制在配额上：`maxSets` 只决定"能开几个集"，而 `poolSize` 是**每种描述符**
// 的个数；一个集里有两个 UBO 时，512 个集的额度只够开 256 个集。游戏路径那一刻的布局是 **6 个动态描述符**
// （set0 2 + set1 4）⇒ 一个池连 128 个集都开不出来，而一帧有几百次绘制、**每次 2 个集**。
// 放大到 1024 集/池 × 每类型 3072 个（两个池 ⇒ 2048 集/槽/帧）。这是**修法**，所以留开关做 A/B。
// ===========================================================================
#define DESC_POOL_BIG_SETS 1024
#define DESC_POOL_BIG_QUOTA 3072u
static int desc_pool_big_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_DESC_POOL_BIG");
        // N4-m：**改成缺省开**（`DM_DESC_POOL_BIG=0` 退回旧行为）。
        // 理由与 `dyn_fix_on()` 同形：N4-l 把这条量成读数（106 帧 27756 次分配失败、≈262/帧被**静默**跳过），
        // 并且回答了"它不是画面全零的成因"（放大后仍 152）⇒ 按规范修掉（`maxSets` 与 `poolSize` 不是一回事），
        // 留 `=0` 让"修前 / 修后"仍能互切。
        v = (e && !strcmp(e, "0")) ? 0 : 1;
        LOG("【查】DM_DESC_POOL_BIG：**%s**（N4-m 起**缺省开**，`DM_DESC_POOL_BIG=0` 退回旧行为）—— "
            "描述符池 %s：%d 集/池 × 每类型 %u 个（两个池 ⇒ %d 集/槽/帧）；旧行为是 %d 集/池 × 每类型 %d 个，"
            "而游戏路径那一刻的布局有 6 个动态描述符 ⇒ 一个池只够开 %d 个集，一帧几百次绘制就是几百次「静默跳过」",
            v ? "开" : "关（旧行为）", v ? "放大" : "原样",
            v ? DESC_POOL_BIG_SETS : DESC_POOL_SETS,
            v ? DESC_POOL_BIG_QUOTA : (unsigned)DESC_POOL_SETS,
            v ? DESC_POOL_BIG_SETS * DESC_POOLS : DESC_POOL_SETS * DESC_POOLS,
            DESC_POOL_SETS, DESC_POOL_SETS, DESC_POOL_SETS / 6);
    }
    return v;
}

static void desc_slot_reset(int slot) {
    for (int k = 0; k < DESC_POOLS; k++)
        if (g_dpool[slot][k]) { vkDestroyDescriptorPool(vk_dev(), g_dpool[slot][k], NULL); g_dpool[slot][k] = VK_NULL_HANDLE;
                                atomic_fetch_add(&g_desc_poolkill, 1); }
    g_dpool_n[slot] = 0;
    if (slot >= 0 && slot < MAX_INFLIGHT) atomic_store(&g_desc_used[slot], 0);
}

static VkDescriptorPool desc_pool_at(int slot, int idx) {
    if (g_dpool[slot][idx]) return g_dpool[slot][idx];
    if (idx >= DESC_POOLS) return VK_NULL_HANDLE;
    // N4-l：`DM_DESC_POOL_BIG=1`（缺省关）——**按类型**给的额度放大（见下面 `desc_pool_big_on` 那段说明）
    // UNIFORM_TEXEL_BUFFER 是**缓冲区贴图**那一类绑定要的：描述符集布局里出现了它，池里就必须有对应的额度，
    // 否则 vkAllocateDescriptorSets 会失败（一个 set 也开不出来），而失败是静默的——每帧都从头再来一遍。
    // UNIFORM_TEXEL_BUFFER is for **buffer textures**: once the descriptor set layout contains that type the pool
    // must carry a matching quota, or vkAllocateDescriptorSets fails (not a single set can be opened) and the failure
    // is silent -- every frame would start over from square one.
    VkDescriptorPoolSize ps[5] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, desc_pool_big_on() ? DESC_POOL_BIG_QUOTA : DESC_POOL_SETS },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, desc_pool_big_on() ? DESC_POOL_BIG_QUOTA : DESC_POOL_SETS },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, desc_pool_big_on() ? DESC_POOL_BIG_QUOTA : DESC_POOL_SETS },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, desc_pool_big_on() ? 512u : 128u },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, desc_pool_big_on() ? DESC_POOL_BIG_QUOTA : DESC_POOL_SETS },
    };
    VkDescriptorPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = desc_pool_big_on() ? DESC_POOL_BIG_SETS : DESC_POOL_SETS, .poolSizeCount = 5, .pPoolSizes = ps };
    VkDescriptorPool p = VK_NULL_HANDLE;
    dm_waitled_enter("vk：建描述符池");
    VkResult dpres = vkCreateDescriptorPool(vk_dev(), &ci, NULL, &p);
    dm_waitled_leave();
    if (dpres != VK_SUCCESS) return VK_NULL_HANDLE;
    atomic_fetch_add(&g_desc_poolnew, 1);
    if (g_dpool_n[slot] < idx + 1) g_dpool_n[slot] = (uint8_t)(idx + 1);
    return (g_dpool[slot][idx] = p);
}

static VkDescriptorSet desc_alloc(int slot, VkDescriptorSetLayout lay) {
    atomic_fetch_add(&g_desc_reqs, 1);
    for (int idx = 0; idx < DESC_POOLS; idx++) {
        VkDescriptorPool p = desc_pool_at(slot, idx);
        if (!p) break;
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = p, .descriptorSetCount = 1, .pSetLayouts = &lay };
        VkDescriptorSet s = VK_NULL_HANDLE;
        dm_waitled_enter("vk：分配描述符集");
        VkResult das = vkAllocateDescriptorSets(vk_dev(), &ai, &s);
        dm_waitled_leave();
        if (das == VK_SUCCESS) {
            if (slot >= 0 && slot < MAX_INFLIGHT) atomic_fetch_add(&g_desc_used[slot], 1);
            return s;
        }
    }
    // N4-l：这条失败**以前是静默的**（返回 NULL ⇒ 调用方跳过这次绘制）。现在有读数：总量 + 每次前 8 条明细。
    uint64_t n = atomic_fetch_add(&g_desc_fails, 1) + 1;
    if (n <= 8 || (n & 1023) == 0) {
        uint64_t used = (slot >= 0 && slot < MAX_INFLIGHT) ? atomic_load(&g_desc_used[slot]) : 0;
        LOG("【黄】描述符集分配失败（第 %llu 次、槽 %d）：%d 个池都没额度了 ⇒ **这一次绘制被静默跳过**"
            "（本槽累计已用 %llu 集；额度是**按类型**给的，一个集里有 k 个同类型描述符时每池只够 %d/k 集）",
            (unsigned long long)n, slot, DESC_POOLS, (unsigned long long)used, DESC_POOL_SETS);
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
static VkBuffer g_bank[3];             // 0 = support_buffer、1 = c1/c2（暂全零）、2 = 驱动段 c0（存储缓冲地址表，活的）| 0 = support_buffer, 1 = c1/c2 (all zero for now), 2 = driver segment c0 (the live storage-buffer address table)
static uint8_t *g_bank_map[3];         // c0 的长期映射（见 bank_get）| c0's long-lived mapping (see bank_get)| 0 = support_buffer, 1 = all zero (c1 and the driver bank)
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
        // c0 是**活的表**（游戏每绑一个存储缓冲就要往里写地址），所以它长期保留映射；其余两块建好即封存。
        // c0 is a **live table** (an address is written into it whenever the game binds a storage buffer), so it keeps its
        // mapping; the other two are sealed once filled.
        if (which == 2) g_bank_map[which] = (uint8_t *)p; else vkUnmapMemory(vk_dev(), m);
    }
    g_bank[which] = b;
    const char *nm = which == 2 ? "驱动段 c0（存储缓冲地址表）" : (which ? "c1 / c2（常量与句柄段）" : "support_buffer");
    LOG("[vk] 特殊常量段 %s 就绪（%u 字节）：%s", nm, (unsigned)BANK_BYTES,
        which == 2 ? "按阶段基址 0x110/0x510/0x310 填已绑存储缓冲的地址" : (which ? "暂以全零代替" : "render_scale = 1"));
    return b;
}

// 把某个阶段某个槽的存储缓冲地址写进驱动段 c0 的地址表。
// 为什么需要：着色器不直接拿"存储缓冲槽 N"，而是从 c0 的表里读地址（表在各阶段的固定基址：顶点 0x110、像素 0x510、
// 计算 0x310，每槽 0x10 字节）。不填这张表，读到的就是 0 —— 现场里 `binding 25（uniform buffer）槽解析 0` 就是它。
// Write a stage/slot storage-buffer address into driver segment c0's address table.
// Why it is needed: a shader does not get "storage buffer slot N" directly, it reads the address out of c0's table (which sits
// at a fixed base per stage: vertex 0x110, pixel 0x510, compute 0x310, 0x10 bytes per slot). Leave it unfilled and the shader
// reads 0 -- which is exactly the `binding 25 (uniform buffer) slot 0` case on the device.
static void drv_c0_set(int stg, int idx, uint64_t addr) {
    static const uint32_t c0_base[6] = { 0x110, 0x510, 0, 0, 0, 0x310 };   // 0=顶点 1=像素 5=计算（其余阶段未实测）| 0 = vertex, 1 = pixel, 5 = compute (the rest unmeasured)
    if (stg < 0 || stg >= 6 || !c0_base[stg] || idx < 0 || idx >= 16) return;
    if (!g_bank_map[2] && !bank_get(2)) return;
    uint8_t *p = g_bank_map[2];
    if (!p) return;
    memcpy(p + c0_base[stg] + (size_t)idx * 16, &addr, 8);
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
        // 名字号 0 → 驱动段 c0（存储缓冲地址表）、1 → c1、2 → c2；没有 cbuf 条目 → support_buffer。都不是"游戏绑的 UBO"。
        // A name number of 0 is driver segment c0 (the storage-buffer address table), 1 is c1 and 2 is c2; no cbuf entry at
        // all is support_buffer. None of them is a UBO the game binds.
        if (slot <= 2) {
            if (!host_buf) return 0;
            *host_buf = bank_get(slot == 0 ? 2 : (slot >= 1 ? 1 : 0));
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

// A/B 开关（DM_NO_TEXEL）：缓冲区贴图这条路是照苹果侧移植过来的，装上真机之后必须能**一键退回空白**，
// 才能把"帧率 / 卡在哪里变了"判给这条改动，而不是判给手机自己（真机不稳是这个项目的常态）。
// 环境变量只在第一次读，之后一直用同一个答案。
// The A/B switch (DM_NO_TEXEL): the buffer-texture path is a port of the Apple one, and once it is on a real device it
// must be possible to **fall back to a blank with one flag**, so that "did the frame rate, or where it stalls, move"
// can be attributed to this change rather than to the phone itself (an unstable device is normal in this project).
// The variable is read once and the same answer is used thereafter.
static int texel_off_by_env(void) {
    static int once = -1;
    if (once < 0) { const char *v = getenv("DM_NO_TEXEL"); once = (v && *v && *v != '0') ? 1 : 0; }
    return once;
}

// ===========================================================================
// N4-k：`DM_UBO_RANGE=1`（**缺省关**）—— 把"指向池块"的 UBO 描述符的**范围**从 `VK_WHOLE_SIZE` 收到
// `min(maxUniformBufferRange, 块剩余字节)`。
//
// 为什么（本轮真机读数，N4-k）：`vsfull` 档（**我们自己的**全屏三角形 + **我们自己的**片段着色器 + 我们自己的
// 状态）三轮整片 85 = 落屏；把片段着色器换成**游戏那份**三轮整片 152 = 一个像素不落；而**我们自己写的**
// 一份"接口与游戏那份逐项相同、代码只有一句 `o0 = 纯红`"的片段着色器（`DM_TRI_FS_VARIANT=iface`）
// **也整片 152** ⇒ **不是着色器的代码，是它声明的绑定**。
// 三条绑定里唯一不合规的那一条就在这儿：`desc_write` 给 UBO 描述符写的范围是 **`VK_WHOLE_SIZE`**，
// 而缓冲是池的**块**（960 MB）⇒ 有效范围 960 MB ≫ `maxUniformBufferRange`（本机 65536）。
// 规范要求**uniform buffer 的范围不得超过 `maxUniformBufferRange`**；旧路（`make_pool_buffer`）的块本来就
// 带 UBO 位（N4-b 的批注把这条"块内小段另绑一个小缓冲"的合规做法留档待将来），而**描述符范围**这一层
// 一直没人查过。判据（写死）：叠在 `iface` 档上，**已知色底盘 152 上出现非 152 = 这条修法成立**。
static int ubo_range_clamp_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_UBO_RANGE");
        v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        if (v) LOG("【查】DM_UBO_RANGE=1：指向池块的 **UBO 描述符范围** 从 VK_WHOLE_SIZE 收到 "
                   "min(maxUniformBufferRange, 块剩余字节)。为什么：块是 960 MB，而 uniform buffer 的范围不得超过 "
                   "maxUniformBufferRange（本机 65536）—— 规范要求；本轮 N4-k 量到「绑上游戏的片段着色器"
                   "（或我们那份接口相同、代码极简的 FS）」时整片不落屏，而「不绑任何描述符」时落屏");
    }
    return v;
}

// `maxUniformBufferRange`（只问一次）
static uint64_t ubo_range_cap(void) {
    static uint64_t cap;
    if (!cap) {
        VkPhysicalDeviceProperties p;
        memset(&p, 0, sizeof p);
        if (vk_phys() != VK_NULL_HANDLE) vkGetPhysicalDeviceProperties(vk_phys(), &p);
        cap = p.limits.maxUniformBufferRange ? p.limits.maxUniformBufferRange : 65536u;
    }
    return cap;
}

// ===========================================================================
// N4-l：`DM_DYN_OFFSET_FIX=1`（**缺省关**）—— 让"布局里是 dynamic 的每一条绑定"**一定**消费一个动态偏移。
//
// 要回答的那句话（写死）：**"片段着色器读一条描述符就让这次绘制一个像素都不落，是不是因为
// `vkCmdBindDescriptorSets` 的 `dynamicOffsetCount` 比布局要求的少？"**
// 读代码得到的机制（本轮先量、改不改由读数定）：`desc_write` 只有"缓冲地址落在零拷贝池里"那一支才
// `dyn[nd++] = al`；另两支**不 push**：
//   · `hb`（support_buffer / c1 / c0 这些**宿主自己的**常量段，`desc_buf_range` 的 `slot <= 2` 那一支）；
//   · 空白回退（游戏没绑、或地址不在任何池里）。
// 而这两支的**描述符类型仍然是 `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`** ⇒ 布局里它们是动态描述符，
// 规范要求"dynamicOffsetCount = 绑定进来的这些 set 里动态描述符的总数"（VUID-vkCmdBindDescriptorSets-
// dynamicOffsetCount-00359）。少一个就是一次**非法**绑定；没有校验层时它的后果由驱动决定（Adreno 上完全
// 可能就是"这次绘制不产片元"）。
// 这正好与已有读数同形：**我们的纯红 FS 一条描述符都不声明**（布局里 0 个动态描述符、nd 也是 0 ⇒ 合法）
// ⇒ 落屏 85；**游戏的 FS 声明 binding 0（support_buffer，走 `hb`）与 binding 4** ⇒ 布局要 2 个、实际 push
// 1 个（甚至 0 个：那份 res.json 的 cbufs 是空的，两条都走 support_buffer）⇒ 非法。
// 判据：叠在**已知落屏**的档上（`vso` = 85 / `vsfullf` = 85），同一份 conf 只加这一行 ⇒ 出现非 152 = 成立。
// ===========================================================================
static int dyn_fix_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_DYN_OFFSET_FIX");
        // N4-m：**改成缺省开**（`DM_DYN_OFFSET_FIX=0` 退回旧行为）。
        // 为什么从"缺省关的 A/B 开关"变成"缺省修的规范问题"：N4-l 已经把它量成读数（游戏路径 `push 3 / 布局要 6`、
        // 每轮 14~596 次），并且把"它是不是画面全零的成因"答成**否**（打开后仍 152，而阶梯在同一情形下照样落屏）
        // ⇒ 它是一条**确定的非法绑定**（VUID-vkCmdBindDescriptorSets-dynamicOffsetCount-00359），按规范修掉；
        // 留 `=0` 只是为了让"修前 / 修后"仍能互切（§11.5 第 1 条）。
        v = (e && !strcmp(e, "0")) ? 0 : 1;
        LOG("【查】DM_DYN_OFFSET_FIX：**%s**（N4-m 起**缺省开**，`DM_DYN_OFFSET_FIX=0` 退回旧行为）—— "
            "布局里是 dynamic 的每一条绑定都消费一个动态偏移（宿主常量段与空白回退那两支原来不 push ⇒ "
            "dynamicOffsetCount 比布局要求的少，是一次非法绑定）", v ? "开" : "关（旧行为）");
    }
    return v;
}
// 一条动态偏移（带越界保护：`dyn[]` 与绑定表同样大小）
static void dyn_push(uint32_t *dyn, int *nd, uint32_t v) {
    if (*nd >= VK_SPV_MAXB) {
        static _Atomic uint64_t over;
        if (atomic_fetch_add(&over, 1) < 4) LOG("【红】动态偏移数组满了（>%d 条）：这一条没地方放", VK_SPV_MAXB);
        return;
    }
    dyn[(*nd)++] = v;
}

static int desc_write(VkDescriptorSet sets[2], const vk_shader *const *sh, int n,
                      const vk_pipe_bind *b, int nb, uint32_t *dyn) {
    // 一屏最多 64 条绑定，用静态数组（mtl_exec 递归，不能用大栈帧 | see the note on gfx_state_fill）
    static VkDescriptorBufferInfo bi[VK_SPV_MAXB];
    static VkDescriptorImageInfo ii[VK_SPV_MAXB];
    // 缓冲区贴图（texel buffer）那一类写的是**视图句柄**（pTexelBufferView），与图像/缓冲三选一，不能混用
    // A buffer texture (a texel buffer) is written as a **view handle** (pTexelBufferView), one of three mutually
    // exclusive forms alongside the image and buffer ones
    static VkBufferView bvr[VK_SPV_MAXB];
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
        // 这条绑定是缓冲区贴图吗（SPIR-V 的 Dim = Buffer → 见 vk_spv.c 的 kind_of）：它走的是 texel buffer，
        // 句柄→注册表那一段与图像共用，但"注册项 → 视图"那一步换成 VkBufferView。
        // Is this binding a buffer texture (the SPIR-V's Dim = Buffer -> see kind_of in vk_spv.c)? It goes through a
        // texel buffer: the handle-to-registry step is shared with images, but the "registry entry -> view" step
        // becomes a VkBufferView.
        int is_texel = (b[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
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
                // N4-l：类型没变（仍是 *_DYNAMIC）⇒ 布局里它是动态描述符 ⇒ 必须消费一个偏移（见 dyn_fix_on 那段）
                if (dyn_fix_on()) dyn_push(dyn, &nd, 0);
            } else if (hb) {
                // 特殊常量段：宿主自己的缓冲（描述符的 offset 就是 0；但它**仍然**是 dynamic 类型）
                // A special constant segment: the host's own buffer (descriptor offset is 0, but the type is still dynamic)
                bi[nw] = (VkDescriptorBufferInfo){ hb, 0, VK_WHOLE_SIZE };
                if (dyn_fix_on()) dyn_push(dyn, &nd, 0);
            } else {
                // N4-b：这条 UBO / SSBO 要被 GPU 读 ⇒ 保证后备是最新的（标 CPU 脏，帧开头搬它）。
                // 这是"绑定以 SPIR-V 反射为唯一真源"之外的另一件事：**读写的位置**由绑定决定，而"到没到 GPU
                // 眼前"由这一句负责（原则 2）。
                vk2_touch_cpu_(a, sz);
                // 动态偏移之和必须落在 minUniform/StorageBufferOffsetAlignment 的整数倍上，而规范保证这个
                // 界限不超过 256，所以按 256 对齐一定成立；客体自己的缓冲地址本该对齐，没对齐就记一次。
                // The sum of offset and dynamic offset must be a multiple of the alignment limit, which the spec
                // guarantees to be at most 256, so aligning to 256 always works; the guest's own buffer addresses
                // should be aligned and a misaligned one is reported once.
                uint64_t al = off & 255u;
                if (al) { static int said; if (!said++) LOG("【黄】缓冲地址没按 256 对齐（偏移 %llu）：按对齐下取整 + 动态偏移补", (unsigned long long)off); }
                // N4-k：**UBO 的范围**（`DM_UBO_RANGE=1`，缺省关）—— 见上面 `ubo_range_clamp_on()` 那段说明。
                // 块是 960 MB，而 `VK_WHOLE_SIZE` 会让这条 uniform buffer 的有效范围变成 960 MB
                // ≫ `maxUniformBufferRange`（本机 65536）⇒ 规范不允许；本轮量到"只要片段着色器真的**读**这条
                // UBO，这次绘制就一个像素都不落"。收到 `min(上限, 块剩余字节)` 之后就是一条合规的描述符。
                uint64_t rng = VK_WHOLE_SIZE;
                if (ubo_range_clamp_on() && b[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
                    uint64_t cap = ubo_range_cap();
                    uint64_t base = off - al;
                    uint64_t bs = vk2_buf_bytes((VkBuffer)buf);       // 0 = 不是池块（宿主自己的缓冲，照旧）
                    if (bs) {
                        uint64_t rem = bs > base ? bs - base : 0;
                        rng = (rem && rem < cap) ? rem : cap;
                        static _Atomic uint64_t ncl;
                        uint64_t k = atomic_fetch_add(&ncl, 1) + 1;
                        if (k <= 4 || (k & 4095) == 0)
                            LOG("【查】DM_UBO_RANGE：binding %u 的 UBO 范围 %llu 字节（块 %llu、块内偏移 %llu；"
                                "上限 %llu）—— 第 %llu 条（VK_WHOLE_SIZE 在这里是 960 MB，超 maxUniformBufferRange）",
                                b[i].binding, (unsigned long long)rng, (unsigned long long)bs,
                                (unsigned long long)base, (unsigned long long)cap, (unsigned long long)k);
                    }
                }
                bi[nw] = (VkDescriptorBufferInfo){ (VkBuffer)buf, off - al, rng };
                dyn_push(dyn, &nd, (uint32_t)al);
            }
            w.descriptorType = b[i].type;
            w.pBufferInfo = &bi[nw];
            nw++;
        } else if (b[i].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b[i].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                   b[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
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
            VkBufferView texbv = VK_NULL_HANDLE;   // 缓冲区贴图那一支的产物 | what the buffer-texture branch produces
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
                        if (is_texel && texel_off_by_env()) {
                            // A/B 开关（DM_NO_TEXEL=1）：把缓冲区贴图退回空白，用来判定"帧率变化"是不是这条新路带来的。
                            // 它是量具不是功能：默认关，只有把环境变量设上才生效。
                            // An A/B switch (DM_NO_TEXEL=1): fall a buffer texture back to a blank so that "did the frame
                            // rate change" can be settled. It is an instrument rather than a feature: off by default, and
                            // it only bites when the variable is set.
                            atomic_fetch_add(&g_desc_texel_fail, 1);
                        } else if (is_texel) {
                            // 缓冲区贴图：这一支**不碰图像视图**。那类贴图的 target 是 10（缓冲），图像那一侧
                            // 永远建不出东西——真机上"贴图写空白"里正有这一批（离线统计：反射里 cbuf_slot<0 且
                            // handle=0x6a 的绑定 2991 条，其 SPIR-V 维度全是 Buffer）。
                            // 成功这条日志同时回答交接单里那条判定动作："注册项的目标确实是 10 吗"。
                            // A buffer texture: this branch **never touches an image view**. Such a texture's target
                            // is 10 (buffer) and the image side can never build anything -- and on the device this
                            // batch is exactly part of the "texture blank" lines (offline count: 2991 bindings in the
                            // reflection have cbuf_slot < 0 and handle 0x6a, all with an SPIR-V Dim of Buffer).
                            // The success line also answers the hand-off's deciding question: "is the registry
                            // entry's target really 10?"
                            texbv = vk_tex_bufview(&e->t);
                            if (texbv) {
                                atomic_fetch_add(&g_desc_tex_ok, 1);
                                if (atomic_fetch_add(&g_desc_texel_ok, 1) < 4)
                                    LOG("【查】缓冲区贴图：binding %u（%s）tid %u → 注册项目标 %d、格式 %#x、元素 %u：texel buffer 已绑上",
                                        b[i].binding, vk_spv_kind_name(b[i].kind), (unsigned)(h & 0xfffffu),
                                        e->t.d.target, e->t.d.format, e->t.d.width);
                            } else if (atomic_fetch_add(&g_desc_texel_fail, 1) < 4) {
                                LOG("【黄】缓冲区贴图：binding %u（%s）tid %u 建不出视图（注册项目标 %d、格式 %#x、元素 %u）：原因见纹理层那几行报红",
                                    b[i].binding, vk_spv_kind_name(b[i].kind), (unsigned)(h & 0xfffffu),
                                    e->t.d.target, e->t.d.format, e->t.d.width);
                            }
                        } else {
                        // 图像 / 图像视图那一支（原来的路；块里不另行缩进，为的是让这回的差异只落在"缓冲区贴图"这一支上）。
                        // The image / image-view branch (the original path; its body is left unindented so that this
                        // round's diff lands only on the buffer-texture branch).
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
            }
            // 配不上时取证（前三次）：缓冲区贴图那一支的"有产物"以 texbv 计——不然它每次都会被当成"配不上"
            // Forensics when unpaired (first three): the buffer-texture branch counts texbv as a product, or it would
            // be treated as unpaired every time
            if (!texv && !texbv && tr) desc_tex_probe(st, tr);
            if (!texv && !texbv) {
                // 用**自己的**计数器：早先和图"拿不到缓冲地址"共用 g_desc_blank，前 8 条被 UBO 的空白占掉，
                // 于是贴图这条诊断在真机上一行都没打出来（查了半天）。
                // Its **own** counter: it used to share g_desc_blank with the "buffer address unavailable" case, so
                // the first eight were taken by UBO blanks and this texture diagnostic never printed a single line on
                // the device (which cost a while to figure out).
                if (atomic_fetch_add(&g_desc_tex_blank, 1) < 8) {
                    int stg = st->stage >= 0 && st->stage < 6 ? st->stage : 0;
                    // 差的那条读数：这处空白到底是"槽没绑"还是"绑的句柄 tid 不在注册表"。
                    // res.json 的 handle **不是贴图号**，是 c2 的字偏移 → 解成贴图槽（见 vk_shader.c 的 decode_slots）：
                    // 这里把"解出来的槽"、"该槽上绑的句柄"、"注册表里查不查得到"一起打出来。
                    // The missing reading: is this blank "the slot was never bound" or "the bound handle's tid is not in the
                    // registry"? res.json's handle is **not a texture id** but a c2 word offset -> resolved into a texture
                    // slot (see decode_slots in vk_shader.c); this prints the resolved slot, the handle bound on it, and
                    // whether the registry has it.
                    int nsl = tr ? tr->nvn_slot : -1;
                    uint64_t bh = (nsl >= 0 && nsl < 128) ? g_rs.tex[stg][nsl] : 0;
                    LOG("【黄】binding %u（%s）贴图写空白：res.json 说 cbuf_slot=%d handle=0x%x → 槽 %d；该槽绑的句柄 %#llx（低 20 位 %u，注册表 %s），注册表共 %llu 项；该阶段贴图句柄 %llx/%llx/%llx、常量段 %llx/%llx/%llx/%llx",
                        b[i].binding, vk_spv_kind_name(b[i].kind), tr ? tr->cbuf_slot : -999, tr ? (unsigned)tr->handle : 0u,
                        nsl, (unsigned long long)bh, (unsigned)(bh & 0xfffffu),
                        bh && treg_of((uint32_t)bh) ? "命中" : "不在", (unsigned long long)atomic_load(&g_treg_n),
                        (unsigned long long)g_rs.tex[stg][0], (unsigned long long)g_rs.tex[stg][1], (unsigned long long)g_rs.tex[stg][2],
                        (unsigned long long)g_rs.ubo[stg][0].addr, (unsigned long long)g_rs.ubo[stg][1].addr,
                        (unsigned long long)g_rs.ubo[stg][2].addr, (unsigned long long)g_rs.ubo[stg][3].addr);
                }
                // 缓冲区贴图的空白：给一个全零元素（NVN 在空槽上就是读 0），别的走图像 + 采样器那一对
                // A buffer texture's blank: one all-zero element (NVN reads 0 on an empty slot); everything else gets
                // the image-plus-sampler pair
                if (is_texel) bvr[nw] = vk_pipe_dummy_bufview();
                else ii[nw] = (VkDescriptorImageInfo){ vk_pipe_dummy_sampler(), vk_pipe_dummy_view(),
                                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            } else if (is_texel) {
                bvr[nw] = texbv;
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
            // 三种"视图"各自只填一个成员（见 VkWriteDescriptorSet 的说明）：texel buffer 用 pTexelBufferView
            // Exactly one member is filled per form (see VkWriteDescriptorSet): a texel buffer uses pTexelBufferView
            if (is_texel) w.pTexelBufferView = &bvr[nw];
            else w.pImageInfo = &ii[nw];
            nw++;
        } else {
            continue;   // VK_SPV_OTHER 之类：布局里没这条就走不到这里 | kinds such as VK_SPV_OTHER never reach here for a binding that is in the layout
        }
        wr[nw - 1] = w;   // nw 已经加过，所以回填的是刚写的那条 | nw was already incremented, so this fills in the entry just written
    }
    if (nw) { dm_waitled_enter("vk：更新描述符集（每帧）"); vkUpdateDescriptorSets(vk_dev(), (uint32_t)nw, wr, 0, NULL); dm_waitled_leave(); }
    return nd;
}

// ===========================================================================
// N4-l：绑定描述符集，**顺手把两个数对一下**：这次 push 了几个动态偏移 vs 布局里有多少个动态描述符。
// 只读（不改行为）；不相等就把这条读数打出来（前 6 次 + 之后每 1024 次），因为
//   · 规范要求相等（VUID-vkCmdBindDescriptorSets-dynamicOffsetCount-00359）；
//   · 没有校验层时后果由驱动定，而"这次绘制不产片元"是这台 Adreno 上已被证明会发生的一种后果形态。
// `DM_LAYOUT_DUMP=1` 时同时把这一对布局逐条摊平（同一个键只打一次）。
// ===========================================================================
static void desc_bind(VkCommandBuffer cb, VkPipelineLayout lay, VkDescriptorSet sets[2],
                      const vk_shader *const *pair, int npair, int nd, const uint32_t *dyn, const char *where,
                      int site) {
    vk_pipe_layout_dump(pair, npair, where);
    int want = vk_pipe_layout_dyn_count(pair, npair);
    if (want >= 0 && want != nd) {
        // 按**站点**分别计数（N4-l 第一版共用一个计数，结果"阶梯"那一处的失败被"游戏路径"的前 6 条吃掉了）
        static _Atomic uint64_t mism[4];
        int si = (site >= 0 && site < 4) ? site : 3;
        uint64_t n = atomic_fetch_add(&mism[si], 1) + 1;
        if (n <= 6 || (n & 255) == 0)
            LOG("【红】动态偏移个数与布局不符（%s，第 %llu 次）：这次 push 了 %d 个、布局要求 %d 个"
                "⇒ `vkCmdBindDescriptorSets` 的 dynamicOffsetCount 不合法（VUID-…-dynamicOffsetCount-00359）；"
                "没有校验层时后果由驱动定。为什么少：`desc_write` 只有「缓冲地址落在池里」那一支才 push 偏移，"
                "而 support_buffer / c1 / c0（宿主自己的常量段）与空白回退这两支**不 push**，"
                "可它们在布局里**仍然是 dynamic** —— 修法是 `DM_DYN_OFFSET_FIX=1`",
                where, (unsigned long long)n, nd, want);
    }
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, 2, sets, (uint32_t)nd, dyn);
}

// ========== 渲染区取证：算出多少、附件多大、谁说了算（优化清单第 3 项）==========
// 苹果侧开通道时**没有"渲染区"这个参数**（`MTL4RenderPassDescriptor` 不接收矩形），它的参考尺寸由
// `pass_dims`（nvn_mtl.m:1386-1395）从**附件尺寸**（按 mip 级）取最小值得到，**完全不读视口**。
// 安卓侧这里却是"视口的 x+width / y+height 与裁剪取小"（见 pass_begin），两者语义不同：视口一旦有偏移
// （NVN 不保证它是 0），算出的渲染区就比附件小，而**渲染区外的片元会被直接丢弃** —— 真机没有校验层，
// 不会报错。"画了却一个像素不落"正是这个形状，所以先读数、后改动。
// DM_PASS_AREA=1 只读数（不改行为）；=2 渲染区改用附件尺寸（与苹果侧同源，对照实验）。
// The render-area trace (optimisation item 3). Apple's side has no render-area parameter at all
// (`MTL4RenderPassDescriptor` takes no rectangle): its reference size comes from `pass_dims` (nvn_mtl.m:1386-1395),
// which takes the **attachment** sizes (per mip level) and **never reads the viewport**. The Android side here
// derives it from the viewport's x+width / y+height min'd with the scissor instead, and the two do not agree: once
// the viewport carries an offset (NVN does not promise zero) the computed area is smaller than the attachment, and
// **fragments outside the render area are discarded outright** -- with no validation layers to say so. That is
// exactly the shape of "drew, and not one pixel landed", so the numbers come first and the change second.
// DM_PASS_AREA=1 measures only (no behaviour change); =2 uses the attachment size instead (same origin as Apple,
// an A/B experiment).
static int pass_area_mode(void) {
    static int once = -1;
    if (once < 0) {
        const char *v = getenv("DM_PASS_AREA");
        once = (v && *v) ? atoi(v) : 0;
        if (once) LOG("【查】DM_PASS_AREA=%d：%s", once,
                      once >= 2 ? "渲染区改用附件尺寸（与苹果侧同源，对照实验）" : "只打印渲染区 / 附件 / 视口 / 裁剪，不改行为");
    }
    return once;
}
static _Atomic uint64_t g_pa_n, g_pa_small, g_pa_tiny, g_pa_novp, g_pa_off;

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
        // DM_PASS_PROBE：color0 命中时把 loadOp 强制成"清成已知色"（见 pass_probe_serial 的说明）
        int probe = (i == 0 && pass_probe_serial() && t->serial == pass_probe_serial());
        if (probe) { clr = 1; g_pp_armed = 1; }
        VkRenderingAttachmentInfo a = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = iv, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = clr ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = { .color = { { clr ? (probe ? 0.0f : g_rs.cc[i][0]) : 0.f,
                                        clr ? (probe ? 0.6f : g_rs.cc[i][1]) : 0.f,
                                        clr ? (probe ? 1.0f : g_rs.cc[i][2]) : 0.f,
                                        clr ? (probe ? 1.0f : g_rs.cc[i][3]) : 0.f } } } };
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
    // DM_PASS_PROBE：这个矩形要落在附件里（与渲染区同源，用附件有效尺寸）
    if (g_pp_armed) { g_pp_w = att_w; g_pp_h = att_h; }
    g_att_w0 = att_w; g_att_h0 = att_h;   // N4-h：附件自身的有效尺寸（"我们自己的三角形"铺满用）
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
    // DM_RASTER_BRUTE 用：本通道的渲染区（= 附件尺寸）当满屏视口/裁剪用（见 g_pass_w/g_pass_h 的说明）
    g_pass_w = w; g_pass_h = h;
    // ---- 渲染区取证 + 对照（说明见 pass_area_mode）----
    // 四个累计数各回答一件事：比附件小（渲染区外的片元被丢）、退成 1x1 级（几乎什么都不画）、
    // 开通道时**根本没设视口**（那份状态还没到，渲染区只能由裁剪或 1x1 兜底）、视口带偏移（"x+width" 算法失真）。
    // Four counters, each answering one thing: smaller than the attachment (fragments outside are discarded),
    // collapsed to 1x1 (almost nothing is drawn), **no viewport set at all** when the pass opened (that state has
    // not arrived yet, so only the scissor or the 1x1 fallback is left), and an offset viewport (which is what
    // distorts the "x+width" arithmetic).
    int pa = pass_area_mode();
    if (pa) {
        const int has_vp = g_rs.nvp ? 1 : 0;
        const float vpx = has_vp ? g_rs.vp[0].x : 0.f, vpy = has_vp ? g_rs.vp[0].y : 0.f;
        const float vpw = has_vp ? g_rs.vp[0].width : 0.f, vph = has_vp ? g_rs.vp[0].height : 0.f;
        const int small = (att_w && w < att_w) || (att_h && h < att_h);
        const int tiny = (w <= 1 || h <= 1);
        const int off = has_vp && (vpx != 0.f || vpy != 0.f);
        uint64_t n = atomic_fetch_add(&g_pa_n, 1) + 1;
        if (small) atomic_fetch_add(&g_pa_small, 1);
        if (tiny) atomic_fetch_add(&g_pa_tiny, 1);
        if (!has_vp) atomic_fetch_add(&g_pa_novp, 1);
        if (off) atomic_fetch_add(&g_pa_off, 1);
        if (n <= 8 || (n & 1023) == 0)
            LOG("【查】渲染区 %ux%u / 附件 %ux%u / 视口 %.0fx%.0f@(%.0f,%.0f)%s / 裁剪 %ux%u@(%d,%d)%s%s（第 %llu 次；比附件小累计 %llu、退成 1x1 累计 %llu、开通道时没视口 %llu、视口带偏移 %llu）",
                w, h, att_w, att_h, vpw, vph, vpx, vpy, has_vp ? "" : "（未设）",
                g_rs.nsc ? g_rs.sc[0].extent.width : 0u, g_rs.nsc ? g_rs.sc[0].extent.height : 0u,
                g_rs.nsc ? (int)g_rs.sc[0].offset.x : 0, g_rs.nsc ? (int)g_rs.sc[0].offset.y : 0,
                small ? " ← 比附件小" : "", tiny ? " ← 退成 1x1 级" : "",
                (unsigned long long)n,
                (unsigned long long)atomic_load(&g_pa_small), (unsigned long long)atomic_load(&g_pa_tiny),
                (unsigned long long)atomic_load(&g_pa_novp), (unsigned long long)atomic_load(&g_pa_off));
        // =2：渲染区改用附件尺寸（与苹果侧 pass_dims 同源）
        if (pa >= 2 && att_w && att_h) { w = att_w; h = att_h; }
    }
    ri.renderArea.extent = (VkExtent2D){ w, h };
    if (g_rs.nvp) vkCmdSetViewport(cb, 0, (uint32_t)g_rs.nvp, g_rs.vp);
    if (g_rs.nsc) vkCmdSetScissor(cb, 0, (uint32_t)g_rs.nsc, g_rs.sc);
    dm_vkCmdBeginRendering(cb, &ri);
    gpu_pass_ts(g_cur_slot, cb, 0);   // 通道真的开成了，写起始时间戳 | the pass really opened, so write the start timestamp   // 桩库不导出，走 vkGetDeviceProcAddr 取的指针（见 vk_device.h）| not exported by the stub library, so it goes through the pointer fetched with vkGetDeviceProcAddr (see vk_device.h)
    g_rs.open = 1;
    atomic_fetch_add(&g_pass_n, 1);
    // N4-h：新的一段通道 ⇒ "我们自己的三角形"在这段通道里还没插过（每段通道只插一次；缺省关时只清一个标志）
    vk_tri_game_pass_begin();
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

// ---------------------------------------------------------------------------
// N4-j：`DM_POLY_MODE_NVN=1`（**缺省关**）—— 多边形模式按 **NVN 的枚举**映射到 Vulkan。
// 这是一条**真 bug 的修法**（旧路新路共有），不是新功能：NVN 的取值是 **POINT=0 / LINE=1 / FILL=2**
// （`dm/gfx/nvn/include/nvn.h:1059-1065` 是权威枚举），而 Vulkan 是 **FILL=0 / LINE=1 / POINT=2**。
// 改前那一行是 `out->poly = (VkPolygonMode)(po.mode <= 2 ? po.mode : 2)` —— **把 NVN 的值直接当
// VkPolygonMode 用** ⇒ 游戏那句 `nvnPolygonStateSetPolygonMode(s, NVN_POLYGON_MODE_FILL)`（= 2）落成
// **VK_POLYGON_MODE_POINT**：**每个三角形只光栅化成顶点那几个像素** ⇒ 整片目标一个非底色像素都没有。
// 这条与全部旧读数相容：校验层**不报**（POINT 是合法模式、设备也支持非实心填充）、`DM_RASTER_BRUTE`
// 不碰它（所以"光栅化侧全放开仍全零"）、顶点/常量/着色器都是好的、我们自己的三角形用 FILL 所以能落屏。
// 口径照苹果侧（`nvn_mtl.m:4400`：`mode == NVN_POLYGON_MODE_LINE ? Lines : Fill`）：**只有 LINE 映射成
// LINE、其余一律 FILL**（Metal 没有点填充；Vulkan 有，但游戏那句 FILL 不该变成 POINT）。
// 缺省关 ⇒ 旧行为一字不变，可做"只差一行"的单变量对照。
static int poly_mode_nvn_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_POLY_MODE_NVN");
        v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        if (v) LOG("【查】DM_POLY_MODE_NVN=1：多边形模式按 NVN 的枚举映射到 Vulkan —— NVN 是 "
                   "POINT=0 / LINE=1 / **FILL=2**（nvn.h:1059-1065）、Vulkan 是 FILL=0 / LINE=1 / POINT=2；"
                   "改前是直接把 NVN 的值当 VkPolygonMode 用 ⇒ 游戏的 `SetPolygonMode(FILL)`=2 落成 "
                   "**VK_POLYGON_MODE_POINT**。本开关照苹果侧 nvn_mtl.m:4400 的口径：只有 LINE → LINE、"
                   "其余一律 FILL");
    }
    return v;
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
    // ★ N4-j：**这条是"整片目标一个像素都没有"的成因**（见上面 poly_mode_nvn_on 那段）。
    // 缺省（关）= 原样直搬（旧行为）；`DM_POLY_MODE_NVN=1` 才按 NVN 的枚举正确映射。
    if (poly_mode_nvn_on()) {
        static _Atomic uint64_t cnt[4];
        uint32_t m2 = (uint32_t)(po.mode & 3);
        uint64_t c = atomic_fetch_add(&cnt[m2], 1) + 1;
        // 自证：**每一种取值的前两次** + 之后每 4096 次各一条 —— 第一版只在"不是 FILL"时才打，
        // 于是"游戏一直在用 FILL"这件事在日志里**看不见**（坑 ㉒ 的同一类：只把自己以为稀罕的那一种打出来）。
        if (c <= 2 || (c & 4095) == 0)
            LOG("【查】DM_POLY_MODE_NVN：游戏这一次的多边形模式 = NVN %u（%s）⇒ 落成 VkPolygonMode %s"
                "（这一类累计 %llu 次）",
                m2, m2 == NVN_POLYGON_MODE_POINT ? "POINT" : m2 == NVN_POLYGON_MODE_LINE ? "LINE"
                     : m2 == NVN_POLYGON_MODE_FILL ? "FILL" : "（越界）",
                m2 == NVN_POLYGON_MODE_LINE ? "LINE" : "FILL", (unsigned long long)c);
        out->poly = (m2 == (uint32_t)NVN_POLYGON_MODE_LINE) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    } else {
        out->poly = (VkPolygonMode)(po.mode <= 2 ? po.mode : 2);
    }
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

// N4-j：把一个 `vk_gfx_state` 的**全部**字段摊成一行（自证用）。为什么要它：N4-i 的自证行只打了
// 拓扑 / 写掩码[0] / 混合[0] 的 en 与 rgb 系数 / 深度 test,write / 剔除 —— 而 `frontFace`、三个偏置系数、
// `a2c`、`src_a`/`dst_a`/`eq_*` 那一组、`poly`、**每个目标的**掩码与混合、模板那一整组**都没打**；
// 本轮要的正是"逐字段对照两边（游戏那一刻 vs 我们这条管线）"，漏打一格就可能把答案漏掉（坑 ㉒：
// "只看第一条 / 第一个"的读数会直接改变结论）。所以这里按**全部字段**摊平，两边各打一行。
static int gfx_state_desc(char *b, size_t cap, const vk_gfx_state *s) {
    if (!b || !cap || !s) return 0;
    uint32_t ben = 0, bm = 0;
    for (int i = 0; i < 8; i++) {
        if (s->blend_en[i]) ben |= 1u << i;
        bm |= (uint32_t)(s->write_mask[i] & 0xf) << (4 * i);
    }
    return snprintf(b, cap,
        "拓扑 %d；写掩码 0x%x（逐目标 %#x/%#x/%#x/%#x/%#x/%#x/%#x/%#x）；采样 %u；a2c %u；"
        "混合 en 0x%x —— 目标0：%d→%d 颜色 op %d、%d→%d alpha op %d；"
        "深度 test %u write %u op %d、格式 %d/%d；"
        "模板 en %u func %d/%d 读掩码 %#x/%#x 写掩码 %#x/%#x 失败 %d/%d 深度失败 %d/%d 通过 %d/%d；"
        "剔除 %d 前向面 %d 多边形 %d；偏置 en %u 系数 %.3f/%.3f/%.3f",
        (int)s->topo, bm,
        s->write_mask[0], s->write_mask[1], s->write_mask[2], s->write_mask[3],
        s->write_mask[4], s->write_mask[5], s->write_mask[6], s->write_mask[7],
        s->samples, s->a2c, ben,
        (int)s->src_rgb[0], (int)s->dst_rgb[0], (int)s->eq_rgb[0],
        (int)s->src_a[0], (int)s->dst_a[0], (int)s->eq_a[0],
        s->depth_test, s->depth_write, (int)s->depth_op, (int)s->depth_fmt, (int)s->stencil_fmt,
        s->stencil_en, (int)s->stencil_func[0], (int)s->stencil_func[1],
        s->stencil_vmask[0], s->stencil_vmask[1], s->stencil_wmask[0], s->stencil_wmask[1],
        (int)s->stencil_fail[0], (int)s->stencil_fail[1],
        (int)s->stencil_zfail[0], (int)s->stencil_zfail[1],
        (int)s->stencil_zpass[0], (int)s->stencil_zpass[1],
        (int)s->cull, (int)s->front, (int)s->poly,
        s->bias_en, (double)s->bias_factor, (double)s->bias_units, (double)s->bias_clamp);
}

// ============================================================================
// N4-i：二分阶梯的"插入"这一半（开关与门在 vk_tri_game.c；这里把某一档真的画出来）
// ============================================================================
// 要回答的那句话、四档的定义与判据都写在 vk_tri_game.c 的文件头（那里是"设计"），这里只做"用游戏那一刻的
// 上下文把这一档画出来"：**我们那条已经证明能落屏的管线当标尺**，一次只多接一样游戏的东西。
// 为什么这一半必须放这儿：游戏的上下文（影子状态 `g_rs`、描述符分配/写入、`mtl_addr` 反查、当前槽）全在
// 这个编译单元里，而 `desc_alloc` / `desc_write` 是它的静态函数 —— 与游戏那条路径**用同一段代码**才算单变量。
static _Atomic uint64_t g_n4i_skipfmt, g_n4i_skipnodraw, g_n4i_skipdesc, g_n4i_skipvb, g_n4i_logs, g_n4i_skipshape;

// ---------------------------------------------------------------------------
// N4-i：`DM_PIPEKEY=1`（**缺省关**）——把"这一对着色器"写进图形管线状态的键里。
// 为什么它是一条**修法**而不是新功能：`vk_gfx_state` 头两个字段就是 `vs_key` / `ps_key`（`vk_pipe.h:85`），
// 而缓存是"整块状态做哈希 + memcmp"（`vk_pipe.c:520-525`）—— 可是回放路径上唯一的填表处
// `gfx_state_fill()` 是 `memset(out,0,...)` 之后**只填状态**、**从不填这两个键**（全库只有 `vk_pipe_probe.c`
// 填过）⇒ 那个 `memcmp` 实际上**不区分子着色器对**：同一批状态字节上谁先建管线谁赢，后来"状态一样、
// 着色器不同"的绘制会拿到**别人那一对的管线**。
// 这一点与已有读数全都相容：A1 的校验层**不报**（管线与通道是兼容的，只是着色器不是它要的）、
// N4-e 把光栅化侧全放开也没用（错的着色器算出来的位置在裁剪体之外，跟状态无关）、
// N4-f 的顶点数据与常量都正常（输入是对的，**管线**是错的）、N4-h 我们自己那条管线（不走这个缓存）能落屏。
// 判据（本轮写死）：`DM_PIPEKEY_PROBE=1` 的计数 > 0（先量）；打开本开关后 `DM_PASS_PROBE=28` 的已知色
// 底子上**出现非 152 的值**（后改）。
static int pipekey_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_PIPEKEY");
        v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        if (v) LOG("【查】DM_PIPEKEY=1：把这一对着色器**写进图形管线状态的键**（`vs_key`/`ps_key`）——"
                   "改前这两个字段在回放路径上从没被填过，缓存因此不区分子着色器（只读的计数见 "
                   "`DM_PIPEKEY_PROBE=1`；打开本开关后**图形管线条数会明显变多**，那是这条修法的直接代价）");
    }
    return v;
}


static void n4i_ladder_draw(VkCommandBuffer cb, uint64_t draw_index, const vk_gfx_state *st,
                            const vk_shader *gvs, const vk_shader *gps,
                            const void *payload, int op, uint32_t att_w, uint32_t att_h,
                            const VkViewport *gvp, int nvp, const VkRect2D *gsc, int nsc) {
    int step = vk_tri_ladder_mode();
    if (!step || !st) return;
    const char *sname = vk_tri_ladder_name(step);
    const vk_shader *vs_idx = vk_tri_ladder_shader(0);   // 全屏三角形（只读 gl_VertexIndex）
    const vk_shader *vs_vb = vk_tri_ladder_shader(1);    // 读游戏顶点缓冲（Location 0）
    const vk_shader *fs = vk_tri_ladder_shader(2);       // 纯红
    if (!vs_idx || !vs_vb || !fs) { atomic_fetch_add(&g_n4i_skipnodraw, 1); return; }
    if (!st->color_count || !att_w || !att_h) { atomic_fetch_add(&g_n4i_skipfmt, 1); return; }

    // ---- 这一次绘制的计数：与游戏路径**同一份载荷、同一套读法** ----
    uint32_t count = 0, instances = 1, first = 0, base_vertex = 0, base_instance = 0;
    int indexed = (op == OP_DRAW_IDX);
    uint64_t ibuf = 0;
    int itype = -1;
    if (indexed) {
        const p_draw_idx *di = (const p_draw_idx *)payload;
        count = (uint32_t)di->count;
        instances = di->instances ? (uint32_t)di->instances : 1;
        base_vertex = (uint32_t)di->base_vertex;
        base_instance = di->base_instance > 0 ? (uint32_t)di->base_instance : 0;
        ibuf = di->ibuf; itype = di->itype;
    } else {
        const p_draw *d = (const p_draw *)payload;
        count = (uint32_t)d->count; first = (uint32_t)d->first;
    }

    // ---- 形状判据：这一次绘制**适不适合当采样点**（三角形族 + 数量 ≥ 3）----
    // 为什么必须有它：N4-i 第一轮 `@28` 打到的第一次绘制是「**3 个索引的线带**」（拓扑 3 = LINE_STRIP；
    // 与苹果侧 nvn_mtl.m:4419-4423 的映射同一套：0=点 1=线 3=线带 4/7=三角形 5=三角带 6=三角扇）——
    // 那种形状就算落下来也只是几个像素，**回答不了"哪一项让它消失"**（坑 ⑨：判据本身要能回答题目）。
    // 不满足就**不消费**"这一段通道已插过"的标记，继续往这一段通道里的下一次绘制走。
    int tri_family = (st->topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
                      st->topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                      st->topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
    if (!tri_family || count < 3) {
        uint64_t sn = atomic_fetch_add(&g_n4i_skipshape, 1) + 1;
        if (sn <= 16 || (sn & 1023) == 0)
            LOG("【查】DM_TRI_LADDER=%s：第 %llu 个候选**形状不合适**（游戏第 %llu 次绘制、拓扑 %d、%s %u）"
                "⇒ 这一档这次不插、继续找这一段通道里的下一次绘制（三角形族且数量 ≥ 3 才当采样点）",
                sname, (unsigned long long)sn, (unsigned long long)draw_index, (int)st->topo,
                indexed ? "索引数" : "顶点数", count);
        return;
    }

    // ---- 我们自己的状态基（与 N4-h 那条最简管线同一口径：只写附件 0、深度/剔除/混合全关）----
    static vk_gfx_state ours;
    memset(&ours, 0, sizeof ours);
    ours.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    ours.color_count = st->color_count > 8 ? 8 : st->color_count;
    memcpy(ours.color, st->color, sizeof(VkFormat) * ours.color_count);
    ours.write_mask[0] = 0xf;
    ours.samples = st->samples ? st->samples : (uint32_t)VK_SAMPLE_COUNT_1_BIT;
    ours.cull = VK_CULL_MODE_NONE;
    ours.front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    ours.poly = VK_POLYGON_MODE_FILL;
    // `DM_TRI_DECL_DS=1`：也声明游戏那份深度/模板附件格式（单变量，见 vk_tri_game.c 里那段说明）
    if (vk_tri_decl_ds()) { ours.depth_fmt = st->depth_fmt; ours.stencil_fmt = st->stencil_fmt; }
    // `DM_TRI_MASK_ALL=1`：把**每个**颜色附件的写掩码都开成 0xf（单变量，见 vk_tri_game.c 里那段说明）
    if (vk_tri_mask_all()) for (uint32_t i2 = 0; i2 < ours.color_count && i2 < 8; i2++) ours.write_mask[i2] = 0xf;
    memcpy(ours.attr, st->attr, sizeof ours.attr);       // `vb` 档要用（我们的 VS 只声明 Location 0）
    memcpy(ours.stream, st->stream, sizeof ours.stream);
    // N4-j：把"游戏那一刻的状态"**逐项**接过来（缺省关，一样都不接；形状照 DM_TRI_MASK_ALL）。
    // 每个开关只改我们这条管线的一个字段 ⇒ "整片 85 变成整片 152"就把责任钉在那一项上。
    vk_tri_apply_fields(&ours, st);

    // ---- 这一档用哪一对着色器 / 哪份状态 / 要不要描述符与顶点缓冲 ----
    const vk_shader *use_vs = NULL, *use_ps = NULL;
    const vk_gfx_state *use_st = &ours;
    const vk_shader *pair[2] = { NULL, NULL };
    int need_desc = 0, vb_mode = 0, draw3 = 0;
    if (step == VK_TRI_LADDER_VB) { use_vs = vs_vb; use_ps = fs; vb_mode = 1; }
    else if (step == VK_TRI_LADDER_STATE) { use_vs = vs_idx; use_ps = fs; use_st = st; draw3 = 1; }
    else if (step == VK_TRI_LADDER_VS) { use_vs = gvs; use_ps = fs; use_st = st; need_desc = 1; vb_mode = 2; }
    // `vsn`：与 `vs` **只差一样** —— 不绑描述符集（描述符/布局这一层单独摘出来）
    else if (step == VK_TRI_LADDER_VSN) { use_vs = gvs; use_ps = fs; use_st = st; need_desc = 0; vb_mode = 2; }
    // `vso`：**游戏的顶点着色器 + 我们自己的状态**（不绑描述符）—— 与 `vs`/`vsn` 只差"状态与声明细节"
    else if (step == VK_TRI_LADDER_VSO) { use_vs = gvs; use_ps = fs; need_desc = 0; vb_mode = 2; }   // use_st 保持 &ours
    // N4-k：`vsf` = **游戏的顶点着色器 + 游戏的片段着色器 + 我们自己的状态**（描述符照 `both` 绑上）。
    // 这是 `vso`（我们的 FS）与 `both`（游戏的状态 + 游戏的 FS）之间的那一格：
    //   · `vsf` 落屏 ⇒ 游戏的 FS 模块本身没问题（责任在"FS 与游戏状态的组合/声明细节"）；
    //   · `vsf` 不落屏而 `vso` 落屏 ⇒ 责任落在**游戏的片段着色器模块**上（两者只差 FS 这一样）。
    // 描述符为什么绑：游戏那份 FS 要 `OpImageSample` 三张贴图，不绑描述符集时采样未定义（引入新变量）；
    // 而描述符这条路已由 `vs` 档（我们的 FS + 游戏状态 + 描述符，叠 N4-j 修法）实测落屏 ⇒ 绑上才是单变量。
    else if (step == VK_TRI_LADDER_VSF) { use_vs = gvs; use_ps = gps; need_desc = 1; vb_mode = 2; }   // use_st 保持 &ours
    // `vsfn`：与 `vsf` 只差"不绑描述符集"（把游戏 FS 的绑定表那一层单独摘出来）
    else if (step == VK_TRI_LADDER_VSFN) { use_vs = gvs; use_ps = gps; need_desc = 0; vb_mode = 2; }
    // N4-k ⑨ `vsfull`：**我们自己的全屏三角形 VS** + **游戏的片段着色器** + 我们自己的状态 + 描述符。
    // 与 `vsf` 只差几何这一样（VS 换成 N4-h 那份只读 `gl_VertexIndex` 的全屏三角形、`vkCmdDraw(3)`）：
    // 仍然 152 ⇒ **这份片段着色器模块在这台驱动上产不出片元**（与几何无关）；落屏 ⇒ 回到几何链上找。
    else if (step == VK_TRI_LADDER_VSFULL) { use_vs = vs_idx; use_ps = gps; need_desc = 1; draw3 = 1; }
    // N4-l ⑩ `vsfullf`：**我们自己的全屏三角形 VS + 我们自己的片段着色器**（含 `DM_TRI_FS_VARIANT` 的变体）
    // + 我们自己的状态 + 描述符照绑。为什么必须有它：⑦/⑨ 的 `use_ps` 都是**游戏那份** FS，在这两档上设
    // `DM_TRI_FS_VARIANT` 一行都不生效（N4-k 的 iface/blit/if_ubo/if_smp 就是这么跑成"又一次 vsfull"的）。
    // 与 `vso`（同一份 FS、**不绑**描述符、两轮 85）只差"描述符绑没绑"这一样 ⇒ 单变量。
    else if (step == VK_TRI_LADDER_VSFULLF) { use_vs = vs_idx; use_ps = fs; need_desc = 1; draw3 = 1; }
    else if (step == VK_TRI_LADDER_BOTH) { use_vs = gvs; use_ps = gps; use_st = st; need_desc = 1; vb_mode = 2; }
    else { use_vs = gvs; use_ps = gps; use_st = st; need_desc = 1; vb_mode = 2; }
    if (!use_vs || use_vs->mod == VK_NULL_HANDLE || (need_desc && (!use_ps || use_ps->mod == VK_NULL_HANDLE))) {
        atomic_fetch_add(&g_n4i_skipnodraw, 1);
        return;
    }
    // ---- 阶梯自己的状态副本：**键里显式带上"我们这一对"** ----
    // 为什么必须有它（本轮真机现场）：图形管线缓存是**按整块状态字节**命中的（`vk_pipe.c:520-525`），
    // 而 `vk_gfx_state` 的 `vs_key`/`ps_key` 在回放路径上**从没被填过**（`gfx_state_fill` 只 memset）
    // ⇒ `vs`/`both` 档传的是**游戏那一刻的状态**，"游戏那一对"与"我们这一对"会撞进同一条条目：
    // 第一次跑 `vs` 档时拿到的其实是**游戏自己那条管线**（读数是"整片 152"，但那是一次空实验）。
    // 这里把键写成我们这一对 ⇒ 与游戏那一条**必然不同** ⇒ 这一档真的用我们这条管线。
    // （`DM_PIPEKEY=1` 打开后游戏的键也被填上，这一层保护依然成立。）
    static vk_gfx_state kst;
    kst = *st;
    snprintf(kst.vs_key, sizeof kst.vs_key, "%s", use_vs->key);
    if (use_ps) snprintf(kst.ps_key, sizeof kst.ps_key, "%s", use_ps->key);
    else kst.ps_key[0] = 0;
    snprintf(ours.vs_key, sizeof ours.vs_key, "%s", use_vs->key);
    if (use_ps) snprintf(ours.ps_key, sizeof ours.ps_key, "%s", use_ps->key);
    if (use_st == st) use_st = &kst;
    if (step == VK_TRI_LADDER_VB && !ours.attr[0].valid) {
        atomic_fetch_add(&g_n4i_skipvb, 1);
        LOG("【黄】DM_TRI_LADDER=vb：游戏那一刻**没有绑 Location 0 的属性**（这一档的顶点位置就取它）"
            "——这一档这次没插（游戏第 %llu 次绘制）", (unsigned long long)draw_index);
        return;
    }

    VkPipeline pl = vk_pipe_graphics(use_vs, use_ps, use_st);
    if (!pl) {
        atomic_fetch_add(&g_n4i_skipnodraw, 1);
        LOG("【黄】DM_TRI_LADDER=%s：这一档的图形管线**没建出来**（游戏第 %llu 次绘制、顶点 %s、"
            "目标 %u 个、拓扑 %d）——这一档这次没插",
            sname, (unsigned long long)draw_index, use_vs->key, use_st->color_count, (int)use_st->topo);
        return;
    }

    // ---- 描述符（只有 `vs` / `both` 档要：用的就是游戏那条路径的 desc_alloc/desc_write 与绑定表）----
    static vk_pipe_bind binds[VK_SPV_MAXB];
    static uint32_t dyn[VK_SPV_MAXB];
    VkDescriptorSet sets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkPipelineLayout lay = VK_NULL_HANDLE;
    int nd = -1;
    if (need_desc) {
        pair[0] = use_vs; pair[1] = use_ps;
        VkDescriptorSetLayout dsl0 = vk_pipe_set_layout_set(pair, 2, 0);
        VkDescriptorSetLayout dsl1 = vk_pipe_set_layout_set(pair, 2, 1);
        lay = vk_pipe_layout(pair, 2);
        if (dsl0) sets[0] = desc_alloc(g_cur_slot, dsl0);
        if (dsl1) sets[1] = desc_alloc(g_cur_slot, dsl1);
        int nb = (sets[0] && sets[1]) ? vk_pipe_binds(pair, 2, binds, VK_SPV_MAXB) : -1;
        nd = nb >= 0 ? desc_write(sets, pair, 2, binds, nb, dyn) : -1;
        if (nd < 0 || !lay) {
            atomic_fetch_add(&g_n4i_skipdesc, 1);
            LOG("【黄】DM_TRI_LADDER=%s：描述符集/布局写不出来（游戏第 %llu 次绘制、顶点 %s）"
                "——这一档这次没插（游戏路径同一处也会跳过）", sname, (unsigned long long)draw_index, use_vs->key);
            return;
        }
    }

    // ---- 顶点缓冲：`vb` 档绑"我们的状态里那一条属性所在的流"；`vs`/`both` 档照游戏路径绑"用到的每一条流" ----
    uint32_t vb_bound = 0, vb_first_stream = 0;
    uint64_t vb_first_addr = 0;
    if (vb_mode == 1) {
        uint32_t s2 = ours.attr[0].stream & 15;
        void *bb = NULL; uint64_t bo = 0;
        vb_first_stream = s2; vb_first_addr = g_rs.vbuf[s2].address;
        if (!g_rs.vbuf[s2].address || !mtl_addr(g_rs.vbuf[s2].address, g_rs.vbuf[s2].size, &bb, &bo)) {
            atomic_fetch_add(&g_n4i_skipvb, 1);
            LOG("【黄】DM_TRI_LADDER=vb：游戏的顶点流 %u（Location 0 所在）**绑不上**（address %#llx）"
                "——这一档这次没插", s2, (unsigned long long)g_rs.vbuf[s2].address);
            return;
        }
        vk2_touch_cpu_(g_rs.vbuf[s2].address, g_rs.vbuf[s2].size);
        VkBuffer vb = (VkBuffer)bb; VkDeviceSize vo = (VkDeviceSize)bo;
        vkCmdBindVertexBuffers(cb, s2, 1, &vb, &vo);
        vb_bound = 1;
    } else if (vb_mode == 2) {
        uint8_t used[16] = { 0 };
        for (int i2 = 0; i2 < 16; i2++) if (st->attr[i2].valid) used[st->attr[i2].stream & 15] = 1;
        for (int s2 = 0; s2 < 16; s2++) if (used[s2]) {
            void *bb = NULL; uint64_t bo = 0;
            if (mtl_addr(g_rs.vbuf[s2].address, g_rs.vbuf[s2].size, &bb, &bo)) {
                vk2_touch_cpu_(g_rs.vbuf[s2].address, g_rs.vbuf[s2].size);
                VkBuffer vb = (VkBuffer)bb; VkDeviceSize vo = (VkDeviceSize)bo;
                vkCmdBindVertexBuffers(cb, (uint32_t)s2, 1, &vb, &vo);
                if (!vb_bound) { vb_bound = 1; vb_first_stream = (uint32_t)s2; vb_first_addr = g_rs.vbuf[s2].address; }
            }
        }
    }
    // 索引缓冲：与游戏路径同一套（8 位索引在 Vulkan 核心不支持 ⇒ 那一档按跳过记）
    int ib_ok = 0;
    if (indexed) {
        VkIndexType it = VK_INDEX_TYPE_UINT16;
        if (itype == 2) it = VK_INDEX_TYPE_UINT32;
        else if (itype != 1) it = VK_INDEX_TYPE_UINT16;
        if (itype != 1 && itype != 2) {
            atomic_fetch_add(&g_n4i_skipdesc, 1);
            LOG("【黄】DM_TRI_LADDER=%s：这一次是**索引绘制**而索引类型 %d 这里不支持（只支持 16/32 位）"
                "——这一档这次没插", sname, itype);
            return;
        }
        void *ib = NULL; uint64_t io = 0;
        if (mtl_addr(ibuf, 4, &ib, &io)) {
            vk2_touch_cpu_(ibuf, (uint64_t)(count > 0 ? count : 1) * 4);
            vkCmdBindIndexBuffer(cb, (VkBuffer)ib, (VkDeviceSize)io, it);
            ib_ok = 1;
        }
    }
    VkBuffer dum = vk_pipe_dummy_buffer();
    if (dum) { VkDeviceSize z = 0; vkCmdBindVertexBuffers(cb, VK_PIPE_DUMMY_BIND, 1, &dum, &z); }

    // ---- 视口/裁剪：铺满附件（游戏那一刻的已经是这个值；插完按游戏原值恢复，与 N4-h 同口径）----
    VkViewport full = { 0.0f, 0.0f, (float)att_w, (float)att_h, 0.0f, 1.0f };
    VkRect2D fullsc = { { 0, 0 }, { att_w, att_h } };
    vkCmdSetViewport(cb, 0, 1, &full);
    vkCmdSetScissor(cb, 0, 1, &fullsc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    if (need_desc && nd >= 0) desc_bind(cb, lay, sets, pair, 2, nd, dyn, "阶梯", 1);
    if (draw3) {
        vkCmdDraw(cb, 3, 1, 0, 0);
    } else if (indexed && ib_ok) {
        vkCmdDrawIndexed(cb, count, instances, 0, base_vertex, base_instance);
    } else if (indexed) {
        LOG("【黄】DM_TRI_LADDER=%s：索引缓冲绑不上（%#llx）——这一档这次没插", sname, (unsigned long long)ibuf);
        vkCmdSetViewport(cb, 0, (uint32_t)(nvp > 0 ? nvp : 1), gvp);
        vkCmdSetScissor(cb, 0, 1, gsc);
        return;
    } else {
        vkCmdDraw(cb, count, 1, first, 0);
    }
    if (nvp > 0) vkCmdSetViewport(cb, 0, (uint32_t)nvp, gvp);
    if (nsc > 0) vkCmdSetScissor(cb, 0, 1, gsc);
    vk_tri_ladder_note();
    vk_tri_ladder_done();   // "这一段通道已插过"只在**真的画了**之后才置上

    uint64_t k = atomic_fetch_add(&g_n4i_logs, 1) + 1;
    if (k == 1 || (k & 255) == 0) {
        // 自证：① 这一档接上了什么（绑定号/格式/步长/偏移/地址）② 游戏那一刻的状态 ③ 这一档真的画了
        char vt[300]; int vo = 0; vt[0] = 0;
        if (vb_mode == 1) {
            uint32_t f = ours.attr[0].nvn_fmt, off = ours.attr[0].offset;
            uint32_t stride = ours.stream[vb_first_stream].stride ? ours.stream[vb_first_stream].stride : 1;
            uint64_t vsize = g_rs.vbuf[vb_first_stream].size;
            const uint8_t *base = (const uint8_t *)(uintptr_t)vk2_ptr_mask((uintptr_t)vb_first_addr);
            void *bb = NULL; uint64_t bo = 0;
            // 索引绘制要**按索引**取"真正被画的那几个顶点"：直接读流的前三个可能一个都不在那个三角形上
            uint32_t vidx[3] = { 0, 1, 2 };
            int have_idx = 0;
            if (indexed && ibuf) {
                void *ib = NULL; uint64_t io = 0;
                uint64_t nb = (itype == 2) ? 12u : 6u;
                if (mtl_addr(ibuf, nb, &ib, &io)) {
                    const uint8_t *ip = (const uint8_t *)(uintptr_t)vk2_ptr_mask((uintptr_t)ibuf);
                    for (int i2 = 0; i2 < 3; i2++) {
                        uint32_t v = 0;
                        if (itype == 2) memcpy(&v, ip + (uint64_t)i2 * 4, 4);
                        else { uint16_t h; memcpy(&h, ip + (uint64_t)i2 * 2, 2); v = h; }
                        vidx[i2] = v + base_vertex;
                    }
                    have_idx = 1;
                }
            }
            if (vb_first_addr && mtl_addr(vb_first_addr, vsize, &bb, &bo)) {
                for (int i2 = 0; i2 < 3; i2++) {
                    uint64_t voff = (uint64_t)off + (uint64_t)vidx[i2] * stride;
                    float c[4]; int nc = 0;
                    if (voff + 16 > vsize) { vtx_cat(vt, sizeof vt, &vo, "%s顶点%u（越出流范围 %llu 字节）", i2 ? " " : "", vidx[i2], (unsigned long long)vsize); continue; }
                    if (!vtx_decode(base + voff, f, c, &nc)) {
                        vtx_cat(vt, sizeof vt, &vo, "%s（格式 %#x 不是浮点/归一化类）", i2 ? " " : "", f);
                        break;
                    }
                    vtx_cat(vt, sizeof vt, &vo, "%s顶点%u=(%g,%g,%g,%g)", i2 ? " " : "", vidx[i2],
                            (double)c[0], (double)c[1], (double)c[2], (double)c[3]);
                }
            } else vtx_cat(vt, sizeof vt, &vo, "（读不了）");
            LOG("【查】DM_TRI_LADDER=vb：顶点流 %u 格式 %#x 步长 %u 偏移 %u 地址 %#llx（流 %llu 字节）；"
                "解码 %s前 3 个顶点：%s（clamp 是量尺：只要取到非零数据就一定看得见；"
                "但直通型的顶点着色器把属性直接当裁剪坐标 ⇒ w 与取值范围才决定它在不在裁剪体里）",
                vb_first_stream, f, ours.stream[vb_first_stream].stride, off, (unsigned long long)vb_first_addr,
                (unsigned long long)vsize, have_idx ? "（按索引）" : "（流的前三个）", vo ? vt : "（空）");
        }
        LOG("【查】DM_TRI_LADDER=%s：第 %llu 次插入 —— 游戏第 %llu 次绘制（%s，%s %u 个、实例 %u、first/base %u）；"
            "顶点 游戏 %s、我们 %s；片元 游戏 %s、**这一档实际用的 %s**（描述符 %s）；目标 %u 个（VkFormat %d/%d、采样 %u）；"
            "**游戏那一刻的状态** 拓扑 %d、写掩码[0] %#x、混合%s（%d→%d）、深度%s/%s（格式 %d/%d）、剔除 %d、"
            "游戏视口 %d 个（%.0fx%.0f）、裁剪 %d 个；"
            "**这一档接上的**：%s；我们这条管线 写掩码[0] %#x、深度关、剔除关、混合关；"
            "**真的画了**：%s（累计 %llu 次）",
            sname, (unsigned long long)k, (unsigned long long)draw_index,
            indexed ? "索引绘制" : "非索引绘制", indexed ? "索引数" : "顶点数", count, instances, indexed ? base_vertex : first,
            gvs ? gvs->key : "无", use_vs->key, gps ? gps->key : "无",
            use_ps ? use_ps->key : "（没有片元阶段）", need_desc ? "绑" : "不绑",
            use_st->color_count, use_st->color_count > 0 ? (int)use_st->color[0] : -1,
            use_st->color_count > 1 ? (int)use_st->color[1] : -1, use_st->samples,
            (int)use_st->topo, use_st->write_mask[0], use_st->blend_en[0] ? "开" : "关",
            (int)use_st->src_rgb[0], (int)use_st->dst_rgb[0],
            use_st->depth_test ? "开" : "关", use_st->depth_write ? "写" : "不写",
            (int)use_st->depth_fmt, (int)use_st->stencil_fmt, (int)use_st->cull,
            nvp, nvp > 0 ? (double)gvp[0].width : 0.0, nvp > 0 ? (double)gvp[0].height : 0.0, nsc,
            (step == VK_TRI_LADDER_VB) ? "游戏的顶点缓冲（Location 0）"
          : (step == VK_TRI_LADDER_STATE) ? "游戏那一刻的状态（拓扑/掩码/混合/剔除/深度）"
          : (step == VK_TRI_LADDER_VS) ? "游戏的描述符 + 顶点缓冲（与游戏路径同一段代码）"
          : (step == VK_TRI_LADDER_VSN) ? "游戏的顶点着色器 + 顶点缓冲（**故意不绑描述符集**：把描述符/布局这一层单独摘出来）"
          : (step == VK_TRI_LADDER_VSO) ? "游戏的顶点着色器 + 顶点缓冲，但状态用**我们自己的**（掩码只开附件 0、剔除/混合/深度全关、不声明深度格式）"
          : (step == VK_TRI_LADDER_VSF) ? "游戏的顶点着色器 + **游戏的片段着色器** + 描述符 + 顶点缓冲，但状态用**我们自己的**（= N4-k：与 `vso` 只差 FS 这一样）"
          : (step == VK_TRI_LADDER_VSFN) ? "同 `vsf`，但**故意不绑描述符集**（把游戏 FS 的绑定表单独摘出来）"
          : (step == VK_TRI_LADDER_VSFULL) ? "**我们自己的全屏三角形 VS** + **游戏的片段着色器** + 我们的状态 + 描述符（= 与 `vsf` 只差几何这一样）"
          : (step == VK_TRI_LADDER_VSFULLF) ? "**我们自己的全屏三角形 VS + 我们自己的片段着色器（含 DM_TRI_FS_VARIANT 变体）** + 我们的状态 + **描述符照绑**（= 与 `vso` 只差描述符这一样）"
          : "游戏的顶点着色器 + 游戏的片段着色器 + 描述符 + 顶点缓冲（= 原地重放游戏那次绘制）",
            ours.write_mask[0],
            draw3 ? "vkCmdDraw(3)（全屏三角形）" : (indexed && ib_ok ? "vkCmdDrawIndexed（照游戏的计数）" : "vkCmdDraw（照游戏的计数）"),
            (unsigned long long)atomic_load(&g_n4i_logs));
        // N4-p：**附件的格式逐项**（只读，本轮第一件事）。为什么要有它：上面那一行只打 `color[0]`/`color[1]`
        // （坑 (60)：第 3 个从来没打过），而且打的只是**管线声明**的格式 —— 它取的是**底图**的 NVN 格式
        // （`gfx_state_fill` 里 `g_rs.color[i]->d.format`），而附件视图走 `vk_tex_view` → `view_for`，
        // 那里 `view_t::set_format` 能**重解释**格式。两条路一旦不同，管线声明与真实附件格式就不同类
        // —— 在 Vulkan 里是 UB，校验层不一定报。"本机可作颜色附件/可混合"两格是 `vkGetPhysicalDeviceFormatProperties`
        // 的直接读数（设备真值，不是名字表），用来判"这个格式到底能不能当附件"。
        {
            // 一行一个附件（**不要**拼成一行：`log_line` 的缓冲是 1200 字节，中文一个字 3 字节 ——
            // N4-p 第一版把三个拼进一行，读数在中途被截断，且 `fo` 溢出后 `sizeof fb - fo` 变成天文数字）。
            // One line per attachment: `log_line`'s buffer is 1200 bytes and a Chinese character is 3 of them.
            LOG("【查】DM_TRI_LADDER=%s：**附件格式逐项**（N4-p 新增，只读）：%u 个颜色附件、深度 %s；"
                "这一刻那份片元着色器的三个输出都是 **vec4 float**（`.work/spv/8e80084d3c30340a` 的 %%34/%%36/%%38）",
                sname, use_st->color_count, g_rs.depth ? "有" : "无");
            for (uint32_t i2 = 0; i2 < use_st->color_count && i2 < 8; i2++) {
                const tex_t *t2 = i2 < g_rs.ncolor ? g_rs.color[i2] : NULL;
                const view_t *v2 = (i2 < g_rs.ncolor && g_rs.has_cview[i2]) ? &g_rs.cview[i2] : NULL;
                VkFormat real = vk_tex_view_fmt(t2, v2);
                VkFormatProperties fp; memset(&fp, 0, sizeof fp);
                if (vk_phys() != VK_NULL_HANDLE && real != VK_FORMAT_UNDEFINED)
                    vkGetPhysicalDeviceFormatProperties(vk_phys(), real, &fp);
                LOG("【查】  [%u] 序号 %llu：底图 NVN %#x → 管线声明 VkFormat %d、**真实（视图）%d**；"
                    "视图%s / %s；本机可作颜色附件 %s、可混合 %s",
                    i2, (unsigned long long)(t2 ? t2->serial : 0), t2 ? (unsigned)t2->d.format : 0u,
                    (int)use_st->color[i2], (int)real,
                    (v2 == NULL) ? "没带 cview" : ((v2->set_format ? "设了 set_format" : "没设 set_format")),
                    (int)use_st->color[i2] == (int)real ? "**两侧一致**" : "**★两侧不一致**",
                    (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ? "是" : "**否**",
                    (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) ? "是" : "否");
            }
            if (g_rs.depth) {
                const view_t *dv2 = g_rs.has_dview ? &g_rs.dview : NULL;
                VkFormat dr = vk_tex_view_fmt(g_rs.depth, dv2);
                LOG("【查】  深度：序号 %llu，底图 NVN %#x → 管线声明 %d/%d、真实 %d；视图%s、%s",
                    (unsigned long long)g_rs.depth->serial, (unsigned)g_rs.depth->d.format,
                    (int)use_st->depth_fmt, (int)use_st->stencil_fmt, (int)dr,
                    (dv2 == NULL) ? "没带 dview" : (dv2->set_format ? "设了 set_format" : "没设 set_format"),
                    (int)use_st->depth_fmt == (int)dr ? "**两侧一致**" : "**★两侧不一致**");
            }
        }
        // N4-j：两边状态的**全字段**摊平（**游戏那一刻** vs **我们这条管线**）—— 本轮要的是"逐字段对照"，
        // 上面那条汇总行只打了其中一小部分（frontFace / 偏置 / a2c / alpha 侧混合 / 模板 / poly 都没打）。
        {
            static char gd[560], od[560];
            gfx_state_desc(gd, sizeof gd, st);
            gfx_state_desc(od, sizeof od, &ours);
            LOG("【查】DM_TRI_LADDER=%s：状态全表（**游戏那一刻**）—— %s", sname, gd);
            LOG("【查】DM_TRI_LADDER=%s：状态全表（**我们这条管线**）—— %s", sname, od);
            char fb[160];
            if (vk_tri_fields_desc(fb, sizeof fb) > 0)
                LOG("【查】DM_TRI_LADDER=%s：**逐项接上的开关** = %s（叠在 `vso` 档上、一次一样；"
                    "判据：`DM_PASS_PROBE=28` 的底盘 152 上出现非 152 = 这一档仍然落屏）", sname, fb);
        }
        if (k == 1)
            LOG("【查】DM_TRI_LADDER：跳过计数（形状不合适 %llu / 管线没建出来 %llu / 目标或附件尺寸不适用 %llu / "
                "描述符写不出 %llu / 顶点流绑不上 %llu）——每一档的「没插」都能归到一条上",
                (unsigned long long)atomic_load(&g_n4i_skipshape), (unsigned long long)atomic_load(&g_n4i_skipnodraw),
                (unsigned long long)atomic_load(&g_n4i_skipfmt), (unsigned long long)atomic_load(&g_n4i_skipdesc),
                (unsigned long long)atomic_load(&g_n4i_skipvb));
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
            // N4-b：**这是一次 GPU 直写后备**（载荷经暂存缓冲 → `vkCmdCopyBuffer`）⇒ 目标在那一段的**影子
            // 从此过期**：标 GPU 脏，帧开头的上传不许碰它（否则会把刚写进后备的常量用旧影子盖掉）。
            vk2_touch_gpu_(dst, n);
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
                    drv_c0_set(stg, idx, b->addr);   // 同时写进驱动段 c0 的地址表（着色器是从那张表里读地址的）| also write it into driver segment c0's address table (shaders read the address from there)
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
            // N4-i：`DM_PIPEKEY=1`（缺省关）——把这一对着色器写进状态的键（见上面 pipekey_on 的说明）。
            // 必须在 `vk_pipe_graphics` 之前、且**只改这两个字段**（它们只参与缓存键，不参与任何状态解释）。
            if (pipekey_on() && shs[0]) {
                snprintf(stv.vs_key, sizeof stv.vs_key, "%s", shs[0]->key);
                stv.ps_key[0] = 0;
                if (shs[1]) snprintf(stv.ps_key, sizeof stv.ps_key, "%s", shs[1]->key);
            }
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
                // DM_RASTER_BRUTE：绘制前把动态视口/裁剪**铺满本通道的渲染区**（客体设的视口/裁剪仍可能吃掉
                // 全部片元；这一覆盖只在这个诊断开关下发生，缺省一行都不走）
                if (g_pass_w && g_pass_h && vk_pipe_brute()) {
                    VkViewport bv = { 0.0f, 0.0f, (float)g_pass_w, (float)g_pass_h, 0.0f, 1.0f };
                    VkRect2D bs = { { 0, 0 }, { g_pass_w, g_pass_h } };
                    static int said;
                    if (!said++) LOG("【查】DM_RASTER_BRUTE：绘制前把视口/裁剪铺成 %ux%u", g_pass_w, g_pass_h);
                    vkCmdSetViewport(cb, 0, 1, &bv);
                    vkCmdSetScissor(cb, 0, 1, &bs);
                }
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
                    // N4-l：改用 `desc_bind` —— 同一句绑定 + 顺手核对"动态偏移个数 vs 布局要求的个数"（只读）
                    desc_bind(cb, lay, sets, (const vk_shader *const *)shs, nsh, nd, dyn, "游戏路径", 0);
                    // 顶点流：只绑管线真的用到的那些；其余属性走常量回退，落在 VK_PIPE_DUMMY_BIND 上
                    // Vertex streams: bind only the ones the pipeline really uses; the remaining attributes come
                    // from the constant fallback that lives on VK_PIPE_DUMMY_BIND
                    uint8_t used[16] = { 0 };
                    for (int i2 = 0; i2 < 16; i2++) if (stv.attr[i2].valid) used[stv.attr[i2].stream & 15] = 1;
                    int nused = 0;
                    for (int s2 = 0; s2 < 16; s2++) if (used[s2]) {
                        nused++;
                        void *bb = NULL; uint64_t bo = 0;
                        if (mtl_addr(g_rs.vbuf[s2].address, g_rs.vbuf[s2].size, &bb, &bo)) {
                            VkBuffer vb = (VkBuffer)bb; VkDeviceSize vo = (VkDeviceSize)bo;
                            // N4-b：顶点流要被 GPU 读 ⇒ 保证后备是最新的（标 CPU 脏；帧开头搬）
                            vk2_touch_cpu_(g_rs.vbuf[s2].address, g_rs.vbuf[s2].size);
                            vkCmdBindVertexBuffers(cb, (uint32_t)s2, 1, &vb, &vo);
                            atomic_fetch_add(&g_vb_ok, 1);
                        } else { atomic_fetch_add(&g_vb_fail, 1); atomic_fetch_add(&g_vb_fail_s[s2], 1); }   // N4-e：没绑上（这种绘制产不出片元）
                    }
                    if (!nused) atomic_fetch_add(&g_vb_none, 1);
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
                            if (!di->count) atomic_fetch_add(&g_draw_i0, 1);   // N4-e：索引数为 0 的绘制
                            // N4-b：索引缓冲要被 GPU 读 ⇒ 保证后备最新。反查只问了 4 字节（旧路如此），
                            // 标脏按"最多 4 字节一个索引"取上界（多标只多搬，不会少搬）。
                            vk2_touch_cpu_(di->ibuf, (uint64_t)(di->count > 0 ? di->count : 1) * 4);
                            vkCmdBindIndexBuffer(cb, (VkBuffer)ib, (VkDeviceSize)io, it);
                            vkCmdDrawIndexed(cb, (uint32_t)di->count, di->instances ? (uint32_t)di->instances : 1,
                                             0, di->base_vertex, di->base_instance > 0 ? (uint32_t)di->base_instance : 0);
                            wrote = 1;
                        } else atomic_fetch_add(&g_desc_skip, 1);
                    } else {
                        if (!d->count) atomic_fetch_add(&g_draw_v0, 1);   // N4-e：顶点数为 0 的绘制
                        vkCmdDraw(cb, (uint32_t)d->count, 1, (uint32_t)d->first, 0);
                        wrote = 1;
                    }
                    // N4-h：这一次**游戏绘制刚录完** ⇒ 在同一段通道、同一批附件上插一次**我们自己的绘制**
                    //（缺省关；设计、开关形状与判据见 vk_tri_game.h）。放在游戏绘制**之后**：我们那条管线
                    // 关着剔除/混合/深度，所以它一定覆盖在游戏写过的地方之上，落屏与否看得最干净。
                    // N4-h: right after a **game draw** is recorded, insert **our own draw** into the same pass with
                    // the same attachments (off by default; see vk_tri_game.h). It goes *after* the game's draw
                    // because our pipeline has culling/blending/depth off, so it lands on top of whatever the game
                    // wrote -- which is the cleanest thing to read "did it land at all" from.
                    if (wrote) {
                        // N4-i：阶梯（`DM_TRI_LADDER`）开着时走阶梯、**不**走 N4-h 那条路（一次只多接一样地二分）；
                        // 阶梯关着时 N4-h 那条路**一字未改**。两条路共用 `DM_TRI_IN_GAME` 的门与"每段通道插一次"。
                        int lstep = vk_tri_ladder_mode();
                        if (lstep) {
                            uint64_t c0 = g_rs.color[0] ? g_rs.color[0]->serial : 0;
                            if (vk_tri_ladder_arm(k + 1, c0))
                                n4i_ladder_draw(cb, k + 1, &stv, shs[0], shs[1], (const void *)a, (int)h.op,
                                                g_att_w0, g_att_h0, g_rs.vp, g_rs.nvp, g_rs.sc, g_rs.nsc);
                        } else
                            vk_tri_game_try_draw(cb, k + 1, (const tex_t *const *)g_rs.color, g_rs.cview,
                                                 g_rs.has_cview, g_rs.ncolor, g_att_w0, g_att_h0, stv.samples,
                                                 g_rs.vp, g_rs.nvp, g_rs.sc, g_rs.nsc);
                    }
                } else atomic_fetch_add(&g_desc_skip, 1);
            } else if (pl) atomic_fetch_add(&g_desc_skip, 1);
            if (wrote) atomic_fetch_add(&g_desc_draws, 1);
            if (k < 8 || (k & 2047) == 2047) {
                uint64_t nlay = 0, nlay2 = 0, ncpso = 0, gpso = 0, nfail = 0;
                vk_pipe_stats(&nlay, &nlay2, &ncpso, &gpso, &nfail);
                char fbs[128]; int fbo = vb_fail_breakdown(fbs, sizeof fbs);
                LOG("%s 第 %llu 次：顶点 %s 片元 %s → 管线 %s；目标 %u 个、拓扑 %d、属性 %u 条；%s；图形管线累计 %llu 条（失败 %llu）；真下发 %llu 次"
                    "；**顶点流** 绑上 %llu / 没绑上 %llu（%s）/ 一条都没 %llu；顶点数 0 的 %llu、索引数 0 的 %llu",
                    g_opname[h.op], (unsigned long long)(k + 1), shs[0] ? shs[0]->key : "无", shs[1] ? shs[1]->key : "无",
                    pl ? "建得成" : "建不出", stv.color_count, (int)stv.topo, shs[0] ? shs[0]->nin : 0,
                    wrote ? "已下发" : "未下发",
                    (unsigned long long)gpso, (unsigned long long)nfail, (unsigned long long)atomic_load(&g_desc_draws),
                    (unsigned long long)atomic_load(&g_vb_ok), (unsigned long long)atomic_load(&g_vb_fail),
                    fbo ? fbs : "无",
                    (unsigned long long)atomic_load(&g_vb_none), (unsigned long long)atomic_load(&g_draw_v0),
                    (unsigned long long)atomic_load(&g_draw_i0));
                // N4-f：**同一个节流点**上采一个顶点/常量样本（缺省关）。为什么不另开节流：判据行本身
                // 已经是"前 8 次 + 每 2048 次"，另开一条节流在一轮 ~3 万次绘制里会**两次都打不出**（第 ⑱ 条）。
                if (vtx_probe_on()) {
                    int dcnt = (h.op == OP_DRAW_IDX) ? (int)((const p_draw_idx *)a)->count : (int)d->count;
                    vtx_sample(&stv, shs[0], k + 1, dcnt);
                }
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
