#ifndef AUDIO_SWITCH_H
#define AUDIO_SWITCH_H

#include <stdint.h>

#include "pc/audio/audio_api.h"

// the game's audio, with mod audio (miniaudio) mixed in, on libnx's audio renderer
extern struct AudioAPI audio_switch;

// Mixes mod audio into interleaved stereo 32 kHz samples. Miniaudio has no playback device on
// Switch, so its engine runs without one (smlua_audio_utils.c) and is pulled from here.
void smlua_audio_custom_mix(int16_t *samples, uint32_t frames);

#endif
