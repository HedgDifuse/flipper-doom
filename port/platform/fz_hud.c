/*
 * flipper-doom: HUD in the 8 columns right of the 3D view (x 120..127).
 *
 * The original status bar does not fit, so health and the ammo of the ready
 * weapon are drawn as 3x5 digits stacked top to bottom, each under a 5x5 icon.
 */

#include <stdint.h>

#include "d_items.h"
#include "d_player.h"
#include "globdata.h"

#include "fz_dither.h"
#include "fz_hud.h"
#include "fz_platform.h"


#define HUD_X   FZ_W           // first HUD column, the separator line
#define DIGIT_X (HUD_X + 3)    // 3 columns wide
#define ICON_X  (HUD_X + 2)    // 5 columns wide

// rows of 3 bits, MSB = left
static const uint8_t digits[11][5] =
{
	{7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
	{7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
	{0, 0, 7, 0, 0}, // '-'
};
#define DIGIT_DASH 10

// rows of 5 bits, MSB = left
static const uint8_t icon_health[5] = {0x0a, 0x1f, 0x1f, 0x0e, 0x04};
static const uint8_t icon_ammo[5]   = {0x04, 0x0e, 0x0e, 0x0e, 0x0e};


// The HUD is composed here first and copied once, so the display thread (which
// reads the frame while it is written) never sees it half drawn.
static uint8_t hud[FZ_SCREEN_H]; // bit x - HUD_X of row y = dark

static void put(uint8_t* out, int16_t x, int16_t y, uint8_t dark)
{
	(void)out;
	uint8_t m = 1 << (x - HUD_X);
	if (dark)
		hud[y] |= m;
	else
		hud[y] &= ~m;
}


static void glyph(uint8_t* out, int16_t x, int16_t y, const uint8_t* rows, int16_t width)
{
	for (int16_t r = 0; r < 5; r++)
		for (int16_t c = 0; c < width; c++)
			put(out, x + c, y + r, (rows[r] >> (width - 1 - c)) & 1);
}


// value < 0 draws "--"; returns the next free row
static int16_t number(uint8_t* out, int16_t y, int16_t value)
{
	if (value < 0)
	{
		glyph(out, DIGIT_X, y, digits[DIGIT_DASH], 3);
		glyph(out, DIGIT_X, y + 6, digits[DIGIT_DASH], 3);
		return y + 12;
	}

	if (value > 999)
		value = 999;
	int16_t div = value >= 100 ? 100 : value >= 10 ? 10 : 1;
	for (; div; div /= 10)
	{
		glyph(out, DIGIT_X, y, digits[(value / div) % 10], 3);
		y += 6;
	}
	return y;
}


void fz_hud_draw(uint8_t* out, uint8_t darkest)
{
	for (int16_t y = 0; y < FZ_SCREEN_H; y++)
		hud[y] = 1; // the separator line in the first HUD column

	const player_t* p = &_g_player;
	glyph(out, ICON_X, 1, icon_health, 5);
	number(out, 8, p->health > 0 ? p->health : 0);

	const ammotype_t ammo = weaponinfo[p->readyweapon].ammo;
	glyph(out, ICON_X, 30, icon_ammo, 5);
	number(out, 37, ammo == am_noammo ? -1 : p->ammo[ammo]);

	for (int16_t y = 0; y < FZ_SCREEN_H; y++)
		for (int16_t x = HUD_X; x < FZ_SCREEN_W; x++)
			fz_gray_put(out, x, y, hud[y] >> (x - HUD_X) & 1 ? darkest : 0);
}
