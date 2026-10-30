// Switch audio output: the game's audio with mod audio mixed in (see audio_switch.h), played
// through libnx's audio renderer driver (audrv) from the game thread.
//
// SDL's Switch audio driver waits for the renderer without a timeout. When the renderer stopped
// consuming audio, its thread blocked forever: no sound, and quitting hung in SDL_CloseAudioDevice.
// Here nothing waits: buffers are queued from play(), progress is polled, and a renderer that
// makes no progress is logged and restarted.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <switch.h>

#include "audio_switch.h"

#define AUDIO_SAMPLE_RATE 32000
#define AUDIO_CHANNELS    2
#define FRAME_BYTES       (AUDIO_CHANNELS * sizeof(int16_t))

// a game frame queues at most 2 * SAMPLES_HIGH (1088) frames per play() call
#define WAVEBUF_COUNT     8
#define WAVEBUF_FRAMES    2048
#define POOL_SIZE         ((WAVEBUF_COUNT * WAVEBUF_FRAMES * FRAME_BYTES + 0xfff) & ~0xfff)

// same limits as audio_sdl.c
#define DESIRED_BUFFERED  1100
#define MAX_BUFFERED      6000

// no played samples for this long while audio is queued counts as a stalled renderer
#define STALL_MS          500
#define RESTART_DELAY_MS  2000

static const AudioRendererConfig sRendererConfig = {
    .output_rate     = AudioRendererOutputRate_48kHz,
    .num_voices      = 2,
    .num_effects     = 0,
    .num_sinks       = 1,
    .num_mix_objs    = 1,
    .num_mix_buffers = 2,
};

static AudioDriver sDriver;
static AudioDriverWaveBuf sWaveBufs[WAVEBUF_COUNT];
static int16_t *sPool = NULL;
static bool sRendererInited = false;
static bool sDriverInited = false;
static bool sReady = false;

static u32 sSubmittedFrames = 0;   // compared with the voice's played sample count
static u32 sLastPlayedFrames = 0;
static u64 sLastProgressTick = 0;
static u64 sLastPollTick = 0;
static u64 sLastRestartTick = 0;
static u32 sRestarts = 0;

static u64 ms_to_ticks(u64 ms) {
    return armNsToTicks(ms * 1000000ULL);
}

static void audio_switch_close(void) {
    sReady = false;
    if (sDriverInited) {
        audrvVoiceStop(&sDriver, 0);
        audrvClose(&sDriver);
        sDriverInited = false;
    }
    if (sRendererInited) {
        audrenExit();
        sRendererInited = false;
    }
}

static bool audio_switch_open(void) {
    static const u8 sinkChannels[] = { 0, 1 };

    Result rc = audrenInitialize(&sRendererConfig);
    if (R_FAILED(rc)) {
        printf("[switch] audio: audrenInitialize failed: 0x%x\n", rc);
        return false;
    }
    sRendererInited = true;

    rc = audrvCreate(&sDriver, &sRendererConfig, AUDIO_CHANNELS);
    if (R_FAILED(rc)) {
        printf("[switch] audio: audrvCreate failed: 0x%x\n", rc);
        audio_switch_close();
        return false;
    }
    sDriverInited = true;

    int pool = audrvMemPoolAdd(&sDriver, sPool, POOL_SIZE);
    if (pool < 0 || !audrvMemPoolAttach(&sDriver, pool) ||
        audrvDeviceSinkAdd(&sDriver, AUDREN_DEFAULT_DEVICE_NAME, AUDIO_CHANNELS, sinkChannels) < 0) {
        printf("[switch] audio: could not set up the audio driver\n");
        audio_switch_close();
        return false;
    }

    rc = audrvUpdate(&sDriver);
    if (R_SUCCEEDED(rc)) { rc = audrenStartAudioRenderer(); }
    if (R_FAILED(rc)) {
        printf("[switch] audio: could not start the audio renderer: 0x%x\n", rc);
        audio_switch_close();
        return false;
    }

    audrvVoiceInit(&sDriver, 0, AUDIO_CHANNELS, PcmFormat_Int16, AUDIO_SAMPLE_RATE);
    audrvVoiceSetDestinationMix(&sDriver, 0, AUDREN_FINAL_MIX_ID);
    audrvVoiceSetMixFactor(&sDriver, 0, 1.0f, 0, 0);
    audrvVoiceSetMixFactor(&sDriver, 0, 0.0f, 0, 1);
    audrvVoiceSetMixFactor(&sDriver, 0, 0.0f, 1, 0);
    audrvVoiceSetMixFactor(&sDriver, 0, 1.0f, 1, 1);
    audrvVoiceStart(&sDriver, 0);

    memset(sWaveBufs, 0, sizeof(sWaveBufs));
    for (int i = 0; i < WAVEBUF_COUNT; i++) {
        sWaveBufs[i].data_raw = sPool;
        sWaveBufs[i].size = POOL_SIZE;
        sWaveBufs[i].start_sample_offset = i * WAVEBUF_FRAMES;
        sWaveBufs[i].end_sample_offset = i * WAVEBUF_FRAMES;
    }

    sSubmittedFrames = 0;
    sLastPlayedFrames = 0;
    sLastProgressTick = sLastPollTick = armGetSystemTick();
    sReady = true;
    return true;
}

