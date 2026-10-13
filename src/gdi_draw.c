/* Drawing: the clipped drawing context, ROP2, pens, brushes and the shape
 * primitives. See gdi.h.
 *
 * Shapes are rasterized into a small mask (one byte per pixel, limited to the
 * visible part of the shape) carrying three layers: brush fill, pen, and the
 * gaps of a dashed pen. The mask is then applied once, so XOR/NOT ROP2 modes
 * touch every pixel exactly once.
 *
 * Differences from a real Windows 3.1 display driver, all cosmetic:
 *  - lines use the textbook Bresenham walk from the first to the last point
 *    (ties stay on the start side) and never draw the last point;
 *  - pens wider than one pixel are drawn as a disc (a square when 2 wide) stamped
 *    along the line, always solid; dashes are 18/6 (dash), 3/3 (dot) and the
 *    9/6/3/6 and 9/3/3/3/3/3 combinations, restarted at every call;
 *  - the outline of ellipses and rounded rectangles is the set of shape pixels
 *    with a 4-neighbour outside the shape, always solid;
 *  - polygon fills include the pixels whose centre is inside (edges exactly
 *    through a centre follow the left/top rule);
 *  - hatch patterns are 8x8 cells defined here (horizontal on row 3, vertical on
 *    column 3, the diagonals corner to corner). */
#include <math.h>

#include "gdi_priv.h"

#define PI_ 3.14159265358979323846

/* ---- drawing context ---------------------------------------------------------------------- */

bool gdc_surface(gdi_t *g, gdc *dc, gsurf *s) {
    fb_rect all;
    memset(s, 0, sizeof *s);
    if (dc->mem) {
        gobj *o = gobj_typed(g, dc->bmp, GO_BITMAP);
        if (!o) return false;
        s->pix = o->u.bmp.bits;
        s->w = o->u.bmp.w;
        s->h = o->u.bmp.h;
        s->mono = o->u.bmp.bpp == 1;
        s->base.right = s->w;
        s->base.bottom = s->h;
        return true;
    }
    {
        wm_dc *w = wm_dc_get(g->wm, dc->h);
        if (!w) return false;
        s->pix = g->fb->p8;
        s->w = g->fb->w;
        s->h = g->fb->h;
        s->ox = w->org_x;
        s->oy = w->org_y;
        all.left = all.top = 0;
        all.right = s->w;
        all.bottom = s->h;
        if (!fb_rect_intersect(&s->base, &w->clip, &all)) memset(&s->base, 0, sizeof s->base);
    }
    return true;
}

bool gctx_open(gdi_t *g, uint16_t hdc, gctx *x) {
    gdc *dc = gdc_get(g, hdc);
    gsurf s;
    memset(x, 0, sizeof *x);
    grgn_init(&x->clipr);
    if (!dc || !gdc_surface(g, dc, &s)) return false;
    x->g = g;
    x->dc = dc;
    x->pix = s.pix;
    x->sw = s.w;
    x->sh = s.h;
    x->mono = s.mono;
    x->ox = s.ox;
    x->oy = s.oy;
    x->rop2 = dc->rop2;
    x->hdc = hdc;
    if (!grgn_set_rect(&x->clipr, s.base.left, s.base.top, s.base.right, s.base.bottom)) return false;
    if (dc->has_clip) {
        grgn t;
        grgn_init(&t);
        if (!grgn_copy(&t, &dc->clip)) return false;
        grgn_offset(&t, s.ox, s.oy);
        if (!grgn_combine(&x->clipr, &x->clipr, &t, RGN_AND)) {
            grgn_free(&t);
            return false;
        }
        grgn_free(&t);
    }
    return true;
}

void gctx_close(gctx *x) { grgn_free(&x->clipr); }

/* ---- colours -------------------------------------------------------------------------------- */

uint8_t gdi_nearest(gdi_t *g, uint32_t rgb_cref) {
    unsigned h = (rgb_cref * 2654435761u) >> 20;     /* 4096 entries */
    if (g->ccache_gen != g->pal_gen) {
        memset(g->ccache, 0, sizeof g->ccache);
        g->ccache_gen = g->pal_gen;
    }
    if (g->ccache[h].valid && g->ccache[h].key == rgb_cref) return g->ccache[h].val;
    g->ccache[h].valid = true;
    g->ccache[h].key = rgb_cref;
    g->ccache[h].val = (uint8_t)fb_pixel_of(g->fb, rgb_cref);
    return g->ccache[h].val;
}

uint8_t gctx_pixel(const gctx *x, uint32_t c) {
    gdi_t *g = x->g;
    if (x->mono) {
        uint32_t r = c & 0xFF, gg = c >> 8 & 0xFF, b = c >> 16 & 0xFF;
        if ((c >> 24) == 0x05) return (uint8_t)(c & 1);
        return (r * 30 + gg * 59 + b * 11) / 100 >= 128;
    }
    switch (c >> 24) {
    case 0x01: {                                   /* PALETTEINDEX */
        gobj *o = gobj_typed(g, x->dc->pal, GO_PALETTE);
        unsigned i = c & 0xFFFF;
        if (o && (int)i < o->u.pal.n) {
            if (o->u.pal.map && o->u.pal.realized) return o->u.pal.map[i];
            return gdi_nearest(g, (uint32_t)o->u.pal.e[i][0] | (uint32_t)o->u.pal.e[i][1] << 8 |
                                      (uint32_t)o->u.pal.e[i][2] << 16);
        }
        return 0;
    }
    case 0x02: {                                   /* PALETTERGB: the nearest entry of the selected logical palette */
        gobj *o = gobj_typed(g, x->dc->pal, GO_PALETTE);
        if (o && !o->stock && o->u.pal.realized && o->u.pal.n > 0) return o->u.pal.map[gdi_pal_nearest_entry(&o->u.pal, c & 0xFFFFFF)];
        return gdi_nearest(g, c & 0xFFFFFF);
    }
    case 0x04: return (uint8_t)c;
    default: return gdi_nearest(g, c & 0xFFFFFF);
    }
}

uint32_t gctx_colorref_of(const gctx *x, uint8_t pixel) {
    if (x->mono) return pixel ? 0xFFFFFF : 0;
    {
        uint32_t rgb = x->g->fb->pal[pixel];   /* 0x00RRGGBB -> COLORREF 0x00BBGGRR */
        return (rgb & 0xFF) << 16 | (rgb & 0xFF00) | (rgb >> 16 & 0xFF);
    }
}

/* ---- ROP2 ----------------------------------------------------------------------------------- */

