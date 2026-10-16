// 新地基 N3：帧批提交 + 有界在飞的实现（规格与"与 N2 搬运窗口的关系"见 vk2_sub.h 的文件头）。
// N3 part three: frame-batch submission with bounded in-flight work (see vk2_sub.h for the spec notes).
#define _GNU_SOURCE 1
#include "vk2_sub.h"

#include "vk2_xfer.h"   // 槽回收时要把 N2 的上传窗口交还（vk2_xfer_window_done）| recycling hands the ring back

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void def_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
static void (*g_log)(const char *fmt, ...) = def_log;
void vk2_sub_set_log(void (*fn)(const char *fmt, ...)) { g_log = fn ? fn : def_log; }
#define XLOG(...) g_log(__VA_ARGS__)

static uint64_t now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

// ---------------------------------------------------------------- 状态
static vk2_dev       g_dev;
static int           g_ready;
static int           g_n;            // 在飞深度（槽数）| in-flight depth
static vk2_slot     *g_slots;
static vk2_slot     *g_rec;          // 正在录制的槽（begin 之后、commit/abort 之前）| slot being recorded
static uint64_t      g_frame_seq;    // 客体提交序号（1 起）| guest submission sequence, from 1
static uint64_t      g_last_frame;   // 最近一次成功提交的 frame | the frame of the most recent successful commit
static vk2_sub_stats g_st;
// N4-c：回放层挂上来的两个钩子（见 vk2_sub.h 的说明）| N4-c: the two hooks the replay layer installs
static vk2_sub_reap_fn g_reap_fn;
static void           *g_reap_ud;
static void          (*g_qlock)(void);
static void          (*g_qunlock)(void);

// ---------------------------------------------------------------- G4 的硬判据（不许 vkQueueWaitIdle）
// 与 N2 的 `vk2_xfer.c` 同一套纪律手法：把 `vkQueueWaitIdle` 这个名字在本编译单元里宏成「计数 + 报红」的包装，
// 于是「手滑写上一句」这件事**不可能静默**（G4 的判据就是这个计数恒为 0）。
// The same discipline as vk2_xfer.c: the name vkQueueWaitIdle becomes a counter plus a red log inside this
// translation unit, so a slip cannot silently wait for the queue (G4's criterion is that the count stays zero).
static VkResult vk2_sub_idle_forbidden(VkQueue q, int line);
#define vkQueueWaitIdle(q) vk2_sub_idle_forbidden((q), __LINE__)

static VkResult vk2_sub_idle_forbidden(VkQueue q, int line) {
    (void)q;
    g_st.idle_calls++;
    XLOG("【红】vk2_sub 里出现了 vkQueueWaitIdle（第 %d 行）：G4 要求这个计数恒为 0 —— 只准等定向栅栏", line);
    return VK_ERROR_DEVICE_LOST;
}

int  vk2_sub_ready(void)    { return g_ready; }
int  vk2_sub_inflight(void) { return g_n; }
uint64_t vk2_sub_idle_calls(void) { return g_st.idle_calls; }
void vk2_sub_stats_get(vk2_sub_stats *out) { if (out) *out = g_st; }
void vk2_sub_stats_reset(void) { memset(&g_st, 0, sizeof g_st); }

uint64_t vk2_sub_frame(void) { return g_rec ? g_rec->frame : 0; }

int vk2_sub_pending(void) {
    int n = 0;
    for (int i = 0; i < g_n; i++) if (g_slots[i].in_flight) n++;
    return n;
}

static vk2_slot *find_in_flight(uint64_t frame) {
    for (int i = 0; i < g_n; i++)
        if (g_slots[i].in_flight && g_slots[i].frame == frame) return &g_slots[i];
    return NULL;
}

static vk2_slot *oldest_in_flight(void) {
    vk2_slot *best = NULL;
    for (int i = 0; i < g_n; i++) {
        if (!g_slots[i].in_flight) continue;
        if (!best || g_slots[i].frame < best->frame) best = &g_slots[i];
    }
    return best;
}

