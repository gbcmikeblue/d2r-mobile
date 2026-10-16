// 新地基 N3：**帧批提交 + 有界在飞**（规格：docs/rewrite/ANDROID_REWRITE.zh-CN.md §9.6，判据 **G4**）。
//
// 这一块回答的是新地基的假设 3（"Metal 的小提交节奏可以逐个 op 平移"）：Vulkan 上没有"小提交便宜"这回事，
// 而旧路的做法（8 槽轮转 + 每槽一条命令缓冲 + **队列锁** + 到处 `vkQueueWaitIdle`）正是驱动在"每帧上千次微提交
// 且没有依赖表达"下停摆的形状（`ANDROID_REWRITE` §二·假设 3）。新做法只有两条：
//
//   ① **一个「客体提交」= 一次 `vkQueueSubmit`**（这正是 NVN `nvnQueueSubmitCommands` 的语义）：
//      命令缓冲内部只记不提交，`vk2_sub_begin()` 取槽开始录、`vk2_sub_commit()` 收尾并提交一次。
//   ② **有界在飞**（`VK2_INFLIGHT` 个槽）：槽满了**只等最老那个槽的栅栏**（`vkWaitForFences`，**带超时**，
//      超时**如实报失败**）—— **绝不等队列空闲**。要等就等**具体栅栏**（§9.6 / §六·G4）。
//
// **`vkQueueWaitIdle` 在这个编译单元里也宏成"计数 + 报红"**（同 N2 的 `vk2_xfer.c` 那套纪律手法）：
// 谁以后手滑写上一句，计数不会静默为 0 —— 因为 **G4 的硬判据就是它恒为 0**。
//
// -------------------------------------------------------------- 与 N2 搬运窗口的关系（N4 怎么接）
// N2 的 `vk2_upload(cb, frame, pool)` 把"这一池的 CPU_DIRTY run"**录进调用方给的 cb**，而暂存环是**一份**、
// 且 `vk2_upload()` 在"上一个窗口还没收尾"时会**拒绝**（否则新的 memcpy 会盖掉在飞窗口的环内容）。
// ⇒ 两者必须按**槽的存活期**绑在一起：
//   · 录制：`s = vk2_sub_begin(); moved = vk2_upload(s->cb, frame, pool); vk2_sub_note_upload(); vk2_sub_commit();`
//     —— `vk2_sub_note_upload()` 告诉本模块"这个槽载着一个上传窗口"；
//   · 回收：**这个槽的栅栏被等到（`vk2_sub_wait` / `vk2_sub_wait_all` / 取槽时的轮询）时**，本模块
//     自动调一次 `vk2_xfer_window_done()` 把环交还（`upload_recycled` 计数）—— 环只有在"录它的那次提交
//     确实做完了"之后才能被下一窗口复用，这就是"环的存活期 ≤ 槽的存活期"这条不变式。
//   · **本轮的边界（量出来的事实，写在这里当 N4 的入口）**：暂存环是**一份**，所以**同一时刻只能有一个
//     在飞窗口**；在飞深度 3 时，第 2、3 个槽想录上传窗口会看到"环还被占着"（探针的 Q6 把这件事量出来，
//     见下方 `vk2_xfer_window_open()`）。**N4 要按槽分环**（或给环做分配器）才能让上传与在飞深度解耦。
//
// **不做**（本轮明确不做）：不接管回放层（`nvn_vk.c` / `vk_tex.c` / `vk_region.c` 一行不动，那是 N4）、
// 不做时间线信号量（§9.6 那条"前端上屏与被显示的提交排进同一条队列的序"要等有第二个调用方 = N4）。
//
// ---------------------------------------------------------------------------------------------------------------
// N3 part three: frame-batch submission with bounded in-flight work (spec: section 9.6, gate G4). Vulkan has no
// "small submissions are cheap", and the old road's 8 slots + queue lock + vkQueueWaitIdle everywhere is exactly
// the shape that stalls the driver. Two rules replace it: one guest submission is one vkQueueSubmit, and at most
// VK2_INFLIGHT submissions are in flight -- a full set waits on *the oldest slot's fence* (with a timeout, reported
// as failure), never on queue idle. vkQueueWaitIdle is macro'd into a counter plus a red log inside this file,
// because G4's hard criterion is that it stays zero. The header also records how a slot carries an N2 upload
// window (note_upload / recycle on reap) and the limit this round measured: the staging ring is single-window, so
// only one in-flight slot can carry an upload window -- N4 has to give the ring per-slot storage.
#ifndef DM_VK2_SUB_H
#define DM_VK2_SUB_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "vk2_pool.h"   // vk2_dev：本模块要的那几件设备事实 | the device facts this module needs

