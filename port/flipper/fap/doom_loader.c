// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * flipper-doom: the Doom app. A small loader; the engine itself (Doom8088
 * port, ~100 KB) runs in place from the free internal flash gap, where this
 * app installs it once from engine.bin.
 *
 * Steps: check/install the engine image, open the WAD, give the engine its
 * RAM block (initial .got/.data, relocated to wherever malloc put it), then
 * call it with r9 pointing at its GOT and fz_api_t as its window to the
 * firmware. The screen shows gray levels by flashing subframes (gray_display),
 * tuned per device by the screen calibration.
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>

#include "../../platform/fz_graymap.h"
#include "../fz_api.h"
#include "calibration.h"
#include "flash_install.h"
#include "gray_display.h"
#include "sound.h"
#include "doom_menu.h"
#include "ui.h"

#define TAG "Doom"

#define ENGINE_FILE APP_ASSETS_PATH("engine.bin")
// exists while the engine is being written to flash: if the Flipper is reset
// meanwhile, the next start rewrites it without reading the flash first
#define INSTALL_MARKER EXT_PATH("apps_data/flipper_doom/installing")
#define WAD_FILE    EXT_PATH("apps_data/flipper_doom/FZDOOM.WAD") // copied once, too big to ship in the app

// The engine's RAM block and zone are reserved early, right after the app's
// own small allocations. The zone is made of as many pieces of the heap as it
// takes (the largest free blocks first, see Z_Init in the engine), everything
// up to ZONE_MAX but HEAP_RESERVE. The simulator needs ~55 KB walking around
// E1M1/E1M8 (E1M4 ~81, E1M9 ~83, ~10% more when in pieces); short of that the
// engine stops with a Z_Malloc error instead of crashing the firmware.
#define ZONE_MIN        (56 * 1024)
#define ZONE_MAX        (128 * 1024)
#define ZONE_PIECE_MIN  (2 * 1024) // smaller free blocks are left to the firmware
#define ZONE_PIECES     16 // FZ_ZONE_MAX_REGIONS
#define HEAP_RESERVE    (6 * 1024) // stays free for the firmware while the engine runs
#define RUN_LOG      EXT_PATH("apps_data/flipper_doom/last_run.txt")
// while this file exists, New Game runs Doom's demo3 as a benchmark (-timedemo)
#define BENCHMARK    EXT_PATH("apps_data/flipper_doom/benchmark")
#define MAX_ALLOCS   8

int __wrap_vsnprintf(char* str, size_t size, const char* format, va_list args);

typedef struct
{
	Gui* gui;
	GrayDisplay* display;
	GrayConfig gray;
	uint8_t lum_to_level[64];
	uint8_t* frame; // GRAY_FRAME_SIZE

	FuriMutex* text_mutex;
	char lines[4][32];

	FuriPubSub* input_events;
	FuriPubSubSubscription* input_sub;
	FuriMessageQueue* input; // for menus
	volatile uint16_t buttons;
	volatile uint16_t pressed; // since the engine last asked
	volatile bool quit;
	uint32_t engine_frames; // for the fps measurement
	uint32_t wad_reads;
	uint32_t wad_cycles; // time spent reading the WAD since the last heartbeat
	uint32_t beat_tick, beat_reads; // at the last heartbeat

	Storage* storage;
	File* wad;
	void* allocs[MAX_ALLOCS];

	uint8_t* engine_mem; // engine RAM block
	size_t engine_ram;
	void* zone_base[ZONE_PIECES];
	uint32_t zone_bytes[ZONE_PIECES];
	uint8_t zone_pieces;
	size_t zone_size; // all pieces
	char reserve_log[160];
	char engine_message[96]; // the last line the engine logged (its error, if it failed)
	Sound* sound;
	FuriMutex* wad_lock; // app->wad is shared with the sound thread
	// the menu's sounds in the WAD (menu_sounds order)
	uint32_t sound_pos[16];
	uint16_t sound_len[16];
	void* ui_scratch; // UI_SCRATCH bytes while a menu or splash is up (NULL: none)
	bool ui_scratch_malloced;
} DoomApp;

static DoomApp* app; // the engine calls the api without a context


// Trace to the SD card, closed after every line so it survives a crash.
static void write_run_log(DoomApp* a, const char* text);

