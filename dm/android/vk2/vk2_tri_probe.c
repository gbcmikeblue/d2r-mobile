// 新地基 N4-g 的**最小自证探针**：把"我们自己的一个三角形"画进一张离屏图，读回来数像素。
//
// 它回答的那一句话（写死，读数的三格就是它）：
//   **「`gl_PerVertex` 里声明了 `gl_ClipDistance[8]` + `gl_CullDistance[1]`（合计 9 > 本设备
//   maxCombinedClipAndCullDistances = 8），而且这两个数组**从没被写过**」这一条，是不是就足以让
//   图元在光栅化之前消失？**
//
// 为什么要问它（本轮的证据链）：
//   N4-e 把"画面在哪"收进"光栅化之前"（`DM_RASTER_BRUTE` 全放开仍全零）；N4-f 排掉"顶点数据 / 常量缓冲"；
//   A1 的校验层给出一条负结果 —— **16 种 VUID 里没有一条是 draw 层面的用法错误** ⇒ 几何是"自己算没了"；
//   而两臂共有的 15 种 VUID 里，**唯一落在"顶点输出那一层"的就是**
//   `VUID-RuntimeSpirv-maxCombinedClipAndCullDistances-00710`（A 臂 ×20 / B 臂 ×20）。
//   离线核对（`.work/logs/clipdist-use.txt`、`.work/logs/dis-f88d27f131d71049.txt`）把它的形状钉死了：
//   **2345 份顶点着色器都声明 ClipDistance[8] + CullDistance[1]，而 0 份读写过它们**；
//   那个形状是译文器**无条件**产出的（`dm/third_party/ryujinx/…/CodeGen/Spirv/Declarations.cs:454-476`）。
//
// 为什么不用游戏的绘制来问（本轮约束）：抓 `gl_Position` 的硬办法（`VK_EXT_transform_feedback`）要求给
//   游戏那份 SPIR-V 加 `XfbBuffer`/`Offset` 装饰（离线核对：6923 份里 **0 份**有），而本轮**不许改产物**
//   ⇒ 退到"我自己画一个三角形"。这也正好是本轮计划里写的替代路 ②。
//
// 一次跑四格（**只差一个变量**，见 `--` 自证行）：
//   A / `tri_min.vert`  ：glslang 对**没提过裁剪距离**的顶点着色器产出的形状 ⇒ Clip[1] + Cull[1] = 2 ≤ 8
//   B / `tri_clip.vert` ：照游戏那份的形状显式声明     ⇒ Clip[8] + Cull[1] = 9 > 8
//   两格的片段着色器、图、清除色、视口、绘制调用**完全一样**；两份顶点着色器都**不写**裁剪/剔除距离。
//   另外各跑一次"把 `shaderClipDistance`/`shaderCullDistance` 启用"的版本（`DM_TRI_CLIPDIST=1`），
//   回答"打开那两个特性会不会改变这一格的答案"。
//
// 自证（N0 的形状）：每一格都打 `[问 n] …` + `[答] …`，并且打出**判定性字段**：
//   设备名、shaderClipDistance/shaderCullDistance 的设备支持值与本轮启用值、
//   maxCombinedClipAndCullDistances、两份顶点着色器**从文件里读进来的字节数**、
//   管线创建结果、绘制调用次数、以及读回图的"红像素数 / 非黑像素数 / 全图像素数"。
//   "没问成"与"问了、答案是不是"必须分得开：读不到 SPIR-V 就【红】报出来，不许静默跳过。
//
// 用法（一律 `.venv` Python；SPIR-V 由 `.work/n4g_tri_spv.py` 生成、`.work/n4g_tri_push.py` 推上去）：
//   & "<仓库>/.venv/Scripts/python.exe" -X utf8 .work/probes.py run vk2_tri_probe --serial 8b1f1e50
//   & "<仓库>/.venv/Scripts/python.exe" -X utf8 .work/probes.py run vk2_tri_probe --serial 8b1f1e50 \
//         --env "DM_TRI_CLIPDIST=1" --tag clipdist
// 读数落 .work/logs/probe-<串号>-vk2_tri_probe[-tag].txt。
//
// ------------------------------------------------------------------------------------------------
// vk2_tri_probe.c -- the minimal self-proof probe of N4-g: draw our own triangle into an offscreen
// image and count pixels. It answers one question: is "gl_PerVertex declares gl_ClipDistance[8] +
// gl_CullDistance[1] (9 > this device's maxCombinedClipAndCullDistances = 8) and never writes either"
// enough to make primitives disappear before rasterization? Two vertex shaders differing only in that
// declaration, everything else (fragment shader, image, clear colour, viewport, the draw call)
// identical, plus an optional run with shaderClipDistance/shaderCullDistance enabled. Part of neither
// the host nor any APK.
#define _GNU_SOURCE 1
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#define IMG_W 256
#define IMG_H 256
#define DEFAULT_SPV_DIR "/data/local/tmp/dm/tri"

