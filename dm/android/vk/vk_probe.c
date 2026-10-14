// M2 第 1 步：验证安卓上的 Vulkan 通路，并量出这套设备的内存模型。
// 为什么先做离屏：M1 的宿主现在是 adb 推上去直接跑的可执行文件，没有窗口；而窗口/交换链要 APK 才有。
// 离屏渲染 + 回读能把「Vulkan 能不能用、清屏对不对、内存怎么摆」这三件事一次问清，
// 其中内存模型正是可行性分析里列为头号风险的那条假设（NVN 的「GPU 地址 = CPU 地址」零拷贝）。
// M2 step 1: prove the Vulkan path on Android and measure this device's memory model.
// Why offscreen first: the M1 host is an executable pushed with adb and has no window, and a window /
// swapchain needs an APK. An offscreen render plus readback settles three questions at once -- can Vulkan
// be used, is the clear correct, and how is memory laid out -- and the memory model is exactly the
// assumption the feasibility analysis flagged as risk number one (NVN's zero-copy "GPU address = CPU address").
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static void die(const char *what, VkResult r) {
    fprintf(stderr, "【红】%s 失败：VkResult %d\n", what, (int)r);
    exit(1);
}
#define VKOK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) die(#x, _r); } while (0)

// 内存属性的可读名字：零拷贝能不能做，就看有没有 HOST_VISIBLE | HOST_COHERENT 的类型
// Readable names for memory property flags: whether zero-copy is possible depends on there being a
// HOST_VISIBLE | HOST_COHERENT type.
static void flags_str(VkMemoryPropertyFlags f, char *out, size_t n) {
    struct { VkMemoryPropertyFlags b; const char *s; } t[] = {
        {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "DEVICE_LOCAL"},
        {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "HOST_VISIBLE"},
        {VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "HOST_COHERENT"},
        {VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "HOST_CACHED"},
        {VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "LAZILY_ALLOCATED"},
    };
    out[0] = 0;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (f & t[i].b) { strncat(out, t[i].s, n - strlen(out) - 1); strncat(out, "|", n - strlen(out) - 1); }
    if (!out[0]) snprintf(out, n, "(none)");
}

static uint32_t pick_memory(VkPhysicalDeviceProperties *pp, VkPhysicalDeviceMemoryProperties *mp,
                            uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "【红】找不到满足 %#x 的内存类型（可用的见上面的清单）\n", (unsigned)want);
    exit(1);
}

