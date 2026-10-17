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

// N4-l：布局里**动态描述符的总数** —— 也就是 `vkCmdBindDescriptorSets` 必须收到的 `dynamicOffsetCount`
// （我们总是绑 set 0 + set 1，所以是两个 set 的和）。少一个就是一次非法绑定（VUID-…-dynamicOffsetCount-00359）。
// 返回 -1 = 合并绑定表失败（与 `vk_pipe_layout` 同时为 NULL）。
int vk_pipe_layout_dyn_count(const vk_shader *const *sh, int n);

// N4-l：`DM_LAYOUT_DUMP=1`（缺省关、只读）——把这个键的**两个描述符集布局逐条摊平**（set/绑定号/类型/个数/
// 阶段位）+ 动态描述符总数 + 三个句柄（set0 / set1 / 管线）。同一个键只打一次。
void vk_pipe_layout_dump(const vk_shader *const *sh, int n, const char *where);

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

// N4-h：回放层**以外**的模块（诊断用的"我们自己的三角形"，见 vk_tri_game.c）自己建了一条图形管线时调一次。
// 为什么要有它：心跳那一格的"图形管线 N 条"原本只统计游戏路径建的，混进去会让 N0~N4-g 的读数不可比；
// 但"我们这条管线真的建成了"这条自证又需要一个**既有判据上的 +1**。两个要求分开满足：计数照加，
// 含义写在日志与文档里（判据行会同时给出"含 N4-h 自绘 1 条"这层信息）。
// N4-h: called once when a module **outside** the replay layer (the diagnostic triangle in vk_tri_game.c)
// creates a graphics pipeline of its own. The heartbeat's "graphics pipelines" counter used to mean "built by
// the game path" only, and mixing that in would make N0..N4-g's readings incomparable -- but the self-proof
// that our pipeline really got built does want a +1 on an existing criterion, so the counter is bumped and the
// meaning is stated in the log and the docs.
void vk_pipe_note_external_gfx(void);

// N4-i（**只读**诊断）：图形管线缓存里"**命中、但条目当初是按另一对着色器建的**"发生过多少次。
// 只在 `DM_PIPEKEY_PROBE=1` 时增长（缺省关 ⇒ 恒 0 ⇒ 心跳那一行与上一轮逐字节相同，第 ㉓ 条）。
// 背景见 vk_pipe.c 里 `pipekey_probe_on()` 上方那段：`vk_gfx_state` 的 `vs_key`/`ps_key` 在回放路径上
// **从没被填过**，所以那个按整块状态做的 `memcmp` 实际上不区分子着色器。
uint64_t vk_pipe_keymiss(void);

// DM_RASTER_BRUTE（诊断，缺省关）：光栅化侧那几个开关的总闸。nvn_vk.c 用它决定"要不要在绘制前把
// 动态视口/裁剪铺满附件"（另一半在 vk_pipe.c 里打开深度/模板/剔除/混合/写掩码）。
// DM_RASTER_BRUTE (a diagnostic, off by default): the master switch for the raster-side ones. nvn_vk.c uses
// it to decide whether to override the dynamic viewport/scissor across the whole attachment before a draw
// (the other half opens depth/stencil/cull/blend/write-mask in vk_pipe.c).
int vk_pipe_brute(void);

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

// 常量回退属性所在的绑定号：绘制时要把 vk_pipe_dummy_buffer() 绑到它上面
// The binding the constant-fallback attributes live on: a draw must bind vk_pipe_dummy_buffer() to it
#define VK_PIPE_DUMMY_BIND 31
VkBuffer vk_pipe_dummy_buffer(void);

// 空白图像 / 采样器：描述符写入时给"游戏没绑、或还没解析出来"的贴图绑定用。写空描述符是必须的——
// 未写的描述符内容是不确定的，驱动可能据此做任何事；而"写什么"这件事上，游戏绑了空槽时 NVN 自己也读 0。
// Dummy image / sampler: used when writing descriptors for texture bindings the game left unbound or that cannot
// be resolved yet. Writing something is mandatory -- an unwritten descriptor holds undefined contents and the
// driver may do anything with them -- and as for what to write, NVN itself reads 0 when the game binds an empty
// slot.
VkImageView vk_pipe_dummy_view(void);
VkSampler vk_pipe_dummy_sampler(void);
// 空白 texel 视图：缓冲区贴图（texel buffer）那条绑定的回退，见 vk_pipe.c。
// A dummy texel view: the fallback for a buffer-texture (texel-buffer) binding, see vk_pipe.c.
VkBufferView vk_pipe_dummy_bufview(void);

// 合并后的绑定表（**布局顺序 = binding 升序**）。描述符写入必须按同一个顺序填动态偏移数组，
// 所以这张表要能取出来，而不是在两边各算一次。
// The merged binding table (**layout order = ascending binding**). Descriptor writing must fill its dynamic
// offset array in that same order, so the table must be obtainable rather than computed separately on both sides.
// set 是"阶段空间"：顶点阶段在 set 0、片元阶段在 set 1（见 vk_shader.c 装载处的改写说明）。表按
// (set, binding) 升序——描述符写入要按同一个顺序填动态偏移数组。
// `set` is the stage space: the vertex stage lives on set 0 and the fragment stage on set 1 (see the rewrite note at
// the load site in vk_shader.c). The table is sorted by (set, binding) -- descriptor writing fills its dynamic offset
// array in that same order.
typedef struct { uint32_t set, binding, count; VkDescriptorType type; VkShaderStageFlags stage; int kind; } vk_pipe_bind;
int vk_pipe_binds(const vk_shader *const *sh, int n, vk_pipe_bind *out, int cap);

// 指定 set 的描述符集布局（0 = 顶点空间，1 = 片元空间）；该 set 没有绑定时返回一个**空布局**（不是 NULL），
// 因为管线布局要求 set 号连续，写入侧也据此决定要绑几个 set。
// The descriptor set layout of one set (0 = the vertex space, 1 = the fragment space); a set with no bindings yields
// an **empty layout** (not NULL), because a pipeline layout wants consecutive set numbers and the write side uses
// that to decide how many sets to bind.
VkDescriptorSetLayout vk_pipe_set_layout_set(const vk_shader *const *sh, int n, uint32_t set);

// 按 NVN 图元号的累计绘制次数（out 至少 16 项）：用来看那四种没有对应物的图元到底会不会出现
// Cumulative draw count per NVN primitive number (out needs at least 16 entries): shows whether those four
// primitives without counterparts ever appear
void vk_pipe_prim_counts(uint64_t *out, int n);