// 有界在飞的缺省深度 = **3**（N0 的读数起步值，见 §9.9）。真正定值是 N3 的读数说了算：
// 「槽等待率」+「长等条数」（探针的 Q3 把 D=1/2/3/4/8 各量一遍）。
// Default in-flight depth = 3 (N0's starting value); N3's readings (slot wait rate + long-wait count) settle it.
#define VK2_INFLIGHT_DEFAULT 3
#define VK2_INFLIGHT_MAX     16

// 等栅栏的超时（毫秒）：超时 = **如实报失败**，不重试、不放宽。
// 纪律：不许用"加超时 / 加保活 / 加子"掩盖同步问题 —— 但"带超时地等定向栅栏、超时即报失败"是 §9.6 明确要的，
// 两者不是一回事（前者掩盖，后者把失败暴露出来）。
// Fence wait timeout in ms; a timeout is a reported failure, never a retry (section 9.6 asks for exactly this).
#define VK2_SUB_TIMEOUT_MS   5000u

// 「长等」的门槛（微秒）：等待超过它记一笔（11.3 的"长等条数"）。**机制读数**：真帧的长等条数等 N4。
// The threshold that makes a wait a "long wait" (section 11.3's long-wait count); a mechanism reading this round.
#define VK2_SUB_LONG_WAIT_US 1000ull

// 一个提交槽（§9.6）：自己的命令池 + 命令缓冲 + 栅栏 + 它承载的客体提交序号。
// One submission slot (section 9.6): its own pool, command buffer, fence and the guest submission it carries.
typedef struct {
    VkCommandPool   pool;       // **必须**带 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT（N0 的第二个坑）
    VkCommandBuffer cb;         // 每次取槽都 vkResetCommandBuffer 后重录 | re-recorded on every acquire
    VkFence         fence;      // 定向等待的唯一凭据（创建时不带 SIGNALED）| the only handle a wait may use
    uint64_t        frame;      // 这一槽承载的客体提交序号（1 起）；不在飞时无意义 | the frame it carries
    int             in_flight;  // 1 = 已提交、栅栏还没被观察到 | submitted, fence not yet observed
    int             has_upload; // 1 = 这一槽里录过 N2 的上传窗口（回收时要把环交还）| carries an upload window
    int             bank;       // N4：这一槽用暂存环的哪一段（缺省 0 = N3 的单环行为）| which ring bank
    uint64_t        uses;       // 这一槽被取用过多少次 | how often this slot has been acquired
} vk2_slot;

