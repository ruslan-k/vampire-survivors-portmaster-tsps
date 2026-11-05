# DLC: the purchasing gate and the two-instruction patch

The port runs the game's own IL2CPP binary, so the Play Store purchase check is
compiled in. Faking the Play Billing Java surface is not enough: the game asks
`PurchasingController.GetOwnedDlcTypes()` and, when the answer is "nothing
owned", never mounts the DLC bundles.

`tools/patch-dlc.py` changes two instructions in **your own** `libil2cpp.so` so
that the gate reports all DLC types as owned. Nothing else is touched and no game
code is redistributed.

## What the gate looks like

Relevant types (from `Il2CppDumper` output):

```
class PurchasingController {
    List<DlcType> GetOwnedDlcTypes();
    bool          DoesUserOwnAnyPaidDlc();
}
class DlcSystem { ... LoadDlc(...) ... }
```

Callers of `GetOwnedDlcTypes()` are what decide whether a DLC bundle is mounted;
`DoesUserOwnAnyPaidDlc()` short-circuits the "no paid DLC" UI path.

## The patch

Build: `v1.16.111 (64959276R)`, Unity `6000.0.62f1`, IL2CPP, arm64.

| File offset | Original (hex) | Original instruction | Patched (hex) | Patched instruction |
|---|---|---|---|---|
| `0x9EF6418` | `ff 83 02 d1` | `sub sp, sp, #0xa0` | `4b cf fe 17` | `b 0x9EAA144` → `SortedDlcTypes` path |
| `0x9EF7300` | `ff 83 01 d1` | `sub sp, sp, #0x60` | `20 00 80 52` + `c0 03 5f d6` | `mov w0, #1` ; `ret` |

The first site skips the ownership filtering and jumps into the path that builds
the DLC list from the (existing) sorted type table; the second makes
`DoesUserOwnAnyPaidDlc()` return `true` unconditionally.

Both original words are function prologues, which is why the patch is safe to
apply blind: if the bytes at those offsets are not the ones above, the script
refuses to write.

## Hashes

| File | SHA-256 |
|---|---|
| stock `libil2cpp.so` (this build) | `5c549da080edd2c4217ee28c463ac68f5dc144fc8e4a63dfe2b45193a73c1f15` |
| patched | `88fb2b4f175bc815865c77017319e703…` (see `tools/patch-dlc.py --print-hash` after patching) |
| size, both | 189 643 920 bytes |

## Usage

```
# from game/lib/ on the handheld, with python3 available
python3 tools/patch-dlc.py game/lib/libil2cpp.so
# -> writes game/lib/libil2cpp.so.patched, verifies the result
mv game/lib/libil2cpp.so game/lib/libil2cpp.so.stock
mv game/lib/libil2cpp.so.patched game/lib/libil2cpp.so
```

Then select the DLC in the game's **Options → DLC** screen (the selection is
stored in `game/home/shared-preferences.bin` as `selecteddlc=…` and survives
restarts).

## Effect on memory

Measured on a 1 GB TrimUI Smart Pro, same run otherwise:

| | stock (no DLC) | all 8 DLC |
|---|---|---|
| VmRSS | 654 MB | 723 MB |
| VmSwap | 150 MB | 477 MB |
| textures resident | 173 MB / 461 objects | 250 MB / 783 objects |

The gate patch itself costs nothing; the DLC content does. On a 1 GB device,
consider leaving the largest DLC (the big bundle pack) unselected.

## Notes

- The patch is **build-specific**. A new game version needs new offsets: dump
  with `Il2CppDumper` (`CodeRegistration`, `MetadataRegistration` come from the
  `libil2cpp.so` `Il2CppCodeRegistration` symbols), find
  `PurchasingController::GetOwnedDlcTypes` and `DoesUserOwnAnyPaidDlc` in
  `dump.cs`, then read the offsets out of the binary.
- The stock APK used here is an Android arm64 build. Another build (different
  RVA layout, different metadata version) will not match — the script says so
  instead of corrupting the file.
- Only for DLC you own. This document exists so the mechanism is auditable, not
  as a way to obtain content you did not buy.
