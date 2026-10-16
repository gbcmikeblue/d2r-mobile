// 新地基（Vulkan 原生显式实现）N0 的常数标定探针 —— **只问不接管**。
//
// 它回答的唯一一件事：`docs/rewrite/ANDROID_REWRITE.zh-CN.md` §9.9 那四个"待实测"的常数，在这台设备上
// 各是多少。它**不**创建任何池、**不**写脏记账、**不**接管回放层 —— 那些是 N1~N3（`vk2_pool` / `vk2_dirty` /
// `vk2_xfer` / `vk2_sub`）。本文件与 `dm/android/vk2/` 里其它文件无关，是个独立可执行文件。
//
// 四个常数怎么问（每个都给真机读数，不许估）：
//   ① 块大小：先问 `maxStorageBufferRange`；再从大到小问"能**全功能**建出（建 + 分配 + 绑定 + 真用一下）
//      带 `STORAGE_BUFFER` 的 `VkBuffer` 的最大尺寸"，取那个最大值**再减一档**（已知 4 GB 带 STORAGE 建不出来）。
//      一档 = `DM_VK2_NOTCH_MB`（缺省 64 MiB）。**一档一个进程**：某档失败后不再在同一进程里量小尺寸
//      （AHB 探针踩过"一次失败会把同进程之后的小分配也带坏"，见 `.work/probes.py` 里那段说明）。
//   ② 脏位图粒度：4 KiB 起步；把"位图多大、扫一遍多贵、全脏时几个 run / 几条拷贝"记成机制读数，
//      并给出 4 / 16 / 64 KiB 三档的对照（`每帧脏 run 数` 与 `每帧搬运字节数` 两个读数要等 N2 接上游戏路径才有，
//      本探针只记机制上界，读数表里留槽）。
//   ③ `VK2_INFLIGHT`：3 起步；先记下"提交一次的开销"——一次 `vkQueueSubmit` 装 N 条命令的开销，
//      以及"有界在飞深度 D"下平均每次提交的开销 + 栅栏超时行为（N3 要"带超时地等栅栏"）。
//   ④ 暂存环大小：量"把一池全搬一遍"的字节数与耗时（= 每帧搬运量的上界估计），并给出环大小按 2~3 倍取时的档位。
//
// 自证格式（旧路踩过"一个初始值错、工具一次都没跑却看着像有效"）：每个问题都打
//   `[问 n] 问题…` + `[答] 答案…`；一档一进程的尺寸问询由父进程打 `[问]`、子进程打 `[答]`。
// 末尾一段 `--- VK2_PROBE_SUMMARY ---` 是关键结论键值（KEY=VALUE），供文档与 N1 直接抄。
//
// 跑法（一律走项目内 `.venv` Python，见 `.codebuddy/rules/windows-python-only/RULE.mdc`）：
//   & "d:/yiyi/d2r-mobile/.venv/Scripts/python.exe" -X utf8 .work/probes.py build vk2_probe
//   & "d:/yiyi/d2r-mobile/.venv/Scripts/python.exe" -X utf8 .work/probes.py run vk2_probe --serial 8b1f1e50
// 读数落 `.work/logs/probe-<串号>-vk2_probe[-tag].txt`（`--tag` 不覆盖旧读数；`--env "…"` 透传下面这些开关）。
//
// 开关（全部缺省值 = 文档里的起点值，改了要重新标定）：
//   DM_VK2_SKIP_LADDER=1      跳过块大小阶梯（用 DM_VK2_CHUNK_MB 当假设值）
//   DM_VK2_CHUNK_MB=<MB>      假定的块大小（跳过阶梯时才用；缺省 1024）
//   DM_VK2_NOTCH_MB=<MB>      一档 = 多少 MB（缺省 64）
//   DM_VK2_POOL_MB=<MB>       客体池大小（缺省 4096 = 4 GB，与宿主划分一致）
//   DM_VK2_STAGE_KB=<KiB>     暂存环大小（缺省 16384 = 16 MiB，与旧路 DM_UP_RING 同一档）
//   DM_VK2_XFER_MB=<MB>       搬运总字节（缺省 64；"一池全搬一遍"那一段固定按池大小算）
//   DM_VK2_SUBMITS=<n>        提交开销测试的重复次数（缺省 200）
//   DM_VK2_ONLY=<MB>          内部用：父进程拿它派生子进程（不给用户用；等价于 `vk2_probe size <MB>`）
//
// ------------------------------------------------------------------------------------------------
// vk2_probe.c -- the N0 constant-calibration probe of the new foundation (Vulkan native, explicit).
// ASK ONLY, TAKE OVER NOTHING. It answers exactly one thing: what the four "to be measured" constants of
// sections 9.9 of docs/rewrite/ANDROID_REWRITE.zh-CN.md are on this device (chunk size, dirty-bitmap
// granularity, VK2_INFLIGHT, staging ring). It creates no pool, writes no dirty accounting and does not
// take over the replay layer -- those are N1..N3. Every question prints "[问 n] question" and
// "[答] answer" so the run can self-prove that it really asked, and the run ends with a KEY=VALUE
// summary block. The chunk-size ladder runs **one size per process** (a failure can poison later small
// allocations in the same process, as the AHB probe learned) via posix_spawn of this same binary.
#define _GNU_SOURCE 1
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <spawn.h>
#include <sys/wait.h>
#include <vulkan/vulkan.h>

extern char **environ;

#define MB (1024ull * 1024ull)
#define GB (1024ull * MB)

// "全功能"的用法位：块要做 SSBO（STORAGE_BUFFER）、要能上传与回读（TRANSFER_*）、
// 还要能当顶点/索引/间接缓冲（几何常常建在池里）。**故意不含 UNIFORM_BUFFER**：
// 它的描述符范围另有 64 KB 级别的上限（maxUniformBufferRange），混进来会把"块大小"这个读数弄脏。
// The "full-featured" usage set: the chunk must be an SSBO, must upload/read back, and must be usable as
// vertex/index/indirect data. UNIFORM_BUFFER is deliberately left out: it has its own (64 KB-ish) range
// limit, which would muddy the chunk-size reading.
#define FULL_USAGE (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | \
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | \
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)
#define MIN_USAGE  (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)

// ---------------------------------------------------------------- 通用小工具
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
static long pagesize(void) { long p = sysconf(_SC_PAGESIZE); return p > 0 ? p : 4096; }

// /proc/meminfo 的 MemAvailable（KB）：大分配前后各读一次，判断"要不要得到"与"真占了没有"
// MemAvailable from /proc/meminfo (KB): read before/after big allocations to tell "can we get it" from "did we really take it"
static unsigned long long mem_avail_kb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256]; unsigned long long kb = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
    }
    fclose(f);
    return kb;
}

// ---------------------------------------------------------------- 设备
typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue q;
    uint32_t qfi;
    VkPhysicalDeviceMemoryProperties mp;
    VkPhysicalDeviceProperties2 p2;
    VkPhysicalDeviceMaintenance3Properties m3;
    VkPhysicalDeviceDriverProperties drv;
} vk_t;

