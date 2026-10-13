/* GDI object table, stock objects, DC state, device caps and coordinate mapping
 *. Every call goes through real guest code (gfixture.h). */
#include "gfixture.h"

/* ordinals (Win 3.1 export list; WRITER/ARTIST import all of them but the
 * ones marked UNV in src/gdi.c) */
enum {
    O_SETBKCOLOR = 1, O_SETBKMODE = 2, O_SETMAPMODE = 3, O_SETROP2 = 4, O_SETPOLYFILLMODE = 6,
    O_SETSTRETCHBLTMODE = 7, O_SETTEXTCHAREXTRA = 8, O_SETTEXTCOLOR = 9, O_SETWINDOWORG = 11,
    O_SETWINDOWEXT = 12, O_SETVIEWPORTORG = 13, O_SETVIEWPORTEXT = 14, O_OFFSETWINDOWORG = 15,
    O_SCALEWINDOWEXT = 16, O_OFFSETVIEWPORTORG = 17, O_SCALEVIEWPORTEXT = 18, O_MOVETO = 20,
    O_SAVEDC = 30, O_RESTOREDC = 39, O_SELECTOBJECT = 45, O_CREATECOMPATIBLEBITMAP = 51,
    O_CREATECOMPATIBLEDC = 52, O_CREATEDC = 53, O_CREATEBRUSHINDIRECT = 50, O_CREATEFONT = 56,
    O_CREATEFONTINDIRECT = 57, O_CREATEHATCHBRUSH = 58, O_CREATEPEN = 61, O_CREATEPENINDIRECT = 62,
    O_CREATESOLIDBRUSH = 66, O_DPTOLP = 67, O_DELETEDC = 68, O_DELETEOBJECT = 69, O_GETBKCOLOR = 75,
    O_GETBKMODE = 76, O_GETCURRENTPOSITION = 78, O_GETDEVICECAPS = 80, O_GETMAPMODE = 81,
    O_GETOBJECT = 82, O_GETPOLYFILLMODE = 84, O_GETROP2 = 86, O_GETSTOCKOBJECT = 87,
    O_GETSTRETCHBLTMODE = 88, O_GETTEXTCHAREXTRA = 89, O_GETTEXTCOLOR = 90, O_GETVIEWPORTEXT = 94,
    O_GETVIEWPORTORG = 95, O_GETWINDOWEXT = 96, O_GETWINDOWORG = 97, O_LPTODP = 99, O_SETBRUSHORG = 148,
    O_UNREALIZEOBJECT = 150, O_CREATEIC = 153, O_GETTEXTALIGN = 345, O_SETTEXTALIGN = 346,
    O_ISGDIOBJECT = 462, O_GETCURRENTPOSITIONEX = 470, O_GETVIEWPORTEXTEX = 472,
    O_GETVIEWPORTORGEX = 473, O_GETWINDOWEXTEX = 474, O_GETWINDOWORGEX = 475,
    O_SETVIEWPORTEXTEX = 479, O_SETVIEWPORTORGEX = 480, O_MULDIV = 128
};

static uint16_t lo(uint32_t v) { return (uint16_t)v; }
static uint16_t hi(uint32_t v) { return (uint16_t)(v >> 16); }

