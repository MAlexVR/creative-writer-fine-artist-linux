/* GDI HLE core: the object table, device context state, mapping, device
 * capabilities and registration. See gdi.h.
 *
 * Ordinals. WRITER.EXE imports 116 GDI ordinals; each pairs one-to-one with the
 * name in the program's own import table, which makes those VERIFIED
 * as a set (tests/test_gdi_api.c checks it against that table).
 * ARTIST-only ordinals (55 76 84 85 88 95 106 128 379 380) are named from the
 * Win 3.1 export list, which is alphabetical in the 70-100 range (every WRITER
 * ordinal there fits it) and checked against the arguments at their call sites:
 * 128 is MulDiv (3 words pushed, -points * logpixels / 72), 106 takes a handle and
 * four ints (SetRectRgn), 379/380 follow StartDoc/EndDoc. Ordinals neither binary
 * imports (marked UNV below) are from memory of the export list and UNVERIFIED. */
#include "gdi_priv.h"

#include <ctype.h>

/* ---- object table ---------------------------------------------------------------- */

static int slot_of(uint16_t h) {
    if (h < GDI_HANDLE_BASE || (h & 1)) return -1;
    return (h - GDI_HANDLE_BASE) / 2;
}

gobj *gobj_get(gdi_t *g, uint16_t h) {
    int i = slot_of(h);
    if (i < 0 || i >= g->nobjs || !g->objs[i] || g->objs[i]->type == GO_FREE) return NULL;
    return g->objs[i];
}

gobj *gobj_typed(gdi_t *g, uint16_t h, gobj_type t) {
    gobj *o = gobj_get(g, h);
    return o && o->type == t ? o : NULL;
}

uint16_t gobj_new(gdi_t *g, gobj_type t, gobj **out) {
    int i;
    gobj *o;
    for (i = 0; i < g->nobjs; i++)
        if (g->objs[i] && g->objs[i]->type == GO_FREE) break;
    if (i == g->nobjs) {
        if (i >= GDI_MAX_OBJECTS) return 0;
        if (i == g->objcap) {
            int nc = g->objcap ? g->objcap * 2 : 64;
            gobj **n = realloc(g->objs, (size_t)nc * sizeof *n);
            if (!n) return 0;
            memset(n + g->objcap, 0, (size_t)(nc - g->objcap) * sizeof *n);
            g->objs = n;
            g->objcap = nc;
        }
        g->nobjs++;
    }
    if (!g->objs[i]) g->objs[i] = calloc(1, sizeof *o);
    o = g->objs[i];
    if (!o) return 0;
    memset(o, 0, sizeof *o);
    o->type = (uint8_t)t;
    if (t == GO_REGION) grgn_init(&o->u.rgn);
    if (out) *out = o;
    return (uint16_t)(GDI_HANDLE_BASE + 2 * i);
}

void gdi_stats(const gdi_t *g, unsigned *objects, unsigned *dcs) {
    int i;
    unsigned n = 0, d = 0;
    for (i = 0; i < g->nobjs; i++) {
        if (!g->objs[i] || g->objs[i]->type == GO_FREE) continue;
        n++;
        if (g->objs[i]->type == GO_DC) d++;
    }
    if (objects) *objects = n;
    if (dcs) *dcs = d;
}

void gobj_release(gdi_t *g, gobj *o) {
    (void)g;
    switch (o->type) {
    case GO_BITMAP: free(o->u.bmp.bits); break;
    case GO_PALETTE: free(o->u.pal.e); free(o->u.pal.map); free(o->u.pal.own); break;
    case GO_REGION: grgn_free(&o->u.rgn); break;
    default: break;
    }
    memset(o, 0, sizeof *o);
}

static void sel_adjust(gdi_t *g, uint16_t h, int d) {
    gobj *o = gobj_get(g, h);
    if (o) o->sel += d;
}

/* ---- device contexts ------------------------------------------------------------------ */

static uint16_t stock_h(gdi_t *g, int idx) { return g->stock[idx]; }

void gdc_select_defaults(gdi_t *g, gdc *dc) {
    dc->pen = stock_h(g, 7);
    dc->brush = stock_h(g, 0);
    dc->font = stock_h(g, 13);
    dc->pal = stock_h(g, 15);
    dc->bmp = dc->mem ? g->defbmp : 0;
    sel_adjust(g, dc->pen, 1);
    sel_adjust(g, dc->brush, 1);
    sel_adjust(g, dc->font, 1);
    sel_adjust(g, dc->pal, 1);
    sel_adjust(g, dc->bmp, 1);
}

static void mapping_for(gdi_t *g, int mode, gdc *dc);

static void dc_init(gdi_t *g, gdc *dc, bool mem) {
    memset(dc, 0, sizeof *dc);
    dc->used = true;
    dc->mem = mem;
    grgn_init(&dc->clip);
    dc->bk = 0xFFFFFF;
    dc->fg = 0;
    dc->bkmode = OPAQUE;
    dc->rop2 = 13;
    dc->polyfill = 1;
    dc->stretch = 1;
    dc->wex = dc->wey = dc->vex = dc->vey = 1;
    dc->mapmode = MM_TEXT;
    gdc_select_defaults(g, dc);
}

static void dc_free_saved(gdc *dc) {
    while (dc->saved) {
        gdc *s = dc->saved;
        dc->saved = s->saved;
        grgn_free(&s->clip);
        free(s);
    }
    dc->nsaved = 0;
}

static void dc_destroy(gdi_t *g, gdc *dc) {
    sel_adjust(g, dc->pen, -1);
    sel_adjust(g, dc->brush, -1);
    sel_adjust(g, dc->font, -1);
    sel_adjust(g, dc->pal, -1);
    sel_adjust(g, dc->bmp, -1);
    dc_free_saved(dc);
    grgn_free(&dc->clip);
    memset(dc, 0, sizeof *dc);
}

gdc *gdc_get(gdi_t *g, uint16_t h) {
    gobj *o;
    wm_dc *w;
    int i, freei = -1;
    if (h >= GDI_HANDLE_BASE) {
        o = gobj_typed(g, h, GO_DC);
        return o ? o->u.dc : NULL;
    }
    w = wm_dc_get(g->wm, h);
    if (!w) return NULL;
    for (i = 0; i < WM_MAX_DCS; i++) {
        gwmdc *s = &g->wmdc[i];
        if (s->used && s->h == h) {
            if (s->serial == w->serial) return &s->dc;
            dc_destroy(g, &s->dc);
            s->used = false;
            freei = i;
            break;
        }
        if (!s->used && freei < 0) freei = i;
    }
    if (freei < 0) {                      /* reclaim a slot whose window DC is gone */
        for (i = 0; i < WM_MAX_DCS; i++) {
            gwmdc *s = &g->wmdc[i];
            wm_dc *x = wm_dc_get(g->wm, s->h);
            if (!x || x->serial != s->serial) {
                dc_destroy(g, &s->dc);
                s->used = false;
                freei = i;
                break;
            }
        }
    }
    if (freei < 0) return NULL;
    g->wmdc[freei].used = true;
    g->wmdc[freei].h = h;
    g->wmdc[freei].serial = w->serial;
    dc_init(g, &g->wmdc[freei].dc, false);
    g->wmdc[freei].dc.h = h;
    return &g->wmdc[freei].dc;
}

