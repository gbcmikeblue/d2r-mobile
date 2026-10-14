// 线程策略的定义已并入 mach/mach.h（兼容层里 mach 相关替身集中放一处）。这里只做转发，
// 让 `#include <mach/thread_policy.h>` 照常可用。
// The thread-policy definitions live in mach/mach.h (the compat layer keeps all mach stand-ins in
// one place). This header just forwards so that `#include <mach/thread_policy.h>` keeps working.
#pragma once
#include "mach.h"
