// 安卓图形后端的 Vulkan 设备：进程内建一次，回放层与内存池共用。
// The Android graphics backend's Vulkan device: created once per process and shared by the replay layer
// and the memory pools.
//
// 扩展只开「查到的、且确实需要的」：真机上 Adreno 830 有 VK_KHR_external_memory_fd（内存换轨要靠它把
// Vulkan 内存导出成 fd），没有 VK_EXT_external_memory_host（见 docs/ANDROID_PLAN 的 M2 第 3 步）。
// 少开一个不存在的扩展就会 vkCreateDevice 失败，所以一律先查再开。
// Only extensions that are both present and actually needed are enabled: on the real device the Adreno 830
// has VK_KHR_external_memory_fd (the memory overlay needs it to export Vulkan memory as an fd) and does not
// have VK_EXT_external_memory_host (see M2 step 3 in docs/ANDROID_PLAN). Enabling one that is absent makes
// vkCreateDevice fail, so everything is looked up before it is enabled.
// 安卓 surface / VK_KHR_swapchain（M3 的窗口要用）：**这个宏必须排在所有包含之前** —— vk_device.h 自己会把
// vulkan.h 带进来，而 vulkan.h 的包含卫一旦生效，后面再定义这个宏就没有 `VkAndroidSurfaceCreateInfoKHR` 那一批
// 类型了（踩过一次：编译报的全是"未声明的类型"，看不出是宏位置的问题）。这个文件是安卓专有的，定义它没有副作用。
// Android surface / VK_KHR_swapchain (needed by the M3 window): **this macro has to come before every include** --
// vk_device.h pulls in vulkan.h itself, and once vulkan.h's include guard is set, defining the macro later leaves the
// `VkAndroidSurfaceCreateInfoKHR` family undeclared (learned the hard way: the compiler only says "unknown type name",
// which does not point at the macro's position). This file is Android-only, so defining it has no side effects.
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <android/native_window.h>   // ANativeWindow（surface 的窗口类型）| ANativeWindow, the surface's window type
#include "vk_device.h"
#include "dm.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>   // 定期写回的限频（见 vk_device_pcache_tick）| for the tick's rate limit

static VkInstance g_inst;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qfi;
static VkPhysicalDeviceMemoryProperties g_mem;
static uint32_t g_zero_copy = UINT32_MAX;
static int g_tried;
static int g_dyn_render;                       // 动态渲染可用（回放层开通道的前提）| dynamic rendering available (the replay layer's precondition for opening passes)
static int g_fill_non_solid;                   // 非实心多边形模式（线框等）| non-solid polygon modes (wireframe and such)
static int g_attr_divisor, g_attr_divisor_zero; // 顶点流每实例步进 > 1 / 可为 0 | per-instance vertex stream step > 1 / may be 0
static int g_aniso;                            // 各向异性过滤（采样器用）| anisotropic filtering (for samplers)
static VkExtensionProperties *g_exts; static uint32_t g_nexts;
static void *g_window;                         // 窗口（M3）：有它才开 surface / 交换链 / 呈现队列 | the window (M3): with it, surface, swapchain and the present queue are enabled
static VkSurfaceKHR g_surf;
static int g_present_fam = -1;                 // 能呈现的队列族；-1 = 没有 | the queue family that can present; -1 = none

