// 新地基 N4-g 的**能力探针**（只问不接管，N0 的形状）：它回答的唯一一件事 ——
// 「这台设备到底支不支持我们下面要用的几件东西，以及那些被校验层点名的东西在设备侧长什么样」。
//
// 为什么先问它（N4-g 的第 0 步）：
//   N4-e / N4-f / A1 之后只剩「顶点着色器输出」这一支（顶点数据与常量缓冲都正常、draw 在 API 层面合法）。
//   要拿到 `gl_Position` 的**真值**，唯一"不改产物"的硬办法是 `VK_EXT_transform_feedback`（把 vs 算出来的
//   输出抓进缓冲，于是"经变换后在不在裁剪体里 / 有没有 NaN"变成可以读的字节）。它有没有，决定 N4-g 走哪条路。
//   顺带把 A1 那一轮层点名的三个扩展与几条上限一起问掉（"扩展依赖缺失会让某些功能静默不生效"）。
//
// 自证要求（旧路踩过"一个初始值错、工具一次都没跑却看着像有效"）：每一问都打 `[问 n] 问题…` + `[答] 答案…`，
// 并且**必须分开报**"没问成"与"问了、答案是不是"：
//   · 扩展枚举要报"设备一共报了几个扩展"（不是"我找到了几个"）；
//   · 点名扩展要报"在第几条"（找不到就报"没找到"）；
//   · 特性结构体要逐 bool 打出来（0/1），不打"支持/不支持"这种二次加工的词。
// 末尾一段 `--- VK2_CAP_SUMMARY ---` 是关键结论键值（KEY=VALUE），供文档直接抄。
//
// 它与 `vk2_probe.c`（N0 的常数标定）是**两个**探针：那一个问"四个常数是多少"，这一个问"几件能力有没有"。
// 只是把它们放进同一个目录、同一个 CMake 里编。
//
// 跑法（一律走项目内 `.venv` Python，见 .codebuddy/rules/windows-python-only/RULE.mdc）：
//   & "<仓库>/.venv/Scripts/python.exe" -X utf8 .work/probes.py run vk2_cap_probe --serial 8b1f1e50
// 读数落 .work/logs/probe-<串号>-vk2_cap_probe[-tag].txt。
//
// ------------------------------------------------------------------------------------------------
// vk2_cap_probe.c -- the N4-g capability probe (ask only, take over nothing; the shape of N0).
// It answers one thing: whether this device supports the handful of things N4-g wants to use, and what
// the things the validation layer named look like from the device's side. Getting the real value of
// `gl_Position` without touching any artifact needs VK_EXT_transform_feedback; whether it exists decides
// which route N4-g takes. Every question prints "[问 n] …" and "[答] …", the extension enumeration
// always reports "how many extensions the device reported" (not "how many I recognised"), and feature
// structs are printed bool by bool (0/1) rather than as the words "supported/unsupported".
#define _GNU_SOURCE 1
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

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

// ================================================================= 实例 / 物理设备
static VkInstance g_inst;
static VkPhysicalDevice g_phys;
static uint32_t g_ndev;
static char g_devname[256];

