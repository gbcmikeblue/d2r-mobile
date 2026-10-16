# 换一台电脑继续（跨机器开发调试）

English: [CROSS_MACHINE.md](CROSS_MACHINE.md)

这一页只讲一件事：Android 侧的开发调试换一台电脑时，除了 `git clone` 到的代码，还要带哪些文件、从哪拿、恢复到什么样才算能用。代码本身很小（`.work/`、`.tools/`、`.venv/` 都在 `.gitignore` 里，全在仓库之外），真正要搬的是数据。

**只是要接着往下跑（不搬家）的话，直接看第七节**：那里写了"现在停在哪、下一步查什么、怎么跑一轮并拿到读数"。

## 一、手机就是数据载体

跨机器要带的东西一律放在手机的**中转区** `/sdcard/d2rm/pc/`：写入用 `adb push`，换机器时用 `adb pull` 取回。这样不必再找移动硬盘，也不必重新解包游戏。

| 手机上的路径 | 体积 | 恢复到新电脑的位置 | 不带会怎样 |
|---|---|---|---|
| `/sdcard/d2rm/pc/data/` | 8.4 GiB / 51 个文件 | `<仓库>\.work\data\` | 游戏文件不可再生：要用你自己的 `.xci/.nsp` + `prod.keys` + hactool 重新解包 |
| `/sdcard/d2rm/pc/spv.tar` | 616 MB / 13847 项 | 解包到 `<仓库>\.work\spv\` | 6923 个着色器的 SPIR-V 要重算（要 .NET 加全库翻译） |
| `/sdcard/d2rm/pc/out/` | 300 MB | `<仓库>\.work\build\cache\dm\out\` | 改写器产物，重建便宜（`dm/tools/product.sh build`，但得先有 Python 环境） |
| `/sdcard/d2rm/pc/apk-keys/` | 2.6 KB | `<仓库>\.work\apk-keys\` | 重建密钥会换签名，手机上必须先卸载旧包才能装 |
| `/sdcard/d2rm/pc/android-save/` | 47 KB | `<仓库>\.work\android-save\` | 丢掉存档与设置 |
| `/sdcard/d2rm/pc/hactool/` | 16 MiB | `<仓库>\.tools\hactool\` | 只有重新解包游戏时才用到（也可以不拷，用时再编） |

合计约 9.3 GB。

**不必带的**（换机器重跑即可，都是几十秒到几分钟的东西）：`.work/android-build`、`.work/apk-build`、`.work/dmsh`、`.work/xlate`、`.work/probe`、`.work/recon`、`.venv`。注意**别把生成物里的 `CMakeCache.txt` 带过去** —— 里面记的是上一台机器的绝对路径。

关于那 6923 个同名 `.bin`：`.work\spv` 目录里除了 `.spv` 与 `.res.json`，还会有 6923 个与 `.spv` 同名的 `.bin`，那是生成过程带出来的中间物。运行时只按 `<key>.spv` 读（`dm/android/vk/vk_shader.c:484-488`），也只统计 `.spv` 与 `.res.json` 两类（`:67-71`），所以 `spv.tar` 里没有它们，解包后少了它们不影响运行。

## 二、新电脑上要装什么

Android 侧最小工具集（版本是当前实测能用的那一套）：

| 工具 | 版本 |
|---|---|
| NDK | 30.0.16248370 |
| build-tools | 36.0.0 |
| platforms | android-36 |
| CMake | 3.31.6（含 ninja） |
| platform-tools | adb |
| JDK | 21（apksigner 要 `JAVA_HOME`） |
| Python | 3.10 + `pillow numpy lz4 capstone unicorn` |

只有要重新生成 `.work/spv` 时才需要 dotnet 10。Xcode、spirv-cross、xcodegen 属于 iOS 那条链，Android 侧不需要（安卓在 SPIR-V 处就下车了）。

Python 环境：`python -m venv .venv`，再 `.venv\Scripts\python.exe -m pip install pillow numpy lz4 capstone unicorn`。
**所有命令一律用这个 `.venv` 的解释器**（`.venv\Scripts\python.exe -X utf8 …`）：本机 **PATH 上没有 `python` / `py`**
（裸写会 `CommandNotFoundException`），而 `.venv` 在 `.gitignore` 里、**新机器必须重建**。规则文件与替换配方见
`.codebuddy/rules/windows-python-only/RULE.mdc`（alwaysApply，2026-10-16 起）。

### 工具链路径：**不要**改文件

**正常情况下不用改**：`dm\android\apk\build_apk.cmd` 与 `dm\android\apk\install_apk.cmd` 都不再写死任何工具路径 —— 全部交给
`dm\android\apk\toolchain.cmd` **一处**决定，它按下面的顺序发现（谁先说话算数）：

1. `DM_SDK_ROOT` / `DM_NDK` / `DM_BUILD_TOOLS` / `DM_CMAKE` / `DM_PLATFORM` / `DM_JDK` 显式覆盖
2. 机器自己声明的 `JAVA_HOME` / `ANDROID_SDK_ROOT` / `ANDROID_HOME`
3. 已知落点里**取最新的一版**（JDK 优先 `jdk-21*`，再退到任意 `jdk-*`）
4. 兜底：Android Studio 自带的 `jbr`，最后是 `PATH` 上的 `java`

**装到别处**就用上面那些 `DM_*` 或 `ANDROID_HOME` 指过去（任选一个），**不必改仓库里的任何文件**。选中哪一条会在构建
开头打出来（"结论不该靠记忆"）。

`.work\apk-build`、`.work\apk-keys` 是相对脚本自身推导的，本来就不用动。

**为什么 JDK 单独值得这条规则**：Adoptium 把目录名从 `jdk-21.0.12.1+1` 改成 `jdk-21.0.12.101-hotspot` ——**主版本没变、
目录名会变**——于是"按文档装同一版 JDK"也会得到不一样的路径。SDK 侧那些版本号是 SDK Manager 起的，按文档装就一致。

**历史**（这次重构之前的状态，仅作记录）：`build_apk.cmd:22-27` 和 `install_apk.cmd:13` 写死了 NDK / JDK / build-tools /
CMake / ninja / android.jar / adb 七条绝对路径，前两台机器实测都只动了 JDK 一行（Adoptium 目录改名那一次）；现在这七条
全部走 `toolchain.cmd`，仓库里再没有任何"上一台机器的路径"。

### 三个由游戏文件生成的头文件（不在上面那张表里）

`dm\host` 下有三个**由游戏文件生成、被 `dm\host\.gitignore` 忽略**的头文件：

| 头文件 | 生成脚本 |
|---|---|
| `dm\host\dm_vault_tab.h` | `dm\ios\tools\gen_vault_table.py` |
| `dm\host\dm_hud_exp.h` | `dm\ios\tools\gen_hud_exp.py` |
| `dm\host\dm_bright_levels.inc` | `dm\tools\gen_bright_levels.py` |

它们**既不在 git 里，也不在上面那张「要带什么」的表里**，但缺了它们 `build_apk.cmd` 走不过第 1 步：`dm\host\diag.c:6` 直接报
`'dm_hud_exp.h' file not found`（是编译期，不是链接期）。这不是缺陷而是设计——它们由玩家自己的游戏文件派生，所以
**数据取回之后按 `dm\host\Makefile:19-29` 的规则重生成一次**即可（约 1 分钟）。

两条 Windows 约定（第一条见 [ANDROID_PLAN.zh-CN.md](ANDROID_PLAN.zh-CN.md) 第六节）：

- 必须带 `PYTHONUTF8=1` 与 `PYTHONIOENCODING=utf-8`，否则按 GBK 输出会**截断表头**；
- **重定向必须用 cmd 的 `>`**：这些是 C 头文件，PowerShell 的 `>` 写出来是 **UTF-16LE**（`FF FE` 开头、体积约两倍），下一步编译直接过不去。

照抄即可（cmd；`REPO` 换成自己的仓库路径）：

```bat
set "REPO=<仓库路径>"
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"
set "ROMFS=%REPO%\.work\data\switch\3.3.94012\Update\RomFS"
cd /d "%REPO%\dm\host"
"%REPO%\.venv\Scripts\python.exe" ..\ios\tools\gen_vault_table.py "%ROMFS%" > dm_vault_tab.h
"%REPO%\.venv\Scripts\python.exe" ..\ios\tools\gen_hud_exp.py    "%ROMFS%" > dm_hud_exp.h
"%REPO%\.venv\Scripts\python.exe" ..\tools\gen_bright_levels.py  "%ROMFS%" > dm_bright_levels.inc
```

## 三、恢复步骤

PowerShell；`<仓库>` 换成新电脑上的仓库路径，adb 换成新机器上的路径。

```powershell
$adb  = 'C:\Android\Sdk\platform-tools\adb.exe'
$repo = 'D:\yiyi\d2r-mobile'

# 1) 手机在开发者选项里打开 USB 调试并插上
& $adb devices

# 2) 取回数据（约 9.3 GB，实测 32.8 MB/s，约 5 分钟）
& $adb pull /sdcard/d2rm/pc/data/.         "$repo\.work\data\"
& $adb pull /sdcard/d2rm/pc/out/.          "$repo\.work\build\cache\dm\out\"
& $adb pull /sdcard/d2rm/pc/apk-keys/.     "$repo\.work\apk-keys\"
& $adb pull /sdcard/d2rm/pc/android-save/. "$repo\.work\android-save\"
& $adb pull /sdcard/d2rm/pc/hactool        "$repo\.tools\"
& $adb pull /sdcard/d2rm/pc/spv.tar        "$repo\.work\"

# 3) spv.tar 是平铺的（里面直接就是 .spv 与 .res.json，没有顶层目录），必须解到 .work\spv
New-Item -ItemType Directory -Force "$repo\.work\spv" | Out-Null
tar -xf "$repo\.work\spv.tar" -C "$repo\.work\spv"
(Get-ChildItem "$repo\.work\spv" -File | Measure-Object).Count   # 应得到 13847（不含 6923 个 .bin，见上）
```

`tar` 用 Windows 自带的 bsdtar 即可；Mac / Linux 上把路径分隔符换成 `/`。

**别忘了第 4 步**：三个派生头文件要由取回的游戏文件重生成一次，见第二节「三个由游戏文件生成的头文件」——
注意那一步得在 **cmd** 里跑（PowerShell 的 `>` 会把头文件写成 UTF-16）。做完这四步，再按第四节验。

### 手机不稳时：别整目录 pull，改成逐文件 + 重试

整目录 `adb pull` 一断就得从头来。第二台机器上实测的现场是 Hyper-V 虚拟机（`ROOT\VMBUS\0000`）走 USB 直通：
整目录 pull 传到第 2 个文件（1 GiB 的 `00-00000001.data`）就断，随后 `adb devices` 直接变空。**熄屏不是原因**——
那台机器 `stay_on_while_plugged_in` 已经是 15、`screen_off_timeout` 5 分钟。改成下面这种做法后 9.3 GB 一遍过：
1 GiB 那个文件第一次失败、等设备回来重试一次就成功，其余按约 40 MB/s 走完。

形状就三件事：

- **逐文件取**（`adb shell 'cd <源> && find . -type f'` 拿清单，一个个 `adb pull`），**本地大小与远端一致就跳过**，
  于是脚本可以反复重跑，断了只补没拿到的；
- 失败先看设备还在不在，不在就 `adb wait-for-device` 等它回来再重试（本机实测：断开约 1 分钟后自行恢复）；
- 在 cmd 里等重试用 `ping -n 4 127.0.0.1 >nul`，**不要用 `timeout /t`**——它要控制台输入，重定向环境下会直接
  `ERROR: Input redirection is not supported` 退出，把整个脚本带走。

## 四、怎么算恢复好了

1. `dm\android\apk\build_apk.cmd` 能出包（产物 `.work\apk-build\dm.apk`）—— 这一步同时验证了 NDK 那段路径改对了。前提是第二节那三个派生头文件已经生成，否则会停在 `dm\host\diag.c` 的 `'dm_hud_exp.h' file not found`。
2. `dm\android\apk\install_apk.cmd` 能装上并启动。
3. 起不来的话按顺序查这三处：`.work\data\switch\3.3.94012\Update\ExeFS\` 里有 `main`/`sdk`/`subsdk0`（校验值见 `setup.sh:84-86`）；`.work\data\switch\3.3.94012\Update\RomFS\data\` 里的资源档案；`.work\build\cache\dm\out\3.3.94012-r41-android\`。

## 五、设备侧（只有换手机才要重做）

这台手机上已经放好的：

| 手机上 | 体积 | 是什么 |
|---|---|---|
| `/data/local/tmp/dm/` | 9.1 GB | `romfs` 8.3 G、`spv` 709 M、`patched` 75 M、`dmhost`、几个 `vk_*_probe` |
| `/sdcard/Android/data/ai.d2rm.dm/files/` | — | app 的可写区：`save`、`save.work`、`temp`、`rt`、`logs`、`dm.conf` |

三条老规矩：

- `/data/local/tmp` 对 app 域**只读**（写会 errno 13），存档与临时文件走 `/sdcard/Android/data/ai.d2rm.dm/files/`；
- `MANAGE_EXTERNAL_STORAGE` 要在系统设置里手动打开；
- 游戏资源放 `/data/local/tmp/dm/romfs`（读得快）或 `/sdcard/d2rm`（重启不丢）。

**第二台（2026-10-16 备齐，`VC9TGM45MNJB8TSG` / Redmi `pearl` / MT6896 / **Mali-G610 MC6** / Android 15）**：
同一份 9.1 GB 已按本节推上去（用时 5 分 12 秒、28.5 MB/s），`save` 也推了 10 个文件。
做法上有两条与第一台不同、值得沿用（脚本 `.work/push_dev2.py`）：
① **逐文件推 + 远端大小相等就跳过** ⇒ 断了可以整脚本重跑，只补没拿到的；
② **`spv` 走 `spv.tar` + 设备端 `tar -xf`**（13847 项 **8 秒**解完；20770 个小文件逐个 `adb push` 会慢得多）。
唯一的缺口曾经是装机：**这台 MIUI 拒绝 `adb install` / `pm install`**（`INSTALL_FAILED_USER_RESTRICTED: Install canceled by user`，
`settings put … verifier_verify_adb_installs` 又被 `WRITE_SECURE_SETTINGS` 拒）⇒ 须在手机上手工打开"通过 USB 安装"。
**更正（2026-10-16 当天）**：用户**已经打开了那个开关**（第一台本来就开着）⇒ 装机这一条**应当已解除**，
但写档时两台都没插在电脑上，所以**还没被验证**；下一轮第一件事：
`.work/mkapk.py --skip-build --install VC9TGM45MNJB8TSG`（出包走 `build_u8.cmd` 包装，装机先试设备端 `pm install -r`），
成功后再跑 1~2 轮（预期判定 `启动受阻`）确认"APK 这条路上也被内存换轨挡住"。
**注意**：装机≠卡点 B 能在第二台上量 —— 那要等零拷贝内存路线（AHB 支路）落地。

**再更正（2026-10-16 晚些，已实测验证）**：**装机这条已经验证通过** —— `pm install -r /data/local/tmp/dm.apk`
→ `Success`，装后 sha256 与本地一致（读数 `.work/logs/dev2-wait-install.txt`）；其后两轮 APK 的判定是
**启动受阻**（`建载体缓冲失败（256 MB）` → 堆/别名区各 0 MB → `SVC 0x2c 越出 alias 区` → `Break#1`）。
另外两条在这台上踩到的：

- **app 的日志目录别用 shell 建**：`/sdcard/Android/data/ai.d2rm.dm/files/logs` 若由 `adb shell mkdir` 建出来
  （属主 `shell`、组 `ext_data_rw`、0770），app **打不开** `latest.log`（errno 13：
  `开不了日志文件 …/logs/latest.log`）⇒ `ab_run.py` 拉到的路径是空的，判类**静默变成"崩溃轮"**。
  让 **app 自己建**（把旧目录 `mv` 掉即可，可逆；`.work/push_dev2.py` 里那两条 `mkdir … logs` 已去掉）。
- **⚠ 这台设备会被我们的 AHB 支路搞成内核 panic 重启**：`persist.sys.boot.reason.history` 连记三条
  `kernel_panic`（2026-10-16 的 10:45:49 / 10:46:38 / 10:47:49），时间对上"**把 ~4 GB 的 `AHardwareBuffer`
  铺成客体堆之后**"（同一台上只做 3968 MB 的 AHB 分配那几轮一直没事）。该机**内核日志拿不到**
  （无 root、`dmesg` 被 `klogctl` 拒、`/sys/fs/pstore` 与 `/data/system/dropbox` 读不了）。
  AHB 支路**已默认关**（`DM_REGION_AHB=1` 才走；`DM_REGION_AHB_MAX=<MB>` 给单次分配上限），
  细节与"分级复量"配方见 ANDROID_PLAN「M3-3e」。**在这台上跑任何会走那条路的动作之前先问用户**（可能重启设备）；
  复核有没有重启用 `.work/uptime.py`（只读：开机时长 + `ro.boot.bootreason`）。
- **USB 掉线 ≠ 重启**：本会话里第二台掉过一次、第一台掉过两次，而第一台的开机时长是 64.8 天、
  bootreason 是 8 月的系统更新 ⇒ 那是 CROSS_MACHINE §六 记的 USB 掉线。**判"有没有重启"一律看 `/proc/uptime`
  与 `ro.boot.bootreason`，别拿 `adb devices` 变空当证据**（重启与掉线在 adb 上长得一样）。
- **⚠ 第二台的 AHB 线已暂停（2026-10-16 下午，用户决定）**：分级复量把那台设备又重启了两次（累计 5 次），
  结论已经足够定性 —— **AHB 支路不能用来铺客体堆**：这块 GPU-only 用法位的内存**没有原子/独占访问语义**
  （每页一次 `LDADDAL` 就让宿主进程 `SIGBUS`），而客体一拿到堆就要用互斥与引用计数。**主线回第一台**。
  第二台现在是安全的（装的是**默认关闸**的包；`dmhost` 上新加的开关也全部缺省关）；会重启设备的脚本
  `.work/dev2_ahb_ladder.py` 已加"必须显式 `--yes-may-reboot` 且人要守着"的闸；只读复核用 `.work/uptime.py`。
  读数与完整结论见 ANDROID_PLAN「M3-3e」第八、九节（含"为什么要多台机器"的实测理由）。

## 六、容易踩的几点

- **git 身份**：这台机器没有配 `user.name` / `user.email`（提交时用 `-c` 临时传与历史一致的身份：`gbcmikeblue <1061589622@qq.com>`）。新电脑先把身份配好，否则 `git commit` 直接以 `Author identity unknown` 失败。
  注意新机器上**可能已经有一个别人的全局身份**（第二台机器实测：全局是 `bicheng.gao <bicheng.gao@yiartkeji.com>`，与仓库历史不一致）。不要改全局，只设仓库级即可：
  `git config user.name gbcmikeblue` + `git config user.email 1061589622@qq.com`（`git config` 不带 `--global` 就只作用于本仓库）。
