/* GDI drawing primitives through the DC clip, ROP2, pens and brushes.
 * Expected pixels come from independent per-pixel references (closed
 * form line, centre-sampled shapes), not from the code under test. */
#include <math.h>

#include "gfixture.h"

enum {
    O_SETROP2 = 4, O_SETBKMODE = 2, O_SETBKCOLOR = 1, O_SETPOLYFILLMODE = 6, O_SETVIEWPORTORG = 13,
    O_LINETO = 19, O_MOVETO = 20, O_INTERSECTCLIPRECT = 22, O_ARC = 23, O_ELLIPSE = 24, O_FLOODFILL = 25,
    O_PIE = 26, O_RECTANGLE = 27, O_ROUNDRECT = 28, O_SETPIXEL = 31, O_POLYGON = 36, O_POLYLINE = 37,
    O_FILLRGN = 40, O_FRAMERGN = 41, O_INVERTRGN = 42, O_PAINTRGN = 43, O_SELECTCLIPRGN = 44,
    O_SELECTOBJECT = 45, O_CREATEPEN = 61, O_CREATESOLIDBRUSH = 66, O_CREATEHATCHBRUSH = 58,
    O_CREATERECTRGN = 64, O_DELETEOBJECT = 69, O_GETPIXEL = 83, O_GETSTOCKOBJECT = 87,
    O_CREATEPOLYGONRGN = 63, O_EXTFLOODFILL = 372, O_POLYPOLYGON = 450, O_SETMAPMODE = 3,
    O_SETWINDOWEXT = 12, O_SETVIEWPORTEXT = 14, O_CHORD = 348, O_CREATEELLIPTICRGN = 54,
    U_FILLRECT = 81, U_INVERTRECT = 82, U_FRAMERECT = 83, U_DRAWFOCUSRECT = 466
};

#define W 640
#define H 480

static uint8_t bk_pixel;

static void fresh(void) {
    gboot();
    g_clear(0xF0);
    bk_pixel = 0xF0;
}

static uint8_t pix_of(uint32_t cref) { return (uint8_t)fb_pixel_of(g_fb(), cref); }

static uint16_t pen(int style, int width, uint32_t color) { return GA(O_CREATEPEN, (uint16_t)style, (uint16_t)width, LONG(color)); }
static uint16_t brush(uint32_t color) { return GA(O_CREATESOLIDBRUSH, LONG(color)); }
static void select_(uint16_t dc, uint16_t h) { GA(O_SELECTOBJECT, dc, h); }
static void stock(uint16_t dc, int idx) { GA(O_SELECTOBJECT, dc, GA(O_GETSTOCKOBJECT, (uint16_t)idx)); }

/* expectation image: 0x100 = untouched */
typedef struct { uint16_t v[H][W]; } expect;
static expect ex;

static void ex_clear(void) {
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) ex.v[y][x] = 0x100;
}
static void ex_put(int x, int y, uint8_t v) {
    if (x >= 0 && y >= 0 && x < W && y < H) ex.v[y][x] = v;
}

/* compares the whole framebuffer with the expectation; returns mismatches */
static int ex_compare(const char *what) {
    int x, y, bad = 0;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint8_t want = ex.v[y][x] == 0x100 ? bk_pixel : (uint8_t)ex.v[y][x];
            if (g_px(x, y) != want) {
                if (bad < 3) printf("  %s mismatch at (%d,%d): got %u want %u\n", what, x, y, g_px(x, y), want);
                bad++;
            }
        }
    CHECK_EQ(bad, 0);
    return bad;
}

static int fdiv(int a, int b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

/* closed form: the minor offset after k steps is ceil((2 k dMinor - dMajor) / (2 dMajor)) (ties stay) */
static void ref_line(int x0, int y0, int x1, int y1, bool inc_end, uint8_t v) {
    int dx = abs(x1 - x0), dy = abs(y1 - y0), sx = x1 >= x0 ? 1 : -1, sy = y1 >= y0 ? 1 : -1, k;
    int major = dx >= dy ? dx : dy, minor = dx >= dy ? dy : dx;
    for (k = 0; k <= major; k++) {
        int off = major ? -fdiv(-(2 * k * minor - major), 2 * major) : 0;
        int px = dx >= dy ? x0 + sx * k : x0 + sx * off, py = dx >= dy ? y0 + sy * off : y0 + sy * k;
        if (k == major && !inc_end) break;
        ex_put(px, py, v);
    }
}

static unsigned rng = 777;
static int rnd(int n) {
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 16) % (unsigned)n);
}

