// 安卓图形后端的着色器装载：控制段 → 键 → <键>.spv → VkShaderModule，并把 <键>.res.json 的绑定表读成定长数组。
// Shader loading for the Android graphics backend: control segment -> key -> <key>.spv -> VkShaderModule, plus
// reading the binding table of <key>.res.json into fixed-length arrays.
//
// 键与苹果侧**同一条算法**（dm/gfx/nvn/nvn_mtl.m 的 prog_get）：键 = sha256(控制段 0x880 字节) 的前 16 位十六进制。
// 着色器产物是从原版翻译出来的 `<键>.spv`（dm/gfx/tools/dmsh_xlate），运行时按这个键去找，两边算出的是同一个键。
// The key uses **the same algorithm as the Apple side** (prog_get in dm/gfx/nvn/nvn_mtl.m): the first 16 hex
// digits of sha256 of the 0x880-byte control segment. The shader artifacts are `<key>.spv`, translated from the
// originals by dm/gfx/tools/dmsh_xlate, and are found at run time by that key, which both sides compute alike.
//
// 反射的来源与苹果不同：苹果读 `<键>_b.json`（dmsh_build.py 的产物，含 MSL 类型、c1 段、驱动段读法这些
// Metal 参数缓冲才需要的东西）；安卓读翻译器直接给出的 `<键>.res.json`——它只讲一件事：**每个 SPIR-V
// binding 对应客体哪一份资源**（uniform 缓冲的 c 槽、存储缓冲地址在 c[..] 里的偏移、贴图/采样器句柄的来处）。
// 那正是建描述符集所需要的最小集，也正是苹果侧那一大堆字段里我们唯一用得上的部分。
// The reflection comes from a different place than on Apple: Apple reads `<key>_b.json` (an output of
// dmsh_build.py carrying MSL types, the c1 segment and driver-segment reads -- things only Metal's argument
// buffers need), while Android reads `<key>.res.json` straight from the translator. That file says exactly one
// thing: **which guest resource each SPIR-V binding corresponds to** (the c slot of a uniform buffer, the offset
// inside c[..] holding a storage buffer's address, where a texture/sampler handle comes from). That is the
// minimum needed to build descriptor sets, and it is the only part of Apple's pile of fields we can use.
#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

// NVN 阶段号，取值与 nvn_mtl.m 的 stage_of 一致——那边从映射表的中文列名翻出来（顶点/像素/计算/外壳/域），
// 安卓这边直接从 res.json 的 stage 列（vs/ps/cs）翻，两个来源指向同一组阶段号。
// NVN stage numbers, the same values nvn_mtl.m's stage_of produces -- there they come from the Chinese column
// names in the mapping table (vertex/pixel/compute/hull/domain), here from the stage field of res.json
// (vs/ps/cs); both sources name the same set of stages.
enum { VK_STAGE_VS = 0, VK_STAGE_PS = 1, VK_STAGE_HS = 3, VK_STAGE_DS = 4, VK_STAGE_CS = 5 };
#define VK_STAGE_NONE (-1)

// 控制段长度与键长度（键是 16 位十六进制，刚好是字段名里那串的样子）| control segment length and key length
#define VK_SHADER_CTL_LEN 0x880
#define VK_SHADER_KEY_LEN 16

// uniform 缓冲：SPIR-V binding ↔ 客体常量段槽号（机器码里的 c[3 + slot]）| uniform buffer: SPIR-V binding <-> guest constant slot (c[3 + slot] in the machine code)
typedef struct { uint32_t binding; int32_t slot; } vk_cbuf_ref;
// 存储缓冲：地址不在常量段里、而在 c[cb_slot][cb_offset] 指向的全局内存里 | storage buffer: its address is not in the constant segment but in global memory pointed at by c[cb_slot][cb_offset]
typedef struct { uint32_t binding; int32_t slot, cb_slot, cb_offset; uint32_t flags; } vk_sbuf_ref;
// 贴图/图像：句柄的来处（cbuf_slot < 0 表示句柄是常量里 baked 进来的那个数）与类型/格式，见 res.json。
// writable 区分来源：res.json 的 images 数组（可写图像 → STORAGE_IMAGE）与 textures 数组（只读 → 组合图像采样器）。
// Texture/image: where the handle comes from (cbuf_slot < 0 means the handle is the number baked into the constants)
// plus type/format, as in res.json. `writable` records the source: the images array of res.json (a writable image ->
// STORAGE_IMAGE) versus the textures array (read-only -> a combined image sampler).
typedef struct { uint32_t binding, type, array, separate, flags, writable; int32_t cbuf_slot, handle; char format[24]; } vk_tex_ref;

typedef struct {
    char key[VK_SHADER_KEY_LEN + 1];
    int stage;                       // VK_STAGE_* | one of VK_STAGE_*
    vk_cbuf_ref *cbuf; uint32_t ncbuf;
    vk_sbuf_ref *sbuf; uint32_t nsbuf;
    vk_tex_ref  *tex;  uint32_t ntex;
    VkShaderModule mod;              // 没建成是 VK_NULL_HANDLE（此时整条记录不会进缓存）| VK_NULL_HANDLE when creation failed (such a record never enters the cache)
} vk_shader;

// 解析 <DM_ROOT>/spv（或 DM_NVN_SPV_DIR 指定的目录），幂等。
// Resolve the shader directory (<DM_ROOT>/spv, or the one named by DM_NVN_SPV_DIR); idempotent.
void vk_shader_init(void);
const char *vk_shader_dir(void);

// 只算键，不装载（诊断、探针、以及将来核对映射表用）| Compute the key only, without loading (for diagnostics, probes and future cross-checks against the mapping table)
int vk_shader_ctl_key(const void *control, char out[VK_SHADER_KEY_LEN + 1]);

// 按控制段取着色器：算键 → 装载 → 缓存。找不到 <键>.spv、json 读不动、SPIR-V 建不出模块都返回 NULL，
// 并已计入 vk_shader_stats（调用方据此在日志里报红、按设计计数——见 docs/ANDROID_PLAN 的硬边界）。
// Fetch a shader by control segment: key -> load -> cache. A missing <key>.spv, an unreadable json or a
// SPIR-V module that cannot be created all return NULL and are counted by vk_shader_stats (callers report them
// in the log and count them, per the hard boundary in docs/ANDROID_PLAN).
const vk_shader *vk_shader_get(const void *control);

// 已知键与阶段（stage 传 VK_STAGE_NONE 表示从 res.json 里读）：探针按目录里的文件直接装载时用。
// By known key and stage (pass VK_STAGE_NONE to take the stage from res.json): used by probes that load the
// files in the directory directly.
const vk_shader *vk_shader_by_key(const char *key, int stage);

// 装载计数：成功 / 键不在产物里（缺）、产物在但坏掉（json 或 SPIR-V 有问题）| Load counters: succeeded / key not among the artifacts (missing) / artifact present but broken (json or SPIR-V)
void vk_shader_stats(uint32_t *loaded, uint32_t *missing, uint32_t *bad);
