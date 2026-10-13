#define _POSIX_C_SOURCE 200809L
/* The SDL audio backend on SDL's dummy audio driver: the device thread pulls the engine at the device rate, so
 * a buffer completes in real time, and it is the only code that touches SDL audio. */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio_sdl.h"
#include "fixture.h"

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void test_dummy_device(void) {
    audio_sdl_t *dev = NULL;
    audio_t *a = audio_sdl_create(&(audio_sdl_config){.dummy = true}, &dev);
    audio_stream *s;
    int16_t *p;
    uint32_t t = 0;
    long t0, spent;
    size_t i;
    printf("test_audio_sdl_dummy\n");
    CHECK(a != NULL);
    if (!a) return;
    CHECK(!strcmp(audio_sdl_driver(dev), "dummy"));
    s = audio_stream_open(a, 22050, 1);
    p = malloc(4410 * sizeof *p);                                   /* 0.2 s */
    for (i = 0; i < 4410; i++) p[i] = 1000;
    t0 = now_ms();
    CHECK(audio_stream_queue(s, p, 4410, 0, 0, 33));
    audio_stream_wait(s);                                           /* sleeps while the device thread consumes */
    spent = now_ms() - t0;
    CHECK(spent >= 120 && spent < 2000);                            /* real time, not instant */
    CHECK(audio_stream_pop_done(s, &t));
    CHECK_EQ(t, 33);
    CHECK(audio_frames_mixed(a) >= 8820);                           /* 0.2 s at 44100 Hz */
    audio_stream_close(s);
    audio_sdl_destroy(dev, a);
}

int main(void) {
    test_dummy_device();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
