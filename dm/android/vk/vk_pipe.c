// 安卓图形后端的管线层实现，见 vk_pipe.h 的文件头（三条决定形状的事实）。
// Implementation of the Android graphics backend's pipeline layer; see the header of vk_pipe.h for the three
// facts that shape it.
//
// 这一块先只做「描述符集布局 + 管线布局 + 计算管线」。图形管线要等影子状态补齐形状——回放层目前对
// vattr / vstream / vbuf / ubo / ssbo / tex / samp 这些绑定**只计数、不保存**（见 nvn_vk.c 的 OP_BIND_*），
// 而建图形管线恰恰要用到顶点属性/顶点流，画绘制要用到资源绑定。所以顺序是：
//   ① 布局与计算管线（本文件，可独立验证：计算管线不需要任何渲染状态）；
//   ② 绑定的影子存储 + 状态对象的解释 + 图形管线；
//   ③ DRAW / DRAW_IDX 真正下发。
// This piece covers only "descriptor set layout + pipeline layout + compute pipeline" for now. Graphics pipelines
// wait for the shadow state to be completed: the replay layer currently only **counts** the vattr / vstream /
// vbuf / ubo / ssbo / tex / samp bindings without storing them (see OP_BIND_* in nvn_vk.c), while building a
// graphics pipeline needs the vertex attributes and streams, and drawing needs the resource bindings. So the order is:
//   1. layouts and the compute pipeline (this file, independently verifiable: a compute pipeline needs no render state);
//   2. shadow storage for the bindings plus interpretation of the state objects, and the graphics pipeline;
//   3. really issuing DRAW / DRAW_IDX.
#include "vk_pipe.h"
#include "vk_device.h"
#include "dm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_B 160

typedef struct { uint32_t binding; VkDescriptorType type; VkShaderStageFlags stage; } bind_t;

static uint64_t g_n_dsl, g_n_lay, g_n_cpso, g_n_gpso, g_n_fail;

static VkShaderStageFlags stage_flag(int st) {
    switch (st) {
        case VK_STAGE_VS: return VK_SHADER_STAGE_VERTEX_BIT;
        case VK_STAGE_PS: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case VK_STAGE_CS: return VK_SHADER_STAGE_COMPUTE_BIT;
        case VK_STAGE_HS: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case VK_STAGE_DS: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        default: return 0;
    }
}

// 往合并表里加一条：同号则并入阶段（类型不一致要报红——同一号两种类型是翻译产物本身有问题）
// Add one entry to the merged table: an existing number merges its stages (a type mismatch is reported as a
// failure -- one number with two types means the translated artifacts themselves are inconsistent)
static int add_bind(bind_t *b, int *nb, int cap, uint32_t binding, VkDescriptorType type, VkShaderStageFlags sf) {
    for (int i = 0; i < *nb; i++) {
        if (b[i].binding != binding) continue;
        if (b[i].type != type) {
            static int said;
            if (!said++) LOG("【红】binding %u 的类型不一致（%d 与 %d）：按先见到的那个建布局", binding, (int)b[i].type, (int)type);
        }
        b[i].stage |= sf;
        return 0;
    }
    if (*nb >= cap) return -1;
    b[*nb].binding = binding; b[*nb].type = type; b[*nb].stage = sf;
    (*nb)++;
    return 0;
}

static int cmp_bind(const void *a, const void *b) {
    uint32_t x = ((const bind_t *)a)->binding, y = ((const bind_t *)b)->binding;
    return x < y ? -1 : x > y ? 1 : 0;
}

// SPIR-V 声明的种类 → Vulkan 描述符类型。缓冲一律用 dynamic（数据在零拷贝池里，每次绘制只换偏移）。
// A kind declared by the SPIR-V -> a Vulkan descriptor type. Buffers are always dynamic (the data sits in the
// zero-copy pools and only the offset changes from draw to draw).
static VkDescriptorType desc_type_of(int kind) {
    switch (kind) {
        case VK_SPV_UBO: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        case VK_SPV_SSBO: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        case VK_SPV_SAMPLED_IMAGE: case VK_SPV_IMAGE: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case VK_SPV_STORAGE_IMAGE: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case VK_SPV_SAMPLER: return VK_DESCRIPTOR_TYPE_SAMPLER;
        case VK_SPV_TEXEL: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        default: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;   // 认不出来时用最"能装"的一种，并报红（下面）| the most permissive kind when unrecognised, and it is reported below
    }
}