// ---------------------------------------------------------------- 回收（槽的存活期在这里结束）
// 回收 = 槽的栅栏已经被观察到：① 槽可以再用；② 如果它载着 N2 的上传窗口，**把环交还**给搬运窗口
// （环只有在"录它的那次提交确实做完了"之后才能被下一窗口复用）。这条不变式写在 vk2_sub.h 的文件头。
// Reaping a slot: the fence has been observed, so the slot is reusable -- and if it carried an N2 upload window
// the ring is handed back only now (the window may not outlive the slot that recorded it).
static void slot_reap(vk2_slot *s) {
    if (!s->in_flight) return;
    // N4-c：**先把"这个槽做完了"这件事通知出去**，然后才让槽可重用。栅栏句柄会被下一个提交 `vkResetFences`
    // 重用 ⇒ 回收之后再拿 frame 去问栅栏没有意义；所以回放层的三件事（副作用 / GPU→池回读 / 描述符池）
    // 只能在这里做，而且必须在这一刻做。
    // N4-c: announce "this slot finished" first, then let it be reused -- the fence handle is reset by the next
    // submit, so the replay layer's work (post ops, round-trip readback, descriptor pool) can only happen here.
    uint64_t frame = s->frame;
    VkFence  fen = s->fence;
    if (g_reap_fn) { g_st.reap_hook_calls++; g_reap_fn((int)(s - g_slots), frame, fen, g_reap_ud); }
    s->in_flight = 0;
    s->frame = 0;
    g_st.reclaims++;
    if (s->has_upload) {
        s->has_upload = 0;
        g_st.upload_recycled++;
        // 环的窗口必须交还，否则下一个窗口会被 vk2_upload 拒绝（"上一个窗口还没收尾"）。
        // N4：交还的是**这一槽自己那一段**（`vk2_sub_use_banks()` 没调时 bank = 0 = N3 的行为）。
        // The window must be handed back, and N4 hands back the slot's own bank (bank 0 unless use_banks was called).
        if (vk2_xfer_ready()) vk2_xfer_window_done_bank(s->bank);
        else XLOG("【黄】这一槽载着上传窗口，但 vk2_xfer 没就绪 ⇒ 环的窗口没人交还（调用方要自己 vk2_xfer_window_done）");
    }
}

// 把 frame ≤ upto 的、栅栏已经 signal 的槽收掉（**单队列 ⇒ 栅栏按提交序 signal**，所以不会漏）。
static void reap_done_up_to(uint64_t upto) {
    for (int i = 0; i < g_n; i++) {
        vk2_slot *s = &g_slots[i];
        if (!s->in_flight || s->frame > upto) continue;
        if (vkGetFenceStatus(g_dev.dev, s->fence) == VK_SUCCESS) slot_reap(s);
    }
}

