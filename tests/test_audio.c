/* The audio engine: mixing, resampling, completion tokens, loops, pause/reset, volume and the null sink clock. */
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "fixture.h"

#define RATE 1000

static int16_t *pcm(size_t frames, unsigned ch, int16_t base) {
    int16_t *p = malloc(frames * ch * sizeof *p);
    size_t i;
    for (i = 0; i < frames * ch; i++) p[i] = (int16_t)(base + (int)i);
    return p;
}

static audio_t *mk(void) { return audio_create(&(audio_config){.rate = RATE}); }

static void test_mix_and_done(void) {
    audio_t *a = mk();
    audio_stream *s = audio_stream_open(a, RATE, 1);
    int16_t out[40], *p = pcm(10, 1, 100);
    uint32_t tok = 0;
    size_t i;
    printf("test_audio_mix_and_done\n");
    CHECK(a && s);
    CHECK(audio_stream_queue(s, p, 10, 0, 0, 77));
    CHECK(audio_stream_busy(s));
    audio_mix(a, out, 6);
    for (i = 0; i < 6; i++) { CHECK_EQ(out[2 * i], 100 + i); CHECK_EQ(out[2 * i + 1], 100 + i); }   /* mono to both sides */
    CHECK(!audio_stream_pop_done(s, &tok));
    CHECK_EQ(audio_stream_position(s), 6);
    audio_mix(a, out, 10);                                         /* 4 frames left, then silence */
    CHECK_EQ(out[2 * 3], 109);
    CHECK_EQ(out[2 * 4], 0);
    CHECK_EQ(out[2 * 9 + 1], 0);
    CHECK(audio_stream_pop_done(s, &tok));
    CHECK_EQ(tok, 77);
    CHECK(!audio_stream_pop_done(s, &tok));
    CHECK(!audio_stream_busy(s));
    CHECK_EQ(audio_stream_position(s), 10);
    CHECK_EQ(audio_frames_mixed(a), 16);
    audio_stream_close(s);
    audio_destroy(a);
}

static void test_stereo_and_order(void) {
    audio_t *a = mk();
    audio_stream *s = audio_stream_open(a, RATE, 2);
    int16_t out[16];
    uint32_t t;
    printf("test_audio_stereo_and_order\n");
    audio_stream_queue(s, pcm(2, 2, 10), 2, 0, 0, 1);              /* frames (10,11) (12,13) */
    audio_stream_queue(s, pcm(2, 2, 50), 2, 0, 0, 2);
    audio_mix(a, out, 3);
    CHECK_EQ(out[0], 10); CHECK_EQ(out[1], 11); CHECK_EQ(out[2], 12); CHECK_EQ(out[3], 13);
    CHECK_EQ(out[4], 50); CHECK_EQ(out[5], 51);                    /* the second buffer follows without a gap */
    CHECK(audio_stream_pop_done(s, &t)); CHECK_EQ(t, 1);
    CHECK(!audio_stream_pop_done(s, &t));
    audio_mix(a, out, 1);
    CHECK(audio_stream_pop_done(s, &t)); CHECK_EQ(t, 2);
    audio_destroy(a);
}

static void test_resample(void) {
    audio_t *a = mk();
    audio_stream *half = audio_stream_open(a, RATE / 2, 1), *dbl = audio_stream_open(a, RATE * 2, 1);
    int16_t out[2 * 400], *p = malloc(100 * sizeof *p);
    uint32_t t;
    size_t i;
    printf("test_audio_resample\n");
    for (i = 0; i < 100; i++) p[i] = (int16_t)(i * 100);
    audio_stream_queue(half, p, 100, 0, 0, 5);
    audio_mix(a, out, 199);
    CHECK(!audio_stream_pop_done(half, &t));
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[2 * 1], 50);                                      /* linear interpolation between neighbours */
    CHECK_EQ(out[2 * 2], 100);
    audio_mix(a, out, 1);
    CHECK(audio_stream_pop_done(half, &t));                        /* 100 source frames last 200 output frames */
    p = malloc(100 * sizeof *p);
    for (i = 0; i < 100; i++) p[i] = 7;
    audio_stream_queue(dbl, p, 100, 0, 0, 6);
    audio_mix(a, out, 49);
    CHECK(!audio_stream_pop_done(dbl, &t));
    audio_mix(a, out, 1);
    CHECK(audio_stream_pop_done(dbl, &t));                         /* twice the rate: half the time */
    CHECK_EQ(t, 6);
    audio_destroy(a);
}

