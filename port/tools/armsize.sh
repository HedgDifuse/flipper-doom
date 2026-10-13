#!/bin/bash
# Copyright (C) 2026 HedgDifuse
# SPDX-License-Identifier: GPL-2.0-or-later
# Compile engine + platform glue for Cortex-M4 and report flash/RAM use.
# Unresolved platform symbols are ignored; this is only for the budget.
set -e
cd "$(dirname "$0")/.."
source defs.sh
T=~/.ufbt/toolchain/current/bin
OUT=$(mktemp -d)
FL="-Os -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -ffunction-sections -fdata-sections -w -funsigned-char"
for f in engine/*.c platform/i_fz_video.c platform/fz_lumcolormap.c; do
    $T/arm-none-eabi-gcc.exe -c $FL "${DOOM_DEFS[@]}" -Iplatform/compat -Iplatform -Iengine $f -o $OUT/$(basename $f .c).o
done
$T/arm-none-eabi-gcc.exe -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -nostartfiles -nostdlib \
    -Wl,--gc-sections -Wl,-e,D_DoomMain -Wl,--unresolved-symbols=ignore-all $OUT/*.o -o $OUT/doom.elf
$T/arm-none-eabi-size.exe -A $OUT/doom.elf | awk '/^\.(text|rodata|data|bss)/ {printf "%-8s %7d\n", $1, $2; if ($1==".text"||$1==".rodata"||$1==".data") fl+=$2; if ($1==".data"||$1==".bss") ram+=$2} END {printf "FLASH    %7d\nRAM      %7d\n", fl, ram}'
if [ -n "$TOP" ]; then $T/arm-none-eabi-nm.exe -S --size-sort -r $OUT/doom.elf | grep -iE " [bBdD] " | head -$TOP; fi
rm -rf $OUT
