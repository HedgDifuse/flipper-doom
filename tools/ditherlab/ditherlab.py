# Copyright (C) 2026 HedgDifuse
# SPDX-License-Identifier: GPL-2.0-or-later
"""Dithering lab: real Doom demo frames -> Flipper Zero 128x64 1-bit.

Input: frames.rgb from tools/framedump (raw 320x200 RGB per game tic).
Output: out/stills/*.png (comparison grid) and out/clips/*.gif (animated, per algorithm).

Everything that should end up on the device is threshold-matrix based or
row-local, so it maps 1:1 to C. Error diffusion is here for comparison only.
"""
import os
import sys
import numpy as np
from PIL import Image

W, H = 128, 64
SRC = os.path.join(os.path.dirname(__file__), "..", "framedump", "frames.rgb")
OUT = os.path.join(os.path.dirname(__file__), "out")
FRAME = 320 * 200 * 3
# Flipper LCD: orange backlight, dark pixels
LCD_BG = (255, 140, 41)
LCD_FG = (40, 24, 10)


def load_frame(data, i):
    return np.frombuffer(data, np.uint8, FRAME, i * FRAME).reshape(200, 320, 3)


def to_view(rgb):
    """Crop the 3D view (no status bar) and area-downscale to 128x64 luminance.

    Area averaging of a 320-wide frame is what the real port gets by rendering
    at 256x128 and box-filtering 2x2, so this is representative.
    """
    view = rgb[8:168].astype(np.float32)  # 320x160, mild vertical squish vs true aspect
    lum = view @ np.array([0.299, 0.587, 0.114], np.float32)
    img = Image.fromarray(lum).resize((W, H), Image.BOX)
    return np.asarray(img, np.float32) / 255.0


def tone(l, gamma=0.55, black=0.02, white=0.75):
    """Doom is dark: lift shadows, clip highlights. On device this is baked into
    the luminance colormaps (34 light levels x 256 palette entries)."""
    l = np.clip((l - black) / (white - black), 0, 1)
    return l ** gamma


def unsharp(l, amount=0.8):
    p = np.pad(l, 1, mode="edge")
    blur = (p[:-2, 1:-1] + p[2:, 1:-1] + p[1:-1, :-2] + p[1:-1, 2:] + 4 * l) / 8
    return np.clip(l + amount * (l - blur), 0, 1)


# ---------- threshold matrices ----------

def bayer(n):
    m = np.array([[0]])
    while m.shape[0] < n:
        m = np.block([[4 * m, 4 * m + 2], [4 * m + 3, 4 * m + 1]])
    return (m + 0.5) / m.size


def blue_noise(n=64, sigma=1.5, seed=1):
    """Void-and-cluster (Ulichney). Toroidal gaussian via FFT."""
    rng = np.random.default_rng(seed)
    f = np.fft.fftfreq(n)
    g = np.exp(-2 * (np.pi * sigma) ** 2 * (f[:, None] ** 2 + f[None, :] ** 2))

    def blur(b):
        return np.real(np.fft.ifft2(np.fft.fft2(b) * g))

    pts = np.zeros((n, n), bool)
    pts.flat[rng.choice(n * n, n * n // 10, replace=False)] = True
    # relax initial pattern: move tightest cluster to largest void
    while True:
        e = blur(pts)
        c = np.argmax(np.where(pts, e, -np.inf))
        pts.flat[c] = False
        e = blur(pts)
        v = np.argmin(np.where(pts, np.inf, e))
        pts.flat[v] = True
        if v == c:
            break
    rank = np.zeros(n * n, np.int32)
    ones = int(pts.sum())
    p = pts.copy()
    for r in range(ones - 1, -1, -1):
        e = blur(p)
        c = np.argmax(np.where(p, e, -np.inf))
        p.flat[c] = False
        rank[c] = r
    p = pts.copy()
    for r in range(ones, n * n):
        e = blur(p)
        v = np.argmin(np.where(p, np.inf, e))
        p.flat[v] = True
        rank[v] = r
    return (rank.reshape(n, n) + 0.5) / (n * n)


def ordered(l, m):
    th = np.tile(m, (H // m.shape[0] + 1, W // m.shape[1] + 1))[:H, :W]
    return l > th


# ---------- error diffusion (reference only) ----------

def diffuse(l, kernel, div, serpentine=False):
    e = l.copy()
    out = np.zeros_like(l, bool)
    for y in range(H):
        rev = serpentine and y % 2
        xs = range(W - 1, -1, -1) if rev else range(W)
        for x in xs:
            old = e[y, x]
            new = old > 0.5
            out[y, x] = new
            err = (old - new) / div
            for dx, dy, w in kernel:
                dx = -dx if rev else dx
                xx, yy = x + dx, y + dy
                if 0 <= xx < W and yy < H:
                    e[yy, xx] += err * w
    return out


FS = [(1, 0, 7), (-1, 1, 3), (0, 1, 5), (1, 1, 1)]
ATK = [(1, 0, 1), (2, 0, 1), (-1, 1, 1), (0, 1, 1), (1, 1, 1), (0, 2, 1)]


BN = None


def algos():
    global BN
    if BN is None:
        cache = os.path.join(OUT, "bluenoise64.npy")
        if os.path.exists(cache):
            BN = np.load(cache)
        else:
            BN = blue_noise(64)
            np.save(cache, BN)
    b4, b8 = bayer(4), bayer(8)
    return {
        "threshold": lambda l: l > 0.5,
        "bayer4": lambda l: ordered(l, b4),
        "bayer8": lambda l: ordered(l, b8),
        "bluenoise": lambda l: ordered(l, BN),
        "floyd": lambda l: diffuse(l, FS, 16, serpentine=True),
        "atkinson": lambda l: diffuse(l, ATK, 8),
        "bn_sharp": lambda l: ordered(unsharp(l), BN),
    }


def lcd(bits, scale):
    img = np.where(bits[..., None], np.array(LCD_BG, np.uint8), np.array(LCD_FG, np.uint8))
    return Image.fromarray(img.astype(np.uint8)).resize((W * scale, H * scale), Image.NEAREST)


def main():
    os.makedirs(os.path.join(OUT, "stills"), exist_ok=True)
    os.makedirs(os.path.join(OUT, "clips"), exist_ok=True)
    data = open(SRC, "rb").read()
    n = len(data) // FRAME
    A = algos()

    stills = [int(s) for s in os.environ.get("STILLS", "300,900,1800,2700,3600,4500").split(",")]
    for i in stills:
        rgb = load_frame(data, i)
        Image.fromarray(rgb).save(os.path.join(OUT, "stills", f"{i}_src.png"))
        l = tone(to_view(rgb))
        for name, fn in A.items():
            lcd(fn(l), 4).save(os.path.join(OUT, "stills", f"{i}_{name}.png"))
        print("still", i, flush=True)

    start, length, step = (int(v) for v in os.environ.get("CLIP", "780,210,2").split(","))
    for name, fn in A.items():
        frames = []
        for i in range(start, min(start + length, n), step):
            frames.append(lcd(fn(tone(to_view(load_frame(data, i)))), 3))
        frames[0].save(os.path.join(OUT, "clips", f"{name}.gif"), save_all=True,
                       append_images=frames[1:], duration=int(1000 * step / 35), loop=0)
        print("clip", name, flush=True)


if __name__ == "__main__":
    main()