static void test_stock(void) {
    uint16_t h[20];
    int i, j;
    printf("test_gdi_stock\n");
    gboot();
    for (i = 0; i <= 16; i++) {
        h[i] = GA(O_GETSTOCKOBJECT, (uint16_t)i);
        if (i == 9) CHECK_EQ(h[i], 0);          /* 9 is not a stock object */
        else CHECK(h[i] != 0);
    }
    for (i = 0; i <= 16; i++)
        for (j = i + 1; j <= 16; j++)
            if (i != 9 && j != 9) CHECK(h[i] != h[j]);
    CHECK_EQ(GA(O_GETSTOCKOBJECT, 4), h[4]);   /* the same handle every time */
    CHECK_EQ(GA(O_GETSTOCKOBJECT, 17), 0);
    CHECK_EQ(GA(O_GETSTOCKOBJECT, 0xFFFF), 0);
    {
        uint16_t s = g_seg(0x100);
        static const struct { int idx; uint16_t style; uint32_t color; } br[] = {
            {0, 0, 0xFFFFFF}, {1, 0, 0xC0C0C0}, {2, 0, 0x808080}, {3, 0, 0x404040}, {4, 0, 0x000000}, {5, 1, 0}};
        for (i = 0; i < 6; i++) {          /* LOGBRUSH: style, color, hatch */
            CHECK_EQ(GR(O_GETOBJECT, h[br[i].idx], 8, FARP(s, 0)), 8);
            CHECK_EQ(urd16(s, 0), br[i].style);
            CHECK_EQ(urd32(s, 2), br[i].color);
        }
        for (i = 6; i <= 8; i++) {         /* WHITE_PEN BLACK_PEN NULL_PEN: LOGPEN style, width x/y, color */
            CHECK_EQ(GR(O_GETOBJECT, h[i], 10, FARP(s, 0)), 10);
            CHECK_EQ(urd16(s, 0), i == 8 ? 5 : 0);
            CHECK_EQ(urd32(s, 6), i == 6 ? 0xFFFFFF : 0);
        }
        CHECK_EQ(GR(O_GETOBJECT, h[13], 50, FARP(s, 0)), 50);   /* SYSTEM_FONT */
        CHECK(strncmp((char *)umem(s, 18, 8), "System", 7) == 0);
        CHECK_EQ(GR(O_GETOBJECT, h[15], 2, FARP(s, 0)), 2);     /* DEFAULT_PALETTE: entry count */
        CHECK_EQ(urd16(s, 0), 20);
        CHECK_EQ(GR(O_GETOBJECT, 0x7777, 8, FARP(s, 0)), 0);    /* not a handle */
    }
    gdown();
}

/* gdi_stats: live GDI objects and device contexts, for the freeze diagnostics (leaks show up as a growing count). */
static void test_stats(void) {
    unsigned obj0, dc0, obj1, dc1;
    uint16_t hb, hdc;
    printf("test_gdi_stats\n");
    gboot();
    gdi_stats(gg, &obj0, &dc0);
    hb = GA(O_CREATESOLIDBRUSH, LONG(0x0000FF00));
    hdc = GA(O_CREATECOMPATIBLEDC, 0);
    CHECK(hb != 0 && hdc != 0);
    gdi_stats(gg, &obj1, &dc1);
    CHECK_EQ(obj1, obj0 + 2);                            /* the brush and the memory DC are objects */
    CHECK_EQ(dc1, dc0 + 1);
    GA(O_DELETEDC, hdc);
    GA(O_DELETEOBJECT, hb);
    gdi_stats(gg, &obj1, &dc1);
    CHECK_EQ(obj1, obj0);
    CHECK_EQ(dc1, dc0);
    gdown();
}

