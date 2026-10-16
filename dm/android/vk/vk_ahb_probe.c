// M2 第五个能力探针（「M3-3d」之后的延伸）：AHardwareBuffer 这条路，在非 Adreno（Mali）上能不能走。
//
// 为什么有这一问（「M3-3d」的结论）：零拷贝内存池走的是 vk_region.c 的**甲案** —— 分配**可导出**的 Vulkan
// 内存 → 取 fd → `mmap(MAP_FIXED)` 把它压在客体该在的地址上。第二台是 MediaTek MT6896 / **Mali-G610 MC6**，
// 不透明 fd 两种能力都没有、dma-buf 只有 IMPORTABLE ⇒ **甲案在那台上不成立**，它还没跑到卡点 B 就死在
// 「建载体缓冲失败（256 MB）」→ 堆与别名区各 0 MB → `SVC 越出 alias 区` → `Break#1`。
// 同源的另一条路是 **AHardwareBuffer**（`VK_ANDROID_external_memory_android_hardware_buffer` **两台都有**）：
// AHB 自己就是 dma-buf，fd 可以 `mmap(MAP_FIXED)` 到客体地址，再把它导入 Vulkan 当 VkDeviceMemory 用。
//
// 这个探针**只问一问**（它是 `vk_region_find` 与整个池子用法的前提）：
//   导入 AHB 之后，能不能在**同一份内存上按偏移绑多块缓冲**？
// 顺带记（都是"问这一问"必须经过的读数）：
//   ① `AHardwareBuffer_allocate` 的尺寸上限与用法位；`AHardwareBuffer_isSupported` 怎么看；
//   ② `vkGetAndroidHardwareBufferPropertiesANDROID` 给的 allocationSize 与 memoryTypeBits；
//   ③ 同一份 allocation 上两个偏移各绑一块 buffer 的返回码；
//   ④ fd 从哪来 —— NDK 的**公开**头文件里没有取 fd 的入口（只有 send/recvHandleToUnixSocket），
//      所以两条路都量：`dlsym` 私有 `AHardwareBuffer_getNativeHandle`，与扫 `/proc/self/fd` 里新出现的 dmabuf；
//   ⑤ `mmap(MAP_FIXED)` 能不能落在指定地址（"不动客体地址空间布局"那一步的前提）。
//
// Why this question exists (the conclusion of M3-3d): the zero-copy pool uses approach A of vk_region.c --
// allocate **exportable** Vulkan memory, take its fd, and mmap(MAP_FIXED) it at the address the guest expects.
// Device 2 is a MediaTek MT6896 / **Mali-G610 MC6**: opaque fds have neither capability and dma-buf is only
// IMPORTABLE, so approach A does not exist there; it dies before reaching blocker B with "carrier buffer
// creation failed (256 MB)" -> heap and alias region both 0 MB -> "SVC outside the alias region" -> Break#1.
// The sibling route is **AHardwareBuffer** (`VK_ANDROID_external_memory_android_hardware_buffer`, present on
// **both** devices): an AHB is itself a dma-buf, its fd can be mmap(MAP_FIXED)'d at a guest address, and it can
// be imported into Vulkan as a VkDeviceMemory.
//
// This probe **asks one question** (the precondition of vk_region_find and of the whole pool usage):
//   after importing an AHB, can several buffers be bound at different offsets of **the same memory**?
// Along the way it records (all readings needed to ask that):
//   (1) AHARDWAREBUFFER size limits and usage bits; what AHardwareBuffer_isSupported says;
//   (2) the allocationSize and memoryTypeBits that vkGetAndroidHardwareBufferPropertiesANDROID reports;
//   (3) the return codes of binding two buffers at two offsets of one allocation;
//   (4) where the fd comes from -- the public NDK headers have no get-fd entry point (only
//       send/recvHandleToUnixSocket), so both routes are measured: dlsym of the private
//       AHardwareBuffer_getNativeHandle, and scanning /proc/self/fd for a newly appeared dmabuf;
//   (5) whether mmap(MAP_FIXED) lands at a chosen address (the precondition of "do not touch the guest layout").
//
// 跑法：`.work/probes.py build vk_ahb_probe`，再 `.work/probes.py run vk_ahb_probe`（两台都会跑一遍）。
// 可调：DM_AHB_SIZE=<MB> 主测试用多大（缺省 64）；DM_AHB_USAGE=<十进制> 自己点用法位；
//       DM_AHB_LADDER=1,64,256,... 换尺寸阶梯。
// Run: `.work/probes.py build vk_ahb_probe` then `.work/probes.py run vk_ahb_probe` (it walks every device).
// Knobs: DM_AHB_SIZE=<MB> for the main test (default 64); DM_AHB_USAGE=<decimal> to name the usage bits;
//        DM_AHB_LADDER=1,64,256,... to replace the size ladder.
//
// ⚠ **别在第二台（MTK/Mali）上随手跑大尺寸**：默认阶梯已经缩到 1/64/256 MB —— 「M3-3e」实测，
// 把 ~4 GB 的 AHB 铺成客体堆、再让客体用它，会让这台设备**内核 panic 重启**（三次）；而只做 3968 MB 的
// 分配本身当时没事。这条探针的尺寸阶梯只是"分配 + describe + 释放"，但**要不要往 1 GB 以上问，得先跟人确认**。
// ⚠ **Do not casually run large sizes on device 2 (MTK/Mali)**: the default ladder is now 1/64/256 MB -- M3-3e
// measured that turning ~4 GB of AHB into the guest heap and letting the guest use it makes that device **panic and
// reboot** (three times), while allocating 3968 MB as such was fine at the time. This probe's ladder only allocates,
// describes and releases, but **ask a human before asking about anything above 1 GB**.
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// 安卓专有的 Vulkan 类型（VkImportAndroidHardwareBufferInfoANDROID 等）只在定义了这个宏之后才声明 ——
// 与 vk_device.c 同一条规矩。
// Android-specific Vulkan types (VkImportAndroidHardwareBufferInfoANDROID and friends) are only declared when
// this macro is defined -- the same rule as in vk_device.c.
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>

