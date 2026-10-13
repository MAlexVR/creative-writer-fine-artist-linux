/* Regions (rect-list representation) and the clip API. The host
 * side is checked against independent per-pixel references; the Win16 API side
 * through guest code. */
#include <math.h>

#include "gdi_priv.h"
#include "gfixture.h"

#define GW 40
#define GH 40

/* ---- independent references ------------------------------------------------------ */

typedef struct { uint8_t m[GH][GW]; } bmp;

static void bmp_clear(bmp *b) { memset(b, 0, sizeof *b); }
static void bmp_rect(bmp *b, int l, int t, int r, int bo) {
    int x, y;
    for (y = t; y < bo; y++)
        for (x = l; x < r; x++)
            if (x >= 0 && y >= 0 && x < GW && y < GH) b->m[y][x] = 1;
}
static void bmp_of_rgn(bmp *b, const grgn *g) {
    int x, y;
    for (y = 0; y < GH; y++)
        for (x = 0; x < GW; x++) b->m[y][x] = grgn_has_point(g, x, y);
}
static int bmp_count(const bmp *b) {
    int x, y, n = 0;
    for (y = 0; y < GH; y++)
        for (x = 0; x < GW; x++) n += b->m[y][x];
    return n;
}
static bool bmp_same(const bmp *a, const bmp *b) { return memcmp(a, b, sizeof *a) == 0; }

static unsigned rng = 12345;
static int rnd(int n) {
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 16) % (unsigned)n);
}

/* random region made of up to 3 rectangles, together with its bitmap */
static void random_region(grgn *g, bmp *b) {
    int k, n = 1 + rnd(3);
    grgn t;
    grgn_init(g);
    grgn_init(&t);
    bmp_clear(b);
    for (k = 0; k < n; k++) {
        int l = rnd(30), tp = rnd(30), r = l + rnd(14), bo = tp + rnd(14);
        grgn one, sum;
        grgn_init(&one);
        grgn_init(&sum);
        CHECK(grgn_set_rect(&one, l, tp, r, bo));
        CHECK(grgn_combine(&sum, g, &one, RGN_OR));
        CHECK(grgn_copy(g, &sum));
        grgn_free(&one);
        grgn_free(&sum);
        bmp_rect(b, l, tp, r, bo);
    }
    grgn_free(&t);
}

static bool canonical(const grgn *g) {
    int i, j;
    fb_rect box = {0, 0, 0, 0};
    for (i = 0; i < g->n; i++) {
        const fb_rect *a = &g->r[i];
        if (fb_rect_empty(a)) return false;
        if (i == 0) box = *a;
        else fb_rect_union(&box, &box, a);
        if (i + 1 < g->n) {
            const fb_rect *b = &g->r[i + 1];
            if (b->top < a->top) return false;
            if (b->top == a->top) {
                if (b->bottom != a->bottom || b->left <= a->right) return false;   /* sorted, not touching */
            } else if (b->top < a->bottom) {
                return false;                                                       /* bands overlap */
            }
        }
    }
    /* adjacent bands with identical rectangles must have been merged */
    for (i = 0; i < g->n; i++) {
        for (j = 0; j < g->n; j++) {
            const fb_rect *a = &g->r[i], *b = &g->r[j];
            if (a->bottom == b->top && a->left == b->left && a->right == b->right) {
                /* same left/right on touching bands: allowed only if the bands differ in rect count */
                int ca = 0, cb = 0, k;
                for (k = 0; k < g->n; k++) {
                    ca += g->r[k].top == a->top;
                    cb += g->r[k].top == b->top;
                }
                if (ca == 1 && cb == 1) return false;
            }
        }
    }
    if (g->n == 0) return g->box.left == 0 && g->box.right == 0 && g->box.top == 0 && g->box.bottom == 0;
    return g->box.left == box.left && g->box.top == box.top && g->box.right == box.right && g->box.bottom == box.bottom;
}

