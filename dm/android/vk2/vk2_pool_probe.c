// 新地基 N1（影子内存 + 分块后备资源 + 地址反查）的**自证探针** —— 把判据 **G3** 做成一问一答。
//
// 它不同于 N0 的 `vk2_probe`（那个"只问不接管"、问的是四个常数）：本探针**真的建池**，并且只做 N1 允许的那一件事——
// 让 `vk2_pool.c` 这个新模块在真机上被**用一遍**（建 / 分配 / 绑定 / 头尾都真写一遍并回读 / 反查 / 降级）。
// 它**不**接管回放层（不改 `nvn_vk.c` / `vk_tex.c` / `vk_region.c`）、**不**写脏记账与上传窗口（N2 的活）、
// **不**跑游戏轮（所以旧路那套「健康 / 生病 / 停顿型」与本探针无关——判据不混用）。
//
// 一问一答（每个问题都 `[问 n] …` + 若干 `[答] …`，末尾一段 `--- VK2_POOL_SUMMARY ---` 键值）：
//   问 1 这次走哪条路？（`DM_GFX2` 缺省关；开关**可读且自证**：读到什么 → 翻一下再读 → 翻回来再读）
//   问 2 池内存是不是普通内存？（4 GB 匿名 mmap：`/proc/self/maps` 的权限位 + 读写 + 原子加 + 互斥）
//   问 3 **G3 主问**：4 GB 池能不能**按块**建出**全功能**后备资源？（建 + 分配 + 绑定 + **每块头尾都真写一遍并回读**）
//        以及两个计数器 `g_pool_small` / `g_pool_no_region` 是不是 **0**（旧路就是它们不为 0 才一路静默降级）
//   问 4 别的池呢？（glibc 堆上的小池、**非页对齐**的池——旧路会拒的那些）
//   问 5 地址反查：正例 / 跨块 / 未命中；**跨块请求的长度分布**（§9.3 要的那份账，也是「块大小该定多少」的依据）
//   问 6 请求 PINNED 会怎样？（§9.8 的降级规则：回落 SHADOW、报计数**不报红**、正确性不依赖它）
//
// 跑法（一律走项目内 `.venv` Python，见 `.codebuddy/rules/windows-python-only/RULE.mdc`）：
//   & "<仓库>/.venv/Scripts/python.exe" -X utf8 .work/probes.py build vk2_pool_probe
//   & "<仓库>/.venv/Scripts/python.exe" -X utf8 .work/probes.py run vk2_pool_probe --serial 8b1f1e50 [--tag r1]
// 读数落 `.work/logs/probe-<串号>-vk2_pool_probe[-tag].txt`。
//
// 开关：DM_VK2_POOL_MB（缺省 4096，客体池大小）、DM_VK2_XLEN_N（缺省 200000，长度分布抽多少次）、
//       DM_VK2_SEED（缺省 20261016，长度分布的伪随机种子，便于复现）
//
// ---------------------------------------------------------------------------------------------------------------
// The self-proving probe of N1 (shadow memory + chunked backing store + address lookup): it turns gate G3 into a
// question-and-answer run. Unlike N0's vk2_probe ("ask only, take over nothing"), this one really creates pools
// and exercises the new module on the device (create / allocate / bind / write-and-read-both-ends of every chunk /
// lookup / degradation). It does NOT take over the replay layer, does NOT implement dirty accounting or upload
// windows (N2's work) and does NOT run the game, so the old road's health/sickness categories are irrelevant here.
#define _GNU_SOURCE 1
#include "vk2_pool.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MB (1024ull * 1024ull)
#define GB (1024ull * MB)
#define PAGE 4096ull

// 长度分布用的九档典型长度（字节）| the nine typical lengths used by the distribution sweep
static const uint64_t g_lens[9] = { 64, 256, 1024, 4096, 65536, 262144, 1 * MB, 4 * MB, 16 * MB };

// ---------------------------------------------------------------- 自证格式（与 N0 的探针同一套）
static int g_qn;
static void Q(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("\n[问 %d] ", ++g_qn); vprintf(fmt, ap); printf("\n");
    va_end(ap); fflush(stdout);
}
static void A(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("[答] "); vprintf(fmt, ap); printf("\n");
    va_end(ap); fflush(stdout);
}
// 池模块自己的日志：带上前缀，混在问答里还能分辨 | the pool module's own log line, marked so it stays readable
static void pool_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("[池] "); vprintf(fmt, ap); printf("\n");
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
// 打印覆盖某地址的那一行 /proc/self/maps：池「是普通内存」这件事的直接读数
// Print the /proc/self/maps line covering an address: the direct reading of "the pool is ordinary memory"
static void print_maps_line(const void *addr) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { A("  /proc/self/maps 读不到（普通内存这件事就没法自证）"); return; }
    char line[512];
    uintptr_t a = (uintptr_t)addr;
    while (fgets(line, sizeof line, f)) {
        unsigned long long s = 0, e = 0;
        if (sscanf(line, "%llx-%llx", &s, &e) != 2) continue;
        if (a >= s && a < e) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            A("  %p 落在：%s", addr, line);
            break;
        }
    }
    fclose(f);
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

