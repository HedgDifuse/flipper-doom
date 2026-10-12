# Gray levels on the Flipper Zero's 1-bit LCD

The Flipper Zero's screen is a 128x64 monochrome STN LCD driven by an ST7567
controller. Each pixel is either on or off. This document describes how this
project shows up to 9 stable gray levels on it, fast enough for a game, on the
stock firmware. The technique is not specific to Doom: any app that can draw
on the canvas can use it.

The description in this document is dedicated to the public domain (see the end).

## 1. Why a blinking pixel looks gray

STN liquid crystal is slow: a pixel needs tens of milliseconds to turn fully
dark or fully clear. If a pixel is switched on in some subframes and off in
others, faster than the crystal can follow, the eye (and the crystal itself)
averages it to a gray. A pixel that is dark in L of every P subframes shows
level L of P: 0 is white, P is black.

The trick is to make the blinking invisible: the subframes must reach the panel
at a steady rate, the dark pulses must be spread out, and neighbouring pixels
must not blink together.

## 2. Spreading the pulses

For level L of period P, the pixel is dark in subframe t (0 <= t < P) when

    floor((t + 1) * L / P) != floor(t * L / P)

This is Bresenham's line algorithm in time: the L dark subframes are spread as
evenly as possible over the period. Level 4 of 8 is on, off, on, off, ... rather
than four on and four off, so the flicker is at the highest possible frequency.

Each pixel also starts its period at a different phase, taken from its position
in a 4x4 ordered (Bayer) matrix:

    t = (subframe + bayer4[y % 4][x % 4] * P / 16) mod P

So in any subframe, about L/P of the pixels in every 4x4 block are dark, never
all of them at once. A flat gray area does not pulse as a whole: what flicker
is left becomes a fine grain that is close to invisible. This spreading and
phasing comes from zephray's work on gray levels for monochrome LCDs.

`subframe_table()` in [gray_display.c](../port/flipper/fap/gray_display.c)
precomputes, for each subframe, a table `on[y % 4][x % 4][level]`. The frame
the app draws holds a darkness level 0..P per pixel (4 bits, two pixels a byte).

## 3. Matching the controller's frame rate

This is the part that makes it work or not.

The ST7567 copies its display RAM to the glass at the frame rate of its own
internal oscillator, typically 70-80 Hz. That rate cannot be set and there is no
signal to synchronize with. If the app sends subframes at a different rate, the
panel shows some subframes twice and skips others. Over every cycle of the
difference between the two rates, each pixel's dark fraction drifts away from
L/P and back, and the grays slowly pulse or roll across the screen.

So the subframe rate must equal the controller's frame rate, closely:

- **Calibration in 0.1 Hz steps.** The user adjusts the rate until a test
  gradient stands still. At the right rate the drift period becomes many
  seconds or minutes, and the pulsing disappears. On the test unit the rate is
  exactly 75.0 Hz, whatever the number of levels.
- **Cycle-exact pacing.** A high-priority thread builds the next subframe first
  (its build time varies), then sleeps until about 1 ms before the deadline,
  then busy-waits on the CPU cycle counter and sends the subframe at the exact
  moment. Deadlines advance by a fixed step (no accumulated error); if the
  thread falls far behind (more than 50 ms), it resynchronizes instead of
  sending a burst.
- **Lateness is measured.** The thread records how late each subframe reached
  the panel, which is how the problems below were found.

## 4. Sharing the bus with the SD card

On the Flipper the SD card and the LCD share one SPI bus. While the SD card
holds the bus, the subframe waits, arrives late, and a late subframe shows as a
brief change of gray (flicker). Two measures keep this rare:

- **Short SD transfers.** Every SD read is split into pieces of at most 512
  bytes (about 1 ms on the bus), so a subframe never waits longer than that.
- **Black and white while the bus is busy.** Screens shown during long SD or
  flash work (loading a level, installing to flash) use only levels 0 and P:
  every subframe is then identical, so a late one changes nothing. Gradients on
  these screens are ordered-dithered in space instead.

## 5. Calibration

The panel's response depends on the unit, its contrast setting and the
temperature, so three values are calibrated once and saved
([calibration.c](../port/flipper/fap/calibration.c)):

1. **Subframe rate**, in 0.1 Hz steps (section 3).
2. **Number of levels P** (3, 4, 6 or 8): as many as stay distinguishable on
   that panel. More levels mean a longer period, so a lower blink frequency.
3. **Gamma**: how darkness maps to the fraction of dark subframes, so that a
   gradient looks even.

## 6. From image brightness to levels

An image brightness `lum` (0 = black, 63 = white) becomes a darkness level with
a fractional part ([fz_graymap.h](../port/platform/fz_graymap.h)):

    darkness = (1 - lum / 63) ^ gamma
    v        = round(darkness * P * 16)       // level * 16 + fraction
    level    = v / 16 + (v % 16 > bayer4[y % 4][x % 4])

The fraction is spread over space with the same 4x4 ordered dither, between
neighbouring levels only, so gradients run smoothly between the P + 1 real
levels; the pattern is fine enough to disappear behind the temporal blink.

## 7. Making a small game picture readable

At 128x64 with a few grays, a 3D view turns to mush unless the renderer helps.
The engine marks every pixel with what it drew there (wall, floor/ceiling,
sprite), and [fz_dither.c](../port/platform/fz_dither.c) treats each class
differently:

- **Walls and floors**: the calibrated tone above; floors a little lighter.
- **Geometry edges**: where a wall meets a floor, a ceiling or the sky, the
  pixel is drawn black. The room's shape stays visible even when the tones
  are close.
- **Sprites** (monsters, items, the weapon): local contrast is boosted (the
  difference from the 3x3 neighbourhood mean, times 2.2), the sprite's outermost
  pixels are black, and a one-pixel white halo separates it from the background.

Error diffusion (Floyd-Steinberg) was tried and rejected: it changes about 45%
of the pixels from one frame to the next, which looks like boiling noise on a
moving picture. The ordered dither stays put.

## 8. Doing it fast enough

At 75 subframes a second the thread has 13 ms per subframe, and the game needs
most of the CPU. On the 64 MHz Cortex-M4:

- The canvas buffer uses the u8g2 page layout (8-row pages of 128 bytes); this
  is checked once, and the subframe is written straight into the buffer.
- For each subframe a table maps one frame byte (two pixels) to their two
  output bits, so the frame walk is one lookup per two pixels.
- Measured: about 2.5 ms to build a subframe and 2.6 ms to send it to the
  panel, so the gray display costs about 40% of the CPU at 75 Hz. The game
  renders into the 4-bit frame at its own pace, independently.

## 9. Limits

- **Every screen is different.** The best rate, number of levels and gamma vary
  from unit to unit, which is why there is a calibration. The same image can
  look lighter, darker or more contrasted on another Flipper.
- **Screenshots lie.** qFlipper and other screen captures grab single
  subframes, so they show noise instead of grays. To show what the eye sees,
  average the subframes, as [lcdvideo.py](../port/tools/lcdvideo.py) does for
  the simulator.
- **The controller is never truly synchronized.** A very small rate mismatch
  still drifts, just very slowly; temperature can shift the oscillator.

## Public domain dedication

To the extent possible under law, the authors of this document have waived all
copyright and related rights to it and to the technique it describes, under the
[CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/)
dedication. Use it, implement it, change it, in any project, commercial or not,
with or without credit.

This covers this document only. The source code in this repository remains under
its own license (GPL-2.0, see [LICENSE](../LICENSE)); anyone can write their own
implementation from this description without being bound by it.
