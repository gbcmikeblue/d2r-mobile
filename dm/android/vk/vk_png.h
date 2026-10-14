// 最小 PNG 写出（8 位 RGBA8，行不过滤、DEFLATE 用存储块即不压缩）。
// 为什么要自带一个：出图截图要落盘成**能直接看的文件**，而安卓侧不能链 Apple 的图像框架；
// 这是一次性调试输出，几百行以内、无依赖、结果任何看图工具都能打开，比引第三方库合适。
// Minimal PNG writer (8-bit RGBA8, no per-row filtering and DEFLATE stored blocks, i.e. no compression).
// Why roll one: a present screenshot has to land on disk as a file that can be **looked at directly**, and Android
// cannot link Apple's imaging frameworks; this is a one-off debugging output where a couple of hundred dependency-free
// lines that any viewer opens beats pulling in a third-party library.
#pragma once
#include <stdint.h>

// 把 w×h 的 RGBA8 像素写成 PNG；成功返回 1。
// Write w x h RGBA8 pixels as a PNG; returns 1 on success.
int vk_png_write(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
