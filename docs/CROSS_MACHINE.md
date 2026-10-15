# Moving to another computer (cross-machine development and debugging)

中文：[CROSS_MACHINE.zh-CN.md](CROSS_MACHINE.zh-CN.md)

This page covers one thing: when Android-side development and debugging move to another computer, which files besides the `git clone`d code have to come along, where they come from, and what "restored" looks like. The code itself is small (`.work/`, `.tools/` and `.venv/` are all in `.gitignore`, hence outside the repository); the thing that actually needs moving is the data.

**If you are just carrying on rather than moving house, go straight to section 7**: it says where the work stands, what the next step is, and how to run one round and get a reading out.

## 1. The phone is the data carrier

Everything that has to cross machines lives in the phone's **staging area** `/sdcard/d2rm/pc/`: write it in with `adb push`, pull it back with `adb pull` on the next machine. No external drive, and no need to re-extract the game.

| Path on the phone | Size | Restored to | Cost of leaving it out |
|---|---|---|---|
| `/sdcard/d2rm/pc/data/` | 8.4 GiB / 51 files | `<repo>\.work\data\` | The game files cannot be regenerated: you would re-extract them from your own `.xci/.nsp` + `prod.keys` with hactool |
| `/sdcard/d2rm/pc/spv.tar` | 616 MB / 13,847 entries | unpack into `<repo>\.work\spv\` | The SPIR-V for 6,923 shaders would have to be recomputed (needs .NET plus a full-library translation) |
| `/sdcard/d2rm/pc/out/` | 300 MB | `<repo>\.work\build\cache\dm\out\` | Rewriter output; cheap to rebuild (`dm/tools/product.sh build`), but the Python environment has to exist first |
| `/sdcard/d2rm/pc/apk-keys/` | 2.6 KB | `<repo>\.work\apk-keys\` | A regenerated key changes the signature, so the old package must be uninstalled from the phone first |
| `/sdcard/d2rm/pc/android-save/` | 47 KB | `<repo>\.work\android-save\` | Saves and settings are lost |
| `/sdcard/d2rm/pc/hactool/` | 16 MiB | `<repo>\.tools\hactool\` | Only used to re-extract the game (or skip it and build hactool when the need arises) |

About 9.3 GB in total.

**Not needed** (seconds to minutes to rebuild on the new machine): `.work/android-build`, `.work/apk-build`, `.work/dmsh`, `.work/xlate`, `.work/probe`, `.work/recon`, `.venv`. Note in particular: do **not** carry over any generated `CMakeCache.txt` -- it records the previous machine's absolute paths.

## 2. What the new machine needs installed

The minimal Android-side tool set (the versions measured to work here):

| Tool | Version |
|---|---|
| NDK | 30.0.16248370 |
| build-tools | 36.0.0 |
| platforms | android-36 |
| CMake | 3.31.6 (with ninja) |
| platform-tools | adb |
| JDK | 21 (apksigner needs `JAVA_HOME`) |
| Python | 3.10 + `pillow numpy lz4 capstone unicorn` |

dotnet 10 is needed only to regenerate `.work/spv`. Xcode, spirv-cross and xcodegen belong to the iOS chain and are not needed on the Android side (Android gets off the train at SPIR-V).

Python environment: `python -m venv .venv`, then `.venv\Scripts\pip install pillow numpy lz4 capstone unicorn`.

### Toolchain paths: **do not** edit a file

**Normally nothing needs editing.** `dm\android\apk\build_apk.cmd` and `dm\android\apk\install_apk.cmd` no longer hard-code any
tool path -- everything is decided in **one** place, `dm\android\apk\toolchain.cmd`, which discovers in this order (first match
wins):

1. an explicit override: `DM_SDK_ROOT` / `DM_NDK` / `DM_BUILD_TOOLS` / `DM_CMAKE` / `DM_PLATFORM` / `DM_JDK`
2. what the machine itself declares: `JAVA_HOME` / `ANDROID_SDK_ROOT` / `ANDROID_HOME`
3. the **newest** version under the known install roots (JDK prefers `jdk-21*`, then any `jdk-*`)
4. fallbacks: the `jbr` that ships with Android Studio, then `java` on `PATH`

**Installed elsewhere?** Use any of `DM_*` or `ANDROID_HOME` to point at it; **no file in the repo needs editing.** What was
chosen is printed at the top of the build ("a conclusion should not rest on memory").

`.work\apk-build` and `.work\apk-keys` are derived relative to the scripts themselves and never needed editing.

**Why the JDK deserves this rule by itself**: Adoptium renamed its directory from `jdk-21.0.12.1+1` to
`jdk-21.0.12.101-hotspot` -- **same major version, different directory name** -- so "install the JDK the doc names" still
yields an inconsistent path. The SDK-side version names come from the SDK Manager, so following the doc keeps them identical.

**History** (state before this refactor, recorded for the record): `build_apk.cmd:22-27` and `install_apk.cmd:13` hard-coded
the seven absolute paths (NDK / JDK / build-tools / CMake / ninja / android.jar / adb); on the first two machines only the
JDK line ever had to change (the one time Adoptium renamed the directory). All seven now go through `toolchain.cmd`, and the
repo no longer carries "the previous machine's path".

### The three headers generated from the game files (not in the table above)

`dm\host` holds three headers that are **generated from the game files and gitignored** (`dm\host\.gitignore`):

| Header | Generator |
|---|---|
| `dm\host\dm_vault_tab.h` | `dm\ios\tools\gen_vault_table.py` |
| `dm\host\dm_hud_exp.h` | `dm\ios\tools\gen_hud_exp.py` |
| `dm\host\dm_bright_levels.inc` | `dm\tools\gen_bright_levels.py` |

They are **neither in git nor in the "what to carry" table above**, yet without them `build_apk.cmd` never gets past step 1: `dm\host\diag.c:6` stops with `'dm_hud_exp.h' file not found` (compile time, not link time). That is by design rather than a defect -- they are derived from the player's own game files, so **regenerate them once the data is back**, following `dm\host\Makefile:19-29` (about a minute).

Two Windows rules (the first is also in [ANDROID_PLAN.md](ANDROID_PLAN.md), section 6):

- `PYTHONUTF8=1` and `PYTHONIOENCODING=utf-8` are mandatory, otherwise the GBK output **truncates the table headers**;
- the **redirect has to be cmd's `>`**: these are C headers, and PowerShell's `>` writes **UTF-16LE** (`FF FE` at the front, roughly twice the size), which the next compile step will not accept.

Copy and run as is (cmd; set `REPO` to your repository path):

```bat
set "REPO=<repository path>"
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"
set "ROMFS=%REPO%\.work\data\switch\3.3.94012\Update\RomFS"
cd /d "%REPO%\dm\host"
"%REPO%\.venv\Scripts\python.exe" ..\ios\tools\gen_vault_table.py "%ROMFS%" > dm_vault_tab.h
"%REPO%\.venv\Scripts\python.exe" ..\ios\tools\gen_hud_exp.py    "%ROMFS%" > dm_hud_exp.h
"%REPO%\.venv\Scripts\python.exe" ..\tools\gen_bright_levels.py  "%ROMFS%" > dm_bright_levels.inc
```

## 3. Restoring

PowerShell; replace `<repo>` with the repository path on the new machine and the adb path with the one there.

```powershell
$adb  = 'C:\Android\Sdk\platform-tools\adb.exe'
$repo = 'D:\yiyi\d2r-mobile'

