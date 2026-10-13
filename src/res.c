/* Resource decoders. See res.h. */
#include "res.h"

#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }

typedef struct { const uint8_t *b; size_t len, pos; bool bad; } rd_t;

static unsigned rd_u8(rd_t *r) {
    if (r->pos + 1 > r->len) { r->bad = true; return 0; }
    return r->b[r->pos++];
}
static unsigned rd_u16(rd_t *r) {
    unsigned v;
    if (r->pos + 2 > r->len) { r->bad = true; r->pos = r->len; return 0; }
    v = rd16(r->b + r->pos);
    r->pos += 2;
    return v;
}
static uint32_t rd_u32(rd_t *r) {
    uint32_t v;
    if (r->pos + 4 > r->len) { r->bad = true; r->pos = r->len; return 0; }
    v = rd32(r->b + r->pos);
    r->pos += 4;
    return v;
}
/* NUL-terminated string, truncated into out */
static void rd_str(rd_t *r, char *out, size_t cap) {
    size_t n = 0;
    for (;;) {
        unsigned c;
        if (r->pos >= r->len) { r->bad = true; break; }
        c = r->b[r->pos++];
        if (!c) break;
        if (n + 1 < cap) out[n++] = (char)c;
    }
    out[n] = 0;
}

/* ---- resource table ------------------------------------------------------------ */

res_err res_find(const ne_image *ne, unsigned type, bool by_name, unsigned id, const char *name,
                 const uint8_t **data, size_t *len) {
    const ne_resource *r;
    ne_resid t = {(uint16_t)type, NULL}, n = {(uint16_t)id, by_name ? name : NULL};
    if (ne_find_resource(ne, t, n, &r) != NE_OK) return RES_NOTFOUND;
    if ((uint64_t)r->offset + r->length > ne->size) return RES_BAD;
    *data = ne->data + r->offset;
    *len = r->length;
    return RES_OK;
}

/* ---- strings ------------------------------------------------------------------- */

bool res_bundle_string(const uint8_t *blob, size_t len, unsigned idx, const uint8_t **s, size_t *n) {
    size_t pos = 0;
    unsigned i;
    if (idx > 15) return false;
    for (i = 0;; i++) {
        size_t l;
        if (pos >= len) return false;
        l = blob[pos++];
        if (pos + l > len) return false;
        if (i == idx) {
            *s = blob + pos;
            *n = l;
            return true;
        }
        pos += l;
    }
}

int res_string(const ne_image *ne, unsigned id, char *out, size_t cap) {
    const uint8_t *d, *s;
    size_t n, l;
    if (res_find(ne, RT_STRING, false, id / 16 + 1, NULL, &d, &n) != RES_OK) return -1;
    if (!res_bundle_string(d, n, id & 15, &s, &l)) return -1;
    if (cap) {
        size_t c = l < cap - 1 ? l : cap - 1;
        memcpy(out, s, c);
        out[c] = 0;
        return (int)c;
    }
    return (int)l;
}

/* ---- menus ----------------------------------------------------------------------- */

#define MENU_MAX_DEPTH 16

static res_err menu_level(rd_t *r, int depth, res_menu_fn fn, void *ctx) {
    if (depth >= MENU_MAX_DEPTH) return RES_BAD;
    for (;;) {
        unsigned flags = rd_u16(r), id = 0;
        char text[128];
        bool popup = (flags & MFR_POPUP) != 0;
        if (!popup) id = rd_u16(r);
        rd_str(r, text, sizeof text);
        if (r->bad) return RES_BAD;
        if (!fn(ctx, depth, (uint16_t)flags, (uint16_t)id, text, popup)) return RES_BAD;
        if (popup) {
            res_err e = menu_level(r, depth + 1, fn, ctx);
            if (e != RES_OK) return e;
        }
        if (flags & MFR_END) return RES_OK;
    }
}

res_err res_menu_walk(const uint8_t *blob, size_t len, res_menu_fn fn, void *ctx) {
    rd_t r = {blob, len, 0, false};
    unsigned cb;
    rd_u16(&r);
    cb = rd_u16(&r);
    if (r.bad || r.pos + cb > len) return RES_BAD;
    r.pos += cb;
    return menu_level(&r, 0, fn, ctx);
}

typedef struct { unsigned items, popups, depth; } mcount;
static bool count_cb(void *ctx, int depth, uint16_t flags, uint16_t id, const char *text, bool popup) {
    mcount *c = ctx;
    (void)flags; (void)id; (void)text;
    c->items++;
    if (popup) c->popups++;
    if ((unsigned)depth + 1 > c->depth) c->depth = (unsigned)depth + 1;
    return true;
}

res_err res_menu_count(const uint8_t *blob, size_t len, unsigned *items, unsigned *popups, unsigned *depth) {
    mcount c = {0, 0, 0};
    res_err e = res_menu_walk(blob, len, count_cb, &c);
    if (e != RES_OK) return e;
    *items = c.items;
    *popups = c.popups;
    *depth = c.depth;
    return RES_OK;
}

