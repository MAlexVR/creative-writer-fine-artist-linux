/* Palettes: logical palettes, SelectPalette/RealizePalette against the 256-entry
 * system palette, PALETTEINDEX colours, animation, nearest colour queries and
 * DIB_PAL_COLORS. */
#include "gfixture.h"

enum {
    O_SELECTOBJECT = 45, O_GETOBJECT = 82, O_GETPIXEL = 83, O_SETPIXEL = 31, O_GETSTOCKOBJECT = 87, O_DELETEOBJECT = 69,
    O_GETNEARESTCOLOR = 154, O_CREATEPALETTE = 360, O_GETPALETTEENTRIES = 363, O_SETPALETTEENTRIES = 364,
    O_UPDATECOLORS = 366, O_ANIMATEPALETTE = 367, O_RESIZEPALETTE = 368, O_GETNEARESTPALETTEINDEX = 370,
    O_GETSYSTEMPALETTEENTRIES = 375, O_SETSYSTEMPALETTEUSE = 373, O_GETSYSTEMPALETTEUSE = 374, O_UNREALIZEOBJECT = 150,
    O_CREATESOLIDBRUSH = 66, O_PATBLT = 29, O_STRETCHDIBITS = 439, O_GETDEVICECAPS = 80, O_RESETDC = 376,
    U_SELECTPALETTE = 282, U_REALIZEPALETTE = 283
};

#define PC_RESERVED 1
#define PC_EXPLICIT 2
#define PC_NOCOLLAPSE 4

/* a LOGPALETTE of n entries (r,g,b,flags) at s:off */
static void logpal(uint16_t s, uint32_t off, int n, const uint8_t (*e)[4]) {
    int i;
    uwr16(s, off, 0x300);
    uwr16(s, off + 2, (uint16_t)n);
    for (i = 0; i < n; i++) memcpy(umem(s, off + 4 + 4u * (unsigned)i, 4), e[i], 4);
}

static uint32_t sys_rgb(int idx) { return g_fb()->pal[idx] & 0xFFFFFF; }
static uint32_t rgb3(const uint8_t *e) { return (uint32_t)e[0] << 16 | (uint32_t)e[1] << 8 | e[2]; }

static const uint8_t odd8[8][4] = {{10, 20, 30, 0}, {40, 50, 60, 0}, {70, 80, 90, 0}, {100, 110, 120, 0},
                                   {130, 140, 150, 0}, {160, 170, 180, 0}, {190, 200, 210, 0}, {220, 230, 240, 0}};

