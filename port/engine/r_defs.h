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
 *  Copyright 2023-2026 by
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
 *      Refresh/rendering module, shared data struct definitions.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __R_DEFS__
#define __R_DEFS__

// Screenwidth.
#include "doomdef.h"

// Some more or less basic data types
// we depend on.
#include "m_fixed.h"

// We rely on the thinker data struct
// to handle sound origins in sectors.
#include "d_think.h"

// SECTORS do store MObjs anyway.
#include "p_mobj.h"


//
// INTERNAL MAP TYPES
//  used by play and refresh
//

//
// Your plain vanilla vertex.
// Note: transformed values not buffered locally,
// like some DOOM-alikes ("wt", "WebView") do.
//
typedef struct
{
  fixed_t x, y;
} vertex_t;

typedef struct
{
  int16_t x, y;
} vertex16_t;

// Each sector has a degenmobj_t in its center for sound origin purposes.
typedef struct
{
  fixed_t x;
  fixed_t y;
} degenmobj_t;

//
// The SECTORS record, at runtime.
// Stores things/mobjs.
//

#if defined FLIPPER_DOOM
// flipper-doom: a sector's sound origin in map units (S_StartSound2)
typedef struct
{
  int16_t x, y;
} sndorg_t;

// flipper-doom: 36 bytes instead of 60: what never changes is read from the
// map's SEC lump, the fields are by size, flat numbers and specials in a byte
// (as the WAD has them; lightlevel stays 16 bits: the light effects go below
// their minimum and back before they compare)
typedef struct
{
  fixed_t floorheight;
  fixed_t ceilingheight;

  mobj_t __far* soundtarget;   // thing that made a sound (or null)
  mobj_t __far* thinglist;     // list of mobjs in sector

  // thinker_t for reversable actions
  void __far* floordata;    // jff 2/22/98 make thinkers on
  void __far* ceilingdata;  // floors, ceilings, lighting,

  // its lines, sound origin and blockmap cells never change: they are in the
  // map's SEC lump (tools/fzsec.py), see SEC_LINE and P_SectorInfo
  uint16_t linefirst; // where its lines start in the lump's list

  uint16_t validcount;        // if == validcount, already checked
  int16_t linecount;
  int16_t lightlevel;
  int16_t tag;

  uint8_t floorpic;
  uint8_t ceilingpic;
  int8_t special;
  int8_t oldspecial;      //jff 2/16/98 remembers if sector WAS secret (automap)
  uint8_t soundtraversed;    // 0 = untraversed, 1,2 = sndlines-1
} sector_t;
#else
typedef struct
{
  fixed_t floorheight;
  fixed_t ceilingheight;

  mobj_t __far* soundtarget;   // thing that made a sound (or null)
  degenmobj_t soundorg;  // origin for any sounds played by the sector
  uint16_t validcount;        // if == validcount, already checked
  mobj_t __far* thinglist;     // list of mobjs in sector


  // thinker_t for reversable actions
  void __far* floordata;    // jff 2/22/98 make thinkers on
  void __far* ceilingdata;  // floors, ceilings, lighting,

  // list of mobjs that are at least partially in the sector
  // thinglist is a subset of touching_thinglist
#if defined FLIPPER_DOOM
  // flipper-doom: the blockmap cells the sector covers, widened by MAXRADIUS,
  // for P_CheckSector (Doom 1.9's way instead of the touching lists)
  uint8_t blockbox[4];
#else
  struct msecnode_s __far* touching_thinglist;
#endif

  // flipper-doom: line numbers instead of pointers (half the size),
  // the line is SEC_LINE(sector, i)
  uint16_t __far* linenums;

  int16_t linecount;

  int16_t floorpic;
  int16_t ceilingpic;

  int16_t lightlevel;
  int16_t special;
  int16_t oldspecial;      //jff 2/16/98 remembers if sector WAS secret (automap)
  int16_t tag;

  int16_t soundtraversed;    // 0 = untraversed, 1,2 = sndlines-1

} sector_t;
#endif

//
// The SideDef.
//

// flipper-doom: 8 bytes instead of 16. The Doom8088 WAD format already
// limits sectors and textures to 256 (mapsidedef_t), so an index and byte
// texture numbers are enough; the sector pointer is SD_SECTOR(side).
typedef struct
{
    int16_t textureoffset; // add this to the calculated texture column
    int16_t rowoffset;     // add this to the calculated texture top

    uint8_t sectornum;     // Sector the SideDef is facing.
    uint8_t toptexture;
    uint8_t bottomtexture;
    uint8_t midtexture;
} side_t;

