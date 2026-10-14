# 安卓窗口最小工程（M3 第一块）的构建：一个 NativeActivity 的 .so，名叫 dm（清单里 android.app.lib_name=dm）。
# Build of the minimal Android window project (the first piece of M3): one NativeActivity .so named dm (the manifest's
# android.app.lib_name=dm).
#
# 与 dm/android 那个工程的关系：那个编的是**控制台宿主**（静态库 + dmhost 可执行文件），跑在 adb shell 里；这个编的是
# **装进手机的应用**。两者现在互不依赖 —— 这一版不跑游戏，只把窗口那条链（APK → ANativeWindow → VkSurface → 交换链
# → 上屏）打通；下一步游戏进 APK 时，这里再链上 libdmhost_android.a，并把窗口做成"贴图查看器"。
# Relation to the dm/android project: that one builds the **console host** (a static library plus the dmhost executable)
# which runs under adb shell; this one builds an app that is **installed on the phone**. The two are independent for now
# -- this version does not run the game, it only gets the window chain (APK -> ANativeWindow -> VkSurface -> swapchain ->
# present) working; when the game moves into the APK, this target links libdmhost_android.a and the window becomes a
# texture viewer.
cmake_minimum_required(VERSION 3.22)
project(dm_apk C)

if(NOT ANDROID)
  message(FATAL_ERROR "这个工程只用于安卓：请用 NDK 工具链配置（-DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake）| This project is for Android only: configure with the NDK toolchain (-DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake)")
endif()

# app_glue 是 NDK 自带的（sources/android/native_app_glue）：它提供 ANativeActivity_onCreate，并在主线程上调 android_main。
# app_glue ships with the NDK (sources/android/native_app_glue): it provides ANativeActivity_onCreate and calls
# android_main on the main thread.
add_library(dm SHARED
  ${CMAKE_CURRENT_SOURCE_DIR}/main_apk.c
  ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

target_include_directories(dm PRIVATE ${ANDROID_NDK}/sources/android/native_app_glue)
target_link_libraries(dm PRIVATE vulkan android log)
