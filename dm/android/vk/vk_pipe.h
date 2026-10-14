// 安卓图形后端的管线层（M2-6d 的第 2 块）：描述符集布局、管线布局、以及管线本身。
// Pipeline layer of the Android graphics backend (the second piece of M2-6d): descriptor set layouts, pipeline
// layouts, and pipelines themselves.
//
// 三个事实决定了这一层的形状（前两个已在本地核过，第三个是真机行为）：
//   ① **绑定号就是 SPIR-V 的 binding**：`<键>.res.json` 里的 binding 是 SPIR-V 里的号（翻译器的产物，
//      采样器名字出现在 MSL 的 `spvDescriptorSet0` 里 → 全部落在 set 0）；
//   ② **没有数组绑定**：全库 44,156 条贴图/图像绑定的 `array` 字段都是 1，所以 descriptorCount 取 1；
//      入口点全库都叫 `main`（抽 150 份核过）；res.json 里没有入口点字段，这是唯一来源。
//   ③ **缓冲用动态偏移**：客体的 uniform / 存储缓冲数据就在零拷贝池里，每次绘制只有"哪一段"不同，
//      所以布局里声明成 dynamic，绘制时用 vkCmdBindDescriptorSets 的 dynamicOffsets 指过去——
//      与苹果侧的 setBuffer:offset: 同一个意思。
// Three facts shape this layer (the first two were checked locally, the third is device behaviour):
//   1. **A binding number is the SPIR-V binding**: the binding in <key>.res.json is the number in the SPIR-V
//      (a translator output; the sampler names show up in the MSL as `spvDescriptorSet0`, so all of them live in
//      set 0);
//   2. **There are no array bindings**: the `array` field is 1 for all 44,156 texture/image bindings in the
//      library, so descriptorCount is 1; the entry point is called `main` everywhere (150 files sampled); .res.json
//      carries no entry-point field, so this is the only source for it.
//   3. **Buffers use dynamic offsets**: the guest's uniform and storage buffer data already sits in zero-copy pools
//      and only "which range" differs from draw to draw, so the layout declares them dynamic and the draw passes
//      the offset through vkCmdBindDescriptorSets' dynamicOffsets -- the same meaning as setBuffer:offset: on Apple.
#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>
#include "vk_shader.h"

// 按一组着色器（顶点 + 片元，或单个计算）合并它们的绑定表，得到一个描述符集布局 / 管线布局，按键缓存。
// 同一条 binding 只出现一次、阶段取并集；类型冲突（同一号两种类型）报红一次并取先见到的那个。
// Merge the binding tables of a set of shaders (vertex + fragment, or a single compute) into one descriptor set
// layout / pipeline layout, cached by key. A binding appears once with the stages unioned; a type conflict (one
// number, two types) is reported once and the first seen wins.
VkDescriptorSetLayout vk_pipe_set_layout(const vk_shader *const *sh, int n);
VkPipelineLayout vk_pipe_layout(const vk_shader *const *sh, int n);

// 计算管线：先用它独立验证「反射 ↔ SPIR-V ↔ 驱动」这条链——计算管线不需要任何渲染状态，
// 所以建得成就是"描述符布局的解读是对的"。（图形管线要等影子状态补齐，见 vk_pipe.c 的文件头。）
// Compute pipeline: used first to verify the chain "reflection <-> SPIR-V <-> driver" on its own -- a compute
// pipeline needs no render state at all, so creating one proves the descriptor-layout interpretation is right.
// (Graphics pipelines wait for the shadow state to be completed; see the header of vk_pipe.c.)
VkPipeline vk_pipe_compute(const vk_shader *cs);

// 诊断变体（不进缓存，只为分辨建管线失败是哪一种不匹配）：
//   1 = 加一段 push constant（0..255，计算阶段）；2/3/4 = 把表里 0..15 中缺的 binding 号分别补成
//   uniform 缓冲 / 存储缓冲 / 组合图像采样器。
// Diagnostic variants (not cached; they exist only to tell which kind of mismatch makes pipeline creation fail):
//   1 adds a push-constant range (0..255, compute stage); 2/3/4 add every binding number in 0..15 missing from
//   the table as a uniform buffer / storage buffer / combined image sampler respectively.
VkPipeline vk_pipe_compute_variant(const vk_shader *cs, int variant);

