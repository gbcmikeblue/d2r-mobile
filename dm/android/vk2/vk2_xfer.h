// 新地基 N2 之二：**上传窗口与回读窗口**（规格：docs/rewrite/ANDROID_REWRITE.zh-CN.md §9.5）。
//
// 这一块回答的是新地基的假设 2（"写完即可见"）：Vulkan 上没有它。CPU 写的只是**影子**，
// 要把影子变成 GPU 能看见的内容，必须**显式**做三件事：把影子拷进 host-visible 的**暂存环** →
// `vkCmdCopyBuffer` 到后备资源 → **在每个 run 后面插一条屏障**（`TRANSFER_WRITE → 各种 READ`）。
// 反过来，GPU 写过的东西要回到 CPU 视野里，必须**等这次提交的栅栏**再拷回来。
//
// 与 §9.5 的两处措辞差异（都能自证，不是省事）：
//   ① §9.5 写的是 `vk2_upload_begin/end` 两半。两半存在的理由是 N4 的回放层要把上传与别的命令**交错录制**；
//      N2 的搬运是**一次录完**的，所以先提供合体的 `vk2_upload()`。等 N4 真需要交错时再拆
//      （那时才有第二个调用方）——本轮拆了也只是多一个空壳。
//   ② §9.5 的回读签名是 `vk2_readback(src, off, dst, n)`，其中"等哪次提交的栅栏"在 N3 之前还没有
//      `vk2_sub` 可问，所以显式加一个 `vk2_readback_after(fence)`：**没指定要等谁就不回读**（返回 0 并报【黄】），
//      绝不"不知道等谁也照读"。
//
// 硬纪律（G4 的前置，本文件也执行）：**任何地方都不许 `vkQueueWaitIdle`**。本模块只用定向栅栏
// （`vkWaitForFences`，带超时；超时**如实报失败**，不重试、不放宽）。代码里放了自检计数器
// （`vkQueueWaitIdle_calls` 恒 0）。
//
// ---------------------------------------------------------------------------------------------------------------
// N2 part two: the upload and readback windows (spec: section 9.5). Vulkan has no "write is immediately visible",
// so turning a shadow update into something the GPU can see takes three explicit steps: copy the shadow into the
// host-visible staging ring, vkCmdCopyBuffer into the backing store, and insert a barrier after every run. Going
// the other way requires waiting on the producer's fence before copying back. Two deliberate differences from
// section 9.5 are documented in the header (the merged vk2_upload and the explicit readback fence). Hard rule:
// vkQueueWaitIdle is never called here, only targeted fences with a timeout, and a timeout is reported as failure.
#ifndef DM_VK2_XFER_H
#define DM_VK2_XFER_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "vk2_dirty.h"
#include "vk2_pool.h"

// 暂存环 = 16 MiB 起步（N0 的读数：与旧路 `DM_UP_RING` 同档）| the staging ring, 16 MiB to start (N0's reading)
#define VK2_RING_BYTES_DEFAULT (16ull * 1024ull * 1024ull)
// ---------------------------------------------------------------------------------------------------------------
// N4：**环按槽分**（N3 量出来的待办）。N2/N3 的环是**一份**、同一时刻只允许一个在飞窗口 ⇒ 在飞深度 3 时
// 第 2、3 个槽想录上传窗口会看到"环还被占着"，上传会把在飞深度**串行化**。N4 把它切成 `banks` 段（每个
// 槽一段，段内是一个**环形分配器**：一帧里可以录很多次上传，只要总和不超过这一段）。
//   ① 缺省 = **1 段**（= N2/N3 的一模一样的行为，N2/N3 的探针与回归读数因此不变）；
//   ② `vk2_xfer_set_banks(n)` 只在**没有任何段被占着**时能改（改法是把环均分，所以必须没有在飞窗口）。
// 环的**总**字节数还是 `ring_bytes`（上面那个常数 / 调用方给的值），切段只改"每段多少"。
// N4: the ring is split per slot (the to-do N3 measured). The ring was a single window, so only one in-flight slot
// could carry an upload; splitting it into banks (one per slot, each an allocator for a whole frame) is what keeps
// uploads from serialising the in-flight depth. One bank by default, which is exactly the N2/N3 behaviour.
#define VK2_BANKS_MAX 16
// 回读用的 host-visible 暂存缓冲 = 4 MiB 起步（一次 `vk2_readback` 内部按它切片循环）| readback staging, 4 MiB
#define VK2_RB_BYTES_DEFAULT   (4ull * 1024ull * 1024ull)
// 提交与等待的超时（毫秒）；超时 = 报失败，**不重试**（纪律：不许用加超时掩盖同步问题）
// Submission/wait timeout in milliseconds; a timeout is a reported failure, never a retry.
#define VK2_XFER_TIMEOUT_MS    5000u

