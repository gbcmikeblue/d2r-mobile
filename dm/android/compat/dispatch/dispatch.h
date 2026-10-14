// 安卓没有 GCD（libdispatch）：这里只提供本仓库确实用到的那一小块——信号量。
// 用途：hle_audio.c 里「缓冲被释放」的通知（音频线程触发、普通线程等待）。
// 其它 GCD 设施（dispatch_after / dispatch_source / block）本仓库只在 hle_lifecycle.c 用到，
// 那里已改为安卓专用的线程实现，不再包含本头文件。
// Android has no GCD (libdispatch): this provides only the small piece the repository actually uses,
// the semaphore. Use: the "buffer released" notification in hle_audio.c (signalled by the audio
// thread, waited on by an ordinary thread). The rest of GCD (dispatch_after / dispatch_source /
// blocks) is used only by hle_lifecycle.c, which has an Android thread-based implementation and no
// longer includes this header.
#pragma once

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <semaphore.h>
#include <time.h>

typedef int64_t dispatch_time_t;
#define DISPATCH_TIME_FOREVER INT64_MAX

typedef struct { sem_t s; } dm_dispatch_semaphore_t;
typedef dm_dispatch_semaphore_t *dispatch_semaphore_t;

static inline dispatch_semaphore_t dispatch_semaphore_create(long value) {
    dm_dispatch_semaphore_t *d = calloc(1, sizeof *d);
    if (d && sem_init(&d->s, 0, (unsigned)value) != 0) { free(d); return NULL; }
    return d;
}
static inline long dispatch_semaphore_signal(dispatch_semaphore_t d) {
    if (d) sem_post(&d->s);
    return 0;
}
static inline long dispatch_semaphore_wait(dispatch_semaphore_t d, dispatch_time_t timeout) {
    if (!d) return -1;
    if (timeout == DISPATCH_TIME_FOREVER) {
        while (sem_wait(&d->s) != 0 && errno == EINTR) { }
        return 0;
    }
    // 相对/绝对时间：本仓库只用 FOREVER，这里按"绝对纳秒"实现一个够用的版本
    // Relative/absolute time: the repository only uses FOREVER; this is a good-enough absolute-nanosecond version
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t now_ns = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    int64_t left = timeout - now_ns;
    if (left < 0) left = 0;
    ts.tv_sec += (time_t)(left / 1000000000ll);
    ts.tv_nsec += (long)(left % 1000000000ll);
    if (ts.tv_nsec >= 1000000000l) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000l; }
    while (sem_timedwait(&d->s, &ts) != 0 && errno == EINTR) { }
    return 0;
}
static inline void dispatch_release(dispatch_semaphore_t d) {
    if (d) { sem_destroy(&d->s); free(d); }
}
