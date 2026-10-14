// Apple 的线程 QoS（pthread/qos.h）在安卓上没有：给 nop 替身。
// 宿主用途：给线程标优先级类别，属性能提示，不参与正确性。
// Apple thread QoS (pthread/qos.h) does not exist on Android: no-op stand-ins.
// Host use: tag a thread's priority class; a performance hint only, no bearing on correctness.
#pragma once

#include <pthread.h>

typedef unsigned int qos_class_t;

#define QOS_CLASS_USER_INTERACTIVE 0x21
#define QOS_CLASS_USER_INITIATED 0x19
#define QOS_CLASS_DEFAULT 0x15
#define QOS_CLASS_UTILITY 0x11
#define QOS_CLASS_BACKGROUND 0x09
#define QOS_CLASS_UNSPECIFIED 0x00

static inline int pthread_set_qos_class_self_np(qos_class_t qos_class, int relative_priority) {
    (void)qos_class; (void)relative_priority;
    return 0;
}
static inline int pthread_set_qos_class_np(pthread_t thread, qos_class_t qos_class, int relative_priority) {
    (void)thread; (void)qos_class; (void)relative_priority;
    return 0;
}
static inline int pthread_get_qos_class_np(pthread_t thread, qos_class_t *qos_class, int *relative_priority) {
    (void)thread;
    if (qos_class) *qos_class = QOS_CLASS_DEFAULT;
    if (relative_priority) *relative_priority = 0;
    return 0;
}