# 1) Enable USB debugging on the phone, plug it in
& $adb devices

# 2) Pull the data back (about 9.3 GB, measured at 32.8 MB/s, so roughly 5 minutes)
& $adb pull /sdcard/d2rm/pc/data/.         "$repo\.work\data\"
& $adb pull /sdcard/d2rm/pc/out/.          "$repo\.work\build\cache\dm\out\"
& $adb pull /sdcard/d2rm/pc/apk-keys/.     "$repo\.work\apk-keys\"
& $adb pull /sdcard/d2rm/pc/android-save/. "$repo\.work\android-save\"
& $adb pull /sdcard/d2rm/pc/hactool        "$repo\.tools\"
& $adb pull /sdcard/d2rm/pc/spv.tar        "$repo\.work\"

# 3) spv.tar is flat (it holds .spv and .res.json directly, no top-level directory),
#    so it has to be extracted into .work\spv
New-Item -ItemType Directory -Force "$repo\.work\spv" | Out-Null
tar -xf "$repo\.work\spv.tar" -C "$repo\.work\spv"
(Get-ChildItem "$repo\.work\spv" -File | Measure-Object).Count   # should print 13847 (the 6,923 .bin files are not in it; see below)
```

Windows' bundled bsdtar is enough for `tar`; on Mac / Linux change the separators to `/`.

**Do not forget step 4**: the three generated headers have to be regenerated once from the data just pulled -- see "The three headers generated from the game files" in section 2, and note that this step has to run in **cmd** (PowerShell's `>` would write those headers as UTF-16). With all four done, verify as in section 4.

### When the phone is flaky: pull file by file, not directory by directory

A directory-wide `adb pull` that drops has to start over. What a second machine actually showed, on a Hyper-V VM (`ROOT\VMBUS\0000`) with USB passthrough: the directory pull died on the second file (the 1 GiB `00-00000001.data`) and `adb devices` went empty right after. **The screen locking was not the cause** -- that machine already had `stay_on_while_plugged_in = 15` and a 5-minute `screen_off_timeout`. The shape below got all 9.3 GB through in one pass: the 1 GiB file failed once, succeeded on the retry after the device came back, and the rest ran at about 40 MB/s.

Three things make the shape:

- pull **file by file** (take the list from `adb shell 'cd <src> && find . -type f'`, then `adb pull` each one) and **skip whatever already matches the remote size**, so the script can be rerun as often as needed and a drop only costs the files not yet fetched;
- on failure check whether the device is still there; if not, wait for it with `adb wait-for-device` and retry (measured here: it came back on its own after about a minute);
- to wait inside cmd use `ping -n 4 127.0.0.1 >nul`, **not `timeout /t`** -- that one needs console input and exits outright with `ERROR: Input redirection is not supported` when the output is redirected, taking the whole script down with it.

## 4. How to tell it is restored

1. `dm\android\apk\build_apk.cmd` produces a package (`.work\apk-build\dm.apk`) -- which also proves the NDK block was edited correctly. This presumes the three generated headers from section 2 exist; otherwise it stops at `'dm_hud_exp.h' file not found` in `dm\host\diag.c`.
2. `dm\android\apk\install_apk.cmd` installs and starts it.
3. If it will not start, check these three in order: `main`/`sdk`/`subsdk0` under `.work\data\switch\3.3.94012\Update\ExeFS\` (their checksums are in `setup.sh:84-86`); the resource archives under `.work\data\switch\3.3.94012\Update\RomFS\data\`; and `.work\build\cache\dm\out\3.3.94012-r41-android\`.

### A note on the 6,923 `.bin` files

`.work\spv` also holds 6,923 `.bin` files with the same names as the `.spv` ones: they are intermediates carried out of the generation step. At run time the shaders are read as `<key>.spv` (see `dm/android/vk/vk_shader.c:484-488`) and only `.spv` / `.res.json` are counted (`:67-71`), so the tar does not contain them and their absence after extraction changes nothing.

## 5. The device side (only a new phone makes this necessary)

What the current phone already carries:

| On the phone | Size | What it is |
|---|---|---|
| `/data/local/tmp/dm/` | 9.1 GB | `romfs` 8.3 G, `spv` 709 M, `patched` 75 M, `dmhost`, several `vk_*_probe` binaries |
| `/sdcard/Android/data/ai.d2rm.dm/files/` | -- | the app's writable area: `save`, `save.work`, `temp`, `rt`, `logs`, `dm.conf` |

Three old rules:

- `/data/local/tmp` is **read-only** for the app's domain (writing fails with errno 13), so saves and temporary files go to `/sdcard/Android/data/ai.d2rm.dm/files/`;
- `MANAGE_EXTERNAL_STORAGE` has to be switched on by hand in system settings;
- the game data goes in `/data/local/tmp/dm/romfs` (fast to read) or `/sdcard/d2rm` (survives a reboot).

## 6. Things that bite

- **git identity**: this machine has no `user.name` / `user.email` configured (commits pass an identity matching the history via `-c`: `gbcmikeblue <1061589622@qq.com>`). Set one up first on a new machine, or `git commit` fails outright with `Author identity unknown`.
  Note that a new machine may **already carry somebody else's global identity** (measured on the second machine: the global one was `bicheng.gao <bicheng.gao@yiartkeji.com>`, which does not match the repository history). Do not touch the global one; set it per repository instead:
  `git config user.name gbcmikeblue` plus `git config user.email 1061589622@qq.com` (without `--global`, `git config` only affects this repository).
- `origin` is already the fork `git@github.com:gbcmikeblue/d2r-mobile.git`, and the `android` branch tracks `origin/android`. SSH uses the `gbcmikeblue` key, so a new machine has to generate its own and add it to the GitHub account.
- To update the staging area, just `adb push` over it; to clear it, `adb shell rm -rf /sdcard/d2rm/pc` and push it back later.
- This staging copy is **not** the data the app runs on: at run time the app reads `/data/local/tmp/dm`. Do not mix the two up.
- **The three generated headers are not in the table above**: `dm\host`'s `dm_vault_tab.h` / `dm_hud_exp.h` / `dm_bright_levels.inc` come from the game files and are gitignored, so without them `build_apk.cmd` stops at `'dm_hud_exp.h' file not found` in `dm\host\diag.c`. Regenerate them from section 2 once the data is back.
- **PowerShell's `>` changes the encoding** (it writes UTF-16LE). Whenever the raw bytes matter -- generating C headers, dropping logs -- use **cmd's `>`**. Measured on the same script here: 93,550 bytes of UTF-16 from PowerShell against 47,124 bytes of UTF-8 from cmd.
- **USB drops out entirely**: on this machine (Hyper-V plus USB passthrough) a directory-wide `adb pull` of large files dies after about 30 seconds and `adb devices` goes empty with it, and that has nothing to do with the screen locking. See "When the phone is flaky" in section 3.

## 7. Where things stand and what comes next (hand-over: start here)

> **This section goes stale.** Written 2026-10-15 against commit `06cc706` (which carries the M3-2p pipeline cache and the
> toolchain.cmd unification); for the detail and the conclusions, the authority is [ANDROID_PLAN.md](ANDROID_PLAN.md), its
> "7. Next step" and "8. Progress log".

### Where it stops

- **M1 is done**: the host loads the patched artifact on Android, takes over the service layer, finishes SDK
  initialisation and reaches `nnMain`.
- **Item 1 of the "9. Optimisation list" landed (2026-10-15)**: one `VkPipelineCache` shared by graphics and compute,
  written to disk and loaded back on the next launch ("M3-2p"). Four device runs: with no cache it writes 0.6 MB; with one
  it loads 636 KB and keeps appending; a corrupt file and one with a fake device id were both rejected. **It also brought
  out a device fact**: MIUI treats this package as a game and SIGKILLs it on backgrounding -- persisting anything means
  writing **periodically in the foreground** (see the traps below).
- **The rendering chain's warnings are cleared**: the descriptor blanks (UBO name number minus 3, the storage buffer
  derived from `cb_offset`, buffer textures) and the vertex-attribute test (two independent faults; see "M3-2k" and
  "M3-2l").
- **The blocker**: draws go out (about 400k per run, 78-84 graphics pipelines, zero failures) yet the **render targets
  read back all zero**. `DM_NVN_SHOT_SCAN` is now **level-complete** (levels 0/1/2 each once), and on the frames it can
  reach (50/54/57) **every one reads `brightness mean 0.0`** (part six of "M3-2m"). In other words the picture has not
  been written anywhere readable, or the reading side still hides something.
- **One normal thing**: this device **stalls at 55-60 presents** (stage `CharacterCreate:Hover:Barbarian`, occasionally
  `MainMenuHD:CheckUpdateLinks`). That is a known scene, not a crash, and the judgement has to be read from "what it is
  doing across several runs".
- **The shape of milestone B has been measured (2026-10-15, "M3-2r")**: the main thread polls a **task-completion bitmap**
  at `main+0x15ee…` and that bitmap is **always 0** (it waits for the low 22 bits to be set) => **not one of the task
  graph's 22 tasks ever completes**; at the same moment worker thread `0x1005` finishes a Granny decode and then **waits on
  a lock held by the main thread** (waiting 15 s, and that lock **was never handed off by the kernel**). So the next
  question is "**why are the tasks never dispatched**", not "what is wrong with the locks" (two lock-semantics changes were
  both ruled out by measurement).
- **The same evening, "M3-2s" walked this machinery through (and the wording has to follow)**: that object carries **three**
  32-bit bitmaps (`+0x3320` dep source / `+0x3324` to-dispatch / `+0x3328` completed); the setter is **one place only**, the
  tail of the task wrapper (`main+0x15f0e6c`); the arming site is `main+0x1636f50` (writes the low `count` bits, then clears
  the completion bitmap, then calls the dispatcher `0x15ee598`, then the poll `0x15ee898`). On the device all three bitmaps
  read **0** while the main thread's stack carries a **live** wrapper frame (it is running one of this graph's tasks), so
  "**the 22 tasks were never dispatched**" **does not hold**; `0x1005` really is inside one of this graph's tasks and waits
  on a lock word of `0x40001001` (owner `0x1001` = the main thread). **Next: a time series of the three bitmaps**, to
  separate "armed 0 bits" from "completed and then cleared" (`+0x3328` has another clearer, `main+0x15eef90`, the tail of the
  other graph runner). Details and readings in "M3-2s".
- **One sporadic thing**: about once per run, `【红】一次性提交等待失败 VkResult -4` (`VK_ERROR_DEVICE_LOST`), followed by a
  hundred-plus `贴图上传失败` lines. **Unrelated to command-buffer sharing** (ruled out; "M3-2o").

### Next steps (detail in "7. Next step" of ANDROID_PLAN)

1. **Milestone B: the guest stall** (**first -- the main blocker**) -- **the shape has been measured (2026-10-15, "M3-2r")**:
   the main thread polls a task-completion bitmap at `main+0x15ee…` and that bitmap is **always 0** (22 tasks, none done).
   **Later the same day "M3-2s" finished this route**: the three bitmaps, the arming site (`main+0x1636f50`) and the single
   setter (the wrapper's tail, `main+0x15f0e6c`) are all located, and the device readings plus one contradiction (all
   bitmaps 0 versus a live wrapper frame on the main thread's stack) are written up. **Next: a time series of the three
   bitmaps** (sample on every main-thread SVC, log on change), to separate "armed 0 bits" from "completed and then cleared".
   How to read the samples, the new instruments, and the two lock hypotheses that measurement killed are all in "M3-2r",
   with this round's instruments and next steps in "M3-2s"; the one-shot stall sampler and the wait-point dump live in
   "M3 step 3" (the old label "M3-2g" is dangling). It covers the most ground: the picture is not found before the stall.
2. **That sporadic device loss** (**likely the same source as item 1; watch for it while doing item 1**) -- the next
   candidate is "**who loses it first**". What to measure: what else was on the queue at that moment, whether the device
   had already been lost on the guest's side, and the `vkQueueSubmit` return value with its context.
3. ~~**Item 2 of the "9. Optimisation list"** (preferring `HOST_CACHED` for the zero-copy memory type)~~ -- **measured and
   ruled out (2026-10-15, "M3-2q")**: type 6 **can** export an fd (256 MB / 512 MB / 3968 MB all pass, `mmap` at the chosen
   address and both directions too), but it means **stepping outside the buffer's declared allowed types** (an external
   buffer reports `0x13`, which excludes type 6 -- outside the spec, tolerated by the driver), and the A/B shows **no
   measurable CPU-time difference** (under 3% within the same mode, against a +-100% run-to-run spread), so **type 4 stays**
   and the `DM_ZC_CACHED=1` switch is kept (off by default) for a one-command re-check elsewhere. (Item 1 has landed, see
   "M3-2p"; the "time to `nnMain`" A/B waits until item 1 has moved.)

### How to run one round (the smallest loop on a new machine)

Paths and tooling on the PC side are in sections 2 and 3; this is only **run once and get a reading out**:

```powershell
$adb = 'C:\Android\Sdk\platform-tools\adb.exe'
cd <repository>

