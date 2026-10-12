# Full Doom (all 9 E1 maps) on the Flipper: options with no flash or minimal flash

Research from 11.10.2026, PC only (the `port/sim` simulator, ARM builds); the device was not touched.
Current state on the device: FAP, the Doom8088 engine executes from a free chunk of flash (101.9 of 104 KB), engine RAM 27.9 KB, zone 84 KB, largest heap block after engine RAM 89-97 KB.

## 1. How much memory the maps need

Minimum zone at which the scripted route passes (`tools/zonefit.sh`, 3000 frames, paging of SEGS/NODES/BLOCKMAP/REJECT/directory already enabled):

| map | E1M1 | E1M2 | E1M3 | E1M4 | E1M5 | E1M6 | E1M7 | E1M8 | E1M9 |
|---|---|---|---|---|---|---|---|---|---|
| min. zone, KB | 85 | 143 | 158 | 123 | 128 | **194** | 150 | 85 | 125 |
| shortfall vs 84 KB | 1 | 59 | 74 | 39 | 44 | 110 | 66 | 1 | 41 |

Device budget: zone 84 KB. Today only E1M1 and E1M8 fit, and only barely.

What a level consists of in the zone (KB, `-DFZ_MEMTRACE`, engine baseline before the level is 30.5 KB: texture and sprite descriptions, frame, directory hashes):

| | object pool (THINGS × 128 B) | lines (36 B) | sectors (60 B) | sides (16 B) | block links, subsectors, line lists, specials | whole level |
|---|---|---|---|---|---|---|
| E1M1 | 14.9 | 17.1 | 5.1 | 6.6 | 11.5 | 55.2 |
| E1M2 | 31.0 | 37.2 | 12.0 | 15.4 | 20.4 | 116.0 |
| E1M3 | 47.1 | 37.0 | 10.6 | 14.6 | 22.4 | 131.8 |
| E1M6 | 57.5 | 48.7 | 15.0 | 16.4 | 31.6 | 169.2 |
| E1M7 | 43.2 | 34.5 | 10.2 | 13.0 | 21.2 | 122.1 |

All of these are mutable structures with pointers to each other (objects, lines with `validcount` counters, sectors with heights). They cannot be paged as simply as segs.

Immutable level lumps that are already paged from SD (bytes):

| | E1M1 | E1M2 | E1M3 | E1M4 | E1M5 | E1M6 | E1M7 | E1M8 | E1M9 |
|---|---|---|---|---|---|---|---|---|---|
| SEGS+NODES+BLOCKMAP+REJECT | 24 624 | 50 468 | 48 651 | 38 644 | 38 807 | 68 259 | 46 703 | 23 379 | 33 162 |

## 2. SD paging speed on large maps

SD time per frame using the model from the device (0.9 ms + 1.1 us/byte per miss), 2000-frame route. Current cache is 22 pages (11 KB):

| map | E1M1 | E1M2 | E1M3 | E1M4 | E1M5 | E1M6 | E1M7 | E1M8 | E1M9 |
|---|---|---|---|---|---|---|---|---|---|
| ms of SD per frame, 11 KB cache | 3.0 | 33 | 68 | 14 | 44 | 62 | 22 | 0.5 | 24 |

Dependence on cache size on the heavy maps (ms per frame, average):

| cache | E1M2 | E1M3 | E1M6 | E1M7 | E1M9 |
|---|---|---|---|---|---|
| 16 KB | 14.1 | 39.7 | 32.6 | 1.1 | 5.7 |
| 24 KB | 1.2 | 15.2 | 7.2 | 0.5 | 1.0 |
| **32 KB** | 0.8 | 1.0 | 2.6 | 0.4 | 0.7 |
| 48 KB | 0.5 | 0.5 | 0.9 | 0.4 | 0.3 |

Worst frame (level entry) is 220-830 ms with any cache size. Conclusion: for all of E1 the page cache needs ~32 KB of RAM instead of 11, i.e. another +21 KB.

## 3. Hot and cold code

Method: the simulator is built with `-finstrument-functions` (`sim/build_instr.sh`, hooks in `sim/fz_instr.c`, the normal build is unaffected). Functions called after frame 150 are considered hot. Runs: the route through all 9 maps plus demo3 (combat on E1M7). Sizes are taken from ARM objects in the same PIC build as the image, but without LTO (`tools/hotcode.py`).

