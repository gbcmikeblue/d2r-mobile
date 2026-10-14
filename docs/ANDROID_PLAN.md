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

- [x] Build `dm/gfx/tools/dmsh_xlate` with the .NET SDK
- [x] Run shader translation over the game files in `.work/`, producing SPIR-V
- [x] Confirm the output is **Vulkan-targeted** SPIR-V (version and magic)
- [x] Record the output layout and the reflection json format (the Vulkan replay needs them)
- [x] **Acceptance**: "NVN shader → Vulkan SPIR-V" reproduces on this machine

**Result (measured)**

- `dotnet publish dm/gfx/tools/dmsh_xlate -c Release` **succeeds** (.NET SDK 10.0.402 plus the in-repo Ryujinx libraries; `Spv.Generator` / `Ryujinx.Common` / `Ryujinx.Graphics.Shader` / `dmsh_xlate` all restore and build).
- **162 shader packages** extracted from the game RomFS (vs 43 / ps 96 / cs 21 / ds 1 / hs 1).
- Splitting each package into records on `BTCDBTCD` gives **10847 variants**; de-duplicated by control-segment `sha256[:16]` that is **6923 unique keys**.
- `dmsh_xlate`: **10847 translated, 0 failed**.
- Output: **6923 `.spv` files** (magic `0x07230203` correct in every one, 0 anomalies) plus 6923 `.res.json` reflection files.
- **SPIR-V version 1.3** (a Vulkan 1.1 target; the Adreno driver on the Redmi K90 supports newer and is backward compatible).

**Why it matters**: this is the most favourable piece of the whole plan, and it is now **banked on this machine** — when the Android Vulkan replay layer is written, 6923 ready-made SPIR-V shaders are available with **no runtime translation at all**. That was one of the key reasons Android looked feasible in the first place.

Only the **front half** of the chain was run here (NVN → SPIR-V). The back half (`spirv-cross` → MSL → `metallib`) is Apple-specific and is not needed on Android.

The output lives in `.work/spv/` (gitignored; not committed).

### M1 — host runs headless

- [x] Add `dm/android/` and set up the project skeleton
- [ ] Write a minimal `front_android.*`: `front_run` runs the guest on the current thread, `front_metal_layer` returns NULL
- [x] **Platform-independence work**: add `__ANDROID__` branches beside Apple-specific sections of the host
  - [x] `hle_fs.c`: xattr argument counts (Bionic takes 4 / 5 / 2 where macOS takes 6 / 6 / 3)
  - [x] `hle_audio.c`: every AudioUnit call becomes a no-op and `au` stays NULL, so the **null sink** always runs (no real audio in M1)
  - [x] `crash.c` / `diag.c`: the new `dm/host/dm_uctx.h` unifies register access behind `DM_UC_*` macros
  - [x] `hle_lifecycle.c`: two ordinary threads stand in for GCD (a 1.5-second fallback plus `sigwait` for SIGTERM)
  - [x] `nvn_thunks.S` / `nvn_entry.S`: Mach-O → dual Mach-O/ELF, with the platform difference emitted as two macro branches by `gen_protos.py`
  - [x] everything else (mach, CommonCrypto, dispatch semaphores, Apple headers) is covered by the compat layer in `dm/android/compat/`, force-included
- [ ] Load the rewritten output: ship `patched.bin` in the app assets and map it for execution (mind `PROT_EXEC` and W^X)
- [ ] Run `nnMain` and watch the log
- [ ] **Acceptance**: the APK installs on the device, the screen is black, but the log shows the game's `UNIMPL` lines

**M1 progress log (in progress, 2026-10-14)**

- [x] **Rewritten output produced**: `dm/tools/patch_nso.py` ran all three product kinds across all three modules; code name `3.3.94012-r41`, and `keypage` (the one M1 uses) has all 25 files.
- [x] **Android compatibility layer**: `dm/android/compat/` (12 headers plus one `.c`, force-included). The host layer went from **31 files / 1090 error lines** down to **0**.
- [x] **The host compiles**: `cmake -S dm/android -B build/android` with the NDK toolchain, then `ninja` → **0 errors**, producing `libdmhost_android.a` (about 9.0 MB, 88 object files).
- [ ] Not started yet: wiring the loader, a minimal `front_android`, linking an executable, running on the device.