typedef struct {
    uint64_t windows;          // 上传窗口次数 | upload windows
    uint64_t runs;             // 搬过的 run 数 | runs moved
    uint64_t copies;           // 实际 vkCmdCopyBuffer 条数（跨块脏 run 会拆成多条）| actual copy commands
    uint64_t barriers;         // 插过的屏障条数 | barriers inserted
    uint64_t upload_bytes;     // 搬上去的字节数 | bytes uploaded
    uint64_t upload_us;        // 上传窗口累计耗时（含 memcpy 与录制）| cumulative upload window time
    uint64_t split_runs;       // 被拆开**多出来**的拷贝数（跨块，或一条 run 比暂存环还大）| extra copies from splits
    uint64_t ring_full;        // 窗口超出暂存环（拒绝，不是静默截断）| window exceeded the ring (refused)
    uint64_t ring_wrap;        // 环回绕次数 | ring wraps
    uint64_t rb_calls;         // 回读调用次数 | readback calls
    uint64_t rb_bytes;         // 回读字节数 | bytes read back
    uint64_t rb_wait_us;       // 回读里等栅栏的累计耗时 | cumulative fence wait inside readback
    uint64_t rb_no_fence;      // 没指定要等谁就调回读（报【黄】、返回 0）| readback with no producer fence
    uint64_t fence_timeouts;   // 等栅栏超时次数（如实报失败）| fence timeouts (reported as failure)
    uint64_t idle_calls;       // **必须恒为 0**：模块里调 vkQueueWaitIdle 的次数（G4 的前置自检）
    uint64_t banks;            // N4：当前分了几段（1 = N2/N3 的单环）| N4: how many banks the ring is split into
    uint64_t initial_rounds;   // N4-b：初始同步用了几轮"录一批 → 提交 → 等栅栏"（末次）| initial-sync rounds, last call
    uint64_t initial_bytes;    // N4-b：初始同步累计搬了字节 | bytes moved by initial sync, cumulative
} vk2_xfer_stats;

// 一段（给一个提交槽用的那一份环）的读数：N4 要证明"在飞 3 个槽各载着一个上传窗口"。
// Per-bank readings: N4 has to show that three in-flight slots each carry an upload window.
typedef struct {
    uint64_t bytes;      // 这一段的容量 | capacity of this bank
    uint64_t used;       // 本帧已用（对齐后）| used so far in this frame
    uint64_t windows;    // 这一段上开过的窗口数 | windows opened on this bank
    uint64_t moved;      // 这一段搬过的字节 | bytes moved through this bank
    uint64_t ring_full;  // 这一段环满的次数（拒绝，不是静默截断）| times this bank ran out
    int      open;       // 现在被一个在飞窗口占着吗（只读）| held by an in-flight window right now
} vk2_bank_stats;

// 初始化：`d` 里的设备事实会被拷走；ring_bytes / rb_bytes = 0 用缺省。返回 0 = 失败。
// Initialise from the device facts; ring_bytes/rb_bytes of 0 mean the defaults.
int  vk2_xfer_init(const vk2_dev *d, uint64_t ring_bytes, uint64_t rb_bytes);
void vk2_xfer_fini(void);
uint64_t vk2_xfer_ring_bytes(void);
int  vk2_xfer_ready(void);

// ---- N4：环按槽分（见文件头那段）----------------------------------------------------------------
// 把环均分成 n 段（n 夹到 [1, VK2_BANKS_MAX]）；**有任何一段被占着时拒绝改动**（返回 0 并报【红】）。
// 返回实际生效的段数。缺省 = 1（N2/N3 的单环行为）。
// Split the ring into n banks (clamped); refused while any bank is held. Returns the effective count; 1 by default.
int      vk2_xfer_set_banks(int n);
int      vk2_xfer_banks(void);
uint64_t vk2_xfer_bank_bytes(int bank);              // 这一段的容量（banks 未设时 = 环的全部）
void     vk2_xfer_bank_stats_get(int bank, vk2_bank_stats *out);
// 交还一段（它的窗口真的做完了）：使用位置回到 0、段回到空闲。缺省段的 `vk2_xfer_window_done()` 就是它。
void     vk2_xfer_window_done_bank(int bank);
int      vk2_xfer_window_open_bank(int bank);        // 只读：这一段现在被占着吗

// ---- 上传窗口（§9.5）----------------------------------------------------------------------------
// 把一个池里所有 CPU_DIRTY 的 run 搬上去：
//   逐 run：`memcpy` 影子 → 暂存环 → `vkCmdCopyBuffer(环 → 后备)` → **每个 run 之后插一条屏障**。
// run 跨块边界时**拆成多条拷贝**（`split_runs` 计数）；搬完把那些页置 CLEAN。
// 返回搬上去的字节数；0 = 没搬或失败（环装不下 / 没就绪 ⇒ 记账里能看出来）。
// 调用方负责：提交这条 cb 并等它自己的栅栏，然后调 `vk2_xfer_window_done()` 把环重新开一轮。
// Moves every CPU_DIRTY run of one pool: memcpy into the ring, vkCmdCopyBuffer into the chunk, and one barrier
// after each run; runs crossing chunk boundaries are split into several copies. Pages are marked CLEAN once
// copied. The caller submits the command buffer, waits on its own fence, then calls vk2_xfer_window_done().
uint64_t vk2_upload(VkCommandBuffer cb, uint64_t frame, int pool_index);

