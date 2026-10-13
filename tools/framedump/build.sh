#!/bin/bash
# Copyright (C) 2026 HedgDifuse
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the headless frame dumper (Linux/WSL) and record a demo.
# Usage: ./build.sh [demoN]
set -e
cd "$(dirname "$0")"
DG=../../ref/doomgeneric/doomgeneric
SRC=$(grep "^SRC_DOOM" $DG/Makefile | tr -d '\r' | sed 's/SRC_DOOM = //; s/doomgeneric_xlib.o//; s/\.o/.c/g')
SRC=$(for f in $SRC; do echo $DG/$f; done)
gcc -std=gnu99 -O2 -w -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE \
    -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -I$DG $SRC doomgeneric_dump.c -o dgdump -lm
rm -f frames.rgb frames.ids
./dgdump -iwad ../../wad/doom1.wad -playdemo ${1:-demo1} -nosound -nomusic > run.log 2>&1 || true
echo "frames: $(( $(stat -c %s frames.rgb) / 192000 ))"
