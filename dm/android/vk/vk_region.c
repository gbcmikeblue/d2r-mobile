// 内存换轨：把客体地址空间的一段换成 Vulkan 零拷贝内存。两条路，先问再选：
//   甲案（保布局，**首选**）：分配**可导出**的 Vulkan 内存 → 取 fd → `mmap(MAP_FIXED)` 压回客体地址；
//   AHB 支路（保布局，甲案不可用时才走）：`AHardwareBuffer` 分配一块 GPU 可见内存 → 取它的 dma-buf fd →
//   同样 `mmap(MAP_FIXED)` 压回客体地址 → 再用 `vkImportAndroidHardwareBuffer` 把它交给 Vulkan。
// 两条路**都不动客体地址空间的布局**，所以 QueryMemory / GetInfo / vk_region_find 一概不变。
// AHB 支路的来历与实测见 docs/ANDROID_PLAN 的「M3-3d」与「M3-3e」：第二台是 Mali，不透明 fd 不可导出
// ⇒ 甲案在它上面不存在；AHB 支路在**两台**上都成立（探针 `vk_ahb_probe`，读数在 `.work/logs/probe-*`）。
// Memory overlay: turning a piece of the guest address space into Vulkan zero-copy memory. Two routes, asked for
// before being chosen:
//   A (keeps the layout, **preferred**): allocate **exportable** Vulkan memory, take its fd and mmap(MAP_FIXED) it
//     back at the guest address;
//   the AHB route (also keeps the layout, used only when A cannot be): allocate a GPU-visible AHardwareBuffer, take
//     its dma-buf fd, mmap(MAP_FIXED) that back at the guest address, then hand it to Vulkan with
//     vkImportAndroidHardwareBuffer.
// Neither route touches the guest address space layout, so QueryMemory / GetInfo / vk_region_find stay as they are.
// Where the AHB route came from and what was measured is in M3-3d and M3-3e of docs/ANDROID_PLAN: device 2 is a
// Mali, its opaque fds are not exportable so route A does not exist there, and the AHB route works on **both**
// devices (probe `vk_ahb_probe`, readings under `.work/logs/probe-*`).

// 安卓专有的 Vulkan 类型（VkImportAndroidHardwareBufferInfoANDROID 等）要这个宏才声明 ——
// 必须写在**任何**包含 vulkan.h 的头之前（vk_region.h 就包含它）。
// Android-specific Vulkan types (VkImportAndroidHardwareBufferInfoANDROID and friends) are only declared when this
// macro is defined, and it has to come **before** any header that includes vulkan.h (vk_region.h does).
#define VK_USE_PLATFORM_ANDROID_KHR 1

#include "vk_region.h"
#include "vk_device.h"
#include "dm.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <android/hardware_buffer.h>

// 真机上量出来的硬边界（docs/ANDROID_PLAN 的 M2 第 3 步）：单次分配 3968 MB 成、4096 MB 起 OOM。
// 顶到上限时按 128 MB 往下退，直到成功。
// The hard boundary measured on the real device (M2 step 3 in docs/ANDROID_PLAN): 3968 MB succeeds for a
// single allocation and 4096 MB and up fail with OOM. When it hits the limit, step down by 128 MB until it
// succeeds.
#define STEP_DOWN (128ull << 20)
#define MIN_USEFUL (256ull << 20)
#define MAX_REGION 8

static struct {
    uint8_t *base; size_t size; VkDeviceMemory mem; VkBuffer owner;
    // AHB 支路：这一段背后的 AHB 对象要**保活** —— VkDeviceMemory 是从它导入的，客体那份 mmap 也还指着它的
    // dma-buf。甲案没有这个字段（owner 就是那个载体缓冲）。
    // The AHB route: the AHardwareBuffer behind this region must be **kept alive** -- the VkDeviceMemory was
    // imported from it and the guest's mmap still points at its dma-buf. Route A has no such field (the carrier
    // buffer is the owner).
    AHardwareBuffer *ahb;
} g_reg[MAX_REGION];
static int g_nreg;
static uint64_t g_bytes;

// ---------------- 选路 ---------------- | ---------------- choosing the route ----------------
// 甲案要求"不透明 fd 可导出"。先在分配**之前**问一次（VkPhysicalDeviceExternalBufferInfo），
// 问不到就退 AHB 支路 —— 「M3-3d」实测第二台（Mali-G610）上不透明 fd 两种能力都没有、dma-buf 只有
// IMPORTABLE，甲案在那里根本不存在。
// Route A needs "opaque fds are exportable". That is asked **before allocating** (VkPhysicalDeviceExternalBufferInfo);
// when it is missing the AHB route is taken -- M3-3d measured on device 2 (Mali-G610) that opaque fds have neither
// capability and dma-buf is only IMPORTABLE, so route A simply does not exist there.
static int g_route = -1;   // -1 未定 / 0 甲案 / 1 AHB | -1 undecided / 0 route A / 1 AHB

