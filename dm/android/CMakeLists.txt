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

# main.c 以 -Dmain=dm_main 编入（与 iOS 同一条路，见 dm/ios/Sources/main.m）：安卓入口在 main_android.c
# main.c is compiled with -Dmain=dm_main (the same route as iOS, see dm/ios/Sources/main.m); the
# Android entry point lives in main_android.c
set_source_files_properties(${DM}/host/main.c PROPERTIES COMPILE_DEFINITIONS "main=dm_main")

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
  ${CMAKE_CURRENT_SOURCE_DIR}/front_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/gfx_stub_android.c
  # 图形后端（M2）：设备建设备与「把客体地址空间换成 Vulkan 零拷贝内存」两件基础设施。
  # 回放层的其余部分陆续加进来，加一块就从 gfx_stub_android.c 里删掉对应的空实现。
  # Graphics backend (M2): two pieces of infrastructure, the device bring-up and "turn the guest address
  # space into Vulkan zero-copy memory". The rest of the replay layer arrives piece by piece, and each piece
  # removes its stub from gfx_stub_android.c.
  ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_device.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_region.c
  # B3 仪器：GPU 操作流水（"最后一次成功提交 ↔ 第一条 -4"），见 vk_ops.h
  # B3 instrument: the GPU-operation ledger (the last successful submit up to the first -4), see vk_ops.h
  ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_ops.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_tex.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_spv.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_shader.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_pipe.c
  # 最小 PNG 写出（自带、无依赖）：出图截图要落盘成能直接看的文件，见 vk_png.h
  # Minimal PNG writer (self-contained, no dependencies): a present screenshot has to land on disk as a file that can
  # be looked at directly, see vk_png.h
  ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_png.c
  # 窗口/交换链/上屏（M3）：让宿主这一个设备也能把画面上到手机屏上，见 vk_win.h
  # Window, swapchain and present (M3): let the host's single device put a picture on the phone screen, see vk_win.h
  ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_win.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk/nvn_vk.c
  # 新地基（Vulkan 原生显式实现）N1：影子内存 + 分块后备资源 + 地址反查。**先让它可用**，接管回放层是 N4
  # （§9.7 的接管点表）；在此之前没有任何调用方，所以它进来只是"编进构建、随时能用"，不改变旧路行为。
  # New foundation N1: shadow memory + chunked backing store + address lookup. Made available now; taking over
  # the replay layer is N4. Until then nothing calls it, so it only becomes part of the build.
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_pool.c
  # 新地基 N2：区间级脏记账（§9.4）+ 上传/回读窗口（§9.5）。同样"先让它可用"，接管回放层是 N4。
  # New foundation N2: interval dirty accounting and the upload/readback windows; made available now, N4 takes over.
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_dirty.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_xfer.c
  # 新地基 N3：帧批提交 + 有界在飞（§9.6）。同样"先让它可用"（没有任何调用方）：接管回放层的提交是 N4。
  # New foundation N3: frame-batch submission with bounded in-flight work; also made available only, N4 takes over.
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_sub.c
  ${HOST_SRC}
  ${DM}/host/entry.S
  ${GFX_SRC}
  ${GFX_ASM}
  ${LIBDEFLATE}
  ${DM}/ios/Sources/dm_touch_controls.c)

# nativewindow 是 AHardwareBuffer_* 所在的库（NDK 的公开库）：内存换轨的 AHB 支路要它，见 vk_region.c。
# PUBLIC 是必须的 —— 符号要用在最终可执行文件（dmhost 与 APK 里的 libdm.so）里，静态库自己不链接。
# nativewindow hosts AHardwareBuffer_* (a public NDK library): the memory overlay's AHB route needs it, see
# vk_region.c. PUBLIC is required -- the symbols are needed by the final binaries (dmhost and the APK's libdm.so)
# and a static library does not link on its own.
target_link_libraries(dmhost_android PUBLIC vulkan nativewindow)