# 1) build the package (precondition: the three generated headers exist, see section 2)
dm\android\apk\build_apk.cmd

# 2) push the parameters, install, run, pull the log back
& $adb push .work\dm.conf /sdcard/Android/data/ai.d2rm.dm/files/dm.conf
& $adb install -r .work\apk-build\dm.apk
& $adb shell am force-stop ai.d2rm.dm
& $adb shell am start -n ai.d2rm.dm/android.app.NativeActivity
Start-Sleep -Seconds 50
& $adb pull /sdcard/Android/data/ai.d2rm.dm/files/logs/latest.log .work\logs\run.log
```

**Android cannot be given environment variables per launch** (`am start` passes no env), so the parameters live in a file
adb can push: `/sdcard/Android/data/ai.d2rm.dm/files/dm.conf`, one `key=value` per line (`#` starts a comment). **Pushing
it and restarting the app is enough -- no reinstall.** The current one:

```
DM_WIN_SRC=28          # the viewer's source serial (0 = off, back to the test pattern)
DM_WIN_LEVEL=1         # which mip level to read (level 0 is all zero, and so is level 1; see above)
DM_WATCH_STALL=0       # NB: this is NOT "log without killing" -- a value <= 0 falls back to 15 s (crash.c:347-354), so a 15 s stall is declared dead and exits
DM_NVN_SHOT_SCAN=50,54,57:/sdcard/Android/data/ai.d2rm.dm/files/scan/
DM_PASS_AREA=1         # render-area trace (measure only; =2 swaps in the attachment size as a control)
```