// ---------- 管线缓存（VkPipelineCache，图形与计算共用）----------
// 为什么要有（docs/ANDROID_PLAN「九」第 1 项）：安卓是**运行时**编译 SPIR-V（没有 iOS 那条"离线编好 metallib"的路），
// 而本仓建管线一直传 VK_NULL_HANDLE —— 进程内没有驱动级去重，跨进程每次都从零编译。建一个图形与计算**共用**的缓存，
// 把 blob 落到可写目录，下次启动用 pInitialData 载回：这就是 iOS"预先编好、进场景不现编"的安卓对应物。
// blob 什么时候写：**前台定期**（vk_device_pcache_tick，真机上这条是主力）+ 切后台（TERM_WINDOW）+ 退出
// （atexit / DESTROY）。为什么不能只靠后两条：MIUI 把这个包按游戏处理，退到后台直接 SIGKILL，那些回调一次都跑不到
// （实测记录见 vk_device_pcache_tick 上面那段）。
// 三条约束决定了下面的形状：
//   ① blob **与驱动/设备绑定**（换驱动后必须丢弃）：所以文件前面压一个头，带上 vendor/device/driver/api 与
//      PPhysicalDeviceProperties::pipelineCacheUUID；对不上直接不用（宁可从零编译，也不要一份脏缓存）；
//   ② 对 VkPipelineCache 的访问**必须由调用方串行化**（建管线与取数据都碰它）：所以配一把锁，建管线前后要配
//      vk_dev_pcache_lock/unlock（见 vk_pipe.c 的两处）；
//   ③ 路径由 **DM_PIPE_CACHE** 给；没给就退回 <DM_ROOT>/pipecache.bin（控制台宿主在 shell 下可写）。APK 侧由
//      main_apk.c 指到 app 的**内部**目录 —— /data/local/tmp 对 APK 只读（SELinux），写在那儿只会静默失败。
//      DM_PIPE_CACHE=0 表示整体关掉（做"清缓存 / 带缓存"的 A/B 用）。
// ---------- The pipeline cache (shared by graphics and compute) ----------
// Why it exists (item 1 of section "nine" in docs/ANDROID_PLAN): Android compiles SPIR-V **at run time** (there is no
// offline-metallib route like iOS's) while this repo has always passed VK_NULL_HANDLE when creating pipelines -- no
// driver-level dedup inside the process and a from-scratch compile across processes. One cache **shared** by graphics
// and compute, its blob written to a writable directory on exit and background, and loaded back through pInitialData on
// the next start, is the Android counterpart of iOS's "precompiled, nothing compiled when entering a scene".
// Three constraints shape what follows:
//   1. the blob is **tied to the driver and device** (it must be dropped after a driver change), so a header carrying
//      vendor/device/driver/api plus VkPhysicalDeviceProperties::pipelineCacheUUID sits in front of it; a mismatch means
//      it is not used at all (compiling from scratch beats a stale cache);
//   2. access to a VkPipelineCache **must be serialised by the caller** (both creating pipelines and reading the data
//      touch it), hence the lock and the vk_dev_pcache_lock/unlock pairs at the two sites in vk_pipe.c;
//   3. the path comes from **DM_PIPE_CACHE**, falling back to <DM_ROOT>/pipecache.bin (writable for the console host
//      under adb shell). On the APK side main_apk.c points it at the app's **internal** directory -- /data/local/tmp is
//      read-only for the APK (SELinux) and a write there would fail silently. DM_PIPE_CACHE=0 disables it outright
//      (for the "clear / keep" A/B).
#define PC_MAGIC 0x43504d44u        // "DMPC"（小端读出）| "DMPC" as read little-endian
#define PC_HDR_VER 1u               // 头的版本：以后改字段就加这个号 | header version, bumped when fields change
#define PC_MAX (256u << 20)         // 上限 256 MB：超过就当文件坏了或串了味，宁可不用 | cap at 256 MB; beyond that the file is treated as corrupt or foreign and not used

typedef struct {
    uint32_t magic, hdr_ver;
    uint32_t vendor_id, device_id, driver_version, api_version;
    uint8_t uuid[VK_UUID_SIZE];     // VkPhysicalDeviceProperties::pipelineCacheUUID
    uint32_t blob_size;
    uint32_t reserved;
} pcache_hdr;

static VkPipelineCache g_pcache;
static pthread_mutex_t g_pcache_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_pcache_off;            // DM_PIPE_CACHE=0：整体关掉 | cache turned off outright
static int g_pcache_atexit;         // 写回钩子只登记一次 | the write-back hook is registered once
static char g_pcache_path[512];
// 定期写回（见 vk_device_pcache_tick）：上一次成功写出的字节数与时刻
// Periodic write-back (see vk_device_pcache_tick): the size and time of the last successful write
static size_t g_pcache_saved_n;
static double g_pcache_last_save;

void vk_dev_pcache_lock(void) { pthread_mutex_lock(&g_pcache_mu); }
void vk_dev_pcache_unlock(void) { pthread_mutex_unlock(&g_pcache_mu); }
VkPipelineCache vk_dev_pcache(void) { return g_pcache; }

// 字节数按可读单位打：一律用 MB 的话，"0.0 MB"分不出是"刚建了一条管线"还是"缓存几乎是空的"
// A readable unit for a byte count: always printing MB turns "0.0 MB" into something that cannot be told apart
// between "one pipeline just went in" and "the cache is nearly empty"
static void pc_size(char *buf, size_t cap, size_t n) {
    if (n >= (1u << 20)) snprintf(buf, cap, "%.1f MB", (double)n / 1048576.0);
    else if (n >= 1024) snprintf(buf, cap, "%.1f KB", (double)n / 1024.0);
    else snprintf(buf, cap, "%zu B", n);
}

static const char *pcache_path(void) {
    const char *e = getenv("DM_PIPE_CACHE");
    if (e && *e) {
        if (!strcmp(e, "0")) { g_pcache_off = 1; return NULL; }
        snprintf(g_pcache_path, sizeof g_pcache_path, "%s", e);
        return g_pcache_path;
    }
    const char *root = getenv("DM_ROOT");
    snprintf(g_pcache_path, sizeof g_pcache_path, "%s/pipecache.bin", (root && *root) ? root : ".");
    return g_pcache_path;
}

