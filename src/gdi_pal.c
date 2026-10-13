/* Logical palettes and the 256-entry system palette. See gdi.h.
 *
 * The framebuffer holds palette indices, so changing a system palette entry
 * recolours everything drawn with it, which is exactly what palette animation
 * relies on. The model:
 *  - 20 static colours (0-9, 246-255) never change; SYSPAL_NOSTATIC frees all of
 *    them but entries 0 and 255;
 *  - RealizePalette maps every logical entry to a system entry: PC_EXPLICIT uses
 *    its first byte as the index; otherwise an exact colour match (static colours
 *    first, then entries other palettes already hold, never ones allocated with
 *    PC_NOCOLLAPSE/PC_RESERVED) is shared, else a free entry is taken (the one the
 *    entry had before when it is free), else the nearest colour is used;
 *  - the return value counts the logical entries now mapped to a different
 *    system entry (all of them the first time);
 *  - the default palette (the 20 static colours) realizes to nothing. There is no
 *    foreground/background distinction: every realization is a foreground one. */
#include "gdi_priv.h"

#define PC_RESERVED 1
#define PC_EXPLICIT 2
#define PC_NOCOLLAPSE 4

static uint32_t entry_rgb(const uint8_t *e) { return (uint32_t)e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16; }

/* system palette entry as a COLORREF */
static uint32_t sys_cref(const gdi_t *g, int idx) {
    uint32_t v = g->fb->pal[idx];
    return (v & 0xFF) << 16 | (v & 0xFF00) | (v >> 16 & 0xFF);
}

static void set_sys(gdi_t *g, int idx, uint32_t cref) {
    g->fb->pal[idx] = (cref & 0xFF) << 16 | (cref & 0xFF00) | (cref >> 16 & 0xFF);
}

static bool is_static(const gdi_t *g, int idx) {
    if (idx == 0 || idx == 255) return true;
    return g->sysuse != 2 && (idx < 10 || idx >= 246);
}

static bool is_free(const gdi_t *g, int idx) { return !is_static(g, idx) && g->sysref[idx] == 0; }

static long dist2(uint32_t a, uint32_t b) {
    long dr = (long)(a & 0xFF) - (long)(b & 0xFF), dg = (long)(a >> 8 & 0xFF) - (long)(b >> 8 & 0xFF),
         db = (long)(a >> 16 & 0xFF) - (long)(b >> 16 & 0xFF);
    return dr * dr + dg * dg + db * db;
}

