"""Render frames dumped by the simulator (FZ_DUMP) to PNG/GIF.

Each frame is FZ_W x FZ_H bytes: 6-bit luminance + 2-bit pixel class.
  --mode lum    grayscale luminance (default)
  --mode class  luminance tinted by class: wall gray, flat blue, sprite red
usage: simview.py dump.bin out_prefix [--frames a,b] [--gif start:end]
"""
import argparse
import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("dump")
ap.add_argument("out")
ap.add_argument("--w", type=int, default=120)
ap.add_argument("--h", type=int, default=64)
ap.add_argument("--frames", default="")
ap.add_argument("--gif", default="")
ap.add_argument("--scale", type=int, default=4)
ap.add_argument("--mode", default="lum")
a = ap.parse_args()

data = open(a.dump, "rb").read()
fsize = a.w * a.h
n = len(data) // fsize
print("frames:", n)
TINT = np.array([[1.0, 1.0, 0.6], [1, 1, 1], [0.6, 0.7, 1.0], [1.0, 0.5, 0.5]])


def frame(i):
    px = np.frombuffer(data, np.uint8, fsize, i * fsize).reshape(a.h, a.w)
    lum = (px & 63).astype(np.float32) * (255 / 63)
    if a.mode == "class":
        rgb = lum[..., None] * TINT[px >> 6]
    else:
        rgb = np.repeat(lum[..., None], 3, axis=2)
    img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8))
    return img.resize((a.w * a.scale, a.h * a.scale), Image.NEAREST)


for f in filter(None, a.frames.split(",")):
    frame(int(f)).save(f"{a.out}_{f}.png")
if a.gif:
    s, e = (int(v) for v in a.gif.split(":"))
    fr = [frame(i) for i in range(s, min(e, n))]
    fr[0].save(f"{a.out}.gif", save_all=True, append_images=fr[1:], duration=29, loop=0)
