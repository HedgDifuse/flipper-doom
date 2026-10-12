/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *  Copyright 2023-2025 by
 *  Frenkel Smeijers
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *      LineOfSight/Visibility checks, uses REJECT Lookup Table.
 *
 *-----------------------------------------------------------------------------*/

#include "d_player.h"
#include "r_main.h"
#include "p_map.h"
#include "p_maputl.h"
#include "p_setup.h"

#include "globdata.h"
#include "z_zone.h"


typedef struct {
  fixed_t sightzstart, t2x, t2y;   // eye z of looker
  divline_t strace;                // from t1 to t2
  fixed_t topslope, bottomslope;   // slopes to top and bottom of target
  fixed_t bbox[4];
  fixed_t maxz,minz;               // cph - z optimisations for 2sided lines
} los_t;


static los_t los;


//
// P_DivlineSide
// Returns side 0 (front), 1 (back), or 2 (on).
//

static int8_t P_DivlineSide(fixed_t x, fixed_t y, const divline_t *node)
{
  fixed_t left, right;
  return
    !node->dx ? x == node->x ? 2 : x <= node->x ? node->dy > 0 : node->dy < 0 :
    !node->dy ? y == node->y ? 2 : y <= node->y ? node->dx < 0 : node->dx > 0 :
    (right = ((y - node->y) >> FRACBITS) * (node->dx >> FRACBITS)) <
    (left  = ((x - node->x) >> FRACBITS) * (node->dy >> FRACBITS)) ? 0 :
    right == left ? 2 : 1;
}


static uint16_t PUREFUNC P_InterceptVector2(const divline_t *v2, const divline_t *v1)
{
	fixed_t a = (v1->dy >> FRACBITS) * ((v1->x - v2->x) >> 8);
	fixed_t b = (v1->dx >> FRACBITS) * ((v2->y - v1->y) >> 8);

	fixed_t num = a + b;

	if (num == 0)
		return 0;

	fixed_t c = FixedMul(v2->dx, v1->dy >> 8);
	fixed_t d = FixedMul(v2->dy, v1->dx >> 8);

	fixed_t den = c - d;

	if (den == 0 || (den >> 12) == 0)
		return 0;

	fixed_t r = (num << 4) / (den >> 12);
	return (uint32_t)r <= 0xffffu ? r : 0xffffu;
}


//
// P_CrossSubsector
// Returns true
//  if strace crosses the given subsector successfully.
//