static void test_lines(void) {
    uint16_t dc, p;
    int i;
    printf("test_draw_lines\n");
    fresh();
    dc = g_screen_dc();
    /* default pen is black; the end point is not drawn */
    CHECK_EQ(GR(O_MOVETO, dc, 10, 10), 0);
    CHECK_EQ(GA(O_LINETO, dc, 20, 10), 1);
    ex_clear();
    ref_line(10, 10, 20, 10, false, 0);
    ex_compare("horizontal");
    CHECK_EQ(GR(20, dc, 0, 0), 20u | 10u << 16);               /* MoveTo returns the position LineTo left */
    /* random lines in all octants against the closed form */
    for (i = 0; i < 300; i++) {
        int x0 = 100 + rnd(60), y0 = 100 + rnd(60), x1 = 100 + rnd(60), y1 = 100 + rnd(60);
        g_clear(0xF0);
        GR(O_MOVETO, dc, (uint16_t)x0, (uint16_t)y0);
        GA(O_LINETO, dc, (uint16_t)x1, (uint16_t)y1);
        ex_clear();
        ref_line(x0, y0, x1, y1, false, 0);
        if (ex_compare("random line")) printf("    line %d,%d -> %d,%d\n", x0, y0, x1, y1);
    }
    /* pen colour goes through the palette */
    g_clear(0xF0);
    p = pen(0, 1, 0x0000FF);
    select_(dc, p);
    GR(O_MOVETO, dc, 5, 5);
    GA(O_LINETO, dc, 5, 15);
    ex_clear();
    ref_line(5, 5, 5, 15, false, pix_of(0x0000FF));
    ex_compare("red vertical");
    /* a NULL pen draws nothing */
    g_clear(0xF0);
    stock(dc, 8);
    GR(O_MOVETO, dc, 5, 5);
    GA(O_LINETO, dc, 50, 50);
    ex_clear();
    ex_compare("null pen");
    /* a wide pen stamps a w x w square along the line */
    g_clear(0xF0);
    select_(dc, pen(0, 3, 0));
    GR(O_MOVETO, dc, 50, 50);
    GA(O_LINETO, dc, 70, 50);
    ex_clear();
    {
        int x, y;
        for (x = 49; x <= 70; x++)       /* stamps at 50..69 (the end point 70 is not drawn) cover 49..70 */
            for (y = 49; y <= 51; y++) ex_put(x, y, 0);
    }
    ex_compare("wide horizontal");
    /* coordinates are logical: the viewport origin moves the drawing */
    g_clear(0xF0);
    stock(dc, 7);
    GR(O_SETVIEWPORTORG, dc, 100, 200);
    GR(O_MOVETO, dc, 0, 0);
    GA(O_LINETO, dc, 10, 0);
    ex_clear();
    ref_line(100, 200, 110, 200, false, 0);
    ex_compare("viewport origin");
    GR(O_SETVIEWPORTORG, dc, 0, 0);
    gdown();
}

static void test_rop2(void) {
    uint16_t dc, p;
    int rop;
    printf("test_draw_rop2\n");
    fresh();
    dc = g_screen_dc();
    p = pen(0, 1, 0x0000FF);                       /* red */
    select_(dc, p);
    {
        uint8_t pv = pix_of(0x0000FF), dv = 0x35;
        for (rop = 1; rop <= 16; rop++) {
            uint8_t want;
            switch (rop) {
            case 1: want = 0; break;
            case 2: want = (uint8_t)~(dv | pv); break;
            case 3: want = dv & (uint8_t)~pv; break;
            case 4: want = (uint8_t)~pv; break;
            case 5: want = pv & (uint8_t)~dv; break;
            case 6: want = (uint8_t)~dv; break;
            case 7: want = pv ^ dv; break;
            case 8: want = (uint8_t)~(dv & pv); break;
            case 9: want = dv & pv; break;
            case 10: want = (uint8_t)~(pv ^ dv); break;
            case 11: want = dv; break;
            case 12: want = dv | (uint8_t)~pv; break;
            case 13: want = pv; break;
            case 14: want = pv | (uint8_t)~dv; break;
            case 15: want = pv | dv; break;
            default: want = 0xFF; break;
            }
            g_clear(dv);
            CHECK_EQ(GA(O_SETROP2, dc, (uint16_t)rop) != 0, 1);
            GR(O_MOVETO, dc, 30, 30);
            GA(O_LINETO, dc, 40, 30);
            CHECK_EQ(g_px(35, 30), want);
            CHECK_EQ(g_px(35, 31), dv);
        }
    }
    /* XOR drawn twice restores the surface; a polyline's shared vertices are hit once */
    GA(O_SETROP2, dc, 7);
    g_clear(0x55);
    {
        uint16_t pts = g_seg(0x40);
        uwr16(pts, 0, 100); uwr16(pts, 2, 100); uwr16(pts, 4, 140); uwr16(pts, 6, 100);
        uwr16(pts, 8, 140); uwr16(pts, 10, 140);
        select_(dc, pen(0, 1, 0xFFFFFF));
        GA(O_POLYLINE, dc, FARP(pts, 0), 3);
        CHECK_EQ(g_px(140, 100), (uint8_t)(0x55 ^ 0xFF));
        CHECK_EQ(g_px(120, 100), (uint8_t)(0x55 ^ 0xFF));
        CHECK_EQ(g_px(140, 120), (uint8_t)(0x55 ^ 0xFF));
        GA(O_POLYLINE, dc, FARP(pts, 0), 3);
        CHECK_EQ(g_px(140, 100), 0x55);
        CHECK_EQ(g_px(120, 100), 0x55);
    }
    gdown();
}

