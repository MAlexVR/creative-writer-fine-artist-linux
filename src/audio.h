#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The audio engine behind MMSYSTEM: streams of signed 16 bit PCM that are mixed to stereo at one output rate.
 * It knows nothing about SDL or the guest. A sink pulls samples with audio_mix (a real device, on its own
 * thread, through audio_sdl.c) or nobody does and audio_advance consumes them as the program clock moves
 * (the null sink: tests and machines without a sound device), which makes completion times deterministic.
 *
 * A stream plays a queue of buffers. A buffer that has played to its end puts its token on the stream's done
 * list (audio_stream_pop_done), which is how waveOutWrite learns of WOM_DONE; looped buffers (WHDR_BEGINLOOP
 * .. WHDR_ENDLOOP) play dwLoops times and report done when the loop is left. */

typedef struct audio audio_t;
typedef struct audio_stream audio_stream;

typedef struct {
    unsigned rate;                         /* output rate in Hz, default 44100 */
    bool real_time;                        /* a device calls audio_mix by itself; audio_advance does nothing */
    void *sink_ctx;
    void (*lock)(void *ctx);               /* NULL: no other thread touches the engine */
    void (*unlock)(void *ctx);
    void (*wait_ms)(void *ctx, unsigned ms); /* real_time sinks: sleep, for audio_stream_wait */
    uint32_t (*now_ms)(void *ctx);         /* real_time sinks: clock for audio_stream_wait; NULL: the host's monotonic clock */
    unsigned stall_ms;                     /* real_time sinks: how far behind the program clock the device may fall (default 250) */
} audio_config;

audio_t *audio_create(const audio_config *cfg);
void audio_destroy(audio_t *a);

/* Mixes frames stereo frames (2 samples each) and moves every stream on; out may be NULL to just consume. */
void audio_mix(audio_t *a, int16_t *out, size_t frames);
/* Null sink clock: consumes the time elapsed since the last call (the first call only sets the origin).
 * Real-time sinks: a watchdog against a device that stops pulling (suspended stream) or runs slow: once it is more
 * than stall_ms behind the program clock the engine consumes the difference itself, so a program that waits for a
 * buffer to finish (WOM_DONE, sndPlaySound) is released at the buffer's duration plus stall_ms and never blocks. */
void audio_advance(audio_t *a, uint32_t now_ms);
uint64_t audio_frames_mixed(const audio_t *a);

/* Master volume (0..0xFFFF per channel, 0xFFFF is unity). */
void audio_set_volume(audio_t *a, uint16_t left, uint16_t right);
void audio_get_volume(audio_t *a, uint16_t *left, uint16_t *right);

audio_stream *audio_stream_open(audio_t *a, unsigned rate, unsigned channels);
void audio_stream_close(audio_stream *s);       /* drops what is queued, no tokens are reported */

#define AUDIO_LOOP_BEGIN 1u
#define AUDIO_LOOP_END 2u
/* Queues pcm (malloc'ed, owned by the stream from now on, also on failure) of frames frames. loops counts
 * the passes of a loop, from the BEGIN buffer; 0xFFFFFFFF never ends until audio_stream_break_loop. */
bool audio_stream_queue(audio_stream *s, int16_t *pcm, size_t frames, unsigned flags, uint32_t loops, uint32_t token);
bool audio_stream_pop_done(audio_stream *s, uint32_t *token);
void audio_stream_pause(audio_stream *s, bool paused);
bool audio_stream_paused(audio_stream *s);
void audio_stream_reset(audio_stream *s);       /* everything queued is finished now: tokens go to the done list */
void audio_stream_break_loop(audio_stream *s);
uint64_t audio_stream_position(audio_stream *s); /* source frames played since open or the last reset */
bool audio_stream_busy(audio_stream *s);        /* buffers still queued */
void audio_stream_set_volume(audio_stream *s, uint16_t left, uint16_t right);
/* Blocks until the stream has nothing queued: offline sinks consume the time at once, real-time ones sleep. */
void audio_stream_wait(audio_stream *s);

#endif