static void test_pens_brushes(void) {
    uint16_t s, hp, hb, hdc;
    printf("test_gdi_pens_brushes\n");
    gboot();
    s = g_seg(0x100);
    hp = GA(O_CREATEPEN, 2, 3, LONG(0x00123456));
    CHECK(hp != 0);
    CHECK_EQ(GR(O_GETOBJECT, hp, 10, FARP(s, 0)), 10);
    CHECK_EQ(urd16(s, 0), 2);
    CHECK_EQ(urd16(s, 2), 3);
    CHECK_EQ(urd32(s, 6), 0x123456);
    uwr16(s, 0x20, 1); uwr16(s, 0x22, 7); uwr16(s, 0x24, 0); uwr32(s, 0x26, 0x00FF0080);
    hp = GA(O_CREATEPENINDIRECT, FARP(s, 0x20));
    CHECK(hp != 0);
    CHECK_EQ(GR(O_GETOBJECT, hp, 10, FARP(s, 0x40)), 10);
    CHECK_EQ(urd16(s, 0x40), 1);
    CHECK_EQ(urd16(s, 0x42), 7);
    CHECK_EQ(urd32(s, 0x46), 0xFF0080);
    CHECK_EQ(GR(O_GETOBJECT, hp, 4, FARP(s, 0x60)), 4);          /* cb smaller than the struct: truncated */
    hb = GA(O_CREATESOLIDBRUSH, LONG(0x0000FF00));
    CHECK(hb != 0);
    CHECK_EQ(GR(O_GETOBJECT, hb, 8, FARP(s, 0)), 8);
    CHECK_EQ(urd16(s, 0), 0);
    CHECK_EQ(urd32(s, 2), 0x00FF00);
    hb = GA(O_CREATEHATCHBRUSH, 3, LONG(0x00000FF0));
    CHECK_EQ(GR(O_GETOBJECT, hb, 8, FARP(s, 0)), 8);
    CHECK_EQ(urd16(s, 0), 2);
    CHECK_EQ(urd16(s, 6), 3);
    CHECK_EQ(GA(O_CREATEHATCHBRUSH, 6, LONG(0)), 0);         /* hatch styles 0..5 */
    uwr16(s, 0x80, 0); uwr32(s, 0x82, 0x112233); uwr16(s, 0x86, 0);
    hb = GA(O_CREATEBRUSHINDIRECT, FARP(s, 0x80));
    CHECK(hb != 0);
    CHECK_EQ(GR(O_GETOBJECT, hb, 8, FARP(s, 0x90)), 8);
    CHECK_EQ(urd32(s, 0x92), 0x112233);
    /* selection returns the previous object of the kind */
    hdc = g_screen_dc();
    CHECK(hdc != 0);
    CHECK_EQ(GA(O_SELECTOBJECT, hdc, hp), GA(O_GETSTOCKOBJECT, 7));        /* default pen: BLACK_PEN */
    CHECK_EQ(GA(O_SELECTOBJECT, hdc, hb), GA(O_GETSTOCKOBJECT, 0));        /* default brush: WHITE_BRUSH */
    CHECK_EQ(GA(O_SELECTOBJECT, hdc, GA(O_GETSTOCKOBJECT, 13)), GA(O_GETSTOCKOBJECT, 13)); /* default font: SYSTEM_FONT */
    CHECK_EQ(GA(O_SELECTOBJECT, hdc, 0x7777), 0);
    CHECK_EQ(GA(O_SELECTOBJECT, hdc, GA(O_GETSTOCKOBJECT, 8)), hp);
    /* deletion: selected objects stay, others go, handles die */
    CHECK_EQ(GA(O_DELETEOBJECT, hb), 0);
    CHECK_EQ(GA(O_SELECTOBJECT, hdc, GA(O_GETSTOCKOBJECT, 0)), hb);
    CHECK_EQ(GA(O_DELETEOBJECT, hb), 1);
    CHECK_EQ(GA(O_DELETEOBJECT, hb), 0);
    CHECK_EQ(GR(O_GETOBJECT, hb, 8, FARP(s, 0)), 0);
    CHECK_EQ(GA(O_ISGDIOBJECT, hp), 1);
    CHECK_EQ(GA(O_ISGDIOBJECT, hb), 0);
    /* a DC lives in the GDI heap too: Creative Writer asks IsGDIObject(hdc) before deleting a memory DC's objects,
     * and with 0 it never freed them (GDI table exhausted, "I am almost out of space!") */
    {
        uint16_t mdc2 = GA(O_CREATECOMPATIBLEDC, hdc);
        CHECK_EQ(GA(O_ISGDIOBJECT, mdc2), 1);
        CHECK_EQ(GA(O_DELETEDC, mdc2), 1);
        CHECK_EQ(GA(O_ISGDIOBJECT, mdc2), 0);
        CHECK_EQ(GA(O_ISGDIOBJECT, hdc), 1);                 /* a window/screen DC handle too */
    }
    CHECK_EQ(GA(O_DELETEOBJECT, GA(O_GETSTOCKOBJECT, 4)), 1);           /* stock objects: reported deleted, kept */
    CHECK(GA(O_GETSTOCKOBJECT, 4) != 0);
    CHECK_EQ(GA(O_UNREALIZEOBJECT, hp), 1);
    gdown();
}

