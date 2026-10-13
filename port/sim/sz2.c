// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <stddef.h>
#include "doomdef.h"
#include "d_player.h"
#include "p_mobj.h"
int main(void){ printf("mobj %zu static %zu\n", sizeof(mobj_t), (size_t)STATIC_MOBJ_SIZE); return 0; }
