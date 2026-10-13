#!/bin/bash
# Copyright (C) 2026 HedgDifuse
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the Doom8088-based simulator (32-bit, WSL/Linux).
# Usage: [EXTRA=-DFZ_MEMTRACE] ./build.sh   (zone size: FZ_ZONE_KB at run time)
set -e
cd "$(dirname "$0")"
source ../defs.sh
ENGINE=$(ls ../engine/*.c)
gcc -m32 -std=gnu11 -O2 -g -w -funsigned-char \
    "${DOOM_DEFS[@]}" -DRANGECHECK $EXTRA \
    -Icompat -I../platform/compat -I../platform -I../engine \
    $ENGINE ../platform/i_fz_video.c ../platform/fz_lumcolormap.c ../platform/fz_dither.c ../platform/fz_hud.c i_fz_sim.c -o fzsim -lm
echo "built fzsim"
