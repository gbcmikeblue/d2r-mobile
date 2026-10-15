#include <TargetConditionals.h>
// 崩溃现场：打印出错地址、pc、lr（换成模块内偏移）与寄存器；缺页钩子分发。
// 只放信号处理本身：诊断功能（探针 / UI 消息 / 采样）在 diag.c，本文件不依赖它们（图形单测只链本文件）。
// Crash state: prints the faulting address, pc, lr (converted to module-relative offsets) and the registers; dispatches page fault hooks.
// Only signal handling itself lives here: diagnostic features (probes / UI messages / sampling) are in diag.c, and this file does not depend on them (the graphics unit tests link only this file).
#include "dm.h"
#include <signal.h>
#include <dlfcn.h>
#include <sys/ucontext.h>
#include "dm_uctx.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <stdatomic.h>
#include <time.h>

static _Atomic uint64_t g_watch_last_ns;
static _Atomic uint64_t g_watch_count;
static _Atomic uint32_t g_watch_tick;
static _Atomic int g_watch_pregame = 1;
static _Atomic int g_watch_stage_idx;
static char g_watch_stage[2][256] = {"启动", "启动"};
static unsigned g_watch_stall;
static void (*g_dump_hook)(void);
static void (*g_wait_dump)(void);
static void (*g_stall_sample)(void);   // 定义在后面（diag.c 注册）；看门狗要用，所以在这里先声明 | Defined further down (registered by diag.c); declared here because the watchdog uses it

