/* TrueType rasteriser: metrics, 1-bit glyphs and the face lookup of the font set, over the Liberation faces shipped in
 * third_party/fonts (run from the repository root). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ttf.h"

static int g_checks, g_fails;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (a), _b = (b); g_checks++; if (_a != _b) { g_fails++; \
    printf("  FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

#define DIR "third_party/fonts"

static void test_face_and_unicode(void) {
    ttf_face *f;
    printf("test_ttf_face\n");
    CHECK(ttf_face_from_buffer("not a font", 10) == NULL);
    CHECK(ttf_face_from_file(DIR "/missing.ttf") == NULL);
    f = ttf_face_from_file(DIR "/LiberationSans-Regular.ttf");
    CHECK(f != NULL);
    if (f) {
        CHECK(!strcmp(ttf_face_family(f), "Liberation Sans"));
        ttf_face_free(f);
    }
    CHECK_EQ(ttf_unicode('A'), 'A');
    CHECK_EQ(ttf_unicode(0x92), 0x2019);
    CHECK_EQ(ttf_unicode(0x93), 0x201C);
    CHECK_EQ(ttf_unicode(0x80), 0x20AC);
    CHECK_EQ(ttf_unicode(0xE9), 0xE9);
}

static void test_metrics_and_glyphs(void) {
    ttf_set *s = ttf_set_create();
    const ttf_face *sans, *mono;
    ttf_inst *i, *m, *c;
    ttf_glyph g;
    int x, y, ink = 0, bad = 0;
    printf("test_ttf_metrics\n");
    CHECK_EQ(ttf_set_add_dir(s, DIR), 6);
    sans = ttf_set_pick(s, "Arial", 400, 0x22, NULL);
    mono = ttf_set_pick(s, "Courier New", 400, 0x31, NULL);
    CHECK(sans && mono && sans != mono);
    i = ttf_set_inst(s, sans, 0, 16);                        /* em 16 px */
    CHECK(i != NULL);
    if (!i) return;
    CHECK_EQ(ttf_inst_metrics(i)->height, ttf_inst_metrics(i)->ascent + ttf_inst_metrics(i)->descent);
    CHECK_EQ(ttf_inst_metrics(i)->intlead, ttf_inst_metrics(i)->height - 16);
    CHECK(ttf_inst_metrics(i)->ascent > 10 && ttf_inst_metrics(i)->ascent < 20);
    CHECK(ttf_inst_advance(i, 'i') < ttf_inst_advance(i, 'W'));
    CHECK(ttf_inst_advance(i, ' ') > 0);
    CHECK(ttf_inst_metrics(i)->avgw >= 6 && ttf_inst_metrics(i)->avgw <= 10);
    CHECK(ttf_inst_metrics(i)->maxw >= ttf_inst_advance(i, 'W'));
    /* cell sizes: the cell height is what the program asked for */
    c = ttf_set_inst(s, sans, 13, 0);
    CHECK(c != NULL);
    if (c) {
        CHECK_EQ(ttf_inst_metrics(c)->height, 13);
        CHECK(ttf_inst_metrics(c)->intlead >= 1);
    }
    CHECK(ttf_set_inst(s, sans, 0, 16) == i);                /* instances are shared */
    /* 'l' is ink; the bitmap is strictly 0/1 */
    CHECK(ttf_inst_glyph(i, 'l', &g));
    CHECK(g.w > 0 && g.h >= 8 && g.yoff < 0);
    for (y = 0; y < g.h; y++)
        for (x = 0; x < g.w; x++) {
            if (g.bits[y * g.w + x] > 1) bad++;
            ink += g.bits[y * g.w + x];
        }
    CHECK(ink > 0 && !bad);
    CHECK(ttf_inst_glyph(i, ' ', &g) && g.w == 0);
    CHECK(ttf_inst_glyph(i, 0x92, &g) && g.w > 0);           /* the right single quote, from Windows-1252 */
    /* fixed pitch */
    m = ttf_set_inst(s, mono, 0, 16);
    CHECK(m && ttf_inst_advance(m, 'i') == ttf_inst_advance(m, 'W'));
    CHECK(m && ttf_inst_advance(m, 'i') >= 8 && ttf_inst_advance(m, 'i') <= 11);
    ttf_set_free(s);
}