// 计数：G4 的判据（`commits == submits`、`idle_calls == 0`）+ 槽等待率与长等条数的分子分母。
// Counters: G4's criteria plus the numerator/denominator of the slot wait rate and the long-wait count.
typedef struct {
    uint64_t begins, aborts;      // 取槽成功次数 / 取了槽又没提交（abort）的次数 | acquisitions / aborts
    uint64_t commits, submits;    // **G4：这两个必须相等**（一次客体提交 = 一次 vkQueueSubmit）
    uint64_t submit_fail;         // vkQueueSubmit 返回非 VK_SUCCESS 的次数（必须 0）| failed vkQueueSubmit calls
    uint64_t waits;               // **取槽时真等栅栏**的次数（槽等待率 = waits ÷ begins）| real waits in begin
    uint64_t wait_us, wait_max_us;// 取槽等待的累计 / 最长（µs）| begin-side wait time, cumulative / longest
    uint64_t long_waits, long_max_us; // 取槽等待超过 VK2_SUB_LONG_WAIT_US 的条数与最长一次 | long waits
    uint64_t poll_reaps;          // 取槽前用 vkGetFenceStatus 就发现已完成、**没等**的槽数 | polled, not waited
    uint64_t reclaims;            // 槽被回收的次数（轮询回收 + 等待回收）| slots reclaimed
    uint64_t timeout_fails;       // 等栅栏超时（如实报失败）| fence wait timeouts
    uint64_t no_slot_fail;        // 槽满且等待失败 ⇒ 取不到槽的次数 | no slot could be acquired
    uint64_t wait1_calls;         // vk2_sub_wait(frame) 调用次数 | targeted single-frame waits
    uint64_t wait1_waited;        // 其中真等了栅栏的次数 | how many of them actually waited
    uint64_t wait1_timeouts;      // N4-c：**按调用方超时**等、真的超时了的次数 | user-timeout waits that expired
    uint64_t wait1_us, wait1_max_us;   // 它们等到的时长（累计 / 最长）| their wait time
    uint64_t wait_all_calls;      // vk2_sub_wait_all 调用次数 | vk2_sub_wait_all calls
    uint64_t wait_all_waited;     // 它等到的在飞槽数（**不是等队列空闲**）| in-flight slots it waited on
    uint64_t wait_all_us, wait_all_max_us, wait_all_long;
    uint64_t upload_marks;        // vk2_sub_note_upload 调用次数 | note_upload calls
    uint64_t upload_recycled;     // 回收时真的把上传窗口交还（vk2_xfer_window_done）的次数 | windows recycled
    uint64_t banks;               // N4：环按槽分了几段（0 = 没分，所有槽共用第 0 段 = N3 行为）| ring banks
    uint64_t use_banks_calls;     // vk2_sub_use_banks 调用次数 | use_banks calls
    uint64_t idle_calls;          // **必须恒为 0**：本文件里出现 vkQueueWaitIdle 的次数（G4 的硬判据）
    uint64_t reaped_max;          // 已确认完成的最大 frame（**单队列 ⇒ 栅栏按提交序 signal**）
    uint64_t reap_hook_calls;     // N4-c：回收钩子被调用的次数（每个被观察到完成的槽一次）
    uint64_t poll_over_deep;      // N4-c：`vk2_sub_poll` 里"这个 frame 已经不在飞了"的次数（= 早就回收）| polled past the window
} vk2_sub_stats;

// ---------------------------------------------------------------- 生命周期
// `d` 里的设备事实会被拷走（调用方不必保活）；inflight ≤ 0 用 VK2_INFLIGHT_DEFAULT、上限 VK2_INFLIGHT_MAX。
// 返回 0 = 失败（日志里说明为什么）。**init 会把 frame 序号与计数清成新一轮**（探针按档重 initWith 不同深度）。
// Initialise (the facts are copied, inflight <= 0 means the default); init starts a fresh frame epoch.
int  vk2_sub_init(const vk2_dev *d, int inflight);
// 收尾：先把在飞的等回来（带超时，等不到就【黄】如实说明），再销毁槽（进程要退了就别留着未等的栅栏）。
// Tear down: try to wait the in-flight work back first (with a timeout), then destroy the slots.
void vk2_sub_fini(void);
int  vk2_sub_ready(void);
int  vk2_sub_inflight(void);       // 实际生效的在飞深度 | the in-flight depth actually in use

