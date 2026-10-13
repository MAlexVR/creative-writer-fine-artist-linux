/* SDL2 audio device for the engine. See audio_sdl.h. */
#include "audio_sdl.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

struct audio_sdl {
    SDL_AudioDeviceID id;
    audio_t *engine;
    char driver[32];
};

static void pull(void *ctx, Uint8 *stream, int len) {
    audio_sdl_t *d = ctx;
    if (d->engine) audio_mix(d->engine, (int16_t *)stream, (size_t)len / 4);
    else memset(stream, 0, (size_t)len);
}

static void sdl_lock(void *ctx) { SDL_LockAudioDevice(((audio_sdl_t *)ctx)->id); }
static void sdl_unlock(void *ctx) { SDL_UnlockAudioDevice(((audio_sdl_t *)ctx)->id); }
static void sdl_wait(void *ctx, unsigned ms) { (void)ctx; SDL_Delay(ms); }

audio_t *audio_sdl_create(const audio_sdl_config *cfg, audio_sdl_t **out) {
    audio_sdl_t *d;
    SDL_AudioSpec want, got;
    audio_config ac;
    const char *drv;
    *out = NULL;
    if (cfg->dummy) SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return NULL;
    if (!(d = calloc(1, sizeof *d))) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return NULL;
    }
    memset(&want, 0, sizeof want);
    want.freq = cfg->rate ? (int)cfg->rate : 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = pull;
    want.userdata = d;
    d->id = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!d->id || got.format != AUDIO_S16SYS || got.channels != 2) {
        if (d->id) SDL_CloseAudioDevice(d->id);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        free(d);
        return NULL;
    }
    drv = SDL_GetCurrentAudioDriver();
    snprintf(d->driver, sizeof d->driver, "%s", drv ? drv : "?");
    memset(&ac, 0, sizeof ac);
    ac.rate = (unsigned)got.freq;
    ac.real_time = true;
    ac.sink_ctx = d;
    ac.lock = sdl_lock;
    ac.unlock = sdl_unlock;
    ac.wait_ms = sdl_wait;
    SDL_LockAudioDevice(d->id);
    d->engine = audio_create(&ac);
    SDL_UnlockAudioDevice(d->id);
    if (!d->engine) {
        SDL_CloseAudioDevice(d->id);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        free(d);
        return NULL;
    }
    SDL_PauseAudioDevice(d->id, 0);
    *out = d;
    return d->engine;
}

void audio_sdl_destroy(audio_sdl_t *d, audio_t *engine) {
    if (!d) return;
    SDL_CloseAudioDevice(d->id);        /* the callback is gone after this: the engine can be freed */
    d->engine = NULL;
    audio_destroy(engine);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    free(d);
}

const char *audio_sdl_driver(const audio_sdl_t *d) { return d->driver; }