// 把若干着色器的绑定表合并成一张表：**以 SPIR-V 声明的绑定为准**（见 vk_spv.h——res.json 不保证列全，
// 真机上 3 条计算管线就是被这个坑掉的）。
// Merge the binding tables of several shaders: **the bindings the SPIR-V declares are authoritative** (see
// vk_spv.h -- res.json does not necessarily list them all, which is what sank 3 compute pipelines on the device).
static int merge_binds(const vk_shader *const *sh, int n, bind_t *out, int cap) {
    int nb = 0;
    for (int i = 0; i < n; i++) {
        const vk_shader *s = sh[i];
        if (!s) continue;
        VkShaderStageFlags sf = stage_flag(s->stage);
        if (!sf) { LOG("【红】着色器 %s 的阶段 %d 没有对应的 Vulkan 阶段位", s->key, s->stage); return -1; }
        for (uint32_t k = 0; k < s->nspv; k++) {
            const vk_spv_bind *b = &s->spv[k];
            if (b->set != 0) {   // 目前只建 set 0 的布局；出现了别的 set 要报红（否则那条绑定会缺）| only set 0 is built for now; another set must be reported or that binding would be missing
                static int said;
                if (!said++) LOG("【红】着色器 %s 的 binding %u 在 set %u：目前只建 set 0 的布局", s->key, b->binding, b->set);
                continue;
            }
            if (b->kind == VK_SPV_OTHER) {
                static int said2;
                if (!said2++) LOG("【黄】着色器 %s 的 binding %u 种类认不出来：按存储缓冲建（可能要改 vk_spv.c）", s->key, b->binding);
            }
            if (add_bind(out, &nb, cap, b->binding, desc_type_of(b->kind), sf) < 0) return -1;
        }
    }
    qsort(out, (size_t)nb, sizeof *out, cmp_bind);
    return nb;
}

// 布局的键：把合并后的表写成稳定的字符串（按 binding 升序，所以同一条表必然同一个键）
// Layout key: write the merged table into a stable string (sorted by binding, so one table always yields one key)
static void key_of_binds(const bind_t *b, int nb, char *out, size_t n) {
    size_t o = 0;
    for (int i = 0; i < nb && o + 32 < n; i++)
        o += (size_t)snprintf(out + o, n - o, "%u:%d:%x;", b[i].binding, (int)b[i].type, (unsigned)b[i].stage);
    out[o] = 0;
}

// 布局缓存：开放寻址、线性探测，存的是键字符串（不超过 1 KB）与两个句柄
// Layout cache: open addressing with linear probing, holding the key string (under 1 KB) and the two handles
#define LAY_TAB 1024
typedef struct { char *key; VkDescriptorSetLayout dsl; VkPipelineLayout lay; } lay_e;
static lay_e g_lay[LAY_TAB];

static uint32_t str_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