**Follow-ups (skipped during M1, must come back after acceptance)**

- MD5 / SHA1 / SHA256 in the compat layer are **placeholders** (fixed-length zero output), so `hle_tact.c`'s sample checks and `dm_padmode.c`'s original-string matching will be wrong;
- Mach-O section lookup always reports "not found"; `F_GETPATH` and `sysctl(KERN_PROC_PID)` are unimplemented;
- Android's SIGSEGV carries no ESR, so the read/write bit is assumed to be a read (this affects the page-fault hook's decision);
- The build currently passes `-w` to silence warnings; turn them on after M1's acceptance.

### M1 device verification log (2026-10-14, in progress)

The host now runs on the device all the way to "load → relocate → enter SDK init → print the game's UNIMPL", which is M1's acceptance criterion:

```
[dm] thread slot measurement: first key 0x80000002 at tpidr_el0+520, key stride 16 bytes
[dm] main: base 0x78fc7e5000 size 0x33dc000
[dm] sdk:  base ... size 0xe80000
[dm] main: reloc relative=60560 abs/glob=2109 jump=615
[dm] force: 80 entries all hit
[dm] >>> nnosInitialize
[dm] UNIMPL nnosGetDefaultUserExceptionStackForApplication (...)      <- the acceptance point
[dm] >>> nninitInitializeSdkModule
[dm] SVC 0x1f ... (placeholder returned success)
```

Two **architectural** platform differences had to be fixed for this. Neither is visible by reading the code; both only surfaced on the device.

**① The platform convention for the thread slot base** (`26158e2`)

The rewriter keeps the guest's `mrs xN, tpidrro_el0` as is and indexes the host's pthread TSD through it plus a key-page offset. That holds on Apple; on Android it breaks at the first step:

| | Apple | Android (bionic) |
|---|---|---|
| `tpidrro_el0` | = the pthread TSD base | **always 0, read-only at EL0, userspace cannot set it** |
| TSD base | `tpidrro_el0` | `tpidr_el0` |
| `pthread_key_t` | the slot number itself (small) | `0x80000000 \| index` (e.g. `0x80000001`) |
| byte offset of the value | `key*8` | keys 1/2/3 -> +88 / +104 / +120 (each key takes two pointer-sized slots) |

