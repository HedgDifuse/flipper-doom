#!/bin/bash
# Build the engine image for the flash gap: port/flipper/out/engine.bin
set -e
cd "$(dirname "$0")"
source ../defs.sh
T=~/.ufbt/toolchain/current/bin
OUT=out
mkdir -p $OUT/obj
CPU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
# globals through the GOT, GOT base in r9 (set by the loader)
PIC="-fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative"
CFLAGS="$CPU $PIC -Os -flto -std=gnu11 -ffunction-sections -fdata-sections -fno-common -w -funsigned-char -ffreestanding -fno-builtin-printf -fno-builtin-malloc -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions"
SRC="$(ls ../engine/*.c) ../platform/i_fz_video.c ../platform/fz_lumcolormap.c ../platform/fz_dither.c ../platform/fz_hud.c i_fz_flipper.c"
rm -f $OUT/obj/*.o
for f in $SRC; do
    $T/arm-none-eabi-gcc.exe -c $CFLAGS "${DOOM_DEFS[@]}" -I. -I../platform/compat -I../platform -I../engine $f -o $OUT/obj/$(basename $f .c).o
done
$T/arm-none-eabi-gcc.exe $CPU -nostartfiles -nostdlib -static -flto -Os -Wl,--gc-sections -Wl,--emit-relocs \
    -Wl,-T,blob.ld -Wl,-Map,$OUT/engine.map $OUT/obj/*.o -lc -lgcc -o $OUT/engine.elf
$T/arm-none-eabi-objcopy.exe -O binary $OUT/engine.elf $OUT/engine.flash
python ../tools/mkblob.py $OUT/engine.elf $OUT/engine.flash $OUT/engine.bin
