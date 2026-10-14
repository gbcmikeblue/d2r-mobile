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

## 7. Next step

Proceed in the order on this page; after each milestone, tick it off here and record the conclusion. **M2 is at step 14
now**: descriptor writing and real draws ✓ (step 10), textures and samplers resolved by NVN slot ✓ (step 11), views and
**the render targets' views** ✓ (steps 12 and 14), and **the present-screenshot instrument is in place and has been used
to reach a conclusion** ✓ (steps 13 and 14). **M2's acceptance (a picture appears) is now blocked by the rendering chain
itself** -- the GPU has written no colour content. What follows is ordered by value:

1. **Fix where the vertex data lands** -- the warning `vertex attribute N (... at position N) has a component type that
   disagrees with the shader: format NxN vs shader float` says the vertex stream and the format may be paired up wrongly;
   geometry collapsing entirely is the most likely reason for "draws issued, nothing lands" (pick the stream from the type
   the shader declares first, then check the stride and offset);
2. **Fix the descriptors falling back to blanks** -- `combined image sampler: writing a blank texture` (the handle source
   that `res.json` calls `cbuf_slot=-N`) and `uniform buffer: no buffer address`; both leave the shader reading an empty
   texture or empty constants;
3. **Scan again once those are fixed** -- `DM_NVN_SHOT_SCAN` answers directly with "has content or not", so there is no
   more guessing (it is already the instrument for the "you look, I change" loop: pictures and the report land in the same
   directory);
4. Docs and cleanup to wrap up (including the interface-description-table code that is no longer called).

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
11. Use it to ask "where is the picture" ✓ (step 14 here: **no colour content** -> the blocker becomes the rendering chain: where vertex data lands, and descriptors falling back to blanks).

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