uint8_t rop2_apply(int rop2, uint8_t p, uint8_t d, uint8_t mask) {
    uint8_t r;
    switch (rop2) {
    case 1: r = 0; break;
    case 2: r = (uint8_t)~(d | p); break;
    case 3: r = d & (uint8_t)~p; break;
    case 4: r = (uint8_t)~p; break;
    case 5: r = p & (uint8_t)~d; break;
    case 6: r = (uint8_t)~d; break;
    case 7: r = p ^ d; break;
    case 8: r = (uint8_t)~(d & p); break;
    case 9: r = d & p; break;
    case 10: r = (uint8_t)~(p ^ d); break;
    case 11: r = d; break;
    case 12: r = d | (uint8_t)~p; break;
    case 14: r = p | (uint8_t)~d; break;
    case 15: r = p | d; break;
    case 16: r = 0xFF; break;
    default: r = p; break;
    }
    return r & mask;
}

static inline uint8_t surf_mask(const gctx *x) { return x->mono ? 1 : 0xFF; }

/* ---- brushes ------------------------------------------------------------------------------------ */

static const uint8_t hatch_bits[6][8] = {
    {0, 0, 0, 0xFF, 0, 0, 0, 0},                       /* HS_HORIZONTAL */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10},   /* HS_VERTICAL */
    {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01},   /* HS_FDIAGONAL */
    {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80},   /* HS_BDIAGONAL */
    {0x10, 0x10, 0x10, 0xFF, 0x10, 0x10, 0x10, 0x10},   /* HS_CROSS */
    {0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81}};  /* HS_DIAGCROSS */

void brush_prepare(const gctx *x, const gbrush *b, bpat *bp) {
    int i;
    memset(bp, 0, sizeof *bp);
    switch (b->style) {
    case BS_SOLID:
        bp->kind = 1;
        bp->solid = gctx_pixel(x, b->color);
        break;
    case BS_HATCHED: {
        uint8_t fg = gctx_pixel(x, b->color), bk = gctx_pixel(x, x->dc->bk);
        int h = b->hatch < 0 || b->hatch > 5 ? 0 : b->hatch;
        bp->kind = 2;
        for (i = 0; i < 64; i++) {
            bool on = (hatch_bits[h][i >> 3] >> (7 - (i & 7))) & 1;
            bp->pat[i] = on ? fg : x->dc->bkmode == OPAQUE ? bk : BPAT_CLEAR;
        }
        break;
    }
    case BS_PATTERN:
    case BS_DIBPATTERN:
        bp->kind = 2;
        for (i = 0; i < 64; i++) {
            uint32_t c = b->pat[i];
            if ((c >> 24) == 0x05) bp->pat[i] = (c & 1) ? gctx_pixel(x, x->dc->bk) : gctx_pixel(x, x->dc->fg);
            else bp->pat[i] = gctx_pixel(x, c);
        }
        break;
    default: break;                                   /* BS_NULL: kind 0 */
    }
}

int bpat_at(const gctx *x, const bpat *bp, int sx, int sy) {
    if (bp->kind == 1) return bp->solid;
    if (bp->kind == 2) {
        int ix = (sx - x->ox - x->dc->box) & 7, iy = (sy - x->oy - x->dc->boy) & 7;
        return bp->pat[iy * 8 + ix];
    }
    return BPAT_CLEAR;
}

/* ---- pixel and span output -------------------------------------------------------------------------- */

static inline bool visible(const gctx *x, int sx, int sy) {
    if (sx < 0 || sy < 0 || sx >= x->sw || sy >= x->sh) return false;
    return grgn_has_point(&x->clipr, sx, sy);
}

static inline void put(const gctx *x, int sx, int sy, uint8_t p, int rop2) {
    uint8_t *d = &x->pix[(size_t)sy * x->sw + sx];
    *d = rop2_apply(rop2, p, *d, surf_mask(x));
}

void fill_rect_surface(gctx *x, const fb_rect *r, const bpat *bp, int rop2) {
    int i, sx, sy;
    if (!bp->kind) return;
    for (i = 0; i < x->clipr.n; i++) {
        fb_rect c;
        const fb_rect *q = &x->clipr.r[i];
        if (!fb_rect_intersect(&c, r, q)) continue;
        for (sy = c.top; sy < c.bottom; sy++)
            for (sx = c.left; sx < c.right; sx++) {
                int p = bpat_at(x, bp, sx, sy);
                if (p != BPAT_CLEAR) put(x, sx, sy, (uint8_t)p, rop2);
            }
    }
    x->g->fb->dirty++;
}

void invert_rect_surface(gctx *x, const fb_rect *r) {
    int i, sx, sy;
    for (i = 0; i < x->clipr.n; i++) {
        fb_rect c;
        if (!fb_rect_intersect(&c, r, &x->clipr.r[i])) continue;
        for (sy = c.top; sy < c.bottom; sy++)
            for (sx = c.left; sx < c.right; sx++) put(x, sx, sy, 0, 6);
    }
    x->g->fb->dirty++;
}

fb_rect dev_rect_of(const gdc *dc, int l, int t, int r, int b) {
    fb_rect q;
    int x0 = gdc_lx(dc, l), y0 = gdc_ly(dc, t), x1 = gdc_lx(dc, r), y1 = gdc_ly(dc, b);
    q.left = x0 < x1 ? x0 : x1;
    q.right = x0 < x1 ? x1 : x0;
    q.top = y0 < y1 ? y0 : y1;
    q.bottom = y0 < y1 ? y1 : y0;
    return q;
}

/* ---- masks ------------------------------------------------------------------------------------------- */

#define MK_FILL 1
#define MK_PEN 2
#define MK_GAP 4

typedef struct { int x0, y0, w, h; uint8_t *m; } gmask;

/* The mask covers box (surface coordinates) intersected with the clip's bounding box. */
static bool mask_open(gmask *m, const gctx *x, const fb_rect *box) {
    fb_rect c;
    memset(m, 0, sizeof *m);
    if (!fb_rect_intersect(&c, box, &x->clipr.box)) return true;       /* nothing visible: empty mask */
    m->x0 = c.left;
    m->y0 = c.top;
    m->w = c.right - c.left;
    m->h = c.bottom - c.top;
    m->m = calloc((size_t)m->w * (size_t)m->h, 1);
    return m->m != NULL;
}

static void mask_close(gmask *m) { free(m->m); m->m = NULL; }

static inline void mk(gmask *m, int x, int y, uint8_t v) {
    x -= m->x0;
    y -= m->y0;
    if (x >= 0 && y >= 0 && x < m->w && y < m->h) m->m[(size_t)y * m->w + x] |= v;
}

