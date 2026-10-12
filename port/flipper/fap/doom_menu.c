// flipper-doom: Doom's menu in the loader, see doom_menu.h.
#include "doom_menu.h"

#include <input/input.h>
#include <string.h>

#define CHOICE_FILE EXT_PATH("apps_data/flipper_doom/menu.conf")

#define FRAME_MS 57 // the background recording's 17.5 fps
#define SKULL_MS 229 // Doom's skull blinks every 8 tics
#define SONG_INTRO 11 // musicenum_t mus_intro: Doom's title music

typedef enum
{
	PageMain,
	PageLevel,
	PageSkill,
	PageNightmare,
	PageOptions,
	PageSound,
	PageReadThis,
	PageQuit,
} Page;

enum { MainNewGame, MainOptions, MainReadThis, MainQuit, MainItems };
enum { OptionsSound, OptionsCalibrate, OptionsItems };
enum { SoundEffects, SoundMusic, SoundItems };

static const char* const main_items[MainItems] = {"NEW GAME", "OPTIONS", "READ THIS!", "QUIT GAME"};
static const char* const levels[9] = {
	"HANGAR",     "NUCLEAR PLANT",      "TOXIN REFINERY",   "COMMAND CONTROL", "PHOBOS LAB",
	"CENTRAL PROCESSING", "COMPUTER STATION", "PHOBOS ANOMALY", "MILITARY BASE",
};
static const char* const skills[5] = {
	"I'M TOO YOUNG TO DIE", "HEY, NOT TOO ROUGH", "HURT ME PLENTY", "ULTRA-VIOLENCE", "NIGHTMARE!",
};
static const char* const options_items[OptionsItems] = {"SOUND VOLUME", "CALIBRATE SCREEN"};
static const char* const read_this[] = {
	"UP, DOWN: WALK", "LEFT, RIGHT: TURN", "OK: FIRE", "BACK: USE, OPEN", "HOLD BACK: QUIT",
};
// Doom 1.9's quit messages
static const char* const quit_messages[] = {
	"ARE YOU SURE YOU WANT TO QUIT THIS GREAT GAME?",
	"PLEASE DON'T LEAVE, THERE'S MORE DEMONS TO TOAST!",
	"LET'S BEAT IT -- THIS IS TURNING INTO A BLOODBATH!",
	"I WOULDN'T LEAVE IF I WERE YOU. DOS IS MUCH WORSE.",
	"YOU'RE TRYING TO SAY YOU LIKE DOS BETTER THAN ME, RIGHT?",
	"DON'T LEAVE YET -- THERE'S A DEMON AROUND THAT CORNER!",
	"YA KNOW, NEXT TIME YOU COME IN HERE I'M GONNA TOAST YA.",
	"GO AHEAD AND LEAVE. SEE IF I CARE.",
};
static const char* const quit_sounds[] = {"DSPLDETH", "DSDMPAIN", "DSPOPAIN", "DSSLOP",
                                          "DSTELEPT", "DSPOSIT1", "DSPOSIT3", "DSSGTATK"};

typedef struct
{
	const MenuEnv* env;
	Page page;
	uint8_t main, level, skill, options, sound; // the selection on each page
	uint8_t quit_message;
	SoundConfig volumes;
	uint32_t started;
} Menu;

// kept between calls: back from the calibration on the Options page
static Page last_page = PageMain;
static uint8_t last_main, last_options;


static void load_choice(Menu* m)
{
	m->level = 0;
	m->skill = 2; // Hurt me plenty, as Doom
	File* f = storage_file_alloc(m->env->storage);
	char c[2];
	if(storage_file_open(f, CHOICE_FILE, FSAM_READ, FSOM_OPEN_EXISTING) && storage_file_read(f, c, 2) == 2) {
		if(c[0] >= '1' && c[0] <= '9') m->level = c[0] - '1';
		if(c[1] >= '1' && c[1] <= '5') m->skill = c[1] - '1';
	}
	storage_file_free(f);
}


static void save_choice(Menu* m)
{
	File* f = storage_file_alloc(m->env->storage);
	const char c[2] = {'1' + m->level, '1' + m->skill};
	if(storage_file_open(f, CHOICE_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS)) storage_file_write(f, c, 2);
	storage_file_free(f);
}


// ---- drawing ----------------------------------------------------------------

#define ITEM_X   11 // the skull left of it
#define TITLE_Y  1
#define LIST_TOP 11 // under a title

static int skull_frame(Menu* m)
{
	return (furi_get_tick() - m->started) / SKULL_MS & 1;
}

// items from top, the selected one with the skull; scrolls to keep it in view
static void list(Menu* m, int top, const char* const* items, int count, int selected)
{
	Ui* ui = m->env->ui;
	const int rows = (64 - top) / UI_FONT_LINE;
	int first = selected - rows + 1;
	if(first < 0) first = 0;
	for(int i = first; i < count && i < first + rows; i++) {
		const int y = top + (i - first) * UI_FONT_LINE;
		const int w = ui_text_width(items[i]);
		// centered; the widest items leave just room for the skull
		int x = (128 - w) / 2;
		if(x < ITEM_X) x = ITEM_X;
		ui_text(ui, x, y, items[i]);
		if(i == selected) ui_draw_skull(ui, x - ITEM_X, y - 1, skull_frame(m));
	}
}

