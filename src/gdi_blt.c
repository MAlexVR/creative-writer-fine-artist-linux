/* Bitmaps, ROP3, BitBlt / StretchBlt / PatBlt, pattern brushes and ScrollDC.
 *
 * Bitmaps are device dependent: one byte per pixel, a palette index of the screen
 * (8 bpp) or 0/1 (monochrome). The bits the API exchanges (CreateBitmap,
 * Get/SetBitmapBits) use the Windows layout: scanlines padded to 16 bits, most
 * significant bit first for monochrome, one byte per pixel for 8 bpp. CreateBitmap
 * also accepts 4 bpp packed pixels and 4 one-bit EGA planes, converted through
 * the 16 EGA colours.
 *
 * Blits work on device indices: ROP3 is evaluated bitwise on the 8-bit values,
 * as a VGA driver does. Differences from Windows: StretchBlt picks the source
 * pixel with truncating integer arithmetic whatever the stretch mode is (no
 * AND/OR merging when shrinking, no halftone); source pixels outside the source
 * surface leave the destination pixel alone; the source DC's clip is ignored. */
#include "gdi_priv.h"

/* ---- ROP3 ------------------------------------------------------------------------------ */

uint8_t rop3_eval(uint8_t rop, uint8_t p, uint8_t s, uint8_t d) {
    uint8_t r = 0, np = (uint8_t)~p, ns = (uint8_t)~s, nd = (uint8_t)~d;
    if (rop & 0x01) r |= np & ns & nd;
    if (rop & 0x02) r |= np & ns & d;
    if (rop & 0x04) r |= np & s & nd;
    if (rop & 0x08) r |= np & s & d;
    if (rop & 0x10) r |= p & ns & nd;
    if (rop & 0x20) r |= p & ns & d;
    if (rop & 0x40) r |= p & s & nd;
    if (rop & 0x80) r |= p & s & d;
    return r;
}

/* an operand matters when flipping it changes the result for some combination of the others */
bool rop3_uses_src(uint8_t rop) { return ((rop >> 2) ^ rop) & 0x33; }
bool rop3_uses_pat(uint8_t rop) { return ((rop >> 4) ^ rop) & 0x0F; }
bool rop3_uses_dst(uint8_t rop) { return ((rop >> 1) ^ rop) & 0x55; }

/* ---- bitmaps ------------------------------------------------------------------------------- */

static int native_row_bytes(int w, int bpp) { return bpp == 1 ? (w + 15) / 16 * 2 : (w + 1) & ~1; }

uint16_t gdi_bitmap_create(gdi_t *g, int w, int h, int bpp, gobj **out) {
    gobj *o;
    uint16_t hnd;
    if (w <= 0 || h <= 0 || (long)w * h > 16 * 1024 * 1024) return 0;
    hnd = gobj_new(g, GO_BITMAP, &o);
    if (!hnd) return 0;
    o->u.bmp.w = w;
    o->u.bmp.h = h;
    o->u.bmp.bpp = bpp;
    o->u.bmp.bits = calloc((size_t)w * (size_t)h, 1);
    if (!o->u.bmp.bits) {
        o->type = GO_FREE;
        return 0;
    }
    if (out) *out = o;
    return hnd;
}

/* the 16 EGA colours as COLORREFs */
static const uint32_t ega16[16] = {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
                                   0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};

/* Fills a bitmap from bits in the (planes, bpp) layout. */
static bool bitmap_import(gdi_t *g, gbitmap *b, int planes, int bpp, const uint8_t *src) {
    int x, y;
    if (!src) return true;
    if (planes == 1 && bpp == 1) {
        int rb = native_row_bytes(b->w, 1);
        for (y = 0; y < b->h; y++)
            for (x = 0; x < b->w; x++) b->bits[(size_t)y * b->w + x] = (src[y * rb + x / 8] >> (7 - (x & 7))) & 1;
    } else if (planes == 1 && bpp == 8) {
        int rb = native_row_bytes(b->w, 8);
        for (y = 0; y < b->h; y++) memcpy(b->bits + (size_t)y * b->w, src + (size_t)y * rb, (size_t)b->w);
    } else if (planes == 1 && bpp == 4) {
        int rb = (b->w * 4 + 15) / 16 * 2;
        for (y = 0; y < b->h; y++)
            for (x = 0; x < b->w; x++) {
                uint8_t v = src[y * rb + x / 2];
                v = (x & 1) ? v & 15 : v >> 4;
                b->bits[(size_t)y * b->w + x] = gdi_nearest(g, ega16[v]);
            }
    } else if (planes == 4 && bpp == 1) {
        int rb = native_row_bytes(b->w, 1), pl;
        for (y = 0; y < b->h; y++)
            for (x = 0; x < b->w; x++) {
                int v = 0;
                for (pl = 0; pl < 4; pl++) v |= ((src[(y * 4 + pl) * rb + x / 8] >> (7 - (x & 7))) & 1) << pl;
                b->bits[(size_t)y * b->w + x] = gdi_nearest(g, ega16[v]);
            }
    } else {
        return false;
    }
    return true;
}

