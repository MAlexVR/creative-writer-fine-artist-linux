/* KWAJ / SZDD expansion. See kwaj.h.
 *
 * KWAJ container: "KWAJ" 88 F0 27 D1, method u16, data offset u16, flags u16, then the optional fields
 * selected by the flags, in order: 0x01 uncompressed length u32; 0x02 u16 (unknown); 0x04 u16 N + N bytes;
 * 0x08 NUL-terminated name; 0x10 NUL-terminated extension. The data starts at the data offset.
 *
 * Method 3 (LZ + Huffman, MSB-first bit stream), as read from the data offset:
 *   six 4-bit table kinds (the sixth is unused), then five Huffman tables in this order, each with the symbol
 *   count shown: MATCHLEN1 (16), MATCHLEN2 (16), LITLEN (32), OFFSET (64), LITERAL (256). Code lengths are
 *   4-bit values, stored by kind:
 *     0  every symbol has length log2(symbol count)
 *     1  first length 4 bits; then per symbol: bit 0 = same, 10 = previous + 1, 11 = a fresh 4-bit length
 *     2  first length 4 bits; then per symbol a 2-bit selector: 0 = previous - 1, 1 = same, 2 = previous + 1,
 *        3 = a fresh 4-bit length
 *     3  one 4-bit length per symbol
 *   Codes are canonical (shorter first, then by symbol), a length of 0 meaning "unused". The 4096-byte window
 *   starts filled with spaces and the write position at 0. Decoding loop, with "flag" starting false:
 *     symbol = decode(flag ? MATCHLEN2 : MATCHLEN1)
 *     symbol 0: a run of literals: n = decode(LITLEN) + 1 bytes, each decode(LITERAL); flag = (n != 32)
 *     otherwise a match of symbol + 2 bytes at distance decode(OFFSET) * 64 + 6 raw bits; flag = false
 *   Verified against libmspack on every compressed file of the Spanish Creative Writer / Fine Artist disks.
 *
 * Method 2 and SZDD are the classic LZSS: 4096-byte window of spaces, write position 4096 - 16, a control
 * byte per 8 items (LSB first; 1 = literal byte, 0 = two bytes: b0 + (b1 & 0xF0) << 4 is the window
 * position, (b1 & 0xF) + 3 the length). Methods 0 and 1 are verified only by synthetic tests; method 2 and
 * SZDD likewise (no real file uses them). */
#include "kwaj.h"

#include <stdlib.h>
#include <string.h>

#define MAX_OUT ((size_t)1 << 30)
#define LZH_WINDOW 4096

static const uint8_t KWAJ_MAGIC[8] = {'K', 'W', 'A', 'J', 0x88, 0xF0, 0x27, 0xD1};
static const uint8_t SZDD_MAGIC[8] = {'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33};

bool kwaj_is_compressed(const uint8_t *buf, size_t len) {
    return buf && len >= 8 && (!memcmp(buf, KWAJ_MAGIC, 8) || !memcmp(buf, SZDD_MAGIC, 8));
}

const char *kwaj_strerror(kwaj_err err) {
    switch (err) {
    case KWAJ_OK: return "ok";
    case KWAJ_NOT_COMPRESSED: return "not a KWAJ or SZDD file";
    case KWAJ_TRUNCATED: return "compressed file is truncated";
    case KWAJ_BAD_METHOD: return "unsupported compression method";
    case KWAJ_CORRUPT: return "corrupt compressed data";
    case KWAJ_NOMEM: return "out of memory";
    }
    return "unknown error";
}

/* ---- output sink --------------------------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    size_t limit;   /* the exact length the header promises, or 0 when unknown */
    bool oom;
} sink;

static bool sink_full(const sink *s) {
    return s->limit && s->len >= s->limit;
}

static void sink_put(sink *s, uint8_t b) {
    if (s->len == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 4096;
        uint8_t *nb;
        if (ncap > MAX_OUT) {
            s->oom = true;
            return;
        }
        nb = realloc(s->buf, ncap);
        if (!nb) {
            s->oom = true;
            return;
        }
        s->buf = nb;
        s->cap = ncap;
    }
    s->buf[s->len++] = b;
}

/* ---- LZSS (method 2, SZDD) ------------------------------------------------------------------------ */

