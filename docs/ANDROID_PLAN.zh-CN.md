# 安卓移植：实施计划与任务清单

English: [ANDROID_PLAN.md](ANDROID_PLAN.md)

这一页把「把 D2R Mobile 移植到安卓」拆成一份可执行、可勾选的任务清单，供逐项确认与分工。技术可行性判断见 [ANDROID.zh-CN.md](ANDROID.zh-CN.md)；给不写代码的人看的版本见 [ANDROID_PLAIN.zh-CN.md](ANDROID_PLAIN.zh-CN.md)。

**当前阶段：M2 进行中（Vulkan 回放层已在真机上跑起来）。** 下面的复选框反映本机的实际进度，不是设想。

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

**M1 进展记录（进行中，2026-10-14）**

- [x] **改写产物已出**：`dm/tools/patch_nso.py` 三种产物 × 三个模块跑通，代号 `3.3.94012-r41`；`keypage`（M1 采用）25 个文件齐全。
- [x] **安卓兼容层**：`dm/android/compat/`（12 个头 + 1 个 `.c`，用 `-include` 强制包含）。宿主层从**31 个文件 / 1090 行错误**降到 **0**。
- [x] **平台差异逐项落地**（改源码，一律 `#if defined(__ANDROID__)` 分支 + 中英双语注释）：

| 项 | 处理方式 |
|---|---|
| 汇编 Mach-O 语法（1053 处） | `nvn_thunks.S` 改**生成器** `gen_protos.py`：平台差异收进两个宏分支（C 符号前导下划线、`@PAGE/@PAGEOFF` ↔ `:lo12:`），展开体保持平台中立；`nvn_entry.S` 用同一套做法手改 |
| `mcontext_t` 结构差异（31 处） | 新增 `dm/host/dm_uctx.h`：寄存器访问统一成 `DM_UC_PC/LR/SP/FP/X` 宏（必须是左值，故不带转型），`crash.c` / `diag.c` 改用 |
| xattr 参数个数（3 处） | `hle_fs.c` 加安卓分支（Bionic 是 4 / 5 / 2 参，macOS 是 6 / 6 / 3 参） |
| `dispatch/dispatch.h`（1 处） | `hle_lifecycle.c` 用**两条普通线程**顶替 GCD（1.5 秒兜底 + `sigwait` 收 SIGTERM）；`hle_audio.c` 只用到信号量，由兼容层的 `dispatch/dispatch.h` 提供 |
| `AudioToolbox`（1 处） | `hle_audio.c` 加安卓分支：AudioUnit 调用全部变空操作、`au` 恒为 NULL，一律走静音接收端（M1 暂不出声） |
| `thread_policy_get` 参数个数 | 兼容层签名补齐（Apple 是 5 参） |

- [x] **宿主编译通过**：`dm/android/CMakeLists.txt` 用 NDK 工具链配置后 `ninja` → **0 错误**，产出 `libdmhost_android.a`（约 9.0 MB，88 个目标文件）。
- [ ] 尚未进行：加载器接入、`front_android` 最小实现、链接成可执行文件、真机运行。

**待办（M1 期间先放过，验收后必须补回来）**

- 兼容层里的 MD5 / SHA1 / SHA256 目前是**占位实现**（定长零输出），`hle_tact.c` 的取样核对与 `dm_padmode.c` 的原文串识别会因此不准；
- `mach-o` 段查找一律按"找不到"返回，`F_GETPATH`、`sysctl(KERN_PROC_PID)` 未实现；
- 安卓侧 SIGSEGV 拿不到 ESR 的「读 / 写」位，暂按「读」处理（影响缺页钩子的判定）；
- 构建暂时用 `-w` 关掉了警告，M1 验收后打开逐项处理。

### M1 真机验证记录（2026-10-14，进行中）

宿主已经在真机上跑通到「装载 → 重定位 → 进 SDK 初始化 → 打出客体 UNIMPL」，也就是 M1 的验收判据：

```
[dm] 线程槽量测：首个键 0x80000002 在 tpidr_el0+520，键间距 16 字节
[dm] main: base 0x78fc7e5000 size 0x33dc000
[dm] sdk:  base ... size 0xe80000
[dm] main: reloc relative=60560 abs/glob=2109 jump=615
[dm] force: 80 条全部命中
[dm] >>> nnosInitialize
[dm] UNIMPL nnosGetDefaultUserExceptionStackForApplication (...)      ← 验收点
[dm] >>> nninitInitializeSdkModule
[dm] SVC 0x1f ... （占位返回成功）
```

为此修掉两个**架构级**的平台差异——它们都不是读代码能看出来的，是跑到真机才暴露：

**① 线程槽基址的平台约定**（`26158e2`）

重写器把客体的 `mrs xN, tpidrro_el0` 原样保留，靠它加键页偏移去索引宿主的 pthread TSD。这在苹果上成立，在安卓上第一步就断：

| | Apple | Android (bionic) |
|---|---|---|
| `tpidrro_el0` | = pthread TSD 基址 | **恒为 0，EL0 只读、用户态改不了** |
| TSD 基址 | `tpidrro_el0` | `tpidr_el0` |
| `pthread_key_t` | 槽号本身（小整数） | `0x80000000 \| 序号`（如 `0x80000001`） |
| 值的字节偏移 | `键*8` | 键 1/2/3 → +88 / +104 / +120（每个键占两个指针位） |

改法：重写器新增 `DM_TLSREG`（缺省 `apple`；`linux` 则把键槽访问的 `mrs` 目标换成 `tpidr_el0`，三个模块都走同一个 `MRS_TPIDRRO()`，只改这一处）；宿主 `loader.c` 的键页偏移改为按平台算——安卓不写死 bionic 的内部编码，而是启动时用哨兵键实测「首个键的偏移」与「键间距」再线性外推。`tsd_check` 用对应平台的基址寄存器校验，不符时报出该用哪个 `DM_TLSREG` 重出产物。

**注意：安卓产物与 iOS 产物因此不再完全同一份**（只差键槽那一条 `mrs` 的目标寄存器）。

**② 登记段在 ELF 上要另外读**（`7ed17c1`）

`HLE` / `HLE_FORCE` 两张登记表原本用 `getsectiondata(&_mh_execute_header, "__DATA", "swhle"/"swforce", …)` 读——那是 Mach-O 的段查找。安卓上读到 NULL ⇒ 表为 0 条 ⇒ 上百条服务层接管（`nndiagStartup`、`oe::Initialize`、`fs::InitializeForApplication`、`pctl::Initialize` …）**一条都没生效**，SDK 拿自己的真实现去连不存在的 Horizon 服务进程 → Panic。改法：段名按平台给（`__DATA,swhle` ↔ `swhle`），读法用 ELF 的 `__start_<节名>` / `__stop_<节名>`，并且**链接时必须 `-Wl,--no-gc-sections`**（NDK 默认开着 GC，这两个段只靠 `__start_/__stop_` 被引用，会被整个丢掉；实测不加时段表里根本看不到它们）。实测 `swforce` 0x780 字节 = 80 条，日志变成 `force: 80 条全部命中`。

顺带：关掉 GC 后，之前被悄悄丢掉的 31 个图形侧符号（`mtl_*`、`dm_nvn_ruler_truth`、`thread_selfcounts`）全部浮出为未定义，已在 `dm/android/gfx_stub_android.c` 补齐空实现。

**当前卡点：`nninitInitializeSdkModule` 里一次 SDK Panic**

```
[dm] Break#1 reason=0 Result=0 svc 位于 sdk+0x39fbd4 缓冲 …+0x4 "...."
[dm] bt: sdk+0x145038 sdk+0x145198 sdk+0x3874a0 sdk+0x386120 sdk+0x2d46dc sdk+0x2d4470 sdk+0x2d33b4 sdk+0x2d335c sdk+0x2b03ec …
```

已经查清与排除的：

- **不是产物问题**：与仓库里原有的 `keypage` 产物逐字比对，改动**恰好**是那 25105 / 12085 / 10486 处 `mrs` 寄存器，其余一个字都没动；
- **不是时间层**：`now_ns()` = `mach_absolute_time()×numer/denom`，兼容层给的是纳秒 + `mach_timebase_info` 1/1，自洽；
- **不是接管表**：已 80 条全命中；
- **不是野值**：把占位返回的 `x0/x1` 都清零后，Panic 位置**完全不变**；
- **Panic 现场已定位**：`sdk+0x146870` 是 SDK 的 `Panic/Result` 小函数（`mov x1,x0; mov w0,wzr; mov w2,#4; bl svc#0x26`），所以 `reason=0` 是写死的、`Result` 就是传进来的那个值（日志读到 0）；它只被间接调用（函数指针表），所以要在 sdk 里按地址反查调用者。
- **SDK 的 svc 跳转表已解出**（`sdk+0x39fb04` 起，按序号排列）：sdk 共用到 `0x13–0x1d、0x1f、0x21–0x23、0x25–0x2a、0x2c、0x2d、0x2f、0x32–0x36、0x4b`，其中宿主目前只实现了 `0x01–0x06 / 0x1e / 0x26 / 0x27 / 0x29 / 0x2c / 0x2d`。表里那几条包装是 `str x0,[sp,#-16]! → svc → str w1,[x2]`，即**从 x1 取结果写进出参指针**。
- **日志里只有两处未实现的 svc 被调到**：`0x1f` 与 `0x21`（占位提示每个号只打一次）。`0x21` 与 `0x22` 是 sdk 里的**直接调用**，在同一个函数里相邻两条（`bl 0x39fb9c` / `bl 0x39fb94`），那个函数（`sdk+0x385040`）先读 TLR、与一个句柄比较，再二选一调用——`0x1f` 则只被间接调用。

**下一步（反汇编路线）**：sdk 的 `Panic` 小函数只被函数指针间接调用，所以要在 sdk 里按地址反查调用者（`adrp/ldr` 取表项后 `blr`），顺藤摸到真正失败的那处检查；同时把 `0x1f`（按序号推断为 `GetSystemTickFrequency` 一类）与 `0x21` 在 `hle_svc.c` 里按语义实现，而不是继续占位。

### M1 收尾：根因不在 SDK，在宿主的链接方式（2026-10-14）

用仓库自带的**带符号反汇编工具** `dm/tools/sdis.py`（用 dynsym 与 PLT 导入名标注）解析回溯，一次把整条链解了出来：