// 按一张绑定表建描述符集布局与管线布局。variant 见 vk_pipe.h（0 = 正常；诊断变体不进缓存）。
// Build a descriptor set layout and pipeline layout from one binding table. See vk_pipe.h for `variant`
// (0 = normal; the diagnostic variants are never cached).
static int build_layout(const bind_t *b, int nb, int variant, VkDescriptorSetLayout *out_dsl, VkPipelineLayout *out_lay, const char *key) {
    bind_t bb[MAX_B * 2];
    int n = 0;
    for (int i = 0; i < nb && n < MAX_B * 2; i++) bb[n++] = b[i];
    if (variant >= 2) {   // 把表里缺的 binding 号补进来：分辨"驱动要的号我没建"这类不匹配 | add the binding numbers the table lacks: tells apart "the driver wants a number I did not create"
        VkDescriptorType t = variant == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                            : variant == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
                                           : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        for (uint32_t num = 0; num < 16 && n < MAX_B * 2; num++) {
            int have = 0;
            for (int i = 0; i < nb; i++) if (b[i].binding == num) { have = 1; break; }
            if (!have) { bb[n].binding = num; bb[n].type = t; bb[n].stage = VK_SHADER_STAGE_COMPUTE_BIT; n++; }
        }
    }
    VkDescriptorSetLayoutBinding lb[MAX_B * 2];
    for (int i = 0; i < n; i++) {
        lb[i].binding = bb[i].binding;
        lb[i].descriptorType = bb[i].type;
        lb[i].descriptorCount = 1;      // 全库没有数组绑定（44,156 条实测都是 1）| no array bindings anywhere (all 44,156 measured are 1)
        lb[i].stageFlags = bb[i].stage;
        lb[i].pImmutableSamplers = NULL;
    }
    VkDescriptorSetLayoutCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = (uint32_t)n, .pBindings = lb };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkResult r = vkCreateDescriptorSetLayout(vk_dev(), &dci, NULL, &dsl);
    if (r != VK_SUCCESS) {
        LOG("【红】建描述符集布局失败：VkResult %d（%d 条绑定，键 %s）", (int)r, n, key);
        return 0;
    }
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 256 };
    VkPipelineLayoutCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &dsl,
                                       .pushConstantRangeCount = variant == 1 ? 1u : 0u,
                                       .pPushConstantRanges = variant == 1 ? &pcr : NULL };
    VkPipelineLayout lay = VK_NULL_HANDLE;
    r = vkCreatePipelineLayout(vk_dev(), &pci, NULL, &lay);
    if (r != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(vk_dev(), dsl, NULL);
        LOG("【红】建管线布局失败：VkResult %d（键 %s）", (int)r, key);
        return 0;
    }
    *out_dsl = dsl; *out_lay = lay;
    return 1;
}

static lay_e *lay_get_or_make(const vk_shader *const *sh, int n) {
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return NULL;
    bind_t b[MAX_B];
    int nb = merge_binds(sh, n, b, MAX_B);
    if (nb < 0) { g_n_fail++; return NULL; }
    char key[1024];
    key_of_binds(b, nb, key, sizeof key);

    uint32_t h = str_hash(key) & (LAY_TAB - 1);
    for (uint32_t k = 0; k < LAY_TAB; k++) {
        lay_e *e = &g_lay[(h + k) & (LAY_TAB - 1)];
        if (!e->key) {   // 空槽：建 | empty slot: build it here
            VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
            VkPipelineLayout lay = VK_NULL_HANDLE;
            if (!build_layout(b, nb, 0, &dsl, &lay, key)) { g_n_fail++; return NULL; }
            e->key = strdup(key);
            e->dsl = dsl; e->lay = lay;
            g_n_dsl++; g_n_lay++;
            return e;
        }
        if (!strcmp(e->key, key)) return e;
    }
    g_n_fail++;
    LOG("【红】布局缓存满了（%d 项）", LAY_TAB);
    return NULL;
}

VkDescriptorSetLayout vk_pipe_set_layout(const vk_shader *const *sh, int n) {
    lay_e *e = lay_get_or_make(sh, n);
    return e ? e->dsl : VK_NULL_HANDLE;
}
VkPipelineLayout vk_pipe_layout(const vk_shader *const *sh, int n) {
    lay_e *e = lay_get_or_make(sh, n);
    return e ? e->lay : VK_NULL_HANDLE;
}

// 计算管线缓存：按键（着色器的键）索引 | compute pipeline cache, keyed by the shader key
#define CPSO_TAB 256
typedef struct { char key[VK_SHADER_KEY_LEN + 1]; VkPipeline p; } cpso_e;
static cpso_e g_cpso[CPSO_TAB];

// 建计算管线。入口点全库都叫 main（抽核 150 份）；失败时把 VkResult 原样交给调用方。
// Create a compute pipeline. The entry point is `main` everywhere (150 files sampled); on failure the VkResult
// is handed straight back to the caller.
static VkResult make_compute(const vk_shader *cs, VkPipelineLayout lay, VkPipeline *out) {
    VkPipelineShaderStageCreateInfo st = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs->mod, .pName = "main" };
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                       .stage = st, .layout = lay };
    return vkCreateComputePipelines(vk_dev(), VK_NULL_HANDLE, 1, &ci, NULL, out);
}

