// 安卓图形后端的 Vulkan 设备（进程内只建一次）：回放层与内存池都从这儿取设备、队列与内存类型。
// The Android graphics backend's Vulkan device (created once per process): the replay layer and the memory
// pools take the device, queue and memory types from here.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

// 建设备（幂等）。真机上量出来的能力见 docs/ANDROID_PLAN 的 M2 第 1、3 步：
// Adreno 830 / Vulkan 1.3.284，有 VK_KHR_external_memory_fd，没有 VK_EXT_external_memory_host。
// Create the device (idempotent). The capabilities measured on a real device are in M2 steps 1 and 3 of
// docs/ANDROID_PLAN: Adreno 830 / Vulkan 1.3.284, with VK_KHR_external_memory_fd and without
// VK_EXT_external_memory_host.
void vk_device_init(void);

// 没建起来就返回 VK_NULL_HANDLE | returns VK_NULL_HANDLE when bring-up failed
VkInstance vk_instance(void);
VkDevice vk_dev(void);
VkQueue vk_queue(void);
uint32_t vk_queue_family(void);
const VkPhysicalDeviceMemoryProperties *vk_mem_props(void);

// 在满足 need_bits 的类型里找属性最贴近 want 的一个；找不到返回 UINT32_MAX。
// Find the type that satisfies need_bits and whose properties best match want; UINT32_MAX when there is none.
uint32_t vk_memory_type(VkMemoryPropertyFlags want, uint32_t need_bits);

// 零拷贝内存类型（DEVICE_LOCAL + HOST_VISIBLE + HOST_COHERENT）：客体内存与 GPU 共用同一份物理内存时用它。
// 真机上是类型 4。找不到返回 UINT32_MAX。
// The zero-copy memory type (DEVICE_LOCAL + HOST_VISIBLE + HOST_COHERENT), used when guest memory and the
// GPU share the same physical memory; it is type 4 on the real device. UINT32_MAX when there is none.
uint32_t vk_zero_copy_type(void);

// 这一段设备是否开了某个扩展 | whether this device enabled a given extension
int vk_have_ext(const char *name);

// 两处"能力缺口"的开关（都是先问再开，没开就由管线层退化并报黄，见 docs/ANDROID_PLAN 第 9 步）：
//   vk_have_fill_non_solid：非实心多边形模式可用（不然只能按实心画）
//   vk_have_attr_divisor：顶点流的每实例步进可大于 1（不然 divisor > 1 只能按 1 处理）
// The two capability switches behind the "known gaps" (both asked for before being enabled; when absent the
// pipeline layer degrades and warns, see step 9 of docs/ANDROID_PLAN):
//   vk_have_fill_non_solid: non-solid polygon modes are usable (otherwise only solid can be drawn)
//   vk_have_attr_divisor: a vertex stream's per-instance step may exceed 1 (otherwise divisor > 1 is treated as 1)
int vk_have_fill_non_solid(void);
int vk_have_attr_divisor(void);
int vk_have_anisotropy(void);          // 各向异性过滤可用（采样器要用）| anisotropic filtering available (needed by samplers)
int vk_have_attr_divisor_zero(void);   // divisor 可为 0（NVN 的"常量流"正好能用这个表达）| divisor may be 0 (exactly what NVN's "constant stream" needs)

// 动态渲染是否可用（回放层开渲染通道的前提；不可用时那一层退回只记状态）
// Whether dynamic rendering is available (the replay layer's precondition for opening passes; without it that
// layer falls back to recording state only)
int vk_have_dyn_render(void);

// 动态渲染的入口。安卓的 libvulkan.so 是**桩库**（只导出到 Vulkan 1.1），1.3 的入口直接引用会链接失败
// （undefined symbol: vkCmdBeginRendering），必须用 vkGetDeviceProcAddr 取——与取 vkGetMemoryFdKHR 同一个套路。
// 设备没起来或没取到时这两个指针是 NULL，调用方必须先看 vk_have_dyn_render()。
// The dynamic-rendering entry points. Android's libvulkan.so is a **stub** library (it exports up to Vulkan 1.1),
// so referring to a 1.3 entry point directly fails to link (undefined symbol: vkCmdBeginRendering); it has to be
// fetched with vkGetDeviceProcAddr -- the same pattern as vkGetMemoryFdKHR. Both pointers are NULL when the device
// is not up or the lookup failed, so callers must check vk_have_dyn_render() first.
extern PFN_vkCmdBeginRendering dm_vkCmdBeginRendering;
extern PFN_vkCmdEndRendering dm_vkCmdEndRendering;