/* ---- mapping ---------------------------------------------------------------------------- */

static int div0(int a, int b) { return b ? a / b : a; }

int gdc_lx(const gdc *dc, int x) { return div0((x - dc->wox) * dc->vex, dc->wex) + dc->vox; }
int gdc_ly(const gdc *dc, int y) { return div0((y - dc->woy) * dc->vey, dc->wey) + dc->voy; }
int gdc_scale_x(const gdc *dc, int dx) { return div0(dx * dc->vex, dc->wex); }
int gdc_scale_y(const gdc *dc, int dy) { return div0(dy * dc->vey, dc->wey); }

void gdc_lp_to_dp(const gdc *dc, int *x, int *y) {
    int nx = gdc_lx(dc, *x), ny = gdc_ly(dc, *y);
    *x = nx;
    *y = ny;
}

void gdc_dp_to_lp(const gdc *dc, int *x, int *y) {
    int nx = div0((*x - dc->vox) * dc->wex, dc->vex) + dc->wox;
    int ny = div0((*y - dc->voy) * dc->wey, dc->vey) + dc->woy;
    *x = nx;
    *y = ny;
}

/* Display geometry reported by GetDeviceCaps. 96 dpi on 640x480 is 169 x 127 mm. */
#define HORZSIZE_MM 169
#define VERTSIZE_MM 127

static void mapping_for(gdi_t *g, int mode, gdc *dc) {
    int w = g->fb->w, h = g->fb->h;
    int hs = HORZSIZE_MM, vs = VERTSIZE_MM, num, den;
    switch (mode) {
    case MM_LOMETRIC: num = 10; den = 1; break;
    case MM_HIMETRIC: num = 100; den = 1; break;
    case MM_LOENGLISH: num = 1000; den = 254; break;
    case MM_HIENGLISH: num = 10000; den = 254; break;
    case MM_TWIPS: num = 14400; den = 254; break;
    default: dc->wex = dc->wey = dc->vex = dc->vey = 1; return;
    }
    dc->wex = hs * num / den;
    dc->wey = vs * num / den;
    dc->vex = w;
    dc->vey = -h;
}

static void isotropic_adjust(gdc *dc) {
    long ax, ay, wx = labs(dc->wex), wy = labs(dc->wey);
    if (dc->mapmode != MM_ISOTROPIC || !wx || !wy) return;
    ax = labs(dc->vex) * wy;
    ay = labs(dc->vey) * wx;
    if (ax > ay) dc->vex = (int)((dc->vex < 0 ? -1 : 1) * (ay / wy));
    else if (ay > ax) dc->vey = (int)((dc->vey < 0 ? -1 : 1) * (ax / wx));
}

uint16_t gdi_palette_stock(gdi_t *g) {
    gobj *o;
    uint16_t h = gobj_new(g, GO_PALETTE, &o);
    int i;
    if (!h) return 0;
    o->u.pal.n = 20;
    o->u.pal.e = calloc(20, sizeof *o->u.pal.e);
    o->u.pal.map = calloc(20, 1);
    o->u.pal.own = calloc(20, 1);
    if (!o->u.pal.e || !o->u.pal.map || !o->u.pal.own) return 0;
    o->u.pal.realized = true;
    g->sysuse = 1;
    for (i = 0; i < 20; i++) {
        int idx = i < 10 ? i : 236 + i;
        uint32_t rgb = g->fb->pal[idx];
        o->u.pal.e[i][0] = (uint8_t)(rgb >> 16);
        o->u.pal.e[i][1] = (uint8_t)(rgb >> 8);
        o->u.pal.e[i][2] = (uint8_t)rgb;
        o->u.pal.map[i] = (uint8_t)idx;
    }
    return h;
}

bool gdi_make_default_bitmap(gdi_t *g) {
    gobj *o;
    uint16_t h = gobj_new(g, GO_BITMAP, &o);
    if (!h) return false;
    o->u.bmp.w = o->u.bmp.h = 1;
    o->u.bmp.bpp = 1;
    o->u.bmp.bits = calloc(1, 1);
    o->stock = true;
    g->defbmp = h;
    return o->u.bmp.bits != NULL;
}

/* ---- handlers: stock objects --------------------------------------------------------------- */

static cpu_status g_getstockobject(hle_call *c) {
    GBEGIN;
    int idx = (int16_t)hle_u16(&a);
    GCHECK;
    hle_ret16(c, idx >= 0 && idx <= 16 && idx != 9 ? g->stock[idx] : 0);
    return CPU_OK;
}

static uint16_t new_pen(gdi_t *g, int style, int width, uint32_t color) {
    gobj *o;
    uint16_t h = gobj_new(g, GO_PEN, &o);
    if (!h) return 0;
    o->u.pen.style = style;
    o->u.pen.width = width;
    o->u.pen.color = color;
    return h;
}

static uint16_t new_brush(gdi_t *g, int style, uint32_t color, int hatch, gobj **out) {
    gobj *o;
    uint16_t h = gobj_new(g, GO_BRUSH, &o);
    if (!h) return 0;
    o->u.brush.style = style;
    o->u.brush.color = color;
    o->u.brush.hatch = hatch;
    if (out) *out = o;
    return h;
}

static cpu_status g_createpen(hle_call *c) {
    GBEGIN;
    int style = (int16_t)hle_u16(&a), width = (int16_t)hle_u16(&a);
    uint32_t color = hle_u32(&a);
    GCHECK;
    hle_ret16(c, style < 0 || style > PS_INSIDEFRAME ? 0 : new_pen(g, style, width, color));
    return CPU_OK;
}

static cpu_status g_createpenindirect(hle_call *c) {
    GBEGIN;
    uint16_t sel, off;
    const uint8_t *p;
    hle_far(&a, &sel, &off);
    GCHECK;
    if (!(p = hle_ptr(c, sel, off, 10))) return CPU_E_SEL;
    hle_ret16(c, new_pen(g, (int16_t)gld16(p), (int16_t)gld16(p + 2), gld32(p + 6)));
    return CPU_OK;
}

static cpu_status g_createsolidbrush(hle_call *c) {
    GBEGIN;
    uint32_t color = hle_u32(&a);
    GCHECK;
    hle_ret16(c, new_brush(g, BS_SOLID, color, 0, NULL));
    return CPU_OK;
}

