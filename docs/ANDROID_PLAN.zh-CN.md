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
- **提交消息不要经由 PowerShell 5.1 读 `.ps1` 去"转写"**。把中文消息写在脚本里再让脚本写进消息文件，这条老办法踩到过一次：
  PowerShell 5.1 读 `.ps1` **按系统码页（GBK）而不是 UTF-8**，于是脚本里的中文在内存里已经是乱码，再经
  `[System.IO.File]::WriteAllText(..., UTF8)` 落盘 → **提交消息被二次编码**（`git log` 里显示成 `M3-2k锛氱紦...`，
  而仓库里更早的提交是好的）。改法：**消息文件由编辑器/工具直接写成 UTF-8**，`git commit -F <文件>` 只读字节，
  中间不经过 PowerShell 的字符串。已经写坏的那条可以还原（UTF-8 字节被当 GBK 读 = 二次编码，逐字节反向即可），
  或直接从脚本文件里把原文抽出来重写——但**别用 PowerShell 抽**，用 Python（`PYTHONUTF8=1`）。
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
- **工具链路径**统一在 `dm/android/apk/toolchain.cmd` **一处**决定（被上面两个脚本 call）：`JAVA_HOME` /
  `ANDROID_SDK_ROOT` / `ANDROID_HOME` / `DM_SDK_ROOT` / `DM_NDK` / `DM_BUILD_TOOLS` / `DM_CMAKE` / `DM_PLATFORM` / `DM_JDK`
  任选其一覆盖，**换机器不必改仓库文件**。见 `docs/CROSS_MACHINE.zh-CN.md` 第二节。

### cmd 脚本纪律（这些这次写 `toolchain.cmd` 全都踩过）

- `if 条件 set A & set B` 里的 `&` **脱离 if**，B 会无条件执行 → 一律写成 `if 条件 (set A & set B)`。
- **rem 行不是纯注释**：里面的 `&`、竖线、`>`、`<` 会被当成命令分隔与重定向 → 注释里一律不写它们。
- `if` 条件里**不要出现**子串替换 `%VAR:STR=REPL%` 或位置语法 `%VAR~n,m%` —— cmd 对 `%` 的配对在 `if` 行是贪心且带边界
  猜的，会与 `==` 串味报"命令语法不正确"。**先 `set` 进临时变量，再用纯变量名比较**。
- 括号块 `( ... )` 里**不要**放 `for /f ... 2^>nul` —— 块内解析与顶层不同，会报"命令语法不正确"；`for /f` 一律留在
  顶层或子例程里。

## 七、下一步

按本页顺序推进；每完成一个里程碑，在这里回勾并记录结论。**里程碑 A（渲染链路的告警）两条都清了**：
描述符的空白（UBO 名字号 −3、SSBO 由 `cb_offset` 反推、**缓冲区贴图**这个新资源种类，见「M3-2k」）与
顶点属性的那条判据（「M3-2l」：两个独立的错，其中一个靠探针照出来）。真机读数：`顶点属性` 0、`贴图写空白` 0、
`拿不到缓冲地址` 0、`target 10 暂不支持` 0。**"扫一遍"已经做过两轮（「M3-2m」）**：第 0 级逐张读**全零**；
改成**逐级**读之后**仍然全零**（第 50 帧逐级读 33 次、**0 次有内容**）。中间那条"查看器读第 1 级有内容"是
**行距错位**读到的陈旧堆字节，修掉行距后两边一致（「M3-2m」第六节）——**第 14 步的结论照旧成立**，
只是仪器现在可信了。**「九」优化清单第 1 项（管线缓存落盘）已于 2026-10-15 落地**（「M3-2p」）：它与卡点无关，
所以下面这五条的顺序不变；它照出来的一条真机事实（MIUI 退到后台直接 SIGKILL、钩子跑不到）也记在「M3-2p」里。
接下来按价值排序（2026-10-15 重排：勾掉已完成的"逐级扫"，合并重复的两条"设备丢失"，把「九」里与卡点无关的项
挪进来，并补上本轮照出来的约束）：

1. ~~**里程碑 B：客体停住**（任务图侧）~~ —— **2026-10-15「M3-2t」结案（任务图这条线）**：三张位图做了时间序列，
   三轮真机读数把「M3-2s」第四节的甲（上膛 0 位）/ 乙（完成后被清）分开了 —— **甲否掉、乙成立**：待派发最大
   `0x3fffff`（上膛确实写满低 `count` 位，派发器再逐位取走）、已完成最大 `0x3fffff`（22 位全置过很多次，
   随后被下一次上膛清 0）。⇒「M3-2r」的"22 个任务从未被派发"与「M3-2s」的"三张位图同时为 0"**都不是停住时的状态**
   （后者只是运行中的一个瞬态）。**顺带把三条旧口径改了**：看门狗的"无进展"用的是**引擎拍**（出图只在 `pregame`
   记），所以本轮三轮都是"出图在推进 + 拍 0"照样判死 ⇒ **转储不等于停住**，判据改用**心跳出图数 + 前端"连续 8 秒
   没有新出图"卡死报告**；真出现"出图停"的那一轮（t2）与**设备丢失**同现（`提交失败 VkResult -4` + 上百条
   `贴图上传失败`，随后 `TERM_WINDOW` 拆窗口、日志截断）；而停住现场里主线程的**当前 PC 落在我们自己的宿主层**
   （`vk_tex_view` / `vk_tex_grab_ser` / `mtl_wait`）——同期的图还在变。细节与三轮读数见「M3-2t」。
2. ~~**宿主侧"落在客体线程上的等待"全部点名**~~ —— **已结案（2026-10-15，「M3-2x」五轮真机）**：先修掉一个
   **仪器的假阴性**（可执行段表只有 64 槽，而真机进程里有 **455~460** 个可执行段 —— 丢掉的正是 `libc.so`（第 403）
   与 `libvulkan.so`（第 404），所以「M3-2w」读到的"栈里没有 libc 帧"是假阴性；已放宽到 1024 并在**表满时明说**）。
   修后点名：**不是我们的 `usleep`，也不是客体的 `SleepThread`** —— 宿主里唯一可能落在客体线程上的 `usleep`
   （「等『读取画面已撤』」，`diag.c:2075`）**是记账的**，而停住时它没开；`pc=nanosleep+0xc lr=usleep+0x54`
   只出现在**看门狗 `watchdog_main` / 音频 `null_sink`** 这两条我们自己的宿主线程上。**主线程停住时等的是驱动**：
   五轮分别是「读回：等队列空闲」10519 ms、「一次性提交等空闲」10051 ms、「vk：建图形管线（驱动内部编译）」10790 ms、
   「vk：贴图内存分配」10268 ms，而同刻**前端线程**也在「上屏：等上一帧的栅栏」等 10074 ms，心跳整段断约 11 秒后
   **自愈** ⇒ **"停住"= 驱动约 10 秒不回答（会重复出现），不是永久挂死**；这五轮 `VkResult -4` 全为 **0 条**
   ⇒ 那 10 秒停顿**不需要**设备丢失就能出现。**下一轮**（先量后改，三条见「M3-2x」第四节）：
   ① 给**驱动调用**加判活 / 上限（**不是**给 `usleep` 加）；② 把诊断读回从"在 `mtl_exec` 中途要求整个队列空"
   改成**帧边界**或"只等这一小段拷贝的栅栏"；③ 用修好的仪器把「M3-2u」的 A/B/C 三臂**再量一遍**，
   看单开时那 ~10 秒停顿是否消失。
   ⇒ **③ 已做完（2026-10-15，「M3-2y」十轮）**，结果把 ①② 的**前提**改了：**A/B/C/D（含两路全关）四臂八轮
   轮轮都有那 ~10 秒停顿**，而同一份 A 配置也出现过 **13.5 帧/秒、零长等**的一轮 ⇒ 停顿**不是诊断造成的**，
   而是**每次启动落在"健康 / 生病"两种状态之一**（画面内容差 13 倍 op/帧：1234 vs 100）。**所以下一步的顺序换过来**：
   - **先问"启动时那一步为什么有时走不过去"**（"生病"的单帧内容只有 ~100 op，是卡在一个更早更轻的场景里）；
     在它没量清之前，①② 属于"给一个可能只是症状的东西加护栏"。
   - **设备丢失**：新日志（`vk_tex.c` 的 `step_fail`）第一次说出了"谁先看见" —— **`vkQueueWaitIdle`（"等队列空闲"）**，
     其后上传都在 `vkQueueSubmit` 上返回 -4（-4 是粘的）。要答"**谁弄丢的**"还差一件仪器：
     **把"最后一次成功的提交"与"第一条 -4"之间那一小段（提交序号 / 出图数 / 时刻）留在日志里**。
   - **量任何帧预算之前**先判这一轮是健康还是生病（判据：`出图` 的 op/帧 与有无 >1 秒的等待），
     否则同一份配置的读数能差 13 倍。
   ⇒ **「M3-2z」又把"两种状态"追到了系统级**：病态时同一时刻 **SystemUI 也卡了 10 秒**
   （`Davey! duration=10018ms`、`waitForever: fence(kgsl-timeline-kgsl-3d0_7-ndroid)`）、**composer HAL 的调用
   9.49 秒**（`IComposerClient code=40`）、`Slow swap buffers`；而**把 app 停掉、清 logcat、等 60 秒 —— 一条都没有**
   ⇒ **那 ~10 秒的系统级停摆是我们引起的**（形状最像"我们让 GPU 挂住 → kgsl 约 10 秒后才判定/恢复"；
   `dmesg` / kernel logcat 读不到，`klogctl: Permission denied`）。**下一步**：① 给 `one_end_on` 加
   "读回 / 上传"标签（**只动日志**），先把 arm D 里那 10 秒的归属分出来（那一轮 viewer 与扫描都关着，
   长等落在客体线程的「一次性提交等空闲」上，而"建中转缓冲 + 分配 + 映射"没有长等 ⇒ 像是**上传**）；
   ② 在停顿窗口抓一次 ftrace / perfetto（`gpu` / `kgsl` / `sync`）；③ 换设备 / 换驱动对照。细节见「M3-2z」。
   ⇒ **「M3-3a」把 ①② 做完了，③ 没做（本机只有一台设备）**：① 标签加上后 **arm D 里那 10 秒确实是「上传」**，但 **arm A 有一轮是「读回：等队列空闲」10800 ms**
   ⇒ **两条路都会中**，"读回是罪魁"作罢；② 换了个更便宜的仪器 —— **`atrace` 在 shell 下能吐文本 ftrace**（含 `prev_state` 与 `sched_blocked_reason` 的 `caller=`），
   在停顿窗口抓到 171 MB 原文后读数四条：我方两个线程各有 ~10 秒的离 CPU 间隔（同一瞬间结束）、**整条 GPU/显示通路（`kgsl-events` / `kgsl_hwsched` / `gmu_f2h` /
   `crtc_commit:205` / `crtc_event:205` / `mFrameBuffer` / SystemUI）一起睡又一起醒**、**全是 S 态且全 trace 无 >0.5 秒的 D 态、CPU 大部分 idle、频率正常**、
   **恢复瞬间是 `gmu_f2h` 中断 + 整套 GPU/GMU 时钟与带宽重编程**（`clk_set_rate gpu_cc_*`、`icc_set_bw`）。**perfetto 这边如实记一条负面结论**：配置与产物只能用
   `/data/misc/perfetto-configs` 与 `/data/misc/perfetto-traces`，而且 **MIUI 的 SELinux 不让 `traced_probes` 读 vendor 事件的 `format`**（`debugfs_tracing_debug`）
   ⇒ **kgsl 那一族原子在这台机上拿不到**（perfetto 对不存在的原子名也不报错，所以"没报错"不能当证据；判据要看 logcat 的 avc）。
   **下一步的第一刀因此换成了「九」第 5 项（贴图上传改常驻上传环）** —— 本轮唯一"我方主动做成了几百次驱动级电源 / 总线投票"的地方。细节见「M3-3a」。
   ⇒ **「M3-3b」把这一刀砍下去了，结论是否定的**：`DM_UP_RING`（常驻上传环，**缺省关**）跑同一配方 6 次启动 —— **3 次病（10.2 / 11.9 / 10.7 秒）**、2 次健康、1 次启动崩溃；
   同构建的环关对照里那 10 秒也在 ⇒ **"每趟上传新建对象"不是那 10 秒的原因**（而且一轮只有 ~19 次上传，量级上也不该是）。它的正面读数：**同一把尺子**下我方线程在驱动里的
   电源 / 总线投票被削掉约 3 倍（`rpmh_write_batch` 42.5/s → 15.3/s、`rpm_resume` 8.9/s → 2.6/s、`gpu_cc_cx_smmu_gdsc` 13 → 0），但**没削干净**，而那 10 秒照旧
   —— "**投票变少**"与"**停摆消失**"是两件事。**所以下一步不再是"用法侧再砍一刀"**，而是：① **换设备 / 换驱动**（分辨力最强）；② 有 `trace_processor` 的机器上读
   `.work\trace\stall.pftrace`；③ 用法侧只剩两个便宜的候选（提交模式 / 破坏性取证），见「M3-3b」第七条。细节见「M3-3b」。
3. ~~**「九」第 2 项 `HOST_CACHED`**~~ —— **已取证、已否掉（2026-10-15，「M3-2q」）**：类型 6 上导出 fd / `mmap`
   到指定地址 / 双向都**能**（256 MB、512 MB、3968 MB 都过了），但同机 A/B 里 **CPU 时间没有可测差异**（同峰配对
   <3%，轮间波动 ±100%），而用它要**越过缓冲声明的可用类型位**（外部缓冲 `0x13` 里没有类型 6，属规范外用法），
   所以**维持类型 4**；`DM_ZC_CACHED=1` 开关留着（默认关），换设备 / 换驱动时可复验。
4. **「九」第 4 / 5 / 6 / 7 项**（按量出的帧预算再定）——每次绘制的重复工作（70.5% 的 op 是纯状态绑定）；贴图上传
   改常驻上传环（与第 2 条同源）；手写管线缓存表偏小（`VkPipelineCache` 落地后优先级下降）；op 流纯状态并入影子（M3/M4）。
5. **收尾**——文档与清理（接口描述表那批已不再被调用的代码）；`.work/recon/pending_rm.md` 里的待删项（等你一句
   「执行待删清单」）；M3-2p 留下的"启动到 `nnMain` 的时间"那条 A/B（**等第 1 条有进展再补**，不然停住会盖住量）。

**三条照出来的、对后面所有工作的约束**（前两条记在「M3-2p」、第三条记在「M3-2q」，这里提一句防忘了）：
- **MIUI 退到后台即 SIGKILL**：`TERM_WINDOW` / `DESTROY` / `atexit` 全都跑不到——**任何"退出时落盘"的设计在本机
  都不成立**，要持久化就得**前台定期写**（管线缓存就是这么落下来的）。
- **`adb shell input keyevent` 会被拒**（`SecurityException: INJECT_EVENTS`）——要让 app 退到后台得显式启动桌面：
  `adb shell am start -n com.miui.home/.launcher.Launcher`。
- **`DM_WATCH_STALL=0` 不是"只记不杀"**：`crash.c:347-354` 里值 ≤ 0 会**回落到 15 秒**，所以写 0 等于"停在原地
  15 秒就判死、打完转储 `_exit`"（本轮 A/B 里进程消失就是这个，不是崩溃）；要完全不看只能**整行不写**。
- **看门狗转储 ≠ "客体停住"**（「M3-2t」第五条）：它的"进展"量的是**引擎拍入口**与 **UI 消息**
  （`crash_watch_progress(cur_tick(),…)`，`diag.c:1752` / `:381`），出图帧**只在 `pregame` 期间**算进展
  （`crash.c:44`）——所以"**出图在推进 + 拍 0**"照样会在第 15 秒判死、转储、`_exit`（本机三轮实测）。
  判"停住"请用**心跳里的出图数**与前端那条"**连续 8 秒没有新出图**"的卡死报告。
