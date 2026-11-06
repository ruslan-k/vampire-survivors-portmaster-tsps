#!/usr/bin/env python3
"""Recompress Unity texture bundles to ASTC RGBA 6x6 at unchanged dimensions.

Same pixel geometry means Sprite rects, atlases and UI stay valid; only the
stored block format changes, so the GPU footprint drops ~2.5x (see numbers in
docs/PORTING-NOTES.md). Mip chains are re-encoded level by level.

Requires: pip install UnityPy pillow, and the ARM astc encoder CLI
(https://github.com/ARM-software/astc-encoder/releases, set ASTCENC).

Usage:
  repack-astc.py --src GAME/assets/aa/Android --out /tmp/astc [--limit 0]
  # copy only the changed files over the device, then clear the port's
  # texture cache if one exists.
"""
import argparse
import os
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor

import UnityPy
from PIL import ImageOps

ASTC_RGBA_6x6 = 56                       # Unity TextureFormat id
RECOMPRESS = (4, 3, 65, 48)              # RGBA32, RGB24, ETC2_RGBA8Crunched, ASTC_RGB_4x4
MIN_PIXELS = 64 * 64                     # below this the block overhead wins


def astc_blocks(img, astcenc, tmpdir):
    """Encode one image to raw ASTC 6x6 blocks (astcenc header stripped)."""
    png = os.path.join(tmpdir, "in.png")
    astc = png + ".astc"
    img.save(png)
    r = subprocess.run([astcenc, "-cl", png, astc, "6x6", "-fast", "-silent"],
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr[:200] or "astcenc failed")
    with open(astc, "rb") as f:
        return f.read()[16:]


def encode(img, mips, astcenc, tmpdir):
    levels = [img]
    w, h = img.size
    for _ in range(mips - 1):
        w, h = max(1, w // 2), max(1, h // 2)
        levels.append(img.resize((w, h), 3))
    # Unity stores rows bottom-up and o.image already flipped them for viewing.
    return b"".join(astc_blocks(ImageOps.flip(i), astcenc, tmpdir) for i in levels)


def repack(path, outdir, astcenc):
    env = UnityPy.load(path)
    done = saved = 0
    with tempfile.TemporaryDirectory() as tmpdir:
        for obj in env.objects:
            if obj.type.name != "Texture2D":
                continue
            tex = obj.read()
            if int(tex.m_TextureFormat) not in RECOMPRESS:
                continue
            if tex.m_Width * tex.m_Height < MIN_PIXELS:
                continue
            try:
                img = tex.image.convert("RGBA")
                data = encode(img, max(1, int(tex.m_MipCount)), astcenc, tmpdir)
            except Exception as exc:                     # keep the original block
                print(f"  skip {tex.m_Name}: {exc}")
                continue
            if len(data) >= int(tex.m_CompleteImageSize):
                continue                    # small/already-compressed: ASTC would grow it
            saved += int(tex.m_CompleteImageSize) - len(data)
            tex.m_TextureFormat = ASTC_RGBA_6x6
            tex.m_CompleteImageSize = len(data)
            tex.image_data = data
            tex.m_StreamData = {"path": "", "offset": 0, "size": 0}
            tex.save()
            done += 1
    if not done:
        return 0, 0
    out = os.path.join(outdir, os.path.basename(path))
    with open(out, "wb") as f:
        f.write(env.file.save())
    return done, saved


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="directory with *.bundle")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--astcenc", default=os.environ.get("ASTCENC", "astcenc"))
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    bundles = sorted(b for b in os.listdir(args.src) if b.endswith(".bundle"))
    total_tex = total_saved = changed = 0

    def work(name):
        return repack(os.path.join(args.src, name), args.out, args.astcenc)

    with ThreadPoolExecutor(args.jobs) as pool:
        for name, (tex, saved) in zip(bundles, pool.map(work, bundles)):
            if tex:
                changed += 1
                total_tex += tex
                total_saved += saved
                print(f"{name}: {tex} tex, {saved/1e6:.1f} MB raw saved")

    print(f"done: {changed}/{len(bundles)} bundles, {total_tex} textures, "
          f"{total_saved/1e6:.1f} MB texture data saved")


if __name__ == "__main__":
    main()