static cpu_status g_createhatchbrush(hle_call *c) {
    GBEGIN;
    int style = (int16_t)hle_u16(&a);
    uint32_t color = hle_u32(&a);
    GCHECK;
    hle_ret16(c, style < 0 || style > 5 ? 0 : new_brush(g, BS_HATCHED, color, style, NULL));
    return CPU_OK;
}

uint16_t gdi_brush_from_logbrush(gdi_t *g, hle_call *c, uint16_t sel, uint16_t off) {
    const uint8_t *p = hle_ptr(c, sel, off, 8);
    int style;
    if (!p) return 0;
    style = (int16_t)gld16(p);
    switch (style) {
    case BS_SOLID: return new_brush(g, BS_SOLID, gld32(p + 2), 0, NULL);
    case BS_NULL: return new_brush(g, BS_NULL, 0, 0, NULL);
    case BS_HATCHED: {
        int hatch = (int16_t)gld16(p + 6);
        return hatch < 0 || hatch > 5 ? 0 : new_brush(g, BS_HATCHED, gld32(p + 2), hatch, NULL);
    }
    case BS_PATTERN: return gdi_brush_pattern(g, gld16(p + 6));
    case BS_DIBPATTERN: return gdi_brush_dib(g, c, gld16(p + 6), (int)gld32(p + 2));
    default: return 0;
    }
}

static cpu_status g_createbrushindirect(hle_call *c) {
    GBEGIN;
    uint16_t sel, off;
    hle_far(&a, &sel, &off);
    GCHECK;
    hle_ret16(c, gdi_brush_from_logbrush(g, c, sel, off));
    return CPU_OK;
}

/* ---- handlers: GetObject, SelectObject, DeleteObject ------------------------------------------ */

static cpu_status g_getobject(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), cb = hle_u16(&a), sel, off;
    uint8_t buf[64], *p;
    unsigned n = 0;
    gobj *o;
    hle_far(&a, &sel, &off);
    GCHECK;
    memset(buf, 0, sizeof buf);
    if (!(o = gobj_get(g, h))) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    switch (o->type) {
    case GO_PEN:
        gst16(buf, (uint16_t)o->u.pen.style);
        gst16(buf + 2, (uint16_t)o->u.pen.width);
        gst32(buf + 6, o->u.pen.color);
        n = 10;
        break;
    case GO_BRUSH:
        gst16(buf, (uint16_t)o->u.brush.style);
        gst32(buf + 2, o->u.brush.color);
        gst16(buf + 6, (uint16_t)o->u.brush.hatch);
        n = 8;
        break;
    case GO_FONT:
        memcpy(buf, o->u.font.raw, 50);
        n = 50;
        break;
    case GO_BITMAP:
        gst16(buf + 2, (uint16_t)o->u.bmp.w);
        gst16(buf + 4, (uint16_t)o->u.bmp.h);
        gst16(buf + 6, (uint16_t)((o->u.bmp.w * o->u.bmp.bpp + 15) / 16 * 2));
        buf[8] = 1;
        buf[9] = (uint8_t)o->u.bmp.bpp;
        n = 14;
        break;
    case GO_PALETTE:
        gst16(buf, (uint16_t)o->u.pal.n);
        n = 2;
        break;
    default: break;
    }
    if (n > cb) n = cb;
    if (n && !(p = hle_ptr(c, sel, off, n))) return CPU_E_SEL;
    else if (n) memcpy(p, buf, n);
    hle_ret16(c, (uint16_t)n);
    return CPU_OK;
}

static int region_kind_of(const grgn *r) { return grgn_kind(r); }

static cpu_status g_selectobject(hle_call *c) {
    GBEGIN;
    uint16_t hdc = hle_u16(&a), h = hle_u16(&a), prev = 0;
    gdc *dc;
    gobj *o;
    GCHECK;
    dc = gdc_get(g, hdc);
    o = gobj_get(g, h);
    if (!dc || !o) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    switch (o->type) {
    case GO_PEN: prev = dc->pen; dc->pen = h; break;
    case GO_BRUSH: prev = dc->brush; dc->brush = h; break;
    case GO_FONT: prev = dc->font; dc->font = h; break;
    case GO_BITMAP:
        if (!dc->mem) {
            hle_ret16(c, 0);
            return CPU_OK;
        }
        prev = dc->bmp;
        dc->bmp = h;
        break;
    case GO_REGION:
        if (!grgn_copy(&dc->clip, &o->u.rgn)) prev = 0;
        else {
            dc->has_clip = true;
            prev = (uint16_t)region_kind_of(&dc->clip);
        }
        hle_ret16(c, prev);
        return CPU_OK;
    default:
        hle_ret16(c, 0);
        return CPU_OK;
    }
    o->sel++;
    sel_adjust(g, prev, -1);
    hle_ret16(c, prev);
    return CPU_OK;
}

static cpu_status g_deleteobject(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    gobj *o;
    GCHECK;
    o = gobj_get(g, h);
    if (!o || o->type == GO_DC) hle_ret16(c, 0);
    else if (o->stock) hle_ret16(c, 1);
    else if (o->sel > 0) hle_ret16(c, 0);
    else {
        if (o->type == GO_PALETTE) gdi_pal_forget(g, o);
        gobj_release(g, o);
        hle_ret16(c, 1);
    }
    return CPU_OK;
}

static cpu_status g_isgdiobject(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    gobj *o;
    GCHECK;
    o = gobj_get(g, h);
    hle_ret16(c, o || (h < GDI_HANDLE_BASE && wm_dc_get(g->wm, h)) ? 1 : 0);   /* DCs live in the GDI heap too */
    return CPU_OK;
}

static cpu_status g_unrealizeobject(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    gobj *o;
    GCHECK;
    o = gobj_get(g, h);
    if (o && o->type == GO_PALETTE) gdi_pal_unrealize(g, o);       /* the next realization maps it afresh */
    hle_ret16(c, o ? 1 : 0);
    return CPU_OK;
}

/* ---- handlers: device contexts ----------------------------------------------------------------- */

uint16_t gdi_new_memdc(gdi_t *g) {
    gobj *o;
    uint16_t h = gobj_new(g, GO_DC, &o);
    if (!h) return 0;
    o->u.dc = calloc(1, sizeof(gdc));
    if (!o->u.dc) {
        o->type = GO_FREE;
        return 0;
    }
    dc_init(g, o->u.dc, true);
    o->u.dc->h = h;
    return h;
}

static cpu_status g_createcompatibledc(hle_call *c) {
    GBEGIN;
    uint16_t hdc = hle_u16(&a);
    GCHECK;
    hle_ret16(c, hdc && !gdc_get(g, hdc) ? 0 : gdi_new_memdc(g));
    return CPU_OK;
}

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

