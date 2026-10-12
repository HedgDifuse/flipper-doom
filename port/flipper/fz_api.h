/*
 * flipper-doom: contract between the FAP (loader, runs from RAM like any app)
 * and the engine image (installed into internal flash, runs in place).
 *
 * The engine never calls the firmware directly: it gets this table of
 * functions from the loader. Its own globals are reached through r9, so the
 * loader must call into it via fz_call_engine().
 */

#ifndef FZ_API_H
#define FZ_API_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define FZ_API_VERSION 9

// Buttons, as a bit mask in fz_api_t.buttons()
enum
{
	FZ_BTN_UP    = 1 << 0,
	FZ_BTN_DOWN  = 1 << 1,
	FZ_BTN_LEFT  = 1 << 2,
	FZ_BTN_RIGHT = 1 << 3,
	FZ_BTN_OK    = 1 << 4,
	FZ_BTN_BACK  = 1 << 5,
};

typedef struct
{
	uint32_t version;

	void* (*malloc)(size_t size);
	void  (*free)(void* ptr);
	// the zone: separate pieces of memory reserved by the loader (see Z_Init)
	uint8_t  zone_regions;
	void*    zone_base[16];
	uint32_t zone_bytes[16];

	// WAD on the SD card; open returns its size, 0 if missing
	uint32_t (*wad_open)(void);
	void     (*wad_read)(uint32_t pos, void* dest, uint16_t length);

	uint32_t (*ticks_ms)(void);
	void     (*delay_ms)(uint32_t ms);

	// currently held buttons (FZ_BTN_*); bit 15 set = quit requested
	uint16_t (*buttons)(void);
	// buttons pressed since the last call (FZ_BTN_*), even if already released
	uint16_t (*buttons_pressed)(void);

	// The LCD gray frame (FZ_GRAY_FRAME_SIZE bytes, see platform/fz_dither.h):
	// 128x64 darkness levels 0..gray_period, 4 bits per pixel. The engine draws
	// into the loader's buffer between lock and unlock; the loader's display
	// thread flashes it onto the LCD as gray_period subframes.
	uint8_t* (*frame_lock)(void);
	void     (*frame_unlock)(void);
	uint8_t        gray_period;
	const uint8_t* lum_to_level; // 64 entries from fz_build_lum_to_level()

	uint8_t start_map;   // E1Mx
	uint8_t start_skill; // 1 (I'm too young to die) .. 5 (Nightmare!)

	int  (*vsnprintf)(char* str, size_t size, const char* format, va_list args);
	void (*log)(const char* message);
	// a sound: the DS* lump (DMX 8-bit PCM) at pos in the WAD; volume 0..120
	void (*sound)(uint32_t pos, uint16_t length, uint8_t volume);
	// a music track (musicenum_t: 1 = E1M1 ... 13 = INTROA), 0 = stop
	void (*music)(uint8_t song, uint8_t looping);
} fz_api_t;

#define FZ_QUIT_REQUESTED 0x8000


// Header in the first 64 bytes of the flash gap, written by tools/mkblob.py.
#define FZ_IMAGE_MAGIC   0x4d4f4f44 // "DOOM"
#define FZ_IMAGE_ADDRESS 0x080BD000
#define FZ_IMAGE_RAM_LINK 0x10000000 // placeholder RAM address the image is linked at

typedef struct
{
	uint32_t magic;
	uint32_t api_version;
	uint32_t image_size;  // bytes of flash used, header included
	uint32_t crc32;       // of the image after this header
	uint32_t entry;       // address of fz_engine_main (thumb bit set)
	uint32_t ram_load;    // flash address of the initial .got + .data
	uint32_t ram_init;    // bytes of .got + .data
	uint32_t ram_size;    // bytes of .got + .data + .bss
	uint32_t got_offset;  // r9 = RAM block + got_offset
	uint32_t relocs;      // flash address of the relocation list
	uint32_t reloc_count; // words in the RAM block that hold RAM addresses
	uint32_t reserved[5];
} fz_image_header_t;

typedef char assertImageHeaderSize[sizeof(fz_image_header_t) == 64 ? 1 : -1];

// Engine entry: runs the game until the player quits; returns 0 or an error.
typedef int (*fz_engine_main_t)(const fz_api_t* api);

#endif