static bool format_ok(int planes, int bpp) {
    return (planes == 1 && (bpp == 1 || bpp == 4 || bpp == 8)) || (planes == 4 && bpp == 1);
}

static uint16_t create_bitmap(gdi_t *g, hle_call *c, int w, int h, int planes, int bpp, uint16_t sel, uint16_t off, bool *fault) {
    gobj *o;
    uint16_t hb;
    const uint8_t *src = NULL;
    *fault = false;
    if (!format_ok(planes, bpp) || w <= 0 || h <= 0) return 0;
    hb = gdi_bitmap_create(g, w, h, planes == 1 && bpp == 1 ? 1 : 8, &o);
    if (!hb) return 0;
    if (sel || off) {
        uint32_t rb = planes == 4 ? (uint32_t)native_row_bytes(w, 1) * 4 : planes == 1 && bpp == 4 ? (uint32_t)(w * 4 + 15) / 16 * 2
                                                                                                   : (uint32_t)native_row_bytes(w, bpp);
        src = hle_ptr(c, sel, off, rb * (uint32_t)h);
        if (!src) {
            gobj_release(g, o);
            *fault = true;
            return 0;
        }
    }
    bitmap_import(g, &o->u.bmp, planes, bpp, src);
    return hb;
}

static cpu_status g_createbitmap(hle_call *c) {
    GBEGIN;
    int w = (int16_t)hle_u16(&a), h = (int16_t)hle_u16(&a), planes = hle_u16(&a) & 0xFF, bpp = hle_u16(&a) & 0xFF;
    uint16_t sel, off, r;
    bool fault;
    hle_far(&a, &sel, &off);
    GCHECK;
    r = create_bitmap(g, c, w, h, planes, bpp, sel, off, &fault);
    if (fault) return CPU_E_SEL;
    hle_ret16(c, r);
    return CPU_OK;
}

static cpu_status g_createbitmapindirect(hle_call *c) {
    GBEGIN;
    uint16_t sel, off, r;
    const uint8_t *p;
    bool fault;
    hle_far(&a, &sel, &off);
    GCHECK;
    if (!(p = hle_ptr(c, sel, off, 14))) return CPU_E_SEL;
    r = create_bitmap(g, c, (int16_t)gld16(p + 2), (int16_t)gld16(p + 4), p[8], p[9], gld16(p + 12), gld16(p + 10), &fault);
    if (fault) return CPU_E_SEL;
    hle_ret16(c, r);
    return CPU_OK;
}

static cpu_status compat_bitmap(hle_call *c) {
    GBEGIN;
    uint16_t hdc = hle_u16(&a);
    int w = (int16_t)hle_u16(&a), h = (int16_t)hle_u16(&a), bpp = 8;
    gdc *dc;
    GCHECK;
    dc = gdc_get(g, hdc);
    if (!dc) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (dc->mem) {
        gobj *o = gobj_typed(g, dc->bmp, GO_BITMAP);
        if (o && o->u.bmp.bpp == 1) bpp = 1;
    }
    hle_ret16(c, gdi_bitmap_create(g, w, h, bpp, NULL));
    return CPU_OK;
}