static int g_qn;
static void Q(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("\n[问 %d] ", ++g_qn); vprintf(fmt, ap); printf("\n");
    va_end(ap); fflush(stdout);
}
static void A(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("[答] "); vprintf(fmt, ap); printf("\n");
    va_end(ap); fflush(stdout);
}
#define CHK(x, what) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    A("【红】%s 失败：VkResult %d", what, (int)_r); return 0; } } while (0)

static VkInstance g_inst;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static VkQueue g_q;
static uint32_t g_qfi;
static VkPhysicalDeviceVulkan13Features g_f13;
static VkPhysicalDeviceFeatures g_core;
static int g_have_cd, g_use_cd;
// 1.3 的两个入口从设备上取（桩库不导出）| the two 1.3 entry points, fetched from the device (not in the stub)
static PFN_vkCmdBeginRendering p_begin;
static PFN_vkCmdEndRendering p_end;

// ---------------------------------------------------------------- 读文件（SPIR-V）
static uint32_t *read_spv(const char *path, uint32_t *words) {
    FILE *f = fopen(path, "rb");
    if (!f) { A("【红】打不开 %s（SPIR-V 是不是没推上去？先跑 .work/n4g_tri_push.py）", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (sz % 4)) { A("【红】%s 大小 %ld 不合法", path, sz); fclose(f); return NULL; }
    uint32_t *w = malloc((size_t)sz);
    if (fread(w, 1, (size_t)sz, f) != (size_t)sz) { A("【红】%s 读不满", path); fclose(f); free(w); return NULL; }
    fclose(f);
    if (w[0] != 0x07230203) { A("【红】%s 不是 SPIR-V（magic = 0x%08x）", path, w[0]); free(w); return NULL; }
    *words = (uint32_t)sz / 4;
    return w;
}

// ---------------------------------------------------------------- 设备
static int open_device(void) {
    VkApplicationInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "dm-vk2-tri-probe";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    CHK(vkCreateInstance(&ici, NULL, &g_inst), "vkCreateInstance");
    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(g_inst, &n, NULL) != VK_SUCCESS || n == 0) { A("【红】没有物理设备"); return 0; }
    VkPhysicalDevice *dv = calloc(n, sizeof *dv);
    vkEnumeratePhysicalDevices(g_inst, &n, dv);
    g_phys = dv[0];
    free(dv);

    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(g_phys, &p);
    printf("# 物理设备 %s（apiVersion %u.%u.%u、驱动 %u.%u.%u）\n", p.deviceName,
           VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion),
           VK_VERSION_MAJOR(p.driverVersion), VK_VERSION_MINOR(p.driverVersion), VK_VERSION_PATCH(p.driverVersion));

    // 动态渲染（Vulkan 1.3 核心）要用：离屏这一格就用它开通道，比 VkRenderPass/VkFramebuffer 少一整套对象
    g_f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    q.pNext = &g_f13;
    vkGetPhysicalDeviceFeatures2(g_phys, &q);
    g_core = q.features;
    g_have_cd = (g_core.shaderClipDistance && g_core.shaderCullDistance) ? 1 : 0;
    const char *env = getenv("DM_TRI_CLIPDIST");
    g_use_cd = ((env && env[0] == '1') && g_have_cd) ? 1 : 0;

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq ? nq : 1, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, qf);
    g_qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_qfi = i; break; }
    free(qf);
    if (g_qfi == UINT32_MAX) { A("【红】没有图形队列族"); return 0; }
    if (!g_f13.dynamicRendering) { A("【红】设备不支持动态渲染，这个探针要它开通道"); return 0; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof qci);
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g_qfi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof dci);
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkPhysicalDeviceFeatures2 feats = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    feats.features = g_core;
    // 只把这两个核心特性**显式清零**，其余保持"设备支持什么就开什么"（与游戏路径一致的地方：
    // 游戏路径走 `feats.features = core` + 逐个覆盖）。`DM_TRI_CLIPDIST=1` 才打开它们。
    feats.features.shaderClipDistance = g_use_cd ? VK_TRUE : VK_FALSE;
    feats.features.shaderCullDistance = g_use_cd ? VK_TRUE : VK_FALSE;
    g_f13.dynamicRendering = VK_TRUE;
    g_f13.pNext = NULL;
    feats.pNext = &g_f13;
    dci.pNext = &feats;
    CHK(vkCreateDevice(g_phys, &dci, NULL, &g_dev), "vkCreateDevice");
    vkGetDeviceQueue(g_dev, g_qfi, 0, &g_q);
    // 安卓的 libvulkan.so 是**桩库**（只导出到 Vulkan 1.1）：1.3 的入口直接引用会链接失败
    // （`undefined symbol: vkCmdBeginRendering`），必须 `vkGetDeviceProcAddr` 取 —— 与游戏路径
    // `dm_vkCmdBeginRendering`（`vk_device.h:146-154`）同一个套路，见 vk_device.c:626-631。
    // Android's libvulkan.so is a **stub** (it exports up to Vulkan 1.1), so a 1.3 entry point referenced
    // directly fails to link; it must be fetched with vkGetDeviceProcAddr -- the same pattern the game path
    // uses (dm_vkCmdBeginRendering, vk_device.h:146-154 / vk_device.c:626-631).
    p_begin = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(g_dev, "vkCmdBeginRendering");
    p_end = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(g_dev, "vkCmdEndRendering");
    if (!p_begin || !p_end) { A("【红】取不到 vkCmdBeginRendering/vkCmdEndRendering（桩库不导出）"); return 0; }
    return 1;
}

