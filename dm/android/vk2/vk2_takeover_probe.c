// 新地基 **N4-b**（把 GPU 侧的内存接管下来）的**自证探针**。
//
// N4-b 只做两件事，本探针把它们各问成一问一答：
//   ① **`mtl_addr` → `vk2_addr`**：绘制 / 拷贝 / 贴图反查的那几处调用点，拿到的不再是"池缓冲 + 池内偏移"，
//      而是**块缓冲 + 块内偏移**（跨块 / 越过池尾 / 没有后备都返回 0）。接管之后 GPU 读的是**后备资源**，
//      所以"客体写进池的字节什么时候到 GPU 眼前"这件事，从"平台替我们保证"变成"我们自己上传"。
//   ② **上传窗口接进提交路径**：一帧 = `vk2_sub_begin()` → 帧开头 `vk2_upload_all_into(slot->bank, …)`
//      （把所有池的 CPU_DIRTY run 搬上去）→ 录别的命令 → `vk2_sub_commit()` → 等这个 frame 的定向栅栏
//      → 回读 → 回收时把环交还。**环按槽分**（N3 量出来的待办：单环会把上传与在飞深度串行化）。
//
// 七问：
//   问 1 这次走哪条路（`DM_GFX2` 可读自证）
//   问 2 池 + 后备 + **反查形状**：命中给块缓冲与块内偏移、偏移随地址前进、越过池尾/池外/无后备都返回 0
//   问 3 **环按槽分**：3 个槽**同时**各载一个上传窗口（N3 的待办：单环时只能 1 个），回收后三段都交还
//   问 4 **接管闭环**（G1/G2 的接管版）：CPU 写影子 → 触碰标脏 → 帧开头上传 → 提交 → 等栅栏 → 回读
//        → 与**逐字节独立重算**的期望比对；再补两半：**没有标脏就不上传**（回读读到旧值）、
//        标脏之后同一帧才读得到新值
//   问 5 **"脏 → 上传 → 清位 → 再触碰 → 再上传"**这条不变量（"下一帧重传"就是接管路径的命根子）
//   问 6 **GPU_DIRTY 不被上传覆盖**（旧路 `UPDATE_UB` / `COPY_B2B` 直写后备之后影子就过期了这条语义）
//   问 8 **N4-c：提交侧接上** —— 槽回收钩子在「栅栏已经被观察到」的那一刻被调到（每次提交一次）；`vk2_sub_poll`
//        的三种返回（已回收 = 1 / 还在飞 = 0 / 不认识 = -1），"还在飞"那一半用一次 **128 MB 的 GPU 拷贝**做前提
//   问 9 **N4-c：往返型（GPU→池回读）** —— GPU 直写后备之后：① 不等就读 = 旧值；② 等栅栏（不回读）还是旧值；
//        ③ 回收钩子按那次提交的栅栏回读之后 = 新值（逐字节独立重算）
//   问 10 **N4-c：半页竞态的两条对策** —— ① 帧开头上传跳过还带 GPU 脏的页（页保持 CPU 脏）；
//        ② 回收时字节精确回读 + 只清 GPU 位 ⇒ 影子里 GPU 的字节与 CPU 的字节各归各位
//   问 11 结论键值（`--- VK2_TAKEOVER_SUMMARY ---`）+ `vkQueueWaitIdle` 计数（两个模块都必须是 0）
//
// 跑法（一律走项目内 `.venv` Python）：
//   & "<仓库>/.venv/Scripts/python.exe" -X utf8 .work/probes.py run vk2_takeover_probe --serial 8b1f1e50 [--tag r1]
// 多轮：`.work/vk2_pool_rounds.py --probe vk2_takeover_probe --rounds 4`
// 读数落 `.work/logs/probe-<串号>-vk2_takeover_probe[-tag].txt`。
//
// 开关：DM_VK2_POOL_MB（探针池多大，缺省 8）、DM_VK2_RING_MB（暂存环，缺省 16）、DM_VK2_INFLIGHT（缺省 3）、
//       DM_VK2_SEED（缺省 20261016）
//
// ---------------------------------------------------------------------------------------------------------------
// The self-proving probe of N4-b: taking over the GPU-side memory. Question 2 asks the address lookup what shape it
// now hands back (chunk buffer plus in-chunk offset), question 3 shows the ring split per slot so three in-flight
// slots can each carry an upload window (the limit N3 measured), question 4 runs the real round trip through the
// submission path -- and shows both halves: without a dirty mark there is no upload and the readback still sees the
// old bytes, with the mark the frame sees the new ones -- question 5 pins the re-dirty invariant down, and question 6
// shows a GPU-written range is not clobbered by an upload.
#define _GNU_SOURCE 1
#include "vk2_sub.h"
#include "vk2_xfer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MB (1024ull * 1024ull)
#define KB (1024ull)

static uint64_t g_qn;
// 问 7（初始同步）的读数，留给末尾的键值段 | question 7's readings, kept for the summary block
static uint64_t g_init_bytes, g_init_rounds, g_init_us;
static void Q(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("\n[问 %d] ", (int)++g_qn); vprintf(fmt, ap); printf("\n");
    va_end(ap); fflush(stdout);
}
static void A(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("[答] "); vprintf(fmt, ap); printf("\n");
    va_end(ap); fflush(stdout);
}
static void plog(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("[池] "); vprintf(fmt, ap); printf("\n");
    va_end(ap); fflush(stdout);
}
static void xlog(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("[搬] "); vprintf(fmt, ap); printf("\n");
    va_end(ap); fflush(stdout);
}
static void slog(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("[槽] "); vprintf(fmt, ap); printf("\n");
    va_end(ap); fflush(stdout);
}
static uint64_t now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
static uint64_t env_u64(const char *k, uint64_t dflt) {
    const char *v = getenv(k);
    if (!v || !*v) return dflt;
    unsigned long long x = strtoull(v, NULL, 0);
    return x ? (uint64_t)x : dflt;
}
static unsigned long long mem_avail_kb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256]; unsigned long long kb = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
    fclose(f);
    return kb;
}

// 逐字节重算的期望（判据不许拿"影子自己"当对照）| the expectation is recomputed per byte
static uint8_t pat_byte(uint64_t i, uint64_t seed) {
    return (uint8_t)(((i * 1103515245ull + seed * 12345ull + (i >> 7)) >> 13) ^ (i * 31ull));
}
static int pat_check(const uint8_t *p, uint64_t n, uint64_t seed) {
    for (uint64_t i = 0; i < n; i++) if (p[i] != pat_byte(i, seed)) return 0;
    return 1;
}
// 从**池内某个绝对偏移**开始的期望值：整池 `pat_fill(mem, …)` 之后，任何一段都要按它在池里的位置算
// （探针自己踩过：尾段用"从 0 开始"的期望照，结果把对的读成错的）。
static int pat_check_at(const uint8_t *p, uint64_t base_off, uint64_t n, uint64_t seed) {
    for (uint64_t i = 0; i < n; i++) if (p[i] != pat_byte(base_off + i, seed)) return 0;
    return 1;
}
static void pat_fill(uint8_t *p, uint64_t n, uint64_t seed) {
    for (uint64_t i = 0; i < n; i++) p[i] = pat_byte(i, seed);
}

// ---------------------------------------------------------------- 设备（与 N1/N2/N3 的探针同一套骨架）
typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue q;
    uint32_t qfi;
    VkPhysicalDeviceMemoryProperties mp;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMaintenance3Properties m3;
} probe_dev;

