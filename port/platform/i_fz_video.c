/*
 * flipper-doom: video backend for Doom8088.
 *
 * Renders luminance + pixel class (see fz_platform.h) into a linear
 * FZ_W x FZ_H buffer that is exactly the 3D view; full-screen 2D graphics
 * are scaled down into it. The platform turns the buffer into pixels in
 * fz_present(): an image dump in the simulator, the 1-bit LCD on Flipper.
 *
 * Based on i_vcgabw.c by Frenkel Smeijers (GPL-2.0-or-later).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"

#include "i_system.h"
#include "i_video.h"
#include "m_random.h"
#include "r_defs.h"
#include "v_video.h"
#include "w_wad.h"

#include "globdata.h"
#include "r_main.h"
#include "st_stuff.h"
#include "z_zone.h"

#include "fz_platform.h"


extern const int16_t CENTERY;

uint8_t* fz_screen;
static int8_t fz_palette;


void I_ReloadPalette(void)
{
	// gamma correction is done in fz_present()
}


void I_InitGraphicsHardwareSpecificCode(void)
{
	fz_screen = Z_MallocStatic(FZ_W * FZ_H);
	memset(fz_screen, 0, FZ_W * FZ_H);
	fz_init_video();
}


void I_ShutdownGraphics(void)
{
}


void I_SetPalette(int8_t pal)
{
	fz_palette = pal;
}


void I_FinishUpdate(void)
{
	fz_present(fz_screen, fz_palette);
}


#define COLEXTRABITS (8 - 1)
#define COLBITS (8 + 1)

static void R_DrawColumnClass(const draw_column_vars_t *dcvars, uint8_t cls)
{
	int16_t count = (dcvars->yh - dcvars->yl) + 1;

	// Zero length, column does not exceed a pixel.
	if (count <= 0)
		return;

	const uint8_t __far* source = dcvars->source;
	const uint8_t* colormap     = dcvars->colormap;
	uint8_t* dest               = fz_screen + (dcvars->yl * FZ_W) + dcvars->x;
	const uint8_t bits          = cls << FZ_CLASS_SHIFT;

	const uint16_t fracstep = dcvars->fracstep;
	uint16_t frac = (dcvars->texturemid >> COLEXTRABITS) + (dcvars->yl - CENTERY) * fracstep;

	while (count--)
	{
		*dest = colormap[source[frac >> COLBITS]] | bits;
		dest += FZ_W;
		frac += fracstep;
	}
}


void R_DrawColumnSprite(const draw_column_vars_t *dcvars)
{
	R_DrawColumnClass(dcvars, FZ_CLASS_SPRITE);
}


void R_DrawColumnWall(const draw_column_vars_t *dcvars)
{
	R_DrawColumnClass(dcvars, FZ_CLASS_WALL);
}


// FZTX walls store every 2nd row as 4-bit codes into 16 palette grays: the
// 7-bit row index r is halved, texel r / 2 sits in byte r / 4, the even ones
// in the low nibble.
void R_DrawColumnWallPacked(const draw_column_vars_t *dcvars, const uint8_t* grays)
{
	int16_t count = (dcvars->yh - dcvars->yl) + 1;
	if (count <= 0)
		return;

	// the 16 codes lit by this column's colormap
	const uint8_t* colormap = dcvars->colormap;
	const uint8_t bits      = FZ_CLASS_WALL << FZ_CLASS_SHIFT;
	uint8_t lit[16];
	for (int16_t c = 0; c < 16; c++)
		lit[c] = colormap[grays[c]] | bits;

	const uint8_t __far* source = dcvars->source;
	uint8_t* dest               = fz_screen + (dcvars->yl * FZ_W) + dcvars->x;

	const uint16_t fracstep = dcvars->fracstep;
	uint16_t frac = (dcvars->texturemid >> COLEXTRABITS) + (dcvars->yl - CENTERY) * fracstep;

	while (count--)
	{
		const uint16_t half = frac >> (COLBITS + 1);
		*dest = lit[(source[half >> 1] >> ((half & 1) << 2)) & 15];
		dest += FZ_W;
		frac += fracstep;
	}
}


void R_DrawColumnSky(const draw_column_vars_t *dcvars)
{
	R_DrawColumnClass(dcvars, FZ_CLASS_NONE);
}


// A wall column whose texture did not fit in the zone: the engine used to draw
// it in "color" = texture number, which here read as a random luminance with
// the flat class (so edged in black). Draw a plain mid gray under the
// column's light instead, as a wall.
#define MISSING_TEXTURE_GRAY 100 // palette index of a mid gray
uint32_t fz_missing_columns;

void R_DrawColumnMissing(int16_t texture, const draw_column_vars_t *dcvars)
{
	(void)texture;
	int16_t count = (dcvars->yh - dcvars->yl) + 1;
	if (count <= 0)
		return;

	fz_missing_columns++;
	const uint8_t pixel = FZ_PIXEL(dcvars->colormap[MISSING_TEXTURE_GRAY] & FZ_LUM_MASK, FZ_CLASS_WALL);
	uint8_t* d = fz_screen + (dcvars->yl * FZ_W) + dcvars->x;
	while (count--)
	{
		*d = pixel;
		d += FZ_W;
	}
}


// color is a luminance already (from the colormap), or the sky color
void R_DrawColumnFlat(uint8_t color, const draw_column_vars_t *dcvars)
{
	int16_t count = (dcvars->yh - dcvars->yl) + 1;
	if (count <= 0)
		return;

	const uint8_t pixel = FZ_PIXEL(color & FZ_LUM_MASK, FZ_CLASS_FLAT);
	uint8_t* d = fz_screen + (dcvars->yl * FZ_W) + dcvars->x;
	while (count--)
	{
		*d = pixel;
		d += FZ_W;
	}
}


#define FUZZOFF (FZ_W)
#define FUZZTABLE 50

static const int8_t fuzzoffset[FUZZTABLE] =
{
	FUZZOFF,-FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
	FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
	FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,
	FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
	FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,
	FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,
	FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF
};


void R_DrawFuzzColumn(const draw_column_vars_t *dcvars)
{
	int16_t dc_yl = dcvars->yl;
	int16_t dc_yh = dcvars->yh;

	if (dc_yl <= 0)
		dc_yl = 1;
	if (dc_yh >= VIEWWINDOWHEIGHT - 1)
		dc_yh = VIEWWINDOWHEIGHT - 2;

	int16_t count = (dc_yh - dc_yl) + 1;
	if (count <= 0)
		return;

	uint8_t* d = fz_screen + (dc_yl * FZ_W) + dcvars->x;

	// spectres: darken what is behind (colormap 6 of 32)
	static int16_t fuzzpos = 0;
	do
	{
		*d = FZ_PIXEL((d[fuzzoffset[fuzzpos]] & FZ_LUM_MASK) * 26 / 32, FZ_CLASS_SPRITE);
		d += FZ_W;

		if (++fuzzpos >= FUZZTABLE)
			fuzzpos = 0;
	} while (--count);
}


void V_ClearViewWindow(void)
{
	memset(fz_screen, 0, FZ_W * FZ_H);
}


void V_InitDrawLine(void)
{
}


void V_ShutdownDrawLine(void)
{
}


// Raw screens are placed in SCREENWIDTH x SCREENHEIGHT space.
#define TO_FZ_Y(y) ((int16_t)(((int32_t)(y) * FZ_H) / SCREENHEIGHT))


void V_DrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color)
{
	// automap is built with MAPWIDTH == FZ_W and draws in view coordinates
	int16_t dx = abs(x1 - x0);
	int16_t sx = x0 < x1 ? 1 : -1;
	int16_t dy = -abs(y1 - y0);
	int16_t sy = y0 < y1 ? 1 : -1;
	int16_t err = dx + dy;

	while (true)
	{
		if ((uint16_t)x0 < FZ_W && (uint16_t)y0 < FZ_H)
			fz_screen[y0 * FZ_W + x0] = fullcolormap[color];

		if (x0 == x1 && y0 == y1)
			break;

		int16_t e2 = 2 * err;
		if (e2 >= dy)
		{
			err += dy;
			x0  += sx;
		}
		if (e2 <= dx)
		{
			err += dx;
			y0  += sy;
		}
	}
}


void V_DrawBackground(int16_t backgroundnum)
{
	const byte __far* src = W_GetLumpByNum(backgroundnum);

	for (int16_t y = 0; y < FZ_H; y++)
		for (int16_t x = 0; x < FZ_W; x++)
			fz_screen[y * FZ_W + x] = fullcolormap[src[((y * SCREENHEIGHT / FZ_H) & 63) * 64 + ((x * SCREENWIDTH / FZ_W) & 63)]];

	Z_ChangeTagToCache(src);
}


void V_DrawRaw(int16_t num, uint16_t offset)
{
	const uint8_t __far* lump = W_TryGetLumpByNum(num);
	if (lump == NULL)
		return;

	// raw lumps are SCREENWIDTH wide, placed at a row of the 240x160 screen
	uint16_t height = W_LumpLength(num) / SCREENWIDTH;
	int16_t y0 = offset / SCREENWIDTH;
	for (int16_t y = TO_FZ_Y(y0); y < FZ_H && y < TO_FZ_Y(y0 + height); y++)
	{
		const uint8_t __far* row = lump + (y * SCREENHEIGHT / FZ_H - y0) * SCREENWIDTH;
		for (int16_t x = 0; x < FZ_W; x++)
			fz_screen[y * FZ_W + x] = fullcolormap[row[x * SCREENWIDTH / FZ_W]];
	}

	Z_ChangeTagToCache(lump);
}


void ST_Drawer(void)
{
	// The original status bar does not fit; the HUD is drawn by fz_present().
}


static void V_DrawPatchColumn(int16_t x, int16_t y, const column_t __far* column, int32_t dy, int16_t dyi)
{
	while (column->topdelta != 0xff)
	{
		int16_t yl = ((int32_t)(y + column->topdelta) * dy) >> FRACBITS;
		int16_t yh = ((int32_t)(y + column->topdelta + column->length) * dy) >> FRACBITS;
		const byte __far* src = (const byte __far*)column + 3;

		int16_t frac = 0;
		for (int16_t yy = yl; yy < yh; yy++, frac += dyi)
			if ((uint16_t)yy < FZ_H)
				fz_screen[yy * FZ_W + x] = fullcolormap[src[frac >> 8]];

		column = (const column_t __far*)((const byte __far*)column + column->length + 4);
	}
}


static void V_DrawPatch(int16_t x, int16_t y, const patch_t __far* patch, int16_t srcw, int16_t srch)
{
	// srcw x srch is the coordinate space of x, y (240x160 or 320x200)
	const int32_t DX  = ((int32_t)FZ_W << FRACBITS) / srcw;
	const int16_t DXI = (((int32_t)srcw << FRACBITS) / FZ_W) >> 8;
	const int32_t DY  = (((int32_t)FZ_H << FRACBITS) + (FRACUNIT - 1)) / srch;
	const int16_t DYI = (((int32_t)srch << FRACBITS) / FZ_H) >> 8;

	y -= patch->topoffset;
	x -= patch->leftoffset;

	const int16_t left  = ((int32_t)x * DX) >> FRACBITS;
	const int16_t right = ((int32_t)(x + patch->width) * DX) >> FRACBITS;

	uint16_t col = 0;
	for (int16_t dc_x = left; dc_x < right; dc_x++, col += DXI)
	{
		if (dc_x < 0)
			continue;
		if (dc_x >= FZ_W)
			break;

		const column_t __far* column = (const column_t __far*)((const byte __far*)patch + (uint16_t)patch->columnofs[col >> 8]);
		V_DrawPatchColumn(dc_x, y, column, DY, DYI);
	}
}


void V_DrawPatchNotScaled(int16_t x, int16_t y, const patch_t __far* patch)
{
	V_DrawPatch(x, y, patch, SCREENWIDTH, SCREENHEIGHT);
}


void V_DrawPatchScaled(int16_t x, int16_t y, const patch_t __far* patch)
{
	V_DrawPatch(x, y, patch, SCREENWIDTH_VGA, SCREENHEIGHT_VGA);
}


// No screen melt: it needs a second full-screen buffer.
void wipe_StartScreen(void)
{
}


void D_Wipe(void)
{
}