// frames queued on the voice and not yet played
static int audio_switch_pending_frames(void) {
    int queued = 0;
    for (int i = 0; i < WAVEBUF_COUNT; i++) {
        AudioDriverWaveBufState state = sWaveBufs[i].state;
        if (state != AudioDriverWaveBufState_Free && state != AudioDriverWaveBufState_Done) {
            queued += sWaveBufs[i].end_sample_offset - sWaveBufs[i].start_sample_offset;
        }
    }
    if (queued == 0) { return 0; }

    // the played count runs across wave buffers; the queued total bounds it if that ever differs
    s32 pending = (s32)(sSubmittedFrames - audrvVoiceGetPlayedSampleCount(&sDriver, 0));
    if (pending < 0) { return 0; }
    return pending < queued ? pending : queued;
}

static void audio_switch_restart(int pending) {
    u32 rendererState = ~0u;
    audrenGetState(&rendererState);
    bool frameSignaled = R_SUCCEEDED(eventWait(audrenGetFrameEvent(), 0));
    printf("[switch] audio: renderer made no progress for %d ms with %d frames queued "
           "(renderer state %u, frame event %s, voice drops %u), restarting it\n",
           STALL_MS, pending, rendererState, frameSignaled ? "signaled" : "not signaled",
           audrvVoiceGetVoiceDropsCount(&sDriver, 0));
    audio_switch_close();
    sRestarts++;
    sLastRestartTick = armGetSystemTick();
    if (audio_switch_open()) {
        printf("[switch] audio: renderer restarted (%u restarts)\n", sRestarts);
    }
}

// sends queued changes to the renderer and watches for a stalled renderer
static void audio_switch_poll(void) {
    u64 now = armGetSystemTick();
    if (!sReady) {
        // retry a failed restart now and then
        if (sRendererInited || now - sLastRestartTick < ms_to_ticks(RESTART_DELAY_MS)) { return; }
        sLastRestartTick = now;
        audio_switch_open();
        return;
    }

    audrvUpdate(&sDriver);

    // the game wasn't running (e.g. suspended by the HOME menu), so there is nothing to judge
    if (now - sLastPollTick > ms_to_ticks(STALL_MS / 2)) { sLastProgressTick = now; }
    sLastPollTick = now;

    u32 played = audrvVoiceGetPlayedSampleCount(&sDriver, 0);
    int pending = audio_switch_pending_frames();
    if (played != sLastPlayedFrames || pending == 0) {
        sLastPlayedFrames = played;
        sLastProgressTick = now;
    } else if (now - sLastProgressTick > ms_to_ticks(STALL_MS)
               && now - sLastRestartTick > ms_to_ticks(RESTART_DELAY_MS)) {
        audio_switch_restart(pending);
    }
}

static bool audio_switch_init(void) {
    sPool = memalign(AUDREN_MEMPOOL_ALIGNMENT, POOL_SIZE);
    if (sPool == NULL) {
        printf("[switch] audio: out of memory\n");
        return false;
    }
    memset(sPool, 0, POOL_SIZE);
    if (!audio_switch_open()) {
        free(sPool);
        sPool = NULL;
        return false;
    }
    return true;
}

// called before play() every game frame
static int audio_switch_buffered(void) {
    audio_switch_poll();
    return sReady ? audio_switch_pending_frames() : 0;
}

static int audio_switch_get_desired_buffered(void) {
    return DESIRED_BUFFERED;
}

static void audio_switch_play(const uint8_t *buf, size_t len) {
    if (!sReady || audio_switch_pending_frames() >= MAX_BUFFERED) { return; }

    AudioDriverWaveBuf *wavebuf = NULL;
    for (int i = 0; i < WAVEBUF_COUNT; i++) {
        if (sWaveBufs[i].state == AudioDriverWaveBufState_Free || sWaveBufs[i].state == AudioDriverWaveBufState_Done) {
            wavebuf = &sWaveBufs[i];
            break;
        }
    }
    if (wavebuf == NULL) { return; }

    u32 frames = len / FRAME_BYTES;
    if (frames > WAVEBUF_FRAMES) { frames = WAVEBUF_FRAMES; }
    int16_t *samples = sPool + wavebuf->start_sample_offset * AUDIO_CHANNELS;
    memcpy(samples, buf, frames * FRAME_BYTES);
    smlua_audio_custom_mix(samples, frames);
    armDCacheFlush(samples, frames * FRAME_BYTES);

    wavebuf->end_sample_offset = wavebuf->start_sample_offset + frames;
    if (audrvVoiceAddWaveBuf(&sDriver, 0, wavebuf)) {
        sSubmittedFrames += frames;
    }
    audrvUpdate(&sDriver);
}

static void audio_switch_shutdown(void) {
    audio_switch_close();
    free(sPool);
    sPool = NULL;
}

struct AudioAPI audio_switch = {
    audio_switch_init,
    audio_switch_buffered,
    audio_switch_get_desired_buffered,
    audio_switch_play,
    audio_switch_shutdown,
};