static void test_fonts(void) {
    uint16_t s, hf;
    printf("test_gdi_fonts_objects\n");
    gboot();
    s = g_seg(0x200);
    memset(umem(s, 0, 0x100), 0, 0x100);
    /* LOGFONT: 5 ints, italic underline strikeout charset outprec clipprec quality pitch, face[32] */
    uwr16(s, 0, (uint16_t)-20);
    uwr16(s, 8, 700);
    umem(s, 10, 1)[0] = 1;
    umem(s, 17, 1)[0] = 0x22;
    strcpy((char *)umem(s, 18, 1), "Arial");
    hf = GA(O_CREATEFONTINDIRECT, FARP(s, 0));
    CHECK(hf != 0);
    CHECK_EQ(GR(O_GETOBJECT, hf, 50, FARP(s, 0x100)), 50);
    CHECK_EQ(urd16(s, 0x100), (uint16_t)-20);
    CHECK_EQ(urd16(s, 0x100 + 8), 700);
    CHECK_EQ(umem(s, 0x100 + 10, 1)[0], 1);
    CHECK_EQ(umem(s, 0x100 + 17, 1)[0], 0x22);
    CHECK(strncmp((char *)umem(s, 0x100 + 18, 6), "Arial", 6) == 0);
    /* CreateFont pushes its 8 BYTE parameters as words, then the face name */
    {
        uint16_t f = g_seg(0x40);
        strcpy((char *)umem(f, 0, 1), "Times");
        hf = GA(O_CREATEFONT, 24, 0, 0, 0, 400, 0, 1, 0, 0, 0, 0, 0, 0x12, FARP(f, 0));
        CHECK(hf != 0);
        CHECK_EQ(GR(O_GETOBJECT, hf, 50, FARP(s, 0x100)), 50);
        CHECK_EQ(urd16(s, 0x100), 24);
        CHECK_EQ(urd16(s, 0x100 + 8), 400);
        CHECK_EQ(umem(s, 0x100 + 10, 1)[0], 0);
        CHECK_EQ(umem(s, 0x100 + 11, 1)[0], 1);                 /* underline */
        CHECK_EQ(umem(s, 0x100 + 17, 1)[0], 0x12);
        CHECK(strncmp((char *)umem(s, 0x100 + 18, 6), "Times", 6) == 0);
    }
    gdown();
}

