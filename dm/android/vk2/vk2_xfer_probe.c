// 新地基 N2（脏记账 + 上传/回读窗口 + 显式屏障）的**自证探针** —— 把判据 **G1 / G2** 做成一问一答。
//
// 这是「可见性」第一次被真机读数建立起来的一轮：N1 只证明「池能建、地址能反查」，谁也没证明**影子上的字节
// 能不能到 GPU 眼前、GPU 写的东西能不能回到 CPU 视野里**。本探针的六问就是这两件事的一问一答：
//   问 1 这次走哪条路（`DM_GFX2` 可读自证，缺省关）
//   问 2 脏记账自证：位图大小对账、**与朴素模型逐位对比**（顺带验「并 run」）、扫描耗时三档、半页竞态
//   问 3 **G1**：往返闭环逐字节正确（CPU 写影子 → 上传 → GPU 拷一遍 → 回读 → 与**逐字节重算**的期望比对）
//   问 4 **G2**：**不等待就读 = 旧值** / **等完栅栏仍不回读 = 还是旧值** / **回读之后 = 新值**；
//        再补两问：没指定要等谁 ⇒ 拒绝回读；等一个永远不 signal 的栅栏 ⇒ 如实报超时失败
//   问 5 机制读数（**不是帧读数**）：全脏 4 GB 的 run 数、真搬 512 MiB 的窗口数与吞吐、一张 1600×900 贴图
//        单段脏 vs 拆成 64 处的 run 数
//   问 6 结论键值（`--- VK2_XFER_SUMMARY ---`）
//
// 跑法（一律走项目内 `.venv` Python）：
//   & "<仓库>/.venv/Scripts/python.exe" -X utf8 .work/probes.py run vk2_xfer_probe --serial 8b1f1e50 [--tag r1]
// 多轮：`.work/vk2_pool_rounds.py --probe vk2_xfer_probe --rounds 4`
// 读数落 `.work/logs/probe-<串号>-vk2_xfer_probe[-tag].txt`。
//
// 开关：DM_VK2_POOL_MB（缺省 4096）、DM_VK2_OPS（问 2 的随机记账次数，缺省 20000）、
//       DM_VK2_RING_MB（缺省 16）、DM_VK2_MOVE_MB（问 5 真搬多少，缺省 512）、DM_VK2_SEED（缺省 20261016）
//
// ---------------------------------------------------------------------------------------------------------------
// The self-proving probe of N2: gate G1 (a byte-exact round trip through the upload and readback windows) and G2
// (visibility is explicitly established: reading without waiting sees the old value, a signalled fence alone still
// does not update the CPU's copy, and only a readback yields the new value). Plus a self-proof of the dirty bitmap
// against a naive model, and clearly-labelled mechanism readings (no frame readings: the six baseline fields stay
// empty until the replay layer is taken over in N4).
#define _GNU_SOURCE 1
#include "vk2_xfer.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MB (1024ull * 1024ull)
#define PAGE 4096ull
#define KB (1024ull)

