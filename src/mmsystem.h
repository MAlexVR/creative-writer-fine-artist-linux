#ifndef MMSYSTEM_H
#define MMSYSTEM_H

#include <stdbool.h>
#include <stdio.h>

#include "audio.h"
#include "hle.h"
#include "kernel.h"
#include "user.h"

/* Win16 MMSYSTEM.DLL on the HLE dispatcher: timers (timeGetTime, timeSetEvent...), sndPlaySound, waveOut
 * playing through the audio engine, MCI, and the devices a machine like this one does not have (waveIn, MIDI,
 * aux, joystick) answering as a PC without that hardware does.
 *
 * Time is the program's clock (user_now): timeGetTime, the timer events and the audio engine (when it is a null
 * sink) all follow it, and everything that completes (WOM_DONE, timer callbacks) is delivered when the program
 * next reads the clock, polls or waits for messages (user_set_tick): callbacks into the guest run on the CPU
 * through user_call_guest, window callbacks are posted to the message queue.
 *
 * WRITER and ARTIST import 22 and 21 MMSYSTEM ordinals: sndPlaySound, 11 waveOut and 9 waveIn functions, and
 * (WRITER) timeGetTime. Neither imports anything of MCI, MIDI, aux, joystick or timeSetEvent; those are
 * implemented from the API description alone and tagged so in the sources. */

typedef struct mmsystem mmsystem_t;

typedef struct {
    audio_t *audio;     /* the output engine; NULL: no wave devices, waveOutGetNumDevs is 0 */
    FILE *log;          /* logged stubs and failures; NULL: silent */
} mmsystem_config;

/* k may be NULL (sndPlaySound then plays memory images only). */
mmsystem_t *mmsystem_create(const mmsystem_config *cfg, hle_t *h, user_t *u, kernel_t *k);
void mmsystem_destroy(mmsystem_t *m);
bool mmsystem_attach(mmsystem_t *m, loader_proc *p);

/* Calls to the logged stubs (devices that are absent). */
size_t mmsystem_stub_count(const mmsystem_t *m);
const char *mmsystem_stub_name(const mmsystem_t *m, size_t i, unsigned *ordinal);

#endif