// ---- N4：按段上传（回放层真正用的入口）----------------------------------------------------------
// 与 `vk2_upload` 同一件事，但走**指定那一段**，而且**一段里可以录很多次**（一帧要搬好几个池、
// 一帧里还会因为"绑定点的触碰标记"再搬几次）：第一次调用开这一段的窗口，之后往同一段里追加。
// 段装满 ⇒ 记账 `ring_full` 并**停**（剩下的页仍然是脏的，下一个窗口/下一帧继续），**不静默截断**。
// 返回这一次搬上去的字节数（0 = 没搬 or 段满/没就绪）。
// Same as vk2_upload but into one named bank, and a bank can host several uploads in one frame (several pools, and
// the re-uploads caused by binding-point touches). A full bank counts ring_full and stops, leaving the rest dirty.
uint64_t vk2_upload_into(int bank, VkCommandBuffer cb, uint64_t frame, int pool_index);
// 把**所有**登记着的池都按段搬一遍（回放层的帧开头上传就是它）：逐池调上面那个，累加返回。
// 一帧的搬运字节数（11.3 的第一个字段）由调用方累加它拿到。
uint64_t vk2_upload_all_into(int bank, VkCommandBuffer cb, uint64_t frame);

// N4-b：**初始同步** —— 池刚登记时整池是脏的（后备资源在第一次上传之前未定义），而"一帧一段环"的节奏
// 要几百帧才搬完，那几百帧里游戏读到的是没搬上去的部分。所以第一次提交之前先把脏页**整批搬完**：
// 循环"录一批（环装满为止）→ 提交 → 等它自己的栅栏 → 交还这一段"，直到没有可搬的脏页。
// 用它自己的命令缓冲与栅栏（**不与回读那条路共用**：回读可能同时从前端线程发出来）。
// 返回搬上去的总字节；0 = 没有脏页或失败（失败会如实报）。
// N4-b initial sync: push the whole dirty set in one batch before the first submission, looping
// record-submit-wait-hand-back. Uses its own command buffer and fence so it cannot collide with readbacks.
uint64_t vk2_xfer_initial_sync(int bank);

// 一个窗口结束后调用：环的使用位置回到 0（**下一次窗口的 memcpy 不能盖掉在飞窗口的环内容**）。
// Called when a window is finished: the ring write position returns to 0, so the next window cannot overwrite
// the data of an in-flight one.
void vk2_xfer_window_done(void);
// 现在有没有一个窗口占着暂存环（**只读**，用于 N3/N4 判断"环能不能交出去"）。
// 背景（N3 量出来的事实）：**环是单窗口的** —— 在飞深度 3 时，第 2、3 个槽想录上传窗口会看到环还被占着
// ⇒ 那时不该调 `vk2_upload`（它会把"上一个窗口还没收尾"报成【红】），而应该等载着窗口的那个槽回收。
// Whether a window currently holds the staging ring (read-only). The ring is single-window: with several
// submissions in flight only one of them can carry an upload window -- N3's probe measures this and N4 has to
// give the ring per-slot storage.
int  vk2_xfer_window_open(void);

// ---- 回读窗口（§9.5）---------------------------------------------------------------------------
// 指定"这次回读要等谁的栅栏"（N2：调用方给；N4：由 `vk2_sub` 给）。不指定 ⇒ `vk2_readback` 返回 0 并报【黄】。
// Names the fence this readback must wait for; without it vk2_readback refuses (0 + yellow log).
void vk2_readback_after(VkFence producer);
// 等 producer 栅栏（超时即失败）→ `vkCmdCopyBuffer(后备 → host-visible)` → 等这条拷贝自己的栅栏 →
// `memcpy` 到 `dst`。返回 1 = 成功、0 = 失败（超时 / 没指定栅栏 / 没就绪）。**内部按 rb 缓冲切片循环**。
// Waits for the producer's fence, copies into host-visible memory, waits for that copy's own fence, then
// memcpys into dst. 1 = success, 0 = failure (timeout / no fence named / not ready).
int  vk2_readback(VkBuffer src, uint64_t off, void *dst, size_t n);
// 池感知版：自己按池内存地址反查出 (块, 块内偏移)，读回**影子**，并把这段置 CLEAN。
// Pool-aware form: resolves the address to (chunk, offset), reads into the shadow, and marks the range CLEAN.
int  vk2_readback_pool(void *pool_mem, uint64_t pool_off, size_t n);

void vk2_xfer_stats_get(vk2_xfer_stats *out);
void vk2_xfer_stats_reset(void);
void vk2_xfer_set_log(void (*fn)(const char *fmt, ...));

#endif // DM_VK2_XFER_H
