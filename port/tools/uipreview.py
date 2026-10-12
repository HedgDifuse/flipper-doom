"""Preview of the loader's screens as the LCD shows them (gray levels as a
picture), composed the way ui.c does it.

  python port/tools/uipreview.py port/flipper/fap menubg.bin out_dir [period]
"""
import os, re, struct, sys
from PIL import Image

fap, bgfile, outdir = sys.argv[1:4]
PERIOD = int(sys.argv[4]) if len(sys.argv) > 4 else 4
h = open(os.path.join(fap, 'ui_assets.h')).read()
D = {k: int(v) for k, v in re.findall(r'#define (\w+) (\d+)', h)}
def arr(name):
    m = re.search(name + r'\[\d+\] = \{([^}]*)\}', h)
    return [int(x, 16) for x in m.group(1).replace('\n', '').split(',') if x.strip()]
font_w, font, skull = arr('ui_font_width'), arr('ui_font'), arr('ui_skull')
uibin = open(os.path.join(fap, 'files', 'ui.bin'), 'rb').read()
bg = open(bgfile, 'rb').read()

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
WASH = 0.6  # the background's darkest is this bright


class Screen:
    def __init__(self, bgframe=None):
        self.b = [[1.0] * 128 for _ in range(64)]  # brightness
        self.solid = [[False] * 128 for _ in range(64)]
        if bgframe is not None:
            f = bg[16 + bgframe * 4096: 16 + (bgframe + 1) * 4096]
            for y in range(64):
                for x in range(128):
                    v = f[(y * 128 + x) >> 1]
                    d = (v >> 4) if x & 1 else v & 15
                    self.b[y][x] = WASH + (1 - WASH) * (1 - d / 15)

    def image(self, data, w, hh, x0, y0):
        bpr = (w + 1) // 2
        for y in range(hh):
            for x in range(w):
                n = (data[y * bpr + (x >> 1)] >> ((x & 1) * 4)) & 15
                if n and 0 <= x0 + x < 128 and 0 <= y0 + y < 64:
                    self.b[y0 + y][x0 + x] = (n - 1) / 14
                    self.solid[y0 + y][x0 + x] = False

    def logo(self, key, x0, y0):
        w, hh, pos = D['UI_LOGO_%s_W' % key], D['UI_LOGO_%s_H' % key], D['UI_LOGO_%s_POS' % key]
        self.image(uibin[pos:], w, hh, x0, y0)

    def text_w(self, s):
        return sum(D['UI_FONT_SPACE'] if c == ' ' else font_w[ord(c.upper()) - 33] for c in s)

    def text(self, x, y, s, b=0.0, halo=True):
        pts = []
        for c in s:
            if c == ' ':
                x += D['UI_FONT_SPACE']; continue
            g = ord(c.upper()) - 33
            for r in range(7):
                for k in range(font_w[g]):
                    if font[g * 7 + r] >> k & 1:
                        pts.append((x + k, y + r))
            x += font_w[g]
        s = set(pts)
        if halo:
            for (px, py) in pts:
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        q = (px + dx, py + dy)
                        if q not in s and 0 <= q[0] < 128 and 0 <= q[1] < 64:
                            self.b[q[1]][q[0]] = 1.0; self.solid[q[1]][q[0]] = True
        for (px, py) in pts:
            if 0 <= px < 128 and 0 <= py < 64:
                self.b[py][px] = b; self.solid[py][px] = True
        return x

    def box(self, x, y, w, hh, b):
        for yy in range(y, y + hh):
            for xx in range(x, x + w):
                self.b[yy][xx] = b; self.solid[yy][xx] = True

    def png(self, name):
        im = Image.new('L', (128, 64)); px = im.load()
        for y in range(64):
            for x in range(128):
                dark = (1 - self.b[y][x]) * PERIOD * 16
                lvl = int(dark + 8) >> 4 if self.solid[y][x] else int(dark + BAYER[y & 3][x & 3]) >> 4
                lvl = min(PERIOD, lvl)
                px[x, y] = 255 - lvl * 255 // PERIOD
        im.resize((512, 256), Image.NEAREST).save(os.path.join(outdir, name))


def menu(frame, title, items, sel, logo=True, values=None):
    s = Screen(frame)
    top = 0
    if logo:
        s.logo('SMALL', (128 - D['UI_LOGO_SMALL_W']) // 2, 0)
        top = D['UI_LOGO_SMALL_H'] + 2
    if title:
        s.text((128 - s.text_w(title)) // 2, top + 1, title)
        top += 10
    for i, it in enumerate(items):
        y = top + i * 8
        s.text(11, y, it)
        if values and values[i] is not None:
            # a slider: 16 steps
            x0 = 128 - 4 - 34
            s.box(x0, y + 2, 34, 3, 0.0)
            s.box(x0 + 1, y + 3, 32, 1, 1.0)
            s.box(x0 + 1 + values[i] * 2, y + 1, 2, 5, 0.0)
    s.image(skull, D['UI_SKULL_W'], D['UI_SKULL_H'], 0, top + sel * 8 - 1)
    return s


menu(100, None, ['NEW GAME', 'OPTIONS', 'READ THIS!', 'QUIT GAME'], 0).png('m_main.png')
menu(400, 'CHOOSE SKILL LEVEL:', ["I'M TOO YOUNG TO DIE", 'HEY, NOT TOO ROUGH', 'HURT ME PLENTY',
                                  'ULTRA-VIOLENCE', 'NIGHTMARE!'], 2, logo=False).png('m_skill.png')
menu(700, 'SOUND VOLUME', ['SFX VOLUME', 'MUSIC VOLUME'], 1, logo=False, values=[12, 8]).png('m_sound.png')
s = Screen(900)
for i, l in enumerate(['ARE YOU SURE YOU', 'WANT TO QUIT THIS', 'GREAT GAME?', '', 'OK: QUIT  BACK: NO']):
    s.text((128 - s.text_w(l)) // 2, 12 + i * 9, l)
s.png('m_quit.png')
s = Screen()
s.logo('BIG', 2, 0)
s.text((128 - s.text_w('INSTALLING')) // 2, 50, 'INSTALLING', halo=False)
s.box(14, 59, 100, 5, 0.0); s.box(15, 60, 98, 3, 1.0); s.box(16, 61, 60, 1, 0.0)
s.png('m_install.png')
print('ok')
