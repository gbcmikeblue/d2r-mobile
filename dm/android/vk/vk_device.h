// 安卓图形后端的 Vulkan 设备（进程内只建一次）：回放层与内存池都从这儿取设备、队列与内存类型。
// The Android graphics backend's Vulkan device (created once per process): the replay layer and the memory
// pools take the device, queue and memory types from here.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

// 建设备（幂等）。真机上量出来的能力见 docs/ANDROID_PLAN 的 M2 第 1、3 步：
// Adreno 830 / Vulkan 1.3.284，有 VK_KHR_external_memory_fd，没有 VK_EXT_external_memory_host。
// Create the device (idempotent). The capabilities measured on a real device are in M2 steps 1 and 3 of
// docs/ANDROID_PLAN: Adreno 830 / Vulkan 1.3.284, with VK_KHR_external_memory_fd and without
// VK_EXT_external_memory_host.
void vk_device_init(void);

// 没建起来就返回 VK_NULL_HANDLE | returns VK_NULL_HANDLE when bring-up failed
// 窗口（M3）：装成 APK 之后才有。**必须在 vk_device_init 之前**告诉它 —— 因为"能不能上屏"决定了实例要不要开
// 安卓 surface 扩展、设备要不要开交换链扩展、以及挑哪个队列族（要能呈现）。控制台宿主不调它，一切照旧。
// The window (M3): only exists once installed as an APK. It **must be handed over before vk_device_init** -- whether
// presenting is possible decides if the instance enables the Android surface extension, if the device enables the
// swapchain extension, and which queue family is picked (it has to support presenting). The console host never calls
// this, so nothing changes there.
void vk_device_set_window(void *native_window);   // ANativeWindow*（用 void* 让这个头保持平台无关）| ANativeWindow* (void* keeps this header platform neutral)
void *vk_device_window(void);
void vk_device_make_surface(void);                // 实例已在、窗口刚设好时（重）建 surface | (re)create the surface when the instance exists and a window was just set
void vk_device_drop_surface(void);                // TERM_WINDOW：销毁 surface 并清掉窗口 | TERM_WINDOW: destroy the surface and clear the window
VkSurfaceKHR vk_dev_surface(void);                // 没窗口时为 VK_NULL_HANDLE | VK_NULL_HANDLE when there is no window
int vk_dev_present_family(void);                  // 能呈现的队列族；-1 = 没有 | the queue family that can present; -1 = none

VkInstance vk_instance(void);
VkDevice vk_dev(void);
VkQueue vk_queue(void);
uint32_t vk_queue_family(void);

// 物理设备（没建起来时为 VK_NULL_HANDLE）：查设备限制与格式能力用——目前只有 texel buffer
// （缓冲区贴图）需要它：VkBufferView 的偏移必须落在 minTexelBufferOffsetAlignment 的整数倍上。
// The physical device (VK_NULL_HANDLE when bring-up failed): for querying device limits and format support -- today
// only the texel buffer (a buffer texture) needs it: a VkBufferView's offset must be a multiple of
// minTexelBufferOffsetAlignment.
VkPhysicalDevice vk_phys(void);

// 窗口（前端）那条队列：能呈现的族上多要一条，取不到就退回主队列。
// 为什么要分开：Vulkan 要求同一条队列上的提交/等待/取图/上屏由调用方串行化，而客体（后台线程）与前端（主线程）
// 天然是两个线程。只加一把锁实测会**互相堵死**：客体在队列调用里等驱动时仍持锁，前端就卡在取锁上（真机现场：
// 在用户态空转的线程 pc 在 libc、返回点 = vk_win_present_rgba+0x260，前端的 1 秒心跳同时消失）。
// 分成两条队列后，两边各用各的，谁都不用等谁；只有退回同一队列时才需要那把锁。
// The window (front end) queue: ask for a second one on the presenting family, falling back to the main queue.
// Why separate: Vulkan requires submit / queue-idle waits / image acquisition / present on one queue to be serialised by
// the caller, and the guest (worker thread) and the front end (main thread) are two threads by construction. A single
// lock was measured to **deadlock them**: while the guest waits for the driver inside a queue call it still holds the
// lock, so the front end sits on the lock (device scene: the user-mode spinning thread had its pc in libc with a return
// address of vk_win_present_rgba+0x260, and the front end's one-second heartbeat stopped at the same time). With two
// queues each side uses its own and neither waits for the other; the lock is needed only in the fallback case.
VkQueue vk_queue_win(void);

// 队列的外部同步（说明见上面）：提交 / 等队列空闲 / 取图 / 上屏都走它。
// 调用方在**取锁前**用 vk_queue_note 说明自己要干什么：卡住时现场导出会打出"谁持有、在做什么"。
// External synchronisation for the queue (see above): submit / queue-idle waits / image acquisition / present take it.
// Callers use vk_queue_note **before** locking to say what they are about to do; a stall dump then prints who holds the
// lock and what for.
void vk_queue_lock(void);
void vk_queue_unlock(void);
void vk_queue_note(const char *phase);
int vk_queue_lock_state(char *buf, size_t cap);   // 写一行"持有者 tid / 在做什么"；返回 1 = 有人持有 | Writes one line "holder tid / what for"; 1 = held
const VkPhysicalDeviceMemoryProperties *vk_mem_props(void);