// ---------------------------------------------------------------- 提交模型（§9.6）
// 取一个空槽（**满了只等最老那个槽的栅栏**，带超时；绝不等队列空闲）。返回的槽的 `cb` 已经处于录制态，
// 调用方往它里面录（含 N2 的 `vk2_upload(s->cb, …)`）。返回 NULL = 取不到（为什么看日志与计数）。
// Acquire a free slot: a full set waits on the oldest fence only. The returned slot's cb is recording.
// 先轮询一遍 `vkGetFenceStatus`（**这不是等待**）把已经做完的槽收掉，再去等；两者分开计数。
vk2_slot *vk2_sub_begin(void);
// 收尾 + `vkResetFences` + `vkQueueSubmit(1, &cb, slot->fence)` —— **一次客体提交 = 一次 vkQueueSubmit**。
// 返回 0 = 失败（cb 收不了尾 / 提交返回非成功），失败也会计数（`commits` 与 `submits` 由此可以看出不一致）。
// Finish recording, reset the fence, submit once. One guest submission is exactly one vkQueueSubmit.
int  vk2_sub_commit(void);
// 取了槽但不想提交（探针的对照臂用）：命令缓冲不提交、槽立刻放回（不计数到 commits/submits 上）。
void vk2_sub_abort(void);

// 等**指定 frame** 的栅栏（定向、带超时）。1 = 成功（**含"这一 frame 早就完成"**）、0 = 失败（超时 / 没有在飞）。
// Wait on the fence of one specific frame (timeout reported as failure); 1 = done, 0 = failed.
int  vk2_sub_wait(uint64_t frame);
// 等**所有在飞槽**的栅栏（**不是等队列空闲**）：逐个定向栅栏，任一超时 ⇒ 报失败。返回 1 = 全部完成。
// Wait on every in-flight slot's fence (never on queue idle); any timeout is a failure.
int  vk2_sub_wait_all(void);
uint64_t vk2_sub_frame(void);      // 当前正在录制的 frame（begin 之后、commit/abort 之前；否则 0）
int  vk2_sub_pending(void);        // 当前在飞槽数（瞬时读数，不进计数）| in-flight slots right now
// 指定 frame 的栅栏（**只在它在飞时有效**；N4 用它喂 `vk2_readback_after(fence)`）。取不到给 VK_NULL_HANDLE。
// The fence of one frame, valid while it is in flight (N4 feeds it to vk2_readback_after).
VkFence vk2_sub_fence(uint64_t frame);
// 标记"当前这个槽里录了 N2 的上传窗口"：该槽被回收时本模块自动 `vk2_xfer_window_done()` 把环交还。
// Mark the slot being recorded as carrying an N2 upload window; the ring is handed back when the slot is reaped.
void vk2_sub_note_upload(void);

// ---------------------------------------------------------------- N4：环按槽分
// 把暂存环切成"每个槽一段"（`vk2_xfer_set_banks(槽数)`），并让第 i 个槽用第 i 段 —— 这样**在飞深度 3 时
// 三个槽可以同时载着各自的上传窗口**（N3 量出来的限制：单环会把上传与在飞深度串行化）。
// 缺省**不调用** ⇒ 所有槽都用第 0 段（= N3 的一模一样的行为，N3 的回归读数因此不变）。
// 返回实际段数；0 = 没分成（xfer 没就绪 / 有段被占着）。
// N4: split the ring per slot so that several in-flight slots can each carry an upload window (N3's limit was that
// a single ring serialised uploads against the in-flight depth). Not calling it keeps every slot on bank 0.
int  vk2_sub_use_banks(void);
int  vk2_sub_slot_bank(int i);     // 第 i 个槽用哪一段（只读，诊断用）| which bank slot i uses