static int open_instance(void) {
    uint32_t ver = 0;
    VkResult r = vkEnumerateInstanceVersion(&ver);
    printf("# 载入器（loader）版本：vkEnumerateInstanceVersion -> VkResult %d，apiVersion %u.%u.%u（%u）\n",
           (int)r, VK_VERSION_MAJOR(ver), VK_VERSION_MINOR(ver), VK_VERSION_PATCH(ver), ver);
    VkApplicationInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "dm-vk2-cap-probe";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    r = vkCreateInstance(&ici, NULL, &g_inst);
    if (r != VK_SUCCESS) { printf("【红】vkCreateInstance 失败：VkResult %d\n", (int)r); return 0; }
    if (vkEnumeratePhysicalDevices(g_inst, &g_ndev, NULL) != VK_SUCCESS || g_ndev == 0) {
        printf("【红】vkEnumeratePhysicalDevices 报了 0 个设备\n");
        return 0;
    }
    VkPhysicalDevice *dv = calloc(g_ndev, sizeof *dv);
    if (vkEnumeratePhysicalDevices(g_inst, &g_ndev, dv) != VK_SUCCESS) { free(dv); return 0; }
    g_phys = dv[0];   // 这台机上只有一个物理设备 | there is only one physical device here
    free(dv);

    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(g_phys, &p);
    snprintf(g_devname, sizeof g_devname, "%s", p.deviceName);
    printf("# 物理设备 %u 个；用第 0 个：%s（deviceType %d、驱动 %u.%u.%u、apiVersion %u.%u.%u）\n",
           g_ndev, p.deviceName, (int)p.deviceType,
           VK_VERSION_MAJOR(p.driverVersion), VK_VERSION_MINOR(p.driverVersion), VK_VERSION_PATCH(p.driverVersion),
           VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
    return 1;
}

// ================================================================= ① 设备扩展清单
static char **g_ext;
static uint32_t g_next;

static void q_extensions(void) {
    uint32_t n = 0;
    VkResult r = vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, NULL);
    if (r != VK_SUCCESS || n == 0) { A("【红】设备扩展枚举失败：VkResult %d、n = %u", (int)r, n); return; }
    VkExtensionProperties *ep = calloc(n, sizeof *ep);
    uint32_t m = n;
    r = vkEnumerateDeviceExtensionProperties(g_phys, NULL, &m, ep);
    if (r != VK_SUCCESS) { A("【红】第二次枚举扩展失败：VkResult %d", (int)r); free(ep); return; }
    g_ext = calloc(m, sizeof *g_ext);
    g_next = m;
    for (uint32_t i = 0; i < m; i++) g_ext[i] = strdup(ep[i].extensionName);
    A("设备一共报了 **%u** 个设备扩展（首次问得 %u、本次取到 %u）⇒ 下面的「第几条」是在这 %u 条里数的",
      n, n, m, m);
    free(ep);
}

static int ext_index(const char *name) {
    for (uint32_t i = 0; i < g_next; i++)
        if (strcmp(g_ext[i], name) == 0) return (int)i;
    return -1;
}

static void q_named_ext(const char *name, const char *why) {
    int ix = ext_index(name);
    if (ix < 0) A("%-50s **没找到**（%s）", name, why);
    else        A("%-50s 在，第 %d 条（共 %u 条）（%s）", name, ix, g_next, why);
}