static int vk_open(vk_t *v, int with_device) {
    memset(v, 0, sizeof *v);
    VkApplicationInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "dm-vk2-probe";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    if (vkCreateInstance(&ici, NULL, &v->inst) != VK_SUCCESS) return 0;

    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(v->inst, &n, NULL) != VK_SUCCESS || n == 0) return 0;
    VkPhysicalDevice *dv = calloc(n, sizeof *dv);
    if (vkEnumeratePhysicalDevices(v->inst, &n, dv) != VK_SUCCESS) { free(dv); return 0; }
    v->phys = dv[0];   // 这台机上只有一个物理设备 | there is only one physical device on this device
    free(dv);

    v->m3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
    v->drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    v->m3.pNext = NULL;
    v->drv.pNext = &v->m3;
    v->p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    v->p2.pNext = &v->drv;
    vkGetPhysicalDeviceProperties2(v->phys, &v->p2);
    vkGetPhysicalDeviceMemoryProperties(v->phys, &v->mp);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq ? nq : 1, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &nq, qf);
    v->qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { v->qfi = i; break; }
    free(qf);
    if (v->qfi == UINT32_MAX) return 0;
    if (!with_device) return 1;

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
static void vk_close(vk_t *v) {
    if (v->dev) vkDestroyDevice(v->dev, NULL);
    if (v->inst) vkDestroyInstance(v->inst, NULL);
    v->dev = VK_NULL_HANDLE; v->inst = VK_NULL_HANDLE;
}

// 按需求位 + 避开位挑一个内存类型；并列时选堆最大的那个
// Pick a memory type by required/avoided property bits, preferring the largest heap on a tie
static uint32_t find_type(const vk_t *v, uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags avoid) {
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

// ---------------------------------------------------------------- ① 块大小（单档，一个进程）
typedef struct {
    int create_full, create_min, alloc, bind, use, verify;
    int32_t rc, ra, rb, rw;
    uint64_t align, fence_us;
} size_res;

static void test_size(const vk_t *v, uint64_t size, size_res *r) {
    memset(r, 0, sizeof *r);
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof bci);
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = FULL_USAGE;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    r->rc = (int32_t)vkCreateBuffer(v->dev, &bci, NULL, &buf);
    if (r->rc == VK_SUCCESS) r->create_full = 1;
    else {
        bci.usage = MIN_USAGE;   // 顺带问一句：是不是那几个额外用法位挡的？| also ask: was it the extra usage bits?
        r->rc = (int32_t)vkCreateBuffer(v->dev, &bci, NULL, &buf);
        if (r->rc != VK_SUCCESS) return;
        r->create_min = 1;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(v->dev, buf, &mr);
    r->align = (uint64_t)mr.alignment;
    uint32_t mi = find_type(v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (mi == UINT32_MAX) mi = find_type(v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (mi == UINT32_MAX) { vkDestroyBuffer(v->dev, buf, NULL); return; }

    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof mai);
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mi;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    r->ra = (int32_t)vkAllocateMemory(v->dev, &mai, NULL, &mem);
    if (r->ra != VK_SUCCESS) { vkDestroyBuffer(v->dev, buf, NULL); return; }
    r->alloc = 1;
    r->rb = (int32_t)vkBindBufferMemory(v->dev, buf, mem, 0);
    if (r->rb != VK_SUCCESS) {
        vkFreeMemory(v->dev, mem, NULL); vkDestroyBuffer(v->dev, buf, NULL); return;
    }
    r->bind = 1;

    // "真用一下"：四点填充 + 把**尾页**回读出来逐字节比 —— 大缓冲最后那 4 KB 到底能不能碰，
    // 是"建出来了"和"能用"之间的差别。| Really use it: fill four spots and read the **last page** back
    // byte for byte -- whether the final 4 KB of a giant buffer is really addressable is the difference
    // between "it was created" and "it works".
    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = v->qfi;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(v->dev, &pci, NULL, &pool) != VK_SUCCESS) goto out_buf;
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof cbai);
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(v->dev, &cbai, &cb) != VK_SUCCESS) goto out_pool;
    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof fci);
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(v->dev, &fci, NULL, &fence) != VK_SUCCESS) goto out_cb;

    {   // 回读用的小缓冲（host 可见 + 一致）| the small readback buffer (host visible and coherent)
        VkBufferCreateInfo sci;
        memset(&sci, 0, sizeof sci);
        sci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        sci.size = 4096;
        sci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        sci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer sb = VK_NULL_HANDLE;
        if (vkCreateBuffer(v->dev, &sci, NULL, &sb) != VK_SUCCESS) goto out_fence;
        VkMemoryRequirements smr;
        vkGetBufferMemoryRequirements(v->dev, sb, &smr);
        uint32_t smi = find_type(v, smr.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        VkMemoryAllocateInfo smai;
        memset(&smai, 0, sizeof smai);
        smai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        smai.allocationSize = smr.size; smai.memoryTypeIndex = smi;
        VkDeviceMemory smem = VK_NULL_HANDLE;
        void *sp = NULL;
        int ok = (smi != UINT32_MAX) &&
                 vkAllocateMemory(v->dev, &smai, NULL, &smem) == VK_SUCCESS &&
                 vkBindBufferMemory(v->dev, sb, smem, 0) == VK_SUCCESS &&
                 vkMapMemory(v->dev, smem, 0, 4096, 0, &sp) == VK_SUCCESS;
        if (!ok) {
            if (smem) { vkUnmapMemory(v->dev, smem); vkFreeMemory(v->dev, smem, NULL); }
            vkDestroyBuffer(v->dev, sb, NULL);
            goto out_fence;
        }
        VkCommandBufferBeginInfo bi;
        memset(&bi, 0, sizeof bi);
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        uint32_t pat = 0x01020304u;
        int okc = vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS;
        // 四个位置：头、1/4、1/2、尾（尾那一页要回读比对）| four spots: head, 1/4, 1/2, tail
        VkDeviceSize spots[4];
        spots[0] = 0; spots[1] = size / 4; spots[2] = size / 2; spots[3] = size - 4096;
        for (int i = 0; i < 4 && okc; i++) vkCmdFillBuffer(cb, buf, spots[i], 4096, pat + (uint32_t)i);
        if (okc) {
            VkBufferMemoryBarrier bb;
            memset(&bb, 0, sizeof bb);
            bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.buffer = buf; bb.offset = 0; bb.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, NULL, 1, &bb, 0, NULL);
            VkBufferCopy cp;
            memset(&cp, 0, sizeof cp);
            cp.srcOffset = spots[3]; cp.dstOffset = 0; cp.size = 4096;
            vkCmdCopyBuffer(cb, buf, sb, 1, &cp);
            okc = vkEndCommandBuffer(cb) == VK_SUCCESS;
        }
        if (okc) {
            VkSubmitInfo si;
            memset(&si, 0, sizeof si);
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            r->rw = (int32_t)vkQueueSubmit(v->q, 1, &si, fence);
            if (r->rw == VK_SUCCESS) {
                uint64_t t0 = now_us();
                VkResult wr = vkWaitForFences(v->dev, 1, &fence, VK_TRUE, 5000ull * 1000000ull);
                r->fence_us = now_us() - t0;
                if (wr == VK_SUCCESS) {
                    r->use = 1;
                    const uint8_t *b = (const uint8_t *)sp;
                    uint32_t want = pat + 3;
                    int same = 1;
                    for (int i = 0; i < 4096; i++) {
                        uint8_t e = (uint8_t)((want >> ((i & 3) * 8)) & 0xff);
                        if (b[i] != e) { same = 0; break; }
                    }
                    r->verify = same;
                } else {
                    r->rw = (int32_t)wr;
                }
            }
        }
        vkUnmapMemory(v->dev, smem);
        vkFreeMemory(v->dev, smem, NULL);
        vkDestroyBuffer(v->dev, sb, NULL);
    }
out_fence:
    vkDestroyFence(v->dev, fence, NULL);
