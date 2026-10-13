/* KWAJ / SZDD expansion (src/kwaj.h). Synthetic streams are built by a tiny encoder in this file, so the
 * decoder is checked against the format's rules and not against itself; with CWFA_ES_CD set (the extracted
 * Spanish Creative Writer CD, reference/es_nrg1) the real compressed game files are expanded as well. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>

#include "fixture.h"
#include "kwaj.h"
#include "ne.h"

/* ---- test-side encoder ---------------------------------------------------------------------------- */

typedef struct {
    uint8_t buf[8192];
    size_t nbits;
} bitw;

static void put_bits(bitw *w, unsigned val, unsigned n) {          /* MSB first */
    while (n--) {
        unsigned bit = (val >> n) & 1;
        if (w->nbits % 8 == 0) w->buf[w->nbits / 8] = 0;
        w->buf[w->nbits / 8] |= (uint8_t)(bit << (7 - w->nbits % 8));
        w->nbits++;
    }
}

static size_t kwaj_header(uint8_t *out, unsigned method, unsigned flags, uint32_t len) {
    size_t n = 0;
    static const uint8_t magic[8] = {'K', 'W', 'A', 'J', 0x88, 0xF0, 0x27, 0xD1};
    memcpy(out, magic, 8);
    out[8] = (uint8_t)method;
    out[9] = (uint8_t)(method >> 8);
    out[12] = (uint8_t)flags;
    out[13] = (uint8_t)(flags >> 8);
    n = 14;
    if (flags & 1) {
        out[n++] = (uint8_t)len;
        out[n++] = (uint8_t)(len >> 8);
        out[n++] = (uint8_t)(len >> 16);
        out[n++] = (uint8_t)(len >> 24);
    }
    out[10] = (uint8_t)n;
    out[11] = (uint8_t)(n >> 8);
    return n;
}

/* ---- format basics ------------------------------------------------------------------------------- */

static void test_detect(void) {
    uint8_t h[32];
    kwaj_header(h, 0, 1, 0);
    CHECK(kwaj_is_compressed(h, 18));
    CHECK(!kwaj_is_compressed(h, 7));
    CHECK(!kwaj_is_compressed((const uint8_t *)"MZ\x90\0\3\0\0\0", 8));
    CHECK(kwaj_is_compressed((const uint8_t *)"SZDD\x88\xF0\x27\x33" "A\0\0\0\0\0", 14));
    CHECK(!kwaj_is_compressed(NULL, 0));
    CHECK(!strcmp(kwaj_strerror(KWAJ_OK), "ok"));
    CHECK(strlen(kwaj_strerror(KWAJ_BAD_METHOD)) > 3);
}

static void test_method0_and_1(void) {
    uint8_t f[64], *out = NULL;
    size_t n, out_len = 0;
    n = kwaj_header(f, 0, 1, 5);
    memcpy(f + n, "hello", 5);
    CHECK_EQ(kwaj_expand(f, n + 5, &out, &out_len), KWAJ_OK);
    CHECK_EQ(out_len, 5);
    CHECK(out && !memcmp(out, "hello", 5));
    free(out);
    out = NULL;

    n = kwaj_header(f, 1, 1, 3);                   /* method 1: every byte XORed with 0xFF */
    f[n] = (uint8_t)~'a';
    f[n + 1] = (uint8_t)~'b';
    f[n + 2] = (uint8_t)~'c';
    CHECK_EQ(kwaj_expand(f, n + 3, &out, &out_len), KWAJ_OK);
    CHECK(out_len == 3 && !memcmp(out, "abc", 3));
    free(out);
    out = NULL;

    n = kwaj_header(f, 0, 0, 0);                   /* no length field: the rest of the file */
    memcpy(f + n, "xyz", 3);
    CHECK_EQ(kwaj_expand(f, n + 3, &out, &out_len), KWAJ_OK);
    CHECK(out_len == 3 && !memcmp(out, "xyz", 3));
    free(out);
}

