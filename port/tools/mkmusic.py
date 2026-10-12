"""Render Doom's music (the MUS lumps of the IWAD) for the Flipper.

    mkmusic.py doom1.wad OUTDIR

Each D_* lump is played through libADLMIDI (an OPL emulator, in WSL at
~/libADLMIDI/build/adlmidiplay) with the DMX instrument bank Doom 1 shipped
(bank 16, Bobby Prince v1) and the DMX volume model, so it sounds like a Sound
Blaster running Doom. The 44.1 kHz stereo WAV is then folded to mono, low-pass
filtered, decimated to 11025 Hz, made loud for the piezo (loudness.py) and
written as raw unsigned 8-bit PCM: OUTDIR/D_E1M1.PCM and so on. The Flipper
streams these from the SD card and loops them.
"""
import os
import struct
import subprocess
import sys
import wave

import numpy as np

from loudness import loud, to_u8

RATE = 11025
BANK = 16          # DMX (Bobby Prince v1)
VOLUME_MODEL = 3   # DMX
ADLMIDI = "~/libADLMIDI/build/adlmidiplay"


def wsl_path(path):
    path = os.path.abspath(path).replace("\\", "/")
    return f"/mnt/{path[0].lower()}{path[2:]}"


def render(mus_path):
    """the WAV adlmidiplay writes next to the input"""
    subprocess.run(["wsl", "-e", "bash", "-lc",
                    f"{ADLMIDI} '{wsl_path(mus_path)}' -vm {VOLUME_MODEL} --emu-nuked-opl2 {BANK} 1 >/dev/null 2>&1"],
                   check=True)
    return mus_path + ".wav"


def to_pcm(wav_path):
    w = wave.open(wav_path)
    assert w.getsampwidth() == 2 and w.getframerate() == 44100, (w.getsampwidth(), w.getframerate())
    x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64) / 32768
    x = x.reshape(-1, w.getnchannels()).mean(axis=1)

    # windowed-sinc low-pass at ~4.8 kHz, then every 4th sample
    taps = 63
    n = np.arange(taps) - (taps - 1) / 2
    cutoff = 4800 / 44100
    h = 2 * cutoff * np.sinc(2 * cutoff * n) * np.hamming(taps)
    x = np.convolve(x, h / h.sum(), mode="same")[::44100 // RATE]

    # as loud as the piezo speaker allows, see loudness.py
    return to_u8(loud(x / (np.abs(x).max() or 1.0)))


def main(iwad, outdir):
    os.makedirs(outdir, exist_ok=True)
    data = open(iwad, "rb").read()
    _, n, ofs = struct.unpack_from("<4sii", data, 0)
    total = 0
    for i in range(n):
        pos, size, name = struct.unpack_from("<ii8s", data, ofs + 16 * i)
        name = name.rstrip(b"\0").decode()
        if not name.startswith("D_"):
            continue
        mus = os.path.join(outdir, name + ".mus")
        open(mus, "wb").write(data[pos:pos + size])
        wav = render(mus)
        pcm = to_pcm(wav)
        open(os.path.join(outdir, name + ".PCM"), "wb").write(pcm)
        os.remove(wav)
        os.remove(mus)
        total += len(pcm)
        print(f"{name}: {len(pcm) / RATE:.0f} s, {len(pcm) / 1024:.0f} KB")
    print(f"total {total / 1024 / 1024:.1f} MB")


if __name__ == "__main__":
    main(*sys.argv[1:3])
