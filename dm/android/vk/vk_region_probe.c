// M2 第三个能力探针：给客体地址空间铺 Vulkan 零拷贝内存，到底能要多大。
//
// 背景（见 vk_hostmem_probe 的结论）：这台设备没有 VK_EXT_external_memory_host，Metal 那条
// 「把宿主任意指针包成零拷贝 MTLBuffer」在 Vulkan 上没有对应物。于是零拷贝只剩一条路：
// 客体地址空间本身由 vkAllocateMemory + vkMapMemory 提供（拿到的映射地址就是宿主指针），
// 池子地址到缓冲的换算全变成算术。
//
// 宿主的划分是：alias 512 MB、stack 256 MB、heap 4 GB（都在一块 5 GB 的预留里）。
// 这个探针量三件事：
//   ① 这种零拷贝类型一次最多能要多少（含 maxMemoryAllocationSize / maxMemoryAllocationCount）
//   ② 要到的块里，前面一部分不碰、只碰几个页，会不会把真内存吃光（惰性提交与否）
//   ③ 在块内某个非零偏移上建 VkBuffer 能不能绑上（池子按页对齐，绑定偏移的粒度要够）
//
// M2's third capability probe: how large a piece of Vulkan zero-copy memory can back the guest address
// space.
//
// Background (see vk_hostmem_probe's result): this device has no VK_EXT_external_memory_host, so Metal's
// "wrap any host pointer in a zero-copy MTLBuffer" has no Vulkan counterpart. Zero-copy is then left with
// one route: let the guest address space itself come from vkAllocateMemory + vkMapMemory (the mapped
// address is a host pointer), which turns pool-address-to-buffer translation into arithmetic.
//
// The host's partition is: alias 512 MB, stack 256 MB, heap 4 GB (all inside a 5 GB reservation).
// This probe measures three things:
//   (1) how much of that zero-copy type can be asked for at once (plus maxMemoryAllocationSize and
//       maxMemoryAllocationCount)
//   (2) whether touching only a few pages of a large block commits real memory or not (i.e. whether
//       allocation is lazy)
//   (3) whether a VkBuffer can be bound at a non-zero offset inside the block (pools are page aligned, so
//       the binding granularity must be fine enough)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static void die(const char *what, VkResult r) {
    fprintf(stderr, "【红】%s 失败：VkResult %d\n", what, (int)r);
    exit(1);
}
#define VKOK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) die(#x, _r); } while (0)
#define GB (1ull << 30)

// 读 /proc/self/statm 的常驻页数（页）：判定大块分配是真占内存还是惰性提交
// Read the resident page count from /proc/self/statm: tells whether a big allocation really costs
// memory or is committed lazily
static unsigned long rss_pages(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long sz = 0, rs = 0;
    if (fscanf(f, "%lu %lu", &sz, &rs) != 2) rs = 0;
    fclose(f);
    return rs;
}