#include <android/hardware_buffer.h>

#define MB (1ull << 20)

static void die(const char *what, VkResult r) {
    fprintf(stderr, "【红】%s 失败：VkResult %d\n", what, (int)r);
    exit(1);
}
#define VKOK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) die(#x, _r); } while (0)

static const char *vkres(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY(-1)";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY(-2)";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED(-3)";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST(-4)";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED(-11)";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE(-1000072003)";
    default: return "(其它)";
    }
}

// 私有符号的句柄布局：native_handle_t 的头三个字段是 version / numFds / numInts，之后是数据（fd 在前）。
// 这么写是为了**不引私有头文件**（NDK 没有 cutils/native_handle.h）。
// Layout of the private handle: native_handle_t starts with version / numFds / numInts followed by the data
// (fds first). Written this way to avoid pulling in a private header (the NDK has no cutils/native_handle.h).
struct nh_abi { int version; int numFds; int numInts; int data[1]; };
typedef const struct nh_abi *(*get_nh_fn)(const AHardwareBuffer *);

// ---- /proc/self/fd 的快照（找 AHB 的 dmabuf 用）| snapshot of /proc/self/fd (to find the AHB's dmabuf) ----
#define MAXFD 4096
struct fdsnap { int fd[MAXFD]; char tgt[MAXFD][80]; int n; };

static void fd_snapshot(struct fdsnap *s) {
    s->n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s->n < MAXFD) {
        int fd = atoi(e->d_name);
        if (fd <= 2) continue;
        char p[64], b[256];
        snprintf(p, sizeof p, "/proc/self/fd/%s", e->d_name);
        ssize_t k = readlink(p, b, sizeof b - 1);
        if (k <= 0) continue;
        b[k] = 0;
        s->fd[s->n] = fd;
        snprintf(s->tgt[s->n], sizeof s->tgt[0], "%s", b);
        s->n++;
    }
    closedir(d);
}

// 分配之后新出现、且指向 dmabuf 的那个 fd（-1 = 没找到）
// The fd that appeared after the allocation and points at a dmabuf (-1 = none found)
static int fd_find_new_dmabuf(const struct fdsnap *before, char *tgt_out, size_t cap) {
    static struct fdsnap after;      // 340 KB 上下：放静态区，别压栈 | ~340 KB: keep it off the stack
    fd_snapshot(&after);
    for (int i = 0; i < after.n; i++) {
        int seen = 0;
        for (int j = 0; j < before->n; j++) if (before->fd[j] == after.fd[i]) { seen = 1; break; }
        if (seen) continue;
        if (strstr(after.tgt[i], "dmabuf") || strstr(after.tgt[i], "dma-buf")) {
            snprintf(tgt_out, cap, "%s", after.tgt[i]);
            return after.fd[i];
        }
    }
    return -1;
}

static void mem_flags_name(VkMemoryPropertyFlags f, char *out, size_t n) {
    out[0] = 0;
    if (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)  strncat(out, "DEVICE_LOCAL|", n - strlen(out) - 1);
    if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)  strncat(out, "HOST_VISIBLE|", n - strlen(out) - 1);
    if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) strncat(out, "HOST_COHERENT|", n - strlen(out) - 1);
    if (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)   strncat(out, "HOST_CACHED|", n - strlen(out) - 1);
    if (!out[0]) strncat(out, "（无）", n - strlen(out) - 1);
}