// 进程退出时写回：APK 正常退出与控制台宿主 return 都走它（切后台另有一条显式调用，见 main_apk.c）
// Write back on process exit: both the APK's normal exit and the console host's return go through it (the background
// case has an explicit call, see main_apk.c)
static void pcache_exit_save(void) { vk_device_pcache_save("进程退出"); }

// 建缓存：有可用的旧 blob 就载入，没有就建一个空的。**必须在设备建好之后调**（要设备属性来核对来历）。
// Create the cache: load a usable old blob when there is one, otherwise create an empty one. **Call it only after the
// device exists** (the device properties are needed to check where the blob came from).
static void pcache_boot(const VkPhysicalDeviceProperties *p) {
    const char *path = pcache_path();
    if (g_pcache_off) { LOG("管线缓存：DM_PIPE_CACHE=0，本次不用缓存（每次都现编）"); return; }
    void *blob = NULL;
    size_t n = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        pcache_hdr h;
        if (fread(&h, sizeof h, 1, f) != 1 || h.magic != PC_MAGIC || h.hdr_ver != PC_HDR_VER) {
            LOG("【黄】管线缓存的头不认得（%s）：丢弃，从零编译", path);
        } else if (h.vendor_id != p->vendorID || h.device_id != p->deviceID || h.driver_version != p->driverVersion ||
                   h.api_version != p->apiVersion || memcmp(h.uuid, p->pipelineCacheUUID, VK_UUID_SIZE)) {
            // 换驱动 / 换设备：官方样例与 zeux.io《Robust pipeline cache serialization》都点名这条必须自己挡
            // A driver or device change: both the official samples and zeux.io's "Robust pipeline cache serialization"
            // call this out as something the caller has to catch
            LOG("【黄】管线缓存的来历与这台设备对不上（设备 0x%04x→0x%04x、驱动 %u→%u）：丢弃，从零编译",
                h.device_id, p->deviceID, h.driver_version, p->driverVersion);
        } else if (h.blob_size == 0 || h.blob_size > PC_MAX) {
            LOG("【黄】管线缓存的大小不合（%u 字节）：丢弃", h.blob_size);
        } else if (!(blob = malloc(h.blob_size))) {
            LOG("【黄】管线缓存读不进来：%u 字节的缓冲分配不出", h.blob_size);
        } else if (fread(blob, 1, h.blob_size, f) != h.blob_size) {
            LOG("【黄】管线缓存只有半截（%s）：丢弃", path);
            free(blob); blob = NULL;
        } else {
            n = h.blob_size;
        }
        fclose(f);
    }
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    if (n) { ci.initialDataSize = n; ci.pInitialData = blob; }
    VkResult r = vkCreatePipelineCache(g_dev, &ci, NULL, &g_pcache);
    if (r != VK_SUCCESS) {
        LOG("【黄】建 VkPipelineCache 失败：VkResult %d（管线照常建，只是每次都现编）", (int)r);
        g_pcache = VK_NULL_HANDLE;
    } else if (n) {
        char sz[32];
        pc_size(sz, sizeof sz, n);
        LOG("【绿】管线缓存：载入 %s（%s）——本次启动的管线编译可复用", sz, path);
    } else {
        LOG("管线缓存：没有可用的旧缓存（%s）——这次从零编译，退出时写一份", path);
    }
    free(blob);
    if (!g_pcache_atexit) { g_pcache_atexit = 1; atexit(pcache_exit_save); }
}