out_cb:
    vkFreeCommandBuffers(v->dev, pool, 1, &cb);
out_pool:
    vkDestroyCommandPool(v->dev, pool, NULL);
out_buf:
    vkFreeMemory(v->dev, mem, NULL);
    vkDestroyBuffer(v->dev, buf, NULL);
}

// 子进程模式：只量一档，打一行 `RESULT …` 给父进程解析，然后退出。
// Child mode: measure exactly one size, print one machine-readable `RESULT ...` line for the parent.
static int mode_size(uint64_t mb) {
    uint64_t size = mb * MB;
    vk_t v;
    if (!vk_open(&v, 1)) { printf("[答] 设备打开失败\n"); return 2; }
    printf("[答] 尺寸 %llu MB：maxStorageBufferRange = %llu MB，maxMemoryAllocationSize = %llu MB\n",
           (unsigned long long)mb,
           (unsigned long long)(v.p2.properties.limits.maxStorageBufferRange >> 20),
           (unsigned long long)(v.m3.maxMemoryAllocationSize >> 20));
    size_res r;
    uint64_t before = mem_avail_kb();
    test_size(&v, size, &r);
    uint64_t after = mem_avail_kb();
    const char *cn = r.create_full ? "full" : (r.create_min ? "min" : "none");
    printf("[答] 建=%s(rc=%d) 分配=%d(rc=%d) 绑定=%d(rc=%d) 用之=%d(rc=%d) 尾页回读一致=%d 对齐=%llu 栅栏=%llu us\n",
           cn, (int)r.rc, r.alloc, (int)r.ra, r.bind, (int)r.rb, r.use, (int)r.rw, r.verify,
           (unsigned long long)r.align, (unsigned long long)r.fence_us);
    printf("[答] MemAvailable %llu MB → %llu MB（差 %lld MB）\n",
           (unsigned long long)(before >> 10), (unsigned long long)(after >> 10),
           (long long)((long long)(before >> 10) - (long long)(after >> 10)));
    printf("RESULT size_mb=%llu create=%s alloc=%d bind=%d use=%d verify=%d "
           "rc=%d ra=%d rb=%d align=%llu fence_us=%llu\n",
           (unsigned long long)mb, cn, r.alloc, r.bind, r.use, r.verify,
           (int)r.rc, (int)r.ra, (int)r.rb,
           (unsigned long long)r.align, (unsigned long long)r.fence_us);
    fflush(stdout);
    vk_close(&v);
    return 0;
}

typedef struct {
    uint64_t mb;
    int create_full, create_min, alloc, bind, use, verify;
    int32_t rc;
    uint64_t align, fence_us;
} ladder_res;

// 跑一档（一个进程），把子进程的输出原样打出来，并从 `RESULT` 行里取回结论
// Run one rung (one process), echo the child's output verbatim and parse its `RESULT` line
static int ladder_run(uint64_t mb, ladder_res *out) {
    memset(out, 0, sizeof *out);
    out->mb = mb;
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) { fprintf(stderr, "【红】读不到 /proc/self/exe\n"); return -1; }
    self[n] = 0;
    char sz[32];
    snprintf(sz, sizeof sz, "%llu", (unsigned long long)mb);
    char *argv[] = { self, (char *)"size", sz, NULL };
    int p[2];
    if (pipe(p) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_addclose(&fa, p[0]);
    posix_spawn_file_actions_addclose(&fa, p[1]);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, self, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (rc != 0) { close(p[0]); fprintf(stderr, "【红】posix_spawn 失败：%d\n", rc); return -1; }
    static char buf[16384];
    size_t used = 0;
    for (;;) {
        ssize_t got = read(p[0], buf + used, sizeof buf - 1 - used);
        if (got <= 0) break;
        used += (size_t)got;
        if (used >= sizeof buf - 1) break;
    }
    buf[used] = 0;
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    // 先解析、后分词：strtok_r 会把 '\n' 改成 '\0'，一改完 strstr 就再也看不到后面的行了
    // （第一版就是这么错的：子进程明明打了 RESULT，父进程却报"没打"——探针同样要自证）
    // Parse first, tokenise second: strtok_r turns '\n' into '\0', after which strstr can no longer see any
    // later line. (The first version failed exactly here: the child did print RESULT but the parent said it
    // did not -- so the probe has to self-prove too.)
    char *res = strstr(buf, "RESULT ");
    int parse_ok = 0;
    unsigned long long v_align = 0, v_fence = 0;
    char v_create[16] = "";
    if (res) {
        unsigned long long v_mb = 0;
        int v_alloc = 0, v_bind = 0, v_use = 0, v_verify = 0, v_rc = 0;
        if (sscanf(res, "RESULT size_mb=%llu create=%15s alloc=%d bind=%d use=%d verify=%d rc=%d",
                   &v_mb, v_create, &v_alloc, &v_bind, &v_use, &v_verify, &v_rc) == 7) {
            char *p2 = strstr(res, "align=");
            if (p2) sscanf(p2, "align=%llu", &v_align);
            p2 = strstr(res, "fence_us=");
            if (p2) sscanf(p2, "fence_us=%llu", &v_fence);
            out->create_full = (strcmp(v_create, "full") == 0);
            out->create_min = (strcmp(v_create, "min") == 0);
            out->alloc = v_alloc; out->bind = v_bind; out->use = v_use; out->verify = v_verify;
            out->rc = v_rc; out->align = v_align; out->fence_us = v_fence;
            parse_ok = 1;
        }
    }
    printf("  --- 子进程（%llu MB）---\n", (unsigned long long)mb);
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        printf("  | %s\n", line);
    if (WIFSIGNALED(st)) {
        printf("  【红】子进程被信号 %d 打死\n", WTERMSIG(st));
        return -2;
    }
    if (!res) { printf("  【红】子进程没打 RESULT 行（rc=%d，读到 %zu 字节）\n",
                       WIFEXITED(st) ? WEXITSTATUS(st) : -1, used); return -1; }
    if (!parse_ok) { printf("  【红】RESULT 行解析失败：%s\n", res); return -1; }
    return 0;
}
static int ladder_ok(const ladder_res *r) {
    return r->create_full && r->alloc && r->bind && r->use && r->verify;
}

// 一档 = 向下取整到档位网格 | one notch = floor to the notch grid
static uint64_t notch_floor(uint64_t mb, uint64_t notch) {
    uint64_t q = mb / notch;
    return q * notch;
}