static void test_pause_reset(void) {
    audio_t *a = mk();
    audio_stream *s = audio_stream_open(a, RATE, 1);
    int16_t out[64];
    uint32_t t;
    printf("test_audio_pause_reset\n");
    audio_stream_queue(s, pcm(10, 1, 1), 10, 0, 0, 1);
    audio_stream_queue(s, pcm(10, 1, 1), 10, 0, 0, 2);
    audio_mix(a, out, 4);
    audio_stream_pause(s, true);
    CHECK(audio_stream_paused(s));
    audio_mix(a, out, 30);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(audio_stream_position(s), 4);
    CHECK(!audio_stream_pop_done(s, &t));
    audio_stream_pause(s, false);
    audio_mix(a, out, 2);
    CHECK_EQ(audio_stream_position(s), 6);
    audio_stream_reset(s);                                         /* both buffers complete, in order, at once */
    CHECK(audio_stream_pop_done(s, &t)); CHECK_EQ(t, 1);
    CHECK(audio_stream_pop_done(s, &t)); CHECK_EQ(t, 2);
    CHECK(!audio_stream_pop_done(s, &t));
    CHECK(!audio_stream_busy(s));
    CHECK_EQ(audio_stream_position(s), 0);
    audio_mix(a, out, 5);
    CHECK_EQ(out[0], 0);
    audio_destroy(a);
}

static void test_loops(void) {
    audio_t *a = mk();
    audio_stream *s = audio_stream_open(a, RATE, 1);
    int16_t out[256];
    uint32_t t;
    printf("test_audio_loops\n");
    audio_stream_queue(s, pcm(10, 1, 1), 10, AUDIO_LOOP_BEGIN | AUDIO_LOOP_END, 3, 9);   /* one buffer, three passes */
    audio_mix(a, out, 10);
    CHECK_EQ(out[2 * 9], 10);
    audio_mix(a, out, 1);
    CHECK_EQ(out[0], 1);                                           /* wrapped around */
    audio_mix(a, out, 18);
    CHECK(!audio_stream_pop_done(s, &t));
    audio_mix(a, out, 1);
    CHECK(audio_stream_pop_done(s, &t));                           /* 30 frames in all */
    CHECK_EQ(t, 9);
    CHECK(!audio_stream_pop_done(s, &t));
    /* two buffers in the loop, infinite until broken */
    audio_stream_queue(s, pcm(4, 1, 1), 4, AUDIO_LOOP_BEGIN, 0xFFFFFFFFu, 1);
    audio_stream_queue(s, pcm(4, 1, 1), 4, AUDIO_LOOP_END, 0, 2);
    audio_stream_queue(s, pcm(4, 1, 1), 4, 0, 0, 3);
    audio_mix(a, out, 80);
    CHECK(!audio_stream_pop_done(s, &t));
    CHECK(audio_stream_busy(s));
    audio_stream_break_loop(s);                                    /* the pass in progress finishes */
    audio_mix(a, out, 3);                                          /* pass 11 ended at frame 88; we are at 83 */
    audio_mix(a, out, 5);
    CHECK(audio_stream_pop_done(s, &t)); CHECK_EQ(t, 1);
    CHECK(audio_stream_pop_done(s, &t)); CHECK_EQ(t, 2);
    CHECK(!audio_stream_pop_done(s, &t));
    audio_mix(a, out, 4);
    CHECK(audio_stream_pop_done(s, &t)); CHECK_EQ(t, 3);
    audio_destroy(a);
}

