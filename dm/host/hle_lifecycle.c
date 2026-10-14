// 宿主生命周期：优雅退出（SIGTERM；iOS 由 App 终止回调调 dm_host_shutdown）。
// 停后台线程（启动块预取）、暂停音频、图形挂起（等在途 NVN 调用结束并清空 GPU 工作），再 _exit(0)；1.5 秒兜底直接 _exit(0)。
// 缺省动作同样约 0.08 秒退出（退出码 143）；本函数的价值是有序停后台线程、退出码 0、给 iOS 生命周期回调一个入口。
// Host lifecycle: graceful exit (SIGTERM; on iOS the App termination callback calls dm_host_shutdown).
// Stop background threads (boot block prefetch), pause audio, suspend graphics (wait for in-flight NVN calls to end and drain GPU work), then _exit(0); a 1.5 s fallback goes straight to _exit(0).
// The default action also exits in about 0.08 s (exit code 143); the value of this function is stopping background threads in order, exit code 0, and giving the iOS lifecycle callback an entry.
#include "hle.h"
#if !defined(__ANDROID__)
#include <dispatch/dispatch.h>
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
void tact_prefetch_stop(void);
void granny_prof_dump(void);
void hlecount_flush(void);
void dm_host_shutdown(const char *why);
#if defined(__ANDROID__)
// 安卓没有 GCD，用两条普通线程顶上：一条做「1.5 秒兜底」，一条用 sigwait 收 SIGTERM。
// Android has no GCD, so two ordinary threads stand in: one for the 1.5-second fallback, one that
// sigwaits for SIGTERM.
static void *dm_shutdown_watchdog(void *p) {
    (void)p;
    struct timespec req = { 1, 500000000L };
    nanosleep(&req, NULL);
    LOG("【红】优雅退出 1.5 秒未完成，直接退出"); _exit(0);
    return NULL;
}
static void *dm_term_thread(void *p) {
    (void)p;
    sigset_t set; sigemptyset(&set); sigaddset(&set, SIGTERM);
    for (;;) { int sig = 0; if (sigwait(&set, &sig) == 0 && sig == SIGTERM) dm_host_shutdown("SIGTERM"); }
    return NULL;
}
#endif
void dm_host_shutdown(const char *why) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    uint64_t t0 = now_ns(); LOG("收到 %s：优雅退出", why);
#if defined(__ANDROID__)
    { pthread_t t; if (pthread_create(&t, NULL, dm_shutdown_watchdog, NULL) == 0) pthread_detach(t); }
#else
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        LOG("【红】优雅退出 1.5 秒未完成，直接退出"); _exit(0); });
#endif
    tact_prefetch_stop();
    granny_prof_dump();
    hlecount_flush();                               // DM_HLECOUNT：_exit 不走 atexit，这里写一份 | DM_HLECOUNT: _exit does not run atexit, write a copy here
    dm_audio_suspend(1);
    dm_gfx_suspend(1);
    LOG("优雅退出完成（%.3f 秒）", (now_ns() - t0) / 1e9);
    _exit(0);
}
void dm_term_install(void) {
#if defined(__ANDROID__)
    // 不走缺省动作：屏蔽 SIGTERM，交给专门的线程在普通线程上收尾（与 GCD 信号源同一个思路）
    // Do not take the default action: block SIGTERM and let a dedicated thread wrap up on an ordinary
    // thread (the same idea as the GCD signal source)
    { sigset_t set; sigemptyset(&set); sigaddset(&set, SIGTERM);
      pthread_sigmask(SIG_BLOCK, &set, NULL);
      static pthread_t th;
      if (pthread_create(&th, NULL, dm_term_thread, NULL) == 0) pthread_detach(th); }
#else
    signal(SIGTERM, SIG_IGN);                       // 不走缺省动作：交给 GCD 信号源在普通线程上收尾 | do not take the default action: hand over to the GCD signal source to wrap up on an ordinary thread
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler(src, ^{ dm_host_shutdown("SIGTERM"); });
    dispatch_resume(src);
#endif
}
