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
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 常量回退属性用的哑顶点缓冲的大小：4096 个实例 × 16 字节。为什么不是 16 字节——回退属性的步进是
// "每实例一个元素"，实例数由游戏定，缓冲太小就会读到界外（真机开了 robustness 也不该指望它）。
// Size of the dummy vertex buffer behind the constant-fallback attributes: 4096 instances x 16 bytes. Why not 16
// bytes -- a fallback attribute advances one element per instance, the instance count is the game's to choose, and
// a buffer that small would be read out of bounds (robustness on the device is not something to rely on).
#define DUMMY_VBUF_SIZE (64u << 10)

#define MAX_B 160

// count：这条绑定的元素个数，直接来自 SPIR-V（见 vk_spv.h）——不写死 1，那会是个静默假设
// count: how many elements this binding has, straight from the SPIR-V (see vk_spv.h) -- not hardcoded to 1, which
// would be a silent assumption
typedef vk_pipe_bind bind_t;   // 与对外的那张表同一个结构（描述符写入要按同一顺序填动态偏移）| the same struct as the exported table (descriptor writing fills dynamic offsets in the same order)

static uint64_t g_n_dsl, g_n_lay, g_n_cpso, g_n_gpso, g_n_fail;

// "顶点属性的分量类别与着色器声明不符"的累计次数（见下面建管线处那段核对）。**这个计数器是必须的**：
// 那条判据本身曾经因为两个 opcode 写反而恒等于 float（见 vk_spv.c 的编号注释），于是它报了一次假告警；
// 修好之后这条要么是 0、要么得给出**总数**才够用，所以按绘制那句日志的同一套写法（前 8 例 + 每满 1024 例）打。
// Running count of "a vertex attribute's component class disagrees with the shader's declaration" (see the
// cross-check where the pipeline is built). **The counter is necessary**: the test itself was constantly float for a
// while because two opcodes were swapped (see the numbering note in vk_spv.c), so it once raised a false warning; after
// the fix this line has to be either zero or carry a **total**, hence the same idiom as the draw log line (the first
// eight plus one per 1024).
static _Atomic uint64_t g_attr_class_bad;

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

// ---------------------------------------------------------------------------
// N4-k（TOOLS §H.3）：`DM_PIPE_DUMP=1`（**缺省关**）——在 `vkCreateGraphicsPipelines` **之前**把**最终结构体**
// 逐字段打一行：`gfx_state_desc()` 打的是**影子状态**（`vk_gfx_state`），而真正交给驱动的是这里拼出来的
// `VkGraphicsPipelineCreateInfo` 与它挂着的那几个 `VkPipeline*StateCreateInfo`。`poly` 那条 bug 的形态正是
// "**状态表对、翻译那一步错**"，这一行专治这一类（代价 ≈ 30 行、一轮读数、不改时序、不装层）。
// 节流：**前 32 条 + 每满 256 条一条**。第一版是"前 8 + 每 2048"，而一轮只有 ~86~200 条图形管线 ⇒
// **绘制 84 那条 3 附件的管线一条都没打出来**（它排在 8 条之后、又永远到不了 2048）——坑 ⑱/㉒的又一次现场：
// 节流必须按"这一轮实际会发生多少"定。
// ---------------------------------------------------------------------------
static int pipe_dump_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_PIPE_DUMP");
        v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        if (v) LOG("【查】DM_PIPE_DUMP=1：每次建图形管线前把**最终结构体**逐字段打一行"
                   "（polygonMode / cullMode / frontFace / depthBias / rasterizerDiscard / 各附件的格式与写掩码与混合 / "
                   "深度模板格式 / 动态状态）；节流 = 前 32 条 + 每满 256 条一条");
    }
    return v;
}

static void pipe_dump(const VkGraphicsPipelineCreateInfo *ci, const VkPipelineRasterizationStateCreateInfo *rs,
                      const VkPipelineMultisampleStateCreateInfo *ms, const VkPipelineDepthStencilStateCreateInfo *ds,
                      const VkPipelineColorBlendStateCreateInfo *cbs, const VkPipelineRenderingCreateInfo *prc,
                      const VkPipelineInputAssemblyStateCreateInfo *ia, const vk_shader *vs, const vk_shader *ps,
                      uint32_t na, uint32_t nb, uint32_t ndiv) {
    static _Atomic uint64_t n;
    uint64_t k = atomic_fetch_add(&n, 1) + 1;
    if (k > 32 && (k & 255) != 0) return;
    char at[260]; int ao = 0; at[0] = 0;
    for (uint32_t i = 0; i < cbs->attachmentCount && i < 8; i++) {
        const VkPipelineColorBlendAttachmentState *a = &cbs->pAttachments[i];
        int w = snprintf(at + ao, sizeof at - (size_t)ao, "%s#%u 格式 %d 掩码 %#x 混合%s(%d→%d op %d/%d)",
                         i ? "；" : "", i, (int)(prc->pColorAttachmentFormats ? prc->pColorAttachmentFormats[i] : -1),
                         (unsigned)a->colorWriteMask, a->blendEnable ? "开" : "关",
                         (int)a->srcColorBlendFactor, (int)a->dstColorBlendFactor, (int)a->colorBlendOp, (int)a->alphaBlendOp);
        if (w <= 0 || (size_t)(ao + w) >= sizeof at) break;
        ao += w;
    }
    if (!ao) snprintf(at, sizeof at, "（没有颜色附件）");
    LOG("【查】管线最终结构体（第 %llu 条）：顶点 %s 片元 %s；阶段 %u；拓扑 %d（重启 %d）；"
        "**光栅化** polygonMode %d、cullMode %d、frontFace %d、rasterizerDiscard %d、depthClamp %d、"
        "lineWidth %g；**深度偏置** en %d（%g/%g/%g）；"
        "**深度模板** test %d write %d op %d、stencil %d、格式 %d/%d；采样 %d、a2c %d；"
        "颜色附件 %u → %s；动态状态 %u 个；顶点输入 属性 %u 条 / 流 %u 条 / divisor %u 条",
        (unsigned long long)k, vs ? vs->key : "无", ps ? ps->key : "无", ci->stageCount,
        (int)ia->topology, ia->primitiveRestartEnable ? 1 : 0,
        (int)rs->polygonMode, (int)rs->cullMode, (int)rs->frontFace,
        rs->rasterizerDiscardEnable ? 1 : 0, rs->depthClampEnable ? 1 : 0, (double)rs->lineWidth,
        rs->depthBiasEnable ? 1 : 0, (double)rs->depthBiasConstantFactor, (double)rs->depthBiasClamp,
        (double)rs->depthBiasSlopeFactor,
        ds->depthTestEnable ? 1 : 0, ds->depthWriteEnable ? 1 : 0, (int)ds->depthCompareOp,
        ds->stencilTestEnable ? 1 : 0, (int)prc->depthAttachmentFormat, (int)prc->stencilAttachmentFormat,
        (int)ms->rasterizationSamples, ms->alphaToCoverageEnable ? 1 : 0,
        cbs->attachmentCount, at, ci->pDynamicState ? ci->pDynamicState->dynamicStateCount : 0,
        na, nb, ndiv);
}

