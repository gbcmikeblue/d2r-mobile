# Moving to another computer (cross-machine development and debugging)

中文：[CROSS_MACHINE.zh-CN.md](CROSS_MACHINE.zh-CN.md)

This page covers one thing: when Android-side development and debugging move to another computer, which files besides the `git clone`d code have to come along, where they come from, and what "restored" looks like. The code itself is small (`.work/`, `.tools/` and `.venv/` are all in `.gitignore`, hence outside the repository); the thing that actually needs moving is the data.

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

## 4. How to tell it is restored

1. `dm\android\apk\build_apk.cmd` produces a package (`.work\apk-build\dm.apk`) -- which also proves the NDK block was edited correctly.
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
- `origin` is already the fork `git@github.com:gbcmikeblue/d2r-mobile.git`, and the `android` branch tracks `origin/android`. SSH uses the `gbcmikeblue` key, so a new machine has to generate its own and add it to the GitHub account.
- To update the staging area, just `adb push` over it; to clear it, `adb shell rm -rf /sdcard/d2rm/pc` and push it back later.
- This staging copy is **not** the data the app runs on: at run time the app reads `/data/local/tmp/dm`. Do not mix the two up.