void trace(const char* fmt, ...) // also used by flash_install.c
{
	char line[96];
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(line, sizeof(line) - 1, fmt, args);
	va_end(args);
	if(n < 0) return;
	if(n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
	line[n++] = '\n';
	File* f = storage_file_alloc(app->storage);
	if(storage_file_open(f, RUN_LOG, FSAM_WRITE, FSOM_OPEN_APPEND)) storage_file_write(f, line, n);
	storage_file_free(f);
}


// ---- screen and buttons --------------------------------------------------

static void input_callback(const void* value, void* context)
{
	const InputEvent* event = value;
	DoomApp* a = context;
	uint16_t bit = 0;
	switch(event->key) {
	case InputKeyUp: bit = FZ_BTN_UP; break;
	case InputKeyDown: bit = FZ_BTN_DOWN; break;
	case InputKeyLeft: bit = FZ_BTN_LEFT; break;
	case InputKeyRight: bit = FZ_BTN_RIGHT; break;
	case InputKeyOk: bit = FZ_BTN_OK; break;
	case InputKeyBack: bit = FZ_BTN_BACK; break;
	default: return;
	}
	if(event->type == InputTypePress) {
		a->buttons |= bit;
		a->pressed |= bit;
	}
	if(event->type == InputTypeRelease) a->buttons &= ~bit;
	if(event->type == InputTypeLong && bit == FZ_BTN_BACK) a->quit = true;
	furi_message_queue_put(a->input, event, 0);
}


static void text_overlay(Canvas* canvas, void* context)
{
	DoomApp* a = context;
	furi_mutex_acquire(a->text_mutex, FuriWaitForever);
	canvas_set_color(canvas, ColorBlack);
	canvas_set_font(canvas, FontPrimary);
	canvas_draw_str(canvas, 2, 12, a->lines[0]);
	canvas_set_font(canvas, FontSecondary);
	for(int i = 1; i < 4; i++) canvas_draw_str(canvas, 2, 12 + i * 14, a->lines[i]);
	furi_mutex_release(a->text_mutex);
}


static void show_text(DoomApp* a, const char* l0, const char* l1, const char* l2, const char* l3)
{
	furi_mutex_acquire(a->text_mutex, FuriWaitForever);
	strlcpy(a->lines[0], l0 ? l0 : "", sizeof(a->lines[0]));
	strlcpy(a->lines[1], l1 ? l1 : "", sizeof(a->lines[1]));
	strlcpy(a->lines[2], l2 ? l2 : "", sizeof(a->lines[2]));
	strlcpy(a->lines[3], l3 ? l3 : "", sizeof(a->lines[3]));
	furi_mutex_release(a->text_mutex);
	memset(a->frame, 0, GRAY_FRAME_SIZE);
	gray_display_set_overlay(a->display, text_overlay, a);
}


// Memory for the screens: the zone's first piece while the engine is not
// running, else the heap for a moment (when the zone could not be reserved).
static Ui* ui_begin(DoomApp* a)
{
	if(!a->ui_scratch) {
		if(a->zone_pieces && a->zone_bytes[0] >= UI_SCRATCH)
			a->ui_scratch = a->zone_base[0];
		else if(memmgr_heap_get_max_free_block() > UI_SCRATCH + HEAP_RESERVE) {
			a->ui_scratch = malloc(UI_SCRATCH);
			a->ui_scratch_malloced = true;
		} else
			return NULL;
	}
	gray_display_set_overlay(a->display, NULL, NULL);
	return ui_open(a->ui_scratch, a->storage, &a->gray);
}

static void ui_end(DoomApp* a, Ui* ui)
{
	if(ui) ui_close(ui);
	if(a->ui_scratch_malloced) free(a->ui_scratch);
	a->ui_scratch = NULL;
	a->ui_scratch_malloced = false;
}

// The Doom logo on white, a status line and, with permille >= 0, a progress bar.
static void show_splash(DoomApp* a, const char* status, int permille)
{
	Ui* ui = ui_begin(a);
	if(!ui) {
		show_text(a, "Doom", status, "", "");
		return;
	}
	ui_clear(ui, false);
	ui_logo(ui, UiLogoBig, (128 - 123) / 2, 0);
	if(permille >= 0) {
		ui_text_center(ui, 49, status);
		ui_progress(ui, 14, 58, 100, permille);
	} else
		ui_text_center(ui, 56, status);
	ui_present(ui, a->frame, true);
	ui_end(a, ui);
}


static InputKey wait_key(DoomApp* a)
{
	InputEvent e;
	while(true) {
		furi_check(furi_message_queue_get(a->input, &e, FuriWaitForever) == FuriStatusOk);
		if(e.type == InputTypeShort) return e.key;
	}
}


static void wait_back(DoomApp* a)
{
	while(wait_key(a) != InputKeyBack) {
	}
}


// ---- fz_api_t for the engine -------------------------------------------

static void* api_malloc(size_t size)
{
	trace("malloc %u: free %u, block %u", size, memmgr_get_free_heap(), memmgr_heap_get_max_free_block());
	if(size + 512 > memmgr_heap_get_max_free_block()) furi_crash("Doom: engine malloc too big");
	for(int i = 0; i < MAX_ALLOCS; i++)
		if(!app->allocs[i]) return app->allocs[i] = malloc(size);
	furi_crash("Doom: too many allocations");
}

static void api_free(void* ptr)
{
	for(int i = 0; i < MAX_ALLOCS; i++)
		if(app->allocs[i] == ptr) app->allocs[i] = NULL;
	free(ptr);
}

static uint32_t api_wad_open(void)
{
	return storage_file_size(app->wad);
}

// The SD card shares the SPI bus with the LCD: read in small pieces so a
// subframe never waits long for the bus (a late subframe makes the grays drift).
#define WAD_READ_CHUNK 512

static void api_wad_read(uint32_t pos, void* dest, uint16_t length)
{
	const uint32_t t0 = DWT->CYCCNT;
	furi_mutex_acquire(app->wad_lock, FuriWaitForever);
	storage_file_seek(app->wad, pos, true);
	uint8_t* d = dest;
	while(length) {
		uint16_t n = length < WAD_READ_CHUNK ? length : WAD_READ_CHUNK;
		if(storage_file_read(app->wad, d, n) != n) furi_crash("Doom: WAD read failed");
		d += n;
		length -= n;
		app->wad_reads++;
	}
	furi_mutex_release(app->wad_lock);
	app->wad_cycles += DWT->CYCCNT - t0;
}

static uint32_t api_ticks_ms(void)
{
	return furi_get_tick() * 1000 / furi_kernel_get_tick_frequency();
}

static void api_delay_ms(uint32_t ms)
{
	furi_delay_ms(ms);
}

static uint16_t api_buttons_pressed(void)
{
	FURI_CRITICAL_ENTER();
	const uint16_t pressed = app->pressed;
	app->pressed = 0;
	FURI_CRITICAL_EXIT();
	return pressed;
}

static uint16_t api_buttons(void)
{
	return app->buttons | (app->quit ? FZ_QUIT_REQUESTED : 0);
}

// The display thread reads the frame while the engine writes it: a torn
// subframe now and then is invisible next to the flicker it already hides.
static uint8_t* api_frame_lock(void)
{
	return app->frame;
}

static void api_frame_unlock(void)
{
	app->engine_frames++;
	// heartbeat: if the game ever hangs, the last line shows where it was
	// (every 100 frames: each line costs an SD write)
	if(app->engine_frames % 100 == 0) {
		const uint32_t now = furi_get_tick(), ms = (now - app->beat_tick) * 1000 / furi_kernel_get_tick_frequency();
		trace("frame %lu: %lu.%lu fps, SD %lu ms/frame, %lu reads/frame, stack free %lu, heap free %u",
		      app->engine_frames, ms ? 100000 / ms : 0, ms ? 1000000 / ms % 10 : 0,
		      app->wad_cycles / furi_hal_cortex_instructions_per_microsecond() / 100 / 1000,
		      (app->wad_reads - app->beat_reads) / 100,
		      furi_thread_get_stack_space(furi_thread_get_current_id()), memmgr_get_free_heap());
		app->wad_cycles = 0;
		app->beat_tick = now;
		app->beat_reads = app->wad_reads;
	}
}

static void api_sound(uint32_t pos, uint16_t length, uint8_t volume)
{
	sound_play(app->sound, pos, length, volume);
}

static void api_music(uint8_t song, uint8_t looping)
{
	sound_music(app->sound, song, looping);
}

static void api_log(const char* message)
{
	FURI_LOG_I("DoomEngine", "%s", message);
	strlcpy(app->engine_message, message, sizeof(app->engine_message));
	trace("%s | free %u, block %u", message, memmgr_get_free_heap(), memmgr_heap_get_max_free_block());
}


// ---- running the engine -------------------------------------------------

// Call fz_engine_main(api) with r9 = the engine's GOT; r9 is saved around it.
__attribute__((naked, noinline)) static int call_engine(const fz_api_t* api, uint32_t entry, void* got)
{
	UNUSED(api);
	UNUSED(entry);
	UNUSED(got);
	__asm volatile("push {r4, r9, lr}\n"
	               "sub sp, #4\n" // keep the stack 8-byte aligned
	               "mov r9, r2\n"
	               "blx r1\n"
	               "add sp, #4\n"
	               "pop {r4, r9, pc}\n");
}


static void install_progress(void* context, uint32_t done, uint32_t total)
{
	show_splash(context, "INSTALLING ENGINE", total ? done * 1000 / total : 0);
}


// Make sure the engine in flash matches engine.bin; install it if needed.
static bool ensure_engine(DoomApp* a, fz_image_header_t* hdr)
{
	File* file = storage_file_alloc(a->storage);
	bool ok = false;
	const char* error = NULL;

	if(!storage_file_open(file, ENGINE_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
		show_text(a, "Doom", "engine.bin is missing", "Reinstall the app", "Back: exit");
		wait_back(a);
	} else {
		const bool interrupted = storage_file_exists(a->storage, INSTALL_MARKER);
		write_run_log(a, "");
		trace("engine check, interrupted install: %d", interrupted);
		switch(flash_image_check(file, hdr, !interrupted)) {
		case FlashImageOk: ok = true; break;
		case FlashImageBadFile:
			show_text(a, "Doom", "engine.bin is damaged", "or for another version", "Back: exit");
			wait_back(a);
			break;
		case FlashImageMissing:
			if(!flash_gap_is_free(&error)) {
				show_text(a, "Cannot install", error, "Free flash may be in use", "Back: exit");
				wait_back(a);
				break;
			}
			while(furi_hal_bt_is_active() && !a->quit) {
				show_text(a, "Engine update", "Goes to flash once.", "Turn Bluetooth OFF,", "then come back here");
				furi_delay_ms(500);
			}
			if(a->quit) break;
			{
				File* marker = storage_file_alloc(a->storage);
				storage_file_open(marker, INSTALL_MARKER, FSAM_WRITE, FSOM_CREATE_ALWAYS);
				storage_file_free(marker);
			}
			ok = flash_image_install(file, hdr, install_progress, a, &error);
			if(ok) storage_simply_remove(a->storage, INSTALL_MARKER);
			trace("install %s, marker removed", ok ? "ok" : error);
			if(!ok) {
				show_text(a, "Install failed", error, "", "Back: exit");
				wait_back(a);
			}
			break;
		}
	}
	storage_file_free(file);
	return ok;
}


// the log starts over when the app starts; later games from the menu append
static void write_run_log(DoomApp* a, const char* text)
{
	static bool started;
	storage_simply_mkdir(a->storage, EXT_PATH("apps_data/flipper_doom"));
	File* f = storage_file_alloc(a->storage);
	if(storage_file_open(f, RUN_LOG, FSAM_WRITE, started ? FSOM_OPEN_APPEND : FSOM_CREATE_ALWAYS)) {
		if(started) storage_file_write(f, "---- next game\n", 15);
		storage_file_write(f, text, strlen(text));
	}
	started = true;
	storage_file_free(f);
}


static int run_engine(DoomApp* a, const fz_image_header_t* hdr, uint8_t map, uint8_t skill)
{
	write_run_log(a, a->reserve_log);
	if(!a->engine_mem || ((hdr->ram_size + 15) & ~15U) != a->engine_ram) {
		show_text(a, "Not enough memory", a->reserve_log, "Restart the app", "Back: back");
		wait_back(a);
		return 0;
	}

	// RAM block: initial .got/.data from flash, zeroed .bss, RAM addresses moved
	uint8_t* ram = a->engine_mem;
	memcpy(ram, (const void*)hdr->ram_load, hdr->ram_init);
	memset(ram + hdr->ram_init, 0, hdr->ram_size - hdr->ram_init);
	const uint32_t delta = (uint32_t)ram - FZ_IMAGE_RAM_LINK;
	const uint32_t* relocs = (const uint32_t*)hdr->relocs;
	for(uint32_t i = 0; i < hdr->reloc_count; i++) *(uint32_t*)(ram + relocs[i]) += delta;
	trace("engine RAM at %p, zone %u, heap free %u, block %u", ram, a->zone_size, memmgr_get_free_heap(),
	      memmgr_heap_get_max_free_block());

	fz_build_lum_to_level(a->lum_to_level, a->gray.period, a->gray.gamma);
	static fz_api_t api;
	api.version = FZ_API_VERSION;
	api.malloc = api_malloc;
	api.free = api_free;
	api.zone_regions = a->zone_pieces;
	for(int i = 0; i < a->zone_pieces; i++) {
		api.zone_base[i] = a->zone_base[i];
		api.zone_bytes[i] = a->zone_bytes[i];
	}
	api.wad_open = api_wad_open;
	api.wad_read = api_wad_read;
	api.ticks_ms = api_ticks_ms;
	api.delay_ms = api_delay_ms;
	api.buttons = api_buttons;
	api.buttons_pressed = api_buttons_pressed;
	api.frame_lock = api_frame_lock;
	api.frame_unlock = api_frame_unlock;
	api.gray_period = a->gray.period;
	api.lum_to_level = a->lum_to_level;
	api.start_map = map;
	api.start_skill = skill;
	api.vsnprintf = __wrap_vsnprintf;
	api.log = api_log;
	api.sound = api_sound;
	api.music = api_music;

	gray_display_set_overlay(a->display, NULL, NULL);
	// the splash stays on until the engine draws its first frame
	a->quit = false;
	a->pressed = 0; // what was pressed in the menu

	a->engine_frames = 0;
	a->wad_reads = 0;
	a->wad_cycles = 0;
	a->beat_reads = 0;
	a->beat_tick = furi_get_tick();
	gray_display_lateness_reset(a->display);
	const uint32_t started = furi_get_tick();
	int result = call_engine(&api, hdr->entry, ram + hdr->got_offset);

	uint32_t build_us, commit_us, late_max_us, late_count, seconds = (furi_get_tick() - started) / furi_kernel_get_tick_frequency();
	gray_display_costs(a->display, &build_us, &commit_us);
	gray_display_lateness(a->display, &late_max_us, &late_count);
	trace("played %lus: Doom %lu.%lu fps; subframes %u/s at %lu us build + %lu us LCD", seconds,
	      seconds ? a->engine_frames / seconds : 0, seconds ? a->engine_frames * 10 / seconds % 10 : 0,
	      gray_display_actual_rate(a->display), build_us, commit_us);
	trace("late subframes: %lu over 1 ms, worst %lu us; WAD reads %lu", late_count, late_max_us, a->wad_reads);

	trace("engine returned %d, stack never used %lu B", result, furi_thread_get_stack_space(furi_thread_get_current_id()));
	for(int i = 0; i < MAX_ALLOCS; i++)
		if(a->allocs[i]) {
			free(a->allocs[i]);
			a->allocs[i] = NULL;
		}
	furi_message_queue_reset(a->input);
	a->buttons = 0;
	return result;
}


static void release_engine_memory(DoomApp* a)
{
	for(int i = 0; i < a->zone_pieces; i++) free(a->zone_base[i]);
	a->zone_pieces = 0;
	a->zone_size = 0;
	free(a->engine_mem);
	a->engine_mem = NULL;
}

// Reserve the engine's RAM block and the zone pieces before the firmware
// gets to fragment the heap further.
static void reserve_engine_memory(DoomApp* a)
{
	fz_image_header_t hdr = {0};
	File* f = storage_file_alloc(a->storage);
	if(storage_file_open(f, ENGINE_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) storage_file_read(f, &hdr, sizeof(hdr));
	storage_file_free(f);
	if(hdr.magic != FZ_IMAGE_MAGIC) return;

	const size_t free_heap = memmgr_get_free_heap(), block = memmgr_heap_get_max_free_block();
	const size_t ram = (hdr.ram_size + 15) & ~15U;
	int len = snprintf(a->reserve_log, sizeof(a->reserve_log), "free %u, block %u: RAM %u, zone", free_heap, block, ram);
	if(ram + 64 > block || ram + HEAP_RESERVE > free_heap) return;
	a->engine_mem = malloc(ram);
	a->engine_ram = ram;

	// the largest free blocks, one after another (the firmware's malloc never
	// fails, it crashes: every request must fit)
	while(a->zone_pieces < ZONE_PIECES && a->zone_size < ZONE_MAX) {
		const size_t heap = memmgr_get_free_heap(), largest = memmgr_heap_get_max_free_block();
		if(heap < HEAP_RESERVE + ZONE_PIECE_MIN || largest < 64) break;
		size_t take = largest - 32; // the heap's block header
		if(take > heap - HEAP_RESERVE) take = heap - HEAP_RESERVE;
		if(take > ZONE_MAX - a->zone_size) take = ZONE_MAX - a->zone_size;
		take &= ~15U;
		if(take < ZONE_PIECE_MIN) break;
		a->zone_base[a->zone_pieces] = malloc(take);
		a->zone_bytes[a->zone_pieces++] = take;
		a->zone_size += take;
		if(len < (int)sizeof(a->reserve_log))
			len += snprintf(a->reserve_log + len, sizeof(a->reserve_log) - len, " %u", take);
	}
	if(len < (int)sizeof(a->reserve_log))
		snprintf(a->reserve_log + len, sizeof(a->reserve_log) - len, " = %u", a->zone_size);
	if(a->zone_size < ZONE_MIN) release_engine_memory(a);
}


// ---- Doom's menu ---------------------------------------------------------

static const char* const menu_sounds[] = {
	"DSPSTOP", "DSPISTOL", "DSSWTCHX", "DSSWTCHN", "DSSTNMOV", "DSPLDETH", "DSDMPAIN",
	"DSPOPAIN", "DSSLOP", "DSTELEPT", "DSPOSIT1", "DSPOSIT3", "DSSGTATK",
};

// Where the menu's sounds are in the WAD: one pass over its directory.
static void find_menu_sounds(DoomApp* a)
{
	struct
	{
		char id[4];
		int32_t count, dir;
	} header;
	struct
	{
		int32_t pos, size;
		char name[8];
	} entry[32];
	furi_mutex_acquire(a->wad_lock, FuriWaitForever);
	if(storage_file_seek(a->wad, 0, true) && storage_file_read(a->wad, &header, sizeof(header)) == sizeof(header) &&
	   storage_file_seek(a->wad, header.dir, true))
		for(int32_t done = 0; done < header.count;) {
			const int32_t n = header.count - done < 32 ? header.count - done : 32;
			if(storage_file_read(a->wad, entry, n * sizeof(entry[0])) != n * sizeof(entry[0])) break;
			for(int32_t k = 0; k < n; k++)
				for(size_t i = 0; i < COUNT_OF(menu_sounds); i++)
					if(strncmp(entry[k].name, menu_sounds[i], 8) == 0) {
						a->sound_pos[i] = entry[k].pos;
						a->sound_len[i] = entry[k].size;
					}
			done += n;
		}
	furi_mutex_release(a->wad_lock);
}

static void play_menu_sound(void* context, const char* lump)
{
	DoomApp* a = context;
	for(size_t i = 0; i < COUNT_OF(menu_sounds); i++)
		if(strcmp(lump, menu_sounds[i]) == 0 && a->sound_len[i]) sound_play(a->sound, a->sound_pos[i], a->sound_len[i], 100);
}

static MenuAction run_menu(DoomApp* a, MenuChoice* choice)
{
	Ui* ui = ui_begin(a);
	if(!ui) {
		show_text(a, "Not enough memory", a->reserve_log, "Restart the app", "Back: exit");
		wait_back(a);
		return MenuActionQuit;
	}
	const MenuEnv env = {
		.ui = ui,
		.frame = a->frame,
		.input = a->input,
		.storage = a->storage,
		.sound = a->sound,
		.play = play_menu_sound,
		.context = a,
	};
	const MenuAction action = doom_menu_run(&env, choice);
	ui_end(a, ui);
	return action;
}


int32_t doom_app(void* p)
{
	UNUSED(p);
	app = malloc(sizeof(DoomApp));
	memset(app, 0, sizeof(DoomApp));
	app->storage = furi_record_open(RECORD_STORAGE);
	app->frame = malloc(GRAY_FRAME_SIZE);
	memset(app->frame, 0, GRAY_FRAME_SIZE);
	app->text_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
	app->input = furi_message_queue_alloc(16, sizeof(InputEvent));
	app->input_events = furi_record_open(RECORD_INPUT_EVENTS);
	app->input_sub = furi_pubsub_subscribe(app->input_events, input_callback, app);
	app->gui = furi_record_open(RECORD_GUI);

	bool calibrated = gray_config_load(app->storage, &app->gray);
	if(!calibrated) app->gray = GRAY_CONFIG_DEFAULT;
	app->display = gray_display_start(app->gui, app->frame, app->gray);
	app->wad_lock = furi_mutex_alloc(FuriMutexTypeNormal);
	app->sound = sound_start(app->storage, sound_config_load(app->storage));
	reserve_engine_memory(app); // after the small allocations, before anything else

	show_splash(app, "LOADING...", -1);
	fz_image_header_t hdr;
	if(ensure_engine(app, &hdr)) {
		if(!calibrated) calibration_run(app->display, app->frame, &app->gray, app->input, app->storage);

		trace("engine ready");
		app->wad = storage_file_alloc(app->storage);
		if(!storage_file_open(app->wad, WAD_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
			show_text(app, "Doom", "FZDOOM.WAD is missing", "", "Back: exit");
			wait_back(app);
		} else {
			sound_set_wad(app->sound, app->wad, app->wad_lock);
			find_menu_sounds(app);
			trace("WAD open, menu");
			MenuChoice choice;
			for(MenuAction action; (action = run_menu(app, &choice)) != MenuActionQuit;) {
				if(action == MenuActionCalibrate) {
					calibration_run(app->display, app->frame, &app->gray, app->input, app->storage);
					continue;
				}
				const bool benchmark = storage_file_exists(app->storage, BENCHMARK);
				char level[16];
				snprintf(level, sizeof(level), benchmark ? "BENCHMARK" : "E1M%d", choice.map);
				show_splash(app, level, -1);
				int result = run_engine(app, &hdr, benchmark ? 0 : choice.map, choice.skill);
				const char* fps = strstr(app->engine_message, "= ");
				if(benchmark && strncmp(app->engine_message, "Timed ", 6) == 0 && fps) {
					// "Timed 2134 gametics in 1234 realtics = 60.512 frames per second"
					char line[32];
					snprintf(line, sizeof(line), "demo3: %.*s fps", (int)strcspn(fps + 2, " "), fps + 2);
					show_text(app, "Benchmark", line, "details: last_run.txt", "Back: menu");
					wait_back(app);
				} else if(result) {
					// the engine's message, e.g. "E1M2: not enough memory (...)",
					// on two lines; the details are in last_run.txt
					char line1[32], line2[32];
					const char* m = app->engine_message;
					const char* cut = strchr(m, '(');
					size_t first = cut ? (size_t)(cut - m) : strlen(m);
					if(first >= sizeof(line1)) first = sizeof(line1) - 1;
					memcpy(line1, m, first);
					line1[first] = 0;
					snprintf(line2, sizeof(line2), "error %d, see last_run.txt", result);
					show_text(app, "Doom", line1, line2, "Back: menu");
					wait_back(app);
				}
			}
		}
		sound_set_wad(app->sound, NULL, NULL);
		storage_file_free(app->wad);
	}

	sound_stop(app->sound);
	gray_display_stop(app->display);
	furi_record_close(RECORD_STORAGE);
	furi_record_close(RECORD_GUI);
	furi_pubsub_unsubscribe(app->input_events, app->input_sub);
	furi_record_close(RECORD_INPUT_EVENTS);
	furi_message_queue_free(app->input);
	furi_mutex_free(app->text_mutex);
	furi_mutex_free(app->wad_lock);
	free(app->frame);
	release_engine_memory(app);
	free(app);
	return 0;
}