```
nninitInitializeSdkModule
└→ nndiagStartup                      ← 走 PLT，本应被宿主的强制接管换掉
   └→ nn::lm::Initialize              ← 连 lm 日志服务
      └→ nn::lm::CreateLogServiceByHipc
         └→ nn::sf::hipc::InitializeHipcServiceResolution
            └→ AbortImpl → VAbortImpl → svcBreak
```

`nndiagStartup` 本来就在宿主的接管清单里（「连 lm 日志服务；诊断日志改由宿主直接输出」），`force` 也报「全部命中」——但**它根本没进表**。把 `swforce` / `swhle` 两段 dump 出来对比：

| | swforce | swhle |
|---|---|---|
| 各目标文件合计 | 263 条 | 55 条 |
| 成品里实际有 | **80 条** | **1 条** |

原因：**静态库的按需抽取**。`libdmhost_android.a` 里，`hle_sdkinit.c`（7 条服务层接管）与 `hle_table.c`（libc 直通表）**只贡献登记项**——里面只有 `static` 函数与 `static const` 表项，不对外定义任何符号，链接器因此认为整个成员无用，**根本不会把它拉进来**。`--gc-sections` 已按上一节关掉，所以这不是 GC 的问题，是静态库的抽取规则。

苹果那边是 `Makefile` 把所有 `.c` 直接编进可执行文件、**不经静态库**，所以每个编译单元都在——**这就是「苹果能跑、安卓崩」的最后一块拼图**。

改法（`dm/android/CMakeLists.txt`）：链接时用 `-Wl,--whole-archive … -Wl,--no-whole-archive` 把库成员全部拉进来。修完实测 264 条 / 64 条，`nndiagStartup` 回到表里。

**修完的真机结果**：

```
[dm] init 接管：nndiagStartup（空操作）            ← 7 条服务层接管全部生效
[dm] init 接管：oe::Initialize（空操作）
[dm] init 接管：aoc::Initialize（空操作）
[dm] init 接管：err::InitializeContextWriterApi（空操作）
[dm] init 接管：fs::InitializeForApplication（空操作）
[dm] init 接管：pctl::Initialize（空操作）
[dm] init 接管：diag::InitializeApplicationAbortObserver（空操作）
[dm] >>> nninitStartup
[dm] SetHeapSize：首次设堆成功 3728 MB（应用内存池 3797 MB − 映像 68 MB；此前被拒 34 次）
[dm] main: DT_INIT +0x30（init_array 852 项）
[dm] sdk: DT_INIT +0x30（init_array 115 项）
[dm] 存档落盘节流：main+0x20cefa8 30 → 3 秒
[dm] >>> nnMain                                    ← ★ M1 的目标
[dm] fs 挂载 $: → …/RomFS（只读）；^: → …/temp
[dm] ToCalendarTime：首次换算 …（时区 CST，偏移 28800 秒）
[dm] nnMain returned
```

也就是：**宿主在安卓上装载改写产物、重定位、把服务层全部接管、跑完 SDK 初始化、进了 `nnMain`。** `nnMain` 随即返回，是因为 M1 没有图形后端（`front_metal_layer()` 为 NULL、NVN 走空设备），客体第一件事实质上就是建窗口 / 交换链，拿不到东西就退出了——这正是 M2 要补的。

顺带把 `svcBreak` 的处理加强了：除 SDK 消息缓冲外，还打印 x0..x7 里「像指针且指向可打印字串」的实参（`VAbortImpl` 的签名正是 expr/file/func/line），崩溃时能直接看到断言文本。

**M1 的真机部署方式**（当前）：产物目录 `dm/android/build` 里出的 `dmhost` 与改写产物用 `adb push` 推到
`/data/local/tmp/dm/{dmhost, patched/}`，然后在 `/data/local/tmp/dm` 下执行 `./dmhost ./patched main sdk`。
打成 APK 留到 M2 之后（那时才有画面）。

**游戏资源（8.3 GB RomFS）放哪**：决定（2026-10-14）是**放工作区里**——本机即 `/data/local/tmp/dm/romfs`，
与工作区同分区、读盘最快，`adb push` 一次到位。由安卓入口 `main_android.c` **按候选顺序自动探测**，找到即用并把
选中的路径打进日志（`romfs: <路径>`）；调用方仍可用 `DM_ROMFS` 直接指定。候选顺序（前面的优先）：

1. **`<根>/romfs`、`<根>/data/romfs`** —— 首选，资源放工作区里（本机 = `/data/local/tmp/dm/romfs`）。
2. **`/sdcard/d2rm/romfs`、`/sdcard/Download/d2rm/romfs`** —— 后备：**唯一「重启不丢且 shell 可写」**的位置
   （`/sdcard` → `/storage/self/primary`，实测 `mkdir` 成功），代价是走 FUSE、加载稍慢。在这里留一份，
   就不怕系统清理 `/data/local/tmp`。
3. **`/data/local/d2rm/romfs`** —— 列在最后、只是"有就用"：`/data/local` 是 `drwxr-x--x root:root`，
   `shell` 用户**建不了子目录**（`mkdir: Permission denied`，实测），要 root 才行。

**重启风险与恢复**：`/data/local/tmp` 有可能被系统清理，届时从电脑重推约 4～5 分钟（源：
`.work/data/switch/3.3.94012/Update/RomFS/data`）；若在 `/sdcard` 留了后备，则只需把 `DM_ROMFS` 指过去
（或让候选探测自动选中它），秒级切换。

做成「按顺序找、找到即用」而不是写死一个路径，就是为了换目录时**只管搬文件、宿主不用改**：同分区内搬动是
改名、秒级完成；`/data/local/tmp` 与 `/sdcard` 之间是不同文件系统，只能实拷（8.3 GB，设备内拷约 2～4 分钟，
仍比从电脑重推快）。

**M1 结论**：验收判据「宿主跑起来、日志出现游戏的 `UNIMPL`」已达成，并超出到 `nnMain`。下一步进入 M2。

### M2 第 1 步：Vulkan 通路与内存模型实测（2026-10-14）

宿主现在是 adb 推上去跑的可执行文件、**没有窗口**，而窗口 / 交换链要 APK 才有。所以 M2 先做**离屏**：
这一步同时把「Vulkan 能不能用」「清屏对不对」「内存怎么摆」三件事问清，而最后一项正是可行性分析里
列为**头号风险**的那条假设——NVN 的「GPU 地址 = CPU 地址」零拷贝。

探针：`dm/android/vk/vk_probe.c`（不进宿主、不进 APK，只量设备）。真机（Redmi K90 / Adreno 830）结果：

```
实例建立成功：loader 支持的 Vulkan 1.4.0
物理设备 1 个
  [0] Adreno (TM) 830：驱动 512.800.71，Vulkan 1.3.284，类型 1
=== 内存模型 ===
  堆 0：15113 MB（设备本地）        堆 1：4095 MB（设备本地）
  类型 0–2：DEVICE_LOCAL
  类型 3：  DEVICE_LOCAL|HOST_VISIBLE|HOST_CACHED
  类型 4/5：DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT
  类型 6：  DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT|HOST_CACHED
  ⇒ 零拷贝（HOST_VISIBLE+HOST_COHERENT）可用：是（类型 6）
  队列族 0：3 个，标志 0x19f（图形 / 计算 / 传输 / 稀疏）；族 1 传输；族 2 计算
离屏贴图 256x256，显存 260 KB
清屏回读：全部符合（首像素 64,128,191,255）
```

**对风险清单的更新**

- **头号风险「统一内存」：确认成立。** Adreno 830 有 `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT`
  （类型 4 / 5 / 6，其中类型 6 还带 `HOST_CACHED`）。也就是 NVN 的零拷贝在 Vulkan 侧有落点：
  内存池映射到 host-visible + host-coherent 的 `VkDeviceMemory`；类型 6 还多给了一层「主机侧带缓存」的选择。
- **着色器**：设备是 Vulkan **1.3**、loader 1.4 ⇒ Recon B 产出的那批 **SPIR-V 1.3**（6923 个）可以直接喂给
  `vkCreateShaderModule`，不需要再降版本。
- **通路**：离屏清屏 + 逐像素回读全部符合 ⇒ 设备 / 队列 / 命令缓冲 / 内存绑定 / 图像屏障 / 图像到缓冲拷贝
  这一整条路是通的，M2 后续只是往里填 NVN 的语义。

**下一步**：内存池映射（NVN MemoryPool → `VkDeviceMemory`，先按零拷贝那条路走）与最小命令集
（`SetRenderTargets` / 清屏 / `DrawArrays` / present）。窗口与交换链要等 APK（`ANativeWindow`）。

### M2 第 2 步：零拷贝内存池双向验证通过（2026-10-14）

内存池是 NVN 的地基：客体在 CPU 侧写内存、GPU 侧要读到同一份（以及反过来）。同一个探针里按这条造了
两条 64 KB 缓冲，落在上面量出的 `DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT|HOST_CACHED`（类型 6）上，
两个方向各验一次：

```
零拷贝内存池（类型 6，每条 64 KB）：CPU→GPU 通，GPU→CPU 通
```

- **CPU 写 → GPU 读**：CPU 往映射地址写 `i*7+3`，GPU 用 `vkCmdCopyBuffer` 拷到另一条，回读逐字节比对全部相符；
- **GPU 写 → CPU 读**：GPU 用 `vkCmdFillBuffer` 填 0xAB，CPU 直接从映射地址读回，全部相符。

**结论：NVN 的内存池可以直接映射成类型 6 的 `VkDeviceMemory`，不需要任何拷贝。** 可行性分析里
「统一内存假设」这条头号风险到此**在真机上验证通过**，M2 其余部分可以照「零拷贝」这条最省的路走。
（探针里两步是分开提交的：拷贝读、填充写同一条缓冲，分开提交就不必自己插屏障。）

### M2 第 3 步：内存池的落法（两次能力实测，设计据此定型）

**① Metal 那条路在 Vulkan 上没有对应物。** 苹果侧内存池是这么落的（`nvn_mtl.m` 的 `mtl_pool_add`）：

```objc
// [mem, mem+size) 外扩到页边界 → newBufferWithBytesNoCopy → 零拷贝
id<MTLBuffer> buf = [g_dev newBufferWithBytesNoCopy:(void *)a length:b - a
                                            options:MTLResourceStorageModeShared deallocator:nil];
```