// 等一个槽的栅栏（定向、带超时）。`where` 只用于日志/计数分组 | wait one fence with a timeout
static int wait_fence(vk2_slot *s, const char *where, int group /* 0=begin 1=wait 2=wait_all */) {
    uint64_t t0 = now_us();
    VkResult r = vkWaitForFences(g_dev.dev, 1, &s->fence, VK_TRUE, (uint64_t)VK2_SUB_TIMEOUT_MS * 1000000ull);
    uint64_t dt = now_us() - t0;
    if (group == 0) {
        g_st.waits++;
        g_st.wait_us += dt;
        if (dt > g_st.wait_max_us) g_st.wait_max_us = dt;
        if (dt > VK2_SUB_LONG_WAIT_US) {
            g_st.long_waits++;
            if (dt > g_st.long_max_us) g_st.long_max_us = dt;
            if (g_st.long_waits <= 8)
                XLOG("【黄】取槽等最老那个槽（frame %llu）等了 %llu µs（> %llu µs 记一笔长等）",
                     (unsigned long long)s->frame, (unsigned long long)dt, (unsigned long long)VK2_SUB_LONG_WAIT_US);
        }
    } else if (group == 1) {
        g_st.wait1_calls++;
        g_st.wait1_waited++;
        g_st.wait1_us += dt;
        if (dt > g_st.wait1_max_us) g_st.wait1_max_us = dt;
    } else {
        g_st.wait_all_waited++;
        g_st.wait_all_us += dt;
        if (dt > g_st.wait_all_max_us) g_st.wait_all_max_us = dt;
        if (dt > VK2_SUB_LONG_WAIT_US) g_st.wait_all_long++;
    }
    if (r != VK_SUCCESS) {
        g_st.timeout_fails++;
        XLOG("【红】%s：等 frame %llu 的栅栏超时（%u ms）：VkResult %d —— **如实报失败，不重试、不放宽**",
             where, (unsigned long long)s->frame, (unsigned)VK2_SUB_TIMEOUT_MS, (int)r);
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------- 生命周期
int vk2_sub_init(const vk2_dev *d, int inflight) {
    if (!d || !d->dev || !d->q) { XLOG("【红】vk2_sub_init：设备事实不全（要 dev + q）"); return 0; }
    vk2_sub_fini();
    g_dev = *d;
    g_n = inflight > 0 ? inflight : VK2_INFLIGHT_DEFAULT;
    if (g_n > VK2_INFLIGHT_MAX) g_n = VK2_INFLIGHT_MAX;
    g_slots = (vk2_slot *)calloc((size_t)g_n, sizeof *g_slots);
    if (!g_slots) { XLOG("【红】槽表分配失败（%d 个槽）", g_n); return 0; }
    for (int i = 0; i < g_n; i++) {
        vk2_slot *s = &g_slots[i];
        VkCommandPoolCreateInfo pci;
        memset(&pci, 0, sizeof pci);
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        // 这条 **必须** 有（N0 踩过：不加时"某一条之后 vkQueueSubmit 开始返回 -3"，看着像驱动的锅）
        // Required (N0's bug: without it vkQueueSubmit started returning -3 after some point).
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = g_dev.qfi;
        if (vkCreateCommandPool(g_dev.dev, &pci, NULL, &s->pool) != VK_SUCCESS) {
            XLOG("【红】第 %d 个槽的命令池建不出来", i);
            vk2_sub_fini(); return 0;
        }
        VkCommandBufferAllocateInfo cbai;
        memset(&cbai, 0, sizeof cbai);
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = s->pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(g_dev.dev, &cbai, &s->cb) != VK_SUCCESS) {
            XLOG("【红】第 %d 个槽的命令缓冲拿不到", i);
            vk2_sub_fini(); return 0;
        }
        VkFenceCreateInfo fci;
        memset(&fci, 0, sizeof fci);
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;   // **不带 SIGNALED**：槽一开始就是"空"的
        if (vkCreateFence(g_dev.dev, &fci, NULL, &s->fence) != VK_SUCCESS) {
            XLOG("【红】第 %d 个槽的栅栏建不出来", i);
            vk2_sub_fini(); return 0;
        }
        s->in_flight = 0; s->has_upload = 0; s->bank = 0; s->frame = 0; s->uses = 0;
    }
    g_rec = NULL;
    g_frame_seq = 0;
    g_last_frame = 0;
    vk2_sub_stats_reset();
    g_ready = 1;
    XLOG("提交槽就绪：**在飞深度 %d**（一槽一池一 cb 一栅栏）、超时 %u ms、长等门槛 %llu µs、"
         "**只等定向栅栏**（`vkQueueWaitIdle` 在本文件里被宏成计数 + 报红）",
         g_n, (unsigned)VK2_SUB_TIMEOUT_MS, (unsigned long long)VK2_SUB_LONG_WAIT_US);
    return 1;
}

void vk2_sub_fini(void) {
    if (!g_dev.dev) { g_ready = 0; return; }
    if (g_rec) {
        XLOG("【黄】收尾时还有一条 cb 在录（既没 commit 也没 abort）⇒ 放弃它");
        g_rec = NULL;
    }
    int pend = vk2_sub_pending();
    if (pend) {
        XLOG("【黄】收尾时还有 %d 个槽在飞 ⇒ 先把它们等回来（带超时，不是等队列空闲）", pend);
        if (!vk2_sub_wait_all()) XLOG("【红】收尾没等回全部在飞槽 —— 如实报（栅栏可能还没 signal，槽照样销毁）");
    }
    for (int i = 0; i < g_n; i++) {
        vk2_slot *s = &g_slots[i];
        if (s->fence) vkDestroyFence(g_dev.dev, s->fence, NULL);
        if (s->pool)  vkDestroyCommandPool(g_dev.dev, s->pool, NULL);   // 池销毁会连 cb 一起收
        s->fence = VK_NULL_HANDLE; s->pool = VK_NULL_HANDLE; s->cb = VK_NULL_HANDLE;
    }
    free(g_slots);
    g_slots = NULL;
    g_n = 0;
    g_ready = 0;
    memset(&g_dev, 0, sizeof g_dev);
}

// ---------------------------------------------------------------- 取槽 / 提交（§9.6）
vk2_slot *vk2_sub_begin(void) {
    if (!g_ready) { XLOG("【红】vk2_sub_begin：模块没就绪"); return NULL; }
    if (g_rec) {
        XLOG("【红】vk2_sub_begin：上一次取的槽（frame %llu）还在录 —— 要么 commit 要么 abort",
             (unsigned long long)g_rec->frame);
        return NULL;
    }
    // ① 先问一遍"谁已经好了"：`vkGetFenceStatus` **不是等待**（与下面的等待分开计数）
    //    First ask which slots are already done: a poll is not a wait, and the two are counted separately.
    for (int i = 0; i < g_n; i++) {
        vk2_slot *s = &g_slots[i];
        if (!s->in_flight) continue;
        if (vkGetFenceStatus(g_dev.dev, s->fence) == VK_SUCCESS) { g_st.poll_reaps++; slot_reap(s); }
    }
    vk2_slot *s = NULL;
    for (int i = 0; i < g_n; i++) if (!g_slots[i].in_flight) { s = &g_slots[i]; break; }
    // ② 还是满的 ⇒ **只等最老那个槽的栅栏**（带超时）；**绝不等队列空闲**
    //    Still full: wait on the oldest slot's fence only, with a timeout -- never on queue idle.
    if (!s) {
        vk2_slot *old = oldest_in_flight();
        if (!old) { XLOG("【红】槽表里既没有空的也没有在飞的（内部状态不一致）"); g_st.no_slot_fail++; return NULL; }
        if (!wait_fence(old, "vk2_sub_begin（等最老那个槽）", 0)) { g_st.no_slot_fail++; return NULL; }
        // 单队列 ⇒ 它完成了意味着比它老的都完成了 | one queue means fences signal in submission order
        if (g_st.reaped_max < old->frame) g_st.reaped_max = old->frame;
        reap_done_up_to(old->frame);
        for (int i = 0; i < g_n; i++) if (!g_slots[i].in_flight) { s = &g_slots[i]; break; }
        if (!s) { XLOG("【红】等到了最老的槽，却还是拿不到空槽（内部状态不一致）"); g_st.no_slot_fail++; return NULL; }
    }
    // ③ 取槽：重置命令缓冲 + 开始录制（池上必须有 RESET 标志，见 init）
    s->frame = ++g_frame_seq;
    s->uses++;
    vkResetCommandBuffer(s->cb, 0);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s->cb, &bi) != VK_SUCCESS) {
        XLOG("【红】frame %llu：命令缓冲开不了录制", (unsigned long long)s->frame);
        return NULL;
    }
    g_rec = s;
    g_st.begins++;
    return s;
}

