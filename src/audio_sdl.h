#ifndef AUDIO_SDL_H
#define AUDIO_SDL_H

#include <stdbool.h>

#include "audio.h"

/* The only module that touches SDL2 audio: opens an output device (signed 16 bit stereo) and feeds it from the
 * engine in audio.h through its audio callback, on SDL's audio thread. The engine is created here with a
 * real-time sink whose lock is SDL_LockAudioDevice, so the program thread can queue and poll safely. */

typedef struct audio_sdl audio_sdl_t;

typedef struct {
    bool dummy;         /* SDL's dummy audio driver: nothing is heard, time still passes at the device rate */
    unsigned rate;      /* default 44100 */
} audio_sdl_config;

/* NULL when SDL has no usable audio device; the caller then uses audio_create with a null sink. */
audio_t *audio_sdl_create(const audio_sdl_config *cfg, audio_sdl_t **dev);
void audio_sdl_destroy(audio_sdl_t *dev, audio_t *engine);
const char *audio_sdl_driver(const audio_sdl_t *dev);

#endif
