/* MMSYSTEM waveOut, waveIn and sndPlaySound over the audio engine's null sink, with the program clock driving
 * time: guest-code frames (Pascal arguments, far pointers to WAVEFORMAT / WAVEHDR / MMTIME), DX:AX results,
 * WOM_DONE delivery by window message and by function callback, loops, reset, ADPCM through the mapper. */
#define _POSIX_C_SOURCE 200809L
#include "hostdir.h"
#include "mmfixture.h"

#define WAVE_MAPPER 0xFFFF
#define WHDR_DONE 1u
#define WHDR_PREPARED 2u
#define WHDR_BEGINLOOP 4u
#define WHDR_ENDLOOP 8u
#define WHDR_INQUEUE 0x10u
#define T(ms) do { uclock = 1000 + (ms); MCALL(607); } while (0)

static void mm_wr16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void mm_wr32(uint8_t *p, uint32_t v) { mm_wr16(p, v & 0xFFFF); mm_wr16(p + 2, v >> 16); }

static uint16_t g_fmt, g_hdr, g_data, g_msg;   /* guest scratch segments */

static void put_pcm_fmt(uint16_t sel, unsigned ch, uint32_t rate, unsigned bits) {
    uwr16(sel, 0, 1); uwr16(sel, 2, (uint16_t)ch); uwr32(sel, 4, rate);
    uwr32(sel, 8, rate * ch * bits / 8); uwr16(sel, 12, (uint16_t)(ch * bits / 8)); uwr16(sel, 14, (uint16_t)bits);
}

static void put_adpcm_fmt(uint16_t sel) {
    static const int16_t co[7][2] = {{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}};
    unsigned i;
    uwr16(sel, 0, 2); uwr16(sel, 2, 1); uwr32(sel, 4, 11025); uwr32(sel, 8, 11025 * 16 / 20); uwr16(sel, 12, 16);
    uwr16(sel, 14, 4); uwr16(sel, 16, 32); uwr16(sel, 18, 20); uwr16(sel, 20, 7);
    for (i = 0; i < 7; i++) { uwr16(sel, 22 + 4 * i, (uint16_t)co[i][0]); uwr16(sel, 24 + 4 * i, (uint16_t)co[i][1]); }
}

/* header i at g_hdr:32*i, data of len bytes at g_data:off */
static void put_hdr(unsigned i, uint32_t off, uint32_t len, uint32_t flags, uint32_t loops) {
    unsigned o = 32 * i;
    uwr16(g_hdr, o, (uint16_t)off); uwr16(g_hdr, o + 2, g_data); uwr32(g_hdr, o + 4, len); uwr32(g_hdr, o + 8, 0);
    uwr32(g_hdr, o + 12, 0xABCD0000u + i); uwr32(g_hdr, o + 16, flags); uwr32(g_hdr, o + 20, loops);
}
static uint32_t hdr_flags(unsigned i) { return urd32(g_hdr, 32 * i + 16); }

static void boot(bool audio) {
    mmboot(audio);
    g_fmt = useg(0x100);
    g_hdr = useg(0x200);
    g_data = useg(0x10000);
    g_msg = useg(0x40);
}

/* PeekMessage(PM_REMOVE) of any window: 1 when a message came, then it is in g_msg. */
static bool peek(void) {
    UCALL(109, FARP(g_msg, 0), 0, 0, 0, 1);
    return fx_ax(&uf) != 0;
}

static uint16_t open_wave(unsigned id, uint32_t cb, uint32_t inst, uint32_t flags, uint16_t *err) {
    uint16_t hs = useg(0x10);
    MCALL(404, FARP(hs, 0), (uint16_t)id, FARP(g_fmt, 0), LONG(cb), LONG(inst), LONG(flags));
    *err = fx_ax(&uf);
    return urd16(hs, 0);
}

