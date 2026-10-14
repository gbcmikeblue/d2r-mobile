// M2 关键能力探针：Vulkan 能不能直接把「宿主自己那块内存」当成 GPU 内存用。
//
// 为什么这是决定性的一问：Metal 那边内存池是 [mem, mem+size) 外扩到页边界后 newBufferWithBytesNoCopy
// 直接包成零拷贝 MTLBuffer（苹果有统一内存，任意宿主指针都能包）。Vulkan 没有对应物，要等价的
// 「客体在 CPU 侧写、GPU 侧读同一份」，只有 VK_EXT_external_memory_host 这一条路：
// 把宿主指针导入成 VkDeviceMemory，再在它上面建 VkBuffer。
//
// 没有这个扩展就得改设计（池子改由 Vulkan 侧分配、或者退化成拷贝），所以先测，别猜。
//
// M2 capability probe: can Vulkan take a piece of the host's own memory and use it as GPU memory?
//
// Why this is decisive: on the Metal side a memory pool is [mem, mem+size) expanded to page boundaries and
// wrapped in a zero-copy MTLBuffer with newBufferWithBytesNoCopy (Apple has unified memory, so any host
// pointer can be wrapped). Vulkan has no equivalent, and the only route to the same "the guest writes on
// the CPU and the GPU reads the same bytes" is VK_EXT_external_memory_host: import the host pointer as a
// VkDeviceMemory and build a VkBuffer on it.
//
// Without that extension the design has to change (pools allocated on the Vulkan side, or falling back to
// copies), so measure it rather than guess.
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