static void mask_fill_rect(gmask *m, int l, int t, int r, int b) {
    int x, y;
    for (y = t; y < b; y++)
        for (x = l; x < r; x++) mk(m, x, y, MK_FILL);
}

static void mask_fill_region(gmask *m, const grgn *g) {
    int i;
    for (i = 0; i < g->n; i++) mask_fill_rect(m, g->r[i].left, g->r[i].top, g->r[i].right, g->r[i].bottom);
}

/* pen state for strokes */
typedef struct {
    gmask *m;
    int width;
    const int *dash;
    int ndash, di, run;
} stroker;

static const int dash_pat[5][6] = {{0}, {18, 6}, {3, 3}, {9, 6, 3, 6}, {9, 3, 3, 3, 3, 3}};
static const int dash_len[5] = {0, 2, 2, 4, 6};

static void stroker_init(stroker *s, gmask *m, const gpen *pen, int width) {
    memset(s, 0, sizeof *s);
    s->m = m;
    s->width = width;
    if (width <= 1 && pen->style >= PS_DASH && pen->style <= PS_DASHDOTDOT) {
        s->dash = dash_pat[pen->style];
        s->ndash = dash_len[pen->style];
        s->run = s->dash[0];
    }
}

static void stamp(gmask *m, int px, int py, int w, uint8_t v) {
    int i, j, l = px - (w - 1) / 2, t = py - (w - 1) / 2;
    for (j = 0; j < w; j++)
        for (i = 0; i < w; i++) {
            if (w >= 3) {
                double dx = i + 0.5 - w / 2.0, dy = j + 0.5 - w / 2.0;
                if (dx * dx + dy * dy > w * w / 4.0) continue;
            }
            mk(m, l + i, t + j, v);
        }
}

static void emit(stroker *s, int x, int y) {
    uint8_t v = MK_PEN;
    if (s->ndash) {
        if (s->di & 1) v = MK_GAP;
        if (--s->run <= 0) {
            s->di = (s->di + 1) % s->ndash;
            s->run = s->dash[s->di];
        }
    }
    if (s->width > 1) stamp(s->m, x, y, s->width, v);
    else mk(s->m, x, y, v);
}

static void stroke_seg(stroker *s, int x0, int y0, int x1, int y1, bool incl_end) {
    int dx = abs(x1 - x0), dy = abs(y1 - y0), sx = x1 >= x0 ? 1 : -1, sy = y1 >= y0 ? 1 : -1, i, d;
    int x = x0, y = y0;
    if (dx >= dy) {
        d = 2 * dy - dx;
        for (i = 0; i <= dx; i++) {
            if (i == dx && !incl_end) break;
            emit(s, x, y);
            if (d > 0) {
                y += sy;
                d -= 2 * dx;
            }
            d += 2 * dy;
            x += sx;
        }
    } else {
        d = 2 * dx - dy;
        for (i = 0; i <= dy; i++) {
            if (i == dy && !incl_end) break;
            emit(s, x, y);
            if (d > 0) {
                x += sx;
                d -= 2 * dy;
            }
            d += 2 * dx;
            y += sy;
        }
    }
}

/* Applies the mask: pen over gaps over brush. */
static void mask_apply(gctx *x, const gmask *m, const bpat *bp, uint8_t penpix, bool use_pen, int rop2) {
    gdc *dc = x->dc;
    uint8_t gap = gctx_pixel(x, dc->bk), mask = surf_mask(x);
    bool complex_clip = x->clipr.n > 1;
    int i, j;
    for (j = 0; j < m->h; j++)
        for (i = 0; i < m->w; i++) {
            uint8_t v = m->m[(size_t)j * m->w + i];
            int sx = m->x0 + i, sy = m->y0 + j, p;
            if (!v) continue;
            if (complex_clip && !grgn_has_point(&x->clipr, sx, sy)) continue;
            if ((v & MK_PEN) && use_pen) p = penpix;
            else if ((v & MK_GAP) && use_pen && dc->bkmode == OPAQUE) p = gap;
            else if ((v & MK_FILL) && bp && bp->kind) p = bpat_at(x, bp, sx, sy);
            else continue;
            if (p == BPAT_CLEAR) continue;
            put(x, sx, sy, (uint8_t)p & mask, rop2);
        }
    x->g->fb->dirty++;
}

/* Everything one drawing call needs. */
typedef struct {
    gctx x;
    const gpen *pen;
    const gbrush *brush;
    bpat bp;
    int pw;        /* pen width in device pixels, 0 when there is no pen */
    uint8_t penpix;
} dctx;

static bool dctx_open(gdi_t *g, uint16_t hdc, dctx *d) {
    gobj *po, *bo;
    if (!gctx_open(g, hdc, &d->x)) {
        gctx_close(&d->x);
        return false;
    }
    po = gobj_typed(g, d->x.dc->pen, GO_PEN);
    bo = gobj_typed(g, d->x.dc->brush, GO_BRUSH);
    d->pen = po ? &po->u.pen : NULL;
    d->brush = bo ? &bo->u.brush : NULL;
    d->pw = 0;
    if (d->pen && d->pen->style != PS_NULL) {
        int w = abs(gdc_scale_x(d->x.dc, d->pen->width));
        d->pw = w < 1 ? 1 : w;
        d->penpix = gctx_pixel(&d->x, d->pen->color);
    }
    if (d->brush) brush_prepare(&d->x, d->brush, &d->bp);
    else memset(&d->bp, 0, sizeof d->bp);
    return true;
}

static void dctx_close(dctx *d) { gctx_close(&d->x); }

static fb_rect expand(fb_rect r, int by) {
    r.left -= by;
    r.top -= by;
    r.right += by;
    r.bottom += by;
    return r;
}

/* ---- handlers: lines ------------------------------------------------------------------------------------ */

static void stroke_path(dctx *d, gmask *m, const int *pts, int n, bool close, bool incl_last) {
    stroker s;
    int i;
    if (!d->pw || n < 1) return;
    stroker_init(&s, m, d->pen, d->pw);
    for (i = 0; i + 1 < n; i++) stroke_seg(&s, pts[2 * i], pts[2 * i + 1], pts[2 * i + 2], pts[2 * i + 3], i + 2 == n && incl_last && !close);
    if (close && n > 1) stroke_seg(&s, pts[2 * n - 2], pts[2 * n - 1], pts[0], pts[1], false);
}