- ~~**两个读回诊断别同时开**~~（「M3-2u」，真机 A/B/C 五轮）—— ⚠ **已被「M3-2y」十轮复量改写**：那一轮是
  **2 轮 vs 3 轮**的对比；A/B/C/**D（两路都关）** 四臂各两轮里，**那 ~10 秒的停顿轮轮都有**（含 D），
  而同一份 A 配置也出现过 **13.5 帧/秒、零长等**的一轮 ⇒ **停顿不是诊断开关造成的**，是**每次启动**落在
  "健康 / 生病"两种状态之一（画面内容本身差 13 倍的 op/帧，见「M3-2y」第四节）。
  "量帧预算时只开一个"仍可留着（少一件事总是好），但要停住现场照「M3-2u」的配方
  （`DM_WATCH_STALL=600` 长跑 + `touch …/dump.trigger` 按需取现场，进程不退出）。
  注意**不保证每次都冻**：本轮 7 次里 5 次冻、2 次干净地跑满 70 秒。
- **停住采样器的判据要含 `nanosleep` 轮询**（「M3-2v」）：只认"状态 R **且** `syscall` 报 `running`"会在
  **两次半拍上都漏掉**轮询式等待（睡的那半拍是 `S`、看的那半拍是 `R`，两次读还会串），于是转储里只剩一个
  光秃秃的 `R`、取不到任何 PC。已改成 `syscall = nanosleep` 也采 —— 这是本轮唯一拿到卡住线程宿主链的原因。
- **x29 帧链在驱动的帧上不可信**（「M3-2w」）：驱动不一定维护 x29，链会读到栈上的旧值。`mtl_wait+0x214`
  （`0x8d63c`）看着"像是卡在等栅栏"，反汇编却是"已滑出在飞窗口 → `return 1`"这条**不等待**分支的返回地址。
  另一层原因：**Android arm64 的返回地址带 PAC**（指针签名在**高 16 位**），带签名位的地址 `dladdr` 一律失败 ——
  回溯里那串 `0x24c6fca47ce57c` 形状的"垃圾地址"就是它。`crash_off` 现在先 `&= 0x0000ffffffffffff` 再符号化
  （用户空间只用低 48 位，对正常地址无副作用）。
- **不要在采样信号处理器里做 unwind**（「M3-2w」）：`_Unwind_Backtrace` 在这条路上**把被采样线程卡住了**
  （实测那一轮"新增样本 0"、处理函数没回来）—— 与项目里"别在处理器里 `fprintf`"那条老教训同源：处理器里不能有
  未知的锁 / 副作用。**读自己的栈**是安全的（sp 一定映射着，一次 `memcpy` 2 KB），要完整链就在 dump 阶段做。
- **读别的线程要趁"没被打断"的时候**（「M3-2w」）：`/proc/self/task/<tid>/syscall` 在该线程被信号打断时会变成
  `running`（**没有 sp 字段**）⇒ 先扫栈、再发采样信号；并且**按名字无条件扫主线程**（它的"睡一下、看一眼"来回
  会让通用的空转判据漏掉它，实测八条里漏过一条，而那正是最要紧的一条）。
- **栈扫描的"可执行段表"会静默地假阴性**（「M3-2x」）：按 `/proc/self/maps` 过滤的那张表原来只有 **64** 个槽，
  而真机进程里有 **455~460** 个 `r-xp` 段 —— **留下的恰好是 `libdm.so`（第 24 个）与驱动 `vulkan.adreno.so`（第 43 个），
  丢掉的恰好是 `libc.so`（第 403 个）与 `libvulkan.so`（第 404 个）**。于是"栈里一个 libc 地址都没有"与"真的没有"
  长得一模一样，把一次点名直接带向了相反的结论（`usleep` 就在 libc 里）。现在放宽到 1024，且**表满时读数里会明说**
  （`【黄】表满…`）—— **凡是有上限的表，都要把"满了"写进读数**。另一个同源的小现象：同一个地址会用两种印法
  （`pc=libc.so+0xd7b88` vs `lr=ioctl+0xa0`，其实同一个地址 —— `dladdr` 能命名 `ioctl`、命不了 libc 里的 syscall 桩）。

- **要看"谁在等、谁在跑、等的是哪个内核函数"，这台机上最便宜的仪器是 `atrace`，不是 perfetto**（「M3-3a」）：`atrace --async_start -b 32768 -c sched freq sync`
  → 命中后 `--async_stop -o /data/local/tmp/x.txt`，**输出是文本**，带 `prev_state`、`sched_blocked_reason: pid=… caller=…`、`cpu_frequency`、`clk_set_rate`、`dma_fence_signaled`，
  开头那条 `tracing_mark_write: trace_event_clock_sync: realtime_ts=…` 能把 trace 时钟对到墙钟。**shell 读不了 `tracefs` 的 `enable` / `available_events` 与
  `/sys/class/kgsl/*`，但 shell 在 `readtracefs` 组里、而 atrace 走的是它自己有权限的那条路** ⇒ 别在"直接抓 ftrace"上放弃。抓取配方（环状缓冲 + "最近 4 条心跳出图数相同且 >0"当扳机）
  已落成 `.work\trace\stall-ftrace.ps1`（**纯 ASCII**，避免 `.ps1` 的 BOM 问题）。
- **perfetto 在这台机上的三条硬约束**（「M3-3a」）：① **配置只能放 `/data/misc/perfetto-configs/`、产物只能写 `/data/misc/perfetto-traces/`** —— perfetto 自己的 SELinux 域
  读不了 `/data/local/tmp` 的配置、也写不了那儿的输出（`errno 13`）；② `compression_type: COMPRESSION_TYPE_NONE` **不是合法枚举**（只有 `UNSPECIFIED` / `DEFLATE`），
  写了直接判"配置非法"，要"不压缩"就**整行不写**；**pbtxt 不能带 UTF-8 BOM**（带 BOM 报 `1:1 error: Unexpected character`）；③
  **perfetto 对"根本不存在的原子名"也不报错**（负对照实测）⇒ **"配置没报错"不能当"原子可用"的证据**，要判得看 logcat 里 `traced_probes` 的
  `avc: denied { read } … name="format" … tcontext=u:object_r:debugfs_tracing_debug:s0` —— 这台机上**kgsl 那一族原子就是被它挡住的**。
- **改"站点名"这类只动日志的改动，保留被分出去的那一侧的旧串**（「M3-3a」）：把「一次性提交等空闲」分成"读回 / 对照"与"上传"时，读回那侧**一个字都没改**，
  于是新旧日志可直接比、读它的脚本也不用动（"改日志正文等于改统计口径"那条坑的正面用法）。
- 顺带：MIUI 上 `adb install -r` 会被拦（`INSTALL_FAILED_USER_RESTRICTED`），改用设备端 `pm install -r` 即可。

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
    格式 `R32G32B32A32_SFLOAT`、65536 个元素、直接建在池缓冲上）；
14. **顶点属性那条判据**修对 ✓（「M3-2l」：两个独立的错——两个 opcode 写反、指针没解引用；探针在设备上 3 秒自证
    `float 10294 / uint 870 / sint 0`，与离线统计逐项吻合；那条告警 **1 条 → 0 条**）。

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

### M3-2l：顶点属性那条判据里藏着**两个**独立的错（一个靠想、一个靠探针照出来）（2026-10-14）

交接单把里程碑 A 剩下的那条写成"顶点属性分量类型 vs 着色器声明"，并给了一个根因候选（上面那对写反的 opcode）。
真修起来才发现：**同一个读数上叠着两个独立的错**，而第二个**不是想出来的，是探针照出来的**。

**一、先量（离线，6923 份产物）**：

| 量的是什么 | 读数 |
|---|---|
| 告警点名的着色器 `3deaf68fd65b5298` 的阶段与输入 | **顶点**阶段；位置 0/1/5 = float x4、**位置 2 = uint x4** |
| 游戏给它的格式 | `0x27` = `R8G8B8A8_UINT`（属性格式表 `(0x27−1) % 12 = 2` → uint 类）→ **两边同类** |
| 自证编号：opcode × 字数 | `21` **只**出现 4 个字（13846 次）、`22` **只**出现 3 个字（6923 次 = 每份一个 float32）→ 21 = `OpTypeInt`、22 = `OpTypeFloat` |
| 全库顶点输入 | 11164 条，**全是 vec4**（float 10294 / uint 870 / sint 0）、**没有矩阵（数组）属性**；580 份顶点着色器带 uint |

所以那条告警**本来就是假警报**（着色器要 uint、游戏给的也是 uint），坏的是判据本身。

**二、编号改对**（`vk_spv.c`）：原来 `FLOAT = 21 / INT = 22`，于是 `OpTypeInt`（21）落进 FLOAT 那一支、
`OpTypeFloat`（22）因为只有 3 个字而进不了要求 `wc >= 4` 的 INT 那一支 —— `== OP_TYPE_INT` **永不成立**
（这两处的字数守卫就是判据的一部分，注释里写明了要一起看）。顺带把这条告警从"只打第一次"改成"前 8 例 + 每满 1024 例 + 累计数"：
判据本身出过错，所以"总量"比"样例"重要。

**三、改完先不跑游戏，跑探针**（设备上 3 秒）——**读数一字没变**：`顶点属性分量：float 11164、uint 0`。
"编号改对了、读数却纹丝不动"这句话本身就把**第二个错**指出来了：`var_type` 存的是 `OpTypePointer`，而属性那段**从没解引用**，
于是 `comps` 恒为 1、`ct` 是指针的 id（`scalar_op` 里当然没有它）→ 类别**永远**是 float。补上"指针 → 输入类型"这一步即可
（数组那一支**不写**：全库实测 11164 条输入全是 vec4，一条矩阵属性都没有）。

**四、设备侧自证**（探针，3.1 秒）：`顶点属性分量：float 10294、uint 870、sint 0`，样例里 `位置 N=uint x4` 直接可见 ——
与离线 Python 统计**逐项吻合**（10294 / 870 / 0；11164 条；全 x4）。这条探针是本轮顺手加的（原来只报绑定种类，不报顶点属性），
价值是：**这条修复不必跑游戏就能自证**，而且下次谁再动这两个 opcode，它第一时间会变红。

**五、真机读数**（Redmi K90 / Adreno 830，标准配置）：`顶点属性` 那条告警 **1 条 → 0 条**；
`贴图写空白` 0、`拿不到缓冲地址` 0、`target 10 暂不支持` 0（上一轮的成绩保持住）；`图形管线 80（失败 0）`；
卡住位置与这台机的常态一致（出图 56、绘制 6084、同一阶段）—— 也就是说**判据修对之后没有新的信号出现**，
里程碑 A 两条至此清完（另一条是「M3-2k」的缓冲区贴图）。

**六、顺带第一次记下一条**（**不是本轮引入**）：有一轮日志里出现 `【红】上传命令等待失败 VkResult -4`
（-4 = `VK_ERROR_DEVICE_LOST`）并级联出上百条 `【红】贴图上传失败 serial=… 格式 …`。跨轮对照：21:27 那轮（远早于本轮）
就有同一条，本轮 A/B 两组也都有，而 21:47 那轮没有 —— **偶发、与最近两次改动无关**；但它说明"设备丢失"会在这条
"一次性提交 + 等队列空闲"的贴图上传路上现形，值得单独查一次（先丢的是谁：客体自己的命令，还是我们这一次提交）。

### M3-2m：渲染区取证 —— 「九」第 3 项结案，并顺带把"画面在第几级"问出了新答案（2026-10-14 真机）

第 3 项押的是"渲染区由视口推导会把片元丢掉"。按"先取证"的规矩，在开通道处加了一行并排读数（`DM_PASS_AREA=1`
只读；`=2` 换成附件尺寸做对照），真机跑一轮。**这个嫌疑被否掉了**，但同一轮里另有三件事浮出来。

**一、渲染区不是卡点**（16384 次开通道的分布）

| 读数 | 值 |
|---|---|
| 主场景通道 | 渲染区 / 附件 / 视口 **三者都是 1600×900**（第 3、267、339、409、423、553、633 次…） |
| 算出的渲染区比附件小 | **267 / 16384 ≈ 1.6%**（第 1024 / 2048 / 4096 / 8192 / 16384 次分别为 11 / 28 / 62 / 131 / 267） |
| 退成 1×1 级 | **0** |
| 开通道时"没设视口" | **0**（视口总是先于通道到达） |
| 视口带偏移（x/y ≠ 0） | **0** |

那 1.6% 不是异常：它们是视口本身就只覆盖一块子区域的通道（例如 400×225 画进 1600×900），在 Metal 语义下同样只画那一块，
两边等价。**第 3 项结案：不是卡点**（对照开关 `DM_PASS_AREA=2` 因此不必再跑）。

**二、取证顺带照出两件新事**

1. **"附件 0x0" 经常出现** —— 这些是**只有深度附件**的通道（影子 `ncolor=0`）。此时渲染区没有附件可钳
   （现有钳位只在有颜色附件时生效），只能靠视口或 1×1 兜底。这是此前没被注意到的一个分支。
2. **出现了"视口 1600×900 / 附件 800×450"这类配对**（第 367、8192 次等）。渲染区被正确钳到 800×450，但
   **下发给 Vulkan 的视口仍然是 1600×900** —— 对"限了 mip 的附件"，视口没有跟着缩放。苹果侧同样没处理，
   是一处单独的不一致，记一笔备查。

**三、按「七」第 1 条扫了一遍：仍全零，但"全零"这句话本身要重审**

`DM_NVN_SHOT_SCAN=50,300,340` 三轮各扫 26 / 63 / 63 张 ≥256×256 的贴图：**每一张都是 `亮度 均值 0.0、最小 0、最大 0`**
（整片一个值，所以 `scan/` 目录仍然是空的）。表面上看，第 14 步"GPU 上没有写出颜色内容"的结论稳如原样。

**但查看器给出了相反的读数**。把它切到**第 1 级**（`DM_WIN_SRC=28`、`DM_WIN_LEVEL=1`），同一个序号 28 立刻读到：

```
查看器：序号 28 第 1 级 800x450 第 61 次，亮度 均值 0.3、最小 0、最大 160（读成功 61、跳过 149）
查看器：序号 28 第 1 级 800x450 第 121 次，亮度 均值 0.2、最小 0、最大 106（读成功 121、跳过 149）
```

**同一张 1600×900 的渲染目标：第 0 级读出来是 0，第 1 级读出来有内容。** 这与取证里那批"附件 800×450"的通道正好对上
（`pass_begin` 的"渲染区宽 1600 超出附件 800（限了 mip 的视图）"说的就是同一件事）。

**所以第 14 步的结论至少要限定为"第 0 级没有内容"**：扫描只读第 0 级，而画面至少在某个层级上是有的。
判据要改成**逐级扫**（每一级用该级自己的尺寸），而不是只读第 0 级。

**四、顺带一条崩溃**：查看器读第 1 级读到约 200 次时 `CRASH sig=11 pc=vulkan.adreno.so+0x2a7ac8`（驱动内）。
与文档里"查看器一开就崩（驱动内 `vk_tex_view`）"属同一类；嫌疑是**读回按底图尺寸分配缓冲、却去拷第 1 级**，
拷贝范围与实际子资源尺寸不符——修的时候要按"该级的尺寸"算。

**五、仪器**：`DM_PASS_AREA=1`（只读）/ `=2`（渲染区换附件尺寸的对照），读数是"渲染区 / 附件 / 视口 / 裁剪"
并排一行 + 四个累计数（比附件小、退成 1×1、开通道时没视口、视口带偏移）。

**六、复验：上面"第 1 级有内容"那条结论是错的（行距）**（2026-10-14 当日补，同一轮之内做掉的复验）

"逐级扫"落地后第一次真机就把它推翻了。错在**查看器**：它的缓冲与行距都按**底图**尺寸给（`g_src_w * 4`），
却拿这个行距去读第 1 级 —— 第 1 级的实际行宽只有底图的一半，于是行与行错开，尾巴落进**没初始化过的堆内存**，
读出来就是"均值 0.3、最大 160"的陈旧字节。第 0 级之所以读对（0.0），恰恰因为它的行宽与底图一致。
（这正是"两个数看着像结论"的典型：**错的仪器给的是一个很像真的数**。）

修法两处：① 缓冲与行距都按**要读的那一级**的尺寸（`>> level`）；② `shot_grab_ser_lv` 的返回值把"读成功"与
"有没有内容"分开（0 = 失败、1 = 读成功但整片一个值、2 = 读成功且有内容），扫描的汇总用它计数。

**修完两边读数就一致了**：

| 读法 | 序号 28 第 0 级 | 序号 28 第 1 级 | 序号 28 第 2 级 |
|---|---|---|---|
| 逐级扫（修后） | 均值 0.0 | **均值 0.0** | 均值 0.0 |
| 查看器（修后） | — | **均值 0.0、最小 0、最大 0** | — |

**所以"画面在第 1 级"不成立。** 真机第 50 帧：26 张 ≥256×256 的贴图**逐级**读了 33 次，**0 次有内容**
（`序号 27` 的第 1/2 级是 **255**，即全白——仍然是没有内容）。第 14 步的"GPU 上没有写出颜色内容"**照旧成立**，
差别只在**仪器**：现在是逐级的，而且行距对了。

**七、顺带：那条崩溃在**我们自己**的代码里**。这一轮记的是 `CRASH sig=11 pc=vk_tex_grab_ser+0x7a0`
（上一轮记成 `vulkan.adreno.so+0x2a7ac8`）。现场是"查看器线程与扫描/出图**同时在读**"，而读回共用**同一块常驻中转缓冲**
（`stg_get`），所以先怀疑两处并发：中转缓冲的复用与队列锁的边界。**下一条就查它**——不再是驱动的事。

**八、这一节的教训（值得单独记）**：`DM_NVN_SHOT_SCAN` 只读第 0 级，是仪器的**缺口**而不是世界的性质；
而查看器报出的"有内容"，是**另一个**仪器缺口。两个缺口合起来，足以让一个不存在的结论看起来有两处证据支持。

### M3-2n：读回那条并发崩溃 —— 修好，并顺带踩了一个"看起来像游戏卡死"的锁序反转（2026-10-14 真机）

「M3-2m」第七节留下的是 `CRASH sig=11 pc=vk_tex_grab_ser+0x7a0`（读回里，查看器与扫描**同时在读**）。
这一轮把它关掉，过程中自己又造了一次死锁 —— 两个都记下来，因为它们都长得像"游戏卡了"。

**一、根因**。读回这条路上有两份**共享且会被改**的东西：常驻中转缓冲（`stg_get`，尺寸不够时**销毁重建**）
与一次性命令缓冲（`g_cb`）。而发起读回的有**两个线程** —— 出图/扫描在客体线程、查看器在前端线程。早先只锁了
"队列提交"那一段，于是"A 正在往命令缓冲里录，B 把中转缓冲销毁重建"直接踩进未定义行为：A 手里是一块**已经销毁**的
缓冲（或还没映射成的指针），现场就是那条 `+0x7a0`。

**二、修法**。一把锁（`g_one_mu`）覆盖"拿中转缓冲 → … → 读完解映射"**整段**；读回与对照清除都走它。

**三、我在上面踩的坑（值得单独记）**。顺手把**上传**也纳进这把锁 —— 想的是"它们共用 `g_cb`，一起串行最省事"。
结果**整套启动就停住、`出图 0 次`**，看门狗只报 `拍 0 / 阶段 MainMenuHD:CheckUpdateLinks`，连跑三轮一模一样：
看着完全像"游戏卡在启动"，其实是**锁序反转**。上传那条路是 `tex_of` **持着 `g_mu`** 调 `ent_upload`，
而读回是 `one_mu → g_mu` —— 凑起来成环：查看器持 `one_mu` 等 `g_mu`，客体持 `g_mu` 等 `one_mu`。
**判据**：把上传那一处撤掉之后，出图**立刻**恢复到常态（56 帧、`CharacterCreate:Hover:Barbarian`）。
**教训**：加锁之前先把"谁持着什么调进来"列一遍；"看起来像游戏卡死"的真身可能是两把锁互相等。

**四、仍然留着的一条（下一处改动）**：上传与读回**共用 `g_cb`** 这件事还没消掉 —— 上传这条**不能**拿 `one_mu`
（见第三节），要真正解决得让上传**用自己的命令缓冲**。已在代码里写明（`vk_tex.c` 上传那一段）并记进「七、下一步」。

**五、真机读数**（Redmi K90 / Adreno 830，`DM_WIN_SRC=28`、`DM_WIN_LEVEL=1`、逐级扫描 50/54/57）：
两轮连跑**均无 `CRASH`**；查看器读成功 **301 / 361 次**（与客体并发），扫描各 **33 次逐级读**（第 50、54 帧），
出图到 **50 / 56 帧**后落在这台机的**常态卡点**（`CharacterCreate:Hover:Barbarian`）——
"看画面"这条回路恢复可用，而卡点与它无关。

### M3-2o：上传改用**自己的命令缓冲** —— 消掉一处真隐患；但设备丢失**没有**跟着消失（2026-10-14 真机）

「M3-2n」第四节留下的那条：上传与读回**共用同一块一次性命令缓冲**（`g_cb`）。这一轮把上传拆到 `g_cb_up`
（`one_begin_on` / `one_end_on` 按槽传入，`one_begin` / `one_end` 留给读回与对照清除）。**两个线程往同一块命令
缓冲里录**本身是未定义行为，这条现在从根上没有；上传也因此**不必**再碰读回那把锁（「M3-2n」第三节那条锁序反转
的前提随之消失）。

**一、修完的读数**（Redmi K90 / Adreno 830，`DM_WIN_SRC=28`、`DM_WIN_LEVEL=1`、逐级扫描，三轮连跑）：
**均无 `CRASH`**；查看器读成功 **541 / 661 / 361 次**（与客体并发）；逐级扫描正常；卡点仍是这台机的常态
（`CharacterCreate:Hover:Barbarian`，其中一轮 `MainMenuHD:CheckUpdateLinks`）。上传这条路**没有报红**：
`解排布失败` 0、`建 VkImage 失败` 0、`贴图内存分配/绑定失败` 0、`建 VkImageView 失败` 0。

**二、但那条设备丢失**没有**消失**：有一轮里 `【红】一次性提交等待失败 VkResult -4`（-4 = `VK_ERROR_DEVICE_LOST`）
仍然出现 **1 次**，后面跟着 **110 条** `贴图上传失败`。也就是说**"共用命令缓冲"不是它的成因** —— 拆开之后频率
看起来没变（旧构建两轮各 1 次，新构建三轮里 1 次；样本太小，只能说"没有变化"）。「七、下一步」第 4 条**继续留着**，
但由此**排除掉一个候选**。

**三、一条自己踩的坑（记录用）**：`one_end` 里那句日志我顺手改成了"一次性提交等待失败"，而统计还在按旧串
`上传命令等待失败` 数 —— 于是第一遍读出来是"0 条"，差一点把"没修好"记成"修好了"。**改日志正文等于改统计口径**：
改的时候要跟着改读它的脚本，或者新旧串都数一遍再下结论。

### M3-2p：管线缓存落盘 —— 「九」第 1 项落地（2026-10-15 真机）

「九」第 1 项（建一个图形与计算**共用**的 `VkPipelineCache`，落盘、下次启动载回）做完，并在 Redmi K90 /
Adreno 830 上验过。这一节记三件事：**落在哪几处**、**blob 的头为什么必要**、**以及一条比缓存本身更值得记的真机事实**
（"切后台"这条路在本机根本走不通）。

**一、落法**（三处）：

| 在哪 | 做什么 |
|---|---|
| `vk_device.c` | 建/载/写全在这一层：`pcache_boot`（`vk_device.c:134`，设备建好后调）、`vk_device_pcache_save`（`185`）、`vk_device_pcache_tick`（`246`）、`vk_dev_pcache` / `_lock` / `_unlock`（`103` 起）；`vk_device_init` 里那一句在 `543` |
| `vk_pipe.c` | 两处 `vkCreate*Pipelines` 的 cache 参数从 `VK_NULL_HANDLE` 换成 `vk_dev_pcache()`：计算 `287-289`、图形 `668-670` |
| `main_apk.c` | `DM_PIPE_CACHE` 缺省指到 app **内部**目录（`609-617`；`/data/local/tmp` 对 APK 只读）；每秒心跳里调 `vk_device_pcache_tick()`（`683`）；TERM_WINDOW（`549`）与 DESTROY（`555`）各写一次 |

为什么放在**设备层**而不是管线层：它的来历要跟设备/驱动核对，落盘路径又跟"哪个进程在跑"有关（APK 只能写内部目录），
两件事设备层都知道。**建管线要外部串行化**（Vulkan 对 `VkPipelineCache` 的访问要求调用方串行化），所以配了一把锁，
写盘走同一把 —— 这也是为什么 `vk_pipe.c` 那两处是 `lock / vkCreate / unlock` 三行而不是一行。

**二、blob 前面为什么压一个头**：`vkGetPipelineCacheData` 出来的字节**与驱动、设备绑定**，换驱动后必须整份丢弃
（官方样例与 zeux.io《Robust pipeline cache serialization》都点名这条）。头里存 vendor / device / driver / api 与
`VkPhysicalDeviceProperties::pipelineCacheUUID`（`vk_device.c:83`），对不上就**不用**：宁可从零编译，也不要一份脏缓存。
两条负向判据都在真机上照过（下面 C1 / C2）。

**三、真机读数**（四轮，`DM_WIN_SRC=28`、`DM_WIN_LEVEL=1`）：

| 轮 | 起始缓存 | 日志（`logs/latest.log`） |
|---|---|---|
| A | 无 | `管线缓存：没有可用的旧缓存…从零编译` → `写出 0.0 MB`（第一条管线）→ `写出 0.6 MB` |
| B | 651 KB | `【绿】管线缓存：载入 636.0 KB` → 复用中继续长到 `743.1 KB`（在旧基础上追加） |
| C1 | 4 KB 随机垃圾 | `【黄】管线缓存的头不认得：丢弃，从零编译` |
| C2 | 64 B"头对、设备号假" | `【黄】管线缓存的来历与这台设备对不上（设备 0x0000→0x44050001、驱动 1→2150760519）：丢弃` |

落盘位置：`/data/user/0/ai.d2rm.dm/files/pipecache.bin`（`adb shell run-as ai.d2rm.dm ls -l files` 可见，641 KB 量级）。
C2 顺带照出这台机的两个数：`deviceID = 0x44050001`、`driverVersion = 2150760519`。
（A / C1 / C2 三轮跑的还是"一律按 MB 打"的那版日志，所以第一条管线读作 `0.0 MB`；现在低于 1 MB 会打成 KB ——
`0.0 MB` 分不出"刚建了一条管线"和"缓存几乎是空的"，这一处是顺手改的。）

**四、这一轮照出来的真机事实（比缓存本身更值得记）**：**"切后台"这条路在这台机上走不通**。按 HOME 退到后台，MIUI 把
这个包按"游戏"处理（logcat：`ANDR-PWR-OPT: GAMEPOWEROPT: isGame() ai.d2rm.dm is Game = 1`），随即
**`libc: kill: send 9`（SIGKILL）**：`TERM_WINDOW` 送不到我们手里，日志里没有 `窗口那一套已拆`，`atexit` 也不会跑。
两轮实测一致（pid 在退到后台后 6 秒内消失）。所以"退出 / 切后台写回"这两个钩子在本机**一次都不会触发** ——
真正让它落盘的是**前台定期写回**（`vk_device_pcache_tick`：每秒看一次，数据长过才写，限频 10 秒）。
代价很小：管线集中在启动与首次进场景时新增，写入因此也集中；0.6 MB 量级、`.tmp` + 改名（进程被 SIGKILL 也不会留半截）。

**五、还没量到的**（「九」第 1 项留的"怎么量"）：**启动到 `nnMain` 的时间、首次进场景的卡顿**需要"清缓存 / 带缓存"
同机 A/B，而「M3-2o」那条常态（出图十几秒后停在 `CharacterCreate:Hover:Barbarian`）会盖住这个量，所以那条 A/B 等
停住那件事有进展再补。当前能确证的是"缓存确实被复用"：B 轮载入 636 KB 之后**继续增长**到 743 KB，就是在旧缓存上追加。

**六、怎么用 / 怎么关**：默认自动（APK 走内部目录、控制台宿主走 `<DM_ROOT>/pipecache.bin`）。要从零编译做对照，
用 `DM_PIPE_CACHE=0`（整条路关掉，比"删文件"更容易复现），或把那个文件删掉；想换个位置就给它一个路径
（`dm.conf` 里写也行，配置比设备初始化先加载）。

### M3-2q：「九」第 2 项（`HOST_CACHED`）取证并**否掉**（2026-10-15 真机）

「九」第 2 项留的待验是"**类型 6 上 `vkGetMemoryFdKHR` 能不能导出**"。先把探针扩了三件事
（`vk_fdmap_probe.c`：打印**全部**内存类型、`DM_FD_MEM_TYPE=<索引>` 指定要试的类型、`DM_FD_FORCE=1` 在被排除的
类型上照样硬试、`DM_FD_SIZE=<MB>` 指定尺寸，另加一段"普通缓冲 vs 外部缓冲"的类型位对照），再用**同一份二进制**做 A/B。

**一、探针读数**（Redmi K90 / Adreno 830）：

| 问 | 读数 |
|---|---|
| 外部（可导出）缓冲的可用类型位 | `0x13` → 类型 0 / 1 / **4**，**类型 6 不在里面** |
| 同尺寸同用途的**普通**缓冲 | `0x41` → 类型 0 与 **类型 6**（说明类型 6 本身能背缓冲） |
| 类型 6 上分配（`DM_FD_MEM_TYPE=6 DM_FD_FORCE=1`） | 256 MB / 512 MB / 3968 MB **都成功**；取 fd、`mmap(MAP_FIXED)` 落在指定地址、绑缓冲、双向**全通** |
| 同上，池子的用法（同一份分配偏移处再绑一块） | 成功，偏移处双向也通 |

⇒ **能导出**，但这是**越过 `vkGetBufferMemoryRequirements` 声明**的用法：规范要求 `memoryTypeIndex` 必须在该缓冲的
可用类型位里，而类型 6 不在 `0x13` 中 —— 靠驱动宽容接受，属**未定义用法**。

**二、A/B**（同一份二进制，只改 `DM_ZC_CACHED`；每轮固定墙钟 **10 秒**采 `/proc/<pid>/stat` 的 `utime+stime`）：

| 轮 | 零拷贝类型 | 出图 | 绘制（有目标） | CPU ticks |
|---|---|---|---|---|
| A1 | 4 | 52 | 3,154 | 778 |
| B1 | 6 | 170 | 59,430 | 1,805 |
| A2 | 4 | 56 | 4,671 | 920 |
| B2 | 6 | 59 | 3,574 | 888 |
| A3 | 4 | 169 | 59,610 | 1,804 |
| B3 | 6 | 52 | 3,154 | 770 |

**结论：没有可测收益，本项否掉、维持类型 4。** CPU ticks 只跟"干了多少活"走：约 3,154 次绘制 ≈ 780 ticks、
59,600 次 ≈ 1,805 ticks，两边比例一致；**同峰配对差异 <3%**（早停峰 A 中位 849 / B 中位 829；跑起来峰 1,804 / 1,805），
而**轮间波动是 ±100%**（这台机的老常态：出图 50 停住 vs 能跑到 170+，见下面那条坑）。到"出图 50"的**墙钟**时间波动也很大
（3.5 s 对 14.5 s），但 CPU 几乎一样 —— 差的是**等待**（GPU 同步 / I/O），与这两类内存无关。
收益看不到、风险（未定义用法）却要付，所以不换。

**留下的东西**：换轨处**显式**优先取"带 `HOST_CACHED` 的零拷贝类型"、取不到或分配失败**回退类型 4**（`vk_region.c`），
由 `DM_ZC_CACHED=1` 控制（**默认关**，缺省行为与改动前完全一致）；探针侧多出上面那四个开关，换设备 / 换驱动时一条命令即可复验。

**三、本轮踩到的两个坑（记下来省得再走一遍）**：

- **`vk_memory_type` 的"退一步"会退到类型 0**。想改成"优先 `HOST_CACHED`"时，若只是往 `want` 里加一位，
  `vk_memory_type`（`vk_device.c:370-378`）第二轮的"取 `need_bits` 里第一个可用"会返回**类型 0（纯 `DEVICE_LOCAL`）**
  —— 零拷贝当场坏掉。正确做法是**显式**取"带 `HOST_CACHED` 的零拷贝类型"，取不到再回退类型 4。
- **MIUI 上 `adb install -r` 会被拦**：报 `INSTALL_FAILED_USER_RESTRICTED: Install canceled by user`（手机端确认弹窗
  被取消或权限没开），而 `install_apk.cmd` 走的就是这条。改用**设备端 `pm install`** 立刻成功：
  `adb push .work\apk-build\dm.apk /data/local/tmp/dm.apk` 之后 `adb shell pm install -r /data/local/tmp/dm.apk`。
- **`DM_WATCH_STALL=0` **不是**"只记不杀"——`dm.conf` 里那句注释是错的（本轮真踩了）**。`crash.c:347-354` 是
  `g_watch_stall = stall && atoi(stall) > 0 ? (unsigned)atoi(stall) : 15;`：**设了但值 ≤ 0 会回落到 15 秒**，也就是
  "停在原地 15 秒 → 判死 → 打完现场转储 `_exit`"。所以 dm.conf 里写 `DM_WATCH_STALL=0`（本意"只记不杀"）实际是**判死**：
  本轮 A/B 里进程在停住后十几秒消失就是这个，**不是崩溃、也不是 MIUI 杀的**（日志尾巴正好是"等待点"+各线程状态那份转储）。
  要**完全不看**看门狗，只能**这一行都不写**（`if (watch || stall)` 才起线程）；要长跑观察，给它一个足够大的值。

### M3-2r：里程碑 B 的形状量出来了 —— 主线程在等一个**从未被派发**的任务图（2026-10-15 真机）

「七、下一步」第 1 条（客体停住）这一轮从"多轮取样"推进到"有形状、有具体读数"。下面说四件事：**取样怎么读**、
**新增了哪些仪器**、**量到了什么**、**哪些假设被实测否掉**；末尾附一条反汇编路子（本机没有 Python，值得记）。

**一、取样怎么读（两个容易读错的地方）**：

- **看门狗报的"卡在 拍 0"是正常的**，不是缺陷：`cur_tick()`（`diag.c:1316`）只在**局内**才读 `G+0x170`，
  而停住发生在**选角界面**（未进局），常量 0 是设计如此。真正的判死依据是 pregame 阶段的**出图帧**
  （`crash_watch_frame`）⇒ "判死"＝**出图停了 15 秒**。
- **这轮取样里出现两个停点**：多数轮出图停在 **49~59**，少数轮能跑到 **377~381**（约 3/8）——后者就是
  「M3-2l」记过的那类"偶发那轮"，判据仍按"多轮里它在干什么"读。

**二、新增仪器**（都在 `dm/host/hle_thread.c`，默认不影响行为）：

| 仪器 | 用途 / 本轮读数 |
|---|---|
| `SleepThread` 时长与时刻 | 记进线程结构、现场导出打印，≥200 ms 额外打一行。**实测：没有长睡**（各线程最近一次 1~50 ms），先前的"主线程长睡"猜测被排除 |
| **svc 环** | 每线程最近 16 次 `号 + x0/x1/x2 + LR`。这轮就是它把互等环照出来的 |
| **x29 调用链** | 沿 `regs[29]` 走，收集落在 `main` 的返回地址（最多 10 层）——**安卓侧第一次能读到"游戏在哪个函数"** |
| 每次等待"已等 X 秒" | 判"等太久"（`0x1005` 实测 **15.1 秒**） |
| 锁移交历史 | 最近 16 次 `ArbitrateUnlock` 的 `地址 → 等待者`，用来认"锁字残留" |
| **任务图完成位图** | 从 `[x29-8]` 取任务图对象（依据见下），直接读 `+0x3328`（位图）与 `+0x3100`（等待掩码） |

两条关键反汇编依据（`main.patched.bin`；本机用 NDK 的 `llvm-objcopy`/`llvm-objdump`，见第五节）：
- **`regs[30]` 不是客体的 LR**：它是门块的返回地址（所以 svc 环里 `lr=sdk+0xe6d09c` 这类只说明"哪个门"）。
  客体真正的返回地址在 **`[x29+8]`**（`patch_nso.py:429` 就是这么认调用者的，门块刻意保留了 x29 链）。
- `main+0x15ee898` 那个函数的序言是 `stp x20,x19,[sp,#0x40]` + `add x29,sp,#0x50` ⇒ **`x19`（任务图对象）= `[x29-8]`**。

**三、量到了什么**（`main+0x15ee8b4` 起，反汇编 + 真机读数）：

```asm
w8 = [x0+0x3328]        ; 任务完成位图（ldar）
x9 = [x0+0x3100]        ; 本等待者的掩码（=22）
w9 = ~(-1 << x9)        ; 低 22 位全 1
cmp w8, w9 ; b.eq 跳过等待
bl  0xdf9794            ; 等不到就调它（就是那些 SleepThread(-1)）
ldar w8, [x25] ; cmp ; b.ne  ↺   ; **轮询循环**
```

真机两轮一致：

```
[dm] 0x1001 任务图 0x7bf5752d40：完成位图 0、等待掩码 0x16
[dm] 0x1001 任务图 0x7bfc214d40：完成位图 0、等待掩码 0x16
```

⇒ **位图恒为 0**：22 个任务**一个都没完成**。

同一时刻其它线程（三轮一致）：

| 线程 | 状态 | 在做什么 |
|---|---|---|
| `0x1001` 主线程 | 用户态 R；svc 环 16 条**全是 `SleepThread(-1)`** | 轮询上面那个位图 |
| `0x1005`（prio 28） | 最近 15 条是 `0x83/0x84`（**Granny 段解码宿主门**）+ 末条 `1a(x0=0x1001 …)` | 解码中**去等一把被主线程持有的锁**（`已等 15.1 秒`） |
| `0x1004`（prio 28） | `1c(条件变量)` ×16 | 等条件变量（与主线程那对 `1d`/`1b` 配对，正常） |
| 其余（FMOD / 网络 / AIO / libcurl / bs Resolver） | 三轮完全相同 | 正常等待 |

**当前定性**：停住的**主因**是「**任务图里 22 个任务从未被派发/从未置位**」——主线程因此永远等；而"工人 `0x1005`
等主线程持有的锁"是它的**副产物**（主线程持着那把锁在等位图）。注意 `0x1005` 虽然一直跑 `0x83/0x84`，
但**位图一位都没置** ⇒ 它执行的**不是**这个任务图里的任务。

> **「M3-2s」更正（同日晚些，反汇编 + 新仪器）**：上面这句"从未被派发"**不准确**。找到上膛点
> （`main+0x1636f50`：把低 `count` 位写进 `+0x3324`）与置位点（**只有**任务包装器末尾 `main+0x15f0e6c`）之后，
> 实测三张位图**同时为 0**（已完成 `+0x3328` / 待派发 `+0x3324` / 依赖源 `+0x3320`），而主线程自己的栈上就有
> **活的包装器帧**（= 它正在执行这张图的任务）——"没派发"与"栈上正跑着"不能同时为真。另外 `0x1005`
> 确实**在这个图的某个任务里**（包装器帧读到的图 = 主线程所等的图），它等的那把锁字是 `0x40001001`
> （持有人 `0x1001` = 主线程）。口径与下一步见「M3-2s」。

旁证两条：**那把锁从未出现在任何一次锁移交里**（移交历史只有 `0x7c110e0bb8` 一类活跃协作锁）⇒ 排除"锁字被
`lock_handoff` 误写";**我们这侧图形是空闲的**（等待点：在飞槽 0/8、通道未开、队列锁无人持有）⇒ 出图停是结果，不是原因。

**四、两个被实测否掉的假设（省得再走一遍）**：

- **不是 Granny 预解码**：`DM_GRANNY_PRE=0`（关预解码、走真解码）跑 3 轮，**照旧全部判死**。
- **不是锁语义**（开关 `DM_LOCK_SEM`，默认 0；两个方向都试了，都**更糟**）：
  - `bit0`（`ArbitrateUnlock` 改成 Horizon 的"锁字清零 + 唤醒一个"）⇒ 3 轮**全部**在启动早期
    `Break#1 reason=0`（客体 `svcBreak`、Panic）终止、出图 **0**；
  - `bit1`（把 `0x1A` 的等待判据从"锁字 == 期望 | 等待位"放宽为"持有人 == 期望"）⇒ 3 轮**全部**在
    `阶段 启动` 就判死、出图 **0**。

  ⇒ 现状的锁实现（"移交式" + 要求等待位）与 SDK 是**自洽**的，**锁不是根因**。开关留在代码里、默认关，
  换设备 / 换驱动时仍可复验。

**五、反汇编路子（本机没有 Python，值得记）**：

```
llvm-objcopy -I binary -O elf64-littleaarch64 \
  --rename-section .data=.text,alloc,load,readonly,code \
  .work/build/cache/dm/out/3.3.94012-r41-keypage/main.patched.bin .work/recon/main_elf/main.elf
llvm-objdump -d --start-address=0x15ee860 --stop-address=0x15eea10 .work/recon/main_elf/main.elf
```

两件都在 NDK 里（`...\ndk\30.0.16248370\toolchains\llvm\prebuilt\windows-x86_64\bin\`）；**`main.patched.bin`
的文件偏移就等于 `main+0x`**；产物落在 `.work/recon/`（已 gitignore）。仓库里的 `dm/tools/sdis.py` 更强
（`--fn` / `--sym` / `--xref`），但它要 capstone/Python，本机没有。

**六、下一步（按此顺序）**：

1. **搜"谁写 `this+0x3328`"**：全量反汇编 `main.patched.bin` 后找写 `#0x3328` 的 `str/stlr` 点 —— 那就是
   "任务完成/置位"的代码，也就回答了"为什么 22 位一个都没置上"；
2. 对照**任务提交点**（`main+0x147f154` 作业对象提交）与**作业池"边帮忙边等"循环**（`main+0x14f08d8`），
   确认这 22 个任务由谁提交、被什么挡住；
3. 顺带（「七」第 2 条设备丢失）：这轮取样里 **3/8 轮的现场是"我们的一次性提交持有队列锁"**（不是客体停），
   与「M3-2l」第六节那条偶发 `VK_ERROR_DEVICE_LOST` 同源，量法不变。

---

### M3-2s：「谁写 `this+0x3328`」有答案了 —— 三张位图、上膛点、以及一处**说不通**（2026-10-15 真机）

「M3-2r」第六节那两步（搜置位点 + 对照提交点/作业池）这一轮做完了，结论比预期多，也多出了一个**新的、
必须靠时间序列才能收口的矛盾**。下面五节：**位图与代码**、**上膛点**、**真机读数**、**矛盾在哪**、**新仪器与下一步**。

**一、那个对象上是三张 32 位位图，不是一张**（`this` = 任务图对象，任务记录 `rec[i] = this + i*0x188`，
`this+0x3100` = 任务数；22 个任务 → 位图只需低 22 位）：

| 偏移 | 谁写 | 谁读 | 含义 |
|---|---|---|---|
| `+0x3320` | **提交者**：`entry->graph + 0x3320` 上 `ldxr/orr/stlxr` 置 `1 << entry->index`（`main+0x14e9758`、`0x14fe97c`、`0x1666a68` 等 7 处同一惯用法） | 派发器的第二个依赖源 | "某个东西就绪了"（依赖 B 的来源） |
| `+0x3324` | **上膛点**：`main+0x1636f50` 一次写入低 `count` 位；**派发器**每派发一个就原子清一位 | 派发器（候选集） | "待派发" |
| `+0x3328` | **只有一处**：任务包装器末尾 `main+0x15f0e6c`（`ldxr/orr/stlxr` 置 `1<<序号`） | 轮询、派发器（依赖 A 的来源） | "已完成" |

`+0x3328` 的清零点共三处：上膛点 `main+0x1636f54`（清完成位图）、**另一条跑图路径的收尾 `main+0x15eef90`**
（只清它、不动任务数与待派发）、构造期的一大片 `str wzr/xzr`（`0xa95d6c`、`0x12c7874`…）。
**"谁写 `+0x3328`"因此有了确凿答案：置位只有包装器末尾那一处**；包作器（`main+0x15f0ce4` 起）是派发器挂进
作业池的**作业体**，它跑完任务的函数指针（`[[rec]+0x10]`）之后才置位——也就是说：**位置不上位，只可能是
"这个任务没跑完"或"这一位被清掉了"**。

**二、上膛点（这是本轮最重要的定位）**：`main+0x1636f3c` 起的一小段就是"跑这张图"的入口
（游戏侧从 `0x856f50` 调进来）：

```asm
1636f3c: ldr  x9, [x26, #0x5e40]   ; 任务数（同一个字段：图 = x26+0x2D40 时它就是 图+0x3100）
1636f40: mov  x10, #-1
1636f44: lsl  x9, x10, x9
1636f48: mvn  w9, w9               ; w9 = 低 count 位全 1
1636f4c: mov  x0, x21              ; x21 = 任务图
1636f50: str  w9, [x8]             ; **上膛：图+0x3324 = 低 count 位**（x8 = x26+0x6064）
1636f54: str  wzr, [x8, #4]        ; 清完成位图（图+0x3328）
1636f58: bl   0x15ee598            ; 派发器
1636f5c: mov  x0, x21
1636f60: bl   0x15ee898            ; 轮询（等全部完成）
```

**派发器的条件**（`0x15ee598`，一次调用把**所有**满足条件的任务都挂进作业池，不是一次一个）：
`+0x3324` 的第 i 位置着 ∧ `rec[i]+0x18 ⊆ +0x3328`（依赖 A）∧ `rec[i]+0x1c ⊆ +0x3320`（依赖 B）；
满足则原子清掉 `+0x3324` 的那一位，再建作业 `{函数指针, 序号 i, 图}` 挂进全局作业池队列
（池对象 = `[0x31143d0]`，队列锁在 `+0x28`，队列头 `+0x68`/尾 `+0x70`）。
所以**`+0x3324` 的位在被取走之后就没了**——"待派发 0"既能读成"从没上膛"，也能读成"全被取走了"。

**三、真机读数（三轮，`.work/logs/r1/r2/r3.log`）**：

| 轮 | 图 | 任务数 | 已完成 `+0x3328` | 待派发 `+0x3324` | 依赖源 `+0x3320` | 出图 |
|---|---|---|---|---|---|---|
| r2 | `0x7bfcf15d40` | 3 | 0 | 0 | 0 | 出图未到停点（小图那轮） |
| r3 | `0x7bfa5a1d40` | **22** | 0 | 0 | 0 | 62（常态停点） |
| r1 | `0x7bf7e45d40` | 22 | 0 | 0 | 0 | 59 |

任务记录（22 条，`序号:依赖A/依赖B/标志`）：`0:0/0/0x19 1:0/0x2/0x9 2:0/0x2/0x14 3:0x1/0x2/0x19
4:0x1/0x2/0x4 5:0/0x2/0x18 6:0x19/0x2/0x9 7:0x39/0x2/0x9 8:0/0x2/0x18 9:0x8/0x2/0x9 10:0/0x4/0x9
11:0xd/0x8/0x9 12:0/0x3/0x18 13:0x1001/0x10/0x9 14:0x1105/0x20/0x9 15:0x9/0x10/0x9 16:0x1105/0x20/0x19
17:0x1005/0/0x19 18:0x1005/0x20/0x9 19:0x5/0/0x19 20:0x1005/0x50/0x19 21:0x177000/0/0`
⇒ 这是一张**以任务 0 为根**的依赖图：3/4 依赖 0，6 依赖 0/3/4，7 依赖 0/3/4/5，13/14/16/18/20 依赖
任务 0（位 0）、12…… 也就是"**任务 0 不完，整张图就停**"。

主线程现场（r3；新仪器读出的调用链，`main` 内的返回地址 + 包装器帧）：
`渲染码(0x856f50) → 跑图入口(ret 0x1636f64) → 轮询(0x15ee898) → 泵(0xdf9794，"边帮忙边等") →`
`任务包装器(0x15f0ce4) → 派发器(0x15ee598) → 解锁(0x1b76d40)`。
**也就是：主线程自己正在执行这张图的任务**（包装器帧就在栈上，而包装器末尾就会置完成位），
**而完成位图读出来是 0**。

同一现场里工人的锁读数（这一轮新加的"最近一次 ArbitrateLock 的锁字"）：

| 线程 | 最近一次 ArbitrateLock 的地址 / 锁字 | 期望持有人 |
|---|---|---|
| `0x1005` | `0x7c2d11f818` = **`0x40001001`**（等待位 \| 持有人 `0x1001`） | `0x1001`（主线程）→ **确实在等它** |
| `0x1004` / `0x1006` / `0x1007` | 锁字 **0** | 期望的持有人都对不上 ⇒ 没卡在锁上 |
| `0x1001` 主线程 | `this+0x3460` 锁字 **0** | 期望 `0x1005` 只是"试一下"，没卡住 |

⇒ 「M3-2r」把"工人等主线程持有的锁"记成**副产物**这一条要改口径：它确实是**互等环的一半**
（工人等主线程持有的锁字 `0x40001001`），另一半在主线程持锁这件事上（`0x7c2d11f818` 是堆上的对象，
不是这张图的字段）。而"有没有真的互等"要看主线程是否真的持有它——这一条本轮没量到，见下面「下一步」。

**四、矛盾在哪（本轮留下的唯一悬念）**：三条读数**不可能同时成立**：
① 上膛点把低 `count` 位全写上（`count` = 22，轮询用的也是这个字段，`~(-1<<22)` = `0x3FFFFF`）；
② 派发器只清"被取走"的位，**依赖不满足的位会留在 `+0x3324`**（`b.ne` 跳回循环，不清）；
③ 实测 `+0x3324 == 0` 且 `+0x3328 == 0`。
按 ① ②，只有在"22 个任务**全部**被派发过"时 `+0x3324` 才会是 0；而"全部被派发过"又要求依赖 A 都满足
（即完成位图里已经有 0/3/4… 这些位）——可完成位图是 0。反过来，若上膛时 `count` 读到的是 0 或很小的值
（例如上膛用的字段与轮询用的**不是同一个值**），那 `+0x3324` 与 `+0x3328` 同时为 0 就自洽了，
但"主线程栈上有**活的**包装器帧"又说明**确实有作业在被执行**。
两者不能同时为真 ⇒ 必然有一处是"**飞行中被抹掉**"或"**上膛与轮询看的不是同一个数**"，而**静态读到这就到尽头了**：
`+0x3328` 另有清零点（`main+0x15eef90`，只清它、任务数与待派发都不动——**与实测的三张位图状态完全吻合**），
它是"另一条跑图路径"（`main+0x15ee9d8`，游戏侧从 `0x853b38` / `0x857270` 调）的收尾；
而同一条渲染码里 `0x853b38`、`0x856f50`、`0x857270` 是**相邻的三次跑图调用**。

**五、新仪器（`dm/host/hle_thread.c`，全部只读、默认不改行为）与下一步**：

| 仪器 | 读什么 |
|---|---|
| `peek32` + 三张位图 | `+0x3320/+0x3324/+0x3328` 与两个"在飞作业表"长度（`+0x3470`、`+0x3488`） |
| 任务记录一览 | 每条任务的 `依赖A(+0x18)/依赖B(+0x1c)/标志(+0x58)` |
| 包装器帧捕获 | `[x29-8]` = 该作业的图、`[x29-0x18]` = 序号（**注意**：包装器范围里也会捕到"从包装器尾调进来的**派发器**"帧，那种情况下读到的是派发器的循环下标，不是任务序号） |
| 最近一次 `ArbitrateLock` 的锁字 | 期望持有人（`x0`）与该地址现在的锁字：互等环的另一半 |

下一步（按顺序，都还是"量"不是"改"）：
1. **时间序列**（唯一能把第四节的甲/乙分开的一步）：主线程每次 SVC 时采样那张图的三张位图与 `count`，
   **值一变就打一行**。一眼就能看出是"上膛 22 位 → 被取走"、"上膛 0 位"、还是"完成后被清"。
2. **宿主侧锁账本**：`arb_lock`/`arb_unlock` 记下"这个地址我们判给了谁"，停住时把锁字与账本对一遍——
   回答"主线程真的持有 `0x7c2d11f818` 吗"、以及"那次移交写下去的 tag 对应的线程真的被唤醒了吗"。
3. 顺带：核 `图 == x26 + 0x2D40`（从跑图入口的帧里读 `x26`），并直接读 `x26+0x6064`（上膛点真正写的那个单元）。

---

### M3-2t：三张位图的时间序列 —— **甲否掉、乙成立**；顺带改掉三条旧口径（2026-10-15 真机三轮）

「M3-2s」第五节第 1 条那个时间序列做完了，「M3-2s」第四节的甲（上膛 0 位）/ 乙（完成后被清）有了确定答案。
下面六节：**仪器**、**反汇编更正**、**三轮读数**、**结论**、**顺带照出来的三条**、**下一步**。

**一、仪器（`dm/host/hle_thread.c`，默认开、只读、`DM_TG_SERIES=0` 可关）**

两条来源合流到同一个"变了才打一行"的过滤器：

- **宿主采样线程**：首次锁存到图就起一条 pthread（64 KiB 栈），每 `DM_TG_SERIES_US`（缺省 **500 微秒**）读一遍；
- **SVC 钩子**：任何已锁存到图的线程，每次 SVC 也读一遍 —— 派发器真在跑时它每几微秒就打一次锁 / 解锁，
  所以"上膛之后、任务被取走"那段窗口只有这条路有机会看见。

为什么不能只在 SVC 钩子里采：**停住的主线程是在客体里空转，可以十几秒不发一次 SVC**（「M3-2r」r3 的现场就是
`调用链 15.0 秒前` + `/proc` 状态 `R` + `系统调用 running`），只按 SVC 采会一无所获。

读的六个数：`图+0x3100` 任务数、`+0x3324` 待派发、`+0x3328` 已完成、`+0x3320` 依赖源、`+0x3470` / `+0x3488`
（两个表的长度；`+0x3470` 那个的表头在 `+0x3468`，包装器尾会扫描它并把本作业摘掉）。明细行有预算
（`DM_TG_SERIES_LINES`，缺省 5000 行；**第一轮用 200 行、5 秒就花光，正好把要看的区间漏掉**）；整个运行期另有
累计（**非零次数 / 历史最大值 / 第一次非零的时刻**），停住时由 `tg_series_summary` 打进现场转储 —— 那才是
判甲/乙的读数。

**二、反汇编更正：`[x29-8]` 是**调用者**的 x19**

「M3-2s」的读法（"[x29-8] = 派发器保存的 x19 = 图"）在**方向上**是反的：`stp x20,x19` 存进去的是**进来时**的值，
也就是**调用者**那个活着的 x19。所以"某帧的返回地址落在函数 F 的区间里"只说明**该帧是被 F 调进来的**，
`[x29-8]` 是 **F 的** x19。判据因此要挑"调用者手里正拿着图"的调用点，并逐条在真机上验：

| 调用点区间 | 调用者 | 调用者的 x19 | 真机结果 |
|---|---|---|---|
| `[0x15ee598,0x15ee894)` 派发器 | 派发器（序言 `mov x19,x0`，x0 就是图） | **图** ✓ | 成立（第 2、3 轮就是这么锁上的） |
| `[0x15f0ce4,0x15f0e98)` 包装器尾（`0x15f0e78 bl 派发器`） | 包装器（尾上 `mov x0,x19`） | **图** ✓ | 成立 ⇒ 那一帧是**派发器**的帧 |
| `[0x15ee898,0x15ee9c0)` 轮询 | 轮询（`mov x19,x0`） | **图** ✓（等待对象） | 成立（第 1 轮就是这么锁上的） |
| `[0x1636f3c,0x1636f68)` 跑图入口 | 跑图入口 | **常数 `0x6064`** ✗ | **不成立**：真机照出 `0x1001 帧判据命中（main+0x1636f5c）但 [x29-8] = 0x6064 不是任务图` |

最后一行就是为什么要加校验：判据取到地址后必须过两道 —— ① `图+0x3100` 任务数 ∈ 1..64；② **两张"按任务序号置位"的
位图（已完成 / 待派发）不许出现超出任务数以外的位**（依赖源 `+0x3320` 是另一种掩码，见过 `0x7f`，不参与）。
②比①强得多，几乎挡掉所有"地址认错但恰好读到个小整数"。另一处口径更正（同一份反汇编）：旧"包装器帧"那条判据
实际认出的帧是**派发器**的帧（`0x15f0e78` 那句 `bl` 的返回地址 `0x15f0e7c` 落在包装器区间里），所以那一行打印的
`[x29-0x18]` 是**派发器的循环下标（x21）**、不是任务序号；`+0x3100` 那个数也一直是**任务数**（22）、不是"等待掩码"。
两处标签都改了。

**三、三轮读数（`.work/logs/t1.log` / `t2.log` / `t3.log`）**

| 轮 | 图 | 任务数 | 待派发 最大 | 已完成 最大 | 时序覆盖 | 出图轨迹 | 结尾 |
|---|---|---|---|---|---|---|---|
| t1 | `0x7bf7c0fd40` | 22 | `0x3ffffe` | `0x3fffff` | 3.98 → 21.53 s | 0 → 375（末条"在推进"） | 看门狗转储（转储前后图**还在变**） |
| t2 | `0x7bf87cfd40` | 22 / 3 | `0x3ffffe` | `0x3fffff` | 3.75 → 7.55 s | 0 → 82 后**停** | 设备丢失 → 窗口被拆（`TERM_WINDOW`） |
| t3 | `0x7bf9e19d40` | 22 / 3 | **`0x3fffff`** | `0x3fffff` | 1.88 → 19.51 s | 0 → 379（末条"在推进"） | 看门狗转储（**还在正常出图**） |

每次运行的形状（t3 最完整）：
`上膛 待派发 = 0x3fffff（一次全置上） → 派发器逐位取走（待派发 → 0） → 已完成 逐位置上到 0x3fffff（22 位全完） →`
`下一次上膛把已完成清 0`，一圈几十毫秒（跟着出图节拍走）。另外：**同一地址会被复用成任务数不同的图**（真机见到
22 与 3 共用一个地址 ⇒ 对象是池化的、复用成不同图的定义），所以仪器必须允许"图地址换" —— 第一版只认第一次，
第 2 轮 7.5 秒之后就跟丢了。

**四、结论**

- **甲（上膛 0 位）否掉**：待派发的最大值 `0x3fffff`（t3 直接抓到整张上膛）、`0x3ffffe`（t1 / t2）——上膛确实写了
  低 `count` 位，派发器再逐位取走。
- **乙（完成后被清）成立**：已完成反复到 `0x3fffff`（22 位全置），随后被**下一次上膛**（`main+0x1636f54`）清成 0；
  `+0x3328` 那条"另一条跑图路径的收尾"（`main+0x15eef90`）本轮没有出场的证据。
- 所以「M3-2r」的"**22 个任务从未被派发**"与「M3-2s」的"**三张位图同时为 0**"**都不是停住时的状态**：后者是运行中
  一个**瞬态**（全部派发完、还没置上任何完成位），随手一读就会读到 —— 「M3-2s」第四节那处"说不通"到此收口。
- **任务图这套机制是好的**：上膛、依赖、派发、置位、清位全部对上，任务确实跑得完（22 位全置出现过很多次）。
  **卡点不在这里。**

**五、顺带照出来的三条（比上面更该记）**

1. **看门狗的"无进展"不等于"客体停住"**：`crash_watch_progress(cur_tick(), …)` 只在**引擎拍入口**（`diag.c:1752`）
   与 **UI 消息**（`diag.c:381`）被调用，出图帧**只在 `pregame` 期间**算进展（`crash.c:44`）。本轮三轮的读数是
   "**出图在推进 + 拍 0**"（t3 末条心跳还是 `出图 379 … 出图在推进`），看门狗照样在第 15 秒判死、转储、`_exit`
   ⇒ **这份转储的含义是"引擎拍停了 15 秒"，不是"画面不动了"**。以后判"停住"要以**心跳里的出图数**与前端那条
   "**连续 8 秒没有新出图**"卡死报告为准，看门狗转储只当现场照。（顺带：转储很长，安卓上要 ~5 秒才写完，
   期间游戏线程照跑 —— 所以转储里夹着 `DRAW_IDX` 一类新行是正常的。）
2. **真出现"出图停"的那一轮与设备丢失同现**：t2 里 `【红】提交失败 VkResult -4` + 上百条 `贴图上传失败`，
   出图停在 82；随后窗口被拆（`TERM_WINDOW`，前台那条管线缓存还写成功了），**日志到此为止**。与「七」第 2 条
   （偶发的设备丢失）合流。
3. **停住现场里，主线程的"当前 PC"落在我们自己的宿主层**：t1 是 `vk_tex_view+0x28`（经 `mtl_exec` 从客体
   `main+0x17443dc` 调进来，外层客体帧是 `main+0x8572b8`），t3 是 `libdm.so+0x152950`（从 `main+0x124db10` 调进来），
   而两次另一条"空转"线程都在 `vk_tex_grab_ser`（t3 那次 PC 停在 `powf` ⇒ 它是**在算**查看器那条伽马换算，
   不是死等；"卡住采样"的"在用户态空转"只表示没在系统调用里）。**而同期的图还在变**（t1 到 21.5 s、t3 到 19.5 s）
   ⇒ 该查的是**宿主侧的读回 / 队列等待**（`vk_tex_view` / `vk_tex_grab_ser` / `mtl_wait`），不是客体的任务图。

**六、下一步**

1. ~~时间序列~~ **已做完（本节）**：任务图这条线结案（机制正常），判"停住"的判据换成"**心跳出图 + 卡死报告**"。
2. **宿主侧读回 / 队列等待**（接第五节 3）：`vk_tex_view` / `vk_tex_grab_ser` / `mtl_wait` 三条路上"谁在等谁"
   （已有 `g_one_mu` 与"一次性提交"两处共享），与「七」第 2 条的设备丢失一起量。
3. **核 `图 == x26+0x2D40`**（「M3-2s」第 3 条）**降级为顺带**：本轮的强校验已经在真机上挡过一次"地址认错"
   （`0x6064` 那次），它不再是前提。
4. **宿主侧锁账本**（「M3-2s」第 2 条）**优先级下调**：那个互等环是"图在跑"时的伴随现象，而图已被证明跑得完；
   等第 2 条有结论再回头做。

改约 20 行、增量构建约 40 秒、一轮真机约 25 秒。

---

### M3-2u：「停住」当场取到现场了，而且它**跟我们自己的读回诊断有关**（2026-10-15 真机五轮，A/B/C 三臂）

「M3-2t」把判据换成"心跳出图数 + 前端卡死报告"之后，这一轮先做"**把停住认出来**"，再做**唯一变量**的 A/B/C：
结果干净得超出预期 —— 停住能稳定复现，而且**只在"查看器读回 + 逐级扫描读回同时在路上"时出现**。顺带加了一件
早就该有的仪器（按需现场导出，进程不退出）。

**一、新仪器：按需转储（`dump.trigger`）**

安卓侧原来没有"按需取现场"的触发口：iOS 有 `DM_LOG_PORT` 的 `dump`，这里只有看门狗那条**导完就 `_exit`** 的路，
而停住要盯很久才出现、一跳就自杀会把观察打断（实测还有一条更糟的：直接给进程发 `SIGALRM` 走 `on_crash`，
**转储的尾巴整段被切掉** —— `_exit` 不等日志泵，`on_crash` 那条路没有看门狗那 400 ms 的等待）。现在：

```bat
adb shell touch /sdcard/Android/data/ai.d2rm.dm/files/dump.trigger
```

`main_apk.c` 每秒查一次这个文件，见到就删掉并导一份（各线程在等什么 + 图形侧等待点，与看门狗同一份），
**进程继续跑**；要连导几次就再建一次。看门狗因此可以放心给一个很大的值（本轮 `DM_WATCH_STALL=600`），
"什么时候取现场"由人决定。

**二、A/B/C 三臂（`DM_TG_SERIES=0`、`DM_WATCH_STALL=600`，其余逐字相同，每轮只差一个开关）**

| 臂 | 查看器 `DM_WIN_SRC` | 逐级扫描 `DM_NVN_SHOT_SCAN` | 出图轨迹 | 结果 |
|---|---|---|---|---|
| **A** `u_a1` / `u_a2` | **28 开** | 开（50/54/57） | 52 → 冻住；55→57 → 冻住 | **停住**（两轮都是） |
| **B** `u_b1` / `u_b2` | 0 关 | 开 | 67→1704（80 s，**21.5/s**）；67→1247（60 s） | **不停** |
| **C** `u_c1` | 28 开 | **关** | 70→1077（70 s，**14.4/s**） | **不停** |

⇒ ① 查看器本身吃吞吐（21.5 → 14.4 帧/秒，约 −33%），但**不会**卡死；② 停住需要**查看器与扫描同时读回**
（A 轮出图冻在 52~57，而 50/54/57 正是扫描触发的帧）；③ B/C 两臂分别是"只有扫描""只有查看器"，都干净地跑满
观察窗口。

**三、停住那一份现场（A 轮按需转储）**

- `心跳：出图 52（55/57）` 连续几十秒不动，`绘制 有目标 4431 / 无目标 1227` 也冻住 ⇒ **客体完全停止推进**，
  前端那条 `卡死报告：连续 8 秒没有新出图` 同时出现。
- 主线程 `0x1001`：`调用链（32.1 秒前）` = `main+0x15ee940 → main+0x1636f64 → main+0x856f50 …`（泵 → 轮询 →
  跑图入口 → 渲染码），`svc 环` 16 条全是 `SleepThread(-1)`，**此后 32 秒一次 SVC 都没发**、`/proc` 状态 `R`、
  `系统调用 running` ⇒ 它卡在**用户态**（客体或宿主），且不再经过任何门。
- `卡住采样：在用户态空转的线程 0 个` —— 采样器一个都没抓到 ⇒ **所有客体线程都在系统调用里等**（与"主线程 R"
  并存只能说明它卡在**宿主侧的 futex / 锁**上：futex 自旋那一小段就是 R）。
- 工人 `0x1005`（优先级 28）：`等:锁 addr=0x7c29693818 持有者 0x1001` —— **它等的锁持有人是主线程**（与「M3-2r」
  同一个形状）；`0x1004` 等条件变量（池的正常等待）。
- 图形侧等待点：`在飞槽 1/8（栅栏 0 1228 …）`（A 轮是 `61`）、`提交 1228 / 出图 1224`、`队列锁 无人持有`。

**四、两条口径更正（都影响以后的读法）**

1. **"在飞槽 1/8、提交比出图多 4"不是卡死签名**：C 轮**明明没停**，读出来是同一形状（在飞槽 1/8、1228/1224）。
   卡死的签名是**这些计数冻住不动**（A 轮停在 61/57）+ **主线程不再发 SVC**（调用链的时间戳可判：`（32.1 秒前）`）。
2. **读回是在客体线程执行命令流的过程中跑起来的**（这条解释了为什么卡的是客体线程）：把停住时采到的宿主 PC
   符号化，t3 的 `libdm.so+0x152950` = **`nvnth_378`**（`nvn_thunks.S:3818`，NVN 门），而 t1 的链是
   `vk_tex_view+0x28 ← mtl_exec+0x2fc0 ← dm_gfx_suspend`（`hle_nvn.c:774`）`← 客体 main+0x17443dc` ⇒
   查看器/扫描的读回与建视图发生在 `mtl_exec`（命令流解释器）里，也就是**客体那个线程自己**在跑 ——
   所以它一卡，整条出图就冻住。

**五、下一步（读回路径的锁账本）**

现在有了稳定的复现配方，下一步是**给读回这条路做一本小账本**（都是"量"）：在 `vk_tex_grab_ser` /
`grab_ser_locked` / `stg_get` / `one_begin` / `vk_queue_lock` / `vk_tex_view` 这些点上记"进入 / 离开 + 当前
持有者"，停住时按需导出一份 ⇒ 直接读到"谁在等谁"（现在只知道"卡在宿主侧等一把锁/futex"，还差**是哪一把**）。
已知的共享面（`g_one_mu` 串行两个读回调用方、`one_mu → g_mu` 的锁序、上传走自己的命令缓冲）都在
`vk_tex.c:320-404` 的注释里，账本要点名的就是它们之间**还没被覆盖**的那条边。

**复现配方（下次直接照做）**：`DM_WATCH_STALL=600` + `DM_WIN_SRC=28` + `DM_NVN_SHOT_SCAN=50,54,57:…` +
`DM_TG_SERIES=0` ⇒ 约 20 秒内出图冻住（不保证每次都冻：本轮 7 次里 5 次冻、2 次干净）；盯 `心跳` 的"出图"数，
冻住 15 秒后 `adb shell touch …/files/dump.trigger` 取现场（进程不退出，可以再取第二份对照）。

---

### M3-2v：给读回路径做的锁账本把"我们自己"排除了，并把光标挪到**宿主侧一次轮询式等待**上（2026-10-15 真机七轮）

「M3-2u」把停住缩到"查看器 + 逐级扫描同时读回"，剩下的问题是"卡在哪把锁上"。这一轮把账本做出来，结论是
**不是我们自己的锁**：三个可疑点全部排除，而卡住的那条线程落在**宿主侧一次 `nanosleep` / `usleep` 轮询**里。

**一、账本（`dm/android/vk/vk_tex.c` 的 g_one_mu + `nvn_vk.c` 的栅栏）**

- **读回锁 `g_one_mu`**：记持有者 tid、在做什么（phase）、已持多久、门外是谁与等了多久；>300 毫秒各打一行日志
  （不看转储也能读出"谁被谁挡住"）。放进 `mtl_wait_state_dump` 的"等待点"那一段一起导出
  （`vk_led_line()`，声明在 `vk_tex.h`）。
- **客体等栅栏**：`inflight_take`（槽池满时等最老的）与 `mtl_wait`（客体按 NVN fence 语义自己等）都记
  "有没有人在等 / 等哪个槽 / 从什么时候开始"，>300 毫秒打一行；转储里另加一行**直接问驱动**
  `vkGetFenceStatus`：`槽的栅栏实况（槽:栅栏值/驱动状态，-1 = 槽空闲）` —— 这一行把"驱动没做完"与"我们的记账没跟上"分开。
- **停住采样器判据放宽**（`dm/host/diag.c`）：原来只采"状态 R **且** `/proc syscall` 报 `running`"的线程；
  `nanosleep` 轮询式等待在"睡 1 毫秒"那半拍是 `S`、在"看一眼"那半拍是 `R`，两次读还会串，于是**五次转储里
  一个样本都取不到**（现场只剩一个光秃秃的 `R`）。现在 `syscall = nanosleep` 也采 —— 一次就抓到 8 个线程。

**二、停住时的三处读数（v_a2 / v_a5，两轮同形）**

```
[vk] 等待点：已提交到栅栏 68、在飞槽 0/8（各槽的栅栏值 0 …）、队列锁 无人持有；窗口队列 自己的
[vk] 读回锁：无人持有、没人等
[vk] 等栅栏：没人在等
[vk] 槽的栅栏实况（槽:栅栏值/驱动状态，-1 = 槽空闲）：0:0/-1 1:0/-1 … 7:0/-1
```

⇒ **不是 `g_one_mu`、不是队列锁、也不是我们槽位的栅栏记账**（那一刻 8 个槽全空、驱动的栅栏全就绪，
>300 毫秒的等待日志一条都没有）。客体侧那一边照旧：主线程 `svc 环` 16 条全是 `SleepThread(-1)`、
**20~32 秒不再发一次 SVC**、调用链时间戳同岁；工人 `0x1005` 等的那把锁持有人 = 主线程；图停在
"全部派发、没有任何完成位"（已完成/待派发/依赖源 0、在飞表 2）。

**三、卡住的那条线程到底在哪（放宽判据后第一次拿到它的宿主链）**

```
STALLSAMPLE #1 [25.273s] thr=0x1001 pc=nanosleep+0xc lr=usleep+0x54
bt: … mtl_wait+0x214  ←  h_nvnSyncWait（dm/gfx/nvn/hle_nvn.c:383）  ←  客体 main+0x1743a0c
     ←  main+0x85745c  ←  main+0x856a38  ←  渲染码  ←  帧循环
```

⇒ **客体主线程卡在宿主侧一次 `nanosleep`（经 `usleep`）的轮询里**，链上最近的一处是我们自己的
`nvnSyncWait → mtl_wait`（同一个形状在「M3-2u」的 r3 也出现过：`mtl_wait+0x184 … 客体 main+0x1743a0c main+0x85745c`）。
这正是"必须把采样判据放宽"的那一条：**旧判据在两次半拍上都把它漏掉了**。

**四、由此得到的下一步（很窄：把宿主侧落在客体线程上的等待全部点名）**

现在只知道"卡在宿主侧一次轮询式等待"，还差**是哪一处**。下一轮把宿主里**所有**可能在客体线程上
`usleep` / `nanosleep` 的点都记一笔进出（用上面同一套账本即可）：`mtl_wait` 的 timeout 分支、
`nvnSyncWait`（`hle_nvn.c:383`，它把客体的 timeout 原样交给 `mtl_wait` ⇒ 超大 timeout 就是"睡到天荒地老"）、
`diag.c` 里等"读取画面已撤"的那个循环（`usleep(10000)`，带 15 秒上限）、`hle_thread.c` 的睡眠循环。
一次就能点名 —— 之后才谈"该不该给这类等待加上限 / 分段复查"（**先量后改**）。
顺带一条**待复核**的口径：`mtl_wait` 在 `timeout_ns = 0` 时会回落到 1 秒（`timeout_ns ? timeout_ns : 1000000000ull`），
而 `nvnSyncWait` 第一次调用正是 `mtl_wait(o->fence, 0)`（本意是"看一眼有没有到"）——这两处混起来的地方要一起看。

---

### M3-2w：宿主侧等待站点账本 —— 主线程"0 层"与「M3-2v」那条链的作废（2026-10-15 真机六轮）

「M3-2v」把光标挪到"宿主侧一次轮询式等待"上；这一轮把它做成**一套统一账本**，并把"点名"需要的那件仪器
（**栈扫描**）做出来。插桩与仪器都已落地，最后的读数见第五节。

**一、账本：统一的"等待站点"进出记录（`dm/host/crash.c`）**

- 三个入口：`dm_waitled_enter(site)` / `dm_waitled_leave()` / `dm_waitled_dump()`（声明在 `dm/host/dm.h`）。
  **实现在宿主侧**（不在 `nvn_vk.c`）：账本要能被**每一种链接组合**取到 —— 探针（`vk_pipe_probe` 链 `vk_pipe.c`
  但**有意不链宿主**）就给它一份空实现（新增 `dm/android/vk/vk_waitled_stub.c`），否则链接直接缺符号（真机上先踩过）。
- **站点清单**（同一套账本，站点名就是人看得懂的中文）：
  - 我们写的等待：`mtl_wait`（含 `timeout_ns = 0` 回落 1 秒那条）、`nvnSyncWait`（第一次 `mtl_wait(o->fence, 0)`
    也走这里）、`gfx_enter`（等图形挂起）、`mtl_drain` / `mtl_finish`（等队列空闲）、`inflight_take`、
    读回 / 对照 / 贴图上传（建中转缓冲 + 分配 + 映射）、`hle_thread.c` 的 `SleepThread` 睡眠、
    `diag.c` 等"读取画面已撤"的循环、`hle_hid.c` 的测试停顿、以及 `nvnQueueSubmitCommands` /
    `nvnQueuePresentTexture`（客体在宿主里的两个最外层入口 —— 包住它们，等于把"命令流执行"整段也算进来了）。
  - **对驱动的调用**：`vkQueueSubmit` / `vkQueueWaitIdle` / `vkQueuePresentKHR` / `vkAcquireNextImageKHR` /
    `vkMapMemory` / 建图形与计算管线 / `vkUpdateDescriptorSets` / 建 `VkImage` / 视图（含子视图）/ 采样器 /
    缓冲视图 / 拆贴图（销毁视图与图像 + 释放内存）/ 描述符池 / 描述符集。**为什么连这些也记**：这是唯一一类
    "看起来只是 CPU 操作、却可能在驱动里等 GPU"的调用，之前一直没被覆盖。
- 读数：每次进出记 `tid` / 客体句柄 / 站点名 / 进入时刻；**>300 毫秒**离开时自动打一行日志；转储里列"当前打开的
  站点"，并给**自检**：账本用过的槽、每个槽的 `tid` / 客体句柄 / 累计进过次数 / **最后等完的是哪个站点、多久之前**
  （自检是为了防"账本自己坏了、看着却像没人等"——「M3-2v」的教训）。
- 一处**口径提醒**：`dm_waitled_enter` 走 `pthread_getspecific`，**前端线程与宿主线程也共用同一套** ——
  转储里那条"客体线程 0"（例如 `tid 20143`，最后等完的是"vk：上屏呈现"）就是**前端线程**，别当成客体。

**二、六轮真机读数（A 配置照「M3-2u」的复现配方；六轮都冻）**

1. 「等待站点」那一段**永远只有其它客体线程的 `SleepThread` 短等**（1~50 毫秒）；**>300 毫秒的等待一条都没有**。
2. **主线程（0x1001）永远"0 层"**：它累计进过 **14340 / 21204 / 21777** 次账本，而卡住那一刻**不在任何站点上**；
   它"最后等完的站点"是**「客体：出图（`nvnQueuePresentTexture`：入队到出图线程）」**，其中一轮**那一次在里面待了
   11.1 秒**（扫描 + 读回都在里面），出来后**6.9 秒没再进过任何站点**。
3. 采样：`pc=nanosleep+0xc lr=usleep+0x54`（与「M3-2v」同形），其它客体线程落在 `svc_thread` 的 `nanosleep`。
4. 主线程**按名字无条件扫栈**的那一份（**发采样信号之前**的时刻）里：**没有任何 `libvulkan` 地址、也没有 libc 帧**
   ⇒ 它那一刻**不在 `usleep` / `nanosleep`**，而是在"睡一下、看一眼"之间来回。同一轮里看门狗线程与音频线程的栈
   都解对了（`watchdog_main` / `null_sink` ✓）⇒ 方法本身可信。

**三、被推翻的：「M3-2v」那条宿主链不能用**

- `mtl_wait+0x214`（`0x8d63c`）反汇编是"`cmp fence, g_fence - MAX_INFLIGHT` → `b.hi …` → **`return 1`**"这条
  **"早已滑出在飞窗口、直接返回"** 的返回地址 —— 它**不等待**，所以它出现在"卡住线程"的链上只能是**栈上的残留**
  （帧链从一个不对的 x29 读出来的旧值）。
- 加固这条结论的第二个事实：**Android arm64 的返回地址带 PAC**（签名位在**高 16 位**），带签名位的地址 `dladdr`
  一律失败 ⇒ 回溯里那串 `0x24c6fca47ce57c` 形状的"垃圾地址"就是这么来的。`crash_off` 现在先
  `&= 0x0000ffffffffffff` 再符号化（用户空间只用低 48 位，对正常地址无副作用），能救回这一批帧。
- 试过并在真机上**撤掉**的路：在采样信号处理器里调 `_Unwind_Backtrace` 取完整链 —— 它**把被采样线程卡住了**
  （那一轮"新增样本 0"、处理函数没回来）。已回退，教训写进「七」的坑清单。

**四、现在的仪器：栈扫描（"读栈 + 只留可执行段"）**

- **同时刻**：采样处理器里从 `uc_mcontext.sp` 起 `memcpy` **2 KB 自己的栈**进环形缓冲（`sp` 一定落在已映射的栈里，
  所以安全；对比之下 unwind 不安全 —— 见上一条）。
- **别的线程**：`/proc/self/task/<tid>/syscall` 给 `sp`（**要在发采样信号之前读**：信号一打进去它就变成 `running`
  而没有 `sp` 字段），再 `pread` `/proc/self/task/<tid>/mem` 4 KB（**不会**让读的人段错误）。
- **过滤**：栈上"看着像代码地址"的**数据指针**很多（实测一条 4 KB 的栈能挑出十几个 `g_inf` / `desc_write` 之类的
  `.bss` 地址，正好把要看的那几帧淹掉），所以按 `/proc/self/maps` 的段权限**只留可执行段**里的值 —— 那些才是
  返回地址。**为什么不用 `dl_iterate_phdr`**：`<link.h>` 的 `Elf64_Sym` / `Elf64_Dyn` / `Elf64_Rela` typedef 与
  `dm.h` 里那份**重名**，一 include 就 `typedef redefinition` 编译不过（真机上先踩过）。
- **主线程按名字无条件扫**（`thr_find_by_name("主线程", …)`）：它在"睡 / 看"之间来回，通用的空转判据漏过它一次
  —— 八条里漏掉的那一条恰好是最要紧的那条。

**五、还没做完的那一步（下一轮从这里开始）**

插桩与仪器都编进 `.work/apk-build/dm.apk` 了，最后一轮真机取现场留在下一轮。**第一件事**：照配方复现 →
`touch …/dump.trigger` → 读新转储里的 **"栈上的返回地址 thr=0x1001 …"** 那一行（那是**同时刻**的栈），
"谁在 `usleep`"就在那里。**先量后改**：点完名再谈"该不该给这类等待**分段 / 加上限**"。
⇒ **已于「M3-2x」完成**（那一步先照出了仪器自己的假阴性）。

---

### M3-2x：点名完成 —— 停住是"驱动约 10 秒不回答"，不是我们的 `usleep`；顺带修掉一个仪器的假阴性（2026-10-15 真机五轮）

**一、先修仪器：可执行段表 64 槽 → 1024（`dm/host/diag.c`）**

- 真机实测：进程里有 **455~460 个可执行段**（`/proc/self/maps` 里 `r-xp` 的行），而 `g_execseg[64]` **只留前 64 个**。
  留下谁 / 丢掉谁恰好决定了你会看到什么：**`libdm.so` 第 24 个（留下）、`vulkan.adreno.so` 第 43（留下）、
  `libc.so`（bionic）第 403、`libvulkan.so` 第 404（两个都丢）**。
- 于是「M3-2w」那句"主线程那一份里没有任何 libc 地址 ⇒ 它不在 `usleep`"**是仪器的假阴性**：`usleep` 住在 libc 里，
  而 libc 从来没进过这张表（驱动反倒看得见 —— 它排第 43）。同一形状的副作用还有一处：**同一个地址会被印成两种样子**
  （`pc=libc.so+0xd7b88` 与 `lr=ioctl+0xa0` 其实是同一个地址 —— `dladdr` 能命名 `ioctl`，命不了 libc 里的 syscall 桩）。
- 现在：表 **1024** 槽，且**表满时在读数里明说**（`（N 个）；可执行段 M 个，【黄】表满…`）——不许再无声地说谎。
  修后真机读数里 libc 帧正常出现（`libc.so+0x82d78 libc.so+0x751f4 …`），可执行段 460、无告警。

**二、点名：谁在 `usleep`**

- **我们这条链上没有一处宿主 `usleep` 能落在客体主线程上**：`dm/android` 与 `dm/gfx` 的安卓分支里根本没有 `usleep`
  （只有 `nanosleep`：`mach_wait_until`，`dm/android/compat/mach/mach.h:115`）；宿主侧那几个 `usleep` 点是看门狗
  （`crash.c:55`）、音频（`hle_audio.c:147`）、测试（`hle_hid.c:388`、`diag.c:1937/2007`），以及**唯一可能落在客体线程上的**
  「等『读取画面已撤』（`diag.c:2075/2084`，10 毫秒一轮、15 秒上限）」——而**它是记账的**。
- 修正后的四轮采样：`pc=nanosleep+0xc lr=usleep+0x54` 只出现在**我们自己的宿主线程**上 —— 看门狗
  （栈：`libc… ← libdm.so+0x95398`，符号化 = `watchdog_main`）与音频（`libdm.so+0x11bc94` = `null_sink+0x88`）；
  客体线程的 `nanosleep` 都落在 `svc_thread ← SleepThread`。
- **修正前**那一轮（表还没放开，`w_c1`）主线程**确实**被采到在 `usleep` 里
  （`STALLSAMPLE #1 thr=0x1001 pc=nanosleep+0xc lr=usleep+0x54`），而**当时账本是"0 层"**
  （`账本槽 tid 21393 = 客体线程 0x1001 … 现在 0 层，最后等完的是「客体：出图」`）⇒ 我们记过账的等待一处都不在
  ⇒ **那只可能是驱动内部**的轮询（`nvn_vk.c:166-170` 早记下：驱动在 `vkWaitForFences` 里"1 毫秒一睡再看"）。
  **⇒ 不是我们的 `usleep`、也不是客体的 `SleepThread`：是驱动自己在轮询，我们只是发起者。**

**三、真正的读数：停住 = 驱动约 10 秒不回答（五轮：10.05 / 10.16 / 10.64 / 10.15 秒）**

五轮的账本（都挂在**客体主线程**上，外层永远是「客体：排队提交命令（`nvnQueueSubmitCommands`）」）：

| 轮 | 主线程内层站点 | 等了 | 同一时刻别处在等什么 |
|---|---|---|---|
| c1 | （0 层，刚从「客体：出图」出来 66 毫秒） | — | `客体：排队提交命令` 10159 ms |
| c2 | `读回：等队列空闲（等驱动做完在飞提交）`（`vk_tex.c:950`） | 10519 ms | 读回锁持有 10529 ms |
| c3 | `一次性提交等空闲（读回 / 对照，等驱动）`（`one_end_on`，`vk_tex.c:488`） | 10051 ms | **前端线程**：`上屏：等上一帧的栅栏（无上限）` **10074 ms** |
| c4 | `vk：建图形管线（驱动内部编译）` | 10790 ms | `读回：等队列空闲` 10644 ms；读回锁 10654 ms |
| c5 | `vk：贴图内存分配` | 10268 ms | `读回：等队列空闲` 10149 ms；读回锁 10160 ms |

- **心跳**：c3 / c4 是**整段约 11 秒一条心跳都没有**（`21:15:07.6 → 18.4`、`21:15:27.2 → 38.5`），与账本里那 10 秒同一长度；
  c5 心跳不断但**出图卡在 73** 而"查看器 跳过"一路涨（读回锁被占）。**它们随后都自愈**（心跳恢复、出图继续）
  ⇒ 这一轮的"停住"要读作**约 10 秒的驱动级停顿（会重复出现）**，不是永久挂死。
- **同时刻的栈（把 libdm 符号化）**：主线程那一份是 `tex_of+0x580`（= `0x813f0: bl one_end_on` 的返回点）
  `← vk_tex_view+0x28 ← mtl_exec+0x2fc0`，pc 落在 **libc 的 syscall 桩**（`lr=ioctl+0xa0`）⇒ **在读回路径上等驱动**；
  另两轮 pc 落在 `vk_shader_get`（也在 `mtl_exec` 里）。**三轮都在 `mtl_exec`（执行客体命令流）的过程中做宿主的事。**
- **四个不同的驱动调用同时等 ~10 秒**（含前端线程的栅栏等待）⇒ 停的是**驱动**，不是某根线程，更不是我们的锁
  （「M3-2v」已把我们的锁排除干净）。
- **设备丢失没跟来**：这五轮里 `VkResult -4` / `提交失败` / `贴图上传失败` 都是 **0 条**
  ⇒ "约 10 秒的驱动停顿"**不需要**设备丢失就能出现（两者像是同一驱动的两种表现，但不是必然连带）。
- 另一条被顺手否掉的：`mtl_wait+0x214` **不是**任何 `usleep` 的返回点 —— 反汇编显示它是 `0x8de48: bl inflight_reap`
  的返回点（`inflight_reap` 里没有 `usleep` / `nanosleep`，只有 `vkGetFenceStatus` / 池销毁 / 时间戳读取），
  而它只在"已滑出在飞窗口 → 直接 `return 1`"这条**不等待**分支上；与「M3-2w」的结论一致，是栈上的残留。

**四、先量后改：点名之后才谈的三件事（都还没动代码）**

1. **不给 `usleep` 分段 / 加上限** —— 这类等待本来就不是我们的 `usleep`。该加的是**"驱动调用判活"**：
   `vkQueueWaitIdle` / `vkCreateGraphicsPipelines` / `vkAllocateMemory` 这类"可能十几秒不回来"的调用，
   超过 N 秒就**记一笔并让这一帧的诊断自觉退出**（现在它们是把整条链一起拖住）。
2. **读回不该在"执行命令流的中途"要求整个队列空**：`vk_tex.c:950` 与 `one_end_on` 都是 `vkQueueWaitIdle`，
   而它们跑在 `mtl_exec` 里（同一根客体线程正录着命令流）—— 把自己正要提交的东西等完，本身是自伤。
   可考虑把诊断读回挪到**帧边界**（`nvnQueuePresentTexture` 之后），或改成"只等这一小段拷贝的栅栏"。
3. **两个读回诊断的并发**：五轮里"查看器 + 逐级扫描"同时在路上时都出现 10 秒停顿。下一步用**修好的仪器**把
   「M3-2u」的 A/B/C 三臂再量一遍：**单开时那 ~10 秒停顿是否消失**（消失 ⇒ 是我们的读回把驱动推进去的；
   不消失 ⇒ 是驱动自己周期性的停顿，那就往"设备丢失 / 驱动恢复"那条线查）。

---

### M3-2y：A/B/C/D 四臂复量 —— 那 ~10 秒停顿**与诊断开关无关**；"每次启动落在两种状态之一"；设备丢失"先丢的是谁"第一次有答案（2026-10-15 真机十轮）

**一、为什么要复量**：「M3-2u」的 A/B/C 结论（"查看器 + 逐级扫描**同时**在路上 ⇒ 20 秒内冻；**只开一个** ⇒ 60~80 秒干净"）
是 **2 轮 vs 3 轮**的对比。「M3-2x」把仪器（可执行段表）修好之后，用同一条配方复量一次是本轮该做的第一件事。

**二、配方与量法**：四臂只差一个开关（`.work/ab-{A,B,C,D}.conf` + 量测脚本 `.work/ab-run.ps1`）——
A = 查看器（`DM_WIN_SRC=28`）+ 逐级扫描（`DM_NVN_SHOT_SCAN=50,54,57`）；B = 只有查看器；C = 只有扫描（`DM_WIN_SRC=0`）；
**D = 两路都关**。每臂 2 轮 × 65 秒；`DM_WATCH_STALL=600`（阶段一直是 `拍 0`，看门狗不会插进来转储）。
读数 = 心跳最大间隔 / >1 秒的等待（条数与最大）/ 读回锁 / 帧率 / `VkResult -4` 与 `贴图上传失败` 条数。

**三、读数（十轮，13.5 → 0.8 帧/秒）**

| 臂 | 轮 | 出图 | 帧率 | 心跳最大间隔 | >1 秒的等待（最大几条） | 备注 |
|---|---|---|---|---|---|---|
| A（两路） | r1 | 52 | 0.8 | **10.6 s** | 10336「客体：排队提交命令」/ 10187「读回：等队列空闲」 | 读回锁 10198 |
| A（两路） | r2 | 64 | 1.0 | 2.3 s | 2108 / 1995 | **`-4`×1、上传失败 83** |
| B（只查看器） | r1 | 53 | 0.8 | **11.9 s** | 11089 / 10871 | 读回锁 10879 |
| B（只查看器） | r2 | 67 | 1.0 | **11.1 s** | 10703 / 10491 | 读回锁 10500 |
| C（只扫描） | r1 | 75 | 1.2 | **11.4 s** | 11293「客体」/ **11087「上屏：等上一帧的栅栏（前端线程）」** | 读回锁 0 |
| C（只扫描） | r2 | 92 | 1.4 | **10.7 s** | 10513 / 10456 / 10439 / 10175（含前端栅栏 ×2） | 读回锁 0 |
| **D（全关）** | r1 | 72 | 4.9 | **11.8 s** | 10963（前端栅栏）/ 10942「一次性提交等空闲」 | **日志泵停了 45 秒**（真挂） |
| **D（全关）** | r2 | 72 | 1.1 | **11.8 s** | 11592 / 10255 / 10032 / 10013（前端栅栏 ×2 + 客体提交） | — |
| A2（两路） | r1 | **869** | **13.5** | **1.0 s** | **一条都没有** | 正常 |
| A2（两路） | r2 | 74 | 1.2 | 2.8 s | 2155 / 2005 / 1977 | **`-4`×2、上传失败 115** |

⇒ **那 ~10 秒的停顿与"两路诊断是否同时开"无关**：八轮里**轮轮都有**，**连 D（两路都关）也有**；
而 A2 r1（同一份 A 配置）**一次都没有**。「M3-2u」那条读法（"同时开才冻"）**今天不复现**
（它是 2 轮 vs 3 轮的样本；今天 10 轮里两种结果都出现）。"量帧预算时只开一个"这条建议**留着**（少一件事总是好的），
但**它不是"停住"的原因**。

**四、真正的形状：每次启动落在两种状态之一（这条比"停住"更值得记住）**

- **健康**（A2 r1）：13.5 帧/秒、**869 帧 / 64 秒**、绘制 **有目标 431003 / 无目标 641213**（≈**1234 op/帧**）、
  图形管线 **86**、**一条 >1 秒的等待都没有**、心跳间隔最大 1.0 秒。
- **生病**（A2 r2 / B r1 / C r1 / D r2 …）：1.1~4.9 帧/秒、**52~92 帧 / 64 秒**、绘制
  **4491~5811 / 1227~1250**（≈**100 op/帧**）、图形管线 **78~80**、每条 **2~11.6 秒**的停顿
  （客体提交 / 前端"等上一帧的栅栏" / 读回"等队列空闲"），并常收尾于 `VkResult -4`。
- **同一份配置连着两轮可以一次健康一次生病**（A2 r1 与 A2 r2 就是）⇒ 不是配置、也不是"哪一路诊断开着"，
  而是**每次启动**落在哪一种。
- 两种状态的**画面内容本身差一个数量级**（op/帧 1234 vs 100）⇒ "生病"的那次**卡在一个更早、更轻的场景**里
  （两边最后一条都是 `前端路径：发 FrontEndNavigation:ToCharacterCreate`，但生病那轮之后就不动了），
  而"健康"的那次走到了真正的角色创建场景。⇒ **下一个要问的是"启动时那一步为什么有时走不过去"**，
  而不是继续在"停住"这个末端现象上堆仪器。（另：D r1 那轮**日志泵停了 45 秒** = 真挂死，且诊断全关。）

**五、"设备丢失先丢的是谁" —— 本轮第一次拿到答案**

`vk_tex.c` 的两条"没走通 / 上传失败"原来**不带是哪一步**，这一轮给每一步补上名字与 VkResult（静态串 `g_step_fail`，
**只动日志**）。真机现场（`ab-A2 r2`，文件顺序）：

```
【红】一次性提交等待失败 VkResult -4                                    ← 第一条 -4
【红】贴图上传失败 serial=3879 格式 0x1（在「vkQueueWaitIdle 返回 -4」）
【红】贴图上传失败 serial=171  格式 0x44（在「vkQueueSubmit 返回 -4」）
【红】贴图上传失败 serial=3880 格式 0x44（在「vkQueueSubmit 返回 -4」）
…
【红】读回：命令缓冲那一套没走通（格式 97 尺寸 800x450，在「vkQueueSubmit 返回 -4」）
```

- **第一个看见设备丢失的调用是 `vkQueueWaitIdle`（"等队列空闲"）**；之后的上传全部在 `vkQueueSubmit` 上返回 -4
  —— 因为 **-4 是"粘"的**：一旦丢了，后续调用一律失败。
- 所以这份日志只能说"**谁第一个看见**"；要说"**谁弄丢的**"还差一件仪器：**把"最后一次成功的提交"与"第一条 -4"
  之间那一小段（提交序号 / 出图数 / 时刻）留在日志里**（下一步，见「七」）。
- 上一轮（`ab-A r2`）形状相同：**第一条红就在读回那条路上**（「读回：命令缓冲那一套没走通」），
  紧跟在一个 **1995 毫秒**的「读回：等队列空闲」之后 ⇒ 两次都落在**读回**上：
  **"等队列空闲"是设备丢失最灵敏的探针**（它要求整条队列走空）。

**六、这一轮改的东西（`dm/android/vk/vk_tex.c`，只动日志、不动行为）**

- 新增 `step_fail()` 与静态串 `g_step_fail`；在 `one_begin_on` / `one_end_on`
  （建池 / 分配命令缓冲 / reset / begin / end / `vkQueueSubmit` / `vkQueueWaitIdle`）与 `ent_upload`
  （建缓冲 / 分配 / 映射 / 解排布）的**每个静默失败点**写上"哪一步 + VkResult"。
- 「读回：命令缓冲那一套没走通」从**只打第一次**改成**前 5 次都打**（原来第一次失败就闭嘴，
  可能把后面"真的那次"盖掉）。
- ⚠ **口径变了**：这三条日志的正文都加了尾巴（`（在「…」）`）。按旧串统计会少算 —— 见「七」的坑
  （"改日志正文等于改统计口径"）。

---

### M3-2z：把"两种状态"追到系统级 —— **那 ~10 秒的停摆是我们引起的**（SystemUI 卡 10 秒 + composer 9.5 秒 + kgsl 栅栏 `waitForever`）；同时收回一条中途的错误读法（2026-10-15 真机）

**一、先否掉一条**（我中途猜的）："装包后第一次启动就健康" —— **不成立**。`probe-H1` 是装完包后的第一次启动，
照样病（出图卡在 52、心跳断 10.8 秒、两条 10.1/10.3 秒的等待）。

**二、两种状态走的是同一条路**（不是"另一条分支"）
- `logdiff.ps1`（按"行形状"归一化后逐行对比）：健康轮与病轮**没有任何只在一边出现的行形状**；
- `pipepairs.ps1`（对比画过的 顶点/片元 对）：病轮画过的**是健康轮的真子集**（3 种 vs 36 种）
  ⇒ 病轮只是**更早、更慢**地走在同一条路上；
- 时间也**不在"帧里"**：病轮 `[长帧]` 写的是"帧内 **33 毫秒**、GPU **0.4 毫秒**，而帧与帧隔 **1.64~1.87 秒**"。

**三、形状是"提交不退休"**：账本同一时刻的两条 ~11 秒等待分属**两个不同的队列** —— 客体线程的 `vkQueueWaitIdle`
（「一次性提交等空闲」，外层是 `nvnQueueSubmitCommands`）与**前端自己那条队列**的 `vkWaitForFences`
（「上屏：等上一帧的栅栏」）；转储的「等待点」是：**已提交到栅栏 77、在飞槽 1/8（槽 1 的栅栏值 77）、提交 77 次、
出图 73 次、队列锁无人持有** ⇒ 我们第 77 次提交交给驱动后**一直没退休**。

**四、同一时刻系统也在停（这一轮最重要的读数，来自 logcat）**
```
I/Fence    (13724/ndroid.systemui): waitForever: fence(kgsl-timeline-kgsl-3d0_7-ndroid) status(0)
W/BpBinder (1983): PerfMonitor binderTransact: time=9491ms   interface=…IComposerClient code=40
I/HWUI     (13724/ndroid.systemui): Davey! duration=10018ms  … GpuCompleted=…（比 SwapBuffers 晚 10 秒）
W/MiuiPerfServiceClient: interceptAndQueuing:13724|com.android.systemui|…|Slow swap buffers|2
```
⇒ **我们 + SystemUI + composer 三条路一起停 ~10 秒**。于是：
- ⚠ **收回一条中途的错误读法**：我中途在病态中跑过 `vk_probe`（离屏最小 Vulkan）只用 0.07~0.11 秒，据此曾写下
  "设备对别人是好的、病在我们进程" —— **那个读数不足以下这个结论**（离屏、64 KB，而且很可能正好撞在两次停顿
  之间）。**撤回**。
- **判别实验**：把 app 停掉、清 logcat、等 60 秒 ⇒ **60 秒里一条 `Davey!` / composer 卡顿都没有**（0 条）
  ⇒ **那 ~10 秒的系统级停摆是我们引起的**，不是这台机器自己周期性的毛病。
- 所以最贴形状的解释是：**我们让 GPU 挂住 → kgsl 约 10 秒后才判定/恢复**（`waitForever` 等的就是那条
  kgsl-timeline），期间所有走 GPU / 显示的东西一起等；有时以 `VkResult -4`（设备丢失）收尾。
  **`dmesg` 与 kernel logcat 都读不到**（`klogctl: Permission denied`）⇒ 要坐实"谁挂住 GPU"得越过 adb shell
  的权限（root / perfetto 的 ftrace 源），或者换设备 / 换驱动对照。

**五、不是这些**（都当场量过）：`Thermal Status: 0`、skin 36.2 ℃、GPU 52~58 ℃、电池 31.4 ℃、省电模式关、
面板 min/max/cur **120 Hz**、`DynFPS:false`、窗口是**当前焦点**（`mCurrentFocus=ai.d2rm.dm`）、
`Refresh Rate Map` 没有 per-app 覆盖。（顺带：`settings put system min_refresh_rate 120` 被 MIUI 拒 ——
shell 没有 `WRITE_SETTINGS`，所以"钉住刷新率"这个实验做不了。）

**六、下一步（按性价比，都还没动代码）**
1. **「M3-2y」的 arm D 里那 10 秒落在"上传"的一次性提交上**（viewer 与扫描都关着，仍是客体线程
   `nvnQueueSubmitCommands` 里的「一次性提交等空闲」10 秒；而同一条路上的「vk：上传：建中转缓冲 + 分配 + 映射」
   **没有**长等）⇒ **先把"读回"与"上传"分开量**：两路现在共用 `one_begin_on/one_end_on`，**站点名分不出来**。
   给 `one_end_on` 加一个"这是读回还是上传"的标签（**只动日志**），下一轮就能指名道姓。
2. **在停顿窗口里抓一次 ftrace / perfetto**（`gpu` / `kgsl` / `sync` 三个源，30 秒）—— 设备上能跑 `perfetto`
   就不用 root；这是唯一能直接看到"谁让 GPU 不动了"的仪器。
3. **换设备 / 换驱动对照**（最有分辨力）：同一条配方在另一台 Adreno 或另一个驱动版本上跑，看那 ~10 秒还在不在。
4. 若要动"用法"侧：第一刀建议切**前端那条自己的队列**（`vk_device.c:526` 无条件
   `vkGetDeviceQueue(g_dev, g_qfi, 1, &g_queue_win)`，没有开关）—— 退回共用队列看那 ~10 秒是否消失；
   代价是会走那条"实测两边互相堵死"的加锁回退路径（见 `vk_win.c:217-225`），要小心。

---

### M3-3a：把「读回 / 上传」分开 + 停顿窗口里的 ftrace —— 那 ~10 秒是**整条 GPU/显示通路一起睡、一起被 GMU 中断放出来**（2026-10-15 真机）

**一、先做的那一刀（只动日志）：「一次性提交」的两条路各自报名字**
- `vk_tex.c` 的 `one_end_on(cb, up)`：三处名字按路分开 —— 队列锁注释、提交站点、等空闲站点（**六个常量串**，因为账本只存指针，见 `crash.c:253`）；`one_end()` 传 0（读回 / 对照），`ent_upload` 传 1（上传）。**读回那侧旧串一字未改**
  （「一次性提交等空闲（读回 / 对照，等驱动，可能跑在客体线程上）」），上传那侧是新串「一次性提交等空闲（上传，等驱动，可能跑在客体线程上）」
  ⇒ 新旧日志可比，读它的脚本不用改（`ab-run.ps1` 的站点解析本来就是"抓到什么打什么"）。
- 读数：
  - **arm D（viewer 与扫描都关，一次读回都没有）**：D3 r2 `10005 ms`、D4 r1 `10262 ms`（外层「客体：排队提交命令」）、PT1 `11993 ms` —— **全部落在「上传」**；同刻前端线程在
    「上屏：等上一帧的栅栏」（10032 / 10282 / 12009 ms）。⇒ **arm D 里那 10 秒就是上传**，「M3-2y」/「M3-2z」的猜测**成立**。
  - **arm A（两路诊断都开）**：A4 r1 = 「上传」`10029 ms`；**A4 r2 =「读回：等队列空闲（等驱动做完在飞提交）」`10800 ms`**（读回锁同时持有了 10642 ms）。
  - ⇒ **两条路都会中**：谁在等队列，谁就吃到那 10 秒。**"读回是罪魁"这条方向正式作罢**，顺带把「M3-2z」第五节第 4 条（前端队列退回共用）的优先级降下去 —— 与"谁的队列、谁在等"无关。
  - 顺带复查：**同一份 arm D conf 连着三轮 = 病、病、健康**（r3：976 帧 / 14.0 帧/秒、零长等），而这几轮 **`VkResult -4` 全 0 条** ⇒ 再次确认"那 10 秒不需要设备丢失就能出现"。

**二、换仪器：`atrace` 在这台机上能吐**文本** ftrace（比 perfetto 好用）**
- 仪器事实：`/system/bin/atrace` 在 shell 下可跑；`atrace --async_start -b 32768 -c sched freq sync` → 命中后 `--async_stop -o /data/local/tmp/x.txt`，
  输出是**文本**，带 `prev_state`、`sched_blocked_reason: pid=… caller=…`、`cpu_frequency`、`clk_set_rate`、`dma_fence_signaled`，开头还有一条
  `tracing_mark_write: trace_event_clock_sync: realtime_ts=…` 可把 trace 时钟对到墙钟（本轮就是靠它把窗口对起来的）。`tracefs` 的 `enable` / `available_events`
  与 `/sys/class/kgsl/*` 对 shell 仍是 `Permission denied` —— **能走通的原因是 shell 在 `readtracefs` 组里（`id` 里的 3012），而 atrace 走的是它自己有权限的那条路**。
- 抓取配方（可复用，已落成 `.work\trace\stall-ftrace.ps1`，纯 ASCII）：环状缓冲 + **"最近 4 条心跳的出图数完全相同且 >0"当扳机**，命中后立刻 `--async_stop -o`。
  本轮一次命中（启动后约 28 秒），拿到 171 MB / 110 万条、覆盖 29.9 秒的原文。
- 窗口：两次 ~10 秒停摆 = `22:14:01.67 → 11.69` 与 `22:14:11.95 → 21.98`（与 app 日志的心跳断口一致）。

**三、决定性读数（同一份 171 MB 原文，四条）**
1. **我方两个线程各有一个 ~10 秒的"离 CPU"间隔，落在同一窗口、同一瞬间结束**：`tid 19581` = 10.028 s + 10.030 s（前端线程，就是「上屏：等上一帧的栅栏」，
   也是写心跳的那条）、`tid 19592` = 10.006 s（客体线程，「一次性提交等空闲（上传）」）。
2. **同刻整条 GPU/显示通路一起睡、一起醒**（结束时刻都落在 `22:14:11.69±0.02`）：`kgsl-events` 10.035 s、`crtc_commit:205` 9.998、`crtc_event:205` 9.995、
   `mFrameBuffer` 10.091、`DISP_EVENT_SUBJ` 10.004、`binder:1780_5` 10.081、`com.miui.daemon` 10.016、`blastAsyncWork` 10.014；第二轮同形（`kgsl_hwsched` 6.03、
   `gmu_f2h` 6.03、SystemUI 的 `SystemUIBg-3..7` 各 6.08、`crtc_*` 9.99）。⇒「M3-2z」从 logcat 看到的"SystemUI 也卡 10 秒"在**调度层面**得到同一条读数，
   而且**多了驱动自己的内核线程**（kgsl-events / kgsl_hwsched / gmu_f2h）。
3. **状态全是 S（可中断），全 trace 没有一条 >0.5 秒的 D 态阻塞**（唯一的 D 在起点那一瞬：`crtc_commit:205` 与我们的 19592 以 D 离开 CPU，之后都是 S）；
   CPU 在这 10 秒里**大部分 idle**（app 只占 1.075 CPU-秒 / 10 秒，其余多是 `swapper/*`），**CPU 频率全程在 556 MHz~1.99 GHz 之间来回、既没塌到最小也没钉在最大**
   ⇒ 不是热 / 省电 / 降频、也不是我方烧 CPU，更不是"内核不可中断地挂住"。
4. **恢复瞬间的形状 = GPU/GMU 通路重编程**：`22:14:11.674–11.679` 的原文是 `gmu_f2h`（GMU 固件→主机中断线程）被唤醒 → `kgsl_hwsched` 运行 →
   **`clk_set_rate: gpu_cc_pll0 1000000000` / `gpu_cc_pll0_out_even 500000000` / `gpu_cc_gmu_clk_src 500000000` / `gpu_cc_gx_gmu_clk 500000000` /
   `gpu_cc_cx_gmu_clk 500000000`**；第二次恢复（`22:14:21.966`）里 `kgsl_hwsched` 阻塞在 **`icc_set_bw`**（总线带宽投票）⇒ 形状是
   "**GPU/GMU 掉了约 10 秒，回来时做了整套时钟 / 带宽重编程**"，由 GMU 中断收场 —— 不是某个进程自己卡住。

**四、我方在起点做了什么（本轮唯一"像原因"的东西）**
- 起点 `22:14:01.641–01.642` 的原文：`dma_fence_signaled: sde_fence:crtc205:85908` → **`dma_fence_signaled: driver=kgsl-timeline timeline=kgsl-3d0_24-ai.d2rm.dm(15947)-a … seqno=560`**
  （**我们自己上下文的时间线 fence 被信号**）→ 我们的 19592 **以 D 离开 CPU**，`gmu_f2h` / `kgsl_hwsched` / `kgsl-events` 一起睡下去。
- 我方线程在驱动里等什么（`caller=` 全 trace 统计）：`folio_wait_bit_common` 1050、**`rpmh_write_batch [qcom_rpmh]` 579**、**`rpm_resume` 106**、
  `gpu_cc_cx_smmu_gdsc [gpucc_sun]` 13、`icc_set_bw` 6、`adreno_hwsched_wait_ack_completion [msm_kgsl]` 4、`gmu_core_timed_poll_check [msm_kgsl]` 2、
  `kgsl_iopgtbl_map [msm_kgsl]` 1 —— **且这些几乎全挤在启动那 4.8 秒**（`boot/normal` 窗口：rpmh 441、rpm_resume 93、gdsc 13、icc 6），停摆窗口里掉到个位数。
  系统侧同一套 caller 也在（`gmu_core_timed_poll_check` 195、`icc_set_bw` 190、`adreno_hwsched_wait_ack_completion` 123、
  **`gen8_hfi_send_cmd_wait_inline` 57（等 GMU 固件回 ack）**、`adreno_hwsched_poll_msg_queue_write_index` 32）。
- ⇒ **第一条有证据的因果链假设**：我们"**每次上传都新建 `VkBuffer` / `VkDeviceMemory` / 视图**"（「九」第 5 项）在驱动里落成**成百次功率域 / 总线投票 + 每次分配都做 IOMMU 映射**，
  某次 GPU 电源 / GMU 状态切换没能按时回来，整条通路等 ~10 秒后由 GMU 中断 + 时钟重编程收场。
  **这把「九」第 5 项（贴图上传改常驻上传环）从"性能优化"提升为"可能与停摆同源"** —— 下一轮该做的第一刀就是它（**先做成 A/B 第五臂，别直接改**）。
- 两个小复查：① 病轮里 app 起手就是**极高频 S↔R 切换**（`pre` 窗口 4.19 秒里我方 `tid 19602` 被调度 19284 次；整进程吃 5.571 CPU-秒 > 4.19 秒墙钟），
  是客体协作式调度"睡一下看一眼"的形状，不是新问题；② 本轮 trace 里**没有 `VkResult -4`**。

**五、perfetto：能用，但在这台机上拿不到 kgsl 的原子（如实记下来）**
- 能用的部分：`/system/bin/perfetto`（v49）在 shell 下可跑（`-c … --txt -o …`）；但**配置必须放 `/data/misc/perfetto-configs/`、产物必须写 `/data/misc/perfetto-traces/`** ——
  perfetto 自己的 SELinux 域**读不了 `/data/local/tmp` 的配置、也写不了那儿的输出**（`errno 13`；`adb push` 配置进 `perfetto-configs` 实测可以）。
- 三个新坑：① `compression_type: COMPRESSION_TYPE_NONE` **不是合法枚举**（只有 `UNSPECIFIED` / `DEFLATE`），写了会被判"配置非法"；要不压缩就**整行不写**。
  ② pbtxt **不能带 UTF-8 BOM**，带 BOM 直接 `1:1 error: Unexpected character ''`（这次用编辑器生成的一份配置就带了一个 BOM，剥掉即可）。
  ③ **perfetto 对"根本不存在的原子名"也不报错**（负对照：`kgsl/no_such_atom_xyz` 照样跑完并出 trace）⇒ **"配置没报错"不能当"原子可用"的证据**。
- 判"原子有没有真被打开"要看 logcat 里 `traced_probes` 的 avc：本轮出现
  `avc: denied { read } for name="format" dev="tracefs" … scontext=u:r:traced_probes:s0 tcontext=u:object_r:debugfs_tracing_debug:s0 permissive=0`（8 条，同一 inode）
  + `name="current_tracer"` 1 条 ⇒ **MIUI 的 SELinux 不让 `traced_probes` 读 vendor 事件的 `format`**，而 perfetto 要靠 `format` 才能解析事件 ⇒
  **kgsl 那一族原子在这台机上不可用**（具体被挡下几个分不出来，按"不可用"记）。
- 留档：`.work\trace\stall.pftrace`（7.1 MB，arm D 的一次 ~12 秒停摆窗口，含 `sched/*` + 那些 kgsl 原子名）—— 在**有 `trace_processor` 的机器**上可直接复核
  （本机无外网、`pip` 装不了 `perfetto` 包）。

**六、没做 / 做不了**
- **换设备 / 换驱动对照（任务 3）没做**：本机 `adb devices` 只有 `8b1f1e50` 一台。
- **任务 4（前端队列退回共用）没做**：本轮读数已把它的优先级降下去（那 10 秒与"谁的队列、谁在等"无关）。
- kgsl 计数器读不了（`/sys/class/kgsl/kgsl-3d0/` → `Permission denied`），与"GPU 频率节点读不了"同源。
- 顺带一条老现象：D3 r1 是**已知的偶发启动崩溃**（`CRASH sig=11 … pc=libc.so+0xd6b18`，与 `.work\logs\run-fixed-lock.log` 里那份签名一致），
  本轮 8 次启动里 1 次，**与本次改动无关**。

**七、下一步**
1. **第一刀：贴图上传改常驻上传环**（「九」第 5 项，加 `DM_UP_RING` 开关，做成 A/B 第五臂）—— 理由见第四条第 3 点。
2. 有 `trace_processor` 的机器上读 `.work\trace\stall.pftrace`；或换一台 Adreno / 换一版驱动跑同一配方（分辨力最强）。
3. 若还要正面看"谁让 GPU 掉下去"：这台机上**只有驱动内部（未导出）能看到** —— adb shell 的权限到此为止，别再在这上面花时间。

---

### M3-3b：常驻上传环（`DM_UP_RING`）—— "每趟新建对象"这一刀砍下去，**那 ~10 秒照旧**；顺带量到驱动级电源 / 总线投票被削掉约 3 倍（2026-10-15 真机 12 次启动）

**一、实现（缺省关，只为一个能做 A/B 的臂）**
- `vk_tex.c`：新增 `up_ring_on` / `up_ring_make` / `up_acq` / `up_rel`，`ent_upload` 里原来那段"建中转缓冲 + 分配 + 映射"整段换成 `up_acq`。
  一块常驻（`HOST_VISIBLE | HOST_COHERENT`）、**只映射一次**的缓冲，每趟上传借 **512 字节对齐**的一段偏移；容量缺省 16 MB（`DM_UP_RING_MB` 可改），
  单次需求更大就扩建，单次 > 64 MB（`UP_RING_MAX`）退回老路。
- **为什么不需要新锁 / 新栅栏**（三条，写在代码注释里）：① 上传只有一条调用路径（`tex_of` **持 `g_mu`**）；② 每趟仍然以"提交 + **等队列空闲**"收尾，
  所以返回时这一段拷贝已经做完、位置可以立刻复用；③ 环的建立 / 扩容都发生在"队列空闲之后"。
- **注意这一刀不去掉那次"等队列空闲"** —— 它去掉的是**对象搯腾**。所以这一臂量的是"那 10 秒跟对象搯腾有没有关系"，而不是"跟等队列有没有关系"。
- 顺手修两个日志坑（当场发现）：回绕打印条件写成 `g_ur_wrap == 1` ⇒ 第一次回绕之后**每趟都打**（一轮刷十几行同样的计数，读数没坏、日志被污染）；
  【黄】把"环关着"与"单次超上限"混进一句 ⇒ 出现过自相矛盾的一行（`单次 1920000 字节 > 上限 67108864`）。

**二、这一臂确实跑了环（先自证，再读数）**
- 每轮都能看到 `配置：DM_UP_RING=1`、`上传环：DM_UP_RING=1（借常驻环）`、`【绿】上传环：常驻中转缓冲就绪 16777216 字节`；
  **一轮只借 19 段、回绕 1 次、建/换 2 次**（第 2 次是因为有一张上传要的比 16 MB 还多，环被扩到 17.64 MB）。
  ⇒ **一轮整轮只有约 19 次上传** —— 这同时纠正了「M3-3a」第四条第 3 点的**量级**：那 441 次 `rpmh_write_batch` 不可能主要来自上传，
  环的用武之地比那条假设小得多（顺带：`[长帧]` 行里那组"上传贴图 首传 N 张 + 重传 M 次"在本轮所有日志里**恒为 0**，那条统计取不到上传——别再拿它读上传量）。

**三、读数：停摆照旧（本轮的主要结论）**
- **E 臂 6 次启动**（`ab-E.conf` = arm D + `DM_UP_RING=1`）：3 次病（**10.2 / 11.9 / 10.7 秒**的长等，落点还是「一次性提交等空闲（上传）」+「上屏：等上一帧的栅栏」）、
  2 次健康（964 / 967 帧、13.9 帧/秒、零长等）、1 次撞上那条已知的启动崩溃。
- 同构建的 D 臂对照（`DM_UP_RING=0`）2 次启动：1 次**设备丢失**（见下面第五条）、1 次启动崩溃（两条崩溃日志都确认是那条已知签名）。
- ⇒ **"每次上传都新建 `VkBuffer` / `vkAllocateMemory` / `vkMapMemory`"不是那 ~10 秒的原因**（环把每轮的对象创建从 ~19 次砍到 2 次，那 10 秒一样出现）。
- 帧率上**也看不到收益**：健康轮 964 / 967 帧 vs 老路健康轮 964~976 帧 ⇒ **开关保持缺省关**（不为"看不见的收益"改缺省行为），
  但留着（与 `DM_ZC_CACHED` 同一条纪律：换设备 / 换驱动时一条命令可复验）。

**四、顺带的正面读数：驱动级电源 / 总线投票被削掉约 3 倍（同一把尺子）**
- 在**环开着**的配置下又抓了一次停顿窗口 ftrace（用「M3-3a」那个脚本 `.work\trace\stall-ftrace.ps1`，`-Conf ab-E.conf`），
  与「M3-3a」那份老路原文用**同一把尺子**（"应用出现后的头 10.4 秒"）比：

| 我方线程在驱动里的阻塞点（`caller=`） | 老路（环关） | 环开 |
|---|---|---|
| `rpmh_write_batch [qcom_rpmh]` | 442 次（42.5/s） | **159 次（15.3/s）** |
| `rpm_resume` | 93（8.9/s） | **27（2.6/s）** |
| `gpu_cc_cx_smmu_gdsc [gpucc_sun]` | 13 | **0**（掉出前 12） |
| 合计（含 `folio_wait_bit_common` 等） | 2896（278/s） | 1761（169/s） |

⇒ 环**确实**把"上传带来的驱动级电源 / 总线投票"削掉大半（约 3 倍），但**没削干净**（还剩 ~15/s，来自别处：建图 / 池 / 其它分配），
而那 ~10 秒照旧 ⇒ 两件事可以分开读：**"投票变少"是真的，"停摆消失"不是**。两条 trace 是不同轮次（相位不完全可比 —— 环那一轮的 app 每秒更忙：
on-CPU 6.7 秒 / 10.4 秒），所以当**量级**读，不当精确数。

**五、顺带一条新边界：设备丢失可以不带那 ~10 秒**
- D6 r1（环关）：`【红】一次性提交等待失败 VkResult -4` → `vkQueueWaitIdle 返回 -4` / `vkQueueSubmit 返回 -4` ⇒ **131 条**「贴图上传失败」，
  而**这一轮最长等待只有 2.1 秒**、心跳最大间隔 2.0 秒 ⇒ 「M3-2x」证过"那 10 秒**不需要**设备丢失"，这一轮补上反过来那半：
  **设备丢失也**可以不带那 10 秒 —— 两者是**两个可分的现象**（很可能是同一条 GPU/GMU 故障的不同收尾）。

**六、样本纪律（新）**
- 本轮 12 次启动里 **3 次**撞上那条已知的启动崩溃（`CRASH sig=11 … pc=libc.so+0xd6b18`，**E 与 D 两臂都出现** ⇒ 与本次改动无关）。
  **排轮次时要把"崩溃轮"剔掉**，否则健康 / 病态的比例会被它带偏；它与"病态"是两回事（崩溃轮里根本没有出图）。

**七、下一步（那 ~10 秒的触发点仍然未知）**
1. 这台机上能用的手段到头了（驱动内部不可见、无 root、只有一台设备）。剩下三条：① 有 `trace_processor` 的机器读 `.work\trace\stall.pftrace`；
   ② **换设备 / 换驱动**（分辨力最强）；③ 等下一轮想到新的**用法侧**候选。
2. 用法侧还剩两个候选，都便宜、都按"先量后改"：
   - **提交模式**：我们"每趟上传 / 读回都单独 submit 一次、然后要求整个队列空闲"，与客体每帧的提交交替（「M3-2z」读数：提交 77 次 / 出图 73 次）。
     可试"把上传的等队列空闲换成只等这一段拷贝的栅栏"（常驻环已经让它可行）—— **期望值不高**（一轮只有 ~19 次上传），但便宜。
   - **破坏性取证**：把上传整个砍掉（`DM_UP_NOSUB`：只解排布、不拷进图）看那 10 秒还在不在 —— 画面肯定是错的，只为分辨"是不是我们的提交 / 拷贝把它带下去的"。

---

---

## 九、优化清单（**待确认**，2026-10-14 记）

这不是"下一步做什么"，而是把「里程碑 A 的两条判据清空之后」（见「M3-2l」）**本可以更快、更稳的地方**集中记下来，
供逐项确认；**确认之前一行代码都不动**。每条都带证据（`文件:行`）与"怎么量"。

排序依据是"性价比 = 收益 ÷ 风险"，不是工作量。

| # | 事项 | 一句话 | 风险 | 时机 |
|---|---|---|---|---|
| 1 | 建 `VkPipelineCache` 并落盘复用（**已做**，见「M3-2p」） | 一条管线缓存都没用，每次启动都从零编译 | 低 | 已落地 2026-10-15 |
| 2 | 零拷贝内存类型优先要 `HOST_CACHED`（**已取证、已否掉**） | 类型 6 能导出 fd，但 CPU 时间没有可测差异，且要越过缓冲声明的可用类型位 | — | 结案（「M3-2q」） |
| 3 | `renderArea`（**已取证、已否掉**） | 真机 16384 次开通道：主场景三者一致，卡点不在这里 | — | 结案（「M3-2m」） |
| 4 | 每次绘制的重复工作 | 管线查找 + 2 个描述符集 + 全量重写 + 重复绑定 | 中 | 卡点量清之后 |
| 5 | 贴图上传改常驻上传环（**已落地、已否掉**） | 常驻环做出来了：与那 ~10 秒**无关**（投票少 3 倍、帧率无差异），缺省关 | 低 | 结案（「M3-3b」） |
| 6 | 手写管线缓存表偏小、淘汰即销毁 | `GPSO_TAB 192` / `CPSO_TAB 256` | 低 | 第 1 项之后 |
| 7 | op 流里 70.5% 是纯状态绑定 | 状态在"记录 + 回放"两趟里各搬一次 | 高（改动面最大） | M3/M4 |

### 1. 建 `VkPipelineCache` 并落盘复用（收益最大、风险最低）——**已落地（2026-10-15，见「M3-2p」）**

**落地在哪**：`vk_device.c` 的 `pcache_boot` / `vk_device_pcache_save` / `vk_device_pcache_tick`（图形与计算共用一个），
两处 `vkCreate*Pipelines` 从 `VK_NULL_HANDLE` 换成 `vk_dev_pcache()`（`vk_pipe.c:287-289`、`668-670`），
路径由 `DM_PIPE_CACHE` 给（APK 侧缺省是 app 内部目录）。**原判据里的那条坑照样成立**：blob 与驱动 / 设备绑定，
换驱动直接丢弃（头里存 vendor/device/driver/api + `pipelineCacheUUID`，真机上两条负向判据都照过）。
另有一条真机事实改了原来的设计：**"切后台"这条路在 MIUI 上走不通**（退到后台直接 SIGKILL，见「M3-2p」第四节），
所以落盘的主力是**前台定期写回**，不是退出 / 切后台那两个钩子。

**原来的证据（改动前）**：`vk_pipe.c:284` 与 `vk_pipe.c:660` 两处建管线都传 `VK_NULL_HANDLE`；全仓没有 `vkCreatePipelineCache`。

**代价**：进程内没有驱动级去重；跨进程每次都从零编译。安卓上是**运行时**编译 SPIR-V（没有 iOS 那条"离线编好
metallib"的路），所以这一项恰好就是 iOS"着色器全部离线翻译、预先编好，进新场景不用现编"那个优势的**安卓对应物**。

**怎么量（**待补**）**：启动到 `nnMain` 的时间、首次进场景的卡顿；同一台机跑两遍（`DM_PIPE_CACHE=0` 对 带缓存）。
当前只确证了"缓存被复用"（「M3-2p」第三节 B 轮），时间那条 A/B 要等常态停住那件事有进展再补。

### 2. 零拷贝内存类型优先要 `HOST_CACHED`（**已取证、已否掉**，2026-10-15，见「M3-2q」）

**原判据**：`vk_device.c:326-327` 要的是 `DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT`，而 `vk_memory_type`
（`vk_device.c:154-162`）**取第一个满足者** → 命中类型 4。项目自己的探针（M2 第 1 步）记过：
类型 6 = `DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT|HOST_CACHED`。若用得上，3.7 GB 客体堆 + 512 MB 别名区就不再走
非缓存路径（游戏逻辑、顶点 / 常量组装、贴图上传全落在上面），也能跟 iOS 侧可缓存的 `MTLResourceStorageModeShared` 对齐。

**取证（真机）**：类型 6 上**能**导出 fd、能 `mmap` 到指定地址、双向通（256 MB / 512 MB / 3968 MB 都过了），
但**它不在缓冲声明的可用类型位里**（外部缓冲是 `0x13`：类型 0 / 1 / 4；同尺寸同用途的**普通**缓冲是 `0x41`，含类型 6）
—— 用它属**规范外用法**，靠驱动宽容接受。

**A/B（真机）**：同机、同场景、每轮固定 10 秒墙钟采 `utime+stime`，类型 6 与类型 4 **没有可测差异**
（同峰配对 <3%，而轮间波动 ±100%；CPU 时间只跟绘制量走）。**收益看不到、风险（未定义用法）却要付 ⇒ 否掉，维持类型 4。**

**留下的东西**：换轨处**显式**优先取"带 `HOST_CACHED` 的零拷贝类型"、取不到或分配失败**回退类型 4**（`vk_region.c`），
由 `DM_ZC_CACHED=1` 控制（**默认关**，缺省行为与改动前完全一致）；探针侧新增 `DM_FD_MEM_TYPE` / `DM_FD_FORCE` /
`DM_FD_SIZE` 与"普通缓冲 vs 外部缓冲"的类型位对照。

**怎么复验**（换设备 / 换驱动时）：
`DM_FD_SIZE=3968 DM_FD_MEM_TYPE=6 DM_FD_FORCE=1 vk_fdmap_probe`（能不能导出）+ `DM_ZC_CACHED=1` 跑一轮比 CPU 时间（有没有收益）。

### 3. `renderArea` 不再由视口推导（卡点的优先排查项）

**证据**：`nvn_vk.c:1806-1810` —— 渲染区取"视口的 `x+width` / `y+height`"与裁剪取小，而不是附件范围。

**风险点**：视口的 `x`/`y` 一旦是负值或非零偏移（NVN 里不罕见），`(uint32_t)` 强转与取小都会把渲染区**算小**，
而**渲染区外的片元会被直接丢弃**；真机没有校验层，不会报错。这正是"画了却一个像素不落"的形状之一。
苹果侧与同类项目（Skyline / Ryujinx）都是**取附件范围**、让视口与裁剪去钳。

**已取证、已结案：不是卡点**（「M3-2m」）。16384 次开通道里主场景三者一致（1600×900），"算出比附件小"只占
**1.6%**（且那 1.6% 是视口本身就只覆盖一块子区域的通道，与 Metal 语义等价）；退成 1×1 的 **0** 次；开通道时没视口的 **0** 次；
视口带偏移的 **0** 次。取证开关保留（`DM_PASS_AREA=1` 只读、`=2` 换成附件尺寸做对照），供回归用。
**取证里顺带照出两条新读数**（见「M3-2m」二）：① "附件 0x0"= **只有深度附件**的通道，现有钳位对它们不生效；
② 出现"**视口 1600×900 / 附件 800×450**"这类配对——渲染区被正确钳住，但**下发给 Vulkan 的视口没跟着缩放**，
这是两侧都还没处理的一处不一致（苹果侧同样），单独记一笔。

### 4. 每次绘制的重复工作

**证据**：`nvn_vk.c:2259`（每次绘制都走 `vk_pipe_graphics`）、`2287-2290`（每次绘制分配 2 个描述符集并整份重写）、
`2292-2307`（管线 / 描述符 / 顶点流每次重绑）；全量重写落在 `nvn_vk.c:1728` 的 `vkUpdateDescriptorSets`。

**对照**：Skyline 有独立的 `descriptor_allocator`，Ryujinx 按内容缓存描述符集，两边都用**脏标志**跳过未变状态；
本项目是"每次绘制全量重算"（`nvn_vk.c` 文件头写明 M2 只求语义等价）。

**建议**：① 按在途槽记住"上一次的管线 + 描述符集 + 顶点缓冲"，内容相同就跳过绑定；② 按解析出的绑定内容
缓存描述符集（三条里收益最大：70.5% 的 op 都是绑定）；③ 再看设备有没有 `VK_EXT_descriptor_indexing` /
`VK_KHR_push_descriptor`。

**时机**：**必须等"有没有写出颜色内容"量出来之后**（遵守项目"一次只改一处"的规矩）。

### 5. 贴图上传改成常驻上传环 —— **已落地并已量（2026-10-15，「M3-3b」）：与那 ~10 秒无关，缺省关**

**证据**：`vk_tex.c:385-397`（`one_end` 是"提交 + `vkQueueWaitIdle`"，且整段在队列锁里）。
**新增证据**（「M3-2l」第六节）：有一轮出现 `【红】上传命令等待失败 VkResult -4`（`VK_ERROR_DEVICE_LOST`）
并级联上百条贴图上传失败；跨轮对照为**偶发**、与最近两次改动无关。

**落地与读数**（「M3-3b」）：`DM_UP_RING=1`（缺省关）= 常驻、只映射一次的中转缓冲 + 512 字节对齐的环内偏移；
一轮只替掉约 19 次对象创建，而**那 ~10 秒照旧**（E 臂 6 次启动：3 病 / 2 健康 / 1 崩溃）⇒ **这一项与停摆无关**。
唯一量得到的收益是"**驱动级电源 / 总线投票少约 3 倍**"（`rpmh_write_batch` 42.5/s → 15.3/s、`rpm_resume` 8.9/s → 2.6/s），
**帧率上看不出来**（健康轮 964 / 967 帧 vs 老路 964~976 帧）。**开关留着、缺省关**（与第 2 项同一条纪律），换设备 / 换驱动时一条命令可复验。

### 6. 手写管线缓存表偏小、淘汰即销毁

**证据**：`vk_pipe.c:449-451`（`GPSO_TAB 192`）、`271-274`（`CPSO_TAB 256`）、`480-485`（整结构体哈希 + 线性探测 +
`memcmp`）、`667-677`（满了淘汰并 `vkDestroyPipeline`）。

**建议**：第 1 项做了之后"重建"变便宜，这一项优先级随之下降；仍值得把每次绘制的"哈希 + 探测 + memcmp 整个状态"
改成"先比上一次"，并把淘汰改成按使用计数的 LRU。

### 7. op 流里 70.5% 是纯状态绑定

**证据**：项目自己的读数（每窗口约 800 万条：`BIND_STATE` 27.8% / `BIND_UBO` 21.9% / `STENCIL` 10.3% /
`BIND_TEX` 5.3% / `BIND_VBUF` 5.2%）；`nvn_vk.c` 文件头："M2 只做语义等价，不搬 Metal 那边的优化
（分配器环、延迟合并通道、参数缓冲、常驻集）"。

**两条路**：① 记录时合并连续 `BIND_*`、消除被覆盖项；② 纯状态在**记录时**直接并入影子、不进流（只有
`SET_RT` / `CLEAR` / `DRAW` 这类才进流）。②更彻底，但与苹果侧同形的那套结构差得最远。

**时机**：M3/M4；改动面最大，收益要用帧预算来证明。

### 建议的动作顺序

1. 「七、下一步」第 1 条（扫一遍）+ 本页第 3 项**取证** —— 并行，零风险，直接回答"卡点在哪"；
2. ~~第 1 项（`VkPipelineCache` + 落盘）~~ —— **已落地**（「M3-2p」）；只剩"启动到 `nnMain` 的时间"那条 A/B 待补；
3. 第 2 项（`HOST_CACHED`）—— 先验"类型 6 能不能导出 fd"，再单独立一次 A/B；
4. 第 4 / 5 / 6 / 7 项按量出的帧预算再定。