static int opaque_fd_exportable(void) {
    VkPhysicalDeviceExternalBufferInfo bi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkExternalBufferProperties bp = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
    vkGetPhysicalDeviceExternalBufferProperties(vk_phys(), &bi, &bp);
    return (bp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
}

static int ahb_available(void) { return vk_have_ext("VK_ANDROID_external_memory_android_hardware_buffer"); }

// ⚠ **AHB 支路缺省关**（`DM_REGION_AHB=1` 才走）。为什么（2026-10-16 真机实测，见 docs/ANDROID_PLAN
// 「M3-3e」）：第二台（MT6896 / Mali-G610）上打开它以后，**内核 panic 重启**了三次 ——
// `persist.sys.boot.reason.history` 连记三条 `kernel_panic`（启动时刻 10:45:49 / 10:46:38 / 10:47:49），
// 分别对上"冒烟跑完"与"紧接着的两次 app 启动"；而同一台设备上**只做 3968 MB 的 AHB 分配**（探针那几轮）
// 一直没事 ⇒ 触发点不是"分配"，更像是"把 ~4 GB 的 AHB 铺成客体堆、客体开始用它"或那次全尺寸 mmap。
// 关着的时候第二台的行为回到原来的"启动受阻"（堆/别名区各 0 MB → SVC 越出 alias 区 → Break#1），不重启。
// `DM_REGION_AHB_MAX=<MB>` 给单次 AHB 分配加个上限，用来一步步逼近那个尺寸（分级复量用）。
// ⚠ **The AHB route is off by default** (`DM_REGION_AHB=1` turns it on). Why (measured on the real device,
// 2026-10-16, see M3-3e in docs/ANDROID_PLAN): with it on, device 2 (MT6896 / Mali-G610) had the **kernel panic and
// reboot** three times -- persist.sys.boot.reason.history recorded three kernel_panic entries (boots at 10:45:49 /
// 10:46:38 / 10:47:49) that line up with "the smoke run finished" and "the two app starts right after it", while on
// the same device **allocating 3968 MB of AHB alone** (those probe runs) was always fine -> the trigger is not the
// allocation but rather "turning ~4 GB of AHB into the guest heap and letting the guest use it", or that full-size
// mmap. With the route off, device 2 goes back to the original "start-up blocked" shape (heap and alias region both
// 0 MB -> SVC outside the alias region -> Break#1) and does not reboot. DM_REGION_AHB_MAX=<MB> caps a single AHB
// allocation so the size limit can be approached step by step (for the staged re-measurement).
static int ahb_enabled(void) {
    const char *e = getenv("DM_REGION_AHB");
    return e && *e && *e != '0';
}

static uint64_t ahb_max_bytes(void) {
    const char *e = getenv("DM_REGION_AHB_MAX");
    if (!e || !*e) return 0;
    long long mb = atoll(e);
    return mb > 0 ? (uint64_t)mb << 20 : 0;
}

// `DM_REGION_AHB_DRYRUN=1`：分配 / 取 fd / mmap / 导入**都做一遍**，然后原路撤掉、**返回 0**（不注册区域）。
// 用途：分离"分配 + mmap + 导入"与"客体真的去用这块内存"—— 复量第二台的内核 panic 时，这一步是判据。
// `DM_REGION_AHB_DRYRUN=1` does the whole allocate / take-fd / mmap / import sequence and then undoes it and
// **returns 0** without registering a region. It separates "allocate + mmap + import" from "the guest actually uses
// the memory" -- the discriminating measurement when pinning down device 2's kernel panic.
static int ahb_dryrun(void) {
    const char *e = getenv("DM_REGION_AHB_DRYRUN");
    return e && *e && *e != '0';
}

// `DM_REGION_AHB_TOUCH=<MB>`：把映射好的这段**宿主自己按页触一遍**（每页写一个字节再读回来），
// 每 128 MB 打一行进度，触完**原路撤掉、返回 0**（同 DRYRUN 的收尾，不交给客体）。
// 用途：把"客体真的去用"再分一层 —— 分离"**单纯 CPU 访问**这块大内存"与"客体/GPU 在用它"。
// 进度行是给"崩在半路"用的：内核 panic 会当场重启，只有崩之前已经写进日志的那几行留得下来。
// `DM_REGION_AHB_TOUCH=<MB>` has the **host itself touch the mapping page by page** (write a byte, read it back),
// logging progress every 128 MB, and then undo it all and **return 0** (the same tail as DRYRUN, so the guest never
// gets it). It splits "the guest actually uses it" one level further: "**plain CPU access** to this large memory"
// versus "the guest / the GPU using it". The progress lines are for the "it died half way" case: a kernel panic
// reboots on the spot, so only the lines already written to the log survive.
static uint64_t ahb_touch_bytes(void) {
    const char *e = getenv("DM_REGION_AHB_TOUCH");
    if (!e || !*e) return 0;
    long long mb = atoll(e);
    return mb > 0 ? (uint64_t)mb << 20 : 0;
}

// `DM_REGION_AHB_MEMSET=<MB>`：与 TOUCH 同一位置，但**用 `memset` 按 1 MB 一块清零**（每 128 MB 打一行进度），
// 再按页读回校验，最后同 DRYRUN 的收尾（撤掉、返回 0）。为什么单列一个：在 arm64 上 bionic 的 `memset`
// 大面积清零会走 **`DC ZVA`**（缓存块清零指令），而 GPU-only 的 AHB 映射很可能是 **Device / 非缓存**属性 ——
// "宿主逐页写读没事、客体一开跑就崩"这个形状，最想排除的就是这一类指令。
// `DM_REGION_AHB_MEMSET=<MB>` sits in the same place as TOUCH but **zeroes 1 MB at a time with `memset`** (logging
// progress every 128 MB), reads it back page by page and then takes the same DRYRUN tail (undo, return 0). It is a
// separate switch because on arm64 bionic's memset uses **`DC ZVA`** for large zeroing, and a GPU-only AHB mapping is
// very likely **Device / non-cached** memory -- the one class of instruction this "host page writes are fine, the
// guest dies as soon as it runs" shape most wants to rule out.
static uint64_t ahb_memset_bytes(void) {
    const char *e = getenv("DM_REGION_AHB_MEMSET");
    if (!e || !*e) return 0;
    long long mb = atoll(e);
    return mb > 0 ? (uint64_t)mb << 20 : 0;
}

// `DM_REGION_AHB_ATOMIC=<MB>`：与 TOUCH / MEMSET 同一位置，但每页做一次**原子加**（`__ATOMIC_SEQ_CST`，
// arm64 上编成 `ldaxr/stlxr` 独占访问），每 128 MB 打一行进度，最后同 DRYRUN 的收尾（撤掉、返回 0）。
// 为什么猜这个：客体一拿到堆就会用互斥 / 引用计数这类原子操作，而**独占访问落在 Device / 非可缓存内存上是
// 不可预测的**（这类内存连 LDXR/STXR 的语义都没有）。宿主那边的写、清零、读回都试过没事，剩下的差别里它最像。
// `DM_REGION_AHB_ATOMIC=<MB>` sits in the same place as TOUCH / MEMSET but does one **atomic add** per page
// (`__ATOMIC_SEQ_CST`, i.e. `ldaxr/stlxr` exclusive access on arm64), logging progress every 128 MB and taking the
// same DRYRUN tail. Why this guess: the guest's first act on a heap is mutexes / refcounts, i.e. atomics, and
// **exclusive access to Device / non-cached memory is unpredictable** (such memory does not even have LDXR/STXR
// semantics). Host writes, zeroing and read-back were all clean, and this is the most likely remaining difference.
static uint64_t ahb_atomic_bytes(void) {
    const char *e = getenv("DM_REGION_AHB_ATOMIC");
    if (!e || !*e) return 0;
    long long mb = atoll(e);
    return mb > 0 ? (uint64_t)mb << 20 : 0;
}

static int route(void) {
    if (g_route >= 0) return g_route;
    if (!opaque_fd_exportable() && ahb_available() && ahb_enabled()) {
        g_route = 1;
        LOG("[vk] 内存换轨选路：这台设备的**不透明 fd 不可导出**（甲案不成立）⇒ 走 AHB 支路");
    } else {
        g_route = 0;
        if (!opaque_fd_exportable() && ahb_available())
            LOG("【黄】内存换轨选路：甲案不成立、AHB 支路可用，但它**缺省关**（第二台上打开会让内核 panic，"
                "见 vk_region.c 的说明）⇒ 这次不走内存换轨，按普通内存继续（DM_REGION_AHB=1 才走 AHB）");
        else
            LOG("[vk] 内存换轨选路：走甲案（分配可导出内存 → 取 fd → mmap 压回客体地址）");
    }
    return g_route;
}

// AHB 的 dma-buf fd 怎么拿：NDK 的**公开**头文件里没有取 fd 的入口（只有 send/recvHandleToUnixSocket），
// 所以走私有符号 AHardwareBuffer_getNativeHandle（真机两台都导出它），再从**句柄里的每一条 fd** 里挑出
// 真 dma-buf —— 不能看 fd[0]：第一台的 fd[0] 是 dmabuf:AHardwareBuffer、第二台的 fd[0] 是
// anon_inode:gralloc_extra（映射它会被拒 EACCES），dma-buf 在 fd[1]。
// How the AHB's dma-buf fd is obtained: the public NDK headers have no get-fd entry point (only
// send/recvHandleToUnixSocket), so the private AHardwareBuffer_getNativeHandle is used (both real devices export it)
// and the real dma-buf is picked out of **every fd in the handle** -- looking only at fd[0] does not work: on
// device 1 fd[0] is dmabuf:AHardwareBuffer while on device 2 fd[0] is anon_inode:gralloc_extra (mapping that one is
// refused with EACCES) and the dma-buf is fd[1].
struct nh_abi { int version; int numFds; int numInts; int data[1]; };
typedef const struct nh_abi *(*ahb_get_nh_fn)(const AHardwareBuffer *);

static int ahb_dmabuf_fd(const AHardwareBuffer *a) {
    ahb_get_nh_fn f = (ahb_get_nh_fn)dlsym(RTLD_DEFAULT, "AHardwareBuffer_getNativeHandle");
    if (!f) return -1;
    const struct nh_abi *nh = f(a);
    if (!nh) return -1;
    for (int i = 0; i < nh->numFds; i++) {
        char p[64], b[128];
        snprintf(p, sizeof p, "/proc/self/fd/%d", nh->data[i]);
        ssize_t k = readlink(p, b, sizeof b - 1);
        if (k <= 0) continue;
        b[k] = 0;
        if (strstr(b, "dmabuf") || strstr(b, "dma-buf")) return nh->data[i];
    }
    return -1;
}

// 零拷贝内存类型要不要优先带 HOST_CACHED（「九」第 2 项）：DM_ZC_CACHED=1 开，缺省关 ——
// 缺省关是因为用它越过了缓冲声明的可用类型位（真机 0x13 里没有它），属于规范外用法，
// 先做 A/B，赢了再翻默认。
// Whether the zero-copy type should prefer HOST_CACHED (item 2 of "section nine"): DM_ZC_CACHED=1 turns it on and
// it is off by default -- off because using it steps outside the buffer's declared allowed types (0x13 on the real
// device does not include it), which is outside the spec, so the A/B comes first and the default is flipped only
// if it wins.
static int zc_cached_wanted(void) {
    const char *e = getenv("DM_ZC_CACHED");
    return e && *e && *e != '0';
}

// 这片「专属」缓冲只是导出句柄的载体：这种句柄标了 DEDICATED_ONLY，规范要求一份分配专属一块缓冲，
// 而不链它的话 vkGetMemoryFdKHR 会直接失败（探针里实测过）。池子的真身是后面按偏移绑进来的那些缓冲。
// 用法只给传输位：这载体不会被真的读写，而带上 STORAGE_BUFFER 会撞上驱动的 maxStorageBufferRange 上限
// （真机上实测：4 GB 的载体缓冲建不出来，见下面 vkCreateBuffer 失败时要往下退）。
// This "owner" buffer only exists to carry the export handle: the handle is marked DEDICATED_ONLY, the spec
// wants one allocation dedicated to one buffer, and vkGetMemoryFdKHR simply fails without it (measured in
// the probe). The pools themselves are the buffers bound at offsets into this allocation later on.
// Its usage is transfer-only: the carrier is never really read or written, and asking for STORAGE_BUFFER
// runs into the driver's maxStorageBufferRange limit (measured on the real device: a 4 GB carrier buffer
// cannot be created, hence the step-down on vkCreateBuffer failure below).
static VkBuffer make_owner(uint64_t size, uint32_t *type_bits) {
    *type_bits = 0;
    VkExternalMemoryBufferCreateInfo ext = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                             .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ext, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer b = VK_NULL_HANDLE;
    if (vkCreateBuffer(vk_dev(), &bci, NULL, &b) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk_dev(), b, &mr);
    *type_bits = mr.memoryTypeBits;
    return b;
}

// 甲案：分配可导出内存 → 取 fd → mmap(MAP_FIXED) 压回客体地址（详见文件头）。
// Route A: allocate exportable memory, take its fd, mmap(MAP_FIXED) it back at the guest address (see the file head).
static size_t overlay_export(void *at, size_t size, const char *what) {
    if (!vk_have_ext("VK_KHR_external_memory_fd")) {
        LOG("【红】%s 内存换轨：设备没有 VK_KHR_external_memory_fd，无法把 Vulkan 内存摆到客体地址上", what);
        return 0;
    }
    if (vk_zero_copy_type() == UINT32_MAX) { LOG("【红】%s 内存换轨：没有零拷贝内存类型", what); return 0; }

    PFN_vkGetMemoryFdKHR getfd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(vk_dev(), "vkGetMemoryFdKHR");
    if (!getfd) { LOG("【红】%s 内存换轨：取不到 vkGetMemoryFdKHR", what); return 0; }

    const size_t pg = (size_t)getpagesize();
    uint64_t try = size & ~(uint64_t)(pg - 1);          // 映射长度要整页 | the mapping length must be whole pages
    for (; try >= MIN_USEFUL; try -= STEP_DOWN) {
        uint32_t bits = 0;
        VkBuffer owner = make_owner(try, &bits);
        if (owner == VK_NULL_HANDLE) {
            // 建不出这么大的缓冲（真机上 4 GB 撞 maxStorageBufferRange）：退一档再试
            // The buffer cannot be created this large (4 GB hits maxStorageBufferRange on the real device):
            // step down and retry
            if (try > MIN_USEFUL) continue;
            LOG("【红】%s 内存换轨：建载体缓冲失败（%llu MB）", what, (unsigned long long)(try >> 20));
            return 0;
        }
        // 「九」第 2 项：DM_ZC_CACHED=1 时优先要"带 HOST_CACHED 的零拷贝类型"（真机上类型 6），客体每一次读写
        // 都走主机侧缓存。它不在缓冲声明的可用类型位里，所以分配失败要**回退类型 4**，而不是像撞上限那样退一档
        // 尺寸。不设这个开关时行为与原来完全一致。
        // Item 2 of "section nine": with DM_ZC_CACHED=1, prefer the zero-copy type that has HOST_CACHED (type 6 on
        // the real device), so every guest access goes through the host-side cache. It is not among the buffer's
        // declared allowed types, so an allocation failure must **fall back to type 4** rather than stepping the size
        // down (that step-down is only for hitting the size limit). Unset, the behaviour is exactly as before.
        uint32_t mi_std = vk_memory_type(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, bits);
        uint32_t mi = mi_std;
        int cached_used = 0;
        if (zc_cached_wanted()) {
            uint32_t c = vk_zero_copy_cached_type();
            if (c != UINT32_MAX && c != mi_std) { mi = c; cached_used = 1; }
        }
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vk_dev(), owner, &mr);

        VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = owner };
        VkExportMemoryAllocateInfo exp = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                           .pNext = &ded, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &exp,
                                     .allocationSize = mr.size, .memoryTypeIndex = mi };
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkResult r = vkAllocateMemory(vk_dev(), &mai, NULL, &mem);
        if (r != VK_SUCCESS && cached_used && mi_std != UINT32_MAX) {
            LOG("【黄】%s 内存换轨：类型 %u（带 HOST_CACHED）分配失败 VkResult %d，回退类型 %u",
                what, mi, (int)r, mi_std);
            mi = mi_std; cached_used = 0; mai.memoryTypeIndex = mi;
            r = vkAllocateMemory(vk_dev(), &mai, NULL, &mem);
        }
        if (r != VK_SUCCESS) {
            vkDestroyBuffer(vk_dev(), owner, NULL);
            if (try > MIN_USEFUL) continue;             // 顶到上限：退 128 MB 再试 | hit the limit: step down and retry
            LOG("【红】%s 内存换轨：分配失败 VkResult %d", what, (int)r);
            return 0;
        }

        VkMemoryGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = mem,
                                    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
        int fd = -1;
        r = getfd(vk_dev(), &gi, &fd);
        if (r != VK_SUCCESS) {
            vkFreeMemory(vk_dev(), mem, NULL); vkDestroyBuffer(vk_dev(), owner, NULL);
            LOG("【红】%s 内存换轨：取 fd 失败 VkResult %d", what, (int)r);
            return 0;
        }
        void *p = mmap(at, try, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        close(fd);                                      // 映射已经拿着引用，fd 可以关 | the mapping holds its own reference, the fd can go
        if (p != at) {
            if (p != MAP_FAILED) munmap(p, try);
            vkFreeMemory(vk_dev(), mem, NULL); vkDestroyBuffer(vk_dev(), owner, NULL);
            LOG("【红】%s 内存换轨：mmap 到 %p 失败（要 %p，errno=%d）", what, p, at, errno);
            return 0;
        }
        g_reg[g_nreg].base = at; g_reg[g_nreg].size = try;
        g_reg[g_nreg].mem = mem; g_reg[g_nreg].owner = owner;
        g_nreg++; g_bytes += try;
        if (try < size)
            LOG("【黄】%s 内存换轨：只换成 %llu MB（要 %llu MB）——顶到单次分配上限，报给客体的上限已相应下调%s",
                what, (unsigned long long)(try >> 20), (unsigned long long)(size >> 20),
                cached_used ? "；用的是带 HOST_CACHED 的类型（越过缓冲声明的可用类型位）" : "");
        else
            LOG("【绿】%s 内存换轨：%llu MB 已是 GPU 直接可见（%p，类型 %u%s）",
                what, (unsigned long long)(try >> 20), at, mi,
                cached_used ? "，带 HOST_CACHED（越过缓冲声明的可用类型位）" : "");
        return (size_t)try;
    }
    LOG("【红】%s 内存换轨：连 %llu MB 都要不到", what, (unsigned long long)(MIN_USEFUL >> 20));
    return 0;
}