static int vk_open(probe_dev *v) {
    memset(v, 0, sizeof *v);
    VkApplicationInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "dm-vk2-takeover-probe";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    if (vkCreateInstance(&ici, NULL, &v->inst) != VK_SUCCESS) return 0;
    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(v->inst, &n, NULL) != VK_SUCCESS || n == 0) return 0;
    VkPhysicalDevice *dv = calloc(n, sizeof *dv);
    if (!dv) return 0;
    if (vkEnumeratePhysicalDevices(v->inst, &n, dv) != VK_SUCCESS) { free(dv); return 0; }
    v->phys = dv[0];
    free(dv);
    VkPhysicalDeviceProperties2 p2;
    memset(&p2, 0, sizeof p2);
    v->m3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &v->m3;
    vkGetPhysicalDeviceProperties2(v->phys, &p2);
    v->props = p2.properties;
    vkGetPhysicalDeviceMemoryProperties(v->phys, &v->mp);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq ? nq : 1, sizeof *qf);
    if (!qf) return 0;
    vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &nq, qf);
    v->qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { v->qfi = i; break; }
    free(qf);
    if (v->qfi == UINT32_MAX) return 0;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof qci);
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = v->qfi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof dci);
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(v->phys, &dci, NULL, &v->dev) != VK_SUCCESS) return 0;
    vkGetDeviceQueue(v->dev, v->qfi, 0, &v->q);
    return 1;
}
static void vk_close(probe_dev *v) {
    if (v->dev) vkDestroyDevice(v->dev, NULL);
    if (v->inst) vkDestroyInstance(v->inst, NULL);
}

// ---------------------------------------------------------------- 一帧：帧开头上传 + 提交 + 等栅栏（回收时交还环）
// 这就是 N4-b 接进提交路径的形状（与回放层要做的一模一样，只是这里不画东西）。
// One frame: upload at the head of the command buffer, submit, wait on that frame's fence, ring handed back on reap.
typedef struct {
    uint64_t frame;
    uint64_t moved;
    int      committed;
    int      waited;
} frame_out;

static int run_frame(frame_out *out) {
    memset(out, 0, sizeof *out);
    vk2_slot *s = vk2_sub_begin();
    if (!s) return 0;
    out->frame = s->frame;
    // ① 帧开头：把这个槽这一段里的所有池的 CPU_DIRTY run 搬上去（录在 cb 的最前面）
    out->moved = vk2_upload_all_into(s->bank, s->cb, s->frame);
    if (out->moved) vk2_sub_note_upload();     // 告诉 vk2_sub：回收这个槽时把这一段交还
    // ② 提交（**一次客体提交 = 一次 vkQueueSubmit**）
    if (!vk2_sub_commit()) return 0;
    out->committed = 1;
    // ③ 回读要等"这次提交的栅栏"：**必须在回收之前把栅栏拿出来**（回收之后槽就空了）
    vk2_readback_after(vk2_sub_fence(out->frame));
    // ④ 等这一 frame 做完（定向栅栏，不是队列空闲）——顺带回收槽、交还环
    out->waited = vk2_sub_wait(out->frame);
    return out->waited;
}

// ---------------------------------------------------------------- N4-c：回放层那三件事的探针版
// 问 8~10 需要三样东西，探针自己照回放层的形状做出来（**不依赖回放层**，这样它才是自证）：
//   ① **回收钩子**：本模块在"观察到某个槽的栅栏已经 signal"时调它（然后才让槽可重用）。探针的钩子做两件事 ——
//      记"钩子被调到了、那一刻栅栏确实已经 signal"；按 `g_hook_mode` 决定要不要把"本槽 GPU 直写过的段"
//      回读进影子（这正是回放层在回收时做的事）。
//   ② **GPU 直写区间的记账**：回放层按提交记（`COPY_B2B` / `UPDATE_UB` 的目标）；探针在录那段拷贝时自己记。
//   ③ **poll**：非阻塞地问"这个 frame 完成了没"（`mtl_wait(fence, 0)` 要的就是它）。
// N4-c: the probe's own version of the three things the replay layer does (reclaim hook, per-submission GPU-written
// ranges, and the non-blocking poll), so questions 8-10 stay self-proving and do not depend on the replay layer.
#define PROBE_NOTE_MAX 16
typedef struct { uint64_t addr, len; } prange;
static prange   g_note[PROBE_NOTE_MAX];
static int      g_nnote;
static int      g_hook_mode;          // 0 = 只记不读（问 9 的 (b) 半）；1 = 回读 + 清位（回放层的形状）
static VkDevice g_hook_dev;
static uint64_t g_hook_calls, g_hook_signalled, g_hook_rb_bytes, g_hook_settled;
static void probe_reap(int slot, uint64_t frame, VkFence fence, void *ud) {
    (void)slot; (void)ud;
    g_hook_calls++;
    if (g_hook_dev && vkGetFenceStatus(g_hook_dev, fence) == VK_SUCCESS) g_hook_signalled++;
    if (g_hook_mode != 1) return;
    vk2_readback_after(fence);            // 生产者 = 这次提交自己（此刻已经 signal）
    for (int k = 0; k < g_nnote; k++) {
        if (vk2_readback_addr(g_note[k].addr, g_note[k].len)) g_hook_rb_bytes += g_note[k].len;
        g_hook_settled += vk2_gpu_settle(g_note[k].addr, g_note[k].len);
    }
    g_nnote = 0;
}

