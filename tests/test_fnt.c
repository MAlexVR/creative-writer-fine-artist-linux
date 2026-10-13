/* Raster fonts: the FNT format (Windows 2.x layout, 3.x extra header) inside an NE .FON file or on its own. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fnt.h"

static int g_checks, g_fails;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (a), _b = (b); g_checks++; if (_a != _b) { g_fails++; \
    printf("  FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

static void p16(uint8_t *b, size_t o, unsigned v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *b, size_t o, unsigned v) { p16(b, o, v & 0xFFFF); p16(b, o + 2, v >> 16); }

/* A 2.x FNT: chars 'A'..'C' (first 65, last 67), height 4, 'A' 3 wide, 'B' 10 wide (two byte columns), 'C' 2 wide;
 * face "Tiny", 11 points ascent 3 internal leading 1. */
static size_t make_fnt(uint8_t *b, unsigned weight, unsigned height, const char *face) {
    unsigned n = 3, tab = 118, bits = tab + (n + 1) * 4, o, face_off;
    unsigned wa = 3, wb = 10, wc = 2;
    memset(b, 0, 512);
    p16(b, 0, 0x200);
    p16(b, 66, 0);
    p16(b, 68, 11);                      /* points */
    p16(b, 70, 96); p16(b, 72, 96);
    p16(b, 74, 3);                       /* ascent */
    p16(b, 76, 1);                       /* internal leading */
    p16(b, 78, 0);
    p16(b, 83, weight);
    p16(b, 86, 0);                       /* pixel width 0: proportional */
    p16(b, 88, height);
    b[90] = 0x21;
    p16(b, 91, 5); p16(b, 93, 10);
    b[95] = 65; b[96] = 67; b[97] = 0; b[98] = 0;
    p16(b, 99, 0);
    face_off = bits + 64;
    p32(b, 105, face_off);
    p32(b, 113, bits);
    o = bits;
    p16(b, tab + 0, wa); p16(b, tab + 2, o); o += 1 * height;
    p16(b, tab + 4, wb); p16(b, tab + 6, o); o += 2 * height;
    p16(b, tab + 8, wc); p16(b, tab + 10, o); o += 1 * height;
    p16(b, tab + 12, 0); p16(b, tab + 14, o);       /* the extra entry: the default (blank) glyph */
    /* 'A': a diagonal: row r has bit r from the left */
    { unsigned r; for (r = 0; r < height; r++) b[bits + r] = (uint8_t)(0x80 >> r); }
    /* 'B': first column all ones, second column (bits 8..9) only the top row's first pixel */
    { unsigned r; for (r = 0; r < height; r++) b[bits + height + r] = 0xFF; b[bits + 2 * height] = 0x80; }
    /* 'C': solid 2 pixels wide */
    { unsigned r; for (r = 0; r < height; r++) b[bits + 3 * height + r] = 0xC0; }
    memcpy(b + face_off, face, strlen(face) + 1);
    p32(b, 2, face_off + (unsigned)strlen(face) + 1);
    return face_off + strlen(face) + 1;
}

static void test_raw_fnt(void) {
    uint8_t b[512];
    size_t n = make_fnt(b, 700, 4, "Tiny");
    fnt_font f;
    const fnt_strike *s;
    printf("test_fnt_raw\n");
    CHECK(fnt_load_buffer(b, n, &f));
    CHECK_EQ(f.n, 1);
    s = &f.s[0];
    CHECK(!strcmp(s->face, "Tiny"));
    CHECK_EQ(s->height, 4);
    CHECK_EQ(s->ascent, 3);
    CHECK_EQ(s->intlead, 1);
    CHECK_EQ(s->weight, 700);
    CHECK_EQ(s->first, 65);
    CHECK_EQ(s->last, 67);
    CHECK_EQ(fnt_width(s, 'A'), 3);
    CHECK_EQ(fnt_width(s, 'B'), 10);
    CHECK_EQ(fnt_width(s, 'C'), 2);
    CHECK_EQ(fnt_width(s, 'D'), fnt_width(s, 0));      /* outside the range: the default glyph (width 0 here) */
    CHECK(fnt_pixel(s, 'A', 0, 0));
    CHECK(!fnt_pixel(s, 'A', 1, 0));
    CHECK(fnt_pixel(s, 'A', 1, 1));
    CHECK(fnt_pixel(s, 'A', 2, 2));
    CHECK(fnt_pixel(s, 'B', 7, 3));                    /* first byte column */
    CHECK(fnt_pixel(s, 'B', 8, 0));                    /* second byte column, top row */
    CHECK(!fnt_pixel(s, 'B', 8, 1));
    CHECK(!fnt_pixel(s, 'B', 9, 0));
    CHECK(!fnt_pixel(s, 'B', 10, 0));                  /* beyond the glyph */
    CHECK(fnt_pixel(s, 'C', 1, 3) && !fnt_pixel(s, 'C', 2, 3));
    CHECK_EQ(fnt_pixel(s, 'A', -1, 0) | fnt_pixel(s, 'A', 0, 4), 0);
    fnt_free(&f);
    b[0] = 9;                                          /* unknown version */
    CHECK(!fnt_load_buffer(b, n, &f));
    CHECK(!fnt_load_buffer(b, 40, &f));                /* truncated */
}

static void test_pick(void) {
    uint8_t b[512];
    fnt_font f;
    size_t n = make_fnt(b, 400, 4, "Tiny");
    printf("test_fnt_pick\n");
    CHECK(fnt_load_buffer(b, n, &f));
    CHECK(fnt_pick(&f, "tiny", 0, 3) != NULL);         /* case-insensitive; em height 3 = 4 - 1 */
    CHECK(fnt_pick(&f, "Other", 0, 3) == NULL);
    CHECK(fnt_pick(&f, "TINY", 4, 0) == &f.s[0]);      /* by cell height */
    fnt_free(&f);
}

static void test_real_pablo(void) {
    const char *cd = getenv("CWFA_CD_ROOT");
    char path[512];
    fnt_font f;
    const fnt_strike *s;
    printf("test_fnt_pablo\n");
    if (!cd || !*cd) {
        printf("  SKIP (CWFA_CD_ROOT not set)\n");
        return;
    }
    snprintf(path, sizeof path, "%s/SETUP/PABLO.FON", cd);
    CHECK(fnt_load_file(path, &f));
    CHECK_EQ(f.n, 3);                                  /* FONTRES: Pablo 9,11,16 (VGA res) */
    s = fnt_pick(&f, "Pablo Graphos", 0, 11);          /* the programs ask for height -11 */
    CHECK(s != NULL);
    if (s) {
        CHECK_EQ(s->height, 13);
        CHECK_EQ(s->ascent, 11);
        CHECK_EQ(s->intlead, 2);
        CHECK(fnt_width(s, 'i') < fnt_width(s, 'W'));  /* proportional */
        CHECK(fnt_width(s, ' ') > 0);
    }
    CHECK(fnt_pick(&f, "Pablo Graphos", 0, 9) != NULL);
    CHECK(fnt_pick(&f, "Pablo Graphos", 0, 16) != NULL);
    fnt_free(&f);
}

int main(void) {
    test_raw_fnt();
    test_pick();
    test_real_pablo();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
