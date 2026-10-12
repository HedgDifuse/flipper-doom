# Real Doom on Flipper Zero: measurements and architecture

## Measurements (10.10.2026, SDK 1.5.1-rc from ~/.ufbt)

**Flipper (STM32WB55, Cortex-M4F 64 MHz)**
- Heap: `__heap_start__` 0x20001500, `__heap_end__` 0x2002FC00, 186 KB total for the firmware and apps. On the device (official 1.5.1-rc, desktop, `free`): 141,264 B free, largest block 132,032 B, historical minimum 98,448 B. Radio stack 1.20.0 takes 116 KB of flash, FUS 24 KB.
- Flash: the firmware ends at 0x080BC4DC (753 KB), the radio stack starts at 0x080D7000 (`Radio address` in update.fuf). Free: 0x080BD000-0x080D7000 = 104 KB (26 pages of 4 KB).
- `furi_hal_flash_*` are marked `-` in api_symbols.csv, so they are not available to a FAP.
- A FAP is loaded entirely into RAM: the code also takes heap.

**Engines (arm-none-eabi-gcc -Os, Cortex-M4)**

| | .text | .data | .bss | note |
|---|---|---|---|---|
| doomgeneric (chocolate) | 221 KB | 60 KB | 244 KB | plus a zone of megabytes, not an option |
| next-hack nRF52840 (GBADoom/PrBoom) | 245 KB | 122 KB* | 19 KB | *113 KB of it is the zone array; tables.o 81 KB, info.o 27 KB |

## Architecture conclusion

A regular FAP on stock firmware cannot handle the real engine: the code alone is ≥150 KB (even after shrinking the tables), plus ≥60-100 KB of working memory, and that is more than the whole heap.

**Recommended path: custom firmware (a fork of the official one), Doom as a built-in app.**
- Code executes from flash (XIP), RAM goes to data.
- Drop heavy apps (NFC/SubGHz/Infrared etc.) to free 250-350 KB of flash: about 200 KB for code and the rest for a level cache.
- Rollback to stock via qFlipper at any time.
- Engine base: next-hack nRF52840Doom (all the memory work is already done, vanilla renderer). Its WAD sits in memory-mapped QSPI. In our case the role of QSPI is played by a region of internal flash, where the needed lumps are copied from SD on level load.

**WAD from SD.** On the PC we convert DOOM1.WAD into a compact format (inspired by MCUDoomWadUtil): textures downscaled 2x, palette to 4-bit brightness, levels in packed structures. The SD card holds this file, not the raw WAD. Streaming from SD per frame is not possible: SPI + FatFS add milliseconds of latency to every read.

**Rendering at 128×64.** An 8 KB framebuffer instead of 64 KB. Fill is cheap (8K pixels vs 38K on the GBA); the bottleneck will be BSP and game logic. Expected 20-35 fps.

### Option "FAP + code in flash" (analyzed 11.10.2026)