int main(void) {
    const uint64_t seed = env_u64("DM_VK2_SEED", 20261016ull);
    const uint64_t pool_mb = env_u64("DM_VK2_POOL_MB", 8);
    const uint64_t ring_mb = env_u64("DM_VK2_RING_MB", 16);
    const int      inflight = (int)env_u64("DM_VK2_INFLIGHT", 3);

    printf("=== vk2_takeover_probe（N4-b：GPU 侧内存接管的自证）===\n");
    printf("构建：%s %s；池 %llu MB、环 %llu MB、在飞 %d\n", __DATE__, __TIME__,
           (unsigned long long)pool_mb, (unsigned long long)ring_mb, inflight);

    probe_dev vd;
    if (!vk_open(&vd)) { printf("【红】Vulkan 设备打不开\n"); return 1; }

    // ------------------------------------------------------------------ 问 1
    Q("这次走哪条路（`DM_GFX2` 可读自证，缺省关）");
    A("vk2_gfx2_enabled = %d；%s", vk2_gfx2_enabled(), vk2_route_line());
    A("探针不依赖开关：它直接按 N4-b 的形状建模块（回放层的选路点才看 DM_GFX2）");

    // ------------------------------------------------------------------ 模块起来
    vk2_dev d;
    memset(&d, 0, sizeof d);
    d.dev = vd.dev; d.phys = vd.phys; d.q = vd.q; d.qfi = vd.qfi;
    d.mp = vd.mp; d.props = vd.props; d.m3 = vd.m3;
    unsigned long long mem0 = mem_avail_kb();
    if (!vk2_pool_init(&d, 0)) { printf("【红】vk2_pool_init 失败\n"); vk_close(&vd); return 1; }
    vk2_pool_set_backing_budget(UINT64_MAX);                 // 探针要真后备（回放层才用 DM_GFX2_BACKING_MB 限）
    if (!vk2_xfer_init(&d, ring_mb * MB, 0)) { printf("【红】vk2_xfer_init 失败\n"); return 1; }
    if (!vk2_sub_init(&d, inflight)) { printf("【红】vk2_sub_init 失败\n"); return 1; }
    vk2_pool_set_log(plog); vk2_xfer_set_log(xlog); vk2_sub_set_log(slog);
    // N4-c：把"回收钩子"挂上（回放层的形状；问 8~10 用它）。**注意：挂钩子之后 N3 的问 3/4 那些读数不受影响**
    // ——钩子只记账、并在 g_hook_mode=1 时回读"本槽自己记下的区间"（没有记区间时它什么都不做）。
    // N4-c: install the reclaim hook (the replay layer's shape; questions 8-10 use it). It only accounts, and with
    // g_hook_mode=1 it reads back the ranges this slot noted -- with none noted it does nothing.
    g_hook_dev = vd.dev;
    vk2_sub_set_reap_hook(probe_reap, NULL);

    // 池 = 普通匿名内存（新地基的唯一前提）| the pool is plain anonymous memory (the new foundation's precondition)
    const uint64_t pool_bytes = pool_mb * MB;
    uint8_t *mem = (uint8_t *)mmap(NULL, (size_t)pool_bytes, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { printf("【红】池内存 mmap 失败\n"); return 1; }
    int ok_add = vk2_pool_add(mem, pool_bytes, "probe");
    unsigned long long mem1 = mem_avail_kb();

    // ------------------------------------------------------------------ 问 2
    Q("池 + 后备 + **地址反查的形状**（接管之后那几处 `mtl_addr` 调用点拿到的就是它）");
    vk2_pool_stats pst;
    vk2_pool_stats_get(&pst);
    A("池登记 = %d；池数 %llu、块试/成 %llu/%llu、no_region=%llu small=%llu pool_fail=%llu；"
      "后备已建 %llu MB（MemAvailable %llu→%llu kB）",
      ok_add, (unsigned long long)pst.pools, (unsigned long long)pst.chunk_try, (unsigned long long)pst.chunk_ok,
      (unsigned long long)pst.no_region, (unsigned long long)pst.small, (unsigned long long)pst.pool_fail,
      (unsigned long long)(pst.backing_bytes >> 20), mem0, mem1);
    {
        const vk2_pool *p = vk2_pool_at(0);
        if (p) plog("池 0：%p+%llu MB、%d 块（第一块 %llu MB）；模式 %d、块大小 %llu MB",
                    (void *)p->lo, (unsigned long long)((p->hi - p->lo) >> 20), p->nchunk,
                    (unsigned long long)(p->nchunk ? p->chunk[0].size >> 20 : 0), p->mode,
                    (unsigned long long)(vk2_pool_chunk_bytes() >> 20));
    }

    int q2_hit = 0, q2_off_ok = 0, q2_over = 0, q2_miss = 0, q2_noback = 0;
    {
        // 命中：池内 1 MiB 处取 4 KiB —— 拿到块缓冲 + 块内偏移
        VkBuffer b1 = VK_NULL_HANDLE, b2 = VK_NULL_HANDLE;
        uint64_t o1 = 0, o2 = 0;
        uint64_t a1 = (uint64_t)(uintptr_t)mem + 1 * MB;
        q2_hit = vk2_addr(a1, 4096, &b1, &o1);
        uint64_t a2 = (uint64_t)(uintptr_t)mem + 1 * MB + 128 * KB;
        int hit2 = vk2_addr(a2, 4096, &b2, &o2);
        q2_off_ok = (q2_hit && hit2 && b1 == b2 && o2 == o1 + 128 * KB) ? 1 : 0;
        A("命中（池内 %llu B 处取 4 KiB）= %d：块缓冲 %p、块内偏移 %llu；再取 +128 KiB 处：偏移 %llu"
          "（随地址前进 %s、同一块 %s）",
          (unsigned long long)(1 * MB), q2_hit, (void *)b1, (unsigned long long)o1, (unsigned long long)o2,
          q2_off_ok ? "是" : "**不是**", (b1 == b2) ? "是" : "**不是**");
    }
    {
        VkBuffer b = VK_NULL_HANDLE; uint64_t o = 0;
        int over = vk2_addr((uint64_t)(uintptr_t)mem + pool_bytes - 4096, 8192, &b, &o);   // 越过池尾
        q2_over = (over == 0) ? 1 : 0;
        int outside = vk2_addr((uint64_t)(uintptr_t)mem - 1 * MB, 4096, &b, &o);          // 根本不在池里
        q2_miss = (outside == 0) ? 1 : 0;
        int beyond = vk2_addr((uint64_t)(uintptr_t)mem + 2 * vk2_pool_chunk_bytes(), 64, &b, &o);  // 池没那么大 ⇒ 无后备
        q2_noback = (beyond == 0) ? 1 : 0;
        A("越过池尾（%llu B 处取 8 KiB）返回 %d（应 0）；池外返回 %d（应 0）；超过池大小的地址返回 %d（应 0）",
          (unsigned long long)(pool_bytes - 4096), over, outside, beyond);
    }
    vk2_pool_stats_get(&pst);
    A("反查账：lookups=%llu hits=%llu misses=%llu cross=%llu",
      (unsigned long long)pst.lookups, (unsigned long long)pst.hits, (unsigned long long)pst.misses,
      (unsigned long long)pst.cross);

    // ------------------------------------------------------------------ 问 3：环按槽分
    Q("**环按槽分**（N3 的待办）：3 个槽能不能**同时**各载一个上传窗口？");
    int banks = vk2_sub_use_banks();
    int q3_banks_ok = (banks >= vk2_sub_inflight() && banks >= 3) ? 1 : 0;
    A("vk2_sub_use_banks() = %d（在飞深度 %d）；每段 %llu KB",
      banks, vk2_sub_inflight(), (unsigned long long)(vk2_xfer_bank_bytes(0) >> 10));
    int q3_all_open = 0, q3_all_moved = 0, q3_recycled = 0, q3_slot_banks_ok = 0, q3_submitted = 0;
    {
        // (i) 配置自证：第 i 个槽用第 i 段（纯配置，与 GPU 快慢无关）
        q3_slot_banks_ok = 1;
        for (int i = 0; i < banks; i++) if (vk2_sub_slot_bank(i) != i) q3_slot_banks_ok = 0;
        A("每槽一段的自证：第 i 个槽用第 i 段（%d 个槽 / %d 段）= %d", vk2_sub_inflight(), banks, q3_slot_banks_ok);

        // (ii) 三段**同时在飞**：这一问**自己建**三条命令缓冲 + 一个栅栏，**一次** `vkQueueSubmit(3 条 cb)`
        // ⇒ 栅栏 signal 之前那三段不可能被回收 ⇒ 判据是**确定性**的。
        // 第一版靠"三次 vk2_sub_begin 拿到三个槽"来问这件事，而提交在 Adreno 上快得来不及 —— 三次 begin 会因为
        // 槽被轮询回收而落到同一个槽上（读数长成"1 段被占着"）：那是**探针自己**的竞态，不是环的问题。
        VkCommandPoolCreateInfo pci;
        memset(&pci, 0, sizeof pci);
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = vd.qfi;
        VkCommandPool cp = VK_NULL_HANDLE;
        VkCommandBuffer cbs[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkFence fen = VK_NULL_HANDLE;
        int ok3 = 0;
        if (vkCreateCommandPool(vd.dev, &pci, NULL, &cp) == VK_SUCCESS) {
            VkCommandBufferAllocateInfo ai;
            memset(&ai, 0, sizeof ai);
            ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            ai.commandPool = cp; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 3;
            if (vkAllocateCommandBuffers(vd.dev, &ai, cbs) == VK_SUCCESS) {
                VkFenceCreateInfo fi;
                memset(&fi, 0, sizeof fi);
                fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                if (vkCreateFence(vd.dev, &fi, NULL, &fen) == VK_SUCCESS) ok3 = 1;
            }
        }
        if (!ok3) A("【红】探针自己建三条 cb / 一个栅栏失败（这一问没问成）");
        if (ok3) {
            int rec = 1;
            for (int i = 0; i < 3 && i < banks; i++) {
                uint64_t off = (uint64_t)i * 256 * KB;
                pat_fill(mem + off, 64 * KB, seed + (uint64_t)i);
                vk2_dirty_mark_cpu(vk2_pool_dirty(0), off, 64 * KB);   // 触碰（= 回放层的绑定点标脏）
                vkResetCommandBuffer(cbs[i], 0);
                VkCommandBufferBeginInfo bi;
                memset(&bi, 0, sizeof bi);
                bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                if (vkBeginCommandBuffer(cbs[i], &bi) != VK_SUCCESS) { rec = 0; break; }
                uint64_t moved = vk2_upload_into(i, cbs[i], 0, 0);
                if (vkEndCommandBuffer(cbs[i]) != VK_SUCCESS) { rec = 0; break; }
                A("第 %d 段：搬 %llu 字节、窗口 %s", i, (unsigned long long)moved, moved ? "开" : "没开");
            }
            if (rec) {
                VkSubmitInfo si;
                memset(&si, 0, sizeof si);
                si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                si.commandBufferCount = 3; si.pCommandBuffers = cbs;
                if (vkQueueSubmit(vd.q, 1, &si, fen) == VK_SUCCESS) q3_submitted = 1;
                else A("【红】一次提交三条 cb 失败");
            }
        }
        if (q3_submitted) {
            int open_cnt = 0, moved_all = 1;
            for (int i = 0; i < 3; i++) {
                vk2_bank_stats bs;
                vk2_xfer_bank_stats_get(i, &bs);
                if (bs.open) open_cnt++;
                if (!bs.moved) moved_all = 0;
            }
            q3_all_open = (open_cnt == 3) ? 1 : 0;
            q3_all_moved = moved_all;
            A("**一次提交三条 cb、栅栏还没等到**：%d 段同时被占着（N3 的单环在这里只能有 1 段）、"
              "每段都搬过东西 = %d", open_cnt, moved_all);
            VkResult wr = vkWaitForFences(vd.dev, 1, &fen, VK_TRUE, 5000000000ull);
            A("等这个栅栏：VkResult %d（0 = 成功）", (int)wr);
            for (int i = 0; i < 3; i++) vk2_xfer_window_done_bank(i);
            int closed = 1;
            for (int i = 0; i < 3; i++) if (vk2_xfer_window_open_bank(i)) closed = 0;
            q3_recycled = closed;
            A("回收（交还三段）：%s", closed ? "三段都还回来了" : "**有段没还回来**");
        }
        if (fen) vkDestroyFence(vd.dev, fen, NULL);
        if (cp) vkDestroyCommandPool(vd.dev, cp, NULL);
    }

    // ------------------------------------------------------------------ 问 4：接管闭环
    Q("**接管闭环**（G1/G2 的接管版）：CPU 写影子 → 触碰标脏 → 帧开头上传 → 提交 → 等栅栏 → 回读 → "
      "与**逐字节独立重算**比对；再补「没标脏就不上传」这一半");
    int q4_ok = 0, q4_half_a = 0, q4_half_b = 0, q4_half_c = 0;
    {
        const uint64_t off = 1 * MB, len = 64 * KB;
        vk2_dirty *dd = vk2_pool_dirty(0);
        frame_out fo;

        // (a) 写 A + 标脏 → 帧 1 上传 → 回读 = A（逐字节独立重算）
        pat_fill(mem + off, len, seed);
        vk2_dirty_mark_cpu(dd, off, len);
        int ran_a = run_frame(&fo);
        memset(mem + off, 0, (size_t)len);
        q4_half_a = ran_a && vk2_readback_pool(mem, off, (size_t)len) && pat_check(mem + off, len, seed);
        A("(a) 写 A + 标脏 → 帧 1（搬 %llu 字节）→ 回读：逐字节等于期望 = %d",
          (unsigned long long)fo.moved, q4_half_a);

        // (b) 写 B、**不标脏** → 帧 2 不搬 → 回读**仍是 A**（"没有上传就看不到"这一半）
        pat_fill(mem + off, len, seed + 1);
        int ran_b = run_frame(&fo);
        memset(mem + off, 0, (size_t)len);
        int rb_b = ran_b && vk2_readback_pool(mem, off, (size_t)len);
        int still_a = rb_b && pat_check(mem + off, len, seed);
        q4_half_b = (fo.moved == 0 && still_a) ? 1 : 0;
        A("(b) 写 B、**不标脏** → 帧 2（搬 %llu 字节，应 0）→ 回读**仍是 A** = %d（旧的可见、新的不可见）",
          (unsigned long long)fo.moved, still_a);

        // (c) 再标脏 → 帧 3 上传 → 回读 = B
        pat_fill(mem + off, len, seed + 1);
        vk2_dirty_mark_cpu(dd, off, len);
        int ran_c = run_frame(&fo);
        memset(mem + off, 0, (size_t)len);
        q4_half_c = ran_c && vk2_readback_pool(mem, off, (size_t)len) && pat_check(mem + off, len, seed + 1);
        A("(c) 补标脏 → 帧 3（搬 %llu 字节）→ 回读：逐字节等于 B = %d", (unsigned long long)fo.moved, q4_half_c);

        q4_ok = (q4_half_a && q4_half_b && q4_half_c) ? 1 : 0;
        A("三半合起来 = %d ⇒ **上传是「客体写的字节到 GPU 眼前」的唯一通道**，而且它是显式的", q4_ok);
    }

    // ------------------------------------------------------------------ 问 5：脏 → 上传 → 清位 → 再触碰 → 再上传
    Q("**「脏 → 上传 → 清位 → 再触碰 → 再上传」**这条不变量（接管路径每帧重传就是靠它）");
    int q5_ok = 0;
    {
        const uint64_t off = 2 * MB, len = 32 * KB;
        vk2_dirty *dd = vk2_pool_dirty(0);
        uint64_t before = vk2_dirty_count(dd, VK2_DIRTY_CPU);
        pat_fill(mem + off, len, seed + 7);
        vk2_dirty_mark_cpu(dd, off, len);
        uint64_t dirty_before_up = vk2_dirty_any(dd, off, len, VK2_DIRTY_CPU);
        frame_out fo, fo2;
        run_frame(&fo);
        uint64_t dirty_after_up = vk2_dirty_any(dd, off, len, VK2_DIRTY_CPU);
        vk2_dirty_mark_cpu(dd, off, len);                 // 再触碰（模拟下一帧的绑定）
        uint64_t dirty_again = vk2_dirty_any(dd, off, len, VK2_DIRTY_CPU);
        run_frame(&fo2);
        q5_ok = (dirty_before_up == 1 && dirty_after_up == 0 && dirty_again == 1 &&
                 fo.moved >= len && fo2.moved >= len) ? 1 : 0;
        A("触碰后脏 = %llu（应 1）；上传后脏 = %llu（应 0）；再触碰后脏 = %llu（应 1）；"
          "第一帧搬 %llu、第二帧搬 %llu（都应 ≥ %llu）⇒ 不变量 %s",
          (unsigned long long)dirty_before_up, (unsigned long long)dirty_after_up,
          (unsigned long long)dirty_again, (unsigned long long)fo.moved, (unsigned long long)fo2.moved,
          (unsigned long long)len, q5_ok ? "成立" : "**不成立**");
        A("（触碰**之前**这一段所在的池里 CPU 脏页总数 %llu —— 说明这一段的脏是刚标上去的，账能对上）",
          (unsigned long long)before);
    }

    // ------------------------------------------------------------------ 问 6：GPU_DIRTY 不被上传覆盖
    Q("**GPU_DIRTY 不被上传覆盖**（旧路 `UPDATE_UB` / `COPY_B2B` 直写后备之后影子就过期了这条语义）");
    int q6_ok = 0;
    {
        const uint64_t off = 3 * MB, len = 64 * KB;
        vk2_dirty *dd = vk2_pool_dirty(0);
        vk2_dirty_clean(dd, off, len);
        vk2_dirty_mark_gpu(dd, off, len);            // 模拟"GPU 直写了后备"（UPDATE_UB 的 dst）
        frame_out fo;
        run_frame(&fo);
        uint64_t cpu_after = vk2_dirty_any(dd, off, len, VK2_DIRTY_CPU);
        uint64_t gpu_after = vk2_dirty_any(dd, off, len, VK2_DIRTY_GPU);
        q6_ok = (cpu_after == 0 && gpu_after == 1) ? 1 : 0;
        A("上传之后：CPU 脏 = %llu（应 0，**上传不许碰 GPU 写过的那一页**）、GPU 脏 = %llu（应 1，影子仍过期）"
          "⇒ %s", (unsigned long long)cpu_after, (unsigned long long)gpu_after,
          q6_ok ? "成立" : "**不成立**");
        A("这一帧搬了 %llu 字节（这一段没被搬，因为上传只看 CPU 脏位）", (unsigned long long)fo.moved);
    }

    // ------------------------------------------------------------------ 问 7：初始同步
    Q("**初始同步**（N4-b 给回放层的整批搬）：整池标脏 → 一次搬完 → 回读逐字节正确");
    int q7_ok = 0;
    g_init_bytes = 0; g_init_rounds = 0; g_init_us = 0;
    {
        int last = vk2_xfer_banks() - 1;      // 用一个与提交路径分开的段做整批搬
        pat_fill(mem, pool_bytes, seed + 3);
        vk2_pool_mark_all_cpu(0);             // 整池标脏（= 池刚登记时回放层做的事）
        vk2_xfer_stats xs0, xs1;
        vk2_xfer_stats_get(&xs0);
        uint64_t t0 = now_us();
        uint64_t total = vk2_xfer_initial_sync(last);
        uint64_t dt = now_us() - t0 + 1;
        vk2_xfer_stats_get(&xs1);
        A("整池 %llu MB 标脏 ⇒ 初始同步搬 %llu MB、%llu 轮、耗时 %llu ms（%.1f MB/ms）；窗口数 %llu → %llu",
          (unsigned long long)(pool_bytes >> 20), (unsigned long long)(total >> 20),
          (unsigned long long)xs1.initial_rounds, (unsigned long long)(dt / 1000),
          (double)(total >> 20) / ((double)dt / 1000.0),
          (unsigned long long)xs0.windows, (unsigned long long)xs1.windows);
        g_init_bytes = total; g_init_rounds = xs1.initial_rounds; g_init_us = dt / 1000;
        // 拿一个已 signal 的栅栏当生产者：**空帧**（没有脏页 ⇒ 不搬东西）提交一次即可
        frame_out fe;
        int ran = run_frame(&fe);
        size_t tail = 64 * KB;
        int rb1 = vk2_readback_pool(mem, 0, tail);
        int ck1 = pat_check(mem, tail, seed + 3);
        int rb2 = vk2_readback_pool(mem, pool_bytes - tail, tail);
        int ck2 = pat_check_at(mem + pool_bytes - tail, pool_bytes - tail, tail, seed + 3);
        q7_ok = (ran && fe.moved == 0 && rb1 && ck1 && rb2 && ck2) ? 1 : 0;
        A("整批搬完之后回读（头 %llu KB 与尾 %llu KB；中间那一次是**空帧**，搬 %llu 字节）："
          "头 rb=%d ck=%d；尾 rb=%d ck=%d；空帧 ran=%d ⇒ 逐字节等于期望 = %d",
          (unsigned long long)(tail >> 10), (unsigned long long)(tail >> 10),
          (unsigned long long)fe.moved, rb1, ck1, rb2, ck2, ran, q7_ok);
        // 取证：不对的话，**第一处不符**在哪、期望与实际各是什么（纪律：只报一句"不对"会把人引向怀疑判法）
        if (!q7_ok) {
            uint64_t bad = UINT64_MAX;
            for (uint64_t i = 0; i < tail; i++)
                if (mem[i] != pat_byte(i, seed + 3)) { bad = i; break; }
            if (bad == UINT64_MAX)
                for (uint64_t i = 0; i < tail; i++) {
                    uint64_t a = pool_bytes - tail + i;
                    if (mem[a] != pat_byte(a, seed + 3)) { bad = a; break; }
                }
            if (bad == UINT64_MAX)
                A("【黄】取证：按**池内绝对偏移**重算，两段都对 ⇒ 不对在别处（见上面那四个布尔：ran/moved/rb/ck）");
            else A("【黄】取证：池内偏移 %llu 处不符：期望 %#x、实际 %#x（前 4 字节 %#x %#x %#x %#x）",
                   (unsigned long long)bad, (unsigned)pat_byte(bad, seed + 3), (unsigned)mem[bad],
                   (unsigned)mem[bad & ~3ull], (unsigned)mem[(bad & ~3ull) + 1],
                   (unsigned)mem[(bad & ~3ull) + 2], (unsigned)mem[(bad & ~3ull) + 3]);
        }
    }

    // ------------------------------------------------------------------ 问 8：提交侧（回收钩子 + poll）
    Q("**提交侧接上**：槽回收钩子在「栅栏已经被观察到」的那一刻被调到吗（回放层的副作用与 GPU→池回读挂在这里）？"
      "`vk2_sub_poll` 的三种返回对不对？");
    int q8_hook_ok = 0, q8_poll_ok = 0, q8_poll_running = -1;
    {
        uint64_t c0 = g_hook_calls;
        vk2_sub_stats ss0, ss1;
        vk2_sub_stats_get(&ss0);
        const uint64_t off = 5 * MB, len = 32 * KB;
        vk2_dirty *dd = vk2_pool_dirty(0);
        frame_out fo;
        pat_fill(mem + off, len, seed + 11);
        vk2_dirty_mark_cpu(dd, off, len);
        g_hook_mode = 0;                       // 只记账，不回读（这一问只问"钩子到没到"）
        int ran = run_frame(&fo);
        vk2_sub_stats_get(&ss1);
        A("跑了 1 帧（frame %llu、搬 %llu 字节）：钩子调用 +%llu（这一帧 1 次；累计 %llu）、其中"
          "「调用时栅栏已经 signal」= %llu 次；`vk2_sub` 的回收数 %llu → %llu、提交 %llu → %llu",
          (unsigned long long)fo.frame, (unsigned long long)fo.moved,
          (unsigned long long)(g_hook_calls - c0), (unsigned long long)g_hook_calls,
          (unsigned long long)g_hook_signalled,
          (unsigned long long)ss0.reclaims, (unsigned long long)ss1.reclaims,
          (unsigned long long)ss0.submits, (unsigned long long)ss1.submits);
        q8_hook_ok = (ran && g_hook_calls - c0 == 1 && g_hook_signalled == g_hook_calls &&
                      ss1.submits == ss0.submits + 1) ? 1 : 0;
        A("⇒ 钩子 = 每次提交 1 次、且**只在那一次提交的栅栏已经 signal 之后**被调到 = %d（回收时刻就是"
          "「GPU 写完了」的时刻，回放层的三件事挂在这里才对）", q8_hook_ok);

        // poll 的两半：① 早已回收的 frame ⇒ 1；② 刚提交、GPU 还在忙 ⇒ 0
        uint64_t last = vk2_sub_last_frame();
        int p_old = vk2_sub_poll(fo.frame);
        A("`vk2_sub_poll(已经回收的 frame %llu)` = %d（应 1，**不阻塞**；这就是 `mtl_wait(fence, 0)` 的"
          "「看一眼」）", (unsigned long long)fo.frame, p_old);
        // 让这一次提交**真有 GPU 活**（128 MB 的拷贝，N3 量过 ≈4 ms）⇒ 提交后立刻 poll 必为"还在飞"。
        // 这是问"同时在飞"那条纪律的同一手（探针第一版栽在"GPU 快得来不及"上，见 TOOLS B⁶）。
        VkBuffer ba = VK_NULL_HANDLE, bb = VK_NULL_HANDLE;
        VkDeviceMemory ma = VK_NULL_HANDLE, mb = VK_NULL_HANDLE;
        const uint64_t big = 128ull * MB;
        int made = 0;
        {
            VkBufferCreateInfo bci;
            memset(&bci, 0, sizeof bci);
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = big;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkMemoryAllocateInfo mai;
            memset(&mai, 0, sizeof mai);
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            made = 1;
            if (vkCreateBuffer(vd.dev, &bci, NULL, &ba) != VK_SUCCESS) made = 0;
            if (made) { VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vd.dev, ba, &mr);
                mai.allocationSize = mr.size;
                uint32_t pick = UINT32_MAX;
                for (uint32_t i = 0; i < vd.mp.memoryTypeCount; i++)
                    if ((mr.memoryTypeBits & (1u << i)) &&
                        (vd.mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { pick = i; break; }
                if (pick == UINT32_MAX || vkAllocateMemory(vd.dev, &mai, NULL, &ma) != VK_SUCCESS) made = 0;
                else if (vkBindBufferMemory(vd.dev, ba, ma, 0) != VK_SUCCESS) made = 0; }
            if (made && vkCreateBuffer(vd.dev, &bci, NULL, &bb) != VK_SUCCESS) made = 0;
            if (made) { VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vd.dev, bb, &mr);
                mai.allocationSize = mr.size;
                uint32_t pick = UINT32_MAX;
                for (uint32_t i = 0; i < vd.mp.memoryTypeCount; i++)
                    if ((mr.memoryTypeBits & (1u << i)) &&
                        (vd.mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { pick = i; break; }
                if (pick == UINT32_MAX || vkAllocateMemory(vd.dev, &mai, NULL, &mb) != VK_SUCCESS) made = 0;
                else if (vkBindBufferMemory(vd.dev, bb, mb, 0) != VK_SUCCESS) made = 0; }
        }
        if (made) {
            uint64_t pf = 0;
            vk2_slot *s = vk2_sub_begin();
            if (s) {
                pf = s->frame;
                VkBufferCopy cp = { .srcOffset = 0, .dstOffset = 0, .size = big };
                vkCmdCopyBuffer(s->cb, ba, bb, 1, &cp);
                if (vk2_sub_commit()) {
                    q8_poll_running = vk2_sub_poll(pf);       // 提交完**立刻**问：应 0（还在飞）
                    // N4-c 的修（真机读数抓到的）：**客体按自己的超时等围栏**，不许被模块的 5 秒上限截短。
                    // 这里用 1 ms 超时去等一条 128 MB 的拷贝 ⇒ 必然超时（如实报失败、计数 +1），
                    // 然后正常等一次 ⇒ 完成（超时之后可恢复）；再按超时等一次已完成的 frame ⇒ 立刻 1。
                    uint64_t to0 = vk2_sub_user_timeouts();
                    int w_to = vk2_sub_wait_timeout(pf, 1000000ull);
                    uint64_t to1 = vk2_sub_user_timeouts();
                    int w = vk2_sub_wait(pf);
                    int p_done = vk2_sub_poll(pf);            // 等完之后：应 1
                    int w_done = vk2_sub_wait_timeout(pf, 1000000ull);
                    A("一次 128 MB 的 GPU 拷贝提交（frame %llu）后**立刻** poll = %d（应 0 = 还在飞）；"
                      "按**客体给的 1 ms 超时**等 = %d（应 0 = 超时，计数 %llu → %llu）；"
                      "正常等自己的栅栏（%d）之后再 poll = %d（应 1）；再按超时等已完成的 frame = %d（应 1，立刻）",
                      (unsigned long long)pf, q8_poll_running, w_to, (unsigned long long)to0,
                      (unsigned long long)to1, w, p_done, w_done);
                    q8_poll_ok = (p_old == 1 && q8_poll_running == 0 && w_to == 0 && to1 == to0 + 1 &&
                                  w && p_done == 1 && w_done == 1) ? 1 : 0;
                } else { vk2_sub_abort(); A("【红】那次大拷贝提交失败"); }
            }
        } else A("【黄】128 MB 的缓冲没建出来 ⇒ 这一半（「提交后立刻 poll = 还在飞」）没量到");
        if (ba) vkDestroyBuffer(vd.dev, ba, NULL);
        if (bb) vkDestroyBuffer(vd.dev, bb, NULL);
        if (ma) vkFreeMemory(vd.dev, ma, NULL);
        if (mb) vkFreeMemory(vd.dev, mb, NULL);
    }

    // ------------------------------------------------------------------ 问 9：往返型（GPU→池回读）闭环
    Q("**往返型（GPU→池回读）**：GPU 直写过后备之后 —— ① **不等**就读影子 = 旧值；② 等栅栏（钩子不回读）还是旧值；"
      "③ 回收钩子按那次提交的栅栏回读之后 = 新值（逐字节独立重算）");
    int q9_naive = 0, q9_afterwait = 0, q9_afterrb = 0, q9_ok = 0;
    {
        const uint64_t src_off = 5 * MB + 128 * KB, dst_off = 4 * MB, len = 64 * KB;
        vk2_dirty *dd = vk2_pool_dirty(0);
        frame_out fo;
        // 先把两段图案都搬上去（源 = B、目标 = A）：这样"GPU 拷贝"的源在后备里是确定的
        pat_fill(mem + src_off, len, seed + 21);
        pat_fill(mem + dst_off, len, seed + 22);
        vk2_dirty_mark_cpu(dd, src_off, len);
        vk2_dirty_mark_cpu(dd, dst_off, len);
        run_frame(&fo);

        // 半 (a)：录一次 GPU 直写（= COPY_B2B / UPDATE_UB 的目标），**提交但不等待** ⇒ 影子仍是 A
        g_hook_mode = 0;
        g_nnote = 0;
        vk2_slot *s = vk2_sub_begin();
        uint64_t frame_x = 0;
        if (s) {
            frame_x = s->frame;
            VkBuffer cbk = VK_NULL_HANDLE; uint64_t choff = 0, srcoff = 0, dstoff = 0;
            uint64_t cbytes = vk2_pool_chunk_bytes();
            // 反查两块：都用池内偏移 → 块缓冲 + 块内偏移
            vk2_addr((uint64_t)(uintptr_t)(mem + src_off), len, &cbk, &srcoff);
            vk2_addr((uint64_t)(uintptr_t)(mem + dst_off), len, &cbk, &dstoff);
            (void)cbytes;
            VkBufferCopy cp = { .srcOffset = srcoff, .dstOffset = dstoff, .size = len };
            vkCmdCopyBuffer(s->cb, cbk, cbk, 1, &cp);
            vk2_dirty_mark_gpu(dd, dst_off, len);                 // 回放层在这一刻标的
            g_note[g_nnote].addr = (uint64_t)(uintptr_t)(mem + dst_off);
            g_note[g_nnote].len = len; g_nnote++;
            vk2_sub_commit();
        }
        q9_naive = pat_check(mem + dst_off, len, seed + 22);       // 不等待就读：应仍是 A
        A("(a) GPU 直写提交（frame %llu）之后**不等待**读影子：仍是 A = %d（应 1 —— 新地基下 GPU 写的是后备，"
          "影子不会自己变）", (unsigned long long)frame_x, q9_naive);

        // 半 (b)：等它的栅栏（钩子 mode 0 = **不回读**）⇒ 影子仍是 A（"等栅栏 ≠ 可见"）
        int w = vk2_sub_wait(frame_x);
        q9_afterwait = w && pat_check(mem + dst_off, len, seed + 22);
        A("(b) 等这一 frame 的定向栅栏（%d）之后、**没有回读**：仍是 A = %d（应 1 —— G2 的第二半：等栅栏不等于可见）",
          w, q9_afterwait);

        // 半 (c)：再来一次，**钩子按那次提交的栅栏回读** ⇒ 影子 = B
        pat_fill(mem + src_off, len, seed + 23);
        vk2_dirty_mark_cpu(dd, src_off, len);
        run_frame(&fo);                                            // 把新的源搬上去
        g_hook_mode = 1;
        g_nnote = 0;
        s = vk2_sub_begin();
        uint64_t frame_y = 0;
        if (s) {
            frame_y = s->frame;
            VkBuffer cbk = VK_NULL_HANDLE; uint64_t srcoff = 0, dstoff = 0;
            vk2_addr((uint64_t)(uintptr_t)(mem + src_off), len, &cbk, &srcoff);
            vk2_addr((uint64_t)(uintptr_t)(mem + dst_off), len, &cbk, &dstoff);
            VkBufferCopy cp = { .srcOffset = srcoff, .dstOffset = dstoff, .size = len };
            vkCmdCopyBuffer(s->cb, cbk, cbk, 1, &cp);
            vk2_dirty_mark_gpu(dd, dst_off, len);
            g_note[g_nnote].addr = (uint64_t)(uintptr_t)(mem + dst_off);
            g_note[g_nnote].len = len; g_nnote++;
            vk2_sub_commit();
        }
        int w2 = vk2_sub_wait(frame_y);                            // 回收钩子在这里回读 + 清位
        q9_afterrb = w2 && pat_check(mem + dst_off, len, seed + 23);
        A("(c) 同样一次 GPU 直写（frame %llu），**回收钩子按它自己的栅栏回读**（回读 %llu 字节、清位 %llu 字节）："
          "影子 = B = %d（应 1，逐字节等于独立重算的期望）", (unsigned long long)frame_y,
          (unsigned long long)g_hook_rb_bytes, (unsigned long long)g_hook_settled, q9_afterrb);
        q9_ok = (q9_naive && q9_afterwait && q9_afterrb) ? 1 : 0;
        A("三半合起来 = %d ⇒ **「GPU 写后备、CPU 读影子」必须显式回读**，而回读挂在「槽回收 + 那次提交自己的"
          "栅栏」这个时刻上（这也是 N4-c 给回放层补的那一条）", q9_ok);
    }

    // ------------------------------------------------------------------ 问 10：半页竞态的两条对策
    Q("**半页竞态的对策**（N2 量出：GPU 写过的页被 CPU 部分写）：① 帧开头上传**跳过**还带 GPU 脏的页；"
      "② 回收时**字节精确**回读那一段 + 只清 GPU 位 ⇒ 同一页上「GPU 写过的区间以 GPU 为准、区间之外的"
      "CPU 字节保住」（这就是 N2 说「要页内字节级记账」的那一半）");
    int q10_skip = 0, q10_merge = 0, q10_ok = 0;
    {
        const uint64_t pg = 4096;                        // 一页（= 脏位图粒度）
        const uint64_t off = 6 * MB, src_off = 7 * MB;   // 目标页 / 源页（都已上传过图案）
        vk2_dirty *dd = vk2_pool_dirty(0);
        frame_out fo;
        vk2_xfer_stats xs0, xs1;
        // 先把源页写成图案 D2、目标页写成图案 D（都搬上去）
        pat_fill(mem + src_off, pg, seed + 31);
        pat_fill(mem + off, pg, seed + 32);
        vk2_dirty_mark_cpu(dd, src_off, pg);
        vk2_dirty_mark_cpu(dd, off, pg);
        g_hook_mode = 0;
        run_frame(&fo);

        // ① 帧 X：GPU 直写目标页的**前半**（真拷贝），提交但**不等** ⇒ 整页带着"GPU 脏"
        const uint64_t gpu_half = pg / 2;
        g_nnote = 0;
        vk2_slot *s = vk2_sub_begin();
        uint64_t frame_x = 0;
        if (s) {
            frame_x = s->frame;
            VkBuffer cbk = VK_NULL_HANDLE; uint64_t so = 0, dof2 = 0;
            vk2_addr((uint64_t)(uintptr_t)(mem + src_off), gpu_half, &cbk, &so);
            vk2_addr((uint64_t)(uintptr_t)(mem + off), gpu_half, &cbk, &dof2);
            VkBufferCopy cp = { .srcOffset = so, .dstOffset = dof2, .size = gpu_half };
            vkCmdCopyBuffer(s->cb, cbk, cbk, 1, &cp);
            vk2_dirty_mark_gpu(dd, off, gpu_half);          // 页粒度 ⇒ 整页都算"GPU 脏"
            g_note[g_nnote].addr = (uint64_t)(uintptr_t)(mem + off);
            g_note[g_nnote].len = gpu_half; g_nnote++;      // 回放层记的是**实际写过的区间**（字节精确）
            vk2_sub_commit();
        }
        A("帧 X（frame %llu）：GPU 把源页的**前半** 2 KiB 拷进目标页前半（= 一次 `COPY_B2B`/`UPDATE_UB` 的目标"
          "直写），提交但**不等** ⇒ 目标**页**带着 GPU 脏（脏位是页粒度的，回放层记的区间是字节精确的）",
          (unsigned long long)frame_x);

        // ② CPU 只写这一页的**后半** 2 KiB = 图案 E，并标 CPU 脏（这就是"半页竞态"的现场）
        const uint64_t half = pg / 2, cpu_pat = seed + 33;
        pat_fill(mem + off + half, half, cpu_pat);
        int noticed = vk2_dirty_any_gpu((uint64_t)(uintptr_t)(mem + off), pg);   // 回放层的"撞上了吗"那一问
        vk2_dirty_mark_cpu(dd, off + half, half);
        A("CPU 只写后半 2 KiB（图案 E）并标 CPU 脏；`vk2_dirty_any_gpu(整页)` = %d（应 1 —— 绑定点会看到"
          "「这次触碰撞上了 GPU 写过的页」）", noticed);

        // ③ 跑一帧（帧 Y）：上传必须**跳过**这一页（不拿旧影子盖掉在飞的 GPU 写），页保持 CPU 脏
        vk2_xfer_stats_get(&xs0);
        run_frame(&fo);
        vk2_xfer_stats_get(&xs1);
        uint64_t cpu_still = vk2_dirty_any(dd, off, pg, VK2_DIRTY_CPU);
        q10_skip = (xs1.up_skip_gpu > xs0.up_skip_gpu && cpu_still == 1) ? 1 : 0;
        A("帧 Y 上传：跳过 GPU 脏页 %llu → %llu（应 +≥1，跳过 %llu 字节）、这一页的 CPU 脏仍在 = %llu（应 1）"
          "⇒ 对策① %s", (unsigned long long)xs0.up_skip_gpu, (unsigned long long)xs1.up_skip_gpu,
          (unsigned long long)(xs1.up_skip_gpu_bytes - xs0.up_skip_gpu_bytes), (unsigned long long)cpu_still,
          q10_skip ? "成立" : "**不成立**");

        // ④ 等帧 X 的栅栏（若还没等）→ **字节精确**回读那一段 + 清 GPU 位（= 回收钩子做的事）
        // 栅栏的取法：先确保帧 X 做完（单队列 ⇒ 之后提交的栅栏 signal 就意味着 X 也做完了），再用**一次空帧**
        // 自己的栅栏（在等它之前把句柄取出来）当"生产者已完成"的凭据 —— 这正是回放层在回收钩子里拿到的那个。
        // N4-c: make sure frame X is done, then use a fresh empty frame's own fence (captured before waiting) as the
        // "producers are done" token -- exactly what the replay layer's reclaim hook is handed.
        if (vk2_sub_poll(frame_x) != 1) vk2_sub_wait(frame_x);
        uint64_t rb = 0, st = 0;
        {
            vk2_slot *s3 = vk2_sub_begin();
            if (s3) {
                uint64_t f3 = s3->frame;
                if (vk2_sub_commit()) {
                    VkFence fen3 = vk2_sub_fence(f3);      // **等之前**取出来（回收之后就拿不到句柄了）
                    if (fen3 && vk2_sub_wait(f3)) {
                        vk2_readback_after(fen3);          // 此刻它已经 signal
                        rb = vk2_readback_addr((uint64_t)(uintptr_t)(mem + off), gpu_half) ? gpu_half : 0;
                        st = vk2_gpu_settle((uint64_t)(uintptr_t)(mem + off), gpu_half);
                    }
                } else vk2_sub_abort();
            }
        }
        uint64_t gpu_left = vk2_dirty_any(dd, off, pg, VK2_DIRTY_GPU);
        uint64_t cpu_left = vk2_dirty_any(dd, off, pg, VK2_DIRTY_CPU);
        // 期望按**独立重算**核（`pat_fill` 是从 0 开始填的 ⇒ 两半都用 pat_check，不是 pat_check_at）
        int front_ok = pat_check(mem + off, half, seed + 31);           // 前半 = GPU 拷贝进来的源页内容
        int back_ok = pat_check(mem + off + half, half, cpu_pat);       // 后半 = CPU 刚写的图案 E（**保住了**）
        q10_merge = (rb == gpu_half && !gpu_left && cpu_left == 1 && front_ok && back_ok) ? 1 : 0;
        A("回收时刻那两下（对**那段 2 KiB**）：回读 %llu 字节（应 %llu）、清 GPU 位 %llu 字节；之后影子 = "
          "前半（GPU 的源页图案）%d / 后半（CPU 的图案 E）%d、GPU 脏 = %llu（应 0）、CPU 脏 = %llu（应 1，"
          "还要上传）⇒ 对策② %s", (unsigned long long)rb, (unsigned long long)gpu_half, (unsigned long long)st,
          front_ok, back_ok, (unsigned long long)gpu_left, (unsigned long long)cpu_left,
          q10_merge ? "成立" : "**不成立**");

        // ⑤ 再跑一帧：这一页（现在只有 CPU 脏）被整页上传 ⇒ 后备 = 前半 GPU 的 + 后半 CPU 的（回读验证）
        vk2_xfer_stats_get(&xs0);
        run_frame(&fo);
        vk2_xfer_stats_get(&xs1);
        int up_now = (xs1.upload_bytes > xs0.upload_bytes);
        uint64_t saved_front = 0, saved_back = 0;
        // 回读（这一次是"整页从后备读回影子"）之后按独立期望核
        memset(mem + off, 0, (size_t)pg);
        int rbv = vk2_readback_pool(mem, off, (size_t)pg);
        saved_front = (uint64_t)pat_check(mem + off, half, seed + 31);
        saved_back = (uint64_t)pat_check(mem + off + half, half, cpu_pat);
        A("帧 Z 再上传（搬了 %llu 字节、上传字节 %llu → %llu）：回读整页核 —— 前半 = GPU 写的 %llu、"
          "后半 = CPU 写的 %llu ⇒ 两个写者都保住了 = %d",
          (unsigned long long)fo.moved, (unsigned long long)(xs0.upload_bytes >> 10),
          (unsigned long long)(xs1.upload_bytes >> 10), (unsigned long long)saved_front,
          (unsigned long long)saved_back, (rbv && up_now && saved_front && saved_back) ? 1 : 0);
        q10_ok = (q10_skip && q10_merge && rbv && up_now && saved_front && saved_back) ? 1 : 0;
        g_hook_mode = 0;
    }

    // ------------------------------------------------------------------ 问 11：结论键值
    Q("结论键值 + `vkQueueWaitIdle` 计数（新地基里必须恒为 0）");
    vk2_xfer_stats xs;
    vk2_sub_stats  ss;
    vk2_xfer_stats_get(&xs);
    vk2_sub_stats_get(&ss);
    vk2_pool_stats_get(&pst);
    uint64_t banks_with_windows = 0;
    for (int i = 0; i < vk2_xfer_banks(); i++) {
        vk2_bank_stats bs;
        vk2_xfer_bank_stats_get(i, &bs);
        if (bs.windows) banks_with_windows++;
    }
    printf("\n--- VK2_TAKEOVER_SUMMARY ---\n");
    printf("VK2_GFX2_ENABLED=%d\n", vk2_gfx2_enabled());
    printf("POOL_ADD_OK=%d\n", ok_add);
    printf("POOL_NO_REGION=%llu\n", (unsigned long long)pst.no_region);
    printf("POOL_SMALL=%llu\n", (unsigned long long)pst.small);
    printf("POOL_FAIL=%llu\n", (unsigned long long)pst.pool_fail);
    printf("BACKING_MB=%llu\n", (unsigned long long)(pst.backing_bytes >> 20));
    printf("ADDR_HIT=%d\n", q2_hit);
    printf("ADDR_OFF_ADVANCES=%d\n", q2_off_ok);
    printf("ADDR_OVER_POOL_REFUSED=%d\n", q2_over);
    printf("ADDR_OUTSIDE_MISS=%d\n", q2_miss);
    printf("ADDR_NO_BACKING_MISS=%d\n", q2_noback);
    printf("BANKS=%d\n", banks);
    printf("BANKS_OK=%d\n", q3_banks_ok);
    printf("THREE_SLOTS_WINDOWS=%d\n", q3_all_open);
    printf("THREE_SLOTS_MOVED=%d\n", q3_all_moved);
    printf("THREE_SLOTS_RECYCLED=%d\n", q3_recycled);
    printf("SLOT_BANKS_OK=%d\n", q3_slot_banks_ok);
    printf("THREE_SLOTS_SUBMITTED=%d\n", q3_submitted);
    printf("BANKS_WITH_WINDOWS=%llu\n", (unsigned long long)banks_with_windows);
    printf("TAKEOVER_ROUNDTRIP=%d\n", q4_ok);
    printf("REDIRTY_INVARIANT=%d\n", q5_ok);
    printf("GPU_DIRTY_SURVIVES=%d\n", q6_ok);
    printf("INIT_SYNC_OK=%d\n", q7_ok);
    // N4-c 的三问
    printf("REAP_HOOK_OK=%d\n", q8_hook_ok);
    printf("REAP_HOOK_CALLS=%llu\n", (unsigned long long)g_hook_calls);
    printf("REAP_HOOK_SIGNALLED=%llu\n", (unsigned long long)g_hook_signalled);
    printf("SUB_POLL_OK=%d\n", q8_poll_ok);
    printf("SUB_POLL_RUNNING=%d\n", q8_poll_running);      // 提交后立刻 poll 的读数（应 0）
    printf("ROUNDTRIP_POOL_OK=%d\n", q9_ok);
    printf("ROUNDTRIP_NAIVE_STALE=%d\n", q9_naive);       // 不等就读 = 旧值
    printf("ROUNDTRIP_AFTERWAIT_STALE=%d\n", q9_afterwait); // 等栅栏仍不给可见性
    printf("ROUNDTRIP_AFTER_RB=%d\n", q9_afterrb);        // 回收钩子回读之后 = 新值
    printf("HALFPAGE_OK=%d\n", q10_ok);
    printf("HALFPAGE_SKIP=%d\n", q10_skip);
    printf("HALFPAGE_MERGE=%d\n", q10_merge);
    printf("INIT_SYNC_BYTES=%llu\n", (unsigned long long)g_init_bytes);
    printf("INIT_SYNC_ROUNDS=%llu\n", (unsigned long long)g_init_rounds);
    printf("INIT_SYNC_MS=%llu\n", (unsigned long long)g_init_us);   // 时间字段（抖动，按区间报）
    printf("XFER_WINDOWS=%llu\n", (unsigned long long)xs.windows);
    printf("XFER_UPLOAD_BYTES=%llu\n", (unsigned long long)xs.upload_bytes);
    printf("XFER_COPIES=%llu\n", (unsigned long long)xs.copies);
    printf("XFER_BARRIERS=%llu\n", (unsigned long long)xs.barriers);
    printf("XFER_RING_FULL=%llu\n", (unsigned long long)xs.ring_full);
    printf("XFER_IDLE_CALLS=%llu\n", (unsigned long long)xs.idle_calls);
    // N4-c 的搬运侧读数
    printf("XFER_RB_ADDR_CALLS=%llu\n", (unsigned long long)xs.rb_addr_calls);
    printf("XFER_RB_ADDR_BYTES=%llu\n", (unsigned long long)xs.rb_addr_bytes);
    printf("XFER_GPU_SETTLED=%llu\n", (unsigned long long)xs.gpu_settled);
    printf("XFER_UP_SKIP_GPU_PAGES=%llu\n", (unsigned long long)xs.up_skip_gpu);
    printf("XFER_UP_SKIP_GPU_BYTES=%llu\n", (unsigned long long)xs.up_skip_gpu_bytes);
    printf("SUB_COMMITS=%llu\n", (unsigned long long)ss.commits);
    printf("SUB_SUBMITS=%llu\n", (unsigned long long)ss.submits);
    printf("SUB_SUBMIT_FAIL=%llu\n", (unsigned long long)ss.submit_fail);
    printf("SUB_UPLOAD_MARKS=%llu\n", (unsigned long long)ss.upload_marks);
    printf("SUB_UPLOAD_RECYCLED=%llu\n", (unsigned long long)ss.upload_recycled);
    printf("SUB_IDLE_CALLS=%llu\n", (unsigned long long)ss.idle_calls);
    printf("LOOKUPS=%llu\n", (unsigned long long)pst.lookups);
    printf("HITS=%llu\n", (unsigned long long)pst.hits);
    printf("MISSES=%llu\n", (unsigned long long)pst.misses);
    printf("CROSS=%llu\n", (unsigned long long)pst.cross);
    printf("--- END ---\n");

    vk2_sub_fini();
    vk2_xfer_fini();
    vk2_pool_fini();
    munmap(mem, (size_t)pool_bytes);
    vk_close(&vd);

    int all = ok_add && q2_hit && q2_off_ok && q2_over && q2_miss && q2_noback && q3_banks_ok &&
              q3_slot_banks_ok && q3_all_open && q3_all_moved && q3_recycled &&
              q4_ok && q5_ok && q6_ok && q7_ok &&
              xs.idle_calls == 0 && ss.idle_calls == 0;
    printf("\n结论：%s（判据全过 = 1）\n", all ? "**新地基的 GPU 侧接管自证通过**" : "**有判据没过，见上面的【红】/【答】**");
    return all ? 0 : 2;
}
