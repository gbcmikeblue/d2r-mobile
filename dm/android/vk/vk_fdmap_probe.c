// M2 第四个能力探针：能不能把 Vulkan 的内存导出成 fd、再由宿主 mmap 到**指定地址**。
//
// 为什么这一问决定改法（见 docs/ANDROID_PLAN 的 M2 第 3 步）：上一轮量出这台设备没有
// VK_EXT_external_memory_host，所以「把宿主任意指针包成缓冲」不通，改让 Vulkan 来铺客体地址空间。
// 铺法有两种，取舍就看这个探针：
//
//   甲案（保布局）：Vulkan 内存导出成 fd（VK_KHR_external_memory_fd / DMA_BUF），宿主用
//                   mmap(固定地址, MAP_FIXED, fd) 把它摆在 heap / alias 该在的地址上。
//                   → 现有布局、大小、QueryMemory 的算法全都不用动，改的只是「那两个 mmap 的来源」。
//   乙案（改布局）：不能导出，只能让驱动选址，heap 与 alias 变成两次独立分配，
//                   客体地址空间不再是连续的一块，QueryMemory / GetInfo 的多块处理要跟着改。
//
// 甲案明显更小、更稳，所以先测。探针会：
//   ① 列出所有名字含 external 的设备扩展（不管认不认识，先看清楚有什么）
//   ② 对 dma-buf 与不透明 fd 两种句柄，用 vkGetPhysicalDeviceExternalBufferProperties **在分配前**问支持情况
//   ③ 支持的话真做一遍：分配可导出内存 → 取 fd → mmap 到指定地址 → 验 CPU / GPU 双向
//
// M2's fourth capability probe: can Vulkan memory be exported as an fd and mmap'd by the host at a
// **chosen address**?
//
// Why this decides the approach (see M2 step 3 in docs/ANDROID_PLAN): the previous round measured that this
// device has no VK_EXT_external_memory_host, so "wrap any host pointer in a buffer" does not work and
// Vulkan is to back the guest address space instead. There are two ways to do that, and this probe picks
// between them:
//
//   A (keep the layout): export the Vulkan memory as an fd (VK_KHR_external_memory_fd / DMA_BUF) and have
//      the host place it at the address the heap / alias should have with mmap(fixed address, MAP_FIXED, fd).
//      -> Not a single thing about the existing layout, sizes or QueryMemory arithmetic has to change; only
//         where those two mmaps come from.
//   B (change the layout): if exporting is unavailable, the driver picks the addresses, the heap and alias
//      become two independent allocations, the guest address space is no longer one contiguous block, and
//      the multi-block handling in QueryMemory / GetInfo has to follow.
//
// A is clearly smaller and safer, so measure it first. The probe will:
//   (1) list every device extension whose name contains "external" (see what is there, known or not)
//   (2) use vkGetPhysicalDeviceExternalBufferProperties to ask about both dma-buf and opaque-fd handles
//       **before allocating anything**
//   (3) if supported, actually do it: allocate exportable memory, get the fd, mmap it at a chosen address,
//       and verify both directions
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

static void die(const char *what, VkResult r) {
    fprintf(stderr, "【红】%s 失败：VkResult %d\n", what, (int)r);
    exit(1);
}
#define VKOK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) die(#x, _r); } while (0)
#define GB (1ull << 30)

static void feat_name(VkExternalMemoryFeatureFlags f, char *out, size_t n) {
    out[0] = 0;
    if (f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)   strncat(out, "DEDICATED_ONLY|", n - strlen(out) - 1);
    if (f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)       strncat(out, "EXPORTABLE|", n - strlen(out) - 1);
    if (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)       strncat(out, "IMPORTABLE|", n - strlen(out) - 1);
    if (!out[0]) strncat(out, "（都不支持）", n - strlen(out) - 1);
}