static void test_dash(void) {
    uint16_t dc;
    int x;
    printf("test_draw_dashes\n");
    fresh();
    dc = g_screen_dc();
    select_(dc, pen(1, 1, 0));                      /* PS_DASH: 18 on, 6 off */
    GA(O_SETBKMODE, dc, 1);                         /* TRANSPARENT: gaps untouched */
    GR(O_MOVETO, dc, 0, 10);
    GA(O_LINETO, dc, 96, 10);
    for (x = 0; x < 96; x++) CHECK_EQ(g_px(x, 10), x % 24 < 18 ? 0 : 0xF0);
    g_clear(0xF0);
    select_(dc, pen(2, 1, 0));                      /* PS_DOT: 3 on, 3 off */
    GR(O_MOVETO, dc, 0, 10);
    GA(O_LINETO, dc, 48, 10);
    for (x = 0; x < 48; x++) CHECK_EQ(g_px(x, 10), x % 6 < 3 ? 0 : 0xF0);
    g_clear(0xF0);
    GA(O_SETBKMODE, dc, 2);                         /* OPAQUE: gaps take the background colour */
    GR(O_SETBKCOLOR, dc, LONG(0x0000FF));
    GR(O_MOVETO, dc, 0, 10);
    GA(O_LINETO, dc, 48, 10);
    for (x = 0; x < 48; x++) CHECK_EQ(g_px(x, 10), x % 6 < 3 ? 0 : pix_of(0x0000FF));
    g_clear(0xF0);
    select_(dc, pen(1, 2, 0));                      /* styles other than solid fall back to solid when wide */
    GR(O_MOVETO, dc, 0, 10);
    GA(O_LINETO, dc, 30, 10);
    for (x = 1; x < 29; x++) CHECK_EQ(g_px(x, 10), 0);
    gdown();
}

static void test_clipping(void) {
    uint16_t dc, rg, hw;
    printf("test_draw_clipping\n");
    fresh();
    dc = g_screen_dc();
    GA(O_INTERSECTCLIPRECT, dc, 100, 100, 150, 130);
    GR(O_MOVETO, dc, 50, 110);
    GA(O_LINETO, dc, 300, 110);
    GR(O_MOVETO, dc, 120, 50);
    GA(O_LINETO, dc, 120, 300);
    ex_clear();
    {
        int x, y;
        for (x = 100; x < 150; x++) ex_put(x, 110, 0);
        for (y = 100; y < 130; y++) ex_put(120, y, 0);
    }
    ex_compare("rect clip");
    /* a region with a hole */
    g_clear(0xF0);
    GA(O_SELECTCLIPRGN, dc, 0);
    rg = GA(O_CREATERECTRGN, 200, 200, 260, 240);
    {
        uint16_t hole = GA(O_CREATERECTRGN, 220, 210, 240, 230);
        GA(47, rg, rg, hole, 4);                    /* CombineRgn(rg, rg, hole, RGN_DIFF) */
    }
    GA(O_SELECTCLIPRGN, dc, rg);
    select_(dc, brush(0x0000FF));
    GA(O_RECTANGLE, dc, 190, 190, 270, 250);
    {
        int x, y, bad = 0;
        for (y = 180; y < 260; y++)
            for (x = 180; x < 280; x++) {
                bool in = x >= 200 && x < 260 && y >= 200 && y < 240 && !(x >= 220 && x < 240 && y >= 210 && y < 230);
                uint8_t v = g_px(x, y);
                if (!in && v != 0xF0) bad++;
                if (in && v == 0xF0) bad++;
            }
        CHECK_EQ(bad, 0);
    }
    /* a window DC is clipped to the client area */
    GA(O_SELECTCLIPRGN, dc, 0);
    u_register("GWD", 0, 0, 0, 0, 1, 0);
    hw = u_create("GWD", "t", WS_POPUP | WS_VISIBLE, 300, 300, 100, 60, 0, 0, 1);
    g_clear(0xF0);
    {
        uint16_t wdc = (uint16_t)UR(66, hw);
        select_(wdc, pen(0, 1, 0));
        GR(O_MOVETO, wdc, (uint16_t)-20, 10);
        GA(O_LINETO, wdc, 300, 10);
        CHECK_EQ(g_px(299, 310), 0xF0);          /* left of the client area */
        CHECK_EQ(g_px(300, 310), 0);
        CHECK_EQ(g_px(399, 310), 0);
        CHECK_EQ(g_px(400, 310), 0xF0);          /* right of it */
    }
    gdown();
}

typedef struct { int l, t, r, b; } irect;

static const uint8_t hatches[6][8] = {
    {0, 0, 0, 0xFF, 0, 0, 0, 0},                                   /* HS_HORIZONTAL */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10},               /* HS_VERTICAL */
    {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01},               /* HS_FDIAGONAL */
    {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80},               /* HS_BDIAGONAL */
    {0x10, 0x10, 0x10, 0xFF, 0x10, 0x10, 0x10, 0x10},               /* HS_CROSS */
    {0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81}};              /* HS_DIAGCROSS */