static void test_header_flags(void) {
    uint8_t f[64], *out = NULL;
    size_t out_len = 0;
    /* flags 0x1F: length, 2 unknown bytes, 3 extra bytes (length-prefixed), name "A\0", extension "B\0" */
    static const uint8_t hdr[] = {'K', 'W', 'A', 'J', 0x88, 0xF0, 0x27, 0xD1, 0, 0, 0, 0, 0x1F, 0,
                                  2, 0, 0, 0,  0xAA, 0xBB,  3, 0, 1, 2, 3,  'A', 0,  'B', 0};
    memcpy(f, hdr, sizeof hdr);
    f[10] = (uint8_t)sizeof hdr;
    memcpy(f + sizeof hdr, "ok", 2);
    CHECK_EQ(kwaj_expand(f, sizeof hdr + 2, &out, &out_len), KWAJ_OK);
    CHECK(out_len == 2 && !memcmp(out, "ok", 2));
    free(out);
}

static void test_errors(void) {
    uint8_t f[64], *out = (uint8_t *)1;
    size_t out_len = 99, n;
    CHECK_EQ(kwaj_expand((const uint8_t *)"MZxxxxxxxxxxxxxxxxxx", 20, &out, &out_len), KWAJ_NOT_COMPRESSED);
    n = kwaj_header(f, 0, 1, 10);
    memcpy(f + n, "abc", 3);                       /* promises 10 bytes, holds 3 */
    CHECK_EQ(kwaj_expand(f, n + 3, &out, &out_len), KWAJ_TRUNCATED);
    CHECK_EQ(kwaj_expand(f, 10, &out, &out_len), KWAJ_TRUNCATED);   /* cut inside the header */
    n = kwaj_header(f, 9, 1, 3);
    CHECK_EQ(kwaj_expand(f, n + 3, &out, &out_len), KWAJ_BAD_METHOD);
    n = kwaj_header(f, 4, 1, 3);                   /* MSZIP-in-KWAJ: recognised but not supported */
    CHECK_EQ(kwaj_expand(f, n + 3, &out, &out_len), KWAJ_BAD_METHOD);
    CHECK(out == NULL && out_len == 0);            /* failures leave nothing behind */
}

/* ---- method 3 (LZ + Huffman) ----------------------------------------------------------------------- */

/* Tables: five 4-bit type nibbles + a sixth ignored one, then the tables. Type 0 = flat codes. With these
 * lengths the canonical code of a symbol is its index (MATCHLEN1 4 bits, LITLEN 5, OFFSET 6, LITERAL 8). */
static void flat_tables(bitw *w, bool custom_matchlen2) {
    put_bits(w, 0, 4);                                          /* MATCHLEN1: 16 x 4 bits */
    put_bits(w, custom_matchlen2 ? 3 : 0, 4);                   /* MATCHLEN2 */
    put_bits(w, 0, 4);                                          /* LITLEN: 32 x 5 bits */
    put_bits(w, 0, 4);                                          /* OFFSET: 64 x 6 bits */
    put_bits(w, 0, 4);                                          /* LITERAL: 256 x 8 bits */
    put_bits(w, 0, 4);                                          /* sixth nibble: ignored */
    if (custom_matchlen2) {                                     /* symbol 0 unused, then 1..14 lengths 1..14 */
        unsigned i;
        for (i = 0; i < 16; i++) put_bits(w, i == 0 ? 0 : (i < 15 ? i : 14), 4);
    }
}

static void put_literals(bitw *w, const char *s, unsigned n) {
    unsigned i;
    put_bits(w, 0, 4);                                          /* MATCHLEN1 symbol 0 = literal run */
    put_bits(w, n - 1, 5);                                      /* LITLEN: n-1 */
    for (i = 0; i < n; i++) put_bits(w, (uint8_t)s[i], 8);
}

static size_t lzh_file(uint8_t *f, const bitw *w, uint32_t len) {
    size_t n = kwaj_header(f, 3, 1, len), nb = (w->nbits + 7) / 8;
    memcpy(f + n, w->buf, nb);
    return n + nb;
}

