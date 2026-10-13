/* Bitmaps, memory DCs, ROP3, BitBlt/StretchBlt/PatBlt, pattern brushes and
 * ScrollDC. Expected values come from the ROP3 truth table
 * definition and from per-pixel references, not from the code under test. */
#include "gdi_priv.h"
#include "gfixture.h"

#define SRCCOPY 0x00CC0020u
#define SRCPAINT 0x00EE0086u
#define SRCAND 0x008800C6u
#define SRCINVERT 0x00660046u
#define SRCERASE 0x00440328u
#define NOTSRCCOPY 0x00330008u
#define NOTSRCERASE 0x001100A6u
#define MERGECOPY 0x00C000CAu
#define MERGEPAINT 0x00BB0226u
#define PATCOPY 0x00F00021u
#define PATPAINT 0x00FB0A09u
#define PATINVERT 0x005A0049u
#define DSTINVERT 0x00550009u
#define BLACKNESS 0x00000042u
#define WHITENESS 0x00FF0062u

enum {
    O_SETBKCOLOR = 1, O_SETSTRETCHBLTMODE = 7, O_SETTEXTCOLOR = 9, O_SETWINDOWORG = 11, O_SETWINDOWEXT = 12, O_SETMAPMODE = 3,
    O_SETVIEWPORTEXT = 14, O_RECTANGLE = 27, O_PATBLT = 29, O_BITBLT = 34, O_STRETCHBLT = 35, O_SETBITMAPBITS = 46,
    O_CREATEBITMAP = 48, O_CREATEBITMAPINDIRECT = 49, O_CREATECOMPATIBLEBITMAP = 51, O_CREATECOMPATIBLEDC = 52,
    O_CREATEPATTERNBRUSH = 60, O_CREATESOLIDBRUSH = 66, O_DELETEDC = 68, O_DELETEOBJECT = 69, O_GETBITMAPBITS = 74,
    O_GETOBJECT = 82, O_GETPIXEL = 83, O_SELECTOBJECT = 45, O_GETSTOCKOBJECT = 87, O_CREATEDISCARDABLEBITMAP = 156,
    O_GETDCORG = 79, O_CREATERECTRGN = 64, U_SCROLLDC = 221, O_CREATEBRUSHINDIRECT = 50, O_INTERSECTCLIPRECT = 22,
    O_SELECTCLIPRGN = 44, U_FILLRECT = 81, O_SETVIEWPORTORG = 13, O_GETRGNBOX = 134
};

/* reference truth table: bit (P<<2 | S<<1 | D) of the ROP byte, per pixel bit */
static uint8_t ref_rop(unsigned rop, uint8_t p, uint8_t s, uint8_t d) {
    uint8_t r = 0;
    int b;
    for (b = 0; b < 8; b++) {
        int idx = ((p >> b & 1) << 2) | ((s >> b & 1) << 1) | (d >> b & 1);
        r |= (uint8_t)(((rop >> idx) & 1) << b);
    }
    return r;
}

static void test_rop3_evaluator(void) {
    unsigned rop;
    int i;
    uint8_t vals[] = {0x00, 0xFF, 0xAA, 0x55, 0xF0, 0x0F, 0xCC, 0x33, 0x12, 0xED, 0x80, 0x01};
    printf("test_blt_rop3_evaluator\n");
    for (rop = 0; rop < 256; rop++)
        for (i = 0; i < 12 * 12 * 12; i++) {
            uint8_t p = vals[i % 12], s = vals[i / 12 % 12], d = vals[i / 144];
            CHECK_EQ(rop3_eval((uint8_t)rop, p, s, d), ref_rop(rop, p, s, d));
        }
    /* the named operations against their Boolean definitions */
    {
        uint8_t p = 0xB5, s = 0x6C, d = 0x39;
        CHECK_EQ(rop3_eval(0xCC, p, s, d), s);
        CHECK_EQ(rop3_eval(0xEE, p, s, d), (uint8_t)(s | d));
        CHECK_EQ(rop3_eval(0x88, p, s, d), (uint8_t)(s & d));
        CHECK_EQ(rop3_eval(0x66, p, s, d), (uint8_t)(s ^ d));
        CHECK_EQ(rop3_eval(0x44, p, s, d), (uint8_t)(s & ~d));
        CHECK_EQ(rop3_eval(0x33, p, s, d), (uint8_t)~s);
        CHECK_EQ(rop3_eval(0x11, p, s, d), (uint8_t)~(s | d));
        CHECK_EQ(rop3_eval(0xC0, p, s, d), (uint8_t)(p & s));
        CHECK_EQ(rop3_eval(0xBB, p, s, d), (uint8_t)(~s | d));
        CHECK_EQ(rop3_eval(0xF0, p, s, d), p);
        CHECK_EQ(rop3_eval(0xFB, p, s, d), (uint8_t)(d | p | ~s));
        CHECK_EQ(rop3_eval(0x5A, p, s, d), (uint8_t)(p ^ d));
        CHECK_EQ(rop3_eval(0x55, p, s, d), (uint8_t)~d);
        CHECK_EQ(rop3_eval(0x00, p, s, d), 0);
        CHECK_EQ(rop3_eval(0xFF, p, s, d), 0xFF);
    }
    /* which operands an operation reads */
    CHECK(rop3_uses_src(0xCC) && !rop3_uses_src(0xF0) && !rop3_uses_src(0x55) && !rop3_uses_src(0x00));
    CHECK(rop3_uses_src(0xC0) && rop3_uses_pat(0xC0) && rop3_uses_pat(0x5A) && !rop3_uses_pat(0xCC));
    CHECK(rop3_uses_dst(0x66) && !rop3_uses_dst(0xCC) && !rop3_uses_dst(0xF0));
}

