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

// 计数：建了多少个描述符集布局 / 管线布局 / 计算管线 / 图形管线，以及失败次数
// Counters: descriptor set layouts, pipeline layouts, compute pipelines and graphics pipelines created, plus failures
void vk_pipe_stats(uint64_t *set_layouts, uint64_t *layouts, uint64_t *cpso, uint64_t *gpso, uint64_t *fail);