static kwaj_err lzss_expand(const uint8_t *in, size_t n, sink *out) {
    uint8_t win[LZH_WINDOW];
    size_t i = 0, pos = LZH_WINDOW - 16;
    memset(win, ' ', sizeof win);
    while (i < n && !sink_full(out) && !out->oom) {
        unsigned ctl = in[i++], bit;
        for (bit = 0; bit < 8 && !sink_full(out) && !out->oom; bit++) {
            if (i >= n) return out->limit && !sink_full(out) ? KWAJ_TRUNCATED : KWAJ_OK;
            if (ctl >> bit & 1) {
                uint8_t b = in[i++];
                win[pos++ & (LZH_WINDOW - 1)] = b;
                sink_put(out, b);
            } else {
                unsigned b0, b1, from, len, k;
                if (i + 1 >= n) return out->limit && !sink_full(out) ? KWAJ_TRUNCATED : KWAJ_OK;
                b0 = in[i++];
                b1 = in[i++];
                from = b0 | (b1 & 0xF0) << 4;
                len = (b1 & 0x0F) + 3;
                for (k = 0; k < len && !sink_full(out); k++) {
                    uint8_t b = win[(from + k) & (LZH_WINDOW - 1)];
                    win[pos++ & (LZH_WINDOW - 1)] = b;
                    sink_put(out, b);
                }
            }
        }
    }
    if (out->limit && out->len < out->limit) return KWAJ_TRUNCATED;
    return KWAJ_OK;
}

/* ---- LZ + Huffman (method 3) ---------------------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    size_t nbits, pos;
    bool over;          /* a read went past the end of the input */
} bitrd;

static unsigned rd_bits(bitrd *r, unsigned n) {
    unsigned v = 0;
    while (n--) {
        unsigned bit = 0;
        if (r->pos < r->nbits) bit = (r->p[r->pos >> 3] >> (7 - (r->pos & 7))) & 1;
        else r->over = true;
        r->pos++;
        v = v << 1 | bit;
    }
    return v;
}

#define MAXLEN 15

typedef struct {
    uint16_t count[MAXLEN + 1];
    uint16_t sym[256];
    unsigned nsyms;
} huff;

