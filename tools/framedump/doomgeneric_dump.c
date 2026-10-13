// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
// Headless doomgeneric backend: plays a demo on a virtual clock and dumps
// every 35 Hz game tic as a raw 320x200 RGB frame into frames.rgb and the
// per-pixel object ID buffer (uint16 LE) into frames.ids.
#include "doomgeneric.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// filled by the patched renderer (ref/doomgeneric/doomgeneric/dg_ids.h)
extern uint16_t dg_idbuf[DOOMGENERIC_RESX * DOOMGENERIC_RESY];
static FILE *ids;
static int frame_no;

static FILE *out;
static uint32_t now_ms;
static uint32_t last_dump_ms = 0xffffffff;

void DG_Init(void) {
    out = fopen("frames.rgb", "wb");
    ids = fopen("frames.ids", "wb");
    if (!out) { perror("frames.rgb"); exit(1); }
}

void DG_DrawFrame(void) {
    // one frame per game tic (1000/35 ms), drop duplicates
    uint32_t tic = now_ms * 35 / 1000;
    if (tic == last_dump_ms) return;
    last_dump_ms = tic;
    static uint8_t rgb[DOOMGENERIC_RESX * DOOMGENERIC_RESY * 3];
    for (int i = 0; i < DOOMGENERIC_RESX * DOOMGENERIC_RESY; i++) {
        uint32_t p = DG_ScreenBuffer[i];
        rgb[i * 3 + 0] = (p >> 16) & 0xff;
        rgb[i * 3 + 1] = (p >> 8) & 0xff;
        rgb[i * 3 + 2] = p & 0xff;
    }
    fwrite(rgb, 1, sizeof(rgb), out);
    fwrite(dg_idbuf, 2, DOOMGENERIC_RESX * DOOMGENERIC_RESY, ids);
    memset(dg_idbuf, 0, sizeof(dg_idbuf));
    frame_no++;
    fflush(out);
    fflush(ids);
}

void DG_SleepMs(uint32_t ms) { now_ms += ms; }
uint32_t DG_GetTicksMs(void) { return now_ms; }
int DG_GetKey(int *pressed, unsigned char *key) { (void)pressed; (void)key; return 0; }
void DG_SetWindowTitle(const char *title) { (void)title; }

int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);
    for (;;) doomgeneric_Tick();
}
