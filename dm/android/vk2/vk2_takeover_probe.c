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
//   问 7 结论键值（`--- VK2_TAKEOVER_SUMMARY ---`）+ `vkQueueWaitIdle` 计数（两个模块都必须是 0）
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

    // ------------------------------------------------------------------ 问 8：结论键值
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
    printf("INIT_SYNC_BYTES=%llu\n", (unsigned long long)g_init_bytes);
    printf("INIT_SYNC_ROUNDS=%llu\n", (unsigned long long)g_init_rounds);
    printf("INIT_SYNC_MS=%llu\n", (unsigned long long)g_init_us);   // 时间字段（抖动，按区间报）
    printf("XFER_WINDOWS=%llu\n", (unsigned long long)xs.windows);
    printf("XFER_UPLOAD_BYTES=%llu\n", (unsigned long long)xs.upload_bytes);
    printf("XFER_COPIES=%llu\n", (unsigned long long)xs.copies);
    printf("XFER_BARRIERS=%llu\n", (unsigned long long)xs.barriers);
    printf("XFER_RING_FULL=%llu\n", (unsigned long long)xs.ring_full);
    printf("XFER_IDLE_CALLS=%llu\n", (unsigned long long)xs.idle_calls);
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