static void test_lzh_literals_and_match(void) {
    bitw w = {{0}, 0};
    uint8_t f[512], *out = NULL;
    size_t out_len = 0, n;
    flat_tables(&w, false);
    put_literals(&w, "ABC", 3);
    put_bits(&w, 4, 4);                                         /* match, MATCHLEN2 table: length sym+2 = 6 */
    put_bits(&w, 0, 6);                                         /* offset high */
    put_bits(&w, 3, 6);                                         /* offset low = 3 */
    n = lzh_file(f, &w, 9);
    CHECK_EQ(kwaj_expand(f, n, &out, &out_len), KWAJ_OK);
    CHECK_EQ(out_len, 9);
    CHECK(out && !memcmp(out, "ABCABCABC", 9));
    free(out);
}

static void test_lzh_initial_window_is_spaces(void) {
    bitw w = {{0}, 0};
    uint8_t f[512], *out = NULL;
    size_t out_len = 0, n;
    flat_tables(&w, false);
    put_literals(&w, "Q", 1);
    put_bits(&w, 1, 4);                                         /* length 3 */
    put_bits(&w, 0, 6);
    put_bits(&w, 2, 6);                                         /* offset 2: before the start of the output */
    n = lzh_file(f, &w, 4);
    CHECK_EQ(kwaj_expand(f, n, &out, &out_len), KWAJ_OK);
    CHECK(out_len == 4 && !memcmp(out, "Q Q ", 4));              /* distance 2 reaches back into the initial spaces, then repeats */
    free(out);
}

static void test_lzh_table_selection(void) {
    /* After a literal run shorter than 32 the next symbol comes from MATCHLEN2 (unary-ish code here); after a
     * match, or after a run of the maximum 32 literals, from MATCHLEN1. */
    bitw w = {{0}, 0};
    uint8_t f[600], *out = NULL, want[64];
    char lit[33];
    size_t out_len = 0, n;
    unsigned i;
    flat_tables(&w, true);
    put_literals(&w, "AB", 2);
    put_bits(&w, 0x0E, 4);                                      /* MATCHLEN2 symbol 4: code 1110, length 6 */
    put_bits(&w, 0, 6);
    put_bits(&w, 2, 6);                                         /* offset 2 -> ABABABAB */
    put_bits(&w, 1, 4);                                         /* after a match: MATCHLEN1 flat, symbol 1, length 3 */
    put_bits(&w, 0, 6);
    put_bits(&w, 1, 6);                                         /* offset 1 -> BBB */
    memcpy(want, "ABABABABBBB", 11);
    for (i = 0; i < 32; i++) lit[i] = (char)('a' + i % 26);
    lit[32] = 0;
    put_literals(&w, lit, 32);                                  /* a maximal run: MATCHLEN1 again */
    put_bits(&w, 2, 4);                                         /* length 4, flat code of symbol 2 */
    put_bits(&w, 0, 6);
    put_bits(&w, 4, 6);                                         /* offset 4 */
    memcpy(want + 11, lit, 32);
    memcpy(want + 43, lit + 28, 4);
    n = lzh_file(f, &w, 47);
    CHECK_EQ(kwaj_expand(f, n, &out, &out_len), KWAJ_OK);
    CHECK_EQ(out_len, 47);
    CHECK(out && !memcmp(out, want, 47));
    free(out);
}

static void test_lzh_rejects_bad_tables(void) {
    bitw w = {{0}, 0};
    uint8_t f[600], *out = NULL;
    size_t out_len = 0, n, i;
    put_bits(&w, 3, 4);                                         /* MATCHLEN1 type 3: explicit lengths */
    for (i = 0; i < 5; i++) put_bits(&w, 0, 4);
    for (i = 0; i < 16; i++) put_bits(&w, 1, 4);                /* sixteen 1-bit codes: over-subscribed */
    n = lzh_file(f, &w, 4);
    CHECK_EQ(kwaj_expand(f, n, &out, &out_len), KWAJ_CORRUPT);
    CHECK(out == NULL);
    memset(f, 0, sizeof f);
    n = kwaj_header(f, 3, 1, 4);                                /* the tables never arrive */
    CHECK_EQ(kwaj_expand(f, n + 2, &out, &out_len), KWAJ_TRUNCATED);
}

