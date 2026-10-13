# Copyright (C) 2026 HedgDifuse
# SPDX-License-Identifier: GPL-2.0-or-later
"""Render LCD gray frames dumped by the simulator (FZ_DUMP1) as they would look
on the Flipper: each level 0..period is a mix of backlight and pixel colour.

usage: lcdview.py lcd.bin out_prefix --period 4 [--frames a,b] [--gif start:end] [--scale 4]
"""
import argparse
import numpy as np
from PIL import Image

BG, FG = np.array((255, 140, 41), float), np.array((40, 24, 10), float)
ap = argparse.ArgumentParser()
ap.add_argument("dump")
ap.add_argument("out")
ap.add_argument("--period", type=int, default=4)
ap.add_argument("--frames", default="")
ap.add_argument("--gif", default="")
ap.add_argument("--scale", type=int, default=4)
a = ap.parse_args()
data = open(a.dump, "rb").read()
n = len(data) // 4096
print("frames:", n)


def frame(i):
    raw = np.frombuffer(data, np.uint8, 4096, i * 4096)
    levels = np.empty(8192, np.uint8)
    levels[0::2] = raw & 15
    levels[1::2] = raw >> 4
    t = (levels.reshape(64, 128) / a.period)[..., None]
    img = (BG * (1 - t) + FG * t).astype(np.uint8)
    return Image.fromarray(img).resize((128 * a.scale, 64 * a.scale), Image.NEAREST)


for f in filter(None, a.frames.split(",")):
    frame(int(f)).save(f"{a.out}_{f}.png")
if a.gif:
    s, e = (int(v) for v in a.gif.split(":"))
    fr = [frame(i) for i in range(s, min(e, n))]
    fr[0].save(f"{a.out}.gif", save_all=True, append_images=fr[1:], duration=29, loop=0)