static uint64_t watch_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t crash_watch_progress_count(void) { return atomic_load(&g_watch_count); }
// 当前阶段字符串（M3-2w：屏上那行可读数字要把「卡在哪个界面」写出来，见 dm/android/apk/main_apk.c 的 HUD）。
// The current stage string (M3-2w: the on-screen readout prints which screen we are on; see the HUD in
// dm/android/apk/main_apk.c).
const char *crash_watch_stage(void) { return g_watch_stage[atomic_load(&g_watch_stage_idx)]; }
void crash_watch_progress(uint32_t tick, const char *stage) {
    int next = 1 - atomic_load(&g_watch_stage_idx);
    if (stage && *stage) snprintf(g_watch_stage[next], sizeof g_watch_stage[next], "%s", stage);
    atomic_store(&g_watch_tick, tick);
    atomic_store(&g_watch_stage_idx, next);
    atomic_fetch_add(&g_watch_count, 1);
    atomic_store(&g_watch_last_ns, watch_now_ns());
}
void crash_watch_frame(void) { if (atomic_load(&g_watch_pregame)) crash_watch_progress(0, "出图帧"); }
void crash_watch_pregame_done(void) { atomic_store(&g_watch_pregame, 0); }
void crash_watch_pregame_again(void) { atomic_store(&g_watch_pregame, 1); }   // 从局里退回前端：重新按出图帧算进展（diag.c） | Back from a run to the front end: count progress by presented frames again (diag.c)
void crash_dump_now(const char *why);
static void *watchdog_main(void *arg) {
    (void)arg;
    for (;;) {
        usleep(250000);
        // 先读最后进展时刻、再读现在：反过来读时，两次读之间游戏线程刚好记了一次进展，last 会比 now 新，now − last 无符号下溢成「第 18446744073 秒起无进展」，
        // 当场误判卡死退出（accept inv_drag_out，本机负载 11、看门狗线程两次读之间被切走）。这样读 last ≤ now；万一时钟回绕也不判
        // Read the last progress time first, then read now: reading in the opposite order, if the game thread happens to record progress between the two reads, last is newer than now and now − last underflows as unsigned into 「第 18446744073 秒起无进展」(no progress since second 18446744073),
        // which is misjudged on the spot as a hang and exits (accept inv_drag_out, local load 11, the watchdog thread was switched out between the two reads). Reading this way gives last ≤ now; and if the clock ever wraps around it is not judged either
        uint64_t last = atomic_load(&g_watch_last_ns), now = watch_now_ns();
        if (last > now) continue;
        // 秒级卡顿（进展停了 1 秒以上、还没到判死的阈值）：不退出，把各线程在等什么导一份。撞到「每 2 秒才出一帧」的现场，15 秒阈值抓不到、日志里没有它在等什么。
        // 进局之后才导；每个进程最多 3 份、两份之间至少隔 10 秒（读取画面、暂停菜单也会停拍，别让它们刷屏）
        // Second-scale stall (progress has stopped for more than 1 second but has not yet reached the kill threshold): do not exit, dump what each thread is waiting on. When hitting a case of "only one frame every 2 seconds", the 15-second threshold cannot catch it and the log has nothing on what it is waiting for.
        // Dump only after entering a run; at most 3 dumps per process, at least 10 seconds between two dumps (the loading screen and the pause menu also stop ticks, do not let them flood the log)
        { static int dumps; static uint64_t last_dump, dumped_for;
          if (g_watch_stall && !atomic_load(&g_watch_pregame) && atomic_load(&g_watch_tick) > 0 && now - last >= 1000000000ull && now - last < (uint64_t)g_watch_stall * 1000000000ull
              && dumps < 3 && dumped_for != last && (!last_dump || now - last_dump >= 10000000000ull)) { dumps++; last_dump = now; dumped_for = last; crash_dump_now("进展停了 1 秒以上"); } }
        if (g_watch_stall && now - last >= (uint64_t)g_watch_stall * 1000000000ull) {
            uint64_t sec = (now - last) / 1000000000ull;
            int idx = atomic_load(&g_watch_stage_idx);
            LOG("【红】看门狗：第 %llu 秒起无进展，卡在 拍 %u / 阶段 %s", (unsigned long long)sec,
                atomic_load(&g_watch_tick), g_watch_stage[idx]);
            if (g_stall_sample) g_stall_sample();   // 先取一次 PC（在转储之前：转储很长，抢在后面容易看不出来）| Take one PC sample first (before the dump: the dump is long and this is easy to lose behind it)
            if (g_dump_hook) g_dump_hook();
            if (g_wait_dump) g_wait_dump();   // 图形侧所有阻塞等待点的状态（nvn_mtl.m / nvn_vk.c 初始化时注册） | State of all blocking wait points on the graphics side (registered when nvn_mtl.m / nvn_vk.c initializes)
            // 退出前给日志一点时间：安卓那边宿主 stderr 是**管道**，落盘由另一条线程（app 的日志泵）做，
            // 而 _exit 不会等它 —— 实测转储的**尾巴**被整段切掉（"等待点"那行正好在尾巴上）。
            // 苹果侧走不到这里（TARGET_OS_OSX 那次 sample 已经等过一秒），所以这只是安卓的坑。
            // Give the log a moment before exiting: on Android the host's stderr is a **pipe** and writing it to the
            // file is done by another thread (the app's log pump), which _exit does not wait for -- measured as the
            // **tail** of the dump being cut off entirely (the "wait points" line sits exactly at the tail). Apple
            // never reaches this (its TARGET_OS_OSX sample already waited a second), so this is an Android-only trap.
#if !TARGET_OS_OSX
            usleep(400000);
#endif
#if TARGET_OS_OSX
            { const char *d = getenv("DM_WATCH_SAMPLE");   // Mac 诊断：卡住时对自己采样 1 秒，线程栈写到该路径 | Mac diagnostics: when stuck, sample itself for 1 second and write the thread stacks to this path
              if (d) { char c[1200]; snprintf(c, sizeof c, "/usr/bin/sample %d 1 -f '%s' >/dev/null 2>&1", (int)getpid(), d); if (system(c)) LOG("看门狗：采样失败"); else LOG("看门狗：线程栈已写 %s", d); } }
#endif
            _exit(128 + SIGALRM);
        }
    }
    return NULL;
}
void crash_off(const char *tag, uint64_t a) {   // diag.c 也用 | Also used by diag.c
#define off crash_off

    // M3-2w：先剥掉可能的 PAC 高位 —— Android arm64 的返回地址带指针签名，带签名位的地址 `dladdr` 一律失败，
    // 于是在回溯里显示成"一串看起来像垃圾的裸地址"（`0x24c6fca47ce57c` 这种形状，实测）。用户空间地址只用低 48 位，
    // 所以剥掉高 16 位对正常地址没有副作用，却能把这一批帧救回来 —— 「M3-2v」那个说不通的 `mtl_wait+0x214` 就属于这批。
    // M3-2w: strip the possible PAC high bits first -- Android arm64 return addresses carry pointer signatures, and an
    // address with signature bits always fails `dladdr`, showing up in a backtrace as "a bare address that looks like
    // garbage" (measured in the shape `0x24c6fca47ce57c`). User-space addresses use only the low 48 bits, so clearing
    // the top 16 harms nothing for a normal address and rescues this whole batch of frames -- including "M3-2v"'s
    // unexplainable `mtl_wait+0x214`.
    a &= 0x0000ffffffffffffull;
    module_t *m = module_of((void *)a);
    Dl_info di;
    if (m) fprintf(stderr, " %s%s%s+%#llx", tag, *tag ? "=" : "", m->name, a - (uint64_t)m->base);
    else if (dladdr((void *)a, &di) && di.dli_sname) fprintf(stderr, " %s%s%s+%#lx", tag, *tag ? "=" : "", di.dli_sname, (unsigned long)(a - (uint64_t)di.dli_saddr));
    // 没有符号名的动态库（安卓厂商驱动正是如此：dladdr 只给路径）也要报出**哪个 .so + 偏移**——一串裸地址
    // 分不清是驱动、libc 还是我们自己（真机上 "pc=0x79b6…" 这种就是它）。
    // A dynamic library without symbols (Android vendor drivers are exactly like that: dladdr gives only the path) must
    // still report **which .so plus the offset** -- a bare address does not tell the driver from libc from our own code
    // (the "pc=0x79b6..." lines on the device are this case).
    else if (dladdr((void *)a, &di) && di.dli_fname) {
        const char *b = strrchr(di.dli_fname, '/'); b = b ? b + 1 : di.dli_fname;
        fprintf(stderr, " %s%s%s+%#llx", tag, *tag ? "=" : "", b, (unsigned long long)(a - (uint64_t)di.dli_fbase));
    }
    else fprintf(stderr, " %s%s%#llx", tag, *tag ? "=" : "", a);
}
// 帧指针回溯：x29 链 [fp]=上一帧 fp、[fp+8]=返回地址；只走 8 字节对齐、单调上升的帧
// Frame pointer backtrace: x29 chain [fp]=previous frame's fp, [fp+8]=return address; only 8-byte aligned, monotonically increasing frames are walked
void dm_backtrace(uint64_t fp) {
    fprintf(stderr, "[dm] bt:");
    for (int i = 0; i < 32 && fp && !(fp & 7); i++) {
        uint64_t nfp = ((uint64_t *)fp)[0], ra = ((uint64_t *)fp)[1];
        off("", ra);
        if (nfp <= fp) break;
        fp = nfp;
    }
    fputc('\n', stderr);
}
// ---------- 缺页钩子（写保护脏页跟踪） ----------
// on_crash 打印现场前按注册顺序调用；返回 1 = 已处理（直接返回、重执行出错指令），全部返回 0 才走崩溃路径。
// 钩子运行在信号处理上下文：只许查表 / 原子操作 / mprotect，不许分配内存、加锁。
// ---------- Page fault hooks (write-protect dirty page tracking) ----------
// Called in registration order before on_crash prints the crash state; returning 1 = handled (return directly and re-execute the faulting instruction); the crash path is taken only when all return 0.
// Hooks run in signal handler context: only table lookups / atomic operations / mprotect are allowed, no memory allocation and no locking.
#define MAX_FAULT_HOOK 4
static int (*g_fault_hook[MAX_FAULT_HOOK])(void *addr, int is_write, void *uc);
static void (*g_write_prep[MAX_FAULT_HOOK])(void *addr, size_t len);
static int g_nfault_hook, g_nwrite_prep;
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) {
    if (g_nfault_hook >= MAX_FAULT_HOOK) die("缺页钩子过多");
    g_fault_hook[g_nfault_hook++] = fn;
}
static void (*g_res_report)(uint32_t tick, const char *why);   // 资源计数回调（图形层注册，diag.c 按拍 / 事件调用） | Resource count callback (registered by the graphics layer, called by diag.c per tick / event)
void dm_res_report_register(void (*fn)(uint32_t tick, const char *why)) { g_res_report = fn; }
void (*dm_res_report_cb(void))(uint32_t, const char *) { return g_res_report; }
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) {
    if (g_nwrite_prep >= MAX_FAULT_HOOK) die("宿主写前钩子过多");
    g_write_prep[g_nwrite_prep++] = fn;
}
// 宿主自己写客体内存（read() 直读进客体缓冲等）之前调用：写保护页不会对系统调用触发缺页、而是返回 EFAULT
// Called before the host itself writes guest memory (read() straight into a guest buffer, etc.): for a system call a write-protected page does not trigger a page fault but returns EFAULT
void dm_guest_write_prepare(void *addr, size_t len) { for (int i = 0; i < g_nwrite_prep; i++) g_write_prep[i](addr, len); }