static boolean P_CrossSubsector(int16_t num)
{
    int16_t segnum = _g_subsectors[num].firstline;
    seg_t segbuf;
    const seg_t* seg = &segbuf;
    int16_t count;
    fixed_t opentop = 0, openbottom = 0;
    const sector_t __far* front = NULL;
    const sector_t __far* back  = NULL;

    for (count = _g_subsectors[num].numlines; --count >= 0; segnum++)
    { // check lines
        P_GetSeg(segnum, &segbuf);
        int16_t linenum = seg->linenum;

        line_t __far* line = &_g_lines[linenum];
        divline_t divl;

        // allready checked other side?
#if defined FLIPPER_DOOM
        if (P_LineChecked(line))
            continue;
#else
        if(line->validcount == validcount)
            continue;

        line->validcount = validcount;
#endif

        if ((fixed_t)LN_BBOX(line, BOXLEFT)<<FRACBITS > los.bbox[BOXRIGHT ] ||
            (fixed_t)LN_BBOX(line, BOXRIGHT)<<FRACBITS < los.bbox[BOXLEFT  ] ||
            (fixed_t)LN_BBOX(line, BOXBOTTOM)<<FRACBITS > los.bbox[BOXTOP   ] ||
            (fixed_t)LN_BBOX(line, BOXTOP)<<FRACBITS < los.bbox[BOXBOTTOM])
            continue;

        // cph - do what we can before forced to check intersection
        if (line->flags & ML_TWOSIDED)
        {

            // no wall to block sight with?
            front = &_g_sectors[seg->frontsectornum];
            back  = &_g_sectors[seg->backsectornum];
            if (front->floorheight == back->floorheight && front->ceilingheight == back->ceilingheight)
                continue;

            // possible occluder
            // because of ceiling height differences
            opentop = front->ceilingheight < back->ceilingheight ?
                        front->ceilingheight : back->ceilingheight ;

            // because of floor height differences
            openbottom = front->floorheight > back->floorheight ?
                        front->floorheight : back->floorheight ;

            // cph - reject if does not intrude in the z-space of the possible LOS
            if ((opentop >= los.maxz) && (openbottom <= los.minz))
                continue;
        }

        // Forget this line if it doesn't cross the line of sight
        fixed_t v1x = (fixed_t)LN_V1(line).x<<FRACBITS;
        fixed_t v1y = (fixed_t)LN_V1(line).y<<FRACBITS;
        fixed_t v2x = (fixed_t)LN_V2(line).x<<FRACBITS;
        fixed_t v2y = (fixed_t)LN_V2(line).y<<FRACBITS;

        if (P_DivlineSide(v1x, v1y, &los.strace) == P_DivlineSide(v2x, v2y, &los.strace))
            continue;

        divl.x = v1x;
        divl.y = v1y;
        divl.dx = v2x - v1x;
        divl.dy = v2y - v1y;

        // line isn't crossed?
        if (P_DivlineSide(los.strace.x, los.strace.y, &divl) == P_DivlineSide(los.t2x, los.t2y, &divl))
            continue;


        // cph - if bottom >= top or top < minz or bottom > maxz then it must be
        // solid wrt this LOS
        if (!(line->flags & ML_TWOSIDED) || (openbottom >= opentop) ||
                (opentop < los.minz) || (openbottom > los.maxz))
            return false;

        // crosses a two sided line
        uint16_t frac = P_InterceptVector2(&los.strace, &divl);

        if (front->floorheight != back->floorheight)
        {
            fixed_t slope = frac != 0 ? ((openbottom - los.sightzstart) >> FRACBITS) * FixedReciprocalSmall(frac) : INT32_MAX;
            if (slope > los.bottomslope)
                los.bottomslope = slope;
        }

        if (front->ceilingheight != back->ceilingheight)
        {
            fixed_t slope = frac != 0 ? ((opentop - los.sightzstart) >> FRACBITS) * FixedReciprocalSmall(frac) : INT32_MAX;
            if (slope < los.topslope)
                los.topslope = slope;
        }

        if (los.topslope <= los.bottomslope)
            return false;               // stop

    }
    // passed the subsector ok
    return true;
}


static boolean P_CrossBSPNode(int16_t bspnum)
{
    while (!(bspnum & NF_SUBSECTOR))
    {
        mapnode_t node;
        R_GetNode(bspnum, &node);
        const mapnode_t* bsp = &node;

        divline_t dl;
        dl.x = ((fixed_t)bsp->x << FRACBITS);
        dl.y = ((fixed_t)bsp->y << FRACBITS);
        dl.dx = ((fixed_t)bsp->dx << FRACBITS);
        dl.dy = ((fixed_t)bsp->dy << FRACBITS);

        int8_t side,side2;
        side = P_DivlineSide(los.strace.x,los.strace.y,&dl)&1;
        side2= P_DivlineSide(los.t2x, los.t2y, &dl);

        if (side == side2)
            bspnum = bsp->children[side]; // doesn't touch the other side
        else         // the partition plane is crossed here
            if (!P_CrossBSPNode(bsp->children[side]))
                return false;  // cross the starting side
            else
                bspnum = bsp->children[side^1];  // cross the ending side
    }
    return P_CrossSubsector(bspnum == -1 ? 0 : bspnum & ~NF_SUBSECTOR);
}


