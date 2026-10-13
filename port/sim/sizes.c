// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <stddef.h>
#include "doomdef.h"
#include "d_player.h"
#include "r_defs.h"
#include "p_mobj.h"
int main(void){
 printf("mobj %zu sector %zu side %zu line %zu subsector %zu msecnode %zu\n", sizeof(mobj_t), sizeof(sector_t), sizeof(side_t), sizeof(line_t), sizeof(subsector_t), sizeof(msecnode_t));
#define O(f) printf("  %-20s %3zu %zu\n", #f, offsetof(mobj_t,f), sizeof(((mobj_t*)0)->f));
 O(thinker) O(x) O(snext) O(angle) O(sprite) O(frame) O(bnext) O(subsector) O(floorz) O(dropoffz) O(radius) O(height) O(momx) O(health) O(type) O(tics) O(state) O(flags) O(target) O(movedir) O(pursuecount) O(lastenemy) O(touching_sectorlist)
 return 0;}
