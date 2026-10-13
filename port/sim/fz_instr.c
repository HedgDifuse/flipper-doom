// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * flipper-doom sim: which functions run in steady gameplay? (research only)
 *
 * Built only by sim/build_instr.sh: the engine is compiled with
 * -finstrument-functions, this file without. Frames are counted on entry to
 * fz_present(). Every function seen before FZ_HOT_START frames goes to the
 * "init" set, every function seen after it to the "play" set; both are
 * written as hex addresses to FZ_HOT_OUT at exit.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SLOTS 8192

static uintptr_t init_set[SLOTS], play_set[SLOTS];
static uint32_t frames, start = 100;
static int ready;

void fz_present(const uint8_t* screen, int8_t palette);

__attribute__((no_instrument_function)) static void add(uintptr_t* set, uintptr_t fn)
{
	uint32_t h = (uint32_t)(fn * 2654435761u) & (SLOTS - 1);
	while (set[h] && set[h] != fn)
		h = (h + 1) & (SLOTS - 1);
	set[h] = fn;
}

__attribute__((no_instrument_function)) static void dump(void)
{
	const char* path = getenv("FZ_HOT_OUT");
	FILE* f = fopen(path ? path : "hot.txt", "w");
	for (int i = 0; i < SLOTS; i++)
		if (init_set[i])
			fprintf(f, "init %lx\n", (unsigned long)init_set[i]);
	for (int i = 0; i < SLOTS; i++)
		if (play_set[i])
			fprintf(f, "play %lx\n", (unsigned long)play_set[i]);
	fclose(f);
}

__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void* fn, void* caller)
{
	(void)caller;
	if (!ready)
	{
		ready = 1;
		const char* s = getenv("FZ_HOT_START");
		if (s)
			start = atoi(s);
		atexit(dump);
	}
	if (fn == (void*)fz_present)
		frames++;
	add(frames < start ? init_set : play_set, (uintptr_t)fn);
}

__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void* fn, void* caller)
{
	(void)fn;
	(void)caller;
}