- `origin` 已是 fork 地址 `git@github.com:gbcmikeblue/d2r-mobile.git`，`android` 分支跟踪 `origin/android`。SSH 用的是 `gbcmikeblue` 那把 key，新电脑要自己生成并加到 GitHub 账号。
- 中转区要更新就再 `adb push` 一次覆盖；要清空直接 `adb shell rm -rf /sdcard/d2rm/pc`，下次再推回来即可。
- 手机上这份中转副本**不是**运行时用的那份数据：app 跑起来读的是 `/data/local/tmp/dm`，两者别混。
- **三个派生头文件不在上面那张表里**：`dm\host` 的 `dm_vault_tab.h` / `dm_hud_exp.h` / `dm_bright_levels.inc` 由游戏文件生成、被 `.gitignore` 忽略，
  缺了 `build_apk.cmd` 会在 `dm\host\diag.c` 报 `'dm_hud_exp.h' file not found`。数据取回后按第二节的规则重生成一次。
- **PowerShell 的 `>` 会改编码**（写成 UTF-16LE）。要保原始字节的场合——生成 C 头文件、落日志——一律用 **cmd 的 `>`**。
  本机实测同一个脚本：PowerShell 版 93,550 字节 UTF-16，cmd 版 47,124 字节 UTF-8。
- **USB 会整段掉线**：本机（Hyper-V + USB 直通）整目录 `adb pull` 传大文件约 30 秒即断、`adb devices` 随之变空，
  且与熄屏无关。见第三节「手机不稳时」。

## 七、当前进度与下一步（交接：接着跑的人从这里开始）