The other switches are whatever `getenv` picks up in `dm/android/` (`DM_NVN_SHOT`, `DM_NVN_SHOT_ALL`, `DM_GFX_PROBE`,
`DM_NO_DEPTH_TEST`, `DM_NO_TEXEL`, `DM_NVN_SPV_DIR`, `DM_NVN_FACTS`, `DM_ROOT/DM_ROMFS/DM_SAVE`, `DM_PIPE_CACHE`).

**The pipeline cache** (since 2026-10-15) lands in the app's **internal** directory,
`/data/user/0/ai.d2rm.dm/files/pipecache.bin`; adb only sees it through `run-as`
(`adb shell run-as ai.d2rm.dm ls -l files`), while the console host uses `<DM_ROOT>/pipecache.bin`. For a cold-compile
comparison add a `DM_PIPE_CACHE=0` line (or point it somewhere else in the config). It is written by the **one-second
foreground heartbeat**, not "on exit" -- the reason is one of the traps below.

Where the output lands:

| Where | What |
|---|---|
| `/sdcard/Android/data/ai.d2rm.dm/files/logs/latest.log` | this run's log (the previous one is `prev.log`) |
| `/sdcard/Android/data/ai.d2rm.dm/files/scan/` | PNGs the level-by-level scan wrote (only ones with content) |
| `/data/local/tmp/dm/` | the run-time data (`patched` / `spv` / `romfs`), **read-only**, see section 5 |
| `/data/user/0/ai.d2rm.dm/files/pipecache.bin` | the pipeline cache (app-internal, visible through `run-as`; see above) |

