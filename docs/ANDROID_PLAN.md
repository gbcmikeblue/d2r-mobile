# Android port: implementation plan and task list

English: [ANDROID_PLAN.zh-CN.md](ANDROID_PLAN.zh-CN.md)

This page breaks "porting D2R Mobile to Android" down into an actionable, checkable task list, so it can be confirmed item by item and divided up. For the technical feasibility case see [ANDROID.md](ANDROID.md); for the plain-language version see [ANDROID_PLAIN.md](ANDROID_PLAIN.md).

**Current stage: preparation (no Android-side code written yet).** The checkboxes below reflect the actual state of this machine, not intentions.

## 1. Starting-point snapshot

### Done

- [x] Feasibility analysis in the repository (`docs/ANDROID.*`)
- [x] `android` branch created
- [x] **Game files ready** (Switch 1.0.34.0)
  - ExeFS and RomFS restored from a self-supplied `nsz` / `nsp` plus `prod.keys`
  - SHA-256 of `main`, `sdk`, `subsdk0` **matches 3.3.94012 item by item**
  - All 18 resource archives **present**
  - Location: `.work/data/switch/3.3.94012/Update/` (listed in `.gitignore`)
- [x] **Local toolchain**
  - Python 3.10.11 plus `pillow numpy lz4 capstone unicorn nsz` (`.venv/`)
  - hactool (built from source, `.tools/hactool/hactool.exe`)
  - .NET SDK 10.0.402 (for the shader translation tool)
  - LLVM / clang 23.1.3
  - JDK Temurin 21
  - CMake 4.4.4

### Done (continued)

- [x] Android Studio installed
- [x] Android SDK / NDK installed at `C:\Android\Sdk` (pure ASCII path, avoiding the non-ASCII user name)
  - `platform-tools` (`adb` 1.0.41 / 37.0.1)
  - `platforms;android-36` (Android 16)
  - `build-tools;36.0.0`
  - `cmake;3.31.6`
  - `ndk;30.0.16248370` (clang 21.0.0, runs)

### Done (M0 wrap-up)

- [x] Android device prepared for debugging (developer options, USB debugging, `adb devices` sees it)
- [x] Built a minimal native program with the NDK and ran it on the device (device printed `hello from Android NDK` / `pointer size: 8 bytes`)

**Target device (measured over `adb`)**

| Item | Value |
|---|---|
| Brand / model | Redmi K90 (`2510DRK44C`, codename `annibale`), serial `8b1f1e50` |
| OS | **Android 16** (`ro.build.version.release=16`, SDK **36**) |
| SoC | `SM8750` (Snapdragon 8 Elite) |
| GPU / Vulkan driver | Adreno (`ro.hardware.vulkan=adreno`) |
| CPU ABI | `arm64-v8a` |

## 2. Overall route

| Stage | Goal | One-line acceptance |
|---|---|---|
| M0 | Environment ready | The NDK builds a native program that runs on a real device |
| M1 | **Host runs headless** | On Android, load the rewritten output, execute `nnMain`, and see `UNIMPL` / `【红】` in the log |
| M2 | **Minimal Vulkan replay layer** | A picture appears (a clear first, then the main menu) |
| M3 | Front end + touch + audio | Create a character, enter a game, sound works |
| M4 | Wrap-up | Gamepad, save sync, diagnostics, performance tuning |

Principle: **do not rewrite gameplay**; reuse the existing host and replace only the two platform-facing sides (front end and graphics backend).

## 3. Task list

### M0 — environment ready

