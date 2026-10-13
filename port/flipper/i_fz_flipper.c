// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * flipper-doom: engine-side platform layer for Flipper Zero.
 *
 * Lives in the flash image together with the engine. Everything that touches
 * the firmware goes through the fz_api_t table handed over by the loader FAP;
 * the C library functions that need global state (malloc, printf, exit) are
 * provided here on top of it.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "d_event.h"
#include "d_main.h"
#include "doomdef.h"
#include "i_system.h"
#include "i_sound.h"
#include "w_wad.h"
#include "globdata.h"

#include "fz_api.h"
#include "fz_dither.h"
#include "fz_hud.h"
#include "fz_platform.h"


static const fz_api_t* api;
static jmp_buf quit_jmp;
static uint32_t start_ms;


// ---- C library on top of the loader ----------------------------------------

void* malloc(size_t size)
{
	return api->malloc(size);
}


void free(void* ptr)
{
	api->free(ptr);
}


void exit(int status)
{
	longjmp(quit_jmp, status ? status : 1);
}


int vsnprintf(char* str, size_t size, const char* format, va_list args)
{
	return api->vsnprintf(str, size, format, args);
}


int snprintf(char* str, size_t size, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	int n = api->vsnprintf(str, size, format, args);
	va_end(args);
	return n;
}


int sprintf(char* str, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	int n = api->vsnprintf(str, 256, format, args);
	va_end(args);
	return n;
}


int printf(const char* format, ...)
{
	char line[96];
	va_list args;
	va_start(args, format);
	int n = api->vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	api->log(line);
	return n;
}


int puts(const char* s)
{
	api->log(s);
	return 0;
}


// newlib's atoi goes through strtol, which sets errno in global state
int atoi(const char* s)
{
	int sign = 1, n = 0;
	while (*s == ' ')
		s++;
	if (*s == '-' || *s == '+')
		sign = *s++ == '-' ? -1 : 1;
	while (*s >= '0' && *s <= '9')
		n = n * 10 + (*s++ - '0');
	return sign * n;
}


// no C++ exceptions: keep libgcc's unwinder out of the image
void __aeabi_unwind_cpp_pr0(void) {}
void __aeabi_unwind_cpp_pr1(void) {}
void __aeabi_unwind_cpp_pr2(void) {}


// ---- engine platform hooks ---------------------------------------------------

uint32_t fz_wad_open(void)
{
	return api->wad_open();
}


void fz_wad_read(uint32_t pos, void* dest, uint16_t length)
{
	api->wad_read(pos, dest, length);
}


int16_t fz_zone_regions(fz_zone_region_t* regions)
{
	int16_t n = api->zone_regions < FZ_ZONE_MAX_REGIONS ? api->zone_regions : FZ_ZONE_MAX_REGIONS;
	for (int16_t i = 0; i < n; i++)
	{
		regions[i].base = api->zone_base[i];
		regions[i].size = api->zone_bytes[i];
	}
	return n;
}


void fz_init_video(void)
{
}


// Cycle counter (DWT), for the per-frame profile written to the log.
#define DWT_CYCCNT (*(volatile uint32_t*)0xE0001004)
#define CPU_MHZ    64
#define PERF_EVERY 64
static uint32_t perf_frames, perf_dither, perf_rest, perf_last;

void fz_present(const uint8_t* screen, int8_t palette)
{
	(void)palette;
	const uint32_t t0 = DWT_CYCCNT;
	if (perf_last)
		perf_rest += t0 - perf_last; // game logic + rendering since the last frame
	const boolean in_game = _g_gamestate == GS_LEVEL;
	const fz_gray_t gray = {api->gray_period, api->lum_to_level};
	uint8_t* frame = api->frame_lock();
	fz_dither(screen, frame, &gray, in_game);
	if (in_game)
		fz_hud_draw(frame, gray.period);
	const uint32_t t1 = DWT_CYCCNT;
	perf_dither += t1 - t0;
	perf_last = t1;
	if (++perf_frames == PERF_EVERY)
	{
		printf("perf: game+render %u us, dither+hud %u us per frame",
		       (unsigned)(perf_rest / PERF_EVERY / CPU_MHZ), (unsigned)(perf_dither / PERF_EVERY / CPU_MHZ));
		perf_frames = perf_dither = perf_rest = 0;
		perf_last = DWT_CYCCNT; // the log line itself takes time
	}
	api->frame_unlock();
}


