# Android port: implementation plan and task list

English: [ANDROID_PLAN.zh-CN.md](ANDROID_PLAN.zh-CN.md)

This page breaks "porting D2R Mobile to Android" down into an actionable, checkable task list, so it can be confirmed item by item and divided up. For the technical feasibility case see [ANDROID.md](ANDROID.md); for the plain-language version see [ANDROID_PLAIN.md](ANDROID_PLAIN.md).

**Current stage: M2 in progress (the Vulkan replay layer runs on a device).** The checkboxes below reflect the actual state of this machine, not intentions.

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

### M1 wrap-up: the root cause was not the SDK but how the host is linked (2026-10-14)

Using the repository's own symbol-aware disassembler `dm/tools/sdis.py` (it annotates with dynsym and PLT
import names) to resolve the backtrace cracked the whole chain at once:

```
nninitInitializeSdkModule
└→ nndiagStartup                     <- goes through the PLT, so the host's forced take-over should have replaced it
   └→ nn::lm::Initialize             <- connect to the lm log service
      └→ nn::lm::CreateLogServiceByHipc
         └→ nn::sf::hipc::InitializeHipcServiceResolution
            └→ AbortImpl → VAbortImpl → svcBreak
```

`nndiagStartup` was in the host's take-over list all along ("connect to the lm log service; diagnostics go
straight to the host's output") and `force` reported "all hit" -- but **it was not in the table at all**.
Dumping both registry sections and comparing:

| | swforce | swhle |
|---|---|---|
| sum over all object files | 263 entries | 55 entries |
| actually in the binary | **80 entries** | **1 entry** |

The cause is **the on-demand extraction of a static library**. Inside `libdmhost_android.a`, `hle_sdkinit.c`
(the seven service-layer take-overs) and `hle_table.c` (the libc passthrough table) contribute **nothing but
registry entries** -- they hold only `static` functions and `static const` table entries and define no
external symbol, so the linker considers the whole member useless and never pulls it in. `--gc-sections`
was already turned off in the previous section, so this was not GC but the archive extraction rule.

On Apple the `Makefile` compiles every `.c` straight into the executable **without a static library**, so
every translation unit is present -- **this is the last piece of "works on Apple, panics on Android"**.

The fix (`dm/android/CMakeLists.txt`): link with `-Wl,--whole-archive ... -Wl,--no-whole-archive` to pull
every member in. Measured after the fix: 264 / 64 entries, and `nndiagStartup` is back in the table.

**Device result after the fix**:

```
[dm] init take-over: nndiagStartup (no-op)          <- all seven service-layer take-overs now apply
[dm] init take-over: oe::Initialize (no-op)
[dm] init take-over: aoc::Initialize (no-op)
[dm] init take-over: err::InitializeContextWriterApi (no-op)
[dm] init take-over: fs::InitializeForApplication (no-op)
[dm] init take-over: pctl::Initialize (no-op)
[dm] init take-over: diag::InitializeApplicationAbortObserver (no-op)
[dm] >>> nninitStartup
[dm] SetHeapSize: first heap set succeeded, 3728 MB (application pool 3797 MB - images 68 MB; 34 earlier refusals)
[dm] main: DT_INIT +0x30 (852 init_array entries)
[dm] sdk: DT_INIT +0x30 (115 init_array entries)
[dm] save flush throttle: main+0x20cefa8 30 -> 3 s
[dm] >>> nnMain                                    <- M1's goal
[dm] fs mount $: -> .../RomFS (read-only); ^: -> .../temp
[dm] ToCalendarTime: first conversion ... (time zone CST, offset 28800 s)
[dm] nnMain returned
```

In other words: **on Android the host loads the rewritten output, relocates it, applies every
service-layer take-over, finishes SDK initialisation and enters `nnMain`.** It returns immediately
because M1 has no graphics backend (`front_metal_layer()` is NULL and NVN runs on the null device), so the
guest's first real act -- creating a window / swapchain -- finds nothing and it exits. That is exactly
what M2 supplies.

The `svcBreak` handler was strengthened too: besides the SDK message buffer it prints any of x0..x7 that
look like pointers to a printable string (`VAbortImpl`'s signature is exactly expr/file/func/line), so an
abort shows the assertion text directly.

**How M1 is deployed on the device** (currently): the `dmhost` produced by the `dm/android/build` target and
the rewriter's output are pushed with `adb push` to `/data/local/tmp/dm/{dmhost, patched/}`, then
`./dmhost ./patched main sdk` is run from `/data/local/tmp/dm`. Packaging an APK waits until after M2, when
there is something to show.

**Where the game resources (the 8.3 GB RomFS) live**: the decision (2026-10-14) is **inside the workspace** --
`/data/local/tmp/dm/romfs` on this device: same partition, fastest reads, one `adb push` and done. The Android
entry point `main_android.c` **probes candidates in order**, uses the first hit and logs the chosen path
(`romfs: <path>`); the caller can still point at it directly with `DM_ROMFS`. Order (earlier wins):

1. **`<root>/romfs`, `<root>/data/romfs`** -- preferred: resources inside the workspace (on this device
   `/data/local/tmp/dm/romfs`).
2. **`/sdcard/d2rm/romfs`, `/sdcard/Download/d2rm/romfs`** -- the fallback: the **only place that both survives a
   reboot and is writable by `shell`** (`/sdcard` -> `/storage/self/primary`, `mkdir` succeeds in testing), at the
   cost of going through FUSE, so loading is a bit slower. Keeping a copy here means the system cleaning
   `/data/local/tmp` no longer matters.
3. **`/data/local/d2rm/romfs`** -- last, merely "use it if it is there": `/data/local` is `drwxr-x--x root:root`
   and the `shell` user **cannot create a subdirectory** in it (`mkdir: Permission denied`, measured), so it needs
   root.

**Reboot risk and recovery**: `/data/local/tmp` may be cleaned by the system, in which case re-pushing from the
computer takes about 4-5 minutes (from `.work/data/switch/3.3.94012/Update/RomFS/data`); with a copy kept on
`/sdcard` it is instead a matter of pointing `DM_ROMFS` there (or letting the probe pick it), which takes no time.

"Probe in order, use the first hit" rather than hard-coding one path is what makes a directory change a matter of
**moving files only, with no host change**: moving within one partition is a rename and takes no time, while
`/data/local/tmp` to `/sdcard` is a real copy across filesystems (8.3 GB, about 2-4 minutes on the device, still
faster than re-pushing from the computer).

**M1 conclusion**: the acceptance criterion -- "the host runs and the log shows the game's `UNIMPL` lines" --
is met, and exceeded all the way to `nnMain`. Next comes M2.

### M2 step 1: the Vulkan path and the memory model, measured (2026-10-14)

The host is currently an executable pushed with adb and **has no window**, and a window / swapchain needs an
APK. So M2 starts **offscreen**: that settles three questions at once -- can Vulkan be used, is the clear
correct, and how is memory laid out -- and the last one is exactly the assumption the feasibility analysis
flagged as **risk number one**, NVN's zero-copy "GPU address = CPU address".

Probe: `dm/android/vk/vk_probe.c` (not part of the host or any APK, it only measures the device). Device
result (Redmi K90 / Adreno 830):

```
instance created: loader supports Vulkan 1.4.0
1 physical device
  [0] Adreno (TM) 830: driver 512.800.71, Vulkan 1.3.284, type 1
=== memory model ===
  heap 0: 15113 MB (device local)      heap 1: 4095 MB (device local)
  types 0-2: DEVICE_LOCAL
  type 3:    DEVICE_LOCAL|HOST_VISIBLE|HOST_CACHED
  types 4/5: DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT
  type 6:    DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT|HOST_CACHED
  => zero-copy (HOST_VISIBLE+HOST_COHERENT) available: yes (type 6)
  queue family 0: 3 queues, flags 0x19f (graphics / compute / transfer / sparse); family 1 transfer; family 2 compute
offscreen image 256x256, video memory 260 KB
clear readback: all match (first pixel 64,128,191,255)
```

**Update to the risk list**

- **Risk number one, "unified memory": confirmed.** The Adreno 830 offers
  `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT` (types 4 / 5 / 6, type 6 with `HOST_CACHED` as well). NVN's
  zero-copy therefore has a landing place on the Vulkan side: map memory pools to host-visible +
  host-coherent `VkDeviceMemory`, with type 6 additionally offering a host-cached variant.
- **Shaders**: the device is Vulkan **1.3** and the loader 1.4, so the **SPIR-V 1.3** Recon B produced
  (6923 of them) can go straight into `vkCreateShaderModule` with no version downgrade.
- **The path works**: an offscreen clear plus a pixel-by-pixel readback all match, so device / queue /
  command buffer / memory binding / image barriers / image-to-buffer copy are all sound; the rest of M2 only
  fills in NVN's semantics.

**Next**: memory pool mapping (NVN MemoryPool -> `VkDeviceMemory`, starting along the zero-copy route) and a
minimal command set (`SetRenderTargets` / clear / `DrawArrays` / present). The window and swapchain must
wait for the APK (`ANativeWindow`).

### M2 step 2: the zero-copy memory pool passes both directions (2026-10-14)

The memory pool is NVN's foundation: the guest writes on the CPU and the GPU must read the same bytes, and
the reverse. The same probe builds two 64 KB buffers on the measured
`DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT|HOST_CACHED` type (type 6) and checks both directions:

```
zero-copy memory pool (type 6, 64 KB each): CPU->GPU ok, GPU->CPU ok
```

- **CPU write -> GPU read**: the CPU writes `i*7+3` into the mapping, the GPU copies it into the other
  buffer with `vkCmdCopyBuffer`, and the readback matches byte for byte.
- **GPU write -> CPU read**: the GPU fills 0xAB with `vkCmdFillBuffer` and the CPU reads it straight back
  from the mapping; everything matches.

**Conclusion: NVN's memory pools can be mapped straight onto a type 6 `VkDeviceMemory` with no copying at
all.** Risk number one of the feasibility analysis, the "unified memory" assumption, is now **verified on a
real device**, and the rest of M2 can take the cheapest, zero-copy route. (The probe submits the two steps
separately: the copy reads and the fill writes the same buffer, and separate submissions avoid a
hand-written barrier.)

### M2 step 3: how the memory pools land (two capability measurements that fixed the design)

**(1) Metal's route has no Vulkan counterpart.** On the Apple side a pool is set up like this (`mtl_pool_add`
in `nvn_mtl.m`):

```objc
// [mem, mem+size) expanded to page boundaries -> newBufferWithBytesNoCopy -> zero copy
id<MTLBuffer> buf = [g_dev newBufferWithBytesNoCopy:(void *)a length:b - a
                                            options:MTLResourceStorageModeShared deallocator:nil];
```

Apple has unified memory, so **any host pointer can be wrapped in an MTLBuffer**. The only Vulkan equivalent
is `VK_EXT_external_memory_host` (import a host pointer as a `VkDeviceMemory`) -- and on the real device
(Adreno 830, 165 device extensions):

```
  [yes] VK_KHR_external_memory / VK_EXT_descriptor_indexing / VK_KHR_timeline_semaphore
  [yes] VK_KHR_swapchain / VK_ANDROID_external_memory_android_hardware_buffer / VK_EXT_queue_family_foreign
  [no]  VK_EXT_external_memory_host        <- the Vulkan counterpart of Metal's route
  [no]  VK_EXT_host_image_copy
```

**So "wrap the pool pointer in a buffer" does not work on Vulkan and the design has to change.**

**(2) The replacement: back the guest address space itself with Vulkan zero-copy memory.** The host lays that
address space out itself (`mem_init` in `hle_svc.c`):

```c
#define RESERVE (5ull << 30)
g_as    = mmap(NULL, RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
g_alias = g_as + 0x10000000; g_alias_cap = 0x20000000;            // alias 512 MB
g_stack = g_as + 0x30000000; g_stack_cap = 0x10000000;            // stack 256 MB
g_heap  = g_as + 0x40000000; g_heap_cap  = RESERVE - 0x40000000;  // heap 4 GB
```

The address `vkAllocateMemory` + `vkMapMemory` returns **is itself a host pointer** (step 2 already verified
both directions are CPU/GPU visible). Backing the heap and the alias region with Vulkan zero-copy memory
therefore turns pool-address-to-buffer translation into arithmetic, **with no need for that extension**. The
only open question was whether such a large block can be obtained -- measured (`vk_region_probe.c`):

```
maxMemoryAllocationSize = 1024 MB (the advertised value; the real limit is higher), bufferImageGranularity = 1
  3072 / 3200 / 3328 / 3456 / 3584 / 3712 / 3840 / 3968 MB: all allocate and map successfully
  4096 MB: fails with VK_ERROR_OUT_OF_DEVICE_MEMORY
  => the per-allocation limit is 3968 MB
  lazy commit: a 3.9 GB block with only 64 pages touched -> resident pages changed by -96 (it does not really cost that memory)
  binding a buffer at a non-zero offset inside the block: memory alignment 64, binding at offset 16 MB succeeds, and both directions work at that offset
```

**(3) The design is now settled** (M2 implements this):

| region | size | source | why |
|---|---|---|---|
| heap | 4 GB capacity, 3728 MB actually used | **Vulkan zero-copy memory** (one 3968 MB allocation suffices) | the game's `StandardAllocator` carves NVN pools out of the heap, and pools must be directly GPU-visible |
| alias | 512 MB | **Vulkan zero-copy memory** (a second allocation) | physical memory mapped in by `MapPhysicalMemory`, used by the SDK's VammManager |
| stack | 256 MB | ordinary mmap | it is only a CPU stack, the GPU never touches it |
| the rest of the reservation | remainder | ordinary mmap | takes address space only |

That is about 4.5 GB of Vulkan allocations, committed lazily, so it does not really consume that much memory
(heap 0 has 15 GB).

**Two hard boundaries of this design (to handle during implementation)**:

- **The per-allocation limit is 3968 MB**: the guest heap is currently 3728 MB, leaving about 240 MB. If
  `app_pool()` is raised later so the heap exceeds it, the design degrades into "two non-contiguous pieces",
  so `mem_init` must **fail loudly when it would exceed the limit** rather than failing silently.