static uint64_t g_qn;
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
static void xlog(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("[搬] "); vprintf(fmt, ap); printf("\n");
    va_end(ap); fflush(stdout);
}
static void dlog(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("[脏] "); vprintf(fmt, ap); printf("\n");
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
static uint64_t g_rng;
static void rng_seed(uint64_t s) { g_rng = s ? s : 0x9e3779b97f4a7c15ull; }
static uint64_t rng_next(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}
// 逐字节重算的期望：判据不许拿「影子自己」当对照，必须**独立重算** | the expectation is recomputed per byte
static uint8_t pat_byte(uint64_t i, uint64_t seed) {
    return (uint8_t)(((i * 1103515245ull + seed * 12345ull + (i >> 7)) >> 13) ^ (i * 31ull));
}
static void pat_fill(uint8_t *p, uint64_t n, uint64_t seed) {
    for (uint64_t i = 0; i < n; i++) p[i] = pat_byte(i, seed);
}

// 数 run：给 `vk2_dirty_scan` 一个只数数的回调（顺带拿回 run 覆盖的字节数）
// Counting callback for vk2_dirty_scan (also collects the bytes covered by the runs).
static uint64_t g_runs, g_run_bytes;
static int count_run(uint64_t off, uint64_t len, void *ud) {
    (void)off; (void)ud;
    g_runs++; g_run_bytes += len;
    return 0;
}
static uint64_t count_runs(vk2_dirty *d, int state, uint64_t lo, uint64_t hi, uint64_t *bytes) {
    g_runs = 0; g_run_bytes = 0;
    if (!vk2_dirty_scan(d, state, lo, hi, count_run, NULL)) { if (bytes) *bytes = 0; return 0; }
    if (bytes) *bytes = g_run_bytes;
    return g_runs;
}
// 把 run 摊回页集合（验「并 run」这一步）| spread the runs back into a page set (checks the merge step)
static uint8_t *g_seen;
static uint64_t g_seen_pages;
static int seen_run(uint64_t off, uint64_t len, void *ud) {
    (void)ud;
    uint64_t gran = vk2_dirty_gran_default();
    uint64_t p0 = off / gran, p1 = (off + len) / gran;
    if (p1 > g_seen_pages) p1 = g_seen_pages;
    for (uint64_t p = p0; p < p1; p++) g_seen[p] = 1;
    return 0;
}

// ---------------------------------------------------------------- 设备
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

static uint32_t pick_type(const probe_dev *v, uint32_t bits, VkMemoryPropertyFlags need) {
    uint32_t best = UINT32_MAX; VkDeviceSize bestsz = 0;
    for (uint32_t i = 0; i < v->mp.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        if ((v->mp.memoryTypes[i].propertyFlags & need) != need) continue;
        VkDeviceSize sz = v->mp.memoryHeaps[v->mp.memoryTypes[i].heapIndex].size;
        if (best == UINT32_MAX || sz > bestsz) { best = i; bestsz = sz; }
    }
    return best;
}

static int vk_open(probe_dev *v) {
    memset(v, 0, sizeof *v);
    VkApplicationInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "dm-vk2-xfer-probe";
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

// ---------------------------------------------------------------- 探针自己的「窗口」（一条 cb + 一个栅栏）
// 上传窗口录进这条 cb；提交后由探针等它自己的栅栏 —— 这正是 §9.5 说的「调用方负责提交并等自己的栅栏」。
// 命令池上必须有 RESET 标志：这条 cb 会被反复重录（N0 的第二个探针 bug 就栽在这里）。
typedef struct {
    VkCommandPool cpool;
    VkCommandBuffer cb;
    VkFence fence;
} window;

static int window_make(probe_dev *v, window *w) {
    memset(w, 0, sizeof *w);
    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = v->qfi;
    if (vkCreateCommandPool(v->dev, &pci, NULL, &w->cpool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof cbai);
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = w->cpool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(v->dev, &cbai, &w->cb) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof fci);
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    return vkCreateFence(v->dev, &fci, NULL, &w->fence) == VK_SUCCESS;
}
static void window_free(probe_dev *v, window *w) {
    if (w->fence) vkDestroyFence(v->dev, w->fence, NULL);
    if (w->cpool) vkDestroyCommandPool(v->dev, w->cpool, NULL);
}
static int cb_begin(window *w) {
    vkResetCommandBuffer(w->cb, 0);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(w->cb, &bi) == VK_SUCCESS;
}
static int cb_end_submit(probe_dev *v, window *w, const char *what) {
    if (vkEndCommandBuffer(w->cb) != VK_SUCCESS) return 0;
    vkResetFences(v->dev, 1, &w->fence);
    VkSubmitInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &w->cb;
    VkResult r = vkQueueSubmit(v->q, 1, &si, w->fence);
    if (r != VK_SUCCESS) { A("  【红】%s 提交失败：VkResult %d", what, (int)r); return 0; }
    return 1;
}
static int wait_window(probe_dev *v, window *w, const char *what) {
    VkResult r = vkWaitForFences(v->dev, 1, &w->fence, VK_TRUE, 5000ull * 1000000ull);
    if (r != VK_SUCCESS) { A("  【红】%s 等栅栏失败：VkResult %d", what, (int)r); return 0; }
    return 1;
}
// 显式的屏障（§9.4）：写完之后让后来者看得见 | the explicit barrier: writer then readers
static void probe_barrier(VkCommandBuffer cb, VkBuffer buf, uint64_t off, uint64_t size,
                          VkAccessFlags src, VkAccessFlags dst) {
    VkBufferMemoryBarrier bb;
    memset(&bb, 0, sizeof bb);
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb.srcAccessMask = src;
    bb.dstAccessMask = dst;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = buf; bb.offset = off; bb.size = size;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
}

// ---------------------------------------------------------------- 主流程
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("=== vk2_xfer_probe：新地基 N2（脏记账 + 上传/回读窗口 + 显式屏障）的自证探针 ===\n");
    printf("构建：%s %s；pid=%d；页大小=%ld 字节\n", __DATE__, __TIME__, (int)getpid(), sysconf(_SC_PAGESIZE));
    vk2_xfer_set_log(xlog);
    vk2_dirty_set_log(dlog);

    uint64_t pool_mb = env_u64("DM_VK2_POOL_MB", 4096);
    uint64_t ops = env_u64("DM_VK2_OPS", 20000);
    uint64_t seed = env_u64("DM_VK2_SEED", 20261016);
    uint64_t ring_mb = env_u64("DM_VK2_RING_MB", 16);
    uint64_t move_mb = env_u64("DM_VK2_MOVE_MB", 512);

    probe_dev v;
    if (!vk_open(&v)) { fprintf(stderr, "【红】Vulkan 打不开\n"); return 2; }
    printf("设备 %s；Vulkan %u.%u.%u\n", v.props.deviceName, VK_VERSION_MAJOR(v.props.apiVersion),
           VK_VERSION_MINOR(v.props.apiVersion), VK_VERSION_PATCH(v.props.apiVersion));
    unsigned long long mem_start = mem_avail_kb();
    printf("MemAvailable 起始 %llu MB\n", (unsigned long long)(mem_start >> 10));

    // ---- 结果键（末尾汇总）----
    int q_route_gfx2 = -1, q_gfx2_flip = 0;
    uint64_t q_gran = 0, q_bitmap_bytes = 0, q_bitmap_one_bit = 0;
    uint64_t q_naive_bit_mismatch = 0, q_naive_run_mismatch = 0, q_naive_pages = 0, q_naive_ops = 0;
    uint64_t q_scan_empty_us = 0, q_scan_sparse_us = 0, q_scan_sparse_runs = 0;
    uint64_t q_scan_all_us = 0, q_scan_all_runs = 0;
    uint64_t q_hazard_added = 0;
    int q_g1_ok = 0, q_g1_cases = 0;
    int q_g2_old_nowait = 0, q_g2_old_after_fence = 0, q_g2_new_after_rb = 0;
    int q_g2_nofence_refused = 0, q_g2_timeout_failed = 0;
    uint64_t q_tex_runs = 0, q_tex_bytes = 0, q_tex_us = 0;
    uint64_t q_tile_runs = 0, q_tile_bytes = 0, q_tile_us = 0;
    uint64_t q_move_windows = 0, q_move_bytes = 0, q_move_us = 0;
    uint64_t q_move_copies = 0, q_move_split = 0, q_move_ring_full = 0;
    uint64_t q_idle_calls = 0, q_barriers = 0;

    // ---- 池与两个窗口 ----
    uint64_t pool_bytes = pool_mb * MB;
    void *shadow = mmap(NULL, (size_t)pool_bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (shadow == MAP_FAILED) {
        fprintf(stderr, "【红】mmap %llu MB 失败：%s\n", (unsigned long long)pool_mb, strerror(errno));
        return 2;
    }
    vk2_dev d;
    memset(&d, 0, sizeof d);
    d.dev = v.dev; d.phys = v.phys; d.qfi = v.qfi; d.q = v.q; d.mp = v.mp; d.props = v.props; d.m3 = v.m3;
    if (!vk2_pool_init(&d, 0)) { fprintf(stderr, "【红】vk2_pool_init 失败\n"); return 2; }
    if (!vk2_xfer_init(&d, ring_mb * MB, 0)) { fprintf(stderr, "【红】vk2_xfer_init 失败\n"); return 2; }
    if (!vk2_pool_add(shadow, pool_bytes, "probe-xfer")) { fprintf(stderr, "【红】池登记失败\n"); return 2; }
    window w;
    if (!window_make(&v, &w)) { fprintf(stderr, "【红】窗口（cpool/cb/fence）建不出来\n"); return 2; }
    const vk2_pool *pool = vk2_pool_at(0);
    vk2_dirty *dm = vk2_pool_dirty(0);
    const uint64_t CB = vk2_pool_chunk_bytes();
    if (!pool || !dm) { fprintf(stderr, "【红】池/脏位图拿不到\n"); return 2; }
    A("池 %p+%llu MB 就绪：%d 块、块 %llu MB、脏位图粒度 %u 字节；暂存环 %llu MB",
      shadow, (unsigned long long)pool_mb, pool->nchunk, (unsigned long long)(CB >> 20),
      vk2_dirty_gran_default(), (unsigned long long)(vk2_xfer_ring_bytes() >> 20));

    // =============================================================== 问 1
    Q("这次走哪条路？（`DM_GFX2` 缺省关；开关要可读且自证）");
    {
        const char *raw = getenv("DM_GFX2");
        q_route_gfx2 = vk2_gfx2_enabled();
        A("DM_GFX2 = %s ⇒ vk2_gfx2_enabled() = %d；%s", raw && *raw ? raw : "（未设）", q_route_gfx2,
          vk2_route_line());
        setenv("DM_GFX2", "1", 1);
        int on = vk2_gfx2_enabled();
        setenv("DM_GFX2", "0", 1);
        int off = vk2_gfx2_enabled();
        if (raw) setenv("DM_GFX2", raw, 1); else unsetenv("DM_GFX2");
        q_gfx2_flip = (on == 1 && off == 0);
        A("翻一下：置 1 读到 %d、置 0 读到 %d ⇒ 开关可读自证 = %d（选路点仍是 N4，本轮只到「可读」）",
          on, off, q_gfx2_flip);
    }

    // =============================================================== 问 2：脏记账自证
    Q("脏记账自己可不可信？（位图对账 + **与朴素模型逐位对比** + 顺带验「并 run」 + 扫描耗时 + 半页竞态）");
    {
        q_gran = vk2_dirty_gran_default();
        uint64_t pages = dm->pages;
        q_naive_pages = pages;
        q_bitmap_bytes = dm->words * 8 * 2;
        q_bitmap_one_bit = pool_bytes / q_gran / 8;
        A("4 GB 池：%llu 页（粒度 %u 字节）；两张 1 bit 位图 = **%llu 字节**"
          "（N0 量的是 1 bit 的 %llu 字节；三态要两位 ⇒ 翻倍）", (unsigned long long)pages,
          (unsigned)q_gran, (unsigned long long)q_bitmap_bytes, (unsigned long long)q_bitmap_one_bit);

        uint8_t *naive_cpu = calloc((size_t)pages, 1), *naive_gpu = calloc((size_t)pages, 1);
        uint8_t *seen_cpu = calloc((size_t)pages, 1), *seen_gpu = calloc((size_t)pages, 1);
        if (!naive_cpu || !naive_gpu || !seen_cpu || !seen_gpu) {
            A("【红】朴素模型分配失败（%llu 页 × 4 张）", (unsigned long long)pages);
        } else {
            rng_seed(seed);
            uint64_t touched = 0;
            for (uint64_t i = 0; i < ops; i++) {
                int op = (int)(rng_next() % 3);
                uint64_t off = rng_next() % (64 * MB);
                uint64_t len = 1 + rng_next() % (4 * MB);
                if ((rng_next() & 1) && len > 2 * PAGE) len &= ~(PAGE - 1);  // 一半页对齐、一半故意不齐
                uint64_t p0 = off / q_gran, p1 = (off + len - 1) / q_gran;
                if (p1 >= pages) p1 = pages - 1;
                for (uint64_t p = p0; p <= p1; p++) {
                    if (op == 0) naive_cpu[p] = 1;
                    else if (op == 1) naive_gpu[p] = 1;
                    else { naive_cpu[p] = 0; naive_gpu[p] = 0; }
                }
                touched += p1 - p0 + 1;
                if (op == 0) vk2_dirty_mark_cpu(dm, off, len);
                else if (op == 1) vk2_dirty_mark_gpu(dm, off, len);
                else vk2_dirty_clean(dm, off, len);
            }
            q_naive_ops = ops;
            // ① 位图逐位直接对比
            uint64_t bit_bad = 0;
            for (uint64_t p = 0; p < pages; p++) {
                uint64_t cb_bit = (dm->cpu[p >> 6] >> (p & 63)) & 1ull;
                uint64_t gb_bit = (dm->gpu[p >> 6] >> (p & 63)) & 1ull;
                if (cb_bit != (uint64_t)(naive_cpu[p] != 0) || gb_bit != (uint64_t)(naive_gpu[p] != 0)) bit_bad++;
            }
            // ② 用 scan 把 run 摊回页集合再比一次（这一步验的是「并 run」，① 只验位图本身）
            {
                struct { uint8_t *seen; int state; } two[2] = { { seen_cpu, VK2_DIRTY_CPU },
                                                                { seen_gpu, VK2_DIRTY_GPU } };
                for (int t = 0; t < 2; t++) {
                    g_seen = two[t].seen;               // 回调通过文件级指针拿目标数组
                    g_seen_pages = pages;
                    vk2_dirty_scan(dm, two[t].state, 0, pool_bytes, seen_run, NULL);
                }
            }
            uint64_t run_bad = 0;
            for (uint64_t p = 0; p < pages; p++) {
                if ((seen_cpu[p] != 0) != (naive_cpu[p] != 0)) run_bad++;
                if ((seen_gpu[p] != 0) != (naive_gpu[p] != 0)) run_bad++;
            }
            q_naive_bit_mismatch = bit_bad;
            q_naive_run_mismatch = run_bad;
            A("① 位图逐位与朴素模型对比（%llu 页、%llu 次随机记账、覆盖 %llu 页次）：%s",
              (unsigned long long)pages, (unsigned long long)ops, (unsigned long long)touched,
              bit_bad ? "【红】有出入" : "**逐位一致**");
            A("② 把 scan 出来的 run 摊回页集合再比一次（验「并 run」）：%s",
              run_bad ? "【红】有出入" : "**一致**");
            A("   此刻：CPU 脏页 %llu、GPU 脏页 %llu（两个正对照，非 0 才说明这一问真的问过）",
              (unsigned long long)vk2_dirty_count(dm, VK2_DIRTY_CPU),
              (unsigned long long)vk2_dirty_count(dm, VK2_DIRTY_GPU));
            free(naive_cpu); free(naive_gpu); free(seen_cpu); free(seen_gpu);
        }
        vk2_dirty_clean(dm, 0, pool_bytes);
    }
    // 扫描耗时三档 + 半页竞态
    {
        uint64_t bytes = 0;
        vk2_dirty_stats dst;
        vk2_dirty_scan(dm, VK2_DIRTY_CPU, 0, pool_bytes, count_run, NULL);
        vk2_dirty_stats_get(dm, &dst);
        q_scan_empty_us = dst.last_scan_us;
        A("空池扫描（4 GB、%llu 页）：%llu µs（复核 N0 的 0.9~2.7 ms）",
          (unsigned long long)dm->pages, (unsigned long long)dst.last_scan_us);
        rng_seed(seed + 1);
        uint64_t nsparse = dm->pages / 100;
        for (uint64_t i = 0; i < nsparse; i++)
            vk2_dirty_mark_cpu(dm, (rng_next() % (pool_bytes / q_gran)) * q_gran, q_gran);
        q_scan_sparse_runs = count_runs(dm, VK2_DIRTY_CPU, 0, pool_bytes, &bytes);
        vk2_dirty_stats_get(dm, &dst);
        q_scan_sparse_us = dst.last_scan_us;
        A("稀疏（%llu 个随机页 ≈ 1%%、共 %llu MB 脏）：%llu µs、**%llu 条 run**",
          (unsigned long long)nsparse, (unsigned long long)(bytes >> 20),
          (unsigned long long)dst.last_scan_us, (unsigned long long)q_scan_sparse_runs);
        vk2_dirty_clean(dm, 0, pool_bytes);
        vk2_dirty_mark_cpu(dm, 0, pool_bytes);
        q_scan_all_runs = count_runs(dm, VK2_DIRTY_CPU, 0, pool_bytes, &bytes);
        vk2_dirty_stats_get(dm, &dst);
        q_scan_all_us = dst.last_scan_us;
        A("全脏（4 GB 全标 CPU_DIRTY）：%llu µs、**%llu 条 run**（N0 的机制上界写的是 1 条 run）",
          (unsigned long long)dst.last_scan_us, (unsigned long long)q_scan_all_runs);
        vk2_dirty_clean(dm, 0, pool_bytes);
        // 半页竞态：GPU 写过整页后再做**部分** CPU 写 ⇒ 计一笔；**整页** CPU 写 ⇒ 不计
        vk2_dirty_mark_gpu(dm, 1 * MB, PAGE);
        vk2_dirty_mark_gpu(dm, 2 * MB, PAGE);
        uint64_t h0 = dm->st.hazard_halfpage;
        vk2_dirty_mark_cpu(dm, 1 * MB, 16);
        uint64_t h1 = dm->st.hazard_halfpage;
        vk2_dirty_mark_cpu(dm, 2 * MB, PAGE);
        uint64_t h2 = dm->st.hazard_halfpage;
        q_hazard_added = h1 - h0;
        A("半页竞态：GPU 写过整页 + CPU 只写 16 字节 ⇒ 计数 +%llu（应为 1）；GPU 写过整页 + CPU 写满整页 "
          "⇒ 再 +%llu（应为 0）⇒ %s", (unsigned long long)(h1 - h0), (unsigned long long)(h2 - h1),
          (q_hazard_added == 1 && h2 == h1) ? "规则照 vk2_dirty.h 写的那样" : "【红】规则不对");
        vk2_dirty_clean(dm, 0, pool_bytes);
    }

    // =============================================================== 问 3：G1 往返闭环
    Q("**G1**：往返闭环逐字节正确（CPU 写影子 → 上传 → GPU 拷一遍 → 回读 → 与**逐字节重算**的期望比对）");
    {
        struct { const char *name; uint64_t off; uint64_t len; int gpu_copy; } cs[4] = {
            { "64 KiB（小块）",                 64 * KB,          64 * KB,  1 },
            { "1 MiB",                          1 * MB,           1 * MB,   1 },
            { "16 MiB（正好等于暂存环）",        32 * MB,          16 * MB,  1 },
            { "跨块：959.5 MiB 起 1 MiB",        CB - 512 * KB,    1 * MB,   0 },
        };
        uint8_t *expect = malloc((size_t)(16 * MB + PAGE));
        uint8_t *got = malloc((size_t)(16 * MB + PAGE));
        int allok = 1, ncase = 0;
        for (size_t i = 0; i < 4; i++) {
            ncase++;
            const uint64_t off = cs[i].off, len = cs[i].len;
            const vk2_chunk *ch = &pool->chunk[off / CB];
            const uint64_t rel = off - ch->off;
            const uint64_t dst_off = cs[i].gpu_copy ? off + 48 * MB : 0;   // GPU 再拷一遍的落点
            if (!expect || !got) { A("  【红】比对缓冲分配失败"); allok = 0; break; }

            pat_fill((uint8_t *)shadow + off, len, seed + i);                 // CPU 写影子
            for (uint64_t k = 0; k < len; k++) expect[k] = pat_byte(k, seed + i);  // **独立重算**的期望

            vk2_dirty_mark_cpu(dm, off, len);
            vk2_xfer_stats x0, x1;
            vk2_xfer_stats_get(&x0);
            if (!cb_begin(&w)) { A("  【红】cb 开不了"); allok = 0; break; }
            uint64_t t0 = now_us();
            uint64_t moved = vk2_upload(w.cb, 0, 0);
            if (cs[i].gpu_copy) {
                VkBufferCopy cp;
                memset(&cp, 0, sizeof cp);
                cp.srcOffset = rel; cp.dstOffset = dst_off - ch->off; cp.size = len;
                vkCmdCopyBuffer(w.cb, ch->buf, ch->buf, 1, &cp);
                probe_barrier(w.cb, ch->buf, cp.dstOffset, len, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT);
            }
            if (!cb_end_submit(&v, &w, "G1 窗口")) { allok = 0; break; }
            uint64_t t1 = now_us();
            int wok = wait_window(&v, &w, "G1 窗口");
            uint64_t t2 = now_us();
            vk2_xfer_window_done();
            vk2_xfer_stats_get(&x1);
            if (!wok) { allok = 0; break; }

            const uint64_t rb_off = cs[i].gpu_copy ? dst_off : off;
            const vk2_chunk *rbch = &pool->chunk[rb_off / CB];
            vk2_readback_after(w.fence);
            uint64_t t3 = now_us();
            int ok = vk2_readback(rbch->buf, rb_off - rbch->off, got, (size_t)len);
            uint64_t t4 = now_us();
            uint64_t bad = (uint64_t)-1;
            if (ok) {
                for (uint64_t k = 0; k < len; k++) if (got[k] != expect[k]) { bad = k; break; }
            }
            int clean_ok = !vk2_dirty_any(dm, off, len, VK2_DIRTY_CPU);
            int case_ok = (ok && bad == (uint64_t)-1 && clean_ok && moved == len);
            if (!case_ok) allok = 0;
            A("  %-24s 长度 %8llu B ⇒ 搬 %llu B（run %llu / 拷贝 %llu / 跨块多出 %llu）、屏障 %llu；"
              "录制 %llu µs、提交+等栅栏 %llu µs、回读 %llu µs；回读值 %s；搬完置 CLEAN = %d",
              cs[i].name, (unsigned long long)len, (unsigned long long)moved,
              (unsigned long long)(x1.runs - x0.runs), (unsigned long long)(x1.copies - x0.copies),
              (unsigned long long)(x1.split_runs - x0.split_runs), (unsigned long long)(x1.barriers - x0.barriers),
              (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1), (unsigned long long)(t4 - t3),
              bad == (uint64_t)-1 ? "**逐字节等于独立重算的期望**" : "【红】与期望不符", clean_ok);
            if (ok && bad != (uint64_t)-1)
                A("      第一处不符在偏移 %llu（期望 %02x，实得 %02x）", (unsigned long long)bad,
                  expect[bad], got[bad]);
        }
        q_g1_cases = ncase;
        q_g1_ok = (allok && ncase == 4) ? 1 : 0;
        A("⇒ **G1 %s**（%d/4 档；每档都是「影子→暂存环→后备→（GPU 拷一遍）→回读→与重算期望比对」）",
          q_g1_ok ? "成立" : "【红】不成立", (int)q_g1_cases);
        free(expect); free(got);
    }

    // =============================================================== 问 4：G2 可见性
    Q("**G2**：可见性是「显式」的吗？—— 不等待就读 / 等完栅栏还不回读 / 回读之后，三者各读到什么");
    {
        const uint64_t len = 64 * KB;
        const uint64_t off = 4 * MB;
        const vk2_chunk *ch0 = &pool->chunk[0];
        uint8_t *old = malloc((size_t)len), *got = malloc((size_t)len);
        if (!old || !got) {
            A("【红】缓冲分配失败");
        } else {
            // 第一步：把「旧值」搬上 GPU（这样「新值」才是真的变化，而不是「那边本来什么都没有」）
            pat_fill((uint8_t *)shadow + off, len, seed + 100);
            memcpy(old, (uint8_t *)shadow + off, (size_t)len);
            vk2_dirty_mark_cpu(dm, off, len);
            cb_begin(&w);
            uint64_t moved = vk2_upload(w.cb, 0, 0);
            cb_end_submit(&v, &w, "G2 旧值上传");
            wait_window(&v, &w, "G2 旧值上传");
            vk2_xfer_window_done();
            A("先把「旧值」（%llu B 的确定图案）搬上 GPU：搬了 %llu B", (unsigned long long)len,
              (unsigned long long)moved);

            // 第二步：GPU 把这一段写成另一个图案（真 GPU 写），提交后**故意不等待**
            const uint32_t fillpat = 0xdeadbe00u;
            cb_begin(&w);
            vkCmdFillBuffer(w.cb, ch0->buf, off, len, fillpat);
            probe_barrier(w.cb, ch0->buf, off, len, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT);
            cb_end_submit(&v, &w, "G2 GPU 写");
            uint64_t same_old = 0, same_new = 0;
            for (uint64_t k = 0; k < len; k++) {
                uint8_t nb = (uint8_t)((fillpat >> ((k & 3) * 8)) & 0xff);
                if (((uint8_t *)shadow)[off + k] == old[k]) same_old++;
                if (((uint8_t *)shadow)[off + k] == nb) same_new++;
            }
            q_g2_old_nowait = (same_old == len);
            A("(a) **提交后不等待**就读影子：%llu / %llu 字节仍是**旧值**（等于 GPU 新值的只有 %llu 字节）⇒ %s",
              (unsigned long long)same_old, (unsigned long long)len, (unsigned long long)same_new,
              q_g2_old_nowait ? "读到旧值 ⇒「写完即可见」在 Vulkan 上确实不存在" : "【红】不是旧值");

            // 第三步：**等栅栏**但仍然不回读 —— 影子依旧应该是旧值（可见性不是「等」出来的）
            int fr = wait_window(&v, &w, "G2 GPU 写");
            uint64_t same_old2 = 0;
            for (uint64_t k = 0; k < len; k++) if (((uint8_t *)shadow)[off + k] == old[k]) same_old2++;
            q_g2_old_after_fence = (fr && same_old2 == len);
            A("(b) **等栅栏成功**（%d）但**仍不回读**：%llu / %llu 字节还是旧值 ⇒ %s", fr,
              (unsigned long long)same_old2, (unsigned long long)len,
              q_g2_old_after_fence ? "栅栏只说明 GPU 干完了，不改 CPU 那份副本" : "【红】栅栏之外还有别的东西在改影子");

            // 第四步：回读（内部等生产者栅栏）⇒ 影子变成新值
            vk2_readback_after(w.fence);
            uint64_t t0 = now_us();
            int ok = vk2_readback(ch0->buf, off, got, (size_t)len);
            uint64_t dt = now_us() - t0;
            uint64_t same_new2 = 0;
            if (ok) {
                memcpy((uint8_t *)shadow + off, got, (size_t)len);   // 探针自己更新影子（N4 里由回读窗口负责）
                for (uint64_t k = 0; k < len; k++) {
                    uint8_t nb = (uint8_t)((fillpat >> ((k & 3) * 8)) & 0xff);
                    if (got[k] == nb) same_new2++;
                }
            }
            q_g2_new_after_rb = (ok && same_new2 == len);
            A("(c) `vk2_readback`（内部等一次定向栅栏，%llu µs）：%llu / %llu 字节等于 GPU 写的新图案 ⇒ %s",
              (unsigned long long)dt, (unsigned long long)same_new2, (unsigned long long)len,
              q_g2_new_after_rb ? "读到新值 ⇒ 可见性是**回读**这一步建立起来的" : "【红】没读到新值");

            // (d) 没指定要等谁 ⇒ 拒绝（不静默读旧值）
            memset(got, 0x5a, 32);
            vk2_readback_after(VK_NULL_HANDLE);
            int r0 = vk2_readback(ch0->buf, off, got, 32);
            int untouched = 1;
            for (int k = 0; k < 32; k++) if (got[k] != 0x5a) untouched = 0;
            q_g2_nofence_refused = (r0 == 0 && untouched);
            A("(d) 没指定要等谁就回读 ⇒ 返回 %d、目标缓冲%s ⇒ %s",
              r0, untouched ? "原样未动" : "【红】被写了",
              q_g2_nofence_refused ? "拒绝（不许「不知道等谁也照读」）" : "【红】没拒绝");

            // (e) 等一个**永远不会 signal** 的栅栏 ⇒ 如实报超时失败（N0 量过"超时后可恢复"，这里量"报不报"）
            VkFenceCreateInfo fci;
            memset(&fci, 0, sizeof fci);
            fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;   // 不带 SIGNALED 位 ⇒ 没人 signal 它
            VkFence never = VK_NULL_HANDLE;
            vkCreateFence(v.dev, &fci, NULL, &never);
            vk2_xfer_stats xs0, xs1;
            vk2_xfer_stats_get(&xs0);
            vk2_readback_after(never);
            uint64_t t1 = now_us();
            int r1 = vk2_readback(ch0->buf, off, got, 32);
            uint64_t dt1 = now_us() - t1;
            vk2_xfer_stats_get(&xs1);
            q_g2_timeout_failed = (r1 == 0 && xs1.fence_timeouts > xs0.fence_timeouts);
            A("(e) 等一个永远不 signal 的栅栏：回读返回 %d、耗时 %llu ms、超时计数 +%llu ⇒ %s", r1,
              (unsigned long long)(dt1 / 1000), (unsigned long long)(xs1.fence_timeouts - xs0.fence_timeouts),
              q_g2_timeout_failed ? "如实报失败（没有「拿旧值当成功」这回事）" : "【红】没报");
            if (never) vkDestroyFence(v.dev, never, NULL);
            vk2_readback_after(VK_NULL_HANDLE);
            free(old); free(got);
        }
    }

    // =============================================================== 问 5：机制读数（不是帧读数）
    Q("机制读数（**明确不是帧读数**）：一张贴图的 run 数（单段 vs 拆 64 处）、真搬一批的窗口数与吞吐");
    {
        const uint64_t tex = 1600ull * 900ull * 4ull;   // RGBA8 的一张贴图 = 5760000 B
        const uint64_t tex_off = 128 * MB;
        uint64_t bytes = 0;

        vk2_dirty_clean(dm, 0, pool_bytes);
        vk2_dirty_mark_cpu(dm, tex_off, tex);
        q_tex_runs = count_runs(dm, VK2_DIRTY_CPU, 0, pool_bytes, &bytes);
        q_tex_bytes = bytes;
        uint64_t t0 = now_us();
        cb_begin(&w);
        uint64_t moved = vk2_upload(w.cb, 0, 0);
        if (moved) { cb_end_submit(&v, &w, "贴图单段上传"); wait_window(&v, &w, "贴图单段上传"); }
        vk2_xfer_window_done();
        q_tex_us = now_us() - t0;
        A("  单段脏：1600×900 RGBA8（%llu B）⇒ **%llu 条 run**、一次窗口搬 %llu B、耗时 %llu µs",
          (unsigned long long)tex, (unsigned long long)q_tex_runs, (unsigned long long)moved,
          (unsigned long long)q_tex_us);

        vk2_dirty_clean(dm, 0, pool_bytes);
        for (uint64_t k = 0; k < 64; k++) vk2_dirty_mark_cpu(dm, tex_off + k * (tex / 64), PAGE);
        q_tile_runs = count_runs(dm, VK2_DIRTY_CPU, 0, pool_bytes, &bytes);
        q_tile_bytes = bytes;
        uint64_t t1 = now_us();
        cb_begin(&w);
        uint64_t moved2 = vk2_upload(w.cb, 0, 0);
        if (moved2) { cb_end_submit(&v, &w, "贴图分段上传"); wait_window(&v, &w, "贴图分段上传"); }
        vk2_xfer_window_done();
        q_tile_us = now_us() - t1;
        A("  分段脏：同一张贴图拆成 64 处 4 KiB（间隔 %llu B）⇒ **%llu 条 run**、搬 %llu B、耗时 %llu µs",
          (unsigned long long)(tex / 64), (unsigned long long)q_tile_runs, (unsigned long long)moved2,
          (unsigned long long)q_tile_us);

        // 真搬一批（窗口一个一个来，直到搬完）：给出**实测**吞吐 —— 不是照 N0 的机制上界折算
        vk2_dirty_clean(dm, 0, pool_bytes);
        const uint64_t big = move_mb * MB;
        vk2_dirty_mark_cpu(dm, 1 * MB, big);
        vk2_xfer_stats xa, xb;
        vk2_xfer_stats_get(&xa);
        uint64_t t2 = now_us();
        uint64_t total = 0, wins = 0, guard = 0;
        while (guard++ < 8192) {
            cb_begin(&w);
            uint64_t mv = vk2_upload(w.cb, 0, 0);
            if (!mv) { vkEndCommandBuffer(w.cb); break; }
            if (!cb_end_submit(&v, &w, "大范围上传")) break;
            if (!wait_window(&v, &w, "大范围上传")) break;
            vk2_xfer_window_done();
            total += mv; wins++;
        }
        q_move_us = now_us() - t2;
        q_move_bytes = total;
        q_move_windows = wins;
        vk2_xfer_stats_get(&xb);
        q_move_copies = xb.copies - xa.copies;
        q_move_split = xb.split_runs - xa.split_runs;
        q_move_ring_full = xb.ring_full - xa.ring_full;
        A("  真搬：把 %llu MB 全标脏再搬上去 ⇒ **%llu 个窗口**、共 %llu MB、总耗时 %llu ms ⇒ **%llu MB/ms**"
          "（窗口 = 暂存环 %llu MB；这一条是实测吞吐，不是折算）",
          (unsigned long long)move_mb, (unsigned long long)wins, (unsigned long long)(total >> 20),
          (unsigned long long)(q_move_us / 1000),
          (unsigned long long)(q_move_us ? (total >> 20) * 1000 / q_move_us : 0),
          (unsigned long long)(vk2_xfer_ring_bytes() >> 20));
        vk2_dirty_clean(dm, 0, pool_bytes);
    }
    {
        vk2_xfer_stats xs;
        vk2_xfer_stats_get(&xs);
        q_idle_calls = xs.idle_calls;
        q_barriers = xs.barriers;
        A("  搬运窗口的累计账：窗口 %llu、run %llu、`vkCmdCopyBuffer` %llu 条、屏障 %llu 条、"
          "跨块多出的拷贝 %llu、环装不下 %llu 次；**`vkQueueWaitIdle` 计数 = %llu（G4 的前置，必须 0）**",
          (unsigned long long)xs.windows, (unsigned long long)xs.runs, (unsigned long long)xs.copies,
          (unsigned long long)xs.barriers, (unsigned long long)xs.split_runs,
          (unsigned long long)xs.ring_full, (unsigned long long)xs.idle_calls);
        A("  ⇒ 机制对照：全脏 4 GB = **%llu 条 run**（上传时要拆 %d 条块拷贝）｜一张贴图单段 = %llu 条 run｜"
          "同一张拆 64 处 = %llu 条 run。**这三条都不是帧读数**：六个新基线字段留空到 N4（接管回放层）。",
          (unsigned long long)q_scan_all_runs, pool->nchunk, (unsigned long long)q_tex_runs,
          (unsigned long long)q_tile_runs);
    }

    // =============================================================== 问 6：键值
    {
        // 先把账快照下来（池一删，池侧的统计就没了），再做收尾，最后打印 —— 这样"结束时的 MemAvailable"
        // 才是**放手之后**的值（否则读到的是峰值）。
        // Snapshot the statistics first (deleting the pool drops them), then tear down, then print, so that the
        // "end" memory reading is the value *after* everything is released.
        vk2_xfer_stats xs;
        vk2_xfer_stats_get(&xs);
        vk2_dirty_stats ds;
        vk2_dirty_stats_get(dm, &ds);
        unsigned long long mem_peak = mem_avail_kb();
        window_free(&v, &w);
        vk2_pool_del(shadow);
        vk2_xfer_fini();
        vk2_pool_fini();
        munmap(shadow, (size_t)pool_bytes);
        unsigned long long mem_end = mem_avail_kb();

        printf("\n--- VK2_XFER_SUMMARY（结论键值，供文档抄录）---\n");
        printf("VK2_ROUTE=%s\n", q_route_gfx2 ? "new" : "old");
        printf("VK2_GFX2_SWITCH_SELFPROOF=%d\n", q_gfx2_flip);
        printf("VK2_DIRTY_GRAN=%llu\n", (unsigned long long)q_gran);
        printf("VK2_DIRTY_BITMAP_BYTES=%llu\n", (unsigned long long)q_bitmap_bytes);
        printf("VK2_DIRTY_BITMAP_ONE_BIT=%llu\n", (unsigned long long)q_bitmap_one_bit);
        printf("VK2_DIRTY_NAIVE_PAGES=%llu\n", (unsigned long long)q_naive_pages);
        printf("VK2_DIRTY_NAIVE_OPS=%llu\n", (unsigned long long)q_naive_ops);
        printf("VK2_DIRTY_NAIVE_BIT_MISMATCH=%llu\n", (unsigned long long)q_naive_bit_mismatch);
        printf("VK2_DIRTY_NAIVE_RUN_MISMATCH=%llu\n", (unsigned long long)q_naive_run_mismatch);
        printf("VK2_SCAN_EMPTY_US=%llu\n", (unsigned long long)q_scan_empty_us);
        printf("VK2_SCAN_SPARSE_US=%llu\n", (unsigned long long)q_scan_sparse_us);
        printf("VK2_SCAN_SPARSE_RUNS=%llu\n", (unsigned long long)q_scan_sparse_runs);
        printf("VK2_SCAN_ALL_US=%llu\n", (unsigned long long)q_scan_all_us);
        printf("VK2_SCAN_ALL_RUNS=%llu\n", (unsigned long long)q_scan_all_runs);
        printf("VK2_DIRTY_HAZARD_ADDED=%llu\n", (unsigned long long)q_hazard_added);
        printf("VK2_G1_OK=%d\n", q_g1_ok);
        printf("VK2_G1_CASES=%d\n", q_g1_cases);
        printf("VK2_G2_OLD_NO_WAIT=%d\n", q_g2_old_nowait);
        printf("VK2_G2_OLD_AFTER_FENCE=%d\n", q_g2_old_after_fence);
        printf("VK2_G2_NEW_AFTER_RB=%d\n", q_g2_new_after_rb);
        printf("VK2_G2_NOFENCE_REFUSED=%d\n", q_g2_nofence_refused);
        printf("VK2_G2_TIMEOUT_FAILED=%d\n", q_g2_timeout_failed);
        printf("VK2_MECH_TEX_RUNS=%llu\n", (unsigned long long)q_tex_runs);
        printf("VK2_MECH_TEX_BYTES=%llu\n", (unsigned long long)q_tex_bytes);
        printf("VK2_MECH_TEX_US=%llu\n", (unsigned long long)q_tex_us);
        printf("VK2_MECH_TILE64_RUNS=%llu\n", (unsigned long long)q_tile_runs);
        printf("VK2_MECH_TILE64_BYTES=%llu\n", (unsigned long long)q_tile_bytes);
        printf("VK2_MECH_TILE64_US=%llu\n", (unsigned long long)q_tile_us);
        printf("VK2_MECH_MOVE_MB=%llu\n", (unsigned long long)(q_move_bytes >> 20));
        printf("VK2_MECH_MOVE_WINDOWS=%llu\n", (unsigned long long)q_move_windows);
        printf("VK2_MECH_MOVE_US=%llu\n", (unsigned long long)q_move_us);
        printf("VK2_MECH_MOVE_COPIES=%llu\n", (unsigned long long)q_move_copies);
        printf("VK2_MECH_MOVE_SPLIT=%llu\n", (unsigned long long)q_move_split);
        printf("VK2_RING_MB=%llu\n", (unsigned long long)(vk2_xfer_ring_bytes() >> 20));
        printf("VK2_XFER_WINDOWS=%llu\n", (unsigned long long)xs.windows);
        printf("VK2_XFER_RUNS=%llu\n", (unsigned long long)xs.runs);
        printf("VK2_XFER_COPIES=%llu\n", (unsigned long long)xs.copies);
        printf("VK2_XFER_BARRIERS=%llu\n", (unsigned long long)q_barriers);
        printf("VK2_XFER_RB_CALLS=%llu\n", (unsigned long long)xs.rb_calls);
        printf("VK2_XFER_RB_BYTES=%llu\n", (unsigned long long)xs.rb_bytes);
        printf("VK2_XFER_RB_NO_FENCE=%llu\n", (unsigned long long)xs.rb_no_fence);
        printf("VK2_XFER_FENCE_TIMEOUTS=%llu\n", (unsigned long long)xs.fence_timeouts);
        printf("VK2_XFER_IDLE_CALLS=%llu\n", (unsigned long long)xs.idle_calls);
        printf("VK2_XFER_RING_FULL=%llu\n", (unsigned long long)q_move_ring_full);
        printf("VK2_DIRTY_HAZARD_TOTAL=%llu\n", (unsigned long long)ds.hazard_halfpage);
        printf("VK2_MEM_AVAIL_START_MB=%llu\n", (unsigned long long)(mem_start >> 10));
        printf("VK2_MEM_AVAIL_PEAK_MB=%llu\n", (unsigned long long)(mem_peak >> 10));
        printf("VK2_MEM_AVAIL_END_MB=%llu\n", (unsigned long long)(mem_end >> 10));
        fflush(stdout);
    }

    vk_close(&v);
    printf("\n完成（N2：让脏记账与搬运窗口可用，没有接管回放层）\n");
    return 0;
}
