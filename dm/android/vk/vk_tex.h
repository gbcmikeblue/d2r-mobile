// NVN 贴图 → VkImage（安卓回放层）。
// NVN textures -> VkImage (the Android replay layer).
//
// 三件事决定了这一层的形状：
//   ① **按 `t->serial` 建表，不能用指针当键**：游戏会按值拷贝 NVNtexture 对象（nvn_obj.h 里 tex_t 的注释
//      就是这么写的，Metal 侧同样按序号查表），指针会失效；
//   ② **池内存是真值，VkImage 是派生**：客体的贴图数据在池里是 Tegra X1 块线性布局，GPU 要的是驱动的
//      optimal 布局，所以上传时要解排布（`nvn_deswizzle`，仓库既有实现，已编进安卓构建），回读时再排回去
//      （`nvn_swizzle`）；
//   ③ **可寻址 = 零拷贝内存**：池所在的那段本来就是 Vulkan 的零拷贝内存（见 vk_region.c），所以上传/回读
//      不需要额外的中转分配。
// Three things shape this layer:
//   1. **Keyed by `t->serial`, never by pointer**: the game copies NVNtexture objects by value (the comment on
//      tex_t in nvn_obj.h says as much, and the Metal side looks them up by serial too), so pointers go stale;
//   2. **Pool memory is the truth, the VkImage is derived**: the guest's texture data sits in the pool in Tegra
//      X1 block-linear layout while the GPU wants the driver's optimal layout, so an upload deswizzles through
//      the repository's own `nvn_deswizzle` (already compiled into the Android build) and a readback swizzles
//      back with `nvn_swizzle`;
//   3. **Addressable means zero-copy memory**: the pool's range is already Vulkan zero-copy memory (see
//      vk_region.c), so neither upload nor readback needs an extra bounce allocation.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "nvn_obj.h"

// NVN 格式 → VkFormat：0 = 没有对应（调用方报红一次即可）。压缩格式按未压缩的 Vulkan 格式给出，
// 上传时是否解压由调用方决定（安卓这边先直接按压缩格式上传，BC 解压留到需要时）。
// NVN format -> VkFormat: 0 means no counterpart (the caller should report a failure once). Compressed formats
// map to their compressed Vulkan format; whether to decompress on upload is the caller's call (Android uploads
// in compressed form for now, with BC decompression left until it is needed).
int vk_fmt_to_vk(int nvn_fmt, VkFormat *out);

// 取（必要时建 + 上传）这张贴图的 VkImage / 视图；失败返回 VK_NULL_HANDLE。
// **视图要带 view**：NVN 的 texture view（`view_t`）能限定 mip 范围（base_level/nlevels）、限定层范围
// （min_layer/nlayers）、**重解释格式**（set_format）、**重解释目标类型**（set_target，例如 3D 当 2D 数组看）。
// 传 NULL = 底视图。同一张贴图可以注册成多个 view（游戏就这么用，例如每级 mip 各占一个 id 当渲染目标），
// 所以这一层按 (序号, view 内容) 缓存多张 VkImageView。
// Get (creating and uploading if needed) this texture's VkImage / view; VK_NULL_HANDLE on failure.
// **Views matter**: an NVN texture view (`view_t`) can limit the mip range (base_level/nlevels), limit the layer range
// (min_layer/nlayers), **reinterpret the format** (set_format) and **reinterpret the target** (set_target, e.g. a 3D
// texture viewed as a 2D array). NULL means the base view. One texture may be registered as several views (the game
// does exactly that, e.g. one id per mip level as a render target), so this layer caches several VkImageViews per
// (serial, view content).
VkImage vk_tex_image(const tex_t *t);
VkImageView vk_tex_view(const tex_t *t, const view_t *v);

// 取（必要时建）这个采样器的 VkSampler；失败返回 VK_NULL_HANDLE。按参数内容缓存——游戏会按值拷贝
// NVNsampler 对象，同一个参数组合可能有多个对象，所以键是参数而不是对象。
// Get (creating if needed) this sampler's VkSampler; VK_NULL_HANDLE on failure. Cached by the parameters: the game
// copies NVNsampler objects by value, so one parameter set may exist as several objects and the key is the
// parameters rather than the object.
VkSampler vk_samp_of(const samp_t *s);

// 池内容被 CPU 改过（脏页跟踪的结论）：下次取用之前重传。释放时调 vk_tex_drop。
// The pool content was changed on the CPU (per the dirty-page tracking): re-upload before the next use. Call
// vk_tex_drop when the texture is released.
void vk_tex_invalidate(uint64_t serial);
void vk_tex_drop(uint64_t serial);

// 布局跟踪与转换：VkImage 的当前布局由这一层记着，转换只在真的变了时才插屏障。
// 回放层在开通道前把它转成附件布局、收通道后转回可采样布局；不这么做，下一帧采样会读到还没写完的内容。
// Layout tracking and transitions: this layer remembers each VkImage's current layout and only inserts a barrier
// when it actually changes. The replay layer turns a target into an attachment layout before opening a pass and
// back into a sampleable layout after closing it; without that, the next frame would sample unfinished content.
//
// 说明：贴图先创建后才有布局，所以传入 command buffer（上传也是这一层自己做的，创建时期望 UNDEFINED）。
// Note: a texture has a layout only once it exists, so a command buffer is passed in (uploads are done by this
// layer too, and a freshly created image starts out UNDEFINED).
void vk_tex_transition(const tex_t *t, VkImageLayout want, VkCommandBuffer cb);

// 标记/查询"这张贴图被 GPU 写过"：GPU 写过之后，CPU 读回必须走 VkImage 那一侧（把驱动布局排回池的块线性），
// 不能再直读池内存。用作渲染目标时由回放层调。
// Mark / query "the GPU has written this texture": after that, a CPU readback must go through the VkImage (swizzling
// the driver layout back into the pool's block-linear form) instead of reading pool memory directly. Called by the
// replay layer when the texture serves as a render target.
void vk_tex_gpu_written(const tex_t *t);
int vk_tex_is_gpu_written(const tex_t *t);

// CPU 读贴图（NVN ReadTexels）：与 Metal 侧同签名。这里走"池内存是真值"的直读路径（解排布直读），
// 还没考虑 GPU 写出过的贴图——那一类要等回读通了再从 VkImage 取（见 docs/ANDROID_PLAN 的 M2 进度）。
// CPU texture read (NVN ReadTexels), same signature as the Metal side. This takes the "pool memory is the truth"
// direct path (deswizzle straight out) and does not yet cover textures the GPU has written -- those need the
// readback path through the VkImage (see M2's progress in docs/ANDROID_PLAN).
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img);

// 诊断读数：建了多少张、上传多少张/多少字节、CPU 直读多少次、没有对应格式的次数
// Diagnostic counters: images created, uploads and bytes, direct CPU reads, and format misses
void vk_tex_stats(uint64_t *created, uint64_t *uploads, uint64_t *bytes, uint64_t *reads, uint64_t *fmt_miss);

// view 的计数：用底视图 / 命中已建子视图 / 新建子视图（三者的存在共同证明 view 这条路在跑）
// View counters: base view / sub-view cache hit / sub-view created (all three together show the view path runs)
void vk_tex_view_stats(uint64_t *base, uint64_t *hit, uint64_t *made);

// ReadTexels 里读到"被 GPU 写过"的贴图次数（0 = 那套从 VkImage 读回的机制暂时还只是工具，不是功能缺口）
// How many ReadTexels calls read a **GPU-written** texture (0 means readback through the VkImage is still only a
// debugging tool rather than a functional gap)
uint64_t vk_tex_reads_gpu_written(void);