static cpu_status g_createdc(hle_call *c) {
    GBEGIN;
    uint16_t dsel, doff, s2, o2, s3, o3, s4, o4;
    char drv[32] = "", dev[48] = "";
    hle_far(&a, &dsel, &doff);
    hle_far(&a, &s2, &o2);
    hle_far(&a, &s3, &o3);
    hle_far(&a, &s4, &o4);
    GCHECK;
    if (dsel) hle_read_cstr(c, dsel, doff, drv, sizeof drv);
    if (s2) hle_read_cstr(c, s2, o2, dev, sizeof dev);
    if (drv[0] && ieq(drv, "DISPLAY")) hle_ret16(c, wm_get_dc(g->wm, 0));
    else hle_ret16(c, gdi_print_create_dc(g, drv, dev, c->entry->ordinal == 153));   /* CreateIC is ordinal 153 */
    return CPU_OK;
}

static cpu_status g_deletedc(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    gobj *o;
    GCHECK;
    if (h < GDI_HANDLE_BASE) {
        hle_ret16(c, wm_dc_get(g->wm, h) && wm_release_dc(g->wm, 0, h) ? 1 : 0);
        return CPU_OK;
    }
    o = gobj_typed(g, h, GO_DC);
    if (!o) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    gdi_print_dc_deleted(g, o->u.dc);
    dc_destroy(g, o->u.dc);
    free(o->u.dc);
    o->u.dc = NULL;
    o->type = GO_FREE;
    hle_ret16(c, 1);
    return CPU_OK;
}

static bool dc_snapshot(gdc *dc) {
    gdc *s = malloc(sizeof *s);
    if (!s) return false;
    *s = *dc;
    grgn_init(&s->clip);
    if (!grgn_copy(&s->clip, &dc->clip)) {
        free(s);
        return false;
    }
    s->saved = dc->saved;
    dc->saved = s;
    dc->nsaved++;
    return true;
}

static cpu_status g_savedc(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    gdc *dc;
    GCHECK;
    dc = gdc_get(g, h);
    hle_ret16(c, dc && dc_snapshot(dc) ? (uint16_t)dc->nsaved : 0);
    return CPU_OK;
}

static uint16_t valid_or(gdi_t *g, uint16_t h, gobj_type t, uint16_t fallback) {
    return gobj_typed(g, h, t) ? h : fallback;
}

static cpu_status g_restoredc(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int n = (int16_t)hle_u16(&a), target, k;
    gdc *dc, *s;
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    target = n < 0 ? dc->nsaved + n + 1 : n;
    if (target < 1 || target > dc->nsaved) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    for (k = dc->nsaved; k > target; k--) {   /* drop the newer levels */
        s = dc->saved;
        dc->saved = s->saved;
        grgn_free(&s->clip);
        free(s);
        dc->nsaved--;
    }
    s = dc->saved;
    {
        gdc keep = *dc;
        sel_adjust(g, dc->pen, -1);
        sel_adjust(g, dc->brush, -1);
        sel_adjust(g, dc->font, -1);
        sel_adjust(g, dc->pal, -1);
        if (dc->mem) sel_adjust(g, dc->bmp, -1);
        grgn_free(&dc->clip);
        *dc = *s;
        dc->h = keep.h;
        dc->serial = keep.serial;
        dc->used = true;
        dc->mem = keep.mem;
        dc->saved = s->saved;
        dc->nsaved = keep.nsaved - 1;
        dc->clip = s->clip;      /* the snapshot's region now belongs to the live DC */
        free(s);
    }
    dc->pen = valid_or(g, dc->pen, GO_PEN, g->stock[7]);
    dc->brush = valid_or(g, dc->brush, GO_BRUSH, g->stock[0]);
    dc->font = valid_or(g, dc->font, GO_FONT, g->stock[13]);
    dc->pal = valid_or(g, dc->pal, GO_PALETTE, g->stock[15]);
    if (dc->mem) dc->bmp = valid_or(g, dc->bmp, GO_BITMAP, g->defbmp);
    sel_adjust(g, dc->pen, 1);
    sel_adjust(g, dc->brush, 1);
    sel_adjust(g, dc->font, 1);
    sel_adjust(g, dc->pal, 1);
    if (dc->mem) sel_adjust(g, dc->bmp, 1);
    hle_ret16(c, 1);
    return CPU_OK;
}

/* ---- handlers: simple state ------------------------------------------------------------------------ */

typedef enum { K_BKCOLOR, K_TEXTCOLOR, K_BKMODE, K_ROP2, K_POLYFILL, K_STRETCH, K_TALIGN, K_CEXTRA } kstate;

static int *int_slot(gdc *dc, kstate k) {
    switch (k) {
    case K_BKMODE: return &dc->bkmode;
    case K_ROP2: return &dc->rop2;
    case K_POLYFILL: return &dc->polyfill;
    case K_STRETCH: return &dc->stretch;
    case K_TALIGN: return &dc->talign;
    case K_CEXTRA: return &dc->cextra;
    default: return NULL;
    }
}

static bool valid_state(kstate k, int v) {
    switch (k) {
    case K_BKMODE: return v == TRANSPARENT || v == OPAQUE;
    case K_ROP2: return v >= 1 && v <= 16;
    case K_POLYFILL: return v == 1 || v == 2;
    case K_STRETCH: return v >= 1 && v <= 4;
    case K_TALIGN: return v >= 0 && v < 0x200;
    default: return true;
    }
}

static cpu_status set_int(hle_call *c, kstate k) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int v = (int16_t)hle_u16(&a);
    gdc *dc;
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc || !valid_state(k, v)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    hle_ret16(c, (uint16_t)*int_slot(dc, k));
    *int_slot(dc, k) = v;
    return CPU_OK;
}

static cpu_status get_int(hle_call *c, kstate k) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    gdc *dc;
    GCHECK;
    dc = gdc_get(g, h);
    hle_ret16(c, dc ? (uint16_t)*int_slot(dc, k) : 0);
    return CPU_OK;
}

static cpu_status set_color(hle_call *c, kstate k) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    uint32_t v = hle_u32(&a), *slot;
    gdc *dc;
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc) {
        hle_ret32(c, 0);
        return CPU_OK;
    }
    slot = k == K_BKCOLOR ? &dc->bk : &dc->fg;
    hle_ret32(c, *slot);
    *slot = v;
    return CPU_OK;
}

static cpu_status get_color(hle_call *c, kstate k) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    gdc *dc;
    GCHECK;
    dc = gdc_get(g, h);
    hle_ret32(c, dc ? (k == K_BKCOLOR ? dc->bk : dc->fg) : 0);
    return CPU_OK;
}