int main(void) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "dm-hostmem-probe",
                             .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst = VK_NULL_HANDLE; VKOK(vkCreateInstance(&ici, NULL, &inst));

    uint32_t n = 0; VKOK(vkEnumeratePhysicalDevices(inst, &n, NULL));
    VkPhysicalDevice *dv = calloc(n, sizeof *dv); VKOK(vkEnumeratePhysicalDevices(inst, &n, dv));
    VkPhysicalDevice phys = dv[0];
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(phys, &p);
    printf("设备：%s（Vulkan %u.%u.%u）\n", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
           VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));

    // ---- 扩展清单：找 VK_EXT_external_memory_host | extension list: look for VK_EXT_external_memory_host ----
    uint32_t ne = 0; VKOK(vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, NULL));
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    VKOK(vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, ep));
    int have = 0;
    printf("设备扩展 %u 个；与本项目相关的：\n", ne);
    const char *care[] = { "VK_EXT_external_memory_host", "VK_KHR_external_memory", "VK_EXT_host_image_copy",
                           "VK_EXT_descriptor_indexing", "VK_KHR_timeline_semaphore", "VK_KHR_swapchain",
                           "VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign" };
    for (unsigned i = 0; i < sizeof care / sizeof care[0]; i++)
        for (uint32_t j = 0; j < ne; j++)
            if (!strcmp(ep[j].extensionName, care[i])) {
                printf("  [有] %s（修订 %u）\n", care[i], ep[j].specVersion);
                if (!strcmp(care[i], "VK_EXT_external_memory_host")) have = 1;
            }
    int missing = 0;
    for (unsigned i = 0; i < sizeof care / sizeof care[0]; i++) {
        int f = 0;
        for (uint32_t j = 0; j < ne; j++) if (!strcmp(ep[j].extensionName, care[i])) f = 1;
        if (!f) { missing++; printf("  [无] %s\n", care[i]); }
    }
    printf("⇒ VK_EXT_external_memory_host：%s\n", have ? "有" : "没有");
    if (!have) {
        printf("   没有这个扩展就没法把宿主指针直接当 GPU 内存用，零拷贝内存池要改设计（见 docs/ANDROID_PLAN）\n");
        printf("   Without it a host pointer cannot serve as GPU memory, so the zero-copy memory pool needs a different design (see docs/ANDROID_PLAN)\n");
        return 3;
    }

    // ---- 对齐要求 | alignment requirement ----
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hp };
    vkGetPhysicalDeviceProperties2(phys, &p2);
    printf("minImportedHostPointerAlignment = %llu（页大小 %d）\n",
           (unsigned long long)hp.minImportedHostPointerAlignment, getpagesize());

    // ---- 逻辑设备（开这个扩展）| logical device with the extension on ----
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
    const char *en[1] = { "VK_EXT_external_memory_host" };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = en };
    VkDevice dev = VK_NULL_HANDLE; VKOK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue q = VK_NULL_HANDLE; vkGetDeviceQueue(dev, qfi, 0, &q);
    PFN_vkGetMemoryHostPointerPropertiesEXT get_host_props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT");
    if (!get_host_props) { fprintf(stderr, "【红】取不到 vkGetMemoryHostPointerPropertiesEXT\n"); return 1; }

    // ---- 造一块「宿主自己的内存」，对齐要求满足 | make a piece of "the host's own memory" meeting the alignment ----
    const size_t want = 64 * 1024;
    size_t span = want + (hp.minImportedHostPointerAlignment ? hp.minImportedHostPointerAlignment : 4096);
    void *raw = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("mmap"); return 1; }
    uintptr_t al = hp.minImportedHostPointerAlignment ? hp.minImportedHostPointerAlignment : 4096;
    void *guest = (void *)(((uintptr_t)raw + al - 1) & ~(al - 1));   // 客体看到的就是这个指针 | this is the pointer the guest would see
    printf("宿主内存 %p（对齐后 %p，%zu 字节）\n", raw, guest, want);

    VkMemoryHostPointerPropertiesEXT hpp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    VkResult r = get_host_props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, guest, &hpp);
    if (r != VK_SUCCESS) die("vkGetMemoryHostPointerPropertiesEXT", r);
    printf("该指针可用的内存类型位：%#x\n", hpp.memoryTypeBits);

    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t mi = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(hpp.memoryTypeBits & (1u << i))) continue;
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mi = i; break; }
        if (mi == UINT32_MAX) mi = i;
    }
    if (mi == UINT32_MAX) { fprintf(stderr, "【红】导入指针没有可用的内存类型\n"); return 1; }
    char fbuf[128] = { 0 };
    {
        struct { VkMemoryPropertyFlags b; const char *s; } t[] = {
            {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "DEVICE_LOCAL"}, {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "HOST_VISIBLE"},
            {VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "HOST_COHERENT"}, {VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "HOST_CACHED"} };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
            if (mp.memoryTypes[mi].propertyFlags & t[i].b) { strncat(fbuf, t[i].s, sizeof fbuf - strlen(fbuf) - 1); strncat(fbuf, "|", sizeof fbuf - strlen(fbuf) - 1); }
    }
    printf("选用内存类型 %u（%s）\n", mi, fbuf);

    VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = guest };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
                                 .allocationSize = want, .memoryTypeIndex = mi };
    VkDeviceMemory pmem = VK_NULL_HANDLE;
    r = vkAllocateMemory(dev, &mai, NULL, &pmem);
    if (r != VK_SUCCESS) { die("导入宿主指针为 VkDeviceMemory", r); }
    printf("导入成功：客体那块内存已经是 VkDeviceMemory\n");

    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = want,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer gbuf = VK_NULL_HANDLE; VKOK(vkCreateBuffer(dev, &bci, NULL, &gbuf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, gbuf, &mr);
    if (mr.size > want || mr.alignment > al) printf("  注意：缓冲要求 size %llu / alignment %llu（我们给了 %zu / %llu）\n",
        (unsigned long long)mr.size, (unsigned long long)mr.alignment, want, (unsigned long long)al);
    VKOK(vkBindBufferMemory(dev, gbuf, pmem, 0));

    // 参照缓冲：Vulkan 自己分配 | reference buffer: allocated by Vulkan itself
    VkBufferCreateInfo rci = bci;
    VkBuffer rbuf = VK_NULL_HANDLE; VKOK(vkCreateBuffer(dev, &rci, NULL, &rbuf));
    VkMemoryRequirements rmr; vkGetBufferMemoryRequirements(dev, rbuf, &rmr);
    uint32_t rmi = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((rmr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { rmi = i; break; }
    VkMemoryAllocateInfo rmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rmr.size, .memoryTypeIndex = rmi };
    VkDeviceMemory rmem = VK_NULL_HANDLE; VKOK(vkAllocateMemory(dev, &rmai, NULL, &rmem));
    VKOK(vkBindBufferMemory(dev, rbuf, rmem, 0));
    void *rmap = NULL; VKOK(vkMapMemory(dev, rmem, 0, want, 0, &rmap));

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi };
    VkCommandPool pool = VK_NULL_HANDLE; VKOK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb = VK_NULL_HANDLE; VKOK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };

    // ① 客体侧写 → GPU 读 | (1) the guest writes, the GPU reads
    uint8_t *gp = guest;
    for (size_t i = 0; i < want; i++) gp[i] = (uint8_t)(i * 13 + 5);
    VkBufferCopy cp = { .size = want };
    VKOK(vkBeginCommandBuffer(cb, &bi));
    vkCmdCopyBuffer(cb, gbuf, rbuf, 1, &cp);
    VKOK(vkEndCommandBuffer(cb));
    VKOK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    VKOK(vkQueueWaitIdle(q));
    int d1 = 1; size_t bad1 = 0;
    for (size_t i = 0; i < want; i++) if (((uint8_t *)rmap)[i] != (uint8_t)(i * 13 + 5)) { d1 = 0; bad1 = i; break; }

    // ② GPU 写 → 客体侧读 | (2) the GPU writes, the guest reads
    VKOK(vkResetCommandPool(dev, pool, 0));
    VKOK(vkBeginCommandBuffer(cb, &bi));
    vkCmdFillBuffer(cb, gbuf, 0, want, 0x5A5A5A5Au);
    VKOK(vkEndCommandBuffer(cb));
    VKOK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    VKOK(vkQueueWaitIdle(q));
    int d2 = 1; size_t bad2 = 0;
    for (size_t i = 0; i < want; i++) if (gp[i] != 0x5A) { d2 = 0; bad2 = i; break; }

    printf("宿主内存直通 GPU：客体写→GPU读 %s，GPU写→客体读 %s\n", d1 ? "通" : "不通", d2 ? "通" : "不通");
    if (!d1) printf("  客体→GPU 首处不符：%zu（读到 %u，期望 %u）\n", bad1, ((uint8_t *)rmap)[bad1], (uint8_t)(bad1 * 13 + 5));
    if (!d2) printf("  GPU→客体 首处不符：%zu（读到 %u，期望 90）\n", bad2, gp[bad2]);

    vkUnmapMemory(dev, rmem);
    vkDestroyBuffer(dev, gbuf, NULL); vkDestroyBuffer(dev, rbuf, NULL);
    vkFreeMemory(dev, pmem, NULL); vkFreeMemory(dev, rmem, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    printf("宿主内存直通验证完成\n");
    return (d1 && d2) ? 0 : 2;
}
