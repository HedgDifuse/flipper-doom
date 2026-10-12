/*
 * flipper-doom: per-device screen calibration.
 *
 * How a blinking pixel looks depends on the panel (batch, temperature,
 * contrast), so the player tunes three things once and they are saved to
 * display.conf:
 *   1. subframe rate: equal to the LCD's own frame rate, so the grays stand still
 *   2. number of levels: as many as stay distinguishable
 *   3. gamma: the tone gradient looks even
 */

#include <furi.h>
#include <input/input.h>

#include "../../platform/fz_graymap.h"
#include "calibration.h"

#define CONFIG_DIR  EXT_PATH("apps_data/flipper_doom")
#define CONFIG_FILE EXT_PATH("apps_data/flipper_doom/display.conf")

static const uint8_t periods[] = {3, 4, 6, 8};
static const uint8_t bayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};


bool gray_config_load(Storage* storage, GrayConfig* config)
{
	File* f = storage_file_alloc(storage);
	bool ok = false;
	if(storage_file_open(f, CONFIG_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
		char text[64] = {0};
		storage_file_read(f, text, sizeof(text) - 1);
		int rate10 = 0, period = 0, gamma100 = 0;
		if(sscanf(text, "rate10=%d period=%d gamma=%d", &rate10, &period, &gamma100) == 3 && rate10 >= 100 &&
		   rate10 <= 3000 && period >= 2 && period <= FZ_GRAY_MAX_PERIOD && gamma100 >= 20 && gamma100 <= 400) {
			config->rate10 = rate10;
			config->period = period;
			config->gamma = gamma100 / 100.0f;
			ok = true;
		}
	}
	storage_file_free(f);
	return ok;
}


void gray_config_save(Storage* storage, const GrayConfig* config)
{
	storage_simply_mkdir(storage, CONFIG_DIR);
	File* f = storage_file_alloc(storage);
	if(storage_file_open(f, CONFIG_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
		char text[64];
		int n = snprintf(text, sizeof(text), "rate10=%d period=%d gamma=%d\n", config->rate10, config->period,
		                 (int)(config->gamma * 100 + 0.5f));
		storage_file_write(f, text, n);
	}
	storage_file_free(f);
}


void calibration_pattern(uint8_t* frame, const GrayConfig* config, bool full_gradient)
{
	uint8_t lut[64];
	fz_build_lum_to_level(lut, config->period, config->gamma);
	memset(frame, 0, GRAY_FRAME_SIZE);

	const int levels = config->period + 1;
	const int bar_w = 128 / levels;
	for(int y = 12; y < 64; y++) {
		for(int x = 0; x < 128; x++) {
			uint8_t level;
			if(!full_gradient && y < 38) {
				if(x >= levels * bar_w) continue;
				level = x / bar_w; // raw levels, no gamma
			} else if(!full_gradient && y < 42) {
				continue;
			} else {
				// Doom tone: white at the left, black at the right, as the engine maps it
				uint8_t v = lut[63 - x * 63 / 127];
				level = (v >> 4) + ((v & 15) > bayer4[y & 3][x & 3]);
				if(level > config->period) level = config->period;
			}
			gray_frame_put(frame, x, y, level);
		}
	}
}


typedef struct
{
	GrayDisplay* display;
	int step;
	GrayConfig config;
	char line[40];
} Calibration;


static void overlay(Canvas* canvas, void* context)
{
	Calibration* c = context;
	canvas_set_color(canvas, ColorWhite);
	canvas_draw_box(canvas, 0, 0, 128, 11);
	canvas_set_color(canvas, ColorBlack);
	canvas_set_font(canvas, FontSecondary);
	canvas_draw_str(canvas, 1, 9, c->line);
}


static void update_line(Calibration* c)
{
	switch(c->step) {
	case 0:
		snprintf(c->line, sizeof(c->line), "1/3 Steady? <> %d.%dHz (%d)", c->config.rate10 / 10,
		         c->config.rate10 % 10, gray_display_actual_rate(c->display));
		break;
	case 1:
		snprintf(c->line, sizeof(c->line), "2/3 Bars differ? <> %d grays", c->config.period + 1);
		break;
	default:
		snprintf(c->line, sizeof(c->line), "3/3 Even fade? <> gamma %d.%02d", (int)c->config.gamma,
		         (int)(c->config.gamma * 100 + 0.5f) % 100);
		break;
	}
}


bool calibration_run(GrayDisplay* display, uint8_t* frame, GrayConfig* config, FuriMessageQueue* input, Storage* storage)
{
	Calibration c = {.display = display, .step = 0, .config = *config};
	gray_display_set_config(display, c.config);
	calibration_pattern(frame, &c.config, false);
	update_line(&c);
	gray_display_set_overlay(display, overlay, &c);

	bool done = false, cancelled = false;
	while(!done && !cancelled) {
		InputEvent e;
		if(furi_message_queue_get(input, &e, 500) != FuriStatusOk) {
			update_line(&c); // refresh the measured rate
			continue;
		}
		if(e.type != InputTypeShort && e.type != InputTypeRepeat) continue;

		int dir = e.key == InputKeyRight ? 1 : e.key == InputKeyLeft ? -1 : 0;
		if(c.step == 0) {
			// left/right 0.1 Hz, up/down 1 Hz
			if(e.key == InputKeyUp) dir = 10;
			if(e.key == InputKeyDown) dir = -10;
			c.config.rate10 = CLAMP(c.config.rate10 + dir, 3000, 100);
		} else if(c.step == 1 && dir) {
			size_t i = 0;
			while(i < COUNT_OF(periods) && periods[i] != c.config.period) i++;
			i = (i + COUNT_OF(periods) + dir) % COUNT_OF(periods);
			c.config.period = periods[i];
		} else if(c.step == 2 && dir) {
			c.config.gamma = CLAMP(c.config.gamma + dir * 0.1f, 4.0f, 0.2f);
		}

		if(e.key == InputKeyOk) {
			if(++c.step == 3) done = true;
		}
		if(e.key == InputKeyBack) {
			if(c.step == 0)
				cancelled = true;
			else
				c.step--;
		}

		gray_display_set_config(display, c.config);
		calibration_pattern(frame, &c.config, c.step == 2);
		update_line(&c);
	}

	gray_display_set_overlay(display, NULL, NULL);
	if(done) {
		*config = c.config;
		gray_config_save(storage, config);
	}
	gray_display_set_config(display, *config);
	return done;
}
