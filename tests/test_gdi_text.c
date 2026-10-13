/* Fonts, text output, text metrics and DrawText. At cell height 8
 * with a fixed pitch the built-in font is drawn 1:1, so glyph pixels can be
 * compared with the 5x7 column table typed in below. */
#include "gfixture.h"

enum {
    O_SETBKCOLOR = 1, O_SETBKMODE = 2, O_SETTEXTCHAREXTRA = 8, O_SETTEXTCOLOR = 9, O_SETTEXTJUST = 10,
    O_INTERSECTCLIPRECT = 22, O_TEXTOUT = 33, O_SELECTOBJECT = 45, O_CREATEFONT = 56, O_CREATEFONTINDIRECT = 57,
    O_DELETEOBJECT = 69, O_GETSTOCKOBJECT = 87, O_GETTEXTEXTENT = 91, O_GETTEXTFACE = 92, O_GETTEXTMETRICS = 93,
    O_ADDFONTRESOURCE = 119, O_REMOVEFONTRESOURCE = 136, O_GETRASTERIZERCAPS = 313, O_ENUMFONTFAMILIES = 330,
    O_GETCHARWIDTH = 350, O_EXTTEXTOUT = 351, O_SETTEXTALIGN = 346, O_GETTEXTEXTENTPOINT = 471,
    O_GETOUTLINETEXTMETRICS = 308, O_GETGLYPHOUTLINE = 309, O_GETCURRENTPOSITION = 78, O_MOVETO = 20,
    O_ENUMFONTS = 70, U_DRAWTEXT = 85
};

#define TA_RIGHT 2
#define TA_CENTER 6
#define TA_BOTTOM 8
#define TA_BASELINE 24
#define TA_UPDATECP 1

