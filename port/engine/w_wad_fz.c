/*
 * flipper-doom: WAD file access with the directory left on the SD card.
 *
 * Replaces w_wad.c. The original keeps the whole directory in the zone
 * (16 bytes per lump); here only a 16-bit hash of each lump name stays in RAM,
 * entries are read through the demand pager, and a name lookup is confirmed
 * with one read of the candidate entry.
 *
 * Based on w_wad.c from Doom8088 by Frenkel Smeijers (GPL-2.0-or-later).
 */

#include <stdint.h>
#include <string.h>

#include "compiler.h"
#include "doomtype.h"
#include "i_system.h"
#include "z_zone.h"

#include "w_wad.h"

#include "fz_pager.h"
#include "fz_platform.h"


typedef struct
{
	int32_t  filepos;
	uint16_t size;
	int16_t  filler;        // always zero
	char     name[8];
} filelump_t;

typedef char assertFilelumpSize[sizeof(filelump_t) == 16 ? 1 : -1];

typedef struct
{
	char    identification[4]; // Should be "IWAD" or "PWAD".
	int16_t numlumps;
	int16_t filler;            // always zero
	int32_t infotableofs;
} wadinfo_t;


static int16_t numlumps;
static uint32_t infotableofs;
static uint32_t filesize;

static uint16_t __far* namehash;
// Lumps loaded into the zone. A pointer for each of the ~1000 lumps took 4 KB
// of the zone; only a few dozen are ever cached at once. The zone clears ptr
// when it purges the block.
#define LUMPCACHE_SLOTS 64

typedef struct
{
	int16_t num;
	uint16_t frame;  // last drawn in this frame (W_NewFrame)
	void __far* ptr; // NULL = free slot
} lumpslot_t;

static lumpslot_t lumpcache[LUMPCACHE_SLOTS];
static uint16_t frame;


void W_NewFrame(void)
{
	frame++;
}


static lumpslot_t* W_FindCached(int16_t num)
{
	for (int16_t i = 0; i < LUMPCACHE_SLOTS; i++)
		if (lumpcache[i].ptr && lumpcache[i].num == num)
			return &lumpcache[i];
	return NULL;
}


// A free slot, or one whose lump nobody holds (thrown out of the zone for it).
static lumpslot_t* W_FreeSlot(void)
{
	static int16_t next;
	for (int16_t i = 0; i < LUMPCACHE_SLOTS; i++)
		if (!lumpcache[i].ptr)
			return &lumpcache[i];

	for (int16_t i = 0; i < LUMPCACHE_SLOTS; i++)
	{
		lumpslot_t* slot = &lumpcache[next];
		next = (next + 1) % LUMPCACHE_SLOTS;
		if (Z_IsCache(slot->ptr))
		{
			Z_Free(slot->ptr); // clears slot->ptr
			return slot;
		}
	}

	I_Error("W_FreeSlot: %d lumps in use", LUMPCACHE_SLOTS);
	return NULL;
}


static uint16_t W_HashName(const char* name8)
{
	uint16_t h = 0x811c;
	for (int16_t i = 0; i < 8; i++)
		h = (h ^ (uint8_t)name8[i]) * 0x0193;
	return h;
}


static void W_ReadEntry(int16_t num, filelump_t* entry)
{
	PG_Read(infotableofs + (uint32_t)num * sizeof(filelump_t), entry, sizeof(filelump_t));
}


void W_ReadRaw(uint32_t pos, void __far* dest, uint16_t length)
{
	fz_wad_read(pos, dest, length);
}


uint32_t W_FileSize(void)
{
	return filesize;
}


void W_Init(void)
{
	filesize = fz_wad_open();
	if (filesize == 0)
		I_Error("Can't open WAD");

	wadinfo_t header;
	W_ReadRaw(0, &header, sizeof(header));
	numlumps     = header.numlumps;
	infotableofs = header.infotableofs;

	namehash = Z_MallocStatic(numlumps * sizeof(*namehash));
	for (int16_t i = 0; i < numlumps; i++)
	{
		filelump_t entry;
		W_ReadEntry(i, &entry);
		namehash[i] = W_HashName(entry.name);
	}
	PG_Flush();
}


void W_Shutdown(void)
{
}