// 计数：建了多少个描述符集布局 / 管线布局 / 计算管线 / 图形管线，以及失败次数
// Counters: descriptor set layouts, pipeline layouts, compute pipelines and graphics pipelines created, plus failures
void vk_pipe_stats(uint64_t *set_layouts, uint64_t *layouts, uint64_t *cpso, uint64_t *gpso, uint64_t *fail);

// ---------- 图形管线 ----------
// ---------- Graphics pipelines ----------
// 一次绘制需要的图形管线状态。由回放层从**影子状态**解释出来（位域对象 → Vulkan 枚举，照苹果侧同一套映射），
// 调用方先 memset 再逐字段填，所以可以按内容比较与缓存。拓扑也在里面：Vulkan 里它是管线状态，
// 而 NVN 把它放在绘制命令上，所以同一条着色器会因拓扑不同而有多条管线。
// The graphics pipeline state one draw needs. The replay layer interprets it from the **shadow state** (packed
// bit-field objects -> Vulkan enums, following the same mappings as the Apple side), and the caller memsets it
// first and fills it field by field so it can be compared and cached by content. The topology is in here too: in
// Vulkan it is pipeline state while NVN puts it on the draw command, so one shader pair can have several
// pipelines, one per topology.
typedef struct {
    char vs_key[VK_SHADER_KEY_LEN + 1], ps_key[VK_SHADER_KEY_LEN + 1];
    VkPrimitiveTopology topo;
    uint32_t color_count; VkFormat color[8];
    uint32_t write_mask[8];                    // 每目标：r=1 g=2 b=4 a=8 | per target: r=1 g=2 b=4 a=8
    uint8_t blend_en[8];
    VkBlendFactor src_rgb[8], dst_rgb[8], src_a[8], dst_a[8];
    VkBlendOp eq_rgb[8], eq_a[8];
    VkFormat depth_fmt, stencil_fmt;
    uint32_t samples, a2c;
    uint8_t depth_test, depth_write; VkCompareOp depth_op;
    uint8_t stencil_en; VkCompareOp stencil_func[2]; uint32_t stencil_vmask[2], stencil_wmask[2];
    VkStencilOp stencil_fail[2], stencil_zfail[2], stencil_zpass[2];
    VkCullModeFlags cull; VkFrontFace front; VkPolygonMode poly;
    uint8_t bias_en; float bias_factor, bias_units, bias_clamp;
    // 顶点属性：按 NVN 属性号索引（= 着色器的 Location，见第 6 步的说明）；valid=0 表示这条属性游戏没绑，
    // 管线里给它一个常量回退（否则着色器用到它就直接建不出管线）。
    // Vertex attributes, indexed by NVN attribute number (= the shader's Location, see step 6's note); valid=0
    // means the game bound no such attribute and the pipeline supplies a constant fallback for it (without one,
    // creation fails outright the moment the shader uses it).
    struct { uint8_t valid; uint32_t nvn_fmt, offset, stream; } attr[16];
    struct { uint32_t stride, divisor; } stream[16];
} vk_gfx_state;

// 按状态与一对着色器建（或取缓存的）图形管线；失败返回 VK_NULL_HANDLE 并已报红计数。
// 顶点输入以**着色器声明的输入**为准（每一个都要有属性），格式/偏移/流来自影子属性表。
// Build (or fetch from cache) a graphics pipeline for a state and a shader pair; returns VK_NULL_HANDLE on
// failure, already logged and counted. The vertex input follows the **inputs the shader declares** (each needs an
// attribute) while format/offset/stream come from the shadow attribute table.
VkPipeline vk_pipe_graphics(const vk_shader *vs, const vk_shader *ps, const vk_gfx_state *st);

// NVN 图元 → VkPrimitiveTopology（没有对应物的那几个报一次黄并退化，见 vk_pipe.c）
// NVN primitive -> VkPrimitiveTopology (the few without a counterpart warn once and degrade; see vk_pipe.c)
VkPrimitiveTopology vk_topo_of(int nvn_prim);
