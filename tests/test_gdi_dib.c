/* Device independent bitmaps: SetDIBits/GetDIBits, CreateDIBitmap, StretchDIBits,
 * SetDIBitsToDevice and DIB pattern brushes. Rows are bottom-up
 * with 4-byte padding unless the height is negative. */
#include "gfixture.h"

#define SRCCOPY 0x00CC0020u
#define PATCOPY 0x00F00021u

enum {
    O_SELECTOBJECT = 45, O_CREATECOMPATIBLEBITMAP = 51, O_CREATECOMPATIBLEDC = 52, O_PATBLT = 29, O_GETOBJECT = 82,
    O_GETBITMAPBITS = 74, O_STRETCHDIBITS = 439, O_SETDIBITS = 440, O_GETDIBITS = 441,
    O_CREATEDIBITMAP = 442, O_SETDIBITSTODEVICE = 443, O_CREATEDIBPATTERNBRUSH = 445, O_GETPIXEL = 83,
    O_CREATEBRUSHINDIRECT = 50, O_DELETEOBJECT = 69
};

#define BI_RGB 0
#define BI_RLE8 1
#define BI_RLE4 2
#define DIB_RGB_COLORS 0
#define CBM_INIT 4

static uint8_t pix_of(uint32_t cref) { return (uint8_t)fb_pixel_of(g_fb(), cref); }

/* COLORREFs that sit exactly on a static palette entry */
#define RED RGB(255, 0, 0)
#define GREEN RGB(0, 255, 0)
#define BLUE RGB(0, 0, 255)
#define YELLOW RGB(255, 255, 0)
#define WHITE RGB(255, 255, 255)
#define BLACK RGB(0, 0, 0)

static void bih(uint16_t s, uint32_t off, int w, int h, int bpp, int comp, int clrused, uint32_t sizeimage) {
    uwr32(s, off, 40);
    uwr32(s, off + 4, (uint32_t)w);
    uwr32(s, off + 8, (uint32_t)h);
    uwr16(s, off + 12, 1);
    uwr16(s, off + 14, (uint16_t)bpp);
    uwr32(s, off + 16, (uint32_t)comp);
    uwr32(s, off + 20, sizeimage);
    uwr32(s, off + 24, 0);
    uwr32(s, off + 28, 0);
    uwr32(s, off + 32, (uint32_t)clrused);
    uwr32(s, off + 36, 0);
}

/* RGBQUAD entry i at off: blue, green, red, reserved */
static void quad(uint16_t s, uint32_t off, int i, uint32_t cref) {
    umem(s, off + 4u * (unsigned)i, 4)[0] = (uint8_t)(cref >> 16);
    umem(s, off + 4u * (unsigned)i, 4)[1] = (uint8_t)(cref >> 8);
    umem(s, off + 4u * (unsigned)i, 4)[2] = (uint8_t)cref;
    umem(s, off + 4u * (unsigned)i, 4)[3] = 0;
}

static void bgr(uint16_t s, uint32_t off, uint32_t cref) {
    umem(s, off, 3)[0] = (uint8_t)(cref >> 16);
    umem(s, off, 3)[1] = (uint8_t)(cref >> 8);
    umem(s, off, 3)[2] = (uint8_t)cref;
}