// ---------------------------------------------------------------- 内存 / 图 / 缓冲
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags need) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & need) == need) return i;
    return UINT32_MAX;
}

typedef struct {
    VkImage img; VkDeviceMemory img_mem; VkImageView iv;
    VkBuffer buf; VkDeviceMemory buf_mem; void *map;
    VkRenderingAttachmentInfo att;
} target_t;

static int make_target(target_t *t) {
    VkImageCreateInfo ici;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent.width = IMG_W; ici.extent.height = IMG_H; ici.extent.depth = 1;
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHK(vkCreateImage(g_dev, &ici, NULL, &t->img), "vkCreateImage");
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_dev, t->img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size };
    mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) { A("【红】找不到 DEVICE_LOCAL 内存类型"); return 0; }
    CHK(vkAllocateMemory(g_dev, &mai, NULL, &t->img_mem), "vkAllocateMemory(image)");
    CHK(vkBindImageMemory(g_dev, t->img, t->img_mem, 0), "vkBindImageMemory");

    VkImageViewCreateInfo ivc;
    memset(&ivc, 0, sizeof ivc);
    ivc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivc.image = t->img;
    ivc.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivc.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivc.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    CHK(vkCreateImageView(g_dev, &ivc, NULL, &t->iv), "vkCreateImageView");

    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof bci);
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = (VkDeviceSize)IMG_W * IMG_H * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CHK(vkCreateBuffer(g_dev, &bci, NULL, &t->buf), "vkCreateBuffer(readback)");
    vkGetBufferMemoryRequirements(g_dev, t->buf, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mem_type(mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) { A("【红】找不到 HOST_VISIBLE|HOST_COHERENT 内存类型"); return 0; }
    CHK(vkAllocateMemory(g_dev, &mai, NULL, &t->buf_mem), "vkAllocateMemory(readback)");
    CHK(vkBindBufferMemory(g_dev, t->buf, t->buf_mem, 0), "vkBindBufferMemory");
    CHK(vkMapMemory(g_dev, t->buf_mem, 0, VK_WHOLE_SIZE, 0, &t->map), "vkMapMemory");

    memset(&t->att, 0, sizeof t->att);
    t->att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    t->att.imageView = t->iv;
    t->att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    t->att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    t->att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    t->att.clearValue.color.float32[0] = 0.0f;
    t->att.clearValue.color.float32[1] = 0.0f;
    t->att.clearValue.color.float32[2] = 0.0f;
    t->att.clearValue.color.float32[3] = 1.0f;
    return 1;
}

// ---------------------------------------------------------------- 管线
typedef struct { VkShaderModule vs, fs; VkPipeline pipe; } cell_t;

