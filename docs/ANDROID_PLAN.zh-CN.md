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