static void test_rectangle(void) {
    uint16_t dc;
    int x, y, st;
    printf("test_draw_rectangle\n");
    fresh();
    dc = g_screen_dc();
    select_(dc, brush(0x0000FF));
    GA(O_RECTANGLE, dc, 10, 10, 30, 20);
    ex_clear();
    for (y = 10; y < 20; y++)
        for (x = 10; x < 30; x++) ex_put(x, y, x == 10 || x == 29 || y == 10 || y == 19 ? 0 : pix_of(0x0000FF));
    ex_compare("rectangle");
    /* NULL pen: the brush covers the whole rectangle (right and bottom excluded) */
    g_clear(0xF0);
    stock(dc, 8);
    GA(O_RECTANGLE, dc, 10, 10, 30, 20);
    ex_clear();
    for (y = 10; y < 20; y++)
        for (x = 10; x < 30; x++) ex_put(x, y, pix_of(0x0000FF));
    ex_compare("null pen rectangle");
    /* NULL brush: only the outline */
    g_clear(0xF0);
    stock(dc, 7);
    stock(dc, 5);
    GA(O_RECTANGLE, dc, 10, 10, 30, 20);
    ex_clear();
    for (y = 10; y < 20; y++)
        for (x = 10; x < 30; x++)
            if (x == 10 || x == 29 || y == 10 || y == 19) ex_put(x, y, 0);
    ex_compare("hollow rectangle");
    /* corners given in the other order */
    g_clear(0xF0);
    GA(O_RECTANGLE, dc, 30, 20, 10, 10);
    ex_compare("reversed rectangle");
    /* hatch brushes with the background colour in OPAQUE mode */
    for (st = 0; st < 6; st++) {
        g_clear(0xF0);
        stock(dc, 8);
        select_(dc, GA(O_CREATEHATCHBRUSH, (uint16_t)st, LONG(0x0000FF)));
        GR(O_SETBKCOLOR, dc, LONG(0x00FF00));
        GA(O_SETBKMODE, dc, 2);
        GA(O_RECTANGLE, dc, 16, 16, 48, 48);
        ex_clear();
        for (y = 16; y < 48; y++)
            for (x = 16; x < 48; x++)
                ex_put(x, y, (hatches[st][y & 7] >> (7 - (x & 7))) & 1 ? pix_of(0x0000FF) : pix_of(0x00FF00));
        ex_compare("hatch opaque");
        g_clear(0xF0);
        GA(O_SETBKMODE, dc, 1);
        GA(O_RECTANGLE, dc, 16, 16, 48, 48);
        ex_clear();
        for (y = 16; y < 48; y++)
            for (x = 16; x < 48; x++)
                if ((hatches[st][y & 7] >> (7 - (x & 7))) & 1) ex_put(x, y, pix_of(0x0000FF));
        ex_compare("hatch transparent");
    }
    /* the brush origin shifts the pattern */
    g_clear(0xF0);
    GA(O_SETBKMODE, dc, 1);
    select_(dc, GA(O_CREATEHATCHBRUSH, 2, LONG(0x0000FF)));
    GR(148, dc, 3, 0);                                             /* SetBrushOrg */
    GA(O_RECTANGLE, dc, 16, 16, 48, 48);
    ex_clear();
    for (y = 16; y < 48; y++)
        for (x = 16; x < 48; x++)
            if ((hatches[2][y & 7] >> (7 - ((x - 3) & 7))) & 1) ex_put(x, y, pix_of(0x0000FF));
    ex_compare("brush origin");
    /* ROP2 applies to the brush too */
    g_clear(0x55);
    stock(dc, 8);
    select_(dc, brush(0xFFFFFF));
    GA(O_SETROP2, dc, 7);
    GA(O_RECTANGLE, dc, 10, 10, 20, 20);
    CHECK_EQ(g_px(12, 12), (uint8_t)(0x55 ^ 0xFF));
    CHECK_EQ(g_px(25, 12), 0x55);
    gdown();
}

static bool in_ellipse(double cx, double cy, double a, double b, int x, int y) {
    double dx = (x + 0.5 - cx) / a, dy = (y + 0.5 - cy) / b;
    return dx * dx + dy * dy <= 1.0;
}