static void test_pick(void) {
    ttf_set *s = ttf_set_create();
    const ttf_face *a, *b, *t, *h, *c;
    bool synth;
    ttf_face *extra;
    printf("test_ttf_pick\n");
    CHECK(ttf_set_pick(s, "Arial", 400, 0x22, NULL) == NULL);        /* nothing registered: the caller keeps its glyphs */
    ttf_set_add_dir(s, DIR);
    a = ttf_set_pick(s, "Arial", 400, 0x22, &synth);
    CHECK(a && !synth);
    CHECK(ttf_set_pick(s, "ARIAL", 400, 0, NULL) == a);
    CHECK(ttf_set_pick(s, "MS Sans Serif", 400, 0, NULL) == a);
    CHECK(ttf_set_pick(s, "Helv", 400, 0, NULL) == a);
    CHECK(ttf_set_pick(s, "System", 400, 0, NULL) == a);
    CHECK(ttf_set_pick(s, "NoSuchFace", 400, 0x22, NULL) == a);      /* swiss default */
    b = ttf_set_pick(s, "Arial", 700, 0x22, &synth);
    CHECK(b && b != a && !synth);
    t = ttf_set_pick(s, "Times New Roman", 400, 0x12, NULL);
    CHECK(t && t != a);
    CHECK(ttf_set_pick(s, "MS Serif", 400, 0, NULL) == t);
    CHECK(ttf_set_pick(s, "Tms Rmn", 400, 0, NULL) == t);
    CHECK(ttf_set_pick(s, "Whatever", 400, 0x12, NULL) == t);        /* FF_ROMAN */
    c = ttf_set_pick(s, "Courier", 400, 0x31, NULL);
    CHECK(c && c != a && c != t);
    CHECK(ttf_set_pick(s, "Terminal", 400, 0, NULL) == c);
    CHECK(ttf_set_pick(s, "Whatever", 400, 0x31, NULL) == c);        /* fixed pitch */
    /* a face the program added wins by name and has no bold style: synthesise */
    extra = ttf_face_from_file(DIR "/LiberationSerif-Regular.ttf");
    CHECK(extra && ttf_set_add_face(s, "Lucida Sans", extra));
    h = ttf_set_pick(s, "lucida sans", 400, 0x22, &synth);
    CHECK(h == extra && !synth);
    h = ttf_set_pick(s, "Lucida Sans", 700, 0x22, &synth);
    CHECK(h == extra && synth);
    ttf_set_free(s);
    /* a directory with no fonts */
    s = ttf_set_create();
    CHECK_EQ(ttf_set_add_dir(s, "build"), 0);
    CHECK(ttf_set_pick(s, "Arial", 400, 0, NULL) == NULL);
    ttf_set_free(s);
}

