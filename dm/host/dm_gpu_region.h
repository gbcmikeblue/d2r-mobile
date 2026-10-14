// 客体地址空间里「需要 GPU 直接访问」的段（堆、别名区）交给图形后端换轨的钩子。
// The hook that hands the parts of the guest address space needing direct GPU access (the heap and the alias
// region) to the graphics backend for conversion.
//
// 为什么要有这个东西：苹果侧有统一内存，宿主任意指针都能包成零拷贝的 MTLBuffer，所以堆与别名区用普通内存
// 也行；安卓侧没有对应的 Vulkan 扩展（真机上没有 VK_EXT_external_memory_host，见 docs/ANDROID_PLAN 的
// M2 第 3 步），只能反过来——由 Vulkan 出内存、再 mmap 回客体该在的地址。宿主自己不认识 Vulkan / Metal，
// 所以只留一个函数指针，由各平台的图形后端在启动时登记实现。
// Why this exists: Apple has unified memory, so any host pointer can be wrapped in a zero-copy MTLBuffer and
// the heap and alias region get away with ordinary memory; Android has no corresponding Vulkan extension
// (the real device lacks VK_EXT_external_memory_host, see M2 step 3 in docs/ANDROID_PLAN), so the direction
// has to be reversed -- Vulkan provides the memory and it is mmap'd back at the address the guest expects.
// The host itself knows neither Vulkan nor Metal, so it only keeps a function pointer that each platform's
// graphics backend registers an implementation for at startup.
#pragma once
#include <stddef.h>

// 用图形后端的内存占住 [at, at+size)，返回「换成 GPU 可见内存的字节数」：
//   · 等于 size：整段都换成了；
//   · 小于 size（含 0）：只换了这么多。**调用方必须据此调整自己报给客体的大小**，不能静默当成成功——
//     真机上单次分配上限 3968 MB，而客体堆的上限是 4 GB，会顶到这个上限。
// 未登记（苹果侧，或图形后端还没起来）时为 NULL，此时调用方按普通内存走。
// Occupy [at, at+size) with the graphics backend's memory; returns how many bytes became GPU-visible:
//   - equal to size: the whole range was converted;
//   - less than size (including 0): only that much. **The caller must adjust the size it reports to the
//     guest accordingly** rather than silently treating it as success -- the per-allocation limit on the
//     real device is 3968 MB and the guest heap's cap is 4 GB, which does reach that limit.
// It is NULL when nothing is registered (on Apple, or before the graphics backend comes up), and the caller
// then proceeds with ordinary memory.
extern size_t (*dm_gpu_region_use)(void *at, size_t size, const char *what);
