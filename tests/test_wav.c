/* WAVE parsing and decoding: PCM 8/16 bit and Microsoft ADPCM, against hand-built images. The ADPCM vectors
 * are the output of an independent decoder (ffmpeg's adpcm_ms) on blocks built by hand with random-looking
 * headers and nibbles, so a wrong predictor, adaptation or clamp shows up. */
#include <stdlib.h>
#include <string.h>

#include "fixture.h"
#include "wav.h"

static void p16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *p, uint32_t v) { p16(p, v & 0xFFFF); p16(p + 2, v >> 16); }

static const int16_t std_coef[7][2] = {{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}};

/* A WAVEFORMATEX (tag 2) with the standard seven coefficient pairs; returns its size (50). */
static size_t adpcm_fmt(uint8_t *f, unsigned ch, uint32_t rate, unsigned block, unsigned spb) {
    unsigned i;
    p16(f, 2); p16(f + 2, ch); p32(f + 4, rate); p32(f + 8, rate * block / spb); p16(f + 12, block); p16(f + 14, 4);
    p16(f + 16, 32); p16(f + 18, spb); p16(f + 20, 7);
    for (i = 0; i < 7; i++) { p16(f + 22 + 4 * i, (uint16_t)std_coef[i][0]); p16(f + 24 + 4 * i, (uint16_t)std_coef[i][1]); }
    return 50;
}

static void test_format(void) {
    uint8_t f[64];
    wav_format w;
    printf("test_wav_format\n");
    memset(f, 0, sizeof f);
    p16(f, 1); p16(f + 2, 2); p32(f + 4, 22050); p32(f + 8, 88200); p16(f + 12, 4); p16(f + 14, 16);
    CHECK_EQ(wav_format_parse(f, 16, &w), WAV_OK);                  /* PCMWAVEFORMAT: no cbSize */
    CHECK_EQ(w.tag, 1); CHECK_EQ(w.channels, 2); CHECK_EQ(w.rate, 22050); CHECK_EQ(w.bits, 16); CHECK_EQ(w.block_align, 4);
    CHECK(wav_format_supported(&w));
    CHECK_EQ(wav_format_parse(f, 15, &w), WAV_E_SHORT);
    p16(f + 14, 12);                                                /* 12 bit PCM is not a thing here */
    CHECK_EQ(wav_format_parse(f, 16, &w), WAV_OK);
    CHECK(!wav_format_supported(&w));
    p16(f + 14, 8); p16(f + 2, 3);
    wav_format_parse(f, 16, &w);
    CHECK(!wav_format_supported(&w));                               /* three channels */
    p16(f + 2, 1); p32(f + 4, 0);
    wav_format_parse(f, 16, &w);
    CHECK(!wav_format_supported(&w));                               /* rate 0 */
    p16(f, 0x55);                                                   /* MPEG layer 3 */
    p32(f + 4, 22050);
    wav_format_parse(f, 16, &w);
    CHECK(!wav_format_supported(&w));
    CHECK_EQ(adpcm_fmt(f, 1, 11025, 256, 500), 50);
    CHECK_EQ(wav_format_parse(f, 50, &w), WAV_OK);
    CHECK_EQ(w.tag, 2); CHECK_EQ(w.bits, 4); CHECK_EQ(w.spb, 500); CHECK_EQ(w.ncoef, 7);
    CHECK_EQ(w.coef[1][1], -256); CHECK_EQ(w.coef[6][0], 392);
    CHECK(wav_format_supported(&w));
    CHECK_EQ(wav_format_parse(f, 20, &w), WAV_E_SHORT);             /* ADPCM without its extension */
    p16(f + 20, 9);                                                 /* more pairs than a format can hold */
    CHECK_EQ(wav_format_parse(f, 50, &w), WAV_E_SHORT);
}

static void test_pcm(void) {
    static const uint8_t u8[] = {0x80, 0xFF, 0x00, 0x81};
    static const uint8_t s16[] = {0x34, 0x12, 0x00, 0x80, 0xFF, 0x7F};
    wav_format w = {.tag = 1, .channels = 1, .rate = 11025, .bits = 8, .block_align = 1};
    int16_t *o = NULL;
    printf("test_wav_pcm\n");
    CHECK_EQ(wav_decode(&w, u8, 4, &o), 4);                         /* unsigned 8 bit -> signed 16 bit */
    CHECK_EQ(o[0], 0); CHECK_EQ(o[1], 127 * 256); CHECK_EQ(o[2], -128 * 256); CHECK_EQ(o[3], 256);
    free(o);
    w.bits = 16; w.block_align = 2;
    CHECK_EQ(wav_decode(&w, s16, 6, &o), 3);
    CHECK_EQ(o[0], 0x1234); CHECK_EQ(o[1], -32768); CHECK_EQ(o[2], 32767);
    free(o);
    CHECK_EQ(wav_decode(&w, s16, 5, &o), 2);                        /* a trailing odd byte is dropped */
    free(o);
    w.channels = 2; w.block_align = 4;
    CHECK_EQ(wav_decode(&w, s16, 6, &o), 1);                        /* one stereo frame */
    CHECK_EQ(o[1], -32768);
    free(o);
    CHECK_EQ(wav_decode(&w, s16, 0, &o), 0);
}