static int make_cell(cell_t *c, const uint32_t *vsw, uint32_t vsn, const uint32_t *fsw, uint32_t fsn,
                     const char *vsname) {
    VkShaderModuleCreateInfo sm;
    memset(&sm, 0, sizeof sm);
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = vsn * 4; sm.pCode = vsw;
    CHK(vkCreateShaderModule(g_dev, &sm, NULL, &c->vs), "vkCreateShaderModule(vert)");
    sm.codeSize = fsn * 4; sm.pCode = fsw;
    CHK(vkCreateShaderModule(g_dev, &sm, NULL, &c->fs), "vkCreateShaderModule(frag)");

    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = c->vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = c->fs; st[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba;
    memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb;
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkPipelineLayoutCreateInfo plc;
    memset(&plc, 0, sizeof plc);
    plc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    CHK(vkCreatePipelineLayout(g_dev, &plc, NULL, &pl), "vkCreatePipelineLayout");

    VkPipelineRenderingCreateInfo prc;
    memset(&prc, 0, sizeof prc);
    prc.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    prc.colorAttachmentCount = 1;
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    prc.pColorAttachmentFormats = &fmt;

    VkGraphicsPipelineCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &prc;
    ci.stageCount = 2; ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    ci.layout = pl;
    VkResult r = vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &ci, NULL, &c->pipe);
    vkDestroyPipelineLayout(g_dev, pl, NULL);
    A("%-22s 管线创建 VkResult %d（0 = 成功）", vsname, (int)r);
    if (r != VK_SUCCESS) return 0;
    return 1;
}

// ---------------------------------------------------------------- 画一次 + 读回
typedef struct { uint32_t red, nonblack, total; double mean_r; } stats_t;