static void test_volume_and_sum(void) {
    audio_t *a = mk();
    audio_stream *s = audio_stream_open(a, RATE, 1), *s2 = audio_stream_open(a, RATE, 1);
    int16_t out[8], *p = malloc(4 * sizeof *p), *q = malloc(4 * sizeof *q);
    size_t i;
    printf("test_audio_volume\n");
    for (i = 0; i < 4; i++) { p[i] = 20000; q[i] = 20000; }
    audio_stream_set_volume(s, 0xFFFF, 0x8000);
    audio_stream_queue(s, p, 4, 0, 0, 1);
    audio_mix(a, out, 2);
    CHECK_EQ(out[0], 20000);
    CHECK(out[1] > 9990 && out[1] < 10010);                        /* right at half */
    audio_stream_queue(s2, q, 4, 0, 0, 1);
    audio_mix(a, out, 2);                                          /* two voices add and clip */
    CHECK_EQ(out[0], 32767);
    audio_mix(a, out, 4);                                          /* let the rest of both buffers play out */
    audio_set_volume(a, 0x8000, 0);
    {
        uint16_t l, r;
        audio_get_volume(a, &l, &r);
        CHECK_EQ(l, 0x8000); CHECK_EQ(r, 0);
    }
    audio_stream_queue(s, pcm(4, 1, 1000), 4, 0, 0, 1);
    audio_mix(a, out, 1);
    CHECK(out[0] > 490 && out[0] < 510);                           /* master volume applies on top */
    CHECK_EQ(out[1], 0);
    audio_destroy(a);
}

static void test_null_sink_clock(void) {
    audio_t *a = audio_create(&(audio_config){.rate = 1500});
    audio_stream *s = audio_stream_open(a, 1500, 1);
    uint32_t t;
    printf("test_audio_null_sink\n");
    audio_stream_queue(s, pcm(3000, 1, 1), 3000, 0, 0, 4);
    audio_advance(a, 5000);                                        /* the first call only sets the origin */
    CHECK_EQ(audio_frames_mixed(a), 0);
    audio_advance(a, 5001);
    CHECK_EQ(audio_frames_mixed(a), 1);                            /* 1.5 frames per ms: the half carries over */
    audio_advance(a, 5002);
    CHECK_EQ(audio_frames_mixed(a), 3);
    audio_advance(a, 5002);
    CHECK_EQ(audio_frames_mixed(a), 3);
    audio_advance(a, 5000);                                        /* a clock that goes back is ignored */
    CHECK_EQ(audio_frames_mixed(a), 3);
    audio_advance(a, 5000 + 1999);
    CHECK(!audio_stream_pop_done(s, &t));
    audio_advance(a, 5000 + 2000);
    CHECK(audio_stream_pop_done(s, &t));                           /* 3000 frames = 2000 ms */
    audio_stream_queue(s, pcm(1500, 1, 1), 1500, 0, 0, 5);
    audio_stream_wait(s);                                          /* offline: consumed at once */
    CHECK(audio_stream_pop_done(s, &t)); CHECK_EQ(t, 5);
    audio_destroy(a);
}

static int locks, unlocks;
static void lk(void *c) { (void)c; locks++; }
static void ulk(void *c) { (void)c; unlocks++; }
static unsigned waited;
static audio_t *g_dev;
static void wt(void *c, unsigned ms) { (void)c; waited += ms; audio_mix(g_dev, NULL, ms * RATE / 1000 + 1); }   /* the device runs meanwhile */

static void test_real_time_sink(void) {
    audio_t *a = audio_create(&(audio_config){.rate = RATE, .real_time = true, .lock = lk, .unlock = ulk, .wait_ms = wt});
    audio_stream *s = audio_stream_open(a, RATE, 1);
    int16_t out[20];
    uint32_t t;
    printf("test_audio_real_time_sink\n");
    g_dev = a;
    CHECK(locks > 0 && locks == unlocks);
    audio_stream_queue(s, pcm(10, 1, 1), 10, 0, 0, 1);
    audio_advance(a, 0);
    audio_advance(a, 100);                                         /* a device owns the clock: 100 ms is within its slack */
    CHECK_EQ(audio_frames_mixed(a), 0);
    CHECK(audio_stream_busy(s));
    {   /* the "device thread" pulls while the main thread waits */
        int before = locks;
        audio_mix(a, out, 10);
        CHECK(audio_stream_pop_done(s, &t));
        CHECK(locks > before && locks == unlocks);
    }
    audio_stream_queue(s, pcm(10, 1, 1), 10, 0, 0, 2);
    waited = 0;
    audio_mix(a, out, 5);
    audio_stream_wait(s);                                          /* real-time sink: sleeps through wait_ms until the device is done */
    CHECK(waited > 0);
    CHECK(!audio_stream_busy(s));
    audio_destroy(a);
}

/* A real sink that stops pulling (a suspended PipeWire/ALSA stream, a device that runs slow): the program must not
 * wait for a WOM_DONE that never comes. audio_advance, which the program's clock reads drive, finishes the audio by
 * the program clock once the device is more than stall_ms (default 250) behind it. */