> **这一节会过期。** 记于 2026-10-15（M3-3c 的内容基线 `ed900fb`；前几轮「M3-3b」/「M3-3a」/「M3-2z」）；
> **最新提交**：`ff82ba9`（2026-10-16，**N4-g = 把"顶点输出"这一层两刀排掉**）：
> ① **带层的 B 臂**（旧路对照；`.work/vklayer-b.conf` 与 A1 的 A 臂 conf **只差一行**：不设 `DM_GFX2`）
> 用新脚本 `.work/vuid_species_diff.py` **按 VUID 名**逐项分堆 ⇒ **A 15 种 / B 15 种 / 两臂共有 15 种、
> 只有 A 臂 0 种、只有 B 臂 0 种** ⇒ **新地基零新增误用**（15 种里**唯一落在"顶点输出那一层"的是
> `VUID-RuntimeSpirv-maxCombinedClipAndCullDistances-00710`**，A×20 / B×20；组 (a) 第一优先的
> `vkBindBufferMemory-memory-01035/01508/02726` ×14 的原文显示它就是我们自己的**内存换轨**、**旧路一模一样**）。
> ② **能力**（新探针 `dm/android/vk2/vk2_cap_probe.c`，只问不接管）：设备 **165 个扩展**、
> **`VK_EXT_transform_feedback` 在（第 117 条）、`transformFeedback = 1`、`geometryStreams = 1`**；
> **`VK_EXT_fragment_shader_interlock` 不在设备上**（层的 20 条抱怨 = 真没这个扩展）；
> **`shaderClipDistance = 1` / `shaderCullDistance = 1`（设备支持）**、**`maxCombinedClipAndCullDistances = 8`**。
> ③ **★ 一条路线判定**：**TF 这条路本轮不走** —— 离线扫 `.work/spv` 6923 份：**0 份**带
> `XfbBuffer`/`Offset` 装饰 ⇒ 要用 TF 抓游戏的 `gl_Position` **必须给游戏那份 SPIR-V 加装饰**，
> 与"不许改产物"冲突（能力本身在 ⇒ **要重开只需一句"允许加装饰"**）。
> ④ **离线核对**（NDK 自带 `spirv-dis`）：主路径 `f88d27f131d71049` 的 `%gl_PerVertex` 成员
> 0=Position / 1=PointSize / **2=BuiltIn ClipDistance** / **3=BuiltIn CullDistance**，而**全部取值链只有 9 条**
> **没有一条点到成员 2/3**；全库 **2345 份顶点着色器声明 `Clip[8]`+`Cull[1]`（9 > 8）而"写"0 份"读"0 份**
> （形状来自译文器 `Declarations.cs:454-476` 无条件产出）⇒ **驱动看到的是"9 个内容未定义的裁剪/剔除距离"**。
> ⑤ **两刀**：`DM_CLIPDIST`（`vk_device.c`，把两个核心特性挂进特性链，**缺省关**）两轮
> **亮度都是 0/0/0**（负结果）；**新探针 `vk2_tri_probe`** 的**我们自己的三角形** ——
> A = `Clip[1]+Cull[1] = 2`（glslang 的默认形状）vs B = **照游戏那份** `Clip[8]+Cull[1] = 9`，
> 两份都**不写**、其它完全相同 ⇒ **A 32640 红像素 / B 32640 红像素**（总 65536 = 半屏 49.8%），
> **特性关/开两档都一样** ⇒ **"声明 9 个裁剪/剔除距离却从不写"不足以让图元消失**；
> 顺带证明**我们自己的三角形落屏**（光栅化这一层是通的）。
> ⑥ 回归：N1/N2/N3/N4-b 各一轮 + `vk2_sub_probe` 三轮 ⇒ **非时间判定键全一致**、
> `VK2_INFLIGHT_CHOSEN` 三轮都是 3（⚠ `VK2_D3_TO_D4_GAIN_PM` 某档测成 ≈0 时给天文数字，**判据别读它**）。
> **画面仍全零**（两轮查看器打开、亮度 0/0/0）⇒ **M2 的验收仍未过**。
> 结论 `ANDROID_PLAN` §八·N4-g /「七、下一步」末条；仪器事实 `TOOLS` B¹²；**下一个 = N4-h**
> （把"我们自己的三角形"画进**游戏的通道里**，`DM_TRI_IN_GAME=<绘制序号>`）。
> **其前** `cac0a15`（docs：把"A1 之后推荐的下一步"写进提示词文档）；
> **其前** `ab52812`（2026-10-16，**A1 = 真机装上 Vulkan 校验层 —— 答案是"有帮助"，
> 而且它给出一条决定性的负结果**）：层的 `libVkLayer_khronos_validation.so`（**27,684,688 字节**，
> `android-binaries-1.4.363.0.zip`）进 APK 的 `lib/arm64-v8a/`（`build_apk.cmd` 里新增的**缺省关**的
> `DM_VKLAYER=1` 可选段），用 `adb shell setprop debug.vulkan.layers VK_LAYER_KHRONOS_validation` 启用
> —— **不用 root、不用改一行代码**（清单里 `android:debuggable="true"` 本来就有；`/data/app/…/lib/arm64/`
> 里确有抽取出来的 `libdm.so`，所以 loader 的 app 私有层搜索路径可用）。自证：logcat
> `added global layer … from library '/data/app/…/lib/arm64/libVkLayer_khronos_validation.so'` + `Loaded layer`。
> **一轮 60 秒照出 218 条 Error / 16 种 VUID**：
> ① **★ 修掉一条真误用（已复验）**：`SPIR-V Capability DrawParameters` 归
> `VkPhysicalDeviceVulkan11Features::shaderDrawParameters` 管，而 `vk_device.c` 只挂了 Vulkan13 + divisor，
> **这个结构从来没挂过** ⇒ **游戏的顶点着色器 1839 份在用 `gl_BaseInstance`**（离线 `.work/spv` 的
> `BuiltIn 4425`）⇒ 已挂进特性链（`DM_DRAWPARAMS` 缺省开），**修后层不再报它**（按 capability 逐项对比：
> `DrawParameters` 2 → 0）；**但画面没变**（查看器仍 `0/0/0`）⇒ 按"一次只改一个变量"记成**负结果**；
> ② **★ 对卡点 A 最要紧的是一条负结果**：**16 种 VUID 里没有一条是"draw 层面的用法错误"**（没有"顶点缓冲
> 未绑定 / 管线与渲染通道不兼容 / 描述符未绑定 / 顶点属性与着色器声明不匹配"）⇒ **我们的 draw 在 API 层面
> 合法，几何是自己算没了**（与 N4-e / N4-f 的收窄一致）⇒ **下一步只剩顶点着色器输出（N4-g）**；
> ③ 其余 15 种真误用已逐条列档：SPIR-V 自身不合法（`spirv-val`：缺 `StorageImageWriteWithoutFormat`）、
> `Float64` 声明了但设备不支持（**离线核对：0 份真的声明 64 位类型 ⇒ 噪音**）、
> `SPV_EXT_fragment_shader_interlock` 未启用、`ClipDistance 8 + CullDistance 1 = 9 > 上限 8`、
> `vkBindBufferMemory` 的内存类型与 dedicated、`vkCmdCopyImage` 的 aspectMask、信号量重复 signal、
> 交换链 `compositeAlpha`、`imageCreateMaxMipLevels`、`ImageViewCreateInfo` 两族…
> ④ 意外读数：**带层的两轮"出图"是 1453 / 932 帧**（不带层时 72~107）⇒ 层的插桩把时序整个改了、
> **那两轮没停住**（只记不上判据）。设备已用 `.work/vklayer_off.py` **退回常态**（干净包 + 属性已清 +
> `lib/arm64/` 只剩 `libdm.so`）。读数 `.work/logs/logcat-vk-a1.txt`（2.45 MB）/ `logcat-vk-a2.txt`；
> 结论 `ANDROID_PLAN` §八·A1，配方 `TOOLS` §G。
> 其前 `de08e13`（2026-10-16，**N4-f = "顶点数据本身"与"常量缓冲"都是正常的（前半答完）** ——
> **回放逻辑一行未改**，只加了一个**缺省关、只读**的诊断开关 `DM_VTX_PROBE=1`（挂在**既有判据行**那一点上：
> 前 8 次绘制 + 每 2048 次）：
> ① 读**影子里的**字节（新地基的池 = 本进程可读写的普通内存；旧路的池也是 `mmap` 回来的 ⇒ **两臂都能读**）：
> 位置属性（Location 0，缺则取着色器声明里第一条真的绑了的）的 NVN 格式 / 步长 / 偏移 / 流 / 影子地址 +
> 按格式解码前 4 个顶点；顶点阶段**所有**有地址的常量槽各读前 64 字节。**只读**：不改状态 / 不标脏 / 不提交。
> ② **A 臂 5 轮 + B 臂 2 轮**：**位置数据 NaN 0 / Inf 0 / 整片全零 0**（形状两种：UI `3×Float 步长 20`
> （`{float3 位置, float2 uv}`，样例 `(-370,1251,1)`）、主场景 `4×ShortSNORM 步长 28/24/20`
> （归一化压缩顶点，样例 `(0, 0.980132, 0.042787, 0.207434)`））；**常量缓冲 NaN 0 / Inf 0**、
> 逐槽 |max| **两臂逐项相同**（槽0 1~1067、槽2 ≈1、**槽3 = 1600 = 屏幕宽**、槽10 410~1070）；
> **分量类型与 SPIR-V 声明对上**（离线 `.work/attr_types.py`：`f88d27f131d71049` 的 Location 0/1 都是
> float×4；NVN 给 `3×Float` 时第 4 分量补 1，Vulkan 与 NVN 同语义）⇒ **"顶点数据本身坏掉"这一支被排除**。
> ③ **画面仍是全零**（截图轮 `DM_WIN_SRC=28`，自证三处齐：781 次读成功、**亮度 0/0/0**，
> `n4b-n4f-shot-a.png` 17718 字节）—— 本轮没改行为，这条只用来自证"没动画面"。
> ④ **⚠ 仪器自己出过一次错并当场更正**：第一版只查"**第一个**非零常量槽"（`常量[0][0]` 的前 4 个
> `-0.00125,0,0,0` 不像变换矩阵）⇒ 改成**扫全部槽**之后两臂的逐槽 |max| 才对得上（第 ⑩ 条同类）。
> 读数 `.work/logs/n4-n4f-vtx-{a,a2,a3,a4,a5,b1,b2}.log` + `n4-shot-n4f-shot-a.log`；抽表
> `.work/n4f_report.py`、conf `.work/n4f_confs.py`，结论见 `ANDROID_PLAN` §八·N4-f，仪器事实见 `TOOLS` B¹¹。
> **下一步 = N4-g**：只剩"**顶点着色器输出**"（位置经变换后全在裁剪体之外 / 输出 NaN/Inf）——
> **第一件事是问设备能力** `VK_EXT_transform_feedback`（+ `transformFeedback` / `geometryStreams`）：
> 有就抓 `gl_Position` **直接回答"经变换后在不在裁剪体里"**（这是唯一能在**不改产物**的前提下拿到它的办法）；
> 没有就走"离线核对 `gl_Position` 的表达式链 + 已知色底子上的最小"我们自己的三角形"落屏自证"。
> 其前 `ca10c0d`（2026-10-16，**N4-e 的第一刀 = 把"画面在哪"收进"光栅化之前"** ——
> **回放逻辑一行未改**，只加了**两个缺省关的诊断开关** + 三格计数。**读法换了**：不再肉眼看屏幕，
> 改成"**用我们自己的写入当刻度**"：
> ① **扫描器**（现成开关 `DM_NVN_SHOT_SCAN=<帧>[:<路径>]`，一次扫**全部**已知贴图逐张逐级）：
> **A′/B′ 两臂 @出图 40 逐项相同** —— 139 张已知贴图（≥256×256 的 26 张）、读 33 次、**成功 20 / 失败 13**；
> 20 次成功里**只有序号 27 的第 1/2 级是整片 255（纯白）**，其余全 0/0/0；@60 一轮 192 张 / 读 167 次 /
> **成功 13、失败 154**（`【黄】读回不支持格式 138`）⇒ 判据只许写"能读回来的除 27 第 1/2 级外全 0；
> 154 次读不回来的那一档**没有读数**"。
> ② **两个对照**（`DM_GFX_PROBE=7/28`）：序号 7 = **136.0**（与 conf 里预算的值逐位命中）、序号 28 三级 =
> **152.0**（浮点格式经 1/2.2 伽马）⇒ **读回链 + 存储归属通**。
> ③ **通道探针**（新开关 `DM_PASS_PROBE=<序号>[:end]`）：`:end` ⇒ 28 三级 = 152 ⇒ **附件/视图/布局/存储
> 那一层成立**；不带 `:end`（通道从已知色开始、游戏往上画）⇒ **仍是整片 152** ⇒ **游戏 3.9 万次绘制
> （有目标 2.2 万次）一个像素都没落下**。
> ④ **光栅化侧逐条排除**：`DM_NO_DEPTH_TEST=1` 仍全 0、`DM_PASS_AREA=2` 仍全 0、
> **`DM_RASTER_BRUTE=1`（新开关：深度/模板关、剔除关、混合关、写掩码全开、绘制前把动态视口/裁剪铺满附件）
> + 已知色底子 ⇒ 仍是整片 152** ⇒ **片元在光栅化之前就没了**。
> ⑤ **顶点/几何侧新仪表**（`顶点流 绑上 / 没绑上（按流）/ 一条都没`、`顶点数 0`、`索引数 0`）⇒
> 两轮 **没绑上的 2682~2762 次全部是第 4 条流**、**顶点数 0 的绘制 0 次** ⇒ **"顶点流没绑 / 几何为空"
> 都不成立**。**一条更正**：我一度以为"心跳那一行被截断"，其实是 `.work/poll.py` **显示时截行** ——
> 读落盘日志量字节才作数（五轮都完整、最长 643 字节，脚本 `.work/n4e_hb_len.py`）。
> 读数 `.work/logs/n4-shot-n4e-{scan-a40,scan-b40,probe28-a40,probe7-a40}.log` +
> `n4-n4e-{passprobe28,passprobe28open,nodepth,passarea2,brute,vtx,vtx2,vtx3,vtx4}.log`
> （每轮都带**开关自证行**，逐轮核对 `.work/n4e_chk_sw.py`），结论见 `ANDROID_PLAN` §八·N4-e。
> **下一步 = N4-f**：只剩"**顶点数据与着色器输出**"（位置经变换后全在裁剪体之外、或出现 NaN/Inf）——
> 做法 = **直接读影子里的顶点缓冲与 UBO 字节**（新地基的池就是本进程可读写的普通内存）先查 NaN/数量级，
> 再把属性形状/类型与着色器声明对上（现成 `.work/attr_shape.py` / `.work/attr_types.py`）；
> **每组实验的判据 = 一次 `DM_PASS_PROBE=28` 扫描**（已知色底子上出现非 152 的值就是"落下过"，不需要看图）。
> 其前 `a4fa0d5`（2026-10-16，**N4-d 的第二部分 = 那条"暗红带"是我们自己的测试图（**更正**）+
> 把查看器打开重取画面证据 + 水位线口径定案 + atrace 三份落到线程** —— **回放逻辑一行未改**，只加了仪器：
> ① **★ 更正**：N4-a ~ N4-d（第一部分）**两臂所有 conf 都是 `DM_WIN_SRC=0`**（查看器显式关）⇒ 前端帧落到
> `paint()` 测试图（`main_apk.c:592`；带 = 1156×650 = 16:9、白条 133×180 @ y=270 逐位吻合）⇒
> **那三轮"画面没变、M2 未过"的截图证据作废**（原文已留 + 批注），**"画面内容没变"这条结论本身仍成立**；
> ② **把查看器打开重跑 6 轮**（A′ = `DM_GFX2=1` / B′ = 旧路 × 第 0/1 级）：**两臂两级亮度读数全是
> `均值 0.0 / 最小 0 / 最大 0`** ⇒ **游戏自己那张目标（序号 28）里一个非零字节都没有、且不是新地基引入的**；
> 读成功的三张截图 **99.611% 纯黑**、红带 0 行，`vb-s45` 因设备丢失后读回失败（`跳过 4869`）**回落到测试图
> 又长出红带** ⇒ 同一臂两种形态 = 判定性证据。
> ③ **水位线口径定案**（把 `vk2_sub_frame()` 打进心跳/判据行）：`begin` 只给 frame、`commit` 成功后才置
> `in_flight`；`slot_reap()` 先推 `reaped_max` 后清 `in_flight` ⇒ **`Z == X+1`、`K == X − Y + δ`**；
> 真机 95 个采样全自洽（`4/4/5`…`98/97/99`），冻结态 `89/87/0（在飞 3）`（δ=1 稳定几十秒）——
> **"差 1"是采样口径，不是队列不动**。
> ④ **atrace 三份**（`.work/n4d_atrace2.py`，判到停顿后**再等 10 s** 才 stop）：我方事件量 **×6.2** 维持
> 13.0 / 14.5 s、**显示 / GPU 通路阻塞率反而 ×2.3~2.8**、**第 2 份有嵌套站点 `vk：提交命令缓冲 11735 ms`**
> （= 我方这次 `vkQueueSubmit` 自己卡 11.7 s）；**加了线程命名**（宿主 `dm-*`、客体 `dm-g<handle>`；
> **更正**：跑 `android_main` 的是框架线程 `Thread-N`，不是 `tid == pid`）⇒ 停顿期间 **`dm-guest`
> 4.6 kHz 睡→醒、阻塞 caller 0 条**、**前端被 `kgsl-events` 叫醒 2394 次/10 s** ⇒ **GPU 还在报事件**；
> **三份 + 六轮没有一次自愈**。
> ⑤ 新红行 `【红】回读拷贝 等定向栅栏超时（5000 ms）：VkResult 2` 按"**见证不是原因**"记档（不进基线）。
> 读数 `.work/logs/n4-shot-va{,0}-s20.log` / `va-s45` / `vb{,0}-s20` / `vb-s45`、`n4-n4d-va1.log`、
> `.work/trace/at_stall-n4d-at2-{1,2,3}.txt`（+ 命名后 `at_stall-n4d-at3-1.txt`）、
> 截图 `.work/logs/n4b-{va-s20,va-s45,vb-s20,vb-s45}.png`，结论见 `ANDROID_PLAN` §八·N4-d（第二部分），
> 仪器事实见 `TOOLS` B⁹。
> **下一步 = N4-e**：**正面问"为什么游戏自己那张目标（序号 28）第 0/1 级一个非零字节都没有"** ——
> 第 0 步（最高性价比）= **拿查看器当扫描器**（`DM_WIN_SRC=<序号>` 逐张跑、读 `查看器：… 亮度`）回答
> "有没有哪张颜色目标不是全零"；另两条 = 把"没有渲染目标"的真实条数量出来（A′ ≈ 1.07 vs B′ ≈ 0.25）、
> 问"附件 / 目标的存储归属"（GPU 往哪块内存写）。**"为什么只有那条红带"这个问题已作废。**
> 其前 `d12dd44`（2026-10-16，**N4-d 的第一部分 = 把我方自己的失败变成带时间戳的读数 +
> 六轮 A/B 认定"停顿不是 N4-c 引入的"** —— **回放逻辑一行未改**，改的是**仪表与口径**：
> ① `vk_diag_line`（宿主每秒一条、**带时间戳**）挂上「我方超时 回读拷贝 N / 取槽 M；**提交到 frame X、
> 完成到 frame Y**（在飞 K）；ReadTexels 读到 GPU 写过的贴图 N 次」—— 模块自己打的【红】行没有时间戳，
> 只有这一行能把"超时发生的时刻"与"停顿窗口"对上；② `ReadTexels` 那条判据行从"每 300 次提交"挪到
> **每 60 次提交 + 第 1 次**的独立行（两臂都打，N4-c 一整轮只有 ~100~200 次提交 ⇒ 它一行都没打过）；
> ③ **修掉两个读数问题**：`vk2_sub` 的 `reaped_max` 只在"真等栅栏"那几条路更新、而轮询回收的主路不更新
> ⇒ 水位线虚低（新判据行第一次跑就报"提交到 79、完成到 3"），现在在 `slot_reap()` 里统一更新；
> 新地基下"一次性提交"等的是**它自己的定向栅栏**而台账站点名还叫"等空闲"，按 `use_fence` 分开。
> **读数（A 臂 5 轮 + B 臂 2 轮）**：两臂都出现同三种形态（~10~12 s 停顿 / 设备丢失 `-4` / 我方定向栅栏超时）
> —— B 臂 12.1 s 停顿的三站点 11894~12109 ms、B 臂第一条 `-4` 落在 **`vkQueueWaitIdle`** 上
> ⇒ **不是 N4-c 引入的**；`ReadTexels 读到 GPU 写过的贴图` **六轮全是 0** ⇒ 渲染目标写回池**不做**；
> `atrace` 抓到一次停顿：停顿后**我方主/前端线程在自旋**（3 s 内 14229 条事件）而**显示 / GPU 通路的阻塞率
> 反而 ×3.6**（41.6→151.6 条/秒）⇒ **不是"整条通路一起睡"**；**截图仍与 N4-b/N4-c 同形**（黑底 + 一条暗红带
> + 白方块，与参照影片任何时间点都不匹配）⇒ **M2 的验收仍未过**
> ⚠ **【更正：那张"同形"的截图是 `paint()` 测试图，见上面 `a4fa0d5` 的第 ① 条】**。读数
> `.work/logs/n4-n4d-{a,a2,a3,a4,a5,b1,b2}.log`、截图 `.work/logs/n4b-n4d-a5shot.png`、
> trace `.work/trace/at_stall-n4d-at1.txt`，结论见 `ANDROID_PLAN` §八·N4-d，仪器事实见 `TOOLS` B⁸。
> **下一步 = N4-d 的第二部分**：`atrace` 的停止**推迟约 10 s** 复验（排除"停止动作"的边界效应）+
> 把"停顿期间谁在自旋 / 谁在等"落到**具体线程**上 + 把水位线三个数（提交到 / 完成到 / 在飞）差 1 的
> 口径对齐（把 `vk2_sub_frame()` 也打进判据行），**然后才谈画面**。
> 其前 `e01c0ed`（2026-10-16，**N4-c = 接提交侧 + 补 GPU→池的回读** ——
> ① **提交侧换成 `vk2_sub_*`**（`mtl_sub_begin/commit` 用它的槽与 cb、`g_fence` 跟 `vk2_sub_last_frame()`、
> 在飞 **3**）；**新增挂点 `vk2_sub_set_reap_hook`**：模块在"观察到某个槽的栅栏已经 signal"的那一刻先调钩子
> 再让槽可重用（栅栏句柄会被下一个提交 `vkResetFences` 重用 ⇒ 事后轮询没有意义），钩子做副作用 /
> **往返型回读** / 描述符池；`mtl_drain` / `mtl_finish` → `vk2_sub_wait_all()`；
> `mtl_wait` 按**客体给的超时**等 frame（`vk2_sub_wait_timeout` —— 修掉本轮自己引入的真 bug：
> 第一版用模块的 5 s 上限截短了客体的超时，现场读数 5005 ms）。
> ② **GPU→池的回读**：录制期把 `COPY_B2B` / `UPDATE_UB` 的**目标**逐段记进本槽（64 段上限，超了并成上界），
> 槽回收时按**那次提交自己的栅栏**字节精确回读进影子 + 只清 GPU 位（**不扫全池** —— 那会把还没被 GPU 写到的页
> 提前回读 + 置 CLEAN，那一次写就丢了）；语义 = "GPU 写过的区间以 GPU 为准、区间之外的 CPU 字节保住"。
> 半页竞态两条对策：帧开头上传**跳过**还带 GPU 脏的页 + 回收时字节精确回读。
> ③ **核心回放层那五处 `vkQueueWaitIdle` 全部收口**（走 `vk_idle_wait` 计数入口；新地基下一处也不真的等；
> 心跳逐秒报"等队列空闲 N / 被定向栅栏替代 M"）。
> 探针 `vk2_takeover_probe`（十一问）四轮判据字段一致、退出码 0；**回归** N1/N2/N3 **判据字段 0 项不一致**
> （`.work/n4c_regress_diff.py`）。真机 A 臂三轮：**等队列空闲 0**、**每帧提交 1.00**、
> **槽等待率 0‰（0/60，轮询回收 56）**、**每帧脏 run 4.42**、**每帧回读 ≈17 KB / 0.5 ms**
> ⇒ **六个新基线字段全部有真帧读数**；**但出图仍停在 115 / 122 / 96**、**对照臂（旧路）本轮也停在 72**
> （两臂同一形态）、**截图仍是"黑底 + 一条暗红带 + 一个白方块"⇒ M2 的验收仍未通过**
> ⚠ **【更正：那张截图是 `paint()` 测试图，见上面 `a4fa0d5` 的第 ① 条】**。
> 读数 `.work/logs/probe-8b1f1e50-vk2_takeover_probe.txt`、`n4-n4c-a{,2,3}.log`、`n4-n4c-b.log`，
> 截图 `.work/logs/n4b-n4c-a{1,3}.png`，结论见 `ANDROID_PLAN` §八·N4-c。
> **下一步 = N4-d**（把 N4 做到底：出画面 —— 第一嫌疑换成"那 ~10 秒的驱动级停顿"，
> 先按「M3-3a」的 `atrace` 形状把"停顿"与"出图停住"分开）。
> 其前 `86e1631`（2026-10-16，**N4-b = 把 GPU 侧的内存接管下来** —— `mtl_addr` 改成回**后备资源**
> （块缓冲 + 块内偏移；跨块 / 池外 / 该段没有后备都返回 0；**签名不变，8 处调用点一处没改**）、
> 脏标记（描述符 UBO/SSBO、顶点流、索引流、`COPY_B2B` 的源、缓冲区贴图视图）、`UPDATE_UB` / `COPY_B2B`
> 的**目标**标 `GPU_DIRTY`（影子从此过期）、**帧开头把 CPU 脏页搬上去**（录在 `mtl_sub_begin` 的 cb 最前面）、
> 槽回收时交还**那一段**环（**环按槽分** = N3 量出的待办；段数 = `MAX_INFLIGHT`，编译期 `_Static_assert` 钉住）、
> **初始同步**（`vk2_xfer_initial_sync`：整池一次搬完 —— 一帧一段环要两百多帧）、块的用法位加上
> `UNIFORM_BUFFER`（与旧路的池缓冲一致；N1 的读数 `VK2_UNIFORM_BUFFER_BIT_960MB=ok` 已量过这条宽容度）；
> 顺带修一个**真 bug**：上传切片过去按 64 对齐而清位按页 ⇒ 切片落在页中间时尾部不足一页的字节被
> **误标 CLEAN 却从没上传**（现在切片/段容量按页对齐、回读只清"整页"那一部分）。自证探针
> `dm/android/vk2/vk2_takeover_probe.c`（八问）四轮判据字段一致、退出码 0；**回归** N1/N2/N3 判据字段
> 逐项不变。真机 **A 臂（`DM_GFX2=1`）三轮**：7 池 / 1675 MB 全登记、**后备 1675 MB**、
> `no_region/small/pool_fail/dirty_fail` 全 0、**初始同步 1675 MB / 53~105 轮 / 375 ms**、
> 每帧开头上传 **36~52 KB**（个别帧 15.4 MB）、设备丢失 0 条 ⇒ **六个新基线字段第一次有真帧读数**；
> **但 M2 的验收「屏幕出（游戏自己的）画面」仍未通过**：A 臂出图停在 **85 / 88 / 118**、对照臂（旧路）
> 两轮健康推进（318 / 1243），**两臂截图都是"黑底 + 一条暗红带 + 白方块"**（画面内容没变）。
> 当时的**下一步是 N4-c**（接提交侧（`inflight_*` → `vk2_sub_*`）+ 补 **GPU→池**的回读）——
> **N4-c 已完成，见上面的 `e01c0ed`**）；
> 其前 `2109af4` = **N4 的第一步 N4-a = 选路点 + 真机池账** —— 新地基的选路点落在
> `mtl_pool_add` / `mtl_pool_del` 上（`DM_GFX2` 缺省关；`DM_GFX2_BACKING_MB` 给后备预算，本轮 = 0 ⇒ **只登记
> 不建后备**，新增 Vulkan 占用 **0**）+ `vk2_pool` 的后备预算（`chunk_skipped` / `skipped_bytes` / `backing_bytes`
> 的账要能对上）+ 跑轮脚本 `.work/n4_apk_round.py`。真机 **A/B 两臂各一轮 60 秒**：**7 个池 / 1675 MB**
> （512 / 10 / 400 / 1 / 710 / 2 / 40 MB）、每个都登记成功、**`no_region = 0`**（⇒ 新地基"客体池是本进程可读写的
> 普通内存"这条**唯一前提**在**真机游戏路径**上成立）、`small / pool_fail / dirty_fail` 全 0；对照臂日志里
> **一条新地基行都没有**，两臂的旧路池清单与 `【红】/【黄】` 行**逐条相同**（⇒ 零副作用），两臂都在同一处出现
> 旧路**已知的**"出图停住"。**GPU 侧仍全走旧路**（`mtl_addr` 未接管）⇒ 与画面/崩溃率无关；N1/N3 探针回归轮
> **判据字段逐项相同**；§9.7 的接管点表已逐行核对（4 处行号更正 + 1 条"安卓已接"更正 + 往返型的设计缺口）——
> 与画面/崩溃率无关）；
> 其前 `22ee122` = **N3**（帧批提交 + 有界在飞 + `vkQueueWaitIdle` 归零 ——
> `dm/android/vk2/vk2_sub.c/.h`（**一槽一池一 cb 一栅栏**；取槽先轮询 `vkGetFenceStatus`、满了**只等最老那个槽的栅栏**
> （带超时）、**绝不等队列空闲**）+ 自证探针 `vk2_sub_probe.c`，第一台**四轮**（**判据字段逐轮完全一致**）：
> **G4 的前两条判定性成立** —— **一个客体提交 = 一次 `vkQueueSubmit`**（24 次空提交里探针自己数的 24 =
> `commits` 24 = `submits` 24；全程 取槽/提交/`vkQueueSubmit`/回收 = 152/152/152/152）、
> **`vkQueueWaitIdle` 计数 = 0**；**"3 还是 4"由读数定下来 = 3**（D=1/2/3/4/8 各 24 次带 128 MB GPU 拷贝的提交：
> **D=3 → D=4 的每提交耗时变化 -4~+26‰**，四轮全部 < 5%；D=1 是 7822~13587 µs 而 D≥3 稳定在 4059~4085 µs
> ⇒ **有界在飞本身就买到吞吐**）；`vk2_sub_wait_all()` 等的是**所有在飞槽**（不是队列空闲）；
> **提交槽能被 N2 的搬运窗口复用**（上传 1 MB → 回读**逐字节等于独立重算的期望**、回收时自动把环交还）；
> 顺带量出一条 **N4 待办**：**暂存环是单窗口的**（在飞 3 个槽只有 1 个能载上传窗口）⇒ N4 要**按槽分环**；
> **仍只让新模块可用，不接管回放层**（回放层三处一行未动、没有任何调用方、一个游戏轮都没跑）；
> 其前 `04b4dbf` = **N2**（脏记账 + 上传/回读窗口 + 显式屏障 ——
> `dm/android/vk2/vk2_dirty.c/.h` + `vk2_xfer.c/.h` + 自证探针 `vk2_xfer_probe.c`，第一台**四轮**
> （**判据字段逐轮完全一致**，时间字段报区间）：**G1 成立**（4/4 档往返闭环逐字节等于**独立重算**的期望）、
> **G2 成立**（不等待读 = 旧值 / **等完栅栏仍不回读 = 还是旧值** / 回读之后 = 新值）、脏位图与朴素模型
> **逐位一致**、`vkQueueWaitIdle` 计数 **0**；**仍只让新模块可用，不接管回放层**；
> 其前 `b966494` = **N1**（影子 + 分块后备 + 地址反查：`vk2_pool.c/.h` + `vk2_pool_probe.c`，四轮 29 个字段
> 一致，**G3 成立**、`DM_GFX2` 缺省关且可读自证）、
> `0af1312` / `f7d5f24` = **纯文档，口径变更**：**第二台设备不使用** ⇒ 只用第一台、**N5 暂缓 / G5 留档待验**；
> `945f7d7` = **新路径的第一个里程碑 N0**：常数标定 —— 新探针 `dm/android/vk2/vk2_probe.c`（只问不接管）
> + 第一台四轮真机读数，§9.9 四个常数全部落地（块大小 960 MB / 脏位图 4 KiB 起步 / `VK2_INFLIGHT` 3 起步 /
> 暂存环 16 MiB 起步）、`4780b9a` = **新路径文档独立成 `docs/rewrite/`**（设计正本移入该文件夹，新增索引
> `README.zh-CN.md`、工具与仪器清单 `TOOLS.zh-CN.md`、新会话提示词 `NEWSESSION.zh-CN.md`）、
> `3113763` = README 与 `ANDROID.zh-CN.md` 改口径、`9a74398` = 里程碑 **N0~N6**（`ANDROID_REWRITE` §十一））
> —— 新机器从这之后接手，恢复步骤照第三节；
> 进度细节与结论以 [ANDROID_PLAN.zh-CN.md](ANDROID_PLAN.zh-CN.md) 的「七、下一步」与「八、进度记录」为准。
> **★ 新路径的入口（开新会话从这里进）**：先看 [rewrite/README.zh-CN.md](rewrite/README.zh-CN.md)（那个文件夹的索引），
> 开新会话时把 [rewrite/NEWSESSION.zh-CN.md](rewrite/NEWSESSION.zh-CN.md) 里的提示词**整段复制**过去即可；
> 开工前照 [rewrite/TOOLS.zh-CN.md](rewrite/TOOLS.zh-CN.md) 的清单一一点。
> **★ 现在的方向**：按 [rewrite/ANDROID_REWRITE.zh-CN.md](rewrite/ANDROID_REWRITE.zh-CN.md)（「七、下一步」第 0 条，架构口径变更）
> 的**里程碑 N0~N6** 走 —— **N0（常数标定，`945f7d7`）、N1（影子 + 分块后备 + 地址反查，`b966494`）、
> N2（脏记账 + 上传/回读窗口 + 显式屏障，`04b4dbf`）、N3（帧批提交 + 有界在飞 + `vkQueueWaitIdle` 归零，
> `22ee122`）都已完成，**N4 已做到第三小步：N4-a（选路点 + 真机池账，`2109af4`）、N4-b（GPU 侧内存接管，
> `86e1631`）、N4-c（接提交侧 + GPU→池回读，`e01c0ed`）都已完成**（都在 2026-10-16，都在第一台）；
> **N4 还差最后一段：画面本身（N4-d）** —— 下一步先把"那 ~10 秒的驱动级停顿"与"出图停住"分开：
> N0 把 §9.9 的四个待实测常数量成真机读数（**块大小 960 MB / 脏位图粒度 4 KiB 起步 / `VK2_INFLIGHT` 3 起步 /
> 暂存环 16 MiB 起步**，只问不接管）；N1 让 **G3 成立**（4 GB 池按块建成 5 块、全功能后备、两个降级计数器为 0）；
> N2 让 **G1/G2 成立**（往返闭环逐字节正确；**可见性是"回读"这一步显式建立起来的**：
> 不等待读 = 旧值、等完栅栏仍不回读 = 还是旧值、回读之后 = 新值）——
> 读数 `.work/logs/probe-8b1f1e50-vk2_probe.txt` / `…-vk2_pool_probe.txt` / `…-vk2_xfer_probe.txt` /
> `…-vk2_sub_probe.txt`；
> N3 让 **G4 的前两条判定性成立**（一个客体提交 = 一次 `vkQueueSubmit`、`vkQueueWaitIdle` 计数 0）并把
> **`VK2_INFLIGHT` 定在 3**（D=3 → D=4 的变化 -4~+26‰，四轮全 < 5%）；
> **N4-a 把选路点接进回放层并量出真机池账**（7 个池 / 1675 MB、**`no_region = 0`**、A/B 两臂零副作用）→
> **N4-b 才是"GPU 侧接管"**（`mtl_addr` → `vk2_addr` + 上传窗口接进提交路径）→
> **N4-c 接提交侧 + 补 GPU→池的回读**（`vk2_sub_*` 顶替 8 槽 + 槽回收钩子 + 往返型回读 + 半页竞态两对策 +
> 五处 `vkQueueWaitIdle` 收口；真帧上六个新基线字段全有读数）⇒ **N4 还差最后一段：出画面（N4-d）** →
> N5 两台设备都过（**暂缓**）→ N6 旧路退场。
> 每块判据是 **G1~G5**，每块照"自证探针 → 读数落盘 → 再接管"三件套；旧路全留（`DM_GFX2` 缺省关）。
> **N0~N6 都在第一台上做**；**2026-10-16 口径变更：第二台不使用** ⇒ **N4~N6 全在第一台上做、N5 暂缓**
> （G5 暂不作为当前验收，替代口径见 `rewrite/ANDROID_REWRITE.zh-CN.md` §六）。
> **⚠ 上机前先读**：CROSS_MACHINE 第五节「第二台」那几条（logs 目录属主坑 / **AHB 线已暂停且会让内核 panic** /
> USB 掉线≠重启），以及 ANDROID_PLAN「M3-3e」第九节（暂停的现状与重开条件）与「M3-3f」（仪器坑）。

### 现在停在哪

