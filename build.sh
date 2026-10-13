#!/bin/bash
# Copyright (C) 2026 HedgDifuse
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds everything the Flipper needs from your own copy of Doom.
#
#   wad/doom1.wad     the shareware (or registered) Doom 1.9 IWAD
#   wad/DOOM8088.WAD  the same IWAD preprocessed by jWadUtil
#                     (https://github.com/FrenkelS/jWadUtil)
#
# Needs: Python 3 with Pillow and numpy, ufbt (pip install ufbt) with its ARM
# toolchain. The music (port/tools/mkmusic.py, libADLMIDI) and the menu's
# background recording (port/sim, see README) are separate steps.
#
# Output:
#   port/flipper/fap/dist/flipper_doom.fap  -> /ext/apps/Games/
#   wad/FZDOOM.WAD                          -> /ext/apps_data/flipper_doom/
set -e
cd "$(dirname "$0")"

for f in wad/doom1.wad wad/DOOM8088.WAD; do
    [ -f "$f" ] || { echo "missing $f (see the top of build.sh)"; exit 1; }
done

echo "== the light tables (palette -> luminance), compiled into the engine"
python port/tools/mklum.py wad/DOOM8088.WAD port/platform/fz_lumcolormap.c

echo "== FZDOOM.WAD: Flipper lumps (walls, sprite frames, sectors, sounds)"
python port/tools/fztex.py wad/DOOM8088.WAD wad/FZDOOM.WAD wad/doom1.wad

echo "== the loader's graphics (logo, font, skull) from doom1.wad"
mkdir -p port/flipper/fap/files
python port/tools/mkui.py wad/doom1.wad port/flipper/fap/ui_assets.h

echo "== the engine image (runs from the internal flash)"
bash port/flipper/build_blob.sh
cp port/flipper/out/engine.bin port/flipper/fap/files/engine.bin

echo "== the app"
(cd port/flipper/fap && python -m ufbt)
ls -l port/flipper/fap/dist/flipper_doom.fap wad/FZDOOM.WAD