static void test_basics(void) {
    grgn g, h;
    printf("test_rgn_basics\n");
    grgn_init(&g);
    grgn_init(&h);
    CHECK_EQ(grgn_kind(&g), NULLREGION);
    CHECK(grgn_set_rect(&g, 5, 6, 15, 26));
    CHECK_EQ(grgn_kind(&g), SIMPLEREGION);
    CHECK_EQ(g.box.left, 5);
    CHECK_EQ(g.box.bottom, 26);
    CHECK(grgn_has_point(&g, 5, 6));
    CHECK(grgn_has_point(&g, 14, 25));
    CHECK(!grgn_has_point(&g, 15, 25));       /* right and bottom edges are outside */
    CHECK(!grgn_has_point(&g, 14, 26));
    CHECK(!grgn_has_point(&g, 4, 6));
    CHECK(grgn_set_rect(&g, 10, 10, 10, 20));   /* empty rectangles give the empty region */
    CHECK_EQ(grgn_kind(&g), NULLREGION);
    CHECK(grgn_set_rect(&g, 15, 26, 5, 6));     /* reversed corners are normalized */
    CHECK_EQ(g.box.left, 5);
    CHECK_EQ(g.box.top, 6);
    grgn_offset(&g, 3, -2);
    CHECK_EQ(g.box.left, 8);
    CHECK_EQ(g.box.top, 4);
    CHECK_EQ(g.box.right, 18);
    CHECK_EQ(g.box.bottom, 24);
    {
        fb_rect r = {17, 23, 30, 30};
        CHECK(grgn_overlaps_rect(&g, &r));
        r.left = 18;
        CHECK(!grgn_overlaps_rect(&g, &r));
    }
    CHECK(grgn_copy(&h, &g));
    CHECK(grgn_equal(&g, &h));
    grgn_offset(&h, 1, 0);
    CHECK(!grgn_equal(&g, &h));
    grgn_free(&g);
    grgn_free(&h);
}

static void test_combine_grid(void) {
    int iter, mode;
    printf("test_rgn_combine_grid\n");
    for (iter = 0; iter < 400; iter++) {
        grgn a, b, r;
        bmp ba, bb, want, got;
        random_region(&a, &ba);
        random_region(&b, &bb);
        CHECK(canonical(&a));
        CHECK(canonical(&b));
        bmp_of_rgn(&got, &a);
        CHECK(bmp_same(&got, &ba));
        for (mode = RGN_AND; mode <= RGN_DIFF; mode++) {
            int x, y;
            grgn_init(&r);
            CHECK(grgn_combine(&r, &a, &b, mode));
            for (y = 0; y < GH; y++)
                for (x = 0; x < GW; x++) {
                    int p = ba.m[y][x], q = bb.m[y][x];
                    want.m[y][x] = (uint8_t)(mode == RGN_AND ? p & q : mode == RGN_OR ? p | q : mode == RGN_XOR ? p ^ q : p & !q);
                }
            bmp_of_rgn(&got, &r);
            CHECK(bmp_same(&got, &want));
            CHECK(canonical(&r));
            grgn_free(&r);
        }
        grgn_init(&r);
        CHECK(grgn_combine(&r, &a, &b, RGN_COPY));
        bmp_of_rgn(&got, &r);
        CHECK(bmp_same(&got, &ba));
        grgn_free(&r);
        grgn_free(&a);
        grgn_free(&b);
    }
}

static void test_canonical_form(void) {
    grgn a, b, c, u;
    printf("test_rgn_canonical\n");
    grgn_init(&a); grgn_init(&b); grgn_init(&c); grgn_init(&u);
    grgn_set_rect(&a, 0, 0, 10, 10);
    grgn_set_rect(&b, 10, 0, 20, 10);
    grgn_combine(&u, &a, &b, RGN_OR);               /* side by side: one rectangle */
    CHECK_EQ(u.n, 1);
    grgn_set_rect(&c, 0, 0, 20, 10);
    CHECK(grgn_equal(&u, &c));
    grgn_set_rect(&a, 0, 0, 10, 5);
    grgn_set_rect(&b, 0, 5, 10, 12);
    grgn_combine(&u, &a, &b, RGN_OR);               /* stacked: bands merge */
    CHECK_EQ(u.n, 1);
    CHECK_EQ(u.r[0].bottom, 12);
    grgn_set_rect(&a, 0, 0, 20, 20);
    grgn_set_rect(&b, 5, 5, 15, 15);
    grgn_combine(&u, &a, &b, RGN_DIFF);             /* a frame: three bands */
    CHECK_EQ(u.n, 4);
    CHECK_EQ(grgn_kind(&u), COMPLEXREGION);
    CHECK(grgn_has_point(&u, 2, 10));
    CHECK(!grgn_has_point(&u, 10, 10));
    grgn_combine(&c, &u, &b, RGN_OR);               /* putting the hole back gives the full square */
    grgn_set_rect(&a, 0, 0, 20, 20);
    CHECK(grgn_equal(&c, &a));
    grgn_combine(&c, &a, &a, RGN_XOR);
    CHECK_EQ(grgn_kind(&c), NULLREGION);
    /* rectangle helpers */
    grgn_set_rect(&a, 0, 0, 20, 20);
    {
        fb_rect hole = {5, 5, 15, 15}, clipr = {0, 0, 10, 10};
        CHECK(grgn_subtract_rect(&a, &hole));
        CHECK(!grgn_has_point(&a, 10, 10));
        CHECK(grgn_intersect_rect(&a, &clipr));
        CHECK_EQ(a.box.right, 10);
        CHECK(grgn_has_point(&a, 2, 2));
        CHECK(!grgn_has_point(&a, 7, 7));
    }
    grgn_free(&a); grgn_free(&b); grgn_free(&c); grgn_free(&u);
}