- **★ 最新一轮（「N4-g」，2026-10-16，提交 `ff82ba9`）停在**：**把"顶点输出"这一层两刀排掉**。
  改了什么：**新探针** `dm/android/vk2/vk2_cap_probe.c`（**只问不接管**：设备扩展总数 + 点名扩展在第几条 +
  特性结构体逐 bool + 一组上限 + 按游戏路径那份扩展清单建一次逻辑设备）与 `dm/android/vk2/vk2_tri_probe.c`
  （**我们自己的三角形**：两份只差一处的顶点着色器 + 同一个片段着色器，画进 256×256 离屏图、读回来数像素）；
  **另一处只在 `vk_device.c`**：新增 `DM_CLIPDIST`（**缺省关**）把 `shaderClipDistance`/`shaderCullDistance`
  两个核心特性挂进特性链（此前从来没挂过），**缺省关 = 一字不改的既有行为**。
  - 结论：① **带层的 B 臂** + `.work/vuid_species_diff.py` **按 VUID 名**分堆 ⇒ **两臂共有 15 种、只 A 臂 0 种**
    ⇒ **新地基零新增误用**；② **能力**：`VK_EXT_transform_feedback` **在**（第 117/165 条、两个 bool 都 1）、
    `VK_EXT_fragment_shader_interlock` **不在**、`shaderClipDistance`/`shaderCullDistance` 设备支持、
    `maxCombinedClipAndCullDistances = 8`；③ **TF 没走**（抓游戏的 `gl_Position` 要给产物加
    `XfbBuffer`/`Offset` 装饰，离线：6923 份里 0 份有 ⇒ 与"不许改产物"冲突）；④ 离线：**2345 份顶点着色器
    声明 `Clip[8]`+`Cull[1]`（9 > 8）而"写"0 份"读"0 份**；⑤ **两刀都排掉这一层** —— `DM_CLIPDIST` 两轮
    **亮度 0/0/0**、三角形 A/B **两格都是 32640/65536 红像素**（特性关/开一样）；
    ⑥ 回归：四个探针 + `vk2_sub_probe` 三轮，**非时间判定键全一致**。
  - 跑法：`& "<仓库>/.venv/Scripts/python.exe" -X utf8 .work/probes.py run vk2_cap_probe --serial 8b1f1e50`；
    三角形的 SPIR-V 先生成再推：`.work/n4g_tri_spv.py`（NDK 自带 `glslc`，并 `spirv-dis` **自证**两份变体只差数组长度）
    → `.work/n4g_tri_push.py`（推到 `/data/local/tmp/dm/tri/`）→ `.work/n4g_regress.py`（跑第二档 + 四个回归）；
    游戏两轮 `.work/n4g_clip_rounds.py`（对照/实验各一轮，只差 conf 一行）。
  - 读数：`.work/logs/probe-8b1f1e50-vk2_cap_probe.txt`、`vkjson-8b1f1e50.json`、
    `vuid-diff-vk-a2-vk-b1.txt`、`logcat-vk-b1.txt`、`clipdist-use.txt`、`dis-f88d27f131d71049.txt`、
    `tri-spv.txt`、`probe-…-vk2_tri_probe[-clipdist].txt`、`n4-shot-n4g-clip-a{0,1}.log`、
    `n4b-n4g-clip-a{0,1}.png`、`…-vk2_{pool,xfer,sub,takeover}_probe-n4gregress.txt`、
    `…-vk2_sub_probe-n4gsub{1,2,3}.txt`；结论 `ANDROID_PLAN` §八·N4-g，仪器事实 `TOOLS` B¹²。
  - **下一步 = N4-h**：把"我们自己的三角形"画进**游戏的通道里**（`DM_TRI_IN_GAME=<绘制序号>`，缺省关）——
    它落屏 ⇒ **通道与状态在那一刻是好的、只有游戏的绘制产不出片元**；它也不落屏 ⇒ **那一层本身有问题**。
- **其前（「A1」，2026-10-16，提交 `ab52812`）停在**：**真机装上 Vulkan 校验层，
  答案是"有帮助"**。做法：层 `.so`（27,684,688 字节）进 APK 的 `lib/arm64-v8a/`（`DM_VKLAYER=1` 时才进），
  `setprop debug.vulkan.layers VK_LAYER_KHRONOS_validation` 启用 —— **不用 root、不改代码**。
  一轮照出 **218 条 Error / 16 种 VUID**，**修掉一条真误用**（`shaderDrawParameters`，1839 份着色器在用
  `gl_BaseInstance`；修后层不再报它，**但画面没变** ⇒ 负结果）；**最要紧的是一条负结果**：
  **没有一条是"draw 层面的用法错误"** ⇒ 我们的 draw 在 API 层面合法 ⇒ **下一步只剩顶点着色器输出（N4-g）**。
  - 跑法：`.work/vklayer_round.py --conf .work/vklayer-a.conf --tag vk-a1 --secs 60 --build --install`
    （出包带层 → 装机 → 设属性 → **抓 logcat** → 跑 → 拉日志 → **清属性回退**）；
    退回常态：`.work/vklayer_off.py`；归类：`.work/vklayer_report.py`（按 VUID）、
    `.work/vklayer_ctx.py`（完整消息体）、`.work/vklayer_cap_diff.py`（按 capability 名逐项对比）。
  - 读数：`.work/logs/logcat-vk-a1.txt`（2.45 MB，修前）/ `logcat-vk-a2.txt`（修后）；配方见 `TOOLS` §G，
    结论见 `ANDROID_PLAN` §八·A1。
- **「N4-f」（2026-10-16，提交 `de08e13`）停在**：**"顶点数据本身"与"常量缓冲"
  都是正常的（N4-f 的前半答完）** ⇒ 只剩"**顶点着色器输出**"（位置经变换后全在裁剪体之外 / 输出 NaN/Inf）。
  改了什么：**回放逻辑一行未改**，只加了一个**缺省关、只读**的诊断开关 `DM_VTX_PROBE=1`（挂在**既有判据行**
  那一点上：前 8 次绘制 + 每 2048 次）—— `nvn_vk.c` 里新增采样函数 + 格式解码/NaN·Inf 判断三个小函数
  + 心跳一格（**那一格只在开关打开时出现** ⇒ 缺省关时心跳与上一轮逐字节相同，可用 `.work/n4e_hb_len.py` 核）。
  真机读数（A 臂 5 轮 + B 臂 2 轮）：**位置 NaN 0 / Inf 0 / 整片全零 0**、**常量 NaN 0 / Inf 0**、
  逐槽 |max| **两臂逐项相同**（槽0 1~1067、槽2 ≈1、**槽3 = 1600 = 屏幕宽**、槽10 410~1070）、
  分量类型与 SPIR-V 声明对上（`f88d27f131d71049` 的 Location 0/1 都是 float×4）；**画面仍是全零**
  （截图轮 `DM_WIN_SRC=28`：781 次读成功、亮度 0/0/0）。
  - 跑法：`.work/n4_apk_round.py --conf .work/n4f-vtx-a.conf --tag n4f-vtx-a --secs 60 --build --install`
    （A 臂 = `DM_GFX2=1` + `DM_VTX_PROBE=1`；B 臂 = `.work/n4f-vtx-b.conf`；截图轮 = `.work/n4f-shot-a.conf`
    + `.work/n4d_shot2.py`）；conf 由 `.work/n4f_confs.py` 生成，多轮用 `.work/n4d_rounds.py`。
  - 读数：`.work/logs/n4-n4f-vtx-{a,a2,a3,a4,a5,b1,b2}.log` + `n4-shot-n4f-shot-a.log`、
    抽表 `.work/n4f_report.py`、截图 `.work/logs/n4b-n4f-shot-a.png`。
  - **下一步 = N4-g**：**先问设备能力** `VK_EXT_transform_feedback`（+ `transformFeedback` / `geometryStreams`），
    有就抓 `gl_Position` **直接回答"经变换后在不在裁剪体里"**；没有就走"离线核对 + 已知色底子上的最小三角形自证"。
- **「N4-c」（2026-10-16，提交 `e01c0ed`）停在**：**接提交侧 + 补 GPU→池的回读，已完成**
  ⇒ **新地基的内存 / 提交 / 可见性三块都在真机上跑起来了**，六个新基线字段**全部有真帧读数**
  （等队列空闲 **0** / 每帧提交 **1.00** / 槽等待率 **0‰** / 每帧脏 run **4.42** / 每帧回读 **≈17 KB** /
  上传 36~52 KB 常态）。改了什么与怎么跑：
  - **提交侧**：`nvn_vk.c` 的 `mtl_sub_begin/commit` 走 `vk2_sub_begin/commit`（`g_inf[i].cb` 只是接进来）、
    `mtl_wait` → `vk2_sub_poll` / `vk2_sub_wait_timeout`（**按客体给的超时**）、`mtl_drain/finish` →
    `vk2_sub_wait_all`；`vk2_sub` 新增 `vk2_sub_set_reap_hook` / `set_queue_lock` / `last_frame` / `poll` /
    `slot_id` / `wait_timeout`。
  - **回读**：`vk2_xfer` 新增 `vk2_readback_addr` / `vk2_gpu_settle`（字节精确 + 只清 GPU 位）、上传侧
    **跳过 GPU 脏页**；`vk2_pool` 新增 `vk2_dirty_any_gpu` / `vk2_gpu_takenover`。
  - **五处 `vkQueueWaitIdle` 收口**：`vk_device.c` 新增 `vk_idle_wait(site)`（计数 + 打点名）与
    `vk_idle_avoided()`；心跳那一行报"等队列空闲 N / 被定向栅栏替代 M"。
  - 跑法同 N4-a：`.work/n4_apk_round.py --conf .work/n4c-a.conf --tag n4c-a --secs 60 --build --install`
    （A 臂 = `DM_GFX2=1`，B 臂 = `.work/n4-b.conf`）；截图 `.work/n4b_shot.py --conf .work/n4c-a.conf
    --tag n4c-a3 --secs 25`。
  - 读数：探针 `.work/logs/probe-8b1f1e50-vk2_takeover_probe.txt`（十一问四轮 + `…-r1/-r2/-r3.txt`）、
    回归 `…-vk2_{pool,xfer,sub}_probe-n4cregress.txt`（比对脚本 `.work/n4c_regress_diff.py`：**0 项不一致**）、
    游戏 `n4-n4c-a.log` / `-a2` / `-a3` / `n4-n4c-b.log`、截图 `n4b-n4c-a1.png` / `n4b-n4c-a3.png`。
  - **没过的判据与下一步**：**M2 的验收「屏幕出（游戏自己的）画面」仍未过**（两臂截图同形："黑底 + 一条暗红带
    + 一个白方块"），而且**两臂本轮都停在 ~100 帧**（A 115/122/96、B 72）⇒ **下一步 = N4-d**：
    先按「M3-3a」的 `atrace` 形状把"那 ~10 秒的驱动级停顿"与"出图停住"**分开**，再回到画面。
  - **本轮修掉一个自己引入的真 bug**：`mtl_wait` 曾把客体的超时换成模块的 5 s 上限（现场读数 5005 ms）。
- **「N4-a」（2026-10-16，提交 `2109af4`）停在**：**N4 的第一步 = 选路点 + 真机池账，已完成**
  ⇒ **新地基的唯一前提在真机游戏路径上成立**。改了什么：`dm/android/vk/nvn_vk.c` 在 `mtl_pool_add/del` 上
  挂 `vk2_boot()`（幂等、`DM_GFX2=1` 才进来）+ `vk2_pool_ledger()`（池账一行，前 12 个报明细）；`vk2_pool.h/.c`
  加**后备预算**（`vk2_pool_set_backing_budget`，0 = 只登记不建后备；缺省不限 ⇒ 不改既有行为）；
  `dmhost_android` 的 include 加 `vk2`。跑法脚本 `.work/n4_apk_round.py`（出包 → 设备端装机 → 推 conf →
  重启 App → 等 N 秒 → 取日志 → 抽账），两臂 conf `.work/n4-a.conf` / `n4-b.conf`。真机读数（各 60 秒）：
  **7 个池 / 1675 MB**（512 / 10 / 400 / 1 / 710 / 2 / 40 MB）、**每个都登记成功**、**`no_region = 0`**、
  `small / pool_fail / dirty_fail` 全 0、后备已建 0 MB（新增 Vulkan 占用 0）；对照臂（`DM_GFX2` 不设）日志里
  **一条新地基行都没有**；两臂旧路池清单与 `【红】/【黄】` 行**逐条相同**（⇒ 零副作用）；两臂都在同一处出现
  旧路已知的"出图停住"（A 臂出图 56、B 臂出图 74 之后冻住）。**GPU 侧仍全走旧路**（`mtl_addr` 未接管）⇒
  本步与画面/崩溃率无关。**回归**：`vk2_pool_probe`（N1/G3）与 `vk2_sub_probe`（N3/G4）各重跑一轮，
  **判据字段逐项相同**；另留了一个仪器现场（`--no-build --no-push` 跑到了旧二进制）。
  **顺手把 §9.7 的接管点表逐行核对了一遍**（4 处行号漂移、1 条"`nvn_dirty.c` 安卓未接"已过时、
  `mtl_addr` 的 8 处调用点、核心层 `vkQueueWaitIdle` 只有 5 处、**往返型在旧路没有对应物**这处设计缺口）——
  更正批注已写进 `rewrite/ANDROID_REWRITE` §9.7。读数 `.work/logs/n4-n4a.log` / `n4-n4b.log`，结论见
  `ANDROID_PLAN` §八·N4-a。**下一步 = N4-b**（`mtl_addr` → `vk2_addr` + 上传窗口接进提交路径）。
- **上一轮（「N3」，2026-10-16，提交 `22ee122`）停在**：**新路径的第四个里程碑 N3 = 帧批提交 + 有界在飞 +
  `vkQueueWaitIdle` 归零，已完成 ⇒ G4 的前两条判定性成立**。新文件 `dm/android/vk2/vk2_sub.c/.h`（§9.6 的
  提交模型：`vk2_slot` = **一槽一池一 cb 一栅栏**（命令池带 `RESET_COMMAND_BUFFER_BIT`、栅栏创建时不带
  `SIGNALED`）；`vk2_sub_begin()` 先轮询 `vkGetFenceStatus` 把已完成的槽收掉（**这不是等待**，单独计
  `poll_reaps`），还是满的才**只等最老那个槽的栅栏**（带超时，超时**如实报失败**）、**绝不等队列空闲**；
  `vk2_sub_commit()` = 收尾 + `vkResetFences` + **一次 `vkQueueSubmit`**；`vk2_sub_note_upload()` + **回收时自动
  `vk2_xfer_window_done()`** 把"槽的存活期 ≥ 环的存活期"这条不变式做实；**`vkQueueWaitIdle` 在这个编译单元里
  宏成「计数 + 报红」**）+ 自证探针 `vk2_sub_probe.c`（六问一答）。第一台**四轮**、**判据字段逐轮完全一致**：
  **G4（一）成立**（连续 24 次空 cb 提交：探针自己数的 24 = 记账 `commits` 24 = `submits` 24、`submit_fail` 0；
  全程 取槽/提交/`vkQueueSubmit`/回收 = **152/152/152/152**）；**`vkQueueWaitIdle` 计数 = 0**；
  **G4（二）"3 还是 4" = 3**（D=1/2/3/4/8 各 24 次提交、每次录 128 MB 的 GPU 拷贝：**D=3 → D=4 的每提交耗时
  变化 -4~+26‰**（四轮全部 < 5%，两轮里 D=4 更慢）；D=1 = **7822~13587 µs** 而 D≥3 稳定在 **4059~4085 µs**
  ⇒ **有界在飞本身买到吞吐**；最长等待 D≥3 = **3.48~3.81 ms ≈ 一条提交自己的 GPU 活**）；
  `vk2_sub_wait_all()` 等的是**所有在飞槽**（不是队列空闲）；**提交槽能被 N2 的搬运窗口复用**（1 MB 上传 →
  回读**逐字节等于独立重算的期望**、回收时自动交还环、交还之后再录一窗被接受）。
  **本块仍只是"让新模块可用"**（回放层三处一行未动、没有任何调用方、**一个游戏轮都没跑**）；
  N2 的探针在 N3 代码上重跑一轮（`…-vk2_xfer_probe-n3regress.txt`）**判据字段逐项相同**。
  读数 `.work/logs/probe-8b1f1e50-vk2_sub_probe.txt`（前三轮 `…-r1/-r2/-r3.txt`；**旧判定规则版四份换名留档
  `…-pre-r1~-pre-r4.txt`**；`…-dry.txt` / `…-dry2.txt` 是探针自己两个坑的现场），结论与两条新事实见
  `ANDROID_PLAN` §八·N3。**顺手量出一条 N4 待办**：**暂存环是单窗口的**（在飞 3 个槽只有 1 个能载上传窗口）
  ⇒ **N4 要按槽分环**。**下一步 = N4**（接管回放层、出画面，顶替 M2 的验收）。
- **上一轮（「N2」，2026-10-16，提交 `04b4dbf`）停在**：**新路径的第三个里程碑 N2 = 脏记账 + 上传/回读窗口 +
  显式屏障，已完成 ⇒ 可见性第一次被真机读数建立起来**。新文件 `dm/android/vk2/vk2_dirty.c/.h`（区间级三态脏记账：
  CPU/GPU 各一张 1 bit 位图、4 KiB 起步 ⇒ 4 GB 池 262144 字节；连续脏页并成 run；**半页竞态只报计数**）+
  `vk2_xfer.c/.h`（上传窗口：逐 run 影子 → **暂存环 16 MiB** → `vkCmdCopyBuffer` → **每个 run 后一条屏障** →
  置 CLEAN；回读窗口：**等定向栅栏** → 拷回 → memcpy，**一处 `vkQueueWaitIdle` 都没有**，而且它在这个编译单元
  里被**宏成"计数 + 报红"**）+ 自证探针 `vk2_xfer_probe.c`。第一台**四轮**、**判据字段逐轮完全一致**：
  **G1 成立**（4/4 档往返闭环逐字节等于**独立重算**的期望：64 KiB / 1 MiB / **16 MiB 正好填满暂存环** /
  **跨块 959.5 MiB 起 1 MiB ⇒ 拆 2 条拷贝 + 2 条屏障**）；**G2 成立**（(a) 提交后**不等待**读影子 = 65536/65536
  **仍是旧值**、(b) **等栅栏成功后仍不回读** = 还是旧值、(c) `vk2_readback` = **新值**；(d) 没指定要等谁 ⇒
  拒绝回读、(e) 等永不 signal 的栅栏 ⇒ 5.0 秒超时**如实报失败**）；脏位图与朴素模型**逐位一致**
  （1048576 页 × 20000 次随机记账）+ run 摊回页集合一致；扫描 空池 21~47 µs / 稀疏 1% 1051~1674 µs、
  10313 条 run / 全脏 1593~1782 µs、**1 条 run**；机制读数：512 MB 真搬 = **32 个窗口 127~153 ms
  （3.3~4.0 MB/ms）**、贴图单段 = 1 条 run、拆 64 处 = 64 条 run；`vkQueueWaitIdle` 计数 **0**。
  **本块仍只是"让新模块可用"**（回放层三处一行未动、没写 `vk2_sub`、**没跑游戏轮**）；N1 的探针在 N2 代码上
  重跑一轮（`…-n2regress.txt`）**全部键值不变**。读数 `.work/logs/probe-8b1f1e50-vk2_xfer_probe.txt`
  （前三轮 `…-r1/-r2/-r3.txt` 留档），结论与"探针自己的两个 bug"见 `ANDROID_PLAN` §八·N2。
  **下一步 = N3**（帧批提交 + 有界在飞 + `vkQueueWaitIdle` 归零，判据 **G4**）。