// 往合并表里加一条：同号则并入阶段（类型不一致要报红——同一号两种类型是翻译产物本身有问题）
// Add one entry to the merged table: an existing number merges its stages (a type mismatch is reported as a
// failure -- one number with two types means the translated artifacts themselves are inconsistent)
static int add_bind(bind_t *b, int *nb, int cap, uint32_t set, uint32_t binding, uint32_t count, VkDescriptorType type,
                    VkShaderStageFlags sf, const char *skey, int kind) {
    for (int i = 0; i < *nb; i++) {
        if (b[i].binding != binding || b[i].set != set) continue;
        if (b[i].type != type) {
            // 两个阶段对同一条 binding 的解读不同：说清是哪两个着色器、各是什么种类——只报 VkDescriptorType 的数字
            // 看不出是谁（真机上出现过一次"8 与 1"）。
            // The two stages read the same binding differently: name the two shaders and their kinds -- reporting only
            // the VkDescriptorType numbers leaves no way to tell who is who (the device showed "8 versus 1" once).
            static int said;
            if (!said++) LOG("【红】binding %u 的类型不一致：%s 说是 %s（VkDescriptorType %d），%s 说是 %s（%d）；按先见到的建布局——两个阶段不一致本身就值得查",
                             binding, b[i].stage & VK_SHADER_STAGE_VERTEX_BIT ? "顶点" : b[i].stage & VK_SHADER_STAGE_FRAGMENT_BIT ? "片元" : "计算",
                             vk_spv_kind_name(b[i].kind), (int)b[i].type, skey, vk_spv_kind_name(kind), (int)type);
        }
        if (count > b[i].count) b[i].count = count;   // 两个阶段各声明一个数组时取大的那个 | when both stages declare an array, keep the larger count
        b[i].stage |= sf;
        return 0;
    }
    if (*nb >= cap) return -1;
    b[*nb].set = set; b[*nb].binding = binding; b[*nb].count = count ? count : 1; b[*nb].type = type; b[*nb].stage = sf; b[*nb].kind = kind;
    (*nb)++;
    return 0;
}

static int cmp_bind(const void *a, const void *b) {
    const bind_t *x = a, *y = b;
    if (x->set != y->set) return x->set < y->set ? -1 : 1;
    return x->binding < y->binding ? -1 : x->binding > y->binding ? 1 : 0;
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
            // 两个空间：set 0 = 顶点阶段、set 1 = 片元阶段（见 vk_shader.c 装载处的"按阶段分空间"）。
            // 出现别的 set 说明产物形态变了，报红并按设计跳过。
            // Two spaces: set 0 = the vertex stage, set 1 = the fragment stage (see "a space per stage" at the load
            // site in vk_shader.c). Another set means the artifacts changed shape: reported and skipped by design.
            if (b->set > 1) {
                static int said;
                if (!said++) LOG("【红】着色器 %s 的 binding %u 在 set %u：只认识 0/1 两个空间", s->key, b->binding, b->set);
                continue;
            }
            if (b->kind == VK_SPV_OTHER) {
                static int said2;
                if (!said2++) LOG("【黄】着色器 %s 的 binding %u 种类认不出来：按存储缓冲建（可能要改 vk_spv.c）", s->key, b->binding);
            }
            if (add_bind(out, &nb, cap, b->set, b->binding, b->count, desc_type_of(b->kind), sf, s->key, b->kind) < 0) return -1;
        }
    }
    qsort(out, (size_t)nb, sizeof *out, cmp_bind);
    return nb;
}

// 对外暴露同一张合并表：描述符写入要按**同一个顺序**填动态偏移数组（vkCmdBindDescriptorSets 按绑定号升序
// 消费它们），所以两边不能各算一次——算两次迟早会漂。
// Expose the same merged table: descriptor writing must fill its dynamic offset array in **the same order**
// (vkCmdBindDescriptorSets consumes them in ascending binding order), so the two sides must not compute it
// separately -- two computations will drift apart sooner or later.
int vk_pipe_binds(const vk_shader *const *sh, int n, vk_pipe_bind *out, int cap) {
    return merge_binds(sh, n, out, cap);
}

// 布局的键：把合并后的表写成稳定的字符串（按 binding 升序，所以同一条表必然同一个键）
// Layout key: write the merged table into a stable string (sorted by binding, so one table always yields one key)
static void key_of_binds(const bind_t *b, int nb, char *out, size_t n) {
    size_t o = 0;
    for (int i = 0; i < nb && o + 40 < n; i++)
        o += (size_t)snprintf(out + o, n - o, "%u/%u:%u:%d:%x;", b[i].set, b[i].binding, b[i].count, (int)b[i].type, (unsigned)b[i].stage);
    out[o] = 0;
}

// 布局缓存：开放寻址、线性探测，存的是键字符串（不超过 1 KB）与两个句柄
// Layout cache: open addressing with linear probing, holding the key string (under 1 KB) and the two handles
#define LAY_TAB 1024
typedef struct { char *key; VkDescriptorSetLayout dsl[2]; VkPipelineLayout lay; } lay_e;
static lay_e g_lay[LAY_TAB];

static uint32_t str_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