**Device screenshots**: when a PNG has to land on disk, **do not use PowerShell's `>`** (you get UTF-16 and the file is
dead) -- write a `.cmd` and use cmd's redirect, the same rule as section 6.

### Traps hit this round (relevant to carrying on)

- **Scan points have to sit on reachable frames**: this device stalls at 55-60 presents, so `DM_NVN_SHOT_SCAN=300,340`
  will **never fire**, and the log cannot tell "scanned" from "never got there". Use 50/54/57 and the like.
- **Changing a log line's text changes the measuring stick**: after renaming the line in `one_end` from
  `上传命令等待失败` to `一次性提交等待失败`, a tally still counting the old string read "0 lines", and only just failed to
  record "not fixed" as "fixed". **Change the script that reads it along with the text, or count both strings.**
- **The readback has exactly one lock -- do not add it elsewhere**: the viewer (front-end thread) and present/scan (guest
  thread) start readbacks **at the same time**, hence `g_one_mu` over that section ("M3-2n"). But **the upload path must
  not take it** -- the caller `tex_of` holds `g_mu` when it uploads, so taking it is a **lock-order inversion** that shows
  on the device as "**a stall at boot with zero presents**" (the watchdog reporting only `beat 0`), looking exactly like a
  hung game. Uploads now have **their own command buffer** (`g_cb_up`, "M3-2o") and need no lock.