- **上一轮（「N1」，2026-10-16，提交 `b966494`）停在**：**新路径的第二个里程碑 N1 = 影子内存 + 分块后备资源 +
  地址反查，已完成**。新文件 `dm/android/vk2/vk2_pool.c/.h`（**客体池永远是普通内存**、GPU 用**另一份**按块切分的
  `DEVICE_LOCAL` 后备资源；块大小 **960 MB** = N0 的读数写成代码常量，块数按池大小算，4 GB ⇒ 5 块）+
  自证探针 `dm/android/vk2/vk2_pool_probe.c`（六问一答，接进 `dm/android/CMakeLists.txt` 与 `.work/probes.py` 的
  `ALL`）。第一台**四轮**、**29 个关键字段逐轮完全一致**：**G3 成立** —— 4 GB 池按块建成 5 块（4×960 MB + 末块
  256 MB），每块**头页与尾页都真写一遍并回读**逐字节一致（5/5）；**`g_pool_small = 0`、`g_pool_no_region = 0`**；
  跨块请求长度分布（抽 199998 次）给出块大小的复核依据（**≤64 KiB 那一档跨块率 0**）；开关 `DM_GFX2` **缺省关且
  可读自证**；请求 PINNED 按 §9.8 回落 SHADOW（报计数不报红）。**本块的"接管"= 让新池模块可用**：`nvn_vk.c` /
  `vk_tex.c` / `vk_region.c` **一行未动**，没写 `vk2_dirty` / `vk2_xfer` / `vk2_sub`，**一个游戏轮都没跑**。
  读数 `.work/logs/probe-8b1f1e50-vk2_pool_probe.txt`（前三轮 `…-r1/-r2/-r3.txt` 留档），结论与两条更正见
  `ANDROID_PLAN` §八·N1。**下一步 = N2**（脏记账 + 上传/回读窗口 + 显式屏障，判据 **G1/G2**）。
- **上一轮（「N0」，2026-10-16，提交 `945f7d7`）停在**：**新路径的第一个里程碑 N0 = 常数标定，已完成**。
  新目录 `dm/android/vk2/` 建起来，第一个文件是**自证探针** `vk2_probe.c`（编成独立可执行文件，接进
  `dm/android/CMakeLists.txt` 与 `.work/probes.py` 的 `ALL`），第一台**四轮**（三轮完整、结果一致）把 §9.9 的
  四个常数各量成真机读数：**块大小 960 MB**（纸面上限 = `maxMemoryAllocationSize` 1024 MB，由"1088 MB 失败 /
  1024 MB 成功"证明）、**脏位图粒度 4 KiB 起步**（= 本机页大小）、**`VK2_INFLIGHT` 3 起步**、
  **暂存环 16 MiB 起步**（一池全搬一遍 4096 MB ≈ 160 ms ⇒ 24~26 GB/s）。**只问不接管**：`nvn_vk.c` / `vk_tex.c` /
  `vk_region.c` 一行未动、没写 `vk2_pool/dirty/xfer/sub`、没引入任何开关（`DM_GFX2` 归 N1）。
  读数 `.work/logs/probe-8b1f1e50-vk2_probe.txt`（前三轮 `…-r1/-r2/-r3.txt` 留档），结论与"探针自身的两个错"见
  `ANDROID_PLAN` §八·N0。**（当时列的）下一步 = N1**（影子 + 分块后备 + 地址反查，判据 **G3**；已完成，见上一条）。
- **上一轮（「M3-3f」，2026-10-16）停在**：**主线在第一台**（Adreno），第二台的 AHB 线**暂停**
  （见下一段）。旧口径的两支臂**做完且都是负结果** ⇒ **用法侧四刀全为负**（上传环 / 摘前端 / 上传提交 / 喂空活）
  ⇒ **卡点 B 的触发点不在我们这一侧的用法里**。**方向**：按「七、下一步」第 0 条的**架构口径变更**施工
  （正本 `docs/rewrite/ANDROID_REWRITE.zh-CN.md`）：影子内存 + 分块后备资源 → 显式屏障 + 上传/回读窗口 →
  帧批提交（删掉全部 `vkQueueWaitIdle`）→ 绑定以 SPIR-V 反射为唯一真源；旧路全留（`DM_GFX2`，缺省关）。
  细节见 ANDROID_PLAN「M3-3f」。
- **第二台（Mali）的 AHB 线已暂停**（2026-10-16 用户决定）：M3-3e 净结果 = 装机已验证、AHB 支路两台都成立
  并落地 `vk_region.c`（**默认关闸**）、分级复量把内核 panic 的触发点收窄到"**客体在 AHB 堆上的原子/独占访问**"
  （这块 GPU-only 内存没有原子语义，`LDADDAL` 就让宿主进程 `SIGBUS`）⇒ **AHB 支路不能用来铺客体堆**。
  它**放着不动是安全的**（装的仍是关闸的包；`dmhost` 上新开关全部缺省关）；
  **别在第二台上随手跑任何会走 AHB 支路的动作**（重开条件写在 ANDROID_PLAN「M3-3e」第九节末尾）。
- **M1 完成**：宿主在安卓上装载改写产物、接管服务层、跑完 SDK 初始化、进 `nnMain`。
- **渲染链路的告警已清空**：描述符的空白（UBO 名字号 −3、SSBO 由 `cb_offset` 反推、缓冲区贴图）与顶点属性的那条判据
  （两个独立的错，见「M3-2k」「M3-2l」）。
- **「九」优化清单第 1 项已落地（2026-10-15）**：图形与计算共用一个 `VkPipelineCache`、落盘、下次启动载回
  （「M3-2p」）。真机四轮读数：无缓存→写出 0.6 MB；有缓存→载入 636 KB 并继续追加；坏文件与"假设备号"两份都被挡住。
  **它顺带照出一条真机事实**：MIUI 把这个包当游戏，退到后台直接 SIGKILL —— 想持久化就得**前台定期写**（见下面「坑」）。
- **卡点**：绘制在发（每轮约 40 万次、图形管线 78~84 条、零失败），但**渲染目标读出来是全零**。
  `DM_NVN_SHOT_SCAN` 现在**逐级**读（第 0/1/2 级各一次），在够得着的帧（50/54/57）上**每张都是 `亮度 均值 0.0`**
  （「M3-2m」第六节）。也就是说：**画面还没有被写到能读的地方**，或者"读"这一侧还有没照到的问题。
- **一条常态（口径已更正，见下一条）**：这台机**有时**在出图 50~80 停住（阶段 `CharacterCreate:Hover:Barbarian`，
  偶发 `MainMenuHD:CheckUpdateLinks`），但要按"**多轮里它在干什么**"读 —— 本轮的 t1/t3 两轮**根本没停**：
  出图一路涨到 375 / 379、末条心跳还是"出图在推进"，进程却是被看门狗判死退出的（因为它的进展量的是**引擎拍**，
  而那两轮一直是 `拍 0`）。判"停住"请用**心跳出图数**与前端那条"**连续 8 秒没有新出图**"的卡死报告，
  看门狗转储只当现场照。
- **里程碑 B（任务图侧）已结案（2026-10-15，「M3-2t」）**：形状先由「M3-2r」量出（主线程在 `main+0x15ee…`
  轮询任务完成位图）、「M3-2s」把机器读通（三张位图 `+0x3320` / `+0x3324` / `+0x3328`、"上膛"`main+0x1636f50`、
  唯一的"置位"`main+0x15f0e6c`）；本轮把它做成**时间序列**（宿主 500 微秒采样线程 + SVC 钩子补高分辨率，
  变了才打一行）并真机跑三轮：**甲否掉、乙成立** —— 待派发最大 `0x3fffff`（上膛确实写满低 `count` 位、
  派发器再逐位取走）、已完成最大 `0x3fffff`（22 位全置过很多次，随后被**下一次上膛**清 0）。所以「M3-2r」的
  "22 个任务从未被派发"与「M3-2s」的"三张位图同时为 0"**都不是停住时的状态**（后者只是运行中的一个瞬态），
  `0x1005` 那把锁是"图在跑"的伴随现象。**顺带三条**：① **看门狗的"无进展"量的是引擎拍**（出图只在 `pregame`
  期间算进展），本轮三轮都是"**出图在推进 + 拍 0**"照样判死、转储、`_exit` ⇒ **转储 ≠ 停住**，判据改用
  **心跳里的出图数 + 前端"连续 8 秒没有新出图"**；② 真出现"出图停"的那一轮与**设备丢失**同现（`提交失败
  VkResult -4` + 上百条 `贴图上传失败`，随后 `TERM_WINDOW` 拆窗口、日志截断）；③ 停住现场里主线程的**当前 PC
  落在我们自己的宿主层**（`vk_tex_view` / `vk_tex_grab_ser` / `mtl_wait`），而同期的图**还在变** ⇒ 该查宿主侧。
  读数、反汇编更正与下一步见「M3-2t」。
- **「停住」已能稳定复现，而且它跟我们自己的读回诊断有关（2026-10-15，「M3-2u」）**：A/B/C 三臂只差一个开关 ——
  **查看器（`DM_WIN_SRC=28`）+ 逐级扫描（`DM_NVN_SHOT_SCAN`）同时在路上** ⇒ 约 20 秒内出图冻住（两轮都是）；
  **只有其中一个** ⇒ 60~80 秒干净（21.5 / 14.4 帧/秒）。冻住的现场：出图与绘制全冻、主线程**32 秒不发一次 SVC**
  且卡在宿主侧的锁 / futex（`在用户态空转的线程 0 个` = 大家都在系统调用里等）、工人 `0x1005` 等的那把锁持有人
  是主线程；把停住时的宿主 PC 符号化 ⇒ 读回/建视图是在**客体线程执行命令流（`mtl_exec`）的过程中**跑起来的
  （`nvnth_378` / `vk_tex_view ← mtl_exec ← dm_gfx_suspend`），所以卡的是客体那个线程自己。
  两条口径也改了：**"在飞槽 1/8、提交比出图多 4"不是卡死签名**（没停的那轮读数一样），卡死看"计数冻住 + 主线程
  不再发 SVC"。**复现配方**：`DM_WATCH_STALL=600` + `DM_WIN_SRC=28` + `DM_NVN_SHOT_SCAN=50,54,57:…` +
  `DM_TG_SERIES=0`，冻住后 `adb shell touch …/files/dump.trigger` 按需取现场（进程不退出）。
  下一步是"读回路径的锁账本"。细节见「M3-2u」。
- **账本做出来了，结论是"不是我们自己的锁"（2026-10-15，「M3-2v」）**：停住那一刻 —— **读回锁没人持有、没人等；
  队列锁没人持有；8 个槽全空且驱动侧栅栏全就绪；>300 毫秒的等待一条都没有** ⇒ 不是我们的锁、也不是槽位记账。
  而卡住的主线程（`svc 环` 全是 `SleepThread(-1)`、20~32 秒不发一次 SVC）当时读到的宿主链是
  `nanosleep ← usleep ← … ← nvnSyncWait（hle_nvn.c:383）← mtl_wait ← 客体 main+0x1743a0c` ⇒ 像是"一次宿主侧的轮询式等待"。
  **⚠ 这条链已被「M3-2w」推翻**（帧链在驱动帧上失真 + 返回地址带 PAC），见下一条。
  顺带修好一件仪器：停住采样器原来只认"状态 R 且 syscall 报 running"，把轮询式等待在**两次半拍上都漏掉**
  （睡的半拍是 `S`、看的半拍是 `R`），五次转储一个 PC 都没取到；现在 `syscall = nanosleep` 也采。
  细节见「M3-2v」。
- **等待站点账本 + 栈扫描（2026-10-15，「M3-2w」）**：宿主里**所有**可能落在客体线程上的
  `usleep` / `nanosleep` 点（`mtl_wait`、`nvnSyncWait`、`SleepThread`、`gfx_enter`、队列 drain/finish、读回/对照/上传、
  等"读取画面已撤"、以及**对驱动的那些调用**）现在都记"进入 / 离开 + 站点名 + 进入时刻"，>300 毫秒自动打一行，
  转储里列"当前打开的站点"并带**自检**。六轮真机（A 配置，六轮都冻）读数：
  ① **停住时主线程（0x1001）永远"0 层"** —— 累计进过两万多次账本、卡住那一刻**不在任何站点上**；它"最后等完的
  站点"是**「客体：出图（`nvnQueuePresentTexture`）」**（其中一轮那一次在里面待了 **11.1 秒**），出来后 **6.9 秒
  没再进任何站点**。② **「M3-2v」那条链作废**：`mtl_wait+0x214`（`0x8d63c`）反汇编是"已滑出在飞窗口 → `return 1`"
  这条**不等待**分支的返回地址；而 Android arm64 返回地址带 **PAC**（签名位在高 16 位，`dladdr` 全失败 ⇒ 回溯里那串
  `0x24c6fca47ce57c` 形状的"垃圾地址"就是它）。③ 新仪器 **栈扫描**：采样处理器里从 `sp` 起拷 **2 KB 自己的栈**
  （安全）；别的线程读 `/proc/self/task/<tid>/mem`（**要在发采样信号之前读**：信号一进去 `syscall` 就变 `running`、
  没有 `sp` 字段），并按 `/proc/self/maps` **只留可执行段**的地址（栈上数据指针多得能把要看的那几帧淹掉）。
  同一轮里它把看门狗 / 音频线程的栈解对了（`watchdog_main` / `null_sink`），**主线程那一份里没有任何 `libvulkan`
  地址、也没有 libc 帧** ⇒ 它那一刻不在 `usleep`，而在"睡一下、看一眼"之间来回。
  下一步：读新转储里 **"栈上的返回地址 thr=0x1001 …"** 那一行点名。细节见「M3-2w」。
- **点名做完了：停住 = 驱动约 10 秒不回答（2026-10-15，「M3-2x」真机五轮）**。第一步先照出**仪器自己的假阴性**：
  栈扫描按 `/proc/self/maps` 过滤的那张表只有 **64** 个槽，而真机进程里有 **455~460 个可执行段** —— 留下的是
  `libdm.so`（第 24）与驱动 `vulkan.adreno.so`（第 43），**丢掉的是 `libc.so`（第 403）与 `libvulkan.so`（第 404）**；
  `usleep` 就在 libc 里，所以「M3-2w」读到的"栈里没有 libc 帧 ⇒ 不在 `usleep`"**是假阴性**。表已放宽到 1024，
  且**表满时读数里会明说**。修后五轮的读数：**`pc=nanosleep+0xc lr=usleep+0x54` 只出现在我们自己的宿主线程上**
  （看门狗 `watchdog_main`、音频 `null_sink+0x88`）；客体线程的 `nanosleep` 都是 `SleepThread`；而**主线程停住时
  等的是驱动** —— 五轮分别是「读回：等队列空闲」10519 ms、「一次性提交等空闲」10051 ms、「vk：建图形管线（驱动内部
  编译）」10790 ms、「vk：贴图内存分配」10268 ms，同刻**前端线程**也在「上屏：等上一帧的栅栏」等 10074 ms，
  心跳整段断约 11 秒后**自愈** ⇒ **"停住"是"驱动约 10 秒不回答"（会重复出现），不是永久挂死**；
  这五轮 `VkResult -4` 全为 **0 条**（那 10 秒停顿不需要设备丢失就能出现）。同时刻的栈落在 **`mtl_exec`（执行客体
  命令流）中途**（`tex_of+0x580`＝`bl one_end_on` 的返回点 `← vk_tex_view ← mtl_exec`，pc 在 libc 的 syscall 桩上，
  `lr=ioctl+0xa0`）⇒ **读回是在命令流的中途要求整个队列空**。**下一轮**（先量后改）：① 给**驱动调用**加判活 / 上限
  （不是给 `usleep` 加）；② 把读回挪到**帧边界**或改成"只等这一小段拷贝的栅栏"；③ 用修好的仪器把「M3-2u」的
  A/B/C 三臂再量一遍，看单开时那 ~10 秒停顿是否消失。细节见「M3-2x」。
