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

### Hardcoded paths that must be edited after installing

`dm\android\apk\build_apk.cmd:22-27` and `dm\android\apk\install_apk.cmd:13` hardcode the previous machine's absolute paths (NDK / JDK / build-tools / CMake / ninja / android.jar / adb), so both blocks have to be edited on a new machine. `.work\apk-build` and `.work\apk-keys` are derived relative to the scripts themselves and need no change.

**Measured (second machine)**: only the **JDK line** had to change (`build_apk.cmd:23`) -- install the NDK / build-tools / CMake / android.jar under `C:\Android\Sdk` as the table above says and they land exactly where the document points; the adb path in `install_apk.cmd:13` likewise. With that one edit, `build_apk.cmd` produced a package straight away.

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

> **This section goes stale.** Written 2026-10-14 against commit `ea71aab`; for the detail and the conclusions, the
> authority is [ANDROID_PLAN.md](ANDROID_PLAN.md), its "7. Next step" and "8. Progress log".

### Where it stops

- **M1 is done**: the host loads the patched artifact on Android, takes over the service layer, finishes SDK
  initialisation and reaches `nnMain`.
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
- **One sporadic thing**: about once per run, `【红】一次性提交等待失败 VkResult -4` (`VK_ERROR_DEVICE_LOST`), followed by a
  hundred-plus `贴图上传失败` lines. **Unrelated to command-buffer sharing** (ruled out; "M3-2o").

### Next steps (detail in "7. Next step" of ANDROID_PLAN)

1. **Chase that device loss**: the next candidate is "**who loses it first**", the guest's own commands or this one
   one-shot submit of ours. What to measure: what else was on the queue at that moment, whether the guest had already
   lost the device on its side, and the `vkQueueSubmit` return value with its context.
2. **Keep hunting the picture**: the level-by-level scan is in place but only on reachable frames; reaching a later frame
   first needs "why does it stop here" answered (the normal thing above).
3. **Items 1 and 2 of the "9. Optimisation list"** (persisting a `VkPipelineCache`, preferring `HOST_CACHED` for the
   zero-copy memory type) -- unrelated to the blocker, doable whenever.

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
DM_WATCH_STALL=0       # watchdog logs without killing (unset means it does not look at all)
DM_NVN_SHOT_SCAN=50,54,57:/sdcard/Android/data/ai.d2rm.dm/files/scan/
DM_PASS_AREA=1         # render-area trace (measure only; =2 swaps in the attachment size as a control)
```

The other switches are whatever `getenv` picks up in `dm/android/` (`DM_NVN_SHOT`, `DM_NVN_SHOT_ALL`, `DM_GFX_PROBE`,
`DM_NO_DEPTH_TEST`, `DM_NO_TEXEL`, `DM_NVN_SPV_DIR`, `DM_NVN_FACTS`, `DM_ROOT/DM_ROMFS/DM_SAVE`).

Where the output lands:

| Where | What |
|---|---|
| `/sdcard/Android/data/ai.d2rm.dm/files/logs/latest.log` | this run's log (the previous one is `prev.log`) |
| `/sdcard/Android/data/ai.d2rm.dm/files/scan/` | PNGs the level-by-level scan wrote (only ones with content) |
| `/data/local/tmp/dm/` | the run-time data (`patched` / `spv` / `romfs`), **read-only**, see section 5 |

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