// 诊断模块（diag.c 的探针）与线程现场（hle_thread.c）由各自注册进来：单测只链本文件时不依赖它们
// The diagnostics module (the probes in diag.c) and the thread state dump (hle_thread.c) each register themselves here: a unit test that links only this file does not depend on them
static int (*g_trap_hook)(ucontext_t *uc);
void crash_set_trap_hook(int (*fn)(ucontext_t *uc)) { g_trap_hook = fn; }
void crash_set_dump_hook(void (*fn)(void)) { g_dump_hook = fn; }
void crash_set_wait_dump(void (*fn)(void)) { g_wait_dump = fn; }
// 卡住时的一次性采样（diag.c 注册）：给"在用户态空转"的线程各发一次信号、把 pc 取出来。
// 为什么单独一个钩子：它的**时机**很讲究——要在"已经判定卡住"之后、转储之前跑，且只能跑一次；
// 常开的 DM_SAMPLE 采样在安卓上会落在 svc 里（见 dialg.c 里那段实测说明），不能代替它。
// One-shot sampling when stalled (registered by diag.c): send each thread spinning in user mode one signal and read its pc.
// Why a separate hook: its **timing** matters -- it has to run after "stalled" has been decided and before the dumps,
// and only once; the always-on DM_SAMPLE sampler lands on an svc on Android (see the measurements in diag.c) and cannot
// replace it.
void crash_set_stall_sample(void (*fn)(void)) { g_stall_sample = fn; }
// ---------- M3-2w：宿主侧"等待站点"账本（谁在等什么，一次点名）----------
// 「M3-2v」把"停住"缩到"客体主线程卡在宿主侧一次 nanosleep / usleep 轮询里"，宿主链是
//   `nanosleep ← usleep ← … ← nvnSyncWait（hle_nvn.c）← mtl_wait ← 客体 main+0x1743a0c`
// 但链上属于我们的等待点有好几处（`mtl_wait` 的 timeout 分支、`inflight_take` 等槽、`nvnSyncWait`、
// `SleepThread` 的睡眠循环、等「读取画面已撤」的循环…），光看回溯分不出**是哪一处**。这里给它们一本统一的账本：
//   ① 每个站点进入 / 离开各记一笔（站点名 + 进入时刻），超过 300 毫秒立刻打一行日志 —— 真卡死时 leave 永远不会
//      执行，日志尾巴就是唯一的线索；
//   ② 转储（看门狗 / dump.trigger）里列出所有线程"当前打开的站点"与已经等了多久 —— 一次就能点名。
// 为什么**放在本文件**：它必须能被**任何**链接组合取到（gfx 的单测只链 crash.c + 图形层，见 dm/gfx/tests/Makefile），
// 而本文件本来就自带时间源（watch_now_ns）且不依赖诊断模块 —— 这与"crash.c 只管信号处理、单测只链本文件"的
// 既有约束一致。
// 为什么**不加锁**：它自己在**客体线程上**被调用，加锁等于把一个新的等待引进被测路径（「M3-2n」的锁序反转就是这么来的）。
// 所以每个线程一个按 tid 散列的固定槽、用原子占位；读者最坏看到"站点名是旧的"，不会崩、不会等。
// 只记前 WLED_DEPTH 层嵌套，更深的层只计数（站点名照旧是常量字符串，不在热路径上分配）。
// ---------- M3-2w: the host-side "wait sites" ledger (who waits where, named in one round) ----------
// "M3-2v" narrowed the stall to "the guest main thread stuck in a host-side nanosleep / usleep poll" with the host
// chain `nanosleep <- usleep <- ... <- nvnSyncWait (hle_nvn.c) <- mtl_wait <- guest main+0x1743a0c`, but several of the
// waits on that chain are ours (the timeout branch of `mtl_wait`, `inflight_take` waiting for a slot, `nvnSyncWait`, the
// `SleepThread` sleep loop, the "loading screen dismissed" loop ...) and the backtrace alone cannot say **which**.
// So they share one ledger: every site records an enter/leave pair (site name + entry time) and
//   (1) logs a line past 300 ms -- when it really hangs, the leave never runs and the log tail is the only clue;
//   (2) the dump (watchdog / dump.trigger) lists the sites every thread currently has open and how long it has waited,
//       which names it in one round.
// Why **in this file**: it has to be reachable from **every** link combination (the gfx unit tests link only crash.c
// plus the graphics layer, see dm/gfx/tests/Makefile), and this file already carries its own time source
// (watch_now_ns) with no dependency on the diagnostics module -- consistent with the existing constraint that
// "crash.c only handles signals and the unit tests link only this file".
// Why **no lock**: this runs **on guest threads**, and adding a lock would introduce a new wait into the very path
// being measured (that is how the lock-order inversion of "M3-2n" happened). Each thread gets a fixed slot keyed by
// tid, claimed atomically; the worst a reader sees is a stale site name -- no hang, no crash.
// Only the first WLED_DEPTH nesting levels are recorded; deeper ones are merely counted (site names are constant
// strings, so nothing is allocated on the hot path).
#define WLED_SLOTS 64
#define WLED_DEPTH 6
#define WLED_LOG_MS 300
typedef struct {
    _Atomic int tid;                 // 0 = 空闲槽 | 0 = free slot
    int depth;                       // 打开了几层（可能大于 WLED_DEPTH）| how many levels are open (may exceed WLED_DEPTH)
    struct { const char *site; uint64_t since; } st[WLED_DEPTH];
    uint64_t last_ms; const char *last_site;   // 最近一次"等完"的读数 | the most recent completed wait
    uint64_t last_at;                          // 那次"等完"的时刻（"它是多久以前的事"本身就是线索）| when that wait completed ("how long ago" is itself a clue)
    // 自检用：这个槽第一次被占用时的客体线程句柄（宿主线程为 0）与累计进入次数。
    // 为什么需要：转储里"某个线程不在账上"必须能分清两种情况 —— ① 它压根没走过这些站点；② 账本把它漏了。
    // 没有这两项就只能猜（而"账本漏了"恰恰会让一次点名得出相反的结论）。
    // Self-check: the guest thread handle at first use (0 for host threads) and the cumulative entry count. Why: "a
    // thread is not on the books" must be distinguishable between (1) it never walked these sites and (2) the ledger
    // dropped it -- without these two, that can only be guessed (and a dropped entry would flip a naming conclusion).
    _Atomic int handle;
    uint64_t n_enter;
} wled_t;
static wled_t g_wled[WLED_SLOTS];
static int g_wled_on = -1;
static int wled_on(void) {
    if (g_wled_on < 0) { const char *e = getenv("DM_WAITLED"); g_wled_on = !(e && *e && atoi(e) == 0); }
    return g_wled_on;
}
#if defined(__ANDROID__)
static int wled_tid(void) { return (int)pthread_gettid_np(pthread_self()); }
#else
static int wled_tid(void) { return (int)(uintptr_t)pthread_self(); }   // 苹果：pthread_t 本身就是标识 | Apple: the pthread_t itself is the identity
#endif
static wled_t *wled_slot(void) {
    int tid = wled_tid();
    unsigned base = ((unsigned)tid * 2654435761u) % WLED_SLOTS;
    for (unsigned k = 0; k < WLED_SLOTS; k++) {
        wled_t *s = &g_wled[(base + k) % WLED_SLOTS];
        if (atomic_load_explicit(&s->tid, memory_order_relaxed) == tid) return s;
    }
    for (unsigned k = 0; k < WLED_SLOTS; k++) {
        wled_t *s = &g_wled[(base + k) % WLED_SLOTS];
        int expect = 0;
        if (atomic_compare_exchange_strong(&s->tid, &expect, tid)) {
            s->depth = 0; s->last_ms = 0; s->last_site = NULL; s->n_enter = 0;
            for (int i = 0; i < WLED_DEPTH; i++) s->st[i].site = NULL;
            return s;
        }
    }
    return NULL;   // 槽用满（64 条线程同时等在站点上）：宁可不记也不等 | all slots busy (64 threads waiting at once): better to skip than to wait
}
// 进入一个"可能久等"的站点。site 必须是常量字符串（或至少活得比等待长）—— 账本只存指针，不拷贝。
// Enter a site that may wait long. `site` must be a constant string (or at least outlive the wait): the ledger stores
// the pointer, it does not copy.
void dm_waitled_enter(const char *site) {
    if (!wled_on()) return;
    wled_t *s = wled_slot(); if (!s) return;
    uint32_t thr_cur_handle(void);   // 客体线程句柄（hle_thread.c；宿主线程为 0）| guest thread handle (hle_thread.c; 0 for host threads)
    if (!s->n_enter) atomic_store(&s->handle, (int)thr_cur_handle());
    s->n_enter++;
    int d = s->depth;
    if (d < WLED_DEPTH) { s->st[d].site = site; s->st[d].since = watch_now_ns(); }
    s->depth = d + 1;
}
// 离开（同线程严格 LIFO 配对）。超过 300 毫秒打一行：不看转储也能从日志尾巴读出"谁等了多久"。
// Leave (strict LIFO pairing within a thread). Past 300 ms it logs a line, so the log tail alone shows who waited how
// long even without a dump.
void dm_waitled_leave(void) {
    if (!wled_on()) return;
    wled_t *s = wled_slot(); if (!s) return;
    int d = s->depth - 1;
    if (d < 0) { s->depth = 0; return; }
    s->depth = d;
    if (d < WLED_DEPTH && s->st[d].site) {
        uint64_t el = watch_now_ns() - s->st[d].since;
        s->last_ms = el / 1000000; s->last_site = s->st[d].site; s->last_at = watch_now_ns();
        if (el > (uint64_t)WLED_LOG_MS * 1000000ull) {
            if (d > 0 && s->st[d - 1].site)
                LOG("等待站点：%s 等了 %llu 毫秒（外层站点 %s）", s->st[d].site, (unsigned long long)(el / 1000000), s->st[d - 1].site);
            else LOG("等待站点：%s 等了 %llu 毫秒", s->st[d].site, (unsigned long long)(el / 1000000));
        }
        s->st[d].site = NULL;
    }
}
// 停住 / 按需转储时调：列出所有线程"当前打开的站点"（自外向内）与已等多久。真卡死的那条线程一定在这里面。
// Called on a stall / on-demand dump: lists the sites every thread has open right now (outermost first) and how long it
// has been waiting. The thread that really is stuck is guaranteed to be in here.
void dm_waitled_dump(void) {
    if (!wled_on()) { LOG("  等待站点：关（DM_WAITLED=0）"); return; }
    uint64_t now = watch_now_ns();
    int n = 0, used = 0;
    for (int i = 0; i < WLED_SLOTS; i++) {
        wled_t *s = &g_wled[i];
        int tid = atomic_load_explicit(&s->tid, memory_order_relaxed);
        if (!tid) continue;
        used++;
        if (s->depth <= 0) continue;
        n++;
        char nest[512]; int o = 0; nest[0] = 0;
        for (int d = 0; d < WLED_DEPTH && d < s->depth; d++)
            if (s->st[d].site) o += snprintf(nest + o, sizeof nest - o, "%s%s", d ? " > " : "", s->st[d].site);
        LOG("  等待站点：tid %d（客体线程 %#x）已等 %llu 毫秒、%d 层：%s", tid, atomic_load(&s->handle),
            (unsigned long long)((now - s->st[0].since) / 1000000), s->depth, o ? nest : "?");
        if (s->last_site && s->last_ms >= WLED_LOG_MS)
            LOG("  等待站点：tid %d 上一次等完的是「%s」，等了 %llu 毫秒", tid, s->last_site, (unsigned long long)s->last_ms);
    }
    if (!n) LOG("  等待站点：没有线程停在记账的等待里（不在这些站点上等，或它们都是短等）");
    // 自检：账本用过几个槽、各自是谁、进来过几次。**"某个线程不在上面"必须能与"账本漏了它"分开** ——
    // 否则一次点名会把"没走这些站点"误读成"不在等待里"。
    // Self-check: how many slots were ever used, by whom, and how many entries each. "A thread is not listed" must be
    // distinguishable from "the ledger dropped it", or a naming round misreads "never walked these sites" as "not
    // waiting at all".
    LOG("  等待站点：账本用过的槽 %d/%d（DM_WAITLED=0 关）", used, WLED_SLOTS);
    int shown = 0;
    for (int i = 0; i < WLED_SLOTS && shown < 24; i++) {
        wled_t *s = &g_wled[i];
        int tid = atomic_load_explicit(&s->tid, memory_order_relaxed);
        if (!tid || !s->n_enter) continue;
        shown++;
        // 顺便给出"最后等完的是哪个站点"：某条线程"现在 0 层"时，**它上一次是从哪里出来的**就是下一条线索
        //（例如"刚从提交里出来就没再进过任何站点"与"一直在 SleepThread 上"是两种完全不同的处境）。
        // Also give the site it last completed a wait in: when a thread sits at "0 levels", **where it last came out of**
        // is the next clue (e.g. "just came out of a submit and has not entered anything since" and "has been on
        // SleepThread all along" are two very different situations).
        char tail[192] = "";
        if (s->last_site)
            snprintf(tail, sizeof tail, "，最后等完的是「%s」（%llu 毫秒的等，结束于 %llu 毫秒前）", s->last_site,
                     (unsigned long long)s->last_ms, (unsigned long long)((now - s->last_at) / 1000000));
        LOG("  等待站点：账本槽 tid %d = 客体线程 %#x，累计进过 %llu 次（现在 %d 层%s）", tid,
            atomic_load(&s->handle), (unsigned long long)s->n_enter, s->depth, tail);
    }
    if (used > shown) LOG("  等待站点：账本还有 %d 个槽未列出", used - shown);
}

