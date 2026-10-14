// 客体地址空间里「需要 GPU 直接访问」的段（堆、别名区）：把它们换成 Vulkan 的零拷贝内存。
// The parts of the guest address space that need direct GPU access (the heap and the alias region): turning
// them into Vulkan zero-copy memory.
//
// 为什么要这么绕（见 docs/ANDROID_PLAN 的 M2 第 3 步）：Metal 那边是把宿主任意指针包成零拷贝 MTLBuffer
// （苹果有统一内存），而这台设备没有 VK_EXT_external_memory_host，反不过来。可行的是反方向：
// 分配 Vulkan 的可导出内存 → 取 fd → mmap(MAP_FIXED) 压回客体该在的地址。位置与大小一个都不动，
// 所以 QueryMemory / GetInfo 的算法、地址偏移换算一概不变。
// Why this detour (see M2 step 3 in docs/ANDROID_PLAN): on the Metal side any host pointer is wrapped in a
// zero-copy MTLBuffer (Apple has unified memory), and this device has no VK_EXT_external_memory_host, so
// that direction is closed. What works is the reverse: allocate exportable Vulkan memory, take its fd, and
// mmap(MAP_FIXED) it back at the address the guest expects. Neither the addresses nor the sizes change, so
// the QueryMemory / GetInfo arithmetic and every address-offset computation stay exactly as they are.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vulkan/vulkan.h>

// 宿主登记的钩子实现（见 dm/host/dm_gpu_region.h 的 dm_gpu_region_use）。
// The hook implementation the host registers (dm_gpu_region_use in dm/host/dm_gpu_region.h).
size_t vk_region_use(void *at, size_t size, const char *what);

// 把 [at, at+size) 换成 Vulkan 零拷贝内存，返回换成的字节数（小于 size 时必须由调用方据此调整它报给客体的大小，
// 不能静默当成成功——真机上单次分配上限 3968 MB，客体堆上限 4 GB 会顶到这个上限）。
// Turn [at, at+size) into Vulkan zero-copy memory; returns the number of bytes converted. When it is less
// than size the caller must adjust the size it reports to the guest rather than silently treating it as
// success -- the per-allocation limit on the real device is 3968 MB and the guest heap's cap is 4 GB, which
// does reach that limit.
size_t vk_region_overlay(void *at, size_t size, const char *what);

// 按地址反查所在的 Vulkan 分配与偏移：回放层把池子里的地址变成 VkBuffer 时用。成功返回 1。
// Look up which Vulkan allocation contains an address, and at what offset: used by the replay layer when it
// turns an address inside a pool into a VkBuffer. Returns 1 on success.
int vk_region_find(uint64_t addr, uint64_t len, VkDeviceMemory *mem, uint64_t *off);

// 已换轨的字节数与段数（给日志与诊断用）| bytes and regions already overlaid (for logging and diagnostics)
void vk_region_stats(uint64_t *bytes, int *regions);