/* The TrueType files a program's setup installs (the CD's SETUP directory): every *.TTF registers under its own family. */
static void test_installed(void) {
    ttf_set *s = ttf_set_create();
    char dir[96], cmd[700];
    const ttf_face *f;
    const char *cd = getenv("CWFA_CD_ROOT");
    printf("test_ttf_installed\n");
    snprintf(dir, sizeof dir, "build/ttf_inst_%d", (int)getpid());
    snprintf(cmd, sizeof cmd, "mkdir -p %s && cp %s/LiberationMono-Bold.ttf %s/FIXED.TTF && cp %s/LiberationSerif-Bold.ttf %s/readme.txt",
             dir, DIR, dir, DIR, dir);
    CHECK_EQ(system(cmd), 0);
    CHECK_EQ(ttf_set_add_installed(s, dir), 1);                       /* readme.txt is not a font file */
    f = ttf_set_pick(s, "liberation mono", 400, 0, NULL);
    CHECK(f != NULL && !strcmp(ttf_face_family(f), "Liberation Mono"));
    CHECK_EQ(ttf_set_add_installed(s, "build/no_such_dir"), 0);
    snprintf(cmd, sizeof cmd, "rm -r %s", dir);
    CHECK_EQ(system(cmd), 0);
    ttf_set_free(s);
    if (cd) {                                                         /* the CD's own faces */
        char sd[600];
        s = ttf_set_create();
        snprintf(sd, sizeof sd, "%s/SETUP", cd);
        CHECK_EQ(ttf_set_add_installed(s, sd), 6);
        CHECK(ttf_set_pick(s, "Lucida Sans", 400, 0, NULL) != NULL);
        CHECK(ttf_set_pick(s, "McZee", 400, 0, NULL) != NULL);
        CHECK(ttf_set_pick(s, "Impact", 400, 0, NULL) != NULL);
        ttf_set_free(s);
    }
}

/* ink runs (separated horizontal groups) in the rows of a glyph above row yoff_limit (glyph coordinates, negative = above baseline) */
static int runs_above(const ttf_glyph *g, int limit_y) {
    int r, c, best = 0;
    for (r = 0; r < g->h && g->yoff + r < limit_y; r++) {
        int runs = 0, in = 0;
        for (c = 0; c < g->w; c++) {
            int on = g->bits[r * g->w + c];
            if (on && !in) runs++;
            in = on;
        }
        if (runs > best) best = runs;
    }
    return best;
}

/* Accents survive the 1-bit threshold at the sizes the editors use: the two dots of a diaeresis are two ink runs
 * above the letter, at 8..16 px, in every face (the Spanish u-diaeresis lost its dots at 12 px). */
static void test_diaeresis_dots(void) {
    ttf_set *s = ttf_set_create();
    const char *names[] = {"Arial", "Times New Roman", "Courier New"};
    const unsigned char marks[] = {0xFC, 0xF6, 0xE4, 0xEF, 0xEB, 0xFF, 0xDC, 0xD6};   /* u o a i e y U O with diaeresis */
    int n, size, k;
    printf("test_ttf_diaeresis\n");
    ttf_set_add_dir(s, DIR);
    for (n = 0; n < 3; n++) {
        const ttf_face *f = ttf_set_pick(s, names[n], 400, 0x22, NULL);
        for (size = 8; size <= 16; size++) {
            ttf_inst *i = ttf_set_inst(s, f, 0, size);
            for (k = 0; k < (int)sizeof marks; k++) {
                ttf_glyph g, base;
                unsigned char b = marks[k] == 0xFF ? 'y' : marks[k] == 0xFC ? 'u' : marks[k] == 0xF6 ? 'o' : marks[k] == 0xE4 ? 'a' :
                                  marks[k] == 0xEF ? 'i' : marks[k] == 0xEB ? 'e' : marks[k] == 0xDC ? 'U' : 'O';
                CHECK(i && ttf_inst_glyph(i, marks[k], &g) && ttf_inst_glyph(i, b, &base));
                if (!i || g.w == 0) continue;
                if (b == 'i') {                                       /* the plain i has a dot of its own: two dots, not one */
                    CHECK(runs_above(&g, g.yoff + 2) >= 2);           /* the two top rows of the glyph hold two separate dots */
                    continue;
                }
                CHECK(runs_above(&g, base.yoff) >= 2);                /* two dots */
                if (size <= 13) {                                     /* ... one empty row between dots and letter (small sizes) */
                    int q, r0 = 0, gap, ink_row = 0;
                    while (r0 < base.h) {                             /* the first row of the plain letter that has ink */
                        for (q = 0; q < base.w && !base.bits[r0 * base.w + q]; q++) {}
                        if (q < base.w) break;
                        r0++;
                    }
                    gap = base.yoff + r0 - 1 - g.yoff;
                    for (q = 0; q < g.w && gap >= 0 && gap < g.h; q++) ink_row += g.bits[gap * g.w + q];
                    CHECK(gap >= 0 && ink_row == 0);
                }
            }
        }
    }
    ttf_set_free(s);
}