int vk2_sub_commit(void) {
    if (!g_ready || !g_rec) { XLOG("【红】vk2_sub_commit：没有正在录制的槽"); return 0; }
    vk2_slot *s = g_rec;
    g_st.commits++;                            // 先记"试图提交"；成功才记 submits ⇒ 两者不等就是失败
    if (vkEndCommandBuffer(s->cb) != VK_SUCCESS) {
        XLOG("【红】frame %llu：命令缓冲收不了尾", (unsigned long long)s->frame);
        g_rec = NULL;
        return 0;
    }
    vkResetFences(g_dev.dev, 1, &s->fence);
    VkSubmitInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &s->cb;
    // N4-c：客体线程的提交与前端线程的上屏 / 回读共用同一条队列 ⇒ 提交这一步要串行化（与旧路同一把锁；
    // 探针不设这两个钩子，本模块就还是独立可跑）。
    // N4-c: the guest's submits share one queue with the front end's present/readbacks, so this step is serialised
    // with the same lock the old road takes (probes install no hook, so the module stays standalone).
    if (g_qlock) g_qlock();
    VkResult r = vkQueueSubmit(g_dev.q, 1, &si, s->fence);   // **一次客体提交 = 一次 vkQueueSubmit**
    if (g_qunlock) g_qunlock();
    if (r != VK_SUCCESS) {
        g_st.submit_fail++;
        XLOG("【红】frame %llu：vkQueueSubmit 失败 VkResult %d（commits 与 submits 由此不等）",
             (unsigned long long)s->frame, (int)r);
        g_rec = NULL;
        return 0;
    }
    g_st.submits++;
    s->in_flight = 1;
    g_last_frame = s->frame;
    g_rec = NULL;
    return 1;
}