// 按一张绑定表建描述符集布局与管线布局。variant 见 vk_pipe.h（0 = 正常；诊断变体不进缓存）。
// Build a descriptor set layout and pipeline layout from one binding table. See vk_pipe.h for `variant`
// (0 = normal; the diagnostic variants are never cached).
static int build_layout(const bind_t *b, int nb, int variant, VkDescriptorSetLayout out_dsl[2], VkPipelineLayout *out_lay, const char *key) {
    // **两个 set 总是都建**（空的也建）：管线布局要求 set 号连续，而"片元在 set 1、顶点那条 set 0 是空的"
    // 这种组合很常见（比如只有片元的程序），留一个空布局反而省掉写入侧一堆条件分支。
    // **Both sets are always created** (empty ones included): a pipeline layout wants consecutive set numbers, and
    // combinations like "the fragment side is on set 1 while set 0 is empty" are common (a fragment-only program,
    // say), so keeping an empty layout saves the write side a pile of conditionals.
    VkDescriptorSetLayoutBinding lb[MAX_B * 2];
    for (uint32_t set = 0; set < 2; set++) {
        int n = 0;
        for (int i = 0; i < nb && n < MAX_B * 2; i++) {
            if (b[i].set != set) continue;
            lb[n].binding = b[i].binding;
            lb[n].descriptorType = b[i].type;
            lb[n].descriptorCount = b[i].count ? b[i].count : 1;   // 元素个数取自 SPIR-V，见 vk_spv.h | the element count comes from the SPIR-V, see vk_spv.h
            lb[n].stageFlags = b[i].stage;
            lb[n].pImmutableSamplers = NULL;
            n++;
        }
        if (variant >= 2 && set == 0) {   // 把表里缺的 binding 号补进来（只在 set 0，够分辨不匹配了）| add the binding numbers the table lacks (set 0 only, enough to tell a mismatch apart)
            VkDescriptorType t = variant == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                : variant == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
                                               : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            for (uint32_t num = 0; num < 16 && n < MAX_B * 2; num++) {
                int have = 0;
                for (int i = 0; i < nb; i++) if (b[i].set == 0 && b[i].binding == num) { have = 1; break; }
                if (!have) {
                    lb[n].binding = num; lb[n].descriptorType = t; lb[n].descriptorCount = 1;
                    lb[n].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; lb[n].pImmutableSamplers = NULL; n++;
                }
            }
        }
        VkDescriptorSetLayoutCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                .bindingCount = (uint32_t)n, .pBindings = lb };
        VkResult r = vkCreateDescriptorSetLayout(vk_dev(), &dci, NULL, &out_dsl[set]);
        if (r != VK_SUCCESS) {
            LOG("【红】建描述符集布局失败：VkResult %d（set %u、%d 条绑定，键 %s）", (int)r, set, n, key);
            if (out_dsl[0]) { vkDestroyDescriptorSetLayout(vk_dev(), out_dsl[0], NULL); out_dsl[0] = VK_NULL_HANDLE; }
            return 0;
        }
    }
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 256 };
    VkPipelineLayoutCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 2, .pSetLayouts = out_dsl,
                                       .pushConstantRangeCount = variant == 1 ? 1u : 0u,
                                       .pPushConstantRanges = variant == 1 ? &pcr : NULL };
    VkResult r = vkCreatePipelineLayout(vk_dev(), &pci, NULL, out_lay);
    if (r != VK_SUCCESS) {
        LOG("【红】建管线布局失败：VkResult %d（键 %s）", (int)r, key);
        for (int k = 0; k < 2; k++) if (out_dsl[k]) { vkDestroyDescriptorSetLayout(vk_dev(), out_dsl[k], NULL); out_dsl[k] = VK_NULL_HANDLE; }
        return 0;
    }
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
            VkPipelineLayout lay = VK_NULL_HANDLE;
            if (!build_layout(b, nb, 0, e->dsl, &lay, key)) { g_n_fail++; return NULL; }
            e->key = strdup(key);
            e->lay = lay;
            g_n_dsl += 2; g_n_lay++;   // 两个 set 布局（空的那个也算一个）| two set layouts (the empty one included)
            return e;
        }
        if (!strcmp(e->key, key)) return e;
    }
    g_n_fail++;
    LOG("【红】布局缓存满了（%d 项）", LAY_TAB);
    return NULL;
}

VkDescriptorSetLayout vk_pipe_set_layout(const vk_shader *const *sh, int n) {
    return vk_pipe_set_layout_set(sh, n, 0);
}
VkDescriptorSetLayout vk_pipe_set_layout_set(const vk_shader *const *sh, int n, uint32_t set) {
    if (set > 1) return VK_NULL_HANDLE;
    lay_e *e = lay_get_or_make(sh, n);
    return e ? e->dsl[set] : VK_NULL_HANDLE;
}
VkPipelineLayout vk_pipe_layout(const vk_shader *const *sh, int n) {
    lay_e *e = lay_get_or_make(sh, n);
    return e ? e->lay : VK_NULL_HANDLE;
}

// ===========================================================================
// N4-l：`DM_LAYOUT_DUMP=1`（**缺省关**）—— 把"这一对着色器"建出来的**两个描述符集布局**逐条摊平，
// 并把三个句柄（set0 布局 / set1 布局 / 管线布局）一起打出来。
//
// 为什么要它：N4-k 的结论被收在"我们这条描述符路"上，而那条路上游有三处会**静默漂**：
//   ① 布局缓存按"合并后的绑定表"作键（`key_of_binds`），所以"同样的键"必然同一个句柄 —— 这一条要能当场核；
//   ② 布局里每条绑定的 **类型 / count / stage 位** 决定 `desc_write` 该怎么写；写歪了没有校验层就不说话；
//   ③ **动态描述符的个数**决定 `vkCmdBindDescriptorSets` 必须收到几个动态偏移（少一个就是非法绑定）。
// 所以这一行同时给出：逐条绑定（set/binding/类型/count/阶段位）+ 动态描述符总数 + 三个句柄。
// ===========================================================================
static int layout_dump_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_LAYOUT_DUMP");
        v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        if (v) LOG("【查】DM_LAYOUT_DUMP=1：把这一对着色器建出来的**两个描述符集布局**逐条摊平"
                   "（set/绑定号/类型/个数/阶段位）+ **动态描述符总数** + 三个句柄 —— 只读，不改行为");
    }
    return v;
}