static cpu_status bitmap_bits(hle_call *c, bool set) {
    GBEGIN;
    uint16_t hb = hle_u16(&a), sel, off;
    uint32_t cb = hle_u32(&a), n, total, row, pos;
    gobj *o;
    uint8_t *buf;
    int y, x;
    hle_far(&a, &sel, &off);
    GCHECK;
    o = gobj_typed(g, hb, GO_BITMAP);
    if (!o) {
        hle_ret32(c, 0);
        return CPU_OK;
    }
    row = (uint32_t)native_row_bytes(o->u.bmp.w, o->u.bmp.bpp);
    total = row * (uint32_t)o->u.bmp.h;
    n = cb < total ? cb : total;
    if (n && !(buf = hle_ptr(c, sel, off, n))) return CPU_E_SEL;
    for (pos = 0; pos < n; pos++) {
        y = (int)(pos / row);
        x = (int)(pos % row);
        buf = hle_ptr(c, sel, (uint32_t)off + pos, 1);
        if (!buf) return CPU_E_SEL;
        if (o->u.bmp.bpp == 8) {
            if (x < o->u.bmp.w) {
                if (set) o->u.bmp.bits[(size_t)y * o->u.bmp.w + x] = *buf;
                else *buf = o->u.bmp.bits[(size_t)y * o->u.bmp.w + x];
            } else if (!set) {
                *buf = 0;
            }
        } else {
            int bit;
            uint8_t v = 0;
            for (bit = 0; bit < 8; bit++) {
                int px = x * 8 + bit;
                if (px >= o->u.bmp.w) continue;
                if (set) o->u.bmp.bits[(size_t)y * o->u.bmp.w + px] = (*buf >> (7 - bit)) & 1;
                else v |= (uint8_t)(o->u.bmp.bits[(size_t)y * o->u.bmp.w + px] << (7 - bit));
            }
            if (!set) *buf = v;
        }
    }
    hle_ret32(c, n);
    return CPU_OK;
}

static cpu_status g_getbitmapbits(hle_call *c) { return bitmap_bits(c, false); }
static cpu_status g_setbitmapbits(hle_call *c) { return bitmap_bits(c, true); }

static cpu_status g_getdcorg(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    gdc *dc;
    gsurf s;
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc || !gdc_surface(g, dc, &s)) hle_ret32(c, 0);
    else hle_ret32(c, (uint32_t)(uint16_t)s.ox | (uint32_t)(uint16_t)s.oy << 16);
    return CPU_OK;
}

/* ---- pattern brushes -------------------------------------------------------------------------- */

static uint16_t pattern_brush(gdi_t *g, uint16_t hbm) {
    gobj *bo = gobj_typed(g, hbm, GO_BITMAP), *o;
    uint16_t h;
    int x, y;
    if (!bo) return 0;
    h = gobj_new(g, GO_BRUSH, &o);
    if (!h) return 0;
    o->u.brush.style = BS_PATTERN;
    o->u.brush.hatch = hbm;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            uint8_t v = bo->u.bmp.bits[(size_t)(y % bo->u.bmp.h) * bo->u.bmp.w + (size_t)(x % bo->u.bmp.w)];
            o->u.brush.pat[y * 8 + x] = (bo->u.bmp.bpp == 1 ? GCREF_MONO : GCREF_RAW) | v;
        }
    return h;
}

uint16_t gdi_brush_pattern(gdi_t *g, uint16_t hbm) { return pattern_brush(g, hbm); }

static cpu_status g_createpatternbrush(hle_call *c) {
    GBEGIN;
    uint16_t hbm = hle_u16(&a);
    GCHECK;
    hle_ret16(c, pattern_brush(g, hbm));
    return CPU_OK;
}

/* ---- blits ------------------------------------------------------------------------------------------ */

/* A logical extent starting at lo, mapped through the DC: [first, last) normalized plus the sign. */
static void span_of(int p0, int p1, int *lo, int *n, bool *neg) {
    int d = p1 - p0;
    *neg = d < 0;
    if (d >= 0) {
        *lo = p0;
        *n = d;
    } else {
        *lo = p0 + d + 1;
        *n = -d;
    }
}

/* Destination rectangle (dl, dt, dwa x dha, surface coordinates) filled from the source rectangle (sl, st, swa x sha),
 * optionally mirrored; s is NULL for operations that do not read a source. */