static void test_ellipse_roundrect(void) {
    uint16_t dc;
    int x, y, l = 20, t = 30, r = 70, b = 60, inside = 0;
    printf("test_draw_ellipse\n");
    fresh();
    dc = g_screen_dc();
    select_(dc, brush(0x0000FF));
    GA(O_ELLIPSE, dc, (uint16_t)l, (uint16_t)t, (uint16_t)r, (uint16_t)b);
    ex_clear();
    for (y = t - 1; y <= b; y++)
        for (x = l - 1; x <= r; x++) {
            bool in = in_ellipse((l + r) / 2.0, (t + b) / 2.0, (r - l) / 2.0, (b - t) / 2.0, x, y);
            bool edge = in && (!in_ellipse((l + r) / 2.0, (t + b) / 2.0, (r - l) / 2.0, (b - t) / 2.0, x - 1, y) ||
                               !in_ellipse((l + r) / 2.0, (t + b) / 2.0, (r - l) / 2.0, (b - t) / 2.0, x + 1, y) ||
                               !in_ellipse((l + r) / 2.0, (t + b) / 2.0, (r - l) / 2.0, (b - t) / 2.0, x, y - 1) ||
                               !in_ellipse((l + r) / 2.0, (t + b) / 2.0, (r - l) / 2.0, (b - t) / 2.0, x, y + 1));
            if (in) {
                inside++;
                ex_put(x, y, edge ? 0 : pix_of(0x0000FF));
            }
        }
    ex_compare("ellipse");
    CHECK(inside > 1100 && inside < 1250);           /* pi * 25 * 15 = 1178 */
    CHECK_EQ(g_px(45, 29), 0xF0);                    /* touches no pixel above the box */
    CHECK_EQ(g_px(45, 30), 0);
    /* RoundRect */
    g_clear(0xF0);
    GA(O_ROUNDRECT, dc, 100, 100, 160, 140, 20, 16);
    CHECK_EQ(g_px(100, 100), 0xF0);                  /* cut corner */
    CHECK_EQ(g_px(130, 100), 0);                     /* top edge */
    CHECK_EQ(g_px(100, 120), 0);                     /* left edge */
    CHECK_EQ(g_px(130, 120), pix_of(0x0000FF));      /* filled */
    CHECK(g_px(110, 101) != 0xF0);                   /* the corner arc starts inside the box */
    gdown();
}

static void test_polygons(void) {
    uint16_t dc, s;
    int x, y;
    printf("test_draw_polygons\n");
    fresh();
    dc = g_screen_dc();
    s = g_seg(0x100);
    select_(dc, brush(0x0000FF));
    uwr16(s, 0, 20); uwr16(s, 2, 20); uwr16(s, 4, 80); uwr16(s, 6, 30); uwr16(s, 8, 40); uwr16(s, 10, 90);
    GA(O_POLYGON, dc, FARP(s, 0), 3);
    ex_clear();
    ref_line(20, 20, 80, 30, false, 0);
    ref_line(80, 30, 40, 90, false, 0);
    ref_line(40, 90, 20, 20, false, 0);
    {
        int amb = 0;
        for (y = 15; y < 95; y++)
            for (x = 15; x < 85; x++) {
                double px = x + 0.5, py = y + 0.5;
                double c1 = (80.0 - 20) * (py - 20) - (30.0 - 20) * (px - 20);
                double c2 = (40.0 - 80) * (py - 30) - (90.0 - 30) * (px - 80);
                double c3 = (20.0 - 40) * (py - 90) - (20.0 - 90) * (px - 40);
                bool in = (c1 > 0 && c2 > 0 && c3 > 0) || (c1 < 0 && c2 < 0 && c3 < 0);
                if (fabs(c1) < 1e-9 || fabs(c2) < 1e-9 || fabs(c3) < 1e-9) {
                    ex.v[y][x] = ex.v[y][x] == 0x100 ? 0x200 : ex.v[y][x];   /* ambiguous: skip below */
                    amb++;
                    continue;
                }
                if (in && ex.v[y][x] == 0x100) ex.v[y][x] = pix_of(0x0000FF);
            }
        CHECK(amb < 12);
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                if (ex.v[y][x] == 0x200) ex.v[y][x] = g_px(x, y);       /* tolerate the rule-dependent pixels */
    }
    ex_compare("triangle");
    /* fill modes: a pentagram has a hole with ALTERNATE (1) and none with WINDING (2) */
    {
        static const uint16_t star[10] = {200, 100, 230, 190, 160, 130, 240, 130, 170, 190};
        int i;
        for (i = 0; i < 10; i++) uwr16(s, 0x20 + 2 * i, star[i]);
    }
    stock(dc, 8);
    g_clear(0xF0);
    GA(O_SETPOLYFILLMODE, dc, 1);
    GA(O_POLYGON, dc, FARP(s, 0x20), 5);
    CHECK_EQ(g_px(200, 150), 0xF0);
    CHECK_EQ(g_px(200, 110), pix_of(0x0000FF));
    g_clear(0xF0);
    GA(O_SETPOLYFILLMODE, dc, 2);
    GA(O_POLYGON, dc, FARP(s, 0x20), 5);
    CHECK_EQ(g_px(200, 150), pix_of(0x0000FF));
    /* PolyPolygon: two squares, counts in a WORD array */
    g_clear(0xF0);
    GA(O_SETPOLYFILLMODE, dc, 1);
    {
        static const uint16_t sq[16] = {300, 300, 340, 300, 340, 340, 300, 340, 400, 300, 440, 300, 440, 340, 400, 340};
        int i;
        for (i = 0; i < 16; i++) uwr16(s, 0x60 + 2 * i, sq[i]);
        uwr16(s, 0x90, 4);
        uwr16(s, 0x92, 4);
        GA(O_POLYPOLYGON, dc, FARP(s, 0x60), FARP(s, 0x90), 2);
        CHECK_EQ(g_px(320, 320), pix_of(0x0000FF));
        CHECK_EQ(g_px(420, 320), pix_of(0x0000FF));
        CHECK_EQ(g_px(370, 320), 0xF0);
    }
    gdown();
}