- **复量做完了，结论反过来（2026-10-15，「M3-2y」真机十轮）**：A/B/C/**D（两路诊断全关）**四臂各两轮，
  **那 ~10 秒的停顿轮轮都有（连 D 也有）**；而同一份 A 配置也出现过 **13.5 帧/秒、零长等**的一轮
  ⇒ **"停住"不是诊断造成的**，是**每次启动落在"健康 / 生病"两种状态之一**：健康的 869 帧 / 64 秒、
  绘制 ≈**1234 op/帧**、图形管线 86、**一条 >1 秒的等待都没有**；生病的 52~92 帧 / 64 秒、绘制 ≈**100 op/帧**、
  图形管线 78~80、每条 2~11.6 秒的停顿，并常收尾于 `VkResult -4`。**同一份配置连着两轮可以一次健康一次生病**
  ⇒ 不是配置问题。**下一步先问"启动时那一步为什么有时走不过去"**（生病那轮卡在更早更轻的场景里），
  在它量清之前不要去给"停住"加护栏。**设备丢失**：新日志（`vk_tex.c` 的 `step_fail`）第一次说出"谁先看见" ——
  **`vkQueueWaitIdle`（"等队列空闲"）**，其后上传都在 `vkQueueSubmit` 上返回 -4（-4 是粘的）；要答"谁弄丢的"
  还差一件仪器（把"最后一次成功的提交"与"第一条 -4"之间那一小段留在日志里）。
  **量帧预算前先判这一轮是健康还是生病**（判据：op/帧 与有无 >1 秒的等待），否则读数能差 13 倍。细节见「M3-2y」。
- **那 ~10 秒的停摆是"我们引起的"，而且是系统级的（2026-10-15，「M3-2z」）**：病态时同一时刻 **SystemUI 也卡了
  10 秒**（`Davey! duration=10018ms`、`waitForever: fence(kgsl-timeline-kgsl-3d0_7-ndroid)`）、**composer HAL 的
  调用 9.49 秒**（`IComposerClient code=40`）、`Slow swap buffers`；而**把 app 停掉、清 logcat、等 60 秒 ⇒ 一条都没有**
  ⇒ 那 ~10 秒是**我们让 GPU 挂住、kgsl 约 10 秒后才判定/恢复**（期间所有走 GPU/显示的东西一起等），有时以
  `VkResult -4` 收尾。两条边界也要记住：① "装包后第一次启动就健康"**不成立**（见 `probe-H1`）；
  ② 中途那条用 `vk_probe`（离屏最小 Vulkan，0.1 秒就完）推出的"设备对别人是好的、病在我们进程"**已收回**
  （离屏、64 KB，很可能撞在两次停顿之间）。**下一步**：给 `one_end_on` 加"读回 / 上传"标签（只动日志，先把
  arm D 里那 10 秒的归属分出来）、在停顿窗口抓 ftrace/perfetto、换设备/驱动对照。细节见「M3-2z」。
- **归属分完了，而且那 ~10 秒的形状查清了（2026-10-15，「M3-3a」）**：给 `one_end_on` 加"读回 / 对照"与"上传"两个标签（**只动日志**）之后 ——
  **arm D（一次读回都没有）里那 10 秒确定是「上传」**（10005 / 10262 / 11993 ms），但 **arm A 有一轮是「读回：等队列空闲」10800 ms** ⇒ **两条路都会中，
  "读回是罪魁"作罢**。**换仪器**：`/system/bin/atrace` 在 shell 下**能吐文本 ftrace**（`--async_start -b 32768 -c sched freq sync` → `--async_stop -o 文件`；
  带 `prev_state`、`sched_blocked_reason` 的 `caller=`、`cpu_frequency`、`clk_set_rate`，还有一条能把 trace 时钟对到墙钟的 `realtime_ts=`），
  而 `tracefs` 的 `enable` 与 `/sys/class/kgsl/*` 对 shell 仍是 `Permission denied` —— shell 在 `readtracefs` 组里，**atrace 这条路能走通**。
  在停顿窗口抓到 171 MB 原文（扳机＝"最近 4 条心跳出图数相同且 >0"，配方已落成 `.work\trace\stall-ftrace.ps1`），四条读数：
  ① 我方两个线程各有 ~10 秒的离 CPU 间隔、**同一瞬间结束**（前端 tid 19581 = 「上屏：等上一帧的栅栏」；客体 tid 19592 = 「一次性提交等空闲（上传）」）；
  ② **整条 GPU/显示通路一起睡又一起醒**（`kgsl-events` 10.035 s、`kgsl_hwsched`、`gmu_f2h`、`crtc_commit:205`、`crtc_event:205`、`mFrameBuffer`、
  `DISP_EVENT_SUBJ`、SystemUI 的 `SystemUIBg-*`，结束时刻都落在同一 20 毫秒内）；
  ③ **全是 S 态，全 trace 没有一条 >0.5 秒的 D 态阻塞**（唯一 D 在起点那一瞬），CPU 大部分 idle、**频率全程 556 MHz~1.99 GHz 正常来回** ⇒ 不是热 / 省电 / 降频 / 我方烧 CPU，
  也不是"内核不可中断地挂住"；④ **恢复瞬间 = `gmu_f2h`（GMU 固件→主机中断）→ `kgsl_hwsched` → 整套 GPU/GMU 时钟与总线重编程**
  （`clk_set_rate: gpu_cc_pll0/gmu_clk_src/gx_gmu_clk/cx_gmu_clk`、第二次恢复里 `icc_set_bw`）⇒ 形状是"**GPU/GMU 掉了约 10 秒，回来时重编程**"，不是某个进程自卡。
  **唯一"像原因"的东西**：我方在 **启动那 4.8 秒**里于驱动中做成了 **`rpmh_write_batch` 441 次、`rpm_resume` 93 次、`gpu_cc_cx_smmu_gdsc` 13 次、`icc_set_bw` 6 次**，
  外加 `kgsl_iopgtbl_map`（每次分配都做 IOMMU 映射）—— 也就是"**每次上传都新建 `VkBuffer`/`VkDeviceMemory`/视图**"（「九」第 5 项）在驱动里变成成百次电源 / 总线投票；
  系统侧同一套 caller 是 `gmu_core_timed_poll_check` / `adreno_hwsched_wait_ack_completion` / `gen8_hfi_send_cmd_wait_inline`（等 GMU 固件回 ack）。
  ⇒ **下一轮的第一刀改成「九」第 5 项（贴图上传改常驻上传环，加 `DM_UP_RING` 做成 A/B 第五臂）**。细节（含 perfetto 的负面结论）见「M3-3a」。
- **第一刀砍了：常驻上传环做出来了，但那 ~10 秒照旧（2026-10-15，「M3-3b」）**：`DM_UP_RING=1`（**缺省关**）= 常驻、只映射一次的中转缓冲 + 512 字节对齐的环内偏移
  （`up_ring_on / up_ring_make / up_acq / up_rel`，`ent_upload` 里那段"建中转缓冲 + 分配 + 映射"整段换掉）。**先自证**：每轮都看到 `【绿】上传环：常驻中转缓冲就绪 16777216 字节`，
  一轮**只借 19 段、回绕 1 次、建/换 2 次** ⇒ 一轮整轮只有约 19 次上传（这也把「M3-3a」里"441 次电源投票来自上传"那条**量级**纠正掉）。
  **读数**：E 臂（arm D + 环）6 次启动 = **3 次病（10.2 / 11.9 / 10.7 秒，落点还是「一次性提交等空闲（上传）」）**、2 次健康（964 / 967 帧、13.9 帧/秒）、1 次启动崩溃；
  同构建环关对照里那 10 秒也在 ⇒ **"每趟上传新建对象"不是那 10 秒的原因**。**帧率上也看不出收益**（健康轮 964 / 967 vs 老路 964~976）⇒ 开关**保持缺省关**。
  **正面读数**：环开着时再抓一次 ftrace，用**同一把尺子**（应用出现后头 10.4 秒）比 —— 我方线程在驱动里的电源 / 总线投票**少约 3 倍**
  （`rpmh_write_batch` 442→159 次、`rpm_resume` 93→27、`gpu_cc_cx_smmu_gdsc` 13→0），但**没削干净**，而那 10 秒照旧 ⇒ "投票变少"与"停摆消失"是两件事。
  **顺带一条新边界**：D6 r1 是**设备丢失但没有那 10 秒**（131 条上传失败、最长等待 2.1 秒）⇒ 两个现象**可分**。
  **样本纪律**：本轮 12 次启动里 3 次撞上已知的启动崩溃（`CRASH sig=11 … pc=libc.so+0xd6b18`，E/D 两臂都有）——**排轮次时剔掉崩溃轮**。细节见「M3-3b」。
- **前端那条上屏路被排除了；这台机上 dmesg 正式封死（2026-10-15，「M3-3c」）**：新加取证开关 `DM_WIN_NOSUB=1`（**缺省关**）：`vk_win_present_rgba` 在"映射 + 拷贝 + 解映射"之后
  直接返回 ⇒ **不**等栅栏、**不**取交换链图、**不**提交、**不**呈现（交换链本身照旧建）。F 臂 4 次启动里**2 次仍有 10~12 秒的停顿**（12109 / 10985 ms，落点全在客体侧
  「客体：排队提交命令」+「一次性提交等空闲（上传）」）⇒ **触发点在客体侧的提交路或驱动 / GMU 自己**。**读法要改**：F 这种臂里心跳不再断（前端不等栅栏了），
  判据得换成**客体侧的长等**（`waits_ge1s` / `wait_sites`）；自证是"上屏等栅栏"这个站点整轮一次都不出现。
  **两条"到头了"**：① **没有 root / su**（`adb root` → production 构建拒、`su` 不存在、`dmesg` → `klogctl: Permission denied`）⇒ **dmesg 这条路封死**；
  ② 170 MB 原文再挖一层 ⇒ 停摆前 `kgsl_hwsched` 最后一次阻塞是 **`adreno_hwsched_work+0x37c`（正常等新活，不是在等 GMU）**，我方线程最后一批阻塞**全是读文件**
  （`folio_wait_bit_common`）⇒ **提交早已发出、只是没人执行**（"GPU 停了"而不是"我们卡了"）；系统侧只有 `gmu_core_timed_poll_check` / `gen8_hfi_send_cmd_wait_inline`
  这些**驱动与 GMU 固件之间**的等待，我们看不见也改不了。细节见「M3-3c」。
- **一条偶发**：约每轮 1 次 `【红】提交失败 VkResult -4`（`VK_ERROR_DEVICE_LOST`），后面跟上百条
  `贴图上传失败`。**与共用命令缓冲无关**（已排除，「M3-2o」）；「M3-2t」的 t2 里它之后窗口被拆、日志截断 ⇒
  与"停住"很可能是同一条线。
  **2026-10-16「M3-3d」**：这件仪器补上了（`dm/android/vk/vk_ops.c` 的 24 笔 GPU 操作流水）——**第一条 -4** 现在自带
  "出错的调用 + 设备信息 + 前 24 笔前序操作"，并进所有现场转储。真机两次读数**更正**了旧口径："谁先看见"**不止一个**
  （一次 `vkQueueWaitIdle（一次性提交等空闲）`、一次 **`vkQueueSubmit（客体：命令缓冲）`**）。
- **换设备对照（2026-10-16，「M3-3d」）= 卡点 B 的读数没拿到**：第二台是 **Mali**，① MIUI 不许 `adb install`
  （要人工开"通过 USB 安装"）；② 零拷贝内存池的甲案（**可导出**外部句柄 → `mmap(MAP_FIXED)`）在 Mali 上不存在
  ⇒ `dmhost` 冒烟在 `nnosInitialize` 就 `Break#1`。**第一台的干净臂 12 轮**：**健康 1 / 停顿型 7 / 设备丢失型 2 / 崩溃 2**
  （健康那一轮 1154 帧 / 70 秒、op/帧 1276.5、零长等；生病轮 op/帧 96~107、每轮 2~4 条 10.1~11.8 秒长等；崩溃签名
  `pc=libc.so+0xd6b18`，2/12）。
  ⇒ 顺带更正一条口径：**"不写 `DM_WIN_SRC`"不等于关查看器**（缺省 28 就是开着），干净臂必须显式 `DM_WIN_SRC=0`。

### 下一步（细节在 ANDROID_PLAN「七、下一步」）

> **新路径（Vulkan 原生重写）现在停在「N4-h」**：`N0 ~ N4-g` 已完成，台账里的 **A1（真机装 Vulkan 校验层）
> 也已完成**（它照出 16 种 VUID、修掉一条真误用，并给出一条决定性的负结果：**我们的 draw 在 API 层面合法**）。
> **"顶点输出"这一层已被 N4-g 用两刀排掉**（`DM_CLIPDIST` 特性开关 + **我们自己的三角形**的 A/B；
> 顺带证明**我们自己的三角形能在这一层落屏** ⇒ 光栅化是通的）。
> **下一轮第一件事 = 把"我们自己的三角形"画进游戏的通道里**（`DM_TRI_IN_GAME=<绘制序号>`，缺省关）：
> 它落屏 ⇒ **通道与状态在那一刻是好的、只有游戏的绘制产不出片元**；它也不落屏 ⇒ **那一层本身有问题**。
> ⚠ 另一条要记住的路线判定：`VK_EXT_transform_feedback` **能力在**（`transformFeedback = 1`、
> `geometryStreams = 1`），但离线扫 6923 份 `.spv` **0 份**带 `XfbBuffer`/`Offset` 装饰 ⇒ 要用它抓游戏的
> `gl_Position` **必须给游戏那份 SPIR-V 加装饰**，与"不许改产物"冲突 ⇒ **要重开先单独说清**。
> 细节见上面的"最新提交"段与 `ANDROID_PLAN`「七、下一步」第 0 条的最后两条回勾。
> **2026-10-16 现状更新：换设备对照做过了（「M3-3d」），结论是"卡点 B 的读数没拿到"** ——
> 第二台不是"又一台骁龙"，而是 **Redmi `pearl` / MediaTek MT6896 / **Mali-G610 MC6** / Android 15 / Vulkan 1.1.177**。
> 它在卡点 B 之前连撞两关：① **装机**（`adb install` / `pm install` 被 MIUI 拦：`INSTALL_FAILED_USER_RESTRICTED`；
> `settings put` 被 `WRITE_SECURE_SETTINGS` 拒）—— **这条当天已由用户解除**（开关已开，**待下一轮用
> `.work/mkapk.py --skip-build --install VC9TGM45MNJB8TSG` 验证**）；② `dmhost` 冒烟跑到 `nnosInitialize` 就报 **`内存换轨：建载体缓冲失败`**
> —— 堆与别名区各只换到 0 MB，随后 `SVC 越出 alias 区` + `Break#1`。原因是 `vk_region.c` 的甲案依赖
> **可导出**的外部句柄（不透明 fd / dma-buf），而 Mali 上两样都不可导出（dma-buf 只有 IMPORTABLE）。
> 设备指纹与 4 份探针读数在 `.work/logs/devfp-*` / `probe-*`，结论与下一步见 `ANDROID_PLAN`「八、M3-3d」。
> **尺子已经搭好，两台共用**：`.work/ab-clean.conf`（**注意：干净的臂必须显式写 `DM_WIN_SRC=0` —— 查看器缺省就是 28、开着**）
> + `.work/ab_run.py`（推 conf → force-stop → `am start` → 等 N 秒 → 拉日志 → 判类；读数落 `.work/logs/ab-readings.tsv`）。
> 第二台的设备侧数据**已备齐**（`/data/local/tmp/dm/` 9.1 GB + 存档 + 4 个探针），一旦上面两条清了就能直接开跑。
> 另外 `.work/trace/` 仍不在，`stall.pftrace` 那条路要按配方重抓。

1. ~~**里程碑 B：客体停住**（任务图侧）~~ —— **已结案（2026-10-15，「M3-2t」）**：三张位图的时间序列做完，
   真机三轮把甲（上膛 0 位）/ 乙（完成后被清）分开：**甲否掉、乙成立**（待派发最大 `0x3fffff`、已完成最大
   `0x3fffff` 后被下一次上膛清 0）。任务图这套机制是好的，卡点不在这里。
2. ~~**把宿主侧"落在客体线程上的等待"全部点名**~~ —— **已结案（2026-10-15，「M3-2x」真机五轮）**。结论：
   **不是我们的 `usleep`、也不是客体的 `SleepThread`**（宿主里唯一可能落在客体线程上的 `usleep`「等『读取画面已撤』」
   是记账的，停住时它没开；`pc=nanosleep+0xc lr=usleep+0x54` 只出现在**看门狗 `watchdog_main` / 音频 `null_sink`**
   这两条我们自己的宿主线程上）。**主线程停住时等的是驱动**：五轮分别是「读回：等队列空闲」10519 ms、
   「一次性提交等空闲」10051 ms、「vk：建图形管线（驱动内部编译）」10790 ms、「vk：贴图内存分配」10268 ms，
   同刻**前端线程**也在「上屏：等上一帧的栅栏」等 10074 ms，心跳整段断约 11 秒后**自愈**
   ⇒ **"停住"= 驱动约 10 秒不回答（会重复出现），不是永久挂死**；这五轮 `VkResult -4` 全为 0 条
   ⇒ 那 10 秒停顿**不需要**设备丢失就能出现。**下一步（先量后改）**：① 给**驱动调用**加判活 / 上限（不是给 `usleep`
   加）；② 把诊断读回从"在 `mtl_exec` 中途要求整个队列空"改成**帧边界**或"只等这一小段拷贝的栅栏"；
   ③ 用修好的仪器把「M3-2u」的 A/B/C 三臂**再量一遍**，看单开时那 ~10 秒停顿是否消失。细节见「M3-2x」。
   ⇒ **③ 已做完（「M3-2y」十轮），把 ①② 的前提改了**：A/B/C/D（含两路全关）八轮**轮轮都有**那 ~10 秒停顿，
   而同一份 A 配置也有一轮 13.5 帧/秒、零长等 ⇒ 停顿不是诊断造成的，是**每次启动落在"健康 / 生病"两种状态之一**
   （op/帧 1234 vs 100）。**所以下一步的顺序换过来**：先问"**启动时那一步为什么有时走不过去**"（生病那轮卡在
   更早更轻的场景里），在它量清之前不去给"停住"加护栏；**设备丢失**已由新日志（`vk_tex.c` 的 `step_fail`）
   说出"谁先看见" = **`vkQueueWaitIdle`（"等队列空闲"）**，其后上传都在 `vkQueueSubmit` 上返回 -4（-4 是粘的），
   要答"谁弄丢的"还差"最后一次成功的提交 ↔ 第一条 -4"之间那一小段留在日志里。细节见「M3-2y」。
  ⇒ **「M3-3a」把 ①② 做完了（③ 换设备没做：本机只有一台）**：① 标签加上后，**arm D 里那 10 秒是「上传」**，而 **arm A 一轮是「读回：等队列空闲」10800 ms**
  ⇒ **两条路都会中**；② ftrace 抓到了（走 **atrace 文本**，不是 perfetto —— 见下面「坑」里三条 perfetto 约束），读数见「现在停在哪」最后一条。
  **下一步的第一刀因此换成「九」第 5 项（贴图上传改常驻上传环）**，理由见那条 bullet 的末尾。细节见「M3-3a」。
  ⇒ **「M3-3b」把这一刀砍了：结论是否定的** —— 常驻上传环（`DM_UP_RING`，缺省关）做出来并跑过 6 次启动（3 病 / 2 健康 / 1 崩溃），
  **那 ~10 秒照旧**；唯一量得到的收益是"驱动级电源 / 总线投票少约 3 倍"，**帧率上看不出来**（健康轮 964 / 967 vs 老路 964~976）⇒ 开关缺省关、留着复验。
  **所以「七、下一步」的排序又变回**：① **换设备 / 换驱动**（分辨力最强，本机只有一台所以一直没做）；② 有 `trace_processor` 的机器读 `.work\trace\stall.pftrace`；
  ③ 用法侧只剩两个便宜的候选（"上传只等这一段的栅栏" vs "把上传整个砍掉的破坏性取证"）。细节见「M3-3b」。
  ⇒ **「M3-3c」做了最便宜的那一刀并把前端排除掉**（`DM_WIN_NOSUB=1`：上屏路上的 GPU 调用全摘掉，那 10~12 秒照旧）⇒ 触发点在**客体侧的提交路或驱动 / GMU 自己**；
  同时**这台机的 dmesg 正式封死**（无 root / su）。所以「七、下一步」现在只剩两条：① **换设备 / 换驱动**；② **有 `trace_processor` 的机器读 `.work\trace\stall.pftrace`**。
  若还要在这台机上做，下一刀建议是 **"GPU 保活"**（隔 N 毫秒发一次空提交）——依据是"**提交在飞、GPU 却在等新活**"这个自相矛盾的形状。细节见「M3-3c」。
3. ~~**「九」优化清单第 2 项**（零拷贝内存类型优先 `HOST_CACHED`）~~ —— **已取证、已否掉（2026-10-15，「M3-2q」）**：
   类型 6 **能**导出 fd（256 MB / 512 MB / 3968 MB 都过，`mmap` 到指定地址、双向也通），但要**越过缓冲声明的可用
   类型位**（外部缓冲是 `0x13`，里面没有类型 6 —— 属规范外用法、靠驱动宽容），而 A/B 里 CPU 时间**没有可测差异**
   （同峰配对 <3%，轮间波动 ±100%）：收益看不到，风险就别付，**维持类型 4**；`DM_ZC_CACHED=1` 开关留着（默认关），
   换设备 / 换驱动时一条命令可复验。（第 1 项已落地，见「M3-2p」；"启动到 `nnMain` 的时间"那条 A/B 等第 1 条有进展再补。）
4. **外部规划文档带来的新增动作**（2026-10-16 加，来源 `docs/d2r-mobile-android-porting-plan.md`）——
   逐条对照后，把"本项目还没做 / 还没验"的动作落成台账：**`docs/ANDROID_PROGRESS.zh-CN.md` §14**（未做 16 条 / 已满足 1 条）。
   与本节排序直接相关的四条：
   - **卡点 A（画面全零）侧：真机装 Vulkan Validation Layers** —— 真机现在没有校验层，而 `vk_pipe.c:582-586` 记的
     那次"绑定号 off-by-16 ⇒ 几何塌陷 ⇒ 348k 绘制一个像素不落、零报错"就是校验层一眼能喊出来的形状；
     **不依赖换设备**，可与上面第 1 条并行先做。
   - **设备丢失**：写下 `-4` 的失败路径（哪些情形重建设备、哪些必须终止本轮渲染流程），并补"最后一次成功提交 ↔
     第一条 -4"的仪器（规格见 plan §10.1：出错的 Vulkan 调用 / 设备信息 / **前序 GPU 操作**）。
   - **那条 25% 复现的启动崩溃单独立案**（`CRASH sig=11 … pc=libc.so+0xd6b18`，12 次里 3 次），
     并与 M1 欠账（重点：`crash.c` 的 `is_write` 恒为 0）合查；可用 plan §5.1 提的 `lldb-server` + `adb forward`。
   - **M3 生命周期先写"顺序"**（Surface 销毁 → 重建 → 重新 `vk_device_set_window()`，与"窗口与游戏合用一个设备"同序）
     再动 `front_run`；退出路径的线程职责一并定。

### 怎么跑一轮（新机器上的最小闭环）

PC 侧的路径与工具见第二、三节；这里只说**跑一次并拿到读数**：

**2026-10-16 起有现成脚本**（「M3-3d」，都用 `.venv` Python 跑）：

| 脚本 | 干什么 |
|---|---|
| `.work/ab_run.py` | 跑 N 轮并判类：推 conf → `am force-stop` → `am start` → 等 N 秒 → 拉 `latest.log` → 出"出图 / op 每帧 / >1 秒长等 / `-4` 条数"并判成**健康 / 停顿型 / 设备丢失型 / 崩溃轮 / 启动受阻**；读数追加 `.work/logs/ab-readings.tsv`，逐轮日志 `.work/logs/ab-<tag>-<运行编号>-r<N>.log` |
| `.work/ab-clean.conf` | **最干净的臂**：`DM_WIN_SRC=0`（查看器**显式**关）+ 不写扫描 / HUD / 看门狗 + `DM_TG_SERIES=0`。两台共用同一份 |
| `.work/mkapk.py` | 出包（经 `.work/build_u8.cmd` 打 `chcp 65001`）+ 装机（先试设备端 `pm install -r`，MIUI 那个坑） |
| `.work/probes.py` | 编 / 推 / 跑能力探针（`build` / `push` / `run`，带 `--env` / `--tag` / `--serial`；`ALL` 里有 `vk_probe` / `vk_hostmem_probe` / `vk_fdmap_probe` / `vk_region_probe` / `vk_ahb_probe` / `vk_shader_probe` / `vk_pipe_probe` / `t_digest` / **`vk2_probe`** / **`vk2_pool_probe`** / **`vk2_xfer_probe`**）。**2026-10-16（N1）在本机重建过**：`.work` 不进仓库，换机器就缺；读数落 `.work/logs/probe-<串号>-<探针名>[-tag].txt` |
| `.work/vk2_pool_rounds.py` | **N1 起**：把某个探针跑 N 轮（第 1~N−1 轮带 tag 留档、**最后一轮不带 tag = 正式读数**）并打一张"字段 × 轮次"的对照表 ⇒ 一眼看出哪个字段轮间不一致（N1 四轮 29 个字段全一致；N2 判据字段全一致、只有六个时间字段抖动）。**N2 起带 `--probe` 参数**：`--probe vk2_xfer_probe` |
| `.work/push_dev2.py` | 把设备侧数据推齐（逐文件 + 跳过已存在 + 重试；`spv` 走 tar） |
| `.work/bg.py` | 把长任务放后台并把输出落盘（前台一条命令会堵住整个回合） |
| `.work/n4_apk_round.py` | **N4 起**：一轮真机跑法的固定闭环（出包 → 设备端装机 → 推 conf → `force-stop` → `am start` → 等 N 秒 → 拉 `latest.log` → 抽账），读数落 `.work/logs/n4-<tag>.log`（不覆盖） |
| `.work/n4d_rounds.py` | **N4-d 起**：顺序跑一批轮（第一轮带 `--build --install`），末尾打跨轮小结（出图峰值 / 最大心跳空隙 / 停住 / 判据行 / 等待站点 / 红行） |
| `.work/n4d_shot2.py` | **N4-d 第二部分起**：一轮 = 推 conf → 重启 → 等 N 秒 → **截屏** + 完整日志，并把三件自证一起打出来（①启动头 `DM_WIN_SRC` ②**查看器亮度读数** ③心跳）—— **判"屏幕上是不是游戏画面"的专用工具**（`DM_WIN_SRC=0` 时屏幕上是 `paint()` 测试图） |
| `.work/n4d_view_rounds.py` / `.work/n4d_view0_rounds.py` | 把"查看器打开"的 A′/B′ 各两轮（第 1 级）/ 各一轮（第 0 级）跑完并打跨轮小结 |
| `.work/n4d_view_summary.py` | 把上面几轮的日志抽成一张表（出图峰值 / 心跳空隙 / 有目标 · 无目标 / 读成 · 跳过 / 亮度唯一值 / 红黄行数） |
| `.work/n4d_atrace2.py` | **N4-d 第二部分起**：`atrace` 抓停顿（判到停顿后**再等 `--hold-s` 秒**才 stop，躲开停止动作的边界效应；`--want` 次；顺手落 `/proc/<pid>/task/*/comm` 的 tid 映射）。产物 `.work/trace/at_stall-<tag>-<k>.txt` + `tids-<tag>-<k>.txt` |
| `.work/n4d_atrace_report3.py` / `.work/n4d_atrace_focus.py` / `.work/n4d_waker.py` | 读 atrace：第 3 版按"**持续抬升**"找停顿点（不是第一个/最后一个突变）并给逐线程表；`focus` 按**三本账**（换出 `prev_state` / 被唤醒 / 阻塞原因）算"谁在等谁"；`waker` 问"给定 tid **谁在唤醒它**"（跨所有进程，会露出 `<idle>` 那一档） |
| `.work/poll.py` | 看 `.work/logs/bg-<tag>.txt` 的尾部。⚠ **它显示时会把每行截短** ⇒ **不能用来判"某行有没有被截断"**（要读落盘日志量字节，见 `.work/n4e_hb_len.py`） |
| `.work/n4e_scan_report.py` | **N4-e 起**：把一轮日志里的"扫描"读数抽成表 —— 头部（已知贴图数 / ≥256×256 数）、逐张逐级的亮度、按"有内容"分档，还列出**读失败的是哪些序号哪一级**、以及用前后时间戳夹出"扫描耗时" |
| `.work/n4e_chk_sw.py` | **N4-e 起**：开关自证核对（把每轮的启动头 `配置：…` 与开关自己打的 `【查】…` 打出来）。**判据挂在某个开关上时先跑它**：`"读数没变"` 有"开关没生效"与"生效了但没用"两种 |
| `.work/n4e_hb_len.py` | **N4-e 起**：量每一轮"心跳那一行"的字符数 / UTF-8 字节数 / 尾巴，判它有没有被截断（`.work/poll.py` 会截行，看它判不出来） |
| `.work/n4e_driver2.py` | **N4-e 起**：**通用**顺序跑轮器（`tag=conf …`，跑完自动出扫描总表）；`n4e_driver.py` 是写死四轮的那一个 |
| `.work/n4e_build.py` | **N4-e 起**：**只出包、不碰设备**（有别的东西在跑时不能换 APK，否则后面几轮的读数作废） |
| `.work/n4f_confs.py` | **N4-f 起**：写那一轮的 conf（A 臂 = `DM_GFX2=1` + `DM_VTX_PROBE=1`、B 臂 = 旧路对照、截图轮 = `DM_WIN_SRC=28`）—— 都与 N4-c 的 A/B 臂 conf **逐字相同、只加 `DM_VTX_PROBE=1`**，这样"这一轮的形态是不是 N4-c 那个"可以直接对着看 |
| `.work/n4f_report.py` | **N4-f 起**：抽表 —— 每轮的 `【顶点采样】` 行（格式→分量数、步长、偏移、形状分布、前几个顶点）、心跳那一格的累计、逐槽 `|max|`。⚠ **正则要跟紧读数格式**（那一轮的采样行格式中途改过一次：从"只查第一个常量槽"改成"扫全部槽"） |
| `.work/n4f_tsv_append.py` | **N4-f 起**：把 N4-f 行追加到 `.work/logs/vk2-readings.tsv`（UTF-8 无 BOM、行尾 LF） |
| `.work/vallayer_probe.py` | **A1 起**：装层之前的**能力调研**（`debug.vulkan.layers` 能否设、app 有没有 `DEBUGGABLE`、`cmd gpu vkjson` 的 **loader** 与驱动版本、本机有没有层 `.so`、GitHub 通不通）。**先跑它再动手** |
| `.work/vallayer_get.py` / `vallayer_dl.py` / `vallayer_head.py` | **A1 起**：取层（`android-binaries-*.zip` → `arm64-v8a/libVkLayer_khronos_validation.so`）。⚠ GitHub API 会 **403 rate limit** ⇒ 走 HTML / 直链；⚠ 本机走 HTTPS 代理，`curl` 要带 `--connect-timeout 20 --max-time 1800 --retry 5 --retry-all-errors --speed-limit 2048 --speed-time 60 -C -`（实测 26 MB 处断一次，续传才完）；⚠ **`urllib` 对 github.com 会 `IncompleteRead`**。`vallayer_dl.py` 是能下完的那个 |
| `.work/vklayer_drytest.py` | **A1 起**：用 4 KB **占位 .so** 验证 `build_apk.cmd` 里那条"带层打包"的可选段（不必等 32 MB 下完），跑完自动删占位 |
| `.work/vklayer_round.py` | **A1 起**：一轮真机（带层出包 → 装机 → 设属性 → **抓 logcat** → 跑 → 拉日志 → **清属性回退**），并把"层加载了没 / 喊了什么"打在屏幕上 |
| `.work/vklayer_off.py` | **A1 起**：退回常态（干净包 → 装机 → 清属性 → 打 `nativeLibraryDir` 清单自查） |
| `.work/vklayer_peek.py` | **A1 起**：**跑着的时候**实时看 logcat（判"层到底加载了没"，能省一整轮） |
| `.work/vklayer_report.py` / `vklayer_msgs.py` / `vklayer_ctx.py` | **A1 起**：按 VUID 归类 / 抽完整消息体 / 打原文上下文。⚠ 层的"具体原因"在 `MessageID` **之后的续行**里（只看首行会漏最关键那句）；⚠ **VUID 名字中间可以有 `-`**，正则别写窄；⚠ **总数相同 ≠ 内容相同**（每个 module 只报第一个未满足的 capability） |
| `.work/vklayer_cap_diff.py` | **A1 起**：按 capability / extension **名**逐项对比两轮（修前 / 修后）—— 这才是"那条报错真的没了吗"的判据 |
| `.work/vklayer_{vuid,builtin,fp64}_check.py` | **A1 起**：**离线**核层点名的东西（不必上设备）—— 分别扫 `OpCapability` / `OpDecorate … BuiltIn` / 64 位类型与算术。⚠ `builtin_check` **只看 `OpDecorate`、不看 `OpMemberDecorate`**（结构体成员的 BuiltIn，比如 `gl_PerVertex` 里的 `ClipDistance`，会漏） |
| `.work/n4e_{scan_confs,probe_conf,passprobe_confs,raster_confs,brute_conf}.py` | **N4-e 起**：写各轮 conf（扫描器 / `DM_GFX_PROBE` 对照 / 通道探针两档 / 光栅化排除两轮 / 光栅放开轮） |

```powershell
$adb = 'C:\Android\Sdk\platform-tools\adb.exe'
cd <仓库>