// The pointer stays valid until the second call after this one.
const char __far* W_GetNameForNum(int16_t num)
{
	static char names[2][8];
	static uint8_t next;

	filelump_t entry;
	W_ReadEntry(num, &entry);
	next ^= 1;
	memcpy(names[next], entry.name, 8);
	return names[next];
}


uint16_t PUREFUNC W_LumpLength(int16_t num)
{
	filelump_t entry;
	W_ReadEntry(num, &entry);
	return entry.size;
}


uint32_t W_LumpPos(int16_t num)
{
	filelump_t entry;
	W_ReadEntry(num, &entry);
	return entry.filepos;
}


int16_t PUREFUNC W_GetNumForName(const char *name)
{
	char name8[8];
	strncpy(name8, name, sizeof(name8));
	const uint16_t h = W_HashName(name8);

	for (int16_t i = 0; i < numlumps; i++)
	{
		if (namehash[i] != h)
			continue;

		filelump_t entry;
		W_ReadEntry(i, &entry);
		if (memcmp(entry.name, name8, 8) == 0)
			return i;
	}

	I_Error("W_GetNumForName: %.8s not found", name);
	return -1;
}


void W_ReadLumpByNum(int16_t num, void __far* ptr)
{
	filelump_t entry;
	W_ReadEntry(num, &entry);
	W_ReadRaw(entry.filepos, ptr, entry.size);
}


const void __far* PUREFUNC W_GetLumpByNumAutoFree(int16_t num)
{
	filelump_t entry;
	W_ReadEntry(num, &entry);

	void __far* ptr = Z_MallocLevel(entry.size, NULL);
	W_ReadRaw(entry.filepos, ptr, entry.size);
	return ptr;
}


static void __far* PUREFUNC W_GetLumpByNumWithUser(int16_t num, void __far*__far* user)
{
	filelump_t entry;
	W_ReadEntry(num, &entry);

#if defined FZ_ZTRACE
	printf("LUMP %.8s %u\n", entry.name, entry.size);
#endif
	void __far* ptr = Z_MallocStaticWithUser(entry.size, user);
	W_ReadRaw(entry.filepos, ptr, entry.size);
	return ptr;
}


int16_t W_GetFirstInt16(int16_t num)
{
	int16_t firstInt16;
	PG_Read(W_LumpPos(num), &firstInt16, sizeof(int16_t));
	return firstInt16;
}


const void __far* PUREFUNC W_GetLumpByNum(int16_t num)
{
	lumpslot_t* slot = W_FindCached(num);
	if (slot)
		Z_ChangeTagToStatic(slot->ptr);
	else
	{
		slot = W_FreeSlot();
		slot->num = num;
		slot->ptr = W_GetLumpByNumWithUser(num, &slot->ptr);
	}

	return slot->ptr;
}


boolean PUREFUNC W_IsLumpCached(int16_t num)
{
	return W_FindCached(num) != NULL;
}


// flipper-doom: a lump may not throw out one drawn in this or the last
// frame. When the sprites in view do not all fit, the zone kept loading each
// one over another, every frame (13-35 SD reads a frame on E1M2); now those
// that fit stay and the rest go a column at a time through the page cache.
const void __far* PUREFUNC W_TryGetLumpByNum(int16_t num)
{
	lumpslot_t* slot = W_FindCached(num);
	if (slot)
	{
		slot->frame = frame;
		Z_ChangeTagToStatic(slot->ptr);
		return slot->ptr;
	}

	uint8_t locked[(LUMPCACHE_SLOTS + 7) / 8] = {0};
	for (int16_t i = 0; i < LUMPCACHE_SLOTS; i++)
		if (lumpcache[i].ptr && (uint16_t)(frame - lumpcache[i].frame) <= 1 && Z_IsCache(lumpcache[i].ptr))
		{
			Z_ChangeTagToStatic(lumpcache[i].ptr);
			locked[i / 8] |= 1 << (i % 8);
		}

	const void __far* ptr = NULL;
	if (Z_IsEnoughFreeMemory(W_LumpLength(num)))
	{
		ptr = W_GetLumpByNum(num);
		W_FindCached(num)->frame = frame;
	}

	for (int16_t i = 0; i < LUMPCACHE_SLOTS; i++)
		if (locked[i / 8] & (1 << (i % 8)))
			Z_ChangeTagToCache(lumpcache[i].ptr);
	return ptr;
}
