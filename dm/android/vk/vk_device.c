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
#include "vk_device.h"
#include "dm.h"
#include <stdlib.h>
#include <string.h>

static VkInstance g_inst;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qfi;
static VkPhysicalDeviceMemoryProperties g_mem;
static uint32_t g_zero_copy = UINT32_MAX;
static int g_tried;
static int g_dyn_render;                       // 动态渲染可用（回放层开通道的前提）| dynamic rendering available (the replay layer's precondition for opening passes)
static VkExtensionProperties *g_exts; static uint32_t g_nexts;

VkInstance vk_instance(void) { return g_inst; }
VkDevice vk_dev(void) { return g_dev; }
VkQueue vk_queue(void) { return g_queue; }
uint32_t vk_queue_family(void) { return g_qfi; }
const VkPhysicalDeviceMemoryProperties *vk_mem_props(void) { return &g_mem; }
uint32_t vk_zero_copy_type(void) { return g_zero_copy; }

int vk_have_ext(const char *name) {
    for (uint32_t i = 0; i < g_nexts; i++) if (!strcmp(g_exts[i].extensionName, name)) return 1;
    return 0;
}

int vk_have_dyn_render(void) { return g_dyn_render; }

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
    VkResult r = vkCreateInstance(&ici, NULL, &g_inst);
    if (r != VK_SUCCESS) { LOG("【红】建 Vulkan 实例失败：VkResult %d", (int)r); g_inst = VK_NULL_HANDLE; return; }

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
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_qfi = i; break; }
    free(qf);
    if (g_qfi == UINT32_MAX) { LOG("【红】没有图形队列族"); return; }

    const char *want[] = { "VK_KHR_external_memory_fd" };   // 内存换轨（把 Vulkan 内存摆到客体地址上）要用 | needed by the memory overlay (placing Vulkan memory at guest addresses)
    const char *en[4]; uint32_t nec = 0;
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++)
        if (vk_have_ext(want[i])) en[nec++] = want[i];
        else LOG("【红】设备没有扩展 %s：内存换轨会走不通", want[i]);

    // 动态渲染（Vulkan 1.3 核心）：回放层开渲染通道用它——比 VkRenderPass/VkFramebuffer 少一整套对象，
    // 而 NVN 的命令流本来就是"设目标 + 清除 + 绘制"的自由序列，与它正好对得上。
    // 先问再开：不支持就让回放层退回"只记状态不开通道"，不要把设备创建搞失败。
    // Dynamic rendering (core in Vulkan 1.3): the replay layer opens passes with it -- a whole set of objects fewer
    // than VkRenderPass/VkFramebuffer, and NVN's command stream is already a free sequence of "set targets, clear,
    // draw", which matches it exactly. Ask before enabling: if it is unsupported the replay layer falls back to
    // "record state, open no pass" rather than failing device creation.
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    { VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13 };
      vkGetPhysicalDeviceFeatures2(g_phys, &f2); }
    g_dyn_render = f13.dynamicRendering ? 1 : 0;
    if (!g_dyn_render) LOG("【黄】设备不支持动态渲染（Vulkan 1.3）：渲染通道这一层退回只记状态");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = g_qfi, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = g_dyn_render ? &f13 : NULL,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = nec,
                               .ppEnabledExtensionNames = en };
    r = vkCreateDevice(g_phys, &dci, NULL, &g_dev);
    if (r != VK_SUCCESS) { LOG("【红】建 Vulkan 逻辑设备失败：VkResult %d", (int)r); g_dev = VK_NULL_HANDLE; return; }
    vkGetDeviceQueue(g_dev, g_qfi, 0, &g_queue);

    // 1.3 的入口从设备上取（桩库不导出，见 vk_device.h）。取不到就把动态渲染关掉，让渲染通道那一层退回只记状态。
    // Fetch the 1.3 entry points from the device (the stub library does not export them; see vk_device.h). If the
    // lookup fails, dynamic rendering is turned off and the pass layer falls back to recording state only.
    dm_vkCmdBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(g_dev, "vkCmdBeginRendering");
    dm_vkCmdEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(g_dev, "vkCmdEndRendering");
    if (g_dyn_render && (!dm_vkCmdBeginRendering || !dm_vkCmdEndRendering)) {
        g_dyn_render = 0;
        LOG("【黄】取不到 vkCmdBeginRendering / vkCmdEndRendering：渲染通道退回只记状态");
    }

    // 零拷贝内存类型：DEVICE_LOCAL + HOST_VISIBLE + HOST_COHERENT（真机上是类型 4，heap 0 有 15 GB）
    // Zero-copy memory type: DEVICE_LOCAL + HOST_VISIBLE + HOST_COHERENT (type 4 on the real device, whose heap 0 has 15 GB)
    g_zero_copy = vk_memory_type(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);

    if (g_zero_copy == UINT32_MAX)
        LOG("【绿】Vulkan 设备就绪：%s（Vulkan %u.%u.%u）队列族 %u；零拷贝内存类型：无；扩展 fd=%s",
            p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
            VK_API_VERSION_PATCH(p.apiVersion), g_qfi, vk_have_ext("VK_KHR_external_memory_fd") ? "有" : "无");
    else
        LOG("【绿】Vulkan 设备就绪：%s（Vulkan %u.%u.%u）队列族 %u；零拷贝内存类型 %u；扩展 fd=%s；动态渲染 %s",
            p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
            VK_API_VERSION_PATCH(p.apiVersion), g_qfi, g_zero_copy, vk_have_ext("VK_KHR_external_memory_fd") ? "有" : "无",
            g_dyn_render && dm_vkCmdBeginRendering ? "有" : "无");
    if (g_zero_copy == UINT32_MAX) LOG("【红】没有 DEVICE_LOCAL+HOST_VISIBLE+HOST_COHERENT 的内存类型：零拷贝内存池做不了");
}