static fb_rect pts_box(const int *pts, int n) {
    fb_rect r;
    int i;
    r.left = r.right = pts[0];
    r.top = r.bottom = pts[1];
    for (i = 1; i < n; i++) {
        if (pts[2 * i] < r.left) r.left = pts[2 * i];
        if (pts[2 * i] > r.right) r.right = pts[2 * i];
        if (pts[2 * i + 1] < r.top) r.top = pts[2 * i + 1];
        if (pts[2 * i + 1] > r.bottom) r.bottom = pts[2 * i + 1];
    }
    r.right++;
    r.bottom++;
    return r;
}

static bool path_draw(gdi_t *g, uint16_t hdc, int *pts, int n) {
    dctx d;
    gmask m;
    fb_rect box;
    if (!dctx_open(g, hdc, &d)) return false;
    if (d.pw && n >= 2) {
        box = expand(pts_box(pts, n), d.pw);
        if (mask_open(&m, &d.x, &box)) {
            stroke_path(&d, &m, pts, n, false, false);
            mask_apply(&d.x, &m, NULL, d.penpix, true, d.x.rop2);
        }
        mask_close(&m);
    }
    dctx_close(&d);
    return true;
}

static cpu_status g_lineto(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a), pts[4];
    gdc *dc;
    bool ok;
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    pts[0] = gdc_lx(dc, dc->px) + 0;
    pts[1] = gdc_ly(dc, dc->py);
    pts[2] = gdc_lx(dc, x);
    pts[3] = gdc_ly(dc, y);
    {
        gsurf s;
        ok = gdc_surface(g, dc, &s);
        if (ok) {
            pts[0] += s.ox; pts[1] += s.oy; pts[2] += s.ox; pts[3] += s.oy;
        }
    }
    ok = ok && path_draw(g, h, pts, 2);
    dc->px = x;
    dc->py = y;
    hle_ret16(c, ok);
    return CPU_OK;
}

/* guest POINT array to surface coordinates; returns NULL on a bad pointer */
static int *read_points(hle_call *c, gdc *dc, const gctx *x, uint16_t sel, uint16_t off, int n) {
    int *pts, i;
    if (n < 0 || n > 8192) return NULL;
    pts = malloc((size_t)(n ? n : 1) * 2 * sizeof *pts);
    if (!pts) return NULL;
    for (i = 0; i < n; i++) {
        const uint8_t *q = hle_ptr(c, sel, (uint32_t)off + 4u * (unsigned)i, 4);
        if (!q) {
            free(pts);
            return NULL;
        }
        pts[2 * i] = gdc_lx(dc, (int16_t)gld16(q)) + x->ox;
        pts[2 * i + 1] = gdc_ly(dc, (int16_t)gld16(q + 2)) + x->oy;
    }
    return pts;
}

static cpu_status g_polyline(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    int n, *pts;
    gctx x;
    bool ok = false;
    hle_far(&a, &sel, &off);
    n = (int16_t)hle_u16(&a);
    GCHECK;
    if (n >= 2 && gctx_open(g, h, &x)) {
        pts = read_points(c, x.dc, &x, sel, off, n);
        gctx_close(&x);
        if (!pts) return CPU_E_SEL;
        ok = path_draw(g, h, pts, n);
        free(pts);
    } else if (n >= 2) {
        gctx_close(&x);
    }
    hle_ret16(c, ok);
    return CPU_OK;
}

/* ---- handlers: closed shapes -------------------------------------------------------------------------------- */

typedef enum { SH_RECT, SH_ELLIPSE, SH_ROUND, SH_POLY } shape_kind;

/* A closed shape: fill region (surface coordinates) plus the outline either as a path or
 * derived from the region's boundary. */
static bool draw_shape(gdi_t *g, uint16_t hdc, shape_kind kind, int l, int t, int r, int b, int ew, int eh,
                       const int *pts, const int16_t *counts, int npoly, bool winding) {
    dctx d;
    gmask m;
    grgn shape;
    fb_rect box;
    bool ok = true;
    int i, x, y;
    grgn_init(&shape);
    if (!dctx_open(g, hdc, &d)) {
        dctx_close(&d);
        return false;
    }
    switch (kind) {
    case SH_RECT:
        ok = grgn_set_rect(&shape, l, t, r, b);
        break;
    case SH_ELLIPSE: ok = grgn_ellipse(&shape, l, t, r, b); break;
    case SH_ROUND: ok = grgn_roundrect(&shape, l, t, r, b, ew, eh); break;
    default: {
        int total = 0, k;
        int16_t *xy;
        for (i = 0; i < npoly; i++) total += counts[i];
        xy = malloc((size_t)(total ? total : 1) * 4);
        if (!xy) ok = false;
        else {
            for (k = 0; k < total; k++) {
                xy[2 * k] = (int16_t)pts[2 * k];
                xy[2 * k + 1] = (int16_t)pts[2 * k + 1];
            }
            ok = grgn_polypolygon(&shape, xy, counts, npoly, winding);
            free(xy);
        }
        break;
    }
    }
    if (!ok) {
        grgn_free(&shape);
        dctx_close(&d);
        return false;
    }
    box = expand(shape.box, d.pw + 1);
    if (kind == SH_POLY) {
        int total = 0;
        for (i = 0; i < npoly; i++) total += counts[i];
        if (total) {
            fb_rect pb = expand(pts_box(pts, total), d.pw + 1);
            fb_rect_union(&box, &box, &pb);
        }
    }
    if (mask_open(&m, &d.x, &box) && m.m) {
        mask_fill_region(&m, &shape);
        if (d.pw) {
            stroker s;
            if (kind == SH_RECT) {
                int rr[10], dl = l, dt = t, drr = r - 1, db = b - 1;
                if (d.pen->style == PS_INSIDEFRAME && d.pw > 1) {
                    dl += (d.pw - 1) / 2;
                    dt += (d.pw - 1) / 2;
                    drr -= d.pw / 2;
                    db -= d.pw / 2;
                }
                rr[0] = dl; rr[1] = dt; rr[2] = drr; rr[3] = dt; rr[4] = drr; rr[5] = db; rr[6] = dl; rr[7] = db;
                stroke_path(&d, &m, rr, 4, true, false);
            } else if (kind == SH_POLY) {
                int base = 0;
                for (i = 0; i < npoly; i++) {
                    stroke_path(&d, &m, pts + 2 * base, counts[i], true, false);
                    base += counts[i];
                }
            } else {
                stroker_init(&s, &m, d.pen, d.pw);
                s.ndash = 0;                           /* outlines of curves are always solid */
                for (y = box.top; y < box.bottom; y++)
                    for (x = box.left; x < box.right; x++)
                        if (grgn_has_point(&shape, x, y) &&
                            (!grgn_has_point(&shape, x - 1, y) || !grgn_has_point(&shape, x + 1, y) ||
                             !grgn_has_point(&shape, x, y - 1) || !grgn_has_point(&shape, x, y + 1)))
                            emit(&s, x, y);
            }
        }
        mask_apply(&d.x, &m, d.brush ? &d.bp : NULL, d.penpix, d.pw > 0, d.x.rop2);
    }
    mask_close(&m);
    grgn_free(&shape);
    dctx_close(&d);
    return true;
}

