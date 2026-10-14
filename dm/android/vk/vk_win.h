// 窗口/交换链/上屏（M3）：让宿主这一个 Vulkan 设备也能把画面上到手机屏上。
// Window, swapchain and present (M3): let the host's single Vulkan device also put a picture on the phone screen.
//
// 为什么放在宿主侧、而不是像第一版那样在 app 里自建一套：**窗口与游戏必须共用同一个设备** —— 两个设备之间传不了
// 纹理，而"窗口显示游戏画出来的那张贴图"正是这里要做的事（M3 的贴图查看器）。所以这一层只做三件事：从
// `vk_device_set_window()` 给过的窗口建交换链、把**主机内存里的 RGBA8 一帧**送上去、以及拆掉。
// Why this lives on the host side instead of the app building its own set as the first version did: **the window and
// the game have to share one device** -- textures cannot cross between two devices, and "the window shows the texture
// the game drew" is exactly what this is for (M3's texture viewer). So this layer does three things: build a swapchain
// on the window handed to `vk_device_set_window()`, send **one frame of host-memory RGBA8** up, and tear down.
#pragma once
#include <stdint.h>

// 建交换链。要在 vk_device_set_window() 与 vk_device_init() **之后**调用（前者的窗口决定了实例/设备的扩展，
// 后者才是设备本身）。返回 0 = 建不出来（原因已打日志）。
// Build the swapchain. Call it **after** vk_device_set_window() and vk_device_init() (the former decides the instance
// and device extensions, the latter is the device itself). Returns 0 when it cannot be built (the reason is logged).
int vk_win_boot(void);

int vk_win_live(void);          // 交换链可用吗 | is the swapchain usable
void vk_win_shutdown(void);     // 拆掉（TERM_WINDOW / 退出时）；设备本身不在这里拆 | tear down (on TERM_WINDOW / exit); the device itself is not destroyed here

// 把主机内存里的 RGBA8 一帧显示出去：**保持比例缩放并居中**（多出来的地方留黑边），然后上屏。
// 缩放是有意的 —— 游戏那些画面目标多半不是屏幕尺寸（实测 1600×900 对 1156×2510），裁切会看不全。
// 返回 0 = 这一帧没上成（交换链过期等），调用方下一帧重试即可。
// Put one frame of host-memory RGBA8 on screen: **scaled to fit, aspect preserved, centred** (the remainder is black),
// then presented. Scaling is deliberate -- the game's targets are usually not screen sized (1600x900 against
// 1156x2510 on the device) and cropping would not show the whole picture. Returns 0 when this frame did not make it
// (an out-of-date swapchain and the like); the caller simply retries next frame.
int vk_win_present_rgba(const uint8_t *rgba, uint32_t w, uint32_t h);

// 窗口坐标 → 源图坐标：把上屏那套"保持比例居中"的变换反过来用，供触摸命中之用（点在哪块像素上）。
// 返回 1 = 落在图像里（黑边之内为 0）。
// Window coordinates -> source image coordinates: the inverse of the fit-and-centre transform used when presenting,
// for mapping a touch to a pixel. Returns 1 when the point falls inside the image (0 out in the black bars).
int vk_win_window_to_src(float wx, float wy, uint32_t w, uint32_t h, int *ix, int *iy);
