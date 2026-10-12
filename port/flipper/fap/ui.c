// flipper-doom: the loader's Doom-style screens, see ui.h.
#include "ui.h"

#include <furi.h>
#include <string.h>

#include "../../platform/fz_graymap.h"
#include "ui_assets.h"

#define BACKGROUND EXT_PATH("apps_data/flipper_doom/MENUBG.BIN")
#define WASH       26 // of 63: the background's darkest, so dark text (with its halo) stays readable

#define SOLID 0x80 // a pixel not to dither (text, boxes)

struct Ui
{
	uint8_t* px;  // 128x64: brightness 0..63 | SOLID
	uint8_t* out; // the gray frame being made (also the background frame read)
	uint8_t lut[64];
	uint8_t period;
	Storage* storage;
	File* background;
	uint32_t bg_frames, bg_next;
	uint8_t logo_small[(UI_LOGO_SMALL_W + 1) / 2 * UI_LOGO_SMALL_H]; // read once
	bool logo_small_loaded;
};

// 4x4 ordered dither: the gradients with only period + 1 levels
static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

typedef char assertScratch[sizeof(struct Ui) + 128 * 64 + 128 * 64 / 2 <= UI_SCRATCH ? 1 : -1];


Ui* ui_open(void* scratch, Storage* storage, const GrayConfig* gray)
{
	Ui* ui = scratch;
	memset(ui, 0, sizeof(*ui));
	ui->px = (uint8_t*)scratch + sizeof(*ui);
	ui->out = ui->px + 128 * 64;
	ui->period = gray->period;
	fz_build_lum_to_level(ui->lut, gray->period, gray->gamma);
	ui->storage = storage;
	return ui;
}


void ui_close(Ui* ui)
{
	if(ui->background) storage_file_free(ui->background);
	ui->background = NULL;
}


static void background(Ui* ui)
{
	if(!ui->background) {
		ui->background = storage_file_alloc(ui->storage);
		uint8_t header[16];
		if(!storage_file_open(ui->background, BACKGROUND, FSAM_READ, FSOM_OPEN_EXISTING) ||
		   storage_file_read(ui->background, header, sizeof(header)) != sizeof(header) ||
		   memcmp(header, "FZBG", 4) != 0)
			ui->bg_frames = 0;
		else
			memcpy(&ui->bg_frames, header + 4, 4);
	}
	if(!ui->bg_frames) {
		memset(ui->px, 63, 128 * 64);
		return;
	}
	if(ui->bg_next >= ui->bg_frames) ui->bg_next = 0;
	if(!storage_file_seek(ui->background, 16 + ui->bg_next * (128 * 64 / 2), true) ||
	   storage_file_read(ui->background, ui->out, 128 * 64 / 2) != 128 * 64 / 2) {
		memset(ui->px, 63, 128 * 64);
		return;
	}
	ui->bg_next++;
	// darkness 0..15 -> brightness WASH..63
	uint8_t wash[16];
	for(int d = 0; d < 16; d++) wash[d] = 63 - d * (63 - WASH) / 15;
	for(int i = 0; i < 128 * 64 / 2; i++) {
		ui->px[2 * i] = wash[ui->out[i] & 15];
		ui->px[2 * i + 1] = wash[ui->out[i] >> 4];
	}
}


void ui_clear(Ui* ui, bool with_background)
{
	if(with_background)
		background(ui);
	else
		memset(ui->px, 63, 128 * 64);
}


static inline void put(Ui* ui, int x, int y, uint8_t value)
{
	if((unsigned)x < 128 && (unsigned)y < 64) ui->px[y * 128 + x] = value;
}


// rows of a 4-bit image: 0 transparent, 1..15 brightness
static void image_row(Ui* ui, const uint8_t* row, int w, int x0, int y)
{
	for(int x = 0; x < w; x++) {
		const uint8_t n = (row[x >> 1] >> ((x & 1) * 4)) & 15;
		if(n) put(ui, x0 + x, y, (n - 1) * 63 / 14);
	}
}


static void image(Ui* ui, const uint8_t* data, int w, int h, int x0, int y0)
{
	for(int y = 0; y < h; y++) image_row(ui, data + y * ((w + 1) / 2), w, x0, y0 + y);
}


int ui_logo_height(UiLogo logo)
{
	return logo == UiLogoBig ? UI_LOGO_BIG_H : UI_LOGO_SMALL_H;
}


static bool read_ui_bin(Ui* ui, uint32_t pos, void* dest, uint16_t bytes)
{
	File* f = storage_file_alloc(ui->storage);
	const bool ok = storage_file_open(f, APP_ASSETS_PATH("ui.bin"), FSAM_READ, FSOM_OPEN_EXISTING) &&
	                storage_file_seek(f, pos, true) && storage_file_read(f, dest, bytes) == bytes;
	storage_file_free(f);
	return ok;
}