// 按需导出现场（不退出）：iOS 包没有无进展看门狗，冒烟判红时经 DM_LOG_PORT 发 dump 触发——各客体线程在等什么 + 图形侧等待点，和看门狗打的是同一份
// Dump the current state on demand (without exiting): the iOS app has no no-progress watchdog, so when the smoke test fails it sends dump through DM_LOG_PORT to trigger this -- what each guest thread is waiting on + the graphics-side wait points, the same thing the watchdog prints
void crash_dump_now(const char *why) { fprintf(stderr, "[dm] 现场导出（%s）：拍 %u / 阶段 %s\n", why, atomic_load(&g_watch_tick), g_watch_stage[atomic_load(&g_watch_stage_idx)]); if (g_stall_sample) g_stall_sample(); dm_waitled_dump(); if (g_dump_hook) g_dump_hook(); if (g_wait_dump) g_wait_dump(); }
// 崩在客体地址上时，把 /proc/self/maps 里覆盖该地址的那一行打出来。
// 理由：安卓没有 ESR 的读写位（见 dm_uctx.h），光看寄存器分不清「这一页根本没映射」还是「这一页被写保护」——
// 两者的修法完全不同（前者是宿主没铺到那页，后者是脏页跟踪的缺页钩子没接住），所以现场必须带上这一行。
// 只用 open / read：信号上下文里 malloc 可能正好是崩在锁上的那个线程持有的锁，fopen 会死等。
// On a guest-address fault, print the /proc/self/maps entry covering it.
// Why: Android has no ESR read/write bit (see dm_uctx.h), and the registers alone cannot tell "this page is not
// mapped at all" from "this page is write-protected" -- and the two have completely different fixes (the
// former means the host never laid the page out, the latter that the dirty-page fault hook did not catch it),
// so the report has to carry this line.
// Only open / read are used: inside a signal handler malloc's lock may be held by the very thread that
// crashed, and fopen would wait forever on it.
// 打印覆盖 a 的那一行，外加前后各一行：只给一行分不清「这一页没映射」和「这一页在某个大段的边界上」，
// 而边界正是栈出问题的典型形态（栈往下长、撞到下一段的边界）。
// Prints the entry covering a plus the one before and the one after: a single line cannot tell "this page is
// not mapped" from "this page sits on the boundary of a larger region", and boundaries are exactly what a
// stack problem looks like (the stack grows down and hits the next region's edge).
// 读入 /proc/self/maps 的全文（静态缓冲，信号上下文里不分配）；返回读到的字节数。
// Read all of /proc/self/maps into a static buffer (no allocation in signal context); returns the byte count.
static size_t maps_read(char *buf, size_t cap) {
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) { buf[0] = 0; return 0; }
    size_t got = 0;
    for (;;) {
        if (got >= cap - 1) break;
        ssize_t n = read(fd, buf + got, cap - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    buf[got] = 0;
    return got;
}

// 落在客体地址空间里的全部映射（最多列 24 条）：栈上到底是什么、哪一段是谁铺的，一次看完。
// Every mapping inside the guest address space (at most 24 lines): what is really on the stack and who laid
// each part out, in one go.
static size_t maps_in_as(uint8_t *as, size_t asz, char *out, size_t cap) {
    static char buf[1 << 20];
    size_t got = maps_read(buf, sizeof buf), k = 0;
    unsigned long ab = (unsigned long)(uintptr_t)as, ae = ab + asz;
    int shown = 0, more = 0;
    k += (size_t)snprintf(out + k, cap - k, "[dm] 客体 AS 内的映射（读入 %zu 字节）:\n", got);
    for (char *p = buf; p && *p; ) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        unsigned long lo = 0, hi = 0;
        if (sscanf(p, "%lx-%lx", &lo, &hi) == 2 && lo < ae && hi > ab) {
            if (shown < 24) {
                k += (size_t)snprintf(out + k, cap - k, "   +%+#llx +%#llx  %s\n",
                                      (long long)(lo - ab), (unsigned long long)(hi - lo), p);
                shown++;
            } else more++;
        }
        if (!e) break;
        p = e + 1;
    }
    if (!shown) k += (size_t)snprintf(out + k, cap - k, "   （一条也没有）\n");
    else if (more) k += (size_t)snprintf(out + k, cap - k, "   …另有 %d 条未列出\n", more);
    return k;
}

