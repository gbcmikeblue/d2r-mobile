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

Python 环境：`python -m venv .venv`，再 `.venv\Scripts\pip install pillow numpy lz4 capstone unicorn`。

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

> **这一节会过期。** 记于 2026-10-15（基线提交 `ac240c6`，M3-2z；前几轮「M3-2y」/「M3-2x」/「M3-2w」）；
> 进度细节与结论以 [ANDROID_PLAN.zh-CN.md](ANDROID_PLAN.zh-CN.md) 的「七、下一步」与「八、进度记录」为准。

### 现在停在哪

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
- **一条偶发**：约每轮 1 次 `【红】提交失败 VkResult -4`（`VK_ERROR_DEVICE_LOST`），后面跟上百条
  `贴图上传失败`。**与共用命令缓冲无关**（已排除，「M3-2o」）；「M3-2t」的 t2 里它之后窗口被拆、日志截断 ⇒
  与"停住"很可能是同一条线。

### 下一步（细节在 ANDROID_PLAN「七、下一步」）

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
3. ~~**「九」优化清单第 2 项**（零拷贝内存类型优先 `HOST_CACHED`）~~ —— **已取证、已否掉（2026-10-15，「M3-2q」）**：
   类型 6 **能**导出 fd（256 MB / 512 MB / 3968 MB 都过，`mmap` 到指定地址、双向也通），但要**越过缓冲声明的可用
   类型位**（外部缓冲是 `0x13`，里面没有类型 6 —— 属规范外用法、靠驱动宽容），而 A/B 里 CPU 时间**没有可测差异**
   （同峰配对 <3%，轮间波动 ±100%）：收益看不到，风险就别付，**维持类型 4**；`DM_ZC_CACHED=1` 开关留着（默认关），
   换设备 / 换驱动时一条命令可复验。（第 1 项已落地，见「M3-2p」；"启动到 `nnMain` 的时间"那条 A/B 等第 1 条有进展再补。）

### 怎么跑一轮（新机器上的最小闭环）

PC 侧的路径与工具见第二、三节；这里只说**跑一次并拿到读数**：

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

设备上现在那一份**就是「M3-2u」的复现配方**（停下取现场用）——`.work/dm.conf` 里带注释的同一份：

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
- **本机 cmd 的代码页是 936（GBK），而仓库里的 `.cmd` 是 UTF-8 无 BOM** ⇒ 直接跑 `build_apk.cmd` 会被中文注释
  拆行、报一堆 `'xxx' 不是内部或外部命令` + `此时不应有 …`（「M3-2s」第一次构建就这么失败的，产物一个没出）。
  先 `chcp 65001` 再 call 即可（`.work\build_u8.cmd` 就是这个两行包装；脚本**不用改**，这是本机控制台的事）。
  判据：失败时 `build.err` 里出现半截中文注释；成功时日志里那句 `== 1/5 … ==` 虽然显示成乱码但 `BUILD_RC=0` 且出包。
