# Copyright (C) 2026 HedgDifuse
# SPDX-License-Identifier: GPL-2.0-or-later
"""Renderer-aware 1-bit pipeline: uses the per-pixel object ID buffer that the
real port gets for free from the renderer."""
import numpy as np
import ditherlab as D

IDS = D.SRC.replace("frames.rgb", "frames.ids")
WALL, PLANE, SPRITE, WEAPON, SKY = 1, 2, 3, 4, 5
_ids = None
# target pixel centers in source coordinates (view = rows 8..168)
YS = (8 + (np.arange(D.H) + 0.5) * 160 / D.H).astype(int)
XS = ((np.arange(D.W) + 0.5) * 320 / D.W).astype(int)


def load_ids(i):
    global _ids
    if _ids is None:
        _ids = np.memmap(IDS, np.uint16, "r")
    return np.array(_ids[i * 64000:(i + 1) * 64000]).reshape(200, 320)[np.ix_(YS, XS)]


def edges(ids, mask):
    """1px lines where object id changes, drawn on the farther-left/top pixel."""
    e = np.zeros(ids.shape, bool)
    e[:, :-1] |= ids[:, :-1] != ids[:, 1:]
    e[:-1, :] |= ids[:-1, :] != ids[1:, :]
    return e & mask


def boxblur(l, r):
    k = np.ones(2 * r + 1) / (2 * r + 1)
    p = np.pad(l, r, mode="edge")
    t = np.apply_along_axis(lambda v: np.convolve(v, k, "valid"), 1, p)
    return np.apply_along_axis(lambda v: np.convolve(v, k, "valid"), 0, t)


def dilate(m):
    o = m.copy()
    o[1:] |= m[:-1]; o[:-1] |= m[1:]; o[:, 1:] |= m[:, :-1]; o[:, :-1] |= m[:, 1:]
    return o


def render(l, ids, tone_matrix, levels=None, sprite_local=True, halo=True, geo_edges=True,
           lo=0.12, hi=0.92, plane_lift=0.0):
    cls = ids >> 12
    t = l.copy()
    t[cls == PLANE] = np.clip(t[cls == PLANE] + plane_lift, 0, 1)
    if levels:
        t = np.round(t * (levels - 1)) / (levels - 1)
    # environment: soft tones in a clean ordered pattern, kept away from pure black
    env = D.ordered(np.clip(lo + t * (hi - lo), 0, 1), tone_matrix)
    out = env.copy()
    out[cls == SKY] = True
    spr = (cls == SPRITE) | (cls == WEAPON)
    if geo_edges:
        out[edges(ids, (cls == WALL) | (cls == PLANE))] = False
    if sprite_local:
        m = boxblur(l, 3)
        s = (0.5 + 2.2 * (l - m) + 0.6 * (l - 0.45)) > 0.5
        out[spr] = s[spr]
    if halo:
        ring = dilate(spr) & ~spr
        out[ring] = True               # light gap around monsters
        border = spr & ~(~dilate(~spr))  # outermost sprite pixels
        out[border & (cls == SPRITE)] = False
    return out
