// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * flipper-doom: renderer-aware conversion to the LCD gray frame.
 *
 * The Flipper LCD is 1-bit, but its pixels are slow: blinking one fast enough
 * shows a gray (see the loader's display thread, which flashes subframes). So
 * the frame here is a darkness level 0..period per pixel, with a 4x4 ordered
 * dither between neighbouring levels. The mapping from Doom luminance to level
 * (lum_to_level) comes from the per-device screen calibration.
 *
 * Every pixel also tells what the renderer drew there, and each class gets its
 * own treatment so the small picture stays readable:
 *   walls/flats  calibrated tone, flats a little lighter
 *   edges        darkest line where walls meet floors, ceilings or sky
 *   sprites      boosted local contrast, darkest rim, lightest 1px halo
 *
 * Runs once per Doom frame over 120x64 pixels, so it is written for speed:
 * row pointers instead of per-neighbour bound checks, a sliding 3x3 sum for
 * the sprites' local mean, and two pixels stored per byte.
 */

#include <stdint.h>
#include <string.h>

#include "fz_dither.h"
#include "fz_platform.h"


// 4x4 Bayer thresholds 0..15
static const uint8_t bayer4[4][4] =
{
	{ 0,  8,  2, 10},
	{12,  4, 14,  6},
	{ 3, 11,  1,  9},
	{15,  7, 13,  5},
};

#define FLAT_LIFT 10 // flats drawn this much brighter (of 63)

#define CLS(p)   ((p) >> FZ_CLASS_SHIFT)
#define LUM(p)   ((p) & FZ_LUM_MASK)
#define SPRITE   FZ_CLASS_SPRITE
#define WALL     FZ_CLASS_WALL


void fz_gray_put(uint8_t* out, int16_t x, int16_t y, uint8_t level)
{
	uint8_t* b = &out[(y * FZ_SCREEN_W + x) >> 1];
	if (x & 1)
		*b = (*b & 0x0f) | (level << 4);
	else
		*b = (*b & 0xf0) | level;
}


// one blank row stands in for the rows above the first and below the last
static const uint8_t none_row[FZ_W];

__attribute__((optimize("O2")))
void fz_dither(const uint8_t* src, uint8_t* out, const fz_gray_t* g, boolean in_game)
{
	// No clearing first: the display thread reads this frame while it is being
	// written, and a cleared (light) frame showing through washed the grays out,
	// the more the faster Doom ran. Every pixel is overwritten in place.
	if (!in_game) // no HUD then: blank its columns
		for (int16_t y = 0; y < FZ_SCREEN_H; y++)
			memset(out + y * (FZ_SCREEN_W / 2) + FZ_W / 2, 0, (FZ_SCREEN_W - FZ_W) / 2);

	const uint8_t* lut = g->lum_to_level;
	const uint8_t period = g->period;

	// 3x3 luminance sums for the sprites' local mean: colsum[x] holds the sum
	// of rows y-1..y+1 in column x (out-of-frame rows count as their edge row)
	static uint16_t colsum[FZ_W];
	for (int16_t x = 0; x < FZ_W; x++)
		colsum[x] = 2 * LUM(src[x]) + LUM(src[FZ_W + x]);

	for (int16_t y = 0; y < FZ_H; y++)
	{
		const uint8_t* row  = src + y * FZ_W;
		const uint8_t* up   = y > 0 ? row - FZ_W : none_row;
		const uint8_t* down = y < FZ_H - 1 ? row + FZ_W : none_row;
		const uint8_t* bay  = bayer4[y & 3];
		uint8_t* o = out + y * (FZ_SCREEN_W / 2);

		if (y > 0)
		{
			// slide the 3-row window down by one row
			const uint8_t* gone = y > 1 ? row - 2 * FZ_W : row - FZ_W;
			const uint8_t* come = y < FZ_H - 1 ? row + FZ_W : row;
			for (int16_t x = 0; x < FZ_W; x++)
				colsum[x] += LUM(come[x]) - LUM(gone[x]);
		}

		uint8_t pair = 0;
		for (int16_t x = 0; x < FZ_W; x++)
		{
			const uint8_t px  = row[x];
			const uint8_t cls = CLS(px);
			int16_t lum = LUM(px);
			uint8_t level;

			if (!in_game)
				goto tone;

			const uint8_t left  = x > 0 ? CLS(row[x - 1]) : FZ_CLASS_NONE;
			const uint8_t right = x < FZ_W - 1 ? CLS(row[x + 1]) : FZ_CLASS_NONE;
			const uint8_t above = CLS(up[x]);
			const uint8_t below = CLS(down[x]);

			if (cls == SPRITE)
			{
				if (left != SPRITE || right != SPRITE || above != SPRITE || below != SPRITE)
				{
					level = period; // rim: sprite pixels that touch something else
					goto store;
				}
				const int16_t l = x > 0 ? x - 1 : 0, r = x < FZ_W - 1 ? x + 1 : x;
				const int16_t mean = (colsum[l] + colsum[x] + colsum[r]) / 9;
				lum += (lum - mean) * 6 / 5; // boosted local contrast
				if (lum < 0)
					lum = 0;
			}
			else if (left == SPRITE || right == SPRITE || above == SPRITE || below == SPRITE)
			{
				level = 0; // halo: a light gap around sprites
				goto store;
			}
			else if (cls == WALL && ((y < FZ_H - 1 && below != WALL) || (y > 0 && above != WALL)))
			{
				level = period; // geometry edge: wall next to a floor, ceiling or sky
				goto store;
			}
			else if (cls == FZ_CLASS_FLAT)
			{
				lum += FLAT_LIFT;
			}

		tone:
			{
				const uint8_t v = lut[lum > 63 ? 63 : lum];
				level = (v >> 4) + ((v & 15) > bay[x & 3]);
				if (level > period)
					level = period;
			}

		store:
			if (x & 1)
				o[x >> 1] = pair | (level << 4);
			else
				pair = level;
		}
	}
}
