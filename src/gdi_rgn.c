/* Regions as banded rectangle lists, and the region / clip API. See gdi_priv.h
 * for the canonical form. */
#include <math.h>

#include "gdi_priv.h"

/* ---- storage ---------------------------------------------------------------------- */

void grgn_init(grgn *g) { memset(g, 0, sizeof *g); }

void grgn_free(grgn *g) {
    free(g->r);
    memset(g, 0, sizeof *g);
}

static bool reserve(grgn *g, int n) {
    fb_rect *p;
    int cap;
    if (n <= g->cap) return true;
    cap = g->cap ? g->cap * 2 : 8;
    while (cap < n) cap *= 2;
    if (!(p = realloc(g->r, (size_t)cap * sizeof *p))) return false;
    g->r = p;
    g->cap = cap;
    return true;
}

static void refresh_box(grgn *g) {
    int i;
    if (!g->n) {
        memset(&g->box, 0, sizeof g->box);
        return;
    }
    g->box = g->r[0];
    for (i = 1; i < g->n; i++) {
        if (g->r[i].left < g->box.left) g->box.left = g->r[i].left;
        if (g->r[i].right > g->box.right) g->box.right = g->r[i].right;
        if (g->r[i].bottom > g->box.bottom) g->box.bottom = g->r[i].bottom;
    }
    g->box.top = g->r[0].top;
}

bool grgn_copy(grgn *dst, const grgn *src) {
    fb_rect *n = NULL;
    if (dst == src) return true;
    if (src->n) {
        n = malloc((size_t)src->n * sizeof *n);
        if (!n) return false;
        memcpy(n, src->r, (size_t)src->n * sizeof *n);
    }
    free(dst->r);
    dst->r = n;
    dst->n = dst->cap = src->n;
    dst->box = src->box;
    return true;
}

int grgn_kind(const grgn *g) { return g->n == 0 ? NULLREGION : g->n == 1 ? SIMPLEREGION : COMPLEXREGION; }

bool grgn_set_rect(grgn *g, int l, int t, int r, int b) {
    int x;
    if (l > r) x = l, l = r, r = x;
    if (t > b) x = t, t = b, b = x;
    g->n = 0;
    if (l < r && t < b) {
        if (!reserve(g, 1)) return false;
        g->r[0].left = l;
        g->r[0].top = t;
        g->r[0].right = r;
        g->r[0].bottom = b;
        g->n = 1;
    }
    refresh_box(g);
    return true;
}

void grgn_offset(grgn *g, int dx, int dy) {
    int i;
    for (i = 0; i < g->n; i++) {
        g->r[i].left += dx;
        g->r[i].right += dx;
        g->r[i].top += dy;
        g->r[i].bottom += dy;
    }
    g->box.left += dx;
    g->box.right += dx;
    g->box.top += dy;
    g->box.bottom += dy;
}

bool grgn_equal(const grgn *a, const grgn *b) {
    return a->n == b->n && (a->n == 0 || memcmp(a->r, b->r, (size_t)a->n * sizeof *a->r) == 0);
}

/* index of the first rectangle that ends below y (rectangles are ordered by band) */
static int first_below(const grgn *g, int y) {
    int lo = 0, hi = g->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g->r[mid].bottom > y) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

bool grgn_has_point(const grgn *g, int x, int y) {
    int i;
    if (x < g->box.left || x >= g->box.right || y < g->box.top || y >= g->box.bottom) return false;
    for (i = first_below(g, y); i < g->n && g->r[i].top <= y; i++) {
        if (g->r[i].left > x) return false;
        if (x < g->r[i].right) return true;
    }
    return false;
}

bool grgn_overlaps_rect(const grgn *g, const fb_rect *q) {
    int i;
    fb_rect t;
    if (!fb_rect_intersect(&t, &g->box, q)) return false;
    for (i = first_below(g, q->top); i < g->n && g->r[i].top < q->bottom; i++)
        if (g->r[i].right > q->left && g->r[i].left < q->right) return true;
    return false;
}

/* ---- boolean operations --------------------------------------------------------------- */

typedef struct { int *v; int n, cap; } ivl; /* flattened [l, r) pairs */

static bool ivl_push(ivl *o, int l, int r) {
    if (o->n >= 2 && o->v[o->n - 1] == l) {
        o->v[o->n - 1] = r;
        return true;
    }
    if (o->n + 2 > o->cap) {
        int c = o->cap ? o->cap * 2 : 16;
        int *p = realloc(o->v, (size_t)c * sizeof *p);
        if (!p) return false;
        o->v = p;
        o->cap = c;
    }
    o->v[o->n++] = l;
    o->v[o->n++] = r;
    return true;
}

