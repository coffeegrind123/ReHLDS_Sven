#!/usr/bin/env python3
"""Does a retail Half-Life client have room to render this map?

Two ceilings kill a stock client on a Sven Co-op map, both in the renderer and
both before a single frame is drawn:

  Bad surface extents   a face wider than 512 units on either lightmap axis.
                        Mod_LoadFaces calls Sys_Error.  Note 512, not the 256
                        of the original engine: the 25th anniversary hw.dll
                        raised it (`cmp ecx,0x200` at RVA 0x23E4AB, guarding
                        the error at 0x23E4D3), and TEX_SPECIAL faces are
                        exempt from the check entirely.

  AllocBlock: full      the lightmap atlas is 64 blocks of 128x128 luxels, and
                        it is a hard array.  GL_BuildLightmaps calls Sys_Error.

Both numbers were read out of hw.dll (Half-Life 25th anniversary) rather than
taken from a header: the allocator lives at RVA 0x247030, walks `allocated[]`
at 0x10dbb640 in strides of 128 (BLOCK_WIDTH), and errors when the base index
reaches 0x2000 -- 8192/128 = 64 blocks.  The packing below is that function,
instruction for instruction, so the block count it reports is the count the
client will reach, not an estimate.

Usage:
    hlmapcheck.py <map.bsp> [map.bsp ...]
    hlmapcheck.py --dir <maps directory>
"""

import argparse
import math
import os
import struct
import sys

LUMP_TEXTURES = 2
LUMP_VERTEXES = 3
LUMP_TEXINFO = 6
LUMP_FACES = 7
LUMP_EDGES = 12
LUMP_SURFEDGES = 13
LUMP_MODELS = 14

# hw.dll GL_BuildLightmaps / AllocBlock.
BLOCK_WIDTH = 128
BLOCK_HEIGHT = 128
MAX_LIGHTMAPS = 64

# SV_SpawnServer builds the inline-submodel names "*0".."*511" into a 512-entry
# table of 5 bytes each (RVA 0x20B803, base 0x1198480, end 0x1198E80) and then
# indexes it by the world's numsubmodels without a bound (RVA 0x20EE70), so a
# map with 512 or more models walks off the end into zeroed memory and dies with
# "Mod_FindName: NULL name".  Measured: hl_t00.bsp has 541 and does exactly that.
#
# This is a LISTEN-SERVER ceiling only -- it is the retail engine's SERVER that
# overflows.  A client joining a dedicated server never runs that loop, and
# Mod_LoadBrushModel names its submodels through a local buffer instead.  So it
# is reported as a note, not as "cannot render".
MAX_SUBMODELS = 512

# Mod_LoadFaces, hw.dll RVA 0x23E4AB.  Read from the binary, not a header --
# the original GoldSrc limit was 256 and this client's is 512.
MAX_SURFACE_EXTENTS = 512

TEX_SPECIAL = 1


class Bsp:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()

        self.path = path
        (self.version,) = struct.unpack_from("<i", self.data, 0)
        if self.version != 30:
            raise ValueError("not a GoldSrc BSP (version %d)" % self.version)

        self.lumps = []
        for i in range(15):
            ofs, length = struct.unpack_from("<ii", self.data, 4 + i * 8)
            self.lumps.append((ofs, length))

    def lump(self, n):
        ofs, length = self.lumps[n]
        return self.data[ofs:ofs + length]

    def texture_names(self):
        raw = self.lump(LUMP_TEXTURES)
        if len(raw) < 4:
            return []

        (count,) = struct.unpack_from("<i", raw, 0)
        names = []
        for i in range(count):
            (ofs,) = struct.unpack_from("<i", raw, 4 + i * 4)
            if ofs < 0 or ofs + 16 > len(raw):
                names.append("")
                continue
            names.append(raw[ofs:ofs + 16].split(b"\0")[0].decode("latin-1"))
        return names


