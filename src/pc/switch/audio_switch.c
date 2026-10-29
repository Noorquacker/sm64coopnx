// Switch audio output: the game's SDL audio with mod audio mixed in (see audio_switch.h).

#include <stdlib.h>
#include <string.h>

#include "pc/audio/audio_sdl.h"
#include "audio_switch.h"

static int16_t *sMixBuffer = NULL;
static size_t sMixBufferSize = 0;

static bool audio_switch_init(void) {
    return audio_sdl.init();
}

static int audio_switch_buffered(void) {
    return audio_sdl.buffered();
}

static int audio_switch_get_desired_buffered(void) {
    return audio_sdl.get_desired_buffered();
}

static void audio_switch_play(const uint8_t *buf, size_t len) {
    if (len > sMixBufferSize) {
        int16_t *grown = realloc(sMixBuffer, len);
        if (!grown) {
            audio_sdl.play(buf, len);
            return;
        }
        sMixBuffer = grown;
        sMixBufferSize = len;
    }
    memcpy(sMixBuffer, buf, len);
    smlua_audio_custom_mix(sMixBuffer, len / (2 * sizeof(int16_t)));
    audio_sdl.play((const uint8_t *) sMixBuffer, len);
}

static void audio_switch_shutdown(void) {
    audio_sdl.shutdown();
    free(sMixBuffer);
    sMixBuffer = NULL;
    sMixBufferSize = 0;
}

struct AudioAPI audio_switch = {
    audio_switch_init,
    audio_switch_buffered,
    audio_switch_get_desired_buffered,
    audio_switch_play,
    audio_switch_shutdown,
};
