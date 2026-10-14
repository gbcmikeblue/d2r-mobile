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

// 缓冲区贴图（NVN target 10 = TARGET_BUFFER）→ VkBufferView（texel buffer）。**不是** VkImage 那条路：
// 这类贴图的存储就是内存池里的一段（width = 元素个数、格式给每元素字节），视图直接建在池缓冲上、零拷贝，
// 描述符那一侧要写 VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER。非缓冲区贴图或建不出来时返回 VK_NULL_HANDLE。
// A buffer texture (NVN target 10 = TARGET_BUFFER) -> a VkBufferView (a texel buffer). This is **not** the VkImage
// path: such a texture's storage is a stretch of a memory pool (width is the element count, the format gives the
// bytes per element), the view is built straight on the pool buffer with no copy, and the descriptor side has to
// write VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER. Returns VK_NULL_HANDLE for anything else or on failure.
VkBufferView vk_tex_bufview(const tex_t *t);

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

// 从**驱动侧**把一张贴图（某一级、第 0 层）读回主机内存，输出为 RGBA8、行距 `pitch` 字节。
// 与 `mtl_read_texels`（池内存直读）的区别：那条路读的是"池是真值"的表示，这条路读的是 **GPU 真正写过的那份**
// ——两者在"贴图被当渲染目标用过"之后就不一样了。走这里不做块解排布：Vulkan 的图像拷贝已经把驱动布局转成线性，
// 池里的块线性表示是另一回事。需要这张贴图的 VkImage **已经存在**（没被 GPU 用过就不该走这里，直读池即可）。
// 成功返回 1，并把实际尺寸写回 out_w / out_h。
// Read a texture (one level, layer 0) back from the **driver side** into host memory as RGBA8 with a row pitch of
// `pitch` bytes. The difference from `mtl_read_texels` (a direct read of pool memory): that one reads the
// "the pool is the truth" representation while this one reads **what the GPU actually wrote**, and the two diverge
// once a texture has served as a render target. No block deswizzle happens here -- a Vulkan image copy already turns
// the driver layout into linear order, and the pool's block-linear representation is a separate matter. The texture's
// VkImage must **already exist** (a texture the GPU never touched should not come this way; read the pool directly).
// Returns 1 on success and writes the actual size back through out_w / out_h.
int vk_tex_grab(const tex_t *t, unsigned level, uint8_t *dst, size_t pitch, uint32_t *out_w, uint32_t *out_h);

// 同一件事，但按**序号**问：截"游戏自己那些渲染目标"时只有序号可问（它们的 tex_t 不在回放层手上）。
// The same thing but asked by **serial**: grabbing "the game's own render targets" can only be asked by serial (the
// replay layer does not hold their tex_t).
int vk_tex_grab_ser(uint64_t serial, unsigned level, uint8_t *dst, size_t pitch, uint32_t *out_w, uint32_t *out_h);

// 某尺寸下的全部已知贴图序号（最多 cap 个，返回个数）
// Every known texture serial of a given size (at most cap of them; returns the count)
int vk_tex_same_size_serials(uint32_t w, uint32_t h, uint64_t *out, int cap);

// 列出所有已有图像的贴图（序号 + 尺寸 + **级数**），最多 cap 个，返回个数。扫描用：一次看全，不按尺寸筛。
// `lvs` 是每张贴图的 mip 级数——**扫描必须逐级读**：真机上第 0 级全零而第 1 级有内容（「M3-2m」），
// 只读第 0 级等于把"画面在第几级"这个问题跳过去。
// List every texture that already has an image (serial, size and **level count**), at most cap of them, returning the
// count. For a scan: see everything at once without filtering by size.
// `lvs` is each texture's mip count -- **a scan has to go level by level**: on the device level 0 reads all zero while
// level 1 has content ("M3-2m"), so reading level 0 alone skips the question "which level holds the picture".
int vk_tex_list(uint64_t *ser, uint32_t *ws, uint32_t *hs, uint32_t *lvs, int cap);