static void test_dc_state(void) {
    uint16_t hdc, mdc, s;
    printf("test_gdi_dc_state\n");
    gboot();
    hdc = g_screen_dc();
    s = g_seg(0x100);
    /* defaults */
    CHECK_EQ(GR(O_GETBKCOLOR, hdc), 0xFFFFFF);
    CHECK_EQ(GR(O_GETTEXTCOLOR, hdc), 0);
    CHECK_EQ(GA(O_GETBKMODE, hdc), 2);
    CHECK_EQ(GA(O_GETROP2, hdc), 13);
    CHECK_EQ(GA(O_GETMAPMODE, hdc), 1);
    CHECK_EQ(GA(O_GETPOLYFILLMODE, hdc), 1);
    CHECK_EQ(GA(O_GETSTRETCHBLTMODE, hdc), 1);
    CHECK_EQ(GA(O_GETTEXTALIGN, hdc), 0);
    CHECK_EQ(GA(O_GETTEXTCHAREXTRA, hdc), 0);
    /* setters return the previous value */
    CHECK_EQ(GR(O_SETBKCOLOR, hdc, LONG(0x00112233)), 0xFFFFFF);
    CHECK_EQ(GR(O_GETBKCOLOR, hdc), 0x112233);
    CHECK_EQ(GR(O_SETTEXTCOLOR, hdc, LONG(0x00445566)), 0);
    CHECK_EQ(GR(O_GETTEXTCOLOR, hdc), 0x445566);
    CHECK_EQ(GA(O_SETBKMODE, hdc, 1), 2);
    CHECK_EQ(GA(O_GETBKMODE, hdc), 1);
    CHECK_EQ(GA(O_SETBKMODE, hdc, 9), 0);          /* invalid mode: refused */
    CHECK_EQ(GA(O_GETBKMODE, hdc), 1);
    CHECK_EQ(GA(O_SETROP2, hdc, 7), 13);
    CHECK_EQ(GA(O_GETROP2, hdc), 7);
    CHECK_EQ(GA(O_SETROP2, hdc, 17), 0);
    CHECK_EQ(GA(O_GETROP2, hdc), 7);
    CHECK_EQ(GA(O_SETPOLYFILLMODE, hdc, 2), 1);
    CHECK_EQ(GA(O_SETSTRETCHBLTMODE, hdc, 3), 1);
    CHECK_EQ(GA(O_SETTEXTALIGN, hdc, 6 | 24), 0);
    CHECK_EQ(GA(O_GETTEXTALIGN, hdc), 30);
    CHECK_EQ(GA(O_SETTEXTCHAREXTRA, hdc, 3), 0);
    CHECK_EQ(GA(O_GETTEXTCHAREXTRA, hdc), 3);
    CHECK_EQ(GR(O_SETBRUSHORG, hdc, 5, 6), 0);
    CHECK_EQ(GR(O_SETBRUSHORG, hdc, 0, 0), 5u | 6u << 16);
    /* current position: MoveTo returns the old one, x in the low word */
    CHECK_EQ(GR(O_MOVETO, hdc, 10, 20), 0);
    CHECK_EQ(GR(O_MOVETO, hdc, 30, 40), 10u | 20u << 16);
    CHECK_EQ(GR(O_GETCURRENTPOSITION, hdc), 30u | 40u << 16);
    CHECK_EQ(GA(O_GETCURRENTPOSITIONEX, hdc, FARP(s, 0)), 1);
    CHECK_EQ(urd16(s, 0), 30);
    CHECK_EQ(urd16(s, 2), 40);
    /* a different DC has its own state */
    mdc = GA(O_CREATECOMPATIBLEDC, hdc);
    CHECK(mdc != 0);
    CHECK(mdc != hdc);
    CHECK_EQ(GR(O_GETBKCOLOR, mdc), 0xFFFFFF);
    CHECK_EQ(GA(O_GETROP2, mdc), 13);
    CHECK_EQ(GA(O_DELETEDC, mdc), 1);
    CHECK_EQ(GR(O_GETBKCOLOR, mdc), 0);          /* dead handle */
    CHECK_EQ(GA(O_DELETEDC, mdc), 0);
    /* a screen DC is released and recycled: state does not leak */
    UR(68, 0, hdc);                               /* ReleaseDC(0, hdc) */
    hdc = g_screen_dc();
    CHECK_EQ(GR(O_GETBKCOLOR, hdc), 0xFFFFFF);
    CHECK_EQ(GA(O_GETBKMODE, hdc), 2);
    gdown();
}

static void test_save_restore(void) {
    uint16_t hdc, hp, hb;
    printf("test_gdi_savedc\n");
    gboot();
    hdc = g_screen_dc();
    hp = GA(O_CREATEPEN, 0, 1, LONG(0xFF));
    hb = GA(O_CREATESOLIDBRUSH, LONG(0xFF00));
    CHECK_EQ(GA(O_SAVEDC, hdc), 1);
    GA(O_SELECTOBJECT, hdc, hp);
    GA(O_SELECTOBJECT, hdc, hb);
    GR(O_SETBKCOLOR, hdc, LONG(0x123456));
    GA(O_SETROP2, hdc, 3);
    GA(O_SETMAPMODE, hdc, 8);
    GR(O_SETWINDOWORG, hdc, 7, 7);
    CHECK_EQ(GA(O_SAVEDC, hdc), 2);
    GA(O_SETROP2, hdc, 5);
    CHECK_EQ(GA(O_RESTOREDC, hdc, (uint16_t)-1), 1);          /* back to level 2's state */
    CHECK_EQ(GA(O_GETROP2, hdc), 3);
    CHECK_EQ(GR(O_GETBKCOLOR, hdc), 0x123456);
    CHECK_EQ(GA(O_RESTOREDC, hdc, 1), 1);                      /* absolute level 1: the default state */
    CHECK_EQ(GA(O_GETROP2, hdc), 13);
    CHECK_EQ(GR(O_GETBKCOLOR, hdc), 0xFFFFFF);
    CHECK_EQ(GA(O_GETMAPMODE, hdc), 1);
    CHECK_EQ(GR(O_GETWINDOWORG, hdc), 0);
    CHECK_EQ(GA(O_SELECTOBJECT, hdc, GA(O_GETSTOCKOBJECT, 7)), GA(O_GETSTOCKOBJECT, 7));   /* pen is the default again */
    CHECK_EQ(GA(O_RESTOREDC, hdc, (uint16_t)-1), 0);           /* nothing saved any more */
    CHECK_EQ(GA(O_DELETEOBJECT, hp), 1);                       /* no longer selected anywhere */
    CHECK_EQ(GA(O_DELETEOBJECT, hb), 1);
    gdown();
}

