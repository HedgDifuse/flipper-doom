# Copyright (C) 2026 HedgDifuse
# SPDX-License-Identifier: GPL-2.0-or-later
"""Make 11025 Hz audio as loud as the Flipper's piezo speaker allows.

The piezo gives almost nothing below a few hundred Hz and is loudest around
2-4 kHz, and the PWM output has 8 bits and a hard ceiling. So, in order:

  - a high-pass at HIGHPASS_HZ: bass the piezo cannot play only eats headroom;
  - a presence boost (high shelf) where the piezo is loud;
  - a compressor: a smoothed envelope sets the gain towards TARGET_RMS, at
    most MAX_GAIN_DB up, so quiet passages come up and loud ones do not grow;
  - a soft limiter (tanh) instead of clipping at full scale.

loud(x) takes and returns float samples in -1..1; to_u8() makes the unsigned
8-bit PCM the Flipper plays.
"""
import numpy as np
from scipy import signal

RATE = 11025
HIGHPASS_HZ = 280
PRESENCE_HZ, PRESENCE_DB = 1800, 5.0
TARGET_RMS = 0.45
MAX_GAIN_DB = 14.0
ATTACK_S, RELEASE_S = 0.005, 0.12
DRIVE = 1.6  # into the limiter: higher is louder and more squashed


def _high_shelf(f0, gain_db, rate):
    """RBJ cookbook high shelf, S = 1"""
    a = 10 ** (gain_db / 40)
    w0 = 2 * np.pi * f0 / rate
    cosw, alpha = np.cos(w0), np.sin(w0) / 2 * np.sqrt(2)
    b = [a * ((a + 1) + (a - 1) * cosw + 2 * np.sqrt(a) * alpha),
         -2 * a * ((a - 1) + (a + 1) * cosw),
         a * ((a + 1) + (a - 1) * cosw - 2 * np.sqrt(a) * alpha)]
    den = [(a + 1) - (a - 1) * cosw + 2 * np.sqrt(a) * alpha,
           2 * ((a - 1) - (a + 1) * cosw),
           (a + 1) - (a - 1) * cosw - 2 * np.sqrt(a) * alpha]
    return np.array(b) / den[0], np.array(den) / den[0]


def _envelope(x, rate):
    """peak envelope with a fast attack and a slow release"""
    up = np.exp(-1 / (ATTACK_S * rate))
    down = np.exp(-1 / (RELEASE_S * rate))
    env = np.empty_like(x)
    e = 0.0
    for i, v in enumerate(np.abs(x)):
        e = up * e + (1 - up) * v if v > e else down * e + (1 - down) * v
        env[i] = e
    return env


def loud(x, rate=RATE):
    x = np.asarray(x, dtype=np.float64)
    if len(x) == 0:
        return x
    sos = signal.butter(2, HIGHPASS_HZ, "highpass", fs=rate, output="sos")
    x = signal.sosfilt(sos, x)
    b, a = _high_shelf(PRESENCE_HZ, PRESENCE_DB, rate)
    x = signal.lfilter(b, a, x)

    # the envelope follows the peaks; aim them at a level whose RMS is TARGET_RMS
    env = _envelope(x, rate)
    peak_target = TARGET_RMS * 1.6
    gain = np.minimum(peak_target / np.maximum(env, 1e-4), 10 ** (MAX_GAIN_DB / 20))
    x = x * gain
    return np.tanh(x * DRIVE) / np.tanh(DRIVE)


def to_u8(x):
    return np.clip(np.round(np.asarray(x) * 127 + 128), 0, 255).astype(np.uint8).tobytes()