// ================================================================= ② 特性（逐 bool）
static void q_features(void) {
    VkPhysicalDeviceTransformFeedbackFeaturesEXT tf = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT };
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT il = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT };
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vd = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT };
    VkPhysicalDeviceProvokingVertexFeaturesEXT pv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT };
    VkPhysicalDeviceVulkan11Features f11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    void *tail = &f11;
    if (ext_index("VK_EXT_provoking_vertex") >= 0) { pv.pNext = tail; tail = &pv; }
    if (ext_index("VK_EXT_vertex_attribute_divisor") >= 0) { vd.pNext = tail; tail = &vd; }
    if (ext_index("VK_EXT_fragment_shader_interlock") >= 0) { il.pNext = tail; tail = &il; }
    if (ext_index("VK_EXT_transform_feedback") >= 0) { tf.pNext = tail; tail = &tf; }

    VkPhysicalDeviceFeatures2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    q.pNext = tail;
    vkGetPhysicalDeviceFeatures2(g_phys, &q);
    VkPhysicalDeviceFeatures *f = &q.features;

    printf("\n# 特性结构体链：%s\n", (tail == &f11) ? "只有 f11（四个扩展都不在设备上）"
           : "f11 + 在设备上的那些扩展各自的特性结构");
    printf("[答] **VK_EXT_transform_feedback**：transformFeedback = %d、geometryStreams = %d"
           "（%s）\n",
           (int)tf.transformFeedback, (int)tf.geometryStreams,
           (ext_index("VK_EXT_transform_feedback") < 0) ? "**扩展不在设备上 ⇒ 这两个 bool 没有意义**"
                                                        : "扩展在设备上，这两个 bool 就是答案");
    printf("[答] **VK_EXT_fragment_shader_interlock**：fragmentShaderSampleInterlock = %d、"
           "fragmentShaderPixelInterlock = %d、fragmentShaderShadingRateInterlock = %d（%s）\n",
           (int)il.fragmentShaderSampleInterlock, (int)il.fragmentShaderPixelInterlock,
           (int)il.fragmentShaderShadingRateInterlock,
           (ext_index("VK_EXT_fragment_shader_interlock") < 0) ? "**扩展不在设备上 ⇒ 上面三个 bool 没有意义**"
                                                              : "扩展在设备上，这三个 bool 就是答案");
    printf("[答] **VK_EXT_vertex_attribute_divisor**：vertexAttributeInstanceRateDivisor = %d、"
           "vertexAttributeInstanceRateZeroDivisor = %d\n",
           (int)vd.vertexAttributeInstanceRateDivisor, (int)vd.vertexAttributeInstanceRateZeroDivisor);
    printf("[答] VK_EXT_provoking_vertex：provokingVertexLast = %d、"
           "transformFeedbackPreservesProvokingVertex = %d\n",
           (int)pv.provokingVertexLast, (int)pv.transformFeedbackPreservesProvokingVertex);
    printf("[答] Vulkan11（我们在用的那条链）：shaderDrawParameters = %d、"
           "shaderDrawParameters 之外我们没用的那些位不列\n", (int)f11.shaderDrawParameters);
    printf("[答] **核心特性（N4-g 最要紧的一格）**：shaderClipDistance = %d、shaderCullDistance = %d、"
           "geometryShader = %d、tessellationShader = %d、multiViewport = %d、depthClamp = %d、"
           "independentBlend = %d、dualSrcBlend = %d\n",
           (int)f->shaderClipDistance, (int)f->shaderCullDistance, (int)f->geometryShader,
           (int)f->tessellationShader, (int)f->multiViewport, (int)f->depthClamp,
           (int)f->independentBlend, (int)f->dualSrcBlend);
}

// ================================================================= ③ 上限
static void q_limits(void) {
    // transform feedback 的那几条上限**不在** VkPhysicalDeviceLimits 里，在
    // VkPhysicalDeviceTransformFeedbackPropertiesEXT（Vulkan 1.3 起也可由 VkPhysicalDeviceProperties2 链出来）。
    VkPhysicalDeviceTransformFeedbackPropertiesEXT tfp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    p2.pNext = &tfp;
    vkGetPhysicalDeviceProperties2(g_phys, &p2);
    const VkPhysicalDeviceLimits *l = &p2.properties.limits;
    printf("\n[答] 裁剪/剔除距离：maxClipDistances = %u、maxCullDistances = %u、"
           "**maxCombinedClipAndCullDistances = %u**\n",
           l->maxClipDistances, l->maxCullDistances, l->maxCombinedClipAndCullDistances);
    printf("[答] 顶点输出：maxVertexOutputComponents = %u、maxVertexInputAttributes = %u、"
           "maxVertexInputBindings = %u\n",
           l->maxVertexOutputComponents, l->maxVertexInputAttributes, l->maxVertexInputBindings);
    printf("[答] transform feedback 上限（VkPhysicalDeviceTransformFeedbackPropertiesEXT）："
           "maxTransformFeedbackStreams = %u、maxTransformFeedbackBuffers = %u、"
           "maxTransformFeedbackBufferSize = %llu 字节、maxTransformFeedbackStreamDataSize = %u、"
           "maxTransformFeedbackBufferDataSize = %u、maxTransformFeedbackBufferDataStride = %u；"
           "布尔：transformFeedbackQueries = %d、transformFeedbackStreamsLinesTriangles = %d、"
           "transformFeedbackRasterizationStreamSelect = %d、transformFeedbackDraw = %d\n",
           tfp.maxTransformFeedbackStreams, tfp.maxTransformFeedbackBuffers,
           (unsigned long long)tfp.maxTransformFeedbackBufferSize,
           tfp.maxTransformFeedbackStreamDataSize, tfp.maxTransformFeedbackBufferDataSize,
           tfp.maxTransformFeedbackBufferDataStride,
           (int)tfp.transformFeedbackQueries, (int)tfp.transformFeedbackStreamsLinesTriangles,
           (int)tfp.transformFeedbackRasterizationStreamSelect, (int)tfp.transformFeedbackDraw);
    printf("[答] 视口：maxViewports = %u、maxViewportDimensions = [%u, %u]\n",
           l->maxViewports, l->maxViewportDimensions[0], l->maxViewportDimensions[1]);
}

