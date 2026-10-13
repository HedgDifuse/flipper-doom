// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
// flipper-doom: health and ammo in the columns right of the view, see fz_hud.c.
#ifndef FZ_HUD_H
#define FZ_HUD_H

#include <stdint.h>

// out: the gray LCD frame from fz_dither(); darkest: its darkest level
void fz_hud_draw(uint8_t* out, uint8_t darkest);

#endif