// 取数据并写回。**先写 .tmp 再改名**：进程被系统直接杀掉时（ANR/OOM）不会留下一份半截的缓存，下次启动照着它建
// 缓存只会把错误带进下一轮。切后台（TERM_WINDOW）与退出（DESTROY/atexit）各调一次。
// Fetch the data and write it back. It goes to a .tmp file first and is renamed: when the system kills the process
// outright (ANR/OOM) no half-written cache is left behind, which would otherwise carry the damage into the next run.
// It is called on background (TERM_WINDOW) and on exit (DESTROY/atexit).
void vk_device_pcache_save(const char *why) {
    if (!g_dev || !g_pcache || g_pcache_off) return;
    const char *path = pcache_path();
    if (!path) return;
    vk_dev_pcache_lock();
    size_t n = 0;
    VkResult r = vkGetPipelineCacheData(g_dev, g_pcache, &n, NULL);
    if (r != VK_SUCCESS || n == 0 || n > PC_MAX) {
        vk_dev_pcache_unlock();
        LOG("管线缓存：取出数据为空（%zu 字节，VkResult %d；%s）——还没建过管线就退了？", n, (int)r, why);
        return;
    }
    void *blob = malloc(n);
    if (!blob) { vk_dev_pcache_unlock(); LOG("【黄】管线缓存取不出来：%zu 字节的缓冲分配不出", n); return; }
    r = vkGetPipelineCacheData(g_dev, g_pcache, &n, blob);
    vk_dev_pcache_unlock();
    if (r != VK_SUCCESS) { free(blob); LOG("【黄】管线缓存取不出来：VkResult %d（%s）", (int)r, why); return; }

    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(g_phys, &p);
    pcache_hdr h;
    memset(&h, 0, sizeof h);
    h.magic = PC_MAGIC; h.hdr_ver = PC_HDR_VER;
    h.vendor_id = p.vendorID; h.device_id = p.deviceID;
    h.driver_version = p.driverVersion; h.api_version = p.apiVersion;
    memcpy(h.uuid, p.pipelineCacheUUID, VK_UUID_SIZE);
    h.blob_size = (uint32_t)n;

    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(blob); LOG("【黄】管线缓存写不进 %s：%s", tmp, strerror(errno)); return; }
    int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(blob, 1, n, f) == n && fflush(f) == 0;
    fclose(f);
    free(blob);
    if (!ok) { LOG("【黄】管线缓存写了一半（%s）：丢掉，不留半截", tmp); remove(tmp); return; }
    if (rename(tmp, path) != 0) { LOG("【黄】管线缓存落位失败（%s → %s）：%s", tmp, path, strerror(errno)); remove(tmp); return; }
    g_pcache_saved_n = n;
    char sz[32];
    pc_size(sz, sizeof sz, n);
    LOG("【绿】管线缓存：写出 %s 到 %s（%s）", sz, path, why);
}

// 前台定期写回：每秒调一次，**数据长过就写**（没长过就什么都不做），且限频 10 秒。
// 为什么必须有这条：真机上"切后台"这条路走不通 —— MIUI 把这个包按「游戏」处理（logcat 里
// `ANDR-PWR-OPT: GAMEPOWEROPT: isGame() ai.d2rm.dm is Game = 1`），退到后台时系统直接
// `kill: send 9`（SIGKILL），`TERM_WINDOW` 送不到我们手里，所以退出/切后台那两个钩子一次都不会跑
// （实测两轮都是：pid 在 6 秒内消失、日志里没有 `窗口那一套已拆`）。前台定期写回是唯一能落盘的时机。
// 代价：每次写几 MB（`.tmp` + 改名），限频 10 秒、且只在"新管线建过"时才写 —— 管线主要在启动与首次
// 进场景时新增，所以实际写入很集中，不会一直写盘。
// Periodic write-back while in the foreground: called once a second, it writes **only when the data has grown**
// (otherwise it does nothing) and at most once every 10 seconds.
// Why it is mandatory: on the real device the "background" route does not work -- MIUI classifies this package as a
// game (`ANDR-PWR-OPT: GAMEPOWEROPT: isGame() ai.d2rm.dm is Game = 1` in logcat) and kills it outright with
// `kill: send 9` (SIGKILL) when it goes to the background, so `TERM_WINDOW` never reaches us and neither exit nor
// background hook ever runs (measured twice: the pid disappears within 6 seconds and the log holds no
// "窗口那一套已拆"). The foreground tick is the only chance to get the blob onto disk.
// What it costs: a few MB per write (a `.tmp` file plus a rename), rate-limited to once every 10 seconds and only
// when new pipelines have appeared -- new pipelines cluster around startup and the first scene, so the writes stay
// concentrated rather than continuous.
#define PC_SAVE_MIN_INTERVAL 10.0
void vk_device_pcache_tick(void) {
    if (!g_dev || !g_pcache || g_pcache_off) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    if (now - g_pcache_last_save < PC_SAVE_MIN_INTERVAL) return;
    vk_dev_pcache_lock();
    size_t n = 0;
    VkResult r = vkGetPipelineCacheData(g_dev, g_pcache, &n, NULL);
    vk_dev_pcache_unlock();
    if (r != VK_SUCCESS || n == 0 || n == g_pcache_saved_n) return;   // 没长过 = 没建新管线，不必写 | unchanged means no new pipeline: nothing to write
    g_pcache_last_save = now;
    vk_device_pcache_save("前台新建了管线");
}