// ================================================================= ④ 真的建一次管线路径上的东西
// 只建"描述符集布局 + 管线布局"不够回答本轮的题目；这里只做一件**离线可判**的事：
// 按我们游戏路径那份**扩展清单**建一次逻辑设备（扩展位与依赖一起），把"建不成"与"建成了"分开。
static void q_device_ext_deps(void) {
    const char *want[] = { "VK_KHR_external_memory_fd",
                           "VK_ANDROID_external_memory_android_hardware_buffer",
                           "VK_EXT_vertex_attribute_divisor" };
    const char *en[8]; uint32_t nec = 0;
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++)
        if (ext_index(want[i]) >= 0) en[nec++] = want[i];
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof qci);
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof dci);
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = nec; dci.ppEnabledExtensionNames = en;
    VkDevice d = VK_NULL_HANDLE;
    VkResult r = vkCreateDevice(g_phys, &dci, NULL, &d);
    printf("[答] 按游戏路径那份扩展清单（%u 条：", nec);
    for (uint32_t i = 0; i < nec; i++) printf("%s%s", i ? "、" : "", en[i]);
    printf("）vkCreateDevice -> VkResult %d（0 = 成功；缺依赖时层会在这里喊 01387）\n", (int)r);
    if (d) vkDestroyDevice(d, NULL);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("===== vk2_cap_probe（N4-g 的能力探针，只问不接管）=====\n");
    if (!open_instance()) return 2;

    Q("VK_EXT_transform_feedback 在不在？设备一共报了几个设备扩展？（这一问决定 N4-g 走「抓 gl_Position」还是替代路）");
    q_extensions();
    q_named_ext("VK_EXT_transform_feedback", "N4-g 想用它抓 gl_Position");
    q_named_ext("VK_EXT_fragment_shader_interlock", "A1 的层点名过（20 条）");
    q_named_ext("VK_EXT_vertex_attribute_divisor", "顶点流每实例步进 > 1 在用");
    q_named_ext("VK_EXT_provoking_vertex", "顺带（它的特性里有 transformFeedbackPreservesProvokingVertex）");
    q_named_ext("VK_EXT_depth_clip_control", "顺带（vkjson 里有它）");
    q_named_ext("VK_EXT_external_memory_host", "旧路的零拷贝方案用过（顺带核一下还在不在）");

    Q("那几个扩展的**特性结构体**逐 bool 是多少？（「问了」与「答案是是不是」必须分得开）");
    q_features();

    Q("几条与本轮直接相关的**上限**各是多少？（裁剪/剔除距离、顶点输出、transform feedback、视口）");
    q_limits();

    Q("按游戏路径那份扩展清单建一次逻辑设备（顺便回答层喊的 01387 是不是依赖没满足）");
    q_device_ext_deps();

    printf("\n--- VK2_CAP_SUMMARY ---\n");
    printf("VK2_CAP_DEVICE=%s\n", g_devname);
    printf("VK2_CAP_EXT_COUNT=%u\n", g_next);
    {
        int ix = ext_index("VK_EXT_transform_feedback");
        printf("VK2_CAP_TF_EXT=%s\n", ix >= 0 ? "yes" : "no");
        printf("VK2_CAP_TF_EXT_INDEX=%d\n", ix);
    }
    printf("VK2_CAP_CLIPDIST_EXT=%s\n", ext_index("VK_EXT_depth_clip_control") >= 0 ? "yes" : "no");
    printf("VK2_CAP_FSI_EXT=%s\n", ext_index("VK_EXT_fragment_shader_interlock") >= 0 ? "yes" : "no");
    printf("VK2_CAP_VAD_EXT=%s\n", ext_index("VK_EXT_vertex_attribute_divisor") >= 0 ? "yes" : "no");
    printf("VK2_CAP_DONE=1\n");
    return 0;
}