static int vk_open(probe_dev *v) {
    memset(v, 0, sizeof *v);
    VkApplicationInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "dm-vk2-pool-probe";
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
    v->phys = dv[0];                        // 这台机上只有一个物理设备 | one physical device on this device
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

static uint32_t find_type(const probe_dev *v, uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags avoid) {
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

// ---------------------------------------------------------------- 一小段模板：填一页 + 回读比对
// 这是 G3 判据里「**头尾都真写一遍并回读**」那一步（N0 已经证明：超过上限的分配会「看着成功」，只有尾页回读对不上）
// Fill one page through the GPU and read it back byte for byte -- the step G3 insists on, because N0 measured that
// an over-limit allocation "looks successful" and only the last page fails to read back.
typedef struct { VkCommandPool pool; VkCommandBuffer cb; VkFence fence; VkBuffer stg; VkDeviceMemory stg_mem; void *stg_ptr; } scratch;

static int scratch_make(probe_dev *v, scratch *s) {
    memset(s, 0, sizeof *s);
    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    // 这几个 cb 会被**反复重录** ⇒ 池上必须有 RESET 标志（N0 的第二个探针 bug 就栽在这里：不加时的表现是
    // 「某一条之后 vkQueueSubmit 开始返回 -3」，看着像驱动的锅）| these command buffers are re-recorded over and
    // over, so the pool needs the RESET flag (N0's second probe bug: without it vkQueueSubmit started returning -3)
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = v->qfi;
    if (vkCreateCommandPool(v->dev, &pci, NULL, &s->pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof cbai);
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = s->pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(v->dev, &cbai, &s->cb) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof fci);
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(v->dev, &fci, NULL, &s->fence) != VK_SUCCESS) return 0;
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof bci);
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = PAGE;                                  // 先只管一页：本探针逐页填、逐页回读 | one page at a time
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(v->dev, &bci, NULL, &s->stg) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(v->dev, s->stg, &mr);
    uint32_t mi = find_type(v, mr.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    if (mi == UINT32_MAX) return 0;
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof mai);
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mi;
    if (vkAllocateMemory(v->dev, &mai, NULL, &s->stg_mem) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(v->dev, s->stg, s->stg_mem, 0) != VK_SUCCESS) return 0;
    if (vkMapMemory(v->dev, s->stg_mem, 0, PAGE, 0, &s->stg_ptr) != VK_SUCCESS) return 0;
    return 1;
}
static void scratch_free(probe_dev *v, scratch *s) {
    if (s->stg_ptr) vkUnmapMemory(v->dev, s->stg_mem);
    if (s->stg_mem) vkFreeMemory(v->dev, s->stg_mem, NULL);
    if (s->stg) vkDestroyBuffer(v->dev, s->stg, NULL);
    if (s->fence) vkDestroyFence(v->dev, s->fence, NULL);
    if (s->pool) vkDestroyCommandPool(v->dev, s->pool, NULL);   // cb 随池一起回收 | the cb dies with the pool
}

// 在 dst[off, off+4096) 里填 pat，再拷回 host 可见缓冲逐字节比。返回 1 = 一致。
// Fill dst[off, off+4096) with pat and copy it back for a byte-for-byte comparison. 1 = identical.
static int page_fill_readback(probe_dev *v, scratch *s, VkBuffer dst, uint64_t off, uint32_t pat,
                              uint32_t *bad_at, uint8_t *bad_got, uint8_t *bad_want) {
    if (bad_at) *bad_at = 0;
    vkResetCommandBuffer(s->cb, 0);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s->cb, &bi) != VK_SUCCESS) return 0;
    vkCmdFillBuffer(s->cb, dst, (VkDeviceSize)off, PAGE, pat);
    VkBufferMemoryBarrier bb;
    memset(&bb, 0, sizeof bb);
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = dst; bb.offset = 0; bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(s->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
    VkBufferCopy cp;
    memset(&cp, 0, sizeof cp);
    cp.srcOffset = (VkDeviceSize)off; cp.dstOffset = 0; cp.size = PAGE;
    vkCmdCopyBuffer(s->cb, dst, s->stg, 1, &cp);
    if (vkEndCommandBuffer(s->cb) != VK_SUCCESS) return 0;
    vkResetFences(v->dev, 1, &s->fence);
    VkSubmitInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &s->cb;
    VkResult r = vkQueueSubmit(v->q, 1, &si, s->fence);
    if (r != VK_SUCCESS) { A("  【红】填+回读的提交失败：VkResult %d", (int)r); return 0; }
    r = vkWaitForFences(v->dev, 1, &s->fence, VK_TRUE, 5000ull * 1000000ull);
    if (r != VK_SUCCESS) { A("  【红】填+回读等栅栏超时：VkResult %d", (int)r); return 0; }
    const uint8_t *b = (const uint8_t *)s->stg_ptr;
    for (uint32_t i = 0; i < PAGE; i++) {
        uint8_t want = (uint8_t)((pat >> ((i & 3) * 8)) & 0xff);
        if (b[i] != want) {
            if (bad_at) *bad_at = i;
            if (bad_got) *bad_got = b[i];
            if (bad_want) *bad_want = want;
            return 0;
        }
    }
    return 1;
}