苹果有统一内存，**任意宿主指针都能包成 MTLBuffer**。Vulkan 要等价的效果只有
`VK_EXT_external_memory_host`（把宿主指针导入成 `VkDeviceMemory`）——实测真机（Adreno 830，165 个设备扩展）：

```
  [有] VK_KHR_external_memory / VK_EXT_descriptor_indexing / VK_KHR_timeline_semaphore
  [有] VK_KHR_swapchain / VK_ANDROID_external_memory_android_hardware_buffer / VK_EXT_queue_family_foreign
  [无] VK_EXT_external_memory_host        ← ★ Metal 那条路的 Vulkan 对应物
  [无] VK_EXT_host_image_copy
```

**所以「把池子指针直接包成缓冲」这条路在 Vulkan 上不通，设计必须改。**

**② 改成：客体地址空间本身由 Vulkan 零拷贝内存来铺。** 宿主本来就是自己摆这块地址空间的
（`hle_svc.c` 的 `mem_init`）：

```c
#define RESERVE (5ull << 30)
g_as    = mmap(NULL, RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
g_alias = g_as + 0x10000000; g_alias_cap = 0x20000000;            // alias 512 MB
g_stack = g_as + 0x30000000; g_stack_cap = 0x10000000;            // stack 256 MB
g_heap  = g_as + 0x40000000; g_heap_cap  = RESERVE - 0x40000000;  // heap 4 GB
```

`vkAllocateMemory` + `vkMapMemory` 拿到的映射地址**本身就是宿主指针**（第 2 步已验过 CPU / GPU 双向可见）。
于是只要把 heap 与 alias 这两块改由 Vulkan 零拷贝内存提供，池子地址到缓冲就全是算术，**不需要那条扩展**。
关键只剩「一次能不能要到这么大」——实测（`vk_region_probe.c`）：

```
maxMemoryAllocationSize = 1024 MB（广告值，实际能超），bufferImageGranularity = 1
  3072 / 3200 / 3328 / 3456 / 3584 / 3712 / 3840 / 3968 MB：分配并映射全部成功
  4096 MB：失败 VK_ERROR_OUT_OF_DEVICE_MEMORY
  ⇒ 单次上限 3968 MB
  惰性提交：3.9 GB 的块只碰 64 个页 → 常驻增量 -96 页（不真占内存）
  块内非零偏移绑缓冲：memory alignment 64，偏移 16 MB 处绑定成功，且该偏移处双向都通
```

**③ 设计定型**（M2 照此实现）：

| 区域 | 大小 | 来源 | 理由 |
|---|---|---|---|
| heap | 4 GB 容量，实际用到 3728 MB | **Vulkan 零拷贝内存**（单次 3968 MB 够） | 游戏的 `StandardAllocator` 从堆里切出 NVN 内存池，池子必须在 GPU 侧直接可见 |
| alias | 512 MB | **Vulkan 零拷贝内存**（另一次分配） | `MapPhysicalMemory` 映射进来的物理内存，SDK 的 VammManager 用 |
| stack | 256 MB | 普通 mmap | 只是 CPU 栈，GPU 不碰 |
| 其余预留段 | 剩余 | 普通 mmap | 只占地址空间 |

合计约 4.5 GB 的 Vulkan 分配，惰性提交所以不会真吃掉这么多内存（堆 0 有 15 GB）。

**这条设计的两处硬边界（实现时要处理）**：

- **单次分配上限 3968 MB**：客体堆当前是 3728 MB，余量约 240 MB。若将来提高 `app_pool()` 让堆超过这个数，就会退化成「两块不连续」——所以 `mem_init` 要**在超限时明确报红**，而不是静默失败。
- **落不到 Vulkan 区的池子**：客体若把池子开在普通 mmap 段（stack / 预留段），当前无法把它变成缓冲。实现时 `pool_add` 要**明确报红并计数**，先让真机跑出来告诉我们它到底会不会发生，再决定要不要补偿路（拷贝影子或改分配）。

**④ 落地时又量出的两件事（都已处理）**

- 载体缓冲不能用 `STORAGE_BUFFER`：为了导出 fd 必须给这份分配建一块「专属」缓冲（见上），实测把 4 GB 的载体缓冲按
  `STORAGE_BUFFER` 建会直接失败——撞驱动的 `maxStorageBufferRange`。载体不会被真的读写，改成只给传输位即可；
  同时**建不出来时按 128 MB 档往下退**，与分配上限的处理一致。
- 堆换到 3968 MB 就顶到上限，**报给客体的上限必须如实下调**（`g_heap_cap` 由 4 GB 改成 3968 MB）。这一步是必要的：
  否则客体照 4 GB 报的堆去用，最后 128 MB 会落在没换轨的普通内存上。实测下调后游戏照旧把堆设到 3728 MB（< 3968 ✓），
  引导一路走到 `nnMain`。

**当前状态**：`dm/android/vk/` 下已有 `vk_device.c`（Vulkan 设备，进程内建一次）与 `vk_region.c`（内存换轨 + 地址反查），
宿主侧通过 `dm/host/dm_gpu_region.h` 的钩子接入（宿主自己不认识 Vulkan）。真机日志：

```
【绿】Vulkan 设备就绪：Adreno (TM) 830（Vulkan 1.3.284）队列族 0；零拷贝内存类型 4；扩展 fd=有
【黄】堆 内存换轨：只换成 3968 MB（要 4096 MB）——顶到单次分配上限，报给客体的上限已相应下调
【绿】别名区 内存换轨：512 MB 已是 GPU 直接可见（0x6fa0a81000，类型 4）
SetHeapSize：首次设堆成功 3728 MB      ← 换轨后照旧
... nnMain returned
```

下一步是回放层本身（`nvn_vk.c`）：按 `nvn_mtl.h` 的接口先实现 `mtl_init` / `mtl_pool_add` / `mtl_pool_del` / `mtl_addr`，
每实现一块就从 `gfx_stub_android.c` 里删掉对应的空实现。

### M2 —— 最小 Vulkan 回放层（最大工作量）

- [x] 设备与队列（`ANativeWindow` 交换链留到 M3）
- [x] MemoryPool → `VkDeviceMemory` + host-visible/coherent 缓冲（「GPU 地址 = CPU 地址」的零拷贝假设**双向实测通过**）★风险点
- [x] Texture / Sampler / TextureView 的映射（格式表 154/157、采样器按参数缓存、按 view 建子视图；**渲染目标那一路的 view 待接**）
- [x] Program：从 SPIR-V 建 `VkShaderModule` / 管线（描述符写入与真下发已通：122 万次绘制、83 条管线、零失败）
- [x] 最小命令集：`SetRenderTargets`、清屏、`DrawArrays`、`DrawIndexed`（`Present` 留到 M3）
- [ ] **验收**：屏幕出画面（先清屏成一个颜色，再逐步到能显示主菜单）——**没有窗口**；"离屏读回一帧写盘"已经就位（第 13 步），而它量出的事实是 **GPU 上还没有写出颜色内容**（第 14 步）：**卡点在渲染链路本身**，不在"看不见"

### M3 —— 前端 + 触摸 + 音频

**第一块：窗口最小工程（已通，真机看到画面）** —— `dm/android/apk/`（一个 `NativeActivity`、零 Java、清单
`hasCode="false"`），脚本 `build_apk.cmd` / `install_apk.cmd`。真机：装上去启动后，**相隔 3 秒的两张截屏**里底色
从暗红变暗紫、白条从左移到右 —— `APK → ANativeWindow → VkSurface → 交换链 → 上屏` 整条链通了（`1156x2510 格式 37、
5 张、FIFO`）。画法有意不用渲染通道（`vkCmdClearColorImage` + `vkCmdCopyBufferToImage`），所以真机上没有管线要现编。

**装成 APK 之后数据怎么读（实测，两问两答）**：app 域是 `u:r:untrusted_app:s0`，adb 放进 `/data/local/tmp` 的数据
标签是 `shell_data_file`，于是 —— **读得到、写不进**：`能读 /data/local/tmp/dm/{patched,spv,romfs}`（9.1 GB **不用搬**），
但 `写不了 /data/local/tmp/dm/save`（`errno 13`）。把 `save`/`temp` 做成指向 app 目录的**软链也不行**（实测同样
`errno 13`）。正解是宿主本来就有的环境变量：`DM_ROOT=/data/local/tmp/dm` + `DM_SAVE` / `DM_TEMP` 指到
`/sdcard/Android/data/ai.d2rm.dm/files/{save,temp}`（app 正向对照里"能写"的那个目录）。

**权限集（定下来了，只有两条）**：`INTERNET`（普通权限，装完即生效，不需要申请）与 `MANAGE_EXTERNAL_STORAGE`
（特殊权限，必须由用户在系统设置里点开）。窗口里那块**橙色矩形**就是它的申请按钮 —— **按钮的有与无本身就是权限状态**
（有按钮 = 还没授权），所以这一版不必画一个字；点它打开授权页。**这条权限买得到什么、买不到什么，是 A/B 实测的**：

| `appops` 状态 | 写 `/data/local/tmp/dm/save` | 写 `/sdcard/d2rm` | 写 app 私有目录 |
|---|---|---|---|
| `allow`（已授予）| ✗ `errno 13` | **✓ 能写** | ✓ |
| `default`（未授予）| ✗ `errno 13` | ✗ `errno 1`（EPERM）| ✓ |

**第二块：窗口与游戏合一设备（已通）** —— 第一版窗口在 app 里**自建了一套** Vulkan（实例/设备/交换链），但**两个设备
之间传不了纹理**，"窗口显示游戏画出来的那张贴图"就无从谈起。现在窗口的 Vulkan 归宿主：`vk_device_set_window()` 在
`vk_device_init()` **之前**把窗口交过去（它决定实例要不要开安卓 surface 扩展、设备要不要开 `VK_KHR_swapchain`、以及
挑哪个队列族），交换链与上屏进 `vk_win.c`，app 只负责"画一帧主机内存里的 RGBA8 再交给它"。真机：`libdm.so` 从
245 KB 长到 **322 KB**（宿主链进来了），画面上出现**保持比例居中的测试图**（1600×900 缩进 1156×2510 → 上下留黑边），
说明上屏确实走的是宿主那一个设备。上屏路径本身也是下一步"贴图查看器"要走的那条：主机像素 → `vk_win_present_rgba`
→ 手机屏。