void vk2_sub_abort(void) {
    if (!g_rec) return;
    g_rec = NULL;                              // 槽立刻放回：下次 begin 会 vkResetCommandBuffer 重录
    g_st.aborts++;
}

// ---------------------------------------------------------------- 定向等待
int vk2_sub_wait(uint64_t frame) {
    if (!g_ready || !frame) { XLOG("【红】vk2_sub_wait：模块没就绪或 frame = 0"); return 0; }
    if (frame <= g_st.reaped_max) return 1;     // 早就完成（单队列 ⇒ 顺序完成）
    vk2_slot *s = find_in_flight(frame);
    if (!s) {
        XLOG("【红】vk2_sub_wait(%llu)：没有这个 frame 在飞（已确认完成的最大 frame = %llu）",
             (unsigned long long)frame, (unsigned long long)g_st.reaped_max);
        return 0;
    }
    if (!wait_fence(s, "vk2_sub_wait", 1)) return 0;
    if (g_st.reaped_max < frame) g_st.reaped_max = frame;
    reap_done_up_to(frame);
    return 1;
}

int vk2_sub_wait_all(void) {
    if (!g_ready) { XLOG("【红】vk2_sub_wait_all：模块没就绪"); return 0; }
    g_st.wait_all_calls++;
    // **等所有在飞槽**（逐个定向栅栏），不是等队列空闲 | wait every in-flight slot's own fence
    for (int i = 0; i < g_n; i++) {
        vk2_slot *s = &g_slots[i];
        if (!s->in_flight) continue;
        if (!wait_fence(s, "vk2_sub_wait_all", 2)) return 0;
        if (g_st.reaped_max < s->frame) g_st.reaped_max = s->frame;
    }
    reap_done_up_to(g_st.reaped_max);
    return 1;
}

VkFence vk2_sub_fence(uint64_t frame) {
    vk2_slot *s = find_in_flight(frame);
    return s ? s->fence : VK_NULL_HANDLE;
}

void vk2_sub_note_upload(void) {
    if (!g_rec) { XLOG("【红】vk2_sub_note_upload：没有正在录制的槽（先 vk2_sub_begin）"); return; }
    g_rec->has_upload = 1;
    g_st.upload_marks++;
}

// ---------------------------------------------------------------- N4：环按槽分
// 让第 i 个槽用暂存环的第 i 段（`vk2_xfer_set_banks`）。**不调用就还是 N3 的单环行为**（所有槽第 0 段）。
// Point each slot at its own ring bank; not calling this keeps the N3 single-ring behaviour.
int vk2_sub_use_banks(void) {
    if (!g_ready) { XLOG("【红】vk2_sub_use_banks：模块没就绪"); return 0; }
    if (!vk2_xfer_ready()) { XLOG("【红】vk2_sub_use_banks：vk2_xfer 没就绪 ⇒ 没有环可分"); return 0; }
    g_st.use_banks_calls++;
    int n = vk2_xfer_set_banks(g_n);
    if (n <= 0) {
        XLOG("【红】vk2_sub_use_banks：分环被拒（多半是有段还被在飞窗口占着）⇒ 所有槽继续共用第 0 段");
        return 0;
    }
    for (int i = 0; i < g_n; i++) g_slots[i].bank = (i < n) ? i : 0;
    g_st.banks = (uint64_t)n;
    XLOG("N4 环按槽分：%d 个槽各用一段（每段 %llu KB）⇒ 上传不再把在飞深度串行化",
         n, (unsigned long long)(vk2_xfer_bank_bytes(0) >> 10));
    return n;
}