static const char *dt_name(VkDescriptorType t) {
    switch (t) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return "UBO_dyn";
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return "SSBO_dyn";
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return "image+samp";
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return "storage_image";
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return "texel_buffer";
        case VK_DESCRIPTOR_TYPE_SAMPLER: return "sampler";
        default: return "其他";
    }
}

// 布局里**动态描述符的总数** = `vkCmdBindDescriptorSets` 必须收到的 dynamicOffsetCount（我们总是绑 set 0+1）。
int vk_pipe_layout_dyn_count(const vk_shader *const *sh, int n) {
    bind_t b[MAX_B];
    int nb = merge_binds(sh, n, b, MAX_B);
    if (nb < 0) return -1;
    int k = 0;
    for (int i = 0; i < nb; i++)
        if (b[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
            b[i].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
            k += b[i].count ? (int)b[i].count : 1;
    return k;
}

void vk_pipe_layout_dump(const vk_shader *const *sh, int n, const char *where) {
    if (!layout_dump_on()) return;
    bind_t b[MAX_B];
    int nb = merge_binds(sh, n, b, MAX_B);
    if (nb < 0) { LOG("【黄】DM_LAYOUT_DUMP：合并绑定表失败（%s）", where); return; }
    char key[1024];
    key_of_binds(b, nb, key, sizeof key);
    // 同一个键只打一次（键就是布局的身份）
    static char *seen[24];
    static int nseen;
    for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], key)) return;
    if (nseen < 24) seen[nseen++] = strdup(key);
    lay_e *e = lay_get_or_make(sh, n);
    char body[900]; int o = 0; body[0] = 0;
    for (int i = 0; i < nb && o < (int)sizeof body - 48; i++)
        o += snprintf(body + o, sizeof body - (size_t)o, "%sset%u/%u:%s×%u:阶段%#x",
                      i ? "；" : "", b[i].set, b[i].binding, dt_name(b[i].type), b[i].count, (unsigned)b[i].stage);
    int nd = 0, nd0 = 0, nd1 = 0;
    for (int i = 0; i < nb; i++) {
        if (b[i].type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
            b[i].type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) continue;
        int c = b[i].count ? (int)b[i].count : 1;
        nd += c;
        if (b[i].set == 0) nd0 += c; else nd1 += c;
    }
    LOG("【查】DM_LAYOUT_DUMP（%s）：顶点 %s / 片元 %s；**动态描述符 %d 个**（set0 %d / set1 %d）；"
        "句柄 set0 %#llx / set1 %#llx / 管线 %#llx；绑定：%s",
        where, sh[0] ? sh[0]->key : "无", (n > 1 && sh[1]) ? sh[1]->key : "无",
        nd, nd0, nd1,
        (unsigned long long)(uintptr_t)(e ? e->dsl[0] : VK_NULL_HANDLE),
        (unsigned long long)(uintptr_t)(e ? e->dsl[1] : VK_NULL_HANDLE),
        (unsigned long long)(uintptr_t)(e ? e->lay : VK_NULL_HANDLE),
        body[0] ? body : "（空表）");
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
    // 走那个图形与计算共用的管线缓存（见 vk_device.c 里那段说明）：建管线要外部串行化，所以套一对锁
    // Through the pipeline cache shared by graphics and compute (see the note in vk_device.c); pipeline creation needs
    // external serialisation, hence the lock pair
    vk_dev_pcache_lock();
    // M3-2w：建管线是"驱动内部编译"的经典长调用（驱动常在里面 usleep 轮询编译线程），而且它**跑在客体线程上** ——
    // 卡在界面切换（换一批渲染状态就要新管线）时，这里必须能被账本点名。
    // M3-2w: pipeline creation is the classic long "the driver compiles inside" call (drivers often poll a compile thread
    // with usleep) and it **runs on guest threads** -- when a scene change (which needs new pipelines for a new set of
    // render states) hangs, the ledger has to be able to name this. 
    dm_waitled_enter("vk：建计算管线（驱动内部编译）");
    VkResult r = vkCreateComputePipelines(vk_dev(), vk_dev_pcache(), 1, &ci, NULL, out);
    dm_waitled_leave();
    vk_dev_pcache_unlock();
    return r;
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
// 按 NVN 图元号计数：**那四种没有对应物的图元到底会不会出现，交给真机回答**（出现了再决定要不要做转化，
// 而不是先写一堆展开代码）。回放层每次绘制都会经过这里，所以这就是全量统计。
// Counted by NVN primitive number: **whether those four without counterparts ever appear is answered by the
// device** (if they do, decide then whether to write the conversion, rather than writing a pile of expansion code
// first). Every draw passes through here, so this is the full count.
static _Atomic uint64_t g_prim_n[16];
void vk_pipe_prim_counts(uint64_t *out, int n) {
    for (int i = 0; i < n && i < 16; i++) out[i] = atomic_load(&g_prim_n[i]);
}