- **Before adding a lock, list who holds what when they call in**: that deadlock came from skipping this, and the check
  that settled it was "revert that one site and presents return to normal immediately".
- **"Going to the background" means "being killed" on this machine**: MIUI treats this package as a game (logcat
  `ANDR-PWR-OPT: GAMEPOWEROPT: isGame()`) and sends `libc: kill: send 9` (SIGKILL) right after HOME, so **`TERM_WINDOW` /
  `DESTROY` / `atexit` never run once** (measured twice: the pid disappears within 6 seconds and the log holds no
  `窗口那一套已拆`). Any design that persists "on exit" therefore does not work here -- this is exactly how the pipeline
  cache was found out and moved to a **one-second foreground write** ("M3-2p"). Note also that
  `adb shell input keyevent` is refused (`SecurityException: requires INJECT_EVENTS`); to background the app, start the
  launcher explicitly: `adb shell am start -n com.miui.home/.launcher.Launcher`.
- **`DM_WATCH_STALL=0` is *not* "log without killing"** (the comment in `dm.conf` above is wrong, and this round hit it for
  real): `crash.c:347-354` computes `g_watch_stall = stall && atoi(stall) > 0 ? (unsigned)atoi(stall) : 15`, so a value
  <= 0 **falls back to 15 seconds** -- writing 0 means "stalled for 15 s -> declared dead -> full dump -> `_exit`" (that is
  why the process vanished during the A/B in "M3-2q": **not a crash and not MIUI**, the log ends on the "wait points" line
  plus that thread dump). To turn the watchdog off entirely, **leave the line out** (`if (watch || stall)` is what starts
  it); to observe a longer run, give it a large value.
- **MIUI refuses `adb install -r`**: it reports `INSTALL_FAILED_USER_RESTRICTED: Install canceled by user` (the on-device
  confirmation is cancelled or the permission is off), and `install_apk.cmd` uses exactly that. Installing from the device
  side works at once: `adb push .work\apk-build\dm.apk /data/local/tmp/dm.apk` then
  `adb shell pm install -r /data/local/tmp/dm.apk`.
- **This machine's cmd runs with codepage 936 (GBK) while the repo's `.cmd` files are UTF-8 without BOM**, so running
  `build_apk.cmd` directly gets shredded by its own Chinese comments: a pile of `'xxx' is not recognized` plus
  `… was unexpected at this time`, and no artifact is produced (that is how the first build of "M3-2s" failed).
  Run `chcp 65001` first and then `call` it (`.work\build_u8.cmd` is that two-line wrapper; **the scripts themselves need
  no change** -- this is a property of the local console). How to tell: on failure `build.err` holds half a Chinese
  comment; on success the `== 1/5 … ==` line looks like mojibake in the log but `BUILD_RC=0` and the APK appears.