/* ---- dialogs ---------------------------------------------------------------------- */

static const char *const dlg_class[6] = {"BUTTON", "EDIT", "STATIC", "LISTBOX", "SCROLLBAR", "COMBOBOX"};

res_err res_dlg_parse(const uint8_t *blob, size_t len, res_dlg *out) {
    rd_t r = {blob, len, 0, false};
    unsigned i, n, first;
    memset(out, 0, sizeof *out);
    out->style = rd_u32(&r);
    n = rd_u8(&r);
    out->x = (int16_t)rd_u16(&r);
    out->y = (int16_t)rd_u16(&r);
    out->cx = (int16_t)rd_u16(&r);
    out->cy = (int16_t)rd_u16(&r);
    if (r.bad) return RES_BAD;
    first = rd_u8(&r);
    if (first == 0xFF) {
        out->menu_is_id = true;
        out->menu_id = (uint16_t)rd_u16(&r);
    } else if (first != 0) {
        r.pos--;
        rd_str(&r, out->menu, sizeof out->menu);
    }
    first = rd_u8(&r);
    if (first != 0) {
        r.pos--;
        rd_str(&r, out->cls, sizeof out->cls);
    }
    rd_str(&r, out->caption, sizeof out->caption);
    if (out->style & 0x40u) {    /* DS_SETFONT */
        out->pointsize = (uint16_t)rd_u16(&r);
        rd_str(&r, out->face, sizeof out->face);
    }
    if (r.bad) return RES_BAD;
    out->items = calloc(n ? n : 1, sizeof *out->items);
    if (!out->items) return RES_NOMEM;
    for (i = 0; i < n; i++) {
        res_dlg_item *it = &out->items[i];
        unsigned c, k;
        it->x = (int16_t)rd_u16(&r);
        it->y = (int16_t)rd_u16(&r);
        it->cx = (int16_t)rd_u16(&r);
        it->cy = (int16_t)rd_u16(&r);
        it->id = (uint16_t)rd_u16(&r);
        it->style = rd_u32(&r);
        c = rd_u8(&r);
        if (c >= 0x80) {
            if (c - 0x80 < 6) strcpy(it->cls, dlg_class[c - 0x80]);
            else strcpy(it->cls, "?");
        } else {
            r.pos--;
            rd_str(&r, it->cls, sizeof it->cls);
        }
        c = rd_u8(&r);
        if (c == 0xFF) {
            it->text_is_id = true;
            it->text_id = (uint16_t)rd_u16(&r);
        } else {
            r.pos--;
            rd_str(&r, it->text, sizeof it->text);
        }
        it->ncd = (uint8_t)rd_u8(&r);
        for (k = 0; k < it->ncd; k++) {
            unsigned v = rd_u8(&r);
            if (k < sizeof it->cdata) it->cdata[k] = (uint8_t)v;
        }
        if (r.bad) {
            res_dlg_free(out);
            return RES_BAD;
        }
        out->nitems = i + 1;
    }
    return RES_OK;
}

void res_dlg_free(res_dlg *d) {
    free(d->items);
    d->items = NULL;
    d->nitems = 0;
}

/* ---- accelerators --------------------------------------------------------------------- */

res_err res_accel_parse(const uint8_t *blob, size_t len, res_accel *out, size_t cap, size_t *n) {
    size_t pos = 0, k = 0;
    while (pos < len) {
        if (pos + 5 > len) return RES_BAD;
        if (k == cap) return RES_BAD;
        out[k].flags = blob[pos];
        out[k].key = rd16(blob + pos + 1);
        out[k].cmd = rd16(blob + pos + 3);
        k++;
        pos += 5;
        if (blob[pos - 5] & AF_LAST) break;
    }
    *n = k;
    return RES_OK;
}

/* ---- DIBs ----------------------------------------------------------------------------- */

void res_image_free(res_image *im) {
    free(im->cref);
    free(im->mask);
    memset(im, 0, sizeof *im);
}