# 1) 出包（前提：那三个派生头文件已生成，见第二节）
dm\android\apk\build_apk.cmd

# 2) 推参数、装机、跑、把日志取回来
& $adb push .work\dm.conf /sdcard/Android/data/ai.d2rm.dm/files/dm.conf
& $adb install -r .work\apk-build\dm.apk
& $adb shell am force-stop ai.d2rm.dm
& $adb shell am start -n ai.d2rm.dm/android.app.NativeActivity
Start-Sleep -Seconds 50
& $adb pull /sdcard/Android/data/ai.d2rm.dm/files/logs/latest.log .work\logs\run.log
```

**Android 上没法按次给 app 设环境变量**（`am start` 不传 env），所以参数走一个 adb 能推的文件：
`/sdcard/Android/data/ai.d2rm.dm/files/dm.conf`，每行 `key=value`（`#` 开头是注释），**推上去重启 app 即生效，不用重新装机**。
当前那一份：

**2026-10-16 起**：设备上那一份是 **`.work/ab-clean.conf`（最干净的臂：`DM_WIN_SRC=0` + `DM_TG_SERIES=0`）**；
要"停下取现场"就用 **`.work/dm.conf`（「M3-2w」的 A 轮复现配方，带注释）**。后者长这样：

```
DM_WIN_SRC=28          # 贴图查看器的源序号（0 = 关，回测试图）
DM_WIN_LEVEL=1         # 读第几级 mip（第 0 级全零、第 1 级也全零，见上）
DM_WATCH_STALL=600     # 长跑：不判死、进程不退出，好按需取现场（注意：值 ≤ 0 会被**回落到 15 秒**、停住就判死退出）
DM_NVN_SHOT_SCAN=50,54,57:/sdcard/Android/data/ai.d2rm.dm/files/scan/
DM_ZC_CACHED=0
DM_TG_SERIES=0         # 任务图时间序列关掉（默认开，见下）
DM_HUD_TEXT=1          # 顺带（用户参与用、缺省关）：手机屏上一行可读数字（T+秒 / 出图 / 拍 / 阶段 / 查看器读数；冻住时定格）
DM_HUD_TAP=1           # 顺带：点屏幕左下角 = 取一份现场（与 dump.trigger 同一件事）；量帧预算时两个都别开
```

其余可用开关在 `dm/android/` 里搜 `getenv`（`DM_NVN_SHOT`、`DM_NVN_SHOT_ALL`、`DM_GFX_PROBE`、`DM_NO_DEPTH_TEST`、
`DM_NO_TEXEL`、`DM_NVN_SPV_DIR`、`DM_NVN_FACTS`、`DM_ROOT/DM_ROMFS/DM_SAVE`、`DM_PIPE_CACHE`）。**任务图时间序列**
（「M3-2t」，默认开）另有三个：`DM_TG_SERIES=0` 整个关掉、`DM_TG_SERIES_US=采样周期微秒`（缺省 500）、
`DM_TG_SERIES_LINES=明细行预算`（缺省 5000 行；明细行会明显加长日志，一轮 1~2 MB）。**HUD 那两项**（「M3-2w」）
是 `DM_HUD_TEXT`（一行数字）与 `DM_HUD_TAP`（点左下角取现场），**都缺省关**：它们要在前端线程画几百像素，
量帧预算时别开。

**管线缓存**（2026-10-15 起）：落盘位置是 **app 内部目录** `/data/user/0/ai.d2rm.dm/files/pipecache.bin`，
adb 要经 `run-as` 才看得到（`adb shell run-as ai.d2rm.dm ls -l files`）；控制台宿主则用 `<DM_ROOT>/pipecache.bin`。
想从零编译做对照就加一行 `DM_PIPE_CACHE=0`（或在配置里给一个别的路径）。写盘**走的是前台每秒心跳**那一跳，
不是"退出时"——原因见下面「坑」里那一条。

产物落在：

| 落在哪 | 是什么 |
|---|---|
| `/sdcard/Android/data/ai.d2rm.dm/files/logs/latest.log` | 本轮日志（上一轮是 `prev.log`） |
| `/sdcard/Android/data/ai.d2rm.dm/files/scan/` | 逐级扫描里"有内容"才落盘的 PNG |
| `/data/local/tmp/dm/` | 运行时数据（`patched` / `spv` / `romfs`），**只读**，见第五节 |
| `/data/user/0/ai.d2rm.dm/files/pipecache.bin` | 管线缓存（app 内部目录，`run-as` 可见；见上面那段） |

**手机截屏**落 PNG 时**别在 PowerShell 里用 `>`**（写成 UTF-16 就废了）——写一个 `.cmd` 用 cmd 的重定向，规矩同第六节。

### 这一轮新踩的坑（与"接着跑"有关的）

- **扫描点要压在"够得着的帧"上**：这台机常在出图 55~60 停住，`DM_NVN_SHOT_SCAN=300,340` 会**一个都不触发**，
  而日志上看不出是"扫过"还是"没扫到"。用 50/54/57 这类。
- **改日志正文等于改统计口径**：把 `one_end` 里那句从 `上传命令等待失败` 改成 `一次性提交等待失败` 之后，
  按旧串数的统计读出"0 条"，差一点把"没修好"记成"修好了"。**改日志正文时把读它的脚本一起改，或者新旧串都数一遍。**
- **读回的锁只有一把，别乱加**：查看器（前端线程）与出图/扫描（客体线程）会**同时**发起读回，所以读回这一段有
  `g_one_mu`（「M3-2n」）。但**上传那条路不能拿它** —— 调用方 `tex_of` 正持着 `g_mu` 调上传，一加就是**锁序反转**，
  真机表现是"**启动就停住、出图 0 次**"（看门狗只报 `拍 0`），看着像游戏卡死。上传现在有**自己的命令缓冲**
  （`g_cb_up`，「M3-2o」），不需要那把锁。
- **加锁前先列"谁持着什么调进来"**：上面那条死锁就是这么来的，判据是"撤掉那一处，出图立刻恢复常态"。
- **x29 帧链在驱动的帧上不可信**（「M3-2w」）：驱动不一定维护 x29，链会读到栈上的旧值 —— `mtl_wait+0x214`
  （`0x8d63c`）看着"像卡在等栅栏"，反汇编却是"**已滑出在飞窗口 → `return 1`**"这条**不等待**分支的返回地址。
  另一层原因：**Android arm64 的返回地址带 PAC**（签名位在高 16 位），带签名位的地址 `dladdr` 一律失败 ⇒
  回溯里那串 `0x24c6fca47ce57c` 形状的"垃圾地址"就是它。`crash_off` 现在会先 `&= 0x0000ffffffffffff`
  再符号化（用户空间只用低 48 位，对正常地址无副作用）。
- **别在采样信号处理器里做 unwind**（「M3-2w」）：`_Unwind_Backtrace` 在这条路上**把被采样线程卡住了**
  （实测那一轮"新增样本 0"、处理函数没回来）—— 与"别在处理器里 `fprintf`"同源：处理器里不能有未知的锁 / 副作用。
  **读自己的栈**是安全的（`sp` 一定映射着，一次 `memcpy` 2 KB），完整链留到 dump 阶段做。
- **读别的线程要趁它没被打断**（「M3-2w」）：`/proc/self/task/<tid>/syscall` 在该线程被信号打断时会变成 `running`
  （**没有 sp 字段**）⇒ **先扫栈、再发采样信号**；并且**按名字无条件扫主线程** —— 它在"睡一下、看一眼"之间来回，
  通用的空转判据漏过它一次（八条里漏掉的那条恰好是最要紧的）。栈扫描的过滤要用 `/proc/self/maps` 的可执行段，
  **别用 `dl_iterate_phdr`**：`<link.h>` 的 `Elf64_Sym` / `Elf64_Dyn` / `Elf64_Rela` typedef 与 `dm.h` 里那份重名，
  一 include 就 `typedef redefinition` 编译不过。
- **栈扫描那张"可执行段表"会静默地假阴性**（2026-10-15，「M3-2x」）：按 `/proc/self/maps` 过滤的表原来只有 **64** 个槽，
  而真机进程里有 **455~460 个 `r-xp` 段** —— **留下的是 `libdm.so`（第 24 个）与驱动 `vulkan.adreno.so`（第 43 个），
  丢掉的是 `libc.so`（第 403 个）与 `libvulkan.so`（第 404 个）**。`usleep` 就住在 libc 里，于是"栈里一个 libc 地址都
  没有"和"真的没有"长得一样，把一次点名带到了相反的结论。表已放宽到 1024，并且**表满时读数里会明说**（`【黄】表满…`）：
  **凡是有上限的表，都要把"满了"写进读数**。同源的小现象：同一个地址会印成两种样子（`pc=libc.so+0xd7b88` 与
  `lr=ioctl+0xa0` 其实同一个地址 —— `dladdr` 能命名 `ioctl`、命不了 libc 里的 syscall 桩）。