也就是说：**它买不到 `/data/local/tmp` 的写权限**（那条路是 SELinux 的 `shell_data_file`，任何权限都到不了），它买到
的是「**数据放在用户看得见、随时能用文件管理器替换的地方**」（`/sdcard/d2rm`，正是入口里已经写好的那条备用路径）。

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
| **偶发早期崩溃（实测 4 次里 1 次）** | `CRASH sig=11` 落在**宿主**里、进程启动约 **0.1 秒**即崩（`pc` 在 host 段，各线程 CPU 时间都还是 0.00s），连跑 3 次均未复现 | 现场已留档；频率若上升就按"启动 0.1 秒"这条时间线查——那一刻只跑过堆换轨与 SDK 初始化 |

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
- **XML 注释里不能出现 `--`**。`AndroidManifest.xml` 的注释里写了英文破折号 `--`，aapt2 报的是
  `AndroidManifest.xml:10: error: not well-formed (invalid token)` —— 报的行号在注释中间，看不出是注释的错。
- **本机打 APK 不走 gradle**（没有 gradle、PATH 上也没有 java，但装了 JDK 21）：用
  `aapt2 link`（要 `platforms/android-<ver>/android.jar`）→ 把 `lib/arm64-v8a/*.so` 以**不压缩**方式写进包
  （PowerShell 的 `ZipArchive` 就能做）→ `zipalign -p 4` → `apksigner sign`（要 `JAVA_HOME`，密钥用
  `keytool` 生成、只放在 `.work` 里）。清单里 `android:hasCode="false"` 时**不需要 dex**，于是整条链不需要
  一行 Java。脚本：`dm/android/apk/build_apk.cmd` 与 `install_apk.cmd`。

## 七、下一步

按本页顺序推进；每完成一个里程碑，在这里回勾并记录结论。**当前在 M3 的第 3 步**：渲染链路那几条告警已经从
"描述符回退成空白"收到只剩**一条**——`uniform buffer 拿不到缓冲地址` / `storage buffer 拿不到缓冲地址` /
`combined image sampler 贴图写空白` 在 2026-10-14 当天全部**归零**（前者靠常量槽号 −3 与存储缓冲的 cb_offset 反推，
后者靠**缓冲区贴图**这个新资源种类，见「M3-2k」一节）。三条其余告警与两条待办构成现在的阶梯：
接下来按价值排序：

1. **修顶点数据接槽**——告警 `顶点属性 N（… 位置 N）的分量类型与着色器声明不符：格式 NxN vs 着色器 float` 说明
   顶点流与格式的配对可能不对；几何整片塌掉是"画了却什么都没落下来"最可能的原因（先按着色器声明的类型选流，
   再核对步进与偏移）。**先做一件取证的活**：`vk_spv.c` 里 `OP_TYPE_FLOAT = 21 / OP_TYPE_INT = 22` 与 SPIR-V 规范
   **反了**（见「M3-2k」第六节），也就是说这条告警里"着色器 float"那句话本身可能来自一个反了的判据——先把它修对、
   再重跑一遍，看告警是消失、是变样、还是变成另一条；
2. **描述符那一档已清空**（保留作回归判据）：三处空白计数现在都应当是 0，`DM_NO_TEXEL=1` 可把缓冲区贴图那条一键退回空白，
   用来确认"归零"确实是这次改动买来的；
3. **修好之后再扫一遍**——`DM_NVN_SHOT_SCAN` 能直接用"有没有内容"回答，不必再靠猜（它已经是"你观察、我改"那条
   回路的仪器：图与报告落在同一个目录里）；
4. 文档与清理收尾（含接口描述表那批已不再被调用的代码）。

---

## 八、进度记录

**M0 / M1 —— 已完成**

| 里程碑 | 结论 |
|---|---|
| M0 SDK/NDK + 真机 | 工具链与真机就绪（Redmi K90 / Adreno 830 / Android 16 / arm64-v8a） |
| M1 宿主编译 | `dm/android` 工程 + NDK 工具链 → 宿主层 **0 错误**，88 个目标文件，`libdmhost_android.a` |
| M1 真机验收 | 装载改写产物、接管服务层、跑完 SDK 初始化、进 `nnMain`，日志出现游戏的 `UNIMPL` |

**M2 —— 进行中（Vulkan 回放层），以下每条都有真机实测**

| 步骤 | 结论 |
|---|---|
| 1 Vulkan 通路与内存模型 | Adreno 830 / Vulkan 1.3.284 / 零拷贝内存类型 4，CPU↔GPU 双向通 |
| 2 零拷贝内存池双向验证 | 可行性分析里的**头号风险**「统一内存」在真机上验完 |
| 3 内存池落法定型 | 本机**没有 `VK_EXT_external_memory_host`** → 改成由 Vulkan 铺客体地址空间：可导出内存 → 取 fd → `mmap(MAP_FIXED)` 压回原地址。单次分配上限 3968 MB > 客体堆 3728 MB ✓，惰性提交、页对齐偏移绑缓冲双向通 |
| 3 内存换轨落地 | 堆 3968 MB、别名区 512 MB 已是 GPU 直接可见；`SetHeapSize 3728 MB` 照旧，引导进 `nnMain` |
| 回放层第一批 | 池子登记 + 地址反查（真实池子 #1…#7 全部登记、零报红） |
| 启动缺陷三个 | ① NVN 实现表在 ELF 上登记不上（Mach-O 段名）→ **语义 0 → 203**；② xattr 名不合 Linux 规矩 → 加 `user.` 前缀；③ `clonefile` 替身只拷单文件 → 改递归（含 xattr） |
| 前端 sig=11 | 根因：客体按原版 libc 量的 **96 KB 线程栈**被宿主 bionic 压穿（压栈落进栈底 PROT_NONE 页）→ `MapMemory` 在栈长一侧多放开 1 MB。修复后 **5285 帧 / 177 秒无崩溃**，SIGTERM 优雅退出走完 |
| 回放层提交机制 | 提交槽（8 个轮转）+ 栅栏 + 命令流解释器（与 Metal 侧同形的小程序）；CPU 可见副作用按栅栏时序生效。真机 4179 帧无崩溃 |
| op 分布实测 | 每窗口约 800 万条：`BIND_STATE` 27.8% / `BIND_UBO` 21.9% / `STENCIL` 10.3% / `BIND_TEX` 5.3% / `BIND_VBUF` 5.2%（前五合计 70.5%，全是状态与资源绑定） |
| 渲染目标的 view 落地 | `p_set_rt` 的 `cview/dview` 进影子状态：子视图 **0 → 33 张**、命中 34,193 次；渲染区按附件钳位（真机 1600×900 对 800×450，正好一级） |
| 出图截图链路 | 自带最小 PNG + 驱动侧读回 + `mtl_present` 落地；`DM_NVN_SHOT/ALL/SCAN` 与苹果侧同名同义。真机写出 1600×900 PNG（5,761,403 字节），取回能打开 |
| 截图照出的两处真 bug | ① 交换链贴图**没有客体内存**（存储由驱动持有）→ 纹理层拒建图 → 渲染目标**静默跳过**（现在按"存储归属"登记）；② **附件建不出图像曾静默跳过**（现在会出声）。顺带接通 `COPY_T2T`（真机第一次就是 1600×900 全屏） |
| **第 14 步：量出的结论** | 第 300/800 帧各扫 64 张 ≥256×256：能读的颜色目标**全零**、出图纹理**全白** → **GPU 上还没有写出颜色内容**；卡点是渲染链路（两条告警分别指向顶点接槽与描述符回退成空白） |

**下一步（按实测频次排序，含已完成项）**

1. 绑定类 op 做成**影子状态** ✓（占 70%，绑定只是记状态、几乎免费）；
2. 纹理层 ✓（格式表、序号建表、解排布上传、CPU 直读）；
3. 渲染状态影子 + 通道机械 ✓（`CLEAR` 第一次真正落到 GPU）；
4. 着色器装载 ✓（本节第 8 步）；
5. 管线层 ✓（本节第 9 步：描述符集布局、管线布局、计算管线、图形管线）；
6. 回放层的影子绑定存储 + 状态解释 → `DRAW` / `DRAW_IDX` 真正下发 ✓（本节第 10 步：真机 122 万次绘制、83 条管线、零失败）；
7. 贴图与采样器按 NVN 槽解析 ✓（本节第 11 步：从 handle 精确解码，贴上 52 万条、"采样器号查不到" 0）；
8. view 落地 ✓（本节第 12 步）；
9. **渲染目标的 view** 接进影子状态 ✓（本节第 14 步：子视图 0 → 33 张、命中 3.4 万次；渲染区按附件钳位）；
10. **离屏读回一帧写盘** ✓（本节第 13 步：图片真的落盘，5,761,403 字节的合法 PNG，取回能打开）；
11. 用它量"画面在哪" ✓（本节第 14 步：**没有颜色内容** → 卡点转为渲染链路：顶点接槽 + 描述符回退成空白）；
12. **描述符回退成空白**清空 ✓（「M3-2k」：UBO 名字号 −3、SSBO 由 cb_offset 反推、以及**缓冲区贴图**这个新资源种类；
    真机三处空白计数全部归零，且 `DM_NO_TEXEL=1` 能一键复现回去）；
13. 缓冲区贴图落地 ✓（「M3-2k」：`target 10` → texel buffer，与苹果侧同源；**tid 701 的注册项目标确实是 10**，
    格式 `R32G32B32A32_SFLOAT`、65536 个元素、直接建在池缓冲上）。

另有一处**已知潜伏**（已记在提交说明里）：安卓 `crash.c` 的 `is_write` 恒为 0，而脏页跟踪的缺页钩子先看这一位，所以池内被写保护的页一旦被写会走崩溃路径；现场诊断已能读出出错指令编码，据此可正确判读读/写。

### M2 第 8 步：着色器装载（在设备上逐份验过）

**键与苹果侧是同一条算法**：键 = `sha256(控制段 0x880 字节)` 的前 16 位十六进制（`nvn_mtl.m` 的 `prog_get` 就是这么算的）。
运行时 `nvnProgramSetShaders` 拿到的是控制段指针，回放层据此算出与产物同名的键。**这一条差点成了拦路石**：安卓兼容层的
`CC_SHA256` 原本是**占位实现**（输出定长零），那样所有着色器会算成同一个键、一条管线都建不起来；已换成 FIPS 180-4 真实现，
并用标准向量在真机上验过（`dm/android/tests/t_digest.c`，7 项全过）。