- **Pools that fall outside the Vulkan regions**: if the guest opens a pool in an ordinary mmap region
  (stack / the rest of the reservation) it cannot currently become a buffer. `pool_add` must **report failures
  loudly and count them**, so that the real device tells us whether it ever happens before we decide on a
  compensation route (a shadow copy, or changing the allocation).

**(4) Two more things measured while implementing (both handled)**

- The carrier buffer must not use `STORAGE_BUFFER`: exporting an fd requires a "dedicated" buffer for that
  allocation (see above), and a 4 GB carrier buffer created with `STORAGE_BUFFER` fails outright -- it hits the
  driver's `maxStorageBufferRange`. The carrier is never really read or written, so transfer-only usage
  suffices; on top of that, **when it cannot be created, step down by 128 MB**, the same treatment as the
  allocation limit.
- The heap overlay runs into the limit at 3968 MB, so **the limit reported to the guest must be reduced
  honestly** (`g_heap_cap` goes from 4 GB to 3968 MB). This step is necessary: otherwise the guest would use a
  4 GB heap and the last 128 MB would land on ordinary, unconverted memory. With the reduction in place the game
  still sets its heap to 3728 MB (< 3968, as required) and boot proceeds all the way to `nnMain`.

**Current state**: `dm/android/vk/` holds `vk_device.c` (the Vulkan device, created once per process) and
`vk_region.c` (the memory overlay plus address lookup), wired into the host through the hook in
`dm/host/dm_gpu_region.h` (the host itself knows no Vulkan). Log from a real device:

```
[green] Vulkan device ready: Adreno (TM) 830 (Vulkan 1.3.284) queue family 0; zero-copy memory type 4; fd extension = yes
[yellow] heap overlay: only 3968 MB converted (4096 MB requested) -- hit the per-allocation limit, so the limit reported to the guest was lowered accordingly
[green] alias region overlay: 512 MB is now directly GPU-visible (0x6fa0a81000, type 4)
SetHeapSize: first heap setup succeeded, 3728 MB      <- unchanged after the overlay
... nnMain returned
```

The next step is the replay layer itself (`nvn_vk.c`): implementing `mtl_init` / `mtl_pool_add` / `mtl_pool_del` /
`mtl_addr` against the interface in `nvn_mtl.h` first, removing each stub from `gfx_stub_android.c` as it lands.

### M2 — minimal Vulkan replay layer (the largest item)

- [x] Device and queue (`ANativeWindow` swapchain waits for M3)
- [x] MemoryPool → `VkDeviceMemory` plus host-visible/coherent buffers (the "GPU address = CPU address" zero-copy assumption **verified in both directions**) ★risk
- [x] Map Texture / Sampler / TextureView (format table 154/157, samplers cached by parameters, sub-views built per view; **the render-target path's views are still to be wired up**)
- [x] Program: build `VkShaderModule` / pipelines from SPIR-V (descriptor writing and real draws work: 1.22 million draws, 83 pipelines, no failures)
- [x] Minimal command set: `SetRenderTargets`, clear, `DrawArrays`, `DrawIndexed` (`Present` waits for M3)
- [ ] **Acceptance**: a picture appears (a solid clear first, then progressively up to the main menu) -- there is **no window**; reading one frame back offscreen and writing it to disk is in place (step 13) and what it measured is that **the GPU has written no colour content at all** (step 14): **the blocker is the rendering chain itself**, not the ability to see it

### M3 — front end + touch + audio

**First piece: the minimal window project (working; a picture appears on the device)** -- `dm/android/apk/` (one
`NativeActivity`, zero Java, `hasCode="false"` in the manifest), with the scripts `build_apk.cmd` / `install_apk.cmd`.
On the device: installed and launched, **two screencaps three seconds apart** show the background shifting from dark red
to dark purple and the white bar moving left to right -- the whole chain `APK -> ANativeWindow -> VkSurface -> swapchain
-> present` works (`1156x2510 format 37, 5 images, FIFO`). Drawing deliberately avoids render passes
(`vkCmdClearColorImage` plus `vkCmdCopyBufferToImage`), so no pipeline has to be compiled on the device.

**Reading the data once installed as an APK (measured; two questions, two answers)**: the app's domain is
`u:r:untrusted_app:s0` and the data adb put in `/data/local/tmp` carries the `shell_data_file` label, so -- **reads work,
writes do not**: `can read /data/local/tmp/dm/{patched,spv,romfs}` (the 9.1 GB **needs no copying**), but
`cannot write /data/local/tmp/dm/save` (`errno 13`). Making `save`/`temp` **symlinks** into the app's directory does not
help either (measured: the same `errno 13`). The answer is the environment variables the host already has:
`DM_ROOT=/data/local/tmp/dm` together with `DM_SAVE` / `DM_TEMP` pointing at
`/sdcard/Android/data/ai.d2rm.dm/files/{save,temp}` (the directory that "can write" in the positive control).

**The permission set (settled; just two)**: `INTERNET` (a normal permission, effective once installed and needing no
request) and `MANAGE_EXTERNAL_STORAGE` (a special permission the user has to switch on in system settings). The **orange
rectangle** in the window is its request button -- **the button's presence or absence is itself the permission state** (a
button means not granted), so this version draws no words; tapping it opens the grant page. **What that permission buys
and what it does not is an A/B measurement**:

| `appops` state | write `/data/local/tmp/dm/save` | write `/sdcard/d2rm` | write the app's private dir |
|---|---|---|---|
| `allow` (granted) | ✗ `errno 13` | **✓ works** | ✓ |
| `default` (not granted) | ✗ `errno 13` | ✗ `errno 1` (EPERM) | ✓ |

**Second piece: the window and the game share one device (working)** -- the first version's window built **its own** Vulkan
set inside the app (instance, device, swapchain), but **textures cannot cross between two devices**, so "the window shows
the texture the game drew" was impossible. The window's Vulkan now belongs to the host: `vk_device_set_window()` hands the
window over **before** `vk_device_init()` (which decides whether the instance enables the Android surface extension,
whether the device enables `VK_KHR_swapchain`, and which queue family is picked), the swapchain and present moved into
`vk_win.c`, and the app only "paints one frame of host-memory RGBA8 and hands it over". On the device: `libdm.so` grew
from 245 KB to **322 KB** (the host is linked in) and the picture is a **scaled, centred test pattern** (1600x900 fitted
into 1156x2510, leaving black bars top and bottom), which shows that presenting really goes through the host's single
device. That present path is also exactly the one the next step's texture viewer will take: host pixels ->
`vk_win_present_rgba` -> the phone screen.

In other words: **it does not buy write access to `/data/local/tmp`** (that path is SELinux's `shell_data_file` and no
permission reaches it), and what it does buy is "**data in a place the user can see and replace with a file manager at any
time**" (`/sdcard/d2rm`, the very fallback path the entry already knows).

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
| **Sporadic early crash (1 run in 4, measured)** | `CRASH sig=11` lands in the **host** and the process dies about **0.1 s** after start (`pc` in the host range, every thread still at 0.00 s CPU time); three consecutive runs did not reproduce it | The scene is saved in the log; if the frequency rises, follow the "0.1 s after start" timeline -- at that point only the heap switch and SDK init have run |

## 6. Working on Windows (encoding and commands)

The port is currently being driven from a Windows machine. These are the traps already hit, and the rules that follow from them.

### Encoding

- **Always run Python with `PYTHONUTF8=1` (plus `PYTHONIOENCODING=utf-8`).** On Windows, Python defaults to GBK for both files and stdio, which bites in two ways:
  - on output, a character such as `U+2212` in a script's comments raises `UnicodeEncodeError` and **aborts the script midway** (seen as truncated generated headers);
  - on input, the tables in the repository are UTF-8 while a script's `open(path)` carries no encoding → `UnicodeDecodeError` (hit when reading `shader_facts.tsv`).
  `PYTHONUTF8=1` makes every `open()` and stdio default to UTF-8, fixing both sides at once.