- **读 `/proc/<pid>/maps` 数一数有几段可执行段**（同上）：这台机上 460 段里绝大多数是系统 odex 与 vendor 库，
  而"我们自己的"（`libdm.so`）与驱动排得很前 —— **过滤类仪器的上限必须按真机量级设**，不能按"我们认识的那几个模块"设。
- **机器上已有 `ANDROID_HOME` / `JAVA_HOME` 时，`toolchain.cmd` 会听话地用它们**（这是设计：第二节那条"优先级"），
  于是"装了一套能用的工具链却编不过"会很容易发生：这台机器的 `ANDROID_HOME` 指向一个**没有 NDK / CMake / CMake-ninja
  的 SDK**、`JAVA_HOME` 指向 **Zulu 8**，第一条命令就停在 `[红] 缺 NDK`（真踩）。做法是**不改仓库里的任何文件**，
  在这一次构建的环境里覆盖两个变量即可：
  `$env:DM_SDK_ROOT='C:\Android\Sdk'`、`$env:DM_JDK='C:\Program Files\Eclipse Adoptium\jdk-21.0.12.1+1'`
  （构建开头那两行 `[工具链] …（来自 …）` 就是它的自证）。另外本机 `cmd /c "…"` 的引号会被外层壳吞掉
  （报"系统找不到指定的路径"），**从 PowerShell 直接 `& 'dm\android\apk\build_apk.cmd'` 就是对的**。
- **这台机上"退到后台"等于"被杀"**：MIUI 把这个包按游戏处理（logcat `ANDR-PWR-OPT: GAMEPOWEROPT: isGame()`），
  按 HOME 之后系统直接 `libc: kill: send 9`（SIGKILL），**`TERM_WINDOW` / `DESTROY` / `atexit` 一个都跑不到**
  （两轮实测：pid 在退到后台 6 秒内消失，日志里没有 `窗口那一套已拆`）。所以"靠退出时落盘"的设计在本机不成立 ——
  管线缓存就是这么发现并改成**前台每秒定期写**的（「M3-2p」）。另外 `adb shell input keyevent` 也会被拒
  （`SecurityException: requires INJECT_EVENTS`），要让 app 退到后台得显式启动桌面：
  `adb shell am start -n com.miui.home/.launcher.Launcher`。
- **`DM_WATCH_STALL=0` 不是"只记不杀"**（上面 `dm.conf` 里那句注释就是错的，本轮真踩了）：`crash.c:347-354` 是
  `g_watch_stall = stall && atoi(stall) > 0 ? (unsigned)atoi(stall) : 15` —— 值 ≤ 0 **回落到 15 秒**，所以写 0 的效果是
  "停在原地 15 秒 → 判死 → 打完现场转储 `_exit`"（「M3-2q」的 A/B 里进程在停住后十几秒消失就是这个，**不是崩溃、
  也不是 MIUI 杀的**：日志尾巴正好是"等待点"和那份线程转储）。要**完全不看**看门狗只能**整行不写**
  （`if (watch || stall)` 才起线程）；要长跑观察就给它一个足够大的值。
- **看门狗转储 ≠ "客体停住"**（2026-10-15，「M3-2t」第五条）：它量的"进展"是**引擎拍入口**与 **UI 消息**
  （`crash_watch_progress(cur_tick(),…)`，`diag.c:1752` / `:381`），而**出图帧只在 `pregame` 期间**算进展
  （`crash.c:44`）——所以"**出图在推进 + 拍 0**"照样会在第 15 秒判死、转储、`_exit`（本轮 t1/t3 就是这样，
  眼看着出图 375 / 379 还在涨）。判"停住"用**心跳里的出图数**与前端那条"**连续 8 秒没有新出图**"的卡死报告。
  另外转储本身很长（安卓上约 5 秒才写完），期间游戏线程照跑，所以转储里夹着新的 `DRAW_IDX` 行是正常的。
- ~~**两个读回诊断别同时开**~~（2026-10-15，「M3-2u」五轮 A/B/C）—— ⚠ **已被「M3-2y」十轮改写**：A/B/C/**D（两路都关）**
  四臂八轮里那 ~10 秒停顿**轮轮都有**（含 D），而同一份配置也出现过 13.5 帧/秒的一轮 ⇒ 停顿与诊断开关无关；
  真正决定读数的是**每次启动落在"健康 / 生病"哪一种**（op/帧 1234 vs 100）。"量帧预算时只开一个"仍可留着，
  但**别再把"停住"归因到"两路同时开"**（详见「M3-2y」）。
- **"不写 `DM_WIN_SRC`"不等于关查看器**（2026-10-16，「M3-3d」实测量出）：`main_apk.c:749` 是
  `g_src_ser = src ? strtoull(src,0,10) : 28;` ⇒ **缺省 28 就是开着**（启动头会自报 `DM_WIN_SRC=(缺省 28)`）。
  所以"干净的臂"必须**显式写 `DM_WIN_SRC=0`**；`DM_NVN_SHOT_SCAN` 才是"写了才开"。判"这一轮是哪条臂"看启动头
  那两行与心跳里的 `查看器 读成/跳过`（`.work/ab_run.py` 每轮会把 `查看器 <序号>` 打出来当自证）。
- **要停住的现场就按需取**（同上）：`adb shell touch …/files/dump.trigger` ⇒ 一秒内打进现场（线程在等什么 +
  图形侧等待点），**进程不退出**，可以再取一次。看门狗那条是"导完就 `_exit`"，而直接发 `SIGALRM` 走 `on_crash`
  会把**转储的尾巴切掉**（它没有看门狗那 400 ms 的等待）—— 所以停住研究只用这个触发口。
- **`等待点` 里"在飞槽 1/8、提交比出图多 4"不是卡死签名**（同上）：没停的那一轮读数一模一样（1228/1224）。
  卡死的签名是**这些计数冻住不动** + **主线程不再发 SVC**（`调用链（32.1 秒前）`那种时间戳可判）。
- **MIUI 上 `adb install -r` 会被拦**：报 `INSTALL_FAILED_USER_RESTRICTED: Install canceled by user`（手机端确认弹窗
  被取消或权限没开），而 `install_apk.cmd` 用的就是它。改用**设备端**安装立刻成功：
  `adb push .work\apk-build\dm.apk /data/local/tmp/dm.apk` + `adb shell pm install -r /data/local/tmp/dm.apk`。
- **同一份配置连着两轮可以差 13 倍**（2026-10-15，「M3-2y」十轮）：**869 帧 / 64 秒**（13.5 帧/秒、零长等）与
  52~92 帧/64 秒（1.1~1.4 帧/秒、每条 2~11.6 秒的停顿）**都能出现**，判据是**画面内容本身**：
  健康那轮 ≈**1234 op/帧**、生病那轮 ≈**100 op/帧**（`绘制 有目标+无目标` ÷ `出图`）。
  ⇒ **量任何帧预算 / 节拍之前先判这一轮落在哪一种**，否则同一份配置的读数没法比；
  **"哪一路读回诊断开着"不是原因**（连两路都关也有那 ~10 秒停顿）。
- **`.ps1` 与 `.cmd` 一样：本机 PowerShell 5.1 会把"无 BOM 的 UTF-8"按 GBK 读**，中文注释会把脚本拆坏
  （实测 `ab-run.ps1` 报一串"意外的标记"、字符串缺终止符）。**抄仓库里那份 `.cmd` 的老规矩**：
  要么脚本里只写 ASCII，要么给它**补 UTF-8 BOM**（`[System.IO.File]::WriteAllBytes($p, [byte[]](0xEF,0xBB,0xBF) + [System.IO.File]::ReadAllBytes($p))`）。
- **"这轮是健康还是生病"是每次量之前的前置动作**（「M3-2y」/「M3-2z」）：现成的四件工具都在 `.work` 里 ——
  `ab-run.ps1`（跑 N 轮 + 解析：帧率 / 心跳最大间隔 / >1 秒的等待 / 读回锁 / `-4` 计数）、
  `probe-launch.ps1`（一次启动 30~60 秒的读数 + 同时抓系统侧 logcat）、`logdiff.ps1`（两份日志的"行形状"差）、
  `pipepairs.ps1`（两轮画过的 顶点/片元 对）。**判别"停摆是我们还是系统"最便宜的一招**：`am force-stop` +
  `logcat -c` + 等 60 秒，然后看 `Davey!` / `IComposerClient` / `Slow swap buffers` 有没有 —— 有就是系统在停。
- **MIUI 上 shell 改不了刷新率**：`settings put system min_refresh_rate 120` 会被拒
  （`SecurityException: com.android.shell was not granted WRITE_SETTINGS`）⇒ "钉住刷新率"这类实验做不了，
  别在它上面浪费时间（读数用 `dumpsys SurfaceFlinger` 看，面板当时是 min/max/cur 全 120 Hz）。
- **要"谁在等 / 谁在跑 / 等的是哪个内核函数"，本机这台手机上最便宜的仪器是 `atrace`，不是 perfetto**（2026-10-15，「M3-3a」）：
  `atrace --async_start -b 32768 -c sched freq sync` → 命中后 `--async_stop -o /data/local/tmp/x.txt`，**输出是文本 ftrace**（`prev_state`、
  `sched_blocked_reason: pid=… caller=…`、`cpu_frequency`、`clk_set_rate`、`dma_fence_signaled`），**开头那条 `tracing_mark_write: trace_event_clock_sync: realtime_ts=…`
  能把 trace 时钟对到墙钟**（对窗口就靠它）。**shell 读不了 `tracefs` 的 `enable` / `available_events`，也读不了 `/sys/class/kgsl/*`（都是 `Permission denied`）**，
  但 shell 在 `readtracefs` 组里、atrace 走的是它自己有权限的那条路 —— **别在"直接抓 ftrace"上放弃**。抓取配方（环状缓冲 + "最近 4 条心跳出图数相同且 >0"当扳机）
  已落成 `.work\trace\stall-ftrace.ps1`（**纯 ASCII**：`.ps1` 有 BOM 那条老规矩在这里是反过来用的，脚本里一个中文都没有，免得被 GBK 拆坏）。
- **perfetto 在这台机上有三条硬约束，而且"没报错"不能当"原子可用"**（2026-10-15，「M3-3a」）：① **配置只能放 `/data/misc/perfetto-configs/`、产物只能写
  `/data/misc/perfetto-traces/`** —— perfetto 自己的 SELinux 域**读不了 `/data/local/tmp` 的配置、也写不了那儿的输出**（`errno 13`），`cmp` 出来是权限不是路径；
  ② `compression_type: COMPRESSION_TYPE_NONE` **不是合法枚举**（只有 `UNSPECIFIED` / `DEFLATE`），写了会被判"配置非法"，要"不压缩"就**整行不写**；
  **pbtxt 不能带 UTF-8 BOM**（带 BOM 报 `1:1 error: Unexpected character ''`）；③ **`perfetto` 对"根本不存在的原子名"也不报错**（负对照：写 `kgsl/no_such_atom_xyz`
  照样跑完出 trace）⇒ 判据只能看 logcat 里 `traced_probes` 的 `avc: denied { read } … name="format" … tcontext=u:object_r:debugfs_tracing_debug:s0`
  —— **这台机上 kgsl 那一族原子就是被它挡下的**（MIUI 不让 `traced_probes` 读 vendor 事件的 `format`，而 perfetto 要靠 `format` 才能解析事件）。
- **"轮次"现在至少分三类，别再合成一个数**（2026-10-15，「M3-3c」）：**健康**（964~976 帧 / 69 秒、零 >1 秒等待）、**停顿型病态**（55~97 帧、每条 2~12 秒的长等，
  落点是上传的一次性提交 / 客体排队提交）、**设备丢失型**（`VkResult -4` 2 条 + 60~139 条上传失败，而**最长等待只有 ~2 秒**）。再加上第四类**崩溃轮**（无效样本，见下条）。
  读一轮之前先归好类，否则同一份配置的读数能差 13 倍这件事会再重复一次。
- **这台机上没有 root / su，dmesg 读不到**（2026-10-15，「M3-3c」）：`adb root` → `adbd cannot run as root in production builds`；`su` 不存在；
  `dmesg` → `klogctl: Permission denied`。**"读 dmesg 看 GPU 故障"这条路封死**，别再花时间 —— 这台机上能拿到 GPU 侧信息的只剩"换设备/换驱动"与"有 `trace_processor` 的机器读 pftrace"。
- **"崩溃轮"是无效样本，排轮次时先剔掉**（2026-10-15，「M3-3b」）：那条**已知的偶发启动崩溃**（`CRASH sig=11 … pc=libc.so+0xd6b18`，
  与 `.work\logs\run-fixed-lock.log` 同签名）本轮 12 次启动里撞上 3 次，而且**开环 / 不开环两臂都出现** ⇒ 与改动无关、但会把"健康 / 病态"的比例带偏
  （崩溃轮里根本没有出图，`ab-run.ps1` 只报 `hb_count = 1`）。判据：日志里出现 `CRASH sig=11` 或心跳只有 1 条。
- **别拿 `[长帧]` 行里的"上传贴图 首传 N 张 + 重传 M 次"读上传量**（2026-10-15，「M3-3b」）：本轮**所有**日志里这两个数**恒为 0**
  （长帧只统计那一帧内的上传，而上传都发生在启动 / 加载的那些帧上）。要读上传量用**代码里自己的计数**（例如 `DM_UP_RING=1` 时那几行
  `上传环：环内已借 N 段、回绕 M 次、建/换 K 次`）。
- **"里程碑打印"的条件写错会静默刷屏**（2026-10-15，「M3-3b」）：常驻环那行"已借 / 回绕 / 建换"的第一版条件写成 `g_ur_wrap == 1`，
  于是**第一次回绕之后每趟上传都打一行**（一轮刷十几行同样的计数）。读数没坏、日志被污染 —— 这类"里程碑"要用 `last_logged != cur`（或一次性的
  `if (!said++)`）去重；同理，**把两种不同原因混进同一句【黄】会写出自相矛盾的行**（本轮出现过 `单次 1920000 字节 > 上限 67108864`，见「M3-3b」）。
- **本机 cmd 的代码页是 936（GBK），而仓库里的 `.cmd` 是 UTF-8 无 BOM** ⇒ 直接跑 `build_apk.cmd` 会被中文注释
  拆行、报一堆 `'xxx' 不是内部或外部命令` + `此时不应有 …`（「M3-2s」第一次构建就这么失败的，产物一个没出）。
  先 `chcp 65001` 再 call 即可（`.work\build_u8.cmd` 就是这个两行包装；脚本**不用改**，这是本机控制台的事）。
  判据：失败时 `build.err` 里出现半截中文注释；成功时日志里那句 `== 1/5 … ==` 虽然显示成乱码但 `BUILD_RC=0` 且出包。
- **"只查第一个 X"会改变结论（N4-f 现场）**：常量采样第一版只读"**第一个**非零常量槽"，而 `常量[0][0]`
  的前 4 个是 `-0.00125,0,0,0`（|max| 2）—— **不像变换矩阵**；同一轮另一条样本里 `0.766044,0,-0.642788,0`
  （|max| 1055）才是旋转/平移那一类。改成**扫全部槽**之后 A/B 两臂的逐槽 |max| 才对上 ⇒
  **凡"只看第一条 / 第一个 / 第一次"的读数，都要问一句"漏掉的那部分会不会正好是我要找的"**。
- **心跳加格位要包一层"只在开关打开时出现"**（N4-f 的做法）：`DM_VTX_PROBE` 关着时心跳那一行与上一轮
  **逐字节相同**（可用 `.work/n4e_hb_len.py` 量字节比对）⇒ "缺省关不改变既有判据"这条口径**可核**；
  采样行本身另起一行（`【顶点采样】`，约 234~640 字符，`log_line` 上限 1200），**不动既有判据行的正文**。
- **归一化顶点格式的"数量级"没有诊断力**（N4-f 读数）：`0x2b` = 4×ShortSNORM 的值域天然是 [-1,1]
  ⇒ 只能贡献"NaN/Inf = 0"与"不是全零"两条；要判"数量级离谱"必须看**浮点类**（`0x22` = 3×Float、
  `0x2e` = 4×Float；NVN 格式号：分量数 = `(f-1)/12 + 1`、组内类型 = `(f-1)%12`）。
- **🔴 判据别只看首行（A1 现场，最贵的一条）**：Khronos 校验层的消息**跨多行** ——
  `Validation Error: [ VUID-… ] | MessageID = 0x…` 只是**第一行**，"具体原因"（哪个 capability / 哪个 buffer /
  数组多大）在**紧跟的续行**（同 tag 同 tid）里。本机 `.work/vklayer_msgs.py` 第一版就因为 logcat 前缀正则
  写错、续行没拼上，打印出来"每种 VUID 都只有一句 `MessageID`" —— **看着像"层没说什么"，其实是最关键那句被丢了**。
  碰到 digest 不出来的东西，用 `.work/vklayer_ctx.py <tag> <关键字>` **原样打上下文**（最少加工最少出错）。
- **🔴 "总数相同"不等于"内容相同"（A1 现场）**：`VUID-VkShaderModuleCreateInfo-pCode-08740` 是
  **每个 shader module 报一次、只报第一个未满足的 capability** ⇒ 修掉 `DrawParameters` 之后同一个 module
  改报 `Float64`，**总数 40 一点没变**。判"那条真的没了吗"必须**按 capability / extension 名逐项对比**
  （`.work/vklayer_cap_diff.py`：`DrawParameters` 2 → 0，`Float64` 12 → 14）—— 与坑 ⑩ 的"统计快照的位置会改总数"同源。
- **🔴 正则里的 `-`（A1 现场）**：VUID 名字**中间可以有 `-`**（`VUID-vkCreateDevice-ppEnabledExtensionNames-01387`）
  ⇒ `VUID-[A-Za-z0-9_]+-\d+` **一种都匹配不到**，而"匹配不到"与"没有报错"长得一模一样。
  写判据的正则时，**先拿已知会出现的样本喂一遍**（第 ⑨ 条的又一例）。
- **🔴 离线脚本的"覆盖范围"要先说清（A1 现场）**：`vklayer_builtin_check.py` 只看 `OpDecorate`、不看
  `OpMemberDecorate` ⇒ 它报"`ClipDistance` 0 份"，而层报"顶点着色器 ClipDistance 数组 8 + CullDistance 1"
  （那两个是**结构体成员**上的 BuiltIn，走 `OpMemberDecorate`）⇒ **"脚本说没有"不等于"真的没有"**，
  先用一个**已知存在**的样本把脚本喂一遍再信它。
- **一句结论：安卓上装 Vulkan 校验层不需要 root 也不需要改代码**（A1 的正面配方见 `TOOLS` §G）：
  层 `.so` 进 APK 的 `lib/arm64-v8a/` + `setprop debug.vulkan.layers <层名>`，app 只要 `android:debuggable="true"`。
  前提是**该 app 的 native 库目录里确有抽取出来的 `.so`**（先 `ls` 一下 `/data/app/…/lib/arm64/` 确认）。