// 候选用法位（用 isSupported 与真分配逐个问）：BLOB 缓冲要 GPU 位，但哪一位驱动才认只有实测说话。
// Candidate usage bits, asked one by one with isSupported and with a real allocation: a BLOB buffer wants a GPU
// bit, but which one the driver accepts is only settled by measurement.
struct usage_cand { const char *name; uint64_t bits; };
static const struct usage_cand USAGES[] = {
    { "CPU_READ_OFTEN|CPU_WRITE_OFTEN|GPU_DATA_BUFFER",
      AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER },
    { "CPU_READ_OFTEN|CPU_WRITE_OFTEN|GPU_SAMPLED_IMAGE",
      AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE },
    { "只有 CPU_READ_OFTEN|CPU_WRITE_OFTEN",
      AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN },
};
#define NUSAGE ((int)(sizeof USAGES / sizeof USAGES[0]))

static AHardwareBuffer_Desc blob_desc(uint32_t bytes, uint64_t usage) {
    AHardwareBuffer_Desc d;
    memset(&d, 0, sizeof d);
    d.width = bytes;            // BLOB 的"宽"就是字节数 | for a BLOB the "width" is the byte count
    d.height = 1;
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_BLOB;
    d.usage = usage;
    return d;
}

static long env_long(const char *name, long dflt) {
    const char *e = getenv(name);
    return (e && *e) ? strtol(e, NULL, 10) : dflt;
}