static cpu_status g_setbkcolor(hle_call *c) { return set_color(c, K_BKCOLOR); }
static cpu_status g_settextcolor(hle_call *c) { return set_color(c, K_TEXTCOLOR); }
static cpu_status g_getbkcolor(hle_call *c) { return get_color(c, K_BKCOLOR); }
static cpu_status g_gettextcolor(hle_call *c) { return get_color(c, K_TEXTCOLOR); }
static cpu_status g_setbkmode(hle_call *c) { return set_int(c, K_BKMODE); }
static cpu_status g_getbkmode(hle_call *c) { return get_int(c, K_BKMODE); }
static cpu_status g_setrop2(hle_call *c) { return set_int(c, K_ROP2); }
static cpu_status g_getrop2(hle_call *c) { return get_int(c, K_ROP2); }
static cpu_status g_setpolyfillmode(hle_call *c) { return set_int(c, K_POLYFILL); }
static cpu_status g_getpolyfillmode(hle_call *c) { return get_int(c, K_POLYFILL); }
static cpu_status g_setstretchbltmode(hle_call *c) { return set_int(c, K_STRETCH); }
static cpu_status g_getstretchbltmode(hle_call *c) { return get_int(c, K_STRETCH); }
static cpu_status g_settextalign(hle_call *c) { return set_int(c, K_TALIGN); }
static cpu_status g_gettextalign(hle_call *c) { return get_int(c, K_TALIGN); }
static cpu_status g_settextcharextra(hle_call *c) { return set_int(c, K_CEXTRA); }
static cpu_status g_gettextcharextra(hle_call *c) { return get_int(c, K_CEXTRA); }

static cpu_status g_settextjustification(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int extra = (int16_t)hle_u16(&a), count = (int16_t)hle_u16(&a);
    gdc *dc;
    GCHECK;
    dc = gdc_get(g, h);
    if (dc) {
        dc->jextra = extra;
        dc->jcount = count;
    }
    hle_ret16(c, dc ? 1 : 0);
    return CPU_OK;
}

static uint32_t pack_pt(int x, int y) { return (uint32_t)(uint16_t)x | (uint32_t)(uint16_t)y << 16; }

static cpu_status g_setbrushorg(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a);
    gdc *dc;
    GCHECK;
    dc = gdc_get(g, h);
    hle_ret32(c, dc ? pack_pt(dc->box, dc->boy) : 0);
    if (dc) {
        dc->box = x;
        dc->boy = y;
    }
    return CPU_OK;
}

static cpu_status g_moveto(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a);
    gdc *dc;
    GCHECK;
    dc = gdc_get(g, h);
    hle_ret32(c, dc ? pack_pt(dc->px, dc->py) : 0);
    if (dc) {
        dc->px = x;
        dc->py = y;
    }
    return CPU_OK;
}

static cpu_status g_getcurrentposition(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    gdc *dc;
    GCHECK;
    dc = gdc_get(g, h);
    hle_ret32(c, dc ? pack_pt(dc->px, dc->py) : 0);
    return CPU_OK;
}

/* writes a POINT/SIZE (two int16) through a far pointer */
static bool put_pt(hle_call *c, uint16_t sel, uint16_t off, int x, int y) {
    uint8_t *p;
    if (!sel && !off) return true;
    if (!(p = hle_ptr(c, sel, off, 4))) return false;
    gst16(p, (uint16_t)x);
    gst16(p + 2, (uint16_t)y);
    return true;
}

static cpu_status g_getcurrentpositionex(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    gdc *dc;
    hle_far(&a, &sel, &off);
    GCHECK;
    dc = gdc_get(g, h);
    if (dc && !put_pt(c, sel, off, dc->px, dc->py)) return CPU_E_SEL;
    hle_ret16(c, dc ? 1 : 0);
    return CPU_OK;
}

/* ---- handlers: mapping ------------------------------------------------------------------------------ */

static cpu_status g_setmapmode(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int mode = (int16_t)hle_u16(&a);
    gdc *dc;
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc || mode < MM_TEXT || mode > MM_ANISOTROPIC) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    hle_ret16(c, (uint16_t)dc->mapmode);
    dc->mapmode = mode;
    if (mode < MM_ISOTROPIC) mapping_for(g, mode, dc);
    return CPU_OK;
}

static cpu_status g_getmapmode(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    gdc *dc;
    GCHECK;
    dc = gdc_get(g, h);
    hle_ret16(c, dc ? (uint16_t)dc->mapmode : 0);
    return CPU_OK;
}

typedef enum { M_WORG, M_WEXT, M_VORG, M_VEXT } mkind;

static void mslot(gdc *dc, mkind k, int **x, int **y) {
    switch (k) {
    case M_WORG: *x = &dc->wox; *y = &dc->woy; break;
    case M_WEXT: *x = &dc->wex; *y = &dc->wey; break;
    case M_VORG: *x = &dc->vox; *y = &dc->voy; break;
    default: *x = &dc->vex; *y = &dc->vey; break;
    }
}

/* ext changes are ignored in the fixed mapping modes */
static bool mset(gdc *dc, mkind k, int x, int y) {
    int *px, *py;
    mslot(dc, k, &px, &py);
    if ((k == M_WEXT || k == M_VEXT) && dc->mapmode != MM_ISOTROPIC && dc->mapmode != MM_ANISOTROPIC) return false;
    *px = x;
    *py = y;
    if (k == M_WEXT || k == M_VEXT) isotropic_adjust(dc);
    return true;
}

/* mode: 0 Set, 1 SetEx, 2 Get, 3 GetEx, 4 Offset/Scale handled elsewhere */
static cpu_status map_fn(hle_call *c, mkind k, int mode) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel = 0, off = 0;
    int x = 0, y = 0, *px, *py, ox, oy;
    gdc *dc;
    if (mode == 0 || mode == 1) {
        x = (int16_t)hle_u16(&a);
        y = (int16_t)hle_u16(&a);
    }
    if (mode == 1 || mode == 3) hle_far(&a, &sel, &off);
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc) {
        if (mode == 0 || mode == 2) hle_ret32(c, 0);
        else hle_ret16(c, 0);
        return CPU_OK;
    }
    mslot(dc, k, &px, &py);
    ox = *px;
    oy = *py;
    if (mode == 0 || mode == 1) mset(dc, k, x, y);
    if (mode == 0 || mode == 2) hle_ret32(c, pack_pt(ox, oy));
    else {
        if (!put_pt(c, sel, off, mode == 3 ? *px : ox, mode == 3 ? *py : oy)) return CPU_E_SEL;
        hle_ret16(c, 1);
    }
    return CPU_OK;
}