target_include_directories(dmhost_android PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${DM}/host
  ${DM}/gfx/nvn
  ${DM}/gfx/nvn/include
  ${DM}/gfx/tex
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

# 位置无关代码：M3 的窗口工程要把这个静态库**链进一个 .so**（APK 的 libdm.so），而默认编出来的是给可执行文件用的
# 绝对寻址（nvn_slot / nvn_real 那一批表），链进共享库会直接报"relocation ... cannot be used against symbol ...
# recompile with -fPIC"。打开它：对可执行文件只是一点点开销，比维护两套构建划算。
# Position-independent code: M3's window project links this static library **into a .so** (the APK's libdm.so), while the
# default output uses absolute addressing meant for executables (the nvn_slot / nvn_real tables and friends), which makes
# the link fail with "relocation ... cannot be used against symbol ... recompile with -fPIC". Turning it on costs a little
# in the executable and is cheaper than maintaining two builds.
set_target_properties(dmhost_android PROPERTIES POSITION_INDEPENDENT_CODE ON)

# M2 第一步的探针：不进宿主，也不进 APK，只用来量设备的 Vulkan 能力（离屏清屏 + 回读 + 内存模型）。
# 之所以先做离屏：宿主现在是 adb 推上去跑的可执行文件，没有窗口，而窗口/交换链要 APK 才有。
# M2 step 1 probe: not part of the host or any APK, it only measures the device's Vulkan capabilities
# (offscreen clear + readback + memory model). Offscreen comes first because the host is currently an
# executable pushed with adb and has no window, and a window / swapchain needs an APK.
add_executable(vk_probe ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_probe.c)
target_link_libraries(vk_probe PRIVATE vulkan)
target_link_options(vk_probe PRIVATE -Wl,-z,max-page-size=16384)
target_include_directories(vk_probe PRIVATE ${COMPAT})

# M2 的第二个能力探针：Vulkan 能不能把「宿主自己那块内存」直接当 GPU 内存用（VK_EXT_external_memory_host）。
# 苹果有统一内存、任意宿主指针都能包成 MTLBuffer；Vulkan 没对应物，这条扩展是零拷贝内存池的唯一出路。
# M2's second capability probe: can Vulkan take a piece of the host's own memory and use it as GPU memory
# (VK_EXT_external_memory_host). Apple has unified memory so any host pointer can be wrapped in an
# MTLBuffer; Vulkan has no equivalent, and this extension is the only route to a zero-copy memory pool.
add_executable(vk_hostmem_probe ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_hostmem_probe.c)
target_link_libraries(vk_hostmem_probe PRIVATE vulkan)
target_link_options(vk_hostmem_probe PRIVATE -Wl,-z,max-page-size=16384)
target_include_directories(vk_hostmem_probe PRIVATE ${COMPAT})

# M2 的第三个能力探针：零拷贝内存能不能一次要这么多、够不够铺客体地址空间（heap 4 GB + alias 512 MB）。
# 这是「没有 VK_EXT_external_memory_host 之后，零拷贝该怎么办」这条设计路线的可行性依据。
# M2's third capability probe: how much zero-copy memory can be obtained at once, and is it enough to
# back the guest address space (heap 4 GB + alias 512 MB). This is the feasibility evidence for the design
# route "what to do about zero-copy now that VK_EXT_external_memory_host is missing".
# 兼容层摘要实现的自检（标准向量）。回放层的着色器键就靠 sha256(控制段)[:16]，
# 而兼容层原先全是占位实现——这条探针把真实现验住，顺带把「键」这个用法也跑一遍。
# Self-check of the compat layer's digest implementations (standard vectors). The replay layer's shader key is
# sha256(control segment)[:16] and the compat layer used to be all placeholders -- this probe pins the real
# implementation down and exercises the key usage as well.
add_executable(t_digest ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_digest.c)
target_link_options(t_digest PRIVATE -Wl,-z,max-page-size=16384)
target_include_directories(t_digest PRIVATE ${COMPAT})

# 着色器产物的自检：把每一个 <键>.spv 都真的建成 VkShaderModule、解析同名的 res.json。
# 这是"建管线"之前唯一能独立验的两件事——驱动收不收这份 SPIR-V、反射解析对不对。
# Self-check of the shader artifacts: create a VkShaderModule for every <key>.spv and parse the matching
# res.json. These are the only two things verifiable independently before pipelines exist -- whether the driver
# accepts the SPIR-V and whether the reflection parses.
add_executable(vk_shader_probe ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_shader_probe.c ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_shader.c ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_spv.c ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_device.c)
target_link_libraries(vk_shader_probe PRIVATE vulkan)
target_link_options(vk_shader_probe PRIVATE -Wl,-z,max-page-size=16384)
target_include_directories(vk_shader_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${DM}/host ${COMPAT})

# 管线层的自检：给全库每份着色器建一次描述符集布局 + 管线布局（把"每个 binding 号是什么类型、属于哪个阶段"
# 这套解读交给驱动判），并给每个计算着色器建一条计算管线（不需要渲染状态，是补齐影子状态前唯一能闭环验的地方）。
# Self-check of the pipeline layer: build a descriptor set layout plus pipeline layout for every shader in the
# library (handing the reading of "which type is this binding number, which stage owns it" to the driver) and a
# compute pipeline for every compute shader (needing no render state, which makes this the only place the chain can
# be closed before the shadow state is complete).
# vk_waitled_stub.c：vk_pipe.c 里有"等待站点账本"的调用，而账本的真身在宿主（crash.c）—— 探针不链宿主，
# 所以给它一份空实现（见该文件里的说明）。
# vk_waitled_stub.c: vk_pipe.c calls the "wait-site ledger", whose home is the host (crash.c); probes do not link the
# host, so they get empty definitions (see the note in that file).
add_executable(vk_pipe_probe ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_pipe_probe.c ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_pipe.c ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_shader.c ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_spv.c ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_device.c ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_waitled_stub.c)
target_link_libraries(vk_pipe_probe PRIVATE vulkan)
target_link_options(vk_pipe_probe PRIVATE -Wl,-z,max-page-size=16384)
target_include_directories(vk_pipe_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${DM}/host ${COMPAT})

add_executable(vk_fdmap_probe ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_fdmap_probe.c)
target_link_libraries(vk_fdmap_probe PRIVATE vulkan)
target_link_options(vk_fdmap_probe PRIVATE -Wl,-z,max-page-size=16384)
target_include_directories(vk_fdmap_probe PRIVATE ${COMPAT})

add_executable(vk_region_probe ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_region_probe.c)
target_link_libraries(vk_region_probe PRIVATE vulkan)
target_link_options(vk_region_probe PRIVATE -Wl,-z,max-page-size=16384)
target_include_directories(vk_region_probe PRIVATE ${COMPAT})

# M2 的第五个能力探针（「M3-3d」的延伸）：AHardwareBuffer 这条路在**非 Adreno**上能不能走。
# 「M3-3d」量出第二台（MT6896 / Mali-G610 MC6）两种外部句柄都不可导出 ⇒ 甲案（分配可导出内存 → 取 fd →
# mmap 压回客体地址）在它上面不存在；同源的另一条路是 AHB（两台都有该扩展），问的是**一问**：
# 导入 AHB 之后能不能在**同一份内存上按偏移绑多块缓冲**（池子与 vk_region_find 的前提）。
# AHB 的 API 在 NDK 的公开库 libnativewindow.so 里（AHardwareBuffer_*）。
# M2's fifth capability probe (an extension of M3-3d): does the AHardwareBuffer route exist on a **non-Adreno**
# device? M3-3d measured that neither external handle type is exportable on device 2 (MT6896 / Mali-G610 MC6), so
# approach A (allocate exportable memory -> take the fd -> mmap it back at the guest address) does not exist there.
# The sibling route is an AHB (the extension is present on both devices) and it asks **one** question: after
# importing an AHB, can several buffers be bound at offsets of **the same memory** (the precondition of the pool
# and of vk_region_find)? The AHB API lives in the NDK's public libnativewindow.so (AHardwareBuffer_*).
add_executable(vk_ahb_probe ${CMAKE_CURRENT_SOURCE_DIR}/vk/vk_ahb_probe.c)
target_link_libraries(vk_ahb_probe PRIVATE vulkan nativewindow android)
target_link_options(vk_ahb_probe PRIVATE -Wl,-z,max-page-size=16384)
target_include_directories(vk_ahb_probe PRIVATE ${COMPAT})

# 新地基（Vulkan 原生显式实现）N0 的常数标定探针：把 docs/rewrite/ANDROID_REWRITE.zh-CN.md §9.9 那四个"待实测"
# 常数量成真机读数（只问不接管：不建池、不写脏记账、不接管回放层，那些是 N1~N3）。独立可执行文件，不进宿主也不进 APK。
# 一档尺寸派生一个自己的子进程：某档失败会把同进程之后的小分配也带坏（AHB 探针踩过），所以它要能复执行
# /proc/self/exe；读数落 .work/logs/probe-<串号>-vk2_probe.txt（走 .work/probes.py run）。
# N0 constant-calibration probe of the new foundation: turns the four "to be measured" constants of section 9.9 of
# docs/rewrite/ANDROID_REWRITE.zh-CN.md into on-device readings (ask only, take over nothing: no pool, no dirty
# accounting, no replay-layer take-over -- those are N1..N3). A standalone executable, part of neither the host nor
# any APK. Each chunk-size rung is measured in a freshly spawned copy of itself (a failure poisons later small
# allocations in the same process, as the AHB probe learned), hence the re-execution of /proc/self/exe; the readings
# land in .work/logs/probe-<serial>-vk2_probe.txt via .work/probes.py run.
add_executable(vk2_probe ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_probe.c)
target_link_libraries(vk2_probe PRIVATE vulkan)
target_link_options(vk2_probe PRIVATE -Wl,-z,max-page-size=16384)
target_include_directories(vk2_probe PRIVATE ${COMPAT})

# 新地基 N1（影子 + 分块后备 + 地址反查）的自证探针：把判据 **G3** 做成一问一答 —— 4 GB 池按块建出**全功能**
# 后备资源（每块头尾都真写一遍并回读）、`g_pool_small` 与 `g_pool_no_region` 为 0、跨块请求的长度分布、
# 快路 PINNED 的降级、以及 `DM_GFX2` 开关的可读自证。它与 vk2_pool.c 一起编，不进宿主也不进 APK。
# 读数落 .work/logs/probe-<串号>-vk2_pool_probe[-tag].txt（走 .work/probes.py run）。
# The self-proving probe of N1: gate G3 as a question-and-answer run (a 4 GB pool built chunk-wise as
# full-featured backing, both ends of every chunk really written and read back, small/no_region at zero, the
# cross-chunk length distribution, the PINNED degradation, and a self-proof that DM_GFX2 is readable).
# N2 起 vk2_pool.c 会用脏记账（每个池一张位图）⇒ 这个 N1 的探针也要链上 vk2_dirty.c；
# 顺带它就成了"N1 的 G3 在 N2 的代码上还成立吗"的回归探针（N2 轮里跑一次）。
# Since N2 the pool allocates a dirty bitmap, so N1's probe links vk2_dirty.c too -- which incidentally makes it
# the regression probe for "does G3 still hold on N2's code".
add_executable(vk2_pool_probe
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_pool_probe.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_pool.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_dirty.c)
target_link_libraries(vk2_pool_probe PRIVATE vulkan)
target_link_options(vk2_pool_probe PRIVATE -Wl,-z,max-page-size=16384)
target_include_directories(vk2_pool_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vk2 ${COMPAT})

# 新地基 N2（脏记账 + 上传/回读窗口 + 显式屏障）的自证探针：把判据 **G1**（往返闭环逐字节正确）与
# **G2**（不等待读旧值 / 等完栅栏仍不回读还是旧值 / 回读之后才是新值）做成一问一答，外加脏位图与朴素模型的
# 逐位对比、扫描耗时三档、半页竞态、以及一批**机制**读数（明确不是帧读数）。不进宿主也不进 APK。
# 读数落 .work/logs/probe-<串号>-vk2_xfer_probe[-tag].txt（走 .work/probes.py run）。
# The self-proving probe of N2: gates G1 and G2 as a question-and-answer run, plus a per-bit self-proof of the
# dirty bitmap against a naive model, three scan-timing rungs, the half-page hazard and a batch of mechanism
# readings (explicitly not frame readings).
add_executable(vk2_xfer_probe
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_xfer_probe.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_dirty.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_pool.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_xfer.c)
target_link_libraries(vk2_xfer_probe PRIVATE vulkan)
target_link_options(vk2_xfer_probe PRIVATE -Wl,-z,max-page-size=16384)
target_include_directories(vk2_xfer_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vk2 ${COMPAT})

# 新地基 N3（帧批提交 + 有界在飞）的自证探针：把判据 **G4** 拆成可判定的几问一答 —— 一个客体提交 = 一次
# `vkQueueSubmit`（记账与探针各数一遍）、`vkQueueWaitIdle` 计数恒 0、"有界在飞"的 D = 1/2/3/4/8 各量一遍
# （故意让 GPU 忙起来）给出槽等待率与长等条数、`vk2_sub_wait_all()` 等的是所有在飞槽而不是队列空闲、
# 以及"提交槽能被 N2 的搬运窗口复用"（顺带量出暂存环是单窗口的这一限制）。不进宿主也不进 APK。
# 读数落 .work/logs/probe-<串号>-vk2_sub_probe[-tag].txt（走 .work/probes.py run）。
# The self-proving probe of N3: gate G4 as a question-and-answer run -- one guest submission is one vkQueueSubmit,
# vkQueueWaitIdle stays at zero, bounded in-flight work is measured for D = 1/2/3/4/8 with the GPU kept busy (slot
# wait rate and long-wait count), wait_all waits the in-flight slots rather than queue idle, and a submission slot
# can carry an N2 upload window (which incidentally measures that the staging ring is single-window).
add_executable(vk2_sub_probe
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_sub_probe.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_sub.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_dirty.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_pool.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vk2/vk2_xfer.c)
target_link_libraries(vk2_sub_probe PRIVATE vulkan)
target_link_options(vk2_sub_probe PRIVATE -Wl,-z,max-page-size=16384)
target_include_directories(vk2_sub_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vk2 ${COMPAT})

# M1 验收用的可执行文件：没有窗口、没有图形后端，直接在设备上跑。
# 通过 adb push 到 /data/local/tmp 执行（那里对 shell 用户是可执行的），目标是日志里出现游戏的 UNIMPL 行；
# 打成 APK 留到 M2 之后（那时才有画面）。
# The executable used for M1 acceptance: no window, no graphics backend, run directly on the device.
# Push it to /data/local/tmp with adb and execute it there (that path is executable for the shell
# user); the goal is the game's UNIMPL lines showing up in the log. Packaging an APK is left until
# after M2, when there is something to show.
add_executable(dmhost_android_bin ${CMAKE_CURRENT_SOURCE_DIR}/main_android.c)
# --whole-archive 是必须的，不是优化：静态库默认只抽取「能解决某个未定义符号」的成员，而宿主里有一批
# 编译单元**只**贡献登记项（hle_sdkinit.c 的服务层接管、hle_table.c 的 libc 直通表）——它们只有 static
# 函数与 static const 表项，不对外定义任何符号，于是整个成员不会被拉进来，登记段里就少了它们。
# 实测：不加这一对开关时，各目标文件合计 swforce 263 条 / swhle 55 条，成品只剩 80 条 / 1 条，
# 结果 nndiagStartup 等 7 条服务层接管全部失效、SDK 初始化在 nn::lm 那里 Panic。
# 苹果那边是 Makefile 把所有 .c 直接编进可执行文件、不经静态库，所以每个编译单元都在——这正是
# 「苹果能跑、安卓崩」的最后一块拼图。
# --whole-archive is required here, not an optimisation: a static library only extracts the members that
# resolve some undefined symbol, and the host has translation units that contribute nothing but registry
# entries (hle_sdkinit.c's service-layer take-overs, hle_table.c's libc passthrough table) -- they hold
# only static functions and static const table entries and define no external symbol, so the whole member
# is never pulled in and the registry sections lose them.
# Measured: without this pair, the object files add up to 263 swforce / 55 swhle entries while the binary
# keeps only 80 / 1, which kills all seven service-layer take-overs including nndiagStartup and makes SDK
# initialisation panic inside nn::lm. On Apple the Makefile compiles every .c straight into the executable
# without a static library, so every translation unit is there -- this is the last piece of "works on
# Apple, panics on Android".
target_link_libraries(dmhost_android_bin PRIVATE
  -Wl,--whole-archive dmhost_android -Wl,--no-whole-archive)
set_target_properties(dmhost_android_bin PROPERTIES OUTPUT_NAME dmhost)
# 把符号导出到动态表：崩溃现场的 dm_backtrace 用 dladdr 解析宿主自己的帧，而 dladdr 只看 .dynsym
#（.symtab 完整也没用）。不导出的话，回溯里宿主那几帧只剩一串裸地址，正好是最要紧的那几帧——
# 客体帧有模块表可查，宿主帧只能靠这个名字。宿主是可执行文件、不分发体积，代价只有几十 KB。
# Export symbols into the dynamic table: dm_backtrace resolves the host's own frames with dladdr in a crash
# report, and dladdr only looks at .dynsym (a complete .symtab does not help). Without it the host frames in a
# backtrace are bare addresses -- exactly the frames that matter most, since guest frames can be looked up in
# the module table while host frames depend on this name. The host is an executable whose size is not
# distributed, so the cost is a few tens of KB.
target_link_options(dmhost_android_bin PRIVATE -Wl,--export-dynamic)
# 安卓 15 起要求 16 KiB 页对齐；显式声明以免在新设备上装不上 / 跑不起来
# Android 15 and later require 16 KiB page alignment; state it explicitly so the binary loads on
# newer devices
target_link_options(dmhost_android_bin PRIVATE -Wl,-z,max-page-size=16384)
# 关掉 --gc-sections：宿主的两张登记表（HLE 直通 swhle、强制接管 swforce）只靠链接器生成的
# __start_/__stop_ 被引用，GC 会把这两个段整个丢掉——段没了，表就是空的，服务层接管一条都不生效
# （实测：开着它时段表里根本看不到这两个段）。宿主是可执行文件、不分发体积，关掉没有代价。
# Turn off --gc-sections: the host's two registry sections (the HLE passthrough swhle and the forced
# take-over swforce) are reached only through the linker-generated __start_/__stop_, and GC drops both
# sections entirely -- no sections means empty tables and not a single service-layer take-over applies
# (measured: with it on, neither section appears in the section table). The host is an executable whose
# size is not distributed, so turning it off costs nothing.
target_link_options(dmhost_android_bin PRIVATE -Wl,--no-gc-sections)
