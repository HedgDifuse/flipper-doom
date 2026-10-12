"""FZSPR: the sprite frame table, made offline (used by fztex.py).

R_InitSprites built it in the zone at startup (about 7 KB): for every sprite
in the engine's sprnames (engine/info.c), its frames with the lump and flip
of each of the 8 rotations. On Flipper the engine reads the one frame it
draws through the page cache instead.

Layout: "FZSP", uint16 sprites, uint16 first[sprites + 1] (index of the
sprite's first frame), then 18-byte frames: int16 lump[8], uint8 flipmask,
uint8 rotate.
"""
import os
import re
import struct


def sprnames():
    info = open(os.path.join(os.path.dirname(__file__), "..", "engine", "info.c"), encoding="latin-1").read()
    block = info[info.index("sprnames[NUMSPRITES]"):]
    return re.findall(r'"(\w{4})"', block[:block.index("};")])


def sprite_frames(lump_names):
    """R_InitSprites and R_InstallSpriteLump, in the same order"""
    first = lump_names.index("S_START") + 1
    last = lump_names.index("S_END") - 1
    sprites = []
    for name in sprnames():
        frames = {}

        def install(lump, frame, rotation, flipped):
            f = frames.setdefault(frame, {"lump": [-1] * 8, "flip": 0, "rotate": None})
            if rotation == 0:  # the lump is used for all rotations
                f["flip"] = 0
                for r in range(8):
                    if f["lump"][r] == -1:
                        f["lump"][r] = lump
                        if flipped:
                            f["flip"] |= 1 << r
                        f["rotate"] = False
                return
            rotation -= 1
            if f["lump"][rotation] == -1:
                f["lump"][rotation] = lump
                if flipped:
                    f["flip"] |= 1 << rotation
                else:
                    f["flip"] &= ~(1 << rotation)
                f["rotate"] = True

        # the engine walks a hash chain that has the later lumps first
        for lump in range(last, first - 1, -1):
            sn = lump_names[lump]
            if sn[:4] != name:
                continue
            install(lump, ord(sn[4]) - ord("A"), ord(sn[5]) - ord("0"), False)
            if len(sn) > 6:
                install(lump, ord(sn[6]) - ord("A"), ord(sn[7]) - ord("0"), True)

        out = []
        for k in range(max(frames) + 1 if frames else 0):
            f = frames.get(k)
            assert f and f["rotate"] is not None, f"{name} frame {k}: no patches"
            assert not f["rotate"] or -1 not in f["lump"], f"{name} frame {k}: missing rotations"
            out.append(f)
        sprites.append(out)
    return sprites


def fzspr(lump_names):
    """the lump and its number of frames"""
    sprites = sprite_frames(lump_names)
    out = bytearray(struct.pack("<4sH", b"FZSP", len(sprites)))
    index = 0
    for frames in sprites:
        out += struct.pack("<H", index)
        index += len(frames)
    out += struct.pack("<H", index)
    for frames in sprites:
        for f in frames:
            out += struct.pack("<8hBB", *f["lump"], f["flip"], 1 if f["rotate"] else 0)
    return bytes(out), index
