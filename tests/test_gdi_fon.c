/* Raster fonts in GDI: AddFontResource loads a .FNT/.FON from the host, CreateFont picks the strike by face and size, and
 * metrics, text extents and TextOut use its glyphs (Fine Artist and Creative Writer draw their dialogs in "Pablo Graphos"
 * from PABLO.FON). */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <unistd.h>

#include "gfixture.h"

enum {
    O_TEXTOUT = 33, O_SELECTOBJECT = 45, O_CREATEFONT = 56, O_GETTEXTEXTENT = 91, O_GETTEXTMETRICS = 93,
    O_ADDFONTRESOURCE = 119, O_GETCHARWIDTH = 350, O_DELETEOBJECT = 69
};

static char g_fnt_path[256];

static void p16(uint8_t *b, size_t o, unsigned v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *b, size_t o, unsigned v) { p16(b, o, v & 0xFFFF); p16(b, o + 2, v >> 16); }

/* FNT 2.0, chars 'A'..'C', height 4: 'A' 3 wide (diagonal), 'B' 10 wide (two byte columns), 'C' 2 wide (solid). Face
 * "Tiny", ascent 3, internal leading 1, weight 400. */
static void write_fnt(const char *path) {
    uint8_t b[512];
    unsigned tab = 118, bits = tab + 16, o = bits, face = bits + 64, r, h = 4;
    FILE *f;
    memset(b, 0, sizeof b);
    p16(b, 0, 0x200);
    p16(b, 68, 11); p16(b, 70, 96); p16(b, 72, 96);
    p16(b, 74, 3); p16(b, 76, 1);
    p16(b, 83, 400);
    p16(b, 88, h);
    b[90] = 0x21;
    p16(b, 91, 5); p16(b, 93, 10);
    b[95] = 65; b[96] = 67;
    p32(b, 105, face); p32(b, 113, bits);
    p16(b, tab, 3); p16(b, tab + 2, o); o += h;
    p16(b, tab + 4, 10); p16(b, tab + 6, o); o += 2 * h;
    p16(b, tab + 8, 2); p16(b, tab + 10, o); o += h;
    p16(b, tab + 12, 0); p16(b, tab + 14, o);
    for (r = 0; r < h; r++) b[bits + r] = (uint8_t)(0x80 >> r);                 /* A */
    for (r = 0; r < h; r++) b[bits + h + r] = 0xFF;                              /* B, first column */
    b[bits + 2 * h] = 0x80;                                                      /* B, second column */
    for (r = 0; r < h; r++) b[bits + 3 * h + r] = 0xC0;                          /* C */
    strcpy((char *)b + face, "Tiny");
    p32(b, 2, face + 5);
    f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        fwrite(b, 1, face + 5, f);
        fclose(f);
    }
}

static bool host_path(void *ctx, const char *dos, char *host, size_t cap) {
    (void)ctx;
    if (strcmp(dos, "C:\\FONTS\\TINY.FNT")) return false;
    snprintf(host, cap, "%s", g_fnt_path);
    return true;
}

static uint16_t put_str(const char *s) {
    uint16_t seg = g_seg(0x100);
    strcpy((char *)umem(seg, 0, 1), s);
    return seg;
}

static uint16_t mkfont(int height, int weight, const char *face) {
    uint16_t f = put_str(face);
    return GA(O_CREATEFONT, (uint16_t)height, 0, 0, 0, (uint16_t)weight, 0, 0, 0, 0, 0, 0, 0, 0, FARP(f, 0));
}

static void test_raster_face(void) {
    gdi_config cfg;
    uint16_t dc, f, s, str;
    int x, y;
    printf("test_gdi_fon\n");
    snprintf(g_fnt_path, sizeof g_fnt_path, "build/tiny_%d.fnt", (int)getpid());
    write_fnt(g_fnt_path);
    memset(&cfg, 0, sizeof cfg);
    cfg.host_path = host_path;
    gboot_cfg(&cfg);
    dc = g_screen_dc();
    s = g_seg(0x100);
    CHECK_EQ(GA(O_ADDFONTRESOURCE, FARP(put_str("C:\\FONTS\\TINY.FNT"), 0)), 1);
    f = mkfont(-3, 400, "Tiny");                                  /* em height 3: the strike with height 4, leading 1 */
    CHECK(f != 0);
    GA(O_SELECTOBJECT, dc, f);
    CHECK_EQ(GA(O_GETTEXTMETRICS, dc, FARP(s, 0)), 1);
    CHECK_EQ(urd16(s, 0), 4);                                     /* tmHeight */
    CHECK_EQ(urd16(s, 2), 3);                                     /* tmAscent */
    CHECK_EQ(urd16(s, 4), 1);                                     /* tmDescent */
    CHECK_EQ(urd16(s, 6), 1);                                     /* tmInternalLeading */
    CHECK_EQ(urd16(s, 10), 5);                                    /* tmAveCharWidth */
    CHECK_EQ(urd16(s, 12), 10);                                   /* tmMaxCharWidth */
    CHECK_EQ(urd16(s, 14), 400);
    CHECK_EQ(GA(O_GETCHARWIDTH, dc, 'A', 'C', FARP(s, 0x20)), 1);
    CHECK_EQ(urd16(s, 0x20), 3);
    CHECK_EQ(urd16(s, 0x22), 10);
    CHECK_EQ(urd16(s, 0x24), 2);
    str = put_str("AC");
    CHECK_EQ(GR(O_GETTEXTEXTENT, dc, FARP(str, 0), 2), (4u << 16) | 5u);   /* height 4, width 3 + 2 */
    g_clear(7);                                                   /* the cell is painted opaque, white; the ink is black (0) */
    GA(O_TEXTOUT, dc, 10, 20, FARP(str, 0), 2);
    for (y = 0; y < 4; y++)
        for (x = 0; x < 3; x++) CHECK_EQ(g_px(10 + x, 20 + y) == 0, x == y);        /* A: the diagonal */
    CHECK(g_px(13, 20) == 0 && g_px(14, 23) == 0 && g_px(15, 20) == 7);             /* C starts at x + 3, 2 wide */
    /* the same face asked for bold: the raster font has no bold strike, so it is smeared (+1 advance) */
    GA(O_DELETEOBJECT, f);
    f = mkfont(-3, 700, "Tiny");
    GA(O_SELECTOBJECT, dc, f);
    CHECK_EQ(GR(O_GETTEXTEXTENT, dc, FARP(str, 0), 2), (4u << 16) | 7u);
    /* another face is not affected: it still uses the built-in glyphs */
    GA(O_SELECTOBJECT, dc, mkfont(-12, 400, "Arial"));
    GA(O_GETTEXTMETRICS, dc, FARP(s, 0));
    CHECK(urd16(s, 0) > 8);
    gdown();
    remove(g_fnt_path);
}

int main(void) {
    test_raster_face();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
