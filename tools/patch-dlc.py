#!/usr/bin/env python3
"""Patch the paid-DLC gate in the player's own libil2cpp.so.

Build-specific: v1.16.111 (64959276R), Unity 6000.0.62f1, IL2CPP, arm64.
See docs/DLC.md. Only for DLC you own.

    python3 tools/patch-dlc.py game/lib/libil2cpp.so [--out FILE] [--force]
"""
import argparse
import hashlib
import struct
import sys

STOCK_SHA256 = "5c549da080edd2c4217ee28c463ac68f5dc144fc8e4a63dfe2b45193a73c1f15"
PATCHED_SHA256_PREFIX = "88fb2b4f175bc815865c77017319e703"
SIZE = 189_643_920

BRANCH_SITE = 0x9EF6418       # sub sp, sp, #0xa0
BRANCH_TARGET = 0x9EAA144
RET_SITE = 0x9EF7300          # sub sp, sp, #0x60
ORIG_BRANCH = 0xD10283FF
ORIG_RET_SITE = 0xD10183FF

B = 0x14000000
MOV_W0_1 = 0x52800020
RET = 0xD65F03C0


def arm64_b(pc, target):
    delta = target - pc
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise SystemExit("branch out of range: %#x -> %#x" % (pc, target))
    return B | ((delta >> 2) & 0x03FFFFFF)


def word(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lib")
    ap.add_argument("--out")
    ap.add_argument("--force", action="store_true",
                    help="patch even if the input hash is unknown (offsets still verified)")
    args = ap.parse_args()

    data = bytearray(open(args.lib, "rb").read())
    digest = hashlib.sha256(data).hexdigest()
    print("input : %s (%d bytes)" % (args.lib, len(data)))
    print("sha256: %s" % digest)

    if len(data) != SIZE:
        print("size differs from the known build (%d != %d)" % (len(data), SIZE))
        if not args.force:
            raise SystemExit("refusing: unknown build (pass --force to override)")
    if digest == STOCK_SHA256:
        print("stock build detected")
    elif digest.startswith(PATCHED_SHA256_PREFIX):
        raise SystemExit("already patched")
    elif not args.force:
        raise SystemExit("refusing: sha256 does not match the known stock build "
                         "(pass --force to override)")

    for off, want, name in ((BRANCH_SITE, ORIG_BRANCH, "branch site"),
                            (RET_SITE, ORIG_RET_SITE, "return site")):
        got = word(data, off)
        if got != want:
            raise SystemExit("%s @%#x: expected %#010x, found %#010x — wrong build?"
                             % (name, off, want, got))

    branch = arm64_b(BRANCH_SITE, BRANCH_TARGET)
    struct.pack_into("<I", data, BRANCH_SITE, branch)
    struct.pack_into("<I", data, RET_SITE, MOV_W0_1)
    struct.pack_into("<I", data, RET_SITE + 4, RET)

    print("patched @%#x: %#010x -> %#010x (b %#x)" % (BRANCH_SITE, ORIG_BRANCH, branch, BRANCH_TARGET))
    print("patched @%#x: %#010x -> %#010x %#010x (mov w0,#1 ; ret)"
          % (RET_SITE, ORIG_RET_SITE, MOV_W0_1, RET))

    out = args.out or (args.lib + ".patched")
    with open(out, "wb") as fh:
        fh.write(data)
    print("wrote : %s" % out)
    print("sha256: %s" % hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    sys.exit(main())