static uint32_t cref_of(uint8_t pixel) {
    uint32_t c = g_fb()->pal[pixel];
    return (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
}

static uint16_t compat_bmp(uint16_t dc, int w, int h) { return GA(O_CREATECOMPATIBLEBITMAP, dc, (uint16_t)w, (uint16_t)h); }

static void set_bits(uint16_t bmp, const uint8_t *bytes, uint32_t n) {
    uint16_t s = g_seg(n + 16);
    memcpy(umem(s, 0, n), bytes, n);
    CHECK_EQ(GR(O_SETBITMAPBITS, bmp, LONG(n), FARP(s, 0)), n);
}

static void test_bitmaps(void) {
    uint16_t dc, s, bm, mdc;
    printf("test_blt_bitmaps\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x200);
    /* monochrome: 10x3, each row padded to 16 bits, most significant bit first */
    {
        static const uint8_t rows[6] = {0x81, 0x40, 0xFF, 0xC0, 0x00, 0x00};
        int i;
        for (i = 0; i < 6; i++) umem(s, (uint32_t)i, 1)[0] = rows[i];
    }
    bm = GA(O_CREATEBITMAP, 10, 3, 1, 1, FARP(s, 0));
    CHECK(bm != 0);
    CHECK_EQ(GR(O_GETOBJECT, bm, 14, FARP(s, 0x20)), 14);
    CHECK_EQ(urd16(s, 0x20 + 2), 10);                         /* bmWidth */
    CHECK_EQ(urd16(s, 0x20 + 4), 3);                          /* bmHeight */
    CHECK_EQ(urd16(s, 0x20 + 6), 2);                          /* bmWidthBytes */
    CHECK_EQ(umem(s, 0x20 + 8, 1)[0], 1);                     /* planes */
    CHECK_EQ(umem(s, 0x20 + 9, 1)[0], 1);                     /* bits per pixel */
    CHECK_EQ(GR(O_GETBITMAPBITS, bm, LONG(6), FARP(s, 0x40)), 6);
    CHECK_EQ(umem(s, 0x40, 1)[0], 0x81);
    CHECK_EQ(umem(s, 0x41, 1)[0], 0x40);
    CHECK_EQ(umem(s, 0x42, 1)[0], 0xFF);
    CHECK_EQ(umem(s, 0x43, 1)[0], 0xC0);
    CHECK_EQ(GR(O_GETBITMAPBITS, bm, LONG(4), FARP(s, 0x50)), 4);   /* a smaller count copies less */
    CHECK_EQ(GR(O_GETBITMAPBITS, bm, LONG(100), FARP(s, 0x60)), 6);  /* a larger one is clipped to the bitmap */
    /* selected into a memory DC it reads back as pixels: 1 is white, 0 black */
    mdc = GA(O_CREATECOMPATIBLEDC, dc);
    CHECK(GA(O_SELECTOBJECT, mdc, bm) != 0);                   /* the previous bitmap: the 1x1 default */
    CHECK_EQ(GR(O_GETPIXEL, mdc, 0, 0), 0xFFFFFF);
    CHECK_EQ(GR(O_GETPIXEL, mdc, 1, 0), 0x000000);
    CHECK_EQ(GR(O_GETPIXEL, mdc, 7, 0), 0xFFFFFF);
    CHECK_EQ(GR(O_GETPIXEL, mdc, 8, 0), 0x000000);
    CHECK_EQ(GR(O_GETPIXEL, mdc, 9, 0), 0xFFFFFF);            /* second byte 0x40: the second bit, pixel 9 */
    CHECK_EQ(GR(O_GETPIXEL, mdc, 9, 1), 0xFFFFFF);
    CHECK_EQ(GR(O_GETPIXEL, mdc, 0, 2), 0x000000);
    CHECK_EQ(GR(O_GETPIXEL, mdc, 10, 0), 0xFFFFFFFFu);         /* outside the 10x3 bitmap */
    CHECK_EQ(GA(O_DELETEOBJECT, bm), 0);                       /* still selected */
    /* a colour bitmap compatible with the screen: 8 bits per pixel */
    bm = compat_bmp(dc, 20, 10);
    CHECK(bm != 0);
    GR(O_GETOBJECT, bm, 14, FARP(s, 0x20));
    CHECK_EQ(umem(s, 0x20 + 9, 1)[0], 8);
    CHECK_EQ(urd16(s, 0x20 + 6), 20);
    {
        uint8_t row[200];
        int i;
        for (i = 0; i < 200; i++) row[i] = (uint8_t)i;
        set_bits(bm, row, 200);
        CHECK_EQ(GR(O_GETBITMAPBITS, bm, LONG(200), FARP(s, 0x80)), 200);
        CHECK_EQ(umem(s, 0x80 + 123, 1)[0], 123);
        GA(O_SELECTOBJECT, mdc, bm);
        CHECK_EQ(GR(O_GETPIXEL, mdc, 3, 2), cref_of(43));                             /* pixel (3,2) holds index 2*20+3 = 43 */
    }
    /* compatible with a memory DC that holds a mono bitmap: mono again */
    GA(O_SELECTOBJECT, mdc, GA(O_CREATEBITMAP, 4, 4, 1, 1, FARP(0, 0)));
    {
        uint16_t b2 = compat_bmp(mdc, 8, 8);
        GR(O_GETOBJECT, b2, 14, FARP(s, 0x20));
        CHECK_EQ(umem(s, 0x20 + 9, 1)[0], 1);
    }
    /* CreateBitmapIndirect reads a BITMAP through a far pointer, bits included */
    uwr16(s, 0x100, 0); uwr16(s, 0x102, 8); uwr16(s, 0x104, 2); uwr16(s, 0x106, 2);
    umem(s, 0x108, 1)[0] = 1; umem(s, 0x109, 1)[0] = 1;
    uwr16(s, 0x10A, 0x10); uwr16(s, 0x10C, s);                    /* bmBits: far pointer (offset, selector) */
    umem(s, 0x10, 2)[0] = 0xAA; umem(s, 0x10, 2)[1] = 0x00;
    umem(s, 0x12, 2)[0] = 0x0F; umem(s, 0x12, 2)[1] = 0x00;
    bm = GA(O_CREATEBITMAPINDIRECT, FARP(s, 0x100));
    CHECK(bm != 0);
    CHECK_EQ(GR(O_GETBITMAPBITS, bm, LONG(4), FARP(s, 0x140)), 4);
    CHECK_EQ(umem(s, 0x140, 1)[0], 0xAA);
    CHECK_EQ(umem(s, 0x142, 1)[0], 0x0F);
    /* planar EGA data (4 planes of 1 bit) becomes a colour bitmap; plane 0 is the blue bit */
    {
        uint16_t pb = g_seg(0x40), b3;
        /* one 8 pixel row: the planes are stored one after the other for each scanline */
        umem(pb, 0, 8)[0] = 0x80; umem(pb, 0, 8)[1] = 0;
        umem(pb, 0, 8)[2] = 0x80; umem(pb, 0, 8)[3] = 0;          /* planes 0 and 1 set in pixel 0: index 3 */
        umem(pb, 0, 8)[4] = 0; umem(pb, 0, 8)[5] = 0; umem(pb, 0, 8)[6] = 0; umem(pb, 0, 8)[7] = 0;
        b3 = GA(O_CREATEBITMAP, 8, 1, 4, 1, FARP(pb, 0));
        CHECK(b3 != 0);
        GR(O_GETOBJECT, b3, 14, FARP(s, 0x20));
        CHECK_EQ(umem(s, 0x20 + 9, 1)[0], 8);
        GR(O_GETBITMAPBITS, b3, LONG(8), FARP(s, 0x180));
        CHECK_EQ(umem(s, 0x180, 1)[0], (uint8_t)fb_pixel_of(g_fb(), 0x808000));    /* EGA colour 3 = dark cyan (0,128,128) */
        CHECK_EQ(umem(s, 0x181, 1)[0], 0);
    }
    CHECK_EQ(GA(O_CREATEBITMAP, 0, 5, 1, 1, FARP(0, 0)), 0);                      /* empty bitmaps do not exist */
    CHECK_EQ(GA(O_CREATEBITMAP, 5, 5, 1, 24, FARP(0, 0)), 0);                     /* not a device format */
    CHECK(GA(O_CREATEDISCARDABLEBITMAP, dc, 4, 4) != 0);
    gdown();
}

/* a memory DC with an 8 bit bitmap of w x h holding the given bytes */
static uint16_t mem8(uint16_t dc, int w, int h, const uint8_t *px) {
    uint16_t m = GA(O_CREATECOMPATIBLEDC, dc), bm = compat_bmp(dc, w, h);
    GA(O_SELECTOBJECT, m, bm);
    set_bits(bm, px, (uint32_t)w * (uint32_t)h);
    return m;
}

static void test_bitblt_named_rops(void) {
    static const struct { uint32_t rop; const char *name; } ops[] = {
        {SRCCOPY, "SRCCOPY"}, {SRCPAINT, "SRCPAINT"}, {SRCAND, "SRCAND"}, {SRCINVERT, "SRCINVERT"}, {SRCERASE, "SRCERASE"},
        {NOTSRCCOPY, "NOTSRCCOPY"}, {NOTSRCERASE, "NOTSRCERASE"}, {MERGECOPY, "MERGECOPY"}, {MERGEPAINT, "MERGEPAINT"},
        {PATCOPY, "PATCOPY"}, {PATPAINT, "PATPAINT"}, {PATINVERT, "PATINVERT"}, {DSTINVERT, "DSTINVERT"},
        {BLACKNESS, "BLACKNESS"}, {WHITENESS, "WHITENESS"}};
    uint16_t dc, m, br;
    uint8_t sp[4] = {0x6C, 0x0F, 0xF0, 0xAA};
    size_t k;
    int x;
    printf("test_blt_named_rops\n");
    gboot();
    dc = g_screen_dc();
    m = mem8(dc, 4, 1, sp);
    br = GA(O_CREATESOLIDBRUSH, LONG(0x0000FF));
    {
        uint8_t p = (uint8_t)fb_pixel_of(g_fb(), 0x0000FF);
        GA(O_SELECTOBJECT, dc, br);
        for (k = 0; k < sizeof ops / sizeof ops[0]; k++) {
            uint8_t d = 0x39;
            g_clear(d);
            CHECK_EQ(GA(O_BITBLT, dc, 100, 50, 4, 1, m, 0, 0, LONG(ops[k].rop)), 1);
            for (x = 0; x < 4; x++) {
                uint8_t want = ref_rop(ops[k].rop >> 16 & 0xFF, p, sp[x], d);
                if (g_px(100 + x, 50) != want) printf("  %s: pixel %d got %02x want %02x\n", ops[k].name, x, g_px(100 + x, 50), want);
                CHECK_EQ(g_px(100 + x, 50), want);
            }
            CHECK_EQ(g_px(104, 50), d);                      /* nothing outside the 4x1 rectangle */
            CHECK_EQ(g_px(100, 51), d);
        }
        /* the same operations that do not read the source also work as PatBlt, the others fail */
        for (k = 0; k < sizeof ops / sizeof ops[0]; k++) {
            bool src = ((ops[k].rop >> 16 & 0xFF) >> 2 ^ (ops[k].rop >> 16 & 0xFF)) & 0x33;
            uint8_t d = 0x39;
            g_clear(d);
            CHECK_EQ(GA(O_PATBLT, dc, 100, 50, 4, 1, LONG(ops[k].rop)), src ? 0 : 1);
            if (!src) CHECK_EQ(g_px(101, 50), ref_rop(ops[k].rop >> 16 & 0xFF, p, 0, d));
            else CHECK_EQ(g_px(101, 50), d);
        }
    }
    gdown();
}

static void test_bitblt_geometry(void) {
    uint16_t dc, m, mono;
    uint8_t sp[6 * 4];
    int x, y, bad;
    printf("test_blt_geometry\n");
    gboot();
    dc = g_screen_dc();
    for (y = 0; y < 4; y++)
        for (x = 0; x < 6; x++) sp[y * 6 + x] = (uint8_t)(16 + y * 6 + x);
    m = mem8(dc, 6, 4, sp);
    g_clear(0x03);
    GA(O_BITBLT, dc, 200, 100, 6, 4, m, 0, 0, LONG(SRCCOPY));
    bad = 0;
    for (y = 0; y < 4; y++)
        for (x = 0; x < 6; x++) bad += g_px(200 + x, 100 + y) != sp[y * 6 + x];
    CHECK_EQ(bad, 0);
    /* a sub rectangle of the source */
    g_clear(0x03);
    GA(O_BITBLT, dc, 200, 100, 3, 2, m, 2, 1, LONG(SRCCOPY));
    CHECK_EQ(g_px(200, 100), sp[1 * 6 + 2]);
    CHECK_EQ(g_px(202, 101), sp[2 * 6 + 4]);
    CHECK_EQ(g_px(203, 100), 0x03);
    CHECK_EQ(g_px(200, 102), 0x03);
    /* clipped at the screen edge and by the clip region; pixels whose source is outside the bitmap are left alone */
    g_clear(0x03);
    GA(O_BITBLT, dc, 637, 478, 6, 4, m, 0, 0, LONG(SRCCOPY));
    CHECK_EQ(g_px(639, 479), sp[1 * 6 + 2]);
    GA(O_BITBLT, dc, 300, 100, 10, 10, m, 3, 2, LONG(SRCCOPY));
    CHECK_EQ(g_px(300, 100), sp[2 * 6 + 3]);
    CHECK_EQ(g_px(303, 100), 0x03);                          /* the bitmap ends at column 5 */
    CHECK_EQ(g_px(300, 102), 0x03);                          /* and row 3 */
    g_clear(0x03);
    GA(O_INTERSECTCLIPRECT, dc, 202, 101, 204, 103);
    GA(O_BITBLT, dc, 200, 100, 6, 4, m, 0, 0, LONG(SRCCOPY));
    CHECK_EQ(g_px(201, 101), 0x03);
    CHECK_EQ(g_px(202, 101), sp[1 * 6 + 2]);
    CHECK_EQ(g_px(203, 102), sp[2 * 6 + 3]);
    CHECK_EQ(g_px(204, 102), 0x03);
    GA(O_SELECTCLIPRGN, dc, 0);
    /* screen to screen with overlap scrolls without smearing */
    g_clear(0x03);
    for (x = 0; x < 20; x++) g_fb()->p8[100 * 640 + 50 + x] = (uint8_t)(100 + x);
    GA(O_BITBLT, dc, 53, 100, 20, 1, dc, 50, 100, LONG(SRCCOPY));
    for (x = 0; x < 20; x++) CHECK_EQ(g_px(53 + x, 100), 100 + x);
    /* source and destination coordinates are logical */
    g_clear(0x03);
    GR(O_SETWINDOWORG, dc, (uint16_t)-10, (uint16_t)-20);
    GA(O_BITBLT, dc, 0, 0, 6, 4, m, 0, 0, LONG(SRCCOPY));
    CHECK_EQ(g_px(10, 20), sp[0]);
    GR(O_SETWINDOWORG, dc, 0, 0);
    /* monochrome source: 0 bits take the destination text colour, 1 bits its background colour */
    mono = GA(O_CREATECOMPATIBLEDC, dc);
    {
        uint16_t mb = GA(O_CREATEBITMAP, 8, 1, 1, 1, FARP(0, 0)), bs = g_seg(8);
        umem(bs, 0, 2)[0] = 0xA0; umem(bs, 0, 2)[1] = 0;
        GA(O_SELECTOBJECT, mono, mb);
        GR(O_SETBKCOLOR, dc, LONG(0x00FF00));
        GR(O_SETTEXTCOLOR, dc, LONG(0x0000FF));
        set_bits(mb, umem(bs, 0, 2), 2);
        g_clear(0x03);
        GA(O_BITBLT, dc, 400, 300, 8, 1, mono, 0, 0, LONG(SRCCOPY));
        CHECK_EQ(g_px(400, 300), (uint8_t)fb_pixel_of(g_fb(), 0x00FF00));      /* bit 7 set: background */
        CHECK_EQ(g_px(401, 300), (uint8_t)fb_pixel_of(g_fb(), 0x0000FF));      /* clear: text colour */
        CHECK_EQ(g_px(402, 300), (uint8_t)fb_pixel_of(g_fb(), 0x00FF00));
        CHECK_EQ(g_px(407, 300), (uint8_t)fb_pixel_of(g_fb(), 0x0000FF));
    }
    /* colour to monochrome: pixels of the source's background colour become 1, the rest 0 */
    {
        uint16_t c8 = mem8(dc, 4, 1, (const uint8_t[]){0xF9, 0xFF, 0x00, 0xFF}), mdst = GA(O_CREATECOMPATIBLEDC, dc);
        uint16_t mb = GA(O_CREATEBITMAP, 4, 1, 1, 1, FARP(0, 0));
        uint16_t out = g_seg(8);
        GA(O_SELECTOBJECT, mdst, mb);
        GR(O_SETBKCOLOR, c8, LONG(0xFFFFFF));                                   /* source background: white */
        GA(O_BITBLT, mdst, 0, 0, 4, 1, c8, 0, 0, LONG(SRCCOPY));
        GR(O_GETBITMAPBITS, mb, LONG(2), FARP(out, 0));
        CHECK_EQ(umem(out, 0, 1)[0] >> 4, 0x5);                                   /* 0101: white pixels are 1 */
    }
    gdown();
}

static void test_stretchblt(void) {
    uint16_t dc, m;
    uint8_t sp[4 * 2];
    int x, y, bad;
    printf("test_blt_stretch\n");
    gboot();
    dc = g_screen_dc();
    for (y = 0; y < 2; y++)
        for (x = 0; x < 4; x++) sp[y * 4 + x] = (uint8_t)(20 + y * 4 + x);
    m = mem8(dc, 4, 2, sp);
    /* 2x enlargement: every source pixel becomes a 2x2 block */
    g_clear(0x03);
    CHECK_EQ(GA(O_STRETCHBLT, dc, 100, 100, 8, 4, m, 0, 0, 4, 2, LONG(SRCCOPY)), 1);
    bad = 0;
    for (y = 0; y < 4; y++)
        for (x = 0; x < 8; x++) bad += g_px(100 + x, 100 + y) != sp[(y / 2) * 4 + x / 2];
    CHECK_EQ(bad, 0);
    CHECK_EQ(g_px(108, 100), 0x03);
    /* 3x in x, 1x in y */
    g_clear(0x03);
    GA(O_STRETCHBLT, dc, 100, 100, 12, 2, m, 0, 0, 4, 2, LONG(SRCCOPY));
    for (x = 0; x < 12; x++) CHECK_EQ(g_px(100 + x, 101), sp[4 + x / 3]);
    /* shrinking keeps every other pixel (the first of each pair) */
    g_clear(0x03);
    GA(O_STRETCHBLT, dc, 100, 100, 2, 1, m, 0, 0, 4, 2, LONG(SRCCOPY));
    CHECK_EQ(g_px(100, 100), sp[0]);
    CHECK_EQ(g_px(101, 100), sp[2]);
    CHECK_EQ(g_px(102, 100), 0x03);
    /* a negative source width mirrors horizontally */
    g_clear(0x03);
    GA(O_STRETCHBLT, dc, 100, 100, 4, 1, m, 3, 0, (uint16_t)-4, 1, LONG(SRCCOPY));
    for (x = 0; x < 4; x++) CHECK_EQ(g_px(100 + x, 100), sp[3 - x]);
    /* a negative destination height mirrors vertically */
    g_clear(0x03);
    GA(O_STRETCHBLT, dc, 100, 101, 4, (uint16_t)-2, m, 0, 0, 4, 2, LONG(SRCCOPY));
    for (x = 0; x < 4; x++) {
        CHECK_EQ(g_px(100 + x, 100), sp[4 + x]);
        CHECK_EQ(g_px(100 + x, 101), sp[x]);
    }
    /* mode setters exist and the default mode is fine */
    GA(O_SETSTRETCHBLTMODE, dc, 3);
    gdown();
}

static void test_patblt_and_pattern_brushes(void) {
    uint16_t dc, bm, pb, s;
    int x, y;
    printf("test_blt_patblt_patterns\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x100);
    /* PatBlt with a solid brush: PATCOPY, PATINVERT, DSTINVERT, BLACKNESS, WHITENESS */
    GA(O_SELECTOBJECT, dc, GA(O_CREATESOLIDBRUSH, LONG(0x0000FF)));
    g_clear(0x03);
    GA(O_PATBLT, dc, 10, 10, 5, 3, LONG(PATCOPY));
    CHECK_EQ(g_px(10, 10), (uint8_t)fb_pixel_of(g_fb(), 0x0000FF));
    CHECK_EQ(g_px(14, 12), (uint8_t)fb_pixel_of(g_fb(), 0x0000FF));
    CHECK_EQ(g_px(15, 12), 0x03);
    CHECK_EQ(g_px(14, 13), 0x03);
    GA(O_PATBLT, dc, 10, 10, 5, 3, LONG(DSTINVERT));
    CHECK_EQ(g_px(10, 10), (uint8_t)~(uint8_t)fb_pixel_of(g_fb(), 0x0000FF));
    GA(O_PATBLT, dc, 10, 10, 5, 3, LONG(BLACKNESS));
    CHECK_EQ(g_px(11, 11), 0);
    GA(O_PATBLT, dc, 10, 10, 5, 3, LONG(WHITENESS));
    CHECK_EQ(g_px(11, 11), 0xFF);
    /* a monochrome pattern brush: 0 bits in the text colour, 1 bits in the background colour */
    {
        uint8_t pat[8] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};
        uint16_t pbits = g_seg(0x20);
        for (y = 0; y < 8; y++) {
            umem(pbits, 2 * (uint32_t)y, 2)[0] = pat[y];
            umem(pbits, 2 * (uint32_t)y, 2)[1] = 0;
        }
        bm = GA(O_CREATEBITMAP, 8, 8, 1, 1, FARP(pbits, 0));
        pb = GA(O_CREATEPATTERNBRUSH, bm);
        CHECK(pb != 0);
        GR(O_GETOBJECT, pb, 8, FARP(s, 0));
        CHECK_EQ(urd16(s, 0), 3);                                      /* BS_PATTERN */
        CHECK_EQ(GA(O_DELETEOBJECT, bm), 1);                           /* the brush owns a copy */
        GA(O_SELECTOBJECT, dc, pb);
        GR(O_SETBKCOLOR, dc, LONG(0x00FF00));
        GR(O_SETTEXTCOLOR, dc, LONG(0x0000FF));
        g_clear(0x03);
        GA(O_PATBLT, dc, 16, 16, 16, 16, LONG(PATCOPY));
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++) {
                bool bit = (pat[(16 + y) & 7] >> (7 - ((16 + x) & 7))) & 1;
                CHECK_EQ(g_px(16 + x, 16 + y), (uint8_t)fb_pixel_of(g_fb(), bit ? 0x00FF00 : 0x0000FF));
            }
    }
    /* CreateBrushIndirect with BS_PATTERN takes the bitmap in lbHatch */
    {
        uint16_t b8 = compat_bmp(dc, 8, 8), lb = g_seg(0x20), br2;
        uint8_t px[64];
        for (x = 0; x < 64; x++) px[x] = (uint8_t)(0xF0 + x % 5);
        set_bits(b8, px, 64);
        uwr16(lb, 0, 3); uwr32(lb, 2, 0); uwr16(lb, 6, b8);
        br2 = GA(O_CREATEBRUSHINDIRECT, FARP(lb, 0));
        CHECK(br2 != 0);
        GA(O_SELECTOBJECT, dc, br2);
        g_clear(0x03);
        GA(O_PATBLT, dc, 8, 8, 8, 8, LONG(PATCOPY));
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++) CHECK_EQ(g_px(8 + x, 8 + y), px[((8 + y) & 7) * 8 + ((8 + x) & 7)]);
    }
    gdown();
}

