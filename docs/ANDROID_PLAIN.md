# Android port: an explanation anyone can follow

English: [ANDROID_PLAIN.zh-CN.md](ANDROID_PLAIN.zh-CN.md)

This page has no code in it. It covers three things: **what this project does, why it can move to Android, and how much work that takes.** For the technical details (which files change, where the risks are), see [ANDROID.md](ANDROID.md).

## The short answer

**Yes, it can move to Android.** And once it does, the game runs **for real** on the phone — it is not an emulator and not cloud gaming.

## 1. What this project already does

Diablo II: Resurrected originally shipped for PC and Switch only. This project does one thing: **brings it to iPhone and iPad**, completely, not as a demo.

Think of it as giving the game an interpreter:

- The game program is written in the language an ARM chip understands, and **the iPhone and the Switch both happen to carry ARM chips** — as if two people spoke the same family of languages and could already understand most of each other.
- But when the game needs something from the system (memory, reading files, drawing, sound, reading the controller), it speaks with a "Nintendo accent" that the iPhone cannot understand.
- What the project does is **rewrite the few "dialect" lines in the game, and give it a full set of services the iPhone can understand.**

One example makes it concrete: when the game wants to draw a frame, it originally shouts at the Switch's graphics system. Nobody on the iPhone hears that sentence, so the project **wrote a whole new picture-drawing pipeline for the iPhone.**

### Important: this is not an emulator

- An **emulator** is like hiring an actor to imitate everything the Switch does: slow and expensive.
- **This project** lets the game program **itself** run on your phone with an interpreter standing beside it, speaking on its behalf.

Because the chips are the same family, the game program itself needs no translation (no CPU emulation), and that is the root reason the approach is fast and battery-friendly.

## 2. Why it can move to Android

### 1. Android phones and the Switch are the same family of chips

Android phones use ARM chips too. So:

- The game program **still runs directly**, with no CPU emulation;
- The tool that "rewrites the dialect" **does not care which computer it runs on**, so Android can use the same one and produces exactly the same output.

This is the most important foundation of the whole feasibility.

### 2. The translation tool already exists, and it already translates toward Android

Every small visual effect (called a "shader") was originally written for the Switch's GPU. The project translates them into a common format.

The interesting part: **that translation tool already aims at Android**, and only adds two more steps afterwards to turn the result into something Apple GPUs can use. On Android, you simply **get off at the common-format stop** and skip those two steps. This is the easiest part of the whole job.

### 3. Most of the parts can move as they are

The parts of the project that talk to the system (reading files, handling controllers, the software keyboard, audio decoding, save sync, and all the in-game features such as the Rift and the stash) are **mostly generic and not tied to the iPhone**. Those move over untouched.

### 4. The platform-specific work is gathered behind a few "interfaces"

The design is careful: everything tied to "this particular device" is piled behind **a list of only seven items** (like the plugs on an appliance). Porting means reimplementing those seven.

The good news: two of them — touch input and the loading sequence — **are already generic and need no change at all.**

## 3. What has to be redone

### The hardest part: drawing

- iPhone uses a drawing toolkit called **Metal**, Android uses one called **Vulkan**;
- They think differently, so **the drawing code has to be rewritten wholesale**;
- This is about **60 to 70 percent** of the total work and the main event of the whole port.

(You can picture it this way: the sockets are a different shape, but a plug adapter is not enough — the wiring has to be redone and re-tuned.)

### The rest to replace

| What | From (iPhone) | To (Android) |
|---|---|---|
| UI and touch appearance | Apple's UI toolkit | Android's UI toolkit |
| Audio playback | Apple's sound system | Android's sound system |
| Gamepad | Apple's controller support | Android's controller support |
| Cloud save sync | iCloud | An Android-side cloud (for example Google Drive) |
| Crash and diagnostics | Apple's debugging tools | Android's debugging tools |

**Note**: the game's own gameplay logic (tapping the ground to walk, buying and selling, the Rift, the stash and so on) **does not need rewriting** — it sits in the generic layer and moves as it is.

## 4. How much work is it

Put plainly:

| Work | It is like |
|---|---|
| Rewriting the game program | Already done, used as it is |
| Shader translation | Getting off at a different stop, a small change |
| System-facing code | Mostly used as it is |
| The drawing backend | **Redoing the whole wiring** (the biggest) |
| UI, audio, gamepad | Swapping a few "sockets" (medium) |

In one sentence: **feasible, with a lot of reuse, but the core is "writing another drawing engine".**

The earlier move from Mac to iOS was lighter because both are Apple systems and the drawing layer stayed; **this time the entire drawing backend changes, which is a different order of work.**

## 5. What you get when it is done

- The game runs **natively** on Android phones (not cloud, not emulation);
- The same touch controls, controller support, the Rift, the Chronicle stash and the quality-of-life features;
- English and Chinese, fully offline;
- **You still need to supply your own Switch copy of the game files** — that does not change, and the project ships no game content.

## 6. Why it is worth doing

- **No technical blockers**: same chip family, a translation tool that already exists, and mostly generic code;
- **The author wants it too**: the project's own documentation says "the route is fully open" and the author would love to see someone bring it to Android;
- Android has far more devices and users, making it the most natural next step for this approach.

## FAQ

**Is this an emulator?**
No. The game program runs directly on the phone with no CPU emulation. The usual emulator cost of being slow and power-hungry never applies here.

**Do I need a very powerful phone?**
For reference, the lowest iPhone the project supports is an iPhone 14 Pro Max (6 GB). A comparable Android phone should be enough in theory, but **performance has to be re-tuned**; the Apple results cannot be assumed to carry over as they are.

**Does it use my game files?**
You need to **own and export** the Switch game files yourself (expansion included). The project ships and distributes no game content.

**Can it go online?**
No. As now, it is fully offline and needs no online account.

**So when can I use it?**
This document is a feasibility confirmation, not a ship date. The real work starts with the drawing layer and proceeds step by step. For the concrete steps, see "Suggested porting path" in [ANDROID.md](ANDROID.md).