static bool row_ivls(const grgn *g, int y, ivl *o) {
    int i;
    o->n = 0;
    for (i = first_below(g, y); i < g->n && g->r[i].top <= y; i++)
        if (!ivl_push(o, g->r[i].left, g->r[i].right)) return false;
    return true;
}

static bool in_ivls(const ivl *s, int x) {
    int i;
    for (i = 0; i < s->n; i += 2)
        if (x >= s->v[i] && x < s->v[i + 1]) return true;
    return false;
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

static bool want(int mode, bool a, bool b) {
    switch (mode) {
    case RGN_AND: return a && b;
    case RGN_OR: return a || b;
    case RGN_XOR: return a != b;
    default: return a && !b;
    }
}

static bool ivl_op(const ivl *a, const ivl *b, int mode, int *xs, ivl *o) {
    int n = 0, i, m;
    o->n = 0;
    for (i = 0; i < a->n; i++) xs[n++] = a->v[i];
    for (i = 0; i < b->n; i++) xs[n++] = b->v[i];
    qsort(xs, (size_t)n, sizeof *xs, cmp_int);
    for (i = 0, m = 0; i < n; i++)
        if (!m || xs[m - 1] != xs[i]) xs[m++] = xs[i];
    for (i = 0; i + 1 < m; i++)
        if (want(mode, in_ivls(a, xs[i]), in_ivls(b, xs[i])) && !ivl_push(o, xs[i], xs[i + 1])) return false;
    return true;
}

typedef struct {
    grgn *out;
    int prev_first, prev_n, prev_bottom; /* rectangles of the previous band */
} builder;

/* Adds one band [y0, y1) with the intervals iv; merges with the previous band when identical. */
static bool add_band(builder *b, int y0, int y1, const ivl *iv) {
    grgn *g = b->out;
    int k, cnt = iv->n / 2;
    if (!cnt) {
        b->prev_n = 0;
        return true;
    }
    if (b->prev_n == cnt && b->prev_bottom == y0) {
        bool same = true;
        for (k = 0; k < cnt && same; k++)
            same = g->r[b->prev_first + k].left == iv->v[2 * k] && g->r[b->prev_first + k].right == iv->v[2 * k + 1];
        if (same) {
            for (k = 0; k < cnt; k++) g->r[b->prev_first + k].bottom = y1;
            b->prev_bottom = y1;
            return true;
        }
    }
    if (!reserve(g, g->n + cnt)) return false;
    b->prev_first = g->n;
    for (k = 0; k < cnt; k++) {
        fb_rect *r = &g->r[g->n++];
        r->left = iv->v[2 * k];
        r->right = iv->v[2 * k + 1];
        r->top = y0;
        r->bottom = y1;
    }
    b->prev_n = cnt;
    b->prev_bottom = y1;
    return true;
}

bool grgn_combine(grgn *dst, const grgn *a, const grgn *b, int mode) {
    grgn res;
    int *ys, ny = 0, i, m, *xs;
    ivl ia = {0}, ib = {0}, io = {0};
    builder bl;
    bool ok = true;
    if (mode == RGN_COPY) return grgn_copy(dst, a);
    grgn_init(&res);
    ys = malloc((size_t)(2 * (a->n + b->n) + 1) * sizeof *ys);
    xs = malloc((size_t)(2 * (a->n + b->n) + 4) * sizeof *xs);
    if (!ys || !xs) {
        free(ys);
        free(xs);
        return false;
    }
    for (i = 0; i < a->n; i++) ys[ny++] = a->r[i].top, ys[ny++] = a->r[i].bottom;
    for (i = 0; i < b->n; i++) ys[ny++] = b->r[i].top, ys[ny++] = b->r[i].bottom;
    qsort(ys, (size_t)ny, sizeof *ys, cmp_int);
    for (i = 0, m = 0; i < ny; i++)
        if (!m || ys[m - 1] != ys[i]) ys[m++] = ys[i];
    bl.out = &res;
    bl.prev_first = bl.prev_n = bl.prev_bottom = 0;
    for (i = 0; ok && i + 1 < m; i++) {
        ok = row_ivls(a, ys[i], &ia) && row_ivls(b, ys[i], &ib) && ivl_op(&ia, &ib, mode, xs, &io) &&
             add_band(&bl, ys[i], ys[i + 1], &io);
    }
    free(ys);
    free(xs);
    free(ia.v);
    free(ib.v);
    free(io.v);
    if (!ok) {
        grgn_free(&res);
        return false;
    }
    refresh_box(&res);
    free(dst->r);
    *dst = res;
    return true;
}

bool grgn_intersect_rect(grgn *g, const fb_rect *r) {
    grgn t;
    bool ok;
    grgn_init(&t);
    ok = grgn_set_rect(&t, r->left, r->top, r->right, r->bottom) && grgn_combine(g, g, &t, RGN_AND);
    grgn_free(&t);
    return ok;
}

bool grgn_subtract_rect(grgn *g, const fb_rect *r) {
    grgn t;
    bool ok;
    grgn_init(&t);
    ok = grgn_set_rect(&t, r->left, r->top, r->right, r->bottom) && grgn_combine(g, g, &t, RGN_DIFF);
    grgn_free(&t);
    return ok;
}

/* ---- building from masks and shapes ------------------------------------------------------ */

bool grgn_from_mask(grgn *g, const uint8_t *mask, int w, int h, int ox, int oy) {
    grgn res;
    builder bl;
    ivl iv = {0};
    int x, y;
    grgn_init(&res);
    bl.out = &res;
    bl.prev_first = bl.prev_n = bl.prev_bottom = 0;
    for (y = 0; y < h; y++) {
        iv.n = 0;
        for (x = 0; x < w;) {
            int s;
            if (!mask[(size_t)y * w + x]) {
                x++;
                continue;
            }
            s = x;
            while (x < w && mask[(size_t)y * w + x]) x++;
            if (!ivl_push(&iv, s + ox, x + ox)) goto fail;
        }
        if (!add_band(&bl, y + oy, y + oy + 1, &iv)) goto fail;
    }
    free(iv.v);
    refresh_box(&res);
    free(g->r);
    *g = res;
    return true;
fail:
    free(iv.v);
    grgn_free(&res);
    return false;
}

/* rows of one span each: x[2*row] .. x[2*row+1], empty when the first is not below the second */
static bool from_rows(grgn *g, int y0, int rows, const int *x) {
    grgn res;
    builder bl;
    ivl iv = {0};
    int y;
    grgn_init(&res);
    bl.out = &res;
    bl.prev_first = bl.prev_n = bl.prev_bottom = 0;
    for (y = 0; y < rows; y++) {
        iv.n = 0;
        if (x[2 * y] < x[2 * y + 1] && !ivl_push(&iv, x[2 * y], x[2 * y + 1])) goto fail;
        if (!add_band(&bl, y0 + y, y0 + y + 1, &iv)) goto fail;
    }
    free(iv.v);
    refresh_box(&res);
    free(g->r);
    *g = res;
    return true;
fail:
    free(iv.v);
    grgn_free(&res);
    return false;
}

static bool in_ellipse(double cx, double cy, double a, double b, int x, int y) {
    double dx = (x + 0.5 - cx) / a, dy = (y + 0.5 - cy) / b;
    return dx * dx + dy * dy <= 1.0;
}

bool grgn_ellipse(grgn *g, int l, int t, int r, int b) {
    int rows, y, *xs;
    double cx, cy, a, bb;
    bool ok;
    if (l > r) y = l, l = r, r = y;
    if (t > b) y = t, t = b, b = y;
    if (l >= r || t >= b) return grgn_set_rect(g, 0, 0, 0, 0);
    rows = b - t;
    xs = calloc((size_t)rows * 2, sizeof *xs);
    if (!xs) return false;
    cx = (l + r) / 2.0;
    cy = (t + b) / 2.0;
    a = (r - l) / 2.0;
    bb = (b - t) / 2.0;
    for (y = 0; y < rows; y++) {
        int lo = l, hi = r - 1;
        while (lo <= hi && !in_ellipse(cx, cy, a, bb, lo, t + y)) lo++;
        while (hi >= lo && !in_ellipse(cx, cy, a, bb, hi, t + y)) hi--;
        xs[2 * y] = lo <= hi ? lo : 0;
        xs[2 * y + 1] = lo <= hi ? hi + 1 : 0;
    }
    ok = from_rows(g, t, rows, xs);
    free(xs);
    return ok;
}

bool grgn_roundrect(grgn *g, int l, int t, int r, int b, int ew, int eh) {
    int rows, y, x, *xs;
    double ex, ey;
    bool ok;
    if (l > r) y = l, l = r, r = y;
    if (t > b) y = t, t = b, b = y;
    if (l >= r || t >= b) return grgn_set_rect(g, 0, 0, 0, 0);
    if (ew < 0) ew = -ew;
    if (eh < 0) eh = -eh;
    if (ew > r - l) ew = r - l;
    if (eh > b - t) eh = b - t;
    ex = ew / 2.0;
    ey = eh / 2.0;
    rows = b - t;
    xs = calloc((size_t)rows * 2, sizeof *xs);
    if (!xs) return false;
    for (y = 0; y < rows; y++) {
        int lo = r, hi = l - 1;
        for (x = l; x < r; x++) {
            double cx = x + 0.5, cy = t + y + 0.5, ox = cx, oy = cy;
            bool in = true;
            if (cx < l + ex) ox = l + ex;
            else if (cx > r - ex) ox = r - ex;
            if (cy < t + ey) oy = t + ey;
            else if (cy > b - ey) oy = b - ey;
            if (ox != cx || oy != cy) {
                double dx = (cx - ox) / ex, dy = (cy - oy) / ey;
                in = dx * dx + dy * dy <= 1.0;
            }
            if (in) {
                if (x < lo) lo = x;
                hi = x;
            }
        }
        xs[2 * y] = lo <= hi ? lo : 0;
        xs[2 * y + 1] = lo <= hi ? hi + 1 : 0;
    }
    ok = from_rows(g, t, rows, xs);
    free(xs);
    return ok;
}

static int cmp_cross(const void *a, const void *b) {
    double x = ((const double *)a)[0], y = ((const double *)b)[0];
    return x < y ? -1 : x > y;
}

/* Fills the polygons that share one edge list: a pixel is set when its centre is
 * inside (even-odd over all edges, or non-zero winding). */
static bool polys_to_region(grgn *g, const int16_t *xy, const int16_t *counts, int npoly, bool winding) {
    int total = 0, i, k, ne = 0, miny = 0x7FFFFFFF, maxy = -0x7FFFFFFF, y, base = 0;
    double (*e)[4];     /* x0, y0, x1, y1 */
    double *cr;         /* crossings: x, direction */
    grgn acc;
    builder bl;
    ivl iv = {0};
    bool ok = true;
    for (i = 0; i < npoly; i++) total += counts[i];
    if (total < 3) return grgn_set_rect(g, 0, 0, 0, 0);
    e = malloc((size_t)total * sizeof *e);
    cr = malloc((size_t)(total + 1) * 2 * sizeof *cr);
    if (!e || !cr) {
        free(e);
        free(cr);
        return false;
    }
    for (i = 0; i < npoly; i++) {
        int n = counts[i];
        if (n >= 2)
            for (k = 0; k < n; k++) {
                int pa = base + k, pb = base + (k + 1) % n;
                e[ne][0] = xy[2 * pa];
                e[ne][1] = xy[2 * pa + 1];
                e[ne][2] = xy[2 * pb];
                e[ne][3] = xy[2 * pb + 1];
                ne++;
                if (xy[2 * pa + 1] < miny) miny = xy[2 * pa + 1];
                if (xy[2 * pa + 1] > maxy) maxy = xy[2 * pa + 1];
            }
        base += n;
    }
    if (ne < 3 || miny >= maxy) {
        free(e);
        free(cr);
        return grgn_set_rect(g, 0, 0, 0, 0);
    }
    grgn_init(&acc);
    bl.out = &acc;
    bl.prev_first = bl.prev_n = bl.prev_bottom = 0;
    for (y = miny; ok && y < maxy; y++) {
        double yc = y + 0.5;
        int nc = 0, wind = 0;
        bool inside = false;
        double start = 0;
        iv.n = 0;
        for (i = 0; i < ne; i++) {
            double y0 = e[i][1], y1 = e[i][3], lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
            if (y0 == y1 || yc < lo || yc >= hi) continue;
            cr[2 * nc] = e[i][0] + (yc - y0) * (e[i][2] - e[i][0]) / (y1 - y0);
            cr[2 * nc + 1] = y1 > y0 ? 1 : -1;
            nc++;
        }
        qsort(cr, (size_t)nc, 2 * sizeof *cr, cmp_cross);
        for (i = 0; i < nc; i++) {
            bool now;
            wind += (int)cr[2 * i + 1];
            now = winding ? wind != 0 : ((i + 1) & 1) != 0;
            if (now && !inside) {
                start = cr[2 * i];
                inside = true;
            } else if (!now && inside) {
                int xa = (int)ceil(start - 0.5), xb = (int)ceil(cr[2 * i] - 0.5);
                if (xb > xa && !ivl_push(&iv, xa, xb)) ok = false;
                inside = false;
            }
        }
        if (ok && !add_band(&bl, y, y + 1, &iv)) ok = false;
    }
    free(iv.v);
    free(e);
    free(cr);
    if (!ok) {
        grgn_free(&acc);
        return false;
    }
    refresh_box(&acc);
    free(g->r);
    *g = acc;
    return true;
}

bool grgn_polygon(grgn *g, const int16_t *xy, int n, bool winding) {
    int16_t c = (int16_t)n;
    return polys_to_region(g, xy, &c, 1, winding);
}

bool grgn_polypolygon(grgn *g, const int16_t *xy, const int16_t *counts, int npoly, bool winding) {
    return polys_to_region(g, xy, counts, npoly, winding);
}

/* ---- API: region objects ------------------------------------------------------------------- */

static gobj *rgn_obj(gdi_t *g, uint16_t h) { return gobj_typed(g, h, GO_REGION); }

static uint16_t new_region(gdi_t *g, gobj **out) { return gobj_new(g, GO_REGION, out); }

static cpu_status g_creatergn(hle_call *c) {
    GBEGIN;
    int l = (int16_t)hle_u16(&a), t = (int16_t)hle_u16(&a), r = (int16_t)hle_u16(&a), b = (int16_t)hle_u16(&a);
    gobj *o;
    uint16_t h;
    GCHECK;
    h = new_region(g, &o);
    if (h && !grgn_set_rect(&o->u.rgn, l, t, r, b)) h = 0;
    hle_ret16(c, h);
    return CPU_OK;
}

static bool get_rect16(hle_call *c, uint16_t sel, uint16_t off, int *l, int *t, int *r, int *b) {
    const uint8_t *p = hle_ptr(c, sel, off, 8);
    if (!p) return false;
    *l = (int16_t)gld16(p);
    *t = (int16_t)gld16(p + 2);
    *r = (int16_t)gld16(p + 4);
    *b = (int16_t)gld16(p + 6);
    return true;
}

static bool put_rect16(hle_call *c, uint16_t sel, uint16_t off, const fb_rect *q) {
    uint8_t *p = hle_ptr(c, sel, off, 8);
    if (!p) return false;
    gst16(p, (uint16_t)q->left);
    gst16(p + 2, (uint16_t)q->top);
    gst16(p + 4, (uint16_t)q->right);
    gst16(p + 6, (uint16_t)q->bottom);
    return true;
}

static cpu_status g_creatergnindirect(hle_call *c) {
    GBEGIN;
    uint16_t sel, off, h;
    int l, t, r, b;
    gobj *o;
    hle_far(&a, &sel, &off);
    GCHECK;
    if (!get_rect16(c, sel, off, &l, &t, &r, &b)) return CPU_E_SEL;
    h = new_region(g, &o);
    if (h && !grgn_set_rect(&o->u.rgn, l, t, r, b)) h = 0;
    hle_ret16(c, h);
    return CPU_OK;
}

static cpu_status g_createellipticrgn(hle_call *c) {
    GBEGIN;
    int l = (int16_t)hle_u16(&a), t = (int16_t)hle_u16(&a), r = (int16_t)hle_u16(&a), b = (int16_t)hle_u16(&a);
    gobj *o;
    uint16_t h;
    GCHECK;
    h = new_region(g, &o);
    if (h && !grgn_ellipse(&o->u.rgn, l, t, r, b)) h = 0;
    hle_ret16(c, h);
    return CPU_OK;
}

static cpu_status g_createellipticrgnindirect(hle_call *c) {
    GBEGIN;
    uint16_t sel, off, h;
    int l, t, r, b;
    gobj *o;
    hle_far(&a, &sel, &off);
    GCHECK;
    if (!get_rect16(c, sel, off, &l, &t, &r, &b)) return CPU_E_SEL;
    h = new_region(g, &o);
    if (h && !grgn_ellipse(&o->u.rgn, l, t, r, b)) h = 0;
    hle_ret16(c, h);
    return CPU_OK;
}

static cpu_status g_createroundrectrgn(hle_call *c) {
    GBEGIN;
    int l = (int16_t)hle_u16(&a), t = (int16_t)hle_u16(&a), r = (int16_t)hle_u16(&a), b = (int16_t)hle_u16(&a);
    int w = (int16_t)hle_u16(&a), hh = (int16_t)hle_u16(&a);
    gobj *o;
    uint16_t h;
    GCHECK;
    h = new_region(g, &o);
    if (h && !grgn_roundrect(&o->u.rgn, l, t, r, b, w, hh)) h = 0;
    hle_ret16(c, h);
    return CPU_OK;
}

static cpu_status g_createpolygonrgn(hle_call *c) {
    GBEGIN;
    uint16_t sel, off, n, mode, h;
    const uint8_t *p;
    int16_t *pts;
    int i;
    gobj *o;
    hle_far(&a, &sel, &off);
    n = hle_u16(&a);
    mode = hle_u16(&a);
    GCHECK;
    if (mode != 1 && mode != 2) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (n && !(p = hle_ptr(c, sel, off, 4u * n))) return CPU_E_SEL;
    pts = malloc((size_t)(n ? n : 1) * 4);
    if (!pts) return CPU_E_UD;
    for (i = 0; i < n; i++) {
        const uint8_t *q = hle_ptr(c, sel, (uint32_t)off + 4u * (unsigned)i, 4);
        pts[2 * i] = (int16_t)gld16(q);
        pts[2 * i + 1] = (int16_t)gld16(q + 2);
    }
    h = new_region(g, &o);
    if (h && !grgn_polygon(&o->u.rgn, pts, n, mode == 2)) h = 0;
    free(pts);
    hle_ret16(c, h);
    return CPU_OK;
}

static cpu_status g_combinergn(hle_call *c) {
    GBEGIN;
    uint16_t hd = hle_u16(&a), h1 = hle_u16(&a), h2 = hle_u16(&a);
    int mode = (int16_t)hle_u16(&a);
    gobj *d, *r1, *r2;
    GCHECK;
    d = rgn_obj(g, hd);
    r1 = rgn_obj(g, h1);
    r2 = rgn_obj(g, h2);
    if (!d || !r1 || (!r2 && mode != RGN_COPY) || mode < RGN_AND || mode > RGN_COPY ||
        !grgn_combine(&d->u.rgn, &r1->u.rgn, r2 ? &r2->u.rgn : &r1->u.rgn, mode)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    hle_ret16(c, (uint16_t)grgn_kind(&d->u.rgn));
    return CPU_OK;
}

static cpu_status g_equalrgn(hle_call *c) {
    GBEGIN;
    uint16_t h1 = hle_u16(&a), h2 = hle_u16(&a);
    gobj *r1, *r2;
    GCHECK;
    r1 = rgn_obj(g, h1);
    r2 = rgn_obj(g, h2);
    hle_ret16(c, r1 && r2 && grgn_equal(&r1->u.rgn, &r2->u.rgn));
    return CPU_OK;
}

static cpu_status g_offsetrgn(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a);
    gobj *o;
    GCHECK;
    o = rgn_obj(g, h);
    if (o) grgn_offset(&o->u.rgn, x, y);
    hle_ret16(c, o ? (uint16_t)grgn_kind(&o->u.rgn) : 0);
    return CPU_OK;
}

static cpu_status g_getrgnbox(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    gobj *o;
    hle_far(&a, &sel, &off);
    GCHECK;
    o = rgn_obj(g, h);
    if (!o) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (!put_rect16(c, sel, off, &o->u.rgn.box)) return CPU_E_SEL;
    hle_ret16(c, (uint16_t)grgn_kind(&o->u.rgn));
    return CPU_OK;
}

static cpu_status g_setrectrgn(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int l = (int16_t)hle_u16(&a), t = (int16_t)hle_u16(&a), r = (int16_t)hle_u16(&a), b = (int16_t)hle_u16(&a);
    gobj *o;
    GCHECK;
    o = rgn_obj(g, h);
    hle_ret16(c, o && grgn_set_rect(&o->u.rgn, l, t, r, b));
    return CPU_OK;
}

static cpu_status g_ptinregion(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a);
    gobj *o;
    GCHECK;
    o = rgn_obj(g, h);
    hle_ret16(c, o && grgn_has_point(&o->u.rgn, x, y));
    return CPU_OK;
}

