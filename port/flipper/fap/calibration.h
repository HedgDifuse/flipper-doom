// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
// flipper-doom: per-device screen calibration for the gray display.
#pragma once

#include <furi.h>
#include <storage/storage.h>

#include "gray_display.h"

#define GRAY_CONFIG_DEFAULT ((GrayConfig){.rate10 = 750, .period = 4, .gamma = 1.0f}) // ST7567 typical 75 Hz

// false if there is no saved calibration (config is left untouched)
bool gray_config_load(Storage* storage, GrayConfig* config);
void gray_config_save(Storage* storage, const GrayConfig* config);

// Interactive, three steps: subframe rate, number of levels, gamma.
// Reads InputEvents from `input`; saves and returns true on completion,
// returns false (config restored) if the user backs out.
bool calibration_run(GrayDisplay* display, uint8_t* frame, GrayConfig* config, FuriMessageQueue* input, Storage* storage);

// Draw the test pattern: level bars (top) and a tone gradient (bottom).
void calibration_pattern(uint8_t* frame, const GrayConfig* config, bool full_gradient);
