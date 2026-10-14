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