static void test_memdc_drawing(void) {
    uint16_t dc, m, bm, s;
    printf("test_blt_memdc_drawing\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x100);
    m = GA(O_CREATECOMPATIBLEDC, dc);
    bm = compat_bmp(dc, 40, 30);
    GA(O_SELECTOBJECT, m, bm);
    /* GDI calls draw into the selected bitmap, not onto the screen */
    g_clear(0x03);
    GA(O_SELECTOBJECT, m, GA(O_CREATESOLIDBRUSH, LONG(0x0000FF)));
    GA(O_RECTANGLE, m, 5, 5, 15, 12);
    CHECK_EQ(g_px(7, 7), 0x03);
    CHECK_EQ(GR(O_GETPIXEL, m, 7, 7), 0x0000FF);
    CHECK_EQ(GR(O_GETPIXEL, m, 5, 5), 0x000000);                      /* the black pen */
    CHECK_EQ(GR(O_GETPIXEL, m, 50, 50), 0xFFFFFFFFu);                 /* outside the bitmap */
    GA(O_BITBLT, dc, 100, 100, 40, 30, m, 0, 0, LONG(SRCCOPY));
    CHECK_EQ(g_px(107, 107), (uint8_t)fb_pixel_of(g_fb(), 0x0000FF));
    CHECK_EQ(g_px(105, 105), 0);
    /* a monochrome memory bitmap: colours collapse to black and white */
    {
        uint16_t mm = GA(O_CREATECOMPATIBLEDC, dc), mb = GA(O_CREATEBITMAP, 16, 16, 1, 1, FARP(0, 0));
        GA(O_SELECTOBJECT, mm, mb);
        GA(O_SELECTOBJECT, mm, GA(O_CREATESOLIDBRUSH, LONG(0xFFFFFF)));
        GA(O_RECTANGLE, mm, 2, 2, 10, 10);
        CHECK_EQ(GR(O_GETPIXEL, mm, 5, 5), 0xFFFFFF);
        CHECK_EQ(GR(O_GETPIXEL, mm, 14, 14), 0x000000);
        /* mask technique: SRCAND with the mask, SRCPAINT with the image */
        g_clear(0x03);
        GR(O_SETBKCOLOR, dc, LONG(0xFFFFFF));
        GR(O_SETTEXTCOLOR, dc, LONG(0));
        GA(O_BITBLT, dc, 50, 50, 16, 16, mm, 0, 0, LONG(SRCCOPY));
        CHECK_EQ(g_px(55, 55), 0xFF);                                      /* mono 1 -> destination background */
        CHECK_EQ(g_px(60, 60), 0x00);                                      /* mono 0 -> text colour */
    }
    CHECK_EQ(GR(O_GETDCORG, dc), 0);                                        /* the desktop DC starts at the screen corner */
    CHECK_EQ(GA(O_DELETEDC, m), 1);
    CHECK_EQ(GA(O_DELETEOBJECT, bm), 1);                                   /* freed with the DC */
    (void)s;
    gdown();
}

static void test_scrolldc(void) {
    uint16_t dc, s;
    int x;
    printf("test_blt_scrolldc\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x100);
    g_clear(0x03);
    for (x = 0; x < 30; x++) g_fb()->p8[100 * 640 + 100 + x] = (uint8_t)(50 + x);
    uwr16(s, 0, 100); uwr16(s, 2, 90); uwr16(s, 4, 140); uwr16(s, 6, 110);        /* scroll rectangle */
    /* scroll 10 to the right: ScrollDC(hdc, dx, dy, lprcScroll, lprcClip, hrgnUpdate, lprcUpdate) */
    CHECK_EQ(UR(U_SCROLLDC, dc, 10, 0, FARP(s, 0), FARP(0, 0), 0, FARP(s, 0x20)), 1);
    for (x = 0; x < 30; x++) CHECK_EQ(g_px(110 + x, 100), 50 + x);
    CHECK_EQ(g_px(140, 100), 0x03);                                                 /* nothing leaks past the rectangle */
    CHECK_EQ(urd16(s, 0x20), 100);                                                  /* the uncovered strip is reported */
    CHECK_EQ(urd16(s, 0x24), 110);
    CHECK_EQ(urd16(s, 0x22), 90);
    CHECK_EQ(urd16(s, 0x26), 110);
    gdown();
}

/* A program that draws in pane coordinates sets a viewport origin, hands ScrollDC the pane's rectangle in those
 * logical coordinates and expects the uncovered area back in the same coordinates (Creative Writer's document view
 * does: it intersects the returned rectangle with its page rectangles, which start at 0,0). */
static void test_scrolldc_logical(void) {
    uint16_t dc, s, rg;
    int x;
    printf("test_blt_scrolldc_logical\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x100);
    g_clear(0x03);
    GR(O_SETVIEWPORTORG, dc, 40, 30);
    for (x = 0; x < 20; x++) g_fb()->p8[(30 + 10) * 640 + 40 + x] = (uint8_t)(60 + x);       /* logical row 10 */
    uwr16(s, 0, 0); uwr16(s, 2, 0); uwr16(s, 4, 100); uwr16(s, 6, 60);                         /* logical 0,0,100,60 */
    rg = GA(O_CREATERECTRGN, 0, 0, 0, 0);
    CHECK_EQ(UR(U_SCROLLDC, dc, 0, -4, FARP(s, 0), FARP(s, 0), rg, FARP(s, 0x20)), 1);
    for (x = 0; x < 20; x++) CHECK_EQ(g_px(40 + x, 30 + 6), 60 + x);                           /* row 10 moved up 4 */
    CHECK_EQ(urd16(s, 0x20), 0);                                                                /* uncovered strip, logical */
    CHECK_EQ(urd16(s, 0x22), 56);
    CHECK_EQ(urd16(s, 0x24), 100);
    CHECK_EQ(urd16(s, 0x26), 60);
    GA(O_GETRGNBOX, rg, FARP(s, 0x40));                                                         /* and the region likewise */
    CHECK_EQ(urd16(s, 0x40), 0);
    CHECK_EQ(urd16(s, 0x42), 56);
    CHECK_EQ(urd16(s, 0x44), 100);
    CHECK_EQ(urd16(s, 0x46), 60);
    gdown();
}

int main(void) {
    test_rop3_evaluator();
    test_bitmaps();
    test_bitblt_named_rops();
    test_bitblt_geometry();
    test_stretchblt();
    test_patblt_and_pattern_brushes();
    test_memdc_drawing();
    test_scrolldc();
    test_scrolldc_logical();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
