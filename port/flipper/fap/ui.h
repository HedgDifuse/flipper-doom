// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Doom-style screens of the loader (graphics from doom1.wad, see
// tools/mkui.py): a light page, or recorded gameplay washed out behind the
// menu (tools/mkbg.py), the logo, Doom's font with a light halo, the skull.
// A screen is composed in scratch memory (the engine's zone, idle while the
// menus run) and copied to the gray frame at once: no half-drawn subframes.
#include <stdbool.h>
#include <stdint.h>

#include <storage/storage.h>

#include "gray_display.h"

#define UI_SCRATCH (128 * 64 + 128 * 64 / 2 + 1024) // bytes ui_open needs

typedef struct Ui Ui;

Ui* ui_open(void* scratch, Storage* storage, const GrayConfig* gray);
void ui_close(Ui* ui);

// starts a screen: white, or the next frame of the background recording
// (white if there is none)
void ui_clear(Ui* ui, bool background);

typedef enum
{
	UiLogoBig,   // 123x60
	UiLogoSmall, // 60x29
} UiLogo;
int ui_logo_height(UiLogo logo);
void ui_logo(Ui* ui, UiLogo logo, int x, int y);

int ui_text_width(const char* text);
// dark letters with a white halo; returns where the text ends
int ui_text(Ui* ui, int x, int y, const char* text);
void ui_text_center(Ui* ui, int y, const char* text);
// a box, brightness 0 (black) .. 15 (white)
void ui_box(Ui* ui, int x, int y, int w, int h, uint8_t brightness);
void ui_draw_skull(Ui* ui, int x, int y, int frame);
// fraction 0..1000
void ui_progress(Ui* ui, int x, int y, int w, int permille);

// onto the screen; mono: black and white only, for screens up while the CPU
// or the SD card is busy (late subframes make grays flicker)
void ui_present(Ui* ui, uint8_t* frame, bool mono);

#define UI_FONT_LINE 8 // pixels between text lines