// AHB 支路：AHB 分配 → 取它的 dma-buf fd → mmap(MAP_FIXED) 压回客体地址 → 导入 Vulkan 当 DeviceMemory。
// 四条实测口径（每条都是"改了就会坏"，读数见 .work/logs/probe-*-vk_ahb_probe*.txt）：
//   ① **用法位只给 `GPU_DATA_BUFFER`，不带 CPU_* 位**：带 CPU 位时 Mali 的 gralloc 给的是"缓存"内存，
//      客体经 dma-buf 裸映射的直接读写与 GPU 侧不一致（两个方向都不通）；只留 GPU 位时两个方向都通。
//   ② **fd 要从句柄里挑 dmabuf 那一条**（第二台的 fd[0] 是 gralloc_extra，映射它 EACCES），见 ahb_dmabuf_fd。
//   ③ 导入时**不需要** VkMemoryDedicatedAllocateInfo（两台都接受不带 dedicated 的导入）。
//   ④ 池子的用法成立：同一份内存上按偏移能绑多块缓冲（外部缓冲与**普通缓冲**都试过，都 VK_SUCCESS），
//      客体侧双向也通（客体→GPU 通、GPU→客体通，两块互不干扰）。
// The AHB route: allocate an AHB, take its dma-buf fd, mmap(MAP_FIXED) it back at the guest address, then import it
// into Vulkan as a DeviceMemory. Four measured rules (each one breaks things when changed; readings under
// .work/logs/probe-*-vk_ahb_probe*.txt):
//   (1) the usage bits are **GPU_DATA_BUFFER only, with no CPU_\* bit**: with a CPU bit Mali's gralloc hands back
//       cached memory, so the guest's direct access through the raw dma-buf mapping disagrees with the GPU (both
//       directions fail); with the GPU bit alone both directions pass.
//   (2) the fd must be **the dma-buf one out of the handle** (device 2's fd[0] is gralloc_extra and mapping it gives
//       EACCES), see ahb_dmabuf_fd.
//   (3) the import does **not** need VkMemoryDedicatedAllocateInfo (both devices accept an import without it).
//   (4) the pool usage holds: several buffers can be bound at offsets of one allocation (tried with external and with
//       **plain** buffers, both VK_SUCCESS) and both directions work from the guest side (guest->GPU and GPU->guest,
//       with the two regions not disturbing each other).
static size_t overlay_ahb(void *at, size_t size, const char *what) {
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getprops =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(
            vk_dev(), "vkGetAndroidHardwareBufferPropertiesANDROID");
    if (!getprops) {
        LOG("【红】%s 内存换轨（AHB）：取不到 vkGetAndroidHardwareBufferPropertiesANDROID", what);
        return 0;
    }
    const VkPhysicalDeviceMemoryProperties *mp = vk_mem_props();
    const size_t pg = (size_t)getpagesize();
    uint64_t try = size & ~(uint64_t)(pg - 1);          // 映射长度要整页 | the mapping length must be whole pages
    // 单次上限（DM_REGION_AHB_MAX=<MB>；不设就不限）：分级复量时用它一步步逼近那个尺寸 ——
    // 设了上限就**允许小于 MIN_USEFUL** 的一小段（那才是"最小的能复现吗"要问的尺寸）。
    // Per-allocation cap (DM_REGION_AHB_MAX=<MB>; unset means no cap): the staged re-measurement uses it to approach
    // the limit step by step. With a cap set, a piece **smaller than MIN_USEFUL** is allowed -- that is exactly the
    // size the "does the smallest case reproduce it" question asks about.
    uint64_t cap = ahb_max_bytes();
    uint64_t floor = MIN_USEFUL;
    if (cap) {
        if (try > cap) {
            LOG("【黄】%s 内存换轨（AHB）：DM_REGION_AHB_MAX=%llu MB 把这次的 %llu MB 压到上限",
                what, (unsigned long long)(cap >> 20), (unsigned long long)(try >> 20));
            try = cap & ~(uint64_t)(pg - 1);
        }
        floor = 1ull << 20;
    }
    for (; try >= floor; try -= STEP_DOWN) {
        AHardwareBuffer_Desc d = { .width = (uint32_t)try, .height = 1, .layers = 1,
                                   .format = AHARDWAREBUFFER_FORMAT_BLOB,
                                   .usage = AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER };   // 见 ① | see (1)
        AHardwareBuffer *a = NULL;
        errno = 0;
        int arc = AHardwareBuffer_allocate(&d, &a);
        if (arc != 0 || !a) {
            if (try > floor) continue;                   // 顶到上限：退 128 MB 再试 | hit the limit: step down
            LOG("【红】%s 内存换轨（AHB）：连 %llu MB 都分不出来（rc=%d，errno=%d）",
                what, (unsigned long long)(floor >> 20), arc, errno);
            return 0;
        }
        int fd = ahb_dmabuf_fd(a);
        if (fd < 0) {
            AHardwareBuffer_release(a);
            LOG("【红】%s 内存换轨（AHB）：拿不到 AHB 的 dma-buf fd（句柄里没有一条指向 dmabuf 的）", what);
            return 0;
        }
        void *p = mmap(at, try, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        if (p != at) {
            if (p != MAP_FAILED) munmap(p, try);
            AHardwareBuffer_release(a);
            LOG("【红】%s 内存换轨（AHB）：mmap 到 %p 失败（要 %p，errno=%d）", what, p, at, errno);
            return 0;
        }
        VkAndroidHardwareBufferPropertiesANDROID pr = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
        VkResult r = getprops(vk_dev(), a, &pr);
        if (r != VK_SUCCESS) {
            munmap(at, try); AHardwareBuffer_release(a);
            LOG("【红】%s 内存换轨（AHB）：取 AHB 属性失败 VkResult %d", what, (int)r);
            return 0;
        }
        uint32_t mi = UINT32_MAX;
        for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
            if (pr.memoryTypeBits & (1u << i)) { mi = i; break; }
        if (mi == UINT32_MAX) {
            munmap(at, try); AHardwareBuffer_release(a);
            LOG("【红】%s 内存换轨（AHB）：AHB 属性没给出可用的内存类型（memoryTypeBits %#x）", what, pr.memoryTypeBits);
            return 0;
        }
        VkImportAndroidHardwareBufferInfoANDROID imp = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = a };   // 见 ③ | see (3)
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
                                     .allocationSize = pr.allocationSize, .memoryTypeIndex = mi };
        VkDeviceMemory mem = VK_NULL_HANDLE;
        r = vkAllocateMemory(vk_dev(), &mai, NULL, &mem);
        if (r != VK_SUCCESS) {
            munmap(at, try); AHardwareBuffer_release(a);
            LOG("【红】%s 内存换轨（AHB）：导入 AHB 失败 VkResult %d（类型 %u，%llu MB）",
                what, (int)r, mi, (unsigned long long)(pr.allocationSize >> 20));
            return 0;
        }
        uint64_t touch = ahb_touch_bytes();
        uint64_t zbytes = ahb_memset_bytes();
        uint64_t abytes = ahb_atomic_bytes();
        uint8_t *raw = (uint8_t *)at;
        if (touch) {
            if (touch > try) touch = try;
            const uint64_t page = 4096, step = 128ull << 20;
            volatile uint8_t *p8 = (volatile uint8_t *)at;
            for (uint64_t o = 0; o < touch; o += page) {
                p8[o] = (uint8_t)(o >> 12);
                if ((o % step) == 0)
                    LOG("【黄】%s 内存换轨（AHB）：TOUCH 写到 %llu MB / %llu MB",
                        what, (unsigned long long)(o >> 20), (unsigned long long)(touch >> 20));
            }
            LOG("【黄】%s 内存换轨（AHB）：TOUCH 写完一遍（%llu MB），开始按页读回校验",
                what, (unsigned long long)(touch >> 20));
            uint64_t bad = 0;
            for (uint64_t o = 0; o < touch; o += page) if (p8[o] != (uint8_t)(o >> 12)) bad++;
            LOG("【黄】%s 内存换轨（AHB）：TOUCH 读回完成：%llu 个页里 %llu 个不一致",
                what, (unsigned long long)(touch / page), (unsigned long long)bad);
        }
        if (zbytes) {
            if (zbytes > try) zbytes = try;
            const uint64_t chunk = 1ull << 20, step = 128ull << 20;
            volatile uint8_t *p8 = (volatile uint8_t *)at;
            for (uint64_t o = 0; o < zbytes; o += chunk) {
                memset(raw + o, 0, (size_t)chunk);      // 大面积清零：arm64 上 bionic 会走 DC ZVA
                if ((o % step) == 0)
                    LOG("【黄】%s 内存换轨（AHB）：MEMSET 清到 %llu MB / %llu MB",
                        what, (unsigned long long)(o >> 20), (unsigned long long)(zbytes >> 20));
            }
            LOG("【黄】%s 内存换轨（AHB）：MEMSET 清完一遍（%llu MB），开始按页读回校验",
                what, (unsigned long long)(zbytes >> 20));
            uint64_t nz = 0, pages = 0;
            for (uint64_t o = 0; o < zbytes; o += 4096) { pages++; if (p8[o]) nz++; }
            LOG("【黄】%s 内存换轨（AHB）：MEMSET 读回完成：%llu 个页里 %llu 个非零",
                what, (unsigned long long)pages, (unsigned long long)nz);
        }
        if (abytes) {
            if (abytes > try) abytes = try;
            const uint64_t page = 4096, step = 128ull << 20;
            for (uint64_t o = 0; o < abytes; o += page) {
                // 一次原子加：arm64 上是 ldaxr/stlxr（独占访问）。用 volatile 挡住优化，别让它被消掉。
                volatile uint32_t *a32 = (volatile uint32_t *)(raw + o);
                __atomic_fetch_add((uint32_t *)a32, 1u, __ATOMIC_SEQ_CST);
                if ((o % step) == 0)
                    LOG("【黄】%s 内存换轨（AHB）：ATOMIC 加到 %llu MB / %llu MB",
                        what, (unsigned long long)(o >> 20), (unsigned long long)(abytes >> 20));
            }
            LOG("【黄】%s 内存换轨（AHB）：ATOMIC 做完一遍（%llu MB，每页一次），开始读回",
                what, (unsigned long long)(abytes >> 20));
            uint64_t bad = 0, pages = 0;
            for (uint64_t o = 0; o < abytes; o += page) {
                pages++;
                uint32_t v;
                __atomic_load((uint32_t *)(raw + o), &v, __ATOMIC_SEQ_CST);
                if (v != 1u) bad++;
            }
            LOG("【黄】%s 内存换轨（AHB）：ATOMIC 读回完成：%llu 个页里 %llu 个不是 1",
                what, (unsigned long long)pages, (unsigned long long)bad);
        }
        if (ahb_dryrun() || touch || zbytes || abytes) {
            // 撤掉时必须**把这段重新占住**（匿名映射），否则客体地址空间里会留一个没映射的洞 —— 那不是
            // "没做换轨"的状态，会变成"客体一碰就 SIGSEGV"，把要分离的两件事又混在一起。
            // When undoing, the range must be **occupied again** (anonymous mapping); otherwise the guest address
            // space is left with an unmapped hole, which is not the "no overlay" state but "the guest faults on
            // first touch", mixing up exactly the two things this switch is meant to separate.
            LOG("【黄】%s 内存换轨（AHB）：%s —— %llu MB 的分配/映射/导入都成了，原路撤掉、返回 0（不注册区域）",
                what, abytes ? "ATOMIC 之后撤掉（等于 DRYRUN）"
                             : (zbytes ? "MEMSET 之后撤掉（等于 DRYRUN）"
                                       : (touch ? "TOUCH 之后撤掉（等于 DRYRUN）" : "DRYRUN")),
                (unsigned long long)(try >> 20));
            vkFreeMemory(vk_dev(), mem, NULL);
            AHardwareBuffer_release(a);
            munmap(at, try);
            void *q = mmap(at, try, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
            LOG("【黄】%s 内存换轨（AHB）：撤掉收尾 —— 该段已按匿名映射重新占住（%p，要 %p）", what, q, at);
            return 0;
        }
        g_reg[g_nreg].base = at; g_reg[g_nreg].size = try;
        g_reg[g_nreg].mem = mem; g_reg[g_nreg].owner = VK_NULL_HANDLE; g_reg[g_nreg].ahb = a;
        g_nreg++; g_bytes += try;
        if (try < size)
            LOG("【黄】%s 内存换轨（AHB）：只换成 %llu MB（要 %llu MB）——顶到单次上限，报给客体的上限已相应下调",
                what, (unsigned long long)(try >> 20), (unsigned long long)(size >> 20));
        else
            LOG("【绿】%s 内存换轨（AHB）：%llu MB 已是 GPU 直接可见（%p，内存类型 %u，AHB 对象已保活）",
                what, (unsigned long long)(try >> 20), at, mi);
        return (size_t)try;
    }
    LOG("【红】%s 内存换轨（AHB）：连 %llu MB 都要不到", what, (unsigned long long)(floor >> 20));
    return 0;
}

