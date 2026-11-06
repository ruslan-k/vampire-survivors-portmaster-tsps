# Porting notes — Vampire Survivors on a TSPS-class handheld

Everything below was observed on real hardware (TrimUI Smart Pro, SpruceOS,
Mali-G57, 963 MB RAM, 1280x720) or in a QEMU aarch64 stand used to reproduce
crashes quickly. Numbers are measured, not estimated; where something is
inferred it is labelled.

## 1. The build under test

| | |
|---|---|
| Package | `com.poncle.vampiresurvivors` |
| Engine | Unity `6000.0.62f1 (f99f05b3e950)` |
| Scripting | IL2CPP, arm64-v8a only, `global-metadata.dat` present |
| Assets | `assets/bin/Data/**` + ~1813 Addressables bundles under `assets/aa/` |
| Game version reported by the APK | `1.16.111` (`versionCode 64959276`) |
| IL2CPP dump | `Il2CppDumper` (net7) → `dump.cs`; `CodeRegistration a760710`, `MetadataRegistration aacb208` |

There is no x86 library and no Mono path: the loader must provide the Android
runtime the IL2CPP binary expects.

## 2. Loader shape

`port/src/`:

| File | Role |
|---|---|
| `main.c` | entry, env parsing, fps/memory counters, main loop, frame pacing |
| `bionic.c` | linker64 internals: `__loader_*`, namespace/tlsdl resolution, ELF mapping |
| `nx_elf.c` | ELF load/relocate for the game's `.so` files |
| `jni.c` | JNI + the Android surface Unity touches (AssetManager, PackageManager, SharedPreferences, Play Billing, AAudio, logcat) |
| `egl.c`, `egl_sdl.c` | EGL/GLES on KMS+SDL, window/context creation, swap |
| `audio.c` | AAudio/OpenSL ES → ALSA |
| `input.c` | SDL events → Android input queue |
| `etc1.c`, `etc2_decode.c` | ETC1/ETC2 decode fallbacks |
| `mem_guard.c` | allocation guards for the 1 GB device |
| `vendor/nxinput`, `vendor/nxcompat`, `vendor/nxgl` | input/GL support shared with other NextOS ports |

The library layout must be flat: `game/lib/*.so`. A nested
`game/lib/arm64-v8a/` directory makes `libmain.so` unresolvable
(`FATAL: cannot load libmain.so`).

Asset reads go through the AssetManager stub. If `game/game.apk` exists and is a
*stored* (uncompressed) zip of `assets/bin/Data`, the shim reads from it;
otherwise it falls back to opening the directory. The zip is noticeably faster on
an SD card — thousands of small `open/read/close` calls against FAT hurt.

## 3. GLES

- Unity 6 asks for an ES 3.x context and shaders compiled for `#version 300 es`.
  Early runs died with tens of thousands of `Invalid texture unit` errors: the
  game binds more texture units than the driver advertises through
  `GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS`. Fix: report a clamped unit count and
  remap sampler bindings (`SF_GL_CLAMP_UNITS`, `SF_SAMPLER`). After the remap the
  error count went 12 608 → 0.
- `SF_NO_ESSL1=1` is the default: the ESSL1 path is never taken by this game and
  leaving it enabled only produces shader-compile churn on Mali.
- The game renders its 3D/2D content into its own render targets; the presenter
  blits to the swapchain at the panel size. On 720p the picture is letterboxed to
  1152x720 with `srgb(14,25,11)` bars — that is the game's own framing, not a
  port artefact.
- No FBO capture path is used in normal play; `SF_FBO_CAPTURE` exists for
  debugging only.

## 4. Audio

- Unity 6 uses AAudio. The very first run produced silence because the shim
  lacked `AAudioStreamBuilder_setSessionId`: Unity resolves the symbol at startup
  and falls back to a null stream when it is missing. Adding a no-op plus the
  dlsym entry fixed sound.
- Working configuration: ALSA `plughw:0,0`, 48 000 Hz, S16_LE, 2 channels,
  `XRunCount 0` in play.
- **Start the port through the frontend's launcher path.** If the desktop UI
  still owns the audio device, ALSA reports `plughw` busy, dmix gets involved and
  the game stutters audibly. The same launch path also avoids the DRM/CRTC
  conflict below.

## 5. Video / DRM

Launching the binary directly while the frontend still owns the display gives
dozens of `ERROR: Could not queue pageflip: -16` lines and often `rc=255` with an
empty log. The launcher must take over the display the way PortMaster does (the
frontend is stopped, then the port runs). With that path the errors are gone.

## 6. Input

- SDL gamepad events are translated to Android input events. The pad works with
  the stock PortMaster mapping; on firmwares that ship an empty
  `SDL_GAMECONTROLLERCONFIG` the launcher supplies the TrimUI mapping.
- The game needs no mouse: everything (including the DLC selection screen) is
  reachable with the d-pad and A/B.

## 7. Version string