#define SD_SECTOR(side) (&_g_sectors[(side)->sectornum])
#if defined FLIPPER_DOOM
uint16_t P_SectorLine(const sector_t __far* sector, int16_t i);
#define SEC_LINE(sec, i) (&_g_lines[P_SectorLine(sec, i)])

// the sound origin and the blockmap cells (BOXTOP, ...) of a sector
void P_SectorInfo(const sector_t __far* sector, sndorg_t* soundorg, uint8_t* blockbox);
#else
#define SEC_LINE(sec, i) (&_g_lines[(sec)->linenums[i]])
#endif

//
// Move clipping aid for LineDefs.
//

#define ST_HORIZONTAL	0
#define ST_VERTICAL		1
#define ST_POSITIVE		2
#define ST_NEGATIVE		3

typedef enum
{                 // cph:
    RF_TOP_TILE  = 1,     // Upper texture needs tiling
    RF_MID_TILE = 2,     // Mid texture needs tiling
    RF_BOT_TILE = 4,     // Lower texture needs tiling
    RF_IGNORE   = 8,     // Renderer can skip this line
    RF_CLOSED   =16,     // Line blocks view
    RF_MAPPED   =32      // Seen so show on automap.
} r_flags;


// flipper-doom: what never changes in a line (its vertices, sides and tag)
// is not kept in memory but read from the LINEDEFS lump through the page
// cache when needed, see P_LineRO.
typedef struct
{
    vertex16_t v1;
    vertex16_t v2;     // Vertices, from v1 to v2.
    uint16_t sidenum[2];        // Visual appearance: SideDefs.
    int16_t tag;
} line_ro_t;

#if defined FLIPPER_DOOM
// flipper-doom: 2 bytes. validcount is a bit per line (P_LineChecked), the
// renderer's r_flags are worked out for every seg (R_LineFlags) instead of
// kept per line, the automap's "mapped" bit is gone with the automap, and
// special fits in the int8 of the WAD's lines.
typedef struct line_s
{
    uint8_t flags;           // Animation related.
    int8_t special;
} line_t;

// Was the line already checked with this validcount? Marks it if not.
boolean P_LineChecked(const line_t __far* line);
#else
typedef struct line_s
{
    uint8_t flags;           // Animation related.

    uint16_t validcount;        // if == validcount, already checked
    uint16_t r_validcount;      // cph: if == gametic, r_flags already done

    int16_t r_flags;
    int16_t special;
} line_t;
#endif


line_ro_t P_LineRO(const line_t __far* l);

// a line made up by the code (A_BossDeath), with only a tag
extern const line_t __far* _g_fakeline;
extern int16_t _g_fakeline_tag;

#define LN_V1(l)         (P_LineRO(l).v1)
#define LN_V2(l)         (P_LineRO(l).v2)
#define LN_SIDENUM(l, s) (P_LineRO(l).sidenum[s])
#define LN_TAG(l)        (P_LineRO(l).tag)

#define LN_FRONTSECTOR(l) (SD_SECTOR(&_g_sides[LN_SIDENUM(l, 0)]))
#define LN_BACKSECTOR(l) (LN_SIDENUM(l, 1) != NO_INDEX ? SD_SECTOR(&_g_sides[LN_SIDENUM(l, 1)]) : NULL)

#define LN_SPECIAL(l) ((l)->special)

// flipper-doom: dx, dy, the bounding box and the slope type of a line were
// stored in line_t (13 of its 36 bytes); they follow from the vertices.
int16_t LN_DX(const struct line_s __far* l);
int16_t LN_DY(const struct line_s __far* l);
int16_t LN_BBOX(const struct line_s __far* l, int16_t box);
int8_t LN_SLOPETYPE(const struct line_s __far* l);


// phares 3/14/98
//
// Sector list node showing all sectors an object appears in.
//
// There are two threads that flow through these nodes. The first thread
// starts at touching_thinglist in a sector_t and flows through the m_snext
// links to find all mobjs that are entirely or partially in the sector.
// The second thread starts at touching_sectorlist in an mobj_t and flows
// through the m_tnext links to find all sectors a thing touches. This is
// useful when applying friction or push effects to sectors. These effects
// can be done as thinkers that act upon all objects touching their sectors.
// As an mobj moves through the world, these nodes are created and
// destroyed, with the links changed appropriately.
//
// For the links, NULL means top or end of list.