static void test_dib24_and_padding(void) {
    uint16_t dc, s;
    printf("test_dib_24bpp\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x400);
    /* 3x2, 24 bpp: 9 bytes per row padded to 12; the first row in the file is the bottom row */
    bih(s, 0, 3, 2, 24, BI_RGB, 0, 24);
    memset(umem(s, 0x100, 24), 0xEE, 24);                         /* padding bytes must be ignored */
    bgr(s, 0x100, RED); bgr(s, 0x103, GREEN); bgr(s, 0x106, BLUE);              /* bottom row */
    bgr(s, 0x10C, WHITE); bgr(s, 0x10F, BLACK); bgr(s, 0x112, YELLOW);           /* top row */
    g_clear(0x03);
    CHECK_EQ(GA(O_STRETCHDIBITS, dc, 50, 60, 3, 2, 0, 0, 3, 2, FARP(s, 0x100), FARP(s, 0), DIB_RGB_COLORS, LONG(SRCCOPY)), 2);  /* lines copied */
    CHECK_EQ(g_px(50, 60), pix_of(WHITE));
    CHECK_EQ(g_px(51, 60), pix_of(BLACK));
    CHECK_EQ(g_px(52, 60), pix_of(YELLOW));
    CHECK_EQ(g_px(50, 61), pix_of(RED));
    CHECK_EQ(g_px(51, 61), pix_of(GREEN));
    CHECK_EQ(g_px(52, 61), pix_of(BLUE));
    CHECK_EQ(g_px(53, 60), 0x03);
    CHECK_EQ(g_px(50, 62), 0x03);
    /* top-down: a negative height flips the row order */
    bih(s, 0, 3, -2, 24, BI_RGB, 0, 24);
    g_clear(0x03);
    GA(O_STRETCHDIBITS, dc, 50, 60, 3, 2, 0, 0, 3, 2, FARP(s, 0x100), FARP(s, 0), DIB_RGB_COLORS, LONG(SRCCOPY));
    CHECK_EQ(g_px(50, 60), pix_of(RED));
    CHECK_EQ(g_px(50, 61), pix_of(WHITE));
    /* an unmatched colour goes to the nearest static colour (here 40,40,40 -> black) */
    bih(s, 0, 1, 1, 24, BI_RGB, 0, 4);
    bgr(s, 0x100, RGB(40, 40, 40));
    GA(O_STRETCHDIBITS, dc, 70, 70, 1, 1, 0, 0, 1, 1, FARP(s, 0x100), FARP(s, 0), DIB_RGB_COLORS, LONG(SRCCOPY));
    CHECK_EQ(g_px(70, 70), pix_of(BLACK));
    gdown();
}

static void test_dib_palettised(void) {
    uint16_t dc, s;
    int x, y;
    printf("test_dib_1_4_8bpp_and_core\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x800);
    /* 8 bpp, width 5 (rows padded to 8 bytes), colour table of 3 RGBQUADs */
    bih(s, 0, 5, 2, 8, BI_RGB, 3, 16);
    quad(s, 40, 0, RED); quad(s, 40, 1, GREEN); quad(s, 40, 2, BLUE);
    {
        static const uint8_t rows[16] = {0, 1, 2, 0, 1, 9, 9, 9, 2, 2, 1, 1, 0, 9, 9, 9};
        memcpy(umem(s, 0x200, 16), rows, 16);
    }
    g_clear(0x03);
    GA(O_STRETCHDIBITS, dc, 10, 10, 5, 2, 0, 0, 5, 2, FARP(s, 0x200), FARP(s, 0), DIB_RGB_COLORS, LONG(SRCCOPY));
    {
        static const uint32_t want[2][5] = {{BLUE, BLUE, GREEN, GREEN, RED}, {RED, GREEN, BLUE, RED, GREEN}};
        for (y = 0; y < 2; y++)
            for (x = 0; x < 5; x++) CHECK_EQ(g_px(10 + x, 10 + y), pix_of(want[y][x]));
    }
    /* 4 bpp: two pixels per byte, the high nibble first */
    bih(s, 0, 5, 1, 4, BI_RGB, 4, 8);
    quad(s, 40, 0, BLACK); quad(s, 40, 1, RED); quad(s, 40, 2, GREEN); quad(s, 40, 3, YELLOW);
    {
        static const uint8_t row[4] = {0x12, 0x30, 0x20, 0xFF};
        memcpy(umem(s, 0x200, 4), row, 4);
    }
    g_clear(0x03);
    GA(O_STRETCHDIBITS, dc, 10, 10, 5, 1, 0, 0, 5, 1, FARP(s, 0x200), FARP(s, 0), DIB_RGB_COLORS, LONG(SRCCOPY));
    CHECK_EQ(g_px(10, 10), pix_of(RED));
    CHECK_EQ(g_px(11, 10), pix_of(GREEN));
    CHECK_EQ(g_px(12, 10), pix_of(YELLOW));
    CHECK_EQ(g_px(13, 10), pix_of(BLACK));
    CHECK_EQ(g_px(14, 10), pix_of(GREEN));
    /* 1 bpp, the old BITMAPCOREHEADER with RGBTRIPLE entries (blue, green, red) */
    uwr32(s, 0, 12);
    uwr16(s, 4, 10);
    uwr16(s, 6, 2);
    uwr16(s, 8, 1);
    uwr16(s, 10, 1);
    bgr(s, 12, BLUE);
    bgr(s, 15, YELLOW);
    {
        static const uint8_t rows[8] = {0xFF, 0xC0, 0, 0, 0xAA, 0x80, 0, 0};       /* bottom row first, 4 bytes each */
        memcpy(umem(s, 0x200, 8), rows, 8);
    }
    g_clear(0x03);
    GA(O_STRETCHDIBITS, dc, 20, 20, 10, 2, 0, 0, 10, 2, FARP(s, 0x200), FARP(s, 0), DIB_RGB_COLORS, LONG(SRCCOPY));
    for (x = 0; x < 10; x++) CHECK_EQ(g_px(20 + x, 21), pix_of(YELLOW));                    /* bottom row: all ones */
    CHECK_EQ(g_px(20, 20), pix_of(YELLOW));                                                   /* top row 0xAA 0x80 */
    CHECK_EQ(g_px(21, 20), pix_of(BLUE));
    CHECK_EQ(g_px(22, 20), pix_of(YELLOW));
    CHECK_EQ(g_px(28, 20), pix_of(YELLOW));
    CHECK_EQ(g_px(29, 20), pix_of(BLUE));
    /* run-length encoded 8 bpp: runs, an absolute run with its pad byte, end of line, end of bitmap */
    bih(s, 0, 4, 2, 8, BI_RLE8, 8, 16);
    {
        int i;
        for (i = 0; i < 8; i++) quad(s, 40, i, i == 7 ? WHITE : i == 1 ? RED : i == 2 ? GREEN : i == 3 ? BLUE : BLACK);
    }
    {
        static const uint8_t rle[] = {4, 7, 0, 0,                     /* bottom row: 4 x index 7, end of line */
                                      0, 3, 1, 2, 3, 0, 0, 0,         /* top row: absolute run of 3 (+pad), end of line */
                                      0, 1};                          /* end of bitmap */
        memcpy(umem(s, 0x200, sizeof rle), rle, sizeof rle);
    }
    uwr32(s, 20, 14);                                                 /* biSizeImage: the encoded length */
    g_clear(0x03);
    GA(O_STRETCHDIBITS, dc, 40, 40, 4, 2, 0, 0, 4, 2, FARP(s, 0x200), FARP(s, 0), DIB_RGB_COLORS, LONG(SRCCOPY));
    for (x = 0; x < 4; x++) CHECK_EQ(g_px(40 + x, 41), pix_of(WHITE));
    CHECK_EQ(g_px(40, 40), pix_of(RED));
    CHECK_EQ(g_px(41, 40), pix_of(GREEN));
    CHECK_EQ(g_px(42, 40), pix_of(BLUE));
    /* RLE4: a run of two alternating nibbles */
    bih(s, 0, 4, 1, 4, BI_RLE4, 4, 8);
    quad(s, 40, 0, BLACK); quad(s, 40, 1, RED); quad(s, 40, 2, GREEN); quad(s, 40, 3, BLUE);
    {
        static const uint8_t rle[] = {4, 0x12, 0, 1};                  /* 4 pixels: 1 2 1 2, end of bitmap */
        memcpy(umem(s, 0x200, sizeof rle), rle, sizeof rle);
    }
    uwr32(s, 20, 4);
    g_clear(0x03);
    GA(O_STRETCHDIBITS, dc, 40, 40, 4, 1, 0, 0, 4, 1, FARP(s, 0x200), FARP(s, 0), DIB_RGB_COLORS, LONG(SRCCOPY));
    CHECK_EQ(g_px(40, 40), pix_of(RED));
    CHECK_EQ(g_px(41, 40), pix_of(GREEN));
    CHECK_EQ(g_px(42, 40), pix_of(RED));
    CHECK_EQ(g_px(43, 40), pix_of(GREEN));
    gdown();
}

static void test_stretch_and_todevice(void) {
    uint16_t dc, s;
    int x, y;
    printf("test_dib_stretch_todevice\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x400);
    /* 4x4 8bpp image whose value encodes its position; the file is bottom-up */
    bih(s, 0, 4, 4, 8, BI_RGB, 4, 16);
    quad(s, 40, 0, BLACK); quad(s, 40, 1, RED); quad(s, 40, 2, GREEN); quad(s, 40, 3, BLUE);
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++) umem(s, 0x200 + (uint32_t)y * 4 + (uint32_t)x, 1)[0] = (uint8_t)((x + y) & 3);   /* row y of the file */
    /* 2x enlargement of the whole image */
    g_clear(0x09);
    GA(O_STRETCHDIBITS, dc, 100, 100, 8, 8, 0, 0, 4, 4, FARP(s, 0x200), FARP(s, 0), DIB_RGB_COLORS, LONG(SRCCOPY));
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            int fy = 3 - y / 2;                                                       /* screen row 0 is the file's last row */
            uint32_t cols[4] = {BLACK, RED, GREEN, BLUE};
            CHECK_EQ(g_px(100 + x, 100 + y), pix_of(cols[(x / 2 + fy) & 3]));
        }
    /* a source sub-rectangle: ySrc counts from the bottom of the DIB */
    g_clear(0x09);
    GA(O_STRETCHDIBITS, dc, 100, 100, 2, 2, 1, 1, 2, 2, FARP(s, 0x200), FARP(s, 0), DIB_RGB_COLORS, LONG(SRCCOPY));
    {
        uint32_t cols[4] = {BLACK, RED, GREEN, BLUE};
        CHECK_EQ(g_px(100, 100), pix_of(cols[(1 + 2) & 3]));           /* lines 1..2 from the bottom: the first dest row is file row 2 */
        CHECK_EQ(g_px(101, 100), pix_of(cols[(2 + 2) & 3]));
        CHECK_EQ(g_px(100, 101), pix_of(cols[(1 + 1) & 3]));           /* then file row 1 */
        CHECK_EQ(g_px(102, 100), 0x09);
    }
    /* SetDIBitsToDevice(hdc, xd, yd, cx, cy, xs, ys, startscan, scanlines, bits, bmi, usage): 1:1 */
    g_clear(0x09);
    CHECK_EQ(GA(O_SETDIBITSTODEVICE, dc, 200, 50, 4, 4, 0, 0, 0, 4, FARP(s, 0x200), FARP(s, 0), DIB_RGB_COLORS), 4);
    {
        uint32_t cols[4] = {BLACK, RED, GREEN, BLUE};
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++) CHECK_EQ(g_px(200 + x, 50 + y), pix_of(cols[(x + 3 - y) & 3]));
    }
    gdown();
}