void ui_logo(Ui* ui, UiLogo logo, int x, int y)
{
	if(logo == UiLogoSmall) {
		if(!ui->logo_small_loaded)
			ui->logo_small_loaded = read_ui_bin(ui, UI_LOGO_SMALL_POS, ui->logo_small, sizeof(ui->logo_small));
		if(ui->logo_small_loaded) image(ui, ui->logo_small, UI_LOGO_SMALL_W, UI_LOGO_SMALL_H, x, y);
		return;
	}
	// the big one through the out buffer (the splash draws it once)
	const uint16_t bytes = (UI_LOGO_BIG_W + 1) / 2 * UI_LOGO_BIG_H;
	if(read_ui_bin(ui, UI_LOGO_BIG_POS, ui->out, bytes)) image(ui, ui->out, UI_LOGO_BIG_W, UI_LOGO_BIG_H, x, y);
}


static int glyph(char c)
{
	if(c >= 'a' && c <= 'z') c -= 'a' - 'A';
	return c >= UI_FONT_FIRST && c <= UI_FONT_LAST ? c - UI_FONT_FIRST : -1;
}


int ui_text_width(const char* s)
{
	int w = 0;
	for(; *s; s++) w += glyph(*s) < 0 ? UI_FONT_SPACE : ui_font_width[glyph(*s)];
	return w;
}


int ui_text(Ui* ui, int x, int y, const char* s)
{
	// the halo first (white around every letter pixel), then the letters
	int end = x;
	for(int pass = 0; pass < 2; pass++) {
		int cx = x;
		for(const char* c = s; *c; c++) {
			const int g = glyph(*c);
			if(g < 0) {
				cx += UI_FONT_SPACE;
				continue;
			}
			for(int r = 0; r < UI_FONT_H; r++) {
				const uint8_t bits = ui_font[g * UI_FONT_H + r];
				for(int b = 0; b < ui_font_width[g]; b++) {
					if(!(bits >> b & 1)) continue;
					if(pass == 1)
						put(ui, cx + b, y + r, 0 | SOLID);
					else
						for(int dy = -1; dy <= 1; dy++)
							for(int dx = -1; dx <= 1; dx++) put(ui, cx + b + dx, y + r + dy, 63 | SOLID);
				}
			}
			cx += ui_font_width[g];
		}
		end = cx;
	}
	return end;
}


void ui_text_center(Ui* ui, int y, const char* s)
{
	ui_text(ui, (128 - ui_text_width(s)) / 2, y, s);
}


void ui_box(Ui* ui, int x, int y, int w, int h, uint8_t brightness)
{
	for(int yy = y; yy < y + h; yy++)
		for(int xx = x; xx < x + w; xx++) put(ui, xx, yy, (brightness * 63 / 15) | SOLID);
}


void ui_draw_skull(Ui* ui, int x, int y, int frame)
{
	image(ui, ui_skull + (frame & 1) * UI_SKULL_FRAME, UI_SKULL_W, UI_SKULL_H, x, y);
}


void ui_progress(Ui* ui, int x, int y, int w, int permille)
{
	if(permille < 0) permille = 0;
	if(permille > 1000) permille = 1000;
	ui_box(ui, x, y, w, 6, 0);
	ui_box(ui, x + 1, y + 1, w - 2, 4, 15);
	ui_box(ui, x + 2, y + 2, (w - 4) * permille / 1000, 2, 0);
}


void ui_present(Ui* ui, uint8_t* frame, bool mono)
{
	const int period = ui->period;
	if(mono) {
		// every subframe the same: a late one (flash writes, SD reads on the
		// shared bus) cannot make it flicker
		for(int y = 0; y < 64; y++)
			for(int x = 0; x < 128; x += 2) {
				uint8_t two = 0;
				for(int k = 0; k < 2; k++) {
					const uint8_t p = ui->px[y * 128 + x + k];
					const bool black = (p & SOLID) ? (p & 63) < 32 : (p & 63) * 16 / 63 <= bayer[y & 3][(x + k) & 3];
					two |= (black ? period : 0) << (4 * k);
				}
				ui->out[y * 64 + (x >> 1)] = two;
			}
		memcpy(frame, ui->out, 128 * 64 / 2);
		return;
	}
	for(int y = 0; y < 64; y++) {
		const uint8_t* src = ui->px + y * 128;
		uint8_t* dst = ui->out + y * 64;
		for(int x = 0; x < 128; x += 2) {
			uint8_t two = 0;
			for(int k = 0; k < 2; k++) {
				const uint8_t p = src[x + k];
				const int dark = ui->lut[p & 63];
				int v = (dark + ((p & SOLID) ? 8 : bayer[y & 3][(x + k) & 3])) >> 4;
				if(v > period) v = period;
				two |= v << (4 * k);
			}
			dst[x >> 1] = two;
		}
	}
	memcpy(frame, ui->out, 128 * 64 / 2);
}
