# 换一台电脑继续（跨机器开发调试）

English: [CROSS_MACHINE.md](CROSS_MACHINE.md)

这一页只讲一件事：Android 侧的开发调试换一台电脑时，除了 `git clone` 到的代码，还要带哪些文件、从哪拿、恢复到什么样才算能用。代码本身很小（`.work/`、`.tools/`、`.venv/` 都在 `.gitignore` 里，全在仓库之外），真正要搬的是数据。

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

## 四、怎么算恢复好了

1. `dm\android\apk\build_apk.cmd` 能出包（产物 `.work\apk-build\dm.apk`）—— 这一步同时验证了 NDK 那段路径改对了。
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
- `origin` 已是 fork 地址 `git@github.com:gbcmikeblue/d2r-mobile.git`，`android` 分支跟踪 `origin/android`。SSH 用的是 `gbcmikeblue` 那把 key，新电脑要自己生成并加到 GitHub 账号。
- 中转区要更新就再 `adb push` 一次覆盖；要清空直接 `adb shell rm -rf /sdcard/d2rm/pc`，下次再推回来即可。
- 手机上这份中转副本**不是**运行时用的那份数据：app 跑起来读的是 `/data/local/tmp/dm`，两者别混。
