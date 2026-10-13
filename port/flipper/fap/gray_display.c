// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * flipper-doom: gray levels on the Flipper's 1-bit LCD.
 *
 * The STN panel is slow, so a pixel that blinks fast looks gray. A level L of
 * P is shown by lighting the pixel in L of every P subframes, using the
 * zephray.me technique: the L pulses are spread evenly over the period
 * (50% = on/off/on/off rather than on/on/off/off) and each pixel of a 4x4
 * block starts at a different phase, so neighbours do not blink together and
 * the flicker turns into fine, barely visible grain.
 *
 * A thread of its own builds every subframe from the 4-bit frame and pushes it
 * to the panel at the calibrated rate, independently of how fast Doom renders.
 * The ST7567 refreshes the panel from its RAM at its own oscillator's frame
 * rate (70-80 Hz, no way to set or sync it). Unless the subframe rate matches
 * it closely, rows get some subframes twice and miss others, and the grays
 * slowly pulse; hence the 0.1 Hz calibration and the cycle-exact pacing.
 */

#include <furi.h>
#include <furi_hal.h>

#include "gray_display.h"

#define TAG "GrayDisplay"

static const uint8_t bayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

struct GrayDisplay
{
	Gui* gui;
	Canvas* canvas;
	FuriThread* thread;
	FuriMutex* mutex; // guards config and overlay
	uint8_t* frame;
	GrayConfig config;
	GrayOverlay overlay;
	void* overlay_context;
	volatile bool stop;
	volatile uint16_t actual_rate;
	volatile uint32_t build_us, commit_us; // averages over the last second
	volatile uint32_t late_worst_us, late_count;
	bool fast_layout; // canvas buffer is the usual u8g2 layout: 8-row pages of 128 bytes
};


void gray_frame_put(uint8_t* frame, int x, int y, uint8_t level)
{
	uint8_t* b = &frame[(y * 128 + x) >> 1];
	*b = (x & 1) ? (*b & 0x0f) | (level << 4) : (*b & 0xf0) | level;
}


static inline uint8_t frame_level(const uint8_t* frame, int x, int y)
{
	uint8_t b = frame[(y * 128 + x) >> 1];
	return (x & 1) ? b >> 4 : b & 15;
}


// on[row phase][column phase][level] = pixel is dark in subframe sf.
typedef uint8_t SubframeTable[4][4][16];

static void subframe_table(SubframeTable on, uint8_t period, uint8_t sf)
{
	for(int k = 0; k < 16; k++) {
		int t0 = sf + bayer4[k] * period / 16;
		for(int level = 0; level < 16; level++) {
			int l = level > period ? period : level;
			int t = t0 % period;
			on[k >> 2][k & 3][level] = (t + 1) * l / period != t * l / period;
		}
	}
}


// Find out whether the canvas buffer uses the plain u8g2 page layout.
static bool detect_layout(Canvas* canvas)
{
	static const struct { uint8_t x, y; } probes[] = {{5, 9}, {100, 50}, {127, 63}};
	uint8_t* buf = canvas_get_buffer(canvas);
	for(size_t i = 0; i < COUNT_OF(probes); i++) {
		canvas_clear(canvas);
		canvas_set_color(canvas, ColorBlack);
		canvas_draw_dot(canvas, probes[i].x, probes[i].y);
		int expect = (probes[i].y / 8) * 128 + probes[i].x;
		for(int j = 0; j < 1024; j++) {
			uint8_t want = j == expect ? 1 << (probes[i].y & 7) : 0;
			if(buf[j] != want) return false;
		}
	}
	return true;
}


// Per subframe: for each row phase (y & 3) and column pair (x & 2), the two
// dark bits of the two pixels packed in one frame byte. Built once per
// subframe, so the frame walk below is a single lookup per byte.
typedef uint8_t PairTable[4][2][256];

static void pair_table(PairTable pairs, SubframeTable on)
{
	for(int rp = 0; rp < 4; rp++)
		for(int xp = 0; xp < 2; xp++)
			for(int b = 0; b < 256; b++)
				pairs[rp][xp][b] = on[rp][xp * 2][b & 15] | on[rp][xp * 2 + 1][b >> 4] << 1;
}


// Hot: runs for every subframe (~70 times a second).
__attribute__((optimize("O2"))) static void render_subframe(GrayDisplay* d, SubframeTable on, PairTable pairs)
{
	if(d->fast_layout) {
		pair_table(pairs, on);
		uint8_t* buf = canvas_get_buffer(d->canvas);
		for(int page = 0; page < 8; page++) {
			uint8_t* out = buf + page * 128;
			memset(out, 0, 128);
			for(int r = 0; r < 8; r++) {
				const int y = page * 8 + r;
				const uint8_t* row = d->frame + y * 64;
				const uint8_t* p0 = pairs[y & 3][0];
				const uint8_t* p1 = pairs[y & 3][1];
				for(int x = 0; x < 128; x += 4) {
					const uint8_t v0 = p0[row[x >> 1]], v1 = p1[row[(x >> 1) + 1]];
					out[x] |= (v0 & 1) << r;
					out[x + 1] |= (v0 >> 1) << r;
					out[x + 2] |= (v1 & 1) << r;
					out[x + 3] |= (v1 >> 1) << r;
				}
			}
		}
	} else {
		canvas_clear(d->canvas);
		canvas_set_color(d->canvas, ColorBlack);
		for(int y = 0; y < 64; y++)
			for(int x = 0; x < 128; x++)
				if(on[y & 3][x & 3][frame_level(d->frame, x, y)]) canvas_draw_dot(d->canvas, x, y);
	}
}


