"""Add the Flipper lumps to a Doom8088 WAD: FZTX (wall textures pre-composed)
and FZSPR (sprite frame table).

    fztex.py DOOM8088.WAD FZDOOM.WAD [doom1.wad]

With the original IWAD it also adds its digitized sounds (DS*), see
digital_sounds().

Doom8088 draws walls from the patches that make up a texture, loading each
patch lump whole into the zone. On Flipper the zone is too small for the
patches a single view needs, so they were thrown out and re-read from the SD
card every frame. FZTX holds every wall texture already composed:

  - only every STEP-th column: at 120 pixels across, Doom8088 itself samples
    composite textures at (texcolumn & ~3);
  - every 2nd row too (the view is 64 pixels tall): the texture repeated
    vertically with its own height and then halved, so the renderer uses
    row / 2 of its 7-bit row index without checks and tiling is right;
  - 4 bits per texel: a code into 16 grays of the Doom palette, spread evenly
    over the luminance the LCD shows (mklum.py's tone curve), still lit by the
    engine's colormaps. A column is 32 bytes, row r in byte r / 2, even rows in
    the low nibble;
  - read through the engine's page cache 32 bytes at a time.

Layout: "FZTX", uint16 textures, uint16 step, uint32 lump size, uint8 gray[16]
(palette index of each code), uint32 offset[textures] (from the lump start),
then the columns. The Doom8088 directory stores lump sizes
in 16 bits, so FZTX is listed with size 0; the engine only needs its position.

FZSPR is the sprite frame table, see fzspr.py; E1MxSEC the sector data of
each map, see fzsec.py.
"""
import struct
import sys

import numpy as np

from loudness import loud, to_u8

from fzsec import sector_lumps
from fzspr import fzspr

STEP = 4
ROWS = 128  # the renderer's row index range
COLUMN = 32  # bytes: every 2nd row, 2 texels per byte
GRAY_RAMP = range(80, 112)  # the palette's grays, 239 down to 35
GAMMA, BLACK, WHITE = 0.55, 0.02, 0.75  # as tools/mklum.py


def tone(l):
    l = min(max((l - BLACK) / (WHITE - BLACK), 0.0), 1.0)
    return l ** GAMMA


def gray_codes(pal):
    """16 palette indices (black and the gray ramp) evenly spread in tone, and
    for all 256 colors the nearest code"""
    lum = [tone((0.299 * pal[3 * i] + 0.587 * pal[3 * i + 1] + 0.114 * pal[3 * i + 2]) / 255) for i in range(256)]
    candidates = sorted([0] + list(GRAY_RAMP), key=lambda i: -lum[i])
    grays = []
    for k in range(16):
        want = 1 - k / 15
        best = min((i for i in candidates if i not in grays), key=lambda i: abs(lum[i] - want))
        grays.append(best)
    grays.sort(key=lambda i: -lum[i])
    nearest = [min(range(16), key=lambda c: abs(lum[grays[c]] - lum[i])) for i in range(256)]
    return grays, nearest


def read_wad(path):
    data = open(path, "rb").read()
    magic, numlumps, _, infotableofs = struct.unpack_from("<4shhi", data, 0)
    lumps = []
    for i in range(numlumps):
        pos, size, filler, name = struct.unpack_from("<iHh8s", data, infotableofs + 16 * i)
        lumps.append((name.rstrip(b"\0").decode("latin1"), pos, size, filler, name))
    return data, magic, infotableofs, lumps