static void maps_window(uint64_t a, char *out, size_t cap) {
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) { snprintf(out, cap, "（打不开 /proc/self/maps）"); return; }
    static char buf[1 << 20];                       // 静态缓冲：信号上下文里不能分配。1 MB：256 KB 装不下整个 maps 时，
                                                    // pc 那一条会排在栈之后被截掉，现场就变成自相矛盾的两行（实测踩过）
                                                    // Static buffer: no allocation in signal context. 1 MB: with 256 KB the
                                                    // pc entry (which comes after the stack one) got cut off and the report
                                                    // contradicted itself (this really happened)
    size_t got = 0;
    for (;;) {
        if (got >= sizeof buf - 1) break;
        ssize_t n = read(fd, buf + got, sizeof buf - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    buf[got] = 0;
    // 先把行切开（就地改 \n 为 \0）
    char *ln[4096]; int nl = 0;
    for (char *p = buf; p && *p && nl < (int)(sizeof ln / sizeof ln[0]); ) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        ln[nl++] = p;
        if (!e) break;
        p = e + 1;
    }
    int hit = -1;
    for (int i = 0; i < nl; i++) {
        unsigned long lo = 0, hi = 0;
        if (sscanf(ln[i], "%lx-%lx", &lo, &hi) == 2 && a >= lo && a < hi) { hit = i; break; }
    }
    size_t k = 0;
    if (hit < 0) { snprintf(out, cap, "不在任何映射里（这一页没映射；maps 共读入 %zu 字节、%d 行）", got, nl); return; }
    for (int i = hit - 1; i <= hit + 1 && i < nl; i++) {
        if (i < 0) continue;
        k += (size_t)snprintf(out + k, cap - k, "%s%s", i == hit ? ">> " : "   ", ln[i]);
        if (i < hit + 1 && i + 1 < nl) k += (size_t)snprintf(out + k, cap - k, "\n           ");
    }
}
static void on_crash(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    if (sig == SIGTRAP && g_trap_hook && g_trap_hook(uc)) return;
    if (sig == SIGSEGV || sig == SIGBUS) {
#if DM_UC_HAVE_ESR
        int is_write = (int)((uc->uc_mcontext->__es.__esr >> 6) & 1);   // ESR 的 WnR 位 | The WnR bit of ESR
#else
        // 安卓的 sigcontext 没有 ESR：拿不到「读还是写」，先按「读」处理（见 dm_uctx.h）
        // Android's sigcontext carries no ESR: the read/write bit is unavailable, treat it as a read (see dm_uctx.h)
        int is_write = 0;
#endif
        for (int i = 0; i < g_nfault_hook; i++) if (g_fault_hook[i](si->si_addr, is_write, uc)) return;
    }
    fprintf(stderr, "[dm] CRASH sig=%d", sig);
    off("addr", (uint64_t)si->si_addr); off("pc", DM_UC_PC(uc)); off("lr", DM_UC_LR(uc));
    fprintf(stderr, " sp=%#llx\n", DM_UC_SP(uc));
    for (int i = 0; i < 29; i++) fprintf(stderr, "x%d=%#llx%s", i, DM_UC_X(uc, i), i % 6 == 5 ? "\n" : " ");
    fprintf(stderr, "\n");
    // 出错地址、pc、sp 各自落在哪一段、那一段是什么权限（见 maps_line 的说明），以及出错指令的原始编码
    // （安卓拿不到 ESR 的读写位，从编码能看出是 load 还是 store）。
    // 一起拼好、一次 write 出去：崩溃时别的线程还在往 stderr 写，分几次打会交错得读不出来。
    // Which mapping each of the fault address, pc and sp falls into and that mapping's permissions (see
    // maps_line), plus the raw encoding of the faulting instruction (Android has no ESR read/write bit, and the
    // encoding shows whether it is a load or a store).
    // All of it is assembled and written once: other threads are still writing to stderr during a crash, and
    // several separate writes interleave into something unreadable.
    {
        static char mb[24576], t[1024];   // 要装下最多 24 条映射行 | has to hold up to 24 mapping lines
        size_t k = 0;
        maps_window((uint64_t)si->si_addr, t, sizeof t);
        k += (size_t)snprintf(mb + k, sizeof mb - k, "[dm] maps 出错地址: %s\n", t);
        maps_window(DM_UC_PC(uc), t, sizeof t);
        k += (size_t)snprintf(mb + k, sizeof mb - k, "[dm] maps 出错指令: %s\n", t);
        maps_window(DM_UC_SP(uc), t, sizeof t);
        k += (size_t)snprintf(mb + k, sizeof mb - k, "[dm] maps 栈指针: %s\n", t);
        const uint32_t *ip = (const uint32_t *)(uintptr_t)DM_UC_PC(uc);
        k += (size_t)snprintf(mb + k, sizeof mb - k, "[dm] 出错指令编码: %08x %08x（安卓无 ESR，据此判读读/写）\n", ip[0], ip[1]);
        // 三个地址相对客体地址空间的偏移：一眼看出各自落在哪一段（预留段 / alias / 栈 / 堆）。
        // 单看绝对地址看不出这些，而「落在哪一段」正是判断是客体跑野了还是宿主没铺好那页的关键。
        // The three addresses relative to the guest address space: it shows at a glance which part each falls
        // into (reservation / alias / stack / heap). Absolute addresses do not show that, and "which part" is
        // what tells a guest that ran wild from a page the host never laid out.
        { void svc_guest_range(uint8_t **base, size_t *size);
          uint8_t *as = NULL; size_t asz = 0; svc_guest_range(&as, &asz);
          if (as) k += (size_t)snprintf(mb + k, sizeof mb - k,
              "[dm] 相对客体 AS（%p +%#llx）：地址 %+#llx、指令 %+#llx、栈 %+#llx；固定划分 alias +%#x、stack +%#x、heap +%#x\n",
              as, (unsigned long long)asz,
              (long long)((uint64_t)si->si_addr - (uint64_t)(uintptr_t)as),
              (long long)((uint64_t)DM_UC_PC(uc) - (uint64_t)(uintptr_t)as),
              (long long)((uint64_t)DM_UC_SP(uc) - (uint64_t)(uintptr_t)as),
              0x10000000u, 0x30000000u, 0x40000000u);
          // 落到客体 AS 里的所有映射：栈上到底是什么、哪一段是谁铺的，一眼看完（崩溃现场最要紧的一份事实）
          // Every mapping that falls inside the guest address space: what is really on the stack and who laid
          // each part out, all in one place (the single most important fact in a crash report)
          if (as) k += maps_in_as(as, asz, mb + k, sizeof mb - k); }
        if (write(2, mb, k) < 0) { /* 打不出来就算了 | nothing to do if it cannot be written */ }
    }
    dm_backtrace(DM_UC_FP(uc));
    if (g_dump_hook) g_dump_hook();
    _exit(128 + sig);
}
// 信号备用栈是每线程的：每个客体线程起跑时都要调（栈溢出时才能在备用栈上打出现场）
// 备用栈随线程退出释放：pthread 键的析构在线程退出时调用（先停用备用栈再 free）。游戏会频繁起短命线程，不释放每个漏 64 KiB。
// The signal alternate stack is per thread: every guest thread must call this when it starts running (only then can the crash state be printed on the alternate stack on a stack overflow)
// The alternate stack is released when the thread exits: the pthread key destructor is called at thread exit (disable the alternate stack first, then free). The game frequently starts short-lived threads, and without releasing each one leaks 64 KiB.
static pthread_key_t g_altstack_key;
static pthread_once_t g_altstack_once = PTHREAD_ONCE_INIT;
static void altstack_free(void *p) { stack_t off = {.ss_flags = SS_DISABLE}; sigaltstack(&off, 0); free(p); }
static void altstack_key_init(void) { if (pthread_key_create(&g_altstack_key, altstack_free)) die("altstack key"); }
void crash_thread_init(void) {
    pthread_once(&g_altstack_once, altstack_key_init);
    if (pthread_getspecific(g_altstack_key)) return;                 // 同一线程重复调用不重复分配 | Repeated calls on the same thread do not allocate again
    stack_t ss = {.ss_sp = malloc(65536), .ss_size = 65536}; sigaltstack(&ss, 0);
    pthread_setspecific(g_altstack_key, ss.ss_sp);
}
void crash_install(void) {
    crash_thread_init();
    struct sigaction sa = {.sa_sigaction = on_crash, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigaction(SIGALRM, &sa, 0);
    // 两种看门狗都只在测试显式设了 DM_WATCH / DM_WATCH_STALL 时开：产品（玩家从桌面启动，无环境变量）一个不开。
    // 读代码查到：iOS main.m 缺省 setenv DM_WATCH=30 → 玩家启动 30 s 被 SIGALRM 杀；无进展看门狗缺省 15 s → 暂停菜单（拍停）15 s 被杀
    // Both watchdogs are turned on only when a test explicitly sets DM_WATCH / DM_WATCH_STALL: the product (the player launches from the desktop, no environment variables) turns on neither.
    // Found by reading the code: iOS main.m did setenv DM_WATCH=30 by default → the player's launch was killed by SIGALRM at 30 s; the no-progress watchdog defaulted to 15 s → the pause menu (ticks stopped) was killed at 15 s
    const char *watch = getenv("DM_WATCH"), *stall = getenv("DM_WATCH_STALL");
    if (watch) alarm(atoi(watch));
    if (watch || stall) {
        g_watch_stall = stall && atoi(stall) > 0 ? (unsigned)atoi(stall) : 15;
        atomic_store(&g_watch_last_ns, watch_now_ns());
        pthread_t th; if (pthread_create(&th, NULL, watchdog_main, NULL)) die("看门狗线程");
        pthread_detach(th);
    }
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGILL, &sa, 0); sigaction(SIGTRAP, &sa, 0);
}