**反射的来源与苹果不同**。苹果读 `dmsh_build.py` 出的 `<键>_b.json`（含 MSL 类型、c1 段、驱动段读法——那些是 Metal 参数缓冲
才需要的）；安卓读翻译器（`tools/dmsh_xlate`）直接给出的 `<键>.res.json`。后者只讲一件事：**每个 SPIR-V binding 对应客体哪一份
资源**（uniform 缓冲的 c 槽、存储缓冲地址在 `c[cb_slot][cb_offset]`、贴图/采样器句柄的来处），正好是建描述符集所需要的最小集。
仓库里没有 C 的 JSON 读取器（`nvn_refl.h` 走的是 Foundation），所以带了一个极小的扫描器：只服务这种机器生成的规整格式，
一次遍历按需取字段。

**真机自检**（`dm/android/vk/vk_shader_probe.c`，把目录里每个 `<键>.spv` 都真的建成 `VkShaderModule` 并解析同名 json）：

```
【绿】着色器产物就绪：/data/local/tmp/dm/spv（6923 个 .spv，6923 个 .res.json）
阶段分布：顶点 2343、像素 4556、计算 22、其它 2
绑定合计：uniform 34890、存储 11379、贴图/图像 44156（6923 个着色器）
装载：成功 6923、缺产物 0、坏产物 0        耗时 8.5 秒
✓ 全部 SPIR-V 都能被这台驱动收下，反射都能解析
```

**这退掉一个大风险**：Adreno 830 对全部 6,923 份翻译出来的 SPIR-V 一份都没拒。后面管线再出问题，就只剩状态机的事了。

**离线互证**：产物文件名与 `shader_map.tsv` 的 `dm_ctl_sha16` 列比对——表里 6927 键 / 产物 6923 键，**阶段计数几乎完全一致**
（表：顶点 2344 / 像素 4559 / 计算 22 / 外壳 1；产物：顶点 2343 / 像素 4556 / 计算 22 / 其它 2），说明键与阶段的推导跟仓库那张表
是同一套。表里有而产物没有的 5 个键，回放层按设计**报红并计数**（硬边界不变）。

**产物怎么放到设备上（三种方式都量过，Redmi K90 / f2fs）**：

| 方式 | 本地准备 | 推送 | 设备上展开 | 每次部署合计 |
|---|---|---|---|---|
| 直推全集（20,770 个文件 / 665 MB） | 0 | 37.1 s（1.8 ms/文件，17.9 MB/s） | 0 | 37 s |
| 直推运行时子集（13,847 个 / 606 MB） | 0 | ≈25 s | 0 | ≈25 s |
| **tar 缓存后推**（616 MB 单文件） | 一次 60 s（可缓存） | **5.0 s**（122 MB/s） | **1.9 s** | **≈7 s** |
| 预热子集（`pso_manifest` 的 179 个着色器 / 358 个文件） | 0 | **1.4 s** | 0 | 1.4 s |

三个反直觉的点：**小文件推送并不慢**（1.8 ms/文件，20,770 个只要 37 秒）；**单文件快约 7 倍**（122 vs 17.9 MB/s），所以 tar
的价值只在**反复部署**时体现；**设备展开几乎免费**（13,848 个文件 1.9 秒，f2fs + toybox tar）。两种方式对运行时毫无差别——
最终都是同名普通文件——所以**没有为它写任何打包或索引代码**。缓存的 tar 放在 `.work/spv.tar`（`.gitignore` 覆盖；里面是着色器，绝不进仓）。

### M2 第 9 步：管线层（描述符集布局 + 计算管线 + 图形管线）

**这一步真正的收获是三条"谁说了算"的结论**：

- **描述符集布局必须覆盖着色器用到的每一条绑定，而 `<键>.res.json` 不保证列全**——它讲的是"这份资源的数据从客体
  哪里来"，不是"SPIR-V 里这条 binding 是什么类型"。证据：22 条计算管线里 3 条建不出来（`VkResult -13` =
  `VK_ERROR_UNKNOWN`）；把失败者的 `.spv` 里**声明的**绑定读出来对照，那 3 条都在 `set0/b0` 多声明了一个**被用到**
  的 uniform 块、res.json 里没有它；反例是同批通过的一条，同样多声明 b0 但**没被用到**，所以建得成——缺的是
  **覆盖**而非校验（驱动并不逐条比对布局的类型）。
- 于是新增 **SPIR-V 声明扫描器** `dm/android/vk/vk_spv.{h,c}`：只走声明部分（注解 / 类型 / 变量，遇第一个
  `OpFunction` 即停），读出每条描述符变量的 set / binding / **种类**。`vk_shader.c` 在**装载时**扫一次并把清单存下
  （SPIR-V 字节此后就丢掉，那是唯一时机）；布局**以它为准**，`res.json` 回到"数据从哪来"的角色。
- 交叉核对给出一个整齐的数：**SPIR-V 有而 res.json 没有的绑定共 6923 条，正好每份着色器一条**——就是翻译器注入的
  b0 块。

**两个 bug，都是"两个独立实现互相矛盾"逼出来的**（值得记一笔：这条路子比单看日志有效得多）：

- `OpDecorate` 的字长是**分情况**的：`Binding` / `DescriptorSet` 带一个操作数（4 个字），而 **`Block` / `BufferBlock`
  不带**（3 个字）。我按 4 个字统一判，这两种装饰就被整条跳过——真机上表现为"所有结构体类绑定都判成认不出来"
  （uniform / 存储缓冲 → other），而图像类全对（它们的种类来自类型声明、不靠装饰），正是这个组合暴露了它。
- SPIR-V 1.0–1.2 里存储缓冲是 `Uniform` 存储类 + `BufferBlock` 装饰，不是 `StorageBuffer`（1.3 起）——第一版扫描器
  把全库 SSBO 都标成了 UBO，据此得出的第一版结论是错的。

**映射**（与苹果侧同一套语义）：uniform 缓冲 → `UNIFORM_BUFFER_DYNAMIC`、存储缓冲 → `STORAGE_BUFFER_DYNAMIC`
（数据就在零拷贝池里、每次绘制只换偏移，与苹果侧的 `setBuffer:offset:` 同义）、贴图 → `COMBINED_IMAGE_SAMPLER`、
可写图像 → `STORAGE_IMAGE`；全库没有数组绑定（44,156 条实测 `array` 都是 1），所以 `descriptorCount` 取 1。

**顶点属性的对应关系**：图形管线要为着色器声明的**每一个**输入配上属性（少一个就直接建不出管线），而这份信息既不在
`.res.json` 也不在 `shader_map.tsv` 里。做法是把**输入变量**也从 SPIR-V 读出来（Location + 分量个数/类型），再核对
"槽位号 ↔ NVN 属性号"是否同域：`shader_facts.tsv`（翻译器的作答表，按槽位给分量类型）里全库 **11,166 条属性、出现的
最大槽号 = 7**，落在 NVN 属性域（0..15）内 → 按 **location == NVN 属性号** 推进，并加一道**运行时格式交叉核对**
（客体绑的顶点格式 vs 作答表的分量类型），假设若错，第一次绘制就会报出来。

**真机结果**（`dm/android/vk/vk_pipe_probe.c`）：

```
描述符集布局 / 管线布局：成功 6923、失败 0（按内容去重后 334 个）
计算管线：成功 22、失败 0
图形管线（顶点着色器各建一条、属性走常量回退）：试 60、成功 60
交叉核对：SPIR-V 有而 res.json 没有的绑定共 6923 条
耗时 4.2 秒
```

**已知缺口（分层处理，全部留日志、不静默）**：

| 缺口 | 处理 | 依据与影响范围 |
|---|---|---|
| 非实心多边形模式 | **已关闭** | 先问再开核心特性 `fillModeNonSolid`（真机有）→ 原样传；只影响线框类绘制，正常渲染本来都是实心 |
| 顶点流每实例步进 > 1 | **已关闭** | 开 `VK_EXT_vertex_attribute_divisor`（真机有，且含 `divisor=0`）→ 步进原样传；NVN 的"常量流"（步长 0）因此有了正确表达（每实例一个元素、不随顶点推进），不再借常量回退 |
| `descriptorCount` 写死 1 | **已关闭** | 改为读 SPIR-V 声明的元素个数（`OpTypeArray` 的长度常量）；实测全库 **0 条数组绑定**，行为不变，但"碰巧不是数组"变成了"读得出是不是" |
| 图形管线缓存淘汰静默 | **已关闭** | 淘汰时报一次黄；频繁淘汰说明状态组合太散，值得再看 |
| 顶点属性分量类型 | **已兜底** | 建管线时比对"影子格式的分量类别（float/uint/sint）"与"着色器声明的输入类型"，不符报一次黄——上面那条"location == 属性号"是**假设**，若错，顶点数据会接到错的槽**且不报错**，有了这条核对第一次绘制就现形 |
| 常量回退的哑缓冲 | **已备好** | `vk_pipe_dummy_buffer()` 懒建 64 KB 全零顶点缓冲，绘制时绑到 `VK_PIPE_DUMMY_BIND`（回退属性每实例推进一个元素、实例数由游戏定，所以不能只给 16 字节） |
| 四种无对应物的图元 | **先量后定** | `vk_topo_of` 按图元号累计绘制次数（`vk_pipe_prim_counts`），由真机回答它们出不出出现，出现了再决定要不要写展开；计数器只读，不改绘制行为 |
| 两个 set（顶点 set 0 / 片元 set 1） | 已落地 | 见第 10 步：两个阶段抢同一个 binding 号时 Vulkan 无解（一条绑定只有一个类型），按**阶段**分 set 是最小的正确修法（装载时改写 SPIR-V 的 `DescriptorSet` 装饰）|
| 模板掩码进管线键 | 保持不变 | 苹果侧 `pso_key` 同样带 `vm`/`wm`，行为与它一致，不是缺陷 |

设备就绪行随之多出两项能力（真机 Adreno 830）：`非实心填充 有；顶点每实例步进 有（含 0）`。

**下一步**：描述符写入与真下发 ✓（见第 10 步）→ 贴图按 NVN 槽解析 ✓（第 11 步）→ **离屏读回一帧看图**。