VkInstance vk_instance(void) { return g_inst; }
VkDevice vk_dev(void) { return g_dev; }
VkQueue vk_queue(void) { return g_queue; }
uint32_t vk_queue_family(void) { return g_qfi; }
// 物理设备：给需要查"设备限制/格式能力"的层用（目前是 texel buffer 的偏移对齐，见 vk_tex.c）。
// The physical device: for layers that need to query device limits or format support (currently the texel buffer's
// offset alignment, see vk_tex.c).
VkPhysicalDevice vk_phys(void) { return g_phys; }

// 队列的外部同步（说明见 vk_device.h）：一把进程内的锁，提交 / 等队列空闲 / 取图像 / 上屏都走它。
// 做成普通互斥量即可：临界区里只有一次驱动调用，不在里面做别的活。
// The queue's external synchronisation (see vk_device.h): one process-wide lock taken by submit / queue-idle waits /
// image acquisition / present. A plain mutex is enough -- the critical section holds a single driver call and nothing else.
static VkQueue g_queue_win;                    // 窗口那条队列（见 vk_device.h）；为 VK_NULL_HANDLE 时退回主队列 | the window queue (see vk_device.h); falls back to the main queue when VK_NULL_HANDLE
VkQueue vk_queue_win(void) { return g_queue_win ? g_queue_win : g_queue; }

static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_qholder;                  // 持锁线程的 tid（0 = 无人持有）| tid of the lock holder (0 = free)
static const char *volatile g_qphase;          // 它打算干什么（见 vk_queue_note）| what it is about to do (see vk_queue_note)
void vk_queue_note(const char *phase) { g_qphase = phase; }
void vk_queue_lock(void) {
    pthread_mutex_lock(&g_qmu);
    atomic_store(&g_qholder, (int)pthread_gettid_np(pthread_self()));
}
void vk_queue_unlock(void) {
    atomic_store(&g_qholder, 0);
    pthread_mutex_unlock(&g_qmu);
}
int vk_queue_lock_state(char *buf, size_t cap) {
    int h = atomic_load(&g_qholder);
    snprintf(buf, cap, h ? "持有者 tid %d（%s）" : "无人持有", h, g_qphase ? g_qphase : "?");
    return h != 0;
}
const VkPhysicalDeviceMemoryProperties *vk_mem_props(void) { return &g_mem; }
uint32_t vk_zero_copy_type(void) { return g_zero_copy; }

void vk_device_set_window(void *w) { g_window = w; }
// 窗口换代（TERM_WINDOW 之后再 INIT_WINDOW）：旧的 surface 必须销毁，否则第二次拿到的窗口和旧 surface 对不上。
// 实例已经建好时，这里就地（重）建 surface —— 因为 vk_device_init 只会跑一次（g_tried），不能指望它再来一遍。
// A new window (INIT_WINDOW after TERM_WINDOW): the old surface must be destroyed, or the second window will not match
// it. When the instance already exists the surface is (re)created right here, since vk_device_init only ever runs once
// (g_tried) and cannot be relied on to do it again.
void vk_device_drop_surface(void) {
    if (g_surf) { vkDestroySurfaceKHR(g_inst, g_surf, NULL); g_surf = VK_NULL_HANDLE; }
    g_window = NULL;
    g_present_fam = -1;
}
void vk_device_make_surface(void) {
    if (!g_inst || !g_window || g_surf) return;
    VkAndroidSurfaceCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                          .window = (ANativeWindow *)g_window };
    if (vkCreateAndroidSurfaceKHR(g_inst, &sci, NULL, &g_surf) != VK_SUCCESS) {
        LOG("【红】由 ANativeWindow 建 surface 失败：窗口那一条退掉，其余照旧");
        g_window = NULL;
    } else LOG("[vk] 界面就绪：surface 建好（窗口 %p）", g_window);
}
void *vk_device_window(void) { return g_window; }
VkSurfaceKHR vk_dev_surface(void) { return g_surf; }
int vk_dev_present_family(void) { return g_present_fam; }

int vk_have_ext(const char *name) {
    for (uint32_t i = 0; i < g_nexts; i++) if (!strcmp(g_exts[i].extensionName, name)) return 1;
    return 0;
}

int vk_have_dyn_render(void) { return g_dyn_render; }
int vk_have_fill_non_solid(void) { return g_fill_non_solid; }
int vk_have_attr_divisor(void) { return g_attr_divisor; }
int vk_have_attr_divisor_zero(void) { return g_attr_divisor_zero; }
int vk_have_anisotropy(void) { return g_aniso; }

