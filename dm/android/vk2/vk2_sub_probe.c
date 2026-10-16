// 新地基 N3（帧批提交 + 有界在飞）的**自证探针** —— 把判据 **G4** 拆成可判定的几问一答。
//
// G4 的原文（docs/rewrite/ANDROID_REWRITE.zh-CN.md §六）：
//   「一个客体提交 = 一次 `vkQueueSubmit`；`vkQueueWaitIdle` 出现次数为 0；每帧提交次数与长等条数落在新基线内。」
// 后面的"每帧"要等 N4 真帧，本轮能判定的是**前两条**加上"有界在飞"的形状。于是本探针的六问：
//   问 1 这次走哪条路（`DM_GFX2` 可读自证，缺省关）
//   问 2 **G4（一）**：一个「客体提交」= 一次 `vkQueueSubmit`（记账的 `submits` 必须等于调用 `vk2_sub_commit()` 的次数）
//        + `vkQueueWaitIdle` 计数恒 0（模块里那个"宏成计数 + 报红"的自检，探针自己也从不调用它）
//   问 3 **G4（二）有界在飞**：D = 1 / 2 / 3 / 4 / 8 各跑一遍**故意让 GPU 忙起来**的连续提交，
//        报"槽等待率（要等几次 / 总共取槽几次）"、每次等待时长、以及"长等条数（> 1 ms）"
//        ⇒ 这就是 **3 还是 4** 的依据（真帧的取舍要 N4 复核）
//   问 4 `vk2_sub_wait_all()` 在有空槽在飞时的行为：**等所有在飞，不是等队列空闲**（删掉等待的对照）
//   问 5 **提交槽能被 N2 的搬运窗口复用**：上传窗口录进槽的 cb、栅栏到时环才交还（逐字节验真）；
//        并量出**环是单窗口的**这项事实（在飞 3 个槽时只有第一个能录上传窗口）⇒ N4 要按槽分环
//   问 6 结论键值（`--- VK2_SUB_SUMMARY ---`）
//
// 跑法（一律走项目内 `.venv` Python）：
//   & "<仓库>/.venv/Scripts/python.exe" -X utf8 .work/probes.py run vk2_sub_probe --serial 8b1f1e50 [--tag r1]
// 多轮：`.work/vk2_pool_rounds.py --probe vk2_sub_probe --rounds 4`
// 读数落 `.work/logs/probe-<串号>-vk2_sub_probe[-tag].txt`。
//
// 开关：DM_VK2_POOL_MB（缺省 256）、DM_VK2_RING_MB（缺省 16）、DM_VK2_SUBS（每档提交几次，缺省 24）、
//       DM_VK2_BUSY_MB（每次提交录多少 GPU 拷贝，缺省 128）、DM_VK2_SEED（缺省 20261016）
//
// ---------------------------------------------------------------------------------------------------------------
// The self-proving probe of N3: gate G4 as a question-and-answer run. One guest submission is one vkQueueSubmit,
// vkQueueWaitIdle never happens, and bounded in-flight work (D = 1/2/3/4/8) is measured with the GPU deliberately
// kept busy, so the slot wait rate and the long-wait count decide 3 versus 4. A further question proves that a
// submission slot can carry an N2 upload window (byte-exact) and measures the fact that the staging ring is
// single-window, which is where N4 has to split it per slot.
#define _GNU_SOURCE 1
#include "vk2_sub.h"