VkPrimitiveTopology vk_topo_of(int p) {
    if (p >= 0 && p < 16) atomic_fetch_add(&g_prim_n[p], 1);
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
// 分量类别（0 = float 系、1 = uint 系、2 = sint 系）：用来与着色器声明的输入类型对照，见下面在建管线处的核对
// Component class (0 = float-like, 1 = uint-like, 2 = sint-like): used when cross-checking against the input type a
// shader declares, see the check where the pipeline is built)
static int attr_fmt_class(uint32_t f) {
    if (f >= 0x01 && f <= 0x30) {
        uint32_t k = (f - 1) % 12;
        if (k == 2 || k == 7 || k == 10) return 1;
        if (k == 3 || k == 8 || k == 11) return 2;
        return 0;
    }
    return 0;   // 三个特例都是归一化浮点 | the three specials are all normalized floats
}
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

#define DUMMY_BIND VK_PIPE_DUMMY_BIND   // 常量回退属性挂在这一号绑定上（每实例一个元素）| the constant-fallback attributes ride on this binding (one element per instance)

// 非实心填充要开 fillModeNonSolid 特性（设备侧没开就建不出管线），所以退回实心并只报一次
// Non-solid fill needs the fillModeNonSolid feature (not enabled on the device, so creation would fail), so fall
// back to solid and report it once
static void poly_warn_once(int mode) {
    static int said;
    if (!said++) LOG("【黄】多边形模式 %d 需要 fillModeNonSolid 特性：按实心画", mode);
}

#define GPSO_TAB 192
// N4-i：条目里多存一份"这个条目当初是按**哪一对着色器**建的"——**只用于只读诊断**（见下面的命中分支）。
typedef struct { vk_gfx_state st; VkPipeline p; int used; char kvs[VK_SHADER_KEY_LEN + 1], kps[VK_SHADER_KEY_LEN + 1]; } gpso_e;
static gpso_e g_gpso[GPSO_TAB];
static uint32_t bytes_hash(const void *p, size_t n) {
    const uint8_t *b = p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

// N4-i：**只读**诊断（`DM_PIPEKEY_PROBE=1`，缺省关）——"管线缓存键里没有着色器"这件事有多少次真的发生了。
// 背景（读代码得到，不是推断）：`vk_gfx_state` 头两个字段是 `vs_key` / `ps_key`，而回放路径里唯一的填表处
// `gfx_state_fill()` 是 `memset(out, 0, ...)` 之后**只填状态字段**，**从不填这两个键**（全库只有
// `vk_pipe_probe.c` 填过）⇒ 下面那个 `memcmp(&e->st, st, sizeof *st)` 实际上**不区分子着色器对**：
// 同一批状态字节上，谁先建管线谁赢，后来那些"状态一样、着色器不同"的绘制会拿到**别人那一对的管线**。
// 命中时把"条目当初的着色器对"与"这一次要的着色器对"比一下 ⇒ 这个计数就是那件事的发生次数。
static _Atomic uint64_t g_n_keymiss;
static int pipekey_probe_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_PIPEKEY_PROBE");
        v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        if (v) LOG("【查】DM_PIPEKEY_PROBE=1：**只读**——统计「管线缓存命中、但条目当初是按另一对着色器建的」有多少次"
                   "（缓存键 vk_gfx_state 里 vs_key/ps_key 在回放路径上从没被填过 ⇒ 键里不含着色器）");
    }
    return v;
}
uint64_t vk_pipe_keymiss(void) { return atomic_load(&g_n_keymiss); }

// DM_NO_DEPTH_TEST：把深度测试**一律关掉**（诊断用）。
// 为什么值得一试：真机上 34 万次绘制都下发了、目标却读出来全零 —— 而"每个片元都被深度测试挡掉"是最经典的成因之一
// （比较函数、深度范围、附件布局任一处不对都会这样）。关掉它，画面若立刻出来，那就是深度这一路。
// DM_NO_DEPTH_TEST forces depth testing **off** everywhere (a diagnostic). Why it is worth trying: on the device 348k
// draws go out yet the targets read all zero, and "every fragment rejected by the depth test" is one of the classic
// causes (a wrong compare op, depth range or attachment layout all do it). Turn it off and, if the picture appears at
// once, depth is the path to look at.
static int nodepth_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NO_DEPTH_TEST");
        v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        if (v) LOG("【查】DM_NO_DEPTH_TEST：深度测试一律关掉（诊断）");
    }
    return v;
}

// DM_RASTER_BRUTE=1：把"可能把全部片元吃掉"的**光栅化侧**状态一起放开（诊断，缺省关；N4-e）。
// 背景：N4-e 已经证明**附件/视图/布局/存储归属成立**（`:end` 通道探针的已知色落到了读回的那张图上），
// 而**不带 `:end` 的探针**说明"通道从已知色开始、游戏绘制往上画"之后整片仍是已知色
// ⇒ **游戏那些绘制一个像素都没落下**。剩下能吃光片元的只有这几处：深度/模板测试、面剔除、
// 颜色写掩码与混合、以及视口/裁剪（视口/裁剪那一半在 nvn_vk.c 里按同一个开关铺满附件）。
// 判读：打开它之后目标上出现内容 ⇒ 就能把范围收到这几处，再逐个单独放开来定位；仍是全零 ⇒
// 光栅化侧全被排除，去查顶点/着色器/几何（顶点流、属性、索引、着色器输出）。
// DM_RASTER_BRUTE=1 opens up every **raster-side** state that could eat all fragments (a diagnostic,
// off by default; N4-e). N4-e has already proven the attachment/view/layout/storage chain works, and the
// non-`:end` probe says the game's draws leave not one pixel, so only these few places can be eating them:
// depth/stencil test, face culling, colour write mask and blending, plus viewport/scissor (that half lives
// in nvn_vk.c under the same switch). Content appearing then narrows the search to those; all zero still
// means the raster side is cleared and the vertex/shader/geometry side is next.
static int brute_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_RASTER_BRUTE");
        v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        if (v) LOG("【查】DM_RASTER_BRUTE=1：深度/模板测试关、面剔除关、混合关、写掩码全开"
                   "（视口/裁剪在 nvn_vk.c 里铺满附件）——诊断用");
    }
    return v;
}

// 给 nvn_vk.c 用（那半边要把动态视口/裁剪铺满附件，见那边的调用点）
int vk_pipe_brute(void) { return brute_on(); }