/* ---- method 2 and SZDD (LZSS with a 4096-byte window) ----------------------------------------------- */

static void test_lzss(void) {
    uint8_t f[64], *out = NULL;
    size_t out_len = 0, n;
    n = kwaj_header(f, 2, 1, 9);
    /* control byte 0x07: three literals, then a match (bit 0); the window starts as spaces with the write
     * position at 4096-16. Literals a b c land at 4080..4082; the match points at 4080, length 3+3 = 6:
     * b0 = 0xF0, b1 = 0xF3 (high nibble = position bits 8-11, low nibble = length - 3) */
    f[n++] = 0x07;
    f[n++] = 'a';
    f[n++] = 'b';
    f[n++] = 'c';
    f[n++] = 0xF0;
    f[n++] = 0xF3;
    CHECK_EQ(kwaj_expand(f, n, &out, &out_len), KWAJ_OK);
    CHECK_EQ(out_len, 9);
    CHECK(out && !memcmp(out, "abcabcabc", 9));
    free(out);
    out = NULL;

    {   /* SZDD: header 14 bytes, same stream */
        uint8_t s[64];
        static const uint8_t h[14] = {'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33, 'A', 0, 9, 0, 0, 0};
        memcpy(s, h, 14);
        memcpy(s + 14, f + (n - 6), 6);
        CHECK_EQ(kwaj_expand(s, 20, &out, &out_len), KWAJ_OK);
        CHECK(out_len == 9 && !memcmp(out, "abcabcabc", 9));
        free(out);
    }
}

/* ---- the real thing ---------------------------------------------------------------------------- */

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    uint8_t *b;
    long n;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, fp) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(fp);
    *len = (size_t)n;
    return b;
}

static void test_real_cd(void) {
    const char *cd = getenv("CWFA_ES_CD");
    char path[1024];
    size_t in_len, out_len = 0, files = 0;
    uint8_t *in, *out = NULL;
    ne_image ne;
    DIR *d;
    struct dirent *e;
    if (!cd || !*cd) {
        printf("  skip real Spanish CD (CWFA_ES_CD unset)\n");
        return;
    }
    snprintf(path, sizeof path, "%s/WRITER.EX_", cd);
    in = slurp(path, &in_len);
    CHECK(in != NULL);
    if (!in) return;
    CHECK(kwaj_is_compressed(in, in_len));
    CHECK_EQ(kwaj_expand(in, in_len, &out, &out_len), KWAJ_OK);
    CHECK(out_len > 100000 && out[0] == 'M' && out[1] == 'Z');
    CHECK_EQ(ne_open_buffer(&ne, out, out_len), NE_OK);          /* a Win16 program, parsed by our own loader */
    CHECK(ne.seg_count > 10);
    ne_free(&ne);
    free(out);
    free(in);

    /* every compressed file of the CD expands to the length its header promises */
    d = opendir(cd);
    while (d && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        uint32_t want;
        if (n < 2 || e->d_name[n - 1] != '_') continue;
        snprintf(path, sizeof path, "%s/%s", cd, e->d_name);
        in = slurp(path, &in_len);
        if (!in || !kwaj_is_compressed(in, in_len)) {
            free(in);
            continue;
        }
        out = NULL;
        CHECK_EQ(kwaj_expand(in, in_len, &out, &out_len), KWAJ_OK);
        want = (uint32_t)(in[14] | in[15] << 8 | in[16] << 16 | (uint32_t)in[17] << 24);
        CHECK_EQ(out_len, want);
        if (g_fails && files < 3) printf("  (file %s)\n", e->d_name);
        free(out);
        free(in);
        files++;
    }
    if (d) closedir(d);
    CHECK(files > 30);
}

int main(void) {
    test_detect();
    test_method0_and_1();
    test_header_flags();
    test_errors();
    test_lzh_literals_and_match();
    test_lzh_initial_window_is_spaces();
    test_lzh_table_selection();
    test_lzh_rejects_bad_tables();
    test_lzss();
    test_real_cd();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