### M2 第 10 步：描述符写入、真下发与 GPU 计时（三点都在真机上量过）

**描述符写入**：每个**在途提交槽**一个描述符池，槽被回收（栅栏已过）时整池重置——回放层本来就有"在途槽 + 栅栏"
那套（`g_inf`），所以不需要任何额外同步。缓冲一律**动态偏移**（客体数据就在零拷贝池里，`mtl_addr` 给出(缓冲,偏移)）；
**动态偏移数组的顺序与布局同源**（同一张 `vk_pipe_binds` 表，不在两边各算一次——算两次迟早会漂）。地址为 0 或解析
不出来时**写空白**：未写的描述符内容不确定，而 NVN 自己在空槽上读 0，所以空白是忠实的回退。

**真机第一次真下发**：**122 万次真实绘制、83 条图形管线、零失败、无 GPU 挂起、优雅退出**。这一批顺带让**我自己的
一条诊断告警抓到一个真 bug**：NVN 的索引类型是 `0=8 位 / 1=16 位 / 2=32 位`（`nvn.h` 的 `NVNindexType`），而我先前
按"2/4 字节"猜过——真机用的是 1，**恰好**读对了 16 位，但 32 位会被读成 16 位；已按 `nvn.h` 修正，8 位索引（Vulkan
核心不支持，要 KHR 扩展）报出来并跳过该次绘制（与苹果侧一致）。

**GPU 计时：从"空桩报 0"变成真测量**。安卓侧 `mtl_gpu_ms` 原先**是空桩**（`return 0`）——所以日志里的
"GPU 0.00 ms"从来不是测量结果，而且**没人会觉得奇怪**，因为 0.00 看起来正好象"GPU 没干活"。现在每次通道开始
（真的开成之后）与结束（收之前）各写一个时间戳（`vkCmdWriteTimestamp`），槽被回收时读回，取"最后一次结束 −
第一次开始"作为这次提交的 GPU 跨度（TBDR 上通道之间是流水的，跨度不可相加——苹果侧那张通道表写着同样的话），
再累进按帧的环（1024 项）并按帧号回查、未全部完成返回 -1（与苹果侧 `GPUStartTime/GPUEndTime` 同一套语义）。
前提条件按规矩问：**本队列族**的 `timestampValidBits`（不是设备级属性）与 `limits.timestampPeriod`。
真机结果：`时间戳 48 位有效、每刻 52.08 ns`，随后每 300 帧一行 **`GPU p50 6.02 / p95 6.07 / 最大 8.02 ms`**
（帧预算 33 ms = 30 fps）。`-1` 语义也在真机上生效：首个窗口只报 292 帧，另有 7 帧尚未完成被正确排除。

**贴图格式表按 `nvn.h` 的枚举补全**：从真机日志报出的四个缺项（`0xb/0xd/0x6/0x1`）顺着 `NVNformat` 枚举逐项补到
**154/157**，含 **ASTC**（Switch 贴图的主力压缩格式，Adreno 原生支持）与 BGR 系列；32 位的 `UI2F/I2F` 在 Vulkan 里
**没有对应物**（`_USCALED/_SSCALED` 只有 8/16 位通道有），映射成 UINT/SINT 会让着色器读到整数而不是浮点——**不猜**，
报出来。离线反向核对（`case` 值 ↔ 枚举）零拼错。

**贴图注册表**：宿主在游戏注册贴图时调 `mtl_reg_tex`（安卓侧原本是空桩），现在**按值**存副本（游戏会按值拷贝
`NVNtexture`，存指针会失效）；配合同名注册的采样器表 `mtl_reg_samp`（同样是空桩转正）。

### M2 第 11 步：贴图与采样器按 NVN 槽解析（真机首次绑上贴图）

**关键的一环是"binding ↔ NVN 槽"**：SPIR-V 里的贴图 binding 与 NVN 的贴图槽不是一回事，而 `<键>.res.json` 里没有
这个对应关系（苹果侧那份反射有 `reg`，安卓这份没有）。走了两步：

1. **先靠统计配对**（按维度分组、组内按顺序）：用 314 份"单贴图 + 单槽"的**强制配对**推出 `type 3→2D`，补上数量
   消去得到的 `4→3D`、`2→cube` 后全库 44,135 条自洽 98.4%，并做了**反例对照**（故意弄错一条就掉到 65.1%）——所以
   这个一致性指标有分辨力，不是自说自话。
2. **再换成精确解码**（并行探查的结论）：`binding` 其实是**翻译器自己的密集计数器**（`Program.cs` 里四个回调共用
   一个 `_binding++`），而 **`handle` 里本来就带槽号**，公式在仓库里已有两处实现（`dmsh_build.py` 的 `decode_tex`
   就是苹果侧产物用的同一套；`maxwell_refs.py` + `README.md` 是全量实测的约定：贴图槽 r 在 `c2[0x168+8r]`、
   **采样器槽 r 在 `c2[0x568+8r]`**、图像槽在 `c2[0x120+8r]`）。句柄字拆成 `o0/o1` 两个字，**`ty≠0` 时两个字都要试**
   （分离采样器的那一半在 o1 里，只试 o0 会漏掉约 7%）。**三方对账**：抽样 800 份着色器 / 5199 条贴图条目，贴图槽
   **100% 落在 `shader_facts.tsv` 的槽集合内**（那份表来自 PC 反射，是独立来源）、解不出 0 条、采样器槽全部落在
   0..10——与真机上"非零采样器槽 0..10"的现场吻合。真机上两条独立路线也互相印证：解码 414/败 9 与统计配对
   409/14 几乎逐项相同。

**采样器**：贴图槽与采样器槽是**两套编号**（两个句柄表），所以采样器必须按**采样器槽**取；句柄高 20 位是采样器池号，
`0xFFF` 是"填空槽"的哨兵。真机结果：**贴上 52 万条、写空白 2.8 万条、"采样器号查不到" 0 条**——只要采样器槽上真绑了
东西就一定解析成功；那 2.8 万条空白与"空槽"数几乎相等（着色器声明了、这次绘制不用），写空白正是空槽的对应。

**两处我自己的错，都是被实测/探查纠回来的**（记下来备查）：① 早期版本的"该阶段一张贴图都没绑"是**坏样本**
（只打印了前三个槽就下结论，实际绑了 36 张）；② 我曾把"贴图槽去查采样器表"读到的 `0xfff00000` 当成**空槽哨兵**
并"结案"，而真相是**查错了槽**——两套编号。

### M2 第 12 步：view 落地（按游戏的意思采样）

NVN 的 texture view 能**限定 mip 范围**、**限定层范围**、**重解释格式**、**重解释目标类型**（例如 3D 当 2D 数组看）。
这一层以前**完全没有 view 参数**：注册时把 view 存下来了（`treg_e.has_v/v`）**却从没读过**。现在
`vk_tex_view(const tex_t*, const view_t*)`，每张贴图按 view 内容缓存最多 4 张子视图，`subresourceRange` 取自
`base_level/nlevels/min_layer/nlayers`，格式取自 `set_format`、`viewType` 取自 `set_target`；建图时补
`MUTABLE_FORMAT`（重解释格式的前提）与 `2D_ARRAY_COMPATIBLE`（只对 3D 加）。渲染目标那两处暂时仍传底视图
（`p_set_rt` 的 view 还没进影子状态），已在代码里注明。
**真机读数**：`视图：底视图 234,866 次、命中子视图 0 次、新建子视图 0 张` —— **一张子视图都没建**，而这是
**自解释**的：游戏给"会被采样的贴图"注册的都是 `TextureViewSetDefaults`（全 0 = 不限定），真正限定了 mip/层的
view 属于**渲染目标**（`SetLevels/SetLayers` 各有 2369/2030 次调用），而 SET_RT 那一路我明确还传底视图——所以
这个 0 正好指出**下一步该接哪里**：把 `p_set_rt` 的 `cview/dview` 接进影子状态，并把"限了 mip 的附件尺寸不同"
一并处理。

**同一次真机跑还量了那个判据**：`ReadTexels 读到 GPU 写过的贴图 0 次` —— 这一窗里游戏**没有**读回被 GPU 写过的
贴图，所以"从 VkImage 读回"那套暂时仍只是**调试工具**，不是功能缺口（离屏截图正靠它）。

### M2 第 13 步：出图截图链路（自带最小 PNG + 驱动侧读回 + 出图/按需截图）

**这一步是为了"看得见"**：M2 的验收就是"屏幕出画面"，而没有窗口（M3 才有）时唯一的办法是把一帧读回磁盘。四块：
① **`vk_png.c` —— 自带的最小 PNG 写出**（8 位 RGBA、DEFLATE 存储块、零依赖），因为图必须落成"能直接打开"的文件，
而安卓侧不能链苹果的图像框架；② **`vk_tex_grab_ser` —— 从驱动侧读回**（拷进宿主可见缓冲再映射），与"直读池"
互补：GPU 写过之后池就不是真值；③ **`mtl_present` 落地** —— 客体出图时会把**交换链纹理**交出来，这是"游戏要显示
哪张画面"的**权威来源**（不必在几百张渲染目标里猜）；④ `DM_NVN_SHOT=<帧>:<路径>`、`DM_NVN_SHOT_ALL`、
`DM_NVN_SHOT_SCAN`、`dm_gfx_shot_at`，与苹果侧**同名同义**，脚本与经验可以直接搬。

**真机：图片确实落盘了** —— `截图 1600x900 格式 0x25 → shot_900.png 成功`，5,761,403 字节 = 1600×900×4 + PNG 头，
取回后能直接打开。而它第一次用就**照出两处真 bug**（都已修）：① **窗口（交换链）贴图没有客体内存**（存储由驱动
持有），而纹理层见"没有内存"就拒绝建图 → 渲染目标那一侧只会**静默跳过**它；现在"窗口贴图"是一种**存储归属**登记
（`vk_tex_mark_window_ser`，客体建窗口时报过来），照样建图、跳过上传、直接进"GPU 写过"那一档；② **附件建不出图像
曾静默跳过** —— 这正是①藏了这么久的原因，现在它会出声。顺带接通 **`COPY_T2T`**（贴图→贴图拷贝，先前只是计数桩），
真机上它的第一次就是 **1600×900 全屏** —— "把画面搬进某张贴图"走的正是这条路。