bool blt_core(gctx *d, uint8_t r3, int dl, int dt, int dwa, int dha, const bsrc *s, int sl, int st, int swa, int sha, bool fx,
              bool fy) {
    bool uses_p = rop3_uses_pat(r3);
    bpat bp;
    uint8_t dmask = d->mono ? 1 : 0xFF, dfg = 0, dbk = 0;
    int ci;
    gobj *bo;
    memset(&bp, 0, sizeof bp);
    if (dwa <= 0 || dha <= 0 || (s && (swa <= 0 || sha <= 0))) return true;
    if (uses_p) {
        bo = gobj_typed(d->g, d->dc->brush, GO_BRUSH);
        if (bo) brush_prepare(d, &bo->u.brush, &bp);
        if (!bp.kind) return true;                /* a NULL brush: the pattern is not available */
    }
    if (s && s->mono && !d->mono) {
        dfg = gctx_pixel(d, d->dc->fg);
        dbk = gctx_pixel(d, d->dc->bk);
    }
    for (ci = 0; ci < d->clipr.n; ci++) {
        fb_rect cr, box = {dl, dt, dl + dwa, dt + dha};
        int px, py;
        if (!fb_rect_intersect(&cr, &box, &d->clipr.r[ci])) continue;
        for (py = cr.top; py < cr.bottom; py++) {
            int sj = 0;
            if (s) {
                sj = (int)((long)(py - dt) * sha / dha);
                if (fy) sj = sha - 1 - sj;
                sj += st;
                if (sj < s->row_lo || sj >= s->row_hi || sj >= s->h) continue;
            }
            for (px = cr.left; px < cr.right; px++) {
                uint8_t sv = 0, dv = d->pix[(size_t)py * d->sw + px], pv = 0;
                if (s) {
                    int si = (int)((long)(px - dl) * swa / dwa), sx2;
                    if (fx) si = swa - 1 - si;
                    sx2 = si + sl;
                    if (sx2 < 0 || sx2 >= s->w) continue;
                    sv = s->pix[(size_t)sj * s->w + sx2];
                    if (s->mono && !d->mono) sv = sv ? dbk : dfg;
                    else if (!s->mono && d->mono) sv = sv == s->bkpix;
                }
                if (uses_p) {
                    int p = bpat_at(d, &bp, px, py);
                    if (p == BPAT_CLEAR) continue;
                    pv = (uint8_t)p;
                }
                d->pix[(size_t)py * d->sw + px] = rop3_eval(r3, pv, sv, dv) & dmask;
            }
        }
    }
    d->g->fb->dirty++;
    return true;
}

static bool blt(gdi_t *g, uint16_t hd, int x, int y, int cx, int cy, uint16_t hs, int xs, int ys, int cxs, int cys, uint32_t rop,
                bool stretch) {
    uint8_t r3 = (uint8_t)(rop >> 16);
    bool uses_s = rop3_uses_src(r3), ok = false, fx = false, fy = false, nx, ny;
    gctx d, s;
    int dl, dt, dwa, dha, sl = 0, st = 0, swa = 0, sha = 0;
    uint8_t *snap = NULL;
    bsrc src;
    if (!gctx_open(g, hd, &d)) {
        gctx_close(&d);
        return false;
    }
    memset(&s, 0, sizeof s);
    grgn_init(&s.clipr);
    if (uses_s && !gctx_open(g, hs, &s)) goto out;
    {
        int x0 = gdc_lx(d.dc, x), x1 = gdc_lx(d.dc, x + cx), y0 = gdc_ly(d.dc, y), y1 = gdc_ly(d.dc, y + cy);
        span_of(x0, x1, &dl, &dwa, &nx);
        span_of(y0, y1, &dt, &dha, &ny);
        dl += d.ox;
        dt += d.oy;
    }
    if (uses_s) {
        if (stretch) {
            int sx0 = gdc_lx(s.dc, xs), sx1 = gdc_lx(s.dc, xs + cxs), sy0 = gdc_ly(s.dc, ys), sy1 = gdc_ly(s.dc, ys + cys);
            bool snx, sny;
            span_of(sx0, sx1, &sl, &swa, &snx);
            span_of(sy0, sy1, &st, &sha, &sny);
            fx = snx != nx;
            fy = sny != ny;
        } else {
            sl = gdc_lx(s.dc, xs);
            st = gdc_ly(s.dc, ys);
            swa = dwa;
            sha = dha;
        }
        sl += s.ox;
        st += s.oy;
        src.pix = s.pix;
        src.w = s.sw;
        src.h = s.sh;
        src.mono = s.mono;
        src.bkpix = !s.mono && d.mono ? gctx_pixel(&s, s.dc->bk) : 0;
        src.row_lo = 0;
        src.row_hi = s.sh;
        if (s.pix == d.pix) {                    /* same surface: work from a copy so overlapping moves are exact */
            size_t n = (size_t)s.sw * (size_t)s.sh;
            snap = malloc(n);
            if (!snap) goto out;
            memcpy(snap, s.pix, n);
            src.pix = snap;
        }
    }
    ok = blt_core(&d, r3, dl, dt, dwa, dha, uses_s ? &src : NULL, sl, st, swa, sha, fx, fy);
out:
    free(snap);
    gctx_close(&s);
    gctx_close(&d);
    return ok;
}