// 时间戳查询的两个读数（GPU 计时用）。有效位数按**本队列族**取——设备级属性里没有这一项，
// 而一个族支持不代表别的族支持。0 表示这个族测不了，调用方据此关掉计时并说清原因。
// The two readings behind timestamp queries (for GPU timing). The valid-bit count is taken **for this queue
// family**: it is not a device-level property and one family supporting it does not mean another does. Zero means
// this family cannot measure it, and the caller turns timing off and says why.
static int g_ts_bits = -1, g_ts_cached;
static double g_ts_period;
int vk_ts_bits(void) {
    if (g_ts_bits >= 0) return g_ts_bits;
    g_ts_bits = 0;
    vk_device_init();
    if (!g_dev) return 0;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, NULL);
    VkQueueFamilyProperties *qf = nq ? calloc(nq, sizeof *qf) : NULL;
    if (qf) {
        vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, qf);
        if (g_qfi < nq) g_ts_bits = (int)qf[g_qfi].timestampValidBits;
        free(qf);
    }
    return g_ts_bits;
}
double vk_ts_period_ns(void) {
    if (g_ts_cached) return g_ts_period;
    g_ts_cached = 1;
    vk_device_init();
    if (!g_dev) return 0;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(g_phys, &p);
    g_ts_period = (double)p.limits.timestampPeriod;
    return g_ts_period;
}

// 安卓的桩库只到 1.1：1.3 的入口从设备上取（见 vk_device.h 的说明）
// Android's stub library stops at 1.1, so the 1.3 entry points are fetched from the device (see vk_device.h)
PFN_vkCmdBeginRendering dm_vkCmdBeginRendering;
PFN_vkCmdEndRendering dm_vkCmdEndRendering;

