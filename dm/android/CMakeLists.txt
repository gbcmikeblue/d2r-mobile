# 安卓宿主构建（M1）：把 dm/host 的平台无关源码编成静态库 libdmhost_android.a。
# 兼容层 dm/android/compat 用 -include 强制包含，宿主源码本身只保留少量 __ANDROID__ 分支。
# Android host build (M1): compiles the platform-independent sources of dm/host into the static
# library libdmhost_android.a. The compat layer in dm/android/compat is force-included, so the host
# sources carry only a few __ANDROID__ branches.
#
# 用法 / Usage (NDK toolchain):
#   cmake -S dm/android -B build/android -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31
#   ninja -C build/android
cmake_minimum_required(VERSION 3.22)
project(dmhost_android C ASM)

if(NOT ANDROID)
  message(FATAL_ERROR "这个工程只用于安卓构建：请用 NDK 工具链配置（-DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake）| This project is for Android builds only: configure with the NDK toolchain (-DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake)")
endif()

set(DM ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(COMPAT ${CMAKE_CURRENT_SOURCE_DIR}/compat)

# 宿主：所有 .c（hle_*.c 由通配覆盖），排除平台专有的 touch_mac.c
# （iOS 的触摸覆盖层在 dm/ios，安卓要另写，见 docs/ANDROID_PLAN）
# Host: every .c (hle_*.c covered by the wildcard), excluding the platform-specific touch_mac.c
# (the iOS touch overlay lives in dm/ios and Android gets its own; see docs/ANDROID_PLAN)
file(GLOB HOST_SRC ${DM}/host/*.c)
list(REMOVE_ITEM HOST_SRC ${DM}/host/touch_mac.c)

# 图形层：.c 与 .S（.m 是 Metal 专有，M2 才换成 Vulkan）
# Graphics: only .c and .S (.m is Metal-specific and gets replaced by Vulkan in M2)
file(GLOB GFX_SRC ${DM}/gfx/*.c ${DM}/gfx/nvn/*.c ${DM}/gfx/tex/*.c)
file(GLOB GFX_ASM ${DM}/gfx/nvn/*.S)

# 与 dm/host/Makefile 一致的 5 个解压源 / the same 5 decompression sources as dm/host/Makefile
set(LIBDEFLATE
  ${DM}/third_party/libdeflate/lib/deflate_decompress.c
  ${DM}/third_party/libdeflate/lib/zlib_decompress.c
  ${DM}/third_party/libdeflate/lib/adler32.c
  ${DM}/third_party/libdeflate/lib/utils.c
  ${DM}/third_party/libdeflate/lib/arm/cpu_features.c)

add_library(dmhost_android STATIC
  ${COMPAT}/dm_android_compat.c
  ${HOST_SRC}
  ${DM}/host/entry.S
  ${GFX_SRC}
  ${GFX_ASM}
  ${LIBDEFLATE}
  ${DM}/ios/Sources/dm_touch_controls.c)

target_include_directories(dmhost_android PRIVATE
  ${DM}/host
  ${DM}/gfx/nvn/include
  ${COMPAT})

# -include 只强加给 C（汇编文件不能吃 -include 头文件）
# -include only applies to C (assembly cannot take -include of a header)
target_compile_options(dmhost_android PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-include>
  $<$<COMPILE_LANGUAGE:C>:${COMPAT}/dm_android_compat.h>)

# 待办：M1 期间先关掉警告，先把"能编能跑"打通；M1 验收后打开并逐项处理。
# Follow-up: warnings are off during M1 so that "it builds and runs" comes first; turn them on and
# work through them after M1's acceptance.
target_compile_options(dmhost_android PRIVATE -w)
