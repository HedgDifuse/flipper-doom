"""The menu's background: gameplay recorded by the simulator, looped.

  FZ_NOHUD=1 FZ_PERIOD=15 FZ_DUMP1=demo.bin ./fzsim -nosound -timedemo demo3
  python port/tools/mkbg.py demo.bin MENUBG.BIN [step]

Takes every step-th frame (default 2: 35 / 2 = 17.5 fps) of the LCD gray
frames (128x64, 4 bits a pixel, darkness 0..15). The file: a 16-byte header
("FZBG", frames u32, frames per 10 s u16, 6 bytes zero), then the frames.
"""
import struct, sys

src, dst = sys.argv[1], sys.argv[2]
step = int(sys.argv[3]) if len(sys.argv) > 3 else 2
F = 128 * 64 // 2
d = open(src, 'rb').read()
frames = [d[i * F:(i + 1) * F] for i in range(0, len(d) // F, step)]
with open(dst, 'wb') as f:
    f.write(b'FZBG' + struct.pack('<IH6x', len(frames), 350 // step))
    for fr in frames:
        f.write(fr)
print(len(frames), 'frames,', 16 + len(frames) * F, 'bytes')
