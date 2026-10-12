// flipper-doom: interface between the Doom8088 engine glue and the platform
// (simulator on PC, Flipper Zero on device).
#ifndef FZ_PLATFORM_H
#define FZ_PLATFORM_H

#include <stdint.h>
#include "doomdef.h"

// Render buffer = the 3D view. Full-screen 2D graphics (menus, intermission)
// are scaled from SCREENWIDTH x SCREENHEIGHT into it.
#define FZ_W VIEWWINDOWWIDTH
#define FZ_H VIEWWINDOWHEIGHT

// Each pixel is one byte: a 6-bit luminance (the colormap is a luminance table,
// see tools/mklum.py) and, in the top 2 bits, what the renderer drew there.
// The class drives the 1-bit conversion.
enum { FZ_CLASS_NONE, FZ_CLASS_WALL, FZ_CLASS_FLAT, FZ_CLASS_SPRITE }; // NONE: sky, 2D
#define FZ_CLASS_SHIFT 6
#define FZ_LUM_MASK    0x3f
#define FZ_PIXEL(lum, cls) ((uint8_t)((lum) | ((cls) << FZ_CLASS_SHIFT)))

// WAD file on the SD card: open returns its size (0 = missing)
uint32_t fz_wad_open(void);
void fz_wad_read(uint32_t pos, void* dest, uint16_t length);

// The zone: any number of separate pieces of memory, see Z_Init.
#define FZ_ZONE_MAX_REGIONS 16

typedef struct
{
	void*    base;
	uint32_t size;
} fz_zone_region_t;

// Fills regions (up to FZ_ZONE_MAX_REGIONS) and returns how many.
int16_t fz_zone_regions(fz_zone_region_t* regions);

// Play the digitized sound in lump num (DS*, 8-bit PCM); volume 0..120.
void fz_sound_play(int16_t num, int16_t volume);

// Play music track song (musicenum_t, mus_None = stop), pre-rendered from the
// MUS lumps by tools/mkmusic.py.
void fz_music_play(int16_t song, boolean looping);

void fz_init_video(void);
void fz_present(const uint8_t* screen, int8_t palette);

#endif
