// B3 仪器（外部规划 §10.1 第 1 条 / ANDROID_PROGRESS §14 B3）：把"最后一次成功提交 ↔ 第一条 VK_ERROR_DEVICE_LOST"
// 之间那一小段留在日志里 —— **出错的 Vulkan 调用 / 设备信息 / 前序 GPU 操作** 三样齐全。
// B3 instrument (external plan 10.1 item 1 / ANDROID_PROGRESS section 14 B3): keep the short stretch between "the last
// successful submit" and "the first VK_ERROR_DEVICE_LOST" in the log -- the **failing Vulkan call, the device
// information and the preceding GPU operations**, all three.
//
// 为什么要有它：「M3-2y」只量出"**谁先看见** -4"（`vkQueueWaitIdle`），而"**谁弄丢的**"要的是**前序操作序列**；
// 现状是 `-4` 之后"记录并继续"，等于假设原来的资源还能用（plan §10.1 第 6 条）。
// Why: "M3-2y" only measured **who saw it first** (`vkQueueWaitIdle`), while **who dropped it** needs the sequence of
// preceding operations; today the code "logs and continues" after -4, which amounts to assuming the original
// resources are still usable (plan 10.1 item 6).
#pragma once
#include <vulkan/vulkan.h>

// 记一笔**成功**的 GPU 操作（提交 / 呈现 / 等队列空闲）。站点名必须是**常量串**（只存指针、不拷贝）。
// Record one **successful** GPU operation (submit / present / queue idle). The site name must be a **constant string**
// (only the pointer is stored, nothing is copied).
void vk_op_note(const char *site);

// 任何一个 Vulkan 调用返回非 VK_SUCCESS 时调一次：DEVICE_LOST 那一条会把"出错调用 + 设备信息 + 最近 24 笔成功操作"
// 打一次（只打第一条 -4，其后按 32 取模各报一行，免得刷屏）；别的错误码最多报 8 行。
// Call once whenever a Vulkan call returns something other than VK_SUCCESS: DEVICE_LOST prints the failing call, the
// device information and the last 24 successful operations once (only the first -4; afterwards one line every 32nd,
// to avoid flooding). Other error codes are reported at most 8 times in total.
void vk_op_result(const char *call, VkResult r);

// 把流水整段打出来（看门狗 / 按需转储 / 崩溃现场都会走到；幂等，可重复调）。
// Print the whole ledger (the watchdog, an on-demand dump and a crash scene all reach it; idempotent, safe to repeat).
void vk_op_dump(const char *why);
