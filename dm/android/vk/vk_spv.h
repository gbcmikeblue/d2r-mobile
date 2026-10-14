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

typedef struct { uint32_t set, binding; int kind; } vk_spv_bind;

// 扫描一个 SPIR-V 模块里所有描述符变量（存储类 UniformConstant / Uniform），按 set、binding 升序返回。
// 返回条数；不是 SPIR-V、或结果超过 cap 时返回 -1（调用方报红）。
// max: the module's words. Only declarations are read (everything precedes the first OpFunction).
// Scans every descriptor variable (UniformConstant / Uniform storage class) of a SPIR-V module and returns them
// sorted by set then binding. Returns the count, or -1 when the module is not SPIR-V or the result exceeds cap
// (the caller reports a failure). The scan reads declarations only (everything precedes the first OpFunction).
int vk_spv_scan(const uint32_t *code, size_t bytes, vk_spv_bind *out, int cap);

// 种类名（日志用）| a kind's name (for logs)
const char *vk_spv_kind_name(int kind);