int main(void) {
    const char *e;

    // ---- 0. 实例 / 物理设备 / 设备（开 AHB 扩展）| (0) instance, physical device, device (AHB enabled) ----
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "dm-ahb-probe",
                             .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst = VK_NULL_HANDLE; VKOK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 0; VKOK(vkEnumeratePhysicalDevices(inst, &n, NULL));
    VkPhysicalDevice *dv = calloc(n, sizeof *dv); VKOK(vkEnumeratePhysicalDevices(inst, &n, dv));
    VkPhysicalDevice phys = dv[0];

    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(phys, &pp);
    printf("=== 设备 ===\n  %s（Vulkan %u.%u.%u、驱动 %u）\n", pp.deviceName,
           VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion), VK_VERSION_PATCH(pp.apiVersion),
           pp.driverVersion);

    // ---- ① 扩展清单与 AHB 句柄的支持情况（分配之前先问）| (1) extensions and AHB handle support ----
    uint32_t ne = 0; VKOK(vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, NULL));
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    VKOK(vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, ep));
    int have_ahb = 0, have_ycbcr = 0, have_ded = 0;
    printf("=== AHB 相关的设备扩展 ===\n");
    for (uint32_t i = 0; i < ne; i++) {
        if (!strcmp(ep[i].extensionName, "VK_ANDROID_external_memory_android_hardware_buffer")) have_ahb = 1;
        if (!strcmp(ep[i].extensionName, "VK_KHR_sampler_ycbcr_conversion")) have_ycbcr = 1;
        if (!strcmp(ep[i].extensionName, "VK_KHR_dedicated_allocation")) have_ded = 1;
        if (strstr(ep[i].extensionName, "android") || strstr(ep[i].extensionName, "external"))
            printf("  %s（修订 %u）\n", ep[i].extensionName, ep[i].specVersion);
    }
    printf("  ⇒ VK_ANDROID_external_memory_android_hardware_buffer %s；VK_KHR_sampler_ycbcr_conversion %s；"
           "VK_KHR_dedicated_allocation %s（共 %u 个扩展）\n",
           have_ahb ? "有" : "**没有**", have_ycbcr ? "有" : "没有", have_ded ? "有" : "没有", ne);
    if (!have_ahb) {
        printf("⇒ 这条路不成立：这台设备没有 AHB 导入扩展\n");
        return 3;
    }

    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
    const char *en[2]; uint32_t nec = 0;
    en[nec++] = "VK_ANDROID_external_memory_android_hardware_buffer";
    if (have_ycbcr) en[nec++] = "VK_KHR_sampler_ycbcr_conversion";
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci, .enabledExtensionCount = nec, .ppEnabledExtensionNames = en };
    VkDevice dev = VK_NULL_HANDLE;
    VkResult dr = vkCreateDevice(phys, &dci, NULL, &dev);
    if (dr != VK_SUCCESS) {
        printf("【红】建设备失败：VkResult %d（%s）—— 试只开 AHB 扩展\n", (int)dr, vkres(dr));
        nec = 1; dr = vkCreateDevice(phys, &dci, NULL, &dev);
    }
    if (dr != VK_SUCCESS) { printf("【红】建设备还是失败：VkResult %d\n", (int)dr); return 3; }
    VkQueue q = VK_NULL_HANDLE; vkGetDeviceQueue(dev, qfi, 0, &q);
    printf("  设备建好：队列族 %u，开了 %u 个扩展\n", qfi, nec);

    VkPhysicalDeviceExternalBufferInfo ebi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID };
    VkExternalBufferProperties ebp = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
    vkGetPhysicalDeviceExternalBufferProperties(phys, &ebi, &ebp);
    VkExternalMemoryFeatureFlags feat = ebp.externalMemoryProperties.externalMemoryFeatures;
    printf("=== 分配之前先问（VkPhysicalDeviceExternalBufferInfo，handleType = AHB）===\n");
    printf("  能力：%s%s%s；compatibleHandleTypes %#x\n",
           (feat & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) ? "DEDICATED_ONLY|" : "",
           (feat & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ? "EXPORTABLE|" : "",
           (feat & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? "IMPORTABLE|" : "（都不支持）",
           ebp.externalMemoryProperties.compatibleHandleTypes);
    int importable = (feat & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
    int dedicated_only = (feat & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;

    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    printf("=== 这台设备的内存类型 ===\n");
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        char fn[160];
        mem_flags_name(mp.memoryTypes[i].propertyFlags, fn, sizeof fn);
        printf("  类型 %u：%s heap %u\n", i, fn, mp.memoryTypes[i].heapIndex);
    }

    // ---- ② AHB 侧：isSupported 与尺寸阶梯 | (2) AHB side: isSupported and the size ladder ----
    printf("=== AHardwareBuffer：尺寸阶梯（每次分配后 describe 一次，然后释放）===\n");
    long ladder[16]; int nl = 0;
    e = getenv("DM_AHB_LADDER");
    if (e && *e) {
        char tmp[256]; snprintf(tmp, sizeof tmp, "%s", e);
        char *save = NULL;
        for (char *t = strtok_r(tmp, ",", &save); t && nl < 16; t = strtok_r(NULL, ",", &save))
            ladder[nl++] = strtol(t, NULL, 10);
    } else {
        // 默认阶梯**只到 256 MB**：第一台实测 —— 512 MB 那一步失败之后，同一个进程连 64 MB 都分不出来了
        // （rc=-129）⇒ 一次失败会把本进程搞坏，默认阶梯不去踩它。要量"最大尺寸"就单独跑一次、从大往小问：
        // `DM_AHB_LADDER=3968,2048,1024,512,256`。
        // The default ladder stops at 256 MB: measured on device 1, after the 512 MB step failed the same process
        // could no longer allocate even 64 MB (rc=-129), i.e. one failure poisons the process. To measure the real
        // size limit, run once with DM_AHB_LADDER=3968,2048,1024,512,256 (largest first).
        static const long d[] = { 1, 64, 256 };
        for (size_t i = 0; i < sizeof d / sizeof d[0]; i++) ladder[nl++] = d[i];
    }
    uint64_t usage0 = USAGES[0].bits;
    e = getenv("DM_AHB_USAGE");
    if (e && *e) usage0 = strtoull(e, NULL, 0);
    for (int i = 0; i < NUSAGE; i++) {
        AHardwareBuffer_Desc d = blob_desc(4096, USAGES[i].bits);
        printf("  isSupported(%s) = %d\n", USAGES[i].name, AHardwareBuffer_isSupported(&d));
    }
    long max_ok_mb = 0; int stopped = 0;
    for (int i = 0; i < nl; i++) {
        AHardwareBuffer_Desc d = blob_desc((uint32_t)(ladder[i] * MB), usage0);
        AHardwareBuffer *a = NULL;
        errno = 0;
        int rc = AHardwareBuffer_allocate(&d, &a);
        if (rc == 0 && a) {
            AHardwareBuffer_Desc got; AHardwareBuffer_describe(a, &got);
            printf("  %5ld MB → 成功（describe：宽 %u、高 %u、层 %u、格式 %#x、用法 %#llx）\n",
                   ladder[i], got.width, got.height, got.layers, got.format,
                   (unsigned long long)got.usage);
            max_ok_mb = ladder[i];
            AHardwareBuffer_release(a);
        } else {
            printf("  %5ld MB → 失败（rc=%d，errno %d：%s）\n", ladder[i], rc, errno, strerror(errno));
            stopped = 1;
            break;
        }
    }
    printf("  ⇒ 用这份用法位，最大成功尺寸 = %ld MB%s\n", max_ok_mb, stopped ? "（阶梯在此中止）" : "（阶梯走完）");
    if (stopped)
        printf("  ⚠ 阶梯里出现过一次失败：**同一个进程之后的小分配可能也跟着失败**（第一台实测 512 MB 失败后连"
               " 64 MB 都分不出来）⇒ 下面主测试若也失败，先怀疑这一条，换 DM_AHB_LADDER 单独复量\n");
    if (!max_ok_mb) { printf("⇒ 这条路不成立：AHardwareBuffer 一个尺寸都分不出来\n"); return 4; }

    // ---- ③ 主测试：分配 → 取 fd → mmap 固定地址 → 导入 Vulkan → 两个偏移各绑一块 buffer ----
    // ---- (3) the main test: allocate -> get the fd -> mmap at a fixed address -> import -> bind at two offsets ----
    long probe_mb = env_long("DM_AHB_SIZE", 64);
    if ((uint64_t)probe_mb > max_ok_mb) probe_mb = (long)max_ok_mb;
    size_t wsz = (size_t)(probe_mb * MB);
    const size_t chunk = 1 * MB;                                   // 每块缓冲 1 MB（偏移 0 与 1 MB）
    printf("=== 主测试：%ld MB（DM_AHB_SIZE）===\n", probe_mb);

    struct fdsnap before; fd_snapshot(&before);                    // 先照一张 fd 表，好认出 AHB 的 dmabuf
    AHardwareBuffer_Desc bd = blob_desc((uint32_t)wsz, usage0);
    AHardwareBuffer *ahb = NULL;
    int arc = AHardwareBuffer_allocate(&bd, &ahb);
    if (arc != 0 || !ahb) { printf("【红】AHB 分配失败 rc=%d errno %d\n", arc, errno); return 4; }
    AHardwareBuffer_Desc got; AHardwareBuffer_describe(ahb, &got);
    printf("  AHB 分配成功：请求 %zu MB → 实得 宽 %u 字节、高 %u、层 %u、格式 %#x、用法 %#llx\n",
           wsz >> 20, got.width, got.height, got.layers, got.format, (unsigned long long)got.usage);
    size_t ahb_bytes = (size_t)got.width * (got.height ? got.height : 1) * (got.layers ? got.layers : 1);

    // fd 从哪来：公开 NDK 头文件里**没有**取 fd 的入口，所以两条路都量：
    //   ① 私有符号 AHardwareBuffer_getNativeHandle —— 句柄里的**每一个** fd 都列出来（**不能只看第一个**：
    //      第一台与第二台的 fd[0] 都是 `anon_inode:gralloc_extra`，不是 dma-buf）；
    //   ② 扫 /proc/self/fd 里分配之后新出现的 dmabuf。
    // 然后拿**每一条候选**去试 mmap(MAP_FIXED)：哪一条能映射、能不能落在指定地址，由 mmap 说话。
    // Where the fd comes from: the public NDK headers have **no** get-fd entry point, so both routes are measured:
    //   (1) the private AHardwareBuffer_getNativeHandle -- **every** fd in the handle is listed (do not look only at
    //       the first: on both real devices fd[0] is anon_inode:gralloc_extra, not the dma-buf);
    //   (2) a scan of /proc/self/fd for dmabufs that appeared after the allocation.
    // Every candidate is then tried with mmap(MAP_FIXED): which one maps, and whether it lands at the chosen
    // address, is settled by mmap itself.
    int cand[16]; int ncand = 0; char candwhy[16][110];
    get_nh_fn gnh = (get_nh_fn)dlsym(RTLD_DEFAULT, "AHardwareBuffer_getNativeHandle");
    if (gnh) {
        const struct nh_abi *nh = gnh(ahb);
        if (nh && nh->numFds > 0) {
            printf("  dlsym(\"AHardwareBuffer_getNativeHandle\") 可用：numFds=%d、numInts=%d\n", nh->numFds, nh->numInts);
            for (int i = 0; i < nh->numFds && ncand < 16; i++) {
                char p[64], b[256];
                snprintf(p, sizeof p, "/proc/self/fd/%d", nh->data[i]);
                ssize_t k = readlink(p, b, sizeof b - 1);
                if (k > 0) { b[k] = 0; } else { snprintf(b, sizeof b, "(readlink 失败)"); }
                printf("    fd[%d] = %d → %s%s\n", i, nh->data[i], b,
                       (strstr(b, "dmabuf") || strstr(b, "dma-buf")) ? "   ← 这一条才是 dma-buf" : "");
                cand[ncand] = nh->data[i];
                snprintf(candwhy[ncand], sizeof candwhy[0], "句柄 fd[%d]（%s）", i, b);
                ncand++;
            }
        } else {
            printf("  dlsym 到了 AHardwareBuffer_getNativeHandle，但它没给出 fd（numFds=%d）\n", nh ? nh->numFds : -1);
        }
    } else {
        printf("  dlsym(\"AHardwareBuffer_getNativeHandle\") 取不到\n");
    }
    {
        char t[160]; int f = fd_find_new_dmabuf(&before, t, sizeof t);
        if (f >= 0) {
            printf("  /proc/self/fd 里新出现的 dmabuf：fd %d → %s\n", f, t);
            if (ncand < 16) {
                cand[ncand] = f;
                snprintf(candwhy[ncand], sizeof candwhy[0], "/proc/self/fd 新出现的 dmabuf（%s）", t);
                ncand++;
            }
        } else {
            printf("  /proc/self/fd 里没有新出现的 dmabuf\n");
        }
    }

    // mmap(MAP_FIXED)：客体地址空间那一步的前提 | mmap(MAP_FIXED): the precondition of the guest-address-space step
    printf("=== 摆到客体地址（mmap(MAP_FIXED)）===\n");
    void *want = mmap(NULL, ahb_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    void *m = MAP_FAILED; int mmap_ok = 0; int fd = -1;
    char fdhow[200] = "(没有可用候选)";
    if (want == MAP_FAILED) { printf("  【黄】预留地址失败：%s\n", strerror(errno)); want = NULL; }
    for (int i = 0; i < ncand && !mmap_ok && want; i++) {
        errno = 0;
        void *p = mmap(want, ahb_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, cand[i], 0);
        if (p == MAP_FAILED) {
            printf("  fd %d（%s）：mmap %zu MB 失败 —— %s\n", cand[i], candwhy[i], ahb_bytes >> 20, strerror(errno));
            continue;
        }
        if (p == want) {
            mmap_ok = 1; fd = cand[i]; m = want;
            snprintf(fdhow, sizeof fdhow, "%s", candwhy[i]);
            printf("  fd %d（%s）：%zu MB 落在**指定地址** %p\n", cand[i], candwhy[i], ahb_bytes >> 20, want);
        } else {
            printf("  fd %d（%s）：映射成了，但落在别处 %p（要求 %p）\n", cand[i], candwhy[i], p, want);
            munmap(p, ahb_bytes);
        }
    }
    if (!mmap_ok)
        printf("  ⇒ 没有一条候选能落在指定地址%s\n", ncand ? "" : "（连候选都没有）");

    // ---- 导入 Vulkan | import into Vulkan ----
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getprops =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
    if (!getprops) { printf("【红】取不到 vkGetAndroidHardwareBufferPropertiesANDROID\n"); return 3; }
    VkAndroidHardwareBufferPropertiesANDROID props = { .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
    VkResult r = getprops(dev, ahb, &props);
    if (r != VK_SUCCESS) { printf("【红】取 AHB 属性失败：%s\n", vkres(r)); return 4; }
    printf("  vkGetAndroidHardwareBufferPropertiesANDROID：allocationSize %llu MB、memoryTypeBits %#x\n",
           (unsigned long long)(props.allocationSize >> 20), props.memoryTypeBits);

    // 两块缓冲：同一份外部句柄，各自按偏移绑进同一份内存（这就是池子的用法）
    // Two buffers with the same external handle, each bound at an offset of the same memory (the pool usage)
    VkExternalMemoryBufferCreateInfo embci = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &embci, .size = chunk,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer b0 = VK_NULL_HANDLE, b1 = VK_NULL_HANDLE;
    r = vkCreateBuffer(dev, &bci, NULL, &b0);
    if (r != VK_SUCCESS) { printf("【红】建 AHB 外部缓冲失败：%s\n", vkres(r)); return 4; }
    r = vkCreateBuffer(dev, &bci, NULL, &b1);
    if (r != VK_SUCCESS) { printf("【红】建第二块 AHB 外部缓冲失败：%s\n", vkres(r)); return 4; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b0, &mr);
    printf("  外部缓冲要求：size %llu KB、alignment %llu，AHB 可用类型位 %#x（上面那份属性 %#x）\n",
           (unsigned long long)(mr.size >> 10), (unsigned long long)mr.alignment, mr.memoryTypeBits, props.memoryTypeBits);
    uint32_t mi = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(props.memoryTypeBits & (1u << i))) continue;
        char fn[160]; mem_flags_name(mp.memoryTypes[i].propertyFlags, fn, sizeof fn);
        printf("    候选类型 %u：%s\n", i, fn);
        if (mi == UINT32_MAX) mi = i;
    }
    if (mi == UINT32_MAX) { printf("【红】AHB 属性没给任何可用内存类型\n"); return 4; }
    printf("  用它导入：类型 %u\n", mi);

    VkImportAndroidHardwareBufferInfoANDROID imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
                                                     .buffer = ahb };
    VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = b0 };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
                                 .allocationSize = props.allocationSize, .memoryTypeIndex = mi };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult ir = vkAllocateMemory(dev, &mai, NULL, &mem);
    printf("  导入 AHB（不带 dedicated）：%s\n", vkres(ir));
    if (ir != VK_SUCCESS) {
        imp.pNext = &ded; mai.pNext = &imp;
        ir = vkAllocateMemory(dev, &mai, NULL, &mem);
        printf("  导入 AHB（链上 VkMemoryDedicatedAllocateInfo）：%s\n", vkres(ir));
        imp.pNext = NULL; mai.pNext = &imp;
    } else if (dedicated_only) {
        // 这台设备报的是 DEDICATED_ONLY，却接受了不带 dedicated 的导入 —— 值得记下来
        // The device reports DEDICATED_ONLY yet accepted an import without a dedicated allocation -- worth recording
        printf("    （注：能力位报了 DEDICATED_ONLY，却接受了不带 dedicated 的导入）\n");
    }
    if (ir != VK_SUCCESS) {
        printf("⇒ 结论：AHB **导不进来**（%s）⇒ 这条支路在本设备上不成立\n", vkres(ir));
        return 5;
    }

    // ---- ④ 关键一问：同一份内存上按偏移绑多块缓冲 | (4) the question: several buffers at offsets of one memory ----
    VkResult r0 = vkBindBufferMemory(dev, b0, mem, 0);
    size_t off2 = chunk;                                  // 偏移 1 MB（页对齐）| offset 1 MB (page aligned)
    VkResult r1 = (off2 + chunk <= props.allocationSize) ? vkBindBufferMemory(dev, b1, mem, off2)
                                                         : VK_ERROR_UNKNOWN;
    printf("=== 池子的用法：同一份 allocation 上按偏移绑多块缓冲 ===\n");
    printf("  偏移 0 处绑第一块（%llu KB）：%s\n", (unsigned long long)(chunk >> 10), vkres(r0));
    printf("  偏移 %llu KB 处绑第二块：%s%s\n", (unsigned long long)(off2 >> 10), vkres(r1),
           (off2 + chunk > props.allocationSize) ? "（这份 allocation 放不下第二块，没试）" : "");

    // 池子里的那些缓冲其实是**普通缓冲**（回放层建的，不带外部分配信息），按偏移绑进这份内存 ——
    // 所以还要问一句：普通缓冲的可用类型位里有没有这份 AHB 内存的类型？绑定成不成？
    // The pool's buffers are in fact **plain buffers** (created by the replay layer without external memory
    // information) bound at offsets into this memory, so one more question: is the AHB memory's type among the
    // plain buffer's allowed types, and does the binding succeed?
    VkBufferCreateInfo pbi2 = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = chunk,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer b2 = VK_NULL_HANDLE;
    uint32_t bits2 = 0; size_t off3 = off2 + chunk;
    VkResult cr2 = vkCreateBuffer(dev, &pbi2, NULL, &b2);
    VkResult rb2 = VK_ERROR_UNKNOWN;
    if (cr2 == VK_SUCCESS) {
        VkMemoryRequirements mr2p; vkGetBufferMemoryRequirements(dev, b2, &mr2p);
        bits2 = mr2p.memoryTypeBits;
        rb2 = (off3 + chunk <= props.allocationSize) ? vkBindBufferMemory(dev, b2, mem, off3) : VK_ERROR_UNKNOWN;
    }
    printf("  普通缓冲（不带外部分配信息）：可用类型位 %#x（%s AHB 那份类型 %u），偏移 %llu KB 处绑定：%s\n",
           bits2, (bits2 & (1u << mi)) ? "含" : "**不含**", mi,
           (unsigned long long)(off3 >> 10), cr2 == VK_SUCCESS ? vkres(rb2) : "（缓冲都没建出来）");
    int multi_ok = (r0 == VK_SUCCESS && r1 == VK_SUCCESS && rb2 == VK_SUCCESS);

    // ---- ⑤ 双向验证（有映射才做）| (5) two-way check (only when there is a mapping) ----
    int d1 = 0, d2 = 0, e1 = 0, e2 = 0, intact = 0;
    if (multi_ok && mmap_ok) {
        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi };
        VkCommandPool pool = VK_NULL_HANDLE; VKOK(vkCreateCommandPool(dev, &pci, NULL, &pool));
        VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer cb = VK_NULL_HANDLE; VKOK(vkAllocateCommandBuffers(dev, &cbai, &cb));
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        VkBufferCopy cp = { .size = chunk };

        VkBufferCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = chunk,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        VkBuffer rb = VK_NULL_HANDLE; VKOK(vkCreateBuffer(dev, &rci, NULL, &rb));
        VkMemoryRequirements rmr; vkGetBufferMemoryRequirements(dev, rb, &rmr);
        uint32_t rmi = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((rmr.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { rmi = i; break; }
        VkMemoryAllocateInfo rmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                      .allocationSize = rmr.size, .memoryTypeIndex = rmi };
        VkDeviceMemory rmem = VK_NULL_HANDLE; VKOK(vkAllocateMemory(dev, &rmai, NULL, &rmem));
        VKOK(vkBindBufferMemory(dev, rb, rmem, 0));
        void *rm = NULL; VKOK(vkMapMemory(dev, rmem, 0, chunk, 0, &rm));

        uint8_t *g0 = (uint8_t *)m, *g2 = (uint8_t *)m + off2;

        for (size_t i = 0; i < chunk; i++) g0[i] = (uint8_t)(i * 7 + 3);
        VKOK(vkBeginCommandBuffer(cb, &bi)); vkCmdCopyBuffer(cb, b0, rb, 1, &cp); VKOK(vkEndCommandBuffer(cb));
        VKOK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); VKOK(vkQueueWaitIdle(q));
        d1 = 1; for (size_t i = 0; i < chunk; i++) if (((uint8_t *)rm)[i] != (uint8_t)(i * 7 + 3)) { d1 = 0; break; }

        VKOK(vkResetCommandPool(dev, pool, 0));
        VKOK(vkBeginCommandBuffer(cb, &bi)); vkCmdFillBuffer(cb, b0, 0, chunk, 0x1F1F1F1Fu); VKOK(vkEndCommandBuffer(cb));
        VKOK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); VKOK(vkQueueWaitIdle(q));
        d2 = 1; for (size_t i = 0; i < chunk; i++) if (g0[i] != 0x1F) { d2 = 0; break; }

        for (size_t i = 0; i < chunk; i++) g2[i] = (uint8_t)(i * 11 + 9);
        VKOK(vkResetCommandPool(dev, pool, 0));
        VKOK(vkBeginCommandBuffer(cb, &bi)); vkCmdCopyBuffer(cb, b1, rb, 1, &cp); VKOK(vkEndCommandBuffer(cb));
        VKOK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); VKOK(vkQueueWaitIdle(q));
        e1 = 1; for (size_t i = 0; i < chunk; i++) if (((uint8_t *)rm)[i] != (uint8_t)(i * 11 + 9)) { e1 = 0; break; }

        VKOK(vkResetCommandPool(dev, pool, 0));
        VKOK(vkBeginCommandBuffer(cb, &bi)); vkCmdFillBuffer(cb, b1, 0, chunk, 0x6D6D6D6Du); VKOK(vkEndCommandBuffer(cb));
        VKOK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); VKOK(vkQueueWaitIdle(q));
        e2 = 1; for (size_t i = 0; i < chunk; i++) if (g2[i] != 0x6D) { e2 = 0; break; }
        intact = (g0[0] == 0x1F);      // 偏移 1 MB 那一块没把偏移 0 的数据冲掉

        printf("=== 双向 ===\n");
        printf("  偏移 0：客体→GPU %s、GPU→客体 %s\n", d1 ? "通" : "不通", d2 ? "通" : "不通");
        printf("  偏移 %llu KB：客体→GPU %s、GPU→客体 %s；偏移 0 的数据未被冲掉 %s\n",
               (unsigned long long)(off2 >> 10), e1 ? "通" : "不通", e2 ? "通" : "不通", intact ? "（是）" : "（否）");
        vkUnmapMemory(dev, rmem);
    } else if (multi_ok) {
        printf("=== 双向 ===\n  跳过：没有落在指定地址的映射（mmap 那一步没成）\n");
    } else {
        printf("=== 双向 ===\n  跳过：多偏移绑缓冲没成\n");
    }

    // ---- 结论 | conclusion ----
    printf("=== 结论 ===\n");
    printf("  AHB 走通的部分：分配（最大 %ld MB）%s、导入 Vulkan %s、同一份内存上按偏移绑多块缓冲 %s\n",
           max_ok_mb, max_ok_mb ? "成" : "不成", (ir == VK_SUCCESS) ? "成" : "不成", multi_ok ? "成" : "**不成**");
    if (fd >= 0) printf("  fd 拿到了（%s），mmap 到指定地址 %s\n", fdhow, mmap_ok ? "成" : "不成");
    else printf("  fd **没拿到**（公开 API 没有这个入口，私有符号与 /proc/self/fd 都没给）\n");
    if (multi_ok && mmap_ok && d1 && d2 && e1 && e2 && intact)
        printf("⇒ AHB 支路在**本设备上成立**：既能摆到客体地址，又能在同一份内存上按偏移绑多块缓冲（池子的用法）\n");
    else if (multi_ok)
        printf("⇒ AHB 能导入、也能多偏移绑缓冲，但「摆到客体地址」这一步%s（这就是能不能替甲案的分水岭）\n",
               mmap_ok ? "的双向有问题" : "没成");
    else
        printf("⇒ AHB 支路在**本设备上不成立**：导进来了也没法在同一份内存上按偏移绑多块缓冲 ⇒ 池子用法不成立，得走乙案\n");

    AHardwareBuffer_release(ahb);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    return (multi_ok && mmap_ok && d1 && d2 && e1 && e2 && intact) ? 0 : 2;
}
