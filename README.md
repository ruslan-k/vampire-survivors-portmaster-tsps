# Vampire Survivors for PortMaster (TSPS / SpruceOS)

Play **Vampire Survivors** on a Linux handheld with
[PortMaster](https://portmaster.games/) — no Android, no emulator, no container.

- **BYO-APK.** This repository ships **no game files**: no APK, no `assets/`, no
  `libil2cpp.so`, no bundles. You bring your own arm64 APK of the game and the
  port turns it into a handheld build on first setup (`tools/setup-from-apk.sh`).
- **How it runs:** the game is Unity 6, IL2CPP, arm64 only. The port is a *host
  loader*: a small C shim re-implements the bionic linker entry point, the JNI
  surface and the handful of Android APIs Unity touches, then loads the game's
  own `libmain.so` / `libunity.so` / `libil2cpp.so` and drives real EGL/GLES on
  the handheld's GPU.
- **Tested on:** TrimUI Smart Pro (Allwinner A133P, Mali-G57, 1 GB RAM,
  1280x720) with SpruceOS. Other GLES3 handhelds with a 64-bit userland are
  likely to work; they are untested.
- **State:** playable, 30 fps locked, audio working, all 8 DLC selectable,
  saves in `game/home/`.

## Contents

- [What you need](#what-you-need)
- [Install](#install)
- [First setup (your APK → game data)](#first-setup-your-apk--game-data)
- [Controls](#controls)
- [Performance and memory](#performance-and-memory)
- [Options](#options)
- [DLC](#dlc)
- [Known issues](#known-issues)
- [Build from source](#build-from-source)
- [For developers](#for-developers)
- [Licence and credits](#licence-and-credits)

## What you need

| | |
|---|---|
| **A handheld** | One that runs PortMaster, has a 64-bit ARM userland, OpenGL ES 3.2 and a working `libSDL2`. Tested: TrimUI Smart Pro / SpruceOS (Mali-G57, 1 GB RAM, 720p). |
| **Your own copy of the game** | An **arm64** APK of Vampire Survivors for Android. The loader was written against build `v1.16.111 (64959276R)`, Unity `6000.0.62f1`, IL2CPP, `global-metadata.dat` v31. Other arm64 builds may work but the offsets in `tools/patch-dlc.py` are build-specific. |
| **Disk** | ~1.9 GB for the extracted game data (1.2 GB of Addressables bundles) plus ~100 MB for the store-zip `game.apk`. A fast SD card matters: texture streaming reads from it during play. |

Nothing from the game is redistributed here. Do not ask for APKs or bundles.

## Install

1. Copy `Vampire Survivors.sh` to your `PORTS` folder and the
   `vampiresurvivors/` folder next to it (PortMaster layout):

   ```
   Roms/PORTS/Vampire Survivors.sh
   Roms/PORTS/vampiresurvivors/vampiresurvivors-nextos
   Roms/PORTS/vampiresurvivors/game/            <- you fill this
   ```

2. Run the first-time setup from a shell on the handheld (or over SSH):

   ```
   sh /roms/ports/vampiresurvivors/tools/setup-from-apk.sh /path/to/your.apk
   ```

3. Start **Vampire Survivors** from the Ports menu.

## First setup (your APK → game data)

`tools/setup-from-apk.sh` does exactly three things, and refuses to touch
anything else:

1. extracts `assets/bin/Data/**` and `assets/aa/**` from your APK into `game/`;
2. copies `lib/arm64-v8a/*.so` into `game/lib/` (flat — the loader expects that
   layout);
3. packs `game/assets/bin/Data` into a **store-only** zip `game/game.apk`, which
   the shim opens as a zip instead of doing thousands of small file reads.

The script fails loudly if the APK has no `arm64-v8a` libraries or no
`assets/bin/Data`. It never downloads anything.

## Controls

| Input | Action |
|---|---|
| D-pad / left stick | move / menu navigation |
| A | confirm, attack |
| B | back |
| Start | pause |
| Select | map / options |

The pad is discovered through SDL; on firmwares that ship an empty
`SDL_GAMECONTROLLERCONFIG` the launcher supplies a mapping for the TrimUI pad.
The game itself has no touch-only screens, so a mouse is not needed.

## Performance and memory

Measured on the TrimUI Smart Pro (963 MB RAM, no zram, swap on SD), in
gameplay with all 8 DLC enabled:

| Metric | Value |
|---|---|
| FPS, locked at 30 | 25–30 sustained, dips to 20–25 while streaming new DLC textures |
| Unlocked FPS (no cap) | 57–60 in gameplay, dips to 25–28 on bundle streaming |
| VmRSS | ~723 MB |
| VmSwap | ~477 MB |
| MemAvailable | ~70 MB |
| Textures resident (shim counter) | ~250 MB in ~780 GL texture objects |

Without the DLC selection the same build sits at RSS ~654 MB, swap ~150 MB,
textures ~173 MB — the difference is DLC bundles and their textures.

The launcher caps the game at 30 fps (`SF_FRAME_US=33333`). On a 1 GB device the
cap is not about the GPU: it keeps the SoC cool and leaves memory bandwidth for
SD-card texture streaming, which is where the frame dips come from.

## Options

All are environment variables read by the loader; either export them in the
launcher or prefix the binary.

| Variable | Meaning |
|---|---|
| `SF_FRAME_US` | Frame budget in microseconds (`33333` = 30 fps, `16667` = 60, `0` = uncapped) |
| `SF_FPS` | Print `[sf/fps]` lines to the log |
| `SF_NO_FPS` | Silence the FPS lines |
| `SF_VERBOSE` | Verbose loader log (JNI calls, DLC/licence checks) |
| `SF_JNILOG` | Per-call JNI trace |
| `SF_LOGCAT` | Route the game's Android logcat output into the log |
| `SF_TEX_HALF_MIN` | Halve textures smaller than N bytes (memory lever, quality loss) |
| `SF_ETC1` / `SF_ETC1_MIN` | ETC1 fallback for uncompressed uploads |
| `SF_GL_CLAMP_UNITS` | Clamp texture units (old Mali drivers) |
| `SF_SAMPLER` / `SF_NO_SAMPLER` | Texture/sampler binding remap |
| `SF_GC_*` | IL2CPP GC slice and heap knobs |

Everything that changes the picture is off by default.

## DLC

The game gates paid DLC on a Play Store purchase check
(`PurchasingController.GetOwnedDlcTypes`). The loader fakes the Play Billing
surface, but the gate itself is compiled into `libil2cpp.so`, so a stock APK
reports "no paid DLC".

`tools/patch-dlc.py` patches your own `libil2cpp.so` (two instructions) so the
gate reports every DLC as owned. It verifies the SHA-256 of the input first and
refuses to patch an unknown build. Details, offsets and the exact instruction
words are in [docs/DLC.md](docs/DLC.md).

Only use this with DLC you actually own. The repository ships no game code.

## Known issues

- **First launch after a cold boot is slow** (30–90 s): the loader maps ~1.9 GB
  of game data from the SD card.
- **Frame dips to ~20 fps** during DLC texture streaming; caused by swap-in, not
  by the renderer.
- **Memory headroom is thin** (≈70 MB available). A long session with all DLC on
  a 1 GB device can be OOM-killed; disabling DLC in Options frees ~170 MB RSS.
- **`v1.3.6` used to be shown** in the corner of the main menu — that was a
  hard-coded string in the loader's `PackageInfo` stub, fixed to report the real
  APK version (`1.16.111`). If you see an old version, rebuild the loader.

## Build from source

```
cd port
bash build.sh            # docker/podman, ghcr.io/monkeyx-net/...:aarch64-latest
# → port/vampiresurvivors-nextos
```

The build cross-compiles the shim for `aarch64-linux-gnu` with glibc ≤ 2.29 and
links against stubbed Android/SDL symbols that are resolved at runtime.

## For developers

- `port/src/` — the loader: `main.c` (entry), `bionic.c` (linker internals),
  `jni.c` (Android API surface), `egl.c` / `egl_sdl.c` (GLES + window),
  `audio.c` (AAudio/OpenSL → ALSA), `input.c`, `etc1.c` / `etc2_decode.c`
  (texture decode), `nx_elf.c` (module loading), `mem_guard.c`.
- `port/vendor/` — nxinput / nxcompat / nxgl support code shared with other
  NextOS ports.
- `docs/PORTING-NOTES.md` — what breaks and why (bionic, IL2CPP, GLES, audio,
  input, memory), measured on real hardware.
- `docs/DLC.md` — the purchasing gate and the two-instruction patch.

## Licence and credits

GPL-3.0-only. The loader's structural base is the `sallyface-nextos` port
(`ports/sallyface-nextos/upstream`) and the NextOS port framework; see the file
headers for SPDX tags.

Vampire Survivors is © poncle. This project ships no game code and is not
affiliated with poncle.
