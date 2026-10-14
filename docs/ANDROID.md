# Android port: feasibility analysis

English: [ANDROID.zh-CN.md](ANDROID.zh-CN.md)

This page answers one question: **can D2R Mobile be brought to Android phones?** The conclusion comes first and the reasoning follows item by item, so it can be used to confirm a plan and divide up the work. For a non-technical explanation, see [ANDROID_PLAIN.md](ANDROID_PLAIN.md).

## Conclusion

**It is technically feasible.** The architecture was already designed for "swap the platform": the top-level README says that Android phones use the same class of processor as the Switch, that the game program executes natively there as well, and that the only parts to replace are the sides of the host and the graphics layer that face the platform.

**But it is not a small job.** The work is concentrated in the graphics layer: the replay layer that currently runs on Apple Metal has to be rewritten on Vulkan.

| Part | Reusable | Notes |
|---|---|---|
| Game program (rewritten output) | Yes | The rewriter is platform independent; the output is aarch64 and executes natively on Android |
| Shader translation chain | Mostly | Its intermediate output is already Vulkan-targeted SPIR-V; skip the two Metal steps |
| Host layer (system services) | Mostly | Apple-specific code is concentrated in three `.m` files and a few local sections |
| Graphics replay layer | No | `dm/gfx/nvn/nvn_mtl.m`, about 5,800 lines of Metal, must become Vulkan |
| Front end and touch UI | No | UIKit / CAMetalLayer must become an Android UI stack |
| Resource generation scripts, third-party runtime libraries | Yes | Plain Python / plain C, reusable as is |
| Where the work goes | — | The graphics layer is roughly 60–70% |

## 1. Why it is feasible

### 1. The game program itself is aarch64, and Android is the same architecture

The Switch uses an ARM processor (Tegra X1), and Android phones are ARM too (ARMv8 / ARMv9). This is the foundation of the whole port:

- The game program **needs no emulated CPU** and runs directly on the phone; performance is not the problem;
- The rewriter `dm/tools/patch_nso.py` changes only the few instructions that cannot run unchanged under the Apple / Android kernel (thread-pointer registers, `svc` system calls, timers) and leaves the rest of the program byte for byte as shipped;
- The rewriter is an **offline, plain-Python** tool with no dependence on the runtime platform, so Android can use the very same one and produce the very same output.

### 2. The shader translation chain already produces Vulkan-targeted SPIR-V (the biggest advantage)

The game's shaders are Tegra X1 GPU machine code (the Maxwell instruction set). The existing chain is:

```
Switch original shader pack
  → tools/dmsh_xlate        generates SPIR-V with Ryujinx's shader translation library (C#)
  → spirv-cross             SPIR-V to Metal Shading Language (MSL)
  → xcrun metal / metallib  compiles into a Metal shader library
```

The key point: `dmsh_xlate`'s target language and target API are already **Vulkan** (see `TargetLanguage.Spirv, TargetApi.Vulkan` in `dm/gfx/tools/dmsh_xlate/Program.cs`). That means:

- The **front half of the chain (NVN machine code → SPIR-V) plus the reflection json** can be reused as is on Android;
- The MSL and metallib steps are **skipped**, and the SPIR-V is handed to the Vulkan driver directly (or run through SPIRV-Cross to GLSL);
- Very few shaders have to be written by hand: the 4 compute shaders embedded in the main program (currently hand-written MSL in `dm/gfx/nvn/nvn_builtin.h`) and 1 GPU deswizzle shader (`dm/gfx/tex/nvn_deswizzle_gpu.m`, which already has a CPU fallback that can stand in for the time being).

### 3. Most of the host layer is platform-independent POSIX/C

Of the 147 files under `dm/host/`, only three `.m` files are Apple-specific (the Mac front end `front_mac.m`, the Mac bootstrap `app_mac.m`, the memory accounting `memstat.m`), plus a few local sections inside `.c` files (audio, file system, crash diagnostics). A large part is directly reusable:

- Kernel system-call take-over (`hle_svc.c`, `hle_thread.c`);
- Threads and synchronization, C library pass-through (`hle_thread.c`, `hle_table.c`);
- File system, input (`hle_fs.c` body, `hle_hid.c`, `hle_touch.c`), software-keyboard protocol (`hle_swkbd.c`), text input (`dm_textinput.c`);
- Opus audio decoding (`hle_codec.c`, which uses the SDK's own libopus rather than a system framework);
- The audio **null sink** (`null_sink` in `hle_audio.c`, plain POSIX, consumes buffers in real time);
- The save-sync core logic (`dm_cloudsync.c`, which explicitly does not touch the iCloud interface and only moves files between a local and a cloud directory);
- The great majority of in-game features (`dm_*.c`: touch picking, world interaction, buying and stat allocation, stash, rift, loot, HUD and so on).

### 4. The front-end interface is cleanly abstracted (only seven symbols)

Everything platform-specific is funnelled behind one interface, `dm/host/front.h`, implemented by `front_mac.m` on the Mac and by a same-named implementation on iOS. Porting to Android means writing one `front_android.*` that provides the same symbols:

| Symbol | Purpose | Porting difficulty |
|---|---|---|
| `front_run` | Runs the guest (main-loop ownership) | Platform specific, must be rewritten |
| `front_metal_layer` | The present target (currently returns a `CAMetalLayer`) | Platform specific, and must become a Vulkan swapchain |
| `front_game_res` | Output resolution (plain arithmetic) | Easy to rewrite |
| `front_text_input` | Software-keyboard input box | Platform specific, must be rewritten |
| `front_save_failed` | Save-failure prompt | Platform specific (a Toast) |
| `dm_touch_set` | Pushes touch points | **Already cross-platform C (`hle_touch.c`), no change** |
| `dm_host_preload_start` | Early start of host loading | **Plain POSIX, no change** |

### 5. The build toolchain is already proven cross-platform

The top-level README states it plainly: **the game program produced on Windows / Linux is byte-for-byte identical to the one produced on a Mac**. That shows patching and shader translation are independent of the runtime platform. Android packaging can reuse that path directly (replacing only the iOS-specific packaging steps such as `gen_embed.py` and `ios_metal.py`).

## 2. What to reuse, adapt, or add

Three groups by location, usable as-is for task breakdown.

### A. Reuse as is (almost no change)

| Location | Content |
|---|---|
| `dm/tools/patch_nso.py` | The program rewriter, offline and plain Python |
| `dm/gfx/tools/` (front half of the chain) | `dmsh_xlate` (emits SPIR-V), `dmsh_chain*`, `shader_map.py`, `maxwell_refs.py` and so on |
| `dm/host/hle_svc.c`, `hle_thread.c`, `hle_table.c` | Kernel calls, threads and synchronization, C library pass-through |
| `dm/host/hle_touch.c`, `hle_hid.c`, `hle_swkbd.c`, `dm_textinput.c` | Touch conversion, gamepad bottom layer, software-keyboard protocol, text input |
| `dm/host/hle_codec.c` | Opus decoding (the SDK's own libopus) |
| `dm/host/dm_cloudsync.c` | Save-sync core logic |
| `dm/host/dm_*.c` (almost all) | In-game features and touch adaptation logic |
| `dm/ios/tools/*.py` (resource generation) | Plain Python plus PNG / JSON, platform-neutral output |
| `dm/third_party/libdeflate`, `bcdec` | Runtime libraries that already support ARM / Linux |

### B. Adapt (same logic, replace the Apple-specific calls)

| Location | Currently uses | Replace with |
|---|---|---|
| `dm/host/hle_audio.c` | CoreAudio `AudioUnit` (AudioToolbox) | AAudio or Oboe; the null-sink path can stay |
| `dm/host/hle_fs.c` | `clonefile()`, `getxattr/setxattr` | `copy_file_range` / `sendfile` plus a replacement table |
| `dm/host/crash.c`, `diag.c`, `memstat.m` | mach, `OSCacheControl`, signal stacks | `/proc`, `backtrace`, or weaken as needed |
| `dm/ios/Sources/dm_gamepad.m` | The GameController framework | Android `InputDevice` |
| `dm/ios/Sources/dm_touch_controls.c`, `dm_touch_game_state.c` | Plain C (only filed under iOS) | Move into the shared layer, logic unchanged |
| `dm/ios/Sources/dm_metal_cache_guard.c` | Plain C | Logic unchanged, retarget to Vulkan |

### C. Add / rewrite (Android-specific)

| Location | Content |
|---|---|
| `dm/gfx/nvn/nvn_vk.*` (new) | **The GPU replay layer**: map the Metal calls in `nvn_mtl.m` onto Vulkan class by class (the largest item) |
| `dm/host/front_android.*` (new) | The Android front end: window, present, input, text, save prompt, lifecycle |
| Android touch UI (new) | Replaces the UIKit rendering layer of `DmTouchOverlay.m` / `front_ios.m` |
| `dm/gfx/tex/nvn_deswizzle_vk.*` (new, optional) | A Vulkan version of GPU deswizzle (or use the CPU fallback for now) |
| `dm/gfx/nvn/nvn_builtin.h` (edit) | The 4 embedded compute shaders: MSL → GLSL / SPIR-V |
| Android packaging (new) | Replaces iOS-specific steps `gen_embed.py`, `ios_metal.py`, `project.yml` and so on |
| Save sync (edit) | Replaces `cloud_ios.m` (iCloud) with an Android-side implementation |

## 3. Platform differences and risks

These need dedicated on-device verification; reasoning alone is not enough:

1. **The unified-memory assumption.** Metal builds the game memory directly into zero-copy `MTLBuffer`s, assuming "GPU address = CPU address". Vulkan must map to host-visible plus host-coherent memory (or use `AHardwareBuffer`). Modern Android SoC integrated GPUs generally support this, but it must be measured device by device.
2. **TBDR / render-pass optimization.** `nvn_mtl.m` carries a lot of Metal-specific pass merging and attachment load/store optimization. Vulkan has render passes and subpasses, but the architectural differences between Mali (TBR) and Adreno (TBDR) mean this logic has to be redone, and performance tuning has to be redone too.
3. **Page size and alignment.** Metal aligns memory pools to 16 KiB pages; Android commonly mixes 4 KiB and 16 KiB, so this has to be adjusted and checked.
4. **Executable memory and W^X.** iOS does not allow executable code to be written at runtime, which is why the current design embeds the rewritten output as inline assembly into the app; Android is more permissive (it allows `mmap PROT_EXEC`), but Android 10 and later impose W^X restrictions that must be handled per version.
5. **Workspace paths.** The `~/dm-work` layout has to become an app-private directory plus `Storage Access Framework` or `MANAGE_EXTERNAL_STORAGE`.
6. **Performance targets.** The 60 fps on an A16 comes from deep Metal optimization; the Vulkan backend has to be re-tuned and cannot be assumed to deliver the same result.

## 4. Effort estimate

Roughly split for a minimal playable version ("can enter a game and fight"):

| Part | Share |
|---|---|
| GPU Vulkan replay layer (including redoing pass optimization and tuning) | about 60–70% |
| Front end + touch UI + audio + file system | about 25% |
| Shader chain work (skip MSL, take SPIR-V) | small (SPIR-V already produced) |
| Gamepad, save sync, diagnostics | about 5% |

In one sentence: **feasibility is high and the reuse rate is high, but the core is "write another graphics backend"**. This differs from the earlier move from Mac to iOS, which only replaced the front end; this time the whole GPU backend changes.

## 5. Suggested porting path

1. **Run headless first**: get `dmhost` on Android to load the rewritten output, execute `nnMain`, and watch the `UNIMPL` / `【红】` lines in the log, confirming the host base runs.
2. **A minimal Vulkan replay layer**: support only the small NVN subset the game needs to boot (clear, basic draw, one texture), with the goal of "a picture appears".
3. **Fill in render-pass optimization**: align with TBDR semantics and complete pass merging and attachment load/store.
4. **Front end + touch UI + audio**: get to "playable". The touch decision layer (`dm_touch_controls.c`) is plain C and reusable; only the rendering and input capture are rewritten.
5. **Wrap up**: gamepad, save sync, diagnostics and crash handling.

## 6. Apple API → Android mapping

| Purpose | Now (Apple) | Android counterpart |
|---|---|---|
| Graphics API | Metal | Vulkan |
| Present target | `CAMetalLayer` / `CAMetalDrawable` | `ANativeWindow` / `VkSwapchainKHR` / `AHardwareBuffer` |
| Present pacing | `CAMetalDisplayLink` | `Choreographer` / `AVSync` |
| Window and app lifecycle | UIKit / AppKit | `NativeActivity` / `Activity` |
| Audio output | CoreAudio `AudioUnit` | AAudio / Oboe |
| Gamepad | GameController | `InputDevice` (`KEYCODE_*` / `MotionEvent`) |
| Software keyboard | UIKit input box | `AlertDialog` / `InputConnection` |
| Save sync | iCloud (`NSFileManager` ubiquity) | Google Drive / a self-hosted sync |
| Chip-specific memory operations | `OSCacheControl`, mach `vm_*` | `__builtin___clear_cache`, `/proc/self/maps` |
| Crash and memory diagnostics | mach task / `vm_region` | `/proc/self/statm`, `backtrace`, `ptrace` |

## 7. Relation to the rest of the documentation

- How the whole thing works: [../dm/README.md](../dm/README.md).
- The graphics layer and the shader translation chain: [../dm/gfx/README.md](../dm/gfx/README.md).
- Building without a Mac (the existing precedent for cross-platform packaging): [NO_XCODE.md](NO_XCODE.md).
- The plain-language explanation: [ANDROID_PLAIN.md](ANDROID_PLAIN.md).
- How to start, in which stages, and what each stage verifies: [ANDROID_PLAN.md](ANDROID_PLAN.md).

The conclusions here come from a source-code review of `dm/host`, `dm/gfx`, `dm/ios` and `dm/third_party`; when it comes to implementation, the header comments of each file and [DERIVED.md](DERIVED.md) are authoritative.
