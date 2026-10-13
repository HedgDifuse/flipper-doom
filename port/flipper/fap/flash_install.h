// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
// flipper-doom: writing the engine image into the free internal flash gap.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <storage/storage.h>

#include "../fz_api.h"

typedef enum
{
	FlashImageOk,       // installed image matches the file
	FlashImageMissing,  // nothing (or another version) installed
	FlashImageBadFile,  // the file on the SD card is not a valid image
} FlashImageState;

// Compare the installed image with the image file (header + CRC). With
// read_flash false only the file is checked and the image counts as missing.
FlashImageState flash_image_check(File* file, fz_image_header_t* file_header, bool read_flash);

// Is it safe to write the gap on this firmware? (its code must end below it)
bool flash_gap_is_free(const char** why);

typedef void (*FlashProgress)(void* context, uint32_t done, uint32_t total);

// Erase the pages and program the image from file. Bluetooth must be off.
bool flash_image_install(File* file, const fz_image_header_t* header, FlashProgress progress, void* context, const char** error);
