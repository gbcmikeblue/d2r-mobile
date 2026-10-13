# 安卓（Android）移植可行性分析

English: [ANDROID.md](ANDROID.md)

这一页回答一个问题：**能不能把 D2R Mobile 搬到安卓手机上？** 结论放在最前面，依据逐条列出，供立项和分工时确认。想给不写代码的人看，见 [ANDROID_PLAIN.zh-CN.md](ANDROID_PLAIN.zh-CN.md)。

## 结论

**技术上可行。** 这套架构本来就是照着「换一个平台」设计的：顶层 README 里写着，安卓手机与 Switch 是同一类处理器，游戏程序同样可以原生执行，要换的只是宿主与图形层里面对平台的那一侧。

**但它不是小工程。** 工作量集中在图形层：现在跑在苹果 Metal 上的回放层，要重写成 Vulkan。

| 部分 | 能否复用 | 说明 |
|---|---|---|
| 游戏程序（改写产物） | 能 | 改写器与平台无关；产物是 aarch64，安卓原生执行 |
| 着色器翻译链 | 大部分能 | 中间产物本就是 Vulkan 目标 SPIR-V，跳过 Metal 那两步即可 |
| 宿主层（系统服务） | 大部分能 | Apple 专有代码集中在 3 个 `.m` 文件与若干局部段落 |
| 图形回放层 | 要重写 | `dm/gfx/nvn/nvn_mtl.m` 约 5,800 行 Metal 代码 → Vulkan |
| 前端与触摸界面 | 要重写 | UIKit / CAMetalLayer → 安卓界面框架 |
| 资源生成脚本、第三方运行期库 | 能 | 纯 Python / 纯 C，可直接复用 |
| 工作量重心 | — | 图形层约占 60–70% |

## 一、为什么可行

### 1. 游戏程序本身就是 aarch64，安卓原生同架构

Switch 用的是 ARM 处理器（Tegra X1），安卓手机也是 ARM（ARMv8 / ARMv9）。这一点是整个移植成立的基础：

- 游戏程序**不需要模拟 CPU**，可以直接在手机上执行；性能不是问题；
- 改写器 `dm/tools/patch_nso.py` 只改写在苹果/安卓内核下不能原样执行的少数指令（线程指针寄存器、`svc` 系统调用、计时器），程序其余部分逐字节保持原样；
- 改写器是**离线、纯 Python** 的工具，与运行平台无关，安卓可以直接用同一份、出同样的产物。

### 2. 着色器翻译链已经产出 Vulkan 目标的 SPIR-V（最大的有利点）

游戏的着色器是 Tegra X1 的 GPU 机器码（Maxwell 指令集）。现有的翻译链是：

```
Switch 原版着色器包
  → tools/dmsh_xlate        用 Ryujinx 的着色器翻译库出 SPIR-V（C# 工具）
  → spirv-cross             SPIR-V 转 Metal 着色语言（MSL）
  → xcrun metal / metallib  编成 Metal 着色器库
```

关键点：`dmsh_xlate` 的目标语言与目标 API 本就是 **Vulkan**（见 `dm/gfx/tools/dmsh_xlate/Program.cs` 里 `TargetLanguage.Spirv, TargetApi.Vulkan`）。也就是说：

- 翻译链的**前段（NVN 机器码 → SPIR-V）+ 反射 json** 在安卓上可以原样复用；
- 移植时**跳过 MSL 与 metallib 两步**，直接把 SPIR-V 交给 Vulkan 驱动（或经 SPIRV-Cross 出 GLSL）；
- 需要手写的着色器很少：主程序里内嵌的 4 个计算着色器（现在是手写 MSL，在 `dm/gfx/nvn/nvn_builtin.h`）与 1 个 GPU 解排布着色器（`dm/gfx/tex/nvn_deswizzle_gpu.m`，本身已有 CPU 回退可临时顶替）。

### 3. 宿主层大部分是平台无关的 POSIX/C

`dm/host/` 的 147 个文件里，苹果专有的只有 3 个 `.m` 文件（Mac 前端 `front_mac.m`、Mac 引导 `app_mac.m`、内存记账 `memstat.m`），加上若干 `.c` 文件里的局部段落（音频、文件系统、崩溃诊断）。可以直接复用的部分很大：

- 内核系统调用接管（`hle_svc.c`、`hle_thread.c`）；
- 线程与同步、C 库直通（`hle_thread.c`、`hle_table.c`）；
- 文件系统、输入（`hle_fs.c` 主体、`hle_hid.c`、`hle_touch.c`）、软键盘协议（`hle_swkbd.c`）、文本输入（`dm_textinput.c`）；
- Opus 音频解码（`hle_codec.c`，用 SDK 自带的 libopus，非系统框架）；
- 音频的**静音接收端**（`hle_audio.c` 的 `null_sink`，纯 POSIX，按真实时间消耗缓冲）；
- 存档云同步核心逻辑（`dm_cloudsync.c`，本身明确「不碰 iCloud 接口」，只做本地目录 ↔ 云端目录的文件搬运）；
- 绝大多数游戏内功能（`dm_*.c`：触摸点选、世界交互、买卖加点、仓库、秘境、掉落、HUD 等）。

