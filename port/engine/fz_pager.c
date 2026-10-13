// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * flipper-doom: demand pager for read-only WAD data, see fz_pager.h.
 *
 * The pages are a static LRU cache of their own. (Putting them in the zone as
 * purgeable blocks, to share the memory with cached textures, was tried: the
 * two then evict each other and E1M1 spent ~26 ms per frame on SD reads.)
 */

#include <stdint.h>
#include <string.h>

#include "compiler.h"
#include "w_wad.h"
#include "z_zone.h"

#include "fz_pager.h"


#define NO_PAGE 0xffff
#define HINTS   128 // power of 2

typedef struct
{
	uint16_t page;  // WAD file page, NO_PAGE = empty
	uint16_t stamp; // last use, for LRU
} pgslot_t;

#define MAX_SLOTS  127 // slot numbers fit hint[]
#define MAX_CHUNKS 4

static pgslot_t slots[MAX_SLOTS];
static uint8_t  static_pages[PG_SLOTS][PG_PAGE_SIZE];

// flipper-doom: with the level loaded, the zone the level does not need
// gives more slots (PG_AddPages), in up to MAX_CHUNKS pieces: slot
// PG_SLOTS + n is page n of the chunks, one after another. More pages matter:
// the sight checks of a big level walk through most of its NODES, SEGS and
// LINEDEFS.
static uint8_t* chunks[MAX_CHUNKS];
static int16_t  chunk_end[MAX_CHUNKS]; // slot after the chunk's last
static int16_t  numchunks;
static int16_t  numslots = PG_SLOTS;

static uint16_t clock;
static int16_t  last;         // slot of the previous hit, checked first
static int8_t   hint[HINTS];  // page & (HINTS - 1) -> a slot it was in, to skip the scan
static boolean  initialized;

uint32_t pg_misses, pg_bytes;


static uint8_t* PG_Slot(int16_t i)
{
	if (i < PG_SLOTS)
		return static_pages[i];
	int16_t c = 0, first = PG_SLOTS;
	while (i >= chunk_end[c])
		first = chunk_end[c++];
	return chunks[c] + (uint32_t)(i - first) * PG_PAGE_SIZE;
}


void PG_Flush(void)
{
	for (int16_t i = 0; i < numslots; i++)
		slots[i].page = NO_PAGE;
	last = 0;
	initialized = true;
}


void PG_AddPages(void* memory, uint32_t bytes)
{
	int16_t n = bytes / PG_PAGE_SIZE;
	if (n > MAX_SLOTS - numslots)
		n = MAX_SLOTS - numslots;
	if (n <= 0 || numchunks == MAX_CHUNKS)
		return;
	chunks[numchunks] = memory;
	for (int16_t i = numslots; i < numslots + n; i++)
		slots[i].page = NO_PAGE;
	numslots += n;
	chunk_end[numchunks++] = numslots;
#if defined FZ_MEMTRACE
	printf("page cache: %d + %d pages\n", PG_SLOTS, numslots - PG_SLOTS);
#endif
}


boolean PG_ReleasePages(void)
{
	if (!numchunks)
		return false;
	uint8_t* old[MAX_CHUNKS];
	const int16_t count = numchunks;
	memcpy(old, chunks, sizeof(old));
	PG_RemovePages();
	for (int16_t c = 0; c < count; c++)
		Z_Free(old[c]);
	return true;
}


void PG_RemovePages(void)
{
	numchunks = 0;
	numslots = PG_SLOTS;
	if (last >= PG_SLOTS)
		last = 0;
	memset(hint, 0, sizeof(hint));
}


#if defined FZ_PGTRACE
#include <stdio.h>
FILE* pg_trace;
#endif

static const uint8_t* PG_Page(uint16_t page)
{
#if defined FZ_PGTRACE
	if (pg_trace) fwrite(&page, 2, 1, pg_trace);
#endif
	if (!initialized)
		PG_Flush();

	if (slots[last].page == page)
	{
		slots[last].stamp = ++clock;
		return PG_Slot(last);
	}
	int8_t* h = &hint[page & (HINTS - 1)];
	if (*h < numslots && slots[*h].page == page)
	{
		last = *h;
		slots[last].stamp = ++clock;
		return PG_Slot(last);
	}

	int16_t victim = 0;
	for (int16_t i = 0; i < numslots; i++)
	{
		if (slots[i].page == page)
		{
			slots[i].stamp = ++clock;
			last = *h = i;
			return PG_Slot(i);
		}
		if (slots[i].page == NO_PAGE || (int16_t)(slots[i].stamp - slots[victim].stamp) < 0)
			victim = i;
	}

	uint32_t pos = (uint32_t)page * PG_PAGE_SIZE;
	uint16_t length = PG_PAGE_SIZE;
	if (pos + length > W_FileSize())
		length = W_FileSize() - pos;

	W_ReadRaw(pos, PG_Slot(victim), length);
	pg_misses++;
	pg_bytes += length;

	slots[victim].page  = page;
	slots[victim].stamp = ++clock;
	last = *h = victim;
	return PG_Slot(victim);
}


void PG_Read(uint32_t pos, void* dest, uint16_t length)
{
	uint8_t* d = dest;
	while (length)
	{
		uint16_t within = pos % PG_PAGE_SIZE;
		uint16_t chunk  = PG_PAGE_SIZE - within;
		if (chunk > length)
			chunk = length;

		memcpy(d, PG_Page(pos / PG_PAGE_SIZE) + within, chunk);
		d      += chunk;
		pos    += chunk;
		length -= chunk;
	}
}
