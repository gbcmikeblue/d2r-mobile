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

void vk_pipe_stats(uint64_t *set_layouts, uint64_t *layouts, uint64_t *cpso, uint64_t *gpso, uint64_t *fail) {
    if (set_layouts) *set_layouts = g_n_dsl;
    if (layouts) *layouts = g_n_lay;
    if (cpso) *cpso = g_n_cpso;
    if (gpso) *gpso = g_n_gpso;
    if (fail) *fail = g_n_fail;
}