VkPipeline vk_pipe_graphics(const vk_shader *vs, const vk_shader *ps, const vk_gfx_state *st) {
    if (!vs || !st || vs->mod == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    uint32_t h = bytes_hash(st, sizeof *st) & (GPSO_TAB - 1);
    for (uint32_t k = 0; k < GPSO_TAB; k++) {
        gpso_e *e = &g_gpso[(h + k) & (GPSO_TAB - 1)];
        if (!e->used) break;
        if (!memcmp(&e->st, st, sizeof *st)) {
            // N4-i：命中。**只读**地看一眼"这个条目当初是按哪一对着色器建的"（缺省关时一行都不走）
            if (pipekey_probe_on() && (strcmp(e->kvs, vs->key) || (ps && strcmp(e->kps, ps->key)))) {
                uint64_t n = atomic_fetch_add(&g_n_keymiss, 1) + 1;
                if (n <= 8 || (n & 1023) == 0)
                    LOG("【查】DM_PIPEKEY_PROBE：**管线缓存命中、但着色器不是这一对**（第 %llu 次）—— 条目当初是"
                        "顶点 %s / 片元 %s，这一次要的是 顶点 %s / 片元 %s（状态：目标 %u 个、拓扑 %d、写掩码[0] %#x）"
                        "⇒ 这次绘制拿到的是**别人那一对的管线**（校验层不会报：管线与通道是兼容的，只是着色器不是它要的）",
                        (unsigned long long)n, e->kvs[0] ? e->kvs : "（空）", e->kps[0] ? e->kps : "（空）",
                        vs->key, ps ? ps->key : "无", st->color_count, (int)st->topo, st->write_mask[0]);
            }
            return e->p;
        }
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
    VkVertexInputBindingDivisorDescriptionEXT vdiv[16];
    uint32_t nb = 0, na = 0, dummies = 0, ndiv = 0;
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
        int mapped = st->attr[loc].valid && attr_fmt_vk(st->attr[loc].nvn_fmt, &f);
        // 兜底核对（第 9 步的"待实现"）：这条属性的**分量类型**要与着色器声明的输入类型同类
        //（float / uint / sint）。不成立就报一次黄——若"location == NVN 属性号"这条假设错了，就会在这里现形，
        // 而不是等画面错了再回头找。
        // Fallback cross-check (the "to be implemented" of step 9): this attribute's **component class**
        // (float / uint / sint) must agree with the type the shader declares for that input. A mismatch warns once --
        // if the "location == NVN attribute number" assumption is wrong it shows up here rather than as a wrong
        // picture later.
        if (mapped) {
            int want = vs->in[i].ctype == VK_SPV_I32 ? 2 : vs->in[i].ctype == VK_SPV_U32 ? 1 : 0;
            int got = attr_fmt_class(st->attr[loc].nvn_fmt);
            if (got != want) {
                // 改前这里只打第一次（`static int saidc`），而**判据本身写反过一次**，所以"只打一条"正好把
                // 假告警的样子留了下来、把真告警的数量藏了起来。现在两边都把类别写出来，并给累计数。
                // It used to print only the first occurrence (`static int saidc`), and since **the test itself was once
                // inverted**, printing one line preserved the look of a false warning while hiding how many real ones
                // there are. Both classes are named now, and the running count comes with it.
                uint64_t n = atomic_fetch_add(&g_attr_class_bad, 1) + 1;
                if (n <= 8 || (n & 1023) == 0)
                    LOG("【黄】顶点属性 %u（%s 位置 %u）的分量类型与着色器声明不符：格式 %#x 属%s类 vs 着色器声明 %s（第 %llu 例；若这条判据错了，顶点数据接的槽就不对）",
                        loc, vs->key, loc, st->attr[loc].nvn_fmt,
                        got == 2 ? " sint" : got == 1 ? " uint" : " float",
                        vs->in[i].ctype == VK_SPV_I32 ? "sint" : vs->in[i].ctype == VK_SPV_U32 ? "uint" : "float",
                        (unsigned long long)n);
            }
        }
        // NVN 的"常量流"（步长 0）：有 divisor 可为 0 的扩展就用它表达（每个实例一个元素、不随顶点推进），
        // 这才是它的正确语义 | NVN's "constant stream" (stride 0): expressed with the zero-divisor extension when
        // available (one element per instance, not advancing per vertex), which is its correct meaning
        int const_stream = st->stream[st_].stride == 0;
        if (mapped && const_stream && !vk_have_attr_divisor_zero()) mapped = 0;
        if (!mapped) {   // 回退：常量属性（全零的小缓冲，每个实例一个元素）| fallback: a constant attribute (a small zeroed buffer, one element per instance)
            vatt[na].location = loc; vatt[na].format = fallback_fmt(vs->in[i].ctype, vs->in[i].comps);
            vatt[na].offset = 0; vatt[na].binding = DUMMY_BIND;
            na++; dummies++;
            if (st->attr[loc].valid && st->attr[loc].nvn_fmt && st->stream[st_].stride) {
                static int said2;
                if (!said2++) LOG("【黄】顶点属性格式 %#x 未映射：用常量回退", st->attr[loc].nvn_fmt);
            }
            continue;
        }
        vatt[na].location = loc; vatt[na].format = f;
        // 绑定号 = **顶点流号本身**（0..15）。这里原先写的是 `16 + st_`，而另一侧的绘制循环把缓冲绑在 `s2`（也就是流号
        // 本身）—— 两边差 16，于是**管线真正要读的那些绑定从来没被绑过**：没有校验层的 Vulkan 会去读不确定内存，
        // 几何整片塌掉，表现正是"34 万次绘制、一个像素不落、且零报错"。绑定号只有 32 个（0..31），16 条流占满高位，
        // 所以"低位给流、高位留给常量回退"（`VK_PIPE_DUMMY_BIND = 31`）才是两边能同时对上的一套；顺带原来的
        // `16 + 15` 与 31 撞车这件事也消失了。
        // The binding number **is the vertex stream number** (0..15). This used to be `16 + st_` while the draw loop on the
        // other side bound buffers at `s2` (the stream number itself) -- off by 16, so the bindings the pipeline actually
        // reads were **never bound**: with no validation layers Vulkan reads undefined memory, the geometry collapses, and
        // that is exactly "348k draws, not one pixel landing, zero errors". Only 32 bindings exist (0..31) and 16 streams
        // would fill the high half, so streams on the low half with the constant fallback high (`VK_PIPE_DUMMY_BIND = 31`)
        // is the only layout both sides can agree on -- which also removes the old `16 + 15` collision with 31.
        vatt[na].offset = st->attr[loc].offset; vatt[na].binding = st_;
        na++;
        if (!stream_used[st_]) {
            stream_used[st_] = 1;
            vbind[nb].binding = st_;
            vbind[nb].stride = const_stream ? 16 : st->stream[st_].stride;
            vbind[nb].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            uint32_t div = st->stream[st_].divisor;
            if (const_stream) { vbind[nb].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE; vdiv[ndiv].binding = st_; vdiv[ndiv].divisor = 0; ndiv++; }
            else if (div) {
                vbind[nb].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
                if (vk_have_attr_divisor()) { vdiv[ndiv].binding = st_; vdiv[ndiv].divisor = div; ndiv++; }
                else {
                    static int said3;
                    if (div > 1 && !said3++) LOG("【黄】顶点流 %u 的 divisor=%u：设备不支持 VK_EXT_vertex_attribute_divisor，按 1 处理", st_, div);
                }
            }
            nb++;
        }
    }
    if (dummies) {
        vbind[nb].binding = DUMMY_BIND; vbind[nb].stride = 16;
        vbind[nb].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        nb++;
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT vds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
        .vertexBindingDivisorCount = ndiv, .pVertexBindingDivisors = vdiv };

    void *vi_divisor = ndiv ? (void *)&vds : NULL;   // divisor 状态只在真用到时挂上去（挂了就必须与绑定一一对应）| the divisor state is chained only when it is really used (once chained it must match the bindings)
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = vi_divisor,
        .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = nb ? vbind : NULL,
        .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = na ? vatt : NULL };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = st->topo, .primitiveRestartEnable = VK_FALSE };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        // 没有片元阶段时规范要求丢弃光栅化（探针用顶点着色器单独验管线时走这条）| the spec requires rassterizer discard when there is no fragment stage (the probe exercises a pipeline with the vertex shader alone)
        .rasterizerDiscardEnable = nsh == 1 ? VK_TRUE : VK_FALSE,
        .polygonMode = (st->poly == VK_POLYGON_MODE_FILL || vk_have_fill_non_solid())
                     ? st->poly : (poly_warn_once(st->poly), VK_POLYGON_MODE_FILL),
        .cullMode = brute_on() ? VK_CULL_MODE_NONE : st->cull, .frontFace = st->front,
        .depthBiasEnable = st->bias_en ? VK_TRUE : VK_FALSE,
        .depthBiasConstantFactor = st->bias_factor, .depthBiasClamp = st->bias_clamp, .depthBiasSlopeFactor = st->bias_units,
        .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = st->samples ? st->samples : VK_SAMPLE_COUNT_1_BIT,
        .alphaToCoverageEnable = st->a2c ? VK_TRUE : VK_FALSE };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ((nodepth_on() || brute_on()) ? VK_FALSE : (st->depth_test ? VK_TRUE : VK_FALSE)),
        .depthWriteEnable = st->depth_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (nodepth_on() || brute_on() || !st->depth_test) ? VK_COMPARE_OP_ALWAYS : st->depth_op,
        .stencilTestEnable = (brute_on() ? VK_FALSE : (st->stencil_en ? VK_TRUE : VK_FALSE)) };
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
        cb[i].blendEnable = (brute_on() ? VK_FALSE : (st->blend_en[i] ? VK_TRUE : VK_FALSE));
        cb[i].srcColorBlendFactor = st->src_rgb[i]; cb[i].dstColorBlendFactor = st->dst_rgb[i];
        cb[i].srcAlphaBlendFactor = st->src_a[i]; cb[i].dstAlphaBlendFactor = st->dst_a[i];
        cb[i].colorBlendOp = st->eq_rgb[i]; cb[i].alphaBlendOp = st->eq_a[i];
        cb[i].colorWriteMask = (VkColorComponentFlags)(brute_on() ? 0xf : st->write_mask[i]);
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
    // N4-k（TOOLS §H.3）：把**真正交给驱动的那份结构体**打出来（缺省关）。放在 create 之前 ⇒ 与"驱动收到了什么"逐字段对上。
    if (pipe_dump_on()) pipe_dump(&ci, &rs, &ms, &ds, &cbs, &prc, &ia, vs, ps, na, nb, ndiv);
    // 同计算管线：走共用的管线缓存（建管线要外部串行化，套一对锁）
    // Same as the compute path: through the shared pipeline cache (creation needs external serialisation, hence the lock)
    vk_dev_pcache_lock();
    dm_waitled_enter("vk：建图形管线（驱动内部编译）");   // M3-2w：同计算管线那条说明 | M3-2w: same note as the compute path
    VkResult r = vkCreateGraphicsPipelines(vk_dev(), vk_dev_pcache(), 1, &ci, NULL, &p);
    dm_waitled_leave();
    vk_dev_pcache_unlock();
    if (r != VK_SUCCESS) {
        g_n_fail++;
        LOG("【红】建图形管线失败：VkResult %d（顶点 %s 片元 %s，属性 %u 条、流 %u 条、目标 %u 个、拓扑 %d）",
            (int)r, vs->key, ps ? ps->key : "无", na, nb, st->color_count, (int)st->topo);
        return VK_NULL_HANDLE;
    }
    // 存进缓存（满了就丢掉最老的那个槽，只求不无限增长）| store in the cache (when full, drop the oldest slot rather than grow without bound)
    uint32_t slot = 0;
    for (uint32_t k = 0; k < GPSO_TAB; k++) { slot = (h + k) & (GPSO_TAB - 1); if (!g_gpso[slot].used) break; }
    if (g_gpso[slot].used) {   // 满了要淘汰：报一次，别静默（频繁出现说明状态组合太散，值得再看）| eviction when full: report once, never silently (frequent eviction means the state combinations are too scattered)
        static int said;
        if (!said++) LOG("【黄】图形管线缓存满（%d 项）：开始淘汰最旧的", GPSO_TAB);
        vkDestroyPipeline(vk_dev(), g_gpso[slot].p, NULL);
    }
    g_gpso[slot].st = *st; g_gpso[slot].p = p; g_gpso[slot].used = 1;
    // N4-i：把"这一条是按哪一对着色器建的"记下来（只读诊断用；不影响任何行为）
    memcpy(g_gpso[slot].kvs, vs->key, VK_SHADER_KEY_LEN + 1);
    g_gpso[slot].kps[0] = 0;
    if (ps) memcpy(g_gpso[slot].kps, ps->key, VK_SHADER_KEY_LEN + 1);
    g_n_gpso++;
    return p;
}

