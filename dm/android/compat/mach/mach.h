// 安卓上没有 Mach：给宿主的 mach 调用提供最小替身。
// 覆盖范围（按宿主实际用法）：任务信息 vm_read_overwrite/mach_time、线程句柄与线程状态读取、
// 线程信息（CPU 时间）与线程策略。这些在宿主里主要服务于**诊断与性能统计**，
// 返回默认值不影响正确性（真需要精确值时再补，见文档待办）。
// Mach does not exist on Android: minimal stand-ins for the host's mach calls.
// Coverage (by actual host use): task info, vm_read_overwrite / mach_time, thread handles and
// thread-state reads, thread info (CPU time) and thread policies. These serve mainly *diagnostics
// and profiling* in the host, so default values do not affect correctness (follow-up: fill in real
// values when genuinely needed).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <pthread.h>
#include "../dm_android_compat.h"

typedef int mach_port_t;
typedef int kern_return_t;
typedef int vm_map_t;
typedef int32_t integer_t;
typedef uintptr_t vm_address_t;
typedef uintptr_t vm_size_t;
typedef uint64_t mach_vm_address_t;
typedef uint64_t mach_vm_size_t;
typedef uint64_t mach_vm_offset_t;
typedef uint32_t vm_prot_t;
typedef unsigned int mach_msg_type_number_t;
typedef int boolean_t;
typedef mach_port_t thread_act_t;
typedef thread_act_t *thread_act_array_t;
typedef integer_t *thread_policy_t;
typedef integer_t *thread_info_t;
typedef integer_t *thread_state_t;
typedef integer_t *task_info_t;

#define KERN_SUCCESS 0
#define KERN_INVALID_ADDRESS 1
#define KERN_FAILURE 5
#define MACH_PORT_NULL 0
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_FLAGS_ANYWHERE 1
#define VM_INHERIT_NONE 0

// ---- 任务信息 / task info ----
typedef struct { uint64_t virtual_size, resident_size, resident_size_max; uint64_t phys_footprint; } task_vm_info_data_t;
#define TASK_VM_INFO 0
#define TASK_VM_INFO_COUNT (sizeof(task_vm_info_data_t) / sizeof(integer_t))
#define TASK_BASIC_INFO 1
#define TASK_BASIC_INFO_COUNT 0

// ---- 线程信息 / thread info ----
typedef struct { integer_t seconds, microseconds; } time_value_t;
typedef struct {
    time_value_t user_time, system_time;
    integer_t cpu_usage, policy, run_state, flags, suspend_count;
    integer_t sleep_time;
} thread_basic_info_data_t;
typedef struct {
    uint64_t pth_user_time, pth_system_time;
    int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time;
    int32_t pth_curpri, pth_priority, pth_maxpriority;
    char pth_name[64];
} thread_extended_info_data_t;
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT (sizeof(thread_basic_info_data_t) / sizeof(integer_t))
#define THREAD_EXTENDED_INFO 7
#define THREAD_EXTENDED_INFO_COUNT (sizeof(thread_extended_info_data_t) / sizeof(integer_t))

// ---- 线程策略 / thread policies ----
typedef struct { uint32_t affinity_tag; } thread_affinity_policy_data_t;
typedef struct {
    uint32_t period, computation, constraint, preemptible;
} thread_time_constraint_policy_data_t;
#define THREAD_AFFINITY_POLICY 4
#define THREAD_AFFINITY_TAG_NULL 0
#define THREAD_STANDARD_POLICY 1
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT (sizeof(thread_time_constraint_policy_data_t) / sizeof(integer_t))

// ---- AArch64 线程状态 / AArch64 thread state ----
// Apple 用 __darwin_arm_thread_state64_* 访问寄存器；安卓这边同名字段，语义一致。
// Apple accesses registers via __darwin_arm_thread_state64_*; same fields here, same meaning.
typedef struct {
    uint64_t __x[29];
    uint64_t __fp, __lr, __sp, __pc;
    uint32_t __cpsr, __pad;
} arm_thread_state64_t;
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT (sizeof(arm_thread_state64_t) / sizeof(integer_t))
#define __darwin_arm_thread_state64_get_pc(st) ((st).__pc)
#define __darwin_arm_thread_state64_get_fp(st) ((st).__fp)
#define __darwin_arm_thread_state64_get_lr(st) ((st).__lr)
#define __darwin_arm_thread_state64_get_sp(st) ((st).__sp)

