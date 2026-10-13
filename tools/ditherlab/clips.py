# Copyright (C) 2026 HedgDifuse
# SPDX-License-Identifier: GPL-2.0-or-later
"""Animated comparison clips (temporal stability is what kills error diffusion)."""
import os
import numpy as np
from PIL import Image
import ditherlab as D, aware as R

data = open(D.SRC, "rb").read()
D.algos(); BN = D.BN; B4 = D.bayer(4)
A = {
    "naive_floyd": lambda l, ids: D.diffuse(l, D.FS, 16, serpentine=True),
    "naive_bluenoise": lambda l, ids: D.ordered(l, BN),
    "local_thr": lambda l, ids: (lambda m: np.clip(0.5 + 2 * (l - m) + 0.35 * (m - l.mean()), 0, 1) > 0.5)(R.boxblur(l, 6)),
    "aware_F": lambda l, ids: R.render(l, ids, B4, levels=3, lo=0.45, hi=1.0),
    "aware_G": lambda l, ids: R.render(l, ids, B4, levels=3, lo=0.45, hi=1.0, plane_lift=0.35),
}
CLIPS = {"turn": (780, 175), "imp": (4380, 175)}
os.makedirs("out/clips", exist_ok=True)
for cname, (start, n) in CLIPS.items():
    for name, fn in A.items():
        frames = []
        for i in range(start, start + n):
            l = D.tone(D.to_view(D.load_frame(data, i)))
            frames.append(D.lcd(fn(l, R.load_ids(i)), 3))
        frames[0].save(f"out/clips/{cname}_{name}.gif", save_all=True, append_images=frames[1:],
                       duration=29, loop=0, optimize=True)
    src = [Image.fromarray(D.load_frame(data, i)[8:168]).resize((384, 192)) for i in range(start, start + n)]
    src[0].save(f"out/clips/{cname}_source.gif", save_all=True, append_images=src[1:], duration=29, loop=0)
    print(cname, flush=True)
