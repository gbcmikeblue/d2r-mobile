// B3 仪器：GPU 操作流水（见 vk_ops.h 的开头那段"为什么"）。
// B3 instrument: the GPU-operation ledger (see the "why" note at the top of vk_ops.h).
#include "vk_ops.h"
#include "vk_device.h"
#include "dm.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// 出图数：定义在 nvn_vk.c（主线程的心跳也用它）。就地声明，不为了一个计数器去建头文件。
// The present count: defined in nvn_vk.c (the heartbeat uses it too). Declared locally rather than adding a header for
// one counter.
uint64_t vk_present_count(void);

// 环大小：只留"最近 24 笔" —— 长等 / 停顿动辄 10 秒，24 笔足以覆盖"最后一次成功提交 ↔ 第一条 -4"之间那一小段，
// 又不至于把日志撑爆（每笔一行，24 行）。
// Ring size: the last 24 entries only. A stall lasts about ten seconds, so 24 entries comfortably cover the stretch
// between the last successful submit and the first -4 without bloating the log (one line each, 24 lines).
#define VK_OPS_RING 24

static struct { uint64_t seq; const char *site; uint64_t present; uint64_t ms; } g_ring[VK_OPS_RING];
static unsigned long long g_ops;          // 累计成功操作笔数 | total successful operations
static uint64_t g_t0;                     // 第一笔的时刻（读数里给相对毫秒，好看）| time of the first entry (relative ms reads better)
static uint64_t g_lost_ms;                // 第一条 -4 的时刻（0 = 还没见过）| time of the first -4 (0 = none yet)
static unsigned long long g_lost_n;       // -4 的条数 | number of -4 results
static unsigned long long g_other_n;      // 其他非 SUCCESS 的条数 | other non-SUCCESS results
static int g_lost_dumped;                 // 第一条 -4 的那份"前序操作"已经打过 | the "preceding operations" report for the first -4 is out
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   // 提交跑在两条线程上（客体 / 前端）| submits run on two threads (guest / front end)

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void device_info(void) {
    VkPhysicalDeviceProperties p;
    VkPhysicalDevice ph = vk_phys();
    if (ph == VK_NULL_HANDLE) { LOG("  [ops] 物理设备还没起来（设备信息取不到）"); return; }
    vkGetPhysicalDeviceProperties(ph, &p);
    LOG("  [ops] 设备 %s：驱动 %u.%u.%u，Vulkan %u.%u.%u；扩展 external_memory_fd=%d，动态渲染=%d，"
        "零拷贝内存类型 %u", p.deviceName,
        VK_VERSION_MAJOR(p.driverVersion), VK_VERSION_MINOR(p.driverVersion), VK_VERSION_PATCH(p.driverVersion),
        VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
        vk_have_ext("VK_KHR_external_memory_fd"), vk_have_dyn_render(), vk_zero_copy_type());
}

void vk_op_note(const char *site) {
    pthread_mutex_lock(&g_mu);
    uint64_t now = now_ms();
    if (!g_t0) g_t0 = now;
    unsigned i = (unsigned)(g_ops % VK_OPS_RING);
    g_ops++;
    g_ring[i].seq = g_ops;
    g_ring[i].site = site;
    g_ring[i].present = vk_present_count();   // 提交序号 / 出图数 / 时刻 —— 三样都留 | submit serial, present count and time
    g_ring[i].ms = now;
    pthread_mutex_unlock(&g_mu);
}

void vk_op_dump(const char *why) {
    pthread_mutex_lock(&g_mu);
    unsigned n = g_ops < VK_OPS_RING ? (unsigned)g_ops : VK_OPS_RING;
    LOG("[ops] GPU 操作流水（%s）：累计 %llu 笔成功操作（提交/呈现/等空闲），-4 共 %llu 条、其他错误 %llu 条；"
        "下面按时间从早到晚列最近 %u 笔（相对毫秒 = 第一笔起算）",
        why, g_ops, g_lost_n, g_other_n, n);
    if (!n) { LOG("  [ops] （一笔都没有：设备还没提交过任何东西）"); }
    for (unsigned k = 0; k < n; k++) {
        unsigned i = (unsigned)((g_ops - n + k) % VK_OPS_RING);
        LOG("  [ops] #%llu %s  出图 %llu  T+%llu ms",
            (unsigned long long)g_ring[i].seq, g_ring[i].site,
            (unsigned long long)g_ring[i].present,
            (unsigned long long)(g_ring[i].ms - (g_t0 ? g_t0 : g_ring[i].ms)));
    }
    if (g_lost_ms) LOG("  [ops] 第一条 -4 出现在 T+%llu ms（其上就是它之前最后几笔成功操作）",
                       (unsigned long long)(g_lost_ms - (g_t0 ? g_t0 : g_lost_ms)));
    pthread_mutex_unlock(&g_mu);
}

void vk_op_result(const char *call, VkResult r) {
    if (r == VK_SUCCESS) return;
    if (r == VK_ERROR_DEVICE_LOST) {
        pthread_mutex_lock(&g_mu);
        g_lost_n++;
        unsigned long long n = g_lost_n;
        if (!g_lost_ms) g_lost_ms = now_ms();
        int first = !g_lost_dumped;
        if (first) g_lost_dumped = 1;
        pthread_mutex_unlock(&g_mu);
        if (first) {
            // 第一条 -4：出错调用 + 设备信息 + 前序 GPU 操作，三样一次给全（plan §10.1 第 1 条）
            // The first -4: the failing call, the device information and the preceding GPU operations, all at once
            LOG("【红】[ops] 第一条 VK_ERROR_DEVICE_LOST：出错的调用 = %s（VkResult %d）；此刻出图 %llu",
                call, (int)r, (unsigned long long)vk_present_count());
            device_info();
            vk_op_dump("第一条 -4 之前的前序 GPU 操作");
        } else if (n % 32 == 0) {
            LOG("【红】[ops] 第 %llu 条 VK_ERROR_DEVICE_LOST：%s（-4 是粘的，其后每 32 条再报一行）", n, call);
        }
        return;
    }
    // 其他错误码：最多 8 行（`OUT_OF_DATE_KHR` / `SUBOPTIMAL_KHR` 是上屏的正常返回值，由调用方处理，不在这里报）
    // Other error codes: at most 8 lines (`OUT_OF_DATE_KHR` / `SUBOPTIMAL_KHR` are normal present results handled by
    // the caller, so they are not reported here)
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return;
    pthread_mutex_lock(&g_mu);
    unsigned long long n = ++g_other_n;
    pthread_mutex_unlock(&g_mu);
    if (n <= 8) LOG("【黄】[ops] %s 返回 VkResult %d（非 SUCCESS；这类最多报 8 行）", call, (int)r);
}
