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

### 装完必改的硬编码路径

`dm\android\apk\build_apk.cmd:22-27` 和 `dm\android\apk\install_apk.cmd:13` 里写死了上一台机器的绝对路径（NDK / JDK / build-tools / CMake / ninja / android.jar / adb），换机器必须改这两段。`.work\apk-build`、`.work\apk-keys` 是相对脚本自身推导的，不用动。

**实测（第二台机器）**：要改的只有 **JDK 一行**（`build_apk.cmd:23`）——NDK / build-tools / CMake / android.jar 只要按上面那张表装到 `C:\Android\Sdk`，就与文档里写的一致；`install_apk.cmd:13` 的 adb 路径同理。改完直接 `build_apk.cmd` 出包通过。

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

> **这一节会过期。** 记于 2026-10-14，对应提交 `ea71aab`；进度细节与结论以
> [ANDROID_PLAN.zh-CN.md](ANDROID_PLAN.zh-CN.md) 的「七、下一步」与「八、进度记录」为准。

### 现在停在哪

- **M1 完成**：宿主在安卓上装载改写产物、接管服务层、跑完 SDK 初始化、进 `nnMain`。
- **渲染链路的告警已清空**：描述符的空白（UBO 名字号 −3、SSBO 由 `cb_offset` 反推、缓冲区贴图）与顶点属性的那条判据
  （两个独立的错，见「M3-2k」「M3-2l」）。
- **卡点**：绘制在发（每轮约 40 万次、图形管线 78~84 条、零失败），但**渲染目标读出来是全零**。
  `DM_NVN_SHOT_SCAN` 现在**逐级**读（第 0/1/2 级各一次），在够得着的帧（50/54/57）上**每张都是 `亮度 均值 0.0`**
  （「M3-2m」第六节）。也就是说：**画面还没有被写到能读的地方**，或者"读"这一侧还有没照到的问题。
- **一条常态**：这台机**常在出图 55~60 停住**（阶段 `CharacterCreate:Hover:Barbarian`，偶发 `MainMenuHD:CheckUpdateLinks`）。
  这是已知现场，不是崩溃；判据要按"多轮里它在干什么"读。
- **一条偶发**：约每轮 1 次 `【红】一次性提交等待失败 VkResult -4`（`VK_ERROR_DEVICE_LOST`），后面跟上百条
  `贴图上传失败`。**与共用命令缓冲无关**（已排除，「M3-2o」）。

### 下一步（细节在 ANDROID_PLAN「七、下一步」）

1. **查那条设备丢失**：下一个候选是"**先丢的是谁**"——客体自己的命令，还是我们这一次一次性提交。
   要量的东西：丢的那一刻队列上还有什么、设备是否已被客体那边先弄丢、`vkQueueSubmit` 的返回值与前后文。
2. **继续找画面**：逐级扫已就位，但只在够得着的帧上；要拿更晚的帧，得先弄清"为什么停在这里"（上面那条常态）。
3. **「九」优化清单**第 1/2 项（`VkPipelineCache` 落盘、零拷贝内存类型优先 `HOST_CACHED`）——与卡点无关，随时可做。

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

```
DM_WIN_SRC=28          # 贴图查看器的源序号（0 = 关，回测试图）
DM_WIN_LEVEL=1         # 读第几级 mip（第 0 级全零、第 1 级也全零，见上）
DM_WATCH_STALL=0       # 看门狗只记不杀（不设就完全不看）
DM_NVN_SHOT_SCAN=50,54,57:/sdcard/Android/data/ai.d2rm.dm/files/scan/
DM_PASS_AREA=1         # 渲染区取证（只读；=2 改成用附件尺寸做对照）
```

其余可用开关在 `dm/android/` 里搜 `getenv`（`DM_NVN_SHOT`、`DM_NVN_SHOT_ALL`、`DM_GFX_PROBE`、`DM_NO_DEPTH_TEST`、
`DM_NO_TEXEL`、`DM_NVN_SPV_DIR`、`DM_NVN_FACTS`、`DM_ROOT/DM_ROMFS/DM_SAVE`）。

产物落在：

| 落在哪 | 是什么 |
|---|---|
| `/sdcard/Android/data/ai.d2rm.dm/files/logs/latest.log` | 本轮日志（上一轮是 `prev.log`） |
| `/sdcard/Android/data/ai.d2rm.dm/files/scan/` | 逐级扫描里"有内容"才落盘的 PNG |
| `/data/local/tmp/dm/` | 运行时数据（`patched` / `spv` / `romfs`），**只读**，见第五节 |

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
