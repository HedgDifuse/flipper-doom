// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
// flipper-doom: renderer-aware conversion to the LCD gray frame, see fz_dither.c.
#ifndef FZ_DITHER_H
#define FZ_DITHER_H

#include <stdint.h>
#include "doomtype.h"

#define FZ_SCREEN_W 128
#define FZ_SCREEN_H 64

// Gray frame: 128x64 darkness levels 0..period, 4 bits per pixel, even x in
// the low nibble. The loader flashes it onto the 1-bit LCD as subframes.
#define FZ_GRAY_FRAME_SIZE (FZ_SCREEN_W * FZ_SCREEN_H / 2)

typedef struct
{
	uint8_t period;               // darkest level = number of subframes
	const uint8_t* lum_to_level;  // 64 entries: level * 16 + dither fraction (0..15)
} fz_gray_t;

// src: FZ_W x FZ_H pixels (luminance + class). Columns FZ_W..127 are left at 0.
void fz_dither(const uint8_t* src, uint8_t* out, const fz_gray_t* g, boolean in_game);

void fz_gray_put(uint8_t* out, int16_t x, int16_t y, uint8_t level);

#endif