static cpu_status g_rectinregion(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    int l, t, r, b;
    fb_rect q;
    gobj *o;
    hle_far(&a, &sel, &off);
    GCHECK;
    if (!get_rect16(c, sel, off, &l, &t, &r, &b)) return CPU_E_SEL;
    o = rgn_obj(g, h);
    q.left = l < r ? l : r;
    q.right = l < r ? r : l;
    q.top = t < b ? t : b;
    q.bottom = t < b ? b : t;
    hle_ret16(c, o && !fb_rect_empty(&q) && grgn_overlaps_rect(&o->u.rgn, &q));
    return CPU_OK;
}

/* ---- API: the DC clip ------------------------------------------------------------------------ */

/* Effective clip of the DC in DC device coordinates (origin at the DC origin). */
static bool dc_clip_now(gdi_t *g, gdc *dc, grgn *out) {
    gsurf s;
    fb_rect base;
    if (!gdc_surface(g, dc, &s)) return false;
    base.left = s.base.left - s.ox;
    base.right = s.base.right - s.ox;
    base.top = s.base.top - s.oy;
    base.bottom = s.base.bottom - s.oy;
    if (!grgn_set_rect(out, base.left, base.top, base.right, base.bottom)) return false;
    if (dc->has_clip && !grgn_combine(out, out, &dc->clip, RGN_AND)) return false;
    return true;
}