static void test_caps(void) {
    uint16_t hdc;
    printf("test_gdi_devicecaps\n");
    gboot();
    hdc = g_screen_dc();
    CHECK_EQ(GA(O_GETDEVICECAPS, hdc, 0), 0x0300);
    CHECK_EQ(GA(O_GETDEVICECAPS, hdc, 2), 1);        /* TECHNOLOGY: DT_RASDISPLAY */
    CHECK_EQ(GA(O_GETDEVICECAPS, hdc, 8), 640);      /* HORZRES */
    CHECK_EQ(GA(O_GETDEVICECAPS, hdc, 10), 480);     /* VERTRES */
    CHECK_EQ(GA(O_GETDEVICECAPS, hdc, 12), 8);       /* BITSPIXEL */
    CHECK_EQ(GA(O_GETDEVICECAPS, hdc, 14), 1);       /* PLANES */
    CHECK_EQ(GA(O_GETDEVICECAPS, hdc, 24), 20);      /* NUMCOLORS: the static colours */
    CHECK_EQ(GA(O_GETDEVICECAPS, hdc, 88), 96);      /* LOGPIXELSX */
    CHECK_EQ(GA(O_GETDEVICECAPS, hdc, 90), 96);
    CHECK_EQ(GA(O_GETDEVICECAPS, hdc, 104), 256);    /* SIZEPALETTE */
    CHECK_EQ(GA(O_GETDEVICECAPS, hdc, 106), 20);     /* NUMRESERVED */
    CHECK_EQ(GA(O_GETDEVICECAPS, hdc, 36), 1);       /* CLIPCAPS: CP_RECTANGLE */
    {
        uint16_t rc = GA(O_GETDEVICECAPS, hdc, 38);   /* RASTERCAPS */
        CHECK(rc & 0x0001);     /* RC_BITBLT */
        CHECK(rc & 0x0008);     /* RC_BITMAP64 */
        CHECK(rc & 0x0080);     /* RC_DI_BITMAP */
        CHECK(rc & 0x0100);     /* RC_PALETTE */
        CHECK(rc & 0x0200);     /* RC_DIBTODEV */
        CHECK(rc & 0x0800);     /* RC_STRETCHBLT */
        CHECK(rc & 0x1000);     /* RC_FLOODFILL */
        CHECK(rc & 0x2000);     /* RC_STRETCHDIB */
        CHECK(!(rc & 0x0002));  /* not a banding device */
    }
    CHECK_EQ(GA(O_GETDEVICECAPS, hdc, 40), 36);      /* ASPECTX/Y square pixels */
    CHECK_EQ(GA(O_GETDEVICECAPS, hdc, 42), 36);
    CHECK(GA(O_GETDEVICECAPS, hdc, 4) > 0);          /* HORZSIZE mm */
    CHECK_EQ(GA(O_GETDEVICECAPS, hdc, 999), 0);      /* unknown index */
    /* a memory DC reports the same display, its bitmap decides the rest only for BITSPIXEL */
    {
        uint16_t m = GA(O_CREATECOMPATIBLEDC, hdc);
        CHECK_EQ(GA(O_GETDEVICECAPS, m, 8), 640);
        CHECK_EQ(GA(O_GETDEVICECAPS, m, 88), 96);
    }
    gdown();
}