uint32_t vk_memory_type(VkMemoryPropertyFlags want, uint32_t need_bits) {
    // 先要「want 全都要满足」的；退一步只看 need_bits 里第一个可用的
    // First look for one satisfying all of want; failing that, take the first type allowed by need_bits
    for (uint32_t i = 0; i < g_mem.memoryTypeCount; i++)
        if ((!need_bits || (need_bits & (1u << i))) && (g_mem.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < g_mem.memoryTypeCount; i++)
        if (need_bits & (1u << i)) return i;
    return UINT32_MAX;
}

void vk_device_init(void) {
    if (g_tried) return;
    g_tried = 1;

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "dmhost-android",
                             .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    // 有窗口就开安卓 surface 那两个实例扩展（**必须在建实例时就开**，所以窗口要在 init 之前告诉它）
    // With a window, the two Android surface instance extensions are enabled (they **must** be enabled at instance
    // creation, which is why the window has to be handed over before init)
    const char *iext[4]; uint32_t niec = 0;
    if (g_window) { iext[niec++] = "VK_KHR_surface"; iext[niec++] = "VK_KHR_android_surface"; }
    ici.enabledExtensionCount = niec;
    ici.ppEnabledExtensionNames = niec ? iext : NULL;
    VkResult r = vkCreateInstance(&ici, NULL, &g_inst);
    if (r != VK_SUCCESS) { LOG("【红】建 Vulkan 实例失败：VkResult %d", (int)r); g_inst = VK_NULL_HANDLE; return; }

    vk_device_make_surface();   // 有窗口就建 surface（同一个函数也用于"实例已在、窗口刚设好"那种情况）| build the surface when there is a window (the same function serves the "instance exists, window just set" case)

    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(g_inst, &n, NULL) != VK_SUCCESS || !n) { LOG("【红】没有 Vulkan 设备"); return; }
    VkPhysicalDevice *dv = calloc(n, sizeof *dv);
    vkEnumeratePhysicalDevices(g_inst, &n, dv);
    g_phys = dv[0];
    free(dv);

    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(g_phys, &p);
    vkGetPhysicalDeviceMemoryProperties(g_phys, &g_mem);

    // 设备扩展表先查出来（开扩展前要确认存在）| look the device extension list up first (an extension must exist before it is enabled)
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &g_nexts, NULL);
    g_exts = calloc(g_nexts, sizeof *g_exts);
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &g_nexts, g_exts);

    // 队列族：挑有图形位的（真机上同一个族同时有计算与传输）
    // Queue family: take the one with the graphics bit (on the real device the same family also has compute and transfer)
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, qf);
    g_qfi = UINT32_MAX;
    // 有窗口就先找「既能画又能上屏」的族（真机上是同一个）；找不到不硬上，退回图形族并说清代价
    // With a window, look for the family that can both draw and present first (the same one on the device); if there is
    // none it does not force it, falling back to the graphics family and saying what that costs
    if (g_surf) {
        for (uint32_t i = 0; i < nq; i++) {
            VkBool32 pres = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g_phys, i, g_surf, &pres);
            if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pres) { g_qfi = i; g_present_fam = (int)i; break; }
        }
        if (g_qfi == UINT32_MAX) LOG("【黄】没有「既能画又能上屏」的队列族：交换链会建不出来");
    }
    if (g_qfi == UINT32_MAX) for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_qfi = i; break; }
    free(qf);
    if (g_qfi == UINT32_MAX) { LOG("【红】没有图形队列族"); return; }

    const char *want[] = { "VK_KHR_external_memory_fd",        // 内存换轨（把 Vulkan 内存摆到客体地址上）要用 | needed by the memory overlay (placing Vulkan memory at guest addresses)
                           "VK_EXT_vertex_attribute_divisor" };   // 顶点流每实例步进 > 1（有则开，没有就退化，见第 9 步的已知缺口）| per-instance vertex stream step > 1 (enabled when present, degraded when not; see the known gaps in step 9)
    const char *en[8]; uint32_t nec = 0;
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++)
        if (vk_have_ext(want[i])) en[nec++] = want[i];
        else if (i == 0) LOG("【红】设备没有扩展 %s：内存换轨会走不通", want[i]);
        else LOG("【黄】设备没有扩展 %s：顶点流的每实例步进 > 1 只能按 1 处理", want[i]);

    // 动态渲染（Vulkan 1.3 核心）：回放层开渲染通道用它——比 VkRenderPass/VkFramebuffer 少一整套对象，
    // 而 NVN 的命令流本来就是"设目标 + 清除 + 绘制"的自由序列，与它正好对得上。
    // 先问再开：不支持就让回放层退回"只记状态不开通道"，不要把设备创建搞失败。
    // Dynamic rendering (core in Vulkan 1.3): the replay layer opens passes with it -- a whole set of objects fewer
    // than VkRenderPass/VkFramebuffer, and NVN's command stream is already a free sequence of "set targets, clear,
    // draw", which matches it exactly. Ask before enabling: if it is unsupported the replay layer falls back to
    // "record state, open no pass" rather than failing device creation.
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vdf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT };
    int have_vd = vk_have_ext("VK_EXT_vertex_attribute_divisor");
    VkPhysicalDeviceFeatures core = { 0 };
    {
        VkPhysicalDeviceFeatures2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        void *tail = &f13;
        if (have_vd) { vdf.pNext = &f13; tail = &vdf; }   // 扩展不在链上时不挂它的结构 | the extension's struct is not chained when the extension is absent
        q.pNext = tail;
        vkGetPhysicalDeviceFeatures2(g_phys, &q);
        core = q.features;
    }
    g_dyn_render = f13.dynamicRendering ? 1 : 0;
    if (!g_dyn_render) LOG("【黄】设备不支持动态渲染（Vulkan 1.3）：渲染通道这一层退回只记状态");
    // 非实心多边形模式（线框）与顶点流的每实例步进：先问再开，没有就由管线层退化并报黄
    // Non-solid polygon modes (wireframe) and per-instance vertex stream steps: asked for before being enabled;
    // when absent the pipeline layer degrades and warns
    g_fill_non_solid = core.fillModeNonSolid ? 1 : 0;
    g_attr_divisor = have_vd && vdf.vertexAttributeInstanceRateDivisor ? 1 : 0;
    g_attr_divisor_zero = have_vd && vdf.vertexAttributeInstanceRateZeroDivisor ? 1 : 0;
    // 各向异性过滤：采样器要用（NVN 的 aniso 参数直接对应 Vulkan 的 maxAnisotropy），问一下再开
    // Anisotropic filtering: samplers need it (NVN's aniso maps straight to Vulkan's maxAnisotropy); ask first
    g_aniso = core.samplerAnisotropy ? 1 : 0;

    // 有 surface 就要开交换链扩展（同样先查再开：设备没有就别开，否则 vkCreateDevice 直接失败）
    // With a surface the swapchain extension is enabled (again looked up first: enabling one that is absent makes
    // vkCreateDevice fail outright)
    if (g_surf) {
        if (vk_have_ext("VK_KHR_swapchain")) en[nec++] = "VK_KHR_swapchain";
        else { LOG("【红】设备没有 VK_KHR_swapchain：交换链建不出来"); g_surf = VK_NULL_HANDLE; }
    }

    float prio = 1.0f;
    // 有窗口且这个族能给出两条队列就要两条：一条客体用、一条前端用（见 vk_device.h 里"为什么分开"）。
    // With a window, ask for two queues when the family has them: one for the guest, one for the front end (see the
    // "why separate" note in vk_device.h).
    uint32_t want_q = 1;
    if (g_surf) {
        uint32_t nq2 = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq2, NULL);
        VkQueueFamilyProperties *all = nq2 ? calloc(nq2, sizeof *all) : NULL;
        if (all) {
            vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq2, all);
            if (g_qfi < nq2 && all[g_qfi].queueCount >= 2) want_q = 2;
            free(all);
        }
    }
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = g_qfi, .queueCount = want_q, .pQueuePriorities = &prio };
    // 开特性：核心特性走 VkPhysicalDeviceFeatures2，扩展特性走各自的特性结构，一条链挂起来
    // Enabling features: core features ride VkPhysicalDeviceFeatures2 and extension features their own structures,
    // all chained together
    VkPhysicalDeviceFeatures2 feats = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    feats.features = core;
    feats.features.fillModeNonSolid = g_fill_non_solid ? VK_TRUE : VK_FALSE;
    feats.features.samplerAnisotropy = g_aniso ? VK_TRUE : VK_FALSE;
    f13.dynamicRendering = g_dyn_render ? VK_TRUE : VK_FALSE;
    {
        void *tail = &f13;
        if (have_vd) {
            vdf.vertexAttributeInstanceRateDivisor = g_attr_divisor ? VK_TRUE : VK_FALSE;
            vdf.vertexAttributeInstanceRateZeroDivisor = g_attr_divisor_zero ? VK_TRUE : VK_FALSE;
            vdf.pNext = &f13;
            tail = &vdf;
        }
        feats.pNext = tail;
    }
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &feats,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = nec,
                               .ppEnabledExtensionNames = en };
    r = vkCreateDevice(g_phys, &dci, NULL, &g_dev);
    if (r != VK_SUCCESS) { LOG("【红】建 Vulkan 逻辑设备失败：VkResult %d", (int)r); g_dev = VK_NULL_HANDLE; return; }
    vkGetDeviceQueue(g_dev, g_qfi, 0, &g_queue);
    if (want_q >= 2) {
        vkGetDeviceQueue(g_dev, g_qfi, 1, &g_queue_win);
        LOG("[vk] 两条队列：客体用 #0、前端（取图/上屏）用 #1（各自不需要串行化）");
    } else {
        LOG("【黄】这个队列族只给了一条队列：前端与客体共用它，必须靠 vk_queue_lock 串行化（两边会互相等）");
    }

    // 1.3 的入口从设备上取（桩库不导出，见 vk_device.h）。取不到就把动态渲染关掉，让渲染通道那一层退回只记状态。
    // Fetch the 1.3 entry points from the device (the stub library does not export them; see vk_device.h). If the
    // lookup fails, dynamic rendering is turned off and the pass layer falls back to recording state only.
    dm_vkCmdBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(g_dev, "vkCmdBeginRendering");
    dm_vkCmdEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(g_dev, "vkCmdEndRendering");
    if (g_dyn_render && (!dm_vkCmdBeginRendering || !dm_vkCmdEndRendering)) {
        g_dyn_render = 0;
        LOG("【黄】取不到 vkCmdBeginRendering / vkCmdEndRendering：渲染通道退回只记状态");
    }

    // 管线缓存：设备已经在了，可以按这台设备/这个驱动的来历去核对旧 blob（见本文件上面那段说明）
    // Pipeline cache: the device exists now, so an old blob can be checked against this device/driver's identity
    // (see the note further up in this file)
    pcache_boot(&p);

    // 零拷贝内存类型：DEVICE_LOCAL + HOST_VISIBLE + HOST_COHERENT（真机上是类型 4，heap 0 有 15 GB）
    // Zero-copy memory type: DEVICE_LOCAL + HOST_VISIBLE + HOST_COHERENT (type 4 on the real device, whose heap 0 has 15 GB)
    g_zero_copy = vk_memory_type(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);

    if (g_zero_copy == UINT32_MAX)
        LOG("【绿】Vulkan 设备就绪：%s（Vulkan %u.%u.%u）队列族 %u；零拷贝内存类型：无；扩展 fd=%s",
            p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
            VK_API_VERSION_PATCH(p.apiVersion), g_qfi, vk_have_ext("VK_KHR_external_memory_fd") ? "有" : "无");
    else
        LOG("【绿】Vulkan 设备就绪：%s（Vulkan %u.%u.%u）队列族 %u；零拷贝内存类型 %u；扩展 fd=%s；动态渲染 %s；非实心填充 %s；顶点每实例步进 %s%s",
            p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
            VK_API_VERSION_PATCH(p.apiVersion), g_qfi, g_zero_copy, vk_have_ext("VK_KHR_external_memory_fd") ? "有" : "无",
            g_dyn_render && dm_vkCmdBeginRendering ? "有" : "无", g_fill_non_solid ? "有" : "无",
            g_attr_divisor ? (g_attr_divisor_zero ? "有（含 0）" : "有") : "无", "");
    if (g_zero_copy == UINT32_MAX) LOG("【红】没有 DEVICE_LOCAL+HOST_VISIBLE+HOST_COHERENT 的内存类型：零拷贝内存池做不了");
}