int main(void) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "dm-region-probe",
                             .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst = VK_NULL_HANDLE; VKOK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 0; VKOK(vkEnumeratePhysicalDevices(inst, &n, NULL));
    VkPhysicalDevice *dv = calloc(n, sizeof *dv); VKOK(vkEnumeratePhysicalDevices(inst, &n, dv));
    VkPhysicalDevice phys = dv[0];

    VkPhysicalDeviceMaintenance3Properties m3 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m3 };
    vkGetPhysicalDeviceProperties2(phys, &p2);
    printf("设备：%s\n", p2.properties.deviceName);
    printf("maxMemoryAllocationSize = %llu MB，maxMemoryAllocationCount = %u\n",
           (unsigned long long)(m3.maxMemoryAllocationSize >> 20), p2.properties.limits.maxMemoryAllocationCount);
    printf("bufferImageGranularity = %llu，nonCoherentAtomSize = %llu\n",
           (unsigned long long)p2.properties.limits.bufferImageGranularity,
           (unsigned long long)p2.properties.limits.nonCoherentAtomSize);

    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev = VK_NULL_HANDLE; VKOK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue q = VK_NULL_HANDLE; vkGetDeviceQueue(dev, qfi, 0, &q);

    // 选零拷贝类型：DEVICE_LOCAL + HOST_VISIBLE + HOST_COHERENT（前面的探针已量出是类型 6）
    // Pick the zero-copy type: DEVICE_LOCAL + HOST_VISIBLE + HOST_COHERENT (type 6, per the earlier probe)
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t mi = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mi = i; break; }
    }
    if (mi == UINT32_MAX) { fprintf(stderr, "【红】没有 DEVICE_LOCAL+HOST_VISIBLE+HOST_COHERENT 类型\n"); return 1; }
    printf("零拷贝内存类型 %u\n", mi);

    // ---- ① 逐档试要 | (1) try progressively larger blocks ----
    printf("=== 一次能要多大（零拷贝类型）===\n");
    unsigned long base_rss = rss_pages();
    // 先粗后细：3 GB 成、4 GB 不成，中间这段要量准——客体堆是 3728 MB，卡在这里就要改设计
    // Coarse first then fine: 3 GB works and 4 GB does not, and this middle stretch must be measured
    // exactly, because the guest heap is 3728 MB and getting stuck here changes the design
    uint64_t sizes[] = { 3 * GB, 3200ull << 20, 3328ull << 20, 3456ull << 20, 3584ull << 20,
                         3712ull << 20, 3840ull << 20, 3968ull << 20, 4 * GB };
    VkDeviceMemory big = VK_NULL_HANDLE; void *bigp = NULL; uint64_t bigsz = 0;
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        VkMemoryAllocateInfo ai2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     .allocationSize = sizes[i], .memoryTypeIndex = mi };
        VkDeviceMemory m = VK_NULL_HANDLE;
        VkResult r = vkAllocateMemory(dev, &ai2, NULL, &m);
        if (r == VK_SUCCESS) {
            void *p = NULL;
            VkResult rm = vkMapMemory(dev, m, 0, sizes[i], 0, &p);
            printf("  %5llu MB：分配成功，映射 %s（%p）\n", (unsigned long long)(sizes[i] >> 20),
                   rm == VK_SUCCESS ? "成功" : "失败", p);
            if (rm == VK_SUCCESS && p) { big = m; bigp = p; bigsz = sizes[i]; vkUnmapMemory(dev, m); vkFreeMemory(dev, m, NULL); }
            else { vkFreeMemory(dev, m, NULL); }
        } else {
            printf("  %5llu MB：分配失败 VkResult %d\n", (unsigned long long)(sizes[i] >> 20), (int)r);
        }
    }
    if (!big) { fprintf(stderr, "【红】连 256 MB 都要不到\n"); return 1; }
    printf("⇒ 最大可用 %llu MB\n", (unsigned long long)(bigsz >> 20));

    // ---- ② 惰性提交：只碰几个页 | (2) lazy commit: touch only a few pages ----
    printf("=== 惰性提交（块 %llu MB，只碰 64 个页）===\n", (unsigned long long)(bigsz >> 20));
    VkMemoryAllocateInfo ai3 = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bigsz, .memoryTypeIndex = mi };
    VkDeviceMemory mem = VK_NULL_HANDLE; VKOK(vkAllocateMemory(dev, &ai3, NULL, &mem));
    void *p = NULL; VKOK(vkMapMemory(dev, mem, 0, bigsz, 0, &p));
    for (int i = 0; i < 64; i++) ((volatile uint8_t *)p)[(uint64_t)i * (bigsz / 64)] = (uint8_t)i;
    unsigned long now_rss = rss_pages();
    long pages = (long)now_rss - (long)base_rss;
    printf("  常驻增量 %ld 页（约 %ld MB）；整块 %llu MB ⇒ %s\n", pages, pages * 4,
           (unsigned long long)(bigsz >> 20),
           (pages * 4096 < (long)(bigsz / 8)) ? "惰性提交（只占了碰到的页）" : "看起来整块都提交了");

    // ---- ③ 块内非零偏移上绑缓冲 | (3) bind a buffer at a non-zero offset inside the block ----
    printf("=== 块内非零偏移绑缓冲（池子按页对齐）===\n");
    const VkDeviceSize off = 16 * 1024 * 1024;   // 16 MB 处，模拟第二个池子 | 16 MB in, simulating a second pool
    const VkDeviceSize wsz = 64 * 1024;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = wsz,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer b2 = VK_NULL_HANDLE; VKOK(vkCreateBuffer(dev, &bci, NULL, &b2));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b2, &mr);
    printf("  缓冲要求 alignment = %llu（偏移 %llu 是它的 %s 倍）\n", (unsigned long long)mr.alignment,
           (unsigned long long)off, (off % mr.alignment == 0) ? "" : "非整");
    VkResult rb = vkBindBufferMemory(dev, b2, mem, off);
    printf("  在偏移 %llu 处绑定：%s\n", (unsigned long long)off, rb == VK_SUCCESS ? "成功" : "失败");

    int ok = (rb == VK_SUCCESS);
    if (ok) {
        // 客体内存（宿主指针）↔ GPU：偏移处那一段也要双向通 | guest memory (host pointer) <-> GPU: both directions at that offset
        VkBufferCreateInfo rci = bci;
        VkBuffer rb2 = VK_NULL_HANDLE; VKOK(vkCreateBuffer(dev, &rci, NULL, &rb2));
        VkMemoryRequirements rmr; vkGetBufferMemoryRequirements(dev, rb2, &rmr);
        uint32_t rmi = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((rmr.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { rmi = i; break; }
        VkMemoryAllocateInfo rmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rmr.size, .memoryTypeIndex = rmi };
        VkDeviceMemory rmem = VK_NULL_HANDLE; VKOK(vkAllocateMemory(dev, &rmai, NULL, &rmem));
        VKOK(vkBindBufferMemory(dev, rb2, rmem, 0));
        void *rm = NULL; VKOK(vkMapMemory(dev, rmem, 0, wsz, 0, &rm));

        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi };
        VkCommandPool pool = VK_NULL_HANDLE; VKOK(vkCreateCommandPool(dev, &pci, NULL, &pool));
        VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer cb = VK_NULL_HANDLE; VKOK(vkAllocateCommandBuffers(dev, &cbai, &cb));
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        VkBufferCopy cp = { .size = wsz };

        uint8_t *gp = (uint8_t *)p + off;   // 客体看到的就是这个大块里的偏移 | what the guest sees is an offset into this big block
        for (VkDeviceSize i = 0; i < wsz; i++) gp[i] = (uint8_t)(i * 3 + 1);
        VKOK(vkBeginCommandBuffer(cb, &bi));
        vkCmdCopyBuffer(cb, b2, rb2, 1, &cp);
        VKOK(vkEndCommandBuffer(cb));
        VKOK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
        VKOK(vkQueueWaitIdle(q));
        int d1 = 1;
        for (VkDeviceSize i = 0; i < wsz; i++) if (((uint8_t *)rm)[i] != (uint8_t)(i * 3 + 1)) { d1 = 0; break; }

        VKOK(vkResetCommandPool(dev, pool, 0));
        VKOK(vkBeginCommandBuffer(cb, &bi));
        vkCmdFillBuffer(cb, b2, 0, wsz, 0x3C3C3C3Cu);
        VKOK(vkEndCommandBuffer(cb));
        VKOK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
        VKOK(vkQueueWaitIdle(q));
        int d2 = 1;
        for (VkDeviceSize i = 0; i < wsz; i++) if (gp[i] != 0x3C) { d2 = 0; break; }

        printf("  偏移处双向：客体→GPU %s，GPU→客体 %s\n", d1 ? "通" : "不通", d2 ? "通" : "不通");
        ok = ok && d1 && d2;

        vkUnmapMemory(dev, rmem);
        vkDestroyBuffer(dev, rb2, NULL); vkFreeMemory(dev, rmem, NULL);
        vkDestroyCommandPool(dev, pool, NULL);
    }

    vkUnmapMemory(dev, mem);
    vkDestroyBuffer(dev, b2, NULL);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    printf("客体地址空间铺零拷贝内存：%s\n", ok ? "可行" : "有问题");
    return ok ? 0 : 2;
}