### M2 第 14 步：量出来的结论 —— **GPU 上还没有写出颜色内容**（M2 验收的实际卡点）

仪器到位后第一次用它回答"画面在哪"，答案是**没有画面**（第 300 与第 800 帧各扫一遍，每轮 64 张 ≥256×256）：

| 看的对象 | 结果 |
|---|---|
| 能读的颜色渲染目标（序号 28 `RGBA16F`、序号 23 `B10G11R11`，均 1600×900）| **全零**（原始峰值 0.000）|
| 出图纹理（交换链序号 5/6/7）| **全白**（亮度均值 255，第 300 与第 900 帧一样）|
| 其余 62 张大贴图为何读不了 | **没有一张是画面**：BC3 压缩（格式 138）、深度/模板、`R32_UINT` 整数、`R16_UNORM` 单通道 |

**结论**：这一窗里画了 **99 万多次**绘制，目标上**一个像素都没落下** → 卡住的是**渲染链路本身**，不是"看不见"
（同一条读回链路已经写出过合法的 1600×900 PNG）。日志里同时有两条**指向原因**的告警：

- `binding N（combined image sampler）贴图写空白：res.json 说 cbuf_slot=-N …` 与
  `binding N（uniform buffer）拿不到缓冲地址（地址 N、槽解析 N）：写空白`
  → 描述符**大量回退成空白**：着色器取到的是空贴图 / 空常量；
- `顶点属性 N（… 位置 N）的分量类型与着色器声明不符：格式 NxN vs 着色器 float`
  → 顶点数据接的槽可能不对，几何可能整片塌掉。

**另记**：为了让"扫一遍"真能用，读回支持了本机实际用到的格式（8 位 RGBA/BGRA、`RGBA16F`、`RGBA16_UNORM`、
`RGBA32F`、`B10G11R11_UFLOAT` —— 编号是**对着 NDK 头文件查实**的，不是猜的）；每次截图都打**亮度均值/最小/最大**
（"白"与"黑"从此是日志里的数）；中转缓冲常驻后每张读回从约 0.45 秒降到可忽略；扫描只扫 ≥256×256（画面不可能
是一张 16×24 精灵图，而每读一张都要等一次队列）。

### M3 第 3 步：从"一个像素不落"到"真的画进了目标" —— 七个坑（2026-10-14）

这一轮把"绘制发出去了、目标却全零"这件事拆开了。先做两个**不改代码**的对照实验把范围钉死：`DM_GFX_PROBE=<序号>`
（把一张目标清成已知的蓝再读回）读出**亮度 152.0**，与清除色 (0, 0.6, 1.0) 经伽马换算后的 (0, 201, 255) 均值分毫不差
→ **读回链路可信，"全零"是真的**；`DM_NO_DEPTH_TEST=1` 仍全零 → 深度测试不是那道门。之后逐个是：

| # | 症状（真机读数） | 根因 | 修法 | 验证 |
|---|---|---|---|---|
| 1 | 34 万次绘制、目标全零、零报错 | **安卓侧没有 NVN 默认状态**：影子是零初始化的，而 `gfx_state_fill` 无条件读它 → **写掩码 0（一个通道都不写）**、深度范围 `[0,0]`。游戏有权不设这些（NVN 有默认值），苹果侧正是为此写了 `state_reset()` | 加 `state_defaults()`（照 `nvn_mtl.m:2306-2314` 逐项对齐），在**第一次 `mtl_exec`** 生效（与苹果侧同位置）；`OP_VIEWPORTS` 不再去读那份从来没被录进来的 `v[4]/v[5]`，近远只来自 `SetDepthRange`（默认 `[0,1]`） | 序号 27 从 **0 → 255**（有内容了） |
| 2 | 日志里成片的"目标 1 个 … **未下发**" | **通道只在清除时开**：文件头的通道机械写的是"第一次清除**或绘制**时开"，但代码里只有 `CLEAR_COLOR`/`CLEAR_DS` 会开 → `SET_RT → 屏障 → 绘制` 这类流里绘制被整片丢掉 | 绘制路径补 `pass_begin()`，并在开通道时补写动态视口/裁剪（`VIEWPORTS`/`SCISSOR` 到达时通道还没开） | 被丢弃的绘制 **48% → 31%** |
| 3 | `binding N（uniform buffer）拿不到缓冲地址（地址 0、槽解析 N）：写空白` | **常量槽号不是 res.json 直接给的那个数**。规则（苹果侧同源，`dm/gfx/tools/dmsh_build.py` 的 `reflect()`）：名字形如 `fp_c3` → NVN 常量槽 = **号 − 3**；名字里的 `1` 是特殊段 `c1`、`2` 是驱动段，都不是普通 UBO；一条 `cbuf` 都没有 = Ryujinx 的 `support_buffer`。用真 SPIR-V 名字核对：`fp_c3`→槽 0、`fp_c13`→槽 10，而游戏绑的正是 **0、3、10**（槽位图 `0x409`）—— 我们拿 1/3/13 去查，只有 3 撞对 | `desc_buf_range` 按上述规则解析；另造两块 64KB **特殊常量段**（`support_buffer` 的 `render_scale = 1`，为 0 会让着色器按 0 缩放；`c1`/驱动段先给全零——真实 `c1` 数据在控制段里，见"剩下"） | UBO 写空白**消失**；阶段绑定计数 6/6/3 → **1094/3367/695** |
| 4 | 查看器一开就崩（驱动内 `vk_tex_view`）、前端约 3 秒后不再刷新、客体中途停住 | **两个线程共用一条 VkQueue 且没有外部同步**。Vulkan 要求 `vkQueueSubmit` / `vkQueueWaitIdle` / `vkAcquireNextImageKHR` / `vkQueuePresentKHR` 对同一队列由调用方串行化，而这里客体在后台线程提交、前端在主线程上屏与读回。三种形态是同一件事 | `vk_device.c` 加 `vk_queue_lock/unlock`，包住提交、等队列空闲、取图像、上屏 | 见下（这一条还没定性完） |
| 5 | 上面那条修完后，**两端一起停住**（等待点显示客体"正在录制、通道开着"，前端的 1 秒心跳也没了） | **取图像不能长占队列锁**：屏幕关掉 / 合成器忙时交换链可能长时间没有可用图像，锁被它占住 → 客体提交全堵在那里 | `vkAcquireNextImageKHR` 带 **100 ms 超时**，超时放掉这一帧、下一帧再来 | 待长跑复验 |

**仪器与文档本身的两处坑**（同样要记住，否则下次还是抓不到现场）：

6. **安卓侧一直没注册"等待点转储"**。苹果侧在设备初始化时注册 `mtl_wait_state_dump`（`nvn_mtl.m:275`），安卓侧没有 →
   客体停住时日志里只有心跳，"卡在哪"看不出来。现在 `nvn_vk.c` 补了同名同义的一件：在飞槽数与各槽栅栏值、正在录制的槽、
   通道是否开着、提交与出图次数。**第一次用它就把"客体卡在录制中、通道开着"读了出来**。
7. **看门狗默认不开，而且转储的尾巴会被 `_exit` 抢掉**。`DM_WATCH_STALL` 不设就完全不看（产品默认，见 `crash.c:314-325`）；
   设了之后它打完整份现场就 `_exit`，而安卓的宿主 stderr 是**管道**、落盘由另一条线程（app 的日志泵）做 ——
   实测转储的**尾巴被整段切掉**（"等待点"那行正好在尾巴上）。修：`crash.c` 退出前给日志 400 ms（仅安卓分支）。

8. **安卓侧只有"等待状态"，没有 PC / 回溯**。同一份现场导出里，线程行（在等什么、最近一次 svc 从哪发出）是好的，
   但紧跟其后的 `寄存器 pc=… x19=…` 与"回溯"**全是 0** —— 因为 `thr_bt`（`hle_thread.c`）用的是 mach 的
   `thread_suspend` / `thread_get_state` / `module_of`，安卓没有这些，它又不报错，于是静默打零。
   现在看"客体在跑客体代码"这件事只能靠 `最近 svc` + `lr=模块+偏移` 两个字段推。
   **已补**：`hle_thread.c` 增加安卓分支（`proc_thread_line`），每个客体线程按 `/proc/self/task/<tid>/` 报三样 ——
   `stat` 的状态字符（R 在跑 / S 睡 / D 不可中断等待）、`wchan` 的内核等待点符号名（`futex_wait_queue`、
   `__arm64_sys_nanosleep`、`pipe_read` …）、`syscall` 的**号 + 参数 + 栈指针 + PC**（阻塞在系统调用里才有；
   纯用户态空转时它只给 `running`）。**这两者正好是最需要分开的**。顺带把宿主自建线程也列出来了
   （原来那段走 mach 的 `task_threads`，在安卓上是空跑，一份现场里只有客体线程）。

**它立刻把"客体停住"定性了（2026-10-14 真机）**：停在 `CharacterCreate:Hover:Barbarian` 时 ——

| 线程 | 读数 | 说明 |
|---|---|---|
| 客体主线程 0x1001 | **状态 R、`系统调用 running`** | **纯用户态空转**，不在任何系统调用/等待里 |
| 其余客体线程 | `S` + `futex_wait_queue` / `__arm64_sys_nanosleep` | 全在睡（正常空闲） |
| 我们这一侧（等待点） | 没开通道、没在录制、在飞槽 1/8 | 回放层空闲，不欠谁 |
| 进程主线程 / 日志泵 | `S` 在 `do_epoll_wait`、`R` 在 `pipe_read` | Android 的主线程与日志泵，都是正常等待 |

也就是说：**这不是"卡在驱动或锁里"**（那会是 S + futex），而是**游戏自己在用户态空转，等一个永远不会完成的作业**
—— 与苹果侧注释里那条已知现场同形（主线程在作业池"边帮忙边等"里空转，`nvn_mtl.m` 的 `thr_bt` 注释）。
下一步要拿它的 PC：安卓没有 ptrace 权限，只能走**信号采样**（给自己的线程发信号，在处理器里取 `ucontext`
的 pc，再按模块 + 偏移符号化）。

**采样器（`DM_SAMPLE=<毫秒>[:线程]`）在安卓上的实测**（仓库里已有，没改代码，记录备查）：它就是"给自己的线程发 `SIGUSR2`、
在处理函数里把 pc / lr / 帧指针链拷进环形缓冲、由采样线程符号化打印"。**能用**（真机拿到过 `thr=0x1001 pc=… lr=ioctl+0xa0`，
说明客体主线程当时正频繁进出系统调用），但两处要当心：

