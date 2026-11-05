#!/usr/bin/env python3
"""Minimal UnityFS container inspector: list nodes (file names)."""
import struct
import sys
import zlib


def read_cstr(data, off):
    end = data.index(b"\x00", off)
    return data[off:end].decode("utf-8", "replace"), end + 1


def lz4_block_decompress(src: bytes, uncompressed_size: int) -> bytes:
    """Minimal LZ4 block decompressor (no frame header)."""
    dst = bytearray()
    i = 0
    n = len(src)
    while i < n and len(dst) < uncompressed_size:
        token = src[i]
        i += 1
        lit_len = token >> 4
        if lit_len == 15:
            while True:
                b = src[i]
                i += 1
                lit_len += b
                if b != 255:
                    break
        dst.extend(src[i:i + lit_len])
        i += lit_len
        if i >= n:
            break
        offset = src[i] | (src[i + 1] << 8)
        i += 2
        match_len = token & 0x0F
        if match_len == 15:
            while True:
                b = src[i]
                i += 1
                match_len += b
                if b != 255:
                    break
        match_len += 4
        start = len(dst) - offset
        if start < 0:
            raise ValueError("lz4 offset out of range")
        for _ in range(match_len):
            dst.append(dst[start])
            start += 1
    return bytes(dst[:uncompressed_size])


def inspect(path):
    data = open(path, "rb").read()
    if data[:8] != b"UnityFS\x00":
        print("not UnityFS:", data[:8])
        return
    ver = struct.unpack(">I", data[8:12])[0]
    off = 12
    unity_ver, off = read_cstr(data, off)
    unity_rev, off = read_cstr(data, off)
    size = struct.unpack(">q", data[off:off + 8])[0]
    off += 8
    ci_c, ci_u, flags = struct.unpack(">III", data[off:off + 12])
    off += 12
    print(f"file={path}")
    print(f"  version={ver} unity={unity_ver} rev={unity_rev} size={size}")
    print(f"  blocksInfo compressed={ci_c} uncompressed={ci_u} flags={flags:#x}")

    comp = (flags & 0x3F)
    raw_info = data[off:off + ci_c]
    if comp == 0:
        info = raw_info
    elif comp in (2, 3):
        try:
            info = lz4_block_decompress(raw_info, ci_u)
        except Exception as e:
            print("  (lz4 decompress failed:", e, ")")
            return
    else:
        print(f"  (compression {comp} not handled)")
        return
    print(f"  blocks info decompressed: {len(info)} bytes")

    p = 0
    h = info[p:p + 16]
    p += 16
    nblocks = struct.unpack(">i", info[p:p + 4])[0]
    p += 4
    print(f"  hash={h.hex()} blocks={nblocks}")
    for i in range(nblocks):
        us, cs, fl = struct.unpack(">iii", info[p:p + 12])
        p += 12
        if i < 8:
            print(f"    block[{i}] unc={us} comp={cs} flags={fl:#x}")
    nnodes = struct.unpack(">i", info[p:p + 4])[0]
    p += 4
    print(f"  nodes={nnodes}")
    for i in range(nnodes):
        node_off, node_size = struct.unpack(">qq", info[p:p + 16])
        p += 16
        nflags = struct.unpack(">i", info[p:p + 4])[0]
        p += 4
        name, p = read_cstr(info, p)
        print(f"    node[{i}] off={node_off} size={node_size} flags={nflags:#x} name={name!r}")


if __name__ == "__main__":
    inspect(sys.argv[1])