static void test_shapes(void) {
    grgn g;
    bmp b;
    int x, y, n;
    printf("test_rgn_shapes\n");
    grgn_init(&g);
    /* ellipse: pixels whose centre is inside the inscribed ellipse */
    CHECK(grgn_ellipse(&g, 2, 3, 22, 15));
    bmp_clear(&b);
    for (y = 3; y < 15; y++)
        for (x = 2; x < 22; x++) {
            double dx = (x + 0.5 - 12.0) / 10.0, dy = (y + 0.5 - 9.0) / 6.0;
            b.m[y][x] = dx * dx + dy * dy <= 1.0;
        }
    {
        bmp got;
        bmp_of_rgn(&got, &g);
        CHECK(bmp_same(&got, &b));
        CHECK(canonical(&g));
    }
    n = bmp_count(&b);
    CHECK(n > 180 && n < 200);                 /* about pi * 10 * 6 = 188 */
    for (y = 3; y < 15; y++)                   /* symmetric left/right and top/bottom */
        for (x = 2; x < 22; x++) {
            CHECK_EQ(b.m[y][x], b.m[y][23 - x]);
            CHECK_EQ(b.m[y][x], b.m[17 - y][x]);
        }
    CHECK(grgn_ellipse(&g, 0, 0, 1, 1));       /* 1x1: the pixel centre is inside */
    CHECK(grgn_has_point(&g, 0, 0));
    CHECK(grgn_ellipse(&g, 5, 5, 5, 9));
    CHECK_EQ(grgn_kind(&g), NULLREGION);
    /* round rectangle: corners cut by quarter ellipses of width 8 and height 6 */
    CHECK(grgn_roundrect(&g, 2, 2, 30, 20, 8, 6));
    bmp_clear(&b);
    for (y = 2; y < 20; y++)
        for (x = 2; x < 30; x++) {
            double cx = x + 0.5, cy = y + 0.5, ex = 4.0, ey = 3.0, ox = cx, oy = cy;
            int in = 1;
            if (cx < 2 + ex) ox = 2 + ex;
            else if (cx > 30 - ex) ox = 30 - ex;
            if (cy < 2 + ey) oy = 2 + ey;
            else if (cy > 20 - ey) oy = 20 - ey;
            if (ox != cx || oy != cy) {
                double dx = (cx - ox) / ex, dy = (cy - oy) / ey;
                in = dx * dx + dy * dy <= 1.0;
            }
            b.m[y][x] = (uint8_t)in;
        }
    {
        bmp got;
        bmp_of_rgn(&got, &g);
        CHECK(bmp_same(&got, &b));
    }
    CHECK(!grgn_has_point(&g, 2, 2));          /* a cut corner */
    CHECK(grgn_has_point(&g, 16, 2));
    /* polygons: triangle by per-pixel sign tests, alternate and winding star */
    {
        const int16_t tri[6] = {2, 2, 30, 6, 10, 28};
        bmp got, amb;
        int ambiguous = 0;
        CHECK(grgn_polygon(&g, tri, 3, false));
        bmp_clear(&b);
        bmp_clear(&amb);
        for (y = 0; y < GH; y++)
            for (x = 0; x < GW; x++) {
                double px = x + 0.5, py = y + 0.5;
                double c1 = (30.0 - 2) * (py - 2) - (6.0 - 2) * (px - 2);
                double c2 = (10.0 - 30) * (py - 6) - (28.0 - 6) * (px - 30);
                double c3 = (2.0 - 10) * (py - 28) - (2.0 - 28) * (px - 10);
                b.m[y][x] = (c1 >= 0 && c2 >= 0 && c3 >= 0) || (c1 <= 0 && c2 <= 0 && c3 <= 0);
                amb.m[y][x] = fabs(c1) < 1e-9 || fabs(c2) < 1e-9 || fabs(c3) < 1e-9;   /* centre on an edge: rule dependent */
                ambiguous += amb.m[y][x];
            }
        bmp_of_rgn(&got, &g);
        for (y = 0; y < GH; y++)
            for (x = 0; x < GW; x++)
                if (!amb.m[y][x]) CHECK_EQ(got.m[y][x], b.m[y][x]);
        CHECK(ambiguous < 12);
        CHECK(bmp_count(&got) > 300);
    }
    {
        /* a five-point star drawn as a pentagram: the middle is a hole with alternate, filled with winding */
        const int16_t star[10] = {20, 2, 30, 36, 3, 14, 37, 14, 10, 36};
        grgn alt, win;
        grgn_init(&alt); grgn_init(&win);
        CHECK(grgn_polygon(&alt, star, 5, false));
        CHECK(grgn_polygon(&win, star, 5, true));
        CHECK(!grgn_has_point(&alt, 20, 20));
        CHECK(grgn_has_point(&win, 20, 20));
        CHECK(grgn_has_point(&alt, 20, 5));
        CHECK(grgn_has_point(&win, 20, 5));
        grgn_free(&alt); grgn_free(&win);
    }
    {
        /* two polygons at once: even-odd over all edges, overlapping part is a hole */
        const int16_t pts[16] = {0, 0, 20, 0, 20, 20, 0, 20, 10, 10, 30, 10, 30, 30, 10, 30};
        const int16_t counts[2] = {4, 4};
        grgn_free(&g);
        grgn_init(&g);
        CHECK(grgn_polypolygon(&g, pts, counts, 2, false));
        CHECK(grgn_has_point(&g, 5, 5));
        CHECK(!grgn_has_point(&g, 15, 15));
        CHECK(grgn_has_point(&g, 25, 25));
    }
    grgn_free(&g);
}