static void test_set_get_dibits(void) {
    uint16_t dc, s, bm, mdc;
    int x, y;
    printf("test_dib_setget\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x1000);
    bm = GA(O_CREATECOMPATIBLEBITMAP, dc, 4, 2);
    /* SetDIBits: 24 bpp 4x2 */
    bih(s, 0, 4, 2, 24, BI_RGB, 0, 24);
    {
        static const uint32_t bottom[4] = {RED, GREEN, BLUE, YELLOW}, top[4] = {WHITE, BLACK, RED, BLUE};
        for (x = 0; x < 4; x++) {
            bgr(s, 0x100 + 3 * (uint32_t)x, bottom[x]);
            bgr(s, 0x100 + 12 + 3 * (uint32_t)x, top[x]);
        }
    }
    CHECK_EQ(GA(O_SETDIBITS, dc, bm, 0, 2, FARP(s, 0x100), FARP(s, 0), DIB_RGB_COLORS), 2);
    mdc = GA(O_CREATECOMPATIBLEDC, dc);
    GA(O_SELECTOBJECT, mdc, bm);
    CHECK_EQ(GR(O_GETPIXEL, mdc, 0, 0), WHITE);               /* the DIB's top row is the bitmap's top row */
    CHECK_EQ(GR(O_GETPIXEL, mdc, 3, 1), YELLOW);
    /* GetDIBits with a NULL buffer: fills the header */
    memset(umem(s, 0x400, 0x80), 0, 0x80);
    uwr32(s, 0x400, 40);
    CHECK_EQ(GA(O_GETDIBITS, dc, bm, 0, 2, FARP(0, 0), FARP(s, 0x400), DIB_RGB_COLORS), 2);
    CHECK_EQ(urd32(s, 0x400 + 4), 4);
    CHECK_EQ(urd32(s, 0x400 + 8), 2);
    CHECK_EQ(urd16(s, 0x400 + 12), 1);
    CHECK(urd16(s, 0x400 + 14) == 8 || urd16(s, 0x400 + 14) == 24);
    /* 24 bpp readback */
    bih(s, 0x400, 4, 2, 24, BI_RGB, 0, 0);
    memset(umem(s, 0x500, 32), 0xAA, 32);
    CHECK_EQ(GA(O_GETDIBITS, dc, bm, 0, 2, FARP(s, 0x500), FARP(s, 0x400), DIB_RGB_COLORS), 2);
    CHECK_EQ(umem(s, 0x500, 3)[0], 0);                          /* bottom row, pixel 0: red = B 0 G 0 R 255 */
    CHECK_EQ(umem(s, 0x502, 1)[0], 255);
    CHECK_EQ(umem(s, 0x500 + 3 * 3 + 0, 1)[0], 0);              /* yellow: B 0, G 255, R 255 */
    CHECK_EQ(umem(s, 0x500 + 3 * 3 + 1, 1)[0], 255);
    CHECK_EQ(umem(s, 0x500 + 12, 1)[0], 255);                   /* top row pixel 0: white */
    CHECK_EQ(urd32(s, 0x400 + 20), 24);                          /* biSizeImage */
    /* 8 bpp: indices of the device plus the system palette as the colour table */
    bih(s, 0x400, 4, 2, 8, BI_RGB, 0, 0);
    CHECK_EQ(GA(O_GETDIBITS, dc, bm, 0, 2, FARP(s, 0x900), FARP(s, 0x400), DIB_RGB_COLORS), 2);
    CHECK_EQ(umem(s, 0x900, 1)[0], pix_of(RED));
    CHECK_EQ(umem(s, 0x904, 1)[0], pix_of(WHITE));              /* second line in the buffer, 4 bytes per row */
    {
        int idx = pix_of(RED);
        uint8_t *q = umem(s, 0x400 + 40 + 4u * (unsigned)idx, 4);
        CHECK_EQ(q[0], 0);
        CHECK_EQ(q[1], 0);
        CHECK_EQ(q[2], 255);
    }
    /* one scan line only */
    memset(umem(s, 0xA00, 8), 0x77, 8);
    CHECK_EQ(GA(O_GETDIBITS, dc, bm, 1, 1, FARP(s, 0xA00), FARP(s, 0x400), DIB_RGB_COLORS), 1);   /* scan line 1 = the top row */
    CHECK_EQ(umem(s, 0xA00, 1)[0], pix_of(WHITE));
    /* CreateDIBitmap: sized from the header, filled from the bits with CBM_INIT */
    bih(s, 0x400, 4, 2, 24, BI_RGB, 0, 24);
    {
        uint16_t b2 = GA(O_CREATEDIBITMAP, dc, FARP(s, 0x400), LONG(CBM_INIT), FARP(s, 0x100), FARP(s, 0), DIB_RGB_COLORS);
        CHECK(b2 != 0);
        GR(O_GETOBJECT, b2, 14, FARP(s, 0xB00));
        CHECK_EQ(urd16(s, 0xB02), 4);
        CHECK_EQ(urd16(s, 0xB04), 2);
        CHECK_EQ(umem(s, 0xB09, 1)[0], 8);
        GA(O_SELECTOBJECT, mdc, b2);
        CHECK_EQ(GR(O_GETPIXEL, mdc, 1, 0), BLACK);
        CHECK_EQ(GR(O_GETPIXEL, mdc, 0, 1), RED);
        /* without CBM_INIT the bitmap is blank */
        b2 = GA(O_CREATEDIBITMAP, dc, FARP(s, 0x400), LONG(0), FARP(0, 0), FARP(0, 0), DIB_RGB_COLORS);
        GA(O_SELECTOBJECT, mdc, b2);
        CHECK_EQ(GR(O_GETPIXEL, mdc, 0, 1), BLACK);
    }
    /* a 1 bpp header gives a monochrome bitmap */
    bih(s, 0x400, 8, 1, 1, BI_RGB, 2, 4);
    {
        uint16_t b3 = GA(O_CREATEDIBITMAP, dc, FARP(s, 0x400), LONG(0), FARP(0, 0), FARP(0, 0), DIB_RGB_COLORS);
        GR(O_GETOBJECT, b3, 14, FARP(s, 0xB00));
        CHECK_EQ(umem(s, 0xB09, 1)[0], 1);
    }
    (void)y;
    gdown();
}

static void test_dib_pattern_brush(void) {
    uint16_t dc, g8, s;
    int x, y;
    printf("test_dib_pattern_brush\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x40);
    g8 = g_seg(0x400);                                    /* the "global memory handle": a selector in this runtime */
    bih(g8, 0, 8, 8, 24, BI_RGB, 0, 192);
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) bgr(g8, 40 + (uint32_t)(y * 24 + x * 3), (x + y) & 1 ? RED : GREEN);   /* file row y = pattern row 7 - y */
    {
        uint16_t br = GA(O_CREATEDIBPATTERNBRUSH, g8, DIB_RGB_COLORS);
        CHECK(br != 0);
        GR(O_GETOBJECT, br, 8, FARP(s, 0));
        CHECK_EQ(urd16(s, 0), 5);                          /* BS_DIBPATTERN */
        GA(O_SELECTOBJECT, dc, br);
        g_clear(0x03);
        GA(O_PATBLT, dc, 16, 16, 16, 16, LONG(PATCOPY));
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++) {
                int py = (16 + y) & 7, px = (16 + x) & 7, fy = 7 - py;
                CHECK_EQ(g_px(16 + x, 16 + y), pix_of((px + fy) & 1 ? RED : GREEN));
            }
        CHECK_EQ(GA(O_DELETEOBJECT, br), 0);                 /* selected */
    }
    /* CreateBrushIndirect with BS_DIBPATTERN: lbHatch is the global handle, lbColor the usage */
    {
        uint16_t lb = g_seg(0x20), br;
        uwr16(lb, 0, 5); uwr32(lb, 2, DIB_RGB_COLORS); uwr16(lb, 6, g8);
        br = GA(O_CREATEBRUSHINDIRECT, FARP(lb, 0));
        CHECK(br != 0);
    }
    gdown();
}

int main(void) {
    test_dib24_and_padding();
    test_dib_palettised();
    test_stretch_and_todevice();
    test_set_get_dibits();
    test_dib_pattern_brush();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
