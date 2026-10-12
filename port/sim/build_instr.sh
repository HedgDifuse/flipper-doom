#!/bin/bash
# Research build: simulator with function-entry tracing (see fz_instr.c).
# Usage: ./build_instr.sh  ->  fzsim_instr
set -e
cd "$(dirname "$0")"
source ../defs.sh
ENGINE=$(ls ../engine/*.c)
gcc -m32 -std=gnu11 -O1 -g -w -funsigned-char -fno-inline -fno-optimize-sibling-calls \
    -finstrument-functions -finstrument-functions-exclude-file-list=fz_instr.c \
    "${DOOM_DEFS[@]}" -DRANGECHECK \
    -Icompat -I../platform/compat -I../platform -I../engine \
    $ENGINE ../platform/i_fz_video.c ../platform/fz_lumcolormap.c ../platform/fz_dither.c ../platform/fz_hud.c \
    i_fz_sim.c fz_instr.c -o fzsim_instr -lm
echo "built fzsim_instr"