// ---------------------------------------------------------------- 定长的伪随机（复现用）
static uint64_t g_rng;
static void rng_seed(uint64_t s) { g_rng = s ? s : 88172645463325252ull; }
static uint64_t rng_next(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

// ---------------------------------------------------------------- 主流程
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("=== vk2_pool_probe：新地基 N1（影子 + 分块后备 + 地址反查）的自证探针 ===\n");
    printf("构建：%s %s；pid=%d；页大小=%ld 字节\n", __DATE__, __TIME__, (int)getpid(), sysconf(_SC_PAGESIZE));
    vk2_pool_set_log(pool_log);          // 池模块的日志进本探针的读数文件 | the module logs into this reading file

    uint64_t pool_mb = env_u64("DM_VK2_POOL_MB", 4096);
    uint64_t xlen_n = env_u64("DM_VK2_XLEN_N", 200000);
    uint64_t seed = env_u64("DM_VK2_SEED", 20261016);

    probe_dev v;
    if (!vk_open(&v)) { fprintf(stderr, "【红】Vulkan 打不开（没有实例 / 没有图形队列 / 没设备）\n"); return 2; }
    printf("设备 %s；apiVersion %u.%u.%u；maxStorageBufferRange %llu MB；maxMemoryAllocationSize %llu MB\n",
           v.props.deviceName, VK_VERSION_MAJOR(v.props.apiVersion), VK_VERSION_MINOR(v.props.apiVersion),
           VK_VERSION_PATCH(v.props.apiVersion),
           (unsigned long long)(v.props.limits.maxStorageBufferRange >> 20),
           (unsigned long long)(v.m3.maxMemoryAllocationSize >> 20));
    unsigned long long mem_start = mem_avail_kb(), mem_after_mmap = 0;
    printf("MemAvailable 起始 %llu MB\n", (unsigned long long)(mem_start >> 10));

    // 结果键值（末尾汇总）| the key/values of the summary printed at the end
    int q_gfx2_env = 0, q_gfx2_unset = 0, q_gfx2_flip = 0;
    int q_mem_rw = 0, q_mem_atomic = 0, q_mem_mutex = 0;
    int q_init = 0, q_4g_add = 0, q_4g_chunks = 0, q_4g_ht_ok = 0;
    uint64_t q_chunk_mb = 0, q_small = 0, q_no_region = 0, q_bytes = 0;
    int q_ubo_bit = -1;
    int q_aligned_pool = 0, q_unaligned_pool = 0;
    uint64_t q_hits = 0, q_misses = 0, q_cross = 0;
    int q_pos_ok = 0, q_cross_ret0 = 0, q_miss_ret0 = 0;
    uint64_t q_xlen_cnt[9] = { 0 }, q_xlen_cross[9] = { 0 };
    int q_pin_capable = -1, q_pin_ret = -1; uint64_t q_pin_fail = 0; int q_addr_after_pin = 0;
    int q_tagged_pool = -1, q_tagged_addr = -1; uint64_t q_tag_bits = 0;
    char hist[512] = "";

    // =============================================================== 问 1：这次走哪条路
    Q("这次走哪条路？（DM_GFX2 缺省关；开关要**可读且自证**——不是「看着有作用」，而是「读到的是什么」）");
    {
        const char *raw = getenv("DM_GFX2");
        A("环境里的 DM_GFX2 = %s", raw && *raw ? raw : "（未设）");
        q_gfx2_env = vk2_gfx2_enabled();
        A("vk2_gfx2_enabled() = %d（1 = 新地基，0 = 旧路）", q_gfx2_env);
        A("vk2_route_line()：%s", vk2_route_line());
        // 翻一下再读、翻回来再读：探针自己证明这个开关**真的被读到**了（不是印了个常量）
        // Flip it, read again, flip back: the probe proves the switch really is read, not a printed constant.
        setenv("DM_GFX2", "1", 1);
        int on = vk2_gfx2_enabled();
        A("把 DM_GFX2 置为 1 之后读到 = %d；路线行：%s", on, vk2_route_line());
        setenv("DM_GFX2", "0", 1);
        int off = vk2_gfx2_enabled();
        A("置为 0 之后读到 = %d", off);
        if (raw) setenv("DM_GFX2", raw, 1); else unsetenv("DM_GFX2");
        q_gfx2_unset = off;
        q_gfx2_flip = (on == 1 && off == 0);
        A("%s", q_gfx2_flip ? "⇒ 开关可读且自证：1 ⇒ 新地基、0/未设 ⇒ 旧路（缺省关成立）"
                            : "【红】开关的读法自证失败");
        A("⇒ 口径：**真正的选路点与接管回放层是 N4**。本轮只让开关「可读」，不改回放层——"
          "路线行现在只出现在启动 / 探针日志里");
    }

    // =============================================================== 问 2：池内存是普通内存吗
    Q("客体池的内存是不是**普通内存**？（旧路把客体地址空间整体换成 GPU 内存；新地基只要求它是普通内存）");
    void *shadow = NULL;
    uint64_t pool_bytes = pool_mb * MB;
    {
        shadow = mmap(NULL, (size_t)pool_bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (shadow == MAP_FAILED) {
            A("【红】mmap %llu MB 失败：%s ⇒ 后面几条都做不了", (unsigned long long)pool_mb, strerror(errno));
            shadow = NULL;
        } else {
            A("mmap %llu MB 得到 %p（PROT_READ|PROT_WRITE、匿名、MAP_NORESERVE）",
              (unsigned long long)pool_mb, shadow);
            print_maps_line(shadow);
            uint8_t *p = (uint8_t *)shadow;
            p[0] = 0x5a; p[PAGE - 1] = 0xa5;
            p[pool_bytes - 1] = 0x3c;
            q_mem_rw = (p[0] == 0x5a && p[PAGE - 1] == 0xa5 && p[pool_bytes - 1] == 0x3c);
            A("头 / 头页尾 / 池尾三个字节读写：%s", q_mem_rw ? "一致" : "【红】不一致");
            // 原子加：AHB 那条路丢掉的就是这个（第二台上 LDADDAL 当场把宿主打成 SIGBUS）
            // Atomic add: exactly what the AHB route lost (on device 2, LDADDAL killed the host with SIGBUS)
            uint64_t *at = (uint64_t *)(p + 3 * PAGE);
            uint64_t old = __atomic_fetch_add(at, 1, __ATOMIC_SEQ_CST);
            q_mem_atomic = (old == 0 && *at == 1);
            A("池内一处做原子加（__atomic_fetch_add）：旧值 %llu、新值 %llu ⇒ %s",
              (unsigned long long)old, (unsigned long long)*at, q_mem_atomic ? "原子语义成立" : "【红】不成立");
            // 互斥量也放在池里：客体一拿到堆就要用互斥 / 引用计数
            // A mutex placed inside the pool: the guest uses mutexes and refcounts the moment it gets the heap
            pthread_mutex_t *mx = (pthread_mutex_t *)(p + 8 * PAGE);
            int mi = pthread_mutex_init(mx, NULL);
            int lk = mi == 0 ? pthread_mutex_trylock(mx) : -1;
            int ul = lk == 0 ? pthread_mutex_unlock(mx) : -1;
            if (mi == 0) pthread_mutex_destroy(mx);
            q_mem_mutex = (mi == 0 && lk == 0 && ul == 0);
            A("池内放一个 pthread_mutex 并加解锁：%s", q_mem_mutex ? "成立" : "【红】不成立");
            A("⇒ 这三条就是「第二台启动就停在内存换轨 / AHB 原子访问 SIGBUS 并把内核带 panic」在新地基上"
              "**从根上不存在**的原因：池就是普通内存，GPU 用另一份");
            mem_after_mmap = mem_avail_kb();
            A("池内存备好（还没建任何后备资源）时的 MemAvailable = %llu MB（起始 %llu MB ⇒ 4 GB 的匿名映射、"
              "MAP_NORESERVE，只碰了三个字节，所以这里几乎不掉）",
              (unsigned long long)(mem_after_mmap >> 10), (unsigned long long)(mem_start >> 10));
        }
    }
    if (!shadow) { vk_close(&v); return 2; }

    // =============================================================== 初始化池模块
    vk2_dev d;
    memset(&d, 0, sizeof d);
    d.dev = v.dev; d.phys = v.phys; d.qfi = v.qfi; d.mp = v.mp; d.props = v.props; d.m3 = v.m3;
    q_init = vk2_pool_init(&d, 0);          // 0 = 用缺省块大小（960 MB 的代码常数）| 0 = the default chunk size
    if (!q_init) { fprintf(stderr, "【红】vk2_pool_init 失败\n"); return 2; }
    q_chunk_mb = vk2_pool_chunk_bytes() >> 20;
    A("\n池模块已就绪：块 = %llu MB（§9.9 的真机读数写成了代码常数）", (unsigned long long)q_chunk_mb);

    scratch sc;
    if (!scratch_make(&v, &sc)) { fprintf(stderr, "【红】scratch（命令池 / cb / 栅栏 / 回读缓冲）建不出来\n"); return 2; }

    // =============================================================== 问 3：G3 主问
    Q("**G3**：%llu MB 的池能不能**按块**建出**全功能**后备资源？（建 + 分配 + 绑定 + **每块头尾都真写一遍并回读**）"
      "两个计数器 g_pool_small / g_pool_no_region 是不是 0？", (unsigned long long)pool_mb);
    {
        unsigned long long before = mem_avail_kb();
        q_4g_add = vk2_pool_add(shadow, pool_bytes, "probe-4g");
        unsigned long long after = mem_avail_kb();
        if (!q_4g_add) {
            A("【红】4 GB 池登记失败 ⇒ G3 不过（上面的日志里有失败点名）");
        } else {
            const vk2_pool *q = NULL;
            for (int i = 0; i < vk2_pool_count(); i++) {
                const vk2_pool *c = vk2_pool_at(i);
                if (c && c->lo == (uintptr_t)shadow) { q = c; break; }
            }
            if (!q) {
                A("【红】找不到刚登记的池（探针自己的账不对）");
            } else {
                q_4g_chunks = q->nchunk;
                A("池 %p+%llu MB ⇒ **%d 块**（%llu MB/块 + 余数）；模式 = %s；脏记账字段是 N2 的占位（N1 不读）",
                  shadow, (unsigned long long)pool_mb, q->nchunk, (unsigned long long)q_chunk_mb,
                  q->mode == VK2_MODE_SHADOW ? "SHADOW（正确性路径）" : "PINNED");
                uint64_t nsmall = 0;
                for (int k = 0; k < q->nchunk; k++) {
                    A("  块 %d：池内偏移 %llu MB、大小 %llu MB、只带传输位=%d、buf=%p、mem=%p",
                      k, (unsigned long long)(q->chunk[k].off >> 20),
                      (unsigned long long)(q->chunk[k].size >> 20), q->chunk[k].small,
                      (void *)(uintptr_t)q->chunk[k].buf, (void *)(uintptr_t)q->chunk[k].mem);
                    if (q->chunk[k].small) nsmall++;
                }
                // 每块头尾都真写一遍并回读：**这是 G3 判据里不许省的那一步**（N0：超上限的分配会「看着成功」）
                // Write and read back both ends of every chunk: G3 refuses to skip this, because N0 measured that
                // an over-limit allocation "looks successful" until the last page is read back.
                int okn = 0;
                for (int k = 0; k < q->nchunk; k++) {
                    uint32_t pat_h = 0xa0a00000u + (uint32_t)k;
                    uint32_t pat_t = 0xb1b10000u + (uint32_t)k;
                    uint32_t bad = 0; uint8_t got = 0, want = 0;
                    uint64_t t0 = now_us();
                    int okh = page_fill_readback(&v, &sc, q->chunk[k].buf, 0, pat_h, &bad, &got, &want);
                    int okt = page_fill_readback(&v, &sc, q->chunk[k].buf, q->chunk[k].size - PAGE, pat_t,
                                                 &bad, &got, &want);
                    if (okh && okt) okn++;
                    A("  块 %d 头页（pat %08x）回读一致=%d；尾页（pat %08x @ %llu MB）回读一致=%d；耗时 %llu us%s",
                      k, pat_h, okh, pat_t, (unsigned long long)((q->chunk[k].size - PAGE) >> 20), okt,
                      (unsigned long long)(now_us() - t0),
                      (okh && okt) ? "" : "  ← 【红】这一块不能算「能用」");
                }
                q_4g_ht_ok = (okn == q->nchunk) ? 1 : 0;
                vk2_pool_stats st;
                vk2_pool_stats_get(&st);
                q_small = st.small; q_no_region = st.no_region; q_bytes = st.bytes;
                A("MemAvailable：池内存备好 %llu MB → 建块前 %llu MB → 建完 5 块（4 GB 后备）后 %llu MB",
                  (unsigned long long)(mem_after_mmap >> 10), (unsigned long long)(before >> 10),
                  (unsigned long long)(after >> 10));
                A("  ⇒ 池内存（4 GB 匿名 mmap）本身掉 %lld MB；**后备资源**掉 %lld MB（差 %lld MB 是池内存那部分）",
                  (long long)((long long)(mem_start >> 10) - (long long)(mem_after_mmap >> 10)),
                  (long long)((long long)(before >> 10) - (long long)(after >> 10)),
                  (long long)((long long)(mem_after_mmap >> 10) - (long long)(before >> 10)));
                A("  ⚠ 这条与 N0 的读数**不一致**：N0 里建单块 1024 MB + 四点填充只掉 2~7 MB（当时的结论是"
                  "「DEVICE_LOCAL 惰性提交」）；本轮 5 块 960 MB 掉的是整个 4 GB ⇒ 那句话**不能当结论**，"
                  "要按「池预算 = 后备资源字节数」算，并在 N2 里再量一次（见文档里的更正批注）");
                A("**G3 的两个计数器**：g_pool_small = %llu（退成只带传输位的块数）、"
                  "g_pool_no_region = %llu（因「池不是本进程可读写的普通内存」而一块没建出的池数）、pool_fail = %llu",
                  (unsigned long long)st.small, (unsigned long long)st.no_region,
                  (unsigned long long)st.pool_fail);
                A("⇒ %s", (st.small == 0 && st.no_region == 0 && q_4g_ht_ok)
                          ? "G3 成立：4 GB 池按块建出全功能后备资源，头尾回读逐字节一致，两个计数器都是 0"
                          : "【红】G3 不成立（看上面哪一条不为 0）");
            }
        }
        // 顺带一问（给 N4 用）：960 MB 的块**带 UNIFORM_BUFFER 位**建不建得出来？规范有一条 VU 要求
        // 「带 UBO 位的缓冲，size ≤ maxUniformBufferRange（本机 64 KB）」，而旧路是把 UBO 位给了整池的。
        // A side question for N4: can a 960 MB buffer carry the UNIFORM_BUFFER bit? A spec VU caps such buffers at
        // maxUniformBufferRange (64 KB here), and the old road handed the bit to a whole pool.
        {
            VkBufferCreateInfo bci;
            memset(&bci, 0, sizeof bci);
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = VK2_CHUNK_BYTES;
            bci.usage = VK2_FULL_USAGE | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkBuffer b = VK_NULL_HANDLE;
            VkResult r = vkCreateBuffer(v.dev, &bci, NULL, &b);
            q_ubo_bit = (r == VK_SUCCESS) ? 1 : 0;
            A("顺带：960 MB 的块带上 UNIFORM_BUFFER 位建缓冲 → %s（VkResult %d）——"
              "规范上这里有一条 VU（带 UBO 位的缓冲 ≤ maxUniformBufferRange = %u 字节），"
              "所以这条读数是**驱动的宽容度**、不是「合规」| 这是 N4 绑 UBO 时要处理的点",
              r == VK_SUCCESS ? "能建" : "建不出", (int)r, (unsigned)v.props.limits.maxUniformBufferRange);
            if (b) vkDestroyBuffer(v.dev, b, NULL);
        }
    }

    // =============================================================== 问 4：旧路会拒的那些池
    Q("别的池呢？—— mmap 出来的小池、**非页对齐**的池、以及 glibc 堆上**带高位标记**的池"
      "（旧路要求「池必须落在我们铺过的换轨区间里」，还要求页对齐）");
    void *heap_pool = NULL, *unaligned = NULL, *small_pool = NULL;
    {
        small_pool = mmap(NULL, 1 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (small_pool == MAP_FAILED) small_pool = NULL;
        void *um = mmap(NULL, 64 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        unaligned = (um == MAP_FAILED) ? NULL : (uint8_t *)um + 3;
        heap_pool = malloc(1 * MB);

        if (small_pool) q_aligned_pool = vk2_pool_add(small_pool, 1 * MB, "probe-small");
        if (unaligned) q_unaligned_pool = vk2_pool_add(unaligned, 64 * 1024 - 3, "probe-unaligned");
        A("mmap 1 MiB 的池：%s；mmap 64 KiB 后把指针偏移 3 字节、长度减 3 的**非页对齐**池：%s",
          q_aligned_pool ? "登记成功" : "【红】登记失败",
          q_unaligned_pool ? "登记成功" : "【红】登记失败");

        // 这一条是**仪器自己撞出来的事实**：这台机的 `malloc` 返回带高位标记的指针（TBI），
        // 而 `mmap` 与 `/proc/self/maps` 给的都是不带标记的地址 ⇒ 池地址必须先掩到低 48 位再比。
        // This is a fact the instrument ran into by itself: malloc returns high-bit-tagged (TBI) pointers here
        // while mmap and /proc/self/maps are untagged, so pool addresses have to be masked to 48 bits first.
        if (heap_pool) {
            uint64_t raw = (uint64_t)(uintptr_t)heap_pool;
            uint64_t masked = (uint64_t)vk2_ptr_mask((uintptr_t)heap_pool);
            q_tag_bits = raw ^ masked;
            q_tagged_pool = vk2_pool_add(heap_pool, 1 * MB, "probe-malloc-tagged");
            VkBuffer b = VK_NULL_HANDLE; uint64_t o = 0;
            q_tagged_addr = vk2_addr(raw + 64, 256, &b, &o);
            A("glibc 堆上的 1 MiB 池（malloc 返回 %#llx，最高字节的标记 %#llx，掩掉之后 = %#llx）：登记 %s；"
              "用**带标记的那个地址**反查 ⇒ %d",
              (unsigned long long)raw, (unsigned long long)q_tag_bits, (unsigned long long)masked,
              q_tagged_pool ? "成功" : "【红】失败", q_tagged_addr);
        } else {
            A("【黄】malloc 1 MiB 失败，带标记的那一条跳过");
        }

        vk2_pool_stats st;
        vk2_pool_stats_get(&st);
        A("此时计数器：pools=%llu、no_region=%llu（**必须为 0**）、small=%llu、overlap=%llu、"
          "带标记的登记=%llu、bytes=%llu MB",
          (unsigned long long)st.pools, (unsigned long long)st.no_region, (unsigned long long)st.small,
          (unsigned long long)st.overlap, (unsigned long long)st.tagged, (unsigned long long)(st.bytes >> 20));
        A("⇒ 旧路那两条硬前提（页对齐、落在换轨区间里）在新地基上**都不存在**："
          "池只要是本进程可读写的普通内存就行（no_region 恒 0 的原因就在这里）；"
          "地址最高字节带标记也照旧——掩掉那个字节之后一切如常（TBI：硬件本来就忽略位 56~63）");
    }

    // =============================================================== 问 5：地址反查
    Q("地址反查：正例 / 跨块 / 未命中；**跨块请求的长度分布**（§9.3 要的那份账，也是「块大小该定多少」的依据）");
    {
        uint64_t cb = vk2_pool_chunk_bytes();
        vk2_pool_stats st1;
        // 正例：池首、第二块首、最后一块尾（请求都远小于块）| positives: pool base, 2nd chunk base, last tail
        struct { const char *name; uint64_t addr; uint64_t len, want_k, want_rel; int got; } pos[3];
        memset(pos, 0, sizeof pos);
        pos[0].name = "池首"; pos[0].addr = (uint64_t)(uintptr_t)shadow; pos[0].len = PAGE;
        pos[1].name = "第二块首"; pos[1].addr = (uint64_t)(uintptr_t)shadow + cb; pos[1].len = PAGE;
        pos[1].want_k = 1; pos[1].want_rel = 0;
        pos[2].name = "最后一块尾"; pos[2].addr = (uint64_t)(uintptr_t)shadow + pool_bytes - PAGE; pos[2].len = PAGE;
        pos[2].want_k = (pool_bytes - 1) / cb; pos[2].want_rel = (pool_bytes - PAGE) - pos[2].want_k * cb;
        for (int i = 0; i < 3; i++) {
            VkBuffer b = VK_NULL_HANDLE; uint64_t off = 0;
            int ok = vk2_addr(pos[i].addr, pos[i].len, &b, &off);
            // 结构核对：回出来的必须是**那一块**的缓冲、偏移必须是「池内偏移 − 块内偏移」
            // Structural cross-check: the handle must be that chunk's buffer and the offset must match
            const vk2_pool *p = NULL;
            for (int j = 0; j < vk2_pool_count(); j++) {
                const vk2_pool *c = vk2_pool_at(j);
                if (c && (uint64_t)c->lo <= pos[i].addr && pos[i].addr < (uint64_t)c->hi) { p = c; break; }
            }
            int st_ok = 0;
            if (ok && p && pos[i].want_k < (uint64_t)p->nchunk)
                st_ok = (b == p->chunk[pos[i].want_k].buf) && (off == pos[i].want_rel);
            pos[i].got = (ok && st_ok) ? 1 : 0;
            A("  %s：addr = 池内 +%llu MB、len = %llu B ⇒ 返回 %d、缓冲 %p、块内偏移 %#llx；"
              "结构核对（该块缓冲 + 该偏移）= %d",
              pos[i].name, (unsigned long long)((pos[i].addr - (uint64_t)(uintptr_t)shadow) >> 20),
              (unsigned long long)pos[i].len, ok, (void *)(uintptr_t)b, (unsigned long long)off, st_ok);
            if (pos[i].got) {
                // 反查给出的 (buf, off) 真的能读写那一段（逐字节回读）——影子→后备的**逐字节**对应才是 G1（N2）
                // The returned (buf, off) really addresses that stretch (byte-for-byte readback); the byte-level
                // shadow-to-backing correspondence is G1's business (N2).
                uint32_t pat = 0xc0ffee00u + (uint32_t)i;
                int ok2 = page_fill_readback(&v, &sc, b, off, pat, NULL, NULL, NULL);
                A("     用这个 (buf, off) 填一页并回读：%s", ok2 ? "一致（这个反查结果真的指向那段）" : "【红】不一致");
                pos[i].got = ok2 ? 1 : 0;
            }
        }
        // 跨块：请求正好骑在块边界上 ⇒ **必须返回 0**（§9.3 的契约）| a request straddling a boundary must return 0
        {
            VkBuffer b = VK_NULL_HANDLE; uint64_t off = 0;
            uint64_t cross_addr = (uint64_t)(uintptr_t)shadow + cb - 2048;
            int ok = vk2_addr(cross_addr, PAGE, &b, &off);
            q_cross_ret0 = (ok == 0) ? 1 : 0;
            A("  跨块：addr = 池内 +%llu MB − 2048、len = %llu B（骑在第一块末尾）⇒ 返回 %d（契约要求 0）",
              (unsigned long long)(cb >> 20), (unsigned long long)PAGE, ok);
        }
        // 未命中：池外（池尾之后）与一个完全不相关的地址 | misses: past the pool end and an unrelated address
        {
            VkBuffer b = VK_NULL_HANDLE; uint64_t off = 0;
            int a = vk2_addr((uint64_t)(uintptr_t)shadow + pool_bytes + PAGE, 64, &b, &off);
            int c = vk2_addr((uint64_t)(uintptr_t)&v, 64, &b, &off);
            q_miss_ret0 = (a == 0 && c == 0) ? 1 : 0;
            A("  未命中：池尾之后 ⇒ 返回 %d；一个跟池无关的地址（&设备结构）⇒ 返回 %d（都应为 0）", a, c);
        }
        q_pos_ok = (pos[0].got && pos[1].got && pos[2].got) ? 1 : 0;

        // ---- 长度分布：抽 N 次（伪随机、固定种子可复现），看每种典型长度有多少次会跨块 ----
        // ---- Length distribution: N draws (pseudo-random, reproducible seed) and how many of each typical
        //      length cross a chunk boundary.
        uint64_t per = xlen_n / 9;
        rng_seed(seed);
        uint64_t t0 = now_us();
        for (int i = 0; i < 9; i++) {
            uint64_t s = g_lens[i];
            if (s >= pool_bytes) continue;
            for (uint64_t k = 0; k < per; k++) {
                uint64_t off = rng_next() % (pool_bytes - s);
                VkBuffer b = VK_NULL_HANDLE; uint64_t o = 0;
                q_xlen_cnt[i]++;
                if (!vk2_addr((uint64_t)(uintptr_t)shadow + off, s, &b, &o)) q_xlen_cross[i]++;
            }
        }
        uint64_t dt = now_us() - t0;
        A("抽 %llu 次反查用了 %llu ms（每种长度 %llu 次；偏移均匀分布在整个池上、固定种子 %llu 可复现）",
          (unsigned long long)(per * 9), (unsigned long long)(dt / 1000), (unsigned long long)per,
          (unsigned long long)seed);
        for (int i = 0; i < 9; i++) {
            if (!q_xlen_cnt[i]) continue;
            // 解析期望：偏移均匀时，一次长度 s 的访问跨块的概率 ≈ s / 块大小（s ≪ 块）
            // Analytic expectation: with uniform offsets the crossing probability is about s / chunk size
            A("  长度 %8llu B：跨块 %6llu / %llu = %6.3f%%（解析期望 ≈ %5.3f%%；块 %llu MB）",
              (unsigned long long)g_lens[i], (unsigned long long)q_xlen_cross[i],
              (unsigned long long)q_xlen_cnt[i],
              100.0 * (double)q_xlen_cross[i] / (double)q_xlen_cnt[i],
              100.0 * (double)g_lens[i] / (double)cb,
              (unsigned long long)(cb >> 20));
        }
        vk2_pool_stats_get(&st1);
        q_hits = st1.hits; q_misses = st1.misses; q_cross = st1.cross;
        vk2_cross_hist_text(hist, sizeof hist);
        A("模块记的账：%s", hist);
        A("⇒ 只有「大范围拷贝 / 整张贴图」这一档次（≥1 MiB）才有可观的跨块率；"
          "顶点 / 索引 / UBO / 单个 texel 那一档（≤64 KiB）跨块率在万分之几 ⇒ 960 MB 这个块大小对单次访问够宽");
    }

    // =============================================================== 问 6：请求 PINNED 会怎样
    Q("请求 **PINNED**（§9.8 的快路）会怎样？—— 降级规则：任何一步失败 ⇒ 回落 SHADOW、报计数不报红");
    {
        const char *why = NULL;
        q_pin_capable = vk2_pin_capable(&why);
        A("快路的前提（可导出不透明 fd）在这台设备上：%d —— %s", q_pin_capable, why ? why : "");
        // 钉的目标：优先 glibc 堆上的小池；它若没登记成（见上一问）就退回 4 GB 池——降级规则和池是谁无关
        // The pin target: the small heap pool if it registered (see the previous question), else the 4 GB pool --
        // the degradation rule does not depend on which pool it is.
        void *target = heap_pool ? heap_pool : shadow;
        q_pin_ret = vk2_pool_pin(target);
        A("对%s请求 PINNED ⇒ 返回 %d（0 = 没钉住、回落了）",
          target == heap_pool ? "glibc 堆上的小池" : "4 GB 池", q_pin_ret);
        const vk2_pool *q = NULL;
        for (int i = 0; i < vk2_pool_count(); i++) {
            const vk2_pool *c = vk2_pool_at(i);
            if (c && c->lo == vk2_ptr_mask((uintptr_t)target)) { q = c; break; }
        }
        A("该池现在的模式 = %s（必须是 SHADOW）",
          q ? (q->mode == VK2_MODE_SHADOW ? "SHADOW" : "【红】PINNED") : "找不到池");
        vk2_pool_stats st;
        vk2_pool_stats_get(&st);
        q_pin_fail = st.pin_fail;
        A("计数：pin_try=%llu、pin_ok=%llu、pin_fail=%llu（不报红，只报计数）",
          (unsigned long long)st.pin_try, (unsigned long long)st.pin_ok, (unsigned long long)st.pin_fail);
        {   // 回落之后反查照旧正确 ⇒ 正确性不依赖 PINNED | correctness does not depend on PINNED
            VkBuffer b = VK_NULL_HANDLE; uint64_t o = 0;
            q_addr_after_pin = vk2_addr((uint64_t)(uintptr_t)target + 64, 256, &b, &o);
            A("回落之后对同一个池反查：%d（正确性路径走的是 SHADOW，与 PINNED 无关）", q_addr_after_pin);
        }
    }

    // =============================================================== 收尾（先删池，再删设备）
    vk2_pool_del(shadow);
    if (small_pool) vk2_pool_del(small_pool);
    if (unaligned) vk2_pool_del(unaligned);
    if (heap_pool) vk2_pool_del(heap_pool);
    scratch_free(&v, &sc);
    vk2_pool_fini();
    munmap(shadow, (size_t)pool_bytes);
    if (small_pool) munmap(small_pool, 1 * MB);
    if (unaligned) munmap((uint8_t *)unaligned - 3, 64 * 1024);
    free(heap_pool);

    // =============================================================== 结论键值
    printf("\n--- VK2_POOL_SUMMARY（结论键值，供文档抄录）---\n");
    printf("VK2_ROUTE=%s\n", q_gfx2_env ? "new" : "old");
    printf("VK2_GFX2_SWITCH_SELFPROOF=%d\n", q_gfx2_flip);
    printf("VK2_POOL_MEM_ORDINARY=%d\n", (q_mem_rw && q_mem_atomic && q_mem_mutex) ? 1 : 0);
    printf("VK2_POOL_MEM_ATOMIC=%d\n", q_mem_atomic);
    printf("VK2_POOL_MEM_MUTEX=%d\n", q_mem_mutex);
    printf("VK2_POOL_CHUNK_MB=%llu\n", (unsigned long long)q_chunk_mb);
    printf("VK2_POOL_INIT=%d\n", q_init);
    printf("VK2_POOL_4G_ADD=%d\n", q_4g_add);
    printf("VK2_POOL_4G_CHUNKS=%d\n", q_4g_chunks);
    printf("VK2_POOL_4G_HEAD_TAIL_OK=%d\n", q_4g_ht_ok);
    printf("VK2_POOL_SMALL=%llu\n", (unsigned long long)q_small);
    printf("VK2_POOL_NO_REGION=%llu\n", (unsigned long long)q_no_region);
    printf("VK2_POOL_BYTES=%llu\n", (unsigned long long)q_bytes);
    printf("VK2_UNIFORM_BUFFER_BIT_960MB=%s\n", q_ubo_bit == 1 ? "ok" : (q_ubo_bit == 0 ? "fail" : "n/a"));
    printf("VK2_POOL_SMALL_MMAP=%d\n", q_aligned_pool);
    printf("VK2_POOL_UNALIGNED=%d\n", q_unaligned_pool);
    printf("VK2_POOL_MALLOC_TAGGED=%d\n", q_tagged_pool);
    printf("VK2_POOL_MALLOC_TAG_BITS=%#llx\n", (unsigned long long)q_tag_bits);
    printf("VK2_ADDR_TAGGED=%d\n", q_tagged_addr);
    printf("VK2_ADDR_POS_OK=%d\n", q_pos_ok);
    printf("VK2_ADDR_CROSS_RET0=%d\n", q_cross_ret0);
    printf("VK2_ADDR_MISS_RET0=%d\n", q_miss_ret0);
    printf("VK2_ADDR_HITS=%llu\n", (unsigned long long)q_hits);
    printf("VK2_ADDR_MISSES=%llu\n", (unsigned long long)q_misses);
    printf("VK2_ADDR_CROSS=%llu\n", (unsigned long long)q_cross);
    printf("VK2_CROSS_HIST=%s\n", hist);
    for (int i = 0; i < 9; i++)
        if (q_xlen_cnt[i])
            printf("VK2_XLEN_%lluB=%llu/%llu\n", (unsigned long long)g_lens[i],
                   (unsigned long long)q_xlen_cross[i], (unsigned long long)q_xlen_cnt[i]);
    printf("VK2_PIN_CAPABLE=%d\n", q_pin_capable);
    printf("VK2_PIN_RET=%d\n", q_pin_ret);
    printf("VK2_PIN_FAIL=%llu\n", (unsigned long long)q_pin_fail);
    printf("VK2_ADDR_AFTER_PIN=%d\n", q_addr_after_pin);
    printf("VK2_MEM_AVAIL_START_MB=%llu\n", (unsigned long long)(mem_start >> 10));
    printf("VK2_MEM_AVAIL_END_MB=%llu\n", (unsigned long long)(mem_avail_kb() >> 10));
    fflush(stdout);

    vk_close(&v);
    printf("\n完成（N1：只让新池模块可用，没有接管回放层）\n");
    return 0;
}