static uint16_t mk_window(void) {
    u_register("MMW", u_far(U_CODE_A), 0, 0, 0, 1, 1);
    return u_create("MMW", "w", 0, 0, 0, 50, 50, 0, 0, 1);
}

static void test_devices(void) {
    uint16_t caps, err;
    printf("test_mm_wave_devices\n");
    boot(true);
    MCALL(401);
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    caps = useg(0x80);
    memset(umem(caps, 0, 0x80), 0xEE, 0x80);
    MCALL(402, 0, FARP(caps, 0), 48);
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    CHECK(urd16(caps, 0) != 0xEEEE);                       /* wMid */
    CHECK(umem(caps, 6, 1)[0] != 0 && umem(caps, 6, 1)[0] != 0xEE);   /* a name */
    CHECK_EQ(urd32(caps, 38) & 0xFFF, 0xFFF);              /* every standard format up to 44.1 kHz stereo 16 bit */
    CHECK_EQ(urd16(caps, 42), 2);
    CHECK_EQ(urd32(caps, 44) & 0xC, 0xC);                  /* WAVECAPS_VOLUME | WAVECAPS_LRVOLUME */
    CHECK_EQ(urd16(caps, 48), 0xEEEE);                     /* nothing written past the structure */
    memset(umem(caps, 0, 0x80), 0xEE, 0x80);
    MCALL(402, WAVE_MAPPER, FARP(caps, 0), 10);            /* the mapper answers too; wSize truncates */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK(urd16(caps, 10) == 0xEEEE);
    MCALL(402, 1, FARP(caps, 0), 48);
    CHECK_EQ(fx_ax(&uf), 2);                               /* MMSYSERR_BADDEVICEID */
    memset(&err, 0, sizeof err);
    mmdown();
    boot(false);                                           /* no audio engine: no wave devices */
    MCALL(401);
    CHECK_EQ(fx_ax(&uf), 0);
    caps = useg(0x80);
    MCALL(402, 0, FARP(caps, 0), 48);
    CHECK_EQ(fx_ax(&uf), 2);
    put_pcm_fmt(g_fmt, 1, 11025, 8);
    open_wave(0, 0, 0, 0, &err);
    CHECK_EQ(err, 2);
    mmdown();
}