static bool dev_box(hle_call *c, gdi_t *g, uint16_t h, int l, int t, int r, int b, fb_rect *out) {
    gdc *dc = gdc_get(g, h);
    gsurf s;
    fb_rect q;
    (void)c;
    if (!dc || !gdc_surface(g, dc, &s)) return false;
    q = dev_rect_of(dc, l, t, r, b);
    q.left += s.ox; q.right += s.ox; q.top += s.oy; q.bottom += s.oy;
    *out = q;
    return true;
}

static cpu_status g_rectangle(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int l = (int16_t)hle_u16(&a), t = (int16_t)hle_u16(&a), r = (int16_t)hle_u16(&a), b = (int16_t)hle_u16(&a);
    fb_rect q;
    GCHECK;
    if (!dev_box(c, g, h, l, t, r, b, &q)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (fb_rect_empty(&q)) {
        hle_ret16(c, 1);
        return CPU_OK;
    }
    hle_ret16(c, draw_shape(g, h, SH_RECT, q.left, q.top, q.right, q.bottom, 0, 0, NULL, NULL, 0, false));
    return CPU_OK;
}

static cpu_status g_ellipse(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int l = (int16_t)hle_u16(&a), t = (int16_t)hle_u16(&a), r = (int16_t)hle_u16(&a), b = (int16_t)hle_u16(&a);
    fb_rect q;
    GCHECK;
    if (!dev_box(c, g, h, l, t, r, b, &q) || fb_rect_empty(&q)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    hle_ret16(c, draw_shape(g, h, SH_ELLIPSE, q.left, q.top, q.right, q.bottom, 0, 0, NULL, NULL, 0, false));
    return CPU_OK;
}

static cpu_status g_roundrect(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int l = (int16_t)hle_u16(&a), t = (int16_t)hle_u16(&a), r = (int16_t)hle_u16(&a), b = (int16_t)hle_u16(&a);
    int ew = (int16_t)hle_u16(&a), eh = (int16_t)hle_u16(&a);
    fb_rect q;
    gdc *dc;
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc || !dev_box(c, g, h, l, t, r, b, &q) || fb_rect_empty(&q)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    hle_ret16(c, draw_shape(g, h, SH_ROUND, q.left, q.top, q.right, q.bottom, abs(gdc_scale_x(dc, ew)),
                            abs(gdc_scale_y(dc, eh)), NULL, NULL, 0, false));
    return CPU_OK;
}

static cpu_status poly_common(hle_call *c, bool multi) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off, csel = 0, coff = 0;
    int n, *pts = NULL, npoly = 1, i, total;
    int16_t cnt1, *counts = NULL;
    gctx x;
    gdc *dc;
    bool ok = false;
    hle_far(&a, &sel, &off);
    if (multi) hle_far(&a, &csel, &coff);
    n = (int16_t)hle_u16(&a);
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc || !gctx_open(g, h, &x)) {
        gctx_close(&x);
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (multi) {
        npoly = n;
        if (npoly < 1 || npoly > 4096) goto done;
        counts = malloc((size_t)npoly * sizeof *counts);
        if (!counts) goto done;
        total = 0;
        for (i = 0; i < npoly; i++) {
            const uint8_t *q = hle_ptr(c, csel, (uint32_t)coff + 2u * (unsigned)i, 2);
            if (!q) {
                free(counts);
                gctx_close(&x);
                return CPU_E_SEL;
            }
            counts[i] = (int16_t)gld16(q);
            if (counts[i] < 0) goto done;
            total += counts[i];
        }
        n = total;
    } else {
        cnt1 = (int16_t)n;
        counts = &cnt1;
    }
    if (n >= 2) {
        pts = read_points(c, dc, &x, sel, off, n);
        if (!pts) {
            if (multi) free(counts);
            gctx_close(&x);
            return CPU_E_SEL;
        }
        gctx_close(&x);
        ok = draw_shape(g, h, SH_POLY, 0, 0, 0, 0, 0, 0, pts, counts, npoly, dc->polyfill == 2);
        free(pts);
        if (multi) free(counts);
        hle_ret16(c, ok);
        return CPU_OK;
    }
done:
    if (multi) free(counts);
    gctx_close(&x);
    hle_ret16(c, 0);
    return CPU_OK;
}

static cpu_status g_polygon(hle_call *c) { return poly_common(c, false); }
static cpu_status g_polypolygon(hle_call *c) { return poly_common(c, true); }

/* ---- arcs, chords, pies ---------------------------------------------------------------------------------------- */

typedef enum { ARC_ARC, ARC_PIE, ARC_CHORD } arc_kind;

static double angle_of(double u, double v) {
    double t = atan2(v, u);
    return t < 0 ? t + 2 * PI_ : t;
}

static cpu_status arc_common(hle_call *c, arc_kind kind) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int l = (int16_t)hle_u16(&a), t = (int16_t)hle_u16(&a), r = (int16_t)hle_u16(&a), b = (int16_t)hle_u16(&a);
    int xs = (int16_t)hle_u16(&a), ys = (int16_t)hle_u16(&a), xe = (int16_t)hle_u16(&a), ye = (int16_t)hle_u16(&a);
    gdc *dc;
    dctx d;
    gmask m;
    grgn shape;
    fb_rect q, box;
    double cx, cy, ra, rb, as, ae, sweep;
    int x, y, sx0, sy0, sx1, sy1;
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc || !dev_box(c, g, h, l, t, r, b, &q) || fb_rect_empty(&q)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    {
        gsurf s;
        gdc_surface(g, dc, &s);
        sx0 = gdc_lx(dc, xs) + s.ox; sy0 = gdc_ly(dc, ys) + s.oy;
        sx1 = gdc_lx(dc, xe) + s.ox; sy1 = gdc_ly(dc, ye) + s.oy;
    }
    grgn_init(&shape);
    if (!grgn_ellipse(&shape, q.left, q.top, q.right, q.bottom) || !dctx_open(g, h, &d)) {
        grgn_free(&shape);
        dctx_close(&d);
        hle_ret16(c, 0);
        return CPU_OK;
    }
    cx = (q.left + q.right) / 2.0;
    cy = (q.top + q.bottom) / 2.0;
    ra = (q.right - q.left) / 2.0;
    rb = (q.bottom - q.top) / 2.0;
    as = angle_of((sx0 - cx) / ra, (cy - sy0) / rb);
    ae = angle_of((sx1 - cx) / ra, (cy - sy1) / rb);
    sweep = ae - as;
    if (sweep <= 0) sweep += 2 * PI_;
    if (fabs(as - ae) < 1e-9) sweep = 2 * PI_ + 1;                     /* same point: the whole ellipse */
    box = expand(shape.box, d.pw + 1);
    if (mask_open(&m, &d.x, &box) && m.m) {
        stroker s;
        if (d.pw) stroker_init(&s, &m, d.pen, d.pw), s.ndash = 0;
        /* the arc: boundary pixels of the ellipse whose angle lies in the sweep */
        for (y = box.top; y < box.bottom; y++)
            for (x = box.left; x < box.right; x++) {
                bool in = grgn_has_point(&shape, x, y), edge, onarc;
                double ang, rel;
                if (!in) continue;
                edge = !grgn_has_point(&shape, x - 1, y) || !grgn_has_point(&shape, x + 1, y) ||
                       !grgn_has_point(&shape, x, y - 1) || !grgn_has_point(&shape, x, y + 1);
                ang = angle_of((x + 0.5 - cx) / ra, (cy - (y + 0.5)) / rb);
                rel = ang - as;
                if (rel < 0) rel += 2 * PI_;
                onarc = rel <= sweep;
                if (kind == ARC_ARC) {
                    if (edge && onarc && d.pw) emit(&s, x, y);
                } else if (kind == ARC_PIE) {
                    if (onarc) mk(&m, x, y, MK_FILL);
                    if (edge && onarc && d.pw) emit(&s, x, y);
                } else {
                    /* chord: the side of the chord line that holds the arc's midpoint */
                    double mid = as + sweep / 2, mx = cx + ra * cos(mid), my = cy - rb * sin(mid);
                    double side_mid = (sx1 - sx0) * (my - sy0) - (sy1 - sy0) * (mx - sx0);
                    double side_px = (sx1 - sx0) * (y + 0.5 - sy0) - (sy1 - sy0) * (x + 0.5 - sx0);
                    bool same = side_mid * side_px > 0;
                    if (sweep > 2 * PI_) same = true;
                    if (same) mk(&m, x, y, MK_FILL);
                    if (edge && onarc && d.pw) emit(&s, x, y);
                }
            }
        if (d.pw && kind == ARC_PIE) {
            int cxi = (int)floor(cx), cyi = (int)floor(cy);
            stroker_init(&s, &m, d.pen, d.pw);
            stroke_seg(&s, cxi, cyi, sx0, sy0, true);
            stroker_init(&s, &m, d.pen, d.pw);
            stroke_seg(&s, cxi, cyi, sx1, sy1, true);
        } else if (d.pw && kind == ARC_CHORD) {
            stroker_init(&s, &m, d.pen, d.pw);
            stroke_seg(&s, sx0, sy0, sx1, sy1, true);
        }
        mask_apply(&d.x, &m, kind == ARC_ARC || !d.brush ? NULL : &d.bp, d.penpix, d.pw > 0, d.x.rop2);
    }
    mask_close(&m);
    grgn_free(&shape);
    dctx_close(&d);
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status g_arc(hle_call *c) { return arc_common(c, ARC_ARC); }
static cpu_status g_pie(hle_call *c) { return arc_common(c, ARC_PIE); }
static cpu_status g_chord(hle_call *c) { return arc_common(c, ARC_CHORD); }

/* ---- pixels and flood fill ------------------------------------------------------------------------------------------ */

static cpu_status g_setpixel(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int xx = (int16_t)hle_u16(&a), yy = (int16_t)hle_u16(&a);
    uint32_t color = hle_u32(&a), res = 0xFFFFFFFFu;
    gctx x;
    GCHECK;
    if (gctx_open(g, h, &x)) {
        int sx = gdc_lx(x.dc, xx) + x.ox, sy = gdc_ly(x.dc, yy) + x.oy;
        if (visible(&x, sx, sy)) {
            uint8_t p = gctx_pixel(&x, color);
            put(&x, sx, sy, p, x.rop2 == 13 ? 13 : x.rop2);
            g->fb->dirty++;
            res = gctx_colorref_of(&x, x.pix[(size_t)sy * x.sw + sx]);
        }
    }
    gctx_close(&x);
    hle_ret32(c, res);
    return CPU_OK;
}

static cpu_status g_getpixel(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int xx = (int16_t)hle_u16(&a), yy = (int16_t)hle_u16(&a);
    uint32_t res = 0xFFFFFFFFu;
    gctx x;
    GCHECK;
    if (gctx_open(g, h, &x)) {
        int sx = gdc_lx(x.dc, xx) + x.ox, sy = gdc_ly(x.dc, yy) + x.oy;
        if (visible(&x, sx, sy)) res = gctx_colorref_of(&x, x.pix[(size_t)sy * x.sw + sx]);
    }
    gctx_close(&x);
    hle_ret32(c, res);
    return CPU_OK;
}

/* Flood fill with the DC's brush. surface == false: fill until pixels of `color` (border);
 * true: fill the connected area of pixels equal to `color`. */
static bool flood(gdi_t *g, uint16_t h, int xx, int yy, uint32_t color, bool surface) {
    dctx d;
    gmask m;
    int sx, sy, *stack, sp = 0, cap = 1024, w, hh;
    uint8_t target, start;
    bool ok = false;
    if (!dctx_open(g, h, &d)) {
        dctx_close(&d);
        return false;
    }
    sx = gdc_lx(d.x.dc, xx) + d.x.ox;
    sy = gdc_ly(d.x.dc, yy) + d.x.oy;
    if (!visible(&d.x, sx, sy)) goto out;
    target = gctx_pixel(&d.x, color);
    start = d.x.pix[(size_t)sy * d.x.sw + sx];
    if (surface ? start != target : start == target) goto out;
    {
        fb_rect box = d.x.clipr.box;
        if (!mask_open(&m, &d.x, &box) || !m.m) {
            mask_close(&m);
            goto out;
        }
    }
    w = m.w;
    hh = m.h;
    stack = malloc((size_t)cap * 2 * sizeof *stack);
    if (!stack) {
        mask_close(&m);
        goto out;
    }
    stack[0] = sx;
    stack[1] = sy;
    sp = 1;
    while (sp) {
        int px = stack[2 * (sp - 1)], py = stack[2 * (sp - 1) + 1], l, r, i, dy;
        sp--;
        if (px - m.x0 < 0 || px - m.x0 >= w || py - m.y0 < 0 || py - m.y0 >= hh) continue;
#define OPEN(X, Y) (grgn_has_point(&d.x.clipr, (X), (Y)) && !(m.m[(size_t)((Y) - m.y0) * w + ((X) - m.x0)] & MK_FILL) && \
                    (surface ? d.x.pix[(size_t)(Y) * d.x.sw + (X)] == target : d.x.pix[(size_t)(Y) * d.x.sw + (X)] != target))
        if (!OPEN(px, py)) continue;
        for (l = px; OPEN(l - 1, py) && l - 1 >= m.x0; l--) {}
        for (r = px; r + 1 < m.x0 + w && OPEN(r + 1, py); r++) {}
        for (i = l; i <= r; i++) m.m[(size_t)(py - m.y0) * w + (i - m.x0)] |= MK_FILL;
        for (dy = -1; dy <= 1; dy += 2) {
            int yy2 = py + dy;
            bool run = false;
            if (yy2 < m.y0 || yy2 >= m.y0 + hh) continue;
            for (i = l; i <= r; i++) {
                bool o = OPEN(i, yy2);
                if (o && !run) {
                    if (sp + 1 >= cap) {
                        int *ns = realloc(stack, (size_t)cap * 4 * sizeof *stack);
                        if (!ns) break;
                        stack = ns;
                        cap *= 2;
                    }
                    stack[2 * sp] = i;
                    stack[2 * sp + 1] = yy2;
                    sp++;
                }
                run = o;
            }
        }
#undef OPEN
    }
    free(stack);
    mask_apply(&d.x, &m, d.brush ? &d.bp : NULL, 0, false, 13);
    mask_close(&m);
    ok = true;
out:
    dctx_close(&d);
    return ok;
}