res_err res_dib_decode(const uint8_t *dib, size_t len, bool has_mask, res_image *out) {
    uint32_t hsize;
    int w, h, bpp, ncol, entsz, vh, y, x;
    bool topdown = false;
    uint32_t pal[256];
    size_t tab_off, bits_off, stride, mstride, need;
    memset(out, 0, sizeof *out);
    if (len < 12) return RES_BAD;
    hsize = rd32(dib);
    if (hsize == 12) {
        w = rd16(dib + 4);
        h = rd16(dib + 6);
        bpp = rd16(dib + 10);
        entsz = 3;
        ncol = bpp <= 8 ? 1 << bpp : 0;
    } else if (hsize >= 40 && len >= 40) {
        int32_t sh = (int32_t)rd32(dib + 8);
        unsigned used = rd32(dib + 32);
        w = (int32_t)rd32(dib + 4);
        if (sh < 0) { topdown = true; sh = -sh; }
        h = sh;
        bpp = rd16(dib + 14);
        entsz = 4;
        if (rd32(dib + 16) != 0) return RES_BAD;       /* BI_RGB only */
        ncol = bpp <= 8 ? (used ? (int)used : 1 << bpp) : 0;
    } else {
        return RES_BAD;
    }
    if (w <= 0 || h <= 0 || w > 1024 || h > 2048) return RES_BAD;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24) return RES_BAD;
    if (ncol > 256) return RES_BAD;
    vh = has_mask ? h / 2 : h;
    if (vh <= 0) return RES_BAD;
    tab_off = hsize;
    bits_off = tab_off + (size_t)ncol * (size_t)entsz;
    stride = (((size_t)w * (size_t)bpp + 31) / 32) * 4;
    mstride = (((size_t)w + 31) / 32) * 4;
    need = bits_off + stride * (size_t)vh + (has_mask ? mstride * (size_t)vh : 0);
    if (need > len) return RES_BAD;
    for (x = 0; x < ncol; x++) {
        const uint8_t *q = dib + tab_off + (size_t)x * (size_t)entsz;
        pal[x] = (uint32_t)q[2] | (uint32_t)q[1] << 8 | (uint32_t)q[0] << 16;
    }
    out->w = w;
    out->h = vh;
    out->bpp = bpp;
    out->cref = malloc((size_t)w * (size_t)vh * sizeof *out->cref);
    if (has_mask) out->mask = malloc((size_t)w * (size_t)vh);
    if (!out->cref || (has_mask && !out->mask)) {
        res_image_free(out);
        return RES_NOMEM;
    }
    for (y = 0; y < vh; y++) {
        int sy = topdown ? y : vh - 1 - y;
        const uint8_t *row = dib + bits_off + stride * (size_t)sy;
        for (x = 0; x < w; x++) {
            uint32_t c;
            if (bpp == 24) {
                c = (uint32_t)row[x * 3 + 2] | (uint32_t)row[x * 3 + 1] << 8 | (uint32_t)row[x * 3] << 16;
            } else {
                unsigned idx;
                if (bpp == 8) idx = row[x];
                else if (bpp == 4) idx = (row[x / 2] >> (x & 1 ? 0 : 4)) & 15;
                else idx = (row[x / 8] >> (7 - (x & 7))) & 1;
                c = (int)idx < ncol ? pal[idx] : 0;
            }
            out->cref[(size_t)y * (size_t)w + (size_t)x] = c;
        }
        if (has_mask) {
            const uint8_t *mrow = dib + bits_off + stride * (size_t)vh + mstride * (size_t)sy;
            for (x = 0; x < w; x++) out->mask[(size_t)y * (size_t)w + (size_t)x] = (mrow[x / 8] >> (7 - (x & 7))) & 1;
        }
    }
    return RES_OK;
}

res_err res_icon_decode(const uint8_t *blob, size_t len, res_image *out) {
    return res_dib_decode(blob, len, true, out);
}

res_err res_cursor_decode(const uint8_t *blob, size_t len, res_image *out, int *hot_x, int *hot_y) {
    if (len < 4) return RES_BAD;
    *hot_x = rd16(blob);
    *hot_y = rd16(blob + 2);
    return res_dib_decode(blob + 4, len - 4, true, out);
}

/* ---- group directories ------------------------------------------------------------------ */

int res_group_count(const uint8_t *blob, size_t len) {
    unsigned n;
    if (len < 6) return -1;
    n = rd16(blob + 4);
    if (6 + (size_t)n * 14 > len) return -1;
    return (int)n;
}

res_err res_group_pick_best(const uint8_t *blob, size_t len, bool cursor, int want_w, int want_h,
                            res_group_pick *out) {
    int n = res_group_count(blob, len), i, best = -1, bscore = 0, bbpp = 0;
    for (i = 0; i < n; i++) {
        const uint8_t *e = blob + 6 + (size_t)i * 14;
        int w, h, bpp, score;
        if (cursor) {
            w = rd16(e);
            h = rd16(e + 2) / 2;
            bpp = rd16(e + 6) * (rd16(e + 4) ? rd16(e + 4) : 1);
        } else {
            w = e[0];
            h = e[1];
            bpp = rd16(e + 6) * (rd16(e + 4) ? rd16(e + 4) : 1);
            if (bpp == 0) {
                int cc = e[2] ? e[2] : 256;
                bpp = cc <= 2 ? 1 : cc <= 16 ? 4 : 8;
            }
            if (w == 0) w = 256;
            if (h == 0) h = 256;
        }
        score = abs(w - want_w) + abs(h - want_h);
        if (bpp > 8) score += 1000;           /* the screen is an 8-bit palette */
        if (best < 0 || score < bscore || (score == bscore && bpp > bbpp)) {
            best = i;
            bscore = score;
            bbpp = bpp;
            out->id = rd16(e + 12);
            out->w = w;
            out->h = h;
            out->bpp = bpp;
        }
    }
    return best < 0 ? RES_BAD : RES_OK;
}