int main(void) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "dm-fdmap-probe",
                             .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst = VK_NULL_HANDLE; VKOK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 0; VKOK(vkEnumeratePhysicalDevices(inst, &n, NULL));
    VkPhysicalDevice *dv = calloc(n, sizeof *dv); VKOK(vkEnumeratePhysicalDevices(inst, &n, dv));
    VkPhysicalDevice phys = dv[0];

    // ---- ① 所有含 external 的扩展（认不认识都先看清楚）| (1) every extension containing "external" ----
    uint32_t ne = 0; VKOK(vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, NULL));
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    VKOK(vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, ep));
    printf("=== 名字含 external 的设备扩展 ===\n");
    int have_fd = 0, have_dma = 0;
    for (uint32_t i = 0; i < ne; i++)
        if (strstr(ep[i].extensionName, "external") || strstr(ep[i].extensionName, "foreign")) {
            printf("  %s（修订 %u）\n", ep[i].extensionName, ep[i].specVersion);
            if (!strcmp(ep[i].extensionName, "VK_KHR_external_memory_fd") ||
                !strcmp(ep[i].extensionName, "VK_EXT_external_memory_dma_buf")) have_fd = 1;
            if (!strcmp(ep[i].extensionName, "VK_EXT_external_memory_dma_buf")) have_dma = 1;
        }
    printf("  ⇒ VK_KHR_external_memory_fd %s；VK_EXT_external_memory_dma_buf %s\n",
           have_fd ? "有" : "没有", have_dma ? "有" : "没有");

    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
    const char *en[2]; uint32_t nec = 0;
    if (have_fd && have_dma) { en[nec++] = "VK_KHR_external_memory_fd"; en[nec++] = "VK_EXT_external_memory_dma_buf"; }
    else if (have_dma) en[nec++] = "VK_EXT_external_memory_dma_buf";
    else if (have_fd) en[nec++] = "VK_KHR_external_memory_fd";
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci, .enabledExtensionCount = nec, .ppEnabledExtensionNames = en };
    VkDevice dev = VK_NULL_HANDLE; VKOK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue q = VK_NULL_HANDLE; vkGetDeviceQueue(dev, qfi, 0, &q);

    // ---- ② 分配之前先问句柄支持情况 | (2) ask about handle support before allocating ----
    VkPhysicalDeviceExternalBufferInfo ebi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    printf("=== 缓冲的外部句柄支持（分配前先问）===\n");
    VkExternalMemoryFeatureFlags ok_feat = 0;
    VkExternalMemoryHandleTypeFlagBits ok_type = 0;
    for (int t = 0; t < 2; t++) {
        ebi.handleType = t ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
                           : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkExternalBufferProperties ebp = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
        vkGetPhysicalDeviceExternalBufferProperties(phys, &ebi, &ebp);
        char fn[128];
        feat_name(ebp.externalMemoryProperties.externalMemoryFeatures, fn, sizeof fn);
        printf("  %-14s → %s（可导入的内存类型位 %#x）\n",
               t ? "dma-buf" : "不透明 fd", fn, ebp.externalMemoryProperties.compatibleHandleTypes);
        if ((ebp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) &&
            (ebp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
            if (t == 0 || !ok_feat) { ok_feat = ebp.externalMemoryProperties.externalMemoryFeatures; ok_type = ebi.handleType; }
        }
    }
    if (!ok_feat) {
        printf("⇒ 两种句柄都不支持「可导出」，甲案（保布局）走不通，得用乙案：让驱动选址、改客体地址空间的布局\n");
        printf("   Neither handle type is exportable, so approach A (keeping the layout) is out and approach B\n");
        printf("   (the driver picks addresses and the guest address space layout changes) is required\n");
        return 3;
    }
    printf("⇒ 可用句柄 %s，走甲案（保布局）\n",
           ok_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ? "dma-buf" : "不透明 fd");

    // ---- ③ 真做一遍：可导出内存 → fd → 指定地址 mmap → 双向验 | (3) do it for real ----
    // 先占住想要的那段地址（模拟客体 heap 该在的位置）| first take the address we want (simulating where the guest heap should be)
    const size_t wsz = 256 * 1024 * 1024;   // 先拿 256 MB 试，方案成立再上 4 GB | try 256 MB first, scale to 4 GB once the approach holds
    void *want = mmap(NULL, wsz, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (want == MAP_FAILED) { perror("mmap 预留"); return 1; }
    printf("预留地址 %p（%zu MB），下面要把 Vulkan 内存摆到这里\n", want, wsz >> 20);

    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);

    // 上一步报的 DEDICATED_ONLY 是关键：这种句柄要求「一份分配专属一个缓冲」，所以顺序必须是先有缓冲，
    // 分配时再链 VkMemoryDedicatedAllocateInfo，否则 vkGetMemoryFdKHR 会失败（上一版就是这么失败的）。
    // The DEDICATED_ONLY reported above is the key: this handle requires one allocation dedicated to one
    // buffer, so the buffer must come first and the allocation must chain VkMemoryDedicatedAllocateInfo,
    // otherwise vkGetMemoryFdKHR fails (which is exactly how the previous version failed).
    VkExternalMemoryBufferCreateInfo embci = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                               .handleTypes = ok_type };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &embci, .size = wsz,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer gb = VK_NULL_HANDLE; VKOK(vkCreateBuffer(dev, &bci, NULL, &gb));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, gb, &mr);
    printf("缓冲要求 size %llu MB / alignment %llu、可用内存类型位 %#x\n",
           (unsigned long long)(mr.size >> 20), (unsigned long long)mr.alignment, mr.memoryTypeBits);

    uint32_t mi = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(mr.memoryTypeBits & (1u << i))) continue;
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mi = i; break; }
    }
    if (mi == UINT32_MAX) { fprintf(stderr, "【红】这块缓冲没有零拷贝内存类型\n"); return 1; }

    VkMemoryDedicatedAllocateInfo mdai = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = gb };
    VkExportMemoryAllocateInfo emai = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                        .pNext = &mdai, .handleTypes = ok_type };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &emai,
                                 .allocationSize = mr.size, .memoryTypeIndex = mi };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult r = vkAllocateMemory(dev, &mai, NULL, &mem);
    if (r != VK_SUCCESS) { printf("可导出内存分配失败：VkResult %d（类型 %u）\n", (int)r, mi); return 2; }
    printf("可导出内存 %llu MB 分配成功（类型 %u，专属上面那块缓冲）\n", (unsigned long long)(mr.size >> 20), mi);

    PFN_vkGetMemoryFdKHR getfd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    if (!getfd) { fprintf(stderr, "【红】取不到 vkGetMemoryFdKHR\n"); return 1; }
    VkMemoryGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = mem, .handleType = ok_type };
    int fd = -1;
    r = getfd(dev, &gi, &fd);
    if (r != VK_SUCCESS) { printf("取 fd 失败：VkResult %d\n", (int)r); return 2; }
    printf("取得 fd = %d\n", fd);

    PFN_vkGetMemoryFdPropertiesKHR getfdprops =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
    if (getfdprops) {
        VkMemoryFdPropertiesKHR fp = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
        if (getfdprops(dev, ok_type, fd, &fp) == VK_SUCCESS)
            printf("该 fd 可用的内存类型位：%#x\n", fp.memoryTypeBits);
    }

    // 关键一步：摆到**指定地址** | the key step: place it at a **chosen address**
    void *m = mmap(want, mr.size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap(MAP_FIXED) 摆到指定地址"); return 2; }
    printf("mmap(固定地址) %s：%p（要求 %p）\n", (m == want) ? "成功落在指定地址" : "落在了别处", m, want);

    // 双向验：客体（这份映射）↔ GPU | both directions: the guest (this mapping) <-> the GPU
    r = vkBindBufferMemory(dev, gb, mem, 0);
    printf("缓冲绑到这份可导出内存：%s\n", r == VK_SUCCESS ? "成功" : "失败");
    if (r != VK_SUCCESS) return 2;

    VkBufferCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = wsz,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer rb = VK_NULL_HANDLE; VKOK(vkCreateBuffer(dev, &rci, NULL, &rb));
    VkMemoryRequirements rmr; vkGetBufferMemoryRequirements(dev, rb, &rmr);
    uint32_t rmi = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((rmr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { rmi = i; break; }
    VkMemoryAllocateInfo rmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rmr.size, .memoryTypeIndex = rmi };
    VkDeviceMemory rmem = VK_NULL_HANDLE; VKOK(vkAllocateMemory(dev, &rmai, NULL, &rmem));
    VKOK(vkBindBufferMemory(dev, rb, rmem, 0));
    void *rm = NULL; VKOK(vkMapMemory(dev, rmem, 0, wsz, 0, &rm));

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi };
    VkCommandPool pool = VK_NULL_HANDLE; VKOK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb = VK_NULL_HANDLE; VKOK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    const size_t chunk = 1 * 1024 * 1024;   // 1 MB 足够判双向 | 1 MB is enough to tell the two directions apart
    VkBufferCopy cp = { .size = chunk };

    uint8_t *gp = (uint8_t *)m;   // 客体看到的就是这份 mmap | what the guest sees is this mmap
    for (size_t i = 0; i < chunk; i++) gp[i] = (uint8_t)(i * 7 + 3);
    VKOK(vkBeginCommandBuffer(cb, &bi));
    vkCmdCopyBuffer(cb, gb, rb, 1, &cp);
    VKOK(vkEndCommandBuffer(cb));
    VKOK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    VKOK(vkQueueWaitIdle(q));
    int d1 = 1;
    for (size_t i = 0; i < chunk; i++) if (((uint8_t *)rm)[i] != (uint8_t)(i * 7 + 3)) { d1 = 0; break; }

    VKOK(vkResetCommandPool(dev, pool, 0));
    VKOK(vkBeginCommandBuffer(cb, &bi));
    vkCmdFillBuffer(cb, gb, 0, chunk, 0x1F1F1F1Fu);
    VKOK(vkEndCommandBuffer(cb));
    VKOK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    VKOK(vkQueueWaitIdle(q));
    int d2 = 1;
    for (size_t i = 0; i < chunk; i++) if (gp[i] != 0x1F) { d2 = 0; break; }

    printf("指定地址处的 Vulkan 内存：客体→GPU %s，GPU→客体 %s\n", d1 ? "通" : "不通", d2 ? "通" : "不通");
    printf("⇒ %s\n", (d1 && d2 && m == want)
           ? "甲案成立：Vulkan 内存可导出成 fd 并由宿主摆到指定地址，客体地址空间布局不用改"
           : "甲案不成立：导出或固定地址 mmap 有问题，要走乙案（改布局）");

    // ---- ④ 对本项目最要紧的一条：一份分配上能不能绑多块缓冲 | (4) the one that matters most here: can many buffers share one allocation ----
    // 池子就是「一份大内存上按偏移绑很多缓冲」。而这种句柄标了 DEDICATED_ONLY（规范上要求一份分配专属一块缓冲），
    // 所以必须实测驱动到底认不认：认，甲案就能直接用；不认，就得改布局（乙案）。
    // A pool is exactly "one big allocation with many buffers bound at offsets". This handle is marked
    // DEDICATED_ONLY (the spec wants one allocation per buffer), so whether the driver tolerates it must be
    // measured: if it does, approach A can be used as is; if not, the layout has to change (approach B).
    VkBufferCreateInfo bci2 = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = chunk,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer gb2 = VK_NULL_HANDLE; VKOK(vkCreateBuffer(dev, &bci2, NULL, &gb2));
    VkMemoryRequirements mr2; vkGetBufferMemoryRequirements(dev, gb2, &mr2);
    const VkDeviceSize off2 = 1 * 1024 * 1024;   // 1 MB 处（页对齐）
    VkResult rb2 = vkBindBufferMemory(dev, gb2, mem, off2);
    printf("在同一份分配内偏移 %llu 处再绑一块缓冲（池子的用法）：%s\n",
           (unsigned long long)off2, rb2 == VK_SUCCESS ? "成功" : "失败");
    int multi = 0;
    if (rb2 == VK_SUCCESS) {
        uint8_t *g2 = (uint8_t *)m + off2;
        for (size_t i = 0; i < chunk; i++) g2[i] = (uint8_t)(i * 11 + 9);
        VKOK(vkResetCommandPool(dev, pool, 0));
        VKOK(vkBeginCommandBuffer(cb, &bi));
        vkCmdCopyBuffer(cb, gb2, rb, 1, &cp);
        VKOK(vkEndCommandBuffer(cb));
        VKOK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
        VKOK(vkQueueWaitIdle(q));
        int e1 = 1;
        for (size_t i = 0; i < chunk; i++) if (((uint8_t *)rm)[i] != (uint8_t)(i * 11 + 9)) { e1 = 0; break; }
        VKOK(vkResetCommandPool(dev, pool, 0));
        VKOK(vkBeginCommandBuffer(cb, &bi));
        vkCmdFillBuffer(cb, gb2, 0, chunk, 0x6D6D6D6Du);
        VKOK(vkEndCommandBuffer(cb));
        VKOK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
        VKOK(vkQueueWaitIdle(q));
        int e2 = 1;
        for (size_t i = 0; i < chunk; i++) if (g2[i] != 0x6D) { e2 = 0; break; }
        multi = e1 && e2;
        printf("  偏移处双向：客体→GPU %s，GPU→客体 %s；且 0 偏移处那份数据未被冲掉 %s\n",
               e1 ? "通" : "不通", e2 ? "通" : "不通",
               (gp[0] == 0x1F) ? "（是）" : "（否）");
    }
    printf("⇒ 甲案 %s\n", multi
           ? "完全可用：既能摆到指定地址，又能在同一份分配上绑多块缓冲（池子的用法）"
           : (rb2 == VK_SUCCESS ? "地址可用，但偏移处双向有问题" : "只能「一份分配一块缓冲」，池子用法不成立，必须走乙案（改布局）"));

    // 收尾（进程结束前不 unmap，避免把结论建立在已经失效的映射上）
    // Clean up (leave the mapping until exit, so the conclusion does not rest on a mapping already torn down)
    vkUnmapMemory(dev, rmem);
    vkDestroyBuffer(dev, gb, NULL); vkDestroyBuffer(dev, rb, NULL);
    vkFreeMemory(dev, rmem, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    return (d1 && d2 && m == want) ? 0 : 2;
}