/* Runs of ink in one row of a glyph bitmap. */
static int runs_in_row(const ttf_glyph *g, int row, int *longest) {
    int x, runs = 0, len = 0;
    *longest = 0;
    for (x = 0; x <= g->w; x++) {
        int on = x < g->w && g->bits[row * g->w + x];
        if (on) len++;
        else if (len) {
            runs++;
            if (len > *longest) *longest = len;
            len = 0;
        }
    }
    return runs;
}

/* Hinted rendering (libfreetype, loaded at run time): at 12 px the three stems of a serif m stay three separate pixel
 * columns and a sans l is one pixel wide, where thresholded unhinted outlines merge the stems into a blob. */
static void test_freetype_hinting(void) {
    ttf_set *s = ttf_set_create();
    const ttf_face *serif, *sans;
    ttf_inst *is, *ia, *ist;
    ttf_glyph g;
    int row, best = 0, longest, r;
    printf("test_ttf_freetype_hinting\n");
    CHECK_EQ(ttf_set_add_dir(s, DIR), 6);
    serif = ttf_set_pick(s, "Times New Roman", 400, 0x12, NULL);
    sans = ttf_set_pick(s, "Arial", 400, 0x22, NULL);
    if (!ttf_set_use_freetype(s, true)) {
        if (getenv("CWFA_NO_FREETYPE")) printf("  SKIP (CWFA_NO_FREETYPE set)\n");
        else if (access("/usr/lib64/libfreetype.so.6", R_OK) == 0 || access("/usr/lib/x86_64-linux-gnu/libfreetype.so.6", R_OK) == 0 ||
            access("/usr/lib/libfreetype.so.6", R_OK) == 0)
            CHECK(!"libfreetype is installed but could not be used");
        else
            printf("  SKIP (libfreetype not available)\n");
        ttf_set_free(s);
        return;
    }
    is = ttf_set_inst(s, serif, 0, 12);
    ia = ttf_set_inst(s, sans, 0, 12);
    CHECK(is && ia);
    if (!is || !ia) {
        ttf_set_free(s);
        return;
    }
    CHECK(ttf_inst_glyph(is, 'm', &g) && g.w > 0);
    for (row = 0; row < g.h; row++) {
        r = runs_in_row(&g, row, &longest);
        if (r > best) best = r;
        if (row > g.h / 2 && row < g.h - 1) CHECK(longest <= 2);   /* below the arches only stems: no blob */
    }
    CHECK(best >= 3);                                             /* the three stems are apart */
    CHECK(ttf_inst_glyph(ia, 'l', &g) && g.w >= 1 && g.w <= 2);
    for (row = 0; row < g.h; row++) CHECK_EQ(g.bits[row * g.w] | (g.w > 1 ? g.bits[row * g.w + 1] : 0), 1);
    CHECK(ttf_inst_advance(is, 'm') >= ttf_inst_advance(is, 'n'));
    CHECK(ttf_inst_advance(is, ' ') > 0);
    CHECK(ttf_inst_glyph(is, 0xFC, &g) && g.h > 0);               /* u with diaeresis exists with its two dots */
    ist = ttf_set_inst(s, serif, 0, 12);
    CHECK(ist == is);                                             /* instances stay cached */
    ttf_set_free(s);
}

int main(void) {
    test_face_and_unicode();
    test_metrics_and_glyphs();
    test_pick();
    test_installed();
    test_diaeresis_dots();
    test_freetype_hinting();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