static void test_arcs(void) {
    uint16_t dc;
    int x, y, n = 0;
    printf("test_draw_arcs\n");
    fresh();
    dc = g_screen_dc();
    /* Arc counter-clockwise from the 3 o'clock point to the 12 o'clock point: the upper right quarter */
    GA(O_ARC, dc, 100, 100, 200, 180, 200, 140, 150, 100);
    for (y = 90; y < 190; y++)
        for (x = 90; x < 210; x++)
            if (g_px(x, y) == 0) {
                double dx = (x + 0.5 - 150.0) / 50.0, dy = (y + 0.5 - 140.0) / 40.0;
                CHECK(x >= 148 && y <= 141);                       /* only the upper right quarter */
                CHECK(fabs(dx * dx + dy * dy - 1.0) < 0.12);       /* on the ellipse */
                n++;
            }
    CHECK(n > 50 && n < 90);
    /* start == end: the full ellipse */
    g_clear(0xF0);
    GA(O_ARC, dc, 100, 100, 200, 180, 200, 140, 200, 140);
    CHECK_EQ(g_px(100, 140), 0);
    CHECK_EQ(g_px(199, 140), 0);
    CHECK_EQ(g_px(150, 100), 0);
    CHECK_EQ(g_px(150, 179), 0);
    /* Pie: sector filled and bounded by two radii */
    g_clear(0xF0);
    select_(dc, brush(0x0000FF));
    GA(26, dc, 100, 100, 200, 180, 200, 140, 150, 100);
    CHECK_EQ(g_px(170, 120), pix_of(0x0000FF));                  /* inside the quarter */
    CHECK_EQ(g_px(130, 160), 0xF0);                              /* the opposite quarter */
    CHECK_EQ(g_px(130, 120), 0xF0);
    CHECK_EQ(g_px(170, 160), 0xF0);
    CHECK_EQ(g_px(170, 140), 0);                                 /* the radius to the start point */
    CHECK_EQ(g_px(150, 120), 0);                                 /* the radius to the end point */
    /* Chord: the cut ends on the line between the end points */
    g_clear(0xF0);
    GA(O_CHORD, dc, 100, 100, 200, 180, 200, 140, 150, 100);
    CHECK_EQ(g_px(185, 115), pix_of(0x0000FF));
    CHECK_EQ(g_px(130, 150), 0xF0);
    CHECK_EQ(g_px(160, 130), 0xF0);                              /* on the far side of the chord, still inside the ellipse */
    gdown();
}

static void test_pixels_flood(void) {
    uint16_t dc;
    int x, y;
    printf("test_draw_pixels_flood\n");
    fresh();
    dc = g_screen_dc();
    CHECK_EQ(GR(O_SETPIXEL, dc, 7, 8, LONG(0x0000FF)), 0x0000FF);        /* the colour actually set */
    CHECK_EQ(g_px(7, 8), pix_of(0x0000FF));
    CHECK_EQ(GR(O_GETPIXEL, dc, 7, 8), 0x0000FF);
    CHECK_EQ(GR(O_SETPIXEL, dc, 9, 8, LONG(0x010203)), 0x000000);        /* nearest static colour */
    CHECK_EQ(GR(O_GETPIXEL, dc, 700, 8), 0xFFFFFFFFu);                    /* CLR_INVALID outside the surface */
    GA(O_INTERSECTCLIPRECT, dc, 0, 0, 20, 20);
    CHECK_EQ(GR(O_GETPIXEL, dc, 30, 8), 0xFFFFFFFFu);                     /* and outside the clip */
    CHECK_EQ(GR(O_SETPIXEL, dc, 30, 8, LONG(0x0000FF)), 0xFFFFFFFFu);
    GA(O_SELECTCLIPRGN, dc, 0);
    /* FloodFill to a border colour: the interior of a black box becomes red */
    g_clear(0xFF);
    select_(dc, pen(0, 1, 0));
    select_(dc, brush(0x0000FF));
    stock(dc, 5);
    GA(O_RECTANGLE, dc, 100, 100, 140, 130);
    stock(dc, 5);
    select_(dc, brush(0x0000FF));
    CHECK_EQ(GA(O_FLOODFILL, dc, 120, 110, LONG(0x000000)), 1);
    for (y = 100; y < 130; y++)
        for (x = 100; x < 140; x++) {
            bool border = x == 100 || x == 139 || y == 100 || y == 129;
            CHECK_EQ(g_px(x, y), border ? 0 : pix_of(0x0000FF));
        }
    CHECK_EQ(g_px(99, 110), 0xFF);                                        /* nothing leaked out */
    /* ExtFloodFill, surface type: replaces the region of one colour */
    g_clear(0xFF);
    GR(O_SETPIXEL, dc, 50, 50, LONG(0));
    CHECK_EQ(GA(O_EXTFLOODFILL, dc, 10, 10, LONG(0xFFFFFF), 1), 1);
    CHECK_EQ(g_px(10, 10), pix_of(0x0000FF));
    CHECK_EQ(g_px(50, 50), 0);                                            /* a different colour stays */
    CHECK_EQ(g_px(60, 50), pix_of(0x0000FF));
    /* a start pixel that is already the border colour: nothing to fill */
    g_clear(0xFF);
    CHECK_EQ(GA(O_FLOODFILL, dc, 10, 10, LONG(0xFFFFFF)), 0);
    CHECK_EQ(g_px(10, 10), 0xFF);
    gdown();
}