The main menu shows the value returned by
`PackageManager.getPackageInfo().versionName`. The shim used to return a
hard-coded `"1.3.6"`, so the menu proudly displayed `v1.3.6 (64959276R)` while the
APK on the card was `1.16.111`. The loader now returns the real
`1.16.111` (and `versionCode 64959276`), read from the manifest stub. The
in-game `VersionChecker` compares this string against the server, so a wrong value
is not only cosmetic.

## 8. Memory: where 1.7 GB of "usage" on a 1 GB device comes from

Steady state in gameplay with all 8 DLC selected:

| Counter | Value |
|---|---|
| `VmRSS` | ~723 MB |
| `VmSwap` | ~477 MB |
| `MemAvailable` | ~70 MB |
| textures resident (shim accounting) | ~250 MB in ~783 GL texture objects |
| IL2CPP GC | used 278 MB, heap 299 MB, 3 ms slices |

Same build without the DLC selection: RSS 654 MB, swap 150 MB, textures
173 MB/461. The delta is DLC bundles plus their textures.

Consequences observed:

- Frame pacing at 30 fps holds 25–30 fps with dips to 20–21 fps while new DLC
  texture data is being paged in, and occasional ≈1 s hitches that line up with
  swap-in from the SD card.
- Unlocked, the game runs 57–60 fps in gameplay (dips 25–28 on streaming), so the
  GPU is not the limit at 30.
- With ~70 MB available, a long session can end in the OOM killer. Turning DLC off
  in the game's options is the reliable lever (≈170 MB RSS, ≈330 MB swap).

Levers that did **not** help: forcing the CPU governor, capping GPU frequency,
texture downscaling of the whole atlas set (quality destroyed for ~30 MB), and
`drop_caches` before launch. Levers that did: the sampler/unit fix (removed the
per-frame error storm), the store-zip asset path (fewer SD reads), and the 30 fps
cap (lower thermal and bandwidth pressure).

## 9. Frame pacing

`SF_FRAME_US` (µs per frame) is enforced in the main loop by sleeping the
remainder of the budget after `eglSwapBuffers`. `33333` = 30 fps, `16667` = 60,
`0` = uncapped. `SF_FPS=1` prints `[sf/fps] N fps over 120 frames` windows, which
is how the numbers in this document were taken.

## 10. Debugging recipes

- Screenshot on the device: `SF_SCREENSHOT=/mnt/SDCARD/s.ppm SF_SCREENSHOT_LIVE=1`
  env for the launcher, then `touch /tmp/bcshot` to grab the next frame
  (`/mnt/SDCARD/s.ppm.001`, 1280x720 PPM); without `SF_SCREENSHOT_LIVE` the
  capture fires once at `SF_SCREENSHOT_FRAME` (default 300).
- Frame proof without looking at the panel: `SF_GPVIRT=1 SF_GPVDUR=…` counts
  non-black frames and prints a verdict (`best_non_black=98.8%`).
- Loader log: everything goes to `<port>/log.txt`; `exec > >(tee …)` in the
  launcher keeps the file empty if the port is started without a shell, so use the
  launcher, not `setsid`.
- IL2CPP: dump with `Il2CppDumper` in `mcr.microsoft.com/dotnet/runtime:7.0`
  (the 8.0 image lacks the 7.0 framework the tool needs).

## 11. Asset recompression to ASTC 6x6

Textures dominate the GPU-side footprint. `tools/repack-astc.py` re-encodes
RGBA32 / RGB24 / ETC2_RGBA8Crunched / ASTC_4x4 textures to **ASTC RGBA 6x6 at
unchanged dimensions** (Unity `TextureFormat=56`), so sprite rects, atlases and
UI layouts stay valid — unlike a downscale. Mip chains are re-encoded level by
level with `astcenc -cl <in> <out> 6x6 -fast`.

Measured on the 448 bundles that carry such textures:

| | before | after |
|---|---|---|
| texture data (uncompressed equivalent, 653 textures) | 421 MB | 166 MB (−60%) |
| files on disk | 356 MB | 468 MB |

The disk growth is real: new blocks are inlined while the old `.resS` payload
stays in the bundle as dead weight (bundle rewriting is not done). RAM/GPU win
is the point; SD cost is ~110 MB.

Two gotchas, both already fixed in the script:

- UnityPy hands back `o.image` already flipped on Y. Encoding it as-is mirrors
  every atlas (mean abs diff vs source 72.1); flipping back gives 4.88, which is
  normal ASTC 6x6 loss.
- The stream header has to be neutralised (`m_StreamData = {path:"",offset:0,
  size:0}`) or the engine keeps reading the old `.resS` slice. Writing
  `m_StreamData = {}` crashes UnityPy with `KeyError: 'offset'`.

Verification used here: object count per bundle equal to the source, and
`m_CompleteImageSize == sum(ceil(w/6)*ceil(h/6)*16)` per mip level — 448/448
bundles clean. On device the game runs 30.0 fps with the recompressed assets and
captured frames show no compression artifacts.

The port's `tex=` counter is *format-blind* (pixels x 4 bytes), and neither
`/sys/kernel/debug/mali0/gpu_memory` nor `dma_buf/bufinfo` reflects texture
residency on this firmware, so the GPU-side saving is proven at file level only.
