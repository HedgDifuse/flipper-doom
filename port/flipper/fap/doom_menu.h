// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Doom's menu on the loader's screen: New Game (level, skill), Options
// (sound volumes, screen calibration), Read This!, Quit Game, over the
// recorded gameplay, with Doom's menu sounds and title music.
#include <furi.h>
#include <storage/storage.h>

#include "sound.h"
#include "ui.h"

typedef enum
{
	MenuActionPlay,      // map and skill in MenuChoice
	MenuActionCalibrate, // run the calibration, then the menu again (it comes back to Options)
	MenuActionQuit,
} MenuAction;

typedef struct
{
	uint8_t map;   // 1..9
	uint8_t skill; // 1..5
} MenuChoice;

typedef struct
{
	Ui* ui;
	uint8_t* frame;
	FuriMessageQueue* input;
	Storage* storage;
	Sound* sound;
	void (*play)(void* context, const char* lump); // a DS* sound by name
	void* context;
} MenuEnv;

MenuAction doom_menu_run(const MenuEnv* env, MenuChoice* choice);