### 4. 前端接口抽象干净（只有 7 个符号）

平台相关的部分被收敛在 `dm/host/front.h` 这一份接口后面，Mac 由 `front_mac.m` 实现、iOS 另实现同名接口。移植到安卓，只需要新写一份 `front_android.*` 实现同样的符号：

| 接口 | 作用 | 移植难度 |
|---|---|---|
| `front_run` | 跑客体（主循环归属） | 平台相关，需重写 |
| `front_metal_layer` | 出图目标（现返回 `CAMetalLayer`） | 平台相关，且要换成 Vulkan 交换链 |
| `front_game_res` | 输出分辨率（纯算术） | 易重写 |
| `front_text_input` | 软键盘输入框 | 平台相关，需重写 |
| `front_save_failed` | 存档失败提示 | 平台相关（Toast） |
| `dm_touch_set` | 触屏推点 | **已在跨平台 C 里（`hle_touch.c`），无需改** |
| `dm_host_preload_start` | 装载提前起跑 | **纯 POSIX，无需改** |

### 5. 构建工具链已证明可跨平台

顶层 README 明确：**在 Windows / Linux 上做出的游戏程序与 Mac 逐字节相同**。这说明打补丁、翻译着色器这些步骤独立于运行平台。安卓打包可以直接复用这套（只需替换 `gen_embed.py`、`ios_metal.py` 这类 iOS 专有的打包步骤）。

## 二、需要复用 / 改写 / 新增的清单

按位置给出三类分工，可直接作为立项时的任务分解。

### A. 原样复用（几乎零改动）

| 位置 | 内容 |
|---|---|
| `dm/tools/patch_nso.py` | 程序改写器，离线、纯 Python |
| `dm/gfx/tools/`（翻译链前段） | `dmsh_xlate`（出 SPIR-V）、`dmsh_chain*`、`shader_map.py`、`maxwell_refs.py` 等 |
| `dm/host/hle_svc.c`、`hle_thread.c`、`hle_table.c` | 内核调用、线程与同步、C 库直通 |
| `dm/host/hle_touch.c`、`hle_hid.c`、`hle_swkbd.c`、`dm_textinput.c` | 触屏换算、手柄底层、软键盘协议、文本输入 |
| `dm/host/hle_codec.c` | Opus 软解（SDK 自带 libopus） |
| `dm/host/dm_cloudsync.c` | 云同步核心逻辑 |
| `dm/host/dm_*.c`（绝大部分） | 游戏内功能与触摸适配逻辑 |
| `dm/ios/tools/*.py`（资源生成） | 纯 Python + PNG / JSON，产出平台无关 |
| `dm/third_party/libdeflate`、`bcdec` | 运行期库，本身已支持 ARM / Linux |

### B. 改写（逻辑照搬，替换苹果专有调用）

| 位置 | 现在用什么 | 换成什么 |
|---|---|---|
| `dm/host/hle_audio.c` | CoreAudio `AudioUnit`（AudioToolbox） | AAudio 或 Oboe；静音路径可保留 |
| `dm/host/hle_fs.c` | `clonefile()`、`getxattr/setxattr` | `copy_file_range` / `sendfile` + 替代表 |
| `dm/host/crash.c`、`diag.c`、`memstat.m` | mach、`OSCacheControl`、信号栈 | `/proc`、`backtrace` 或按需弱化 |
| `dm/ios/Sources/dm_gamepad.m` | GameController 框架 | 安卓 `InputDevice` |
| `dm/ios/Sources/dm_touch_controls.c`、`dm_touch_game_state.c` | 纯 C（仅目录归属 iOS） | 挪进共用层，逻辑不变 |
| `dm/ios/Sources/dm_metal_cache_guard.c` | 纯 C | 逻辑不变，改接 Vulkan |

### C. 新增 / 重写（安卓专有）

| 位置 | 内容 |
|---|---|
| `dm/gfx/nvn/nvn_vk.*`（新） | **GPU 回放层**：把 `nvn_mtl.m` 的 Metal 调用逐类映射到 Vulkan（本项目最大工作量） |
| `dm/host/front_android.*`（新） | 安卓前端：窗口、出图、输入、文本、存档提示、生命周期 |
| 安卓触摸界面（新） | 替换 `DmTouchOverlay.m` / `front_ios.m` 的 UIKit 渲染层 |
| `dm/gfx/tex/nvn_deswizzle_vk.*`（新，可选） | GPU 解排布的 Vulkan 版（也可先用 CPU 回退） |
| `dm/gfx/nvn/nvn_builtin.h`（改） | 4 个内嵌计算着色器：MSL → GLSL / SPIR-V |
| 安卓打包（新） | 替换 `gen_embed.py`、`ios_metal.py`、`project.yml` 等 iOS 专有步骤 |
| 云同步（改） | 替换 `cloud_ios.m`（iCloud）为安卓侧实现 |

