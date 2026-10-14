# 安卓窗口工程（M3）的构建：一个 NativeActivity 的 .so，名叫 dm（清单里 android.app.lib_name=dm）。
# Build of the Android window project (M3): one NativeActivity .so named dm (the manifest's android.app.lib_name=dm).
#
# 与 dm/android 那个工程的关系（这一版变了）：窗口**不再自建 Vulkan**，而是链上宿主那套（`dmhost_android`）——
# 因为**窗口与游戏必须共用同一个设备**，否则纹理传不过去，"窗口显示游戏画出来的那张贴图"就无从谈起。于是
# dm/android 作为子目录加进来，本工程直接链它的静态库；设备由 vk_device.c 建（窗口在 init 之前交给它），交换链与
# 上屏由 vk_win.c 管，本工程只画一帧主机内存里的 RGBA8 再交给它。
# How this relates to the dm/android project (changed in this version): the window **no longer builds its own Vulkan**
# but links the host's (`dmhost_android`) -- because **the window and the game have to share one device**, or textures
# cannot cross and "the window shows the texture the game drew" is impossible. So dm/android comes in as a subdirectory
# and this project links its static library; the device is built by vk_device.c (the window is handed to it before init),
# the swapchain and present belong to vk_win.c, and this project only paints one frame of host-memory RGBA8 and hands it
# over.
cmake_minimum_required(VERSION 3.22)
project(dm_apk C)

if(NOT ANDROID)
  message(FATAL_ERROR "这个工程只用于安卓：请用 NDK 工具链配置（-DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake）| This project is for Android only: configure with the NDK toolchain (-DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake)")
endif()

# 宿主那套（设备、内存池、回放层、窗口模块）
# The host side (device, memory pools, replay layer, window module)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/dmhost)

# app_glue 是 NDK 自带的：它提供 ANativeActivity_onCreate，并在主线程上调 android_main
# app_glue ships with the NDK: it provides ANativeActivity_onCreate and calls android_main on the main thread
add_library(dm SHARED
  ${CMAKE_CURRENT_SOURCE_DIR}/main_apk.c
  ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

target_include_directories(dm PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../vk          # vk_device.h / vk_win.h
  ${CMAKE_CURRENT_SOURCE_DIR}/..             # 宿主侧的头（dm.h 等）| host-side headers (dm.h and friends)
  ${ANDROID_NDK}/sources/android/native_app_glue)

# 注意：**不给**本工程加 dm_android_compat 的 -include —— 那是宿主源码（假扮苹果/Unix 接口）用的，
# app 这一侧是干净的安卓代码，只吃 NDK 头。
# Note: the dm_android_compat force-include is deliberately **not** added here -- that one is for the host sources
# (which stand in for Apple/Unix interfaces); the app side is clean Android code using only NDK headers.
target_link_libraries(dm PRIVATE dmhost_android vulkan android log)