def _f32(x):
    """Round a double to float32, the way the engine's cvtpd2ps does."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def face_extents(bsp, verts, edges, surfedges, texinfos, firstedge, numedges, tex):
    """CalcSurfaceExtents (RVA 0x23E2A0), as the engine computes it.

    The one detail that matters, and it is worth 1207 of abandoned.bsp's 20453
    surfaces: the engine accumulates the dot product in double but ROUNDS IT TO
    FLOAT before comparing (cvtpd2ps at 0x23E393).  Keeping full double
    precision here makes a face whose extent lands exactly on a 16-unit
    boundary -- which, in a grid-built map, is most of them -- come out one
    luxel wider, and the atlas one block fuller than the client ever sees.

    999999.0 / -99999.0 are the engine's own initialisers (0x102C1A44,
    0x102C5DD8), and the /16 is a float multiply by 0.0625 (0x102C0744), which
    is exact, so only the dot product needs the narrowing.
    """
    vecs = tex[0]
    mins = [999999.0, 999999.0]
    maxs = [-99999.0, -99999.0]

    for k in range(numedges):
        e = surfedges[firstedge + k]
        if e >= 0:
            v = verts[edges[e][0]]
        else:
            v = verts[edges[-e][1]]

        for j in range(2):
            val = _f32(v[0] * vecs[j][0] + v[1] * vecs[j][1] +
                       v[2] * vecs[j][2] + vecs[j][3])
            if val < mins[j]:
                mins[j] = val
            if val > maxs[j]:
                maxs[j] = val

    extents = []
    for j in range(2):
        bmin = math.floor(mins[j] / 16.0)
        bmax = math.ceil(maxs[j] / 16.0)
        extents.append(int((bmax - bmin) * 16))
    return extents


def analyse(path):
    bsp = Bsp(path)

    submodels = len(bsp.lump(LUMP_MODELS)) // 64

    verts = [struct.unpack_from("<3f", bsp.lump(LUMP_VERTEXES), i * 12)
             for i in range(len(bsp.lump(LUMP_VERTEXES)) // 12)]
    edge_raw = bsp.lump(LUMP_EDGES)
    edges = [struct.unpack_from("<2H", edge_raw, i * 4)
             for i in range(len(edge_raw) // 4)]
    se_raw = bsp.lump(LUMP_SURFEDGES)
    surfedges = list(struct.unpack_from("<%di" % (len(se_raw) // 4), se_raw, 0))

    ti_raw = bsp.lump(LUMP_TEXINFO)
    texinfos = []
    for i in range(len(ti_raw) // 40):
        f = struct.unpack_from("<8f", ti_raw, i * 40)
        miptex, flags = struct.unpack_from("<2i", ti_raw, i * 40 + 32)
        texinfos.append((((f[0], f[1], f[2], f[3]), (f[4], f[5], f[6], f[7])),
                         miptex, flags))

    names = bsp.texture_names()

    # AllocBlock state. The atlas grows without limit here so the report is a
    # COUNT rather than a yes/no -- the client's ceiling is applied afterwards.
    allocated = []
    blocks_used = 0
    overflowed = False
    size_hist = {}

    face_raw = bsp.lump(LUMP_FACES)
    nfaces = len(face_raw) // 20
    lit_faces = 0
    worst_extent = 0
    bad_extent_faces = 0

    for i in range(nfaces):
        (planenum, side, firstedge, numedges, texinfo_i,
         s0, s1, s2, s3, lightofs) = struct.unpack_from("<HhihhBBBBi", face_raw, i * 20)

        if texinfo_i < 0 or texinfo_i >= len(texinfos):
            continue
        tex = texinfos[texinfo_i]
        name = names[tex[1]] if 0 <= tex[1] < len(names) else ""

        ext = face_extents(bsp, verts, edges, surfedges, texinfos,
                           firstedge, numedges, tex)

        special = bool(tex[2] & TEX_SPECIAL)
        if not special:
            worst_extent = max(worst_extent, ext[0], ext[1])
            if ext[0] > MAX_SURFACE_EXTENTS or ext[1] > MAX_SURFACE_EXTENTS:
                bad_extent_faces += 1

        # GL_BuildLightmaps (RVA 0x24709B) skips a face when
        #   surf->flags & (SURF_DRAWSKY|SURF_DRAWTURB)          -- 0x14
        #   or (surf->flags & SURF_DRAWTILED and TEX_SPECIAL)   -- 0x20
        # and Mod_LoadFaces (RVA 0x2401E0) sets those from the texture name:
        #   "sky"    strncmp,  3  -> SKY|TILED
        #   "scroll" strncmp,  6  -> TILED
        #   '!'      first char   -> TURB
        #   "laser"  strnicmp, 5  -> TURB
        #   "water"  strnicmp, 5  -> TURB
        #   TEX_SPECIAL           -> TILED
        # so the net rule is: sky, turbulent, or special takes no atlas space.
        # Nothing else does -- in particular a face with no lighting data still
        # gets a block, because GL_BuildLightmaps never looks at lightofs.
        low = name.lower()
        if (name[:3] == "sky" or name[:1] == "!"
                or low[:5] == "laser" or low[:5] == "water"
                or special):
            continue

        lit_faces += 1

        w = (ext[0] >> 4) + 1
        h = (ext[1] >> 4) + 1
        if w > BLOCK_WIDTH or h > BLOCK_HEIGHT:
            # cannot be placed at all; the extents check above already flags it
            overflowed = True
            continue

        size_hist[(w, h)] = size_hist.get((w, h), 0) + 1

        placed = False
        texnum = -1
        while True:
            texnum += 1
            if texnum >= len(allocated):
                allocated.append([0] * BLOCK_WIDTH)
            best = BLOCK_HEIGHT
            best_x = -1
            row = allocated[texnum]

            for x in range(BLOCK_WIDTH - w):
                best2 = 0
                j = 0
                while j < w:
                    if row[x + j] >= best:
                        break
                    if row[x + j] > best2:
                        best2 = row[x + j]
                    j += 1
                if j == w:
                    best = best2
                    best_x = x

            if best + h > BLOCK_HEIGHT:
                continue

            for j in range(w):
                row[best_x + j] = best + h
            blocks_used = max(blocks_used, texnum + 1)
            placed = True
            break

        if not placed:
            overflowed = True
            break

    overflowed = blocks_used > MAX_LIGHTMAPS

    return {
        "hist": size_hist,
        "submodels": submodels,
        "faces": nfaces,
        "lit_faces": lit_faces,
        "blocks": blocks_used,
        "overflow": overflowed,
        "worst_extent": worst_extent,
        "bad_extent_faces": bad_extent_faces,
    }


# This model is exact, not an estimate, and that was measured rather than
# assumed. Breaking on GL_BuildLightmaps' per-surface entry (RVA 0x2470DB) and
# logging (w, h) for every surface of abandoned.bsp gives 20453 surfaces; this
# code produces the same 20453 in the same order with ZERO differing sizes.
# Breaking on the function's epilogue and reading allocated[] back gives the
# block count the client actually reached:
#
#   map              engine                     this model
#   abandoned        64 blocks, atlas full      64
#   toadsnatch       61 blocks                  61
#   turretfortress   "AllocBlock: full"         65  (correctly rejected)
#
# So there is no margin to allow for. Anything over MAX_LIGHTMAPS fails.
MARGIN = 0


def verdict(r):
    reasons = []
    if r["bad_extent_faces"]:
        reasons.append("%d face(s) over %d extents" %
                       (r["bad_extent_faces"], MAX_SURFACE_EXTENTS))
    if r["blocks"] > MAX_LIGHTMAPS:
        reasons.append("needs %d lightmap blocks, client has %d" %
                       (r["blocks"], MAX_LIGHTMAPS))
    return reasons


def notes(r):
    if r["submodels"] >= MAX_SUBMODELS:
        return ["%d submodels: no retail listen server (max %d)" %
                (r["submodels"], MAX_SUBMODELS - 1)]
    return []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("maps", nargs="*")
    ap.add_argument("--dir", help="scan every .bsp in this directory")
    args = ap.parse_args()

    paths = list(args.maps)
    if args.dir:
        paths += sorted(os.path.join(args.dir, f)
                        for f in os.listdir(args.dir) if f.lower().endswith(".bsp"))

    if not paths:
        ap.error("no maps given")

    bad = 0
    noted = 0
    print("%-40s %7s %6s %7s %7s  %s" %
          ("map", "faces", "submdl", "blocks", "maxext", "verdict"))
    for p in paths:
        try:
            r = analyse(p)
        except Exception as e:
            print("%-40s %s" % (os.path.basename(p)[:40], "ERROR: %s" % e))
            continue

        reasons = verdict(r)
        extra = notes(r)
        if reasons:
            bad += 1
        elif extra:
            noted += 1
        print("%-40s %7d %6d %7d %7d  %s" % (
            os.path.basename(p)[:40],
            r["faces"],
            r["submodels"],
            r["blocks"],
            r["worst_extent"],
            "; ".join(reasons + extra) if (reasons or extra) else "ok"))

    print("\n%d of %d map(s) a retail Half-Life client cannot render." %
          (bad, len(paths)))
    if noted:
        print("%d further map(s) load on a dedicated server but not a retail "
              "listen server." % noted)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