// ---------------------------------------------------------------- N4-c：回放层怎么挂上来
// ① **槽回收钩子**：本模块**观察到**某个槽的栅栏已经 signal（取槽前的轮询 / `vk2_sub_wait` / `wait_all` / 收尾）
//    时，先调一次钩子，**然后**才让那个槽可以被重用。回放层需要它做三件事，顺序都不能错：
//      · 把"GPU 完成后才生效"的副作用做掉（事件写值 / 计数器 / 未对齐拷贝的 CPU memcpy）；
//      · **把这次提交 GPU 直写过后备的那几段回读到影子**（N4-c 的往返型：旧路 GPU 写的就是池内存本身、
//        CPU 后读得到；新地基下写落在后备资源上 ⇒ 不回读就是永远读到旧值）；
//      · 重置这个槽的描述符池、读回它的时间戳。
//    **为什么必须是钩子而不是自己轮询**：栅栏句柄会被下一个提交 `vkResetFences` 重用 ⇒ "事后拿 frame 去问
//    栅栏"在槽已被回收之后没有意义（栅栏说的已经是新提交的事）。所以"谁看到栅栏 signal"这件事必须由本模块
//    在**回收那一刻**通知出去。钩子里**不许**再调本模块的接口（会重入），只许录自己的命令缓冲 / 提交别的东西。
// ② **队列锁钩子**：客体线程的提交与前端线程的上屏 / 回读共用同一条队列（见 vk_device.h 的 vk_queue_lock），
//    回放层把它设进来 ⇒ 本模块的 `vkQueueSubmit` 与旧路一样在锁里（探针不设，保持独立可跑）。
// ③ `vk2_sub_poll(frame)`：非阻塞地问"这个 frame 完成了没"（`mtl_wait(fence, 0)` 要的就是它）。
// N4-c: how the replay layer hangs itself on. A reclaim hook (called the moment this module observes a slot's fence,
// before the slot may be reused -- a fence handle is reset by the next submit, so asking after the fact is meaningless,
// and the hook is where the round-trip readback of GPU-written ranges happens); a queue-lock hook (the guest's submits
// share the queue with the front end, so this module takes the same lock the old road did); and a non-blocking poll.
typedef void (*vk2_sub_reap_fn)(int slot, uint64_t frame, VkFence fence, void *ud);
void vk2_sub_set_reap_hook(vk2_sub_reap_fn fn, void *ud);
void vk2_sub_set_queue_lock(void (*lock)(void), void (*unlock)(void));
uint64_t vk2_sub_last_frame(void);            // 最近一次**成功**提交的 frame 号（回放层的 g_fence 用它）| last committed frame
int  vk2_sub_poll(uint64_t frame);            // 1 = 已完成（顺手回收）；0 = 还在飞；-1 = frame 不合法
// **按调用方给的超时**等指定 frame 的栅栏（N4-c：客体自己的 `nvnSyncWait` 有它自己的超时语义 ——
// 客体说等多久就等多久，**不许被模块的 5 秒上限截短**；超时照旧如实报失败）。1 = 完成、0 = 超时/失败。
// Wait on one frame's fence with **the caller's timeout** (the guest's own nvnSyncWait timeout must not be
// shortened by this module's 5 s cap); a timeout is reported as failure.
int  vk2_sub_wait_timeout(uint64_t frame, uint64_t timeout_ns);
uint64_t vk2_sub_user_timeouts(void);         // 按调用方超时等、且真的超时了的次数 | user-timeout waits that expired
int  vk2_sub_slot_id(const vk2_slot *s);      // 这个槽在表里的下标（回放层按槽索引自己的 per-slot 状态）

// ---------------------------------------------------------------- 读数与日志
void vk2_sub_stats_get(vk2_sub_stats *out);
void vk2_sub_stats_reset(void);
uint64_t vk2_sub_idle_calls(void);   // G4 的硬判据（恒 0）| G4's hard criterion (always zero)
void vk2_sub_set_log(void (*fn)(const char *fmt, ...));

#endif // DM_VK2_SUB_H