// ---------- 管线缓存（VkPipelineCache，图形与计算共用；说明见 vk_device.c 里那一整段）----------
// 为什么在设备层：它的来历要与设备/驱动核对，落盘路径也要与"哪个进程在跑"有关（APK 只能写 app 内部目录）。
// 用法就是一句话：把 vk_dev_pcache() 传给 vkCreate*Pipelines 的 cache 参数，**外面套一对 lock/unlock**
// （Vulkan 要求对 VkPipelineCache 的访问由调用方串行化，写盘那条路也走同一把锁）。
// ---------- The pipeline cache (shared by graphics and compute; the full note is in vk_device.c) ----------
// Why it lives in the device layer: where the blob came from has to be checked against the device and driver, and the
// path depends on which process is running (the APK may only write its internal directory). In use it is one line:
// hand vk_dev_pcache() to the cache parameter of vkCreate*Pipelines, **wrapped in a lock/unlock pair** (Vulkan requires
// the caller to serialise access to a VkPipelineCache, and the write-back path takes the same lock).
VkPipelineCache vk_dev_pcache(void);             // 设备没起来或没建成为 VK_NULL_HANDLE | VK_NULL_HANDLE when the device is down or creation failed
void vk_dev_pcache_lock(void);
void vk_dev_pcache_unlock(void);
// 切后台 / 退出时写回（先 .tmp 再改名，被 SIGKILL 不会留半截）。可重复调用（第二次起只是再写一遍）。
// Write back on background / exit (via a .tmp file then rename, so a SIGKILL leaves no half-written file). Safe to call
// repeatedly (a second call simply writes again).
void vk_device_pcache_save(const char *why);

// 前台每秒调一次：数据长过就写（限频 10 秒，见 vk_device.c 里那段说明）。**真机上这条是主力**：
// MIUI 把这个包当游戏，退到后台直接 SIGKILL，切后台/退出那两个钩子都不会跑。
// Call it once a second while in the foreground: it writes when the data has grown (rate-limited to 10 seconds; see the
// note in vk_device.c). **On the real device this is the main path**: MIUI treats this package as a game and SIGKILLs it
// on backgrounding, so neither the background nor the exit hook ever runs.
void vk_device_pcache_tick(void);

// 在满足 need_bits 的类型里找属性最贴近 want 的一个；找不到返回 UINT32_MAX。
// Find the type that satisfies need_bits and whose properties best match want; UINT32_MAX when there is none.
uint32_t vk_memory_type(VkMemoryPropertyFlags want, uint32_t need_bits);

// 零拷贝内存类型（DEVICE_LOCAL + HOST_VISIBLE + HOST_COHERENT）：客体内存与 GPU 共用同一份物理内存时用它。
// 真机上是类型 4。找不到返回 UINT32_MAX。
// The zero-copy memory type (DEVICE_LOCAL + HOST_VISIBLE + HOST_COHERENT), used when guest memory and the
// GPU share the same physical memory; it is type 4 on the real device. UINT32_MAX when there is none.
uint32_t vk_zero_copy_type(void);

// 这一段设备是否开了某个扩展 | whether this device enabled a given extension
int vk_have_ext(const char *name);

// 两处"能力缺口"的开关（都是先问再开，没开就由管线层退化并报黄，见 docs/ANDROID_PLAN 第 9 步）：
//   vk_have_fill_non_solid：非实心多边形模式可用（不然只能按实心画）
//   vk_have_attr_divisor：顶点流的每实例步进可大于 1（不然 divisor > 1 只能按 1 处理）
// The two capability switches behind the "known gaps" (both asked for before being enabled; when absent the
// pipeline layer degrades and warns, see step 9 of docs/ANDROID_PLAN):
//   vk_have_fill_non_solid: non-solid polygon modes are usable (otherwise only solid can be drawn)
//   vk_have_attr_divisor: a vertex stream's per-instance step may exceed 1 (otherwise divisor > 1 is treated as 1)
int vk_have_fill_non_solid(void);
int vk_have_attr_divisor(void);
int vk_have_anisotropy(void);          // 各向异性过滤可用（采样器要用）| anisotropic filtering available (needed by samplers)

// GPU 计时（时间戳查询）的两个前置条件：本队列族的时间戳有效位数（0 = 测不了）与每刻多少纳秒。
// Two prerequisites for GPU timing (timestamp queries): this queue family's timestamp valid-bit count (0 means it
// cannot be measured) and how many nanoseconds a tick is.
int vk_ts_bits(void);
double vk_ts_period_ns(void);
int vk_have_attr_divisor_zero(void);   // divisor 可为 0（NVN 的"常量流"正好能用这个表达）| divisor may be 0 (exactly what NVN's "constant stream" needs)

// 动态渲染是否可用（回放层开渲染通道的前提；不可用时那一层退回只记状态）
// Whether dynamic rendering is available (the replay layer's precondition for opening passes; without it that
// layer falls back to recording state only)
int vk_have_dyn_render(void);

// 动态渲染的入口。安卓的 libvulkan.so 是**桩库**（只导出到 Vulkan 1.1），1.3 的入口直接引用会链接失败
// （undefined symbol: vkCmdBeginRendering），必须用 vkGetDeviceProcAddr 取——与取 vkGetMemoryFdKHR 同一个套路。
// 设备没起来或没取到时这两个指针是 NULL，调用方必须先看 vk_have_dyn_render()。
// The dynamic-rendering entry points. Android's libvulkan.so is a **stub** library (it exports up to Vulkan 1.1),
// so referring to a 1.3 entry point directly fails to link (undefined symbol: vkCmdBeginRendering); it has to be
// fetched with vkGetDeviceProcAddr -- the same pattern as vkGetMemoryFdKHR. Both pointers are NULL when the device
// is not up or the lookup failed, so callers must check vk_have_dyn_render() first.
extern PFN_vkCmdBeginRendering dm_vkCmdBeginRendering;
extern PFN_vkCmdEndRendering dm_vkCmdEndRendering;