static uint32_t linearAddress(const void __far* ptr)
{
#if defined _M_I86
	uint32_t seg = D_FP_SEG(ptr);
	uint16_t off = D_FP_OFF(ptr);

	return seg * 16 + off;
#else
	return (uint32_t)ptr;
#endif
}


//
// P_CheckSight
// Returns true
//  if a straight line between t1 and t2 is unobstructed.
// Uses REJECT.
//
// killough 4/20/98: cleaned up, made to use new LOS struct

#if defined FLIPPER_DOOM
// flipper-doom: REJECT is paged, and nearly every sight check is a monster
// looking for the player: read for every sector in a row from scattered
// pages (34000 page reads in 3000 frames on E1M6). So the REJECT column of
// the player's sector, a bit per sector, is kept and only rebuilt when the
// player enters another sector. Same answers.
static boolean P_Rejected(uint16_t pnum)
{
  uint8_t reject;
  PG_Read(_g_rejectpos + pnum / 8, &reject, 1);
  return reject & (1 << (pnum & 7));
}

static boolean P_RejectedPlayer(int16_t s1, int16_t s2)
{
  static uint32_t column_map;   // _g_rejectpos of the map it is for
  static int16_t column_sector; // s2 it is for
  static uint8_t column[256 / 8];

  if (column_map != _g_rejectpos || column_sector != s2)
  {
    for (int16_t s = 0; s < _g_numsectors; s++)
    {
      const uint8_t bit = 1 << (s & 7);
      if (P_Rejected((uint16_t)(s * _g_numsectors + s2)))
        column[s >> 3] |= bit;
      else
        column[s >> 3] &= ~bit;
    }
    column_map = _g_rejectpos;
    column_sector = s2;
  }
  return column[s1 >> 3] & (1 << (s1 & 7));
}
#endif

#if defined FLIPPER_DOOM && SIGHT_MEMO_TICS > 0
// flipper-doom: whether a monster sees the player is remembered for
// SIGHT_MEMO_TICS tics (a big level's line of sight walks NODES, SEGS and
// LINEDEFS on the SD card). A monster notices the player, or loses sight of
// him, at most that much later; Doom's own reaction time is 8 tics and more.
#if !defined SIGHT_MEMO_IDLE_TICS
#define SIGHT_MEMO_IDLE_TICS SIGHT_MEMO_TICS
#endif

// 64 entries, 4 bytes each: the looker as a zone short pointer, the tic (15
// bits) and the answer; a looker may sit in its slot or the next (E1M3 has
// 33 monsters looking on the same tics)
#define SIGHT_MEMO_SLOTS 64
static uint32_t sightmemo[SIGHT_MEMO_SLOTS];

#define MEMO_LOOKER(e) ((uint16_t)(e))
#define MEMO_TIC(e)    (((e) >> 16) & 0x7fff)
#define MEMO_SEEN      0x80000000u

static boolean P_CheckSightNow(mobj_t __far* t1, mobj_t __far* t2);

boolean P_CheckSight(mobj_t __far* t1, mobj_t __far* t2)
{
  if (t2 != _g_player.mo)
    return P_CheckSightNow(t1, t2);

  // a monster that has not seen the player yet looks again later than one
  // that is after him
  const uint16_t memo = t1->target == t2 ? SIGHT_MEMO_TICS : SIGHT_MEMO_IDLE_TICS;
  const uint16_t looker = Z_PointerToShort(t1);
  const uint16_t now = _g_leveltime & 0x7fff;
  const uint16_t slot = (uint16_t)(looker * 40503u) >> 10 & (SIGHT_MEMO_SLOTS - 1);
  const uint16_t other = (slot + 1) & (SIGHT_MEMO_SLOTS - 1);

  for (uint16_t s = slot; ; s = other)
  {
    const uint32_t e = sightmemo[s];
    if (MEMO_LOOKER(e) == looker && ((now - MEMO_TIC(e)) & 0x7fff) < memo)
      return (e & MEMO_SEEN) != 0;
    if (s == other)
      break;
  }

  const boolean seen = P_CheckSightNow(t1, t2);
  // into the slot this looker had, else the older one
  uint16_t s = slot;
  if (MEMO_LOOKER(sightmemo[other]) == looker
   || (MEMO_LOOKER(sightmemo[slot]) != looker
    && ((now - MEMO_TIC(sightmemo[other])) & 0x7fff) > ((now - MEMO_TIC(sightmemo[slot])) & 0x7fff)))
    s = other;
  sightmemo[s] = looker | (uint32_t)now << 16 | (seen ? MEMO_SEEN : 0);
  return seen;
}