// 报一张贴图的身份：序号、尺寸、格式、是否窗口贴图、是否 GPU 写过。用来回答"这个序号到底是什么"——
// 真机上渲染目标前 5 里混着 143/67/68 这种每窗口上千次的序号，它们很像三张轮转的交换链纹理，而那件事必须问清。
// Report one texture's identity: serial, size, format, whether it is a window texture, whether the GPU has written it.
// It answers "what is this serial" -- on the device the top render targets are mixed with serials like 143/67/68 used
// thousands of times per window, which look like three rotating swapchain textures, and that has to be settled.
void vk_tex_dump_ser(uint64_t serial);

// 对照实验：给一张贴图清一个**已知的颜色**（走一次性的命令缓冲，与出图截图/扫描同一条"写入 → 读回 → 上屏"链路）。
// 它分开的是两件完全不同的事：屏幕/截图出现了这个颜色，说明**抓取与上屏那条链路是通的**，问题在游戏自己的绘制上；
// 屏幕还是黑的，说明**我们的写入/布局/读回本身有问题** —— 那么此前"目标全零"那条结论也要重新审视。
// A control experiment: clear one texture to a **known colour** (through a one-shot command buffer, the same
// "write -> read back -> present" chain the screenshots and scans use). It separates two very different things: if the
// screen or a screenshot shows that colour then **the grab-and-present chain works** and the trouble is in the game's own
// drawing; if the screen stays black then **our write, layout or readback is at fault**, and the earlier "the targets
// are all zero" conclusion has to be revisited too.
int vk_tex_probe_clear(uint64_t serial, float r, float g, float b, float a);

// 贴图 → 贴图拷贝（NVN 的 nvnCommandBufferCopyTextureToTexture）。记进**回放层的命令缓冲**，不另开一次提交
// ——顺序就是语义：拷贝必须排在产生源内容的那些绘制之后。区域里宽/高/深 <= 0 表示"从偏移到边界"（NVN 的约定）。
// 放在纹理层是因为只有这里同时知道两边的布局与"池是真值还是 GPU 写过"。
// Texture-to-texture copy (NVN's nvnCommandBufferCopyTextureToTexture). Recorded into the **replay layer's command
// buffer** rather than a separate submission -- order is the semantics here: the copy has to follow the draws that
// produce the source content. A region width/height/depth of <= 0 means "from the offset to the edge" (NVN's
// convention). It lives in the texture layer because only this layer knows both sides' layouts and whether the pool
// or the GPU is the truth.
int vk_tex_copy_t2t(const tex_t *src, const view_t *sv, const NVNcopyRegion *sr,
                    const tex_t *dst, const view_t *dv, const NVNcopyRegion *dr, VkCommandBuffer cb);
uint64_t vk_tex_t2t_count(void);

// 登记"这张贴图是窗口（交换链）贴图"（客体建窗口时逐个报过来）。这一档不是记账，是**存储归属**：
// 交换链贴图没有客体内存（存储由驱动持有），所以它照样要能建出图像——内容来自渲染或拷贝，不来自池。
// 不登记的话这类贴图会被"没有内存就不建图"那条规矩挡掉，而渲染目标那一侧只是静默跳过它：
// 结果是画面永远到不了出图纹理上（截图失败查出来的正是这条）。
// Mark a texture as a window (swapchain) texture (the guest reports each one when it builds a window). This is not
// bookkeeping but **storage ownership**: a swapchain texture has no guest memory (the driver owns its storage), yet it
// must still get an image -- its content comes from rendering or copying, not from the pool. Without the mark, the
// "no memory, no image" rule rejects it, and the render-target path merely skips it in silence: the picture then
// never reaches the present texture (which is exactly what the failed screenshot uncovered).
void vk_tex_mark_window_ser(uint64_t serial);
int vk_tex_is_window_ser(uint64_t serial);

// 诊断：列出"与给定尺寸相同的已知贴图"都是谁（序号、格式、是否窗口贴图、是否被 GPU 写过）。出图纹理读不回来时
// 用它对着看——"客体到底把画面画进了哪一张"这件事不该靠猜。
// Diagnostics: list which known textures share a given size (serial, format, whether it is a window texture, whether
// the GPU has written it). Use it when the present texture cannot be read back: "which texture did the guest actually
// draw the picture into" is not something to guess at.
void vk_tex_dump_size(uint32_t w, uint32_t h);

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