static int32_t display_thread(void* context)
{
	GrayDisplay* d = context;
	const uint32_t cpu_us = furi_hal_cortex_instructions_per_microsecond();
	uint32_t next = DWT->CYCCNT, window = DWT->CYCCNT, shown = 0, build = 0, commit = 0;
	static SubframeTable on;
	static PairTable pairs; // 2 KB

	while(!d->stop) {
		furi_mutex_acquire(d->mutex, FuriWaitForever);
		GrayConfig config = d->config;
		furi_mutex_release(d->mutex);

		for(uint8_t sf = 0; sf < config.period && !d->stop; sf++) {
			// build first, then send at the exact moment: the build time varies,
			// the moment the LCD gets the subframe must not
			uint32_t t0 = DWT->CYCCNT;
			subframe_table(on, config.period, sf);
			render_subframe(d, on, pairs);
			furi_mutex_acquire(d->mutex, FuriWaitForever);
			if(d->overlay) d->overlay(d->canvas, d->overlay_context);
			furi_mutex_release(d->mutex);
			build += DWT->CYCCNT - t0;

			// sleep until ~1 ms before the deadline, then spin (high priority,
			// so keep the spin short)
			next += (uint32_t)((uint64_t)cpu_us * 10000000U / config.rate10);
			int32_t wait_us = (int32_t)(next - DWT->CYCCNT) / (int32_t)cpu_us;
			if(wait_us > 1500)
				furi_delay_ms((wait_us - 1000) / 1000);
			else if(wait_us < -50000)
				next = DWT->CYCCNT; // fell far behind: resync
			while((int32_t)(next - DWT->CYCCNT) > 0) {
			}

			// a commit that starts late or takes long (SD holding the shared SPI
			// bus) shows the subframe at the wrong time
			uint32_t t1 = DWT->CYCCNT;
			canvas_commit(d->canvas);
			uint32_t took = DWT->CYCCNT - t1;
			commit += took;
			int32_t late_us = (int32_t)(t1 + took - next) / (int32_t)cpu_us - (int32_t)d->commit_us;
			if(d->commit_us && late_us > 1000) d->late_count++;
			if(d->commit_us && late_us > (int32_t)d->late_worst_us) d->late_worst_us = late_us;
			shown++;

			uint32_t elapsed = (DWT->CYCCNT - window) / cpu_us;
			if(elapsed >= 1000000) {
				d->actual_rate = shown * 1000000ULL / elapsed;
				d->build_us = build / shown / cpu_us;
				d->commit_us = commit / shown / cpu_us;
				shown = build = commit = 0;
				window = DWT->CYCCNT;
			}
		}
	}
	return 0;
}


GrayDisplay* gray_display_start(Gui* gui, uint8_t* frame, GrayConfig config)
{
	GrayDisplay* d = malloc(sizeof(GrayDisplay));
	memset(d, 0, sizeof(GrayDisplay));
	d->gui = gui;
	d->frame = frame;
	d->config = config;
	d->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
	d->canvas = gui_direct_draw_acquire(gui);
	d->fast_layout = detect_layout(d->canvas);
	FURI_LOG_I(TAG, "canvas layout: %s", d->fast_layout ? "u8g2 pages" : "unknown, slow path");

	d->thread = furi_thread_alloc_ex("GrayDisplay", 1024, display_thread, d); // text overlays need it
	furi_thread_set_priority(d->thread, FuriThreadPriorityHigh);
	furi_thread_start(d->thread);
	return d;
}


void gray_display_stop(GrayDisplay* d)
{
	d->stop = true;
	furi_thread_join(d->thread);
	furi_thread_free(d->thread);
	gui_direct_draw_release(d->gui);
	furi_mutex_free(d->mutex);
	free(d);
}


void gray_display_set_config(GrayDisplay* d, GrayConfig config)
{
	furi_mutex_acquire(d->mutex, FuriWaitForever);
	d->config = config;
	furi_mutex_release(d->mutex);
}


void gray_display_set_overlay(GrayDisplay* d, GrayOverlay overlay, void* context)
{
	furi_mutex_acquire(d->mutex, FuriWaitForever);
	d->overlay = overlay;
	d->overlay_context = context;
	furi_mutex_release(d->mutex);
}


uint16_t gray_display_actual_rate(GrayDisplay* d)
{
	return d->actual_rate;
}


void gray_display_lateness(GrayDisplay* d, uint32_t* worst_us, uint32_t* over_1ms)
{
	*worst_us = d->late_worst_us;
	*over_1ms = d->late_count;
}


void gray_display_lateness_reset(GrayDisplay* d)
{
	d->late_worst_us = 0;
	d->late_count = 0;
}


void gray_display_costs(GrayDisplay* d, uint32_t* build_us, uint32_t* commit_us)
{
	*build_us = d->build_us;
	*commit_us = d->commit_us;
}
