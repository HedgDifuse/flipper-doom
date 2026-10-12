/*
 * flipper-doom: Doom's original sounds and music on the Flipper's speaker.
 *
 * Effects: the DS* lumps (added to the WAD from the IWAD by tools/fztex.py)
 * are DMX sounds: uint16 format 3, uint16 sample rate (11025 Hz), uint32
 * count, then count unsigned 8-bit samples of which the first and last 16
 * are padding. The engine already picks which effect plays (one channel, by
 * priority) and how loud it is for the distance; a new one replaces the one
 * playing.
 *
 * Music: Doom's MUS tracks, rendered offline through an OPL emulator with the
 * DMX instrument bank (tools/mkmusic.py) into raw 8-bit 11025 Hz files in
 * apps_data/flipper_doom/music, streamed from the SD card and looped.
 *
 * Played the way the Flipper's WAV player does it: the speaker's timer TIM16
 * runs as an 8-bit PWM (125 kHz carrier), and TIM2 at 11025 Hz makes DMA1
 * channel 1 copy the next sample into its compare register, from a ring
 * buffer of two halves. When the DMA is done with a half, its interrupt wakes
 * the player thread, which refills that half: the music, and the effect mixed
 * on top (the music ducked meanwhile). The CPU touches every sample once or
 * twice. Both are made as loud as the piezo allows offline
 * (tools/loudness.py: no bass, compressed, soft-limited).
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_bus.h>
#include <stm32wbxx_ll_dma.h>
#include <stm32wbxx_ll_tim.h>

#include "sound.h"

#define TAG "DoomSound"

#define RATE         11025
#define HALF         512 // samples per half of the ring buffer, 46 ms
#define TIMER_HZ     64000000U
#define PAD          16 // DMX padding samples at each end
#define SILENCE      128
#define MUSIC_DUCKED 140 // of 256: the music under an effect (both are made loud, see tools/loudness.py)
#define CONFIG       EXT_PATH("apps_data/flipper_doom/sound.conf")
#define MUSIC_DIR    EXT_PATH("apps_data/flipper_doom/music")

#define PWM_TIMER  TIM16 // the speaker's
#define RATE_TIMER TIM2
#define DMA        DMA1, LL_DMA_CHANNEL_1

enum
{
	FlagEffect = 1 << 0, // an effect to start
	FlagMusic  = 1 << 1, // a track to start (or stop)
	FlagHalf   = 1 << 2, // the DMA moved on to the second half: refill the first
	FlagFull   = 1 << 3, // and the other way round
	FlagExit   = 1 << 4,
	FlagAll    = FlagEffect | FlagMusic | FlagHalf | FlagFull | FlagExit,
};

// musicenum_t order
static const char* const tracks[] = {
	NULL, "D_E1M1", "D_E1M2", "D_E1M3", "D_E1M4", "D_E1M5", "D_E1M6", "D_E1M7",
	"D_E1M8", "D_E1M9", "D_INTER", "D_INTRO", "D_VICTOR", "D_INTROA",
};

struct Sound
{
	FuriThread* thread;
	Storage* storage;
	File* volatile wad; // the engine's, see sound_set_wad
	FuriMutex* volatile lock;
	volatile bool effects, music; // on now (a volume above 0, not in stealth mode)
	volatile uint8_t effects_volume, music_volume; // 0..SOUND_VOLUME_MAX
	bool allocated;               // the thread exists (something was on at app start)

	// requests from the engine's thread
	volatile uint32_t effect_pos;
	volatile uint16_t effect_length;
	volatile uint8_t effect_volume;
	volatile uint8_t track;
	volatile bool track_loops;

	// player thread only
	uint32_t pos, end; // WAD file positions of the effect samples left
	int16_t volume;    // of the effect, 0..256
	File* track_file;  // NULL: no music
	bool looping;
	bool running;
	uint8_t silent_halves;
	uint8_t mix[HALF];
	uint8_t buffer[2 * HALF];
};


static void dma_isr(void* context)
{
	Sound* s = context;
	if(LL_DMA_IsActiveFlag_HT1(DMA1)) {
		LL_DMA_ClearFlag_HT1(DMA1);
		furi_thread_flags_set(furi_thread_get_id(s->thread), FlagHalf);
	}
	if(LL_DMA_IsActiveFlag_TC1(DMA1)) {
		LL_DMA_ClearFlag_TC1(DMA1);
		furi_thread_flags_set(furi_thread_get_id(s->thread), FlagFull);
	}
}


static void hw_init(Sound* s)
{
	if(!furi_hal_bus_is_enabled(FuriHalBusTIM2)) furi_hal_bus_enable(FuriHalBusTIM2);

	// the speaker: 8-bit PWM, its duty cycle is the sample
	LL_TIM_InitTypeDef tim = {0};
	tim.Prescaler = 1;
	tim.Autoreload = 255;
	LL_TIM_Init(PWM_TIMER, &tim);
	LL_TIM_OC_InitTypeDef oc = {0};
	oc.OCMode = LL_TIM_OCMODE_PWM1;
	oc.OCState = LL_TIM_OCSTATE_ENABLE;
	oc.CompareValue = SILENCE;
	LL_TIM_OC_Init(PWM_TIMER, LL_TIM_CHANNEL_CH1, &oc);

	// the sample clock, its update event is the DMA request
	tim.Prescaler = 0;
	tim.Autoreload = TIMER_HZ / RATE - 1;
	LL_TIM_Init(RATE_TIMER, &tim);

	LL_DMA_ConfigAddresses(DMA, (uint32_t)s->buffer, (uint32_t)&PWM_TIMER->CCR1, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
	LL_DMA_SetPeriphRequest(DMA, LL_DMAMUX_REQ_TIM2_UP);
	LL_DMA_SetDataTransferDirection(DMA, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
	LL_DMA_SetChannelPriorityLevel(DMA, LL_DMA_PRIORITY_VERYHIGH);
	LL_DMA_SetMode(DMA, LL_DMA_MODE_CIRCULAR);
	LL_DMA_SetPeriphIncMode(DMA, LL_DMA_PERIPH_NOINCREMENT);
	LL_DMA_SetMemoryIncMode(DMA, LL_DMA_MEMORY_INCREMENT);
	LL_DMA_SetPeriphSize(DMA, LL_DMA_PDATAALIGN_HALFWORD);
	LL_DMA_SetMemorySize(DMA, LL_DMA_MDATAALIGN_BYTE);
	LL_DMA_EnableIT_TC(DMA);
	LL_DMA_EnableIT_HT(DMA);
	furi_hal_interrupt_set_isr(FuriHalInterruptIdDma1Ch1, dma_isr, s);
}


static void hw_start(Sound* s)
{
	LL_DMA_SetDataLength(DMA, sizeof(s->buffer));
	LL_DMA_EnableChannel(DMA);
	LL_TIM_EnableDMAReq_UPDATE(RATE_TIMER);
	LL_TIM_EnableAllOutputs(PWM_TIMER);
	LL_TIM_EnableCounter(PWM_TIMER);
	LL_TIM_EnableCounter(RATE_TIMER);
	s->running = true;
}


static void hw_stop(Sound* s)
{
	LL_TIM_DisableCounter(RATE_TIMER);
	LL_TIM_DisableDMAReq_UPDATE(RATE_TIMER);
	LL_DMA_DisableChannel(DMA);
	LL_TIM_DisableAllOutputs(PWM_TIMER);
	LL_TIM_DisableCounter(PWM_TIMER);
	s->running = false;
}


static void hw_deinit(void)
{
	furi_hal_interrupt_set_isr(FuriHalInterruptIdDma1Ch1, NULL, NULL);
	if(furi_hal_bus_is_enabled(FuriHalBusTIM2)) furi_hal_bus_disable(FuriHalBusTIM2);
}


// Read from the shared WAD handle; false if there is none or it failed.
static bool wad_read(Sound* s, uint32_t pos, void* dest, size_t length)
{
	FuriMutex* lock = s->lock;
	if(!lock) return false;
	furi_mutex_acquire(lock, FuriWaitForever);
	bool ok = s->wad && storage_file_seek(s->wad, pos, true) && storage_file_read(s->wad, dest, length) == length;
	furi_mutex_release(lock);
	return ok;
}


static void close_track(Sound* s)
{
	if(s->track_file) {
		storage_file_free(s->track_file);
		s->track_file = NULL;
	}
}


static void open_track(Sound* s)
{
	close_track(s);
	const uint8_t track = s->track;
	if(!s->music || track == 0 || track >= COUNT_OF(tracks)) return;
	char path[64];
	snprintf(path, sizeof(path), MUSIC_DIR "/%s.PCM", tracks[track]);
	s->track_file = storage_file_alloc(s->storage);
	if(!storage_file_open(s->track_file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
		FURI_LOG_W(TAG, "no %s", path);
		close_track(s);
	}
	s->looping = s->track_loops;
}


// The next HALF samples of the music (looped) into out, at full volume or
// ducked under an effect; the number of samples that were music.
static size_t fill_music(Sound* s, uint8_t* out)
{
	size_t n = 0;
	while(s->track_file && s->music && n < HALF) {
		size_t got = storage_file_read(s->track_file, out + n, HALF - n);
		n += got;
		if(n < HALF && !(s->looping && got + n > 0 && storage_file_seek(s->track_file, 0, true))) close_track(s);
	}
	int32_t gain = s->music_volume * 256 / SOUND_VOLUME_MAX;
	if(s->effects && s->pos < s->end) gain = gain * MUSIC_DUCKED >> 8;
	if(gain != 256)
		for(size_t i = 0; i < n; i++) out[i] = SILENCE + (((int16_t)out[i] - SILENCE) * gain >> 8);
	memset(out + n, SILENCE, HALF - n);
	return n;
}


// The next HALF samples of the effect mixed into out; how many there were.
static size_t mix_effect(Sound* s, uint8_t* out)
{
	if(!s->effects || s->pos >= s->end) return 0;
	size_t n = s->end - s->pos < HALF ? s->end - s->pos : HALF;
	if(!wad_read(s, s->pos, s->mix, n)) {
		s->pos = s->end;
		return 0;
	}
	s->pos += n;
	for(size_t i = 0; i < n; i++) {
		int16_t v = (int16_t)out[i] + (((int16_t)s->mix[i] - SILENCE) * s->volume >> 8);
		out[i] = v < 0 ? 0 : v > 255 ? 255 : v;
	}
	return n;
}


static void fill(Sound* s, int half)
{
	uint8_t* out = s->buffer + half * HALF;
	size_t n = fill_music(s, out);
	n += mix_effect(s, out);
	s->silent_halves = n ? 0 : s->silent_halves + 1;
}


static void start(Sound* s)
{
	if(!s->running) {
		fill(s, 0);
		fill(s, 1);
		if(s->silent_halves < 2) hw_start(s);
	}
}


static void start_effect(Sound* s)
{
	struct
	{
		uint16_t format, rate;
		uint32_t count;
	} header;
	const uint32_t pos = s->effect_pos;
	const uint16_t length = s->effect_length;
	s->volume = s->effect_volume * 256 / 120 * s->effects_volume / SOUND_VOLUME_MAX;
	if(length < sizeof(header) + 2 * PAD || !wad_read(s, pos, &header, sizeof(header)) || header.format != 3 ||
	   header.rate != RATE || header.count > length - sizeof(header))
		return;

	s->pos = pos + sizeof(header) + PAD;
	s->end = pos + sizeof(header) + header.count - PAD;

	if(!s->running)
		start(s);
	else if(!s->track_file) {
		// no music to keep in step: the half the DMA is not reading gets the
		// effect at once (with music it waits for the next half, <= 46 ms)
		const int reading = LL_DMA_GetDataLength(DMA) > HALF ? 0 : 1;
		fill(s, 1 - reading);
	}
}


static int32_t sound_thread(void* context)
{
	Sound* s = context;
	if(!furi_hal_speaker_acquire(1000)) {
		FURI_LOG_W(TAG, "speaker busy, no sound");
		while(!(furi_thread_flags_wait(FlagAll, FuriFlagWaitAny, FuriWaitForever) & FlagExit)) {
		}
		return 0;
	}
	hw_init(s);

	while(true) {
		const uint32_t flags = furi_thread_flags_wait(FlagAll, FuriFlagWaitAny, FuriWaitForever);
		if(flags & FlagExit) break;
		if(flags & FlagMusic) {
			open_track(s);
			start(s);
		}
		if(flags & FlagEffect) start_effect(s);
		if(flags & FlagHalf) fill(s, 0);
		if(flags & FlagFull) fill(s, 1);
		if(s->running && s->silent_halves >= 2) hw_stop(s); // both halves silent: rest
	}

	hw_stop(s);
	hw_deinit();
	close_track(s);
	furi_hal_speaker_release();
	return 0;
}


static bool stealth(void)
{
	return furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode);
}


Sound* sound_start(Storage* storage, SoundConfig config)
{
	const bool on = config.effects || config.music;
	const size_t size = on ? sizeof(Sound) : offsetof(Sound, mix);
	Sound* s = malloc(size);
	memset(s, 0, size);
	s->storage = storage;
	sound_set_config(s, config);
	if(on) {
		s->allocated = true;
		s->thread = furi_thread_alloc_ex("DoomSound", 1024, sound_thread, s);
		furi_thread_set_priority(s->thread, FuriThreadPriorityHigh);
		furi_thread_start(s->thread);
	}
	return s;
}


void sound_stop(Sound* s)
{
	if(s->allocated) {
		furi_thread_flags_set(furi_thread_get_id(s->thread), FlagExit);
		furi_thread_join(s->thread);
		furi_thread_free(s->thread);
	}
	free(s);
}


void sound_set_wad(Sound* s, File* wad, FuriMutex* lock)
{
	if(s->lock) furi_mutex_acquire(s->lock, FuriWaitForever); // not in the middle of a read
	FuriMutex* old = s->lock;
	s->wad = wad;
	s->lock = lock;
	if(old) furi_mutex_release(old);
}


void sound_set_config(Sound* s, SoundConfig config)
{
	s->effects_volume = config.effects > SOUND_VOLUME_MAX ? SOUND_VOLUME_MAX : config.effects;
	s->music_volume = config.music > SOUND_VOLUME_MAX ? SOUND_VOLUME_MAX : config.music;
	s->effects = s->effects_volume && !stealth();
	s->music = s->music_volume && !stealth();
}


SoundConfig sound_config(Sound* s)
{
	return (SoundConfig){.effects = s->effects_volume, .music = s->music_volume};
}


bool sound_muted(Sound* s)
{
	UNUSED(s);
	return stealth();
}


bool sound_needs_restart(Sound* s)
{
	return (s->effects || s->music) && !s->allocated;
}


void sound_play(Sound* s, uint32_t pos, uint16_t length, uint8_t volume)
{
	if(!s->effects || !s->allocated) return;
	s->effect_pos = pos;
	s->effect_length = length;
	s->effect_volume = volume > 120 ? 120 : volume;
	furi_thread_flags_set(furi_thread_get_id(s->thread), FlagEffect);
}


void sound_music(Sound* s, uint8_t song, bool looping)
{
	if(!s->allocated) return;
	s->track = song;
	s->track_loops = looping;
	furi_thread_flags_set(furi_thread_get_id(s->thread), FlagMusic);
}


SoundConfig sound_config_load(Storage* storage)
{
	// "Vem": the volumes in hex; the old "01" on/off format is full or none
	SoundConfig config = {.effects = SOUND_VOLUME_MAX, .music = 12};
	File* f = storage_file_alloc(storage);
	char c[3];
	if(storage_file_open(f, CONFIG, FSAM_READ, FSOM_OPEN_EXISTING)) {
		size_t n = storage_file_read(f, c, 3);
		if(n == 3 && c[0] == 'V') {
			config.effects = c[1] <= '9' ? c[1] - '0' : c[1] - 'a' + 10;
			config.music = c[2] <= '9' ? c[2] - '0' : c[2] - 'a' + 10;
		} else {
			if(n >= 1) config.effects = c[0] != '0' ? SOUND_VOLUME_MAX : 0;
			if(n >= 2) config.music = c[1] != '0' ? SOUND_VOLUME_MAX : 0;
		}
	}
	storage_file_free(f);
	return config;
}


void sound_config_save(Storage* storage, SoundConfig config)
{
	File* f = storage_file_alloc(storage);
	static const char hex[] = "0123456789abcdef";
	const char c[3] = {'V', hex[config.effects & 15], hex[config.music & 15]};
	if(storage_file_open(f, CONFIG, FSAM_WRITE, FSOM_CREATE_ALWAYS)) storage_file_write(f, c, 3);
	storage_file_free(f);
}