static void test_open_close(void) {
    uint16_t err, hwo, w;
    printf("test_mm_wave_open_close\n");
    boot(true);
    w = mk_window();
    put_pcm_fmt(g_fmt, 1, 11025, 8);
    hwo = open_wave(WAVE_MAPPER, w, 0, 0x10000, &err);       /* CALLBACK_WINDOW */
    CHECK_EQ(err, 0);
    CHECK(hwo != 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    CHECK(peek());                                           /* MM_WOM_OPEN */
    CHECK_EQ(urd16(g_msg, 0), w);
    CHECK_EQ(urd16(g_msg, 2), 0x3BB);
    CHECK_EQ(urd16(g_msg, 4), hwo);
    CHECK(!peek());
    MCALL(405, hwo);
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    CHECK(peek());                                           /* MM_WOM_CLOSE */
    CHECK_EQ(urd16(g_msg, 2), 0x3BC);
    MCALL(405, hwo);
    CHECK_EQ(fx_ax(&uf), 5);                                 /* MMSYSERR_INVALHANDLE */
    /* WAVE_FORMAT_QUERY: validates only */
    {
        uint16_t hs = useg(0x10);
        uwr16(hs, 0, 0x7777);
        MCALL(404, FARP(hs, 0), WAVE_MAPPER, FARP(g_fmt, 0), LONG(0), LONG(0), LONG(1));
        CHECK_EQ(fx_ax(&uf), 0);
        CHECK_EQ(urd16(hs, 0), 0x7777);
        CHECK(!peek());
        MCALL(404, FARP(0, 0), WAVE_MAPPER, FARP(g_fmt, 0), LONG(0), LONG(0), LONG(1));   /* a NULL handle pointer is fine for a query */
        CHECK_EQ(fx_ax(&uf), 0);
    }
    uwr16(g_fmt, 0, 0x55);                                   /* MPEG */
    open_wave(WAVE_MAPPER, 0, 0, 0, &err);
    CHECK_EQ(err, 32);                                       /* WAVERR_BADFORMAT */
    put_pcm_fmt(g_fmt, 1, 11025, 12);
    open_wave(WAVE_MAPPER, 0, 0, 0, &err);
    CHECK_EQ(err, 32);
    put_pcm_fmt(g_fmt, 2, 44100, 16);
    open_wave(3, 0, 0, 0, &err);
    CHECK_EQ(err, 2);                                        /* no such device */
    {   /* a PCMWAVEFORMAT that ends its segment has no cbSize to read past */
        uint16_t tight = useg(16), saved = g_fmt;
        put_pcm_fmt(tight, 1, 8000, 8);
        g_fmt = tight;
        hwo = open_wave(WAVE_MAPPER, 0, 0, 0, &err);
        g_fmt = saved;
        CHECK_EQ(err, 0);
        MCALL(405, hwo);
    }
    put_adpcm_fmt(g_fmt);
    open_wave(0, 0, 0, 0, &err);
    CHECK_EQ(err, 32);                                       /* the card itself does not play ADPCM ... */
    hwo = open_wave(WAVE_MAPPER, 0, 0, 0, &err);
    CHECK_EQ(err, 0);                                        /* ... the mapper converts it */
    CHECK(hwo != 0);
    MCALL(405, hwo);
    mmdown();
}

static void test_play_and_done(void) {
    uint16_t err, hwo, w;
    printf("test_mm_wave_play_done\n");
    boot(true);
    w = mk_window();
    put_pcm_fmt(g_fmt, 1, 11025, 8);
    T(0);
    hwo = open_wave(WAVE_MAPPER, w, 0, 0x10000, &err);
    peek();
    memset(umem(g_data, 0, 11025), 0x90, 11025);              /* one second */
    put_hdr(0, 0, 11025, 0xC, 5);                              /* BEGINLOOP | ENDLOOP set by the program, preserved by Prepare */
    MCALL(408, hwo, FARP(g_hdr, 0), 32);                       /* waveOutWrite before Prepare: WAVERR_UNPREPARED */
    CHECK_EQ(fx_ax(&uf), 34);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    MCALL(406, hwo, FARP(g_hdr, 0), 32);                       /* waveOutPrepareHeader */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    CHECK_EQ(hdr_flags(0), WHDR_PREPARED | WHDR_BEGINLOOP | WHDR_ENDLOOP);
    uwr32(g_hdr, 20, 1);                                       /* one pass: a plain buffer in loop clothes */
    MCALL(408, hwo, FARP(g_hdr, 0), 32);                       /* waveOutWrite */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    CHECK(hdr_flags(0) & WHDR_INQUEUE);
    CHECK(!(hdr_flags(0) & WHDR_DONE));
    MCALL(407, hwo, FARP(g_hdr, 0), 32);                       /* Unprepare while queued */
    CHECK_EQ(fx_ax(&uf), 33);
    MCALL(405, hwo);
    CHECK_EQ(fx_ax(&uf), 33);                                  /* Close while playing */
    T(999);
    CHECK(!(hdr_flags(0) & WHDR_DONE));
    CHECK(!peek());
    T(1000);
    CHECK(hdr_flags(0) & WHDR_DONE);
    CHECK(!(hdr_flags(0) & WHDR_INQUEUE));
    CHECK(hdr_flags(0) & WHDR_PREPARED);
    CHECK(peek());                                             /* MM_WOM_DONE: wParam = handle, lParam = the header */
    CHECK_EQ(urd16(g_msg, 2), 0x3BD);
    CHECK_EQ(urd16(g_msg, 4), hwo);
    CHECK_EQ(urd32(g_msg, 6), (uint32_t)g_hdr << 16);
    CHECK(!peek());
    MCALL(407, hwo, FARP(g_hdr, 0), 32);
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(hdr_flags(0) & WHDR_PREPARED, 0);
    MCALL(405, hwo);
    CHECK_EQ(fx_ax(&uf), 0);
    mmdown();
}

static void test_function_callback_and_order(void) {
    uint16_t err, hwo;
    printf("test_mm_wave_function_callback\n");
    boot(true);
    put_pcm_fmt(g_fmt, 1, 11025, 8);
    T(0);
    hwo = open_wave(WAVE_MAPPER, (uint32_t)u_cs() << 16 | M_REC, 0x1357, 0x30000, &err);   /* CALLBACK_FUNCTION */
    CHECK_EQ(err, 0);
    CHECK_EQ(ulog_n(), 1);
    CHECK_EQ(mrec_id(0), hwo);
    CHECK_EQ(mrec_msg(0), 0x3BB);                              /* WOM_OPEN */
    CHECK_EQ(mrec_user(0), 0x1357);
    put_hdr(0, 0, 1000, 0, 0);
    put_hdr(1, 1000, 2000, 0, 0);
    MCALL(406, hwo, FARP(g_hdr, 0), 32);
    MCALL(406, hwo, FARP(g_hdr, 32), 32);
    MCALL(408, hwo, FARP(g_hdr, 0), 32);
    MCALL(408, hwo, FARP(g_hdr, 32), 32);
    T(90);                                                     /* 1000 frames = 90.7 ms */
    CHECK_EQ(ulog_n(), 1);
    T(91);
    CHECK_EQ(ulog_n(), 2);
    CHECK_EQ(mrec_msg(1), 0x3BD);
    CHECK_EQ(mrec_dw1(1), (uint32_t)g_hdr << 16);              /* first header ... */
    T(273);                                                    /* 3000 frames = 272.1 ms */
    CHECK_EQ(ulog_n(), 3);
    CHECK_EQ(mrec_dw1(2), (uint32_t)g_hdr << 16 | 32);         /* ... then the second */
    /* Reset completes what is queued, in order, at once */
    put_hdr(0, 0, 11025, 0, 0);
    put_hdr(1, 0, 11025, 0, 0);
    MCALL(406, hwo, FARP(g_hdr, 0), 32);
    MCALL(406, hwo, FARP(g_hdr, 32), 32);
    MCALL(408, hwo, FARP(g_hdr, 0), 32);
    MCALL(408, hwo, FARP(g_hdr, 32), 32);
    CHECK(hdr_flags(0) & WHDR_INQUEUE);
    MCALL(411, hwo);                                           /* waveOutReset */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    CHECK_EQ(ulog_n(), 5);
    CHECK_EQ(mrec_dw1(3), (uint32_t)g_hdr << 16);
    CHECK_EQ(mrec_dw1(4), (uint32_t)g_hdr << 16 | 32);
    CHECK(hdr_flags(0) & WHDR_DONE);
    CHECK(hdr_flags(1) & WHDR_DONE);
    MCALL(405, hwo);
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(mrec_msg(5), 0x3BC);                              /* WOM_CLOSE */
    mmdown();
}

static void test_loops_pause_position(void) {
    uint16_t err, hwo, mt;
    printf("test_mm_wave_loops\n");
    boot(true);
    put_pcm_fmt(g_fmt, 1, 11025, 8);
    mt = useg(0x20);
    T(0);
    hwo = open_wave(WAVE_MAPPER, 0, 0, 0, &err);
    put_hdr(0, 0, 1103, WHDR_BEGINLOOP | WHDR_ENDLOOP, 3);   /* 100 ms, three passes */
    MCALL(406, hwo, FARP(g_hdr, 0), 32);
    MCALL(408, hwo, FARP(g_hdr, 0), 32);
    T(299);
    CHECK(!(hdr_flags(0) & WHDR_DONE));
    T(301);
    CHECK(hdr_flags(0) & WHDR_DONE);
    /* infinite loop until waveOutBreakLoop */
    put_hdr(0, 0, 1103, WHDR_BEGINLOOP | WHDR_ENDLOOP, 0xFFFFFFFFu);
    MCALL(406, hwo, FARP(g_hdr, 0), 32);
    MCALL(408, hwo, FARP(g_hdr, 0), 32);
    T(301 + 1050);
    CHECK(!(hdr_flags(0) & WHDR_DONE));
    MCALL(419, hwo);                                           /* waveOutBreakLoop */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    T(301 + 1090);
    CHECK_EQ(hdr_flags(0) & WHDR_DONE, 0);                     /* the pass in progress finishes */
    T(301 + 1110);
    CHECK(hdr_flags(0) & WHDR_DONE);
    /* pause / restart / position: counted from the last reset */
    MCALL(411, hwo);
    put_hdr(0, 0, 11025, 0, 0);
    MCALL(406, hwo, FARP(g_hdr, 0), 32);
    uclock = 1000 + 2000;
    MCALL(607);
    MCALL(408, hwo, FARP(g_hdr, 0), 32);
    T(2000 + 500);
    MCALL(409, hwo);                                           /* waveOutPause */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    T(2000 + 5000);
    CHECK(!(hdr_flags(0) & WHDR_DONE));
    uwr16(mt, 0, 4);                                           /* TIME_BYTES */
    MCALL(412, hwo, FARP(mt, 0), 6);
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    CHECK_EQ(urd16(mt, 0), 4);
    CHECK(urd32(mt, 2) >= 5500 && urd32(mt, 2) <= 5530);       /* 0.5 s of 11025 Hz 8 bit mono */
    uwr16(mt, 0, 2);                                           /* TIME_SAMPLES */
    MCALL(412, hwo, FARP(mt, 0), 6);
    CHECK_EQ(urd16(mt, 0), 2);
    CHECK(urd32(mt, 2) >= 5500 && urd32(mt, 2) <= 5530);
    uwr16(mt, 0, 1);                                           /* TIME_MS */
    MCALL(412, hwo, FARP(mt, 0), 6);
    CHECK_EQ(urd16(mt, 0), 1);
    CHECK(urd32(mt, 2) >= 499 && urd32(mt, 2) <= 501);
    uwr16(mt, 0, 8);                                           /* TIME_SMPTE: not supported, answers in bytes */
    MCALL(412, hwo, FARP(mt, 0), 6);
    CHECK_EQ(urd16(mt, 0), 4);
    MCALL(410, hwo);                                           /* waveOutRestart */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    T(2000 + 5000 + 499);
    CHECK(!(hdr_flags(0) & WHDR_DONE));
    T(2000 + 5000 + 502);
    CHECK(hdr_flags(0) & WHDR_DONE);
    MCALL(405, hwo);
    mmdown();
}

static void test_volume(void) {
    uint16_t dw, l, r;
    printf("test_mm_wave_volume\n");
    boot(true);
    dw = useg(0x10);
    MCALL(415, 0, FARP(dw, 0));                                /* waveOutGetVolume: full scale at start */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    CHECK_EQ(urd32(dw, 0), 0xFFFFFFFFu);
    MCALL(416, 0, LONG(0x80004000u));                          /* waveOutSetVolume: left in the low word */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    MCALL(415, 0, FARP(dw, 0));
    CHECK_EQ(urd32(dw, 0), 0x80004000u);
    audio_get_volume(maudio, &l, &r);
    CHECK_EQ(l, 0x4000);
    CHECK_EQ(r, 0x8000);
    MCALL(415, 5, FARP(dw, 0));
    CHECK_EQ(fx_ax(&uf), 2);                                   /* a device id that does not exist */
    mmdown();
}

static void test_adpcm_through_the_mapper(void) {
    static const uint8_t blk[16] = {3, 40, 0, 0xE8, 0x03, 0x0C, 0xFE, 0x12, 0x7F, 0x80, 0xF0, 0x35, 0xAA, 0x09, 0x70, 0xFF};
    int16_t out[2 * 40];
    uint16_t err, hwo;
    printf("test_mm_wave_adpcm\n");
    boot(true);
    put_adpcm_fmt(g_fmt);
    T(0);
    hwo = open_wave(WAVE_MAPPER, 0, 0, 0, &err);
    memcpy(umem(g_data, 0, 16), blk, 16);
    put_hdr(0, 0, 16, 0, 0);
    MCALL(406, hwo, FARP(g_hdr, 0), 32);
    MCALL(408, hwo, FARP(g_hdr, 0), 32);
    CHECK_EQ(fx_ax(&uf), 0);
    audio_mix(maudio, out, 40);                                /* 11025 -> 44100 Hz: four output frames per sample */
    CHECK_EQ(out[0], -500);                                    /* decoded by the Microsoft ADPCM rules */
    CHECK_EQ(out[2 * 4], 1000);
    CHECK_EQ(out[2 * 8], 665);
    T(2);                                                      /* 20 samples last 1.8 ms */
    CHECK(hdr_flags(0) & WHDR_DONE);
    MCALL(405, hwo);
    mmdown();
}

static void test_snd_play_sound(void) {
    uint16_t img;
    uint8_t *p;
    int16_t out[2 * 8];
    unsigned i;
    printf("test_mm_snd_play_sound\n");
    boot(true);
    img = useg(0x4000);
    p = umem(img, 0, 0x4000);
    memset(p, 0, 0x4000);
    {
        uint16_t f = useg(0x20);
        put_pcm_fmt(f, 1, 11025, 8);
        memcpy(p + 20, umem(f, 0, 16), 16);
    }
    memcpy(p, "RIFF", 4);
    uwr32(img, 4, 4 + 24 + 8 + 4400);
    memcpy(p + 8, "WAVEfmt ", 8);
    uwr32(img, 16, 16);
    memcpy(p + 36, "data", 4);
    uwr32(img, 40, 4400);
    for (i = 0; i < 4400; i++) p[44 + i] = 0xC0;               /* 0.4 s, level (0xC0 - 0x80) * 256 = 16384 */
    T(0);
    MCALL(2, FARP(img, 0), 1 | 4);                             /* SND_ASYNC | SND_MEMORY */
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    audio_mix(maudio, out, 8);
    CHECK_EQ(out[0], 16384);
    CHECK_EQ(out[1], 16384);
    T(399);
    MCALL(2, FARP(0, 0), 0);                                   /* sndPlaySound(NULL, 0) stops it */
    CHECK_EQ(fx_ax(&uf), 1);
    audio_mix(maudio, out, 8);
    CHECK_EQ(out[0], 0);
    /* synchronous: returns only when it has played; with the null sink that takes no host time */
    MCALL(2, FARP(img, 0), 4);
    CHECK_EQ(fx_ax(&uf), 1);
    audio_mix(maudio, out, 8);
    CHECK_EQ(out[0], 0);
    /* looping stays audible until stopped */
    MCALL(2, FARP(img, 0), 1 | 4 | 8);
    T(399 + 5000);
    audio_mix(maudio, out, 8);
    CHECK_EQ(out[0], 16384);
    MCALL(2, FARP(0, 0), 0);
    audio_mix(maudio, out, 8);
    CHECK_EQ(out[0], 0);
    /* not a WAVE image */
    uwr32(img, 8, 0x20202020);
    MCALL(2, FARP(img, 0), 1 | 4);
    CHECK_EQ(fx_ax(&uf), 0);
    mmdown();
}

/* sndPlaySound by file name: resolved through KERNEL's DOS paths onto a host directory */
static void test_snd_play_file(void) {
    kernel_config kc;
    kernel_t *k;
    char *dir = hd_make();
    uint8_t wav[44 + 1100];
    int16_t out[16];
    uint16_t name;
    printf("test_mm_snd_play_file\n");
    memset(&kc, 0, sizeof kc);
    kc.host_root['C' - 'A'] = dir;
    hd_mkdir(dir, "SND");
    memset(wav, 0, sizeof wav);
    memcpy(wav, "RIFF", 4); mm_wr32(wav + 4, 36 + 1100); memcpy(wav + 8, "WAVEfmt ", 8); mm_wr32(wav + 16, 16);
    mm_wr16(wav + 20, 1); mm_wr16(wav + 22, 1); mm_wr32(wav + 24, 11025); mm_wr32(wav + 28, 11025); mm_wr16(wav + 32, 1);
    mm_wr16(wav + 34, 8);
    memcpy(wav + 36, "data", 4); mm_wr32(wav + 40, 1100);
    memset(wav + 44, 0xC0, 1100);
    hd_write(dir, "SND/BEEP.WAV", wav, sizeof wav);
    uboot();
    k = kernel_create(&kc, &uh, 0);
    CHECK(k != NULL);
    maudio = audio_create(&(audio_config){.rate = 44100});
    mm = mmsystem_create(&(mmsystem_config){.audio = maudio}, &uh, uu, k);
    CHECK(mm && mmsystem_attach(mm, uf.p) && kernel_attach(k, uf.p));
    name = ustr("C:\\SND\\beep.wav");                       /* any case, like every DOS path */
    T(0);
    MCALL(2, FARP(name, 0), 1);
    CHECK_EQ(fx_ax(&uf), 1);
    audio_mix(maudio, out, 4);
    CHECK_EQ(out[0], 16384);
    name = ustr("C:\\SND\\NOPE.WAV");
    MCALL(2, FARP(name, 0), 1);
    CHECK_EQ(fx_ax(&uf), 0);                                  /* not found: FALSE, and what played before is stopped */
    audio_mix(maudio, out, 4);
    CHECK_EQ(out[0], 0);
    name = ustr("C:\\SND\\BEEP.WAV");
    MCALL(2, FARP(name, 0), 0);                               /* synchronous */
    CHECK_EQ(fx_ax(&uf), 1);
    mmsystem_destroy(mm);
    mm = NULL;
    audio_destroy(maudio);
    maudio = NULL;
    kernel_destroy(k);
    udown();
    hd_remove(dir);
}

static void test_wave_in_absent(void) {
    static const struct { unsigned ord, bytes, err; } calls[] = {
        {502, 8, 2}, {504, 22, 2}, {505, 2, 5}, {506, 8, 5}, {507, 8, 5}, {508, 8, 5}, {509, 2, 5}, {511, 2, 5}, {512, 8, 5}};
    size_t i;
    printf("test_mm_wave_in_absent\n");
    boot(true);
    for (i = 0; i < sizeof calls / sizeof calls[0]; i++) {
        uint16_t w[11] = {0};
        w[0] = 0; w[1] = 0;
        fx_call_words(&uf, "MMSYSTEM", calls[i].ord, w, calls[i].bytes / 2);
        CHECK_EQ(fx_ax(&uf), calls[i].err);
        CHECK_EQ(uf.sp_after, uf.sp_before);
    }
    CHECK(mmsystem_stub_count(mm) >= 9);
    mmdown();
}

int main(void) {
    test_devices();
    test_open_close();
    test_play_and_done();
    test_function_callback_and_order();
    test_loops_pause_position();
    test_volume();
    test_adpcm_through_the_mapper();
    test_snd_play_sound();
    test_snd_play_file();
    test_wave_in_absent();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