static void test_mapping(void) {
    uint16_t hdc, s;
    int hs, vs, y;
    printf("test_gdi_mapping\n");
    gboot();
    hdc = g_screen_dc();
    s = g_seg(0x100);
    CHECK_EQ(GR(O_GETWINDOWEXT, hdc), 1u | 1u << 16);
    CHECK_EQ(GR(O_GETVIEWPORTEXT, hdc), 1u | 1u << 16);
    /* MM_TEXT: the extents cannot be changed */
    CHECK_EQ(GR(O_SETWINDOWEXT, hdc, 5, 5), 1u | 1u << 16);
    CHECK_EQ(GR(O_GETWINDOWEXT, hdc), 1u | 1u << 16);
    /* origins work in every mode */
    CHECK_EQ(GR(O_SETVIEWPORTORG, hdc, 100, 50), 0);
    CHECK_EQ(GR(O_SETWINDOWORG, hdc, 10, 20), 0);
    CHECK_EQ(GR(O_GETVIEWPORTORG, hdc), 100u | 50u << 16);
    CHECK_EQ(GR(O_GETWINDOWORG, hdc), 10u | 20u << 16);
    uwr16(s, 0, 15); uwr16(s, 2, 25);
    CHECK_EQ(GA(O_LPTODP, hdc, FARP(s, 0), 1), 1);
    CHECK_EQ((int16_t)urd16(s, 0), 15 - 10 + 100);
    CHECK_EQ((int16_t)urd16(s, 2), 25 - 20 + 50);
    CHECK_EQ(GA(O_DPTOLP, hdc, FARP(s, 0), 1), 1);
    CHECK_EQ(urd16(s, 0), 15);
    CHECK_EQ(urd16(s, 2), 25);
    /* anisotropic scaling */
    CHECK_EQ(GA(O_SETMAPMODE, hdc, 8), 1);
    GR(O_SETWINDOWEXT, hdc, 100, 50);
    GR(O_SETVIEWPORTEXT, hdc, 200, 100);
    GR(O_SETWINDOWORG, hdc, 10, 10);
    GR(O_SETVIEWPORTORG, hdc, 5, 5);
    uwr16(s, 0, 30); uwr16(s, 2, 30); uwr16(s, 4, 0xFFFF); uwr16(s, 6, 0xFFF0);
    GA(O_LPTODP, hdc, FARP(s, 0), 2);
    CHECK_EQ((int16_t)urd16(s, 0), (30 - 10) * 200 / 100 + 5);
    CHECK_EQ((int16_t)urd16(s, 2), (30 - 10) * 100 / 50 + 5);
    CHECK_EQ((int16_t)urd16(s, 4), (-1 - 10) * 200 / 100 + 5);
    CHECK_EQ((int16_t)urd16(s, 6), (-16 - 10) * 100 / 50 + 5);
    GA(O_DPTOLP, hdc, FARP(s, 0), 1);
    CHECK_EQ((int16_t)urd16(s, 0), 30);
    /* the Ex forms return the previous value through a pointer */
    CHECK_EQ(GA(O_SETVIEWPORTEXTEX, hdc, 400, 300, FARP(s, 0x10)), 1);
    CHECK_EQ(urd16(s, 0x10), 200);
    CHECK_EQ(urd16(s, 0x12), 100);
    CHECK_EQ(GA(O_GETVIEWPORTEXTEX, hdc, FARP(s, 0x10)), 1);
    CHECK_EQ(urd16(s, 0x10), 400);
    CHECK_EQ(GA(O_SETVIEWPORTORGEX, hdc, 1, 2, FARP(s, 0x10)), 1);
    CHECK_EQ(urd16(s, 0x10), 5);
    CHECK_EQ(GA(O_GETWINDOWORGEX, hdc, FARP(s, 0x10)), 1);
    CHECK_EQ(urd16(s, 0x10), 10);
    CHECK_EQ(GA(O_GETWINDOWEXTEX, hdc, FARP(s, 0x10)), 1);
    CHECK_EQ(urd16(s, 0x10), 100);
    /* fixed modes: LOMETRIC has y pointing up, scaled by the display size */
    CHECK_EQ(GA(O_SETMAPMODE, hdc, 2), 8);
    GR(O_SETWINDOWORG, hdc, 0, 0);
    GR(O_SETVIEWPORTORG, hdc, 0, 0);
    hs = GA(O_GETDEVICECAPS, hdc, 4);
    vs = GA(O_GETDEVICECAPS, hdc, 6);
    uwr16(s, 0, 100); uwr16(s, 2, 100);
    GA(O_LPTODP, hdc, FARP(s, 0), 1);
    CHECK_EQ((int16_t)urd16(s, 0), 100 * 640 / (hs * 10));
    y = 100 * -480 / (vs * 10);
    CHECK_EQ((int16_t)urd16(s, 2), y);
    CHECK(y < 0);
    /* TWIPS: 1440 per inch, 96 pixels per inch */
    GA(O_SETMAPMODE, hdc, 6);
    uwr16(s, 0, 1440); uwr16(s, 2, 0);
    GA(O_LPTODP, hdc, FARP(s, 0), 1);
    CHECK(abs((int16_t)urd16(s, 0) - 96) <= 1);
    /* relative moves */
    GA(O_SETMAPMODE, hdc, 8);
    GR(O_SETWINDOWORG, hdc, 0, 0);
    GR(O_SETVIEWPORTORG, hdc, 0, 0);
    CHECK_EQ(GR(O_OFFSETWINDOWORG, hdc, 3, 4), 0);
    CHECK_EQ(GR(O_OFFSETVIEWPORTORG, hdc, 5, 6), 0);
    CHECK_EQ(GR(O_GETWINDOWORG, hdc), 3u | 4u << 16);
    CHECK_EQ(GR(O_GETVIEWPORTORG, hdc), 5u | 6u << 16);
    GR(O_SETWINDOWEXT, hdc, 10, 10);
    GR(O_SETVIEWPORTEXT, hdc, 10, 10);
    GR(O_SCALEWINDOWEXT, hdc, 1, 2, 3, 4);               /* ext = ext * num / denom */
    CHECK_EQ(GR(O_GETWINDOWEXT, hdc), 5u | 7u << 16);
    GR(O_SCALEVIEWPORTEXT, hdc, 3, 1, 1, 1);
    CHECK_EQ(GR(O_GETVIEWPORTEXT, hdc), 30u | 10u << 16);
    /* isotropic keeps the aspect: the smaller scale wins */
    GA(O_SETMAPMODE, hdc, 7);
    GR(O_SETWINDOWEXT, hdc, 100, 100);
    GR(O_SETVIEWPORTEXT, hdc, 400, 200);
    {
        uint32_t ve = GR(O_GETVIEWPORTEXT, hdc);
        CHECK_EQ(lo(ve), 200);
        CHECK_EQ(hi(ve), 200);
    }
    gdown();
}