/* Canonical codes from lengths. Over-subscribed and incomplete (but non-empty) sets are refused. */
static bool huff_build(huff *h, const uint8_t *lens, unsigned n) {
    unsigned len, i;
    uint16_t offs[MAXLEN + 2];
    int left = 1;
    memset(h, 0, sizeof *h);
    h->nsyms = n;
    for (i = 0; i < n; i++) h->count[lens[i]]++;
    if (h->count[0] == n) return true;                 /* no symbol is used */
    for (len = 1; len <= MAXLEN; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return false;
    }
    if (left > 0) return false;
    offs[1] = 0;
    for (len = 1; len <= MAXLEN; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (i = 0; i < n; i++)
        if (lens[i]) h->sym[offs[lens[i]]++] = (uint16_t)i;
    return true;
}

/* Returns the symbol, or -1 on an invalid code (or an empty table). */
static int huff_decode(const huff *h, bitrd *r) {
    int code = 0, first = 0, index = 0;
    unsigned len;
    for (len = 1; len <= MAXLEN; len++) {
        int count = h->count[len];
        code |= (int)rd_bits(r, 1);
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static unsigned log2_exact(unsigned n) {
    unsigned k = 0;
    while ((1u << k) < n) k++;
    return k;
}

static void read_lengths(bitrd *r, unsigned kind, unsigned n, uint8_t *lens) {
    unsigned i, c;
    switch (kind) {
    case 0:
        memset(lens, (int)log2_exact(n), n);
        break;
    case 1:
        c = rd_bits(r, 4);
        lens[0] = (uint8_t)c;
        for (i = 1; i < n; i++) {
            if (rd_bits(r, 1)) c = rd_bits(r, 1) ? rd_bits(r, 4) : c + 1;
            lens[i] = (uint8_t)c;
        }
        break;
    case 2:
        c = rd_bits(r, 4);
        lens[0] = (uint8_t)c;
        for (i = 1; i < n; i++) {
            unsigned sel = rd_bits(r, 2);
            c = sel == 3 ? rd_bits(r, 4) : c + sel - 1;
            lens[i] = (uint8_t)c;
        }
        break;
    default:
        for (i = 0; i < n; i++) lens[i] = (uint8_t)rd_bits(r, 4);
        break;
    }
}

static kwaj_err lzh_expand(const uint8_t *in, size_t n, sink *out) {
    static const unsigned nsym[5] = {16, 16, 32, 64, 256};
    enum { MATCHLEN1, MATCHLEN2, LITLEN, OFFSET, LITERAL };
    bitrd r = {in, n * 8, 0, false};
    huff tab[5];
    unsigned kind[6], i;
    uint8_t lens[256], win[LZH_WINDOW];
    size_t wpos = 0;
    bool flag = false;

    for (i = 0; i < 6; i++) kind[i] = rd_bits(&r, 4);
    for (i = 0; i < 5; i++) {
        if (r.over) return KWAJ_TRUNCATED;
        read_lengths(&r, kind[i], nsym[i], lens);
        if (r.over) return KWAJ_TRUNCATED;
        if (!huff_build(&tab[i], lens, nsym[i])) return KWAJ_CORRUPT;
    }
    memset(win, ' ', sizeof win);

    while (!sink_full(out) && !out->oom) {
        int sym;
        size_t mark_len = out->len;
        if (!out->limit && r.pos >= r.nbits) break;        /* unknown length: the input is the end marker */
        sym = huff_decode(&tab[flag ? MATCHLEN2 : MATCHLEN1], &r);
        if (sym < 0) return r.over ? KWAJ_TRUNCATED : KWAJ_CORRUPT;
        if (sym == 0) {
            int run = huff_decode(&tab[LITLEN], &r);
            int k;
            if (run < 0) return r.over ? KWAJ_TRUNCATED : KWAJ_CORRUPT;
            for (k = 0; k <= run && !sink_full(out); k++) {
                int lit = huff_decode(&tab[LITERAL], &r);
                if (lit < 0) break;
                win[wpos++ & (LZH_WINDOW - 1)] = (uint8_t)lit;
                sink_put(out, (uint8_t)lit);
            }
            flag = run != 31;
        } else {
            int hi = huff_decode(&tab[OFFSET], &r);
            unsigned dist, len = (unsigned)sym + 2, k;
            if (hi < 0) return r.over ? KWAJ_TRUNCATED : KWAJ_CORRUPT;
            dist = (unsigned)hi << 6 | rd_bits(&r, 6);
            for (k = 0; k < len && !sink_full(out); k++) {
                uint8_t b = win[(wpos - dist) & (LZH_WINDOW - 1)];
                win[wpos++ & (LZH_WINDOW - 1)] = b;
                sink_put(out, b);
            }
            flag = false;
        }
        if (r.over) {                                     /* this operation read bits that are not there */
            if (out->limit) return KWAJ_TRUNCATED;
            out->len = mark_len;                          /* unknown length: drop the partial operation */
            break;
        }
    }
    return KWAJ_OK;
}

/* ---- containers ----------------------------------------------------------------------------------- */

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

kwaj_err kwaj_expand(const uint8_t *buf, size_t len, uint8_t **out, size_t *out_len) {
    sink s = {NULL, 0, 0, 0, false};
    kwaj_err err = KWAJ_OK;
    const uint8_t *data;
    size_t dlen;

    *out = NULL;
    *out_len = 0;
    if (!kwaj_is_compressed(buf, len)) return KWAJ_NOT_COMPRESSED;

    if (!memcmp(buf, SZDD_MAGIC, 8)) {
        if (len < 14) return KWAJ_TRUNCATED;
        if (buf[8] != 'A') return KWAJ_BAD_METHOD;
        s.limit = le32(buf + 10);
        data = buf + 14;
        dlen = len - 14;
        if (s.limit > MAX_OUT) return KWAJ_CORRUPT;
        if (s.limit) {
            s.buf = malloc(s.limit);
            if (!s.buf) return KWAJ_NOMEM;
            s.cap = s.limit;
        }
        err = lzss_expand(data, dlen, &s);
    } else {
        unsigned method, offset, flags;
        size_t p = 14;
        if (len < 14) return KWAJ_TRUNCATED;
        method = (unsigned)(buf[8] | buf[9] << 8);
        offset = (unsigned)(buf[10] | buf[11] << 8);
        flags = (unsigned)(buf[12] | buf[13] << 8);
        if (flags & 0x01) {
            if (len < p + 4) return KWAJ_TRUNCATED;
            s.limit = le32(buf + p);
            p += 4;
            if (s.limit > MAX_OUT) return KWAJ_CORRUPT;
        }
        if (method > 4) return KWAJ_BAD_METHOD;
        if (offset < 14 || offset > len) return offset > len ? KWAJ_TRUNCATED : KWAJ_CORRUPT;
        data = buf + offset;
        dlen = len - offset;
        if (method == 4) return KWAJ_BAD_METHOD;
        if (s.limit) {
            s.buf = malloc(s.limit);
            if (!s.buf) return KWAJ_NOMEM;
            s.cap = s.limit;
        }
        switch (method) {
        case 0:
        case 1: {
            size_t i, n = s.limit ? s.limit : dlen;
            if (n > dlen) {
                err = KWAJ_TRUNCATED;
                break;
            }
            for (i = 0; i < n; i++) sink_put(&s, method ? (uint8_t)~data[i] : data[i]);
            break;
        }
        case 2: err = lzss_expand(data, dlen, &s); break;
        default: err = lzh_expand(data, dlen, &s); break;
        }
    }
    if (err == KWAJ_OK && s.oom) err = KWAJ_NOMEM;
    if (err != KWAJ_OK) {
        free(s.buf);
        return err;
    }
    *out = s.buf ? s.buf : malloc(1);
    *out_len = s.len;
    return *out ? KWAJ_OK : KWAJ_NOMEM;
}