static cpu_status g_setwindoworg(hle_call *c) { return map_fn(c, M_WORG, 0); }
static cpu_status g_setwindowext(hle_call *c) { return map_fn(c, M_WEXT, 0); }
static cpu_status g_setviewportorg(hle_call *c) { return map_fn(c, M_VORG, 0); }
static cpu_status g_setviewportext(hle_call *c) { return map_fn(c, M_VEXT, 0); }
static cpu_status g_getwindoworg(hle_call *c) { return map_fn(c, M_WORG, 2); }
static cpu_status g_getwindowext(hle_call *c) { return map_fn(c, M_WEXT, 2); }
static cpu_status g_getviewportorg(hle_call *c) { return map_fn(c, M_VORG, 2); }
static cpu_status g_getviewportext(hle_call *c) { return map_fn(c, M_VEXT, 2); }
static cpu_status g_setwindoworgex(hle_call *c) { return map_fn(c, M_WORG, 1); }
static cpu_status g_setwindowextex(hle_call *c) { return map_fn(c, M_WEXT, 1); }
static cpu_status g_setviewportorgex(hle_call *c) { return map_fn(c, M_VORG, 1); }
static cpu_status g_setviewportextex(hle_call *c) { return map_fn(c, M_VEXT, 1); }
static cpu_status g_getwindoworgex(hle_call *c) { return map_fn(c, M_WORG, 3); }
static cpu_status g_getwindowextex(hle_call *c) { return map_fn(c, M_WEXT, 3); }
static cpu_status g_getviewportorgex(hle_call *c) { return map_fn(c, M_VORG, 3); }
static cpu_status g_getviewportextex(hle_call *c) { return map_fn(c, M_VEXT, 3); }

static cpu_status offset_org(hle_call *c, mkind k, bool ex) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel = 0, off = 0;
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a), *px, *py, ox, oy;
    gdc *dc;
    if (ex) hle_far(&a, &sel, &off);
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc) {
        hle_ret32(c, 0);
        return CPU_OK;
    }
    mslot(dc, k, &px, &py);
    ox = *px;
    oy = *py;
    *px += x;
    *py += y;
    if (ex) {
        if (!put_pt(c, sel, off, ox, oy)) return CPU_E_SEL;
        hle_ret16(c, 1);
    } else {
        hle_ret32(c, pack_pt(ox, oy));
    }
    return CPU_OK;
}

static cpu_status g_offsetwindoworg(hle_call *c) { return offset_org(c, M_WORG, false); }
static cpu_status g_offsetviewportorg(hle_call *c) { return offset_org(c, M_VORG, false); }
static cpu_status g_offsetwindoworgex(hle_call *c) { return offset_org(c, M_WORG, true); }
static cpu_status g_offsetviewportorgex(hle_call *c) { return offset_org(c, M_VORG, true); }

static cpu_status scale_ext(hle_call *c, mkind k, bool ex) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel = 0, off = 0;
    int xn = (int16_t)hle_u16(&a), xd = (int16_t)hle_u16(&a), yn = (int16_t)hle_u16(&a), yd = (int16_t)hle_u16(&a);
    int *px, *py, ox, oy;
    gdc *dc;
    if (ex) hle_far(&a, &sel, &off);
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc) {
        hle_ret32(c, 0);
        return CPU_OK;
    }
    mslot(dc, k, &px, &py);
    ox = *px;
    oy = *py;
    if (xd && yd && (dc->mapmode == MM_ISOTROPIC || dc->mapmode == MM_ANISOTROPIC)) {
        *px = *px * xn / xd;
        *py = *py * yn / yd;
        isotropic_adjust(dc);
    }
    if (ex) {
        if (!put_pt(c, sel, off, ox, oy)) return CPU_E_SEL;
        hle_ret16(c, 1);
    } else {
        hle_ret32(c, pack_pt(ox, oy));
    }
    return CPU_OK;
}

static cpu_status g_scalewindowext(hle_call *c) { return scale_ext(c, M_WEXT, false); }
static cpu_status g_scaleviewportext(hle_call *c) { return scale_ext(c, M_VEXT, false); }
static cpu_status g_scalewindowextex(hle_call *c) { return scale_ext(c, M_WEXT, true); }
static cpu_status g_scaleviewportextex(hle_call *c) { return scale_ext(c, M_VEXT, true); }

static cpu_status convert_points(hle_call *c, bool to_dp) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off, n;
    uint8_t *p;
    gdc *dc;
    int i;
    hle_far(&a, &sel, &off);
    n = hle_u16(&a);
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (n && !(p = hle_ptr(c, sel, off, 4u * n))) return CPU_E_SEL;
    for (i = 0; i < n; i++) {
        uint8_t *q = hle_ptr(c, sel, (uint32_t)off + 4u * (unsigned)i, 4);
        int x = (int16_t)gld16(q), y = (int16_t)gld16(q + 2);
        if (to_dp) gdc_lp_to_dp(dc, &x, &y);
        else gdc_dp_to_lp(dc, &x, &y);
        gst16(q, (uint16_t)x);
        gst16(q + 2, (uint16_t)y);
    }
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status g_dptolp(hle_call *c) { return convert_points(c, false); }
static cpu_status g_lptodp(hle_call *c) { return convert_points(c, true); }

/* ---- handlers: device capabilities ---------------------------------------------------------------------- */

#define RASTERCAPS_VALUE 0x3F99 /* BITBLT BITMAP64 GDI20_OUTPUT DI_BITMAP PALETTE DIBTODEV BIGFONT STRETCHBLT FLOODFILL STRETCHDIB */

static int device_cap(gdi_t *g, const gdc *dc, int idx) {
    int pv;
    if (gdi_print_device_cap(g, dc, idx, &pv)) return pv;
    switch (idx) {
    case 0: return 0x0300;                 /* DRIVERVERSION */
    case 2: return 1;                      /* TECHNOLOGY: DT_RASDISPLAY */
    case 4: return HORZSIZE_MM;
    case 6: return VERTSIZE_MM;
    case 8: return g->fb->w;
    case 10: return g->fb->h;
    case 12: return 8;                     /* BITSPIXEL */
    case 14: return 1;                     /* PLANES */
    case 16: return -1;                    /* NUMBRUSHES: device-independent */
    case 18: return 16;                    /* NUMPENS */
    case 20: return 0;                     /* NUMMARKERS */
    case 22: return 0;                     /* NUMFONTS: fonts come from the font mapper */
    case 24: return 20;                    /* NUMCOLORS: the static colours */
    case 26: return 0;                     /* PDEVICESIZE */
    case 28: return 0x00FF;                /* CURVECAPS */
    case 30: return 0x007F;                /* LINECAPS */
    case 32: return 0x00FF;                /* POLYGONALCAPS */
    case 34: return 0x3C01;                /* TEXTCAPS: TC_OP_CHARACTER IA UA SO RA */
    case 36: return 1;                     /* CLIPCAPS: CP_RECTANGLE */
    case 38: return RASTERCAPS_VALUE;
    case 40: case 42: return 36;           /* ASPECTX/Y: square pixels */
    case 44: return 51;                    /* ASPECTXY */
    case 88: case 90: return 96;           /* LOGPIXELSX/Y */
    case 104: return 256;                  /* SIZEPALETTE */
    case 106: return 20;                   /* NUMRESERVED */
    case 108: return 18;                   /* COLORRES */
    default: return 0;
    }
}