static void test_muldiv_dc_create(void) {
    uint16_t dc;
    printf("test_gdi_muldiv_createdc\n");
    gboot();
    CHECK_EQ(GA(O_MULDIV, 10, 3, 4), 8);                 /* rounds to nearest: 7.5 -> 8 */
    CHECK_EQ(GA(O_MULDIV, (uint16_t)-10, 3, 4), (uint16_t)-8);
    CHECK_EQ(GA(O_MULDIV, 1, 1, 0), (uint16_t)-1);        /* divide by zero: -1 */
    CHECK_EQ(GA(O_MULDIV, 100, 96, 72), 133);
    {
        uint16_t s = g_seg(0x40);
        strcpy((char *)umem(s, 0, 1), "DISPLAY");
        dc = GA(O_CREATEDC, FARP(s, 0), FARP(0, 0), FARP(0, 0), FARP(0, 0));
        CHECK(dc != 0);
        CHECK_EQ(GA(O_GETDEVICECAPS, dc, 8), 640);
        CHECK_EQ(GA(O_DELETEDC, dc), 1);
        dc = GA(O_CREATEIC, FARP(s, 0), FARP(0, 0), FARP(0, 0), FARP(0, 0));
        CHECK(dc != 0);
        CHECK_EQ(GA(O_GETDEVICECAPS, dc, 12), 8);
        strcpy((char *)umem(s, 0x20, 1), "HPPCL5MS");
        CHECK_EQ(GA(O_CREATEDC, FARP(s, 0x20), FARP(0, 0), FARP(0, 0), FARP(0, 0)), 0);   /* no printers */
    }
    gdown();
}

int main(void) {
    test_stock();
    test_stats();
    test_pens_brushes();
    test_fonts();
    test_dc_state();
    test_save_restore();
    test_caps();
    test_mapping();
    test_muldiv_dc_create();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
