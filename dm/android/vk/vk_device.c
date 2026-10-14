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
static int g_fill_non_solid;                   // 非实心多边形模式（线框等）| non-solid polygon modes (wireframe and such)
static int g_attr_divisor, g_attr_divisor_zero; // 顶点流每实例步进 > 1 / 可为 0 | per-instance vertex stream step > 1 / may be 0
static int g_aniso;                            // 各向异性过滤（采样器用）| anisotropic filtering (for samplers)
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
int vk_have_fill_non_solid(void) { return g_fill_non_solid; }
int vk_have_attr_divisor(void) { return g_attr_divisor; }
int vk_have_attr_divisor_zero(void) { return g_attr_divisor_zero; }
int vk_have_anisotropy(void) { return g_aniso; }

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

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = g_qfi, .queueCount = 1, .pQueuePriorities = &prio };
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
        LOG("【绿】Vulkan 设备就绪：%s（Vulkan %u.%u.%u）队列族 %u；零拷贝内存类型 %u；扩展 fd=%s；动态渲染 %s；非实心填充 %s；顶点每实例步进 %s%s",
            p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
            VK_API_VERSION_PATCH(p.apiVersion), g_qfi, g_zero_copy, vk_have_ext("VK_KHR_external_memory_fd") ? "有" : "无",
            g_dyn_render && dm_vkCmdBeginRendering ? "有" : "无", g_fill_non_solid ? "有" : "无",
            g_attr_divisor ? (g_attr_divisor_zero ? "有（含 0）" : "有") : "无", "");
    if (g_zero_copy == UINT32_MAX) LOG("【红】没有 DEVICE_LOCAL+HOST_VISIBLE+HOST_COHERENT 的内存类型：零拷贝内存池做不了");
}
