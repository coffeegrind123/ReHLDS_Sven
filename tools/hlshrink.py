#!/usr/bin/env python3
"""
Resample every texture larger than 256x256 down to within it, so a stock GoldSrc
renderer can upload them: GL_LoadTexture Sys_Error's above that, and Sven Co-op
ships well past it (1321 model textures in 496 of 1649 models, 37 sprites up to
1024x1024, and WADs like snd.wad with 151 at 512x512).

Handles .mdl and .spr; textures are 8-bit palette indices, so sampling is
nearest-neighbour -- averaging palette INDICES would produce colour noise.

  python3 tools/hlshrink.py --check  <dir>      report only
  python3 tools/hlshrink.py --out <dir> <dir>   write a converted mirror
"""
import argparse, os, struct, shutil, sys

MAX = 256

def target(w, h, align=1):
    """Downscale factor and target size.

    Three constraints, all fatal to a stock client:
      GL_LoadTexture  Sys_Error's above MAX on either side
      GL_Upload16     Sys_Error's on (width*height) & 3
      world textures  need a 4-level mip chain, so both sides align to 16

    Never grows a dimension: a 4x1 texture is already legal (4 & 3 == 0), and
    "rounding" it up to 4x2 reads a row that does not exist.
    """
    f = 1
    while w // f > MAX or h // f > MAX:
        f *= 2
    nw, nh = w // f, h // f

    if align > 1:
        aw = (nw // align) * align or nw
        ah = (nh // align) * align or nh
        return f, min(aw, w), min(ah, h)

    if not ((nw * nh) & 3):
        return f, nw, nh
    # shave at most a few pixels off an edge until the product is a multiple of 4
    for dw in range(4):
        for dh in range(4):
            cw, ch = nw - dw, nh - dh
            if cw >= 1 and ch >= 1 and not ((cw * ch) & 3):
                return f, cw, ch
    return f, nw, nh

def needs(w, h, align=1):
    _, nw, nh = target(w, h, align)
    return (nw, nh) != (w, h)

def resample(pix, w, h, f, nw, nh):
    out = bytearray(nw * nh)
    for y in range(nh):
        base = (y * f) * w
        # C-level strided slice; a per-pixel loop here costs minutes over a
        # full content tree.
        out[y * nw:(y + 1) * nw] = pix[base:base + w][::f][:nw]
    return bytes(out), nw, nh

# --- studio models ----------------------------------------------------------
def mdl_convert(path, dst, check):
    d = bytearray(open(path, 'rb').read())
    if len(d) < 244 or bytes(d[:4]) not in (b'IDST', b'IDSQ'):
        return 0
    numtex, texidx = struct.unpack_from("<ii", d, 180)
    if numtex <= 0 or texidx <= 0 or texidx + 80 * numtex > len(d):
        return 0

    entries = []
    for i in range(numtex):
        b = texidx + 80 * i
        flags, w, h, idx = struct.unpack_from("<iiii", d, b + 64)
        entries.append([b, flags, w, h, idx])
    if not any(needs(w, h) for _, _, w, h, _ in entries):
        return 0
    if check:
        return sum(1 for _, _, w, h, _ in entries if needs(w, h))

    first = min(e[4] for e in entries)
    head = bytes(d[:first])
    blob = bytearray()
    n = 0
    for e in entries:
        b, flags, w, h, idx = e
        pix = bytes(d[idx: idx + w * h])
        pal = bytes(d[idx + w * h: idx + w * h + 768])
        f, nw, nh = target(w, h)
        if (nw, nh) != (w, h):
            pix, w, h = resample(pix, w, h, f, nw, nh)
            n += 1
        e[2], e[3] = w, h
        e[4] = first + len(blob)
        blob += pix + pal

    out = bytearray(head + bytes(blob))
    for b, flags, w, h, idx in entries:
        struct.pack_into("<iiii", out, b + 64, flags, w, h, idx)
    struct.pack_into("<i", out, 72, len(out))          # studiohdr.length
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(bytes(out))
    return n

# --- sprites ----------------------------------------------------------------
def spr_convert(path, dst, check):
    d = bytearray(open(path, 'rb').read())
    if len(d) < 42 or bytes(d[:4]) != b'IDSP':
        return 0
    w, h = struct.unpack_from("<ii", d, 20)
    if not needs(w, h):
        return 0
    if check:
        return 1
    # sprite header: ...width(20) height(24) numframes(28)... palsize(40)
    nframes = struct.unpack_from("<i", d, 28)[0]
    palsize = struct.unpack_from("<h", d, 40)[0]
    p = 42 + palsize * 3
    f, tw, th = target(w, h)
    out = bytearray(d[:p])
    n = 0
    for _ in range(nframes):
        grp = struct.unpack_from("<i", d, p)[0]
        if grp != 0:
            return 0                       # grouped frames: leave the file alone
        out += d[p:p+4]; p += 4
        ox, oy, fw, fh = struct.unpack_from("<iiii", d, p)
        pix = bytes(d[p+16: p+16+fw*fh])
        _, fnw, fnh = target(fw, fh)
        npix, nw, nh = resample(pix, fw, fh, f, fnw, fnh)
        out += struct.pack("<iiii", ox // f, oy // f, nw, nh) + npix
        p += 16 + fw * fh
        n += 1
    struct.pack_into("<ii", out, 20, tw, th)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(bytes(out))
    return n

# --- WAD3 ------------------------------------------------------------------
def mip_chain(pix, w, h):
    """mip0..mip3 as GoldSrc stores them (each half the previous)."""
    mips = [pix]
    cw, ch = w, h
    for _ in range(3):
        nx, ny = cw // 2, ch // 2
        m = bytearray(nx * ny)
        for y in range(ny):
            base = (y * 2) * cw
            m[y * nx:(y + 1) * nx] = mips[-1][base:base + cw][::2][:nx]
        mips.append(bytes(m)); cw, ch = nx, ny
    return mips

def wad_convert(path, dst, check):
    d = open(path, 'rb').read()
    if d[:4] != b'WAD3':
        return 0
    count, tableofs = struct.unpack_from("<ii", d, 4)
    lumps = []
    for i in range(count):
        e = tableofs + 32 * i
        filepos, disksize, size, typ, comp, _ = struct.unpack_from("<iiiBBh", d, e)
        name = d[e + 16:e + 32]
        lumps.append([filepos, disksize, size, typ, comp, name])
    over = 0
    for filepos, disksize, size, typ, comp, name in lumps:
        if typ == 0x43 and filepos + 24 <= len(d):
            w, h = struct.unpack_from("<II", d, filepos + 16)
            if needs(w, h, 16):
                over += 1
    if not over:
        return 0
    if check:
        return over

    out = bytearray(b'WAD3' + struct.pack("<ii", count, 0))
    newtab = []
    n = 0
    for filepos, disksize, size, typ, comp, name in lumps:
        body = d[filepos:filepos + disksize]
        if typ == 0x43 and comp == 0:
            w, h = struct.unpack_from("<II", body, 16)
            if needs(w, h, 16):
                f, tnw, tnh = target(w, h, 16)
                offs_old = struct.unpack_from("<IIII", body, 24)
                off0 = offs_old[0]
                pix = body[off0:off0 + w * h]
                # The tail is [palsize:2][palette:palsize*3][pad:2] -- locate it
                # from the end of mip3, never relative to the end of the lump.
                mipend = offs_old[3] + (w // 8) * (h // 8)
                palsize = struct.unpack_from("<H", body, mipend)[0]
                palette = bytes(body[mipend + 2: mipend + 2 + palsize * 3])
                pix, nw, nh = resample(pix, w, h, f, tnw, tnh)
                mips = mip_chain(pix, nw, nh)
                hdr = bytearray(body[:16] + struct.pack("<II", nw, nh))
                o = 40
                offs = []
                for m in mips:
                    offs.append(o); o += len(m)
                hdr += struct.pack("<IIII", *offs)
                tail = struct.pack("<H", palsize) + palette + b"\x00\x00"
                body = bytes(hdr) + b"".join(mips) + tail
                n += 1
        newtab.append((len(out), len(body), len(body), typ, comp, name))
        out += body
        while len(out) % 4:
            out += b"\x00"
    tab = len(out)
    for filepos, disksize, size, typ, comp, name in newtab:
        out += struct.pack("<iiiBBh", filepos, disksize, size, typ, comp, 0) + name
    struct.pack_into("<ii", out, 4, count, tab)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(bytes(out))
    return n

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--out")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--changed-only", action="store_true",
                    help="write only the files that needed converting")
    a = ap.parse_args()
    if not a.check and not a.out:
        sys.exit("need --out or --check")
    tot = {"mdl": [0, 0], "spr": [0, 0], "wad": [0, 0]}
    for dirpath, _, files in os.walk(a.root):
        for fn in files:
            src = os.path.join(dirpath, fn)
            rel = os.path.relpath(src, a.root)
            dst = os.path.join(a.out, rel) if a.out else None
            ext = fn.lower().rsplit(".", 1)[-1]
            if ext not in ("mdl", "spr", "wad"):
                continue
            try:
                fn_ = {"mdl": mdl_convert, "spr": spr_convert, "wad": wad_convert}[ext]
                n = fn_(src, dst, a.check)
            except Exception as e:
                print("  !! %s: %s" % (rel, e)); continue
            if n:
                tot[ext][0] += 1; tot[ext][1] += n
            elif dst and not a.changed_only:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(src, dst)
    for k, (files, tex) in tot.items():
        print("%s: %d files %s, %d textures resampled" %
              (k, files, "would be converted" if a.check else "converted", tex))

main()