The fix: the rewriter gained `DM_TLSREG` (default `apple`; `linux` switches the slot access's `mrs` target to `tpidr_el0` -- all three modules go through one `MRS_TPIDRRO()`, so that is the only change); the host's `loader.c` computes the key-page offsets per platform, and on Android it does not hard-code bionic's encoding but measures "offset of the first key" and "key stride" at startup with sentinel keys and extrapolates. `tsd_check` validates with the platform's base register and, on mismatch, says which `DM_TLSREG` the build output should have been made with.

**Note: the Android and iOS build outputs are therefore no longer byte-identical** (they differ only in the target register of that one `mrs`).

**② The registry sections need a different read on ELF** (`7ed17c1`)

The `HLE` / `HLE_FORCE` tables used to be read with `getsectiondata(&_mh_execute_header, "__DATA", "swhle"/"swforce", ...)` -- a Mach-O section lookup. On Android it returns NULL, so both tables had 0 entries and not one of the hundred-odd service-layer take-overs (`nndiagStartup`, `oe::Initialize`, `fs::InitializeForApplication`, `pctl::Initialize`, ...) applied; the SDK ran its own implementations, tried to reach Horizon service processes that do not exist, and panicked. The fix: section names per platform (`__DATA,swhle` vs `swhle`), reading via ELF's `__start_<name>` / `__stop_<name>`, and **`-Wl,--no-gc-sections` at link time** (the NDK enables GC by default and these two sections are reached only through `__start_/__stop_`, so they were dropped entirely; measured, without the flag neither appears in the section table at all). Measured after the fix: `swforce` is 0x780 bytes = 80 entries, and the log reads `force: 80 entries all hit`.

Also: with GC off, 31 graphics-side symbols that had been quietly discarded (`mtl_*`, `dm_nvn_ruler_truth`, `thread_selfcounts`) surfaced as undefined and got empty implementations in `dm/android/gfx_stub_android.c`.

**Current blocker: a Panic inside `nninitInitializeSdkModule`**

```
[dm] Break#1 reason=0 Result=0 at sdk+0x39fbd4, buffer ...+0x4 "...."
[dm] bt: sdk+0x145038 sdk+0x145198 sdk+0x3874a0 sdk+0x386120 sdk+0x2d46dc sdk+0x2d4470 sdk+0x2d33b4 sdk+0x2d335c sdk+0x2b03ec ...
```

Established and ruled out so far:

- **Not the build output**: compared word by word against the existing `keypage` output, the only differences are exactly the 25105 / 12085 / 10486 `mrs` register changes; nothing else moved.
- **Not the time layer**: `now_ns()` = `mach_absolute_time() * numer / denom`, and the compat layer returns nanoseconds with `mach_timebase_info` = 1/1, so it is self-consistent.
- **Not the take-over table**: all 80 entries now hit.
- **Not a wild value**: zeroing both `x0` and `x1` in the placeholder return leaves the panic **exactly where it was**.
- **The panic site is identified**: `sdk+0x146870` is the SDK's `Panic/Result` helper (`mov x1,x0; mov w0,wzr; mov w2,#4; bl svc#0x26`), so `reason=0` is hard-coded and `Result` is simply the value passed in (the log reads 0). It is only called indirectly through a function-pointer table, so the caller has to be found by address in the sdk.
- **The SDK's svc trampoline table is decoded** (from `sdk+0x39fb04`, ordered by ordinal): the sdk uses `0x13-0x1d, 0x1f, 0x21-0x23, 0x25-0x2a, 0x2c, 0x2d, 0x2f, 0x32-0x36, 0x4b`, while the host currently implements only `0x01-0x06 / 0x1e / 0x26 / 0x27 / 0x29 / 0x2c / 0x2d`. Several wrappers in that table are `str x0,[sp,#-16]! -> svc -> str w1,[x2]`, i.e. they take their result from **x1** and store it through an out-pointer.
- **Only two unimplemented svcs were reached** (the placeholder line is printed once per ordinal): `0x1f` and `0x21`. `0x21` and `0x22` are **direct** calls in the sdk, adjacent in the same function (`bl 0x39fb9c` / `bl 0x39fb94`); that function (`sdk+0x385040`) reads the TLR, compares it against a handle, and then calls one of the two. `0x1f` is only called indirectly.

**Next step (disassembly route)**: the SDK's `Panic` helper is only reached through function pointers, so the caller must be found by address in the sdk (`adrp/ldr` to fetch the table entry, then `blr`) and followed back to the failing check; at the same time `0x1f` (by ordinal likely `GetSystemTickFrequency` and friends) and `0x21` should be implemented in `hle_svc.c` according to their semantics instead of staying placeholders.

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

- **Always run Python with `PYTHONUTF8=1` (plus `PYTHONIOENCODING=utf-8`).** On Windows, Python defaults to GBK for both files and stdio, which bites in two ways:
  - on output, a character such as `U+2212` in a script's comments raises `UnicodeEncodeError` and **aborts the script midway** (seen as truncated generated headers);
  - on input, the tables in the repository are UTF-8 while a script's `open(path)` carries no encoding → `UnicodeDecodeError` (hit when reading `shader_facts.tsv`).
  `PYTHONUTF8=1` makes every `open()` and stdio default to UTF-8, fixing both sides at once.
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
