/*
 * flipper-doom: PC simulator platform (32-bit Linux, gcc -m32).
 *
 * No window: it plays demos and optionally dumps frames so the 1-bit pipeline
 * and memory limits can be checked before touching the device.
 *   FZ_ZONE_KB=n        zone (heap) size, default 256
 *   FZ_FRAMES=n         quit after n presented frames
 *   FZ_FAST=1           virtual clock that ticks on every poll, no waiting
 *   FZ_INPUT="U:60 R:20 B:5 ..."  held key (U D L R A B S=strafe .=none)
 *                       and its duration in tics; the script loops
 *   FZ_DUMP=frames.bin  write every presented frame (FZ_W x FZ_H bytes of
 *                       luminance + pixel class, see fz_platform.h)
 *   FZ_EXIT_AT=n        finish the level at frame n (intermission, next map)
 *   FZ_NOHUD=1          FZ_DUMP1 without the status line
 *   FZ_DUMP1=lcd.bin    write every frame as the LCD gray frame (4 bits per pixel,
 *                       levels 0..FZ_PERIOD, default 4; FZ_GAMMA default 1.0)
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "compiler.h"
#include "d_event.h"
#include "d_main.h"
#include "doomdef.h"
#include "i_system.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#include "fz_dither.h"
#include "fz_graymap.h"
#include "fz_hud.h"
#include "fz_pager.h"
#include "globdata.h"
#include "p_inter.h"
#include "fz_platform.h"


static FILE* dump;
static uint32_t sounds_played; // counted, to see that the game asks for sounds
static FILE* dump1;
static uint32_t frames;
static int32_t fast_time; // FZ_FAST clock: one tic per poll, so the loop never waits
static struct timespec start;

// demand paging per frame, priced with SD timings measured on the device:
// a random read costs about 0.9 ms + 1.1 us per byte
#define PG_HIST 16
static uint32_t pg_frames_with[PG_HIST + 1];
static uint32_t pg_max, pg_last_misses, pg_last_bytes;
static double pg_ms_total, pg_ms_max;


static FILE* wad;

// FZ_LUMPSTATS=1: SD reads per lump (after the first FZ_LUMPSTATS_SKIP reads)
#define MAXLUMPS 2048
static struct { int32_t pos; char name[9]; uint32_t reads, bytes; } lumps[MAXLUMPS];
static int32_t numlumps_sim;
static uint32_t total_reads;

uint32_t fz_wad_open(void)
{
	wad = fopen(WAD_FILE, "rb");
	if (!wad)
		return 0;
	int32_t hdr[3];
	if (fread(hdr, 4, 3, wad) == 3)
	{
		numlumps_sim = (int16_t)hdr[1];
		fseek(wad, hdr[2], SEEK_SET);
		for (int32_t i = 0; i < numlumps_sim && i < MAXLUMPS; i++)
		{
			uint8_t e[16];
			if (fread(e, 1, 16, wad) != 16)
				break;
			memcpy(&lumps[i].pos, e, 4);
			memcpy(lumps[i].name, e + 8, 8);
		}
	}
	fseek(wad, 0, SEEK_END);
	return ftell(wad);
}


static void lump_stat(uint32_t pos, uint16_t length)
{
	const char* skip = getenv("FZ_LUMPSTATS_SKIP");
	if (++total_reads <= (uint32_t)(skip ? atoi(skip) : 0))
		return;
	int32_t best = -1;
	for (int32_t i = 0; i < numlumps_sim && i < MAXLUMPS; i++)
		if (lumps[i].pos <= (int32_t)pos && (best < 0 || lumps[i].pos > lumps[best].pos))
			best = i;
	if (best >= 0)
	{
		lumps[best].reads++;
		lumps[best].bytes += length;
	}
}


static void lump_report(void)
{
	if (!getenv("FZ_LUMPSTATS"))
		return;
	for (int round = 0; round < 15; round++)
	{
		int32_t best = -1;
		for (int32_t i = 0; i < numlumps_sim && i < MAXLUMPS; i++)
			if (lumps[i].reads && (best < 0 || lumps[i].reads > lumps[best].reads))
				best = i;
		if (best < 0)
			break;
		printf("  %-8s %6u reads %6u KB\n", lumps[best].name, lumps[best].reads, lumps[best].bytes / 1024);
		lumps[best].reads = 0;
	}
}


// every SD read after FZ_SD_SKIP frames (the level load), priced like the pager's
static uint32_t sd_reads, sd_bytes, sd_frames;
#if defined FZ_READSTATS
int16_t fz_reading_for; // 0 game, 1 sight checks, 2 rendering (until the frame is shown)
uint32_t fz_sight_by[2][256]; // full sight checks by [t2 is the player][t1 type]
static uint32_t sd_reads_for[3];
#endif

void fz_wad_read(uint32_t pos, void* dest, uint16_t length)
{
	extern int16_t _g_fz_loading_map; // not the level loads
	if (!_g_fz_loading_map && frames >= (uint32_t)(getenv("FZ_SD_SKIP") ? atoi(getenv("FZ_SD_SKIP")) : 1))
	{
		sd_reads++;
		sd_bytes += length;
#if defined FZ_READSTATS
		sd_reads_for[fz_reading_for]++;
#endif
	}
	if (getenv("FZ_LUMPSTATS"))
		lump_stat(pos, length);
	fseek(wad, pos, SEEK_SET);
	if (fread(dest, 1, length, wad) != length)
		I_Error("fz_wad_read: short read at %u", pos);
}


// FZ_ZONE_KB: one region of that size (default 256 KB);
// FZ_ZONE_SPLIT="40,20,8,4": separate regions of these sizes in KB, in
// shuffled address order with gaps between them, like a fragmented heap.
int16_t fz_zone_regions(fz_zone_region_t* regions)
{
	const char* split = getenv("FZ_ZONE_SPLIT");
	if (!split)
	{
		const char* kb = getenv("FZ_ZONE_KB");
		regions[0].size = (kb ? atol(kb) : 256) * 1024L;
		// 256 bytes past the zone: Doom's column drawers can read past a post
		// (a sprite far away: up to ~100 bytes), harmless in RAM but reported by
		// AddressSanitizer when the patch is the last block of the zone
		regions[0].base = malloc(regions[0].size + 256);
		return 1;
	}

	int16_t n = 0;
	for (const char* p = split; *p && n < FZ_ZONE_MAX_REGIONS; n++)
	{
		regions[n].size = atol(p) * 1024L;
		p = strchr(p, ',') ? strchr(p, ',') + 1 : p + strlen(p);
	}
	// one arena, regions placed in reverse order with 4 KB gaps
	uint32_t total = 0;
	for (int16_t i = 0; i < n; i++)
		total += regions[i].size + 4096 + 3;
	uint8_t* arena = malloc(total);
	uint8_t* at = arena + total;
	for (int16_t i = 0; i < n; i++)
	{
		at -= regions[i].size + 4096;
		regions[i].base = at + 3; // misaligned on purpose
	}
	return n;
}


void fz_init_video(void)
{
	const char* path1 = getenv("FZ_DUMP1");
	if (path1)
		dump1 = fopen(path1, "wb");

	const char* path = getenv("FZ_DUMP");
	if (!path)
		return;

	dump = fopen(path, "wb");
}


#if defined FZ_PGTRACE
extern FILE* pg_trace;
#endif

void fz_present(const uint8_t* screen, int8_t palette)
{
#if defined FZ_PGTRACE
	if (!pg_trace && getenv("FZ_PGTRACE"))
		pg_trace = fopen(getenv("FZ_PGTRACE"), "wb");
	if (pg_trace)
	{
		uint16_t mark = 0xffff;
		fwrite(&mark, 2, 1, pg_trace);
	}
#endif
	(void)palette;
	frames++;
#if defined FZ_READSTATS
	fz_reading_for = 0;
#endif

	uint32_t m = pg_misses - pg_last_misses, b = pg_bytes - pg_last_bytes;
	pg_last_misses = pg_misses;
	pg_last_bytes = pg_bytes;
	double ms = m * 0.9 + b * 0.0011;
	pg_ms_total += ms;
	if (ms > pg_ms_max)
		pg_ms_max = ms;
	if (m > pg_max)
		pg_max = m;
	pg_frames_with[m < PG_HIST ? m : PG_HIST]++;
	if (getenv("FZ_CHECKHEAP"))
		Z_CheckHeap();
	if (getenv("FZ_MISSLOG") && frames % 100 == 0)
	{
		static uint32_t last;
		printf("frames %u: %u misses\n", frames, pg_misses - last);
		last = pg_misses;
	}
	if (getenv("FZ_DIE_AT") && frames == (uint32_t)atoi(getenv("FZ_DIE_AT")))
	{
		extern player_t _g_player;
		printf("FZ_DIE_AT: the player dies at frame %u\n", frames);
		P_DamageMobj(_g_player.mo, NULL, NULL, 10000);
	}
	if (getenv("FZ_DIE_AT") && frames == (uint32_t)atoi(getenv("FZ_DIE_AT")) + 20)
	{
		extern player_t _g_player;
		_g_player.playerstate = PST_REBORN; // as if use was pressed
	}
	if (getenv("FZ_EXIT_AT") && frames == (uint32_t)atoi(getenv("FZ_EXIT_AT")))
	{
		void G_ExitLevel(void);
		printf("FZ_EXIT_AT: leaving the level at frame %u\n", frames);
		G_ExitLevel();
	}
	if (dump)
		fwrite(screen, 1, FZ_W * FZ_H, dump);
	if (dump1)
	{
		static uint8_t lcd[FZ_GRAY_FRAME_SIZE];
		static uint8_t lut[64];
		static fz_gray_t gray;
		if (!gray.period)
		{
			gray.period = getenv("FZ_PERIOD") ? atoi(getenv("FZ_PERIOD")) : 4;
			fz_build_lum_to_level(lut, gray.period, getenv("FZ_GAMMA") ? atof(getenv("FZ_GAMMA")) : 1.0f);
			gray.lum_to_level = lut;
		}
		fz_dither(screen, lcd, &gray, _g_gamestate == GS_LEVEL);
		if (_g_gamestate == GS_LEVEL && !getenv("FZ_NOHUD"))
			fz_hud_draw(lcd, gray.period);
		fwrite(lcd, 1, sizeof(lcd), dump1);
	}

	const char* limit = getenv("FZ_FRAMES");
	if (limit && frames >= (uint32_t)atoi(limit))
		I_Quit();
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


static int16_t script_key(char c)
{
	switch (c)
	{
		case 'U': return KEYD_UP;
		case 'D': return KEYD_DOWN;
		case 'L': return KEYD_LEFT;
		case 'R': return KEYD_RIGHT;
		case 'A': return KEYD_A;
		case 'B': return KEYD_B;
		case 'S': return KEYD_STRAFE;
		default:  return -1;
	}
}


// Replays FZ_INPUT: holds one key at a time, posting down/up transitions.
void I_StartTic(void)
{
	static const char* script;
	static const char* pos;
	static int32_t step_end;
	static int16_t held = -1;

	if (!script && !(script = getenv("FZ_INPUT")))
		return;

	int32_t now = I_GetTime();
	if (pos && now < step_end)
		return;

	if (!pos || !*pos)
		pos = script;
	while (*pos == ' ')
		pos++;

	int16_t key = script_key(pos[0]);
	int32_t tics = atoi(pos + 2);
	pos = strchr(pos, ' ') ? strchr(pos, ' ') : pos + strlen(pos);
	step_end = now + (tics > 0 ? tics : 1);

	event_t ev;
	if (held != -1 && held != key)
	{
		ev.type = ev_keyup;
		ev.data1 = held;
		D_PostEvent(&ev);
	}
	if (key != -1 && held != key)
	{
		ev.type = ev_keydown;
		ev.data1 = key;
		D_PostEvent(&ev);
	}
	held = key;
}


void I_InitTimer(void)
{
	clock_gettime(CLOCK_MONOTONIC, &start);
}


int32_t I_GetTime(void)
{
	if (getenv("FZ_FAST"))
		return fast_time++;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t us = (now.tv_sec - start.tv_sec) * 1000000LL + (now.tv_nsec - start.tv_nsec) / 1000;
	return (int32_t)(us * TICRATE / 1000000);
}


void I_Quit(void)
{
	if (dump)
		fclose(dump);
	if (dump1)
		fclose(dump1);
	extern uint32_t fz_missing_columns;
	printf("I_Quit: %u frames, %u sounds\n", frames, sounds_played);
	printf("wall columns without texture (zone full): %u\n", fz_missing_columns);
	printf("pager: %u misses, %u KB read, %.2f misses/frame, max %u\n", pg_misses, pg_bytes / 1024,
	       frames ? (double)pg_misses / frames : 0.0, pg_max);
	printf("pager SD time: avg %.2f ms/frame, worst frame %.1f ms\n",
	       frames ? pg_ms_total / frames : 0.0, pg_ms_max);
	sd_frames = frames - (getenv("FZ_SD_SKIP") ? atoi(getenv("FZ_SD_SKIP")) : 1);
	printf("all SD reads in play: %.2f reads/frame, %.2f ms/frame\n", (double)sd_reads / sd_frames,
	       (sd_reads * 0.9 + sd_bytes * 0.0011) / sd_frames);
#if defined FZ_READSTATS
	for (int t = 0; t < 512; t++) if (fz_sight_by[t / 256][t % 256]) printf("sight %s type %d: %u%c", t / 256 ? "->player" : "->other", t % 256, fz_sight_by[t / 256][t % 256], 10);
	printf("reads: game %u, sight %u, render %u" "%c", sd_reads_for[0], sd_reads_for[1], sd_reads_for[2], 10);
#endif
	printf("frames by misses:");
	for (int i = 0; i <= PG_HIST; i++)
		if (pg_frames_with[i])
			printf(" %d%s:%u", i, i == PG_HIST ? "+" : "", pg_frames_with[i]);
	printf("\n");
	lump_report();
	exit(0);
}


void I_Error(const char* error, ...)
{
	va_list argptr;
	va_start(argptr, error);
	vprintf(error, argptr);
	va_end(argptr);
	printf("\n");
	if (dump)
		fclose(dump);
	exit(1);
}


void fz_sound_play(int16_t num, int16_t volume)
{
	(void)num;
	(void)volume;
	sounds_played++;
}


void fz_music_play(int16_t song, boolean looping)
{
	(void)looping;
	if (getenv("FZ_MUSICLOG"))
		printf("music %d\n", song);
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


int main(int argc, const char* const* argv)
{
	D_DoomMain(argc, argv);
	return 0;
}
