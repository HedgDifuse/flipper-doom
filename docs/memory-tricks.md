# Как другие порты ужимали память под объекты и уровни (исследование 12.10.2026)

Ближе всего к нам порты next-hack (IKEA-лампа MG21: 108 КБ RAM, zone 78,6 КБ, E1M6 «typically under 75 kB»; nRF52840; MG24). Они, как и Doom8088, выросли из GBADoom, так что приёмы ложатся почти дословно.

## Счёт по doom1.wad (skill 2, без мультиплеерных вещей)

| Карта | Монстры | Пикапы | Бочки | Декор | Sectors | Sides | sec->lines | Subsectors | Ячеек BM |
|---|---|---|---|---|---|---|---|---|---|
| E1M2 | 41 | 106 | 24 | 28 | 200 | 1323 | 1322 | 448 | 1302 |
| E1M3 | 74 | 175 | 28 | 32 | 177 | 1326 | 1318 | 461 | 850 |
| E1M5 | 91 | 93 | 28 | 3 | 143 | 1053 | 1051 | 384 | 832 |
| E1M6 | 118 | 214 | 22 | 15 | 250 | 1727 | 1719 | 606 | 1748 |
| E1M7 | 84 | 146 | 16 | 20 | 170 | 1223 | 1220 | 467 | 864 |
| E1M8 | 27 | 36 | 28 | 7 | 74 | 511 | 507 | 177 | 2912 |

На E1M6 кандидатов в static ~229 из 369 вещей (62%). Бочки не годятся (MF_SHOOTABLE).

## Приёмы по убыванию «экономия / объём правок»

1. **Префиксный static_mobj_t для пикапов и декораций** (nRF52840, MG21, MG24, rp2040, D32XR). static_mobj_t — начало mobj_t с тем же порядком полей (thinker, flags, ссылки blockmap/sector, tics, type, sprite/frame, state, subsector, x, y, z); всё ниже маркера отсутствует. Код работает через `mobj_t*`, тип выбирается флагом `MF_STATIC (MF_DECORATION | MF_SPECIAL)`. radius/height через аксессоры из mobjinfo. В nRF: 92 → 44 байта, 29 упоминаний в 6 файлах. Анимированные декорации получают «безмозглый» thinker, неанимированные — NULL. Подводные камни: предметы на лифтах и в давилках (только пересчёт z, без гибсов), звук от предмета, телепорт/ZMovement не вызываются, респаун, сейвы. Прогноз для нас на E1M6: ~17 КБ, с x/y из THINGS на SD (как MG24) 20–22 КБ.
   - nRF52840Doom: `Doom/include/p_mobj.h` (MOBJ_SIZE 92, STATIC_MOBJ_SIZE 44), `source/p_mobj.c` (P_SpawnStaticMobj, P_RemoveStaticMobj), `p_map.c`.
   - rp2040: ядро mobj_t 32 байта + mobjfull_t 84 байта, `src/doom/p_mobj.h`.
   - D32XR: `static_mobj_size (offsetof(mobj_t,angle))`, P_PreSpawnMobjs.
   - MG24: static 20/29 байт, x/y во flash по pos_index, константные флаги из mobjinfo.
2. **Убрать msecnode_t, вернуть ванильный P_ChangeSector** (MG24, `USE_MSECNODE`). touching_thinglist нужен только P_CheckSector; вместо него обход blockmap по bbox сектора + MAXRADIUS. Экономия: ноды × 28 байт + 4 байта на mobj и сектор, на E1M6 ~10 КБ (снять счётчиком). Бонус: меньше CPU на P_SetThingPosition.
3. **Short pointers / 16-битные индексы** (nRF `i_memory.h`, rp2040 `shortptr_t`, D32XR `SPTR`). У STM32WB55 RAM 0x20000000–0x2003FFFF, 16 бит × 4 покрывают всё (при выравнивании zone на 4). По выгоде: blocklinks uint16 (E1M6 −3,5 КБ, E1M8 −5,8 КБ; правки в P_LoadBlockMap, P_Set/UnsetThingPosition, P_BlockThingsIterator), ссылки в mobj и thinker (−2 байта каждая), индексы в subsector/mobj (sector_num, state_idx).
4. **Односвязные цепочки** sector/blockmap: убрать sprev/bprev (−8 байт на mobj), unlink линейным проходом.
5. **Уплотнение полей mobj** (MG24 92 → 52): z/floorz/ceilingz в 16 бит, momx/momy 24 бита, angle 16 бит, битовые поля счётчиков, без lastenemy. Ещё 4–9 КБ на E1M6, но трогает арифметику.
6. **Константное — во flash/на SD, в RAM только изменяемое**: sector_t делится на константу и ramSector_t (16 байт; на E1M6 9–11 КБ), sides целиком константны + маленький список изменённых текстур переключателей (E1M6 −13,8 КБ, но рендер читает sides с SD — риск fps), sector->lines готовым лампом, subsectors битовыми полями.
7. **Аллокатор**: заголовок блока 8 байт вместо 16 (nRF), пулы с битовой маской.

## Порядок для E1M6 (128 → ≤82 КБ)

static_mobj (~17 КБ) → убрать msecnode (~10 КБ) → blocklinks uint16 (~3,5 КБ) → sides на SD или split sector_t (~10–14 КБ). Первые три: ~30 КБ при небольших правках.

## Источники

- https://next-hack.com/index.php/2021/11/13/porting-doom-to-an-nrf52840-based-usb-bluetooth-le-dongle/
- https://github.com/next-hack/nRF52840Doom
- https://next-hack.com/index.php/2023/12/10/multiplayer-doom-on-the-sparkfun-thing-plus-matter-board/
- https://github.com/next-hack/MG24_Doom_BLE
- https://github.com/marciopocebon/MG21DOOM
- https://kilograham.github.io/rp2040-doom/speed_and_ram.html
- https://github.com/kilograham/rp2040-doom
- https://github.com/viciious/d32xr
