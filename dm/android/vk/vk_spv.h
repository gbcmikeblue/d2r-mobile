// 从 SPIR-V 里读描述符绑定——这是**唯一权威来源**。
// Reading descriptor bindings out of the SPIR-V -- the **only authoritative source**.
//
// 为什么不能只用 NVN 反射（<键>.res.json）：实测发现两者并不一一对应。res.json 讲的是"这份资源的数据从客体
// 的哪里来"（常量段哪个槽、地址在 c[..] 的哪个偏移），而**不是**"SPIR-V 里这条 binding 是什么类型"。
// 真机上的证据：22 条计算管线里 3 条建不出来（VkResult -13），对照 SPIR-V 后发现那 3 条在 SPIR-V 里多声明了
// 一个 binding 0 的 uniform 块、res.json 里没有它，而那个块是被用到的；描述符集布局不覆盖用到的绑定，
// 建管线就会失败。（反例：另一条着色器同样多声明了 b0 却建得成，因为它的 b0 没被用到。）
// Why the NVN reflection (<key>.res.json) alone is not enough: measurement showed the two do not correspond one to
// one. res.json says **where this resource's data comes from** in the guest (which constant slot, which offset inside
// c[..]), not which type the binding has in the SPIR-V. Evidence from the device: 3 of 22 compute pipelines failed
// to build (VkResult -13), and comparing against the SPIR-V showed those three additionally declare a uniform block
// at binding 0 that res.json does not mention and that is actually used; a descriptor set layout that does not cover
// a used binding makes pipeline creation fail. (Counter-example: another shader also declares an extra b0 and builds
// fine, because its b0 is unused.)
#pragma once

#include <stdint.h>
#include <stddef.h>

// 绑定种类（取自 SPIR-V 的类型声明）| binding kind (taken from the SPIR-V's type declarations)
enum {
    VK_SPV_UBO = 1,          // OpTypeStruct + Block（存储类 Uniform）| OpTypeStruct with Block (Uniform storage class)
    VK_SPV_SSBO,             // OpTypeStruct + BufferBlock（SPIR-V 1.0-1.2 的写法）或存储类 StorageBuffer | OpTypeStruct with BufferBlock (the SPIR-V 1.0-1.2 encoding) or StorageBuffer storage class
    VK_SPV_SAMPLED_IMAGE,    // OpTypeSampledImage（分离的贴图 + 采样器合并绑定）| OpTypeSampledImage (a separate texture and sampler bound together)
    VK_SPV_STORAGE_IMAGE,    // OpTypeImage 且 sampled = 2 | OpTypeImage with sampled = 2
    VK_SPV_IMAGE,            // OpTypeImage 且 sampled = 1（只取样、不合并采样器）| OpTypeImage with sampled = 1 (sampled only, no sampler folded in)
    VK_SPV_SAMPLER,          // OpTypeSampler
    VK_SPV_TEXEL,            // 缓冲贴图 | a buffer texture
    VK_SPV_OTHER
};

// count：这条绑定在 SPIR-V 里声明的**元素个数**（不是数组就是 1）。描述符集布局的 descriptorCount 直接用它——
// 早先固定写 1 是个静默假设（实测全库 44,156 条都不是数组，但那是"碰巧"而不是"保证"）。
// count: how many elements this binding declares in the SPIR-V (1 when it is not an array). A descriptor set
// layout's descriptorCount uses it directly -- hardcoding 1 was a silent assumption (measured: none of the
// library's 44,156 bindings is an array, but that was a coincidence rather than a guarantee).
typedef struct { uint32_t set, binding, count; int kind; } vk_spv_bind;

// 输入变量（顶点属性）：Location 与分量个数/类型。图形管线必须为着色器声明的**每一个**输入都配上顶点属性，
// 否则建管线直接失败；而"哪些属性"这件事只在 SPIR-V 里（res.json 只有资源绑定，实测顶点属性一条都没有）。
// 分量类型用于格式认不出来时的回退（与苹果侧的 NVN_CT_* 一套：Float4 / SInt4 / UInt4）。
// Input variables (vertex attributes): the Location and the component count/type. A graphics pipeline must supply
// a vertex attribute for **every** input a shader declares or creation fails outright; and "which attributes" is
// only in the SPIR-V (res.json has resource bindings only and, measured, no vertex attributes at all). The
// component type serves as the fallback when the format is unrecognised (the same set as Apple's NVN_CT_*:
// Float4 / SInt4 / UInt4).
enum { VK_SPV_F32 = 0, VK_SPV_I32, VK_SPV_U32 };
typedef struct { uint32_t loc, comps, ctype; } vk_spv_in;

// 扫描一个 SPIR-V 模块：描述符变量写进 out（最多 cap 条），输入变量写进 ins（最多 incap 条，可为 NULL）。
// 返回描述符条数；不是 SPIR-V、结果超限、或模块头的 id 上界超过表容量时返回 -1（调用方报红）。
// max: the module's words. Only declarations are read (everything precedes the first OpFunction).
// Scans a SPIR-V module: descriptor variables go into out (at most cap) and input variables into ins (at most
// incap; may be NULL). Returns the descriptor count, or -1 when the module is not SPIR-V, the result exceeds a
// cap, or the header's id bound exceeds the table capacity (the caller reports a failure).
int vk_spv_scan(const uint32_t *code, size_t bytes, vk_spv_bind *out, int cap,
                vk_spv_in *ins, int incap, int *nins_out);

// 种类名（日志用）| a kind's name (for logs)
const char *vk_spv_kind_name(int kind);
