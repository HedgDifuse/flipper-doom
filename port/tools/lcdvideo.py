"""A video of the simulator's frames as the Flipper's LCD shows them.

  FZ_PERIOD=8 FZ_GAMMA=0.8 FZ_DUMP1=lcd.bin ./fzsim -nosound -timedemo demo3
  python port/tools/lcdvideo.py lcd.bin 8 out.mp4 [--step 2] [--gif out.gif]

A pixel at gray level L of a period P is dark in L of every P subframes the
LCD shows (75 a second): the eye sees the average. Screen grabs (qFlipper)
catch single subframes instead, which is why they look like noise. Here every
pixel gets that average between the backlit orange and a dark pixel, scaled
up with the gaps between the LCD's pixels. The levels are mixed evenly as the
eye sees them: the screen calibration (gamma) made the steps look even.
"""
import argparse, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('dump')
ap.add_argument('period', type=int)
ap.add_argument('out')
ap.add_argument('--step', type=int, default=2, help='every n-th frame (35 fps / 2 = the device on light maps)')
ap.add_argument('--scale', type=int, default=6)
ap.add_argument('--gif', help='also a GIF')
ap.add_argument('--start', type=int, default=0)
ap.add_argument('--gap', type=float, default=0.8, help='1: no visible pixel grid')
ap.add_argument('--frames', type=int, default=0)
a = ap.parse_args()

BACKLIGHT = (255, 139, 41)  # the orange of the lit LCD
INK = (34, 26, 18)          # a dark pixel on it


P, S, GAP = a.period, a.scale, a.gap  # GAP: how much a dark pixel darkens the gap around it
colors = []  # per level: (pixel, gap)
for level in range(16):
    f = min(level, P) / P
    px = [int(BACKLIGHT[i] * (1 - f) + INK[i] * f + 0.5) for i in range(3)]
    gap = [int(BACKLIGHT[i] * (1 - f * GAP) + INK[i] * f * GAP + 0.5) for i in range(3)]
    colors.append((bytes(px), bytes(gap)))

F = 128 * 64 // 2
data = open(a.dump, 'rb').read()
count = len(data) // F
idx = list(range(a.start, count, a.step))
if a.frames:
    idx = idx[:a.frames]
W, H = 128 * S, 64 * S
fps = 35 / a.step

enc = subprocess.Popen(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                        '-s', '%dx%d' % (W, H), '-r', '%g' % fps, '-i', '-',
                        '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '18', a.out], stdin=subprocess.PIPE)
for i in idx:
    fr = data[i * F:(i + 1) * F]
    rows = []
    for y in range(64):
        line, gapline = bytearray(), bytearray()
        for x in range(128):
            v = fr[(y * 128 + x) >> 1]
            px, gap = colors[(v >> 4) if x & 1 else v & 15]
            line += px * (S - 1) + gap
            gapline += gap * S
        rows.append(bytes(line) * (S - 1) + bytes(gapline))
    enc.stdin.write(b''.join(rows))
enc.stdin.close()
enc.wait()
print('%d frames at %g fps -> %s' % (len(idx), fps, a.out))

if a.gif:
    subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', a.out, '-vf',
                    'scale=%d:%d:flags=neighbor,split[a][b];[a]palettegen=max_colors=32[p];[b][p]paletteuse=dither=none'
                    % (W, H), a.gif], check=True)
    print('->', a.gif)