// ---------------------------------------------------------------- ⑤ 脏位图（机制读数）
typedef struct { uint8_t *bits; uint64_t n; } bm;
static void bm_init(bm *b, uint64_t pages) {
    b->n = pages;
    b->bits = calloc((pages + 7) / 8, 1);
}
static void bm_clear(bm *b) { memset(b->bits, 0, (b->n + 7) / 8); }
static void bm_set(bm *b, uint64_t i) { b->bits[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static int bm_get(const bm *b, uint64_t i) { return (b->bits[i >> 3] >> (i & 7)) & 1; }
// 扫一遍并把相邻的脏页并成 run：返回 run 数，*bytes 是脏字节数（= 会真正搬运的量）
// Sweep once and merge adjacent dirty pages into runs: returns the run count, *bytes the dirty bytes
static uint64_t bm_runs(const bm *b, uint64_t *bytes) {
    uint64_t runs = 0, npages = 0; int prev = 0;
    for (uint64_t i = 0; i < b->n; i++) {
        int c = bm_get(b, i);
        if (c) { if (!prev) runs++; npages++; }
        prev = c;
    }
    *bytes = npages;
    return runs;
}

// ---------------------------------------------------------------- ③ 提交开销
static uint64_t submit_bench(vk_t *v, VkCommandBuffer *cbs, uint32_t ncbs, uint32_t submits, int inflight,
                             uint64_t *fence_wait_us) {
    VkFence fences[16];
    uint32_t nf = (uint32_t)(inflight > 0 ? inflight : 1);
    if (nf > 16) nf = 16;
    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof fci);
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (uint32_t i = 0; i < nf; i++) vkCreateFence(v->dev, &fci, NULL, &fences[i]);
    uint64_t t_wait = 0, t0 = now_us();
    for (uint32_t i = 0; i < submits; i++) {
        uint32_t slot = i % nf;
        if (i >= nf) {
            uint64_t w0 = now_us();
            vkWaitForFences(v->dev, 1, &fences[slot], VK_TRUE, 5000ull * 1000000ull);
            t_wait += now_us() - w0;
        }
        vkResetFences(v->dev, 1, &fences[slot]);
        VkSubmitInfo si;
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = ncbs;
        si.pCommandBuffers = cbs;   // 每次提交同样的前 ncbs 条（里面是空的，量的就是提交本身）
        vkQueueSubmit(v->q, 1, &si, fences[slot]);
    }
    for (uint32_t i = 0; i < nf; i++) vkWaitForFences(v->dev, 1, &fences[i], VK_TRUE, 5000ull * 1000000ull);
    uint64_t total = now_us() - t0;
    for (uint32_t i = 0; i < nf; i++) vkDestroyFence(v->dev, fences[i], NULL);
    if (fence_wait_us) *fence_wait_us = t_wait;
    return total;   // 总微秒；调用方除以 submits | total microseconds; the caller divides by submits
}

// ---------------------------------------------------------------- 主流程
int main(int argc, char **argv) {
    // 内部子进程模式（父进程派生出来的）| internal child mode, spawned by the parent
    if (argc >= 3 && strcmp(argv[1], "size") == 0) return mode_size(strtoull(argv[2], NULL, 0));
    if (getenv("DM_VK2_ONLY")) return mode_size(strtoull(getenv("DM_VK2_ONLY"), NULL, 0));

    printf("=== vk2_probe：新地基 N0 常数标定（只问不接管）===\n");
    printf("构建：%s %s；pid=%d；页大小=%ld 字节\n", __DATE__, __TIME__, (int)getpid(), pagesize());
    printf("MemAvailable 起始 %llu MB\n", (unsigned long long)(mem_avail_kb() >> 10));

    uint64_t pool_mb = env_u64("DM_VK2_POOL_MB", 4096);
    uint64_t stage_kb = env_u64("DM_VK2_STAGE_KB", 16384);
    uint64_t notch = env_u64("DM_VK2_NOTCH_MB", 64);
    uint64_t xfer_mb = env_u64("DM_VK2_XFER_MB", 64);
    uint32_t submits = (uint32_t)env_u64("DM_VK2_SUBMITS", 200);
    int skip_ladder = env_u64("DM_VK2_SKIP_LADDER", 0) != 0;
    uint64_t chunk_mb_hint = env_u64("DM_VK2_CHUNK_MB", 1024);

    vk_t v;
    if (!vk_open(&v, 0)) { fprintf(stderr, "【红】Vulkan 打不开（没有实例或没有图形队列）\n"); return 2; }

    // ---- 问 1：设备与各项上限 | Q1: the device and its relevant limits ----
    Q("这台设备是谁，和「块大小」有关的各项上限是多少？");
    A("设备 %s；apiVersion %u.%u.%u；驱动 %s %u（vendor 0x%04x device 0x%04x，driverID %u/%u）",
      v.p2.properties.deviceName,
      VK_VERSION_MAJOR(v.p2.properties.apiVersion), VK_VERSION_MINOR(v.p2.properties.apiVersion),
      VK_VERSION_PATCH(v.p2.properties.apiVersion),
      v.drv.driverName, (unsigned)v.p2.properties.driverVersion,
      (unsigned)v.p2.properties.vendorID, (unsigned)v.p2.properties.deviceID,
      (unsigned)v.drv.driverID, (unsigned)v.drv.conformanceVersion.major);
    A("maxStorageBufferRange = %llu 字节（%llu MB）",
      (unsigned long long)v.p2.properties.limits.maxStorageBufferRange,
      (unsigned long long)(v.p2.properties.limits.maxStorageBufferRange >> 20));
    A("maxMemoryAllocationSize = %llu 字节（%llu MB）；maxMemoryAllocationCount = %u",
      (unsigned long long)v.m3.maxMemoryAllocationSize,
      (unsigned long long)(v.m3.maxMemoryAllocationSize >> 20),
      (unsigned)v.p2.properties.limits.maxMemoryAllocationCount);
    A("bufferImageGranularity = %llu；nonCoherentAtomSize = %llu",
      (unsigned long long)v.p2.properties.limits.bufferImageGranularity,
      (unsigned long long)v.p2.properties.limits.nonCoherentAtomSize);
    A("minStorageBufferOffsetAlignment = %llu；minTexelBufferOffsetAlignment = %llu",
      (unsigned long long)v.p2.properties.limits.minStorageBufferOffsetAlignment,
      (unsigned long long)v.p2.properties.limits.minTexelBufferOffsetAlignment);
    A("optimalBufferCopyOffsetAlignment = %llu；optimalBufferCopyRowPitchAlignment = %llu",
      (unsigned long long)v.p2.properties.limits.optimalBufferCopyOffsetAlignment,
      (unsigned long long)v.p2.properties.limits.optimalBufferCopyRowPitchAlignment);
    for (uint32_t i = 0; i < v.mp.memoryHeapCount; i++)
        A("堆 %u：%llu MB（%s）", i, (unsigned long long)(v.mp.memoryHeaps[i].size >> 20),
          (v.mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL" : "host");
    for (uint32_t i = 0; i < v.mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = v.mp.memoryTypes[i].propertyFlags;
        A("类型 %u：堆 %u；%s%s%s%s%s", i, v.mp.memoryTypes[i].heapIndex,
          (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL " : "",
          (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "HOST_VISIBLE " : "",
          (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "HOST_COHERENT " : "",
          (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "HOST_CACHED " : "",
          (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? "LAZILY_ALLOCATED " : "");
    }

    // ---- 问 2：块大小阶梯 | Q2: the chunk-size ladder ----
    uint64_t chunk_max_mb = 0, chunk_mb = 0, cap_mb_reported = 0;
    Q("能**全功能**建出（建 + 分配 + 绑定 + 四点填充 + 尾页回读一致）带 STORAGE_BUFFER 的 VkBuffer 的最大尺寸是多少？"
      "（一档一进程：某档失败后不在同进程里继续量小尺寸）");
    if (skip_ladder) {
        chunk_max_mb = chunk_mb_hint;
        chunk_mb = notch_floor(chunk_mb_hint, notch) - notch;
        A("按 DM_VK2_SKIP_LADDER=1 跳过阶梯：这里**没有**真机读数，直接用假设值 %llu MB",
          (unsigned long long)chunk_mb_hint);
    } else {
        // 两个已公布的上限里更小的那个是"纸面上"的上限：块大小不可能超过它。
        // 阶梯的最上面一档故意放在它**之上一档** —— 只有量到"上面那档失败、它成功"才算**证明**了上限，
        // 否则报出来的"最大值"只是我们自己阶梯的顶端（第一版就吃不到这个证明：cap 是 1024 MB，
        // 阶梯顶端也是 1024 MB，等于没证明）。
        // The smaller of the two published limits is the paper ceiling for a chunk. The top rung sits one
        // notch **above** it on purpose: only "one notch above fails, the ceiling succeeds" proves the
        // ceiling; otherwise the reported maximum is merely the top of our own ladder.
        uint64_t cap = (v.p2.properties.limits.maxStorageBufferRange < v.m3.maxMemoryAllocationSize
                        ? v.p2.properties.limits.maxStorageBufferRange
                        : v.m3.maxMemoryAllocationSize) >> 20;
        uint64_t capf = notch_floor(cap, notch);
        cap_mb_reported = cap;
        uint64_t coarse[8];
        uint32_t nc = 0;
        coarse[nc++] = capf + notch;      // 纸面上限之上一档：这一档必须失败，上限定得才算数 | must fail: it is the proof
        if (capf > 0) coarse[nc++] = capf;
        uint64_t pw[] = { 4096, 2048, 1024, 512, 256 };
        for (unsigned i = 0; i < sizeof pw / sizeof pw[0] && nc < 8; i++)
            if (pw[i] < capf) coarse[nc++] = pw[i];
        A("纸面上限 = min(maxStorageBufferRange %llu MB, maxMemoryAllocationSize %llu MB) = **%llu MB**；"
          "阶梯从它上面一档（%llu MB）开始往下走",
          (unsigned long long)(v.p2.properties.limits.maxStorageBufferRange >> 20),
          (unsigned long long)(v.m3.maxMemoryAllocationSize >> 20),
          (unsigned long long)cap, (unsigned long long)(capf + notch));
        uint64_t good = 0, bad = 0;
        for (uint32_t i = 0; i < nc; i++) {
            ladder_res r;
            if (ladder_run(coarse[i], &r) != 0) {
                A("粗档 %llu MB：子进程异常，粗档定位停在这里", (unsigned long long)coarse[i]);
                break;
            }
            if (ladder_ok(&r)) { good = coarse[i]; break; }
            bad = coarse[i];
        }
        if (!good) {
            A("粗档全部不通过（最大只到 %llu MB）——块大小这一项量不出来", (unsigned long long)bad);
        } else {
            A("粗档定位：%llu MB 能全功能建出%s", (unsigned long long)good,
              bad ? "；上一档失败" : "（已到阶梯顶端）");
            if (bad) {
                // 二分收窄到一档（档位网格 = notch）：每步要么把下界抬高 ≥ notch，要么把上界压到 notch 之内
                // Binary-search down to one notch; every step either raises the lower bound by >= notch or
                // squeezes the upper bound to within one notch, so it terminates
                uint64_t lo = good, hi = bad;
                while (hi - lo > notch) {
                    uint64_t mid = notch_floor(lo + (hi - lo) / 2, notch);
                    if (mid <= lo) mid = lo + notch;
                    if (mid >= hi) break;
                    ladder_res r;
                    if (ladder_run(mid, &r) != 0) break;
                    if (ladder_ok(&r)) lo = mid; else hi = mid;
                }
                good = lo;
            }
            chunk_max_mb = good;
            uint64_t fl = notch_floor(good, notch);
            chunk_mb = fl > notch ? fl - notch : fl;
            if (bad) {
                A("上限已被证明：%llu MB 失败、%llu MB 成功 ⇒ 全功能能建出的最大值 = **%llu MB**"
                  "（对照纸面上限 %llu MB）",
                  (unsigned long long)bad, (unsigned long long)good,
                  (unsigned long long)chunk_max_mb, (unsigned long long)cap_mb_reported);
            } else {
                A("【黄】阶梯最上面那一档（= 纸面上限之上一档）也过了 ⇒ 这里的最大值 %llu MB 只是阶梯顶端，"
                  "**上限没有被证明**（要更准就得把阶梯再往上抬）", (unsigned long long)chunk_max_mb);
            }
            A("⇒ **再减一档**（档位 %llu MB）= **%llu MB**，块大小取这一档",
              (unsigned long long)notch, (unsigned long long)chunk_mb);
        }
    }

    // ---- 剩下的问题都要一个真设备 | the rest needs a real VkDevice ----
    vk_close(&v);
    if (!vk_open(&v, 1)) { fprintf(stderr, "【红】建设备失败\n"); return 2; }

    // ---- 问 3：暂存环 / 一池全搬一遍 | Q3: staging ring, one full-pool pass ----
    uint64_t stage = stage_kb * 1024ull;
    uint64_t pool_ms = 0, pool_copies = 0, small_ms = 0, small_copies = 0;
    uint64_t pool_bytes_done = 0, pool_done_copies = 0;
    int pool_failed = 0;
    int32_t pool_fail_code = 0;
    double pool_mbps = 0.0;
    Q("暂存环能怎么用：把一池全搬一遍要多少字节、多长时间？（这就是「每帧搬运量」的上界估计）");
    {
        VkBufferCreateInfo sbi;
        memset(&sbi, 0, sizeof sbi);
        sbi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        sbi.size = stage;
        sbi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        sbi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer sbuf = VK_NULL_HANDLE;
        if (vkCreateBuffer(v.dev, &sbi, NULL, &sbuf) != VK_SUCCESS) {
            A("【黄】暂存缓冲建不出来（%llu KiB）", (unsigned long long)stage_kb);
        } else {
            VkMemoryRequirements smr;
            vkGetBufferMemoryRequirements(v.dev, sbuf, &smr);
            uint32_t smi = find_type(&v, smr.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
            VkMemoryAllocateInfo smai;
            memset(&smai, 0, sizeof smai);
            smai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            smai.allocationSize = smr.size; smai.memoryTypeIndex = smi;
            VkDeviceMemory smem = VK_NULL_HANDLE;
            void *sp = NULL;
            int ok = (smi != UINT32_MAX) && vkAllocateMemory(v.dev, &smai, NULL, &smem) == VK_SUCCESS &&
                     vkBindBufferMemory(v.dev, sbuf, smem, 0) == VK_SUCCESS &&
                     vkMapMemory(v.dev, smem, 0, stage, 0, &sp) == VK_SUCCESS;
            if (!ok) { A("【黄】暂存缓冲分配/映射失败"); }
            else {
                memset(sp, 0x5a, (size_t)stage);
                uint64_t dst_mb = chunk_max_mb ? chunk_max_mb : chunk_mb_hint;
                if (dst_mb * MB < stage * 2) dst_mb = (stage * 4) / MB + 1;
                VkDeviceSize dstsz = dst_mb * MB;
                VkBufferCreateInfo dbi;
                memset(&dbi, 0, sizeof dbi);
                dbi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                dbi.size = dstsz;
                dbi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                dbi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                VkBuffer dbuf = VK_NULL_HANDLE;
                VkDeviceMemory dmem = VK_NULL_HANDLE;
                int dok = (vkCreateBuffer(v.dev, &dbi, NULL, &dbuf) == VK_SUCCESS);
                if (dok) {
                    VkMemoryRequirements dmr;
                    vkGetBufferMemoryRequirements(v.dev, dbuf, &dmr);
                    uint32_t dmi = find_type(&v, dmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
                    if (dmi == UINT32_MAX) dmi = find_type(&v, dmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
                    VkMemoryAllocateInfo dmai;
                    memset(&dmai, 0, sizeof dmai);
                    dmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                    dmai.allocationSize = dmr.size; dmai.memoryTypeIndex = dmi;
                    dok = (dmi != UINT32_MAX) && vkAllocateMemory(v.dev, &dmai, NULL, &dmem) == VK_SUCCESS &&
                          vkBindBufferMemory(v.dev, dbuf, dmem, 0) == VK_SUCCESS;
                }
                if (!dok) { A("【黄】目的地块建不出来（%llu MB），搬运这一段跳过", (unsigned long long)dst_mb); }
                else {
                    // 一次搬运 = 一条 vkCmdCopyBuffer；提交用有界在飞（深度 3，与 VK2_INFLIGHT 的起点一致）
                    // One transfer = one vkCmdCopyBuffer; submits use bounded in-flight (depth 3, the VK2_INFLIGHT start)
                    const int depth = 3;
                    VkCommandPoolCreateInfo pci;
                    memset(&pci, 0, sizeof pci);
                    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                    // RESET_COMMAND_BUFFER 是必须的：这几个 cb 要被**反复重录**。池上没有这个标志时
                    // vkBeginCommandBuffer 只允许用在"初始态"的 cb 上，重录属未定义行为
                    // （第一版就是在这上面栽的：跑到第 31 条时 vkQueueSubmit 开始返回 -3，看着像驱动的锅）。
                    // RESET_COMMAND_BUFFER is required: these command buffers get re-recorded over and over.
                    // Without the pool flag, vkBeginCommandBuffer is only allowed on a command buffer in the
                    // initial state; re-recording is undefined behaviour. (The first version tripped exactly
                    // here: vkQueueSubmit started returning -3 from copy 31 on, looking like a driver fault.)
                    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                    pci.queueFamilyIndex = v.qfi;
                    VkCommandPool pool = VK_NULL_HANDLE;
                    vkCreateCommandPool(v.dev, &pci, NULL, &pool);
                    VkCommandBuffer cbs[3];
                    VkCommandBufferAllocateInfo cbai;
                    memset(&cbai, 0, sizeof cbai);
                    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                    cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = depth;
                    vkAllocateCommandBuffers(v.dev, &cbai, cbs);
                    VkFence fences[3];
                    VkFenceCreateInfo fci;
                    memset(&fci, 0, sizeof fci);
                    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                    for (int i = 0; i < depth; i++) vkCreateFence(v.dev, &fci, NULL, &fences[i]);
                    uint64_t dstoff_max = (uint64_t)dstsz > stage ? (uint64_t)dstsz - stage : 0;
                    for (int pass = 0; pass < 2; pass++) {
                        uint64_t total = (pass == 0 ? xfer_mb : pool_mb) * MB;
                        uint64_t copies = (total + stage - 1) / stage;
                        uint64_t t0 = now_us();
                        int bad2 = 0;
                        // 失败要能被点名（哪一条、哪个 VkResult、走到哪儿）——"看着像有效"是旧路踩过的坑
                        // A failure must be nameable (which copy, which VkResult, how far it got): "looks like it
                        // worked" is the trap the old road fell into
                        const char *why = "";
                        int32_t wres = 0;
                        uint64_t done = 0;
                        for (uint64_t i = 0; i < copies; i++) {
                            uint32_t slot = (uint32_t)(i % (uint64_t)depth);
                            if (i >= (uint64_t)depth) {
                                VkResult w = vkWaitForFences(v.dev, 1, &fences[slot], VK_TRUE, 5000ull * 1000000ull);
                                if (w != VK_SUCCESS) { bad2 = 1; why = "等栅栏失败"; wres = (int32_t)w; break; }
                            }
                            vkResetFences(v.dev, 1, &fences[slot]);
                            VkCommandBufferBeginInfo bi;
                            memset(&bi, 0, sizeof bi);
                            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                            // flags 留 0：这几个 cb 要被反复重录、反复提交，ONE_TIME_SUBMIT 只允许提交一次
                            // flags stay 0: these command buffers are re-recorded and re-submitted; ONE_TIME_SUBMIT allows one submit only
                            vkResetCommandBuffer(cbs[slot], 0);
                            vkBeginCommandBuffer(cbs[slot], &bi);
                            VkBufferCopy cp;
                            memset(&cp, 0, sizeof cp);
                            cp.srcOffset = 0;
                            cp.dstOffset = (VkDeviceSize)((i * stage) % (dstoff_max ? dstoff_max : 1));
                            cp.size = stage;
                            vkCmdCopyBuffer(cbs[slot], sbuf, dbuf, 1, &cp);
                            vkEndCommandBuffer(cbs[slot]);
                            VkSubmitInfo si;
                            memset(&si, 0, sizeof si);
                            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                            si.commandBufferCount = 1; si.pCommandBuffers = &cbs[slot];
                            VkResult s = vkQueueSubmit(v.q, 1, &si, fences[slot]);
                            if (s != VK_SUCCESS) { bad2 = 1; why = "提交失败"; wres = (int32_t)s; break; }
                            done++;
                            if ((i + 1) % 64 == 0)
                                A("  搬运进度 %llu/%llu 条，已用 %llu ms", (unsigned long long)(i + 1),
                                  (unsigned long long)copies, (unsigned long long)((now_us() - t0) / 1000));
                        }
                        for (int i = 0; i < depth; i++) vkWaitForFences(v.dev, 1, &fences[i], VK_TRUE, 5000ull * 1000000ull);
                        uint64_t ms = (now_us() - t0) / 1000ull;
                        double mbps = ms ? ((double)(total / MB) * 1000.0 / (double)ms) : 0.0;
                        double done_mbps = ms ? ((double)(done * stage / MB) * 1000.0 / (double)ms) : 0.0;
                        if (pass == 0) {
                            small_ms = ms; small_copies = copies;
                            A("小搬运 %llu MB（环 %llu KiB，%llu 条拷贝，在飞 %d）：%llu ms ⇒ %.0f MB/s%s",
                              (unsigned long long)xfer_mb, (unsigned long long)stage_kb,
                              (unsigned long long)copies, depth, (unsigned long long)ms, mbps,
                              bad2 ? "（中途有提交/等待失败）" : "");
                        } else {
                            pool_ms = ms; pool_copies = copies; pool_mbps = mbps; pool_bytes_done = total;
                            pool_done_copies = done; pool_failed = bad2; pool_fail_code = wres;
                            A("一池全搬一遍 %llu MB（= 池大小，%llu 条拷贝）：%llu ms ⇒ **%.0f MB/s**%s",
                              (unsigned long long)pool_mb, (unsigned long long)copies,
                              (unsigned long long)ms, mbps, bad2 ? "（中途有提交/等待失败）" : "");
                            if (bad2)
                                A("  失败点名：第 %llu 条上%s，VkResult %d；**实际搬完 %llu 条 = %llu MB**，"
                                  "按实际搬完算 %.0f MB/s ⇒ 上面那个 %llu ms 里含超时等待，不是纯搬运",
                                  (unsigned long long)(done + 1), why, (int)wres,
                                  (unsigned long long)done, (unsigned long long)(done * stage >> 20),
                                  done_mbps, (unsigned long long)ms);
                            else
                                A("  全程无失败：%llu 条拷贝、%llu MB、%llu ms", (unsigned long long)done,
                                  (unsigned long long)(done * stage >> 20), (unsigned long long)ms);
                            A("⇒ 这是「每帧搬运量」的**上界**：真帧不可能比把整池搬一遍还多；"
                              "按它折出的环大小档位见下");
                        }
                    }
                    if (pool_ms) {
                        A("若一帧搬运量 = 上界的 0.1%% / 1%% / 10%% ⇒ %.1f / %.1f / %.1f MB/帧 ⇒ 环（2~3 倍）≈ %.1f~%.1f / %.1f~%.1f / %.1f~%.1f MB",
                          (double)pool_mb * 0.001, (double)pool_mb * 0.01, (double)pool_mb * 0.1,
                          (double)pool_mb * 0.002, (double)pool_mb * 0.003,
                          (double)pool_mb * 0.02, (double)pool_mb * 0.03,
                          (double)pool_mb * 0.2, (double)pool_mb * 0.3);
                    }
                    A("块数算术（未实际分配）：池 %llu MB ÷ 块 %llu MB ⇒ %llu 块；"
                      "目的地这一块 %llu MB（本测试只建了它一块）",
                      (unsigned long long)pool_mb, (unsigned long long)(chunk_max_mb ? chunk_max_mb : chunk_mb_hint),
                      (unsigned long long)(((chunk_max_mb ? chunk_max_mb : chunk_mb_hint) ?
                                           (pool_mb + (chunk_max_mb ? chunk_max_mb : chunk_mb_hint) - 1) /
                                           (chunk_max_mb ? chunk_max_mb : chunk_mb_hint) : 0)),
                      (unsigned long long)dst_mb);
                    for (int i = 0; i < depth; i++) vkDestroyFence(v.dev, fences[i], NULL);
                    vkFreeCommandBuffers(v.dev, pool, depth, cbs);
                    vkDestroyCommandPool(v.dev, pool, NULL);
                    vkFreeMemory(v.dev, dmem, NULL);
                    vkDestroyBuffer(v.dev, dbuf, NULL);
                }
                vkUnmapMemory(v.dev, smem);
                vkFreeMemory(v.dev, smem, NULL);
            }
            vkDestroyBuffer(v.dev, sbuf, NULL);
        }
    }

    // ---- 问 4：提交一次的开销 + VK2_INFLIGHT | Q4: the cost of one submit and VK2_INFLIGHT ----
    uint64_t sub1_us = 0, sub4_us = 0, sub16_us = 0, sub64_us = 0;
    uint64_t inf_us[5] = { 0, 0, 0, 0, 0 };
    int inf_depth[5] = { 1, 2, 3, 4, 8 };
    int fence_timeout_ok = -1, fence_recover_ok = -1;
    Q("一次 vkQueueSubmit 装 N 条命令的开销是多少？有界在飞深度 D 下平均每次提交多少？栅栏超时返回什么？");
    {
        VkCommandPoolCreateInfo pci;
        memset(&pci, 0, sizeof pci);
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = v.qfi;
        VkCommandPool pool = VK_NULL_HANDLE;
        vkCreateCommandPool(v.dev, &pci, NULL, &pool);
        VkCommandBufferAllocateInfo cbai;
        memset(&cbai, 0, sizeof cbai);
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 64;
        VkCommandBuffer cbs[64];
        vkAllocateCommandBuffers(v.dev, &cbai, cbs);
        for (int i = 0; i < 64; i++) {   // 空命令缓冲：量的就是提交本身 | empty command buffers: we measure the submit itself
            VkCommandBufferBeginInfo bi;
            memset(&bi, 0, sizeof bi);
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            // flags 留 0（会被反复提交）| flags stay 0 (they get submitted many times)
            vkBeginCommandBuffer(cbs[i], &bi);
            vkEndCommandBuffer(cbs[i]);
        }
        uint64_t t;
        t = submit_bench(&v, cbs, 1, submits, 1, NULL);  sub1_us  = t / submits;
        t = submit_bench(&v, cbs, 4, submits, 1, NULL);  sub4_us  = t / submits;
        t = submit_bench(&v, cbs, 16, submits, 1, NULL); sub16_us = t / submits;
        t = submit_bench(&v, cbs, 64, submits, 1, NULL); sub64_us = t / submits;
        A("一次提交装 N 条空命令（每次提交都等栅栏）：N=1 %llu us；N=4 %llu us；N=16 %llu us；N=64 %llu us",
          (unsigned long long)sub1_us, (unsigned long long)sub4_us,
          (unsigned long long)sub16_us, (unsigned long long)sub64_us);
        for (int i = 0; i < 5; i++) {
            uint64_t w = 0;
            uint64_t tt = submit_bench(&v, cbs, 1, submits, inf_depth[i], &w);
            inf_us[i] = tt / submits;
            A("有界在飞 D=%d：平均 %llu us/提交（其中等栅栏 %llu us）",
              inf_depth[i], (unsigned long long)inf_us[i],
              (unsigned long long)(w / (submits > 1 ? submits - inf_depth[i] : 1)));
        }
        // VK2_INFLIGHT 从哪一档起：D=3 与 D=4 的每提交开销差多少？差得不多就取 3（N3 的起点）
        // Where VK2_INFLIGHT starts: how much does D=4 cost over D=3? Not much => start at 3 (N3's start)
        A("⇒ VK2_INFLIGHT 起点 = 3（§9.6 的起点）；D=3 与 D=4 的每提交开销差 %lld us，"
          "放大到 8 反而 %s（真取舍要看 N3 的「槽等待率」）",
          (long long)inf_us[3] - (long long)inf_us[2],
          inf_us[4] < inf_us[2] ? "更便宜" : "更贵");
        // 栅栏超时行为（N3 的"有界在飞"要带超时地等）| fence timeout behaviour (N3 waits with a timeout)
        VkFenceCreateInfo fci;
        memset(&fci, 0, sizeof fci);
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence f = VK_NULL_HANDLE;
        vkCreateFence(v.dev, &fci, NULL, &f);
        VkResult r0 = vkWaitForFences(v.dev, 1, &f, VK_TRUE, 0);   // 没提交过、没置位的栅栏
        fence_timeout_ok = (r0 == VK_TIMEOUT);
        A("对未置位栅栏 vkWaitForFences(timeout=0)：%s（VkResult %d）",
          r0 == VK_TIMEOUT ? "VK_TIMEOUT（如规范所期）" : "不是 VK_TIMEOUT", (int)r0);
        VkSubmitInfo si;
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cbs[0];
        if (vkQueueSubmit(v.q, 1, &si, f) == VK_SUCCESS) {
            VkResult r1 = vkWaitForFences(v.dev, 1, &f, VK_TRUE, 5000000000ull);
            fence_recover_ok = (r1 == VK_SUCCESS);
            A("超时之后同一栅栏能不能恢复：提交后等 5 秒 → %s", r1 == VK_SUCCESS ? "成功" : "失败");
        }
        vkDestroyFence(v.dev, f, NULL);
        vkDestroyCommandPool(v.dev, pool, NULL);
    }

    // ---- 问 5：脏位图粒度 | Q5: dirty-bitmap granularity ----
    uint64_t gran_kb_list[3] = { 4, 16, 64 };
    uint64_t bm_bytes[3] = { 0, 0, 0 }, bm_us[3] = { 0, 0, 0 };
    uint64_t bm_synth_runs[3] = { 0, 0, 0 }, bm_synth_bytes[3] = { 0, 0, 0 };
    uint64_t bm_all_bytes = 0, bm_all_runs = 0;
    Q("脏位图按每页 %ld 字节的粒度做有多大、扫一遍多贵？（4 KiB 起步，要不要放大）", pagesize());
    {
        for (int g = 0; g < 3; g++) {
            uint64_t gran = gran_kb_list[g] * 1024;
            uint64_t pages = (pool_mb * MB) / gran;
            bm b;
            bm_init(&b, pages);
            bm_bytes[g] = (pages + 7) / 8;
            uint64_t t0 = now_us();
            for (int rep = 0; rep < 3; rep++) {
                bm_clear(&b);
                // 合成图案（**不是**游戏读数）：每 1 MiB 标第一个页 —— 用来验"标 + 并 run"两件事是对的
                // Synthetic pattern (NOT a game reading): mark the first page of every 1 MiB, which exercises
                // mark and run-merge
                for (uint64_t off = 0; off < pool_mb * MB; off += MB) bm_set(&b, off / gran);
                uint64_t bytes = 0;
                uint64_t runs = bm_runs(&b, &bytes);
                if (rep == 0) { bm_synth_runs[g] = runs; bm_synth_bytes[g] = bytes * gran; }
                // 全脏（上界）：全置位再扫 | fully dirty (upper bound): set everything, then sweep
                memset(b.bits, 0xff, (size_t)((pages + 7) / 8));
                uint64_t bytes2 = 0;
                uint64_t runs2 = bm_runs(&b, &bytes2);
                if (rep == 0) { bm_all_runs = runs2; bm_all_bytes = bytes2 * gran; }
            }
            bm_us[g] = (now_us() - t0) / 3;
            A("粒度 %llu KiB：位图 %llu 字节（%llu 页）；标记+并 run 一遍 %llu us；"
              "合成图案（每 1 MiB 一页）= %llu run / %llu MB",
              (unsigned long long)gran_kb_list[g], (unsigned long long)bm_bytes[g],
              (unsigned long long)pages, (unsigned long long)bm_us[g],
              (unsigned long long)bm_synth_runs[g], (unsigned long long)(bm_synth_bytes[g] >> 20));
            free(b.bits);
        }
        A("全脏（机制上界）：%llu run / %llu MB / 池内 %llu 条块拷贝", (unsigned long long)bm_all_runs,
          (unsigned long long)(bm_all_bytes >> 20),
          (unsigned long long)(chunk_max_mb ? (pool_mb + chunk_max_mb - 1) / chunk_max_mb : 0));
        A("⇒ 粒度取 **4 KiB 起步**：它比页（%ld）细，能描述「小于一页的脏区间」去驱动拷贝，"
          "而位图本身只有 %llu 字节、扫一遍 %llu us（相对一帧可以忽略）",
          pagesize(), (unsigned long long)bm_bytes[0], (unsigned long long)bm_us[0]);
        A("「每帧脏 run 数」与「每帧搬运字节数」两个读数要等 N2 接上游戏路径才有 —— 本探针只给机制上界，"
          "读数表里留槽，不许拿全脏上界当帧读数");
    }

    // ---- 结论键值 | the conclusion key/values ----
    printf("\n--- VK2_PROBE_SUMMARY（结论键值，供文档抄录）---\n");
    printf("VK2_DEVICE=%s\n", v.p2.properties.deviceName);
    printf("VK2_API_VERSION=%u.%u.%u\n", VK_VERSION_MAJOR(v.p2.properties.apiVersion),
           VK_VERSION_MINOR(v.p2.properties.apiVersion), VK_VERSION_PATCH(v.p2.properties.apiVersion));
    printf("VK2_MAX_STORAGE_BUFFER_RANGE_MB=%llu\n",
           (unsigned long long)(v.p2.properties.limits.maxStorageBufferRange >> 20));
    printf("VK2_MAX_MEMORY_ALLOCATION_MB=%llu\n", (unsigned long long)(v.m3.maxMemoryAllocationSize >> 20));
    printf("VK2_PAGE_SIZE=%ld\n", pagesize());
    printf("VK2_CHUNK_NOTCH_MB=%llu\n", (unsigned long long)notch);
    printf("VK2_CHUNK_CAP_MB=%llu\n", (unsigned long long)cap_mb_reported);
    printf("VK2_CHUNK_MAX_MB=%llu\n", (unsigned long long)chunk_max_mb);
    printf("VK2_CHUNK_MB=%llu\n", (unsigned long long)chunk_mb);
    printf("VK2_POOL_MB=%llu\n", (unsigned long long)pool_mb);
    printf("VK2_DIRTY_GRAN_KB=4\n");
    printf("VK2_DIRTY_BITMAP_BYTES_4K=%llu\n", (unsigned long long)bm_bytes[0]);
    printf("VK2_DIRTY_SWEEP_US_4K=%llu\n", (unsigned long long)bm_us[0]);
    printf("VK2_DIRTY_ALL_RUNS=%llu\n", (unsigned long long)bm_all_runs);
    printf("VK2_DIRTY_ALL_MB=%llu\n", (unsigned long long)(bm_all_bytes >> 20));
    printf("VK2_INFLIGHT=3\n");
    printf("VK2_STAGE_KB=%llu\n", (unsigned long long)stage_kb);
    printf("VK2_XFER_SMALL_MB=%llu\n", (unsigned long long)xfer_mb);
    printf("VK2_XFER_SMALL_MS=%llu\n", (unsigned long long)small_ms);
    printf("VK2_XFER_SMALL_COPIES=%llu\n", (unsigned long long)small_copies);
    printf("VK2_POOL_PASS_BYTES=%llu\n", (unsigned long long)pool_bytes_done);
    printf("VK2_POOL_PASS_COPIES=%llu\n", (unsigned long long)pool_copies);
    printf("VK2_POOL_PASS_DONE_COPIES=%llu\n", (unsigned long long)pool_done_copies);
    printf("VK2_POOL_PASS_FAILED=%d\n", pool_failed);
    printf("VK2_POOL_PASS_FAIL_CODE=%d\n", (int)pool_fail_code);
    printf("VK2_POOL_PASS_MS=%llu\n", (unsigned long long)pool_ms);
    printf("VK2_POOL_PASS_MBPS=%.0f\n", pool_mbps);
    printf("VK2_SUBMIT_N1_US=%llu\n", (unsigned long long)sub1_us);
    printf("VK2_SUBMIT_N4_US=%llu\n", (unsigned long long)sub4_us);
    printf("VK2_SUBMIT_N16_US=%llu\n", (unsigned long long)sub16_us);
    printf("VK2_SUBMIT_N64_US=%llu\n", (unsigned long long)sub64_us);
    printf("VK2_SUBMIT_INFLIGHT1_US=%llu\n", (unsigned long long)inf_us[0]);
    printf("VK2_SUBMIT_INFLIGHT2_US=%llu\n", (unsigned long long)inf_us[1]);
    printf("VK2_SUBMIT_INFLIGHT3_US=%llu\n", (unsigned long long)inf_us[2]);
    printf("VK2_SUBMIT_INFLIGHT4_US=%llu\n", (unsigned long long)inf_us[3]);
    printf("VK2_SUBMIT_INFLIGHT8_US=%llu\n", (unsigned long long)inf_us[4]);
    printf("VK2_FENCE_TIMEOUT_ZERO=%s\n", fence_timeout_ok == 1 ? "VK_TIMEOUT" : (fence_timeout_ok == 0 ? "other" : "n/a"));
    printf("VK2_FENCE_RECOVER=%s\n", fence_recover_ok == 1 ? "ok" : (fence_recover_ok == 0 ? "fail" : "n/a"));
    printf("VK2_MEM_AVAIL_START_MB=%llu\n", (unsigned long long)(mem_avail_kb() >> 10));
    fflush(stdout);

    vk_close(&v);
    printf("\n完成（只问不接管：本轮没有建任何池、没有写脏记账、没有接管回放层）\n");
    return 0;
}