/* Normalized device rectangle of a logical one. */
static fb_rect dev_rect(const gdc *dc, int l, int t, int r, int b) {
    fb_rect q;
    int x0 = gdc_lx(dc, l), y0 = gdc_ly(dc, t), x1 = gdc_lx(dc, r), y1 = gdc_ly(dc, b);
    q.left = x0 < x1 ? x0 : x1;
    q.right = x0 < x1 ? x1 : x0;
    q.top = y0 < y1 ? y0 : y1;
    q.bottom = y0 < y1 ? y1 : y0;
    return q;
}

static cpu_status g_selectclippgn_impl(hle_call *c) {
    GBEGIN;
    uint16_t hd = hle_u16(&a), hr = hle_u16(&a);
    gdc *dc;
    gobj *o;
    GCHECK;
    dc = gdc_get(g, hd);
    if (!dc) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (hr == 0) {
        grgn_free(&dc->clip);
        dc->has_clip = false;
        hle_ret16(c, SIMPLEREGION);
        return CPU_OK;
    }
    o = rgn_obj(g, hr);
    if (!o || !grgn_copy(&dc->clip, &o->u.rgn)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    dc->has_clip = true;
    hle_ret16(c, (uint16_t)grgn_kind(&dc->clip));
    return CPU_OK;
}

static cpu_status clip_rect(hle_call *c, bool exclude) {
    GBEGIN;
    uint16_t hd = hle_u16(&a);
    int l = (int16_t)hle_u16(&a), t = (int16_t)hle_u16(&a), r = (int16_t)hle_u16(&a), b = (int16_t)hle_u16(&a);
    gdc *dc;
    fb_rect q;
    GCHECK;
    dc = gdc_get(g, hd);
    if (!dc) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    q = dev_rect(dc, l, t, r, b);
    if (!dc->has_clip) {                   /* start from the visible area */
        if (!dc_clip_now(g, dc, &dc->clip)) {
            hle_ret16(c, 0);
            return CPU_OK;
        }
        dc->has_clip = true;
    }
    if (!(exclude ? grgn_subtract_rect(&dc->clip, &q) : grgn_intersect_rect(&dc->clip, &q))) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    hle_ret16(c, (uint16_t)grgn_kind(&dc->clip));
    return CPU_OK;
}

static cpu_status g_intersectcliprect(hle_call *c) { return clip_rect(c, false); }
static cpu_status g_excludecliprect(hle_call *c) { return clip_rect(c, true); }

static cpu_status g_offsetclipregion(hle_call *c) {
    GBEGIN;
    uint16_t hd = hle_u16(&a);
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a);
    gdc *dc;
    GCHECK;
    dc = gdc_get(g, hd);
    if (!dc) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (dc->has_clip) grgn_offset(&dc->clip, gdc_scale_x(dc, x), gdc_scale_y(dc, y));
    hle_ret16(c, dc->has_clip ? (uint16_t)grgn_kind(&dc->clip) : SIMPLEREGION);
    return CPU_OK;
}