/* the classic 5x7 table: one byte per column, bit 0 on top, bit 7 the descender row */
static const uint8_t colA[5] = {0x7C, 0x12, 0x11, 0x12, 0x7C};
static const uint8_t colH[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
static const uint8_t colI[5] = {0x00, 0x41, 0x7F, 0x41, 0x00};
static const uint8_t colg[5] = {0x18, 0xA4, 0xA4, 0x9C, 0x78};

static uint16_t mkfont(int height, int weight, int italic, int underline, int strike, int pitchfam, const char *face) {
    uint16_t f = g_seg(0x40);
    strcpy((char *)umem(f, 0, 1), face);
    return GA(O_CREATEFONT, (uint16_t)height, 0, 0, 0, (uint16_t)weight, (uint16_t)italic, (uint16_t)underline,
              (uint16_t)strike, 0, 0, 0, 0, (uint16_t)pitchfam, FARP(f, 0));
}

static uint16_t put_str(const char *s) {
    uint16_t seg = g_seg(0x400);
    strcpy((char *)umem(seg, 0, 1), s);
    return seg;
}

static bool glyph_bit(const uint8_t col[5], int c, int r) { return c >= 0 && c < 5 && ((col[c] >> r) & 1); }

static void text_out(uint16_t dc, int x, int y, const char *s) {
    uint16_t seg = put_str(s);
    GA(O_TEXTOUT, dc, (uint16_t)x, (uint16_t)y, FARP(seg, 0), (uint16_t)strlen(s));
}

static void test_metrics(void) {
    uint16_t dc, s, f;
    printf("test_text_metrics\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x100);
    /* the system font: 8x16 */
    CHECK_EQ(GA(O_GETTEXTMETRICS, dc, FARP(s, 0)), 1);
    CHECK_EQ(urd16(s, 0), 16);                       /* tmHeight */
    CHECK_EQ(urd16(s, 2) + urd16(s, 4), 16);         /* ascent + descent */
    CHECK_EQ(urd16(s, 10), 8);                       /* tmAveCharWidth */
    CHECK(urd16(s, 12) >= 8);                        /* tmMaxCharWidth */
    CHECK_EQ(urd16(s, 14), 700);                     /* tmWeight */
    CHECK_EQ(umem(s, 16, 1)[0], 0);                  /* italic */
    CHECK_EQ(umem(s, 19, 1)[0], 0x20);               /* tmFirstChar */
    CHECK_EQ(umem(s, 20, 1)[0], 0xFF);               /* tmLastChar */
    CHECK_EQ(umem(s, 22, 1)[0], 0x20);               /* tmBreakChar */
    CHECK_EQ(urd16(s, 25), 0);                       /* tmOverhang */
    CHECK_EQ(urd16(s, 27), 96);                      /* tmDigitizedAspectX */
    /* a fixed pitch font has no TMPF_VARIABLE_PITCH, a proportional one has */
    GA(O_SELECTOBJECT, dc, GA(O_GETSTOCKOBJECT, 16));
    GA(O_GETTEXTMETRICS, dc, FARP(s, 0));
    CHECK_EQ(umem(s, 23, 1)[0] & 1, 0);
    GA(O_SELECTOBJECT, dc, mkfont(-20, 400, 0, 0, 0, 0x22, "Arial"));
    GA(O_GETTEXTMETRICS, dc, FARP(s, 0));
    CHECK_EQ(umem(s, 23, 1)[0] & 1, 1);
    CHECK_EQ(umem(s, 23, 1)[0] & 0xF0, 0x20);        /* family from the LOGFONT */
    CHECK_EQ(urd16(s, 0), urd16(s, 2) + urd16(s, 4));
    CHECK(urd16(s, 6) > 0);                          /* a negative height means em height: leading is added */
    CHECK_EQ(urd16(s, 0) - urd16(s, 6), 20);         /* ... so height - internal leading = the requested size */
    /* GetTextFace */
    CHECK_EQ(GA(O_GETTEXTFACE, dc, 32, FARP(s, 0x40)), 5);
    CHECK(strcmp((char *)umem(s, 0x40, 6), "Arial") == 0);
    CHECK_EQ(GA(O_GETTEXTFACE, dc, 3, FARP(s, 0x40)), 2);        /* truncated: room for 2 chars and the NUL */
    CHECK(strcmp((char *)umem(s, 0x40, 3), "Ar") == 0);
    /* widths */
    f = mkfont(16, 400, 0, 0, 0, 0x31, "Courier");
    GA(O_SELECTOBJECT, dc, f);
    GA(O_GETCHARWIDTH, dc, 'A', 'F', FARP(s, 0x80));
    {
        int i;
        for (i = 0; i < 6; i++) CHECK_EQ(urd16(s, 0x80 + 2 * i), 8);
    }
    {
        uint16_t str = put_str("Hello");
        CHECK_EQ(GR(O_GETTEXTEXTENT, dc, FARP(str, 0), 5), 40u | 16u << 16);
        GA(O_SETTEXTCHAREXTRA, dc, 2);
        CHECK_EQ(GR(O_GETTEXTEXTENT, dc, FARP(str, 0), 5), 50u | 16u << 16);
        GA(O_SETTEXTCHAREXTRA, dc, 0);
        CHECK_EQ(GA(O_GETTEXTEXTENTPOINT, dc, FARP(str, 0), 5, FARP(s, 0xA0)), 1);
        CHECK_EQ(urd16(s, 0xA0), 40);
        CHECK_EQ(urd16(s, 0xA2), 16);
        CHECK_EQ(GR(O_GETTEXTEXTENT, dc, FARP(str, 0), 0), 0u | 16u << 16);     /* empty text still has a height */
    }
    /* proportional: narrow and wide letters */
    GA(O_SELECTOBJECT, dc, mkfont(16, 400, 0, 0, 0, 0x22, "Arial"));
    {
        uint16_t i = put_str("i"), w = put_str("W"), sp = put_str(" ");
        uint16_t wi = (uint16_t)GR(O_GETTEXTEXTENT, dc, FARP(i, 0), 1), ww = (uint16_t)GR(O_GETTEXTEXTENT, dc, FARP(w, 0), 1),
                 ws = (uint16_t)GR(O_GETTEXTEXTENT, dc, FARP(sp, 0), 1);
        CHECK(wi < ww);
        CHECK(ws > 0 && ws < ww);
        GA(O_GETCHARWIDTH, dc, 'i', 'i', FARP(s, 0x80));
        CHECK_EQ(urd16(s, 0x80), wi);
    }
    /* bold and the average width */
    GA(O_SELECTOBJECT, dc, mkfont(16, 700, 0, 0, 0, 0x31, "Courier"));
    GA(O_GETTEXTMETRICS, dc, FARP(s, 0));
    CHECK_EQ(urd16(s, 14), 700);
    CHECK_EQ(urd16(s, 25), 1);                       /* synthesized bold overhang */
    {
        uint16_t h = put_str("H");
        CHECK_EQ(GR(O_GETTEXTEXTENT, dc, FARP(h, 0), 1) & 0xFFFF, 9u);
    }
    /* TrueType-only calls and the rasterizer */
    CHECK_EQ(GA(O_GETRASTERIZERCAPS, FARP(s, 0x60), 6), 1);
    CHECK_EQ(urd16(s, 0x60), 6);
    CHECK_EQ(urd16(s, 0x62) & 3, 3);                 /* TT_AVAILABLE | TT_ENABLED */
    CHECK_EQ(GR(O_GETOUTLINETEXTMETRICS, dc, 0, FARP(0, 0)), 0);
    /* GGO_METRICS: no outline is served, but the glyph's black box, origin and advance are (Creative Writer fits its
     * drop capitals with them: without them it falls into a path that ends in "You're running out of space") */
    CHECK_EQ(GR(O_GETGLYPHOUTLINE, dc, 'A', 1, FARP(s, 0), LONG(0), FARP(0, 0), FARP(s, 0x20)), 0xFFFFFFFFu);   /* GGO_BITMAP: GDI_ERROR */
    {
        unsigned bx, by, oy, cell;
        memset(umem(s, 0x80, 12), 0xEE, 12);
        CHECK_EQ(GR(O_GETGLYPHOUTLINE, dc, 'A', 0, FARP(s, 0x80), LONG(0), FARP(0, 0), FARP(s, 0x20)), 0u);
        bx = urd16(s, 0x80);
        by = urd16(s, 0x82);
        oy = urd16(s, 0x86);
        cell = urd16(s, 0x88);
        CHECK(bx >= 6 && bx <= 12);                   /* about a character wide */
        CHECK(by >= 8 && by <= 16);                   /* a capital's height inside the 16 pixel cell */
        CHECK(oy >= by && oy <= 16);                  /* the top of the box above the baseline */
        CHECK_EQ(cell, 9);                            /* the advance: what GetTextExtent said for "H" */
        CHECK_EQ(urd16(s, 0x8A), 0);                  /* gmCellIncY */
        GA(O_SELECTOBJECT, dc, mkfont(-40, 400, 0, 0, 0, 0x22, "Arial"));
        CHECK_EQ(GR(O_GETGLYPHOUTLINE, dc, 'O', 0, FARP(s, 0x80), LONG(0), FARP(0, 0), FARP(s, 0x20)), 0u);
        CHECK(urd16(s, 0x82) > by * 2);               /* a bigger font, a bigger glyph */
        CHECK_EQ(GR(O_GETGLYPHOUTLINE, dc, ' ', 0, FARP(s, 0x80), LONG(0), FARP(0, 0), FARP(s, 0x20)), 0u);
        CHECK_EQ(urd16(s, 0x82), 0);                  /* a blank has no black box */
    }
    gdown();
}

static void test_textout_pixels(void) {
    uint16_t dc;
    int x, y, c, r;
    printf("test_text_pixels\n");
    gboot();
    dc = g_screen_dc();
    GA(O_SELECTOBJECT, dc, mkfont(8, 400, 0, 0, 0, 0x31, "Courier"));
    GA(O_SETBKMODE, dc, 1);                             /* TRANSPARENT */
    g_clear(0xF0);
    text_out(dc, 10, 20, "A");
    for (y = 18; y < 32; y++)
        for (x = 8; x < 20; x++) {
            bool on = x >= 10 && x < 15 && y >= 20 && y < 28 && glyph_bit(colA, x - 10, y - 20);
            CHECK_EQ(g_px(x, y), on ? 0 : 0xF0);
        }
    /* the second character starts one advance (6) further */
    g_clear(0xF0);
    text_out(dc, 10, 20, "AH");
    for (r = 0; r < 8; r++)
        for (c = 0; c < 5; c++) CHECK_EQ(g_px(16 + c, 20 + r), glyph_bit(colH, c, r) ? 0 : 0xF0);
    /* opaque background fills the whole cell with the background colour */
    g_clear(0xF0);
    GA(O_SETBKMODE, dc, 2);
    GR(O_SETBKCOLOR, dc, LONG(0x00FF00));
    GR(O_SETTEXTCOLOR, dc, LONG(0x0000FF));
    text_out(dc, 10, 20, "A");
    for (r = 0; r < 8; r++)
        for (c = 0; c < 6; c++) {
            bool on = c < 5 && glyph_bit(colA, c, r);
            CHECK_EQ(g_px(10 + c, 20 + r), on ? (uint8_t)fb_pixel_of(g_fb(), 0x0000FF) : (uint8_t)fb_pixel_of(g_fb(), 0x00FF00));
        }
    CHECK_EQ(g_px(16, 20), 0xF0);                       /* the cell is 6 wide */
    CHECK_EQ(g_px(10, 28), 0xF0);                       /* and 8 high */
    /* the descender row */
    g_clear(0xF0);
    GA(O_SETBKMODE, dc, 1);
    GR(O_SETTEXTCOLOR, dc, LONG(0));
    text_out(dc, 10, 20, "g");
    for (r = 0; r < 8; r++)
        for (c = 0; c < 5; c++) CHECK_EQ(g_px(10 + c, 20 + r), glyph_bit(colg, c, r) ? 0 : 0xF0);
    /* proportional at 8 pixels: the left and right bearings are trimmed; 'I' uses source columns 1..3 */
    g_clear(0xF0);
    GA(O_SELECTOBJECT, dc, mkfont(8, 400, 0, 0, 0, 0x22, "Arial"));
    text_out(dc, 10, 20, "I");
    for (r = 0; r < 8; r++)
        for (c = 0; c < 3; c++) CHECK_EQ(g_px(10 + c, 20 + r), glyph_bit(colI, c + 1, r) ? 0 : 0xF0);
    CHECK_EQ(g_px(13, 24), 0xF0);
    /* a DC clip applies to text */
    g_clear(0xF0);
    GA(O_SELECTOBJECT, dc, mkfont(8, 400, 0, 0, 0, 0x31, "Courier"));
    GA(O_INTERSECTCLIPRECT, dc, 12, 22, 14, 26);
    text_out(dc, 10, 20, "H");
    for (y = 18; y < 30; y++)
        for (x = 8; x < 18; x++) {
            bool in = x >= 12 && x < 14 && y >= 22 && y < 26;
            bool on = x >= 10 && x < 15 && y >= 20 && y < 28 && glyph_bit(colH, x - 10, y - 20);
            CHECK_EQ(g_px(x, y), in && on ? 0 : 0xF0);
        }
    gdown();
}

static void test_alignment(void) {
    uint16_t dc, s;
    printf("test_text_alignment\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x100);
    GA(O_SELECTOBJECT, dc, mkfont(8, 400, 0, 0, 0, 0x31, "Courier"));
    GA(O_SETBKMODE, dc, 1);
    /* "AA" is 12 wide and 8 high; reference point (100, 100) */
    GA(O_SETTEXTALIGN, dc, 0);
    g_clear(0xF0);
    text_out(dc, 100, 100, "AA");
    CHECK_EQ(g_px(100 + 0, 100 + 0), 0xF0);                       /* column 0 of 'A' has bit 2 set from row 2 */
    CHECK_EQ(g_px(100, 102), 0);
    GA(O_SETTEXTALIGN, dc, TA_RIGHT);
    g_clear(0xF0);
    text_out(dc, 100, 100, "AA");
    CHECK_EQ(g_px(88, 102), 0);                                   /* starts at x - 12 */
    CHECK_EQ(g_px(87, 102), 0xF0);
    GA(O_SETTEXTALIGN, dc, TA_CENTER);
    g_clear(0xF0);
    text_out(dc, 100, 100, "AA");
    CHECK_EQ(g_px(94, 102), 0);                                   /* starts at x - 6 */
    CHECK_EQ(g_px(93, 102), 0xF0);
    GA(O_SETTEXTALIGN, dc, TA_BOTTOM);
    g_clear(0xF0);
    text_out(dc, 100, 100, "AA");
    CHECK_EQ(g_px(100, 94), 0);                                   /* row 2 of a cell that ends at y */
    CHECK_EQ(g_px(100, 100), 0xF0);
    GA(O_SETTEXTALIGN, dc, TA_BASELINE);
    g_clear(0xF0);
    text_out(dc, 100, 100, "AA");
    CHECK_EQ(g_px(100, 95), 0);                                   /* ascent is 7: top at y - 7, row 2 at y - 5 */
    /* TA_UPDATECP: the reference point is the current position, advanced by the text width */
    GA(O_SETTEXTALIGN, dc, TA_UPDATECP);
    g_clear(0xF0);
    GR(O_MOVETO, dc, 50, 60);
    text_out(dc, 0, 0, "AA");
    CHECK_EQ(GR(O_GETCURRENTPOSITION, dc), 62u | 60u << 16);
    CHECK_EQ(g_px(50, 62), 0);
    text_out(dc, 0, 0, "A");
    CHECK_EQ(g_px(62, 62), 0);
    CHECK_EQ(GR(O_GETCURRENTPOSITION, dc), 68u | 60u << 16);
    (void)s;
    gdown();
}

static void test_styles_and_exttextout(void) {
    uint16_t dc, s, str;
    int x, y;
    printf("test_text_styles_exttextout\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x100);
    GA(O_SETBKMODE, dc, 1);
    /* bold: the glyph drawn at x and x+1 */
    GA(O_SELECTOBJECT, dc, mkfont(8, 700, 0, 0, 0, 0x31, "Courier"));
    g_clear(0xF0);
    text_out(dc, 10, 20, "H");
    for (y = 20; y < 28; y++)
        for (x = 10; x < 17; x++) {
            bool on = glyph_bit(colH, x - 10, y - 20) || glyph_bit(colH, x - 11, y - 20);
            CHECK_EQ(g_px(x, y), on ? 0 : 0xF0);
        }
    /* italic: the top rows are sheared to the right */
    GA(O_SELECTOBJECT, dc, mkfont(8, 400, 1, 0, 0, 0x31, "Courier"));
    g_clear(0xF0);
    text_out(dc, 10, 20, "H");
    {
        int top_left = -1, bottom_left = -1;
        for (x = 0; x < 12; x++) {
            if (top_left < 0 && g_px(10 + x, 20) == 0) top_left = x;
            if (bottom_left < 0 && g_px(10 + x, 26) == 0) bottom_left = x;
        }
        CHECK(top_left > bottom_left && bottom_left == 0);
    }
    /* underline and strikeout draw a line across the cell */
    GA(O_SELECTOBJECT, dc, mkfont(16, 400, 0, 1, 0, 0x31, "Courier"));
    g_clear(0xF0);
    text_out(dc, 10, 20, "I");
    {
        int below = 0, count = 0;
        for (y = 20; y < 40; y++) {
            int row = 0;
            for (x = 10; x < 18; x++) row += g_px(x, y) == 0;
            if (row >= 8) {
                below = y;
                count++;
            }
        }
        CHECK(count >= 1 && count <= 2);
        CHECK(below > 20 + 12);                      /* under the baseline */
    }
    GA(O_SELECTOBJECT, dc, mkfont(16, 400, 0, 0, 1, 0x31, "Courier"));
    g_clear(0xF0);
    text_out(dc, 10, 20, "I");
    {
        int found = 0;
        for (y = 20; y < 36; y++) {
            int row = 0;
            for (x = 10; x < 18; x++) row += g_px(x, y) == 0;
            if (row >= 8) found = y;
        }
        CHECK(found > 20 + 3 && found < 20 + 12);
    }
    /* ExtTextOut: ETO_OPAQUE fills the rectangle, ETO_CLIPPED clips to it, lpDx sets each advance */
    GA(O_SELECTOBJECT, dc, mkfont(8, 400, 0, 0, 0, 0x31, "Courier"));
    GR(O_SETBKCOLOR, dc, LONG(0x00FF00));
    str = put_str("HH");
    uwr16(s, 0, 100); uwr16(s, 2, 100); uwr16(s, 4, 130); uwr16(s, 6, 112);
    g_clear(0xF0);
    GA(O_SETBKMODE, dc, 1);
    GA(O_EXTTEXTOUT, dc, 102, 102, 2, FARP(s, 0), FARP(str, 0), 2, FARP(0, 0));
    CHECK_EQ(g_px(101, 101), (uint8_t)fb_pixel_of(g_fb(), 0x00FF00));          /* rectangle painted with the background colour */
    CHECK_EQ(g_px(129, 111), (uint8_t)fb_pixel_of(g_fb(), 0x00FF00));
    CHECK_EQ(g_px(130, 111), 0xF0);
    CHECK_EQ(g_px(102, 102), 0);                                                  /* text on top */
    g_clear(0xF0);
    GA(O_EXTTEXTOUT, dc, 102, 102, 4, FARP(s, 0), FARP(str, 0), 2, FARP(0, 0));
    CHECK_EQ(g_px(101, 101), 0xF0);
    CHECK_EQ(g_px(102, 102), 0);
    uwr16(s, 6, 105);                                                             /* clip bottom at 105 */
    g_clear(0xF0);
    GA(O_EXTTEXTOUT, dc, 102, 102, 4, FARP(s, 0), FARP(str, 0), 2, FARP(0, 0));
    CHECK_EQ(g_px(102, 104), 0);
    CHECK_EQ(g_px(102, 108), 0xF0);
    uwr16(s, 0x20, 20); uwr16(s, 0x22, 3);                                       /* advances: 20 then 3 */
    g_clear(0xF0);
    GA(O_EXTTEXTOUT, dc, 102, 102, 0, FARP(0, 0), FARP(str, 0), 2, FARP(s, 0x20));
    CHECK_EQ(g_px(102, 104), 0);
    CHECK_EQ(g_px(122, 104), 0);                                                  /* second glyph at +20 */
    CHECK_EQ(g_px(114, 104), 0xF0);                                               /* none at the default advance */
    /* justification: extra pixels shared by the spaces of the string */
    {
        uint16_t sp = put_str("A A");
        GA(O_SETTEXTJUST, dc, 6, 1);
        CHECK_EQ(GR(O_GETTEXTEXTENT, dc, FARP(sp, 0), 3) & 0xFFFF, 18u + 6u);
        GA(O_SETTEXTJUST, dc, 0, 0);
        CHECK_EQ(GR(O_GETTEXTEXTENT, dc, FARP(sp, 0), 3) & 0xFFFF, 18u);
    }
    gdown();
}

typedef struct { int l, t, r, b; } irect;

static irect get_rc(uint16_t s, uint32_t off) {
    irect r = {(int16_t)urd16(s, off), (int16_t)urd16(s, off + 2), (int16_t)urd16(s, off + 4), (int16_t)urd16(s, off + 6)};
    return r;
}
static void set_rc(uint16_t s, uint32_t off, int l, int t, int r, int b) {
    uwr16(s, off, (uint16_t)l); uwr16(s, off + 2, (uint16_t)t); uwr16(s, off + 4, (uint16_t)r); uwr16(s, off + 6, (uint16_t)b);
}

#define DT_CENTER 1
#define DT_RIGHT 2
#define DT_VCENTER 4
#define DT_BOTTOM 8
#define DT_WORDBREAK 0x10
#define DT_SINGLELINE 0x20
#define DT_EXPANDTABS 0x40
#define DT_NOCLIP 0x100
#define DT_CALCRECT 0x400
#define DT_NOPREFIX 0x800

static int draw_text(uint16_t dc, const char *text, uint16_t rcseg, int fmt) {
    uint16_t str = put_str(text);
    return (int16_t)UR(U_DRAWTEXT, dc, FARP(str, 0), (uint16_t)-1, FARP(rcseg, 0), (uint16_t)fmt);
}

static void test_drawtext(void) {
    uint16_t dc, s;
    irect r;
    int x, y, bad;
    printf("test_text_drawtext\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x100);
    GA(O_SELECTOBJECT, dc, mkfont(16, 400, 0, 0, 0, 0x31, "Courier"));     /* 8 x 16 cells */
    GA(O_SETBKMODE, dc, 1);
    /* DT_CALCRECT: the rectangle shrinks around one line; nothing is drawn */
    g_clear(0xF0);
    set_rc(s, 0, 10, 20, 300, 400);
    CHECK_EQ(draw_text(dc, "Hello", s, DT_CALCRECT | DT_SINGLELINE), 16);
    r = get_rc(s, 0);
    CHECK_EQ(r.l, 10);
    CHECK_EQ(r.t, 20);
    CHECK_EQ(r.r, 50);
    CHECK_EQ(r.b, 36);
    for (y = 0; y < 480; y += 7)
        for (x = 0; x < 640; x += 5) CHECK_EQ(g_px(x, y), 0xF0);
    /* line breaks without wrapping: the rectangle gets the widest line and all lines */
    set_rc(s, 0, 10, 20, 300, 400);
    CHECK_EQ(draw_text(dc, "ab\ncdef\ng", s, DT_CALCRECT), 48);
    r = get_rc(s, 0);
    CHECK_EQ(r.r, 10 + 32);
    CHECK_EQ(r.b, 20 + 48);
    /* DT_WORDBREAK: the width stays, the bottom follows the lines */
    set_rc(s, 0, 10, 20, 10 + 40, 400);
    CHECK_EQ(draw_text(dc, "hello big world", s, DT_CALCRECT | DT_WORDBREAK), 48);   /* hello / big / world */
    r = get_rc(s, 0);
    CHECK_EQ(r.r, 50);
    CHECK_EQ(r.b, 20 + 48);
    set_rc(s, 0, 10, 20, 10 + 88, 400);
    CHECK_EQ(draw_text(dc, "hello big world", s, DT_CALCRECT | DT_WORDBREAK), 32);   /* "hello big" / "world" */
    /* tabs and prefixes in the measured width */
    set_rc(s, 0, 0, 0, 500, 400);
    draw_text(dc, "a\tb", s, DT_CALCRECT | DT_SINGLELINE | DT_EXPANDTABS);
    CHECK_EQ(get_rc(s, 0).r, 8 * 8 + 8);                                              /* tab stop every 8 characters */
    set_rc(s, 0, 0, 0, 500, 400);
    draw_text(dc, "&File", s, DT_CALCRECT | DT_SINGLELINE);
    CHECK_EQ(get_rc(s, 0).r, 32);                                                     /* the '&' is not measured */
    set_rc(s, 0, 0, 0, 500, 400);
    draw_text(dc, "&File", s, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    CHECK_EQ(get_rc(s, 0).r, 40);
    set_rc(s, 0, 0, 0, 500, 400);
    draw_text(dc, "A&&B", s, DT_CALCRECT | DT_SINGLELINE);
    CHECK_EQ(get_rc(s, 0).r, 24);                                                     /* && is one '&' */
    /* drawing: horizontal alignment equals a TextOut at the computed position */
    {
        int fmt[3] = {0, DT_CENTER, DT_RIGHT}, k;
        for (k = 0; k < 3; k++) {
            static uint8_t ref[640 * 480];
            int tx = k == 0 ? 100 : k == 1 ? 100 + (200 - 24) / 2 : 100 + 200 - 24;
            g_clear(0xF0);
            set_rc(s, 0, 100, 100, 300, 130);
            draw_text(dc, "Abc", s, fmt[k] | DT_SINGLELINE);
            memcpy(ref, g_fb()->p8, sizeof ref);
            g_clear(0xF0);
            text_out(dc, tx, 100, "Abc");
            bad = memcmp(ref, g_fb()->p8, sizeof ref) != 0;
            CHECK_EQ(bad, 0);
        }
    }
    /* vertical alignment of a single line */
    {
        static uint8_t ref[640 * 480];
        g_clear(0xF0);
        set_rc(s, 0, 100, 100, 300, 160);
        draw_text(dc, "Abc", s, DT_SINGLELINE | DT_VCENTER);
        memcpy(ref, g_fb()->p8, sizeof ref);
        g_clear(0xF0);
        text_out(dc, 100, 100 + (60 - 16) / 2, "Abc");
        CHECK_EQ(memcmp(ref, g_fb()->p8, sizeof ref), 0);
        g_clear(0xF0);
        draw_text(dc, "Abc", s, DT_SINGLELINE | DT_BOTTOM);
        memcpy(ref, g_fb()->p8, sizeof ref);
        g_clear(0xF0);
        text_out(dc, 100, 160 - 16, "Abc");
        CHECK_EQ(memcmp(ref, g_fb()->p8, sizeof ref), 0);
    }
    /* clipped to the rectangle unless DT_NOCLIP */
    g_clear(0xF0);
    set_rc(s, 0, 100, 100, 110, 108);
    draw_text(dc, "HHHH", s, DT_SINGLELINE);
    bad = 0;
    for (y = 90; y < 130; y++)
        for (x = 90; x < 160; x++)
            if (g_px(x, y) != 0xF0 && !(x >= 100 && x < 110 && y >= 100 && y < 108)) bad++;
    CHECK_EQ(bad, 0);
    g_clear(0xF0);
    draw_text(dc, "HHHH", s, DT_SINGLELINE | DT_NOCLIP);
    CHECK(g_px(125, 108) != 0xF0 || g_px(125, 110) != 0xF0 || g_px(125, 112) != 0xF0);
    /* the prefix underline is drawn under the character after '&' */
    g_clear(0xF0);
    set_rc(s, 0, 100, 100, 300, 130);
    draw_text(dc, "&A", s, DT_SINGLELINE);
    {
        int ul = 0;
        for (y = 100; y < 120; y++) {
            int row = 0;
            for (x = 100; x < 108; x++) row += g_px(x, y) == 0;
            if (row == 8) ul = y;
        }
        CHECK(ul > 100 + 12);
    }
    /* an empty rectangle and a bad string pointer */
    set_rc(s, 0, 5, 5, 5, 5);
    CHECK_EQ(draw_text(dc, "x", s, DT_SINGLELINE), 16);
    gdown();
}

/* A guest EnumFontFamProc: records the first character of each face and the font type. */
static void install_enum_proc(void) {
    static const uint8_t code[] = {
        0x55, 0x89, 0xE5,                         /* push bp; mov bp,sp */
        0x8B, 0x36, 0x60, 0x00,                   /* mov si,[60h] */
        0xD1, 0xE6,                               /* shl si,1 */
        0xC4, 0x5E, 0x10,                         /* les bx,[bp+16]: lplf */
        0x26, 0x8A, 0x47, 0x12,                   /* mov al,es:[bx+18]: first char of the face */
        0x32, 0xE4,                               /* xor ah,ah */
        0x89, 0x84, 0x00, 0x01,                   /* mov [si+100h],ax */
        0x8B, 0x46, 0x0A,                         /* mov ax,[bp+10]: font type */
        0x89, 0x84, 0x00, 0x02,                   /* mov [si+200h],ax */
        0xFF, 0x06, 0x60, 0x00,                   /* inc word [60h] */
        0xB8, 0x01, 0x00,                         /* mov ax,1 */
        0x5D, 0xCA, 0x0E, 0x00};                  /* pop bp; retf 14 */
    u_put(0x800, code, sizeof code);
}

static void test_enum_and_resources(void) {
    uint16_t dc, s, n;
    printf("test_text_enum_fonts\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x100);
    install_enum_proc();
    uwr16(u_ds(), 0x60, 0);
    CHECK_EQ(GA(O_ENUMFONTFAMILIES, dc, FARP(0, 0), FARP(u_cs(), 0x800), LONG(0x1234)), 1);
    n = urd16(u_ds(), 0x60);
    CHECK(n >= 5);                                   /* the built-in families */
    {
        int i, saw_system = 0, saw_arial = 0, saw_courier = 0;
        for (i = 0; i < n; i++) {
            uint16_t ch = urd16(u_ds(), 0x100 + 2 * (unsigned)i);
            saw_system += ch == 'S';
            saw_arial += ch == 'A';
            saw_courier += ch == 'C';
        }
        CHECK(saw_system && saw_arial && saw_courier);
    }
    /* fonts added from files show up and can be removed again */
    {
        uint16_t f = put_str("D:\\SETUP\\IMPACT.TTF");
        CHECK_EQ(GA(O_ADDFONTRESOURCE, FARP(f, 0)), 1);
        uwr16(u_ds(), 0x60, 0);
        GA(O_ENUMFONTFAMILIES, dc, FARP(0, 0), FARP(u_cs(), 0x800), LONG(0));
        {
            uint16_t after = urd16(u_ds(), 0x60);
            int i, saw = 0;
            CHECK(after == n + 1);
            for (i = 0; i < after; i++) saw += urd16(u_ds(), 0x100 + 2 * (unsigned)i) == 'I';
            CHECK(saw >= 1);
        }
        CHECK_EQ(GA(O_REMOVEFONTRESOURCE, FARP(f, 0)), 1);
        uwr16(u_ds(), 0x60, 0);
        GA(O_ENUMFONTFAMILIES, dc, FARP(0, 0), FARP(u_cs(), 0x800), LONG(0));
        CHECK_EQ(urd16(u_ds(), 0x60), n);
    }
    /* one face name: only the sizes of that family */
    {
        uint16_t face = put_str("Courier");
        uwr16(u_ds(), 0x60, 0);
        GA(O_ENUMFONTFAMILIES, dc, FARP(face, 0), FARP(u_cs(), 0x800), LONG(0));
        CHECK(urd16(u_ds(), 0x60) >= 1);
        CHECK_EQ(urd16(u_ds(), 0x100), 'C');
    }
    (void)s;
    gdown();
}

/* WRITER's own error dialog says "can\x92t" and quotes a path with \x93 \x94: Windows-1252 punctuation in 0x80-0x9F
 * must draw as the nearest ASCII mark, not as a gap. */
static void test_ansi_punctuation(void) {
    static const struct { const char *typo, *plain; } pairs[] = {
        {"\x91", "'"}, {"\x92", "'"}, {"\x93", "\""}, {"\x94", "\""}, {"\x96", "-"}, {"\x97", "-"}, {"\x85", "."},
        {"\xA0", " "}, {"\x95", "*"}};
    size_t i;
    int x, y;
    bool ink = false;
    printf("test_text_ansi_punctuation\n");
    gboot();
    for (i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        uint16_t dc = g_screen_dc();
        uint8_t a[16 * 16], b[16 * 16];
        g_clear(0);
        text_out(dc, 0, 0, pairs[i].plain);
        for (y = 0; y < 16; y++) for (x = 0; x < 16; x++) a[y * 16 + x] = g_px(x, y);
        g_clear(0);
        text_out(dc, 0, 0, pairs[i].typo);
        for (y = 0; y < 16; y++) for (x = 0; x < 16; x++) b[y * 16 + x] = g_px(x, y);
        CHECK(!memcmp(a, b, sizeof a));
        if (i == 1) {
            uint8_t blank[16 * 16];
            g_clear(0);
            text_out(dc, 0, 0, " ");
            for (y = 0; y < 16; y++) for (x = 0; x < 16; x++) blank[y * 16 + x] = g_px(x, y);
            ink = memcmp(blank, b, sizeof b) != 0;
        }
        UA(68, 0, dc);
    }
    CHECK(ink);                                      /* the apostrophe leaves ink */
    gdown();
}

int main(void) {
    test_ansi_punctuation();
    test_metrics();
    test_textout_pixels();
    test_alignment();
    test_styles_and_exttextout();
    test_drawtext();
    test_enum_and_resources();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