| code (ARM, PIC, no LTO) | KB |
|---|---|
| total | 63.3 |
| hot (seen in gameplay) | 44.1 (+~1 KB dithering and HUD, not called in the simulator without a dump) |
| startup and level loading only | 4.5 |
| not seen in gameplay | 14.7 (menu 1.3, automap 2.0, intermissions 2.5, finale 0.5, cheats 0.7, teleports 0.3, rare door/weapon branches ~2) |

Hot code is spread across the whole engine: r_draw 9.2 KB, p_map 4.2, p_mobj 3.4, p_inter 2.1, g_game 2.0, p_setup 1.8, p_maputl 1.8 and dozens more files at 0.2-1.5 KB each. There is no "10 KB core" that could be kept separately.

Constants are almost all hot (of 39.8 KB of `.rodata`+`.data.rel.ro`, less than 1 KB is cold): brightness table 8.7, tantoangle 8.2, finetangent 6.1, finesine 4.1, states 5.0, mobjinfo 2.6, viewangletox 2.0. The cold ones are menu strings, cheat targets and the like.

Total: ~85 KB hot (code + constants), ~19 KB cold.

## 4. Options

### a) No flash at all: all code in RAM, cold code as overlays from SD

RAM: hot code and constants ~85 KB + engine RAM 28 + zone ≥85 (E1M1) = **~198 KB** against ~120 KB available to the app. Not even E1M1 fits, let alone E1M6 (zone 194). Overlays save only the 19 KB of cold code. **Not an option.**

### b) Minimal flash: only hot code in flash, cold code as overlays

- Flash: ~85 KB hot, ~15-19 KB freed (minus the overlay manager and jump table, ~1 KB).
- RAM: an overlay region the size of the largest cold module, ~3-4 KB (intermissions wi_stuff+wi_lib ~2.5 KB, automap ~3.5 KB, menu ~2.6 KB).
- Loading an overlay from SD: 2-4 KB sequential ≈ 2-4 ms, which is unnoticeable when entering the menu or an intermission.
- Tooling: GNU ld `OVERLAY` with a shared VMA and different LMAs works; the engine is already PIC via r9/GOT, so calls between overlays are just a stub table (`overlay_call(id, fn)`). Cold modules have to be built without LTO (LTO mixes functions across modules) or marked with sections.
- The ~15 KB of freed flash does not solve the memory problem: the immutable data of large maps is 39-68 KB, and paging needs RAM, not flash.

**What this gives:** the menu, intermissions between levels, the finale and sound (PC speaker driver ~1 KB, hot) can be brought back without growing flash. **What it does not give:** large maps; those are bounded by RAM.

### c) Paging mutable level data with write-back to SD

- Objects: all thinkers are walked every tic (P_RunThinkers), and fields are read and written on each one. The working set = the whole pool (E1M6 57 KB). Paging with write-back means dozens of misses per tic, at ~2-5 ms per page write to SD. Not feasible.
- Lines: `validcount` is written on every visibility and collision check across dozens of lines per tic, and they are scattered across the whole map. Not feasible without splitting the structure.
- A realistic compromise is to **split the structures** into an immutable part (on SD or in flash, via paging) and a small mutable part (in RAM):
  - line 36 → ~8 B in RAM (flags, validcount, special, tag): E1M6 −38 KB;
  - side 16 → ~4 B (texture offset for scrolling): E1M6 −12 KB;
  - object pool: only objects for the current skill (−15-20%) and a compact record (~32 B) for static items and decorations (~half of all things): E1M6 57 → ~29 KB.
  - Estimate for E1M6: level 169 → ~95 KB, plus baseline 30 and cache ~10 = ~135 KB against a budget of 84, and the paging cache needs another +21 KB. **Does not fit.** E1M4/E1M5/E1M9 (zone 123-128) come down to ~85-95 KB after such a rework, i.e. right on the edge. E1M2/3/6/7 stay out.
  - The amount of work is large: line_t, side_t and mobj_t are used in dozens of places (p_map, p_maputl, p_sight, r_draw, p_spec, p_switch, p_doors…).

### d) External memory on the GPIO header