// Doom's thermometer: 16 steps
static void slider(Ui* ui, int x, int y, int value)
{
	ui_box(ui, x, y, 2 + 16 * 6, 7, 0);
	ui_box(ui, x + 1, y + 1, 16 * 6, 5, 15);
	for(int i = 1; i < 16; i++) ui_box(ui, x + 1 + i * 6, y + 3, 1, 3, 0);
	ui_box(ui, x + 1 + value * 6, y, 6, 7, 0);
}

// word wrapped and centered around the middle of the screen
static void message(Ui* ui, const char* text, const char* hint)
{
	char lines[6][40];
	int n = 0;
	const char* p = text;
	while(*p && n < 6) {
		char line[40] = "";
		const char* best = p;
		for(const char* q = p;; q++) {
			if(*q == ' ' || *q == 0) {
				char test[40];
				const int len = q - p < 39 ? q - p : 39;
				memcpy(test, p, len);
				test[len] = 0;
				if(ui_text_width(test) > 124 && best != p) break;
				strcpy(line, test);
				best = q;
				if(*q == 0) break;
			}
		}
		strcpy(lines[n++], line);
		p = *best ? best + 1 : best;
	}
	const int total = (n + 2) * 9;
	int y = (64 - total) / 2;
	for(int i = 0; i < n; i++, y += 9) ui_text_center(ui, y, lines[i]);
	ui_text_center(ui, y + 9, hint);
}

static void draw(Menu* m)
{
	Ui* ui = m->env->ui;
	ui_clear(ui, true);
	switch(m->page) {
	case PageMain: {
		const int logo = ui_logo_height(UiLogoSmall);
		ui_logo(ui, UiLogoSmall, (128 - 60) / 2, 0);
		list(m, logo + 3, main_items, MainItems, m->main);
		break;
	}
	case PageLevel: {
		char title[24];
		snprintf(title, sizeof(title), "E1M%d", m->level + 1);
		ui_text_center(ui, TITLE_Y, title);
		list(m, LIST_TOP, levels, 9, m->level);
		break;
	}
	case PageSkill:
		ui_text_center(ui, TITLE_Y, "CHOOSE SKILL LEVEL:");
		list(m, LIST_TOP + 2, skills, 5, m->skill);
		break;
	case PageNightmare:
		message(ui, "ARE YOU SURE? THIS SKILL LEVEL ISN'T EVEN REMOTELY FAIR.", "OK: YES  BACK: NO");
		break;
	case PageOptions:
		ui_text_center(ui, TITLE_Y, "OPTIONS");
		list(m, LIST_TOP + 4, options_items, OptionsItems, m->options);
		break;
	case PageSound: {
		ui_text_center(ui, TITLE_Y, "SOUND VOLUME");
		static const char* const labels[SoundItems] = {"SFX VOLUME", "MUSIC VOLUME"};
		const uint8_t values[SoundItems] = {m->volumes.effects, m->volumes.music};
		for(int i = 0; i < SoundItems; i++) {
			const int y = LIST_TOP + 3 + i * 2 * UI_FONT_LINE + i * 4;
			const int x = (128 - ui_text_width(labels[i])) / 2;
			ui_text(ui, x, y, labels[i]);
			slider(ui, (128 - 98) / 2, y + UI_FONT_LINE + 1, values[i]);
			if(i == m->sound) ui_draw_skull(ui, x - ITEM_X, y - 1, skull_frame(m));
		}
		if(sound_muted(m->env->sound))
			ui_text_center(ui, 56, "STEALTH MODE: MUTED");
		else if(sound_needs_restart(m->env->sound))
			ui_text_center(ui, 56, "RESTART THE APP");
		break;
	}
	case PageReadThis:
		ui_text_center(ui, TITLE_Y, "READ THIS!");
		for(size_t i = 0; i < COUNT_OF(read_this); i++)
			ui_text_center(ui, LIST_TOP + 3 + i * UI_FONT_LINE + i, read_this[i]);
		break;
	case PageQuit:
		message(ui, quit_messages[m->quit_message], "OK: QUIT  BACK: NO");
		break;
	}
	ui_present(ui, m->env->frame, false);
}


// ---- input --------------------------------------------------------------

static void sfx(Menu* m, const char* lump)
{
	m->env->play(m->env->context, lump);
}

// moves a selection; true if it moved
static bool move(Menu* m, uint8_t* selected, int count, InputKey key)
{
	if(key == InputKeyUp) *selected = (*selected + count - 1) % count;
	else if(key == InputKeyDown) *selected = (*selected + 1) % count;
	else return false;
	sfx(m, "DSPSTOP");
	return true;
}