static int draw_and_measure(target_t *t, cell_t *c, stats_t *s) {
    VkCommandPoolCreateInfo cpci;
    memset(&cpci, 0, sizeof cpci);
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = g_qfi;
    VkCommandPool cp = VK_NULL_HANDLE;
    CHK(vkCreateCommandPool(g_dev, &cpci, NULL, &cp), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof cbai);
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cp; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    CHK(vkAllocateCommandBuffers(g_dev, &cbai, &cb), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHK(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");

    VkRenderingInfo ri;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = IMG_W; ri.renderArea.extent.height = IMG_H;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &t->att;
    p_begin(cb, &ri);
    VkViewport vpp = { 0.0f, 0.0f, (float)IMG_W, (float)IMG_H, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { IMG_W, IMG_H } };
    vkCmdSetViewport(cb, 0, 1, &vpp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipe);
    vkCmdDraw(cb, 3, 1, 0, 0);
    p_end(cb);

    VkMemoryBarrier mb;
    memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    VkBufferImageCopy bic;
    memset(&bic, 0, sizeof bic);
    bic.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bic.imageExtent.width = IMG_W; bic.imageExtent.height = IMG_H; bic.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->buf, 1, &bic);
    CHK(vkEndCommandBuffer(cb), "vkEndCommandBuffer");

    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof fci);
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    CHK(vkCreateFence(g_dev, &fci, NULL, &fence), "vkCreateFence");
    VkSubmitInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VkResult r = vkQueueSubmit(g_q, 1, &si, fence);
    if (r != VK_SUCCESS) { A("【红】vkQueueSubmit 失败：VkResult %d", (int)r); return 0; }
    r = vkWaitForFences(g_dev, 1, &fence, VK_TRUE, 5000000000ull);
    if (r != VK_SUCCESS) { A("【红】等栅栏失败：VkResult %d", (int)r); return 0; }

    const uint8_t *px = (const uint8_t *)t->map;
    memset(s, 0, sizeof *s);
    s->total = IMG_W * IMG_H;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < s->total; i++) {
        const uint8_t *p = px + (size_t)i * 4;
        sum += p[0];
        if (p[0] || p[1] || p[2]) s->nonblack++;
        if (p[0] > 200 && p[1] < 60 && p[2] < 60) s->red++;
    }
    s->mean_r = (double)sum / (double)s->total;
    vkDestroyFence(g_dev, fence, NULL);
    vkDestroyCommandPool(g_dev, cp, NULL);
    return 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *dir = (argc > 1) ? argv[1] : DEFAULT_SPV_DIR;
    printf("===== vk2_tri_probe（N4-g 的最小自证：我们自己的三角形）=====\n");
    printf("# SPIR-V 目录：%s\n", dir);
    if (!open_device()) return 2;

    Q("这一轮的两个变量各是什么？（设备支持 / 本轮启用，必须能分开看）");
    A("shaderClipDistance：设备支持 %d、本轮启用 %d", (int)g_core.shaderClipDistance, g_use_cd);
    A("shaderCullDistance：设备支持 %d、本轮启用 %d", (int)g_core.shaderCullDistance, g_use_cd);
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(g_phys, &p);
        A("maxClipDistances = %u、maxCullDistances = %u、**maxCombinedClipAndCullDistances = %u**",
          p.limits.maxClipDistances, p.limits.maxCullDistances, p.limits.maxCombinedClipAndCullDistances);
    }
    A("动态渲染 = %d（开通道用它）", (int)g_f13.dynamicRendering);

    char path[512];
    uint32_t vsn_min = 0, vsn_clip = 0, fsn = 0;
    snprintf(path, sizeof path, "%s/tri_min.vert.spv", dir);
    uint32_t *vs_min = read_spv(path, &vsn_min);
    snprintf(path, sizeof path, "%s/tri_clip.vert.spv", dir);
    uint32_t *vs_clip = read_spv(path, &vsn_clip);
    snprintf(path, sizeof path, "%s/tri.frag.spv", dir);
    uint32_t *fs = read_spv(path, &fsn);
    if (!vs_min || !vs_clip || !fs) return 3;

    Q("两份顶点着色器从文件里读进来多少字节，管线建不建得成？（这两个文件就是两个变量的载体）");
    A("tri_min.vert.spv = %u 字节（glslang 对「没提过裁剪距离」的着色器产出的形状：Clip[1] + Cull[1] = 2）",
      vsn_min * 4);
    A("tri_clip.vert.spv = %u 字节（照游戏那份的形状：Clip[8] + Cull[1] = 9 > 上限 %u）",
      vsn_clip * 4, (unsigned)8);
    A("tri.frag.spv = %u 字节（两份共用同一个片段着色器：输出纯红 (1,0,0,1)）", fsn * 4);

    target_t t;
    memset(&t, 0, sizeof t);
    if (!make_target(&t)) return 4;

    cell_t cells[2];
    memset(cells, 0, sizeof cells);
    if (!make_cell(&cells[0], vs_min, vsn_min, fs, fsn, "A：tri_min")) return 5;
    if (!make_cell(&cells[1], vs_clip, vsn_clip, fs, fsn, "B：tri_clip")) return 5;

    Q("每一格画一次三角形（3 个顶点、无顶点缓冲、视口铺满 256×256、清除成黑色）之后读到什么？");
    const char *names[2] = { "A：tri_min（Clip[1]+Cull[1] = 2）", "B：tri_clip（Clip[8]+Cull[1] = 9）" };
    stats_t got[2];
    memset(got, 0, sizeof got);
    printf("\n%-34s %10s %12s %12s %10s\n", "格", "红像素", "非黑像素", "总像素", "R 均值");
    for (int i = 0; i < 2; i++) {
        if (!draw_and_measure(&t, &cells[i], &got[i])) { A("【红】第 %d 格没跑成（见上面）", i + 1); return 6; }
        printf("%-34s %10u %12u %12u %10.3f\n", names[i], got[i].red, got[i].nonblack, got[i].total,
               got[i].mean_r);
    }
    A("判据（写死在提示词里）：**A 落屏、B 不落屏** ⇒ 「声明 9 个裁剪/剔除距离、却从不写」这一条就是成因；"
      "**两格都落屏** ⇒ 这一支被排除（回到其余两臂共有的那几条）；"
      "**两格都不落屏** ⇒ 这个探针自己的通道/状态有问题，先修探针（不许据此下成因结论）");

    printf("\n--- VK2_TRI_SUMMARY ---\n");
    printf("VK2_TRI_CLIPDIST_ENABLED=%d\n", g_use_cd);
    printf("VK2_TRI_A_MIN_RED=%u\n", got[0].red);
    printf("VK2_TRI_A_MIN_NONBLACK=%u\n", got[0].nonblack);
    printf("VK2_TRI_B_CLIP_RED=%u\n", got[1].red);
    printf("VK2_TRI_B_CLIP_NONBLACK=%u\n", got[1].nonblack);
    printf("VK2_TRI_DONE=1\n");
    return 0;
}