static void test_stalled_device(void) {
    audio_t *a = audio_create(&(audio_config){.rate = RATE, .real_time = true});
    audio_stream *s = audio_stream_open(a, RATE, 1);
    uint32_t t;
    printf("test_audio_stalled_device\n");
    audio_stream_queue(s, pcm(1000, 1, 1), 1000, 0, 0, 9);        /* one second of sound */
    audio_advance(a, 0);                                           /* sets the origin */
    audio_advance(a, 200);
    CHECK_EQ(audio_frames_mixed(a), 0);                            /* within the slack the device may be late */
    audio_advance(a, 1000);
    CHECK_EQ(audio_frames_mixed(a), 750);                          /* 1000 ms of clock - 250 ms slack */
    CHECK(audio_stream_busy(s) && !audio_stream_pop_done(s, &t));
    audio_advance(a, 1200);
    CHECK(audio_stream_busy(s));
    audio_advance(a, 1260);                                        /* buffer duration + slack: done by the program clock */
    CHECK(!audio_stream_busy(s));
    CHECK(audio_stream_pop_done(s, &t));
    CHECK_EQ(t, 9);
    audio_stream_close(s);
    audio_destroy(a);
}

/* A healthy device that pulls in step with the clock is never skipped ahead, and a slow one is held to
 * the program clock minus the slack. */
static void test_device_in_step_and_slow(void) {
    audio_t *a = audio_create(&(audio_config){.rate = RATE, .real_time = true, .stall_ms = 100});
    audio_stream *s = audio_stream_open(a, RATE, 1);
    uint32_t t;
    int ms;
    printf("test_audio_device_in_step_and_slow\n");
    audio_stream_queue(s, pcm(2000, 1, 1), 2000, 0, 0, 1);
    audio_advance(a, 0);
    for (ms = 10; ms <= 500; ms += 10) {                           /* the device pulls 10 frames per 10 ms */
        audio_mix(a, NULL, 10);
        audio_advance(a, (uint32_t)ms);
    }
    CHECK_EQ(audio_frames_mixed(a), 500);                          /* nothing extra was consumed */
    CHECK_EQ(audio_stream_position(s), 500);
    for (ms = 510; ms <= 3000; ms += 10) {                         /* now the device pulls only 5 frames per 10 ms */
        audio_mix(a, NULL, 5);
        audio_advance(a, (uint32_t)ms);
    }
    CHECK(!audio_stream_busy(s));                                  /* 2000 frames are done well before a half-speed device would be */
    CHECK(audio_stream_pop_done(s, &t));
    audio_stream_close(s);
    audio_destroy(a);
}

/* sndPlaySound without SND_ASYNC and waveOut resets wait through audio_stream_wait: with a stalled device it ends
 * at the buffer's duration plus the slack on the sink's clock (now_ms), never forever. */
static uint32_t g_now;
static uint32_t fake_now(void *c) { (void)c; return g_now; }
static void fake_wait(void *c, unsigned ms) { (void)c; g_now += ms; }   /* time passes, the device never pulls */

static void test_wait_with_stalled_device(void) {
    audio_t *a = audio_create(&(audio_config){.rate = RATE, .real_time = true, .wait_ms = fake_wait, .now_ms = fake_now});
    audio_stream *s = audio_stream_open(a, RATE, 1);
    uint32_t t;
    printf("test_audio_wait_with_stalled_device\n");
    g_now = 5000;
    audio_stream_queue(s, pcm(600, 1, 1), 600, 0, 0, 3);
    audio_advance(a, g_now);
    audio_stream_wait(s);
    CHECK(!audio_stream_busy(s));
    CHECK(g_now >= 5000 + 600 && g_now <= 5000 + 600 + 250 + 20);  /* duration + slack (+ one 5 ms step) */
    CHECK(audio_stream_pop_done(s, &t));
    CHECK_EQ(t, 3);
    audio_stream_close(s);
    audio_destroy(a);
}

int main(void) {
    test_mix_and_done();
    test_stereo_and_order();
    test_resample();
    test_pause_reset();
    test_loops();
    test_volume_and_sum();
    test_null_sink_clock();
    test_real_time_sink();
    test_stalled_device();
    test_device_in_step_and_slow();
    test_wait_with_stalled_device();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