static void test_user_fills(void) {
    uint16_t dc, s, br;
    int x, y;
    printf("test_draw_user_fills\n");
    fresh();
    dc = g_screen_dc();
    s = g_seg(0x40);
    uwr16(s, 0, 10); uwr16(s, 2, 20); uwr16(s, 4, 40); uwr16(s, 6, 50);
    br = brush(0x0000FF);
    CHECK_EQ(UR(U_FILLRECT, dc, FARP(s, 0), br) != 0, 1);
    ex_clear();
    for (y = 20; y < 50; y++)
        for (x = 10; x < 40; x++) ex_put(x, y, pix_of(0x0000FF));
    ex_compare("FillRect");
    /* a system colour index + 1 instead of a brush: COLOR_WINDOW (5) -> 6 */
    g_clear(0xF0);
    UR(U_FILLRECT, dc, FARP(s, 0), 6);
    CHECK_EQ(g_px(20, 30), pix_of(0xFFFFFF));
    /* FrameRect: a one pixel border */
    g_clear(0xF0);
    UR(U_FRAMERECT, dc, FARP(s, 0), br);
    ex_clear();
    for (y = 20; y < 50; y++)
        for (x = 10; x < 40; x++)
            if (x == 10 || x == 39 || y == 20 || y == 49) ex_put(x, y, pix_of(0x0000FF));
    ex_compare("FrameRect");
    /* InvertRect */
    g_clear(0x55);
    UR(U_INVERTRECT, dc, FARP(s, 0));
    CHECK_EQ(g_px(20, 30), 0xAA);
    CHECK_EQ(g_px(40, 30), 0x55);
    CHECK_EQ(g_px(20, 50), 0x55);
    UR(U_INVERTRECT, dc, FARP(s, 0));
    CHECK_EQ(g_px(20, 30), 0x55);
    /* DrawFocusRect: dotted XOR border, drawing it twice removes it */
    g_clear(0x55);
    UR(U_DRAWFOCUSRECT, dc, FARP(s, 0));
    {
        int on = 0;
        for (x = 10; x < 40; x++) on += g_px(x, 20) != 0x55;
        CHECK(on > 10 && on < 20);                                   /* every other pixel */
        CHECK_EQ(g_px(25, 35), 0x55);                                /* interior untouched */
    }
    UR(U_DRAWFOCUSRECT, dc, FARP(s, 0));
    for (y = 15; y < 55; y++)
        for (x = 5; x < 45; x++) CHECK_EQ(g_px(x, y), 0x55);
    /* regions: device coordinates of the DC */
    {
        uint16_t rg = GA(O_CREATERECTRGN, 100, 100, 130, 120);
        g_clear(0xF0);
        CHECK_EQ(GA(O_FILLRGN, dc, rg, br), 1);
        CHECK_EQ(g_px(100, 100), pix_of(0x0000FF));
        CHECK_EQ(g_px(129, 119), pix_of(0x0000FF));
        CHECK_EQ(g_px(130, 119), 0xF0);
        g_clear(0xF0);
        CHECK_EQ(GA(O_FRAMERGN, dc, rg, br, 2, 3), 1);               /* 2 wide, 3 high */
        CHECK_EQ(g_px(100, 110), pix_of(0x0000FF));
        CHECK_EQ(g_px(101, 110), pix_of(0x0000FF));
        CHECK_EQ(g_px(102, 110), 0xF0);
        CHECK_EQ(g_px(115, 100), pix_of(0x0000FF));
        CHECK_EQ(g_px(115, 102), pix_of(0x0000FF));
        CHECK_EQ(g_px(115, 103), 0xF0);
        g_clear(0x55);
        CHECK_EQ(GA(O_INVERTRGN, dc, rg), 1);
        CHECK_EQ(g_px(110, 110), 0xAA);
        select_(dc, brush(0xFFFFFF));
        g_clear(0xF0);
        CHECK_EQ(GA(O_PAINTRGN, dc, rg), 1);
        CHECK_EQ(g_px(110, 110), pix_of(0xFFFFFF));
    }
    CHECK_EQ(user_stub_count(uu), 0);                                 /* no longer logged stubs */
    gdown();
}

/* A real guest loop (registers, not constants) drives MoveTo/LineTo: checks Pascal
 * argument order, SI surviving the calls and the callee's stack cleanup. */