void I_InitGraphicsHardwareSpecificCode(void);

void I_InitGraphics(void)
{
	I_InitGraphicsHardwareSpecificCode();
}


void I_SetScreenMode(uint16_t mode)
{
	(void)mode;
}


void I_InitKeyboard(void)
{
}


void I_InitTimer(void)
{
	start_ms = api->ticks_ms();
}


int32_t I_GetTime(void)
{
	return (int32_t)((api->ticks_ms() - start_ms) * TICRATE / 1000);
}


// Flipper buttons -> Doom keys. OK fires, BACK uses (doors, switches).
static const struct { uint16_t button; int16_t key; } keymap[] =
{
	{FZ_BTN_UP,    KEYD_UP},
	{FZ_BTN_DOWN,  KEYD_DOWN},
	{FZ_BTN_LEFT,  KEYD_LEFT},
	{FZ_BTN_RIGHT, KEYD_RIGHT},
	{FZ_BTN_OK,    KEYD_B},
	{FZ_BTN_BACK,  KEYD_A},
};


static uint16_t held;     // as the game last heard


static void I_PostButtons(uint16_t now)
{
	uint16_t changed = now ^ held;
	for (size_t i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++)
	{
		if (!(changed & keymap[i].button))
			continue;

		event_t ev;
		ev.type  = (now & keymap[i].button) ? ev_keydown : ev_keyup;
		ev.data1 = keymap[i].key;
		D_PostEvent(&ev);
	}
	held = now;
}


void I_StartTic(void)
{
	const uint16_t buttons = api->buttons();
	if (buttons & FZ_QUIT_REQUESTED)
		I_Quit();

	// as held now; a click that was over before this poll still counts for
	// one tic (released at the next poll)
	I_PostButtons(buttons | api->buttons_pressed());
}


extern uint32_t fz_missing_columns;

void I_Quit(void)
{
	printf("wall columns drawn without texture (zone full): %u", (unsigned)fz_missing_columns);
	longjmp(quit_jmp, 1);
}


void I_Error(const char* error, ...)
{
	char line[96];
	va_list args;
	va_start(args, error);
	api->vsnprintf(line, sizeof(line), error, args);
	va_end(args);
	api->log(line);
	longjmp(quit_jmp, 2);
}


void fz_sound_play(int16_t num, int16_t volume)
{
	api->sound(W_LumpPos(num), W_LumpLength(num), volume < 0 ? 0 : volume);
}


void fz_music_play(int16_t song, boolean looping)
{
	api->music(song, looping);
}


// sound effects: through fz_sound_play
void PCFX_Init(void)
{
}


void PCFX_Shutdown(void)
{
}


void PCFX_Play(int16_t lumpnum)
{
	(void)lumpnum;
}


// ---- entry -------------------------------------------------------------------

__attribute__((section(".text.fz_engine_main")))
int fz_engine_main(const fz_api_t* loader_api)
{
	api = loader_api;
	if (api->version != FZ_API_VERSION)
		return 3;

	int code = setjmp(quit_jmp);
	if (code)
		return code == 1 ? 0 : code;

	static char map[2], skill[2];
	map[0] = '0' + api->start_map;
	skill[0] = '0' + api->start_skill;
	static const char* argv[] = {"doom", "-warp", map, "-skill", skill};
	static const char* benchmark[] = {"doom", "-timedemo", "demo3"}; // ends with I_Error("Timed ...")
	if (api->start_map == 0)
		D_DoomMain(3, benchmark);
	else
		D_DoomMain(5, argv);
	return 0;
}
