// 安卓前端（M1 最小实现）：还没有窗口、也还没有图形后端，所以这里只把 front.h 那一个接口面补齐，
// 客体直接在调用线程上跑；出图交给后续的 Vulkan 后端（M2）。
// Android front end (minimal M1 implementation): there is no window and no graphics backend yet, so
// this fills in the front.h interface only; the guest runs directly on the calling thread and
// presentation is left to the later Vulkan backend (M2).
#include "front.h"
#include "dm_gpu_region.h"
#include "vk/vk_device.h"
#include "vk/vk_region.h"
#include <stdint.h>

// 客体直接在调用线程上跑：安卓这边没有「主线程归系统 UI」的约束——宿主自己就是可执行文件，
// 没有 UI 事件循环要留给主线程。
// The guest runs directly on the calling thread: Android has no "main thread belongs to the system
// UI" constraint here, because the host is the executable itself and there is no UI event loop that
// the main thread must be kept free for.
//
// 跑客体之前先把图形后端备好：Vulkan 设备要在客体第一次陷内核（mem_init 铺地址空间）之前就绪，
// 并把内存换轨登记上去，否则堆与别名区会按普通内存铺下去，池子里的顶点/纹理就上不了 GPU。
// The graphics backend is prepared before the guest runs: the Vulkan device must be up before the guest's
// first kernel trap (mem_init laying out the address space) and the memory overlay must be registered, or
// the heap and alias region would be laid out as ordinary memory and vertex/texture data in a pool could
// never reach the GPU.
void front_run(void (*guest)(void)) {
    vk_device_init();
    dm_gpu_region_use = vk_region_use;
    guest();
}

// 还没有图形后端：返回 NULL，宿主据此走「无窗口」路径（不出图、不弹原生输入框）。
// No graphics backend yet: return NULL and the host takes its "no window" paths (no presentation,
// no native input box).
void *front_metal_layer(void) { return NULL; }

// 游戏输出分辨率：M1 先用原版常数（主机 1600x900 / 掌机 1280x720）；
// M2 拿到真实屏幕比例后再按 front.h 的约定「同高度放宽」。
// Game output resolution: M1 reports the original constants (docked 1600x900 / handheld 1280x720);
// once M2 knows the real screen aspect ratio it widens at the same height, as front.h prescribes.
uint64_t front_game_res(int docked) {
    return docked ? (((uint64_t)900 << 32) | 1600) : (((uint64_t)720 << 32) | 1280);
}

// 无窗口时不弹原生输入框，按 front.h 的约定返回 -1。
// With no window there is no native input box; per front.h this returns -1.
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial,
                     int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap;
    return -1;
}