- [x] Install an Android SDK Platform (`android-36`, matching the device's Android 16)
- [x] Install the NDK (`30.0.16248370`)
- [x] Install platform-tools and confirm `adb` works
- [x] Device: enable developer options and USB debugging; `adb devices` sees it
- [x] Record target-device info: model, SoC, Android version, GPU driver version (table above)
- [x] Build a minimal native program with the NDK and run it on the device
- [x] **Acceptance**: a hello-level native program runs on the device — **M0 done**

### Recon A — compile the host layer with the NDK (no new features; just quantify reuse)

- [x] Create a minimal CMake project that compiles the platform-independent sources of `dm/host/`
- [x] First compile; collect the full list of errors
- [x] Classify the errors: Apple-only API / POSIX differences / missing headers / other
- [x] Produce a "host-layer reuse assessment" conclusion
- [x] **Acceptance**: a list of "which files change, and at how many points"

**Result (measured with NDK r30.0.16248370 / clang 21.0.0, `arm64-v8a`, `android-31`)**

**87 translation units** were compiled (70 `dm/host/*.c` + 8 `dm/gfx` + 5 libdeflate + 1 touch-decision file + 3 assembly):

| Outcome | Count |
|---|---|
| **Compiles as is, zero changes** | **56** |
| C files with platform differences | 29 files, **31 sites** (1–4 each) |
| Assembly files to rewrite | 2 (`nvn_thunks.S` 1047 sites, `nvn_entry.S` 6 sites) |

**The 31 C sites — classes and fixes**

| Class | Sites | Forms | Fix |
|---|---|---|---|
| Apple-only headers missing | 17 | `TargetConditionals.h`(7), `mach/mach.h`(3), `mach/mach_time.h`(3), `CommonCrypto/CommonDigest.h`(2), `sys/sysctl.h`(1), `sys/clonefile.h`(1) | add an `__ANDROID__` branch onto a platform equivalent |
| macOS-only APIs / structs | 14 | `st_mtimespec`(6), `clock_gettime_nsec_np`(3), `CLOCK_UPTIME_RAW`(2), `pthread_get_stack{addr,size}_np`(2), `F_GETPATH`(1) | see below |

| Original | Android equivalent |
|---|---|
| `struct stat.st_mtimespec` | `st_mtim` (POSIX) |
| `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` | `clock_gettime(CLOCK_MONOTONIC)` |
| `mach/mach_time.h` | `clock_gettime` |
| `pthread_get_stacksize_np` / `pthread_get_stackaddr_np` | `pthread_getattr_np` + `pthread_attr_getstack` |
| `F_GETPATH` | `/proc/self/fd/<fd>` + `readlink` |
| `sys/sysctl.h` | `/proc` |
| `clonefile()` | `copy_file_range` |
| `CommonCrypto/CommonDigest.h` | an in-tree implementation or another library |

**Assembly**

`nvn_thunks.S` and `nvn_entry.S` use **Mach-O assembly syntax** (`@PAGEOFF`), which does not hold for Android's **ELF**; they need the AArch64 ELF form (`#:lo12:` and friends). Mechanical work; `nvn_thunks.S` is generated (`dm/gfx/tools/gen_protos.py`), so the generator is what changes.

**In one line: the host layer's C code is roughly 95%+ reusable.** Of 87 translation units, 56 compile **unchanged**, and the other 29 come to 31 sites that are all "swap one API" level. This matches the feasibility analysis ("most of the host layer is platform-independent POSIX/C") and is **better than expected**. The real heavyweight remains the graphics layer (the M2 Vulkan replay layer).

**Noted along the way (Windows environment trap)**: `gen_vault_table.py` / `gen_hud_exp.py` / `gen_bright_levels.py` need `PYTHONIOENCODING=utf-8`. Otherwise Python encodes stdout as GBK on Windows, hits the `U+2212` in the scripts' comments, raises `UnicodeEncodeError` and exits midway, producing **truncated headers** (which shows up as a pile of undefined `vt_row`).

### Recon B — land the shader translation chain

- [ ] Build `dm/gfx/tools/dmsh_xlate` with the .NET SDK
- [ ] Run shader translation over the game files in `.work/`, producing SPIR-V
- [ ] Spot-check the output and confirm it is **Vulkan-targeted** SPIR-V (`spirv-dis` / `spirv-val`)
- [ ] Record the output layout and the reflection json format (the Vulkan replay needs them)
- [ ] **Acceptance**: "NVN shader → Vulkan SPIR-V" reproduces on this machine

### M1 — host runs headless

- [ ] Add `dm/android/` and set up the project skeleton (form to be decided)
- [ ] Write a minimal `front_android.*`: `front_run` runs the guest on the current thread, `front_metal_layer` returns NULL
- [ ] **Platform-independence work**: add `__ANDROID__` branches beside Apple-specific sections of the host
  - [ ] `hle_fs.c`: `clonefile()` / `xattr` → `copy_file_range` / a replacement table
  - [ ] `hle_audio.c`: start with the **null sink** (`null_sink`, plain POSIX, reusable as is)
  - [ ] `crash.c` / `diag.c`: mach, signal stacks → `/proc`, `backtrace`, or weaken for now
  - [ ] `dm_i18n.c`: CoreFoundation → standard C
  - [ ] `dm_padmode.c`, `diag.c`: CommonCrypto → an in-tree implementation or another library
  - [ ] `memstat.m`: mach → drop or rewrite against `/proc`
  - [ ] `loader.c`, `main.c`: an Android branch beside the `TARGET_OS_IPHONE` one
- [ ] Load the rewritten output: ship `patched.bin` in the app assets and map it for execution (mind `PROT_EXEC` and W^X)
- [ ] Run `nnMain` and watch the log
- [ ] **Acceptance**: the APK installs on the device, the screen is black, but the log shows the game's `UNIMPL` lines

### M2 — minimal Vulkan replay layer (the largest item)

- [ ] Vulkan skeleton: device, queue, `ANativeWindow` swapchain
- [ ] MemoryPool → `VkDeviceMemory` plus host-visible/coherent buffers (validate the "GPU address = CPU address" zero-copy assumption) ★risk
- [ ] Map Texture / Sampler / TextureView
- [ ] Program: build `VkShaderModule` / pipelines from SPIR-V
- [ ] Minimal command set: `SetRenderTargets`, clear, `DrawArrays`, present
- [ ] **Acceptance**: a picture appears (a solid clear first, then progressively up to the main menu)

### M3 — front end + touch + audio

- [ ] `front_run`: Android window and main loop (`NativeActivity` / `SurfaceView`)
- [ ] `front_text_input`: system software keyboard
- [ ] Touch: reuse `dm_touch_controls.c` (plain C); rewrite input capture and widget rendering
- [ ] Audio: AAudio / Oboe for real sound
- [ ] `front_save_failed`: save-failure prompt (a Toast)
- [ ] Lifecycle: background/foreground, `dm_audio_suspend` / `dm_gfx_suspend`
- [ ] **Acceptance**: create a character, enter a game, sound works

### M4 — wrap-up

- [ ] Gamepad (Android `InputDevice`)
- [ ] Cloud save sync (replacing `cloud_ios.m`)
- [ ] Diagnostics and crash handling
- [ ] Performance tuning: frame rate and power draw (match iOS's 60 fps / 3.8 W)

### Cross-cutting

- [ ] Documentation (Chinese and English) accompanies each finished piece
- [ ] An Android entry point for the host unit tests (the counterpart of `make -C dm/host/tests`)
- [ ] Follow the project rules: **no game content in the repository** (game files and generated data stay out)
- [ ] New on-screen text goes into `dm/i18n/ui_text.tsv`

## 4. Decisions taken

| # | Item | Decision |
|---|---|---|
| 1 | Project form | **Android Studio project** (Gradle + CMake + JNI) |
| 2 | Target device | **Redmi K90** (Snapdragon 8 Elite, Adreno GPU, 6.59" 1.5K 120 Hz; check "About phone" for the exact OS build) |
| 3 | Android SDK Platform | `android-36` (Android 16, matching the device) |
| 4 | NDK version | latest stable (r27 or newer) |
| 5 | How to load the rewritten output | app assets + `mmap` for execution |
| 6 | Minimum Android version | **Android 12 (API 31)**, recommended, see below |

Android 12 / API 31 is the recommendation: Vulkan 1.1 and up is mandatory by then, `AHardwareBuffer` and `AAudio` are mature, and the storage and permission models are modern, which removes a large pile of legacy branches. Drop to API 29 (Android 10) for wider reach, or raise to API 33 (Android 13) for an easier life.

## 5. Main risks (the ones that can block progress)

| Risk | Why | Mitigation |
|---|---|---|
| **Unified memory / zero copy** | The Metal side assumes "GPU address = CPU address"; Vulkan needs host-visible + coherent memory | Validate it first thing in M2; if it fails, the memory model changes |
| **TBDR / render-pass merging** | `nvn_mtl.m`'s pass merging and attachment load/store were validated on macOS; phone GPU architectures differ | Start with the plainest path in M2 (the equivalent of `DM_NVN_TBDR=0`), optimize later |
| **Size of `nvn_mtl.m`** | About 5,800 lines of Metal, the single largest piece | Port by NVN function group, with equivalence tests in the spirit of `dm/gfx/tests` |
| **Performance** | The 60 fps on iOS comes from deep tuning; it cannot be assumed to carry over | A dedicated tuning stage in M4 |

## 6. Working on Windows (encoding and commands)

The port is currently being driven from a Windows machine. These are the traps already hit, and the rules that follow from them.

### Encoding

- **Always set `PYTHONIOENCODING=utf-8` for Python scripts.** The Windows console code page is GBK, and a character such as `U+2212` in a script's comments raises `UnicodeEncodeError` and **aborts the script midway** (seen as truncated generated headers).
- **Redirect to a file with cmd's `>`** (preserves raw bytes); do not use a PowerShell pipeline plus `Out-File` (it changes encoding and line endings).
- **Read files with an explicit encoding**: PowerShell `-Encoding`, or `[System.IO.File]::ReadAllLines()`.
- **Keep every path pure ASCII.** The user directory on this machine contains non-ASCII characters (`C:\Users\ZhuanZ（无密码）`), which has already caused two problems:
  - `hactool` converts `HOME` / `USERPROFILE` to UTF-16 and crashes when that fails → set an ASCII `HOME` before running it;
  - do not put generated headers, caches or logs under the user directory.
  - Agreed locations: the SDK at `C:\Android\Sdk`, tooling and workspace at `d:\yiyi\d2r-mobile\.tools` and `.work`.

### Commands

- **Long tasks must show progress**, never sit silent: `curl` without `--silent` for downloads, `ninja`'s built-in `[n/m]` progress for builds, `adb push/pull` for transfers.
- **Write multi-step commands as `.bat` / `.cmd` and run those**, rather than assembling `cmd /c "..."` inside PowerShell (quote nesting breaks easily).
- To **collect all errors**, use `ninja -k 0` and redirect to a log, then analyse it.
- **`sdkmanager` is deprecated** and cannot accept licences without a TTY; use `android sdk install` instead (it accepts the ToS automatically after showing it).

## 7. Next step

Proceed in the order on this page: finish the rest of M0 (SDK/NDK + device), then Recon A and Recon B, then M1.
After each milestone, tick it off here and record the conclusion.