int main(void) {
    // ---- 实例 | instance ----
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "dm-vk-probe",
                             .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &ai };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r == VK_ERROR_INCOMPATIBLE_DRIVER) { fprintf(stderr, "【红】驱动不兼容 Vulkan 1.1\n"); return 1; }
    if (r != VK_SUCCESS) die("vkCreateInstance", r);
    uint32_t api = 0; vkEnumerateInstanceVersion(&api);
    printf("实例建立成功：loader 支持的 Vulkan %u.%u.%u\n", VK_API_VERSION_MAJOR(api),
           VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));

    uint32_t nd = 0;
    VKOK(vkEnumeratePhysicalDevices(inst, &nd, NULL));
    if (!nd) { fprintf(stderr, "【红】没有物理设备\n"); return 1; }
    VkPhysicalDevice *devs = calloc(nd, sizeof *devs);
    VKOK(vkEnumeratePhysicalDevices(inst, &nd, devs));
    printf("物理设备 %u 个\n", nd);
    for (uint32_t i = 0; i < nd; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(devs[i], &p);
        printf("  [%u] %s：驱动 %u.%u.%u，Vulkan %u.%u.%u，类型 %d\n", i, p.deviceName,
               VK_VERSION_MAJOR(p.driverVersion), VK_VERSION_MINOR(p.driverVersion), VK_VERSION_PATCH(p.driverVersion),
               VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               (int)p.deviceType);
    }

    VkPhysicalDevice phys = devs[0];
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(phys, &props);
    VkPhysicalDeviceMemoryProperties mem; vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    printf("=== 内存模型（%s）===\n", props.deviceName);
    for (uint32_t i = 0; i < mem.memoryHeapCount; i++)
        printf("  堆 %u：%llu MB%s\n", i, (unsigned long long)(mem.memoryHeaps[i].size >> 20),
               (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "（设备本地）" : "");
    int zero_copy_ok = -1;
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        char s[128]; flags_str(mem.memoryTypes[i].propertyFlags, s, sizeof s);
        printf("  类型 %u：堆 %u  %s\n", i, mem.memoryTypes[i].heapIndex, s);
        if ((mem.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            if (zero_copy_ok < 0) zero_copy_ok = i;
            if (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) zero_copy_ok = i;   // 设备本地且主机可见：最理想 | device-local and host-visible: ideal
        }
    }
    printf("  ⇒ 零拷贝（HOST_VISIBLE+HOST_COHERENT）可用：%s（类型 %d）\n",
           zero_copy_ok >= 0 ? "是" : "否", zero_copy_ok);

    // ---- 逻辑设备与队列 | logical device and queue ----
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    uint32_t qf_gfx = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        printf("  队列族 %u：%u 个，标志 %#x\n", i, qf[i].queueCount, qf[i].queueFlags);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qf_gfx == UINT32_MAX) qf_gfx = i;
    }
    if (qf_gfx == UINT32_MAX) { fprintf(stderr, "【红】没有图形队列族\n"); return 1; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = qf_gfx, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev = VK_NULL_HANDLE;
    VKOK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue q = VK_NULL_HANDLE; vkGetDeviceQueue(dev, qf_gfx, 0, &q);
    printf("逻辑设备与图形队列建立成功（队列族 %u）\n", qf_gfx);

    // ---- 离屏渲染：清屏 → 回读 | offscreen render: clear then read back ----
    const uint32_t W = 256, H = 256;
    VkImageCreateInfo imgci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img = VK_NULL_HANDLE; VKOK(vkCreateImage(dev, &imgci, NULL, &img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size, .memoryTypeIndex = pick_memory(&props, &mem, mr.memoryTypeBits, 0) };
    VkDeviceMemory imgmem = VK_NULL_HANDLE; VKOK(vkAllocateMemory(dev, &mai, NULL, &imgmem));
    VKOK(vkBindImageMemory(dev, img, imgmem, 0));
    printf("离屏贴图 %ux%u，显存 %llu KB\n", W, H, (unsigned long long)(mr.size >> 10));

    const VkDeviceSize bytes = (VkDeviceSize)W * H * 4;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf = VK_NULL_HANDLE; VKOK(vkCreateBuffer(dev, &bci, NULL, &buf));
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(dev, buf, &bmr);
    VkMemoryAllocateInfo bmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bmr.size,
                                  .memoryTypeIndex = pick_memory(&props, &mem, bmr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory bufmem = VK_NULL_HANDLE; VKOK(vkAllocateMemory(dev, &bmai, NULL, &bufmem));
    VKOK(vkBindBufferMemory(dev, buf, bufmem, 0));

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf_gfx };
    VkCommandPool pool = VK_NULL_HANDLE; VKOK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb = VK_NULL_HANDLE; VKOK(vkAllocateCommandBuffers(dev, &cbai, &cb));

    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VKOK(vkBeginCommandBuffer(cb, &bi));
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue cc = { .float32 = { 0.25f, 0.5f, 0.75f, 1.0f } };   // 非整数量：回读出来能一眼认出 | non-trivial values, so the readback is recognisable
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &b.subresourceRange);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy rg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                             .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rg);
    VKOK(vkEndCommandBuffer(cb));

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VKOK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    VKOK(vkQueueWaitIdle(q));

    void *p = NULL; VKOK(vkMapMemory(dev, bufmem, 0, bytes, 0, &p));
    const uint8_t *px = p;
    const uint8_t want[4] = { 64, 128, 191, 255 };   // 0.25/0.5/0.75/1.0 取整 | rounded 0.25/0.5/0.75/1.0
    int ok = 1, bad = 0;
    for (uint32_t i = 0; i < W * H; i++)
        for (int k = 0; k < 4; k++)
            if (px[i * 4 + k] != want[k]) { ok = 0; if (bad++ < 3) printf("  像素 %u 通道 %d：%u ≠ %u\n", i, k, px[i * 4 + k], want[k]); }
    printf("清屏回读：%s（首像素 %u,%u,%u,%u）\n", ok ? "全部符合" : "有偏差", px[0], px[1], px[2], px[3]);
    vkUnmapMemory(dev, bufmem);

    // ---- 收尾 | teardown ----
    vkDestroyBuffer(dev, buf, NULL); vkFreeMemory(dev, bufmem, NULL);
    vkDestroyImage(dev, img, NULL); vkFreeMemory(dev, imgmem, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    free(devs); free(qf);
    printf("Vulkan 通路验证完成\n");
    return ok ? 0 : 2;
}