- **Redirect to a file with cmd's `>`** (preserves raw bytes); do not use a PowerShell pipeline plus `Out-File` (it changes encoding and line endings).
- **Do not route a commit message through a `.ps1` that PowerShell 5.1 reads.** Keeping the Chinese message inside a script and
  having the script write the message file is the older habit, and it bit once: PowerShell 5.1 reads a `.ps1` using the **system
  code page (GBK) rather than UTF-8**, so the Chinese is already mojibake in memory, and `[System.IO.File]::WriteAllText(..., UTF8)`
  then lands it on disk **double-encoded** (`git log` shows `M3-2k锛氱紦...`, while the repository's earlier commits are fine).
  The fix: **let the editor or tool write the message file as UTF-8**, and use `git commit -F <file>` so git reads bytes and
  PowerShell never touches the string. A message already damaged this way can be recovered (UTF-8 bytes read as GBK is a
  double encoding, so walking it back byte by byte works), or its text can be extracted from the script file and rewritten --
  but **do not extract it with PowerShell**; use Python (`PYTHONUTF8=1`).
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
- **An XML comment may not contain `--`**. With an English em-dash written as `--` inside `AndroidManifest.xml`'s comment,
  aapt2 reports `AndroidManifest.xml:10: error: not well-formed (invalid token)` -- and the line number points into the
  middle of the comment, so it does not look like the comment's fault.
- **APKs on this machine are built without gradle** (there is no gradle and no java on PATH, but JDK 21 is installed): use
  `aapt2 link` (which needs `platforms/android-<ver>/android.jar`), write `lib/arm64-v8a/*.so` into the package
  **uncompressed** (PowerShell's `ZipArchive` can do that), then `zipalign -p 4` and `apksigner sign` (which needs
  `JAVA_HOME`; the key is made with `keytool` and kept under `.work` only). With `android:hasCode="false"` in the
  manifest **no dex is needed**, so the whole chain needs no Java source at all. Scripts: `dm/android/apk/build_apk.cmd`
  and `install_apk.cmd`.
- **Toolchain paths** are decided in **one** place, `dm/android/apk/toolchain.cmd` (called by the two scripts above):
  `JAVA_HOME` / `ANDROID_SDK_ROOT` / `ANDROID_HOME` / `DM_SDK_ROOT` / `DM_NDK` / `DM_BUILD_TOOLS` / `DM_CMAKE` /
  `DM_PLATFORM` / `DM_JDK` -- any one of them overrides. **A new machine needs no edit of any file in the repo.** See
  section 2 of `docs/CROSS_MACHINE.md`.

### cmd script discipline (each of these bit when writing toolchain.cmd)

- In `if cond set A & set B` the `&` **escapes the if** and B runs unconditionally -> always write `if cond (set A & set B)`.
- **A `rem` line is not pure comment**: the `&`, pipe, `>` and `<` inside it are treated as separators and redirection
  -> none of them appear in the comments here.
- **Never** use substring substitution `%VAR:STR=REPL%` or position syntax `%VAR~n,m%` **inside an `if` condition**: cmd's
  `%` matching on an `if` line is greedy and boundary-guessing, and once it tangles with `==` it misreads (the visible
  symptoms are "syntax incorrect" or an echo that shows the parts concatenated). **Set into a temp first, then compare
  plain variable names.**
- **Never** put `for /f ... 2^>nul` inside a parenthesised block `( ... )`: parsing inside the block differs from the top
  level and the error is again "syntax incorrect". Keep `for /f` at the top level or in a subroutine.

## 7. Next step

Proceed in the order on this page; after each milestone, tick it off here and record the conclusion. **Both items of
milestone A (the rendering chain's warnings) are now done**: the descriptor blanks (the UBO name number minus 3, the
storage buffer derived from `cb_offset`, and **buffer textures** as a new resource kind -- see "M3-2k") and the vertex
attribute test ("M3-2l": two independent faults, one of them caught by a probe). Device readings: `顶点属性` 0,
`贴图写空白` 0, `拿不到缓冲地址` 0, `target 10 暂不支持` 0. **"Scan first" has now been done twice over ("M3-2m")**:
reading level 0 texture by texture gives **all zero**, and going **level by level** is still **all zero** (33 level reads
at frame 50, **0 with content**). The "the viewer reads content at level 1" found in between was stale heap bytes read at
a **wrong pitch**, and with the pitch fixed both sides agree (part six of "M3-2m") -- **step 14's conclusion stands**;
what changed is that the instrument is now trustworthy. **Item 1 of section nine (persisting the pipeline cache) landed
on 2026-10-15** ("M3-2p"): it is unrelated to the blocker, so the five items below keep their order, and the device fact
it brought out (MIUI SIGKILLs on backgrounding, so the hooks never run) is recorded there too. What follows is ordered by
value (reordered 2026-10-15: the finished "level-by-level scan" is ticked off, the two duplicate "device loss" items are
merged, the blocker-independent items from section nine are moved in, and this round's constraints are noted):

1. **Milestone B: the guest stall** (**first -- the main blocker**). This device normally stalls a dozen seconds in at
   `CharacterCreate:Hover:Barbarian` (occasionally `MainMenuHD:CheckUpdateLinks`). **The judgement has to be read from
   "what is it doing across several samples", never against the occasional outlier run**: the one-shot stall sampler and
   the wait-point dump are both in place ("M3-2g"), so sample `/proc` over several rounds to classify it. It covers the
   most ground: the picture is not found before the stall, and both "finding the picture" and "the device loss" are
   suppressed by it -- so "why does it stop here" comes first.
2. **That sporadic device loss** (**likely the same source as item 1; watch for it while doing item 1**) -- about once per
   run, `【红】一次性提交等待失败 VkResult -4` (`VK_ERROR_DEVICE_LOST`), followed by a hundred-plus `贴图上传失败` lines.
   Uploads **are already** off the shared command buffer ("M3-2o") and it did not go away, so the cause lies elsewhere: the
   next candidate is "**who loses it first**", the guest's own commands or this one one-shot submit of ours. What to
   measure: what else was on the queue at that moment, whether the queue family / device had already been lost on the
   guest's side, and the `vkQueueSubmit` return value with its context.
3. ~~**Item 2 of section nine, `HOST_CACHED`**~~ -- **measured and ruled out (2026-10-15, "M3-2q")**: exporting an fd on
   type 6, `mmap`-ing it at a chosen address and both directions all **work** (256 MB, 512 MB and 3968 MB alike), but the
   same-device A/B shows **no measurable difference in CPU time** (under 3% within the same mode, while the run-to-run
   spread is +-100%), and using it means **stepping outside the buffer's declared allowed types** (0x13 does not include
   type 6, so it is outside the spec) -- so **type 4 stays** and the `DM_ZC_CACHED=1` switch is kept (off by default) for
   a re-check on another device or driver.
4. **Items 4 / 5 / 6 / 7 of section nine** (decide once the frame budget is measured) -- the per-draw repeated work (70.5%
   of the ops are pure state binding); texture uploads on a resident upload ring (same source as item 2); the hand-rolled
   pipeline tables being small (lower priority now that the `VkPipelineCache` has landed); folding pure state into the
   shadow (M3/M4).
5. **Wrap-up** -- docs and cleanup (the interface-description-table code that is no longer called); the items in
   `.work/recon/pending_rm.md` (waiting for one "execute the deletion list"); the "time to `nnMain`" A/B left open by
   M3-2p (**only after item 1 has moved**, or the stall masks the number).

**Three constraints brought out so far that apply to everything later** (the first two recorded in "M3-2p", the third in
"M3-2q", restated here so they are not forgotten):
- **MIUI SIGKILLs on backgrounding**: `TERM_WINDOW` / `DESTROY` / `atexit` never run -- **any "persist on exit" design does
  not work on this machine**; to persist, write **periodically in the foreground** (that is exactly how the pipeline cache
  landed).
- **`adb shell input keyevent` is refused** (`SecurityException: INJECT_EVENTS`) -- to background the app, start the
  launcher explicitly: `adb shell am start -n com.miui.home/.launcher.Launcher`.
- **`DM_WATCH_STALL=0` does *not* mean "only log, never kill"**: in `crash.c:347-354` a value <= 0 **falls back to 15
  seconds**, so writing 0 is the same as "stalled for 15 s -> declared dead, full dump written, `_exit`" (that is what made
  the process vanish during this round's A/B, not a crash); to turn the watchdog off entirely, **leave the line out**.
- Also: MIUI refuses `adb install -r` (`INSTALL_FAILED_USER_RESTRICTED`); installing from the device side
  (`pm install -r`) works.

---

## 8. Progress log

**M0 / M1 -- done**

| Milestone | Conclusion |
|---|---|
| M0 SDK/NDK + device | Toolchain and device ready (Redmi K90 / Adreno 830 / Android 16 / arm64-v8a) |
| M1 host compiles | The `dm/android` project with the NDK toolchain -> host layer at **0 errors**, 88 object files, `libdmhost_android.a` |
| M1 device acceptance | Loads the rewritten output, applies the service-layer take-overs, finishes SDK initialisation, enters `nnMain`, and the log shows the game's `UNIMPL` lines |

**M2 -- in progress (Vulkan replay layer); every line below was measured on the real device**

| Step | Conclusion |
|---|---|
| 1 Vulkan path and memory model | Adreno 830 / Vulkan 1.3.284 / zero-copy memory type 4, working CPU<->GPU both ways |
| 2 Zero-copy memory pool, both directions | **Risk number one** of the feasibility analysis, the "unified memory" assumption, verified on a device |
| 3 Pool design settled | This device has **no `VK_EXT_external_memory_host`**, so Vulkan backs the guest address space instead: exportable memory -> fd -> `mmap(MAP_FIXED)` back at the same address. The per-allocation limit is 3968 MB > the 3728 MB guest heap, commit is lazy, and binding buffers at page-aligned offsets works both ways |
| 3 Overlay implemented | The 3968 MB heap and 512 MB alias region are directly GPU-visible; `SetHeapSize 3728 MB` still succeeds and boot reaches `nnMain` |
| Replay layer, first batch | Pool registration plus address lookup (pools #1..#7 all registered on the device, no failures) |
| Three start-up defects | (1) the NVN implementation table registered nothing on ELF (Mach-O section name) -> **0 semantic to 203**; (2) the xattr name broke Linux's rules -> `user.` prefix; (3) the `clonefile` stand-in copied single files -> recursive (with xattrs) |
| Front-end sig=11 | Root cause: a **96 KB guest thread stack**, sized for the original libc, was overrun by the host's bionic (the push landed on the PROT_NONE page below the stack) -> `MapMemory` opens a 1 MB margin on the side the stack grows. After the fix: **5285 frames over 177 seconds with no crash**, and the graceful SIGTERM shutdown completes |
| Replay submission machinery | Eight rotating slots with fences and a command stream interpreter (a small program shaped like the Metal one); CPU-visible effects fire in fence order. 4179 frames on the device with no crash |
| Op distribution, measured | About 8 million ops per window: `BIND_STATE` 27.8% / `BIND_UBO` 21.9% / `STENCIL` 10.3% / `BIND_TEX` 5.3% / `BIND_VBUF` 5.2% (the top five are 70.5%, all of them state and resource binds) |
| Render targets' views land | `p_set_rt`'s `cview/dview` enter the shadow state: sub-views **0 -> 33**, 34,193 hits; the render area is clamped to the attachment (1600x900 against 800x450 on the device, exactly one level apart) |
| Present-screenshot chain | A self-contained minimal PNG writer plus a driver-side readback and `mtl_present`; `DM_NVN_SHOT/ALL/SCAN` share Apple's names and meanings. The device wrote a 1600x900 PNG (5,761,403 bytes) that opens after being pulled |
| Two real bugs the screenshot exposed | (1) a swapchain texture **has no guest memory** (the driver owns it) so the texture layer refused an image and the render-target path **skipped it in silence** (now registered as storage ownership); (2) **an attachment that cannot produce an image used to be skipped in silence** (now it speaks up). `COPY_T2T` landed along the way (its first one on the device is a full-screen 1600x900) |
| **Step 14: the measured conclusion** | At frames 300 and 800, each scan covered 64 textures of 256x256 or larger: the readable colour targets are **all zero** and the present texture is **all white** -> **the GPU has written no colour content**; the blocker is the rendering chain (two warnings point at where vertex data lands and at descriptors falling back to blanks) |

**Next, in the order the measurements dictate (including what is done)**

1. Turn the bind ops into **shadow state** ✓ (70% of the stream; binds only record state and cost almost nothing);
2. The texture layer ✓ (format table, serial-keyed table, deswizzle upload, direct CPU read);
3. Render-state shadow plus the pass machinery ✓ (`CLEAR` reaches the GPU for the first time);
4. Shader loading ✓ (step 8 below);
5. The pipeline layer ✓ (step 9 below: descriptor set layouts, pipeline layouts, compute pipelines, graphics pipelines);
6. The replay layer's shadow binding storage plus the state interpretation -> really issuing `DRAW` / `DRAW_IDX` ✓ (step 10 here: 1.22 million draws on the device, 83 pipelines, no failures);
7. Textures and samplers resolved by NVN slot ✓ (step 11 here: decoded exactly from the handle, 520k bound, "sampler id not found" 0);
8. Views land ✓ (step 12 here);
9. **The render targets' views** into the shadow state ✓ (step 14 here: sub-views 0 -> 33, 34,193 hits; the render area is clamped to the attachment);
10. **Read one frame back offscreen** and write it to disk ✓ (step 13 here: the picture really lands on disk, a valid 5,761,403-byte PNG that opens after being pulled);
11. Use it to ask "where is the picture" ✓ (step 14 here: **no colour content** -> the blocker becomes the rendering chain: where vertex data lands, and descriptors falling back to blanks);
12. **Descriptors falling back to blanks** cleared ✓ ("M3-2k": the UBO name number minus 3, the storage buffer derived from `cb_offset`, and **buffer textures** as a new resource kind; all three blank counters read zero on the device, and `DM_NO_TEXEL=1` brings them straight back);
13. Buffer textures land ✓ ("M3-2k": `target 10` -> a texel buffer, the same idea as Apple's; **texture 701's registry entry really does have target 10**, the format is `R32G32B32A32_SFLOAT`, 65536 elements, built straight on the pool buffer);
14. **The vertex-attribute test** corrected ✓ ("M3-2l": two independent faults -- two swapped opcodes and a missing dereference; the probe proves it on the device in three seconds with `float 10294 / uint 870 / sint 0`, matching the offline count item by item; the warning went **1 -> 0**).

One **known latent issue** is recorded in the commit message: on Android `crash.c` always passes `is_write = 0`, and the dirty-page fault hook looks at that bit first, so a write to a write-protected page inside a pool walks into the crash path. The crash report already prints the faulting instruction encoding, which is what a correct read/write decision needs.

### M2 step 8: shader loading (verified artifact by artifact on a device)

**The key uses the same algorithm as the Apple side**: the first 16 hex digits of `sha256(control segment, 0x880 bytes)`
(that is exactly what `prog_get` in `nvn_mtl.m` computes). At run time `nvnProgramSetShaders` receives the control-segment
pointer and the replay layer derives the key the artifacts are named by. **This nearly became a blocker**: the Android
compat layer's `CC_SHA256` was a **placeholder** (fixed-length zero output), which would give every shader the same key and
make not a single pipeline buildable; it is now a real FIPS 180-4 implementation, checked against the standard vectors on
a device (`dm/android/tests/t_digest.c`, seven out of seven).

**The reflection comes from a different place than on Apple.** Apple reads `<key>_b.json` from `dmsh_build.py` (MSL types,
the c1 segment, driver-segment reads -- things only Metal's argument buffers need); Android reads `<key>.res.json` straight
from the translator (`tools/dmsh_xlate`). That file says exactly one thing: **which guest resource each SPIR-V binding
corresponds to** (the c slot of a uniform buffer, a storage buffer's address at `c[cb_slot][cb_offset]`, where a
texture/sampler handle comes from), which is precisely the minimum needed to build descriptor sets. The repository has no C
JSON reader (`nvn_refl.h` goes through Foundation), so a tiny scanner rides along: it serves only this machine-generated,
regular format and picks fields out in a single pass.

**On-device self-check** (`dm/android/vk/vk_shader_probe.c` really creates a `VkShaderModule` for every `<key>.spv` in the
directory and parses the matching json):

```
[green] shader artifacts ready: /data/local/tmp/dm/spv (6923 .spv, 6923 .res.json)
stage split: vertex 2343, pixel 4556, compute 22, other 2
bindings: uniform 34890, storage 11379, texture/image 44156 (across 6923 shaders)
loaded: 6923 ok, 0 missing, 0 broken        took 8.5 s
all SPIR-V accepted by this driver, all reflection parsed
```

**This retires a large risk**: the Adreno 830 rejected none of the 6,923 translated SPIR-V modules. If a pipeline fails
from here on, only the state machinery can be at fault.

**Offline cross-check**: the artifact file names against the `dm_ctl_sha16` column of `shader_map.tsv` -- 6927 keys in the
table versus 6923 artifacts, with **stage counts agreeing almost exactly** (table: vertex 2344 / pixel 4559 / compute 22 /
hull 1; artifacts: vertex 2343 / pixel 4556 / compute 22 / other 2), which shows the key and stage derivation is the same
scheme as the shipped table. The five keys the table has but no artifact exists for are **logged and counted** by the replay
layer as designed (the hard boundary is unchanged).

**How the artifacts get onto the device (all three ways measured, Redmi K90 / f2fs)**:

| way | local prep | push | extract on device | per deploy |
|---|---|---|---|---|
| push the full set (20,770 files / 665 MB) | 0 | 37.1 s (1.8 ms/file, 17.9 MB/s) | 0 | 37 s |
| push the runtime subset (13,847 / 606 MB) | 0 | ~25 s | 0 | ~25 s |
| **push a cached tar** (616 MB, one file) | 60 s once (cacheable) | **5.0 s** (122 MB/s) | **1.9 s** | **~7 s** |
| prewarm subset (the 179 shaders of `pso_manifest` / 358 files) | 0 | **1.4 s** | 0 | 1.4 s |

Three counter-intuitive points: **many small files are not slow** (1.8 ms/file, 37 s for 20,770 of them); **one big file is
about 7x faster** (122 vs 17.9 MB/s), so a tar pays off only when deploying **repeatedly**; and **extraction is nearly free**
(13,848 files in 1.9 s, f2fs plus toybox tar). The two ways are indistinguishable at run time -- both end up as ordinary
files with the same names -- so **no packing or indexing code was written for either**. The cached tar lives at
`.work/spv.tar` (covered by `.gitignore`; it contains shaders and must never enter the repository).

### M2 step 9: the pipeline layer (descriptor set layouts + compute pipelines + graphics pipelines)

**What this step really produced is three decisions about who is authoritative**:

- **A descriptor set layout must cover every binding a shader uses, and `<key>.res.json` does not necessarily list them
  all** -- it says where a resource's data comes from, not which type a binding has in the SPIR-V. Evidence: 3 of 22
  compute pipelines failed to build (`VkResult -13` = `VK_ERROR_UNKNOWN`); reading the bindings **declared** in the
  failing shaders' `.spv` and comparing showed all three additionally declare a uniform block at `set0/b0` that is
  **actually used** and that res.json has no entry for; the counter-example is a passing shader from the same batch that
  also declares an extra b0 but never uses it, so it builds. What was missing is **coverage**, not validation (the driver
  does not compare layout types entry by entry).
- So a **SPIR-V declaration scanner** was added, `dm/android/vk/vk_spv.{h,c}`: it walks declarations only (annotations /
  types / variables, stopping at the first `OpFunction`) and reads each descriptor variable's set, binding and **kind**.
  `vk_shader.c` scans once **at load time** and keeps the list (the SPIR-V bytes are dropped right after, which is the
  only moment it can be done); layouts are built **from that list**, and res.json returns to its role of saying where the
  data comes from.
- The cross-check produced a tidy number: **6923 bindings present in the SPIR-V but absent from res.json, exactly one
  per shader** -- the b0 block the translator injects.

**Two bugs, both surfaced by two independent implementations disagreeing** (worth recording: that route proved far more
effective than reading logs):

- The word count of `OpDecorate` **varies**: `Binding` / `DescriptorSet` carry one operand (4 words) while
  **`Block` / `BufferBlock` do not** (3 words). Requiring 4 everywhere skipped both decorations wholesale -- on the device
  that showed up as "every struct kind unrecognised" (uniform / storage buffers -> other) while image kinds were all
  correct (they come from the type declarations, not from decorations), and that combination is what exposed it.
- In SPIR-V 1.0-1.2 a storage buffer is the `Uniform` storage class with the `BufferBlock` decoration, not
  `StorageBuffer` (from 1.3 on) -- the first scanner labelled every SSBO in the library as a UBO, and the first
  conclusion drawn from those wrong labels was wrong.

**Mappings** (the same semantics as Apple's): uniform buffer -> `UNIFORM_BUFFER_DYNAMIC`, storage buffer ->
`STORAGE_BUFFER_DYNAMIC` (the data already sits in the zero-copy pools and only the offset changes per draw, which is what
Apple's `setBuffer:offset:` means), texture -> `COMBINED_IMAGE_SAMPLER`, writable image -> `STORAGE_IMAGE`; there are no
array bindings in the library (all 44,156 measured `array` fields are 1), so `descriptorCount` is 1.

**The vertex attribute correspondence**: a graphics pipeline must supply an attribute for **every** input a shader
declares (one missing and creation fails), and that information is in neither `.res.json` nor `shader_map.tsv`. The route
taken is to read the **input variables** out of the SPIR-V too (Location plus component count/type) and then check whether
"slot number" and "NVN attribute number" share a domain: in `shader_facts.tsv` (the translator's answer table, which gives
component types per slot) the library has **11,166 attributes and the largest slot number that appears is 7**, inside the
NVN attribute range (0..15) -> proceed on **location == NVN attribute number**, with a **runtime format cross-check** (the
guest's vertex format against the answer table's component type) so that a wrong assumption is reported at the first draw.

**On the device** (`dm/android/vk/vk_pipe_probe.c`):

```
descriptor set layouts / pipeline layouts: 6923 succeeded, 0 failed (334 distinct after deduplication by content)
compute pipelines: 22 succeeded, 0 failed
graphics pipelines (one per vertex shader, attributes via the constant fallback): 60 tried, 60 succeeded
cross-check: 6923 bindings present in the SPIR-V but absent from res.json
took 4.2 s
```

**Known gaps (handled in layers, all logged, none silent)**:

| gap | status | evidence and impact |
|---|---|---|
| non-solid polygon modes | **closed** | the core feature `fillModeNonSolid` is asked for and enabled (present on the device) and the mode passes through; wireframe-like draws only, ordinary rendering is solid anyway |
| per-instance vertex stream step > 1 | **closed** | `VK_EXT_vertex_attribute_divisor` is enabled (present on the device, and with `divisor=0`), so the step passes through and NVN's "constant stream" (stride 0) finally has its correct expression (one element per instance, not advancing per vertex) instead of borrowing the constant fallback |
| `descriptorCount` hardcoded to 1 | **closed** | now reads the element count the SPIR-V declares (an `OpTypeArray`'s length constant); the library has **0 array bindings** measured so behaviour is unchanged, but "it happens not to be an array" became "we can tell whether it is" |
| silent graphics pipeline cache eviction | **closed** | eviction warns once; frequent eviction means the state combinations are too scattered and worth a second look |
| vertex attribute component class | **fallback added** | when a pipeline is built, the shadow format's component class (float/uint/sint) is compared with the input type the shader declares and a mismatch warns once -- "location == attribute number" above is an **assumption**, and if it is wrong the vertex data attaches to the wrong slots **without any error**, so this check makes it visible at the first draw |
| dummy buffer for the constant fallback | **in place** | `vk_pipe_dummy_buffer()` lazily creates a 64 KB all-zero vertex buffer, bound at `VK_PIPE_DUMMY_BIND` by a draw (a fallback attribute advances one element per instance and the instance count is the game's, so 16 bytes would not do) |
| four primitives without counterparts | **measured before deciding** | `vk_topo_of` accumulates a draw count per primitive number (`vk_pipe_prim_counts`) so the device answers whether they appear, and the expansion is decided then; the counters are read-only and change nothing |
| two sets (vertex on set 0, fragment on set 1) | landed | see step 10: when two stages claim one binding number Vulkan has no answer (one binding, one type), and splitting by **stage** is the smallest correct fix (the `DescriptorSet` decoration is rewritten at load time) |
| stencil masks in the pipeline key | unchanged | Apple's `pso_key` carries `vm`/`wm` the same way, so the behaviour matches and is not a defect |

The device-ready line gained two capabilities along with it (Adreno 830): `non-solid fill: yes; per-instance vertex
step: yes (with 0)`.

**Next**: descriptor writing and really issuing draws (step 10 below) -> resolving textures by NVN slot (step 11) ->
**reading one frame back offscreen** to look at it.

### M2 step 10: descriptor writing, real draws and GPU timing (all three measured on a device)

**Descriptor writing**: one descriptor pool per **in-flight submission slot**, reset whole when the slot is reaped (fence
signalled) -- the replay layer already has in-flight slots with fences (`g_inf`), so no extra synchronisation is needed.
Buffers always use **dynamic offsets** (the guest data sits in zero-copy pools and `mtl_addr` returns (buffer, offset));
**the dynamic offset array has the same source as the layout** (one `vk_pipe_binds` table, never computed twice -- two
computations drift apart sooner or later). A zero or unresolvable address **writes a blank**: an unwritten descriptor is
undefined while NVN itself reads 0 on an empty slot, so a blank is a faithful fallback.

**The first real draws on the device**: **1.22 million real draws, 83 graphics pipelines, zero failures, no GPU hang, clean
exit**. This batch also let **one of my own diagnostics catch a real bug**: NVN's index type is `0 = 8-bit / 1 = 16-bit /
2 = 32-bit` (`NVNindexType` in `nvn.h`) while I had guessed "2/4 bytes" -- the device uses 1, which **happened** to read
correctly as 16-bit, but 32-bit would have been read as 16-bit. Fixed against `nvn.h`; 8-bit indices (not in core Vulkan,
they need a KHR extension) are reported and that draw skipped, as on Apple.

**GPU timing: from a stub reporting 0 to a real measurement.** On Android `mtl_gpu_ms` was **a stub** (`return 0`), so the
"GPU 0.00 ms" in the log was never a measurement -- and **nobody would find that odd**, because 0.00 looks exactly like
"the GPU is doing nothing". Now a timestamp is written at the start of every pass (once it really opened) and at the end
(before it closes) with `vkCmdWriteTimestamp`; when the slot is reaped they are read back and "last end minus first
start" becomes that submission's GPU span (on TBDR passes are pipelined and spans cannot be summed -- Apple's pass table
carries the same note), accumulated into a per-frame ring (1024 entries) and looked up by frame number, returning -1
while incomplete (the same semantics as Apple's `GPUStartTime/GPUEndTime`). The prerequisites are asked for properly:
**this queue family's** `timestampValidBits` (not a device-level property) and `limits.timestampPeriod`. Device result:
`timestamps 48 bits valid, 52.08 ns per tick`, then one line per 300 frames with **`GPU p50 6.02 / p95 6.07 / max 8.02
ms`** (the frame budget is 33 ms = 30 fps). The -1 semantics show up on the device too: the first window reported 292
frames, with seven more not yet complete and correctly excluded.

**The texture format table completed against `nvn.h`**: from the four gaps the device log reported (`0xb/0xd/0x6/0x1`),
filled in along the `NVNformat` enum to **154/157**, including **ASTC** (the Switch's main compressed texture format,
natively supported by Adreno) and the BGR series; the 32-bit `UI2F/I2F` have **no counterpart** in Vulkan
(`_USCALED/_SSCALED` exist only for 8/16-bit channels) and mapping them to UINT/SINT would hand the shader integers where
it expects floats -- so they are **not guessed** but reported. The offline reverse check (`case` values against the enum)
found no typos.

**The texture registry**: the host calls `mtl_reg_tex` when the game registers a texture (a stub on Android before) and it
now stores a copy **by value** (the game copies `NVNtexture` by value and a pointer would go stale), alongside the
sampler registry `mtl_reg_samp` (also promoted from a stub).

### M2 step 11: textures and samplers resolved by NVN slot (the device binds textures for the first time)

**The missing link was "binding <-> NVN slot"**: a texture binding in the SPIR-V is not an NVN texture slot, and
`<key>.res.json` carries no such correspondence (Apple's reflection has `reg`, this one does not). It took two steps:

1. **Statistical pairing first** (group by dimension, then in order): from 314 shaders with one texture and one slot --
   **forced pairings** -- `type 3 -> 2D` came out; after adding the count-elimination results `4 -> 3D` and `2 -> cube`,
   all 44,135 bindings in the library were 98.4% self-consistent, and a **control** (corrupting one mapping on purpose
   drops it to 65.1%) showed the consistency figure discriminates rather than being self-serving.
2. **Then exact decoding** (from a parallel probe): `binding` is really the **translator's own dense counter** (the four
   callbacks in `Program.cs` share one `_binding++`), while **the `handle` already carries the slots**, with the formula
   implemented twice in the repo (`decode_tex` in `dmsh_build.py` is the same one the Apple-side artifacts use;
   `maxwell_refs.py` plus `README.md` record the whole-library measurement: texture slot r at `c2[0x168+8r]`,
   **sampler slot r at `c2[0x568+8r]`**, image slot r at `c2[0x120+8r]`). The handle word splits into `o0`/`o1` and
   **when `ty != 0` both words must be tried** (the separate-sampler half is in o1; trying only o0 misses about 7%).
   **Cross-checked against three sources**: a sample of 800 shaders / 5199 texture entries had every decoded texture slot
   **land in `shader_facts.tsv`'s slot set** (that table comes from the PC reflection, an independent source), zero
   failures and every sampler slot inside 0..10 -- matching the device's "non-zero sampler slots 0..10". On the device
   the two independent routes confirmed each other as well: decode 414 ok / 9 failed against the statistical pairing's
   409/14.

**Samplers**: texture slots and sampler slots are **two numberings** (two handle tables), so a sampler must be taken by
its **sampler slot**; the handle's high 20 bits are the sampler pool id and `0xFFF` is the "slot filled in" sentinel.
Device result: **520k bound, 28k blanked, "sampler id not found" 0** -- whenever a sampler slot really holds something it
resolves; those 28k blanks are about equal to the empty-slot count (declared by the shader, unused by that draw) and a
blank is exactly the counterpart of an empty slot.

**Two mistakes of my own, both corrected by measurement or probing** (recorded for the record): (1) an early version's
"this stage bounds no textures at all" was a **bad sample** (concluded from the first three slots; it actually binds 36);
(2) I once read the sampler table with a **texture** slot, took the `0xfff00000` it returned for an "empty-slot sentinel"
and called it closed -- the truth was that it was simply **the wrong slot**.

### M2 step 12: views land (sampling the way the game means it)

An NVN texture view can **limit the mip range**, **limit the layer range**, **reinterpret the format** and **reinterpret
the target** (a 3D texture seen as a 2D array, say). This layer used to have **no view parameter at all**: the view was
stored at registration (`treg_e.has_v/v`) and **never read**. Now `vk_tex_view(const tex_t*, const view_t*)` caches up to
four sub-views per texture by view content, with `subresourceRange` from `base_level/nlevels/min_layer/nlayers`, format
from `set_format` and `viewType` from `set_target`; image creation adds `MUTABLE_FORMAT` (the precondition for
reinterpreting formats) and `2D_ARRAY_COMPATIBLE` (3D only). The two render-target paths still pass the base view
(`p_set_rt`'s views are not in the shadow state yet), as noted in the code. **Device readings**: `views: 234,866 base, 0 sub-view hits, 0 sub-views created` -- **not a single sub-view was
created**, and that number **explains itself**: the textures the game registers for *sampling* all come from
`TextureViewSetDefaults` (all zero = no limits), while the views that really limit mip levels or layers belong to
**render targets** (`SetLevels/SetLayers` are called 2369/2030 times), and the SET_RT path deliberately still passes the
base view (see "the two render-target paths still pass the base view" above). So the zero points exactly at what to wire
up next: get `p_set_rt`'s `cview/dview` into the shadow state and handle "an attachment whose view limits the mip range
has a different size" along with it.

**The same device run measured that judgment too**: `ReadTexels read a GPU-written texture 0 times` -- this window never
reads back a GPU-written texture, so "read back through the VkImage" is still only a **debugging tool** rather than a
functional gap (the offscreen screenshot rides on it).

### M2 step 13: the present-screenshot chain (self-contained minimal PNG + driver-side readback + present/on-demand shots)

**This step exists so that the picture can be seen at all**: M2's acceptance is "a picture appears", and with no window
(that arrives in M3) the only way is to read a frame back to disk. Four pieces: (1) **`vk_png.c`, a self-contained minimal
PNG writer** (8-bit RGBA, DEFLATE stored blocks, no dependencies), because the picture has to land as a file that opens
directly and Android cannot link Apple's imaging frameworks; (2) **`vk_tex_grab_ser`, a driver-side readback** (copy into
a host-visible buffer and map it) complementing "read the pool directly", since once the GPU has written a texture the
pool is no longer the truth; (3) **`mtl_present` landing** -- the guest hands over the **swapchain texture** when it
presents, the **authoritative** answer to "which picture does the game mean to show" (no guessing among hundreds of render
targets); (4) `DM_NVN_SHOT=<frame>:<path>`, `DM_NVN_SHOT_ALL`, `DM_NVN_SHOT_SCAN` and `dm_gfx_shot_at`, sharing Apple's
**names and meanings**, so scripts and know-how carry over.

**On the device the picture really lands on disk** -- `screenshot 1600x900 format 0x25 -> shot_900.png 成功`, 5,761,403
bytes = 1600x900x4 plus a PNG header, and it opens after being pulled. And its very first use **exposed two real bugs**
(both fixed): (1) **a window (swapchain) texture has no guest memory** (the driver owns its storage) while the texture
layer refuses to build an image without memory, so the render-target path only **skipped it in silence**; "window texture"
is now a kind of **storage-ownership registration** (`vk_tex_mark_window_ser`, reported by the guest when it builds a
window) and such a texture still gets an image, skips the upload and enters as "the GPU has written it"; (2) **an
attachment that cannot produce an image used to be skipped in silence** -- exactly why (1) stayed hidden so long, and now
it speaks up. **`COPY_T2T`** landed along the way (texture-to-texture copy, previously a counting stub); its very first
one on the device is a **full-screen 1600x900**, which is precisely how "move the picture into a texture" works.

### M2 step 14: the measured conclusion -- **the GPU has written no colour content** (where M2's acceptance actually stands)

With the instrument in place, the first question asked of it was "where is the picture", and the answer is **there is
none** (two scans, at frames 300 and 800, 64 textures of 256x256 or larger each):

| What was looked at | Result |
|---|---|
| Readable colour render targets (serial 28 `RGBA16F`, serial 23 `B10G11R11`, both 1600x900) | **all zero** (raw peak 0.000) |
| The present texture (swapchain serials 5/6/7) | **all white** (mean luminance 255, the same at frames 300 and 900) |
| Why the other 62 large textures cannot be read | **not one of them is a picture**: BC3 compressed (format 138), depth/stencil, `R32_UINT` integer, `R16_UNORM` single channel |

**Conclusion**: close to **a million draws** are issued in that window and **not one pixel lands** in any target, so what
blocks progress is **the rendering chain itself**, not the ability to see it (the same readback chain has already produced
a valid 1600x900 PNG). Two warnings in the same log point at the causes:

- `combined image sampler: writing a blank texture (res.json says cbuf_slot=-N ...)` and
  `uniform buffer: no buffer address (address N, slot resolution N): writing a blank`
  -> descriptors **fall back to blanks in large numbers**: the shader samples an empty texture and reads empty constants;
- `vertex attribute N (... at position N) has a component type that disagrees with the shader: format NxN vs shader float`
  -> the vertex stream a format lands on may be wrong, and the geometry may collapse entirely.

**Also worth recording**: so that a scan is actually usable, the readback covers the formats this device really uses (8-bit
RGBA/BGRA, `RGBA16F`, `RGBA16_UNORM`, `RGBA32F`, `B10G11R11_UFLOAT` -- the numbers were **checked against the NDK header**,
not guessed); every screenshot reports **mean/min/max luminance** (so "white" and "black" are numbers in the log); caching
the staging buffer took each readback from about 0.45 s to negligible; and a scan only covers 256x256 and up (a picture
cannot be a 16x24 sprite, and every read waits on the queue once).

### M3 step 3: from "not one pixel lands" to "it really draws into the target" -- seven traps (2026-10-14)

This round took apart "the draws go out yet the targets read all zero". Two **no-code** control experiments first pinned
the scope down: `DM_GFX_PROBE=<serial>` (clear one target to a known blue, then read it back) read a **luminance of 152.0**,
exactly the mean of (0, 201, 255) that the clear colour (0, 0.6, 1.0) gamma-maps to, so **the readback chain is sound and
"all zero" is real**; `DM_NO_DEPTH_TEST=1` stayed all zero, so the depth test is not the gate. Then, one by one:

| # | Symptom (device readings) | Root cause | Fix | Verified by |
|---|---|---|---|---|
| 1 | 348k draws, all-zero targets, zero errors | **The Android side had no NVN default state**: the shadow is zero-initialised while `gfx_state_fill` reads it unconditionally, so the **write mask is 0 (not one channel written)** and the depth range is `[0,0]`. The game may rely on NVN's defaults, which is exactly why Apple wrote `state_reset()` | Add `state_defaults()` (item for item with `nvn_mtl.m:2306-2314`), applied at the **first `mtl_exec`** (the same place Apple uses); `OP_VIEWPORTS` no longer reads the `v[4]/v[5]` that were never recorded, near/far come only from `SetDepthRange` (default `[0,1]`) | Serial 27 went from **0 to 255** (it has content now) |
| 2 | Whole runs of "1 target ... **not issued**" in the log | **A pass only opened on a clear**: the machinery at the head of the file says a pass opens at the first clear **or draw**, but only `CLEAR_COLOR`/`CLEAR_DS` did, so in streams shaped `SET_RT -> barrier -> draws` the draws were dropped wholesale | Call `pass_begin()` from the draw path, and re-supply the dynamic viewport/scissor when a pass opens (`VIEWPORTS`/`SCISSOR` arrived while it was closed) | Dropped draws **48% -> 31%** |
| 3 | `binding N (uniform buffer): no buffer address (address 0, slot resolution N): writing a blank` | **The constant slot is not the number res.json hands over.** The rule (same source Apple uses, `reflect()` in `dm/gfx/tools/dmsh_build.py`): a name like `fp_c3` means NVN constant slot = **that number minus 3**; a `1` in a name is the special `c1` bank and a `2` the driver bank, neither a plain UBO; no `cbuf` entry at all means Ryujinx's `support_buffer`. Checked against the real SPIR-V names: `fp_c3` -> slot 0, `fp_c13` -> slot 10, while the game binds **0, 3 and 10** (slot bitmap `0x409`) -- we looked up 1/3/13 and only 3 collided by luck | `desc_buf_range` now follows the rule; two 64 KB **special constant segments** are built (`support_buffer` with `render_scale = 1`, since 0 would make the shader scale by 0; `c1`/driver bank start all zero -- the real `c1` data lives in the control segment, see "what is left") | UBO blanks **gone**; per-stage bind counts 6/6/3 -> **1094/3367/695** |
| 4 | The viewer crashes the moment it is on (in the driver, `vk_tex_view`), the front end stops refreshing at about 3 s, the guest stops mid-run | **Two threads share one VkQueue with no external synchronisation.** Vulkan requires `vkQueueSubmit` / `vkQueueWaitIdle` / `vkAcquireNextImageKHR` / `vkQueuePresentKHR` to be serialised by the caller on one queue, while here the guest submits on a worker thread and the front end presents and reads back on the main thread. The three shapes are one thing | `vk_device.c` gains `vk_queue_lock/unlock`, taken around submitting, queue-idle waits, image acquisition and present | see below (this one is not fully pinned down yet) |
| 5 | After fixing #4, **both sides stopped together** (the wait-point dump showed the guest "recording, pass open" and the front end's one-second heartbeat gone) | **Image acquisition must not hold the queue lock for long**: with the screen off, or the compositor busy, a swapchain can have no image available for a long time, and while that holds the lock every guest submit piles up behind it | `vkAcquireNextImageKHR` now gets a **100 ms timeout**; on timeout the frame is dropped and the next one retries | to be re-checked with a longer run |

**Two traps in the instruments and documents themselves** (worth remembering, or the next stall will have no scene again):

6. **The Android side never registered a "wait-point dump".** Apple registers `mtl_wait_state_dump` during device
   initialisation (`nvn_mtl.m:275`) and Android did not, so when the guest stopped the log held nothing but heartbeats and
   "where is it stuck" was unanswerable. `nvn_vk.c` now has one of the same name and meaning: in-flight slots and the fence
   value each holds, which slot is recording, whether a pass is open, and the submit / present counts. **Its first use read
   out "the guest is stuck recording with a pass open".**
7. **The watchdog is off by default, and the tail of its dump is lost to `_exit`.** Without `DM_WATCH_STALL` it does not
   look at all (the product default, see `crash.c:314-325`); and when it does fire it prints the whole scene and then
   `_exit`s, while on Android the host's stderr is a **pipe** written to the file by another thread (the app's log pump) --
   measured as the **tail of the dump being cut off entirely** (the "wait points" line sits exactly there). Fix: `crash.c`
   gives the log 400 ms before exiting (Android branch only).

8. **On Android there is wait state but no PC / backtrace.** In the same dump the thread lines (what each thread waits on and
   where its last svc came from) are good, but the `pc=... x19=...` register line and the backtrace that follow are **all
   zero** -- because `thr_bt` (`hle_thread.c`) uses mach's `thread_suspend` / `thread_get_state` / `module_of`, which Android
   does not have, and it does not report the failure, so it silently prints zeros. Reading "the guest is running guest code"
   now rests on two fields only: `last svc` and `lr=module+offset`.
   **Fixed**: `hle_thread.c` gained an Android branch (`proc_thread_line`) that reports three things per guest thread from
   `/proc/self/task/<tid>/` -- the state character from `stat` (R running / S sleeping / D uninterruptible), the kernel
   wait-point symbol from `wchan` (`futex_wait_queue`, `__arm64_sys_nanosleep`, `pipe_read`, ...), and the **number,
   arguments, stack pointer and PC** from `syscall` (present only while blocked inside a syscall; a pure user-space spin
   reports just `running`). **Those are exactly the two cases that need telling apart.** Host-created threads are listed as
   well (the old block went through mach's `task_threads` and did nothing here, so a dump held guest threads only).

**It classified "the guest stopped" immediately (real device, 2026-10-14)**: stopped at `CharacterCreate:Hover:Barbarian` --

| Thread | Reading | Meaning |
|---|---|---|
| guest main thread 0x1001 | **state R, `syscall running`** | **a pure user-space spin**, in no syscall or wait |
| other guest threads | `S` in `futex_wait_queue` / `__arm64_sys_nanosleep` | all asleep (normally idle) |
| our side (wait points) | no pass open, nothing recording, 1/8 slots in flight | the replay layer is idle and owes nobody |
| process main thread / log pump | `S` in `do_epoll_wait`, `R` in `pipe_read` | Android's main thread and the log pump, both waiting normally |

In other words this is **not "stuck in the driver or a lock"** (that would be S + futex) but **the game itself spinning in user
space, waiting for a job that never completes** -- the same shape as the state documented in Apple's comment (the main thread
spinning in the job pool's "help while waiting", see the `thr_bt` note in `nvn_mtl.m`). Getting its PC is the next step:
Android has no ptrace permission, so the way in is **signal sampling** (send a signal to the thread, read the pc out of the
`ucontext` in the handler, symbolize module + offset).

**How the existing sampler (`DM_SAMPLE=<ms>[:thread]`) behaves on Android** (it is already in the repo; no code was changed,
recorded for reference): it sends `SIGUSR2` to a thread of our own, copies pc / lr / the frame-pointer chain into a ring buffer
in the handler, and a sampler thread symbolizes and prints. It **works** (the device produced `thr=0x1001 pc=... lr=ioctl+0xa0`,
showing the guest main thread going in and out of syscalls at that moment), but two things need care:

- at a **10 ms** interval the process hit `CRASH sig=11` **1.4 s** after start (`pc` inside libc's svc stub, so the interrupt
  likely landed on an `svc` instruction);
- at a **100 ms** interval the samples **stopped** after about **9 s** (the guest was never interrupted again) -- from that
  moment the signal is either blocked by the guest or the target thread is no longer in user mode, so it cannot yet answer
  "which function is it spinning in".

Two changes would make it usable: (1) only send the signal while `/proc/self/task/<tid>/syscall` says `running` (that is, while
the thread really is in user code); (2) symbolize host-module pcs as well (only the guest modules `main` / `sdk` are recognized
today). Also, the frame chain in the handler goes through `vm_read_overwrite` and is always empty on Android, so no backtrace.

**Both were added the same day**: (1) the always-on sampler now interrupts only while `/proc` says `running`; (2) a **one-shot
sampler for stalls** (`crash_set_stall_sample`, called by the watchdog and by `crash_dump_now`) sends one signal each to the
threads whose state is R and which are not inside a syscall; (3) `crash_off` now reports **which .so plus the offset** when a
dynamic library carries no symbols (Android vendor drivers are like that; it used to print a bare address).
**Its first run caught it**: the thread spinning in user mode was the **front end** (`thr=0`, `pc=libc.so+0x991ac`, return
address `vk_win_present_rgba+0x260`) -- in other words it was stuck on the **queue lock** that had just been added (the guest
still holds the lock while waiting for the driver inside a queue call, so the front end sits on it and the one-second heartbeat
stops at the same moment).

**The fix that followed**: give the window **its own queue** (a second one on the presenting family, `vk_queue_win`), so the
front end's acquire/present no longer contends for the lock with the guest; the shared-queue plus lock path remains only for
families that hand out a single queue. The device reads `两条队列：客体用 #0、前端（取图/上屏）用 #1` and the **front end's
heartbeat no longer disappears**; the guest still stops at present 59, but that is now a separate guest-side matter (the
wait-point dump shows our side idle: no pass open, 1/8 slots in flight, queue lock unheld). The lock also records "holder /
what for" (`vk_queue_note`) and the wait-point dump prints it, so the next mutual block is visible at a glance.

**A hypothesis that was disproved (worth recording so it is not walked again)**: when the device stopped at
`CharacterCreate:Hover:Barbarian`, it looked like the guest was **loading game data and stuck on disk I/O**. Two checks were
run: (1) `DM_WATCH_STALL=0` (watchdog records but does not kill) over a long run -- the counters **did not recover**, so it is
a real stop and not slowness; (2) the harder one: after the stop **even the front end's one-second heartbeat was gone** (70+
seconds of silence) -- a guest waiting on I/O would not stop the front end. The scene is: the main guest thread with
`last svc 0x1a` (ArbitrateLock), `waiting: running/not waiting`, in-flight slots 1/8, **no pass open**, no crash. In other
words **both threads stopped together inside some driver or lock wait**, and the next step is to give Android the PC /
backtrace instrument before going further in.

### M3-2k: buffer textures (texel buffers) land -- the hand-off's deciding action, and the proper fix, in one go (2026-10-14)

The hand-off said: "**first confirm texture 701 really is `target 10`**, then choose between the proper fix (a new
texel-buffer resource kind) and a stopgap (substitute a 2D texture, which must be recorded)". Both halves happened this
round: the confirmation first, the **proper fix** second (the "2D texture instead" stopgap was never taken).

**1. Measure before changing anything** (the order matters: before this round we did not even know whether the test
existed). The classifier in `vk_spv.c` had **no test at all** that could produce `VK_SPV_TEXEL`: `OpTypeImage`'s `Dim` was
never parsed, `OpTypeSampledImage` was always counted as a combined image sampler, and the descriptor pool had no
`UNIFORM_TEXEL_BUFFER` quota (line 96 of `vk_pipe.c` had mapped that kind for a long time -- it simply had no producer).
Three things were measured first, over the **6923 artifacts** on this machine:

| Offline measurement (6923 `.spv` files plus their `.res.json`) | Reading |
|---|---|
| Bindings with `Dim = Buffer` | **2991**, all with `sampled = 1` and a sampled type of `float`, spread over 2991 shaders |
| Texture bindings whose reflection says `cbuf_slot < 0` | Also exactly **2991**, every one with `handle = 0x6a` -- a **one-to-one** match with the line above |
| Which bindings they are | `binding 2` 2836 times, `binding 1` 154, `binding 4` 1 (which fits the eight blanks in the log appearing only on bindings 1 and 2) |
| Does the `OpTypeImage` an `OpTypeSampledImage` points at always precede it? | **12111 times, always earlier, 0 exceptions** |

The first two lines together are the clincher: **"texture blank" and "missing resource kind" are the same thing** (the
counts match exactly, which is not the same as "looks similar"). The fourth line is what keeps the implementation to one
byte of storage: when the scanner sees an `OpTypeSampledImage` it can simply **copy the `Dim`** over from the image side
instead of keeping a second mapping table -- and that copy is backed by a measurement rather than an assumption.

**2. The fix** (five files, **the same idea as Apple's** `nvn_mtl.m:1204-1220`: build a texture-buffer view straight on
the memory-pool buffer, zero copy, with the GPU reading the pool's current content at execution time):

1. `vk_spv.c`: parse `OpTypeImage`'s `Dim` and classify `Dim = Buffer` (when it is not a storage image) as `VK_SPV_TEXEL`;
   `OpTypeSampledImage` copies the `Dim` over;
2. `vk_tex.c`: a new `vk_tex_bufview` -- the range is **bytes per element times element count**, the pool address goes
   through `mtl_addr` to become (pool buffer, in-pool offset), and then a `VkBufferViewCreateInfo{offset, range, format}`.
   **Two of these are Vulkan's rules rather than choices**: (a) there is no `bytesPerRow` (a texel buffer is a linear
   array of elements to begin with) but the **offset has a hard alignment requirement** (`minTexelBufferOffsetAlignment`);
   (b) the format must carry `VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT`, so compressed and depth/stencil formats are out.
   Both are checked, and a failure is reported **once per cause**;
3. `nvn_vk.c`: the descriptor write gains a branch (`pTexelBufferView`, one of three mutually exclusive forms alongside the
   image and buffer ones); the pool buffer's usage gains `UNIFORM_TEXEL_BUFFER_BIT | STORAGE_TEXEL_BUFFER_BIT` (**the bits
   cannot be added later**); the descriptor pool gains one quota entry;
4. `vk_pipe.c`: a **dummy texel view of one all-zero element** (NVN itself reads 0 on an empty slot -- the same reasoning
   as the image fallback);
5. `vk_device.c/h`: expose the physical device (`vk_phys`), because a texel buffer's offset alignment can only be asked of
   the device's limits.

**3. Device readings** (Redmi K90 / Adreno 830, `DM_WIN_SRC=0` + `DM_WATCH_STALL=15`, the same environment as the run
before it):

| Reading | Before (`run-c0-driver-segment.log`) | After (`run-texel-buffer.log`) |
|---|---|---|
| `【黄】贴图 target 10 ... 暂不支持（缓冲区 / 多采样 / 线性布局）` | 1 line | **0 lines** |
| `贴图写空白` | 8 lines (two on binding 1, six on binding 2) | **0 lines** |
| Buffer texture | -- | 【绿】 lands: serial 22, **65536 elements x 16 bytes**, format `0x2e -> R32G32B32A32_SFLOAT(109)`, pool offset `0x112b2c0` (built straight on the pool buffer) |
| The registry entry's target | -- | 【查】 **tid 701 -> the registry entry's target is 10**; bound as a texel buffer on `binding 1` and `binding 2` |
| New failures | -- | **None**: the red lines are still the five old "shader `16479cc6d75c2b28`'s artifact is not in spv" plus the one watchdog line |

**4. The reading the hand-off was missing came from an A/B.** The path got a one-flag fallback to a blank (`DM_NO_TEXEL=1`,
**an instrument rather than a feature**, off by default). In group B (blank) the eight blank lines **come back exactly**,
and they carry the three numbers the previous round never printed:
`cbuf_slot=-1 handle=0x6a -> slot 8; the handle bound on that slot is 0x2bd (low 20 bits **701**, registry **found**)`.
In other words **all three links -- slot decoding, handle, registry -- are sound**; the one thing missing was turning the
registry entry into a view, which the image side can never do because the texture is `target 10`. That chain matches the
hand-off's inference exactly, and now it has numbers behind it.

**5. The A/B also removed a false suspicion** (worth recording so the next person does not doubt themselves too). Two runs
after the change both stop at "58-61 presents / about 6200 draws" while the 21:47 run reached "340 presents / 370k draws",
which looks like **this change slowed the run down**. So **only one environment variable** was changed (`DM_NO_TEXEL=1`,
nothing else touched) and the two groups read **identically** (58 presents, 6208 draws, 61 submissions, the same stage and
the same last line). Then the last heartbeat of **all nine historical runs** in `ab-viewer-off` was listed: **every one of
them except the 21:47 run stops at 55-60 presents** -- so stopping at 60 is the norm and the 21:47 run is the outlier.
**Conclusion: neither the frame rate nor where it stalls has anything to do with this change.**

**6. One real bug fell out along the way (left for its own step, not fixed here)**: in `vk_spv.c`
`OP_TYPE_FLOAT = 21 / OP_TYPE_INT = 22` are **swapped** relative to the SPIR-V spec (the spec has 21 = `OpTypeInt` and
22 = `OpTypeFloat`; in the local artifacts `OpTypeInt %1 32 0` is four words and `OpTypeFloat %4 32` is three, so it is
plain to see). It sits exactly on the judgment behind milestone A's other remaining item (**vertex attribute component
class vs the shader's declaration**) -- which means the "shader float" half of the warning
`顶点属性 2 ... 格式 0x27 vs 着色器 float` may itself come from an inverted test. Following the "change one thing at a
time" rule, it is left as the next step.

### M3-2l: **two** independent faults hid inside the vertex-attribute test (one reasoned out, one caught by a probe) (2026-10-14)

The hand-off described milestone A's remaining item as "vertex attribute component class vs the shader's declaration" and
offered one root-cause candidate (the swapped opcodes above). Fixing it for real showed that **two independent faults sat
on the same reading** -- and the second one was **caught by a probe, not reasoned out**.

**1. Measure first (offline, over the 6923 artifacts)**:

| What was measured | Reading |
|---|---|
| The shader the warning names, `3deaf68fd65b5298`: its stage and inputs | A **vertex** shader; locations 0/1/5 are float x4 and **location 2 is uint x4** |
| The format the game gives it | `0x27` = `R8G8B8A8_UINT` (attribute format table: `(0x27-1) % 12 = 2` -> uint-like) -- **the two sides agree** |
| Self-proving numbering: opcode x word count | `21` occurs **only** with four words (13846 times), `22` **only** with three (6923 times = one float32 per module) -> 21 = `OpTypeInt`, 22 = `OpTypeFloat` |
| All vertex inputs in the library | 11164, **all vec4** (float 10294 / uint 870 / sint 0), **no matrix (array) attribute**; 580 vertex shaders carry a uint |

So that warning **had always been a false alarm** (the shader wants uint and the game supplies uint); what was broken was
the test itself.

**2. Correct the numbering** (`vk_spv.c`): it read `FLOAT = 21 / INT = 22`, so `OpTypeInt` (21) landed in the FLOAT arm
while `OpTypeFloat` (22) could not enter the INT arm that asks for `wc >= 4` (OpTypeFloat has three words) -- making
`== OP_TYPE_INT` **never true** (those two word-count guards are part of the test; the comment says to read the pair
together). The warning also changed from "print the first occurrence" to "the first eight plus one per 1024 plus a
running count": when the test itself has been wrong, the **total** matters more than a sample.

**3. After the fix, run the probe before the game** (three seconds on the device) -- and the reading **did not move at
all**: `顶点属性分量：float 11164、uint 0`. "The numbering is fixed and yet the reading is identical" is itself what
points at the **second fault**: `var_type` holds an `OpTypePointer`, and the attribute block **never dereferenced it**, so
`comps` stayed 1 and `ct` was the pointer's id (naturally absent from `scalar_op`) -- the class was **always** float.
Adding "pointer -> input type" is the whole fix (the array arm is deliberately **not** written: all 11164 inputs in the
library are measured to be vec4, with not one matrix attribute).

**4. Self-proof on the device** (probe, 3.1 seconds): `顶点属性分量：float 10294、uint 870、sint 0`, with samples showing
`位置 N=uint x4` -- matching the offline Python count **item by item** (10294 / 870 / 0; 11164 inputs; all x4). The probe
gained this line this round (it used to report binding kinds only), and its value is that **this fix can prove itself
without running the game** -- and that anyone touching those two opcodes again turns it red immediately.

**5. Device readings** (Redmi K90 / Adreno 830, standard configuration): the `顶点属性` warning went **1 -> 0**;
`贴图写空白` 0, `拿不到缓冲地址` 0, `target 10 暂不支持` 0 (the previous round's results hold); `图形管线 80（失败 0）`;
and it stalls exactly where this device normally stalls (56 presents, 6084 draws, the same stage). In other words
**correcting the test produced no new signal**, and both of milestone A's items are now done (the other being the buffer
texture of "M3-2k").

**6. One thing recorded for the first time** (**not introduced this round**): one run's log contains
`【红】上传命令等待失败 VkResult -4` (-4 = `VK_ERROR_DEVICE_LOST`) followed by a cascade of a hundred-plus
`【红】贴图上传失败 serial=... 格式 ...` lines. Across runs: the 21:27 run (long before this round) has the same line, this
round's A/B groups both have it, and the 21:47 run does not -- **sporadic, and unrelated to the last two changes**; but it
shows that "device lost" surfaces on this texture-upload path (a one-off submit plus a queue-idle wait), which deserves a
look of its own (who loses it first: the guest's own commands, or that one submit of ours).

### M3-2m: the render-area trace -- item 3 is closed, and it asks "which level is the picture at" on the way (2026-10-14, device)

Item 3 bet on "deriving the render area from the viewport drops fragments". Following the "measure first" rule, a
side-by-side reading was added where a pass opens (`DM_PASS_AREA=1` measures only; `=2` swaps in the attachment size as a
control) and one round was run on the device. **That suspect is dead**, but three other things surfaced in the same round.

**1. The render area is not the blocker** (the distribution over 16384 pass opens)

| Reading | Value |
|---|---|
| Main-scene passes | render area / attachment / viewport **all three 1600x900** (opens 3, 267, 339, 409, 423, 553, 633, ...) |
| Computed render area below the attachment | **267 / 16384 = 1.6%** (11 / 28 / 62 / 131 / 267 at opens 1024 / 2048 / 4096 / 8192 / 16384) |
| Collapsed to 1x1 | **0** |
| "No viewport set" when a pass opened | **0** (the viewport always arrives before the pass) |
| Offset viewport (x/y != 0) | **0** |

That 1.6% is not an anomaly: those are passes whose viewport covers a sub-rectangle anyway (400x225 into 1600x900, say),
which under Metal's semantics also draws only that rectangle. **Item 3 is closed: not the blocker** (so the `=2` control
does not need running).

**2. The trace turned up two new readings**

1. **"Attachment 0x0" shows up often** -- those are **depth-only** passes (`ncolor=0` in the shadow). There is no colour
   attachment to clamp against (the existing clamp only applies when there is one), so only the viewport or the 1x1
   fallback is left. A branch nobody had noticed before.
2. **Pairs like "viewport 1600x900 / attachment 800x450" appear** (opens 367, 8192, ...). The render area is clamped to
   800x450 correctly, but **the viewport handed to Vulkan is still 1600x900** -- for an attachment limited to a mip, the
   viewport is not scaled with it. Apple's side does not handle it either; recorded on its own for later.

**3. The "scan first" of item 7 of "next steps" was run: still all zero, but that sentence itself has to be narrowed**

`DM_NVN_SHOT_SCAN=50,300,340` scanned 26 / 63 / 63 textures at >=256x256 across three rounds: **every one reads `brightness
mean 0.0, min 0, max 0`** (a single value throughout, so the `scan/` directory is still empty). On the face of it, step
14's conclusion "the GPU writes no colour content" stands unchanged.

**But the viewer reads the opposite.** Point it at **level 1** (`DM_WIN_SRC=28`, `DM_WIN_LEVEL=1`) and the same serial 28
immediately reads:

```
查看器：序号 28 第 1 级 800x450 第 61 次，亮度 均值 0.3、最小 0、最大 160（读成功 61、跳过 149）
查看器：序号 28 第 1 级 800x450 第 121 次，亮度 均值 0.2、最小 0、最大 106（读成功 121、跳过 149）
```

**The same 1600x900 render target reads zero at level 0 and content at level 1.** That lines up exactly with the batch of
"attachment 800x450" passes in the trace (the `pass_begin` warning "render area width 1600 exceeds attachment 800
(mip-limited view)" is the same fact).

**So step 14's conclusion has to be narrowed to "level 0 carries no content"**: the scan reads level 0 only, and the
picture is at some level. The test has to become a **level-by-level scan** (each level with its own size) rather than
level 0 alone.

**4. One crash on the way**: reading level 1 about 200 times ends in `CRASH sig=11 pc=vulkan.adreno.so+0x2a7ac8` (inside
the driver). The same class as the documented "opening the viewer crashes in `vk_tex_view`"; the suspicion is that the
readback **allocates its buffer for the base image's size and then copies level 1**, so the copy range does not match the
real subresource size -- size it from "that level's dimensions" when fixing it.

**5. The instruments**: `DM_PASS_AREA=1` (measure only) / `=2` (render area from the attachment size, the control), with one
line carrying "render area / attachment / viewport / scissor" side by side plus four running counts (below the attachment,
collapsed to 1x1, no viewport when the pass opened, offset viewport).

**6. Re-checked: the "level 1 has content" conclusion above is wrong (the pitch)** (added the same day, inside the same round)

The first device run after the level-by-level scan landed overturned it. The fault was in the **viewer**: it sized its
buffer and its pitch by the **base** image (`g_src_w * 4`) and then read level 1 with that pitch -- level 1's real row is
only half as wide, so the rows came back shifted and the tail fell into **uninitialised heap memory**, which reads as
"mean 0.3, max 160" of stale bytes. Level 0 read correctly (0.0) precisely because its row width matches the base.
(This is the classic shape of "two numbers that look like a conclusion": **a broken instrument hands you a very
plausible number**.)

Two fixes: (1) the buffer and the pitch both follow **the level being read** (`>> level`); (2) `shot_grab_ser_lv`'s return
value separates "read succeeded" from "has content" (0 = failed, 1 = read fine but a single value throughout, 2 = read
fine with content), and the scan's summary counts with it.

**Once fixed, the two readings agree**:

| Method | serial 28, level 0 | serial 28, level 1 | serial 28, level 2 |
|---|---|---|---|
| Level-by-level scan (fixed) | mean 0.0 | **mean 0.0** | mean 0.0 |
| Viewer (fixed) | -- | **mean 0.0, min 0, max 0** | -- |

**So "the picture is at level 1" does not hold.** At the device's frame 50 the 26 textures at >=256x256 were read
**level by level** 33 times with **0 having content** (serial 27's levels 1 and 2 read **255**, i.e. pure white -- still no
content). Step 14's "the GPU writes no colour content" **stands as it was**; what changed is the **instrument**: it is now
level-complete, and the pitch is right.

**7. The crash, incidentally, is in **our own** code.** This round records `CRASH sig=11 pc=vk_tex_grab_ser+0x7a0`
(the previous round recorded `vulkan.adreno.so+0x2a7ac8`). The scene is "the viewer thread and the scan/present reading at
the **same time**", and readback shares **one resident staging buffer** (`stg_get`), so the first suspects are the buffer's
reuse and the edge of the queue lock. **That is the next item** -- it is no longer the driver's business.

**8. The lesson worth recording on its own**: `DM_NVN_SHOT_SCAN` reading level 0 alone was a **gap in the instrument**,
not a property of the world; and the viewer's "has content" was **another** gap. Together the two gaps were enough to make
a conclusion that does not exist look supported by two pieces of evidence.

### M3-2n: the readback's concurrency crash -- fixed, and on the way a lock-order inversion that looks like a hung game (2026-10-14, device)

Part seven of "M3-2m" left `CRASH sig=11 pc=vk_tex_grab_ser+0x7a0` open (inside the readback, with the viewer and the scan
reading **at the same time**). This round closes it, and on the way I built a deadlock myself -- both are recorded,
because both look like "the game hung".

**1. Root cause.** The readback path has two **shared and mutated** things: the resident staging buffer (`stg_get`, which
**destroys and recreates** it when it is too small) and the one-shot command buffer (`g_cb`). And a readback is started by
**two threads** -- present/scan on the guest thread, the viewer on the front-end thread. Only the "queue submit" stretch
used to be locked, so "A is recording into the command buffer while B destroys and recreates the staging buffer" went
straight into undefined behaviour: A holds a buffer that has **already been destroyed** (or a pointer never mapped), and
that is the `+0x7a0` line.

**2. The fix.** One lock (`g_one_mu`) spans "take the staging buffer -> ... -> unmap after reading" **in full**; the
readback and the control clear both take it.

**3. The trap I walked into (worth recording on its own).** I also folded **uploads** into that lock, thinking "they share
`g_cb`, serialising them together is simplest". The result was **the whole boot coming to a stop with zero presents**, the
watchdog reporting only `beat 0 / stage MainMenuHD:CheckUpdateLinks`, identically across three runs: it looks exactly like
"the game hung at boot", but it is a **lock-order inversion**. The upload path is `tex_of` holding **`g_mu`** calling
`ent_upload`, while the readback is `one_mu -> g_mu` -- together a cycle: the viewer holds `one_mu` waiting for `g_mu`
while the guest holds `g_mu` waiting for `one_mu`.
**The check**: with that one upload site reverted, presents **immediately** return to normal (56 frames, the usual
`CharacterCreate:Hover:Barbarian`).
**The lesson**: before adding a lock, list who holds what when they call in; "it looks like the game hung" can really be
two locks waiting on each other.

**4. One thing still left open (the next change).** Uploads and readbacks still **share `g_cb`** -- the upload site
**cannot** take `one_mu` (see part 3), and really removing the sharing needs uploads to have **their own command buffer**.
It is written down in the code (the upload block in `vk_tex.c`) and carried into "next steps".

**5. Device readings** (Redmi K90 / Adreno 830, `DM_WIN_SRC=28`, `DM_WIN_LEVEL=1`, level-by-level scan at 50/54/57):
two consecutive runs with **no `CRASH`**; the viewer read **301 / 361 times** successfully (concurrent with the guest),
the scan did **33 level reads** each (frames 50 and 54), and presents reached **50 / 56** before falling on **this
device's usual stall** (`CharacterCreate:Hover:Barbarian`) -- the "look at the picture" loop is usable again, and the
stall is unrelated to it.

### M3-2o: uploads get **their own command buffer** -- a real hazard removed; the device loss did **not** go with it (2026-10-14, device)

The item left open by part four of "M3-2n": uploads shared one one-shot command buffer (`g_cb`) with readbacks. This round
moves uploads to `g_cb_up` (`one_begin_on` / `one_end_on` take the slot; `one_begin` / `one_end` stay with the readback
and the control clear). **Two threads recording into one command buffer** is undefined behaviour in itself and is now
gone by construction; uploads therefore also **no longer need** the readback's lock (the premise of the lock-order
inversion in part three of "M3-2n" disappears with it).

**1. Readings after the change** (Redmi K90 / Adreno 830, `DM_WIN_SRC=28`, `DM_WIN_LEVEL=1`, level-by-level scan, three
consecutive runs): **no `CRASH`** in any of them; the viewer read **541 / 661 / 361 times** successfully (concurrent with
the guest); the level-by-level scan behaved; the stall is still this device's usual one (`CharacterCreate:Hover:Barbarian`,
with one run at `MainMenuHD:CheckUpdateLinks`). The upload path reported **no failures**: `deswizzle failed` 0,
`VkImage creation failed` 0, `texture memory alloc/bind failed` 0, `VkImageView creation failed` 0.

**2. But that device loss did **not** go away**: one run still shows `【红】一次性提交等待失败 VkResult -4`
(-4 = `VK_ERROR_DEVICE_LOST`) **once**, followed by **110** `贴图上传失败` lines. In other words, **"sharing a command
buffer" was not its cause** -- the frequency looks unchanged after the split (the old build: 1 per run in two runs; the new
build: 1 in three runs; the sample is far too small to claim more than "no change"). Item 4 of "next steps" **stays**, but
one candidate has now been **ruled out**.

**3. A trap of my own (recorded for the record)**: I renamed the log line in `one_end` to "一次性提交等待失败" while the
tally was still counting the old string `上传命令等待失败` -- so the first pass read "0 lines", and only just failed to
record "not fixed" as "fixed". **Changing a log line's text changes the measuring stick**: the script that reads it has to
change too, or count both strings before concluding.

### M3-2p: the pipeline cache lands on disk -- item 1 of section nine is done (2026-10-15, device)

Item 1 of section nine (one `VkPipelineCache` **shared** by graphics and compute, persisted and loaded back on the next
launch) is done and verified on a Redmi K90 / Adreno 830. This section records three things: **where it lands**, **why the
blob needs a header**, and **a device fact worth more than the cache itself** (the backgrounding route does not work here
at all).

**1. Where it lands** (three places):

| Where | What it does |
|---|---|
| `vk_device.c` | creation, loading and writing all live in this layer: `pcache_boot` (`vk_device.c:134`, called once the device exists), `vk_device_pcache_save` (`185`), `vk_device_pcache_tick` (`246`), `vk_dev_pcache` / `_lock` / `_unlock` (from `103`); the call inside `vk_device_init` is at `543` |
| `vk_pipe.c` | both `vkCreate*Pipelines` calls take `vk_dev_pcache()` instead of `VK_NULL_HANDLE`: compute `287-289`, graphics `668-670` |
| `main_apk.c` | `DM_PIPE_CACHE` defaults to the app's **internal** directory (`609-617`; `/data/local/tmp` is read-only for the APK); `vk_device_pcache_tick()` runs in the one-second heartbeat (`683`); TERM_WINDOW (`549`) and DESTROY (`555`) each write once |

Why the **device layer** rather than the pipeline layer: where the blob came from has to be checked against the device and
driver, and the path depends on which process is running (the APK may only write its internal directory) -- the device layer
knows both. **Pipeline creation must be serialised by the caller** (Vulkan requires external synchronisation on a
`VkPipelineCache`), hence the lock, which the write-back path shares -- which is why those two sites in `vk_pipe.c` are
`lock / vkCreate / unlock` rather than a single line.

**2. Why a header sits in front of the blob**: what `vkGetPipelineCacheData` returns is **tied to the driver and the
device**, and after a driver change the whole thing must be dropped (both the official sample and zeux.io's "Robust
pipeline cache serialization" call this out). The header carries vendor / device / driver / api plus
`VkPhysicalDeviceProperties::pipelineCacheUUID` (`vk_device.c:83`); a mismatch means the blob is **not used at all** --
compiling from scratch beats a stale cache. Both negative tests were exercised on the device (C1 / C2 below).

**3. Device readings** (four runs, `DM_WIN_SRC=28`, `DM_WIN_LEVEL=1`):

| Run | Cache at start | Log (`logs/latest.log`) |
|---|---|---|
| A | none | `管线缓存：没有可用的旧缓存…从零编译` then `写入 0.0 MB` (first pipeline) then `0.6 MB` |
| B | 651 KB | `【绿】管线缓存：载入 636.0 KB`, then growing to `743.1 KB` while reused (appended onto the old one) |
| C1 | 4 KB of random bytes | `【黄】管线缓存的头不认得：丢弃，从零编译` |
| C2 | 64 B, valid header, fake device id | `【黄】管线缓存的来历与这台设备对不上（设备 0x0000→0x44050001、驱动 1→2150760519）：丢弃` |

Where the file lands: `/data/user/0/ai.d2rm.dm/files/pipecache.bin` (visible through
`adb shell run-as ai.d2rm.dm ls -l files`, around 641 KB). C2 incidentally brought out two numbers for this machine:
`deviceID = 0x44050001` and `driverVersion = 2150760519`.
(Runs A / C1 / C2 still used the version that printed everything in MB, so the first pipeline reads `0.0 MB`; anything
below 1 MB is now printed in KB -- `0.0 MB` cannot be told apart between "one pipeline just went in" and "the cache is
nearly empty", which is why that was changed on the way.)

**4. The device fact this round brought out (worth more than the cache itself)**: **the backgrounding route does not work
on this machine**. On going home, MIUI classifies this package as a game (logcat:
`ANDR-PWR-OPT: GAMEPOWEROPT: isGame() ai.d2rm.dm is Game = 1`) and immediately **`libc: kill: send 9` (SIGKILL)**: the
`TERM_WINDOW` never reaches us, the log holds no `窗口那一套已拆`, and `atexit` never runs either. Two runs agree (the pid
disappears within 6 seconds of going to the background). So on this machine the "exit / background" hooks **never fire
once** -- what actually gets the blob onto disk is the **foreground periodic write-back** (`vk_device_pcache_tick`: once a
second, writing only when the data has grown, rate-limited to every 10 seconds). It costs little: new pipelines cluster
around startup and the first scene, so the writes do too; it is a 0.6 MB blob, a `.tmp` file plus a rename (so a SIGKILL
never leaves half a file).

**5. What has not been measured yet** ("how to measure" in item 1 of section nine): **time to `nnMain` and the first-scene
hitch** need a cold-versus-warm A/B on one device, and the usual stall from "M3-2o" (a stall at `CharacterCreate:Hover:
Barbarian` a dozen seconds in) would mask that number, so that A/B waits until the stall has moved. What is established
today is that the cache really is reused: in run B, after loading 636 KB the file **kept growing** to 743 KB, which is
appending onto the old cache.

**6. How to use it / how to turn it off**: it is automatic by default (the APK uses its internal directory, the console
host uses `<DM_ROOT>/pipecache.bin`). For a cold-compile comparison use `DM_PIPE_CACHE=0` (turning the whole path off is
easier to reproduce than deleting the file), or delete that file; to move it, hand over a path (a line in `dm.conf` works
too, since the config is loaded before the device is created).

### M3-2q: item 2 of section nine (`HOST_CACHED`) measured and **ruled out** (2026-10-15, device)

What item 2 left to verify was "**can `vkGetMemoryFdKHR` export type 6**". The probe first gained three things
(`vk_fdmap_probe.c`: it prints **every** memory type, `DM_FD_MEM_TYPE=<index>` names the type to try, `DM_FD_FORCE=1`
tries it even where it is excluded, `DM_FD_SIZE=<MB>` sets the size, plus a "plain versus external buffer" type-bit
readout), and then the A/B ran on **one single binary**.

**1. Probe readings** (Redmi K90 / Adreno 830):

| Question | Reading |
|---|---|
| Allowed types of an external (exportable) buffer | `0x13` -> types 0 / 1 / **4**; **type 6 is not among them** |
| A **plain** buffer of the same size and usage | `0x41` -> types 0 and **6** (so type 6 can back a buffer) |
| Allocating on type 6 (`DM_FD_MEM_TYPE=6 DM_FD_FORCE=1`) | 256 MB / 512 MB / 3968 MB **all succeeded**; fd taken, `mmap(MAP_FIXED)` landed at the chosen address, buffer bound, both directions **pass** |
| Same, in pool style (a second buffer bound at an offset in the same allocation) | succeeded; both directions at the offset pass too |

=> **it can export**, but this is **outside what `vkGetBufferMemoryRequirements` declares**: the spec wants
`memoryTypeIndex` to be one of the buffer's allowed types, and type 6 is not in `0x13` -- tolerated by the driver, i.e.
**undefined usage**.

**2. A/B** (one binary, only `DM_ZC_CACHED` changes; each run sampled `utime+stime` from `/proc/<pid>/stat` at a fixed
**10-second** wall clock):

| Run | zero-copy type | presents | draws (with target) | CPU ticks |
|---|---|---|---|---|
| A1 | 4 | 52 | 3,154 | 778 |
| B1 | 6 | 170 | 59,430 | 1,805 |
| A2 | 4 | 56 | 4,671 | 920 |
| B2 | 6 | 59 | 3,574 | 888 |
| A3 | 4 | 169 | 59,610 | 1,804 |
| B3 | 6 | 52 | 3,154 | 770 |

**Conclusion: no measurable gain, so the item is ruled out and type 4 stays.** CPU ticks simply track the amount of work:
about 3,154 draws ~ 780 ticks and 59,600 draws ~ 1,805 ticks, on both sides alike; **within the same mode the difference
is under 3%** (early-stall mode: A median 849 / B median 829; running mode: 1,804 / 1,805), while the **run-to-run spread
is +-100%** (this device's old habit: stalling at present 50 versus reaching 170+). The wall-clock time to "present 50"
also varies a lot (3.5 s versus 14.5 s) while the CPU time barely moves -- that difference is **waiting** (GPU
synchronisation / I/O), unrelated to these two memory types. No gain to be had while a real risk (undefined usage) is
paid, so it is not changed.

**What is left**: the overlay path **explicitly** prefers the zero-copy type with `HOST_CACHED` and **falls back to type 4**
when it is missing or the allocation fails (`vk_region.c`), under `DM_ZC_CACHED=1` (**off by default**, so the default
behaviour is exactly as before); the probe keeps the switches above for a one-command re-check on another device or driver.

**3. Traps hit this round (written down so they are not walked into again)**:

- **`vk_memory_type`'s fallback rung lands on type 0.** Changing to "prefer `HOST_CACHED`" by only adding a bit to `want`
  makes the second pass of `vk_memory_type` (`vk_device.c:370-378`, "take the first type allowed by `need_bits`") return
  **type 0 (pure `DEVICE_LOCAL`)** -- zero-copy breaks on the spot. The correct shape, which the overlay path now uses, is
  to take the `HOST_CACHED` **zero-copy** type explicitly and fall back to type 4.
- **MIUI refuses `adb install -r`**: it reports `INSTALL_FAILED_USER_RESTRICTED: Install canceled by user` (the on-device
  confirmation is cancelled or the permission is off), which is exactly what `install_apk.cmd` uses. Installing from the
  device side works at once: `adb push .work\apk-build\dm.apk /data/local/tmp/dm.apk` then
  `adb shell pm install -r /data/local/tmp/dm.apk`.
- **`DM_WATCH_STALL=0` is *not* "only log, never kill"** (the comment in `dm.conf` is wrong): `crash.c:347-354` computes
  `g_watch_stall = stall && atoi(stall) > 0 ? atoi(stall) : 15`, so a value <= 0 **falls back to 15 seconds** -- writing 0
  means "stalled for 15 s -> declared dead -> dump -> `_exit`". That is why the process vanished during this round's A/B
  (not a crash, not MIUI). To turn the watchdog off entirely, leave the line out (`if (watch || stall)` is what starts it);
  to observe a longer run, give it a large value.

---

## 9. Optimisation list (**for confirmation**, recorded 2026-10-14)

This is not "what to do next" but one place for the things that **could be faster or more robust** now that milestone A's
two tests are clear (see "M3-2l"), for item-by-item confirmation. **Not one line of code changes before that
confirmation.** Each item carries its evidence (`file:line`) and "how to measure".

The order is by value for money (gain over risk), not by effort.

| # | Item | In one line | Risk | When |
|---|---|---|---|---|
| 1 | Create a `VkPipelineCache` and persist it (**done**, see "M3-2p") | No pipeline cache at all today, so every launch compiles from scratch | Low | landed 2026-10-15 |
| 2 | Ask for `HOST_CACHED` for the zero-copy type (**measured, and ruled out**) | Type 6 can export an fd, but there is no measurable CPU-time difference and it steps outside the buffer's declared types | -- | closed ("M3-2q") |
| 3 | `renderArea` (**measured, and ruled out**) | 16384 pass opens on the device: the main scene agrees on all three, so the blocker is not here | -- | closed ("M3-2m") |
| 4 | The per-draw repeated work | Pipeline lookup + two descriptor sets + a full rewrite + rebinding | Medium | after the blocker is measured |
| 5 | Texture uploads on a resident upload ring | "One-off submit plus a queue-idle wait", and it has already shown `DEVICE_LOST` | Medium | with item 3 of "next steps" |
| 6 | The hand-rolled pipeline tables are small and destroy on evict | `GPSO_TAB 192` / `CPSO_TAB 256` | Low | after 1 |
| 7 | 70.5% of the op stream is pure state binding | State is carried twice (record and replay) | High (largest surface) | M3/M4 |

### 1. Create a `VkPipelineCache` and persist it (biggest gain, lowest risk) -- **done (2026-10-15, see "M3-2p")**

**Where it landed**: `pcache_boot` / `vk_device_pcache_save` / `vk_device_pcache_tick` in `vk_device.c` (one cache shared
by graphics and compute); both `vkCreate*Pipelines` calls take `vk_dev_pcache()` instead of `VK_NULL_HANDLE`
(`vk_pipe.c:287-289`, `668-670`); the path comes from `DM_PIPE_CACHE` (the app defaults to its internal directory).
**The trap in the original test still holds**: the blob is tied to the driver and device and must be dropped after a
driver change (the header carries vendor/device/driver/api plus `pipelineCacheUUID`, and both negative tests were
exercised on the device). One device fact changed the original design: **the backgrounding route does not work under
MIUI** (SIGKILL on going to the background, part 4 of "M3-2p"), so the main path to disk is the **foreground periodic
write-back**, not the exit / background hooks.

**The original evidence (before the change)**: both pipeline creations passed `VK_NULL_HANDLE` (`vk_pipe.c:284`,
`vk_pipe.c:660`), and there was no `vkCreatePipelineCache` anywhere in the tree.

**What it costs**: no driver-level dedup inside a run, and a from-scratch compile on every run. Android compiles the
SPIR-V **at runtime** (there is no iOS-style "offline metallib" path), so this item is exactly the Android counterpart of
the iOS advantage "every shader is translated offline and compiled ahead of time, so entering a new scene compiles
nothing".

**How to measure (**still to do**)**: time to `nnMain` and the first-scene hitch, on one device, two runs
(`DM_PIPE_CACHE=0` versus warm). What is established today is only that the cache is reused (part 3 of "M3-2p"); the time
A/B waits until the usual stall has moved.

### 2. Ask for `HOST_CACHED` for the zero-copy memory type (**measured, and ruled out**, 2026-10-15, see "M3-2q")

**Original evidence**: `vk_device.c:326-327` asks for `DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT`, and `vk_memory_type`
(`vk_device.c:154-162`) **returns the first match** -> type 4. The project's own probe (M2 step 1) recorded
type 6 = `DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT|HOST_CACHED`. Had it been usable, the 3.7 GB guest heap plus the 512 MB
alias region would have left the uncached path (game logic, vertex/constant building and texture uploads all live there)
and matched Apple's cacheable `MTLResourceStorageModeShared`.

**Measurement (device)**: on type 6 exporting an fd **works**, `mmap` lands at the chosen address and both directions pass
(256 MB / 512 MB / 3968 MB all did), **but type 6 is not among the buffer's declared allowed types** (an external buffer
reports `0x13`: types 0 / 1 / 4, while a plain buffer of the same size and usage reports `0x41`, which includes type 6) --
so using it is **outside the spec**, tolerated by the driver.

**A/B (device)**: same machine, same scene, CPU time (`utime+stime`) sampled at a fixed 10-second wall clock, type 6 versus
type 4 shows **no measurable difference** (under 3% within the same mode, against a +-100% run-to-run spread; CPU time
simply tracks the number of draws). **No gain to be had, a real risk (undefined usage) to be paid => ruled out, type 4
stays.**

**What is left behind**: the overlay path **explicitly** prefers the zero-copy type with `HOST_CACHED` and **falls back to
type 4** when it is missing or the allocation fails (`vk_region.c`), controlled by `DM_ZC_CACHED=1` (**off by default**, so
the default behaviour is exactly as before); the probe gained `DM_FD_MEM_TYPE` / `DM_FD_FORCE` / `DM_FD_SIZE` plus the
plain-versus-external type-bit readout.

**How to re-check** (another device or driver):
`DM_FD_SIZE=3968 DM_FD_MEM_TYPE=6 DM_FD_FORCE=1 vk_fdmap_probe` (can it export) plus one run with `DM_ZC_CACHED=1`
comparing CPU time (is there a gain).

### 3. Stop deriving `renderArea` from the viewport (top suspect for the blocker)

**Evidence**: `nvn_vk.c:1806-1810` -- the render area is "the viewport's `x+width` / `y+height`" min'd with the scissor,
rather than the attachment's extent.

**Why it is a risk**: once the viewport's `x`/`y` is negative or non-zero (not rare in NVN), the `(uint32_t)` cast and the
min both **shrink** the render area, and **fragments outside the render area are discarded outright**; the device has no
validation layers to say so. That is one of the exact shapes of "drew, and not one pixel landed". Apple's side and the
comparable projects (Skyline / Ryujinx) take the **attachment extent** and let the viewport and the scissor do the
clamping.

**Measured and closed: not the blocker** ("M3-2m"). Across 16384 pass opens the main scene agrees on all three
(1600x900); "computed smaller than the attachment" is only **1.6%** (and that 1.6% is passes whose viewport covers a
sub-rectangle anyway, which is equivalent under Metal's semantics); collapsing to 1x1 happens **0** times; "no viewport
when the pass opened" **0** times; an offset viewport **0** times. The switch stays for regression (`DM_PASS_AREA=1`
measures only, `=2` swaps in the attachment size as a control).
**The trace turned up two more readings** (see "M3-2m" part two): (1) "attachment 0x0" means a **depth-only** pass and
the existing clamp does not apply to those; (2) pairs like "**viewport 1600x900 / attachment 800x450**" appear -- the
render area is clamped correctly but **the viewport handed to Vulkan is not scaled along with it**, an inconsistency
neither side handles yet (Apple's side does not either), recorded for later.

### 4. The per-draw repeated work

**Evidence**: every draw runs `vk_pipe_graphics` (`nvn_vk.c:2259`), allocates two descriptor sets and rewrites them whole
(`nvn_vk.c:2287-2290`), and rebinds the pipeline, the descriptor sets and the vertex streams (`nvn_vk.c:2292-2307`); the
full rewrite lands on `vkUpdateDescriptorSets` at `nvn_vk.c:1728`.

**Against the prior art**: Skyline has a dedicated `descriptor_allocator`, Ryujinx caches descriptor sets by content, and
both use **dirty flags** to skip unchanged state; here it is "recompute everything per draw" (the header of `nvn_vk.c`
says M2 aims at semantic equivalence only).

**Suggestion**: (1) remember the last pipeline + descriptor sets + vertex buffers per in-flight slot and skip identical
binds; (2) cache descriptor sets keyed by the resolved binding contents (the biggest of the three -- 70.5% of the ops are
bindings); (3) then look at whether the device has `VK_EXT_descriptor_indexing` / `VK_KHR_push_descriptor`.

**When**: **only after "does the GPU write colour content" has been measured** (the project's one-change-at-a-time rule).

### 5. Texture uploads on a resident upload ring

**Evidence**: `vk_tex.c:385-397` -- `one_end` is "submit plus `vkQueueWaitIdle`", all of it inside the queue lock.
**New evidence** ("M3-2l", section 6): one run shows `【红】上传命令等待失败 VkResult -4` (`VK_ERROR_DEVICE_LOST`) followed
by a hundred-plus failed texture uploads; across runs it is **sporadic and unrelated to the last two changes**.

**Suggestion**: (1) settle "who loses it first" (already item 3 of "next steps"); (2) have uploads share the frame's
command buffer, or build a resident upload ring (the readback path already did this -- a resident staging buffer took a
readback from about 0.45 s to negligible).

### 6. The hand-rolled pipeline tables are small and destroy on evict

**Evidence**: `GPSO_TAB 192` (`vk_pipe.c:449-451`), `CPSO_TAB 256` (`vk_pipe.c:271-274`), a whole-struct hash with linear
probing and a `memcmp` (`vk_pipe.c:480-485`), and an eviction that calls `vkDestroyPipeline` (`vk_pipe.c:667-677`).

**Suggestion**: once item 1 lands, recreating a pipeline is cheap and this drops in priority; it is still worth turning
the per-draw "hash + probe + memcmp of the whole state" into "compare with the last one", and the eviction into a
use-counted LRU.

### 7. 70.5% of the op stream is pure state binding

**Evidence**: the project's own reading (about 8 million ops per window: `BIND_STATE` 27.8% / `BIND_UBO` 21.9% /
`STENCIL` 10.3% / `BIND_TEX` 5.3% / `BIND_VBUF` 5.2%); the header of `nvn_vk.c` says M2 carries none of Metal's
optimisations (allocator rings, deferred merged passes, argument buffers, residency sets).

**Two routes**: (1) merge consecutive `BIND_*` at record time and drop the ones that are overwritten; (2) apply pure
state into the shadow **at record time** so it never enters the stream at all (only `SET_RT` / `CLEAR` / `DRAW` do). Route
2 is more thorough and departs furthest from the Apple-shaped structure.

**When**: M3/M4; the largest surface, so the gain has to be shown against the frame budget.

### Suggested order

1. Item 1 of "next steps" (the re-scan) plus item 3 here as a **measurement** -- parallel, zero risk, and it answers
   "where the blocker is" directly;
2. ~~item 1 (`VkPipelineCache` + persistence)~~ -- **done** ("M3-2p"); only the "time to `nnMain`" A/B is left;
3. item 2 (`HOST_CACHED`) -- verify "can type 6 export an fd" first, then A/B it on its own;
4. items 4 / 5 / 6 / 7 once the frame budget has been measured.
