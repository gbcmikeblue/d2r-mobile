# 安卓移植：实施计划与任务清单

English: [ANDROID_PLAN.md](ANDROID_PLAN.md)

这一页把「把 D2R Mobile 移植到安卓」拆成一份可执行、可勾选的任务清单，供逐项确认与分工。技术可行性判断见 [ANDROID.zh-CN.md](ANDROID.zh-CN.md)；给不写代码的人看的版本见 [ANDROID_PLAIN.zh-CN.md](ANDROID_PLAIN.zh-CN.md)。

**当前阶段：前期准备（尚未开始写安卓端代码）。** 下面的复选框反映本机的实际进度，不是设想。

## 一、起点状态快照

### 已完成

- [x] 可行性分析并入仓库（`docs/ANDROID.*`）
- [x] 建 `android` 分支
- [x] **游戏文件准备就绪**（Switch 版 1.0.34.0）
  - 从自备的 `nsz` / `nsp` + `prod.keys` 还原出 ExeFS 与 RomFS
  - `main`、`sdk`、`subsdk0` 的 SHA-256 与 3.3.94012 **逐项一致**
  - 18 个资源档案 **齐全**
  - 存放位置：`.work/data/switch/3.3.94012/Update/`（已在 `.gitignore` 中）
- [x] **本机工具链**
  - Python 3.10.11 + `pillow numpy lz4 capstone unicorn nsz`（`.venv/`）
  - hactool（由源码编译，`.tools/hactool/hactool.exe`）
  - .NET SDK 10.0.402（着色器翻译工具用）
  - LLVM / clang 23.1.3
  - JDK Temurin 21
  - CMake 4.4.4

### 已完成（续）

- [x] Android Studio 已安装
- [x] Android SDK / NDK 已装到 `C:\Android\Sdk`（纯 ASCII 路径，避开用户名里的中文）
  - `platform-tools`（`adb` 1.0.41 / 37.0.1）
  - `platforms;android-36`（Android 16）
  - `build-tools;36.0.0`
  - `cmake;3.31.6`
  - `ndk;30.0.16248370`（clang 21.0.0，可执行）

### 已完成（M0 收尾）

- [x] 安卓真机联调准备（开发者选项、USB 调试、`adb devices` 能认到）
- [x] 用 NDK 编一个最小原生程序，推到真机执行成功（真机输出 `hello from Android NDK` / `pointer size: 8 bytes`）

**目标机（`adb` 实测）**

| 项 | 值 |
|---|---|
| 品牌 / 型号 | Redmi K90（`2510DRK44C`，代号 `annibale`），序列号 `8b1f1e50` |
| 系统 | **Android 16**（`ro.build.version.release=16`，SDK **36**） |
| SoC | `SM8750`（骁龙 8 Elite） |
| GPU / Vulkan 驱动 | Adreno（`ro.hardware.vulkan=adreno`） |
| CPU ABI | `arm64-v8a` |

## 二、总体路线

| 阶段 | 目标 | 一句话验收 |
|---|---|---|
| M0 | 环境就绪 | NDK 能编出一个原生程序并推到真机上跑起来 |
| M1 | **无窗口跑通宿主** | 安卓上加载改写产物、执行 `nnMain`，日志出现 `UNIMPL` / `【红】` |
| M2 | **最小 Vulkan 回放层** | 屏幕能出画面（先清屏，再到主菜单） |
| M3 | 前端 + 触摸 + 音频 | 能建角、进局、有声音 |
| M4 | 收尾 | 手柄、云同步、诊断、性能调优 |

原则：**不重写游戏逻辑**，复用现有宿主；只替换「面向平台」的两侧（前端与图形后端）。

## 三、任务清单

### M0 —— 环境就绪

- [x] 安装 Android SDK Platform（`android-36`，对应真机的 Android 16）
- [x] 安装 NDK（`30.0.16248370`）
- [x] 安装 platform-tools，确认 `adb` 可用
- [x] 真机：开启开发者选项与 USB 调试，`adb devices` 能认到设备
- [x] 记录目标机信息：机型、SoC、Android 版本、GPU 驱动版本（见上表）
- [x] 用 NDK 编一个最小原生程序，推到真机执行成功
- [x] **验收**：真机上能跑通一个 hello 级别的原生程序 —— **M0 完成**

### 探路 A —— 用 NDK 试编宿主层（不写新功能，只量化复用率）

- [ ] 建一个最小 CMake 工程，把 `dm/host/` 的平台无关源文件加入编译
- [ ] 首次编译，收集完整报错清单
- [ ] 把报错分类：Apple 专有 API / POSIX 差异 / 头文件缺失 / 其他
- [ ] 产出「宿主层复用率评估」结论（写回本页或单独文档）
- [ ] **验收**：拿到一份"要改哪些文件、改多少处"的清单

### 探路 B —— 着色器翻译链落地

- [ ] 用 .NET SDK 编译 `dm/gfx/tools/dmsh_xlate`
- [ ] 对 `.work/` 里的游戏文件跑着色器翻译，产出 SPIR-V
- [ ] 抽查若干产物，确认是 **Vulkan 目标**的 SPIR-V（可用 `spirv-dis` / `spirv-val` 验证）
- [ ] 记录产物目录与反射 json 的格式（后续 Vulkan 回放要用）
- [ ] **验收**：确认「NVN 着色器 → Vulkan SPIR-V」在本机可复现

### M1 —— 无窗口跑通宿主