int gdi_pal_nearest_entry(const gpal *p, uint32_t cref) {
    int i, best = 0;
    long bd = 1L << 30;
    for (i = 0; i < p->n; i++) {
        long d = dist2(entry_rgb(p->e[i]), cref);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

static void release_refs(gdi_t *g, gpal *p) {
    int i;
    if (!p->own) return;
    for (i = 0; i < p->n; i++)
        if (p->own[i] && g->sysref[p->map[i]]) {
            g->sysref[p->map[i]]--;
            p->own[i] = 0;
        }
}

void gdi_pal_forget(gdi_t *g, gobj *po) { release_refs(g, &po->u.pal); }

void gdi_pal_unrealize(gdi_t *g, gobj *po) {
    if (po->stock) return;
    release_refs(g, &po->u.pal);
    po->u.pal.realized = false;
}

static int realize(gdi_t *g, gobj *po) {
    gpal *p = &po->u.pal;
    int i, idx, changed = 0;
    uint8_t *old = malloc((size_t)(p->n ? p->n : 1));
    bool was = p->realized;
    if (!old) return 0;
    if (po->stock) {
        free(old);
        return 0;
    }
    memcpy(old, p->map, (size_t)p->n);
    release_refs(g, p);
    for (i = 0; i < p->n; i++) {
        uint8_t flags = p->e[i][3];
        uint32_t cref = entry_rgb(p->e[i]);
        int newidx = -1;
        bool counted = false;
        if (flags & PC_EXPLICIT) {
            newidx = p->e[i][0];
        } else {
            if (!(flags & (PC_NOCOLLAPSE | PC_RESERVED))) {
                for (idx = 0; idx < 256 && newidx < 0; idx++)
                    if (is_static(g, idx) && sys_cref(g, idx) == cref) newidx = idx;
                for (idx = 0; idx < 256 && newidx < 0; idx++)
                    if (!is_static(g, idx) && g->sysref[idx] > 0 && !g->syspriv[idx] && sys_cref(g, idx) == cref) {
                        newidx = idx;
                        counted = true;
                    }
            }
            if (newidx < 0) {
                if (was && is_free(g, old[i])) newidx = old[i];
                for (idx = 1; idx < 255 && newidx < 0; idx++)
                    if (is_free(g, idx)) newidx = idx;
                if (newidx >= 0) {
                    set_sys(g, newidx, cref);
                    g->syspriv[newidx] = (flags & (PC_NOCOLLAPSE | PC_RESERVED)) != 0;
                    counted = true;
                }
            }
            if (newidx < 0) {                              /* the system palette is full: use the nearest colour */
                long bd = 1L << 30;
                for (idx = 0; idx < 256; idx++) {
                    long d = dist2(sys_cref(g, idx), cref);
                    if (d < bd) {
                        bd = d;
                        newidx = idx;
                    }
                }
            }
        }
        if (counted && g->sysref[newidx] < 255) {
            g->sysref[newidx]++;
            p->own[i] = 1;
        }
        if (!was || old[i] != newidx) changed++;
        p->map[i] = (uint8_t)newidx;
    }
    p->realized = true;
    g->pal_gen++;
    g->fb->dirty++;
    free(old);
    return changed;
}

/* ---- handlers ----------------------------------------------------------------------------------- */

static gobj *pal_obj(gdi_t *g, uint16_t h) { return gobj_typed(g, h, GO_PALETTE); }

static bool pal_alloc(gpal *p, int n) {
    uint8_t (*e)[4] = realloc(p->e, (size_t)n * sizeof *e);
    uint8_t *m, *w;
    if (!e) return false;
    p->e = e;
    m = realloc(p->map, (size_t)n);
    if (!m) return false;
    p->map = m;
    w = realloc(p->own, (size_t)n);
    if (!w) return false;
    p->own = w;
    return true;
}

static cpu_status g_createpalette(hle_call *c) {
    GBEGIN;
    uint16_t sel, off, h;
    const uint8_t *p;
    int n, i;
    gobj *o;
    hle_far(&a, &sel, &off);
    GCHECK;
    if (!(p = hle_ptr(c, sel, off, 4))) return CPU_E_SEL;
    n = gld16(p + 2);
    if (n < 1 || n > 1024) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (!(p = hle_ptr(c, sel, off, 4u + 4u * (unsigned)n))) return CPU_E_SEL;
    h = gobj_new(g, GO_PALETTE, &o);
    if (!h) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    o->u.pal.e = NULL;
    o->u.pal.map = NULL;
    o->u.pal.own = NULL;
    if (!pal_alloc(&o->u.pal, n)) {
        gobj_release(g, o);
        hle_ret16(c, 0);
        return CPU_OK;
    }
    o->u.pal.n = n;
    memcpy(o->u.pal.e, p + 4, (size_t)n * 4);
    memset(o->u.pal.map, 0, (size_t)n);
    memset(o->u.pal.own, 0, (size_t)n);
    for (i = 0; i < n; i++) o->u.pal.e[i][3] &= 7;
    hle_ret16(c, h);
    return CPU_OK;
}

static cpu_status g_getpaletteentries(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    int start = hle_u16(&a), n = hle_u16(&a), i;
    gobj *o;
    hle_far(&a, &sel, &off);
    GCHECK;
    o = pal_obj(g, h);
    if (!o) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (!sel && !off) {
        hle_ret16(c, (uint16_t)o->u.pal.n);
        return CPU_OK;
    }
    if (start >= o->u.pal.n) n = 0;
    else if (start + n > o->u.pal.n) n = o->u.pal.n - start;
    for (i = 0; i < n; i++) {
        uint8_t *q = hle_ptr(c, sel, (uint32_t)off + 4u * (unsigned)i, 4);
        if (!q) return CPU_E_SEL;
        memcpy(q, o->u.pal.e[start + i], 4);
    }
    hle_ret16(c, (uint16_t)n);
    return CPU_OK;
}

static cpu_status g_setpaletteentries(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    int start = hle_u16(&a), n = hle_u16(&a), i;
    gobj *o;
    hle_far(&a, &sel, &off);
    GCHECK;
    o = pal_obj(g, h);
    if (!o || o->stock) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (start >= o->u.pal.n) n = 0;
    else if (start + n > o->u.pal.n) n = o->u.pal.n - start;
    for (i = 0; i < n; i++) {
        const uint8_t *q = hle_ptr(c, sel, (uint32_t)off + 4u * (unsigned)i, 4);
        if (!q) return CPU_E_SEL;
        memcpy(o->u.pal.e[start + i], q, 4);
        o->u.pal.e[start + i][3] &= 7;
    }
    hle_ret16(c, (uint16_t)n);
    return CPU_OK;
}

static cpu_status g_resizepalette(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    int n = hle_u16(&a), i;
    gobj *o;
    GCHECK;
    o = pal_obj(g, h);
    if (!o || o->stock || n < 1 || n > 1024) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    for (i = n; i < o->u.pal.n; i++)              /* dropped entries give back their system entries */
        if (o->u.pal.own[i] && g->sysref[o->u.pal.map[i]]) g->sysref[o->u.pal.map[i]]--;
    {
        int old = o->u.pal.n;
        if (!pal_alloc(&o->u.pal, n)) {
            hle_ret16(c, 0);
            return CPU_OK;
        }
        for (i = old; i < n; i++) {
            memset(o->u.pal.e[i], 0, 4);
            o->u.pal.map[i] = 0;
            o->u.pal.own[i] = 0;
        }
        o->u.pal.n = n;
    }
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status g_animatepalette(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    int start = hle_u16(&a), n = hle_u16(&a), i;
    gobj *o;
    hle_far(&a, &sel, &off);
    GCHECK;
    o = pal_obj(g, h);
    if (!o || o->stock) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (start >= o->u.pal.n) n = 0;
    else if (start + n > o->u.pal.n) n = o->u.pal.n - start;
    for (i = 0; i < n; i++) {
        const uint8_t *q = hle_ptr(c, sel, (uint32_t)off + 4u * (unsigned)i, 4);
        uint8_t *e = o->u.pal.e[start + i];
        if (!q) return CPU_E_SEL;
        if (!(e[3] & PC_RESERVED)) continue;           /* only reserved entries may be animated */
        e[0] = q[0];
        e[1] = q[1];
        e[2] = q[2];
        if (o->u.pal.realized) set_sys(g, o->u.pal.map[start + i], entry_rgb(e));
    }
    g->pal_gen++;
    g->fb->dirty++;
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status g_getnearestpaletteindex(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    uint32_t cref = hle_u32(&a);
    gobj *o;
    GCHECK;
    o = pal_obj(g, h);
    hle_ret16(c, o && o->u.pal.n ? (uint16_t)gdi_pal_nearest_entry(&o->u.pal, cref & 0xFFFFFF) : 0xFFFF);
    return CPU_OK;
}

static cpu_status g_getnearestcolor(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    uint32_t cref = hle_u32(&a);
    gctx x;
    GCHECK;
    if (!gctx_open(g, h, &x)) {
        gctx_close(&x);
        hle_ret32(c, 0xFFFFFFFFu);
        return CPU_OK;
    }
    hle_ret32(c, gctx_colorref_of(&x, gctx_pixel(&x, cref)));
    gctx_close(&x);
    return CPU_OK;
}

static cpu_status g_getsystempaletteentries(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    int start = hle_u16(&a), n = hle_u16(&a), i;
    GCHECK;
    hle_far(&a, &sel, &off);
    GCHECK;
    if (!gdc_get(g, h)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (!sel && !off) {
        hle_ret16(c, 256);
        return CPU_OK;
    }
    if (start >= 256) n = 0;
    else if (start + n > 256) n = 256 - start;
    for (i = 0; i < n; i++) {
        uint8_t *q = hle_ptr(c, sel, (uint32_t)off + 4u * (unsigned)i, 4);
        uint32_t v = g->fb->pal[start + i];
        if (!q) return CPU_E_SEL;
        q[0] = (uint8_t)(v >> 16);
        q[1] = (uint8_t)(v >> 8);
        q[2] = (uint8_t)v;
        q[3] = 0;
    }
    hle_ret16(c, (uint16_t)n);
    return CPU_OK;
}

static cpu_status g_setsystempaletteuse(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), use = hle_u16(&a), prev;
    GCHECK;
    if (!gdc_get(g, h) || (use != 1 && use != 2)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    prev = (uint16_t)g->sysuse;
    g->sysuse = use;
    hle_ret16(c, prev);
    return CPU_OK;
}

static cpu_status g_getsystempaletteuse(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    GCHECK;
    hle_ret16(c, gdc_get(g, h) ? (uint16_t)g->sysuse : 0);
    return CPU_OK;
}

static cpu_status g_updatecolors(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    GCHECK;
    hle_ret16(c, gdc_get(g, h) ? 1 : 0);
    return CPU_OK;
}

static cpu_status g_resetdc(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    hle_far(&a, &sel, &off);
    GCHECK;
    hle_ret16(c, gdc_get(g, h) ? h : 0);             /* there is no printer to reconfigure: the display DC stays as it is */
    return CPU_OK;
}

/* USER.282 SelectPalette(hdc, hpal, bForceBackground), USER.283 RealizePalette(hdc) */
static cpu_status u_selectpalette(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), hp = hle_u16(&a), bk = hle_u16(&a), prev;
    gdc *dc;
    GCHECK;
    (void)bk;
    dc = gdc_get(g, h);
    if (!dc || !pal_obj(g, hp)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    prev = dc->pal;
    dc->pal = hp;
    pal_obj(g, hp)->sel++;
    if (pal_obj(g, prev)) pal_obj(g, prev)->sel--;
    g->pal_gen++;                                      /* PALETTEINDEX colours now resolve through the new palette */
    hle_ret16(c, prev);
    return CPU_OK;
}

static cpu_status u_realizepalette(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    gdc *dc;
    gobj *po;
    GCHECK;
    dc = gdc_get(g, h);
    po = dc ? pal_obj(g, dc->pal) : NULL;
    hle_ret16(c, po ? (uint16_t)realize(g, po) : 0);
    return CPU_OK;
}

#define G GDI_MODULE
#define U USER_MODULE_NAME
const hle_def gdi_pal_defs[] = {
    {G, 154, "GetNearestColor", 6, g_getnearestcolor},
    {G, 360, "CreatePalette", 4, g_createpalette},
    {G, 363, "GetPaletteEntries", 10, g_getpaletteentries},
    {G, 364, "SetPaletteEntries", 10, g_setpaletteentries},
    {G, 366, "UpdateColors", 2, g_updatecolors},                    /* UNV */
    {G, 367, "AnimatePalette", 10, g_animatepalette},               /* UNV */
    {G, 368, "ResizePalette", 4, g_resizepalette},                  /* UNV */
    {G, 370, "GetNearestPaletteIndex", 6, g_getnearestpaletteindex},
    {G, 373, "SetSystemPaletteUse", 4, g_setsystempaletteuse},      /* UNV */
    {G, 374, "GetSystemPaletteUse", 2, g_getsystempaletteuse},      /* UNV */
    {G, 375, "GetSystemPaletteEntries", 10, g_getsystempaletteentries}, /* UNV */
    {G, 376, "ResetDC", 6, g_resetdc},
    {U, 282, "SelectPalette", 6, u_selectpalette},
    {U, 283, "RealizePalette", 2, u_realizepalette},
};
#undef G
#undef U
const size_t gdi_pal_ndefs = sizeof gdi_pal_defs / sizeof gdi_pal_defs[0];