static cpu_status g_getdevicecaps(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int idx = (int16_t)hle_u16(&a);
    GCHECK;
    hle_ret16(c, gdc_get(g, h) ? (uint16_t)device_cap(g, gdc_get(g, h), idx) : 0);
    return CPU_OK;
}

static cpu_status g_muldiv(hle_call *c) {
    GBEGIN;
    long long n = (int16_t)hle_u16(&a), m = (int16_t)hle_u16(&a), d = (int16_t)hle_u16(&a), p, r;
    bool neg;
    GCHECK;
    if (!d) {
        hle_ret16(c, 0xFFFF);
        return CPU_OK;
    }
    p = n * m;
    neg = (p < 0) != (d < 0);
    p = p < 0 ? -p : p;
    d = d < 0 ? -d : d;
    r = (p + d / 2) / d;
    if (neg) r = -r;
    hle_ret16(c, r < -32768 || r > 32767 ? 0xFFFF : (uint16_t)r);
    return CPU_OK;
}

/* ---- logged stubs -------------------------------------------------------------------------------------- */

void gdi_log_stub(hle_call *c, const char *task) {
    gdi_t *g = c->ctx;
    const hle_entry *e = c->entry;
    if (g->nstubs < GDI_STUB_MAX) {
        gdi_stub_rec *r = &g->stubs[g->nstubs];
        r->module = e->module[0] == 'U' ? "USER" : "GDI";
        r->ordinal = e->ordinal;
        r->name = e->name;
        r->task = task;
        r->caller_cs = c->caller_cs;
        r->caller_ip = c->caller_ip;
    }
    g->nstubs++;
    if (g->cfg.log)
        fprintf(g->cfg.log, "gdi: stub %s.%u %s (%s) called from %04x:%04x, returns 0\n", e->module, e->ordinal,
                e->name, task, c->caller_cs, c->caller_ip);
}

/* ---- registration -------------------------------------------------------------------------------------- */

#define G GDI_MODULE
static const hle_def core_defs[] = {
    {G, 1, "SetBkColor", 6, g_setbkcolor},
    {G, 2, "SetBkMode", 4, g_setbkmode},
    {G, 3, "SetMapMode", 4, g_setmapmode},
    {G, 4, "SetROP2", 4, g_setrop2},
    {G, 6, "SetPolyFillMode", 4, g_setpolyfillmode},
    {G, 7, "SetStretchBltMode", 4, g_setstretchbltmode},
    {G, 8, "SetTextCharacterExtra", 4, g_settextcharextra},    /* UNV */
    {G, 9, "SetTextColor", 6, g_settextcolor},
    {G, 10, "SetTextJustification", 6, g_settextjustification}, /* UNV */
    {G, 11, "SetWindowOrg", 6, g_setwindoworg},
    {G, 12, "SetWindowExt", 6, g_setwindowext},
    {G, 13, "SetViewportOrg", 6, g_setviewportorg},
    {G, 14, "SetViewportExt", 6, g_setviewportext},
    {G, 15, "OffsetWindowOrg", 6, g_offsetwindoworg},           /* UNV */
    {G, 16, "ScaleWindowExt", 10, g_scalewindowext},            /* UNV */
    {G, 17, "OffsetViewportOrg", 6, g_offsetviewportorg},       /* UNV */
    {G, 18, "ScaleViewportExt", 10, g_scaleviewportext},        /* UNV */
    {G, 20, "MoveTo", 6, g_moveto},
    {G, 30, "SaveDC", 2, g_savedc},
    {G, 39, "RestoreDC", 4, g_restoredc},
    {G, 45, "SelectObject", 4, g_selectobject},
    {G, 50, "CreateBrushIndirect", 4, g_createbrushindirect},
    {G, 52, "CreateCompatibleDC", 2, g_createcompatibledc},
    {G, 53, "CreateDC", 16, g_createdc},
    {G, 58, "CreateHatchBrush", 6, g_createhatchbrush},
    {G, 61, "CreatePen", 8, g_createpen},
    {G, 62, "CreatePenIndirect", 4, g_createpenindirect},
    {G, 66, "CreateSolidBrush", 4, g_createsolidbrush},
    {G, 67, "DPtoLP", 8, g_dptolp},
    {G, 68, "DeleteDC", 2, g_deletedc},
    {G, 69, "DeleteObject", 2, g_deleteobject},
    {G, 75, "GetBkColor", 2, g_getbkcolor},
    {G, 76, "GetBkMode", 2, g_getbkmode},                       /* ARTIST */
    {G, 78, "GetCurrentPosition", 2, g_getcurrentposition},
    {G, 80, "GetDeviceCaps", 4, g_getdevicecaps},
    {G, 81, "GetMapMode", 2, g_getmapmode},
    {G, 82, "GetObject", 8, g_getobject},
    {G, 84, "GetPolyFillMode", 2, g_getpolyfillmode},           /* ARTIST */
    {G, 86, "GetROP2", 2, g_getrop2},                           /* UNV */
    {G, 87, "GetStockObject", 2, g_getstockobject},
    {G, 88, "GetStretchBltMode", 2, g_getstretchbltmode},       /* ARTIST */
    {G, 89, "GetTextCharacterExtra", 2, g_gettextcharextra},    /* UNV */
    {G, 90, "GetTextColor", 2, g_gettextcolor},
    {G, 94, "GetViewportExt", 2, g_getviewportext},             /* UNV */
    {G, 95, "GetViewportOrg", 2, g_getviewportorg},             /* ARTIST */
    {G, 96, "GetWindowExt", 2, g_getwindowext},                 /* UNV */
    {G, 97, "GetWindowOrg", 2, g_getwindoworg},                 /* UNV */
    {G, 99, "LPtoDP", 8, g_lptodp},
    {G, 128, "MulDiv", 6, g_muldiv},                            /* ARTIST */
    {G, 148, "SetBrushOrg", 6, g_setbrushorg},
    {G, 150, "UnrealizeObject", 2, g_unrealizeobject},
    {G, 153, "CreateIC", 16, g_createdc},
    {G, 345, "GetTextAlign", 2, g_gettextalign},
    {G, 346, "SetTextAlign", 4, g_settextalign},
    {G, 462, "IsGDIObject", 2, g_isgdiobject},
    {G, 470, "GetCurrentPositionEx", 6, g_getcurrentpositionex},
    {G, 472, "GetViewportExtEx", 6, g_getviewportextex},
    {G, 473, "GetViewportOrgEx", 6, g_getviewportorgex},
    {G, 474, "GetWindowExtEx", 6, g_getwindowextex},
    {G, 475, "GetWindowOrgEx", 6, g_getwindoworgex},
    {G, 476, "OffsetViewportOrgEx", 10, g_offsetviewportorgex}, /* UNV */
    {G, 477, "OffsetWindowOrgEx", 10, g_offsetwindoworgex},     /* UNV */
    {G, 479, "SetViewportExtEx", 10, g_setviewportextex},
    {G, 480, "SetViewportOrgEx", 10, g_setviewportorgex},
    {G, 481, "SetWindowExtEx", 10, g_setwindowextex},           /* UNV */
    {G, 482, "SetWindowOrgEx", 10, g_setwindoworgex},           /* UNV */
    {G, 499, "ScaleViewportExtEx", 14, g_scaleviewportextex},   /* UNV */
    {G, 500, "ScaleWindowExtEx", 14, g_scalewindowextex},       /* UNV */
};
#undef G