## 三、平台差异与风险点

这几点需要在真机上专门验证，不能只靠推理：

1. **统一内存假设**。Metal 侧把游戏内存直接建成零拷贝 `MTLBuffer`，假设「GPU 地址 = CPU 地址」。Vulkan 要映射到 host-visible + host-coherent 内存（或 `AHardwareBuffer`）。现代安卓 SoC 的集成 GPU 一般支持，但要逐机型实测。
2. **TBDR / 渲染通道优化**。`nvn_mtl.m` 里有大量针对 Metal 的通道合并、附件读入写回优化。Vulkan 有 render pass / subpass，但 Mali（TBR）与 Adreno（TBDR）的架构差异要求重做这部分逻辑，且性能调优要重来一遍。
3. **页大小与对齐**。Metal 侧按 16 KiB 页对齐内存池；安卓常见 4 KiB / 16 KiB 混用，需要调整并核对。
4. **可执行内存与 W^X**。iOS 不允许运行时写可执行代码，所以现方案把改写产物作为内嵌汇编编进 App；安卓反而更宽松（可 `mmap PROT_EXEC`），但 Android 10+ 有 W^X 限制，需要按版本处理。
5. **工作区路径**。`~/dm-work` 这套路径要换成 App 私有目录 + `Storage Access Framework` 或 `MANAGE_EXTERNAL_STORAGE`。
6. **性能目标**。iOS 在 A16 上 60 fps 靠的是深度 Metal 优化；Vulkan 后端需要重新调优，不能假定同等结果。

## 四、工作量估算

按「能进游戏、能战斗」的最小可用版本粗略拆分：

| 部分 | 占比 |
|---|---|
| GPU Vulkan 回放层（含渲染通道优化重写与调优） | 约 60–70% |
| 前端 + 触摸界面 + 音频 + 文件系统 | 约 25% |
| 着色器链改造（跳过 MSL、接 SPIR-V） | 较小（SPIR-V 已产出） |
| 手柄、云同步、诊断 | 约 5% |

一句话：**可行性高、复用率高，但核心是「再写一个图形后端」**。这与当年从 Mac 扩到 iOS 不同——那次只换前端，这次要换整个 GPU 后端。

## 五、建议的移植路线

1. **无窗口跑通**：先在安卓上让 `dmhost` 加载改写产物、执行 `nnMain`，观察日志里的 `UNIMPL` / `【红】`，确认宿主底座可跑。
2. **最小 Vulkan 回放层**：只支持游戏启动所需的一小撮 NVN 子集（清屏、基础绘制、一张贴图），目标是「能出画面」。
3. **补全渲染通道优化**：对齐 TBDR 语义，补齐通道合并与附件读入写回。
4. **前端 + 触摸界面 + 音频**：做到「能玩」。触摸判定层（`dm_touch_controls.c`）是纯 C，可直接复用，只重写渲染与输入采集。
5. **收尾**：手柄、云同步、诊断与崩溃处理。

## 六、Apple API → 安卓对应表

| 用途 | 现在（Apple） | 安卓对应 |
|---|---|---|
| 图形 API | Metal | Vulkan |
| 出图目标 | `CAMetalLayer` / `CAMetalDrawable` | `ANativeWindow` / `VkSwapchainKHR` / `AHardwareBuffer` |
| 出图节拍 | `CAMetalDisplayLink` | `Choreographer` / `AVSync` |
| 窗口与 App 生命周期 | UIKit / AppKit | `NativeActivity` / `Activity` |
| 音频输出 | CoreAudio `AudioUnit` | AAudio / Oboe |
| 手柄 | GameController | `InputDevice`（`KEYCODE_*` / `MotionEvent`） |
| 软键盘 | UIKit 输入框 | `AlertDialog` / `InputConnection` |
| 存档同步 | iCloud（`NSFileManager` ubiquity） | Google Drive / 自建同步 |
| 芯片专有内存操作 | `OSCacheControl`、mach `vm_*` | `__builtin___clear_cache`、`/proc/self/maps` |
| 崩溃与内存诊断 | mach task / `vm_region` | `/proc/self/statm`、`backtrace`、`ptrace` |

## 七、与仓库现有文档的关系

- 这套东西整体怎么工作：见 [../dm/README.zh-CN.md](../dm/README.zh-CN.md)。
- 图形层与着色器翻译链：见 [../dm/gfx/README.zh-CN.md](../dm/gfx/README.zh-CN.md)。
- 无 Mac 的构建路线（跨平台打包的既有先例）：见 [NO_XCODE.zh-CN.md](NO_XCODE.zh-CN.md)。
- 想看通俗解释：见 [ANDROID_PLAIN.zh-CN.md](ANDROID_PLAIN.zh-CN.md)。

本页的结论基于对 `dm/host`、`dm/gfx`、`dm/ios` 与 `dm/third_party` 的源码核查；具体到实现时，请以各文件开头的注释与 [DERIVED.zh-CN.md](DERIVED.zh-CN.md) 的说明为准。
