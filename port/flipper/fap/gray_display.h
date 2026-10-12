// flipper-doom: gray levels on the 1-bit LCD by flashing subframes, see gray_display.c.
#pragma once

#include <gui/gui.h>
#include <stdint.h>

#define GRAY_FRAME_SIZE (128 * 64 / 2) // 4 bits per pixel, even x in the low nibble

typedef struct
{
	uint16_t rate10; // subframes per 10 seconds (0.1 Hz steps: must match the LCD's own frame rate)
	uint8_t  period; // subframes per cycle = darkest level
	float    gamma;  // LCD response correction, see platform/fz_graymap.h
} GrayConfig;

typedef struct GrayDisplay GrayDisplay;

// Drawn with the canvas over every subframe (menus, calibration hints).
typedef void (*GrayOverlay)(Canvas* canvas, void* context);

// Takes the screen (direct draw) and starts flashing `frame` with `config`.
GrayDisplay* gray_display_start(Gui* gui, uint8_t* frame, GrayConfig config);
void gray_display_stop(GrayDisplay* display);

void gray_display_set_config(GrayDisplay* display, GrayConfig config);
void gray_display_set_overlay(GrayDisplay* display, GrayOverlay overlay, void* context);

// Subframes actually shown per second, measured.
uint16_t gray_display_actual_rate(GrayDisplay* display);

// Average microseconds per subframe spent building it and sending it to the LCD.
void gray_display_costs(GrayDisplay* display, uint32_t* build_us, uint32_t* commit_us);

// How late subframes reached the LCD since the last reset: the worst delay and
// how many were more than 1 ms late (late subframes make the grays drift).
void gray_display_lateness(GrayDisplay* display, uint32_t* worst_us, uint32_t* over_1ms);
void gray_display_lateness_reset(GrayDisplay* display);

void gray_frame_put(uint8_t* frame, int x, int y, uint8_t level);