static inline mach_port_t mach_task_self(void) { return 0; }
static inline mach_port_t mach_thread_self(void) { return (mach_port_t)(uintptr_t)pthread_self(); }
// 安卓上不需要 mach 线程句柄：直接用 pthread_t 的整数值当"线程号"（只做区分用）
// No mach thread port on Android: use the integer value of pthread_t as the "thread id" (for identity only)
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { return (mach_port_t)(uintptr_t)t; }

// mach_wait_until(deadline)：deadline 是 mach_absolute_time 的绝对值（我们按纳秒给）
// mach_wait_until(deadline): deadline is an absolute mach_absolute_time value (nanoseconds here)
static inline kern_return_t mach_wait_until(uint64_t deadline) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (deadline <= now) return KERN_SUCCESS;
    uint64_t d = deadline - now;
    struct timespec req = { (time_t)(d / 1000000000ull), (long)(d % 1000000000ull) };
    while (nanosleep(&req, &req) != 0 && errno == EINTR) { }
    return KERN_SUCCESS;
}

static inline kern_return_t task_info(mach_port_t t, int flavor, void *info, mach_msg_type_number_t *count) {
    (void)t; (void)flavor;
    if (info && count) memset(info, 0, sizeof(task_vm_info_data_t));
    return KERN_SUCCESS;
}
static inline kern_return_t task_threads(mach_port_t t, void **list, mach_msg_type_number_t *count) {
    (void)t; if (list) *list = 0; if (count) *count = 0;
    return KERN_SUCCESS;
}
static inline kern_return_t thread_info(mach_port_t t, int flavor, thread_info_t info, mach_msg_type_number_t *count) {
    (void)t; (void)flavor;
    if (info && count) memset(info, 0, sizeof(thread_basic_info_data_t));
    return KERN_SUCCESS;
}
static inline kern_return_t thread_get_state(thread_act_t t, int flavor, thread_state_t st, mach_msg_type_number_t *count) {
    (void)t; (void)flavor;
    if (st && count) memset(st, 0, sizeof(arm_thread_state64_t));
    return KERN_SUCCESS;
}
static inline kern_return_t thread_suspend(thread_act_t t) { (void)t; return KERN_SUCCESS; }
static inline kern_return_t thread_resume(thread_act_t t) { (void)t; return KERN_SUCCESS; }
static inline kern_return_t thread_policy_set(mach_port_t t, int flavor, void *policy_info, unsigned count) {
    (void)t; (void)flavor; (void)policy_info; (void)count;
    return KERN_SUCCESS;
}
// Apple 的 thread_policy_get 有第 5 个参数：默认值（def = 1 表示该策略没在生效）
// Apple's thread_policy_get takes a fifth argument: the default value (def = 1 means the policy is not in effect)
static inline kern_return_t thread_policy_get(mach_port_t t, int flavor, void *policy_info, unsigned *count, boolean_t *is_default) {
    (void)t; (void)flavor; (void)policy_info;
    if (count) *count = 0;
    if (is_default) *is_default = 1;   // 安卓不设实时策略：恒报「未生效」| Android sets no real-time policy: always report "not in effect"
    return KERN_SUCCESS;
}
static inline kern_return_t mach_port_deallocate(mach_port_t t, mach_port_t p) { (void)t; (void)p; return KERN_SUCCESS; }

// 读自身进程内存 = memcpy / reading our own process memory = memcpy
static inline kern_return_t vm_read_overwrite(vm_map_t task, vm_address_t address, vm_size_t size,
                                              vm_address_t data, vm_size_t *out_size) {
    (void)task;
    if (out_size) *out_size = 0;
    if (!address || !data || !size) return KERN_INVALID_ADDRESS;
    memcpy((void *)(uintptr_t)data, (const void *)(uintptr_t)address, (size_t)size);
    if (out_size) *out_size = size;
    return KERN_SUCCESS;
}
static inline kern_return_t mach_vm_read_overwrite(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size,
                                                   mach_vm_address_t data, mach_vm_size_t *out_size) {
    vm_size_t got = 0;
    kern_return_t r = vm_read_overwrite(task, (vm_address_t)address, (vm_size_t)size, (vm_address_t)data, &got);
    if (out_size) *out_size = got;
    return r;
}
static inline kern_return_t vm_deallocate(vm_map_t task, vm_address_t address, vm_size_t size) {
    (void)task; (void)address; (void)size;
    return KERN_SUCCESS;
}