typedef struct msecnode_s
{
  sector_t          __far* m_sector; // a sector containing this object
  struct mobj_s __far* m_thing;  // this object
  struct msecnode_s __far* m_tprev;  // prev msecnode_t for this thing
  struct msecnode_s __far* m_tnext;  // next msecnode_t for this thing
  struct msecnode_s __far* m_sprev;  // prev msecnode_t for this sector
  struct msecnode_s __far* m_snext;  // next msecnode_t for this sector
  boolean visited; // killough 4/4/98, 4/7/98: used in search algorithms
} msecnode_t;

//
// The LineSeg.
//

/*
typedef struct
{
  vertex_t *v1, *v2;
  fixed_t offset;
  angle_t angle;
  side_t* sidedef;
  const line_t* linedef;

  // Sector references.
  // Could be retrieved from linedef, too
  // (but that would be slower -- killough)
  // backsector is NULL for one sided lines

  sector_t *frontsector, *backsector;
} seg_t;
*/

//
// The LineSeg.
//
typedef struct
{
    vertex16_t v1;
    vertex16_t v2;            // Vertices, from v1 to v2.

    int16_t offset;
    angle16_t angle;

    uint16_t sidenum;
    uint16_t linenum;

    uint8_t frontsectornum;
    uint8_t backsectornum;
} seg_t;

typedef char assertSegSize[sizeof(seg_t) == 18 ? 1 : -1];


//
// A SubSector.
// References a Sector.
// Basically, this is a list of LineSegs,
//  indicating the visible walls that define
//  (all or some) sides of a convex BSP leaf.
//

#if defined FLIPPER_DOOM
// flipper-doom: 4 bytes, the sector an index (the WAD format keeps sectors
// under 256, segs of a subsector too); the sector is SS_SECTOR(subsector)
typedef struct subsector_s
{
  uint8_t sectornum;
  uint8_t numlines;
  uint16_t firstline;
} subsector_t;

#define SS_SECTOR(ss) (&_g_sectors[(ss)->sectornum])
#else
typedef struct subsector_s
{
  sector_t __far* sector;
  uint16_t numlines, firstline;
} subsector_t;

#define SS_SECTOR(ss) ((ss)->sector)
#endif

//
// OTHER TYPES
//

// Patches.
// A patch holds one or more columns.
// Patches are used for sprites and all masked pictures,
// and we compose textures from the TEXTURE1 list
// of patches.
typedef struct
{
    int16_t		width;		// bounding box size
    int16_t		height;
    int16_t		leftoffset;	// pixels to the left of origin
    int16_t		topoffset;	// pixels below the origin
    int32_t		columnofs[8];	// only [width] used, upper word is always zero
    // the [0] is &columnofs[width]
} patch_t;


// posts are runs of non masked source pixels
typedef struct
{
    byte		topdelta;	// -1 is the last post in a column
    byte		length; 	// length data bytes follows
} post_t;

// column_t is a list of 0 or more post_t, (byte)-1 terminated
typedef post_t	column_t;


//
// Sprites are patches with a special naming convention
//  so they can be recognized by R_InitSprites.
// The base name is NNNNFx or NNNNFxFx, with
//  x indicating the rotation, x = 0, 1-7.
// The sprite and frame specified by a thing_t
//  is range checked at run time.
// A sprite is a patch_t that is assumed to represent
//  a three dimensional object and may have multiple
//  rotations pre drawn.
// Horizontal flipping is used to save space,
//  thus NNNNF2F5 defines a mirrored patch.
// Some sprites will only have one picture used
// for all views: NNNNF0
//

typedef struct
{
  // Lump to use for view angles 0-7.
  int16_t lump[8];

  // Flip bit (1 = flip) to use for view angles 0-7.
  //byte  flip[8];
  byte flipmask;

  // If false use 0 for any position.
  // Note: as eight entries are available,
  //  we might as well insert the same name eight times.
  boolean rotate;

} spriteframe_t;


//
// A sprite definition:
//  a number of animation frames.
//

typedef struct
{
  spriteframe_t __far* spriteframes;
} spritedef_t;

//
// Now what is a visplane, anyway?
//
// Go to http://classicgaming.com/doom/editing/ to find out -- killough
//

typedef struct visplane
{
  struct visplane __far* next;        // Next visplane in hash chain -- killough
  int16_t picnum, lightlevel;
  int16_t minx, maxx;
  fixed_t height;
  boolean modified;

  byte		pad1;
  byte		pad2;
  byte		pad3;
  // Here lies the rub for all
  //  dynamic resize/change of resolution.
  byte		top[VIEWWINDOWWIDTH];
  byte		pad4;
  byte		pad5;
  // See above.
  byte		bottom[VIEWWINDOWWIDTH];
  byte		pad6;

} visplane_t;

#endif