#include "vk2_dirty.h"
#include "vk2_pool.h"
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
static void slog(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("[槽] "); vprintf(fmt, ap); printf("\n");
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
// 逐字节重算的期望：判据不许拿"影子自己"当对照 | the expectation is recomputed per byte, never copied
static uint8_t pat_byte(uint64_t i, uint64_t seed) {
    return (uint8_t)(((i * 1103515245ull + seed * 12345ull + (i >> 7)) >> 13) ^ (i * 31ull));
}
static void pat_fill(uint8_t *p, uint64_t n, uint64_t seed) {
    for (uint64_t i = 0; i < n; i++) p[i] = pat_byte(i, seed);
}

// ---------------------------------------------------------------- 设备（与 N1/N2 的探针同一套）
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
    ai.pApplicationName = "dm-vk2-sub-probe";
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

// 每次提交录的"GPU 活"：块内一次大拷贝（**故意让 GPU 忙起来**，才量得到槽等待率）。
// 槽满时等的是"最老那个槽的栅栏"，所以没有这股活的话三种深度看不出差别。
// The busy work recorded into every submission: one large device-local copy, without which the three depths
// cannot be told apart (a full set waits on the oldest fence, which is instant if the queue drains instantly).
static void record_busy(VkCommandBuffer cb, VkBuffer buf, uint64_t busy_bytes, uint64_t chunk_bytes) {
    VkBufferCopy cp;
    memset(&cp, 0, sizeof cp);
    cp.srcOffset = 0;
    cp.dstOffset = chunk_bytes - busy_bytes;      // 与源区间不重叠 | disjoint from the source range
    cp.size = busy_bytes;
    vkCmdCopyBuffer(cb, buf, buf, 1, &cp);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("=== vk2_sub_probe：新地基 N3（帧批提交 + 有界在飞 + `vkQueueWaitIdle` 归零）的自证探针 ===\n");
    printf("构建：%s %s；pid=%d；页大小=%ld 字节\n", __DATE__, __TIME__, (int)getpid(), sysconf(_SC_PAGESIZE));
    vk2_sub_set_log(slog);
    vk2_xfer_set_log(xlog);
    vk2_dirty_set_log(dlog);

    uint64_t pool_mb = env_u64("DM_VK2_POOL_MB", 256);
    uint64_t ring_mb = env_u64("DM_VK2_RING_MB", 16);
    uint64_t subs    = env_u64("DM_VK2_SUBS", 24);
    uint64_t busy_mb = env_u64("DM_VK2_BUSY_MB", 128);
    uint64_t seed    = env_u64("DM_VK2_SEED", 20261016);

    probe_dev v;
    if (!vk_open(&v)) { fprintf(stderr, "【红】Vulkan 打不开\n"); return 2; }
    printf("设备 %s；Vulkan %u.%u.%u\n", v.props.deviceName, VK_VERSION_MAJOR(v.props.apiVersion),
           VK_VERSION_MINOR(v.props.apiVersion), VK_VERSION_PATCH(v.props.apiVersion));
    unsigned long long mem_start = mem_avail_kb();
    printf("MemAvailable 起始 %llu MB\n", (unsigned long long)(mem_start >> 10));

    // ---- 结果键 ----
    int q_route_gfx2 = -1, q_gfx2_flip = 0;
    int q_g4_per_commit = 0, q_g4_idle_zero = 0;
    uint64_t q_g4_my_commits = 0, q_g4_commits = 0, q_g4_submits = 0, q_g4_reclaims = 0;
    uint64_t q_g4_submit_fail = 0, q_g4_empty = 0, q_g4_empty_us = 0;
    const int DS[5] = { 1, 2, 3, 4, 8 };
    uint64_t q_d_begins[5] = {0}, q_d_waits[5] = {0}, q_d_polls[5] = {0}, q_d_long[5] = {0};
    uint64_t q_d_begin_long[5] = {0}, q_d_wait_max[5] = {0}, q_d_avg_us[5] = {0};
    uint64_t q_d_wait_us[5] = {0}, q_d_begin_max[5] = {0}, q_d_reclaims[5] = {0};
    uint64_t q_d_busy_mb[5] = {0}, q_d_first_begin_us[5] = {0}, q_d_warmup_us[5] = {0};
    int q_inflight_chosen = 0, q_d4_buys_little = 0;
    int64_t q_d34_gain_pm = 0;
    uint64_t q_long_total = 0, q_long_max_us = 0;
    int q_waitall_ok = 0;
    uint64_t q_waitall_pending = 0, q_waitall_waited = 0, q_waitall_us = 0, q_waitall_idle_delta = 0;
    int q_slot_up_ok = 0;
    uint64_t q_slot_up_bytes = 0, q_slot_up_bad = 0, q_slot_up_recycled = 0, q_slot_up_ring_free = 0;
    uint64_t q_slot_up_us = 0, q_ring_contended = 0, q_ring_uploaded = 0, q_ring_recycled = 0;
    uint64_t q_idle_total = 0, q_begins_total = 0, q_commits_total = 0, q_submits_total = 0, q_reclaims_total = 0;
    uint64_t q_no_slot_total = 0, q_timeout_total = 0;

    // ---- 池 + 两个窗口 + 提交槽 ----
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
    if (!vk2_pool_add(shadow, pool_bytes, "probe-sub")) { fprintf(stderr, "【红】池登记失败\n"); return 2; }
    const vk2_pool *pool = vk2_pool_at(0);
    vk2_dirty *dm = vk2_pool_dirty(0);
    if (!pool || !dm) { fprintf(stderr, "【红】池/脏位图拿不到\n"); return 2; }
    const vk2_chunk *ch0 = &pool->chunk[0];
    // GPU 活的大小：夹到"块内一半"以内（源与目标各占一段，不重叠）| clamp the busy copy to half a chunk
    uint64_t busy_bytes = busy_mb * MB;
    if (busy_bytes > ch0->size / 2) busy_bytes = (ch0->size / 2) & ~63ull;
    A("池 %p+%llu MB 就绪：%d 块、首块 %llu MB、暂存环 %llu MB；每次提交录 %llu MB 的 GPU 拷贝",
      shadow, (unsigned long long)pool_mb, pool->nchunk, (unsigned long long)(ch0->size >> 20),
      (unsigned long long)(vk2_xfer_ring_bytes() >> 20), (unsigned long long)(busy_bytes >> 20));
    if (!vk2_sub_init(&d, 3)) { fprintf(stderr, "【红】vk2_sub_init 失败\n"); return 2; }
    A("提交槽就绪：在飞深度 %d（缺省 = VK2_INFLIGHT_DEFAULT = %d；本轮要做到的正是把它定下来）",
      vk2_sub_inflight(), VK2_INFLIGHT_DEFAULT);

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

    // =============================================================== 问 2：G4（一）
    Q("**G4（一）**：一个「客体提交」= 一次 `vkQueueSubmit`？`vkQueueWaitIdle` 计数是不是 0？");
    {
        vk2_sub_stats s0, s1;
        vk2_sub_stats_get(&s0);
        uint64_t my_commits = 0, my_begins = 0;
        uint64_t t0 = now_us();
        for (uint64_t i = 0; i < subs; i++) {
            vk2_slot *s = vk2_sub_begin();      // 空 cb：这一问只问"提交账目"，不录命令
            if (!s) break;
            my_begins++;
            if (vk2_sub_commit()) my_commits++;
        }
        uint64_t wall = now_us() - t0;
        int wok = vk2_sub_wait_all();
        vk2_sub_stats_get(&s1);
        q_g4_my_commits = my_commits;
        q_g4_commits = s1.commits - s0.commits;
        q_g4_submits = s1.submits - s0.submits;
        q_g4_reclaims = s1.reclaims - s0.reclaims;
        q_g4_submit_fail = s1.submit_fail - s0.submit_fail;
        q_g4_empty = my_commits;
        q_g4_empty_us = wall;
        q_g4_per_commit = (q_g4_commits == my_commits && q_g4_submits == my_commits &&
                           q_g4_commits == q_g4_submits && q_g4_submit_fail == 0 && my_begins == subs);
        q_g4_idle_zero = (vk2_sub_idle_calls() == 0 && s1.idle_calls == 0);
        A("连续提交 %llu 次**空** cb：探针自己数到的 commit 次数 = %llu（取槽 %llu 次）；模块记账里 "
          "commits = %llu、submits = %llu、submit_fail = %llu",
          (unsigned long long)subs, (unsigned long long)my_commits, (unsigned long long)my_begins,
          (unsigned long long)q_g4_commits, (unsigned long long)q_g4_submits,
          (unsigned long long)q_g4_submit_fail);
        A("⇒ **一次客体提交 = 一次 `vkQueueSubmit`：%s**（判据 = `commits == submits == 探针数的 commit 次数`）",
          q_g4_per_commit ? "成立" : "【红】不成立");
        A("提交完 `vk2_sub_wait_all()`：%d（回收了 %llu 个槽、当前在飞 %d）；%llu 次提交共 %llu µs",
          wok, (unsigned long long)q_g4_reclaims, vk2_sub_pending(),
          (unsigned long long)subs, (unsigned long long)wall);
        A("**`vkQueueWaitIdle` 计数 = %llu**（模块里被宏成「计数 + 报红」；探针自己也从不调用它）⇒ %s",
          (unsigned long long)vk2_sub_idle_calls(), q_g4_idle_zero ? "恒 0 成立" : "【红】不是 0");
        q_idle_total += s1.idle_calls - s0.idle_calls;
        q_no_slot_total += s1.no_slot_fail - s0.no_slot_fail;
        q_timeout_total += s1.timeout_fails - s0.timeout_fails;
        q_begins_total += s1.begins - s0.begins;
        q_commits_total += s1.commits - s0.commits;
        q_submits_total += s1.submits - s0.submits;
        q_reclaims_total += s1.reclaims - s0.reclaims;
    }

    // =============================================================== 问 3：有界在飞（D 的取舍）
    Q("**G4（二）有界在飞**：D = 1 / 2 / 3 / 4 / 8 各提交 %llu 次（每次录 %llu MB 的 GPU 拷贝），"
      "槽等待率与长等条数是多少？", (unsigned long long)subs, (unsigned long long)(busy_bytes >> 20));
    {
        for (int k = 0; k < 5; k++) {
            int D = DS[k];
            if (!vk2_sub_init(&d, D)) { A("【红】D = %d 的槽建不出来", D); break; }
            // 预热：先提交一次并等回来，**不计入读数**（N2 的坑⑧：首次提交 5.9~7.2 ms）
            // Warm-up: one submission, waited on, kept out of the readings (N2's pitfall 8).
            {
                uint64_t w0 = now_us();
                vk2_slot *s = vk2_sub_begin();
                if (!s) { A("【红】D = %d 预热取不到槽", D); break; }
                record_busy(s->cb, ch0->buf, busy_bytes, ch0->size);
                vk2_sub_commit();
                vk2_sub_wait_all();
                q_d_warmup_us[k] = now_us() - w0;
            }
            vk2_sub_stats s0, s1;
            vk2_sub_stats_get(&s0);
            uint64_t begin_long = 0, begin_max = 0, begin_sum = 0, first_begin = 0;
            uint64_t t0 = now_us();
            for (uint64_t i = 0; i < subs; i++) {
                uint64_t b0 = now_us();
                vk2_slot *s = vk2_sub_begin();
                uint64_t b1 = now_us();
                if (!s) break;
                uint64_t bu = b1 - b0;
                begin_sum += bu;
                if (bu > begin_max) begin_max = bu;
                if (bu > VK2_SUB_LONG_WAIT_US) begin_long++;
                if (i == 0) first_begin = bu;
                record_busy(s->cb, ch0->buf, busy_bytes, ch0->size);
                vk2_sub_commit();
            }
            int wok = vk2_sub_wait_all();
            uint64_t total = now_us() - t0;
            vk2_sub_stats_get(&s1);
            q_d_begins[k] = s1.begins - s0.begins;
            q_d_waits[k] = s1.waits - s0.waits;
            q_d_polls[k] = s1.poll_reaps - s0.poll_reaps;
            q_d_long[k] = s1.long_waits - s0.long_waits;
            q_d_begin_long[k] = begin_long;
            q_d_wait_max[k] = s1.wait_max_us - s0.wait_max_us;
            q_d_wait_us[k] = s1.wait_us - s0.wait_us;
            q_d_begin_max[k] = begin_max;
            q_d_first_begin_us[k] = first_begin;
            q_d_reclaims[k] = s1.reclaims - s0.reclaims;
            q_d_avg_us[k] = q_d_begins[k] ? total / q_d_begins[k] : 0;
            q_d_busy_mb[k] = (busy_bytes >> 20) * q_d_begins[k];
            A("D = %d：取槽 %llu 次 ⇒ **等栅栏 %llu 次（槽等待率 %llu‰）**、轮询就发现完成 %llu 次；"
              "等待累计 %llu µs、最长 %llu µs、**长等（> %llu µs）%llu 条**（探针自己数的「取槽耗时 > 1 ms」= %llu 条、"
              "取槽最长 %llu µs、首次取槽 %llu µs）",
              D, (unsigned long long)q_d_begins[k], (unsigned long long)q_d_waits[k],
              (unsigned long long)(q_d_begins[k] ? q_d_waits[k] * 1000 / q_d_begins[k] : 0),
              (unsigned long long)q_d_polls[k], (unsigned long long)q_d_wait_us[k],
              (unsigned long long)q_d_wait_max[k], (unsigned long long)VK2_SUB_LONG_WAIT_US,
              (unsigned long long)q_d_long[k], (unsigned long long)begin_long,
              (unsigned long long)begin_max, (unsigned long long)first_begin);
            A("     全档合计：%llu 次提交、%llu µs ⇒ **每次提交 %llu µs**（含录制、提交、槽等待与最后的 wait_all）；"
              "%llu MB 的 GPU 拷贝是发出去的活（预热 %llu µs、等回全部 = %d）",
              (unsigned long long)q_d_begins[k], (unsigned long long)total,
              (unsigned long long)q_d_avg_us[k], (unsigned long long)q_d_busy_mb[k],
              (unsigned long long)q_d_warmup_us[k], wok);
            q_idle_total += s1.idle_calls - s0.idle_calls;
            q_no_slot_total += s1.no_slot_fail - s0.no_slot_fail;
            q_timeout_total += s1.timeout_fails - s0.timeout_fails;
            q_begins_total += q_d_begins[k];
            q_commits_total += s1.commits - s0.commits;
            q_submits_total += s1.submits - s0.submits;
            q_reclaims_total += q_d_reclaims[k];
        }
        // 取舍规则（写死在这里，免得"看着差不多"）：本轮要回答的是 **3 还是 4** ——
        // 判据 = **从 D=3 到 D=4 的「每次提交耗时」有没有 5% 以上的改善**：没有 ⇒ 取 **3**
        // （多一个在飞槽只会多压驱动，买不到吞吐）；有 ⇒ 再看 4→8（同样 < 5% ⇒ 取 4，否则取 8）。
        // 只看 3 / 4 / 8 三档是有意的：**D=1 与 D=2 的读数轮间抖动（±约 5%）比 2 与 3 的差别还大**，
        // 也就是说"2 还是 3"这件事在本测试的分辨力之下 —— 那件事留给 N4 的真帧，本轮不硬判。
        // 另外注意：「长等条数」**不能当否决条件**：GPU 被我们自己塞满时，取槽等一会儿是**设计要的行为**
        // （有界在飞的本意）；要看的量是**最长等待**有没有失控（应 ≈ 一次提交自己的 GPU 活）与**再深一档值不值**。
        // The question this round answers is 3 versus 4: does D=4 buy 5% or more of the per-submission cost over
        // D=3? If not, keep 3. Only the 3/4/8 rungs take part on purpose -- D=1/D=2 jitter more between rounds than
        // 2 versus 3 differs, so that little question is left to N4's real frames.
        q_inflight_chosen = DS[2];                       // 3
        if (q_d_avg_us[3] * 100 < q_d_avg_us[2] * 95) {  // 3 → 4 改善 ≥ 5% ⇒ 再往深看
            q_inflight_chosen = DS[3];
            if (q_d_avg_us[4] * 100 < q_d_avg_us[3] * 95) q_inflight_chosen = DS[4];
        }
        q_d4_buys_little = (q_d_avg_us[2] && q_d_avg_us[3] * 100 >= q_d_avg_us[2] * 95) ? 1 : 0;
        q_d34_gain_pm = q_d_avg_us[2] ? (int64_t)((q_d_avg_us[3] - q_d_avg_us[2]) * 1000 / q_d_avg_us[2]) : 0;
        for (int k = 0; k < 5; k++) {
            q_long_total += q_d_long[k];
            if (q_d_wait_max[k] > q_long_max_us) q_long_max_us = q_d_wait_max[k];
        }
        A("⇒ 三档的差别（就是「3 还是 4」的依据）：D=1 每次提交 %llu µs / 等待率 %llu‰ ↔ D=3 %llu µs / %llu‰ ↔ "
          "D=4 %llu µs / %llu‰ ↔ D=8 %llu µs / %llu‰",
          (unsigned long long)q_d_avg_us[0],
          (unsigned long long)(q_d_begins[0] ? q_d_waits[0] * 1000 / q_d_begins[0] : 0),
          (unsigned long long)q_d_avg_us[2],
          (unsigned long long)(q_d_begins[2] ? q_d_waits[2] * 1000 / q_d_begins[2] : 0),
          (unsigned long long)q_d_avg_us[3],
          (unsigned long long)(q_d_begins[3] ? q_d_waits[3] * 1000 / q_d_begins[3] : 0),
          (unsigned long long)q_d_avg_us[4],
          (unsigned long long)(q_d_begins[4] ? q_d_waits[4] * 1000 / q_d_begins[4] : 0));
        A("⇒ **3 还是 4**：D=3 → D=4 的每次提交耗时变化 = %+lld‰（%s）⇒ 按规则取 **D = %d**",
          (long long)q_d34_gain_pm, q_d4_buys_little ? "4 买不到吞吐（改善 < 5%）" : "4 确实更快",
          q_inflight_chosen);
        A("   **真帧的取舍要 N4 复核**（本轮的 GPU 活是我们自己造的 %llu MB 拷贝，不是游戏的帧；"
          "D=1 与 D=2 的轮间抖动比 2 与 3 的差别还大 ⇒ 「2 还是 3」本轮不硬判）",
          (unsigned long long)(busy_bytes >> 20));
    }

    // =============================================================== 问 4：删掉等待的对照
    Q("**删掉等待的对照**：`vk2_sub_wait_all()` 在有空槽在飞时等的是谁？（等所有在飞，不是等队列空闲）");
    {
        if (!vk2_sub_init(&d, 3)) { A("【红】槽建不出来"); }
        else {
            vk2_sub_stats s0, sw, s1;
            int began = 0;
            vk2_sub_stats_get(&s0);                       // 全程基准（含这三次取槽 / 提交）
            for (int i = 0; i < 3; i++) {
                vk2_slot *s = vk2_sub_begin();
                if (!s) break;
                record_busy(s->cb, ch0->buf, busy_bytes, ch0->size);
                if (vk2_sub_commit()) began++;
            }
            int pend_before = vk2_sub_pending();
            vk2_sub_stats_get(&sw);                       // 只量 wait_all 这一段
            uint64_t t0 = now_us();
            int ok = vk2_sub_wait_all();
            uint64_t dt = now_us() - t0;
            vk2_sub_stats_get(&s1);
            int pend_after = vk2_sub_pending();
            q_waitall_ok = (began == 3 && pend_before > 0 && ok && pend_after == 0);
            q_waitall_pending = (uint64_t)pend_before;
            q_waitall_waited = s1.wait_all_waited - sw.wait_all_waited;
            q_waitall_us = dt;
            q_waitall_idle_delta = s1.idle_calls - sw.idle_calls;
            A("提交 %d 个（每个带 %llu MB 的 GPU 拷贝，**提交后不等**）⇒ 在飞槽数 = %d；"
              "`vk2_sub_wait_all()` 返回 %d、耗时 %llu µs、**等了 %llu 个在飞槽的定向栅栏**、在飞槽数变成 %d",
              began, (unsigned long long)(busy_bytes >> 20), pend_before, ok,
              (unsigned long long)dt, (unsigned long long)q_waitall_waited, pend_after);
            A("⇒ %s（判据：等之前有在飞的、等之后归零，等的是**每个槽自己的栅栏**，"
              "`vkQueueWaitIdle` 计数变化 = %llu）",
              q_waitall_ok ? "等的是所有在飞槽，**不是等队列空闲**" : "【红】形状不对",
              (unsigned long long)q_waitall_idle_delta);
            q_idle_total += q_waitall_idle_delta;
            q_no_slot_total += s1.no_slot_fail - s0.no_slot_fail;
            q_timeout_total += s1.timeout_fails - s0.timeout_fails;
            q_begins_total += s1.begins - s0.begins;
            q_commits_total += s1.commits - s0.commits;
            q_submits_total += s1.submits - s0.submits;
            q_reclaims_total += s1.reclaims - s0.reclaims;
        }
    }

    // =============================================================== 问 5：槽被搬运窗口复用
    Q("**提交槽能被 N2 的搬运窗口复用**？（这决定了 `vk2_upload(cb, frame, pool)` 在 N4 怎么接）");
    {
        if (!vk2_sub_init(&d, 3)) { A("【红】槽建不出来"); }
        else {
            const uint64_t uoff = 1 * MB, ulen = 1 * MB;     // 池内偏移 1 MB 起 1 MB（首块内）
            uint8_t *got = malloc((size_t)ulen), *expect = malloc((size_t)ulen);
            vk2_sub_stats s0, s1, s2;
            vk2_sub_stats_get(&s0);
            // ---- (a) 真上传：影子写图案 → 标脏 → 录进这个槽的 cb → 提交 → 等这个 frame → 回读验真
            pat_fill((uint8_t *)shadow + uoff, ulen, seed + 7);
            for (uint64_t k = 0; k < ulen; k++) expect[k] = pat_byte(k, seed + 7);
            vk2_dirty_clean(dm, 0, pool_bytes);
            vk2_dirty_mark_cpu(dm, uoff, ulen);
            vk2_slot *s = vk2_sub_begin();
            uint64_t moved = 0, frame = 0;
            VkFence f = VK_NULL_HANDLE;
            int committed = 0;
            if (s) {
                frame = s->frame;
                uint64_t t0 = now_us();
                moved = vk2_upload(s->cb, frame, 0);      // **N2 的上传窗口录进这个槽的 cb**
                if (moved) vk2_sub_note_upload();         // 告诉槽"你载着一个上传窗口"
                committed = vk2_sub_commit();
                f = s->fence;                             // 在飞时取（回收后栅栏还在、还是 signal 的）
                q_slot_up_us = now_us() - t0;
                q_slot_up_bytes = moved;
                A("(a) 一个槽 = 一次客体提交：把池内 %llu B 标脏 ⇒ `vk2_upload(s->cb, %llu, 0)` 搬了 %llu B、"
                  "`vk2_sub_commit()` = %d、耗时 %llu µs",
                  (unsigned long long)ulen, (unsigned long long)frame, (unsigned long long)moved,
                  committed, (unsigned long long)q_slot_up_us);
            }
            if (committed) {
                int wok = vk2_sub_wait(frame);            // 等这个 frame ⇒ 回收这个槽（顺带把环交还）
                int ring_free = (vk2_xfer_window_open() == 0);
                int ok = 0;
                if (wok && f) {
                    vk2_readback_after(f);                // 等的是**这次提交的栅栏**（不是队列空闲）
                    ok = vk2_readback(ch0->buf, uoff, got, (size_t)ulen);
                }
                uint64_t bad = (uint64_t)-1;
                if (ok) {
                    bad = 0;
                    for (uint64_t k = 0; k < ulen; k++) if (got[k] != expect[k]) bad++;
                }
                vk2_sub_stats_get(&s1);
                uint64_t recycled = s1.upload_recycled - s0.upload_recycled;
                q_slot_up_bad = (bad == (uint64_t)-1) ? 0xffffffffull : bad;
                q_slot_up_recycled = recycled;
                q_slot_up_ring_free = ring_free;
                q_slot_up_ok = (wok && ok && bad == 0 && moved == ulen && recycled == 1 && ring_free);
                A("    等 frame %llu：%d（定向栅栏）⇒ 槽回收时**自动把环交还**（`upload_recycled` +%llu）、"
                  "`vk2_xfer_window_open()` = %d", (unsigned long long)frame, wok,
                  (unsigned long long)recycled, !ring_free);
                A("    回读（等这次提交的栅栏再拷回）：%d，%llu 字节与**逐字节重算的期望**比对 ⇒ %s ⇒ "
                  "**槽能被搬运窗口复用：%s**",
                  ok, (unsigned long long)ulen,
                  bad == 0 ? "逐字节相符" : "【红】有出入（第一处不在此打印，见下）",
                  q_slot_up_ok ? "成立" : "【红】不成立");
                if (ok && bad) {
                    for (uint64_t k = 0; k < ulen; k++)
                        if (got[k] != expect[k]) { A("      第一处不符在偏移 %llu（期望 %02x 实得 %02x）",
                                                    (unsigned long long)k, expect[k], got[k]); break; }
                }
                // 交还之后再录一窗：必须被接受（自证"窗口真的还回来了"）
                vk2_dirty_mark_cpu(dm, uoff, ulen);
                vk2_slot *s2 = vk2_sub_begin();
                uint64_t moved2 = s2 ? vk2_upload(s2->cb, s2->frame, 0) : 0;
                if (s2) { if (moved2) vk2_sub_note_upload(); vk2_sub_commit(); vk2_sub_wait(s2->frame); }
                A("    环交还之后再录一窗：搬了 %llu B ⇒ %s", (unsigned long long)moved2,
                  moved2 == ulen ? "被接受（窗口确实还回来了）" : "【红】没被接受");
            } else {
                A("【红】(a) 没提交成功（取槽 / 上传 / 提交任一步失败）");
            }
            // ---- (b) 在飞 3 个槽都带上传：环是**单窗口**的 ⇒ 第 2、3 个槽看到环被占着（N4 要按槽分环）
            vk2_sub_stats_get(&s2);
            int contended = 0, uploaded = 0;
            A("(b) 在飞 3 个槽都想载上传窗口（每个槽另带 %llu MB 的 GPU 拷贝，保证前一个还在飞）：",
              (unsigned long long)(busy_bytes >> 20));
            for (int i = 0; i < 3; i++) {
                vk2_slot *sq = vk2_sub_begin();
                if (!sq) break;
                record_busy(sq->cb, ch0->buf, busy_bytes, ch0->size);
                uint64_t fq = sq->frame;
                if (vk2_xfer_window_open()) {
                    contended++;      // 环被前一个在飞窗口占着：这不该报【红】，它就是本轮量出来的限制
                    A("      槽 %d（frame %llu）：**环还被上一个在飞槽占着** ⇒ 这一帧不录上传窗口", i + 1,
                      (unsigned long long)fq);
                } else {
                    vk2_dirty_mark_cpu(dm, uoff, ulen);
                    uint64_t mv = vk2_upload(sq->cb, fq, 0);
                    if (mv) { vk2_sub_note_upload(); uploaded++; }
                    A("      槽 %d（frame %llu）：录了上传窗口，搬 %llu B", i + 1, (unsigned long long)fq,
                      (unsigned long long)mv);
                }
                vk2_sub_commit();
            }
            if (vk2_sub_wait_all()) { /* 全部做完，窗口都该交还 */ }
            vk2_sub_stats st3;
            vk2_sub_stats_get(&st3);
            q_ring_contended = (uint64_t)contended;
            q_ring_uploaded = (uint64_t)uploaded;
            q_ring_recycled = st3.upload_recycled - s2.upload_recycled;
            A("      ⇒ 3 个在飞槽里：%d 个录到了上传窗口、%d 个看到环被占着；等回全部之后交还 %llu 个窗口 ⇒ %s",
              uploaded, contended, (unsigned long long)q_ring_recycled,
              (uploaded + contended == 3 && q_ring_recycled == (uint64_t)uploaded)
                  ? "账对得上" : "【红】账对不上");
            A("      ⇒ **这项事实是 N4 的入口**：暂存环是**一份**，所以在飞深度 3 时只有**一个**槽能载上传窗口；"
              "N4 要让上传与在飞深度解耦就得**按槽分环**（或给环做分配器）——本轮不改 N2 的模块，只把它量出来");
            q_idle_total += st3.idle_calls - s0.idle_calls;
            q_no_slot_total += st3.no_slot_fail - s0.no_slot_fail;
            q_timeout_total += st3.timeout_fails - s0.timeout_fails;
            q_begins_total += st3.begins - s0.begins;
            q_commits_total += st3.commits - s0.commits;
            q_submits_total += st3.submits - s0.submits;
            q_reclaims_total += st3.reclaims - s0.reclaims;
            free(got); free(expect);
        }
    }

    // =============================================================== 问 6：键值
    {
        vk2_xfer_stats xs;
        vk2_xfer_stats_get(&xs);
        unsigned long long mem_peak = mem_avail_kb();
        vk2_sub_fini();
        vk2_pool_del(shadow);
        vk2_xfer_fini();
        vk2_pool_fini();
        munmap(shadow, (size_t)pool_bytes);
        unsigned long long mem_end = mem_avail_kb();

        printf("\n--- VK2_SUB_SUMMARY（结论键值，供文档抄录）---\n");
        printf("VK2_ROUTE=%s\n", q_route_gfx2 ? "new" : "old");
        printf("VK2_GFX2_SWITCH_SELFPROOF=%d\n", q_gfx2_flip);
        printf("VK2_G4_PER_COMMIT_OK=%d\n", q_g4_per_commit);
        printf("VK2_G4_SELFPROOF_COMMITS=%llu\n", (unsigned long long)q_g4_my_commits);
        printf("VK2_G4_COMMITS=%llu\n", (unsigned long long)q_g4_commits);
        printf("VK2_G4_SUBMITS=%llu\n", (unsigned long long)q_g4_submits);
        printf("VK2_G4_SUBMIT_FAIL=%llu\n", (unsigned long long)q_g4_submit_fail);
        printf("VK2_G4_EMPTY_SUBMITS=%llu\n", (unsigned long long)q_g4_empty);
        printf("VK2_G4_EMPTY_US=%llu\n", (unsigned long long)q_g4_empty_us);
        printf("VK2_G4_RECLAIMS=%llu\n", (unsigned long long)q_g4_reclaims);
        printf("VK2_G4_IDLE_ZERO=%d\n", q_g4_idle_zero);
        for (int k = 0; k < 5; k++) {
            printf("VK2_D%d_BEGINS=%llu\n", DS[k], (unsigned long long)q_d_begins[k]);
            printf("VK2_D%d_WAITS=%llu\n", DS[k], (unsigned long long)q_d_waits[k]);
            printf("VK2_D%d_POLL_REAPS=%llu\n", DS[k], (unsigned long long)q_d_polls[k]);
            printf("VK2_D%d_LONG=%llu\n", DS[k], (unsigned long long)q_d_long[k]);
            printf("VK2_D%d_BEGIN_LONG=%llu\n", DS[k], (unsigned long long)q_d_begin_long[k]);
            printf("VK2_D%d_WAIT_MAX_US=%llu\n", DS[k], (unsigned long long)q_d_wait_max[k]);
            printf("VK2_D%d_AVG_COMMIT_US=%llu\n", DS[k], (unsigned long long)q_d_avg_us[k]);
            printf("VK2_D%d_BUSY_MB=%llu\n", DS[k], (unsigned long long)q_d_busy_mb[k]);
        }
        printf("VK2_SUB_LONG_TOTAL=%llu\n", (unsigned long long)q_long_total);
        printf("VK2_SUB_LONG_MAX_US=%llu\n", (unsigned long long)q_long_max_us);
        printf("VK2_INFLIGHT_CHOSEN=%d\n", q_inflight_chosen);
        printf("VK2_INFLIGHT_DEFAULT=%d\n", VK2_INFLIGHT_DEFAULT);
        printf("VK2_D4_BUYS_LITTLE=%d\n", q_d4_buys_little);
        printf("VK2_D3_TO_D4_GAIN_PM=%lld\n", (long long)q_d34_gain_pm);
        printf("VK2_WAITALL_OK=%d\n", q_waitall_ok);
        printf("VK2_WAITALL_PENDING_BEFORE=%llu\n", (unsigned long long)q_waitall_pending);
        printf("VK2_WAITALL_WAITED=%llu\n", (unsigned long long)q_waitall_waited);
        printf("VK2_WAITALL_US=%llu\n", (unsigned long long)q_waitall_us);
        printf("VK2_WAITALL_IDLE_DELTA=%llu\n", (unsigned long long)q_waitall_idle_delta);
        printf("VK2_SLOT_UPLOAD_OK=%d\n", q_slot_up_ok);
        printf("VK2_SLOT_UPLOAD_BYTES=%llu\n", (unsigned long long)q_slot_up_bytes);
        printf("VK2_SLOT_UPLOAD_BAD=%llu\n", (unsigned long long)q_slot_up_bad);
        printf("VK2_SLOT_UPLOAD_RECYCLED=%llu\n", (unsigned long long)q_slot_up_recycled);
        printf("VK2_SLOT_UPLOAD_RING_FREE=%llu\n", (unsigned long long)q_slot_up_ring_free);
        printf("VK2_SLOT_UPLOAD_US=%llu\n", (unsigned long long)q_slot_up_us);
        printf("VK2_RING_CONTENDED=%llu\n", (unsigned long long)q_ring_contended);
        printf("VK2_RING_UPLOADED=%llu\n", (unsigned long long)q_ring_uploaded);
        printf("VK2_RING_RECYCLED=%llu\n", (unsigned long long)q_ring_recycled);
        printf("VK2_SUB_BEGINS_TOTAL=%llu\n", (unsigned long long)q_begins_total);
        printf("VK2_SUB_COMMITS_TOTAL=%llu\n", (unsigned long long)q_commits_total);
        printf("VK2_SUB_SUBMITS_TOTAL=%llu\n", (unsigned long long)q_submits_total);
        printf("VK2_SUB_RECLAIMS_TOTAL=%llu\n", (unsigned long long)q_reclaims_total);
        printf("VK2_SUB_IDLE_CALLS=%llu\n", (unsigned long long)q_idle_total);
        printf("VK2_SUB_FENCE_TIMEOUTS=%llu\n", (unsigned long long)q_timeout_total);
        printf("VK2_SUB_NO_SLOT_FAIL=%llu\n", (unsigned long long)q_no_slot_total);
        printf("VK2_XFER_FENCE_TIMEOUTS=%llu\n", (unsigned long long)xs.fence_timeouts);
        printf("VK2_XFER_IDLE_CALLS=%llu\n", (unsigned long long)xs.idle_calls);
        printf("VK2_XFER_COPIES=%llu\n", (unsigned long long)xs.copies);
        printf("VK2_XFER_BARRIERS=%llu\n", (unsigned long long)xs.barriers);
        printf("VK2_XFER_WINDOWS=%llu\n", (unsigned long long)xs.windows);
        printf("VK2_XFER_WINDOW_REBUILDS=%llu\n", (unsigned long long)xs.ring_wrap);
        printf("VK2_RING_MB=%llu\n", (unsigned long long)(vk2_xfer_ring_bytes() >> 20));
        printf("VK2_POOL_MB=%llu\n", (unsigned long long)pool_mb);
        printf("VK2_MEM_AVAIL_START_MB=%llu\n", (unsigned long long)(mem_start >> 10));
        printf("VK2_MEM_AVAIL_PEAK_MB=%llu\n", (unsigned long long)(mem_peak >> 10));
        printf("VK2_MEM_AVAIL_END_MB=%llu\n", (unsigned long long)(mem_end >> 10));
        fflush(stdout);
    }

    vk_close(&v);
    printf("\n完成（N3：让提交槽可用，没有接管回放层）\n");
    return 0;
}