static void go(Menu* m, Page page, const char* lump)
{
	m->page = page;
	sfx(m, lump);
}

static void set_volumes(Menu* m)
{
	sound_set_config(m->env->sound, m->volumes);
	sound_config_save(m->env->storage, m->volumes);
}

// one key; returns true when the menu is done with *action
static bool key(Menu* m, InputKey key, bool repeat, MenuAction* action)
{
	const bool back = key == InputKeyBack, ok = key == InputKeyOk;
	switch(m->page) {
	case PageMain:
		if(move(m, &m->main, MainItems, key)) break;
		if(back) {
			m->quit_message = furi_get_tick() % COUNT_OF(quit_messages);
			go(m, PageQuit, "DSSWTCHN");
		} else if(ok && !repeat) {
			static const Page pages[MainItems] = {PageLevel, PageOptions, PageReadThis, PageQuit};
			if(m->main == MainQuit) m->quit_message = furi_get_tick() % COUNT_OF(quit_messages);
			go(m, pages[m->main], "DSPISTOL");
		}
		break;
	case PageLevel:
		if(move(m, &m->level, 9, key)) break;
		if(back) go(m, PageMain, "DSSWTCHX");
		else if(ok && !repeat) go(m, PageSkill, "DSPISTOL");
		break;
	case PageSkill:
		if(move(m, &m->skill, 5, key)) break;
		if(back) go(m, PageLevel, "DSSWTCHX");
		else if(ok && !repeat) {
			if(m->skill == 4) {
				go(m, PageNightmare, "DSSWTCHN");
				break;
			}
			sfx(m, "DSPISTOL");
			*action = MenuActionPlay;
			return true;
		}
		break;
	case PageNightmare:
		if(back) go(m, PageSkill, "DSSWTCHX");
		else if(ok && !repeat) {
			sfx(m, "DSPISTOL");
			*action = MenuActionPlay;
			return true;
		}
		break;
	case PageOptions:
		if(move(m, &m->options, OptionsItems, key)) break;
		if(back) go(m, PageMain, "DSSWTCHX");
		else if(ok && !repeat) {
			if(m->options == OptionsSound) go(m, PageSound, "DSPISTOL");
			else {
				sfx(m, "DSPISTOL");
				*action = MenuActionCalibrate;
				return true;
			}
		}
		break;
	case PageSound: {
		if(move(m, &m->sound, SoundItems, key)) break;
		if(back) {
			go(m, PageOptions, "DSSWTCHX");
			break;
		}
		uint8_t* v = m->sound == SoundEffects ? &m->volumes.effects : &m->volumes.music;
		if(key == InputKeyLeft && *v > 0) (*v)--;
		else if(key == InputKeyRight && *v < SOUND_VOLUME_MAX) (*v)++;
		else break;
		set_volumes(m);
		sfx(m, "DSSTNMOV");
		break;
	}
	case PageReadThis:
		if(back || ok) go(m, PageMain, "DSSWTCHX");
		break;
	case PageQuit:
		if(back) go(m, PageMain, "DSSWTCHX");
		else if(ok && !repeat) {
			sfx(m, quit_sounds[furi_get_tick() % COUNT_OF(quit_sounds)]);
			*action = MenuActionQuit;
			return true;
		}
		break;
	}
	return false;
}


MenuAction doom_menu_run(const MenuEnv* env, MenuChoice* choice)
{
	Menu m = {.env = env, .page = last_page, .main = last_main, .options = last_options};
	m.started = furi_get_tick();
	m.volumes = sound_config(env->sound);
	load_choice(&m);
	sound_music(env->sound, SONG_INTRO, true);
	furi_message_queue_reset(env->input); // what was pressed in the game
	if(m.page == PageMain) sfx(&m, "DSSWTCHN");

	MenuAction action = MenuActionQuit;
	uint32_t next = furi_get_tick();
	bool done = false;
	while(!done) {
		draw(&m);
		next += furi_ms_to_ticks(FRAME_MS);
		int32_t wait;
		while((wait = (int32_t)(next - furi_get_tick())) > 0 && !done) {
			InputEvent e;
			if(furi_message_queue_get(env->input, &e, wait) != FuriStatusOk) break;
			if(e.type == InputTypePress || e.type == InputTypeRepeat)
				done = key(&m, e.key, e.type == InputTypeRepeat, &action);
		}
		if((int32_t)(furi_get_tick() - next) > 500) next = furi_get_tick(); // fell behind (SD busy)
	}

	if(action == MenuActionQuit) {
		// Doom lets the quit sound play out
		const uint32_t until = furi_get_tick() + furi_ms_to_ticks(1200);
		while((int32_t)(until - furi_get_tick()) > 0) {
			draw(&m);
			furi_delay_ms(FRAME_MS);
		}
	}
	last_page = action == MenuActionCalibrate ? PageOptions : PageMain;
	last_main = m.main;
	last_options = m.options;
	choice->map = m.level + 1;
	choice->skill = m.skill + 1;
	if(action == MenuActionPlay) save_choice(&m);
	return action;
}