static cpu_status g_bitblt(hle_call *c) {
    GBEGIN;
    uint16_t hd = hle_u16(&a);
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a), cx = (int16_t)hle_u16(&a), cy = (int16_t)hle_u16(&a);
    uint16_t hs = hle_u16(&a);
    int xs = (int16_t)hle_u16(&a), ys = (int16_t)hle_u16(&a);
    uint32_t rop = hle_u32(&a);
    GCHECK;
    hle_ret16(c, blt(g, hd, x, y, cx, cy, hs, xs, ys, cx, cy, rop, false));
    return CPU_OK;
}

static cpu_status g_stretchblt(hle_call *c) {
    GBEGIN;
    uint16_t hd = hle_u16(&a);
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a), cx = (int16_t)hle_u16(&a), cy = (int16_t)hle_u16(&a);
    uint16_t hs = hle_u16(&a);
    int xs = (int16_t)hle_u16(&a), ys = (int16_t)hle_u16(&a), cxs = (int16_t)hle_u16(&a), cys = (int16_t)hle_u16(&a);
    uint32_t rop = hle_u32(&a);
    GCHECK;
    hle_ret16(c, blt(g, hd, x, y, cx, cy, hs, xs, ys, cxs, cys, rop, true));
    return CPU_OK;
}

static cpu_status g_patblt(hle_call *c) {
    GBEGIN;
    uint16_t hd = hle_u16(&a);
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a), cx = (int16_t)hle_u16(&a), cy = (int16_t)hle_u16(&a);
    uint32_t rop = hle_u32(&a);
    GCHECK;
    hle_ret16(c, !rop3_uses_src((uint8_t)(rop >> 16)) && blt(g, hd, x, y, cx, cy, 0, 0, 0, 0, 0, rop, false));
    return CPU_OK;
}

/* ScrollDC(hdc, dx, dy, lprcScroll, lprcClip, hrgnUpdate, lprcUpdate): moves the pixels of the scroll rectangle
 * (clipped by lprcClip and the DC clip); the area left uncovered is returned as a region and/or rectangle. */