- 间隔 **10 ms** 时进程在启动 **1.4 秒**就 `CRASH sig=11`（`pc` 落在 libc 的 svc 桩上，疑似打断点落在 `svc` 指令里）；
- 间隔 **100 ms** 时样本到约 **9 秒**就不再产生（此后客体一直没被信号打断）—— 那一刻起信号被客体屏蔽、或目标线程
  已不在用户态，所以它现在还回答不了"空转转在哪个函数"。

要让它可用得改两处：① 只在 `/proc/self/task/<tid>/syscall` 报 `running`（确实在用户态）时才发信号；② 宿主模块的 pc
也要能符号化（现在只认客体模块 `main` / `sdk`）。另外处理函数里那段帧链走 `vm_read_overwrite`，安卓上恒为空，回溯拿不到。

**当天已补**：① 常开采样改成"只在 `/proc` 报 `running` 时打断"；② 新增**卡住时的一次性采样**
（`crash_set_stall_sample`，看门狗与 `crash_dump_now` 调用）：只对"状态 R 且不在系统调用里"的线程各发一次信号；
③ `crash_off` 在动态库没有符号名时也报出**哪个 .so + 偏移**（安卓厂商驱动就是这样，原先只给裸地址）。
**第一次用它就抓到**：在用户态空转的是**前端线程**（`thr=0`、`pc=libc.so+0x991ac`、返回点 `vk_win_present_rgba+0x260`）
—— 也就是卡在我新加的那把**队列锁**上（客体在队列调用里等驱动时仍持锁 → 前端卡在取锁上，1 秒心跳同时消失）。

**随后的修法**：给窗口**单独一条队列**（能呈现的族上的第二条，`vk_queue_win`），前端取图/上屏不再与客体抢锁；
只有该族只给一条队列时才退回共用并加锁。真机读数 `两条队列：客体用 #0、前端（取图/上屏）用 #1`，**前端心跳不再消失**；
客体仍停在第 59 次出图，但那已经是客体侧的另一件事（等待点显示我们这侧空闲：没开通道、在飞槽 1/8、队列锁无人持有）。
顺带给锁加了"持有者 / 在做什么"（`vk_queue_note`），等待点转储里会打出来，下次互堵一眼可见。

**一条被证伪的假设（值得记下来，省得再走一遍）**：真机停在 `CharacterCreate:Hover:Barbarian` 时，怀疑是**客体在加载
游戏数据、卡在磁盘 I/O**。判据做了两个：① 把 `DM_WATCH_STALL=0`（看门狗只记不杀）长跑，计数**没有恢复**（不是慢，是真停）；
② 更硬的一条：停住后**连前端那条 1 秒心跳也一起没了**（静默 70 多秒）—— 若只是客体在等 I/O，前端不会停。
现场是：主客体线程 `最近 svc 0x1a`（ArbitrateLock）、`等:运行/未在等`、在飞槽 1/8、**通道没开**、无崩溃。
也就是说：**两个线程一起停在某个驱动/锁的等待里**，下一步要把安卓侧的 PC/回溯这条仪器补上才能继续往里走。

### M3-2k：缓冲区贴图（texel buffer）落地 —— 交接单那条判定动作连同正解一起做完（2026-10-14）

交接单里写的是"**先确认 tid 701 那张贴图确实是 `target 10`**，再决定正解（新增 texel buffer 资源种类）还是取巧兜底
（按 2D 贴图代替，必须留痕）"。这一轮两条都做了：确认在前，**正解**在后（没有走"按 2D 贴图代替"那条路）。

**一、先量，再改**（顺序很重要：改之前我们连"判据存不存在"都不清楚）。`vk_spv.c` 的分类器原来**没有任何判据**能产出
`VK_SPV_TEXEL`：`OpTypeImage` 的 `Dim` 根本没被解析，`OpTypeSampledImage` 一律算成"合并采样器图像"；描述符池里也没有
`UNIFORM_TEXEL_BUFFER` 的额度（`vk_pipe.c:96` 早就把它映射好了，只是一直没有生产者）。拿本地 **6923 份产物**先量了三件事：

| 离线实测（6923 份 `.spv` + 同名 `.res.json`） | 读数 |
|---|---|
| `Dim = Buffer` 的绑定 | **2991 条**（`sampled` 全是 1、取样类型全是 `float`），分布在 2991 份着色器里 |
| 反射侧 `cbuf_slot < 0` 的贴图绑定 | 也正好 **2991 条**，`handle` 全是 `0x6a` —— 与上一条**一一对应** |
| 它们的 binding 分布 | `binding 2` 2836 条、`binding 1` 154 条、`binding 4` 1 条（与日志里 8 条空白只出现在 binding 1/2 吻合） |
| `OpTypeSampledImage` 指向的 `OpTypeImage` 是否总在它之前 | **12111 次全部先出现，0 例外** |

第一、二行合起来就是那条实锤：**"贴图写空白"与"缺资源种类"是同一件事**（数目一一对上，不是"看着像"）。第四行决定了实现
只需一个字节的存储：扫到 `OpTypeSampledImage` 时把图像那一侧的 `Dim` **抄过来**即可，不必再存一张映射表（这条抄法有实测兜底，
不是假设）。

**二、改法**（五个文件，与苹果侧 `nvn_mtl.m:1204-1220` **同源**：直接在内存池缓冲上建贴图缓冲视图、零拷贝，
GPU 执行时读池里的当前内容）：

1. `vk_spv.c`：解析 `OpTypeImage` 的 `Dim`，`Dim = Buffer`（且非存储图像）判成 `VK_SPV_TEXEL`；`OpTypeSampledImage` 抄 `Dim`；
2. `vk_tex.c`：新增 `vk_tex_bufview` —— 范围 = **每元素字节 × 元素个数**，池地址经 `mtl_addr` 换成（池缓冲, 池内偏移），
   再 `VkBufferViewCreateInfo{offset, range, format}`。**两处是 Vulkan 的规矩而不是选择**：① 没有 `bytesPerRow`（texel buffer
   本来就是一维元素数组），但**偏移有对齐硬要求**（`minTexelBufferOffsetAlignment`）；② 格式要带
   `VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT`，压缩与深度/模板格式一概不行。两条都查、失败**分因报红一次**；
3. `nvn_vk.c`：描述符写入多一支（`pTexelBufferView`，与图像/缓冲三选一互斥）；池缓冲的用法位补
   `UNIFORM_TEXEL_BUFFER_BIT | STORAGE_TEXEL_BUFFER_BIT`（**位不能事后补**）；描述符池额度补一项；
4. `vk_pipe.c`：一个**全零元素的空白 texel 视图**（空槽时 NVN 自己就是读 0，与图像那条回退同一套理由）；
5. `vk_device.c/h`：把物理设备暴露出来（`vk_phys`），texel buffer 的偏移对齐只能从设备限制里问。

**三、真机读数**（Redmi K90 / Adreno 830，`DM_WIN_SRC=0` + `DM_WATCH_STALL=15`，与上一轮同环境）：

| 读数 | 改前（`run-c0-driver-segment.log`） | 改后（`run-texel-buffer.log`） |
|---|---|---|
| `【黄】贴图 target 10 … 暂不支持（缓冲区 / 多采样 / 线性布局）` | 1 条 | **0 条** |
| `贴图写空白` | 8 条（binding 1 两、binding 2 六） | **0 条** |
| 缓冲区贴图 | — | 【绿】落地：序号 22、**65536 个元素 × 16 字节**、格式 `0x2e → R32G32B32A32_SFLOAT(109)`、池偏移 `0x112b2c0`（直接建在池缓冲上） |
| 注册项目标 | — | 【查】**tid 701 → 注册项目标 10**；`binding 1` 与 `binding 2` 各绑上（texel buffer） |
| 新增报红 | — | **无**：报红仍是那 5 条"着色器 `16479cc6d75c2b28` 的产物不在 spv 里"的老账 + 看门狗 1 条 |

**四、交接单缺的那个现场，由 A/B 补上了**。给这条路加了一键退回空白的开关（`DM_NO_TEXEL=1`，**量具不是功能**，默认关）。
B 组（退回空白）里那 8 条空白**原样回来**，并且带着上一轮没打出来的那三个数：
`cbuf_slot=-1 handle=0x6a → 槽 8；该槽绑的句柄 0x2bd（低 20 位 **701**，注册表 **命中**）`。
也就是说：**槽解码、句柄、注册表三段都是好的**，唯一缺的就是"把注册项变成视图"那一步 —— 图像侧永远建不出来，因为它是
`target 10`。这条链与交接单的推断完全一致，现在有读数。

**五、A/B 顺带排掉一个误判（记下来，省得下次也怀疑自己）**：改后两轮都停在"出图 58~61 / 绘制约 6200"，而 21:47 那一轮是
"出图 340 / 绘制 37 万"，看着像**这次改动把运行拖慢了**。于是**只改一个环境变量**（`DM_NO_TEXEL=1`，其余一字不动）做 A/B：
两组读数**一字不差**（出图 58、绘制 6208、提交 61、卡在同一个阶段与同一句话）。再把 `ab-viewer-off` 里**历史九轮**的最后一条
心跳都列出来：**除 21:47 那轮之外，每一轮都停在 55~60 出图** —— 所以"停在 60"才是常态，21:47 那轮是异常。
**结论：帧率与卡住位置与本次改动无关。**

**六、顺带量出的一条真 bug（本轮不改，单独作为下一条）**：`vk_spv.c` 里 `OP_TYPE_FLOAT = 21 / OP_TYPE_INT = 22`
与 SPIR-V 规范**反了**（规范是 21 = `OpTypeInt`、22 = `OpTypeFloat`；本地产物里 `OpTypeInt %1 32 0` 是 4 个字、
`OpTypeFloat %4 32` 是 3 个字，一看即知）。它正好落在里程碑 A 另一条待办（**顶点属性分量类型 vs 着色器声明**）的判据上 ——
所以那条告警 `顶点属性 2 … 格式 0x27 vs 着色器 float` 的可信度要重新掂量（这一轮量出"着色器声明 float"这句话本身可能来自
一个反了的判据）。按"一次只改一处"的规矩，它留作下一条。
