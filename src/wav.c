/* WAVE parsing and decoding. See wav.h. */
#include "wav.h"

#include <stdlib.h>
#include <string.h>

static unsigned rd16(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }

wav_err wav_format_parse(const uint8_t *fmt, size_t len, wav_format *out) {
    memset(out, 0, sizeof *out);
    if (len < 16) return WAV_E_SHORT;
    out->tag = (uint16_t)rd16(fmt);
    out->channels = (uint16_t)rd16(fmt + 2);
    out->rate = rd32(fmt + 4);
    out->avg_bytes = rd32(fmt + 8);
    out->block_align = (uint16_t)rd16(fmt + 12);
    out->bits = (uint16_t)rd16(fmt + 14);
    if (out->tag == WAVE_FORMAT_ADPCM) {
        unsigned i;
        if (len < 22) return WAV_E_SHORT;
        out->spb = (uint16_t)rd16(fmt + 18);
        out->ncoef = (uint16_t)rd16(fmt + 20);
        if (out->ncoef > 7 || len < 22 + 4u * out->ncoef) return WAV_E_SHORT;
        for (i = 0; i < out->ncoef; i++) {
            out->coef[i][0] = (int16_t)rd16(fmt + 22 + 4 * i);
            out->coef[i][1] = (int16_t)rd16(fmt + 24 + 4 * i);
        }
    }
    return WAV_OK;
}

bool wav_format_supported(const wav_format *f) {
    if (f->channels < 1 || f->channels > 2 || f->rate < 1 || f->rate > 96000) return false;
    if (f->tag == WAVE_FORMAT_PCM) return f->bits == 8 || f->bits == 16;
    if (f->tag == WAVE_FORMAT_ADPCM)
        return f->bits == 4 && f->ncoef >= 1 && f->spb >= 2 && f->block_align >= 7u * f->channels;
    return false;
}

wav_err wav_parse(const uint8_t *file, size_t len, wav_format *fmt, const uint8_t **data, size_t *data_len) {
    size_t k = 12;
    bool have_fmt = false;
    if (len < 12) return WAV_E_SHORT;
    if (memcmp(file, "RIFF", 4) || memcmp(file + 8, "WAVE", 4)) return WAV_E_RIFF;
    while (k + 8 <= len) {
        uint32_t n = rd32(file + k + 4);
        size_t body = k + 8, avail = len - body, take = n < avail ? n : avail;
        if (!memcmp(file + k, "fmt ", 4)) {
            wav_err e = wav_format_parse(file + body, take, fmt);
            if (e != WAV_OK) return e;
            have_fmt = true;
        } else if (!memcmp(file + k, "data", 4)) {
            if (!have_fmt) return WAV_E_NOFMT;
            *data = file + body;
            *data_len = take;
            return WAV_OK;
        }
        k = body + take + (n & 1);
    }
    return have_fmt ? WAV_E_NODATA : WAV_E_NOFMT;
}

static int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

typedef struct {
    int delta, s1, s2;
    int16_t c1, c2;
} adpcm_ch;

static int16_t adpcm_nibble(adpcm_ch *c, unsigned nib) {
    static const int adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
    int pred = (c->s1 * c->c1 + c->s2 * c->c2) / 256;
    int16_t out = clamp16(pred + ((int)nib - ((nib & 8) ? 16 : 0)) * c->delta);
    c->s2 = c->s1;
    c->s1 = out;
    c->delta = adapt[nib] * c->delta / 256;
    if (c->delta < 16) c->delta = 16;
    return out;
}

/* Decodes (out != NULL) or just counts the frames of MS ADPCM data. A block starts with, per channel: predictor
 * index, then delta, then the two newest samples (the older one is output first); the nibbles follow, high first,
 * one byte per frame when stereo (left in the high nibble). */
static size_t decode_adpcm(const wav_format *f, const uint8_t *d, size_t len, int16_t *out) {
    unsigned ch = f->channels, hdr = 7 * ch;
    size_t frames = 0, pos = 0;
    while (len - pos >= hdr) {
        size_t blen = len - pos < f->block_align ? len - pos : f->block_align;
        const uint8_t *b = d + pos, *nib = b + hdr;
        size_t bytes = blen - hdr, j, at = frames + 2;
        adpcm_ch c[2];
        unsigned k;
        if (out) {
            for (k = 0; k < ch; k++) {
                unsigned bp = b[k] < f->ncoef ? b[k] : 0;
                c[k].c1 = f->coef[bp][0];
                c[k].c2 = f->coef[bp][1];
                c[k].delta = (int16_t)rd16(b + ch + 2 * k);
                c[k].s1 = (int16_t)rd16(b + 3 * ch + 2 * k);
                c[k].s2 = (int16_t)rd16(b + 5 * ch + 2 * k);
                out[frames * ch + k] = (int16_t)c[k].s2;
                out[(frames + 1) * ch + k] = (int16_t)c[k].s1;
            }
            for (j = 0; j < bytes; j++) {
                if (ch == 1) {
                    out[at++] = adpcm_nibble(&c[0], nib[j] >> 4);
                    out[at++] = adpcm_nibble(&c[0], nib[j] & 15u);
                } else {
                    out[at * 2] = adpcm_nibble(&c[0], nib[j] >> 4);
                    out[at * 2 + 1] = adpcm_nibble(&c[1], nib[j] & 15u);
                    at++;
                }
            }
        }
        frames += 2 + bytes * 2 / ch;
        pos += blen;
    }
    return frames;
}

size_t wav_decode(const wav_format *f, const uint8_t *d, size_t len, int16_t **out) {
    size_t frames = 0, i, n;
    *out = NULL;
    if (!wav_format_supported(f)) return 0;
    if (f->tag == WAVE_FORMAT_PCM) {
        unsigned bytes = f->bits / 8u * f->channels;
        frames = len / bytes;
        n = frames * f->channels;
        if (!n || !(*out = malloc(n * sizeof **out))) return *out ? frames : 0;
        for (i = 0; i < n; i++)
            (*out)[i] = f->bits == 8 ? (int16_t)(((int)d[i] - 128) * 256) : (int16_t)rd16(d + 2 * i);
        return frames;
    }
    frames = decode_adpcm(f, d, len, NULL);
    if (!frames || !(*out = malloc(frames * f->channels * sizeof **out))) return 0;
    decode_adpcm(f, d, len, *out);
    return frames;
}