- [ ] 新增 `dm/android/`，建立工程骨架（形态见「待决事项」）
- [ ] 写 `front_android.*` 的最小实现：`front_run` 直接在当前线程跑客体，`front_metal_layer` 返回 NULL
- [ ] **平台无关化**：给宿主里 Apple 专有的段落补 `__ANDROID__` 分支
  - [ ] `hle_fs.c`：`clonefile()` / `xattr` → `copy_file_range` / 替代表
  - [ ] `hle_audio.c`：先接**静音路径**（`null_sink`，纯 POSIX，可直接复用）
  - [ ] `crash.c` / `diag.c`：mach、信号栈 → `/proc`、`backtrace` 或先弱化
  - [ ] `dm_i18n.c`：CoreFoundation → 标准 C
  - [ ] `dm_padmode.c`、`diag.c`：CommonCrypto → 自带实现或其他库
  - [ ] `memstat.m`：mach → 丢弃或按 `/proc` 重写
  - [ ] `loader.c`、`main.c`：`TARGET_OS_IPHONE` 分支旁加安卓分支
- [ ] 改写产物加载：把 `patched.bin` 放进 App 资源，映射进内存执行（注意 `PROT_EXEC` 与 W^X 限制）
- [ ] 跑 `nnMain`，观察日志
- [ ] **验收**：APK 装在真机上，画面全黑，但日志里出现游戏的 `UNIMPL` 行

### M2 —— 最小 Vulkan 回放层（最大工作量）

- [ ] 搭 Vulkan 骨架：设备、队列、`ANativeWindow` 交换链
- [ ] MemoryPool → `VkDeviceMemory` + host-visible/coherent 缓冲（验证「GPU 地址 = CPU 地址」的零拷贝假设）★风险点
- [ ] Texture / Sampler / TextureView 的映射
- [ ] Program：从 SPIR-V 建 `VkShaderModule` / 管线
- [ ] 最小命令集：`SetRenderTargets`、清屏、`DrawArrays`、`Present`
- [ ] **验收**：屏幕出画面（先清屏成一个颜色，再逐步到能显示主菜单）

### M3 —— 前端 + 触摸 + 音频

- [ ] `front_run`：安卓窗口与主循环（`NativeActivity` / `SurfaceView`）
- [ ] `front_text_input`：系统软键盘输入框
- [ ] 触摸：复用 `dm_touch_controls.c`（纯 C），重写输入采集与控件渲染
- [ ] 音频：真出声换 AAudio / Oboe
- [ ] `front_save_failed`：存档失败提示（Toast）
- [ ] 生命周期：切后台/回前台、`dm_audio_suspend` / `dm_gfx_suspend`
- [ ] **验收**：能建角、进局、有声音

### M4 —— 收尾

- [ ] 手柄（Android `InputDevice`）
- [ ] 存档云同步（替换 `cloud_ios.m`）
- [ ] 诊断与崩溃处理
- [ ] 性能调优：帧率、功耗（对标 iOS 的 60 fps / 3.8 W）

### 贯穿任务

- [ ] 每完成一块，补对应说明文档（中英各一份）
- [ ] 宿主单元测试的安卓入口（`make -C dm/host/tests` 的对应物）
- [ ] 遵守项目规矩：**不提交任何游戏内容**（游戏文件、生成物均不进仓库）
- [ ] 屏上新增文字进 `dm/i18n/ui_text.tsv`

## 四、已确认的决定

| # | 事项 | 决定 |
|---|---|---|
| 1 | 工程形态 | **Android Studio 工程**（Gradle + CMake + JNI） |
| 2 | 目标真机 | **红米 K90**（高通骁龙 8 Elite，Adreno GPU，6.59" 1.5K 120Hz；系统版本以真机「关于手机」为准） |
| 3 | Android SDK Platform | `android-36`（Android 16，对应真机系统） |
| 4 | NDK 版本 | 最新稳定（r27 及以上） |
| 5 | 改写产物的加载方式 | App 资源 + `mmap` 执行 |
| 6 | 最低支持的 Android 版本 | **Android 12（API 31）**，建议值，见下 |

最低版本建议取 **Android 12 / API 31**：Vulkan 1.1 及以上已强制定，`AHardwareBuffer`、`AAudio` 都成熟，存储与权限模型现代，能省掉一大摊老机型兼容分支。想覆盖面更宽可退到 API 29（Android 10）；想更省心可提到 API 33（Android 13）。

## 五、主要风险（会阻塞进度的那种）

| 风险 | 说明 | 应对 |
|---|---|---|
| **统一内存 / 零拷贝** | Metal 侧假设「GPU 地址 = CPU 地址」，Vulkan 需 host-visible + coherent | M2 一开始就先验证，跑不通要改内存模型 |
| **TBDR / 通道优化** | `nvn_mtl.m` 的通道合并与附件读写在 macOS 上验证过，手机 GPU 架构不同 | M2 先做最朴素路径（`DM_NVN_TBDR=0` 的等价物），再优化 |
| **`nvn_mtl.m` 体量** | 约 5,800 行 Metal，是最大的一块 | 按 NVN 函数分批移植，配合 `dm/gfx/tests` 的思路做等价测试 |
| **性能** | iOS 的 60 fps 来自深度调优，不能假定直接搬过来 | M4 单独设调优阶段 |

## 六、下一步

按本页顺序推进：先把 M0 剩下的部分（SDK/NDK + 真机）补齐，再做探路 A、探路 B，然后进 M1。
每完成一个里程碑，在这里回勾并记录结论。