- **Memory mapped into the address space (QUADSPI, XIP) is impossible.** On the STM32WB55 the QUADSPI signals are: CLK on PB10/PA3, NCS on PB11/PA2/PD3, IO0 PB2, IO1 PB1, IO2 PA7, IO3 PA6. The Flipper header exposes only PA7, PA6 and PB2; CLK, NCS and IO1 are not broken out. So there is neither XIP for code nor transparent memory for data (short of reworking the board).
- **Plain SPI (PA4 CS, PB3 SCK, PA6 MISO, PA7 MOSI):** `furi_hal_spi_bus_handle_external`, `furi_hal_spi_acquire`, `furi_hal_spi_bus_trx`, `furi_hal_spi_bus_trx_dma` are exported to apps. An 8 MB PSRAM chip (APS6404L / ESP-PSRAM64H) at 16-32 MHz: a 512 B page ≈ 130-260 us plus ~10-20 us per transaction, versus ~1500 us for SD, i.e. 5-10 times faster. A small 16-32 B read is ~15-30 us.
- What this gives: the paging cache for immutable data can be kept small again (8-11 KB instead of 32), i.e. −21 KB of RAM for large maps. Mutable structures with pointers (objects, lines) still have to live in RAM without an MMU, so this does not remove the problem from point c.
- Requires soldering a module or a breadboard. Bus speed on the external SPI in the firmware was not checked.

### e) Other options

1. **Custom firmware + the next-hack engine (GBADoom).** ~300 KB of flash free (without NFC/SubGHz/RFID/IR) and the whole heap (~140 KB, without the FAP loader). In next-hack all immutable level data sits in memory-mapped flash, the structures are compacted, and on the IKEA module (96+12 KB RAM) all shareware levels are playable. Code with constants ~240 KB, immutable E1M6 data 68 KB: ~308 KB in flash, just barely; level data can stay on SD with a 32 KB cache. This is the only path found to the full E1 with already proven RAM headroom. Cost: a separate firmware without NFC and SubGHz.
2. **Video Game Module (official RP2040-based module: 264 KB RAM, 2 MB flash).** There is a full rp2040-doom port for the RP2040 (all episodes, sound, music). In this option the Flipper is the screen and buttons: a 128×64 1-bit frame = 1 KB, at 35 fps that is 35 KB/s over UART or SPI. Medium amount of work (rp2040-doom output to 128×64 with G dithering, a frame and button protocol), but the module itself is required. Whether it can output an image to the Flipper screen was not checked.
3. **Inside the current FAP:** realistically extendable to E1M1, E1M8 and, after splitting the structures (point c), possibly E1M4, E1M5, E1M9. Menu, intermissions and sound via overlays (point b).

## 5. Recommendation

- **The full E1 in a regular app will not work**, and the issue is not flash but RAM: the mutable data of large maps (E1M6 169 KB, E1M3 132, E1M2 116) is 1.5-2 times larger than the whole available heap. Giving up flash only makes it worse: the hot code (~85 KB) would then also have to live in RAM.
- Minimal flash (point b) is useful for something else: bringing back the menu, intermissions, finale and sound without growing the image.
- For a real full Doom: **custom firmware with the next-hack engine** (proven RAM savings, all levels) or the **Video Game Module** (full Doom on the RP2040, the Flipper as screen and buttons).
- If staying in a FAP: overlays for the menu and sound, then splitting the line and side structures and filtering objects by skill to add E1M4, E1M5 and E1M9. E1M2, E1M3, E1M6 and E1M7 are unreachable in a FAP.

## 6. What was not verified

- External SPI speed on the Flipper (`furi_hal_spi_bus_handle_external`) and PSRAM latencies on the real bus.
- Exact gains from splitting line_t/side_t and compact objects: these are estimates from structure sizes, not measurements.
- The route in the simulator walks but does not fight (except demo3 on E1M7): peak memory in combat may be higher.
- How much RAM the next-hack engine would actually take on the Flipper and whether its ~240 KB of code fits in the flash of a custom firmware.
- Video Game Module: output to the Flipper screen and the exchange protocol were not investigated.
- The QUADSPI alternate function table was taken from a search over the DS11929 datasheet; check it against the datasheet itself before drawing any hardware conclusions.

## Research tools

- `port/sim/build_instr.sh`, `port/sim/fz_instr.c`: simulator with function call tracing (`FZ_HOT_START`, `FZ_HOT_OUT`).
- `port/tools/hotcode.py`: hot and cold code in ARM bytes from traces and `nm` of the PIC objects.
- `port/tools/zonefit.sh`: minimum zone for a map.

Hardware sources: [DS11929, STM32WB55 datasheet](https://www.st.com/resource/en/datasheet/stm32wb55cc.pdf), [RM0434](https://cdn.sparkfun.com/assets/1/6/c/b/b/STM32WB55xx_Reference_Manual.pdf), [Flipper Zero GPIO & Modules](https://docs.flipper.net/zero/gpio-and-modules), [GPIO pinout (jamisonderek)](https://github.com/jamisonderek/flipper-zero-tutorials/wiki/GPIO-Signals).