static void test_adpcm_mono(void) {
    static const int16_t want[20] = {-500, 1000, 665, 818, 996, 877, 378, 502, 294, 346, 759, 1290, -55, -2143, -1621,
                                     -6826, 6642, 3275, 372, -2266};
    uint8_t f[64], blk[16] = {3, 40, 0, 0xE8, 0x03, 0x0C, 0xFE, 0x12, 0x7F, 0x80, 0xF0, 0x35, 0xAA, 0x09, 0x70, 0xFF};
    wav_format w;
    int16_t *o = NULL;
    size_t n, i;
    printf("test_wav_adpcm_mono\n");
    adpcm_fmt(f, 1, 11025, 16, 20);
    CHECK_EQ(wav_format_parse(f, 50, &w), WAV_OK);
    n = wav_decode(&w, blk, sizeof blk, &o);
    CHECK_EQ(n, 20);
    for (i = 0; i < n && i < 20; i++) CHECK_EQ(o[i], want[i]);
    free(o);
    /* two blocks decode independently of each other */
    {
        uint8_t two[32];
        memcpy(two, blk, 16);
        memcpy(two + 16, blk, 16);
        n = wav_decode(&w, two, 32, &o);
        CHECK_EQ(n, 40);
        CHECK_EQ(o[20], -500); CHECK_EQ(o[39], -2266);
        free(o);
        n = wav_decode(&w, two, 24, &o);                            /* a short last block: header + 1 byte */
        CHECK_EQ(n, 24);
        free(o);
        n = wav_decode(&w, two, 20, &o);                            /* header (7 bytes) cut: nothing */
        CHECK_EQ(n, 20);
        free(o);
    }
}

static void test_adpcm_stereo(void) {
    static const int16_t want[20] = {1900, -1800, 2000, -2000, 2130, -1755, 2442, -1698, 2258, -1591, 1888, -1491, 2019,
                                     -1212, 1250, -1490, 481, -2222, 1595, -2083};
    uint8_t f[64], blk[22] = {1, 4, 30, 0, 60, 0, 0xD0, 0x07, 0x30, 0xF8, 0x6C, 0x07, 0xF8, 0xF8,
                              0x12, 0x7F, 0x80, 0xF0, 0x35, 0xAA, 0x09, 0x70};
    wav_format w;
    int16_t *o = NULL;
    size_t n, i;
    printf("test_wav_adpcm_stereo\n");
    adpcm_fmt(f, 2, 22050, 22, 10);
    wav_format_parse(f, 50, &w);
    n = wav_decode(&w, blk, sizeof blk, &o);
    CHECK_EQ(n, 10);
    for (i = 0; i < 20 && i < 2 * n; i++) CHECK_EQ(o[i], want[i]);
    free(o);
}

static size_t riff(uint8_t *b, const uint8_t *fmt, size_t fl, const uint8_t *data, size_t dl, bool junk) {
    size_t k = 12;
    memcpy(b, "RIFF", 4); memcpy(b + 8, "WAVE", 4);
    if (junk) { memcpy(b + k, "LIST", 4); p32(b + k + 4, 3); memcpy(b + k + 8, "abc", 3); b[k + 11] = 0; k += 12; }   /* odd size is padded */
    memcpy(b + k, "fmt ", 4); p32(b + k + 4, (uint32_t)fl); memcpy(b + k + 8, fmt, fl); k += 8 + fl + (fl & 1);
    memcpy(b + k, "data", 4); p32(b + k + 4, (uint32_t)dl); memcpy(b + k + 8, data, dl); k += 8 + dl;
    p32(b + 4, (uint32_t)(k - 8));
    return k;
}

static void test_riff(void) {
    uint8_t f[16] = {1, 0, 1, 0, 0x11, 0x2B, 0, 0, 0x11, 0x2B, 0, 0, 1, 0, 8, 0}, b[128];
    static const uint8_t pcm[4] = {1, 2, 3, 4};
    const uint8_t *data;
    size_t dl, n;
    wav_format w;
    printf("test_wav_riff\n");
    n = riff(b, f, 16, pcm, 4, false);
    CHECK_EQ(wav_parse(b, n, &w, &data, &dl), WAV_OK);
    CHECK_EQ(w.rate, 11025); CHECK_EQ(dl, 4); CHECK(data == b + n - 4);
    n = riff(b, f, 16, pcm, 4, true);                               /* unknown chunks are skipped */
    CHECK_EQ(wav_parse(b, n, &w, &data, &dl), WAV_OK);
    CHECK_EQ(dl, 4);
    CHECK_EQ(wav_parse(b, 8, &w, &data, &dl), WAV_E_SHORT);
    b[0] = 'X';
    CHECK_EQ(wav_parse(b, n, &w, &data, &dl), WAV_E_RIFF);
    b[0] = 'R';
    memcpy(b + 8, "AVI ", 4);
    CHECK_EQ(wav_parse(b, n, &w, &data, &dl), WAV_E_RIFF);
    memcpy(b + 8, "WAVE", 4);
    n = riff(b, f, 16, pcm, 4, false);
    p32(b + n - 8, 1000);                                           /* a data chunk longer than the file is clipped */
    CHECK_EQ(wav_parse(b, n, &w, &data, &dl), WAV_OK);
    CHECK_EQ(dl, 4);
    memcpy(b + 12, "fmtx", 4);                                       /* no fmt chunk */
    CHECK_EQ(wav_parse(b, n, &w, &data, &dl), WAV_E_NOFMT);
    memcpy(b + 12, "fmt ", 4);
    memcpy(b + n - 12, "dat_", 4);
    CHECK_EQ(wav_parse(b, n, &w, &data, &dl), WAV_E_NODATA);
}

int main(void) {
    test_format();
    test_pcm();
    test_adpcm_mono();
    test_adpcm_stereo();
    test_riff();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
