# Copyright (C) 2026 HedgDifuse
# SPDX-License-Identifier: GPL-2.0-or-later
"""The sector data that never changes, made offline (used by fztex.py).

For every map E1Mx a lump E1MxSEC with what P_GroupLines worked out in the
zone at every level start: each sector's lines, its sound origin and the
blockmap cells it covers (widened by MAXRADIUS, for P_CheckSector). The
engine keeps only the line count and where the list starts, and reads the
rest through the page cache when needed (a door's sound, a lift moving).

Computed exactly as the engine did (same order, same fixed point quirks):

    uint16 sectors
    sectors x { int16 soundorg x, y; uint8 blockbox[4] (top, bottom, left, right) }
    uint16 first[sectors + 1]   index of the sector's first line in lines[]
    uint16 lines[]              line numbers
"""
import struct

FRACBITS = 16
MAPBLOCKSHIFT = FRACBITS + 7
MAXRADIUS = 32 << FRACBITS
BOXTOP, BOXBOTTOM, BOXLEFT, BOXRIGHT = 0, 1, 2, 3
INT32_MIN, INT32_MAX = -(1 << 31), (1 << 31) - 1
NO_INDEX = 0xFFFF


def c_div(a, b):
    """C integer division (truncates toward zero)"""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def sector_lump(data, lumps, map_index):
    """lumps: the WAD directory [(name, pos, size, ...)], map_index: where the
    map's marker (E1Mx) is; its lumps follow it"""
    names = {}
    i = map_index + 1
    while i < len(lumps) and lumps[i][0] in ("THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES", "SEGS",
                                             "SSECTORS", "NODES", "SECTORS", "REJECT", "BLOCKMAP"):
        names[lumps[i][0]] = lumps[i]
        i += 1

    _, pos, size = names["LINEDEFS"][:3]
    lines = []
    for k in range(size // 15):
        v1x, v1y, v2x, v2y, front, back, flags, special, tag = struct.unpack_from("<hhhhhhBbb", data, pos + 15 * k)
        lines.append((v1x, v1y, v2x, v2y, front & 0xFFFF, back & 0xFFFF))

    _, pos, size = names["SIDEDEFS"][:3]
    side_sector = [data[pos + 7 * k + 6] for k in range(size // 7)]

    _, pos, size = names["SECTORS"][:3]
    numsectors = size // 12  # mapsector_t with FLAT_SPAN

    _, pos, _ = names["BLOCKMAP"][:3]
    orgx, orgy, bmapwidth, bmapheight = struct.unpack_from("<hhhh", data, pos)
    orgx <<= FRACBITS
    orgy <<= FRACBITS

    # the lines of each sector, in P_GroupLines' order
    sector_lines = [[] for _ in range(numsectors)]
    for num, (_, _, _, _, front, back) in enumerate(lines):
        fs = side_sector[front]
        sector_lines[fs].append(num)
        if back != NO_INDEX and side_sector[back] != fs:
            sector_lines[side_sector[back]].append(num)

    records = bytearray()
    for s in range(numsectors):
        assert sector_lines[s], f"sector {s} has no lines"
        box = [INT32_MIN, INT32_MAX, INT32_MAX, INT32_MIN]  # M_ClearBox
        for num in sector_lines[s]:
            v1x, v1y, v2x, v2y = lines[num][:4]
            for x, y in ((v1x << FRACBITS, v1y << FRACBITS), (v2x << FRACBITS, v2y << FRACBITS)):
                if x < box[BOXLEFT]:  # M_AddToBox, else-if and all
                    box[BOXLEFT] = x
                elif x > box[BOXRIGHT]:
                    box[BOXRIGHT] = x
                if y < box[BOXBOTTOM]:
                    box[BOXBOTTOM] = y
                elif y > box[BOXTOP]:
                    box[BOXTOP] = y
        sx = (c_div(box[BOXRIGHT], 2) + c_div(box[BOXLEFT], 2)) >> FRACBITS
        sy = (c_div(box[BOXTOP], 2) + c_div(box[BOXBOTTOM], 2)) >> FRACBITS

        assert bmapwidth <= 256 and bmapheight <= 256
        top = (box[BOXTOP] - orgy + MAXRADIUS) >> MAPBLOCKSHIFT
        bottom = (box[BOXBOTTOM] - orgy - MAXRADIUS) >> MAPBLOCKSHIFT
        right = (box[BOXRIGHT] - orgx + MAXRADIUS) >> MAPBLOCKSHIFT
        left = (box[BOXLEFT] - orgx - MAXRADIUS) >> MAPBLOCKSHIFT
        blockbox = [min(top, bmapheight - 1), max(bottom, 0), max(left, 0), min(right, bmapwidth - 1)]
        records += struct.pack("<hh4B", sx, sy, *blockbox)

    first = [0]
    for s in range(numsectors):
        first.append(first[-1] + len(sector_lines[s]))
    out = struct.pack("<H", numsectors) + records + struct.pack(f"<{numsectors + 1}H", *first)
    out += struct.pack(f"<{first[-1]}H", *(n for sl in sector_lines for n in sl))
    return out


def sector_lumps(data, lumps):
    """[(name, lump)] for every map"""
    out = []
    for i, entry in enumerate(lumps):
        name = entry[0]
        if len(name) == 4 and name[0] == "E" and name[2] == "M":
            out.append(((name + "SEC").encode("latin1"), sector_lump(data, lumps, i)))
    return out
