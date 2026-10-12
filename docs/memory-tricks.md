# How other ports squeezed memory for objects and levels (research from 12.10.2026)

The closest to us are the next-hack ports (IKEA lamp MG21: 108 KB RAM, zone 78.6 KB, E1M6 "typically under 75 kB"; nRF52840; MG24). Like Doom8088, they grew out of GBADoom, so the techniques carry over almost verbatim.

## Counts from doom1.wad (skill 2, no multiplayer things)

| Map | Monsters | Pickups | Barrels | Decor | Sectors | Sides | sec->lines | Subsectors | BM cells |
|---|---|---|---|---|---|---|---|---|---|
| E1M2 | 41 | 106 | 24 | 28 | 200 | 1323 | 1322 | 448 | 1302 |
| E1M3 | 74 | 175 | 28 | 32 | 177 | 1326 | 1318 | 461 | 850 |
| E1M5 | 91 | 93 | 28 | 3 | 143 | 1053 | 1051 | 384 | 832 |
| E1M6 | 118 | 214 | 22 | 15 | 250 | 1727 | 1719 | 606 | 1748 |
| E1M7 | 84 | 146 | 16 | 20 | 170 | 1223 | 1220 | 467 | 864 |
| E1M8 | 27 | 36 | 28 | 7 | 74 | 511 | 507 | 177 | 2912 |

On E1M6 there are ~229 static candidates out of 369 things (62%). Barrels do not qualify (MF_SHOOTABLE).

## Techniques in descending order of "savings / amount of changes"

1. **Prefix static_mobj_t for pickups and decorations** (nRF52840, MG21, MG24, rp2040, D32XR). static_mobj_t is the beginning of mobj_t with the same field order (thinker, flags, blockmap/sector links, tics, type, sprite/frame, state, subsector, x, y, z); everything below the marker is absent. Code works through `mobj_t*`, the type is selected by the flag `MF_STATIC (MF_DECORATION | MF_SPECIAL)`. radius/height via accessors from mobjinfo. In nRF: 92 → 44 bytes, 29 references in 6 files. Animated decorations get a "brainless" thinker, non-animated ones get NULL. Pitfalls: items on lifts and in crushers (only z recalculation, no gibs), sound from an item, teleport/ZMovement are not called, respawn, savegames. Forecast for us on E1M6: ~17 KB, with x/y from THINGS on SD (as in MG24) 20–22 KB.
   - nRF52840Doom: `Doom/include/p_mobj.h` (MOBJ_SIZE 92, STATIC_MOBJ_SIZE 44), `source/p_mobj.c` (P_SpawnStaticMobj, P_RemoveStaticMobj), `p_map.c`.
   - rp2040: mobj_t core 32 bytes + mobjfull_t 84 bytes, `src/doom/p_mobj.h`.
   - D32XR: `static_mobj_size (offsetof(mobj_t,angle))`, P_PreSpawnMobjs.
   - MG24: static 20/29 bytes, x/y in flash by pos_index, constant flags from mobjinfo.
2. **Remove msecnode_t, restore vanilla P_ChangeSector** (MG24, `USE_MSECNODE`). touching_thinglist is needed only by P_CheckSector; instead, walk the blockmap over the sector bbox + MAXRADIUS. Savings: nodes × 28 bytes + 4 bytes per mobj and sector, ~10 KB on E1M6 (to be measured with a counter). Bonus: less CPU in P_SetThingPosition.
3. **Short pointers / 16-bit indices** (nRF `i_memory.h`, rp2040 `shortptr_t`, D32XR `SPTR`). The STM32WB55 RAM is 0x20000000–0x2003FFFF, 16 bits × 4 covers all of it (with the zone aligned to 4). By benefit: blocklinks uint16 (E1M6 −3.5 KB, E1M8 −5.8 KB; changes in P_LoadBlockMap, P_Set/UnsetThingPosition, P_BlockThingsIterator), links in mobj and thinker (−2 bytes each), indices in subsector/mobj (sector_num, state_idx).
4. **Singly linked chains** for sector/blockmap: remove sprev/bprev (−8 bytes per mobj), unlink with a linear pass.
5. **Packing mobj fields** (MG24 92 → 52): z/floorz/ceilingz in 16 bits, momx/momy 24 bits, angle 16 bits, bit fields for counters, no lastenemy. Another 4–9 KB on E1M6, but it touches the arithmetic.
6. **Constant data in flash/on SD, only mutable data in RAM**: sector_t is split into a constant part and ramSector_t (16 bytes; 9–11 KB on E1M6), sides are entirely constant + a small list of changed switch textures (E1M6 −13.8 KB, but the renderer reads sides from SD, a fps risk), sector->lines as a prebuilt lump, subsectors as bit fields.
7. **Allocator**: 8-byte block header instead of 16 (nRF), pools with a bitmask.

## Order for E1M6 (128 → ≤82 KB)

static_mobj (~17 KB) → remove msecnode (~10 KB) → blocklinks uint16 (~3.5 KB) → sides on SD or split sector_t (~10–14 KB). The first three: ~30 KB with small changes.

## Sources

- https://next-hack.com/index.php/2021/11/13/porting-doom-to-an-nrf52840-based-usb-bluetooth-le-dongle/
- https://github.com/next-hack/nRF52840Doom
- https://next-hack.com/index.php/2023/12/10/multiplayer-doom-on-the-sparkfun-thing-plus-matter-board/
- https://github.com/next-hack/MG24_Doom_BLE
- https://github.com/marciopocebon/MG21DOOM
- https://kilograham.github.io/rp2040-doom/speed_and_ram.html
- https://github.com/kilograham/rp2040-doom
- https://github.com/viciious/d32xr