static void test_logical_palette(void) {
    uint16_t s, p, dc;
    printf("test_pal_objects\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x800);
    logpal(s, 0, 8, odd8);
    p = GA(O_CREATEPALETTE, FARP(s, 0));
    CHECK(p != 0);
    CHECK_EQ(GR(O_GETOBJECT, p, 2, FARP(s, 0x400)), 2);
    CHECK_EQ(urd16(s, 0x400), 8);
    CHECK_EQ(GA(O_GETPALETTEENTRIES, p, 0, 0, FARP(0, 0)), 8);                  /* NULL buffer: the entry count */
    CHECK_EQ(GA(O_GETPALETTEENTRIES, p, 2, 3, FARP(s, 0x400)), 3);
    CHECK_EQ(umem(s, 0x400, 1)[0], 70);
    CHECK_EQ(umem(s, 0x408, 1)[0], 130);
    CHECK_EQ(GA(O_GETPALETTEENTRIES, p, 6, 10, FARP(s, 0x400)), 2);             /* clipped to the palette */
    CHECK_EQ(GA(O_GETPALETTEENTRIES, p, 9, 1, FARP(s, 0x400)), 0);
    memcpy(umem(s, 0x500, 4), (const uint8_t[]){1, 2, 3, PC_RESERVED}, 4);
    CHECK_EQ(GA(O_SETPALETTEENTRIES, p, 1, 1, FARP(s, 0x500)), 1);
    GA(O_GETPALETTEENTRIES, p, 1, 1, FARP(s, 0x400));
    CHECK_EQ(umem(s, 0x400, 1)[0], 1);
    CHECK_EQ(umem(s, 0x403, 1)[0], PC_RESERVED);
    CHECK_EQ(GA(O_RESIZEPALETTE, p, 12), 1);
    CHECK_EQ(GR(O_GETOBJECT, p, 2, FARP(s, 0x400)), 2);
    CHECK_EQ(urd16(s, 0x400), 12);
    GA(O_GETPALETTEENTRIES, p, 0, 12, FARP(s, 0x600));
    CHECK_EQ(umem(s, 0x600 + 4 * 1, 1)[0], 1);                                   /* existing entries survive */
    CHECK_EQ(umem(s, 0x600 + 4 * 10, 1)[0], 0);                                  /* new ones are black */
    CHECK_EQ(GA(O_RESIZEPALETTE, p, 0), 0);
    uwr16(s, 0, 0x300); uwr16(s, 2, 0);
    CHECK_EQ(GA(O_CREATEPALETTE, FARP(s, 0)), 0);                                /* a palette needs entries */
    /* the default palette: the 20 static colours */
    {
        uint16_t def = GA(O_GETSTOCKOBJECT, 15);
        CHECK_EQ(GA(O_GETPALETTEENTRIES, def, 0, 0, FARP(0, 0)), 20);
        GA(O_GETPALETTEENTRIES, def, 1, 1, FARP(s, 0x400));
        CHECK_EQ(rgb3(umem(s, 0x400, 4)), 0x800000);
        GA(O_GETPALETTEENTRIES, def, 10, 1, FARP(s, 0x400));
        CHECK_EQ(rgb3(umem(s, 0x400, 4)), sys_rgb(246));
    }
    /* deletion follows the selection rules */
    CHECK_EQ(UA(U_SELECTPALETTE, dc, p, 0), GA(O_GETSTOCKOBJECT, 15));
    CHECK_EQ(GA(O_DELETEOBJECT, p), 0);
    CHECK_EQ(UA(U_SELECTPALETTE, dc, GA(O_GETSTOCKOBJECT, 15), 0), p);
    CHECK_EQ(GA(O_DELETEOBJECT, p), 1);
    CHECK_EQ(UA(U_SELECTPALETTE, dc, p, 0), 0);                                  /* a dead handle */
    gdown();
}

static void test_realize(void) {
    uint16_t s, p, p2, dc, mdc;
    uint32_t before[256];
    int i;
    printf("test_pal_realize\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x800);
    memcpy(before, g_fb()->pal, sizeof before);
    logpal(s, 0, 8, odd8);
    p = GA(O_CREATEPALETTE, FARP(s, 0));
    UA(U_SELECTPALETTE, dc, p, 0);
    CHECK_EQ(UA(U_REALIZEPALETTE, dc), 8);                                       /* eight entries mapped */
    for (i = 0; i < 8; i++) CHECK_EQ(sys_rgb(10 + i), rgb3(odd8[i]));            /* first free entries */
    for (i = 0; i < 10; i++) CHECK_EQ(g_fb()->pal[i], before[i]);                /* the static colours stay */
    for (i = 246; i < 256; i++) CHECK_EQ(g_fb()->pal[i], before[i]);
    CHECK_EQ(g_fb()->pal[18], before[18]);
    CHECK_EQ(UA(U_REALIZEPALETTE, dc), 0);                                       /* nothing changes the second time */
    /* PALETTEINDEX(i) draws with the entry the palette was mapped to */
    CHECK_EQ(GR(O_SETPIXEL, dc, 5, 5, LONG(0x01000000u | 3)), (uint32_t)(100 | 110 << 8 | 120 << 16));
    CHECK_EQ(g_px(5, 5), 13);
    CHECK_EQ(GR(O_GETPIXEL, dc, 5, 5), (uint32_t)(100 | 110 << 8 | 120 << 16));
    /* an RGB colour that equals a mapped entry uses it; PALETTERGB too */
    GR(O_SETPIXEL, dc, 6, 5, LONG(70 | 80 << 8 | 90 << 16));
    CHECK_EQ(g_px(6, 5), 12);
    GR(O_SETPIXEL, dc, 7, 5, LONG(0x02000000u | (71 | 80 << 8 | 90 << 16)));
    CHECK_EQ(g_px(7, 5), 12);                                                   /* the nearest entry of the logical palette */
    /* a second palette shares equal colours and the static colours, and takes new entries for the rest */
    {
        static const uint8_t e2[4][4] = {{40, 50, 60, 0}, {255, 0, 0, 0}, {1, 3, 5, 0}, {7, 9, 11, 0}};
        logpal(s, 0x100, 4, e2);
        p2 = GA(O_CREATEPALETTE, FARP(s, 0x100));
        UA(U_SELECTPALETTE, dc, p2, 0);
        CHECK_EQ(UA(U_REALIZEPALETTE, dc), 4);
        GR(O_SETPIXEL, dc, 8, 5, LONG(0x01000000u | 0));
        CHECK_EQ(g_px(8, 5), 11);                                                /* shared with the first palette's entry 1 */
        GR(O_SETPIXEL, dc, 8, 6, LONG(0x01000000u | 1));
        CHECK_EQ(g_px(8, 6), 249);                                               /* red already is a static colour */
        GR(O_SETPIXEL, dc, 8, 7, LONG(0x01000000u | 2));
        CHECK_EQ(g_px(8, 7), 18);                                                /* new entries come next */
        CHECK_EQ(sys_rgb(18), (uint32_t)(1 << 16 | 3 << 8 | 5));
        CHECK_EQ(sys_rgb(19), (uint32_t)(7 << 16 | 9 << 8 | 11));
    }
    /* realizing from a memory DC works the same */
    mdc = GA(52, dc);
    UA(U_SELECTPALETTE, mdc, p, 0);
    CHECK_EQ(UA(U_REALIZEPALETTE, mdc), 0);                                      /* already mapped */
    /* changing an entry and realizing again reports it */
    memcpy(umem(s, 0x500, 4), (const uint8_t[]){200, 1, 2, 0}, 4);
    GA(O_SETPALETTEENTRIES, p, 0, 1, FARP(s, 0x500));
    CHECK_EQ(UA(U_REALIZEPALETTE, mdc), 0);                                      /* same index, new colour: not a different mapping */
    CHECK_EQ(sys_rgb(10), (uint32_t)(200 << 16 | 1 << 8 | 2));
    /* the default palette realizes to nothing */
    UA(U_SELECTPALETTE, dc, GA(O_GETSTOCKOBJECT, 15), 0);
    CHECK_EQ(UA(U_REALIZEPALETTE, dc), 0);
    CHECK_EQ(UA(U_REALIZEPALETTE, 0x7777), 0);
    CHECK_EQ(GA(O_UPDATECOLORS, dc), 1);
    gdown();
}

static void test_flags_and_animation(void) {
    uint16_t s, p, dc;
    printf("test_pal_flags_animation\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x800);
    /* PC_NOCOLLAPSE and PC_RESERVED entries always get an entry of their own, even when the colour exists */
    {
        static const uint8_t e[4][4] = {{255, 0, 0, PC_NOCOLLAPSE}, {255, 0, 0, 0}, {9, 9, 9, PC_RESERVED}, {9, 9, 9, PC_RESERVED}};
        logpal(s, 0, 4, e);
        p = GA(O_CREATEPALETTE, FARP(s, 0));
        UA(U_SELECTPALETTE, dc, p, 0);
        CHECK_EQ(UA(U_REALIZEPALETTE, dc), 4);
        GR(O_SETPIXEL, dc, 1, 1, LONG(0x01000000u | 0));
        CHECK_EQ(g_px(1, 1), 10);                                                /* a private copy of red */
        GR(O_SETPIXEL, dc, 1, 2, LONG(0x01000000u | 1));
        CHECK_EQ(g_px(1, 2), 249);                                               /* the ordinary entry collapses onto the static red */
        GR(O_SETPIXEL, dc, 1, 3, LONG(0x01000000u | 2));
        GR(O_SETPIXEL, dc, 1, 4, LONG(0x01000000u | 3));
        CHECK(g_px(1, 3) != g_px(1, 4));                                         /* reserved entries are never shared */
        /* AnimatePalette changes the displayed colour of the reserved entries, not the pixels */
        {
            uint8_t pxbefore = g_px(1, 3);
            memcpy(umem(s, 0x500, 4), (const uint8_t[]){77, 88, 99, PC_RESERVED}, 4);
            GA(O_ANIMATEPALETTE, p, 2, 1, FARP(s, 0x500));
            CHECK_EQ(g_px(1, 3), pxbefore);
            CHECK_EQ(sys_rgb(pxbefore), (uint32_t)(77 << 16 | 88 << 8 | 99));
            CHECK_EQ(GR(O_GETPIXEL, dc, 1, 3), (uint32_t)(77 | 88 << 8 | 99 << 16));
            memcpy(umem(s, 0x500, 4), (const uint8_t[]){5, 5, 5, 0}, 4);          /* a non-reserved entry is not animated */
            GA(O_ANIMATEPALETTE, p, 1, 1, FARP(s, 0x500));
            GR(O_SETPIXEL, dc, 1, 2, LONG(0x01000000u | 1));
            CHECK_EQ(g_px(1, 2), 249);
            CHECK_EQ(sys_rgb(249), 0xFF0000u);
        }
    }
    /* PC_EXPLICIT: the entry's first byte is a system palette index */
    {
        static const uint8_t e[2][4] = {{7, 0, 0, PC_EXPLICIT}, {250, 0, 0, PC_EXPLICIT}};
        logpal(s, 0x100, 2, e);
        p = GA(O_CREATEPALETTE, FARP(s, 0x100));
        UA(U_SELECTPALETTE, dc, p, 0);
        UA(U_REALIZEPALETTE, dc);
        GR(O_SETPIXEL, dc, 2, 1, LONG(0x01000000u | 0));
        CHECK_EQ(g_px(2, 1), 7);
        GR(O_SETPIXEL, dc, 2, 2, LONG(0x01000000u | 1));
        CHECK_EQ(g_px(2, 2), 250);
    }
    gdown();
}

static void test_exhaustion_and_static_use(void) {
    uint16_t s, p, dc;
    int i;
    printf("test_pal_exhaustion\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x1000);
    /* 236 entries are free with static colours: the 237th colour maps to the nearest existing one */
    {
        uint16_t n = 240;
        uwr16(s, 0, 0x300);
        uwr16(s, 2, n);
        for (i = 0; i < n; i++) {
            uint8_t *e = umem(s, 4 + 4u * (unsigned)i, 4);
            e[0] = (uint8_t)(i + 1);
            e[1] = (uint8_t)(255 - i);
            e[2] = (uint8_t)((i * 7) & 0xFF) | 1;
            e[3] = 0;
        }
    }
    p = GA(O_CREATEPALETTE, FARP(s, 0));
    CHECK(p != 0);
    UA(U_SELECTPALETTE, dc, p, 0);
    CHECK_EQ(UA(U_REALIZEPALETTE, dc), 240);
    GR(O_SETPIXEL, dc, 3, 3, LONG(0x01000000u | 239));
    {
        int used = 0;
        for (i = 10; i < 246; i++) used += (g_fb()->pal[i] & 0xFFFFFF) != 0;
        CHECK(used >= 230);                                                      /* every free entry was taken */
    }
    /* SYSPAL_NOSTATIC frees the static colours except black and white */
    CHECK_EQ(GA(O_GETSYSTEMPALETTEUSE, dc), 1);
    CHECK_EQ(GA(O_SETSYSTEMPALETTEUSE, dc, 2), 1);
    CHECK_EQ(GA(O_GETSYSTEMPALETTEUSE, dc), 2);
    CHECK_EQ(GA(O_SETSYSTEMPALETTEUSE, dc, 1), 2);
    /* GetSystemPaletteEntries */
    CHECK_EQ(GA(O_GETSYSTEMPALETTEENTRIES, dc, 250, 6, FARP(s, 0x800)), 6);
    CHECK_EQ(rgb3(umem(s, 0x800, 4)), sys_rgb(250));
    CHECK_EQ(GA(O_GETSYSTEMPALETTEENTRIES, dc, 254, 10, FARP(s, 0x800)), 2);       /* clipped at 256 */
    gdown();
}

static void test_nearest(void) {
    uint16_t s, p, dc;
    printf("test_pal_nearest\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x400);
    logpal(s, 0, 8, odd8);
    p = GA(O_CREATEPALETTE, FARP(s, 0));
    CHECK_EQ(GA(O_GETNEARESTPALETTEINDEX, p, LONG(60 | 70 << 8 | 80 << 16)), 2);   /* (60,70,80) is closest to (70,80,90) */
    CHECK_EQ(GA(O_GETNEARESTPALETTEINDEX, p, LONG(0)), 0);
    CHECK_EQ(GA(O_GETNEARESTPALETTEINDEX, p, LONG(255 | 255 << 8 | 255 << 16)), 7);
    CHECK_EQ(GA(O_GETNEARESTPALETTEINDEX, 0x7777, LONG(0)), 0xFFFF);                 /* CLR_INVALID, 16 bits */
    /* GetNearestColor: the colour the device would really use */
    CHECK_EQ(GR(O_GETNEARESTCOLOR, dc, LONG(0x0000FF)), 0x0000FF);                  /* red is static */
    CHECK_EQ(GR(O_GETNEARESTCOLOR, dc, LONG(250 | 1 << 8 | 2 << 16)), 0x0000FF);    /* near red */
    CHECK_EQ(GR(O_GETNEARESTCOLOR, dc, LONG(5 | 5 << 8 | 5 << 16)), 0x000000);
    gdown();
}

static void test_dib_pal_colors(void) {
    uint16_t s, p, dc;
    printf("test_pal_dib_pal_colors\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x800);
    logpal(s, 0x200, 8, odd8);
    p = GA(O_CREATEPALETTE, FARP(s, 0x200));
    UA(U_SELECTPALETTE, dc, p, 0);
    UA(U_REALIZEPALETTE, dc);
    /* a 2x1 8 bpp DIB whose colour table is two WORD indices into the selected palette */
    uwr32(s, 0, 40); uwr32(s, 4, 2); uwr32(s, 8, 1); uwr16(s, 12, 1); uwr16(s, 14, 8); uwr32(s, 16, 0);
    uwr32(s, 20, 4); uwr32(s, 24, 0); uwr32(s, 28, 0); uwr32(s, 32, 2); uwr32(s, 36, 0);
    uwr16(s, 40, 3); uwr16(s, 42, 5);
    umem(s, 0x100, 4)[0] = 0; umem(s, 0x100, 4)[1] = 1; umem(s, 0x100, 4)[2] = 0; umem(s, 0x100, 4)[3] = 0;
    g_clear(0x03);
    GA(O_STRETCHDIBITS, dc, 30, 30, 2, 1, 0, 0, 2, 1, FARP(s, 0x100), FARP(s, 0), 1, LONG(0x00CC0020u));
    CHECK_EQ(g_px(30, 30), 13);                                                    /* entry 3 maps to system entry 13 */
    CHECK_EQ(g_px(31, 30), 15);
    /* the same DIB with RGB colours goes through the nearest-colour search instead */
    uwr32(s, 40, 0); uwr32(s, 44, 0);
    memcpy(umem(s, 40, 8), (const uint8_t[]){90, 80, 70, 0, 120, 110, 100, 0}, 8);  /* B G R 0: (70,80,90) and (100,110,120) */
    g_clear(0x03);
    GA(O_STRETCHDIBITS, dc, 30, 30, 2, 1, 0, 0, 2, 1, FARP(s, 0x100), FARP(s, 0), 0, LONG(0x00CC0020u));
    CHECK_EQ(g_px(30, 30), 12);
    CHECK_EQ(g_px(31, 30), 13);
    gdown();
}

int main(void) {
    test_logical_palette();
    test_realize();
    test_flags_and_animation();
    test_exhaustion_and_static_use();
    test_nearest();
    test_dib_pal_colors();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