static bool make_stock(gdi_t *g) {
    static const uint32_t gray[] = {0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000};
    int i;
    gobj *o;
    uint16_t h;
    for (i = 0; i < 17; i++) {
        o = NULL;
        h = 0;
        if (i <= 4) h = new_brush(g, BS_SOLID, gray[i], 0, &o);
        else if (i == 5) h = new_brush(g, BS_NULL, 0, 0, &o);
        else if (i == 6) h = new_pen(g, PS_SOLID, 1, 0xFFFFFF);
        else if (i == 7) h = new_pen(g, PS_SOLID, 1, 0);
        else if (i == 8) h = new_pen(g, PS_NULL, 1, 0);
        else if (i == 9) continue;
        else if (i >= 10 && i <= 14) h = gdi_font_stock(g, i);
        else if (i == 15) h = gdi_palette_stock(g);
        else h = gdi_font_stock(g, i);          /* SYSTEM_FIXED_FONT */
        if (!h) return false;
        gobj_get(g, h)->stock = true;
        g->stock[i] = h;
    }
    return true;
}

/* Class background brush into the DC's visible/clipped area (WM_ERASEBKGND). */
static bool erase_cb(void *ctx, uint16_t hdc, uint16_t hbr) {
    gdi_t *g = ctx;
    gctx x;
    bpat bp;
    gobj *bo = gobj_typed(g, hbr, GO_BRUSH);
    gbrush sys;
    bool ok = false;
    if (!gctx_open(g, hdc, &x)) {
        gctx_close(&x);
        return false;
    }
    if (bo) {
        brush_prepare(&x, &bo->u.brush, &bp);
        ok = true;
    } else if (hbr >= 1 && hbr <= COLOR_NCOLORS) {
        memset(&sys, 0, sizeof sys);
        sys.style = BS_SOLID;
        sys.color = wm_sys_color(hbr - 1);
        brush_prepare(&x, &sys, &bp);
        ok = true;
    }
    if (ok && !fb_rect_empty(&x.clipr.box)) fill_rect_surface(&x, &x.clipr.box, &bp, 13);
    gctx_close(&x);
    return ok;
}

gdi_t *gdi_create(const gdi_config *cfg, hle_t *h, user_t *u) {
    gdi_t *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->hle = h;
    g->user = u;
    g->wm = user_wm(u);
    g->fb = user_fb(u);
    if (cfg) g->cfg = *cfg;
    grgn_init(&g->clipscratch);
    if (g->fb->bpp != 8 || !make_stock(g) || !gdi_make_default_bitmap(g) ||
        !hle_register_table(h, core_defs, sizeof core_defs / sizeof core_defs[0], g) ||
        !hle_register_table(h, gdi_font_defs, gdi_font_ndefs, g) ||
        !hle_register_table(h, gdi_rgn_defs, gdi_rgn_ndefs, g) ||
        !hle_register_table(h, gdi_draw_defs, gdi_draw_ndefs, g) ||
        !hle_register_table(h, gdi_blt_defs, gdi_blt_ndefs, g) ||
        !hle_register_table(h, gdi_dib_defs, gdi_dib_ndefs, g) ||
        !hle_register_table(h, gdi_pal_defs, gdi_pal_ndefs, g) ||
        !hle_register_table(h, gdi_meta_defs, gdi_meta_ndefs, g) ||
        !hle_register_table(h, gdi_misc_defs, gdi_misc_ndefs, g) ||
        !hle_register_table(h, gdi_print_defs, gdi_print_ndefs, g)) {
        gdi_destroy(g);
        return NULL;
    }
    user_set_eraser(u, erase_cb, g);
    gdi_ui_install(g);
    h->tap = gdi_meta_tap;
    h->tap_ctx = g;
    return g;
}

void gdi_destroy(gdi_t *g) {
    int i;
    if (!g) return;
    if (g->hle && g->hle->tap_ctx == g) {
        g->hle->tap = NULL;
        g->hle->tap_ctx = NULL;
    }
    gdi_meta_free_all(g);
    gdi_print_destroy(g);
    for (i = 0; i < g->nfnts; i++) fnt_free(&g->fnts[i]);
    if (g->user) {
        user_set_eraser(g->user, NULL, NULL);
        user_set_gfx(g->user, NULL);
    }
    for (i = 0; i < WM_MAX_DCS; i++)
        if (g->wmdc[i].used) dc_destroy(g, &g->wmdc[i].dc);
    for (i = 0; i < g->nobjs; i++) {     /* DCs first: they drop selections into other objects */
        gobj *o = g->objs[i];
        if (o && o->type == GO_DC && o->u.dc) {
            dc_destroy(g, o->u.dc);
            free(o->u.dc);
            o->u.dc = NULL;
            o->type = GO_FREE;
        }
    }
    for (i = 0; i < g->nobjs; i++) {
        if (!g->objs[i]) continue;
        gobj_release(g, g->objs[i]);
        free(g->objs[i]);
    }
    free(g->objs);
    grgn_free(&g->clipscratch);
    free(g);
}

bool gdi_attach(gdi_t *g, loader_proc *p) {
    g->p = p;
    g->scratch_sel = mem_alloc(&p->mem, GDI_SCRATCH, MEM_DATA);
    return g->scratch_sel != 0;
}

size_t gdi_stub_count(const gdi_t *g) { return g->nstubs; }
const gdi_stub_rec *gdi_stub_at(const gdi_t *g, size_t i) { return i < g->nstubs && i < GDI_STUB_MAX ? &g->stubs[i] : NULL; }