Technically feasible:
- Firmware 1.5.1-rc does not write to flash at all: `furi_hal_flash_erase/program` are removed by the linker, only `furi_hal_flash_init` remains. So nobody touches the region 0x080BD000-0x080D7000 (104 KB; above it starts the protected radio stack area, SFSA=0xD7).
- On the device (tools/flashprobe) this region is **not empty**: up to 0x080D486C there are leftovers of an earlier, larger firmware. Strings and 0x080Cxxxx pointers; the updater does not erase the tail. So erasing cannot be avoided.
- `furi_hal_bt_lock_core2`, `furi_hal_bt_is_alive`, `__furi_critical_enter`, `furi_hal_cortex_timer_*` are exported to FAPs, and the HSEM/FLASH registers are accessible via CMSIS. The only missing piece is `SHCI_C2_FLASH_EraseActivity`, which is not even in the firmware itself. A workaround via FUS does not work: switching Stack→FUS reboots the whole MCU. Working workaround: erase only with BT off (`furi_hal_bt_is_active() == false`); then the radio core has no radio events that the erase could disrupt.
- The engine blob is linked at the fixed address 0x080BD000. Firmware calls go through a pointer table from the FAP loader; mutable state is reached through a pointer (next-hack's `_g` already works this way). A firmware update may overwrite the region: the loader checks the CRC and reinstalls the blob from SD.

**Verified on hardware 11.10.2026 (tools/flashexec):** the FAP wrote 1088 bytes to an empty page at 0x080D6000 in 11.8 ms with BT on. The function executed directly from flash and called a callback in the app's RAM (result 1084, as expected); a 1024-byte pattern read back without errors; after reboot everything was still there. Erasing with BT off was verified too: a page erases in 22.3 ms, the radio stack stays alive (radio.alive, mode Stack); the full write → run → erase cycle was repeated. Estimate for installing 104 KB: 26 pages × 22 ms for erase plus about 1.2 s for writing, about 2 s in total.

But it does not fit in size:
- Available: 104 KB of flash plus the largest RAM block of 128-132 KB (measured from inside a FAP), about 236 KB in total.
- Needed: the next-hack engine linked with gc-sections is 101 KB of code and 139 KB of constants. After shrinking for 128 px (finesine 40→8 KB, finetangent 16→8, viewangletox 8→4, no stbar/finale/gamma) about 170 KB remain. On top of that comes data: the zone with mobs, level geometry (next-hack keeps it in mapped flash, for us it would be in RAM) and a texture cache, another 100-130 KB.
- A deficit of 40-60 KB even after shrinking. Full Doom as a FAP will not work; only a cut-down version is possible.

For comparison, custom firmware: the NFC family ~90 KB, SubGHz ~74 KB, RFID/iButton/IR ~38 KB, plus the free 104 KB, about 300 KB of flash in total.

Rejected options:
- A Doom-like raycaster as a FAP: already exists (p4nic4ttack), and it is not real Doom.

## Doom8088 as the base (11.10.2026)

The most stripped-down real Doom found: [Doom8088](https://github.com/FrenkelS/Doom8088) (GBADoom → 16-bit DOS). E1 only, untextured floor and ceiling, no light diminishing and no saves. The WAD is preprocessed by [jWadUtil](https://github.com/FrenkelS/jWadUtil) (wad/DOOM8088.WAD, 1.5 MB) and read via fopen/fread, so it works for SD. The same author has [Doom64KB](https://github.com/FrenkelS/Doom64KB): 64 KB of RAM, but level data is in ROM, and only E1M1 and E1M8 are playable. milandr-doom turned out to be the doom-nano raycaster.

Port: `port/engine` (a copy of Doom8088, changes marked `flipper-doom`), `port/platform` (120 px video backend), `port/sim` (simulator, `ZONE_KB=.. bash build.sh` in WSL, `-m32`). Options: `-noxms` (read the WAD from disk, like from SD), `-warp N`, `FZ_FRAMES`, `FZ_DUMP`; a build with `EXTRA=-DFZ_MEMTRACE` prints memory at each step of level loading.

Sizes (arm-none-eabi -Os, gc-sections): 51 KB of code + 42 KB of constants (of which fullcolormap, 8.7 KB, actually has to be in RAM) + 33 KB bss (columnCache 16 KB).

Heap (zone) before level load, 57 KB:
- WAD directory 15.7 KB + lumpcache 3.9 KB;
- 120×160 framebuffer 19.2 KB (can be shrunk to 120×64);
- sprites 8.4 KB, textures 9.7 KB.

Level data in the heap:

| E1M1 | E1M2 | E1M3 | E1M4 | E1M5 | E1M6 | E1M7 | E1M8 | E1M9 |
|---|---|---|---|---|---|---|---|---|
| 80 | 167 | 181 | 136 | 139 | >200 | 169 | 79 | 119 |

(KB, excluding texture and sprite caches.) Speed is not a problem: timedemo demo3 on the PC gives 1821 fps.

FAP budget (104 KB flash + ~132 KB RAM): code and constants ~90 KB in flash; in RAM, bss ~42 KB (with fullcolormap), stack and stub ~9 KB, leaving ~81 KB for the heap. Even E1M1 after shrinking the base (~25 KB) + 80 KB of level + ~10 KB of cache ≈ 115 KB, ~35 KB over. For the FAP to work we need Doom64KB-level surgery (static level data outside RAM) and paging from SD through a cache. Custom firmware (~300 KB of flash for code + static level data) removes the problem for all of E1.

## Shrinking for a FAP (11.10.2026, option "regular app, E1M1 + E1M8")

Target RAM budget: ~116 KB (largest heap block 132 KB minus stack, loader and margin).
How to measure: `bash port/tools/armsize.sh` (static memory on ARM) + `port/tools/zonefit.sh N` in WSL (minimal heap, the player walks a scripted route).

| step | static | E1M1 heap | total |
|---|---|---|---|
| baseline (120×128) | 42.8 | 122 | ~165 |
| 120×64 window, columnCache 128→32 | 30.3 | 122 | 152 |
| paging SEGS/NODES/BLOCKMAP/REJECT, 24 pages of 512 B | 42.8 | 98 | 141 |
| colormap → constant brightness table in flash | 34.1 | 98 | 132 |
| WAD directory on SD via paging (only name hashes in RAM) | 34.0 | 85 | 119 |
| renderer limits for height 64 (openings, drawsegs, vissprites) | 29.9 | 85 | **~115** |

Flash: 97 KB of 104 (including the 8.7 KB brightness table); ~7 KB remain for the Flipper platform code, so unneeded modules (menu, finale, intermission screens, status bar) will have to go.

Paging (SD measurements on the device, tools/sdbench): sequential 1.26 MB/s, random read of 32 B ~0.94 ms, 512 B ~1.5 ms, 4 KB ~4.2 ms, storage service overhead ~0.36 ms. In the simulator with this model: E1M1 ~1 miss per frame (~1.4 ms of SD), E1M8 0.2 (~0.3 ms). An 8 KB cache gave 13.5 ms per frame on E1M1, 12 KB already 1.25 ms.

Pixel = 6 bits of brightness + 2 bits of class (wall/floor/sprite/other); the colormap is generated by `port/tools/mklum.py` with the tone curve from the dithering lab.

The margin is thin: the heap was measured on a scripted route. In a real game with awakened monsters more may be needed. Reserves: filter THINGS for medium difficulty (−2.6 KB), drop unneeded lumps from the WAD.

Next: the engine blob in flash (data via r9/GOT, since the RAM address is only known at launch), the Flipper layer (screen, buttons, SD, timer), dithering G in C, the blob installer.

## Doom runs on the Flipper (11.10.2026)

A regular FAP on stock 1.5.1-rc, E1M1 is playable. Layout:
- `port/flipper/build_blob.sh`: the engine + Flipper layer are built with `-fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative -flto` and linked with `blob.ld` (code from 0x080BD040, data at a placeholder 0x10000000). `tools/mkblob.py` checks that flash has no absolute references to RAM and packs `engine.bin` (header, image, relocation list). Image 101.5 KB of 104, engine RAM 27.9 KB.
- `port/flipper/fap`: the loader. If the image does not match (header + CRC) it writes it to flash (BT must be off, 1.5.1-rc only), allocates RAM, applies relocations, and calls the engine through a trampoline with r9. The engine reaches the firmware only through `fz_api_t` (malloc, WAD reads, time, buttons, framebuffer, vsnprintf=`__wrap_vsnprintf`, log).
- The startup trace is written to `/ext/apps_data/flipper_doom/last_run.txt`.

Pitfalls:
- The OOM happened because ZONE_SIZE was not passed to the image build: z_zone.c used the DOS default of 640 KB. The macro is now in the shared defs.sh.
- In newlib atoi -> strtol -> errno (_impure_ptr in RAM): own atoi. Own malloc/printf/exit on top of the API, stubs for __aeabi_unwind_cpp_pr*.
- ECC: a 64-bit flash word can be written only once; the image header is skipped on the first pass and written last.
- The heap on the device fluctuates: the largest block after engine RAM is 89-97 KB. The zone is now 84 KB (the simulator route needs 85); during play the firmware has ~12.7 KB.
- `input send back press/release` via CLI closes the waiting screen; `ufbt launch` does not restart a hung app.

## Grayscale on a 1-bit LCD (11.10.2026)

The Flipper's panel is slow: a fast-blinking pixel looks gray. The `gray_display` thread in the loader cycles subframes: level L out of P = the pixel is on in L of P subframes. Following the zephray.me technique, pulses are spread over the period (50% = on/off every other subframe), and each pixel in a 4×4 block has its own phase, so flicker turns into fine grain. Test: `tools/lcdgray` (modes naive/spread 3/4/6/8).

Key finding: the ST7567 has its own frame rate from an internal oscillator, 70-80 Hz (typically 75); it cannot be set by command and there is nothing to sync to. If the subframe rate does not match it, rows get some subframes twice and miss others, and the grays slowly "drift" (especially visible on a static wall). Solution: calibrate the rate in 0.1 Hz steps until the picture is still, plus pacing held by the cycle counter (sleep until ~1 ms before the deadline, spin for the rest); the subframe is built in advance and sent exactly on time. On the test device the picture is steady at exactly 75.0 Hz for any number of levels (saved: rate10=750 period=8 gamma=130). Measurement: over 31 s of play one subframe was more than 1 ms late (worst 1.15 ms); the SPI bus shared with SD does not interfere; Doom 10.7 fps, subframe build 2.55 ms, send 2.57 ms.

Calibration (3 steps: rate, number of levels, gamma) is saved to `/ext/apps_data/flipper_doom/display.conf`. The engine writes the frame at 4 bits per pixel through a "brightness → level + fraction" table (`platform/fz_graymap.h`), with checkerboard dithering between levels.

Performance: the subframe thread at ~69 Hz initially ate ~59% CPU (Doom 5 fps). Building subframes with tables 5.8 → 2.1 ms (Doom 8.8 fps), then a table per whole frame byte; sending to the panel is ~2.6 ms per subframe and cannot be sped up through the API.

Memory: engine RAM + heap are reserved as one block right after the app's small allocations (otherwise the heap fragments and the largest block wanders between 78-97 KB). The WAD is stored separately in `/ext/apps_data/flipper_doom/` (1.6 MB in assets broke USB during upload).

Fixed along the way:
- hang when pushing into a wall: the game thread stack had been cut to 4.5 KB, restored to 6 KB (sliding along a wall adds calls);
- "blur" at high fps: fz_dither cleared the frame before writing, while the display thread read it in parallel and saw a light buffer. Now every pixel is overwritten in place; the HUD is built in its own 8x64 buffer and copied as a whole;
- black wall chunks: when the heap had no room for a texture patch, the engine drew a "color" = texture number with the floor class (and a dark outline). Now R_DrawColumnMissing draws mid-gray with lighting, like a wall. It still happens (in the simulator with an 86 KB heap ~2500 columns over 3000 frames of E1M1, with 100 KB zero).
- Tried paging pages in the shared heap (purgeable blocks): the gaps disappear, but SD is 26 ms per frame; reverted.

Open problems:
- connecting over USB CLI during play crashes the Flipper (and possibly closing the app);
- the engine heap is on the edge: texture/sprite patches need to be streamed through the page cache instead of loaded whole into the heap;
- fps ~10-11, sound not done.

## Dithering (tools/ditherlab, comparison: out/compare.html)

Main conclusion: naive brightness dithering turns Doom into mush. The scene's brightness range is narrow, and 128×64 is too small. We need **renderer-aware dithering**: the renderer already knows for each pixel whether it is a wall, floor, sprite or weapon.

Option **G** was chosen:
- Walls: brightness through their own colormaps (tone curve baked in), 3 levels, Bayer 4×4 in the range [0.45..1]. The world comes out light, and everything dark reads as an object.
- Floor and ceiling: +0.35 brightness, almost clean.
- Geometry outlines: a change of seg/visplane gives a dark pixel. Free on the device: the top and bottom of a wall column are known in R_RenderSegLoop.
- Sprites: local contrast and threshold, dark edge, 1 px light halo. Weapon: threshold.

Temporal stability (share of pixels changing between frames, demo1):

| algorithm | turning | fight with an imp |
|---|---|---|
| Floyd-Steinberg | 45% | 43% |
| local contrast + threshold | 28% | 26% |
| G | 12% | 12% |
| naive blue noise | 5% | 4% (stable, but nothing is visible) |

We do not use error diffusion: it "boils".

Still to check on hardware: the slow response of the ST7567 allows trying temporal dithering (alternating frames would give extra gray levels).

## Rendering speed and memory for other maps (11.10.2026)

Walls come from the FZTX lump (`port/tools/fztex.py`): every 4th texture column, every 2nd row, 4 bits per texel (16 palette grays), 32 bytes per column, read through the pager. Wall patches are no longer loaded into the zone; no more "black" walls.

Pager: 31 pages of 512 B (was 22): freed columnCache (2 KB) and cut MAXDRAWSEGS to 72, MAXVISSPRITES to 40, MAXOPENINGS to ×6 (no overflows on three routes through E1M1/E1M8). Offline model of cache policies (`tools/pgsim.py`, trace `FZ_PGTRACE`): separate pools for textures and geometry do not help; only size helps. Miss cost in the model: 0.9 ms + 1.1 us/byte.

E1M1 from the start point, SD ms/frame in the simulator: 32 (FZTX 128 B) → 24 (4 bits) → 12.6 (31 pages). On hardware before this: 14.7 fps on average, dither+HUD ~21 ms/frame (including the display thread: 75 subframes/s × (2.5 ms build + 2.5 ms LCD) ≈ 38% CPU).

Memory (minimal zone in the simulator, KB):

| | E1M1 | E1M2 | E1M3 | E1M4 | E1M5 | E1M6 | E1M7 | E1M8 | E1M9 |
|---|---|---|---|---|---|---|---|---|---|
| before | 85 | 139 | 154 | 122 | 126 | 193 | 161 | 85 | 122 |
| after | 54 | 93 | 104 | 81 | 85 | 130 | 110 | 61 | 83 |

What helped:
- lumpcache: 64 entries instead of a pointer for each of the 984 lumps (−3.4 KB);
- object pool only for the objects of the current difficulty + 16;
- `line_t` 36 → 10 B: dx/dy/bbox/slopetype are computed from the vertices; vertices/sides/tag are read from LINEDEFS through the pager (`P_LineRO`, cache of 64 lines); the fake line for A_BossDeath goes through `_g_fakeline`;
- FZSPR (`tools/fzspr.py`): sprite frame table built offline, cache of 32 frames; match with R_InitSprites verified (`-DFZ_SPRCHECK`, 238 frames);
- `side_t` 16 → 8 B (sector index, textures as a byte: the Doom8088 format already limits them to 256);
- `mobj_t` 128 → 116 B (fields sized to fit, sprite/type as a byte);
- `sector->lines` → `linenums` indices (`SEC_LINE`).

Game behavior did not change: on the same route the SD time in the simulator matches to the hundredth after each step.

On E1M4/E1M9 SD is 32–36 ms/frame: sleeping monsters check every 10 tics whether they can see the player, through the BSP and the lines of the whole map. Decided to keep it as in the original.

Zone made of chunks (`Z_Init` under FLIPPER_DOOM, API v4): any number of regions (up to 16), with a static bridge block between regions, like EMS in Doom8088. The loader takes the largest free blocks one by one, leaving HEAP_RESERVE. The object pool is split in half if a chunk is too small. LINEDEFS/SIDEDEFS are read record by record through the pager at load time (lumps up to 20 KB did not fit in a chunk). Check with `FZ_ZONE_SPLIT` + `FZ_CHECKHEAP`: E1M4 in 10 KB chunks needs 90 KB (81 in one chunk); chunks smaller than 10 KB do not work (line and sector arrays are 8.3 KB each).

Reason: on the device, with the same free memory (118 KB), the largest block was sometimes 101 KB instead of 114, the zone came out at 70 KB, and the old loader (ZONE_MIN 80) failed.

Flash: image 105.8 of 106.5 KB. New changes will require freeing space (candidate: the am_map automap).

## Sound and music (12.10.2026)

- Effects: the original DS* from doom1.wad (8 bit, 11025 Hz), added to FZDOOM.WAD in DP* order (`fztex.py ... doom1.wad`). Playback works like the Flipper WAV player: TIM16 8-bit PWM on the speaker, TIM2 at 11025 Hz triggers DMA1 Ch1 from a 2×512 B ring, the DoomSound thread refills the halves (`fap/sound.c`). Volume by distance from the engine (0..120).
- Music: MUS rendered with libADLMIDI (WSL ~/libADLMIDI, bank 16 DMX Bobby Prince v1, `-vm 3`, Nuked OPL2) into `wad/music/*.PCM` (u8, 11025 Hz, 18.6 MB), `tools/mkmusic.py`; on SD in `apps_data/flipper_doom/music/`, looped, effects mixed on top, music ducked under effects.
- Loudness: `tools/loudness.py` (low cut at 280 Hz, +5 dB shelf at 1.8 kHz, compressor, soft limiter): music ~+8 dB, effects ~+9 dB.
- Pitfalls: the firmware cannot open a file twice (the second open waits for the first to close), so the sound thread reads the WAD through the loader's shared handle under a mutex. Menu: Sound / Music on/off (`sound.conf`).

## All E1 maps fit in memory (12.10.2026)

Done (each step verified: behavior on routes matches, ASan + `FZ_CHECKHEAP` clean on all 9 maps):
- automap and original status bar stubbed out (−5.5 KB of flash);
- msecnodes removed; P_CheckSector walks the blockmap over the sector's bounding box, like Doom 1.9;
- static objects (type ≥ MT_MISC0: items and decorations) use a 64 B mobj_t prefix instead of 116, with their own pool; verified with separate allocations under ASan (`-DFZ_STATIC_ASAN`, currently incompatible with short blockmap pointers);
- line_t 2 B (validcount as a bit, r_flags computed on the fly), subsector 4 B, side 8 B, sector 36 B;
- blockmap: 16-bit short pointers (offset from the zone start /4), bprev with a tag for the chain head;
- `E1MxSEC` lumps (`tools/fzsec.py`): sector line lists, sound origins, blockmap bounding boxes built offline, checked against the engine on all maps;
- THINGS/LINEDEFS/SIDEDEFS are read record by record at load; sprites: from the zone if they fit, otherwise column by column through the pager;
- REJECT: the player sector's column is cached (same answers);
- pager: 16 static pages + pages from free zone space after level load (4 KB reserve), given back when memory runs short (`PG_ReleasePages` from Z_Malloc);
- top of the BSP (63 nodes) kept in memory;
- zone made of chunks (up to 16 regions).

Minimal zone (KB, simulator): E1M1 38, E1M2 52, E1M3 58, E1M4 46, E1M5 50, E1M6 75, E1M7 55, E1M8 37, E1M9 57. On the Flipper with sound the zone is 82.3 KB: all maps start.

Speed on the Flipper: E1M1 ~19 fps; E1M6 4.4 fps, of which SD ~177 ms/frame. Measured with `tools/sdbench`: a sector-aligned 512 B read through the file API 1.0 ms, directly through the HAL 0.67 ms (1 sector, but it goes through the firmware's unsynchronized sector cache), 1.03 ms (2 sectors); unaligned 1.7 ms. In game ~1.8 ms: the gray thread (75 subframes/s × 2.5 ms build + 2.6 ms send to the LCD over the same SPI bus) takes ~40% CPU.

Level transitions and death (12.10.2026): the intermission could not find 3.3 KB for its patches after a large map (the level stays in the zone until the next P_SetupLevel), and E1M6 → E1M7 crashed. Now `P_UnloadLevel` in G_DoCompleted frees the level and the page cache and purges the lump cache before the intermission; before pages are added to the pager the cache is purged (`Z_PurgeCache`), and the pager takes up to 4 zone chunks (the slot table is static, 127). The log gets "E1Mx: page cache +N B" on every load.

A measurement on the device showed that transitions and death are not the cause (E1M2 after a transition and from the menu are the same, 8-9 fps at the start). The cause is sprites: when the visible sprites do not fit in the zone, each one was loaded over another, every frame (E1M2: 13-35 SD reads per frame, also in combat on E1M1). Now `W_TryGetLumpByNum` does not evict lumps drawn in this or the previous frame (`W_NewFrame`); the rest go column by column through the pager. Simulator: E1M2 21.7 → 2.1 ms SD/frame. Flipper log: every 100 frames fps, SD ms/frame and reads; the log accumulates over the whole app session. Simulator: FZ_DIE_AT, FZ_MISSLOG, "all SD reads in play" (excluding level loads). MAXOPENINGS ×7 (E1M2 asked for 728 with 720). Buttons (12.10, "sticking" on slow frames): tried a timestamped event queue (API v7, each tic gets the presses from its moment) — it felt worse in testing, even with rebuilding the last tics on overflow; reverted. Now API v8: polling the current state as before + `buttons_pressed()` (presses since the last poll), so a click shorter than a frame is not lost.

Sight checks (approved 12.10, "as long as they don't get dumb"): the answer "the monster sees the player" is memoized — 4 tics for a chasing monster, 20 for one that has not noticed the player yet (SIGHT_MEMO_TICS / SIGHT_MEMO_IDLE_TICS in defs.sh, table 64×4 B, 2-way). SD reads from sight checks −16% on E1M3, −10-30% on E1M2/E1M9; measure in real time (sim/sightsweep.sh), FZ_FAST packs many tics into one frame. Read attribution: EXTRA=-DFZ_READSTATS. On E1M3/E1M6/E1M9 sight checks still remain (SEGS/LINEDEFS/NODES) — the next step is "sight data".

Next for speed (in order of choice):
1. A "sight data" lump: segments with endpoints and line flags laid out contiguously in BSP traversal order, so that P_CheckSight does not jump around SEGS/LINEDEFS.
2. Speed up subframe building (2.5 → ~1 ms).
3. Send the frame to the LCD via DMA (own driver), ~20% CPU.
4. Optional: limit the number of tics per frame (the game slows down instead of skipping).

## Tools
- `tools/framedump`: headless doomgeneric backend; plays a demo and dumps frames (RGB) and an object ID buffer. Renderer patch: `doomgeneric-ids.patch`. Build in WSL: `bash build.sh demo1`.
- `tools/ditherlab`: Python prototypes (`ditherlab.py`, `aware.py`, `clips.py`).

## Next
1. Capture `free` and `ps` on the device.
2. Build the official firmware via fbt with an empty built-in app, trim the app list, measure free flash.
3. Port the next-hack engine to Furi (input, timer, canvas output, SD).
4. WAD → Flipper format converter.
5. Dithering G in C inside the renderer.

## Doom-style menu (12.10.2026)

- Loader screens: `fap/ui.c` (M_DOOM logo, STCFN font, M_SKULL skull from doom1.wad via `tools/mkui.py`; logos in `files/ui.bin`, read from SD — whatever the FAP holds is taken from the zone). The screen is built in the first zone chunk (the engine is not running in the menu) and copied to the frame as a whole.
- The menu background is recorded gameplay: `FZ_NOHUD=1 FZ_PERIOD=15 FZ_DUMP1=demo.bin ./fzsim -nosound -timedemo demo3` (built with `-DSIGHT_MEMO_TICS=0` so the demo does not desync), `tools/mkbg.py` → `apps_data/flipper_doom/MENUBG.BIN` (1067 frames, 17.5 fps, 4.4 MB), lightened to 60% (WASH), dark text with a light halo. Preview: `tools/uipreview.py`.
- `fap/doom_menu.c`: the Doom tree (New Game → level with its name → difficulty, Nightmare confirmation; Options → volume 0-15 with sliders, calibration; Read This! — controls; Quit — Doom 1.9 quit messages and exit sound), menu sounds (DSPSTOP/DSPISTOL/DSSWTCHX/DSSWTCHN/DSSTNMOV), D_INTRO music. Level and difficulty selection in `menu.conf`, volumes in sound.conf (`V` + two hex digits (effects, music)).
- API v9: `start_skill`, the engine accepts `-skill 1..5`. On UV/Nightmare E1M6 does not fit in an 80 KB zone; the other maps get a smaller page cache (E1M3 7.5 KB).