static cpu_status u_scrolldc(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), ssel, soff, csel, coff, usel, uoff, hr;
    int dx = (int16_t)hle_u16(&a), dy = (int16_t)hle_u16(&a);
    const uint8_t *rp;
    gctx x;
    fb_rect scroll, clip, src, dst, c1, upd_box;
    grgn upd, moved;
    bool ok = false;
    hle_far(&a, &ssel, &soff);
    hle_far(&a, &csel, &coff);
    hr = hle_u16(&a);
    hle_far(&a, &usel, &uoff);
    GCHECK;
    if (!gctx_open(g, h, &x)) {
        gctx_close(&x);
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (ssel || soff) {
        if (!(rp = hle_ptr(c, ssel, soff, 8))) return CPU_E_SEL;
        scroll = dev_rect_of(x.dc, (int16_t)gld16(rp), (int16_t)gld16(rp + 2), (int16_t)gld16(rp + 4), (int16_t)gld16(rp + 6));
    } else {
        scroll = x.clipr.box;
        scroll.left -= x.ox; scroll.right -= x.ox; scroll.top -= x.oy; scroll.bottom -= x.oy;
    }
    if (csel || coff) {
        if (!(rp = hle_ptr(c, csel, coff, 8))) return CPU_E_SEL;
        clip = dev_rect_of(x.dc, (int16_t)gld16(rp), (int16_t)gld16(rp + 2), (int16_t)gld16(rp + 4), (int16_t)gld16(rp + 6));
        if (!fb_rect_intersect(&scroll, &scroll, &clip)) memset(&scroll, 0, sizeof scroll);
    }
    dx = gdc_scale_x(x.dc, dx);
    dy = gdc_scale_y(x.dc, dy);
    scroll.left += x.ox; scroll.right += x.ox; scroll.top += x.oy; scroll.bottom += x.oy;
    grgn_init(&upd);
    grgn_init(&moved);
    if (grgn_set_rect(&upd, scroll.left, scroll.top, scroll.right, scroll.bottom)) {
        dst = scroll;
        dst.left += dx; dst.right += dx; dst.top += dy; dst.bottom += dy;
        if (fb_rect_intersect(&c1, &dst, &scroll)) {          /* destination inside the scroll rectangle */
            uint8_t *snap;
            size_t n = (size_t)x.sw * (size_t)x.sh;
            src = c1;
            src.left -= dx; src.right -= dx; src.top -= dy; src.bottom -= dy;
            snap = malloc(n);
            if (snap) {
                int px, py, i;
                memcpy(snap, x.pix, n);
                for (i = 0; i < x.clipr.n; i++) {
                    fb_rect cr;
                    if (!fb_rect_intersect(&cr, &c1, &x.clipr.r[i])) continue;
                    for (py = cr.top; py < cr.bottom; py++)
                        for (px = cr.left; px < cr.right; px++) x.pix[(size_t)py * x.sw + px] = snap[(size_t)(py - dy) * x.sw + (px - dx)];
                }
                free(snap);
                if (grgn_set_rect(&moved, c1.left, c1.top, c1.right, c1.bottom)) grgn_combine(&upd, &upd, &moved, RGN_DIFF);
                ok = true;
            }
        } else {
            ok = true;                                          /* nothing stays inside: everything is uncovered */
        }
    }
    g->fb->dirty++;
    /* The scroll and clip rectangles come in logical units, so the uncovered area goes back in logical units too (a
     * program that scrolls a pane under a viewport origin intersects it with rectangles in pane coordinates). */
    upd_box = upd.box;
    {
        int lx0 = 0, ly0 = 0, lx1 = upd_box.right - x.ox, ly1 = upd_box.bottom - x.oy, lx2 = upd_box.left - x.ox, ly2 = upd_box.top - x.oy;
        gdc_dp_to_lp(x.dc, &lx1, &ly1);
        gdc_dp_to_lp(x.dc, &lx2, &ly2);
        gdc_dp_to_lp(x.dc, &lx0, &ly0);
        upd_box.left = lx2 < lx1 ? lx2 : lx1;
        upd_box.right = lx2 < lx1 ? lx1 : lx2;
        upd_box.top = ly2 < ly1 ? ly2 : ly1;
        upd_box.bottom = ly2 < ly1 ? ly1 : ly2;
        if (ok && hr) {
            gobj *ro = gobj_typed(g, hr, GO_REGION);
            if (ro) {
                grgn t;
                grgn_init(&t);
                if (x.dc->vex == x.dc->wex && x.dc->vey == x.dc->wey) {       /* origin only: shift the region */
                    if (grgn_copy(&t, &upd)) {
                        grgn_offset(&t, lx0 - x.ox, ly0 - x.oy);
                        grgn_copy(&ro->u.rgn, &t);
                    }
                } else if (grgn_set_rect(&t, upd_box.left, upd_box.top, upd_box.right, upd_box.bottom)) {
                    grgn_copy(&ro->u.rgn, &t);                                /* scaled mapping: its bounding box */
                }
                grgn_free(&t);
            }
        }
    }
    if (ok && (usel || uoff)) {
        uint8_t *up = hle_ptr(c, usel, uoff, 8);
        if (!up) {
            grgn_free(&upd);
            grgn_free(&moved);
            gctx_close(&x);
            return CPU_E_SEL;
        }
        if (!upd.n) memset(&upd_box, 0, sizeof upd_box);
        gst16(up, (uint16_t)upd_box.left);
        gst16(up + 2, (uint16_t)upd_box.top);
        gst16(up + 4, (uint16_t)upd_box.right);
        gst16(up + 6, (uint16_t)upd_box.bottom);
    }
    grgn_free(&upd);
    grgn_free(&moved);
    gctx_close(&x);
    hle_ret16(c, ok);
    return CPU_OK;
}

#define G GDI_MODULE
#define U USER_MODULE_NAME
const hle_def gdi_blt_defs[] = {
    {G, 29, "PatBlt", 14, g_patblt},
    {G, 34, "BitBlt", 20, g_bitblt},
    {G, 35, "StretchBlt", 24, g_stretchblt},
    {G, 46, "SetBitmapBits", 10, g_setbitmapbits},             /* UNV */
    {G, 48, "CreateBitmap", 12, g_createbitmap},
    {G, 49, "CreateBitmapIndirect", 4, g_createbitmapindirect},
    {G, 51, "CreateCompatibleBitmap", 6, compat_bitmap},
    {G, 60, "CreatePatternBrush", 2, g_createpatternbrush},
    {G, 74, "GetBitmapBits", 10, g_getbitmapbits},
    {G, 79, "GetDCOrg", 2, g_getdcorg},                        /* UNV */
    {G, 156, "CreateDiscardableBitmap", 6, compat_bitmap},
    {U, 221, "ScrollDC", 20, u_scrolldc},
};
#undef G
#undef U
const size_t gdi_blt_ndefs = sizeof gdi_blt_defs / sizeof gdi_blt_defs[0];