static void test_mask(void) {
    grgn g;
    uint8_t m[6 * 4] = {0, 1, 1, 0, 0, 0,
                        0, 1, 1, 0, 1, 1,
                        0, 1, 1, 0, 1, 1,
                        0, 0, 0, 0, 0, 0};
    printf("test_rgn_mask\n");
    grgn_init(&g);
    CHECK(grgn_from_mask(&g, m, 6, 4, 10, 20));
    CHECK(grgn_has_point(&g, 11, 20));
    CHECK(grgn_has_point(&g, 14, 21));
    CHECK(!grgn_has_point(&g, 14, 20));
    CHECK(!grgn_has_point(&g, 13, 21));
    CHECK(canonical(&g));
    CHECK_EQ(g.box.left, 11);
    CHECK_EQ(g.box.bottom, 23);
    grgn_free(&g);
}

/* ---- the Win16 API ------------------------------------------------------------------ */

enum {
    O_INTERSECTCLIPRECT = 22, O_EXCLUDECLIPRECT = 21, O_OFFSETCLIPRGN = 32, O_SELECTOBJECT = 45,
    O_COMBINERGN = 47, O_CREATEPOLYGONRGN = 63, O_CREATERECTRGN = 64, O_CREATERECTRGNINDIRECT = 65,
    O_DELETEOBJECT = 69, O_OFFSETRGN = 101, O_GETCLIPBOX = 77, O_SELECTCLIPRGN = 44, O_GETOBJECT = 82,
    O_SETRECTRGN = 106, O_EQUALRGN = 72, O_PTINREGION = 161, O_RECTINREGION = 181, O_GETRGNBOX = 134,
    O_CREATEELLIPTICRGN = 54, O_CREATEELLIPTICRGNINDIRECT = 55, O_CREATEROUNDRECTRGN = 444,
    O_PTVISIBLE = 103, O_RECTVISIBLE = 104, O_SETVIEWPORTORG = 13, O_SETMAPMODE = 3,
    O_SETWINDOWEXT = 12, O_SETVIEWPORTEXT = 14, O_ISGDIOBJECT = 462
};