VkPipeline vk_pipe_compute(const vk_shader *cs) {
    if (!cs || cs->stage != VK_STAGE_CS || cs->mod == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    uint32_t h = (uint32_t)(str_hash(cs->key)) & (CPSO_TAB - 1);
    for (uint32_t k = 0; k < CPSO_TAB; k++) {
        cpso_e *e = &g_cpso[(h + k) & (CPSO_TAB - 1)];
        if (!e->key[0]) {   // 空槽：建 | empty slot: build here
            const vk_shader *one[1] = { cs };
            VkPipelineLayout lay = vk_pipe_layout(one, 1);
            if (lay == VK_NULL_HANDLE) return VK_NULL_HANDLE;
            VkPipeline p = VK_NULL_HANDLE;
            VkResult r = make_compute(cs, lay, &p);
            if (r != VK_SUCCESS) {
                g_n_fail++;
                LOG("【红】建计算管线失败：VkResult %d（着色器 %s，uniform %u 存储 %u 贴图 %u）",
                    (int)r, cs->key, cs->ncbuf, cs->nsbuf, cs->ntex);
                return VK_NULL_HANDLE;
            }
            memcpy(e->key, cs->key, sizeof e->key);
            e->p = p;
            g_n_cpso++;
            return p;
        }
        if (!memcmp(e->key, cs->key, VK_SHADER_KEY_LEN)) return e->p;
    }
    g_n_fail++;
    LOG("【红】计算管线缓存满了（%d 项）", CPSO_TAB);
    return VK_NULL_HANDLE;
}

// 诊断变体：见 vk_pipe.h。不进缓存、不计入统计，成功时也不回收布局（它只在探针里对少量着色器用几次）。
// Diagnostic variants, see vk_pipe.h. Not cached, not counted, and the layout is not recycled even on success
// (this runs a handful of times per shader, in probes only).
VkPipeline vk_pipe_compute_variant(const vk_shader *cs, int variant) {
    if (!cs || cs->stage != VK_STAGE_CS || cs->mod == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    const vk_shader *one[1] = { cs };
    bind_t b[MAX_B];
    int nb = merge_binds(one, 1, b, MAX_B);
    if (nb < 0) return VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout lay = VK_NULL_HANDLE;
    if (!build_layout(b, nb, variant, &dsl, &lay, "diagnostic")) return VK_NULL_HANDLE;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = make_compute(cs, lay, &p);
    if (r != VK_SUCCESS) {
        vkDestroyPipelineLayout(vk_dev(), lay, NULL);
        vkDestroyDescriptorSetLayout(vk_dev(), dsl, NULL);
        return VK_NULL_HANDLE;
    }
    return p;
}

// ========== 图形管线 ==========
// ========== Graphics pipelines ==========

// NVN 图元 → VkPrimitiveTopology。线环 / 四边形 / 多边形在 Vulkan 里没有对应物，报一次黄并退化成最接近的那个
//（D2R 的实际绘制只用三角形与三角形带，所以这条退化路径先只保证"不静默"）。
// NVN primitive -> VkPrimitiveTopology. Line loops / quads / polygons have no Vulkan counterpart; warn once and
// degrade to the closest one (D2R's real draws use triangles and triangle strips only, so this degradation path
// exists mainly to not be silent).
VkPrimitiveTopology vk_topo_of(int p) {
    switch (p) {
        case 0x0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 0x1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case 0x2: { static int said; if (!said++) LOG("【黄】图元 LINE_LOOP 在 Vulkan 里没有对应物：按 LINE_STRIP 画"); return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; }
        case 0x3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 0x4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case 0x5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case 0x6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case 0x7: { static int said; if (!said++) LOG("【黄】图元 QUADS 在 Vulkan 里没有对应物：按 TRIANGLE_LIST 画"); return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; }
        case 0x8: { static int said; if (!said++) LOG("【黄】图元 QUAD_STRIP 在 Vulkan 里没有对应物：按 TRIANGLE_STRIP 画"); return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; }
        case 0x9: { static int said; if (!said++) LOG("【黄】图元 POLYGON 在 Vulkan 里没有对应物：按 TRIANGLE_FAN 画"); return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; }
        case 0xA: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
        case 0xB: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
        case 0xC: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        case 0xD: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
        case 0xE: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

// NVN 顶点属性格式（0x01..0x30）→ VkFormat。编号是有规律的：每 12 个一组（分量数 1..4），组内顺序是
// {UCharN, CharN, UChar, Char, Half, UShortN, ShortN, UShort, Short, Float, UInt, Int}
//（与苹果侧 vfmt 的表同一套编号，只是目标语言不同）。0x3D/0x3F/0x9A 是三个特例。
// NVN vertex attribute format (0x01..0x30) -> VkFormat. The numbering is regular: groups of 12 (component counts
// 1..4) with the order {UCharN, CharN, UChar, Char, Half, UShortN, ShortN, UShort, Short, Float, UInt, Int} inside
// a group (the same numbering as Apple's vfmt table, only a different target language). 0x3D/0x3F/0x9A are specials.
static const VkFormat g_attr_fmt[4][12] = {
    { VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT, VK_FORMAT_R16_SFLOAT,
      VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT, VK_FORMAT_R32_SFLOAT,
      VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT },
    { VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R16G16_SFLOAT,
      VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R32G32_SFLOAT,
      VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT },
    { VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R16G16B16_SFLOAT,
      VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R32G32B32_SFLOAT,
      VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT },
    { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R16G16B16A16_SFLOAT,
      VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R32G32B32A32_SFLOAT,
      VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT },
};
static int attr_fmt_vk(uint32_t f, VkFormat *out) {
    if (f >= 0x01 && f <= 0x30) {
        uint32_t n = (f - 1) / 12, k = (f - 1) % 12;
        if (n < 4) { *out = g_attr_fmt[n][k]; return 1; }
    }
    switch (f) {
        case 0x3D: *out = VK_FORMAT_A2B10G10R10_UNORM_PACK32; return 1;
        case 0x3F: *out = VK_FORMAT_B10G11R11_UFLOAT_PACK32; return 1;
        case 0x9A: *out = VK_FORMAT_B8G8R8A8_UNORM; return 1;
    }
    return 0;
}
// 回退格式：着色器声明的分量类型与个数（与苹果侧的 NVN_CT_* 一套）| fallback format: the component type and count the shader declares (Apple's NVN_CT_* set)
static VkFormat fallback_fmt(uint32_t ctype, uint32_t comps) {
    static const VkFormat f[3][4] = {
        { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT },
        { VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT },
        { VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT },
    };
    if (comps < 1) comps = 1;
    if (comps > 4) comps = 4;
    if (ctype > 2) ctype = 0;
    return f[ctype][comps - 1];
}

#define DUMMY_BIND 31   // 常量回退属性挂在这一号绑定上（1 个元素、每实例推进）| the constant-fallback attributes ride on this binding (one element, advancing per instance)

// 非实心填充要开 fillModeNonSolid 特性（设备侧没开就建不出管线），所以退回实心并只报一次
// Non-solid fill needs the fillModeNonSolid feature (not enabled on the device, so creation would fail), so fall
// back to solid and report it once
static void poly_warn_once(int mode) {
    static int said;
    if (!said++) LOG("【黄】多边形模式 %d 需要 fillModeNonSolid 特性：按实心画", mode);
}

#define GPSO_TAB 192
typedef struct { vk_gfx_state st; VkPipeline p; int used; } gpso_e;
static gpso_e g_gpso[GPSO_TAB];
static uint32_t bytes_hash(const void *p, size_t n) {
    const uint8_t *b = p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

VkPipeline vk_pipe_graphics(const vk_shader *vs, const vk_shader *ps, const vk_gfx_state *st) {
    if (!vs || !st || vs->mod == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    uint32_t h = bytes_hash(st, sizeof *st) & (GPSO_TAB - 1);
    for (uint32_t k = 0; k < GPSO_TAB; k++) {
        gpso_e *e = &g_gpso[(h + k) & (GPSO_TAB - 1)];
        if (!e->used) break;
        if (!memcmp(&e->st, st, sizeof *st)) return e->p;
    }

    const vk_shader *one[2] = { vs, ps };
    VkPipelineLayout lay = vk_pipe_layout(one, ps ? 2 : 1);
    if (lay == VK_NULL_HANDLE) { g_n_fail++; return VK_NULL_HANDLE; }

    VkPipelineShaderStageCreateInfo sh[2];
    int nsh = 0;
    sh[nsh++] = (VkPipelineShaderStageCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs->mod, .pName = "main" };
    if (ps && ps->mod != VK_NULL_HANDLE)
        sh[nsh++] = (VkPipelineShaderStageCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = ps->mod, .pName = "main" };

    // 顶点输入：以**顶点着色器声明的输入**为准（每一个都要有属性），格式/偏移/流来自影子属性表。
    // 游戏没绑、或流步长为 0（NVN 的"常量流"）、或格式认不出来的，挂到常量回退绑定上。
    // Vertex input: follows the **inputs the vertex shader declares** (each needs one), with format/offset/stream
    // from the shadow attribute table. Anything the game did not bind, or whose stream stride is 0 (NVN's
    // "constant stream"), or whose format is unrecognised, rides on the constant-fallback binding.
    VkVertexInputBindingDescription vbind[17];
    VkVertexInputAttributeDescription vatt[VK_SPV_MAXIN];
    uint32_t nb = 0, na = 0, dummies = 0;
    uint8_t stream_used[16];
    memset(stream_used, 0, sizeof stream_used);
    for (uint32_t i = 0; i < vs->nin && na < VK_SPV_MAXIN; i++) {
        uint32_t loc = vs->in[i].loc;
        if (loc >= 16) {   // 影子属性表按 NVN 属性号（0..15）索引，超出的一律走回退 | the shadow table is indexed by NVN attribute number (0..15); anything beyond falls back
            static int said;
            if (!said++) LOG("【黄】顶点着色器 %s 的输入 Location %u 超出 NVN 属性域：用常量回退", vs->key, loc);
            vatt[na].location = loc; vatt[na].format = fallback_fmt(vs->in[i].ctype, vs->in[i].comps);
            vatt[na].offset = 0; vatt[na].binding = DUMMY_BIND;
            na++; dummies++;
            continue;
        }
        const uint32_t st_ = st->attr[loc].stream < 16 ? st->attr[loc].stream : 0;
        VkFormat f = VK_FORMAT_UNDEFINED;
        int ok = st->attr[loc].valid && attr_fmt_vk(st->attr[loc].nvn_fmt, &f) && st->stream[st_].stride;
        if (!ok) {   // 回退：常量属性（全零的小缓冲，每个实例一个元素）| fallback: a constant attribute (a small zeroed buffer, one element per instance)
            vatt[na].location = loc; vatt[na].format = fallback_fmt(vs->in[i].ctype, vs->in[i].comps);
            vatt[na].offset = 0; vatt[na].binding = DUMMY_BIND;
            na++; dummies++;
            if (st->attr[loc].valid && st->attr[loc].nvn_fmt) {
                static int said2;
                if (!said2++) LOG("【黄】顶点属性格式 %#x 未映射：用常量回退", st->attr[loc].nvn_fmt);
            }
            continue;
        }
        vatt[na].location = loc; vatt[na].format = f;
        vatt[na].offset = st->attr[loc].offset; vatt[na].binding = 16 + st_;
        na++;
        if (!stream_used[st_]) {
            stream_used[st_] = 1;
            vbind[nb].binding = 16 + st_;
            vbind[nb].stride = st->stream[st_].stride;
            vbind[nb].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            if (st->stream[st_].divisor) {   // 每实例：Vulkan 的 divisor 需要扩展，暂按 1（见提交说明）| per instance: Vulkan's divisor needs an extension, so 1 for now (see the commit message)
                vbind[nb].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
                static int said3;
                if (st->stream[st_].divisor > 1 && !said3++) LOG("【黄】顶点流 %u 的 divisor=%u：暂按 1 处理（缺 VK_EXT_vertex_attribute_divisor）", st_, st->stream[st_].divisor);
            }
            nb++;
        }
    }
    if (dummies) {
        vbind[nb].binding = DUMMY_BIND; vbind[nb].stride = 16;
        vbind[nb].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        nb++;
    }

    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = nb ? vbind : NULL,
        .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = na ? vatt : NULL };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = st->topo, .primitiveRestartEnable = VK_FALSE };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        // 没有片元阶段时规范要求丢弃光栅化（探针用顶点着色器单独验管线时走这条）| the spec requires rassterizer discard when there is no fragment stage (the probe exercises a pipeline with the vertex shader alone)
        .rasterizerDiscardEnable = nsh == 1 ? VK_TRUE : VK_FALSE,
        .polygonMode = st->poly == VK_POLYGON_MODE_FILL ? VK_POLYGON_MODE_FILL
                     : (poly_warn_once(st->poly), VK_POLYGON_MODE_FILL),
        .cullMode = st->cull, .frontFace = st->front,
        .depthBiasEnable = st->bias_en ? VK_TRUE : VK_FALSE,
        .depthBiasConstantFactor = st->bias_factor, .depthBiasClamp = st->bias_clamp, .depthBiasSlopeFactor = st->bias_units,
        .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = st->samples ? st->samples : VK_SAMPLE_COUNT_1_BIT,
        .alphaToCoverageEnable = st->a2c ? VK_TRUE : VK_FALSE };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = st->depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = st->depth_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = st->depth_test ? st->depth_op : VK_COMPARE_OP_ALWAYS,
        .stencilTestEnable = st->stencil_en ? VK_TRUE : VK_FALSE };
    if (st->stencil_en) {
        ds.front = (VkStencilOpState){ .failOp = st->stencil_fail[0], .passOp = st->stencil_zpass[0],
            .depthFailOp = st->stencil_zfail[0], .compareOp = st->stencil_func[0],
            .compareMask = st->stencil_vmask[0], .writeMask = st->stencil_wmask[0], .reference = 0 };
        ds.back = (VkStencilOpState){ .failOp = st->stencil_fail[1], .passOp = st->stencil_zpass[1],
            .depthFailOp = st->stencil_zfail[1], .compareOp = st->stencil_func[1],
            .compareMask = st->stencil_vmask[1], .writeMask = st->stencil_wmask[1], .reference = 0 };
    }
    VkPipelineColorBlendAttachmentState cb[8];
    memset(cb, 0, sizeof cb);
    for (uint32_t i = 0; i < st->color_count && i < 8; i++) {
        cb[i].blendEnable = st->blend_en[i] ? VK_TRUE : VK_FALSE;
        cb[i].srcColorBlendFactor = st->src_rgb[i]; cb[i].dstColorBlendFactor = st->dst_rgb[i];
        cb[i].srcAlphaBlendFactor = st->src_a[i]; cb[i].dstAlphaBlendFactor = st->dst_a[i];
        cb[i].colorBlendOp = st->eq_rgb[i]; cb[i].alphaBlendOp = st->eq_a[i];
        cb[i].colorWriteMask = (VkColorComponentFlags)st->write_mask[i];
    }
    VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = st->color_count > 8 ? 8 : st->color_count, .pAttachments = cb };
    const VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dsi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 3, .pDynamicStates = dyn };
    // 动态渲染：附件的格式要在这里声明（管线与通道的兼容性由它决定）| dynamic rendering: the attachment formats are declared here (they decide pipeline/pass compatibility)
    VkPipelineRenderingCreateInfo prc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = st->color_count > 8 ? 8 : st->color_count, .pColorAttachmentFormats = st->color,
        .depthAttachmentFormat = st->depth_fmt, .stencilAttachmentFormat = st->stencil_fmt };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &prc,
        .stageCount = (uint32_t)nsh, .pStages = sh, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
        .pColorBlendState = &cbs, .pDynamicState = &dsi, .layout = lay };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(vk_dev(), VK_NULL_HANDLE, 1, &ci, NULL, &p);
    if (r != VK_SUCCESS) {
        g_n_fail++;
        LOG("【红】建图形管线失败：VkResult %d（顶点 %s 片元 %s，属性 %u 条、流 %u 条、目标 %u 个、拓扑 %d）",
            (int)r, vs->key, ps ? ps->key : "无", na, nb, st->color_count, (int)st->topo);
        return VK_NULL_HANDLE;
    }
    // 存进缓存（满了就丢掉最老的那个槽，只求不无限增长）| store in the cache (when full, drop the oldest slot rather than grow without bound)
    uint32_t slot = 0;
    for (uint32_t k = 0; k < GPSO_TAB; k++) { slot = (h + k) & (GPSO_TAB - 1); if (!g_gpso[slot].used) break; }
    if (g_gpso[slot].used) vkDestroyPipeline(vk_dev(), g_gpso[slot].p, NULL);
    g_gpso[slot].st = *st; g_gpso[slot].p = p; g_gpso[slot].used = 1;
    g_n_gpso++;
    return p;
}

void vk_pipe_stats(uint64_t *set_layouts, uint64_t *layouts, uint64_t *cpso, uint64_t *gpso, uint64_t *fail) {
    if (set_layouts) *set_layouts = g_n_dsl;
    if (layouts) *layouts = g_n_lay;
    if (cpso) *cpso = g_n_cpso;
    if (gpso) *gpso = g_n_gpso;
    if (fail) *fail = g_n_fail;
}
