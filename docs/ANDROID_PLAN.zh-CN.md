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

- [x] 建一个最小 CMake 工程，把 `dm/host/` 的平台无关源文件加入编译
- [x] 首次编译，收集完整报错清单
- [x] 把报错分类：Apple 专有 API / POSIX 差异 / 头文件缺失 / 其他
- [x] 产出「宿主层复用率评估」结论
- [x] **验收**：拿到一份"要改哪些文件、改多少处"的清单

**结论（实测：NDK r30.0.16248370 / clang 21.0.0，`arm64-v8a`，`android-31`）**

参与编译 **87 个编译单元**（`dm/host/*.c` 70 个 + `dm/gfx` 8 个 + libdeflate 5 个 + 触摸判定 1 个 + 汇编 3 个）：

| 结果 | 数量 |
|---|---|
| **直接编过，零改动** | **56** |
| C 文件有平台差异 | 29 个文件，共 **31 处**（每个 1–4 处） |
| 汇编文件需重写 | 2 个（`nvn_thunks.S` 1047 处、`nvn_entry.S` 6 处） |

**31 处 C 平台差异 —— 分类与解法**

| 类别 | 处数 | 具体形式 | 解法 |
|---|---|---|---|
| Apple 专有头文件不存在 | 17 | `TargetConditionals.h`(7)、`mach/mach.h`(3)、`mach/mach_time.h`(3)、`CommonCrypto/CommonDigest.h`(2)、`sys/sysctl.h`(1)、`sys/clonefile.h`(1) | 加 `__ANDROID__` 分支，接平台等价实现 |
| macOS 专有 API / 结构 | 14 | `st_mtimespec`(6)、`clock_gettime_nsec_np`(3)、`CLOCK_UPTIME_RAW`(2)、`pthread_get_stack{addr,size}_np`(2)、`F_GETPATH`(1) | 见下表 |

| 原用法 | 安卓等价 |
|---|---|
| `struct stat.st_mtimespec` | `st_mtim`（POSIX） |
| `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` | `clock_gettime(CLOCK_MONOTONIC)` |
| `mach/mach_time.h` | `clock_gettime` |
| `pthread_get_stacksize_np` / `pthread_get_stackaddr_np` | `pthread_getattr_np` + `pthread_attr_getstack` |
| `F_GETPATH` | `/proc/self/fd/<fd>` + `readlink` |
| `sys/sysctl.h` | `/proc` |
| `clonefile()` | `copy_file_range` |
| `CommonCrypto/CommonDigest.h` | 自带实现或其他库 |

**汇编**

`nvn_thunks.S`、`nvn_entry.S` 用的是 **Mach-O 汇编语法**（`@PAGEOFF`），在 Android 的 **ELF** 上不成立，要改成 AArch64 ELF 的写法（`#:lo12:` 等）。这是机械改动；其中 `nvn_thunks.S` 是生成文件（`dm/gfx/tools/gen_protos.py`），改生成器即可。

**一句话结论：宿主层的 C 代码复用率约 95%+。** 87 个编译单元里 56 个**零改动通过**，其余 29 个共 31 处、且都是"换一个 API"级别的局部改动。这与可行性分析里"宿主层大部分是平台无关 POSIX/C"的判断一致，而且**比预期更乐观**。真正的重头戏仍然是图形层（M2 的 Vulkan 回放层）。

**顺带记录（Windows 环境坑）**：跑 `gen_vault_table.py` / `gen_hud_exp.py` / `gen_bright_levels.py` 必须设 `PYTHONIOENCODING=utf-8`。否则 Python 在 Windows 上按 GBK 编码输出，遇到脚本注释里的 `U+2212` 会 `UnicodeEncodeError` 中途退出，生成出**被截断的表头**（表现为一堆 `vt_row` 未定义）。

### 探路 B —— 着色器翻译链落地

- [x] 用 .NET SDK 编译 `dm/gfx/tools/dmsh_xlate`
- [x] 对 `.work/` 里的游戏文件跑着色器翻译，产出 SPIR-V
- [x] 确认产物是 **Vulkan 目标**的 SPIR-V（版本与魔数）
- [x] 记录产物目录与反射 json 的格式（后续 Vulkan 回放要用）
- [x] **验收**：确认「NVN 着色器 → Vulkan SPIR-V」在本机可复现