def compose(data, by_name, pnames, tex, nearest):
    name, width, height, patches = tex
    w = 1
    while w * 2 <= width:
        w *= 2  # the engine wraps texture columns with widthmask = w - 1
    out = bytearray()
    for x in range(0, w, STEP):
        col = bytearray(height)
        for originx, originy, patch in patches:
            pos, _ = by_name[pnames[patch]]
            pw = struct.unpack_from("<h", data, pos)[0]
            px = x - originx
            if not 0 <= px < pw:
                continue
            p = pos + struct.unpack_from("<i", data, pos + 8 + 4 * px)[0]
            while data[p] != 0xFF:
                top, length = data[p], data[p + 1]
                for i in range(length):
                    y = originy + top + i
                    if 0 <= y < height:
                        col[y] = data[p + 3 + i]
                p += length + 4
        codes = [nearest[col[(2 * r) % height]] for r in range(ROWS // 2)]
        out += bytes(codes[2 * i] | codes[2 * i + 1] << 4 for i in range(COLUMN))
    return out


def main(src, dst, iwad=None):
    data, magic, infotableofs, lumps = read_wad(src)
    by_name = {}
    for name, pos, size, _, _ in lumps:
        by_name.setdefault(name, (pos, size))

    pos, _ = by_name["PLAYPAL"]
    grays, nearest = gray_codes(data[pos:pos + 768])

    pos, size = by_name["PNAMES"]
    count = struct.unpack_from("<i", data, pos)[0]
    pnames = [data[pos + 4 + 8 * i: pos + 12 + 8 * i].rstrip(b"\0").decode("latin1").upper() for i in range(count)]

    pos, _ = by_name["TEXTURE1"]
    numtex = struct.unpack_from("<i", data, pos)[0]
    offsets = struct.unpack_from("<%di" % numtex, data, pos + 4)
    textures = []
    for o in offsets:
        t = pos + o
        name = data[t:t + 8].rstrip(b"\0").decode("latin1")
        width, height, patchcount = struct.unpack_from("<hhh", data, t + 8)
        patches = [struct.unpack_from("<hhh", data, t + 14 + 6 * k) for k in range(patchcount)]
        textures.append((name, width, height, patches))

    head_size = 28
    table_size = 4 * numtex
    body = bytearray()
    table = []
    for tex in textures:
        table.append(head_size + table_size + len(body))
        body += compose(data, by_name, pnames, tex, nearest)
    header = struct.pack("<4sHHI16B", b"FZTX", numtex, STEP, head_size + table_size + len(body), *grays)
    assert len(header) == head_size
    lump = header + struct.pack("<%dI" % numtex, *table) + body

    spr, nframes = fzspr([name for name, _, _, _, _ in lumps])
    added = [(b"FZTX", lump, 0), (b"FZSPR", spr, len(spr))]
    added += [(name, sec, len(sec)) for name, sec in sector_lumps(data, lumps)]
    if iwad:
        added += digital_sounds(lumps, iwad)

    # keep the whole old file (jWadUtil puts the directory first), append the
    # new lumps and a new directory; the old directory stays behind unused
    new = bytearray(data)
    directory = bytearray()
    for _, p, s, f, raw in lumps:
        directory += struct.pack("<iHh8s", p, s, f, raw)
    for name, content, size in added:
        while len(new) % 4:
            new.append(0)
        directory += struct.pack("<iHh8s", len(new), size, 0, name.ljust(8, b"\0"))
        new += content
    new_infotableofs = len(new)
    new += directory
    struct.pack_into("<4shhi", new, 0, magic, len(lumps) + len(added), 0, new_infotableofs)
    open(dst, "wb").write(new)

    sounds = sum(len(c) for n, c, _ in added if n.startswith(b"DS"))
    print(f"{numtex} textures, FZTX {len(lump) / 1024:.0f} KB (step {STEP}, {COLUMN} B columns of {ROWS} rows, grays {grays}), "
          f"FZSPR {len(spr)} B ({nframes} frames), DS sounds {sounds / 1024:.0f} KB, "
          f"WAD {len(data) / 1024:.0f} -> {len(new) / 1024:.0f} KB")


def digital_sounds(lumps, iwad):
    """The original digitized sounds (DS*, 8-bit PCM in the DMX format) from
    the IWAD, which Doom8088 drops, in the order of its PC speaker sounds (DP*)
    so that the engine finds them the same way: DSPISTOL + sfx number - 1."""
    data = open(iwad, "rb").read()
    _, n, ofs = struct.unpack_from("<4sii", data, 0)
    found = {}
    for i in range(n):
        pos, size, name = struct.unpack_from("<ii8s", data, ofs + 16 * i)
        found[name.rstrip(b"\0")] = data[pos:pos + size]
    out = []
    for name, _, _, _, _ in lumps:
        if name.startswith("DP"):
            ds = b"DS" + name[2:].encode("latin1")
            pcm = found.get(ds, b"")
            if len(pcm) >= 8:
                fmt, rate, count = struct.unpack_from("<HHI", pcm, 0)
                if rate == 22050:
                    # the Flipper mixes sounds and music at 11025 Hz: every
                    # second sample, averaged with the next
                    s = pcm[8:8 + count]
                    s = bytes((s[i] + s[min(i + 1, len(s) - 1)]) // 2 for i in range(0, len(s), 2))
                    pcm = struct.pack("<HHI", fmt, 11025, len(s)) + s
            if len(pcm) >= 8:
                # as loud as the piezo speaker allows, see loudness.py
                fmt, rate, count = struct.unpack_from("<HHI", pcm, 0)
                x = (np.frombuffer(pcm[8:8 + count], dtype=np.uint8).astype(np.float64) - 128) / 128
                pcm = pcm[:8] + to_u8(loud(x / (np.abs(x).max() or 1.0), rate))
            assert len(pcm) < 65536, ds  # the directory keeps 16-bit sizes
            out.append((ds, pcm, len(pcm)))
    return out


if __name__ == "__main__":
    main(*sys.argv[1:4])