// 常量回退属性要绑的哑缓冲（全零）。绘制时把它绑到 VK_PIPE_DUMMY_BIND 上——不绑的话，用了回退属性的
// 那次绘制是无效的：管线里已经声明了这条绑定，但没有任何缓冲挂上去。
// The dummy (all-zero) buffer the constant-fallback attributes bind to. A draw binds it at VK_PIPE_DUMMY_BIND --
// without that, a draw using a fallback attribute is invalid: the pipeline declares the binding but nothing is
// attached to it.
static VkBuffer g_dummy_vbuf;
VkBuffer vk_pipe_dummy_buffer(void) {
    if (g_dummy_vbuf) return g_dummy_vbuf;
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = DUMMY_VBUF_SIZE,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer b = VK_NULL_HANDLE;
    if (vkCreateBuffer(vk_dev(), &bci, NULL, &b) != VK_SUCCESS) { LOG("【红】建哑顶点缓冲失败"); return VK_NULL_HANDLE; }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk_dev(), b, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = vk_memory_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mr.memoryTypeBits) };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(vk_dev(), &mai, NULL, &mem) != VK_SUCCESS ||
        vkBindBufferMemory(vk_dev(), b, mem, 0) != VK_SUCCESS) {
        LOG("【红】哑顶点缓冲的内存分配/绑定失败");
        vkDestroyBuffer(vk_dev(), b, NULL);
        return VK_NULL_HANDLE;
    }
    void *p = NULL;
    if (vkMapMemory(vk_dev(), mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) memset(p, 0, DUMMY_VBUF_SIZE);
    else LOG("【红】哑顶点缓冲映射失败（内容可能非零，回退属性会读到垃圾）");
    g_dummy_vbuf = b;
    return b;
}

