# Memory: where it goes, and the A/B plan to get it back

Baseline measured on the TrimUI Smart Pro (963 MB RAM, no zram, SD swap), gameplay,
all 8 DLC enabled, 30 fps cap, this build:

| Counter | Value |
|---|---|
| `VmRSS` | 723–730 MB |
| `VmSwap` | 477 MB (376 MB in a fresh run before the DLC textures settle) |
| `MemAvailable` | 70–85 MB |
| textures (shim accounting) | 226–250 MB in ~727–783 GL texture objects |
| IL2CPP GC | used 278 MB / heap 299 MB, 3 ms slices |

Same build without DLC selected: RSS 654 MB, swap 150 MB, textures 173 MB/461.

So the DLC selection costs ~70–170 MB RSS and ~230–330 MB swap, and it is the only
reason this build is close to the OOM line. The game is GPU-idle at 30 fps (57–60
uncapped), so nothing here is a rendering problem: it is resident data.

## Ranked levers (candidates, not measured yet)

Ordered by expected saving per unit of risk. Each one is a single-variable A/B:
same save, same stage, 90 s warm-up, compare `VmRSS`, `VmSwap`, `MemAvailable`,
tracked texture bytes and `[sf/fps]` windows.

1. **Don't preload DLC assets** (est. 60–150 MB). The measured texture delta between
   "no DLC" and "all DLC" is 77 MB, so that is the realistic upper bound. Mechanism:
   defer Addressables/bundle loads for DLC content until a stage actually needs it —
   hook `AssetBundle.LoadFromFileAsync` / `Addressables.LoadAssetAsync` in the shim
   and gate the DLC bundles. Risk: stage-select thumbnails or a first-stage hitch.
2. **Drop CPU-side texture copies** (est. 20–100 MB). Force
   `Texture2D.Apply(updateMipmaps, makeNoLongerReadable=true)` for textures that are
   only ever sampled, via the JNI/IL2CPP bridge. Risk: any texture the game reads
   back (screenshots, colour pickers) breaks — allowlist first.
3. **Lower GPU texture residency** (est. 0–100 MB). `QualitySettings.globalTextureMipmapLimit = 1`
   drops the top mip for textures that have mips. First classify the 783 texture
   objects: many 2D atlases ship without mips, and for those the only real win is
   downsampling the atlas itself (which costs picture quality).
4. **Release unused assets at menu transitions** (est. 20–100 MB).
   `Addressables.Release(handle)` for finished loads + `Resources.UnloadUnusedAssets()`
   + `il2cpp_gc_collect()` right after returning to the main menu. Risk: a released
   handle that the menu still pulls from.
5. **Shrink Unity's native allocator blocks** (est. 0–30 MB). `boot.config` overrides
   `memorysetup-main-allocator-block-size=8388608`,
   `memorysetup-thread-allocator-block-size=8388608`,
   `memorysetup-gfx-main-allocator-block-size=8388608` (16 MiB → 8 MiB). Cheap to
   test, may only add fragmentation.

Not worth it: forcing the CPU governor, capping the GPU clock, `drop_caches` before
launch, downscaling all textures (quality loss for ~30 MB).

## Device-side policy that needs no code

- Swap on SD makes every texture page-in visible as a frame dip. zram (if the
  firmware can provide it) would turn those SD reads into RAM-compressed reads, but
  it also takes RAM away from a 963 MB box; on this device the DLC-off switch is
  the better lever.
- `min_free_kbytes` is 3967 on this firmware, so kswapd sleeps while the game holds
  most of RAM; the OOM line is reached before any proactive reclaim happens.
