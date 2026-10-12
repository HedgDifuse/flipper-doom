// flipper-doom: Doom's original sounds and music on the Flipper's speaker, see sound.c.
#pragma once

#include <storage/storage.h>

typedef struct Sound Sound;

// volumes 0..15 as Doom's sliders, 0 = off
typedef struct
{
	uint8_t effects;
	uint8_t music;
} SoundConfig;

#define SOUND_VOLUME_MAX 15

// The player (a thread, 1.5 KB of samples) only exists when the effects or
// the music are on at app start: the memory is reserved for the engine right
// after.
Sound* sound_start(Storage* storage, SoundConfig config);
void sound_stop(Sound* sound);

// The open WAD to play the effects from (NULL: none), shared with the engine:
// the firmware lets a file be open only once (a second open waits for the
// first to close), so both use this handle, each seek + read under `lock`.
void sound_set_wad(Sound* sound, File* wad, FuriMutex* lock);

// Turning off mutes at once, turning on takes an app restart if the player
// was not started. Always off in the Flipper's stealth mode.
void sound_set_config(Sound* sound, SoundConfig config);
SoundConfig sound_config(Sound* sound);
bool sound_needs_restart(Sound* sound);
// the Flipper's stealth mode keeps it quiet whatever the volumes
bool sound_muted(Sound* sound);

// Start a DS* lump (DMX 8-bit PCM) at pos in the WAD, replacing the effect
// playing; volume 0..120.
void sound_play(Sound* sound, uint32_t pos, uint16_t length, uint8_t volume);

// Start music track song (musicenum_t: 1 = E1M1 ... 13 = INTROA; 0 stops).
void sound_music(Sound* sound, uint8_t song, bool looping);

// The settings, kept on the SD card.
SoundConfig sound_config_load(Storage* storage);
void sound_config_save(Storage* storage, SoundConfig config);