static void test_guest_loop(void) {
    uint16_t dc;
    loader_thunk *tm, *tl;
    uint8_t code[64];
    int k = 0, loop, x, y;
    cpu_t *cpu;
    printf("test_draw_guest_loop\n");
    fresh();
    dc = g_screen_dc();
    tm = fx_thunk(&uf, "GDI", O_MOVETO);
    tl = fx_thunk(&uf, "GDI", O_LINETO);
    CHECK(tm && tl);
    code[k++] = 0xBE; code[k++] = 0; code[k++] = 0;                          /* mov si,0 */
    loop = k;
    code[k++] = 0x68; code[k++] = (uint8_t)dc; code[k++] = (uint8_t)(dc >> 8);   /* push hdc */
    code[k++] = 0x56;                                                        /* push si */
    code[k++] = 0x6A; code[k++] = 0;                                         /* push 0 */
    code[k++] = 0x9A; code[k++] = (uint8_t)tm->off; code[k++] = (uint8_t)(tm->off >> 8);
    code[k++] = (uint8_t)tm->sel; code[k++] = (uint8_t)(tm->sel >> 8);       /* call MoveTo(hdc, si, 0) */
    code[k++] = 0x68; code[k++] = (uint8_t)dc; code[k++] = (uint8_t)(dc >> 8);
    code[k++] = 0x56;
    code[k++] = 0x6A; code[k++] = 25;                                        /* push 25 */
    code[k++] = 0x9A; code[k++] = (uint8_t)tl->off; code[k++] = (uint8_t)(tl->off >> 8);
    code[k++] = (uint8_t)tl->sel; code[k++] = (uint8_t)(tl->sel >> 8);       /* call LineTo(hdc, si, 25) */
    code[k++] = 0x46;                                                        /* inc si */
    code[k++] = 0x83; code[k++] = 0xFE; code[k++] = 8;                       /* cmp si,8 */
    code[k] = 0x75;                                                          /* jne loop */
    code[k + 1] = (uint8_t)(loop - (k + 2));
    k += 2;
    code[k++] = 0xF4;                                                        /* hlt */
    memcpy(umem(u_cs(), 0x200, (uint32_t)k), code, (size_t)k);
    cpu = &uf.p->cpu;
    cpu->seg[CPU_CS] = u_cs();
    cpu->eip = 0x200;
    loader_run(uf.p, 10000);
    CHECK_EQ(uf.p->stop, LOADER_STOP_HLT);
    for (x = 0; x < 8; x++)
        for (y = 0; y < 25; y++) CHECK_EQ(g_px(x, y), 0);
    CHECK_EQ(g_px(8, 5), 0xF0);
    CHECK_EQ(g_px(3, 25), 0xF0);
    gdown();
}

/* LineDDA calls a guest LINEDDAPROC(x, y, lpData) for every point of the line, the end point excluded. */
static void test_linedda(void) {
    static const uint8_t proc[] = {
        0x55, 0x89, 0xE5,                         /* push bp; mov bp,sp */
        0x8B, 0x36, 0x60, 0x00,                   /* mov si,[60h] */
        0xD1, 0xE6, 0xD1, 0xE6,                   /* shl si,1 twice: 4 bytes per point */
        0x8B, 0x46, 0x0C,                         /* mov ax,[bp+12]: x */
        0x89, 0x84, 0x00, 0x01,                   /* mov [si+100h],ax */
        0x8B, 0x46, 0x0A,                         /* mov ax,[bp+10]: y */
        0x89, 0x84, 0x02, 0x01,                   /* mov [si+102h],ax */
        0xFF, 0x06, 0x60, 0x00,                   /* inc word [60h] */
        0x5D, 0xCA, 0x08, 0x00};                  /* pop bp; retf 8 */
    int i;
    printf("test_draw_linedda\n");
    fresh();
    u_put(0x900, proc, sizeof proc);
    uwr16(u_ds(), 0x60, 0);
    GA(100, 10, 20, 17, 23, FARP(u_cs(), 0x900), LONG(0x1234));
    CHECK_EQ(urd16(u_ds(), 0x60), 7);                       /* dx = 7 steps, end point excluded */
    ex_clear();
    ref_line(10, 20, 17, 23, false, 0);
    for (i = 0; i < 7; i++) {
        int x = urd16(u_ds(), 0x100 + 4 * (unsigned)i), y = urd16(u_ds(), 0x102 + 4 * (unsigned)i);
        CHECK_EQ(ex.v[y][x], 0);
        ex.v[y][x] = 0x100;
    }
    {
        int x, left = 0;
        for (i = 0; i < H; i++)
            for (x = 0; x < W; x++) left += ex.v[i][x] != 0x100;
        CHECK_EQ(left, 0);                                    /* every reference point was reported exactly once */
    }
    gdown();
}

int main(void) {
    test_lines();
    test_rop2();
    test_dash();
    test_clipping();
    test_rectangle();
    test_ellipse_roundrect();
    test_polygons();
    test_arcs();
    test_pixels_flood();
    test_user_fills();
    test_guest_loop();
    test_linedda();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