static cpu_status g_floodfill(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a);
    uint32_t color = hle_u32(&a);
    GCHECK;
    hle_ret16(c, flood(g, h, x, y, color, false));
    return CPU_OK;
}

static cpu_status g_extfloodfill(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a);
    uint32_t color = hle_u32(&a);
    uint16_t type = hle_u16(&a);
    GCHECK;
    hle_ret16(c, type <= 1 && flood(g, h, x, y, color, type == 1));
    return CPU_OK;
}

/* ---- regions and rectangles filled with brushes ---------------------------------------------------------------------- */

static void fill_region(gctx *x, const grgn *rg, const bpat *bp, int rop2, bool invert) {
    int i;
    for (i = 0; i < rg->n; i++) {
        fb_rect q = rg->r[i];
        q.left += x->ox; q.right += x->ox; q.top += x->oy; q.bottom += x->oy;
        if (invert) invert_rect_surface(x, &q);
        else fill_rect_surface(x, &q, bp, rop2);
    }
}

static cpu_status region_fill(hle_call *c, int kind) {   /* 0 FillRgn 1 FrameRgn 2 InvertRgn 3 PaintRgn */
    GBEGIN;
    uint16_t h = hle_u16(&a), hr = hle_u16(&a), hb = 0;
    int fw = 0, fh = 0;
    gctx x;
    gobj *ro, *bo;
    bpat bp;
    grgn tmp, inner, sh;
    bool ok = false;
    if (kind == 0 || kind == 1) hb = hle_u16(&a);
    if (kind == 1) {
        fw = (int16_t)hle_u16(&a);
        fh = (int16_t)hle_u16(&a);
    }
    GCHECK;
    ro = gobj_typed(g, hr, GO_REGION);
    if (!ro || !gctx_open(g, h, &x)) {
        gctx_close(&x);
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (kind == 3) {
        bo = gobj_typed(g, x.dc->brush, GO_BRUSH);
    } else {
        bo = gobj_typed(g, hb, GO_BRUSH);
    }
    memset(&bp, 0, sizeof bp);
    if (bo) brush_prepare(&x, &bo->u.brush, &bp);
    grgn_init(&tmp);
    grgn_init(&inner);
    grgn_init(&sh);
    if (kind == 1) {
        int dw = abs(gdc_scale_x(x.dc, fw)), dh = abs(gdc_scale_y(x.dc, fh));
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        ok = grgn_copy(&inner, &ro->u.rgn);
        if (ok) {
            const int off[4][2] = {{dw, 0}, {-dw, 0}, {0, dh}, {0, -dh}};
            int k;
            for (k = 0; ok && k < 4; k++) {
                ok = grgn_copy(&sh, &ro->u.rgn);
                if (ok) {
                    grgn_offset(&sh, off[k][0], off[k][1]);
                    ok = grgn_combine(&inner, &inner, &sh, RGN_AND);
                }
            }
            ok = ok && grgn_combine(&tmp, &ro->u.rgn, &inner, RGN_DIFF);
        }
        if (ok && bo) fill_region(&x, &tmp, &bp, 13, false);
    } else if (kind == 2) {
        ok = true;
        fill_region(&x, &ro->u.rgn, NULL, 13, true);
    } else if (bo) {
        ok = true;
        fill_region(&x, &ro->u.rgn, &bp, kind == 3 ? x.rop2 : 13, false);
    }
    grgn_free(&tmp);
    grgn_free(&inner);
    grgn_free(&sh);
    gctx_close(&x);
    hle_ret16(c, ok);
    return CPU_OK;
}

static cpu_status g_fillrgn(hle_call *c) { return region_fill(c, 0); }
static cpu_status g_framergn(hle_call *c) { return region_fill(c, 1); }
static cpu_status g_invertrgn(hle_call *c) { return region_fill(c, 2); }
static cpu_status g_paintrgn(hle_call *c) { return region_fill(c, 3); }

/* USER: FillRect, FrameRect, InvertRect, DrawFocusRect. A brush argument of 1..21 is
 * a system colour index plus one. */
static cpu_status user_rect(hle_call *c, int kind) {   /* 0 Fill 1 Frame 2 Invert 3 Focus */
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off, hb = 0;
    const uint8_t *p;
    gctx x;
    fb_rect q;
    gobj *bo;
    bpat bp;
    gbrush sys;
    bool ok = false;
    hle_far(&a, &sel, &off);
    if (kind <= 1) hb = hle_u16(&a);
    GCHECK;
    if (!(p = hle_ptr(c, sel, off, 8))) return CPU_E_SEL;
    if (!gctx_open(g, h, &x)) {
        gctx_close(&x);
        hle_ret16(c, 0);
        return CPU_OK;
    }
    q = dev_rect_of(x.dc, (int16_t)gld16(p), (int16_t)gld16(p + 2), (int16_t)gld16(p + 4), (int16_t)gld16(p + 6));
    q.left += x.ox; q.right += x.ox; q.top += x.oy; q.bottom += x.oy;
    memset(&bp, 0, sizeof bp);
    if (kind <= 1) {
        bo = gobj_typed(g, hb, GO_BRUSH);
        if (bo) brush_prepare(&x, &bo->u.brush, &bp);
        else if (hb >= 1 && hb <= COLOR_NCOLORS) {
            memset(&sys, 0, sizeof sys);
            sys.style = BS_SOLID;
            sys.color = wm_sys_color(hb - 1);
            brush_prepare(&x, &sys, &bp);
        } else {
            gctx_close(&x);
            hle_ret16(c, 0);
            return CPU_OK;
        }
    }
    if (fb_rect_empty(&q)) {
        ok = true;
    } else if (kind == 0) {
        fill_rect_surface(&x, &q, &bp, 13);
        ok = true;
    } else if (kind == 1) {
        fb_rect e[4];
        int i;
        e[0] = (fb_rect){q.left, q.top, q.right, q.top + 1};
        e[1] = (fb_rect){q.left, q.bottom - 1, q.right, q.bottom};
        e[2] = (fb_rect){q.left, q.top + 1, q.left + 1, q.bottom - 1};
        e[3] = (fb_rect){q.right - 1, q.top + 1, q.right, q.bottom - 1};
        for (i = 0; i < 4; i++)
            if (!fb_rect_empty(&e[i])) fill_rect_surface(&x, &e[i], &bp, 13);
        ok = true;
    } else if (kind == 2) {
        invert_rect_surface(&x, &q);
        ok = true;
    } else {
        int sx, sy;                     /* dotted XOR border: every other pixel */
        for (sx = q.left; sx < q.right; sx++)
            for (sy = q.top; sy < q.bottom; sy++)
                if ((sx == q.left || sx == q.right - 1 || sy == q.top || sy == q.bottom - 1) && ((sx + sy) & 1) == 0 &&
                    visible(&x, sx, sy))
                    put(&x, sx, sy, 0, 6);
        g->fb->dirty++;
        ok = true;
    }
    gctx_close(&x);
    hle_ret16(c, ok);
    return CPU_OK;
}

static cpu_status u_fillrect(hle_call *c) { return user_rect(c, 0); }
static cpu_status u_framerect(hle_call *c) { return user_rect(c, 1); }
static cpu_status u_invertrect(hle_call *c) { return user_rect(c, 2); }
static cpu_status u_drawfocusrect(hle_call *c) { return user_rect(c, 3); }

/* LineDDA(x1, y1, x2, y2, lpProc, lpData): the proc is called with (x, y, lpData) for every point but the last. */
static cpu_status g_linedda(hle_call *c) {
    GBEGIN;
    int x0 = (int16_t)hle_u16(&a), y0 = (int16_t)hle_u16(&a), x1 = (int16_t)hle_u16(&a), y1 = (int16_t)hle_u16(&a);
    uint32_t proc = hle_u32(&a), data = hle_u32(&a), ret;
    int dx, dy, sx, sy, i, d, x, y, n;
    int16_t *pts;
    GCHECK;
    dx = abs(x1 - x0);
    dy = abs(y1 - y0);
    sx = x1 >= x0 ? 1 : -1;
    sy = y1 >= y0 ? 1 : -1;
    n = dx >= dy ? dx : dy;
    pts = malloc((size_t)(n ? n : 1) * 2 * sizeof *pts);
    if (!pts) return CPU_E_UD;
    x = x0;
    y = y0;
    d = dx >= dy ? 2 * dy - dx : 2 * dx - dy;
    for (i = 0; i < n; i++) {
        pts[2 * i] = (int16_t)x;
        pts[2 * i + 1] = (int16_t)y;
        if (dx >= dy) {
            if (d > 0) { y += sy; d -= 2 * dx; }
            d += 2 * dy;
            x += sx;
        } else {
            if (d > 0) { x += sx; d -= 2 * dy; }
            d += 2 * dx;
            y += sy;
        }
    }
    for (i = 0; i < n; i++) {                   /* the guest runs after the point list is complete */
        uint16_t args[4];
        args[0] = (uint16_t)pts[2 * i];
        args[1] = (uint16_t)pts[2 * i + 1];
        args[2] = (uint16_t)(data >> 16);
        args[3] = (uint16_t)data;
        if (!user_call_guest(g->user, proc, args, 4, &ret)) {
            free(pts);
            return user_abort(g->user);
        }
    }
    free(pts);
    hle_ret16(c, 1);
    return CPU_OK;
}

#define G GDI_MODULE
#define U USER_MODULE_NAME
const hle_def gdi_draw_defs[] = {
    {G, 100, "LineDDA", 16, g_linedda},                 /* UNV */
    {G, 19, "LineTo", 6, g_lineto},
    {G, 23, "Arc", 18, g_arc},                          /* UNV */
    {G, 24, "Ellipse", 10, g_ellipse},
    {G, 25, "FloodFill", 10, g_floodfill},
    {G, 26, "Pie", 18, g_pie},                          /* UNV */
    {G, 27, "Rectangle", 10, g_rectangle},
    {G, 28, "RoundRect", 14, g_roundrect},
    {G, 31, "SetPixel", 10, g_setpixel},                /* UNV */
    {G, 36, "Polygon", 8, g_polygon},
    {G, 37, "Polyline", 8, g_polyline},
    {G, 40, "FillRgn", 6, g_fillrgn},
    {G, 41, "FrameRgn", 10, g_framergn},
    {G, 42, "InvertRgn", 4, g_invertrgn},
    {G, 43, "PaintRgn", 4, g_paintrgn},
    {G, 83, "GetPixel", 6, g_getpixel},
    {G, 348, "Chord", 18, g_chord},                     /* UNV */
    {G, 372, "ExtFloodFill", 12, g_extfloodfill},
    {G, 450, "PolyPolygon", 12, g_polypolygon},
    {U, 81, "FillRect", 8, u_fillrect},
    {U, 82, "InvertRect", 6, u_invertrect},
    {U, 83, "FrameRect", 8, u_framerect},               /* UNV */
    {U, 466, "DrawFocusRect", 6, u_drawfocusrect},      /* UNV */
};
#undef G
#undef U
const size_t gdi_draw_ndefs = sizeof gdi_draw_defs / sizeof gdi_draw_defs[0];
