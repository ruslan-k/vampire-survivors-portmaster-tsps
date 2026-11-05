#!/usr/bin/env python3
"""Check that every media-surface UND dynsym of an ELF64 aarch64 .so is
covered by the SF_MEDIA_TABLE in src/media.c.  Read-only."""
import re
import struct
import sys


def elf_dynsyms(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        raise SystemExit("not an ELF file")
    e_shoff, = struct.unpack_from("<Q", data, 40)
    e_shentsize, e_shnum, _, _ = struct.unpack_from("<HHHH", data, 58)
    e_shstrndx, = struct.unpack_from("<H", data, 62)
    shdrs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        name, typ, _fl, _ad, off, size, link, info, _al, entsize = \
            struct.unpack_from("<IIQQQQIIQQ", data, o)
        shdrs.append(dict(name=name, typ=typ, off=off, size=size,
                          link=link, entsize=entsize))
    shstr = shdrs[e_shstrndx]

    def secname(s):
        off = shstr["off"] + s["name"]
        return data[off:data.index(b"\0", off)].decode("ascii", "replace")

    out = []
    for s in shdrs:
        nm = secname(s)
        if s["typ"] != 11 or nm != ".dynsym":  # SHT_DYNSYM only
            continue
        strtab = shdrs[s["link"]]
        for i in range(s["size"] // s["entsize"]):
            o = s["off"] + i * s["entsize"]
            st_name, st_info, _ot, st_shndx, _v, st_size = \
                struct.unpack_from("<IBBHQQ", data, o)
            if st_shndx != 0 or st_name == 0:
                continue
            fo = strtab["off"] + st_name
            out.append((data[fo:data.index(b"\0", fo)].decode(
                "utf-8", "replace"), st_info & 0xF))
    return out


def covered_names(media_c):
    src = open(media_c, encoding="utf-8").read()
    if "SF_MEDIA_TABLE" not in src:
        return set()  # empty/absent table: everything counts as uncovered
    body = src[src.index("SF_MEDIA_TABLE"):]
    return set(re.findall(r'\{\s*"([^"]+)"', body))


def main():
    elf, media_c = sys.argv[1], sys.argv[2]
    media = [s for s in elf_dynsyms(elf)
             if s[0].startswith(("AMedia", "ANativeWindow", "AMEDIAFORMAT"))]
    cov = covered_names(media_c)
    print(f"media-surface UND: {len(media)}; table entries matched: {len(cov)}")
    missing = [(n, t) for n, t in media if n not in cov]
    for n, t in missing:
        print(f"  MISSING {n} (type={t})")
    print(f"missing: {len(missing)}")
    extra = sorted(cov - {n for n, _ in media})
    if extra:
        print(f"table-only (unused, harmless): {len(extra)}")
    sys.exit(1 if missing else 0)


if __name__ == "__main__":
    main()
