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
  # main_android.c 一起编进来：它那份环境准备（DM_ROOT/DM_SAVE/DM_ROMFS、工作目录、argv 形状）**与 APK 必须完全一致**，
  # 所以复用它而不是抄一遍。它在宿主那边是编进可执行文件的（不在静态库里），所以这里单独收一次。
  # main_android.c is compiled in here as well: its environment setup (DM_ROOT/DM_SAVE/DM_ROMFS, working directory,
  # argument shape) **has to be identical for the APK**, so it is reused rather than copied. On the host side it is
  # compiled into the executable (not into the static library), hence this separate mention.
  ${CMAKE_CURRENT_SOURCE_DIR}/../main_android.c
  ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

target_include_directories(dm PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../vk          # vk_device.h / vk_win.h
  ${CMAKE_CURRENT_SOURCE_DIR}/..             # 宿主侧的头（dm.h 等）| host-side headers (dm.h and friends)
  ${ANDROID_NDK}/sources/android/native_app_glue)

# 注意：**不给**本工程加 dm_android_compat 的 -include —— 那是宿主源码（假扮苹果/Unix 接口）用的，
# app 这一侧是干净的安卓代码，只吃 NDK 头。
# Note: the dm_android_compat force-include is deliberately **not** added here -- that one is for the host sources
# (which stand in for Apple/Unix interfaces); the app side is clean Android code using only NDK headers.
# --whole-archive **是必须的，不是优化**（与 dmhost 可执行文件那边同一个理由，见 dm/android/CMakeLists.txt 里那段
# 长注释）：宿主里有一批编译单元只贡献**登记项**（hle_sdkinit.c 的服务层接管、hle_table.c 的 libc 直通表），它们
# 只有 static 函数与 static const 表项、不对外定义任何符号，所以静态库默认不会把它们拉进来，登记段里就少了它们。
# 实测症状（第一次把宿主链进 APK 时）：SDK 初始化在 nnosInitialize/nninitInitializeSdkModule 之后**立刻 Break#1**，
# 日志里一条 `init 接管：` 都没有 —— 而控制台宿主一切正常，差别就是这对开关。
# --whole-archive is **mandatory, not an optimisation** (the same reason as on the dmhost executable; see the long
# comment in dm/android/CMakeLists.txt): some translation units in the host contribute **registration entries only**
# (hle_sdkinit.c's service-layer take-overs, hle_table.c's libc pass-through table), holding nothing but static
# functions and static const tables with no exported symbols, so a static library will not pull them in by default and
# the registry sections lose them. Measured symptom (the first time the host was linked into the APK): right after
# nnosInitialize/nninitInitializeSdkModule the SDK hit **Break#1** with not one `init 接管：` line, while the console
# host was fine -- and this pair of switches is the only difference.
target_link_libraries(dm PRIVATE
  -Wl,--whole-archive dmhost_android -Wl,--no-whole-archive
  vulkan android log)

# 与 dmhost 可执行文件同一条规矩（见 dm/android/CMakeLists.txt 里那段注释）：**关掉 --gc-sections** —— 宿主的两张
# 登记表（swhle 直通、swforce 强制接管）只靠链接器生成的 __start_/__stop_ 被引用，GC 会把这两个段整个丢掉：段没了，
# 表就是空的，服务层接管一条都不生效。**它和 --whole-archive 缺一不可**：前者让"只贡献登记项"的成员进得来，后者让
# 登记段别被丢掉。真机症状（两条都缺时）：SDK 初始化走到 nnosInitialize/nninitInitializeSdkModule 之后立刻 Break#1，
# 日志里一条 `init 接管：` 都没有，而控制台宿主一切正常。
# The same rule as the dmhost executable (see the comment in dm/android/CMakeLists.txt): **turn off --gc-sections** --
# the host's two registry sections (the HLE passthrough swhle and the forced take-over swforce) are reached only through
# the linker-generated __start_/__stop_, and GC drops both sections entirely: no sections means empty tables and not one
# service-layer take-over applies. **It and --whole-archive are both needed**: the former lets members that only
# contribute registration entries in, the latter stops the registry sections being dropped. Device symptom with both
# missing: right after nnosInitialize/nninitInitializeSdkModule the SDK hits Break#1 with not one `init 接管：` line,
# while the console host is fine.
target_link_options(dm PRIVATE -Wl,--no-gc-sections -Wl,-z,max-page-size=16384)