size_t vk_region_overlay(void *at, size_t size, const char *what) {
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) { LOG("【红】%s 内存换轨：Vulkan 设备没起来", what); return 0; }
    if (g_nreg >= MAX_REGION) { LOG("【红】%s 内存换轨：段数已满（%d）", what, MAX_REGION); return 0; }
    if (route() == 1) return overlay_ahb(at, size, what);
    size_t got = overlay_export(at, size, what);
    if (!got && ahb_available() && ahb_enabled()) {
        // 甲案**问得到却当场做不成**（真机上出现过：能力位说可导出、真分配/真建缓冲失败）⇒ 退到 AHB 再试一次。
        // Route A can be **reported as available and still fail on the spot** (measured: the capability bit says
        // exportable while the real allocation or buffer creation fails) -> fall back to AHB and try again.
        LOG("【黄】%s 甲案（可导出内存 → mmap）没换成 ⇒ 退到 AHB 支路再试一次", what);
        got = overlay_ahb(at, size, what);
    }
    return got;
}

size_t vk_region_use(void *at, size_t size, const char *what) { return vk_region_overlay(at, size, what); }

int vk_region_find(uint64_t addr, uint64_t len, VkDeviceMemory *mem, uint64_t *off) {
    for (int i = 0; i < g_nreg; i++) {
        uint64_t b = (uint64_t)(uintptr_t)g_reg[i].base;
        if (addr >= b && addr + len <= b + g_reg[i].size) {
            if (mem) *mem = g_reg[i].mem;
            if (off) *off = addr - b;
            return 1;
        }
    }
    return 0;
}

void vk_region_stats(uint64_t *bytes, int *regions) {
    if (bytes) *bytes = g_bytes;
    if (regions) *regions = g_nreg;
}