// 空白图像 / 采样器：描述符写入的回退（游戏绑了空槽，或句柄还没解析出来）。
// Dummy image / sampler: the descriptor-writing fallback (the game bound an empty slot, or the handle cannot be
// resolved yet).
static VkImageView g_dummy_view;
static VkSampler g_dummy_samp;

VkImageView vk_pipe_dummy_view(void) {
    if (g_dummy_view) return g_dummy_view;
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 1, 1, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage im = VK_NULL_HANDLE;
    if (vkCreateImage(vk_dev(), &ici, NULL, &im) != VK_SUCCESS) { LOG("【红】建空白图像失败"); return VK_NULL_HANDLE; }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk_dev(), im, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = vk_memory_type(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mr.memoryTypeBits) };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(vk_dev(), &mai, NULL, &mem) != VK_SUCCESS || vkBindImageMemory(vk_dev(), im, mem, 0) != VK_SUCCESS) {
        LOG("【红】空白图像的内存分配 / 绑定失败");
        vkDestroyImage(vk_dev(), im, NULL);
        return VK_NULL_HANDLE;
    }
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(vk_dev(), &vci, NULL, &v) != VK_SUCCESS) { LOG("【红】建空白图像视图失败"); return VK_NULL_HANDLE; }
    g_dummy_view = v;
    return v;
}

// 空白 texel 视图：缓冲区贴图那条绑定的回退（句柄还没解析出来、或那张贴图建不出视图）。
// 描述符**必须写点什么**（不写的描述符内容不确定），而 NVN 在空槽上就是读 0，所以这里给 1 个全零元素。
// A dummy texel view: the fallback for a buffer-texture binding (the handle is not resolved yet, or that texture
// cannot produce a view). Something **must** be written to the descriptor (an unwritten one holds undefined
// contents), and NVN reads 0 on an empty slot, so this is one all-zero element.
static VkBufferView g_dummy_bv;
VkBufferView vk_pipe_dummy_bufview(void) {
    if (g_dummy_bv) return g_dummy_bv;
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4,
        .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer b = VK_NULL_HANDLE;
    if (vkCreateBuffer(vk_dev(), &bci, NULL, &b) != VK_SUCCESS) { LOG("【红】建空白 texel 缓冲失败"); return VK_NULL_HANDLE; }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk_dev(), b, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = vk_memory_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mr.memoryTypeBits) };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(vk_dev(), &mai, NULL, &mem) != VK_SUCCESS || vkBindBufferMemory(vk_dev(), b, mem, 0) != VK_SUCCESS) {
        LOG("【红】空白 texel 缓冲的内存分配 / 绑定失败");
        vkDestroyBuffer(vk_dev(), b, NULL);
        return VK_NULL_HANDLE;
    }
    void *p = NULL;
    if (vkMapMemory(vk_dev(), mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) memset(p, 0, 4);   // 全零元素；宿主可见且一致，映射一直留着 | an all-zero element; the memory is host-visible and coherent, so the mapping stays
    VkBufferViewCreateInfo bvi = { .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = b,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .offset = 0, .range = 4 };
    VkBufferView v = VK_NULL_HANDLE;
    if (vkCreateBufferView(vk_dev(), &bvi, NULL, &v) != VK_SUCCESS) { LOG("【红】建空白 texel 视图失败"); return VK_NULL_HANDLE; }
    g_dummy_bv = v;
    return v;
}

VkSampler vk_pipe_dummy_sampler(void) {
    if (g_dummy_samp) return g_dummy_samp;
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f };
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(vk_dev(), &sci, NULL, &s) != VK_SUCCESS) { LOG("【红】建空白采样器失败"); return VK_NULL_HANDLE; }
    g_dummy_samp = s;
    return s;
}

void vk_pipe_stats(uint64_t *set_layouts, uint64_t *layouts, uint64_t *cpso, uint64_t *gpso, uint64_t *fail) {
    if (set_layouts) *set_layouts = g_n_dsl;
    if (layouts) *layouts = g_n_lay;
    if (cpso) *cpso = g_n_cpso;
    if (gpso) *gpso = g_n_gpso;
    if (fail) *fail = g_n_fail;
}

// N4-h：见 vk_pipe.h —— 只是让"我们自己的三角形"那条诊断管线也在这一格上有个 +1（含义写在日志里）
// N4-h: see vk_pipe.h -- it only gives the diagnostic triangle's pipeline a +1 on this counter
void vk_pipe_note_external_gfx(void) { g_n_gpso++; }