int vk2_sub_slot_bank(int i) {
    if (i < 0 || i >= g_n) return -1;
    return g_slots[i].bank;
}

// ---------------------------------------------------------------- N4-c：回放层挂上来的那三件
void vk2_sub_set_reap_hook(vk2_sub_reap_fn fn, void *ud) { g_reap_fn = fn; g_reap_ud = ud; }
void vk2_sub_set_queue_lock(void (*lock)(void), void (*unlock)(void)) { g_qlock = lock; g_qunlock = unlock; }
uint64_t vk2_sub_last_frame(void) { return g_last_frame; }
int vk2_sub_slot_id(const vk2_slot *s) {
    if (!s || !g_slots || s < g_slots || s >= g_slots + g_n) return -1;
    return (int)(s - g_slots);
}

// N4-c：**按调用方给的超时**等一个 frame 的定向栅栏。为什么不能直接用 `vk2_sub_wait()`（它用模块自己的
// 5 秒上限）：客体的 `nvnSyncWait` 有自己的超时语义（"这类提交等多久是客体说了算"），拿模块的上限去截短它
// 就是**改语义**（N4-c 第一轮真机读数抓到的现场：`mtl_wait` 等了 5005 毫秒 —— 正好是模块那个 5 秒 ——
// 而客体要的是它自己给的那个超时）。超时照旧**如实报失败**，不重试、不放宽。
// N4-c: wait with the caller's timeout (the module's fixed 5 s must not shorten the guest's own nvnSyncWait
// timeout -- the first N4-c device round caught exactly that: 5005 ms, the module's cap).
int vk2_sub_wait_timeout(uint64_t frame, uint64_t timeout_ns) {
    if (!g_ready || !frame) return 0;
    if (frame <= g_st.reaped_max) return 1;
    vk2_slot *s = find_in_flight(frame);
    if (!s) return 1;                       // 已经回收 ⇒ 完成（单队列按序）| already reaped means done
    uint64_t t0 = now_us();
    VkResult r = vkWaitForFences(g_dev.dev, 1, &s->fence, VK_TRUE, timeout_ns ? timeout_ns : 1000000ull);
    uint64_t dt = now_us() - t0;
    g_st.wait1_calls++;
    if (r != VK_SUCCESS) {
        g_st.wait1_timeouts++;
        if (g_st.wait1_timeouts <= 8)
            XLOG("【黄】按调用方超时等 frame %llu：等了 %llu µs 没等到（VkResult %d）⇒ 如实报失败",
                 (unsigned long long)frame, (unsigned long long)dt, (int)r);
        return 0;
    }
    g_st.wait1_waited++;
    g_st.wait1_us += dt;
    if (dt > g_st.wait1_max_us) g_st.wait1_max_us = dt;
    if (g_st.reaped_max < frame) g_st.reaped_max = frame;
    reap_done_up_to(frame);
    return 1;
}
uint64_t vk2_sub_user_timeouts(void) { return g_st.wait1_timeouts; }

// 非阻塞地问"这个 frame 完成了没"（`mtl_wait(fence, 0)` 要的就是它）。**这不是等待**；发现完成了就顺手回收
// （回收会走钩子 —— 回放层的副作用与 GPU→池回读都在那里）。返回 1 = 已完成、0 = 还在飞、-1 = frame 不合法。
int vk2_sub_poll(uint64_t frame) {
    if (!g_ready || !frame) return -1;
    if (frame <= g_st.reaped_max) return 1;      // 早就完成（单队列 ⇒ 顺序完成）| long done (one queue, in order)
    vk2_slot *s = find_in_flight(frame);
    if (!s) { g_st.poll_over_deep++; return 1; } // 不在飞 = 已经回收 ⇒ 当作完成 | not in flight means already reaped
    if (vkGetFenceStatus(g_dev.dev, s->fence) != VK_SUCCESS) return 0;
    if (g_st.reaped_max < frame) g_st.reaped_max = frame;
    reap_done_up_to(frame);
    return 1;
}