static void test_api_regions(void) {
    uint16_t r1, r2, r3, s, k;
    printf("test_rgn_api\n");
    gboot();
    s = g_seg(0x200);
    r1 = GA(O_CREATERECTRGN, 10, 20, 50, 60);
    r2 = GA(O_CREATERECTRGN, 30, 40, 90, 100);
    r3 = GA(O_CREATERECTRGN, 0, 0, 1, 1);
    CHECK(r1 && r2 && r3 && r1 != r2);
    CHECK_EQ(GA(O_GETRGNBOX, r1, FARP(s, 0)), SIMPLEREGION);
    CHECK_EQ(urd16(s, 0), 10);
    CHECK_EQ(urd16(s, 2), 20);
    CHECK_EQ(urd16(s, 4), 50);
    CHECK_EQ(urd16(s, 6), 60);
    CHECK_EQ(GA(O_COMBINERGN, r3, r1, r2, RGN_OR), COMPLEXREGION);
    CHECK_EQ(GA(O_GETRGNBOX, r3, FARP(s, 0)), COMPLEXREGION);
    CHECK_EQ(urd16(s, 0), 10);
    CHECK_EQ(urd16(s, 6), 100);
    CHECK_EQ(GA(O_COMBINERGN, r3, r1, r2, RGN_AND), SIMPLEREGION);
    CHECK_EQ(GA(O_GETRGNBOX, r3, FARP(s, 0)), SIMPLEREGION);
    CHECK_EQ(urd16(s, 0), 30);
    CHECK_EQ(urd16(s, 4), 50);
    CHECK_EQ(GA(O_COMBINERGN, r3, r1, r1, RGN_DIFF), NULLREGION);
    CHECK_EQ(GA(O_COMBINERGN, r3, r1, r2, RGN_COPY), SIMPLEREGION);
    CHECK_EQ(GA(O_EQUALRGN, r3, r1), 1);
    CHECK_EQ(GA(O_EQUALRGN, r3, r2), 0);
    CHECK_EQ(GA(O_COMBINERGN, r3, r1, r2, 9), 0);          /* bad mode: ERROR */
    CHECK_EQ(GA(O_COMBINERGN, 0x7777, r1, r2, RGN_OR), 0);
    CHECK_EQ(GA(O_PTINREGION, r1, 10, 20), 1);
    CHECK_EQ(GA(O_PTINREGION, r1, 50, 20), 0);
    CHECK_EQ(GA(O_PTINREGION, r1, 49, 59), 1);
    uwr16(s, 0x10, 49); uwr16(s, 0x12, 59); uwr16(s, 0x14, 80); uwr16(s, 0x16, 80);
    CHECK_EQ(GA(O_RECTINREGION, r1, FARP(s, 0x10)), 1);
    uwr16(s, 0x10, 50);
    CHECK_EQ(GA(O_RECTINREGION, r1, FARP(s, 0x10)), 0);
    CHECK_EQ(GA(O_OFFSETRGN, r1, 5, (uint16_t)-5), SIMPLEREGION);
    GA(O_GETRGNBOX, r1, FARP(s, 0));
    CHECK_EQ(urd16(s, 0), 15);
    CHECK_EQ(urd16(s, 2), 15);
    CHECK_EQ(GA(O_SETRECTRGN, r1, 1, 2, 3, 4), 1);
    GA(O_GETRGNBOX, r1, FARP(s, 0));
    CHECK_EQ(urd16(s, 4), 3);
    CHECK_EQ(urd16(s, 6), 4);
    /* from a RECT, a polygon, an ellipse and a round rectangle */
    uwr16(s, 0x20, 100); uwr16(s, 0x22, 110); uwr16(s, 0x24, 140); uwr16(s, 0x26, 150);
    k = GA(O_CREATERECTRGNINDIRECT, FARP(s, 0x20));
    CHECK(k != 0);
    GA(O_GETRGNBOX, k, FARP(s, 0));
    CHECK_EQ(urd16(s, 0), 100);
    CHECK_EQ(urd16(s, 6), 150);
    {
        static const uint16_t tri[6] = {0, 0, 40, 0, 0, 40};
        int i;
        for (i = 0; i < 6; i++) uwr16(s, 0x40 + 2 * i, tri[i]);
        k = GA(O_CREATEPOLYGONRGN, FARP(s, 0x40), 3, 1);
        CHECK(k != 0);
        CHECK_EQ(GA(O_PTINREGION, k, 5, 5), 1);
        CHECK_EQ(GA(O_PTINREGION, k, 35, 35), 0);
        CHECK_EQ(GA(O_CREATEPOLYGONRGN, FARP(s, 0x40), 3, 7), 0);   /* bad fill mode */
    }
    k = GA(O_CREATEELLIPTICRGN, 0, 0, 40, 20);
    CHECK_EQ(GA(O_PTINREGION, k, 20, 10), 1);
    CHECK_EQ(GA(O_PTINREGION, k, 0, 0), 0);
    uwr16(s, 0x60, 0); uwr16(s, 0x62, 0); uwr16(s, 0x64, 40); uwr16(s, 0x66, 20);
    k = GA(O_CREATEELLIPTICRGNINDIRECT, FARP(s, 0x60));
    CHECK_EQ(GA(O_PTINREGION, k, 20, 10), 1);
    k = GA(O_CREATEROUNDRECTRGN, 0, 0, 40, 20, 10, 10);
    CHECK_EQ(GA(O_PTINREGION, k, 0, 0), 0);
    CHECK_EQ(GA(O_PTINREGION, k, 20, 0), 1);
    /* objects: GetObject says a region has no structure, IsGDIObject yes, DeleteObject frees */
    CHECK_EQ(GA(O_ISGDIOBJECT, r2), 1);
    CHECK_EQ(GA(O_DELETEOBJECT, r2), 1);
    CHECK_EQ(GA(O_GETRGNBOX, r2, FARP(s, 0)), 0);
    gdown();
}

