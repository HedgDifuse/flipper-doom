// LCD grayscale test: does flickering pixels faster than the slow STN panel
// can follow give stable gray levels on the Flipper screen, and at what rate?
//
// A gray level L of P is shown by lighting the pixel in L of every P
// subframes. Modes (OK cycles):
//   naive  - the L "on" subframes come first, all pixels in phase
//   spread - the zephray.me technique: the L pulses are spread evenly over the
//            period (50% = on/off/on/off) and every pixel of a 4x4 block gets
//            its own phase offset, so neighbours do not blink together
// Top: one bar per level. Bottom: horizontal gradient.
// Left/Right: subframe rate -/+1 Hz (hold to repeat), Up/Down: +/-10 Hz.
// Back: exit.
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>

#define RATE_MIN   10
#define RATE_MAX   300
#define MAX_PERIOD 8

typedef struct {
    const char* name;
    uint8_t period;
    bool spread;
} Mode;

static const Mode modes[] = {
    {"naive", 3, false},
    {"spread", 3, true},
    {"spread", 4, true},
    {"spread", 6, true},
    {"spread", 8, true},
};

static const uint8_t bayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

typedef struct {
    volatile int16_t rate_step; // pending change in Hz
    volatile bool next_mode;
    volatile bool quit;
} State;

static void input_cb(const void* value, void* ctx) {
    const InputEvent* e = value;
    State* s = ctx;
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return;
    if(e->key == InputKeyRight) s->rate_step += 1;
    if(e->key == InputKeyLeft) s->rate_step -= 1;
    if(e->key == InputKeyUp) s->rate_step += 10;
    if(e->key == InputKeyDown) s->rate_step -= 10;
    if(e->key == InputKeyOk) s->next_mode = true;
    if(e->key == InputKeyBack) s->quit = true;
}

// Is a pixel of gray level `level` (0..period) dark in subframe `sf`?
static bool dark(const Mode* m, int level, int sf, int x, int y) {
    const int p = m->period;
    if(!m->spread) return sf < level;
    // per-pixel phase from the 4x4 matrix, then Bresenham-spread pulses:
    // phase t is "on" when the running count floor(t*level/p) steps up
    int t = (sf + bayer4[y & 3][x & 3] * p / 16) % p;
    return (t + 1) * level / p != t * level / p;
}

static void render_subframes(Canvas* canvas, uint8_t frames[][1024], const Mode* m) {
    const int levels = m->period + 1;
    for(int sf = 0; sf < m->period; sf++) {
        canvas_clear(canvas);
        canvas_set_color(canvas, ColorBlack);
        int w = 128 / levels;
        for(int y = 12; y < 38; y++)
            for(int x = 0; x < levels * w; x++)
                if(dark(m, x / w, sf, x, y)) canvas_draw_dot(canvas, x, y);
        for(int y = 42; y < 64; y++)
            for(int x = 0; x < 128; x++)
                if(dark(m, (x * levels) / 128, sf, x, y)) canvas_draw_dot(canvas, x, y);
        memcpy(frames[sf], canvas_get_buffer(canvas), 1024);
    }
}

int32_t lcdgray_app(void* p) {
    UNUSED(p);
    State state = {0};
    FuriPubSub* input = furi_record_open(RECORD_INPUT_EVENTS);
    FuriPubSubSubscription* sub = furi_pubsub_subscribe(input, input_cb, &state);
    Gui* gui = furi_record_open(RECORD_GUI);
    Canvas* canvas = gui_direct_draw_acquire(gui);

    static uint8_t frames[MAX_PERIOD][1024];
    size_t mode = 0;
    int rate = 60;
    render_subframes(canvas, frames, &modes[mode]);

    const uint32_t cpu_us = furi_hal_cortex_instructions_per_microsecond();
    uint32_t shown = 0, window_start = DWT->CYCCNT, actual = 0, commit_ms = 0;
    uint32_t next = DWT->CYCCNT;
    char text[40];

    while(!state.quit) {
        if(state.rate_step) {
            rate += state.rate_step;
            state.rate_step = 0;
            if(rate < RATE_MIN) rate = RATE_MIN;
            if(rate > RATE_MAX) rate = RATE_MAX;
        }
        if(state.next_mode) {
            mode = (mode + 1) % COUNT_OF(modes);
            render_subframes(canvas, frames, &modes[mode]);
            state.next_mode = false;
            next = DWT->CYCCNT;
        }

        const Mode* m = &modes[mode];
        for(int sf = 0; sf < m->period && !state.quit && !state.next_mode; sf++) {
            uint32_t t0 = DWT->CYCCNT;
            memcpy(canvas_get_buffer(canvas), frames[sf], 1024);
            snprintf(text, sizeof(text), "%s%u %d/%luHz %lums", m->name, m->period, rate, actual, commit_ms);
            canvas_set_color(canvas, ColorBlack);
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, 0, 9, text);
            canvas_commit(canvas);
            commit_ms = (DWT->CYCCNT - t0) / cpu_us / 1000;
            shown++;

            // pace to the requested subframe rate
            next += cpu_us * (1000000U / rate);
            int32_t wait = (int32_t)(next - DWT->CYCCNT) / (int32_t)cpu_us;
            if(wait > 1000)
                furi_delay_ms(wait / 1000);
            else if(wait < -100000)
                next = DWT->CYCCNT; // fell behind, resync
            while((int32_t)(next - DWT->CYCCNT) > 0) furi_thread_yield(); // let USB and GUI run

            uint32_t elapsed = (DWT->CYCCNT - window_start) / cpu_us;
            if(elapsed >= 1000000) {
                actual = shown * 1000000ULL / elapsed;
                shown = 0;
                window_start = DWT->CYCCNT;
            }
        }
    }

    gui_direct_draw_release(gui);
    furi_record_close(RECORD_GUI);
    furi_pubsub_unsubscribe(input, sub);
    furi_record_close(RECORD_INPUT_EVENTS);
    return 0;
}