static cpu_status g_getclipbox(hle_call *c) {
    GBEGIN;
    uint16_t hd = hle_u16(&a), sel, off;
    gdc *dc;
    grgn eff;
    fb_rect q;
    int x0, y0, x1, y1;
    bool ok;
    hle_far(&a, &sel, &off);
    GCHECK;
    dc = gdc_get(g, hd);
    if (!dc) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    grgn_init(&eff);
    ok = dc_clip_now(g, dc, &eff);
    if (!ok) {
        grgn_free(&eff);
        hle_ret16(c, 0);
        return CPU_OK;
    }
    x0 = eff.box.left; y0 = eff.box.top; x1 = eff.box.right; y1 = eff.box.bottom;
    gdc_dp_to_lp(dc, &x0, &y0);
    gdc_dp_to_lp(dc, &x1, &y1);
    q.left = x0 < x1 ? x0 : x1;
    q.right = x0 < x1 ? x1 : x0;
    q.top = y0 < y1 ? y0 : y1;
    q.bottom = y0 < y1 ? y1 : y0;
    if (!eff.n) memset(&q, 0, sizeof q);
    hle_ret16(c, (uint16_t)grgn_kind(&eff));
    grgn_free(&eff);
    if (!put_rect16(c, sel, off, &q)) return CPU_E_SEL;
    return CPU_OK;
}