static boolean P_CheckSightNow(mobj_t __far* t1, mobj_t __far* t2)
#else
boolean P_CheckSight(mobj_t __far* t1, mobj_t __far* t2)
#endif
{
  static uint32_t prevlat1;
  static uint32_t prevlat2;
  static boolean prevr;

  if (prevlat1 == linearAddress(t1)
   && prevlat2 == linearAddress(t2))
    return prevr;
  prevlat1 = linearAddress(t1);
  prevlat2 = linearAddress(t2);

  const sector_t __far* s1 = SS_SECTOR(t1->subsector);
  const sector_t __far* s2 = SS_SECTOR(t2->subsector);
  int16_t pnum = (s1-_g_sectors)*_g_numsectors + (s2-_g_sectors);

  // First check for trivial rejection.
  // Determine subsector entries in REJECT table.
  //
  // Check in REJECT table.

#if defined FLIPPER_DOOM
  if (t2->type == MT_PLAYER ? P_RejectedPlayer(s1 - _g_sectors, s2 - _g_sectors) : P_Rejected(pnum))
#else
  uint8_t reject;
  PG_Read(_g_rejectpos + (uint16_t)pnum / 8, &reject, 1);
  if (reject & (1 << (pnum&7)))   // can't possibly be connected
#endif
  {
    prevr = false;
    return prevr;
  }

  /* killough 11/98: shortcut for melee situations
   * same subsector? obviously visible
   * cph - compatibility optioned for demo sync, cf HR06-UV.LMP */
  if (t1->subsector == t2->subsector)
  {
    prevr = true;
    return prevr;
  }

  // An unobstructed LOS is possible.
  // Now look from eyes of t1 to any part of t2.

  validcount++;

  los.topslope = (los.bottomslope = t2->z - (los.sightzstart =
                                             t1->z + t1->height -
                                             (t1->height>>2))) + t2->height;
  los.strace.dx = (los.t2x = t2->x) - (los.strace.x = t1->x);
  los.strace.dy = (los.t2y = t2->y) - (los.strace.y = t1->y);

  if (t1->x > t2->x)
    los.bbox[BOXRIGHT] = t1->x, los.bbox[BOXLEFT] = t2->x;
  else
    los.bbox[BOXRIGHT] = t2->x, los.bbox[BOXLEFT] = t1->x;

  if (t1->y > t2->y)
    los.bbox[BOXTOP] = t1->y, los.bbox[BOXBOTTOM] = t2->y;
  else
    los.bbox[BOXTOP] = t2->y, los.bbox[BOXBOTTOM] = t1->y;


    los.maxz = INT32_MAX; los.minz = INT32_MIN;

  // the head node is the last node output
#if defined FZ_READSTATS
  extern int16_t fz_reading_for; fz_reading_for = 1;
#endif
#if defined FZ_READSTATS
  { extern uint32_t fz_sight_by[2][256]; fz_sight_by[t2 == _g_player.mo][t1->type & 255]++; extern char* getenv(const char*); extern int printf(const char*, ...); if (getenv("FZ_SIGHTLOG")) printf("SIGHT %ld %p %d %d\n", (long)_g_leveltime, (void*)t1, t1->type, t1->target == t2); }
#endif
  prevr = P_CrossBSPNode(numnodes-1);
#if defined FZ_READSTATS
  fz_reading_for = 0;
#endif
  return prevr;
}