**结论（实测）**

- `dotnet publish dm/gfx/tools/dmsh_xlate -c Release` **编译成功**（.NET SDK 10.0.402 + 仓内 Ryujinx 库，`Spv.Generator` / `Ryujinx.Common` / `Ryujinx.Graphics.Shader` / `dmsh_xlate` 四个工程全部还原并生成）。
- 从游戏 RomFS 提取 **162 个着色器包**（vs 43 / ps 96 / cs 21 / ds 1 / hs 1）。
- 逐包按 `BTCDBTCD` 切记录 → 共 **10847 个变体**；按控制段 `sha256[:16]` 去重后 **6923 个唯一键**。
- `dmsh_xlate`：**翻译 10847 成功，0 失败**。
- 产出 **6923 个 `.spv`**（魔数 `0x07230203` 全部正确，异常 0）+ 6923 个 `.res.json` 反射。
- **SPIR-V 版本 1.3**（对应 Vulkan 1.1 目标；红米 K90 的 Adreno 驱动支持更高版本，向下兼容）。

**意义**：这是整套方案里"最有利的一块"，现在**已在本机完整落袋**——安卓侧写 Vulkan 回放层时，直接就有 6923 个现成的 SPIR-V 着色器可用，**不需要任何运行时翻译**（这正是当初断言"安卓可行"的关键依据之一）。

本次只跑了链的**前半段**（NVN → SPIR-V）。后半段（`spirv-cross` → MSL → `metallib`）是苹果专有的，安卓用不上，跳过。

产物在 `.work/spv/`（已 gitignore，不进仓库）。

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

## 六、Windows 作业规范（编码与命令）

本项目目前在一台 Windows 机器上推进安卓移植。这里记下已经踩过的坑与约定，后续一律照此执行。

### 编码

- **Python 一律带 `PYTHONUTF8=1`（并加 `PYTHONIOENCODING=utf-8`）**。Windows 上 Python 默认按 GBK 处理文件与标准输出，会踩两种坑：
  - 输出侧：脚本注释含 `U+2212` 这类字符时 `UnicodeEncodeError` **中途退出**（表现为生成的表头被截断）；
  - 输入侧：仓库里的表是 UTF-8，而脚本里 `open(path)` 未指定编码 → `UnicodeDecodeError`（读 `shader_facts.tsv` 时踩到）。
  `PYTHONUTF8=1` 让所有 `open()` 与 stdio 默认走 UTF-8，一次解决两侧问题。
- **重定向到文件用 cmd 的 `>`**（保持原始字节），不要在 PowerShell 里用管道 + `Out-File`（会改编码与行尾）。
- **读文件显式指定编码**：PowerShell 用 `-Encoding`，或直接 `[System.IO.File]::ReadAllLines()`。
- **路径一律纯 ASCII**。本机用户目录名含中文（`C:\Users\ZhuanZ（无密码）`），已经因此踩过两个坑：
  - `hactool` 会把 `HOME` / `USERPROFILE`（含中文）转 UTF-16 失败而崩溃 → 跑之前先设一个 ASCII 的 `HOME`；
  - 生成的表头、缓存、日志不要落在用户目录下。
  - 约定位置：SDK 在 `C:\Android\Sdk`，工具/工作区在 `d:\yiyi\d2r-mobile\.tools`、`.work`。

### 命令

- **长任务必须给出进度显示**，不要静默等待：下载用 `curl`（不加 `--silent`）、构建用 `ninja`（默认的 `[n/m]` 进度）、`adb push/pull` 自带进度等。
- **多步命令写成 `.bat` / `.cmd` 再执行**，不要在 PowerShell 里拼 `cmd /c "..."`（引号嵌套极易出错）。
- 需要"收集全部错误"时用 `ninja -k 0`，输出重定向到日志后再统一分析。
- **`sdkmanager` 已被官方弃用**，且在无 TTY 环境下无法接受许可；改用 `android sdk install`（启动时展示 ToS 后自动接受）。

## 七、下一步

按本页顺序推进：先把 M0 剩下的部分（SDK/NDK + 真机）补齐，再做探路 A、探路 B，然后进 M1。
每完成一个里程碑，在这里回勾并记录结论。