static cpu_status g_ptvisible(hle_call *c) {
    GBEGIN;
    uint16_t hd = hle_u16(&a);
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a);
    gdc *dc;
    grgn eff;
    bool vis = false;
    GCHECK;
    dc = gdc_get(g, hd);
    grgn_init(&eff);
    if (dc && dc_clip_now(g, dc, &eff)) {
        gdc_lp_to_dp(dc, &x, &y);
        vis = grgn_has_point(&eff, x, y);
    }
    grgn_free(&eff);
    hle_ret16(c, vis);
    return CPU_OK;
}

static cpu_status g_rectvisible(hle_call *c) {
    GBEGIN;
    uint16_t hd = hle_u16(&a), sel, off;
    int l, t, r, b;
    gdc *dc;
    grgn eff;
    bool vis = false;
    hle_far(&a, &sel, &off);
    GCHECK;
    if (!get_rect16(c, sel, off, &l, &t, &r, &b)) return CPU_E_SEL;
    dc = gdc_get(g, hd);
    grgn_init(&eff);
    if (dc && dc_clip_now(g, dc, &eff)) {
        fb_rect q = dev_rect(dc, l, t, r, b);
        vis = !fb_rect_empty(&q) && grgn_overlaps_rect(&eff, &q);
    }
    grgn_free(&eff);
    hle_ret16(c, vis);
    return CPU_OK;
}