static uint16_t make_window(int x, int y, int cx, int cy) {
    uint16_t h;
    u_register("GW", 0, 0, 0, 0, 1, 0);
    h = u_create("GW", "t", WS_POPUP | WS_VISIBLE, x, y, cx, cy, 0, 0, 1);
    return h;
}

static void test_api_clip(void) {
    uint16_t hwnd, hdc, s, rg;
    printf("test_rgn_clip_api\n");
    gboot();
    s = g_seg(0x100);
    hwnd = make_window(100, 50, 200, 100);
    CHECK(hwnd != 0);
    hdc = (uint16_t)UR(66, hwnd);                    /* GetDC(hwnd): the client area */
    CHECK(hdc != 0);
    CHECK_EQ(GA(O_GETCLIPBOX, hdc, FARP(s, 0)), SIMPLEREGION);
    CHECK_EQ(urd16(s, 0), 0);
    CHECK_EQ(urd16(s, 2), 0);
    CHECK_EQ(urd16(s, 4), 200);
    CHECK_EQ(urd16(s, 6), 100);
    CHECK_EQ(GA(O_PTVISIBLE, hdc, 199, 99), 1);
    CHECK_EQ(GA(O_PTVISIBLE, hdc, 200, 99), 0);
    CHECK_EQ(GA(O_PTVISIBLE, hdc, (uint16_t)-1, 5), 0);
    CHECK_EQ(GA(O_INTERSECTCLIPRECT, hdc, 10, 10, 60, 40), SIMPLEREGION);
    CHECK_EQ(GA(O_GETCLIPBOX, hdc, FARP(s, 0)), SIMPLEREGION);
    CHECK_EQ(urd16(s, 0), 10);
    CHECK_EQ(urd16(s, 6), 40);
    CHECK_EQ(GA(O_PTVISIBLE, hdc, 9, 10), 0);
    CHECK_EQ(GA(O_PTVISIBLE, hdc, 10, 10), 1);
    CHECK_EQ(GA(O_INTERSECTCLIPRECT, hdc, 30, 20, 100, 100), SIMPLEREGION);
    GA(O_GETCLIPBOX, hdc, FARP(s, 0));
    CHECK_EQ(urd16(s, 0), 30);
    CHECK_EQ(urd16(s, 2), 20);
    CHECK_EQ(urd16(s, 4), 60);
    CHECK_EQ(urd16(s, 6), 40);
    CHECK_EQ(GA(O_EXCLUDECLIPRECT, hdc, 40, 25, 50, 35), COMPLEXREGION);
    CHECK_EQ(GA(O_PTVISIBLE, hdc, 45, 30), 0);
    CHECK_EQ(GA(O_PTVISIBLE, hdc, 35, 30), 1);
    uwr16(s, 0x10, 41); uwr16(s, 0x12, 26); uwr16(s, 0x14, 49); uwr16(s, 0x16, 34);
    CHECK_EQ(GA(O_RECTVISIBLE, hdc, FARP(s, 0x10)), 0);            /* entirely in the hole */
    uwr16(s, 0x14, 55);
    CHECK_EQ(GA(O_RECTVISIBLE, hdc, FARP(s, 0x10)), 1);
    CHECK_EQ(GA(O_OFFSETCLIPRGN, hdc, 10, 0), COMPLEXREGION);
    CHECK_EQ(GA(O_PTVISIBLE, hdc, 45, 30), 1);                     /* the hole moved */
    CHECK_EQ(GA(O_PTVISIBLE, hdc, 55, 30), 0);
    CHECK_EQ(GA(O_INTERSECTCLIPRECT, hdc, 70, 70, 70, 90), NULLREGION);
    /* SelectClipRgn replaces; 0 removes it */
    rg = GA(O_CREATERECTRGN, 5, 5, 25, 25);
    CHECK_EQ(GA(O_SELECTCLIPRGN, hdc, rg), SIMPLEREGION);
    GA(O_GETCLIPBOX, hdc, FARP(s, 0));
    CHECK_EQ(urd16(s, 0), 5);
    CHECK_EQ(urd16(s, 4), 25);
    CHECK_EQ(GA(O_DELETEOBJECT, rg), 1);                             /* the DC keeps a copy */
    CHECK_EQ(GA(O_PTVISIBLE, hdc, 10, 10), 1);
    CHECK_EQ(GA(O_SELECTCLIPRGN, hdc, 0), SIMPLEREGION);
    GA(O_GETCLIPBOX, hdc, FARP(s, 0));
    CHECK_EQ(urd16(s, 4), 200);
    CHECK_EQ(GA(O_SELECTCLIPRGN, hdc, 0x7777), 0);
    /* the clip region is in device units; IntersectClipRect takes logical ones */
    GA(O_SETMAPMODE, hdc, 8);
    GR(O_SETVIEWPORTORG, hdc, 20, 10);
    CHECK_EQ(GA(O_INTERSECTCLIPRECT, hdc, 0, 0, 30, 30), SIMPLEREGION);
    GA(O_GETCLIPBOX, hdc, FARP(s, 0));                               /* back in logical units */
    CHECK_EQ(urd16(s, 0), 0);
    CHECK_EQ(urd16(s, 4), 30);
    CHECK_EQ(GA(O_PTVISIBLE, hdc, 29, 29), 1);
    CHECK_EQ(GA(O_PTVISIBLE, hdc, 30, 29), 0);
    CHECK_EQ(GA(O_PTVISIBLE, hdc, (uint16_t)-1, 0), 0);
    /* the screen: visible area of the desktop DC */
    {
        uint16_t sd = g_screen_dc();
        GA(O_GETCLIPBOX, sd, FARP(s, 0));
        CHECK_EQ(urd16(s, 4), 640);
        CHECK_EQ(urd16(s, 6), 480);
    }
    /* a memory DC: the bounds of its bitmap (1x1 until a bitmap is selected) */
    {
        uint16_t m = GA(52, hdc);
        CHECK_EQ(GA(O_GETCLIPBOX, m, FARP(s, 0)), SIMPLEREGION);
        CHECK_EQ(urd16(s, 4), 1);
        CHECK_EQ(urd16(s, 6), 1);
    }
    gdown();
}

int main(void) {
    test_basics();
    test_combine_grid();
    test_canonical_form();
    test_shapes();
    test_mask();
    test_api_regions();
    test_api_clip();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