#define G GDI_MODULE
const hle_def gdi_rgn_defs[] = {
    {G, 21, "ExcludeClipRect", 10, g_excludecliprect},         /* UNV */
    {G, 22, "IntersectClipRect", 10, g_intersectcliprect},
    {G, 32, "OffsetClipRgn", 6, g_offsetclipregion},           /* UNV */
    {G, 44, "SelectClipRgn", 4, g_selectclippgn_impl},
    {G, 47, "CombineRgn", 8, g_combinergn},
    {G, 54, "CreateEllipticRgn", 8, g_createellipticrgn},      /* UNV */
    {G, 55, "CreateEllipticRgnIndirect", 4, g_createellipticrgnindirect}, /* ARTIST */
    {G, 63, "CreatePolygonRgn", 8, g_createpolygonrgn},
    {G, 64, "CreateRectRgn", 8, g_creatergn},
    {G, 65, "CreateRectRgnIndirect", 4, g_creatergnindirect},
    {G, 72, "EqualRgn", 4, g_equalrgn},                        /* UNV */
    {G, 77, "GetClipBox", 6, g_getclipbox},
    {G, 101, "OffsetRgn", 6, g_offsetrgn},
    {G, 103, "PtVisible", 6, g_ptvisible},                     /* UNV */
    {G, 104, "RectVisible", 6, g_rectvisible},                 /* UNV */
    {G, 106, "SetRectRgn", 10, g_setrectrgn},                  /* ARTIST: handle + 4 ints, name by inference */
    {G, 134, "GetRgnBox", 6, g_getrgnbox},                     /* UNV */
    {G, 161, "PtInRegion", 6, g_ptinregion},                   /* UNV */
    {G, 181, "RectInRegion", 6, g_rectinregion},               /* UNV */
    {G, 444, "CreateRoundRectRgn", 12, g_createroundrectrgn},  /* UNV */
};
#undef G
const size_t gdi_rgn_ndefs = sizeof gdi_rgn_defs / sizeof gdi_rgn_defs[0];
