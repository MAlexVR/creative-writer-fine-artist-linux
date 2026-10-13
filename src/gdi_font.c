/* Fonts and text: the LOGFONT -> built-in bitmap font mapping, text output,
 * metrics, DrawText and font enumeration.
 *
 * Every face is drawn from one built-in glyph set: the classic 5x7 ASCII table
 * (public domain "standard 5x7 font", one byte per column, bit 0 on top, bit 7
 * the descender row), scaled to the requested cell height. Nothing is read from
 * the program's TrueType files: AddFontResource only registers the family name
 * so enumeration lists it. The mapping:
 *  - cell height: |lfHeight| (a negative height is the em size; the cell then
 *    adds an internal leading of a fifth of it); 0 gives 16;
 *  - rows: the 8 glyph rows are stretched over the cell, the baseline sits at
 *    seven eighths of it (ascent = height - height / 8);
 *  - width: lfWidth when given, otherwise the unit width times 6 where the unit
 *    is height / 12 (fixed pitch) or height / 12.5 (proportional), at least 1.
 *    Fixed pitch fonts advance by that width; proportional fonts advance by the
 *    trimmed glyph width plus one unit, so "i" is narrower than "W";
 *  - bold (weight >= 600) smears the glyph one pixel to the right and widens the
 *    advance by one (tmOverhang 1); italic shears the top rows to the right;
 *    underline, strikeout, escapement are honoured except for escapement/rotation;
 *  - Latin-1 letters with accents are drawn as their base letter. */
#include <ctype.h>
#include <math.h>

#include "gdi_priv.h"

static const uint8_t font5x7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x08, 0x07, 0x03, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x80, 0x70, 0x30, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x00, 0x60, 0x60, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x72, 0x49, 0x49, 0x49, 0x46}, {0x21, 0x41, 0x49, 0x4D, 0x33}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x31}, {0x41, 0x21, 0x11, 0x09, 0x07},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x46, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x00, 0x14, 0x00, 0x00},
    {0x00, 0x40, 0x34, 0x00, 0x00}, {0x00, 0x08, 0x14, 0x22, 0x41}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x59, 0x09, 0x06}, {0x3E, 0x41, 0x5D, 0x59, 0x4E},
    {0x7C, 0x12, 0x11, 0x12, 0x7C}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x41, 0x51, 0x73}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x1C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x26, 0x49, 0x49, 0x49, 0x32}, {0x03, 0x01, 0x7F, 0x01, 0x03}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x59, 0x49, 0x4D, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x41},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x41, 0x7F}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x03, 0x07, 0x08, 0x00}, {0x20, 0x54, 0x54, 0x78, 0x40},
    {0x7F, 0x28, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x28}, {0x38, 0x44, 0x44, 0x28, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x18}, {0x00, 0x08, 0x7E, 0x09, 0x02}, {0x18, 0xA4, 0xA4, 0x9C, 0x78},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x40, 0x3D, 0x00},
    {0x7F, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x78, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0xFC, 0x18, 0x24, 0x24, 0x18},
    {0x18, 0x24, 0x24, 0x18, 0xFC}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x24},
    {0x04, 0x04, 0x3F, 0x44, 0x24}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x4C, 0x90, 0x90, 0x90, 0x7C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00}, {0x00, 0x00, 0x77, 0x00, 0x00},
    {0x00, 0x41, 0x36, 0x08, 0x00}, {0x02, 0x01, 0x02, 0x04, 0x02}};

/* base letters of Latin-1 0xC0..0xFF */
static const char latin1_base[] = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

/* Windows-1252 punctuation in 0x80-0x9F has no glyph in the 5x7 table: draw the nearest ASCII mark. */
static unsigned char cp1252_punct(unsigned char ch) {
    switch (ch) {
    case 0x85: return '.';
    case 0x91: case 0x92: return '\'';
    case 0x93: case 0x94: return '"';
    case 0x95: return '*';
    case 0x96: case 0x97: return '-';
    case 0xA0: return ' ';
    default: return ch;
    }
}

static const uint8_t *glyph_of(unsigned char ch, int *blank) {
    *blank = 0;
    ch = cp1252_punct(ch);
    if (ch >= 0xC0) ch = (unsigned char)latin1_base[ch - 0xC0];
    if (ch >= 32 && ch <= 126) return font5x7[ch - 32];
    *blank = 1;
    return font5x7[0];
}

/* occupied column range of a glyph */
static void glyph_extent(const uint8_t *g, int *lb, int *rb) {
    int c, first = -1, last = -1;
    for (c = 0; c < 5; c++)
        if (g[c]) {
            if (first < 0) first = c;
            last = c;
        }
    *lb = first < 0 ? 0 : first;
    *rb = first < 0 ? -1 : last;
}

/* ---- geometry from the LOGFONT ----------------------------------------------------------- */

static bool name_has(const char *face, const char *what) {
    size_t n = strlen(what), i, j;
    for (i = 0; face[i]; i++) {
        for (j = 0; j < n && face[i + j]; j++)
            if (tolower((unsigned char)face[i + j]) != tolower((unsigned char)what[j])) break;
        if (j == n) return true;
    }
    return false;
}

void gdi_font_resolve(gfont *f) {
    int h = (int16_t)gld16(f->raw), w = (int16_t)gld16(f->raw + 2), em, cell;
    const char *face = (const char *)f->raw + 18;
    f->weight = (int16_t)gld16(f->raw + 8);
    if (f->weight == 0) f->weight = 400;
    f->italic = f->raw[10] != 0;
    f->underline = f->raw[11] != 0;
    f->strikeout = f->raw[12] != 0;
    f->fixed = (f->raw[17] & 3) == 1 || name_has(face, "courier") || name_has(face, "terminal") ||
               name_has(face, "fixedsys") || name_has(face, "console");
    if (h == 0) h = 16;
    if (h < 0) {
        em = -h;
        cell = em + (em + 4) / 5;
    } else {
        cell = h;
        em = cell;
    }
    if (cell < 6) cell = 6;
    if (cell > 200) cell = 200;
    f->height = cell;
    f->intlead = h < 0 ? cell - em : 0;
    f->descent = cell / 8 > 0 ? cell / 8 : 1;
    f->ascent = cell - f->descent;
    f->width = w < 0 ? -w : w;
    f->bold = f->weight >= 600;
    f->overhang = f->bold && !name_has(face, "system") ? 1 : 0;   /* the System font is a real bold raster font */
    if (f->width) f->sx = f->width / 6.0;
    else f->sx = f->fixed ? cell / 12.0 : cell / 12.5;
    if (f->sx < 1.0) f->sx = 1.0;
    f->avgw = f->width ? f->width : (int)floor(6 * f->sx + 0.5);
    f->maxw = f->fixed ? f->avgw : (int)floor(7 * f->sx + 0.5);
    if (f->maxw < f->avgw) f->maxw = f->avgw;
    f->base = 8;
}

/* device advance of a character, without character extra */
static int advance_of(const gfont *f, unsigned char ch) {
    int lb, rb, blank, a;
    const uint8_t *g;
    if (f->tt) return ttf_inst_advance(f->tt, ch) + f->overhang;
    if (f->bmp) return fnt_width(f->bmp, ch) + f->overhang;
    ch = cp1252_punct(ch);
    if (f->fixed) return f->avgw + f->overhang;
    g = glyph_of(ch, &blank);
    if (ch == 0x20 || blank) {
        a = (int)floor(3 * f->sx + 0.5);
    } else {
        glyph_extent(g, &lb, &rb);
        a = (int)floor((rb - lb + 1 + 1) * f->sx + 0.5);
    }
    if (a < 1) a = 1;
    return a + f->overhang;
}

/* ---- text layout and painting ----------------------------------------------------------------- */

typedef struct {
    uint8_t ch;
    bool ul;      /* underline this character only (DrawText prefix) */
    bool skip;    /* advance only */
    int adv;      /* device advance, character extra and justification included */
} titem;

static int font_for(gdi_t *g, const gdc *dc, const gfont **out) {
    gobj *o = gobj_typed(g, dc->font, GO_FONT);
    if (!o) o = gobj_typed(g, g->stock[13], GO_FONT);
    *out = o ? &o->u.font : NULL;
    return o != NULL;
}

static void put_text_px(const gctx *x, const fb_rect *clip, int sx, int sy, uint8_t p) {
    if (clip && (sx < clip->left || sx >= clip->right || sy < clip->top || sy >= clip->bottom)) return;
    if (sx < 0 || sy < 0 || sx >= x->sw || sy >= x->sh || !grgn_has_point(&x->clipr, sx, sy)) return;
    x->pix[(size_t)sy * x->sw + sx] = p & (x->mono ? 1 : 0xFF);
}

static void paint_glyph(const gctx *x, const gfont *f, const fb_rect *clip, int x0, int y0, unsigned char ch, int smear,
                        uint8_t fg) {
    int blank, lb, rb, ncols, gw, xo, yo;
    const uint8_t *g;
    if (f->tt) {
        ttf_glyph tg;
        int r, c, base = y0 + f->ascent;
        if (!ttf_inst_glyph(f->tt, ch, &tg) || !tg.w) return;
        for (r = 0; r < tg.h; r++) {
            int py = tg.yoff + r, shear = f->italic && py < 0 ? -py / 4 : 0;
            for (c = 0; c < tg.w; c++) {
                if (!tg.bits[r * tg.w + c]) continue;
                put_text_px(x, clip, x0 + tg.xoff + c + shear, base + py, fg);
                if (smear) put_text_px(x, clip, x0 + tg.xoff + c + shear + 1, base + py, fg);
            }
        }
        return;
    }
    if (f->bmp) {
        int w = fnt_width(f->bmp, ch);
        for (yo = 0; yo < f->bmp->height; yo++) {
            int shear = f->italic && !f->bmp->italic ? (f->bmp->height - 1 - yo) / 4 : 0;
            for (xo = 0; xo < w; xo++) {
                if (!fnt_pixel(f->bmp, ch, xo, yo)) continue;
                put_text_px(x, clip, x0 + xo + shear, y0 + yo, fg);
                if (smear) put_text_px(x, clip, x0 + xo + shear + 1, y0 + yo, fg);
            }
        }
        return;
    }
    g = glyph_of(ch, &blank);
    if (blank || ch == 0x20) return;
    if (f->fixed) {
        lb = 0;
        ncols = 5;
    } else {
        glyph_extent(g, &lb, &rb);
        ncols = rb - lb + 1;
    }
    gw = (int)floor(ncols * f->sx + 0.5);
    if (gw < 1) gw = 1;
    for (yo = 0; yo < f->height; yo++) {
        int row = yo * 8 / f->height, shear = f->italic ? (f->height - 1 - yo) / 4 : 0;
        for (xo = 0; xo < gw; xo++) {
            int col = lb + xo * ncols / gw;
            if (col < 0 || col > 4 || !((g[col] >> row) & 1)) continue;
            put_text_px(x, clip, x0 + xo + shear, y0 + yo, fg);
            if (smear) put_text_px(x, clip, x0 + xo + shear + 1, y0 + yo, fg);
        }
    }
}

static void fill_cell(const gctx *x, const fb_rect *clip, int l, int t, int r, int b, uint8_t p) {
    int sx, sy;
    for (sy = t; sy < b; sy++)
        for (sx = l; sx < r; sx++) put_text_px(x, clip, sx, sy, p);
}

/* paints one run of items with its top-left cell corner at (x0, y0) */
static int paint_items(gctx *x, const gfont *f, int x0, int y0, const titem *it, int n, bool opaque, const fb_rect *clip) {
    gdc *dc = x->dc;
    uint8_t fg = gctx_pixel(x, dc->fg), bg = gctx_pixel(x, dc->bk);
    int i, cx = x0, total = 0, ulh = f->height >= 32 ? 2 : 1, sy;
    for (i = 0; i < n; i++) total += it[i].adv;
    if (opaque) fill_cell(x, clip, x0, y0, x0 + total, y0 + f->height, bg);
    for (i = 0; i < n; i++) {
        if (!it[i].skip) paint_glyph(x, f, clip, cx, y0, it[i].ch, f->bold, fg);
        if (it[i].ul || f->underline) {
            int ul = y0 + f->ascent + 1;
            if (ul + ulh > y0 + f->height) ul = y0 + f->height - ulh;
            if (f->underline || it[i].ul)
                for (sy = ul; sy < ul + ulh; sy++) fill_cell(x, clip, cx, sy, cx + it[i].adv, sy + 1, fg);
        }
        if (f->strikeout) {
            int st = y0 + f->ascent / 2;
            fill_cell(x, clip, cx, st, cx + it[i].adv, st + ulh, fg);
        }
        cx += it[i].adv;
    }
    x->g->fb->dirty++;
    return total;
}

/* advances of a plain string: character extra, justification (consumed from jextra/jcount copies) */
static int build_items(const gdc *dc, const gfont *f, const uint8_t *s, int n, const int16_t *dx, titem *it) {
    int i, total = 0, extra = dc->jextra, count = dc->jcount;
    for (i = 0; i < n; i++) {
        int a = advance_of(f, s[i]) + dc->cextra;
        if (s[i] == 0x20 && count > 0) {
            int share = extra / count;
            a += share;
            extra -= share;
            count--;
        }
        if (dx) a = gdc_scale_x(dc, dx[i]);
        it[i].ch = s[i];
        it[i].ul = false;
        it[i].skip = false;
        it[i].adv = a;
        total += a;
    }
    return total;
}

static int text_core(gdi_t *g, uint16_t hdc, int lx, int ly, unsigned opts, const fb_rect *lrc, const uint8_t *s, int n,
                     const int16_t *dx) {
    gctx x;
    const gfont *f;
    gdc *dc;
    titem *it;
    int total, rx, ry, x0, y0;
    fb_rect rc, clip, *cp = NULL;
    if (!gctx_open(g, hdc, &x)) {
        gctx_close(&x);
        return 0;
    }
    dc = x.dc;
    if (!font_for(g, dc, &f)) {
        gctx_close(&x);
        return 0;
    }
    it = malloc((size_t)(n > 0 ? n : 1) * sizeof *it);
    if (!it) {
        gctx_close(&x);
        return 0;
    }
    total = build_items(dc, f, s, n, dx, it);
    if (dc->talign & TA_UPDATECP) {
        lx = dc->px;
        ly = dc->py;
    }
    rx = gdc_lx(dc, lx) + x.ox;
    ry = gdc_ly(dc, ly) + x.oy;
    switch (dc->talign & 6) {
    case 2: x0 = rx - total; break;
    case 6: x0 = rx - total / 2; break;
    default: x0 = rx; break;
    }
    switch (dc->talign & 24) {
    case 8: y0 = ry - f->height; break;
    case 24: y0 = ry - f->ascent; break;
    default: y0 = ry; break;
    }
    if (lrc) {
        rc = dev_rect_of(dc, lrc->left, lrc->top, lrc->right, lrc->bottom);
        rc.left += x.ox; rc.right += x.ox; rc.top += x.oy; rc.bottom += x.oy;
        if (opts & 2) {
            uint8_t bg = gctx_pixel(&x, dc->bk);
            fill_cell(&x, NULL, rc.left, rc.top, rc.right, rc.bottom, bg);
        }
        if (opts & 4) {
            clip = rc;
            cp = &clip;
        }
    }
    paint_items(&x, f, x0, y0, it, n, dc->bkmode == OPAQUE, cp);
    if (dc->prn) gdi_print_text(g, dc, x0, y0 + f->ascent, f->height - f->intlead, s, n);
    if (dc->talign & TA_UPDATECP) {
        if ((dc->talign & 6) == 0) {
            int wl = dc->vex ? total * dc->wex / dc->vex : total;
            dc->px += wl;
        }
    }
    dc->jextra = dc->jcount = 0;
    free(it);
    gctx_close(&x);
    return 1;
}

static cpu_status g_textout(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a), n;
    const uint8_t *s = NULL;
    hle_far(&a, &sel, &off);
    n = (int16_t)hle_u16(&a);
    GCHECK;
    if (n < 0 || (n && !(s = hle_ptr(c, sel, off, (uint32_t)n)))) {
        hle_ret16(c, 0);
        return n < 0 ? CPU_OK : CPU_E_SEL;
    }
    hle_ret16(c, (uint16_t)text_core(g, h, x, y, 0, NULL, s, n, NULL));
    return CPU_OK;
}

static cpu_status g_exttextout(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), opts, rsel, roff, ssel, soff, dsel, doff;
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a), n, i;
    const uint8_t *s = NULL, *rp;
    int16_t *dx = NULL;
    fb_rect rc, *rcp = NULL;
    int ok;
    opts = hle_u16(&a);
    hle_far(&a, &rsel, &roff);
    hle_far(&a, &ssel, &soff);
    n = (int16_t)hle_u16(&a);
    hle_far(&a, &dsel, &doff);
    GCHECK;
    if (n < 0) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (n && !(s = hle_ptr(c, ssel, soff, (uint32_t)n))) return CPU_E_SEL;
    if (rsel || roff) {
        if (!(rp = hle_ptr(c, rsel, roff, 8))) return CPU_E_SEL;
        rc.left = (int16_t)gld16(rp);
        rc.top = (int16_t)gld16(rp + 2);
        rc.right = (int16_t)gld16(rp + 4);
        rc.bottom = (int16_t)gld16(rp + 6);
        rcp = &rc;
    }
    if (n && (dsel || doff)) {
        dx = malloc((size_t)n * sizeof *dx);
        if (!dx) return CPU_E_UD;
        for (i = 0; i < n; i++) {
            const uint8_t *q = hle_ptr(c, dsel, (uint32_t)doff + 2u * (unsigned)i, 2);
            if (!q) {
                free(dx);
                return CPU_E_SEL;
            }
            dx[i] = (int16_t)gld16(q);
        }
    }
    ok = text_core(g, h, x, y, opts, rcp, s, n, dx);
    free(dx);
    hle_ret16(c, (uint16_t)ok);
    return CPU_OK;
}

/* ---- metrics --------------------------------------------------------------------------------------- */

static int width_logical(const gdc *dc, int dev) { return dc->vex ? dev * dc->wex / dc->vex : dev; }
static int height_logical(const gdc *dc, int dev) { return dc->vey ? abs(dev * dc->wey / dc->vey) : dev; }

static bool measure(gdi_t *g, uint16_t hdc, const uint8_t *s, int n, int *w, int *h) {
    gdc *dc = gdc_get(g, hdc);
    const gfont *f;
    titem *it;
    if (!dc || !font_for(g, dc, &f)) return false;
    it = malloc((size_t)(n > 0 ? n : 1) * sizeof *it);
    if (!it) return false;
    *w = width_logical(dc, build_items(dc, f, s, n, NULL, it));
    *h = height_logical(dc, f->height);
    free(it);
    return true;
}

static cpu_status g_gettextextent(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    int n, w, hh;
    const uint8_t *s = NULL;
    hle_far(&a, &sel, &off);
    n = (int16_t)hle_u16(&a);
    GCHECK;
    if (n < 0 || (n && !(s = hle_ptr(c, sel, off, (uint32_t)n))) || !measure(g, h, s, n, &w, &hh)) {
        hle_ret32(c, 0);
        return CPU_OK;
    }
    hle_ret32(c, (uint32_t)(uint16_t)w | (uint32_t)(uint16_t)hh << 16);
    return CPU_OK;
}

static cpu_status g_gettextextentpoint(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off, osel, ooff;
    int n, w, hh;
    const uint8_t *s = NULL;
    uint8_t *o;
    hle_far(&a, &sel, &off);
    n = (int16_t)hle_u16(&a);
    hle_far(&a, &osel, &ooff);
    GCHECK;
    if (n < 0 || (n && !(s = hle_ptr(c, sel, off, (uint32_t)n))) || !measure(g, h, s, n, &w, &hh)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (!(o = hle_ptr(c, osel, ooff, 4))) return CPU_E_SEL;
    gst16(o, (uint16_t)w);
    gst16(o + 2, (uint16_t)hh);
    hle_ret16(c, 1);
    return CPU_OK;
}

static void fill_textmetric(const gdc *dc, const gfont *f, uint8_t *p) {
    memset(p, 0, 31);
    gst16(p, (uint16_t)height_logical(dc, f->height));
    gst16(p + 2, (uint16_t)height_logical(dc, f->ascent));
    gst16(p + 4, (uint16_t)height_logical(dc, f->descent));
    gst16(p + 6, (uint16_t)height_logical(dc, f->intlead));
    gst16(p + 8, 0);
    gst16(p + 10, (uint16_t)width_logical(dc, f->avgw));
    gst16(p + 12, (uint16_t)width_logical(dc, f->maxw));
    gst16(p + 14, (uint16_t)f->weight);
    p[16] = f->italic;
    p[17] = f->underline;
    p[18] = f->strikeout;
    p[19] = 0x20;
    p[20] = 0xFF;
    p[21] = 0x1F;
    p[22] = 0x20;
    p[23] = (uint8_t)((f->raw[17] & 0xF0) | (f->fixed ? 0 : 1) | (f->tt ? 6 : 0));   /* TMPF_VECTOR | TMPF_TRUETYPE */
    p[24] = f->raw[13];
    gst16(p + 25, (uint16_t)f->overhang);
    gst16(p + 27, 96);
    gst16(p + 29, 96);
}

static cpu_status g_gettextmetrics(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    gdc *dc;
    const gfont *f;
    uint8_t *p;
    hle_far(&a, &sel, &off);
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc || !font_for(g, dc, &f)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (!(p = hle_ptr(c, sel, off, 31))) return CPU_E_SEL;
    fill_textmetric(dc, f, p);
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status g_gettextface(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    int n;
    gdc *dc;
    const gfont *f;
    char face[33];
    uint8_t *p;
    n = (int16_t)hle_u16(&a);
    hle_far(&a, &sel, &off);
    (void)h;
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc || !font_for(g, dc, &f) || n <= 0) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    memcpy(face, f->raw + 18, 32);
    face[32] = 0;
    if ((int)strlen(face) > n - 1) face[n - 1] = 0;
    if (!(p = hle_ptr(c, sel, off, (uint32_t)strlen(face) + 1))) return CPU_E_SEL;
    memcpy(p, face, strlen(face) + 1);
    hle_ret16(c, (uint16_t)strlen(face));
    return CPU_OK;
}

static cpu_status g_getcharwidth(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), first = hle_u16(&a), last = hle_u16(&a), sel, off;
    gdc *dc;
    const gfont *f;
    unsigned ch;
    hle_far(&a, &sel, &off);
    GCHECK;
    dc = gdc_get(g, h);
    if (!dc || !font_for(g, dc, &f) || last < first || last > 255) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    for (ch = first; ch <= last; ch++) {
        uint8_t *p = hle_ptr(c, sel, (uint32_t)off + 2u * (ch - first), 2);
        if (!p) return CPU_E_SEL;
        gst16(p, (uint16_t)width_logical(dc, advance_of(f, (unsigned char)ch) + dc->cextra));
    }
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status g_getoutlinetextmetrics(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), cb = hle_u16(&a), sel, off;
    hle_far(&a, &sel, &off);
    (void)h; (void)cb;
    GCHECK;
    hle_ret32(c, 0);                              /* no TrueType outlines: the metrics cannot be given */
    return CPU_OK;
}

/* GetGlyphOutline serves GGO_METRICS (format 0, no buffer) only: the black box, origin and advance of one glyph of the
 * built-in font, as the pixels paint_glyph draws. There are no outlines or bitmaps to return (GDI_ERROR), but programs
 * that fit a letter to a box (Creative Writer's drop capitals) take their normal path with the metrics; without
 * them they render the glyph into a bitmap and measure it, a path that ends in an out-of-space alert. */
static cpu_status g_getglyphoutline(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), ch = hle_u16(&a), fmt = hle_u16(&a), gsel, goff, bsel, boff, msel, moff;
    uint32_t cb;
    gdc *dc;
    const gfont *f;
    uint8_t *p;
    hle_far(&a, &gsel, &goff);
    cb = hle_u32(&a);
    hle_far(&a, &bsel, &boff);
    hle_far(&a, &msel, &moff);
    (void)cb;
    GCHECK;
    dc = gdc_get(g, h);
    if (fmt != 0 || !dc || !font_for(g, dc, &f) || ch > 255 || !(p = hle_ptr(c, gsel, goff, 12))) {
        hle_ret32(c, 0xFFFFFFFFu);                /* GDI_ERROR */
        return CPU_OK;
    }
    {
        int blank, lb = 0, rb = 4, ncols, gw = 0, top = 0, bottom = 0, yo;
        const uint8_t *gl = glyph_of(cp1252_punct((unsigned char)ch), &blank);
        if (!blank && ch != 0x20) {
            int first = -1;
            if (!f->fixed) glyph_extent(gl, &lb, &rb);
            ncols = rb - lb + 1;
            gw = (int)floor(ncols * f->sx + 0.5);
            if (gw < 1) gw = 1;
            for (yo = 0; yo < f->height; yo++) {
                int row = yo * 8 / f->height, col, ink = 0;
                for (col = 0; col < 5; col++) ink |= (gl[col] >> row) & 1;
                if (!ink) continue;
                if (first < 0) first = yo;
                bottom = yo + 1;
            }
            top = first < 0 ? 0 : first;
            if (first < 0) bottom = 0;
        }
        gst16(p, (uint16_t)gw);
        gst16(p + 2, (uint16_t)(bottom - top));
        gst16(p + 4, (uint16_t)(gw ? lb * f->sx : 0));
        gst16(p + 6, (uint16_t)(gw ? f->ascent - top : 0));
        gst16(p + 8, (uint16_t)width_logical(dc, advance_of(f, (unsigned char)ch)));
        gst16(p + 10, 0);
    }
    hle_ret32(c, 0);
    return CPU_OK;
}

static cpu_status g_getrasterizercaps(hle_call *c) {
    GBEGIN;
    uint16_t sel, off, cb;
    uint8_t *p;
    hle_far(&a, &sel, &off);
    cb = hle_u16(&a);
    GCHECK;
    if (cb < 6) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (!(p = hle_ptr(c, sel, off, 6))) return CPU_E_SEL;
    gst16(p, 6);
    gst16(p + 2, 3);          /* TT_AVAILABLE | TT_ENABLED: programs take their normal path; no outlines are served */
    gst16(p + 4, 0x0409);
    hle_ret16(c, 1);
    return CPU_OK;
}

/* ---- DrawText ------------------------------------------------------------------------------------------ */

#define DT_CENTER 1
#define DT_RIGHT 2
#define DT_VCENTER 4
#define DT_BOTTOM 8
#define DT_WORDBREAK 0x10
#define DT_SINGLELINE 0x20
#define DT_EXPANDTABS 0x40
#define DT_TABSTOP 0x80
#define DT_NOCLIP 0x100
#define DT_CALCRECT 0x400
#define DT_NOPREFIX 0x800

typedef struct { int first, n, width; } dline;

static cpu_status g_drawtext(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off, rsel, roff, fmt;
    int count, i, nitems = 0, nl = 0, capl = 8, y, th, maxw = 0, rwidth, tabw;
    const uint8_t *s = NULL;
    uint8_t *rp;
    char *tmp = NULL;
    gctx x;
    const gfont *f;
    titem *it = NULL;
    dline *lines = NULL;
    fb_rect rc;
    int ret = 0;
    hle_far(&a, &sel, &off);
    count = (int16_t)hle_u16(&a);
    hle_far(&a, &rsel, &roff);
    fmt = hle_u16(&a);
    GCHECK;
    if (!(rp = hle_ptr(c, rsel, roff, 8))) return CPU_E_SEL;
    if (count < 0) {
        char buf[2048];
        if (!hle_read_cstr(c, sel, off, buf, sizeof buf)) return CPU_E_SEL;
        count = (int)strlen(buf);
        tmp = malloc((size_t)count + 1);
        if (!tmp) return CPU_E_UD;
        memcpy(tmp, buf, (size_t)count + 1);
        s = (const uint8_t *)tmp;
    } else if (count && !(s = hle_ptr(c, sel, off, (uint32_t)count))) {
        return CPU_E_SEL;
    }
    if (!gctx_open(g, h, &x) || !font_for(g, x.dc, &f)) {
        gctx_close(&x);
        free(tmp);
        hle_ret16(c, 0);
        return CPU_OK;
    }
    rc = dev_rect_of(x.dc, (int16_t)gld16(rp), (int16_t)gld16(rp + 2), (int16_t)gld16(rp + 4), (int16_t)gld16(rp + 6));
    rwidth = rc.right - rc.left;
    tabw = ((fmt & DT_TABSTOP) ? (fmt >> 8 & 0xFF) : 8) * f->avgw;
    if (tabw <= 0) tabw = 8 * f->avgw;
    it = malloc((size_t)(count + 1) * sizeof *it);
    lines = malloc((size_t)capl * sizeof *lines);
    if (!it || !lines) {
        free(it); free(lines); free(tmp);
        gctx_close(&x);
        return CPU_E_UD;
    }
    /* items: prefix handling, tabs, line breaks as ch == '\n' with skip */
    for (i = 0; i < count; i++) {
        unsigned char ch = s[i];
        titem t = {ch, false, false, 0};
        if (ch == '\r') continue;
        if (ch == '\n') {
            if (fmt & DT_SINGLELINE) continue;
            t.skip = true;
            it[nitems++] = t;
            continue;
        }
        if (ch == '&' && !(fmt & DT_NOPREFIX)) {
            if (i + 1 < count && s[i + 1] == '&') {
                i++;
            } else {
                if (i + 1 < count && s[i + 1] != '\r' && s[i + 1] != '\n') {
                    i++;
                    t.ch = s[i];
                    t.ul = true;
                } else {
                    continue;
                }
            }
        }
        t.adv = advance_of(f, t.ch) + x.dc->cextra;
        it[nitems++] = t;
    }
    /* lines */
    {
        int start = 0, lastbreak = -1, lastbreak_w = 0, wsum = 0, k;
        lines[0].first = 0;
        for (k = 0; k <= nitems; k++) {
            bool endline = k == nitems || (it[k].skip && it[k].ch == '\n');
            if (!endline) {
                if (it[k].ch == '\t' && (fmt & DT_EXPANDTABS) && !it[k].skip) {
                    int next = (wsum / tabw + 1) * tabw;
                    it[k].adv = next - wsum;
                }
                if ((fmt & DT_WORDBREAK) && !(fmt & DT_SINGLELINE) && wsum + it[k].adv > rwidth && k > start &&
                    it[k].ch != 0x20) {
                    int brk = lastbreak >= 0 ? lastbreak : k;
                    if (nl + 1 >= capl) {
                        dline *nlp = realloc(lines, (size_t)capl * 2 * sizeof *lines);
                        if (!nlp) break;
                        lines = nlp;
                        capl *= 2;
                    }
                    lines[nl].first = start;
                    lines[nl].n = brk - start;
                    lines[nl].width = lastbreak >= 0 ? lastbreak_w : wsum;
                    nl++;
                    start = brk;
                    while (start < nitems && it[start].ch == 0x20 && start < k) start++;   /* spaces at the break are dropped */
                    wsum = 0;
                    for (i = start; i < k; i++) wsum += it[i].adv;
                    lastbreak = -1;
                }
                wsum += it[k].adv;
                if (it[k].ch == 0x20 || it[k].ch == '\t') {
                    lastbreak = k + 1;
                    lastbreak_w = wsum - it[k].adv;
                }
                continue;
            }
            if (nl + 1 >= capl) {
                dline *nlp = realloc(lines, (size_t)capl * 2 * sizeof *lines);
                if (!nlp) break;
                lines = nlp;
                capl *= 2;
            }
            lines[nl].first = start;
            lines[nl].n = k - start;
            lines[nl].width = wsum;
            nl++;
            start = k + 1;
            wsum = 0;
            lastbreak = -1;
        }
    }
    for (i = 0; i < nl; i++) {
        /* trailing spaces are not part of a wrapped line's width */
        if (lines[i].width > maxw) maxw = lines[i].width;
    }
    th = nl * f->height;
    if (fmt & DT_CALCRECT) {
        int wl = width_logical(x.dc, (fmt & DT_WORDBREAK) ? rwidth : maxw);
        int l = (int16_t)gld16(rp), t = (int16_t)gld16(rp + 2);
        gst16(rp + 4, (uint16_t)(l + wl));
        gst16(rp + 6, (uint16_t)(t + height_logical(x.dc, th)));
        ret = height_logical(x.dc, th);
    } else {
        fb_rect dev = rc, clip;
        int yy;
        dev.left += x.ox; dev.right += x.ox; dev.top += x.oy; dev.bottom += x.oy;
        clip = dev;
        y = dev.top;
        if ((fmt & DT_SINGLELINE) && (fmt & DT_VCENTER)) y = dev.top + (dev.bottom - dev.top - th) / 2;
        else if ((fmt & DT_SINGLELINE) && (fmt & DT_BOTTOM)) y = dev.bottom - th;
        for (i = 0; i < nl; i++) {
            int lx = dev.left;
            if (fmt & DT_CENTER) lx = dev.left + (rwidth - lines[i].width) / 2;
            else if (fmt & DT_RIGHT) lx = dev.right - lines[i].width;
            yy = y + i * f->height;
            if (lines[i].n > 0) paint_items(&x, f, lx, yy, it + lines[i].first, lines[i].n, x.dc->bkmode == OPAQUE,
                                            (fmt & DT_NOCLIP) ? NULL : &clip);
        }
        ret = height_logical(x.dc, th);
    }
    free(it);
    free(lines);
    free(tmp);
    gctx_close(&x);
    hle_ret16(c, (uint16_t)ret);
    return CPU_OK;
}

/* ---- creation, stock fonts and enumeration ----------------------------------------------------------------- */

static void fill_logfont(uint8_t *raw, int h, int w, int weight, int pitch_family, const char *face) {
    memset(raw, 0, 50);
    gst16(raw, (uint16_t)h);
    gst16(raw + 2, (uint16_t)w);
    gst16(raw + 8, (uint16_t)weight);
    raw[17] = (uint8_t)pitch_family;
    strncpy((char *)raw + 18, face, 31);
}

static void bind_tt(gdi_t *g, gfont *f);

uint16_t gdi_font_stock(gdi_t *g, int idx) {
    gobj *o;
    uint16_t h = gobj_new(g, GO_FONT, &o);
    if (!h) return 0;
    switch (idx) {
    case 10: fill_logfont(o->u.font.raw, 16, 8, 400, 0x31, "Terminal"); break;      /* OEM_FIXED_FONT */
    case 11: fill_logfont(o->u.font.raw, 16, 8, 400, 0x31, "Courier"); break;       /* ANSI_FIXED_FONT */
    case 12: fill_logfont(o->u.font.raw, 13, 0, 400, 0x22, "MS Sans Serif"); break; /* ANSI_VAR_FONT */
    case 16: fill_logfont(o->u.font.raw, 16, 8, 400, 0x31, "System"); break;        /* SYSTEM_FIXED_FONT */
    default: fill_logfont(o->u.font.raw, 16, 8, 700, 0x22, "System"); break;        /* SYSTEM, DEVICE_DEFAULT */
    }
    gdi_font_resolve(&o->u.font);
    bind_tt(g, &o->u.font);
    return h;
}

/* A face the program added from a raster font file (AddFontResource of a .FON): the closest strike wins. */
static void bind_raster(gdi_t *g, gfont *f) {
    int h = (int16_t)gld16(f->raw), i;
    const char *face = (const char *)f->raw + 18;
    const fnt_strike *s = NULL;
    if (!face[0]) return;
    for (i = 0; i < g->nadded && !s; i++)
        if (g->added[i].fnt >= 0) s = fnt_pick(&g->fnts[g->added[i].fnt], face, h > 0 ? h : 0, h < 0 ? -h : 0);
    if (!s) return;
    f->bmp = s;
    f->height = s->height;
    f->ascent = s->ascent;
    f->descent = s->height - s->ascent;
    f->intlead = s->intlead;
    f->avgw = s->avgw;
    f->maxw = s->maxw;
    f->fixed = (s->pitch_family & 1) == 0;
    f->bold = f->weight >= 600 && s->weight < 600;       /* no bold strike: smear the regular one */
    f->overhang = f->bold ? 1 : 0;
    f->sx = 1.0;
    if (f->weight < s->weight) f->weight = s->weight;
}

/* A face no raster font covers: the TrueType face the font set maps it to (Liberation for Arial, MS Sans Serif, ...). */
static void bind_tt(gdi_t *g, gfont *f) {
    int h = (int16_t)gld16(f->raw);
    bool synth = false;
    const ttf_face *tf;
    ttf_inst *in;
    const ttf_metrics *m;
    if (!g->cfg.fonts || f->bmp) return;
    tf = ttf_set_pick(g->cfg.fonts, (const char *)f->raw + 18, f->weight, f->raw[17], &synth);
    if (!tf) return;
    in = ttf_set_inst(g->cfg.fonts, tf, h > 0 ? h : 0, h < 0 ? -h : 0);
    if (!in) return;
    m = ttf_inst_metrics(in);
    f->tt = in;
    f->height = m->height;
    f->ascent = m->ascent;
    f->descent = m->descent;
    f->intlead = m->intlead;
    f->avgw = m->avgw;
    f->maxw = m->maxw;
    f->fixed = ttf_inst_advance(in, 'i') == ttf_inst_advance(in, 'W');
    f->bold = synth;
    f->overhang = synth ? 1 : 0;
    f->sx = 1.0;
}

static uint16_t font_from_raw(gdi_t *g, const uint8_t *raw) {
    gobj *o;
    uint16_t h = gobj_new(g, GO_FONT, &o);
    if (!h) return 0;
    memcpy(o->u.font.raw, raw, 50);
    o->u.font.raw[49] = 0;
    gdi_font_resolve(&o->u.font);
    bind_raster(g, &o->u.font);
    bind_tt(g, &o->u.font);
    if (getenv("CWFA_FONT_LOG")) fprintf(stderr, "font: '%s' h=%d w=%d wt=%d -> %s\n", (const char *)raw + 18, (int)(int16_t)gld16(raw), (int)(int16_t)gld16(raw + 2), o->u.font.weight, o->u.font.tt ? "ttf" : o->u.font.bmp ? "raster" : "5x7");
    return h;
}

static cpu_status g_createfontindirect(hle_call *c) {
    GBEGIN;
    uint16_t sel, off;
    const uint8_t *p;
    hle_far(&a, &sel, &off);
    GCHECK;
    if (!(p = hle_ptr(c, sel, off, 50))) return CPU_E_SEL;
    hle_ret16(c, font_from_raw(g, p));
    return CPU_OK;
}

/* CreateFont(height, width, escapement, orientation, weight, italic, underline,
 * strikeout, charset, outprecision, clipprecision, quality, pitchandfamily, face):
 * the BYTE parameters are pushed as words. */
static cpu_status g_createfont(hle_call *c) {
    GBEGIN;
    uint8_t raw[50];
    uint16_t v[13], sel, off;
    int i;
    for (i = 0; i < 13; i++) v[i] = hle_u16(&a);
    hle_far(&a, &sel, &off);
    GCHECK;
    memset(raw, 0, sizeof raw);
    for (i = 0; i < 5; i++) gst16(raw + 2 * i, v[i]);
    for (i = 5; i < 13; i++) raw[5 + i] = (uint8_t)v[i];     /* italic .. pitchandfamily at offsets 10..17 */
    if (sel) hle_read_cstr(c, sel, off, (char *)raw + 18, 32);
    hle_ret16(c, font_from_raw(g, raw));
    return CPU_OK;
}

typedef struct { const char *face; int pitch_family, charset; bool truetype; const int *sizes; } family;
static const int sizes_sys[] = {16, 0};
static const int sizes_small[] = {8, 10, 13, 16, 20, 0};
static const int sizes_tt[] = {0};
static const family families[] = {
    {"System", 0x22, 0, false, sizes_sys},       {"Terminal", 0x31, 255, false, sizes_sys},
    {"Fixedsys", 0x31, 0, false, sizes_sys},     {"Courier", 0x31, 0, false, sizes_small},
    {"MS Sans Serif", 0x22, 0, false, sizes_small}, {"MS Serif", 0x12, 0, false, sizes_small},
    {"Symbol", 0x12, 2, false, sizes_small},     {"Arial", 0x22, 0, true, sizes_tt},
    {"Times New Roman", 0x12, 0, true, sizes_tt}, {"Courier New", 0x31, 0, true, sizes_tt}};
#define NFAMILIES (sizeof families / sizeof families[0])

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

/* family name of a font file: from its base name; the CD's TrueType files are named after the face */
static void family_from_path(const char *path, char *out, size_t cap) {
    static const struct { const char *stem, *name; } known[] = {
        {"IMPACT", "Impact"}, {"LBLACK", "Lucida Blackletter"}, {"LHANDW", "Lucida Handwriting"},
        {"LSANS", "Lucida Sans"}, {"MCZEE", "McZee"}, {"STENCIL", "Stencil"}, {"PABLO", "Pablo"}};
    const char *base = path, *p;
    char stem[16];
    size_t i = 0, k;
    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
    while (base[i] && base[i] != '.' && i < sizeof stem - 1) {
        stem[i] = (char)toupper((unsigned char)base[i]);
        i++;
    }
    stem[i] = 0;
    for (k = 0; k < sizeof known / sizeof known[0]; k++)
        if (!strcmp(stem, known[k].stem)) {
            snprintf(out, cap, "%s", known[k].name);
            return;
        }
    snprintf(out, cap, "%s", stem);
    for (k = 1; out[k]; k++) out[k] = (char)tolower((unsigned char)out[k]);
}

static cpu_status g_addfontresource(hle_call *c) {
    GBEGIN;
    uint16_t sel, off;
    char path[128], name[32];
    int i;
    hle_far(&a, &sel, &off);
    GCHECK;
    if (!hle_read_cstr(c, sel, off, path, sizeof path)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    family_from_path(path, name, sizeof name);
    for (i = 0; i < g->nadded; i++)
        if (ieq(g->added[i].name, name)) {
            g->added[i].refs++;
            hle_ret16(c, 1);
            return CPU_OK;
        }
    if (g->nadded >= GDI_MAX_ADDED) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    snprintf(g->added[g->nadded].name, sizeof g->added[0].name, "%s", name);
    snprintf(g->added[g->nadded].path, sizeof g->added[0].path, "%s", path);
    g->added[g->nadded].refs = 1;
    g->added[g->nadded].fnt = -1;
    {
        char host[512];
        if (g->cfg.host_path && g->nfnts < (int)(sizeof g->fnts / sizeof g->fnts[0]) &&
            g->cfg.host_path(g->cfg.host_ctx, path, host, sizeof host) && fnt_load_file(host, &g->fnts[g->nfnts])) {
            g->added[g->nadded].fnt = g->nfnts++;
            if (g->fnts[g->added[g->nadded].fnt].n && g->fnts[g->added[g->nadded].fnt].s[0].face[0])   /* enumerate under the face name */
                snprintf(g->added[g->nadded].name, sizeof g->added[0].name, "%s", g->fnts[g->added[g->nadded].fnt].s[0].face);
        } else if (g->cfg.fonts && g->cfg.host_path && g->cfg.host_path(g->cfg.host_ctx, path, host, sizeof host)) {
            ttf_face *tf = ttf_face_from_file(host);      /* a TrueType file: its glyphs under the name the program uses */
            if (tf) {
                const char *fam = ttf_face_family(tf);
                if (!ttf_set_add_face(g->cfg.fonts, g->added[g->nadded].name, tf)) ttf_face_free(tf);
                else if (fam[0] && !ieq(fam, g->added[g->nadded].name)) ttf_set_alias(g->cfg.fonts, fam, tf);
            }
        }
    }
    g->nadded++;
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status g_removefontresource(hle_call *c) {
    GBEGIN;
    uint16_t sel, off;
    char path[128], name[32];
    int i;
    hle_far(&a, &sel, &off);
    GCHECK;
    if (!hle_read_cstr(c, sel, off, path, sizeof path)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    family_from_path(path, name, sizeof name);
    for (i = 0; i < g->nadded; i++)
        if (ieq(g->added[i].name, name)) {
            if (--g->added[i].refs <= 0) {
                memmove(&g->added[i], &g->added[i + 1], (size_t)(g->nadded - i - 1) * sizeof g->added[0]);
                g->nadded--;
            }
            hle_ret16(c, 1);
            return CPU_OK;
        }
    hle_ret16(c, 0);
    return CPU_OK;
}

/* One callback: LOGFONT and TEXTMETRIC in the guest scratch segment, then the program's function. */
static bool enum_call(gdi_t *g, uint32_t proc, uint32_t data, const char *face, int pitch_family, int charset, int height,
                      int fonttype, bool newstyle, uint32_t *ret) {
    uint8_t *lf = mem_ptr(&g->p->mem, g->scratch_sel, 0, 50 + 31 + 64);
    gfont tmp;
    uint16_t args[7];
    unsigned n = 0;
    if (!lf) return false;
    fill_logfont(lf, height, 0, 400, pitch_family, face);
    lf[13] = (uint8_t)charset;
    memset(&tmp, 0, sizeof tmp);
    memcpy(tmp.raw, lf, 50);
    gdi_font_resolve(&tmp);
    {
        gdc d;
        memset(&d, 0, sizeof d);
        d.wex = d.wey = d.vex = d.vey = 1;
        fill_textmetric(&d, &tmp, lf + 64);
    }
    args[n++] = g->scratch_sel;                      /* LPLOGFONT */
    args[n++] = 0;
    args[n++] = g->scratch_sel;                      /* LPTEXTMETRIC */
    args[n++] = 64;
    args[n++] = (uint16_t)fonttype;
    args[n++] = (uint16_t)(data >> 16);              /* lParam / lpData: four bytes either way */
    args[n++] = (uint16_t)data;
    (void)newstyle;
    return user_call_guest(g->user, proc, args, n, ret);
}

/* EnumFontFamilies(hdc, lpszFamily, lpEnumFontFamProc, lParam) / EnumFonts(hdc, lpszFace, lpEnumFontsProc, lpData):
 * the same four arguments in the same order. */
static cpu_status enum_fonts(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), fsel, foff;
    uint32_t proc, data, r = 1;
    char face[40] = "";
    bool have_face;
    size_t i;
    int k, ncalls = 0;
    hle_far(&a, &fsel, &foff);
    proc = hle_u32(&a);
    data = hle_u32(&a);
    GCHECK;
    (void)h;
    have_face = fsel != 0 || foff != 0;
    if (have_face && !hle_read_cstr(c, fsel, foff, face, sizeof face)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (!g->scratch_sel) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    for (i = 0; i < NFAMILIES && r; i++) {
        const family *fm = &families[i];
        if (have_face && !ieq(face, fm->face)) continue;
        if (!have_face || fm->truetype) {                       /* one entry per family, or the one scalable face */
            if (!enum_call(g, proc, data, fm->face, fm->pitch_family, fm->charset, fm->sizes[0] ? fm->sizes[0] : -16,
                           fm->truetype ? 4 : 1, true, &r))
                return user_abort(g->user);
            ncalls++;
        } else {                                                /* a raster family by name: every size */
            for (k = 0; fm->sizes[k] && r; k++) {
                if (!enum_call(g, proc, data, fm->face, fm->pitch_family, fm->charset, fm->sizes[k], 1, true, &r))
                    return user_abort(g->user);
                ncalls++;
            }
        }
    }
    for (k = 0; k < g->nadded && r; k++) {
        if (have_face && !ieq(face, g->added[k].name)) continue;
        if (!enum_call(g, proc, data, g->added[k].name, 0x22, 0, -16, 4, true, &r)) return user_abort(g->user);
        ncalls++;
    }
    hle_ret16(c, ncalls ? (uint16_t)r : 0);
    return CPU_OK;
}

#define G GDI_MODULE
#define U USER_MODULE_NAME
const hle_def gdi_font_defs[] = {
    {G, 33, "TextOut", 12, g_textout},
    {G, 56, "CreateFont", 30, g_createfont},
    {G, 57, "CreateFontIndirect", 4, g_createfontindirect},
    {G, 70, "EnumFonts", 14, enum_fonts},                       /* UNV */
    {G, 91, "GetTextExtent", 8, g_gettextextent},
    {G, 92, "GetTextFace", 8, g_gettextface},                    /* UNV */
    {G, 93, "GetTextMetrics", 6, g_gettextmetrics},
    {G, 119, "AddFontResource", 4, g_addfontresource},
    {G, 136, "RemoveFontResource", 4, g_removefontresource},
    {G, 308, "GetOutlineTextMetrics", 8, g_getoutlinetextmetrics},
    {G, 309, "GetGlyphOutline", 22, g_getglyphoutline},
    {G, 313, "GetRasterizerCaps", 6, g_getrasterizercaps},
    {G, 330, "EnumFontFamilies", 14, enum_fonts},
    {G, 350, "GetCharWidth", 10, g_getcharwidth},
    {G, 351, "ExtTextOut", 22, g_exttextout},
    {G, 471, "GetTextExtentPoint", 12, g_gettextextentpoint},
    {U, 85, "DrawText", 14, g_drawtext},
};
#undef G
#undef U
const size_t gdi_font_ndefs = sizeof gdi_font_defs / sizeof gdi_font_defs[0];

/* ---- text for the built-in UI (user_gfx): explicit colours, the system font or a regular cell height ---- */

/* cell 0: the (bold) system font; 13: the stock ANSI_VAR_FONT (MS Sans Serif 8, base units 6x13); else a made one. */
static uint16_t ui_font_handle(gdi_t *g, int cell) {
    int i;
    if (cell & 0x10000) {                                 /* a font the program gave the control with WM_SETFONT */
        if (gobj_typed(g, (uint16_t)cell, GO_FONT)) return (uint16_t)cell;
        cell = 0;                                         /* deleted since: the system font */
    }
    if (cell <= 0) return g->stock[13];
    if (cell == 13) return g->stock[12];
    for (i = 0; i < g->nuifont; i++)
        if (g->uifont[i].cell == cell) return g->uifont[i].h;
    if (g->nuifont < (int)(sizeof g->uifont / sizeof g->uifont[0])) {
        uint8_t raw[50];
        uint16_t h;
        fill_logfont(raw, cell, 0, 400, 0x22, "MS Sans Serif");
        if ((h = font_from_raw(g, raw))) {
            gobj_get(g, h)->stock = true;
            g->uifont[g->nuifont].cell = cell;
            g->uifont[g->nuifont++].h = h;
            return h;
        }
    }
    return g->stock[12];
}

int gdi_ui_text_width(gdi_t *g, const char *s, int n, int cell) {
    gobj *o = gobj_typed(g, ui_font_handle(g, cell), GO_FONT);
    int i, total = 0;
    if (!o) return 8 * n;
    for (i = 0; i < n; i++) total += advance_of(&o->u.font, (unsigned char)s[i]);
    return total;
}

int gdi_ui_text_height(gdi_t *g, int cell) {
    gobj *o = gobj_typed(g, ui_font_handle(g, cell), GO_FONT);
    return o ? o->u.font.height : 16;
}

void gdi_ui_font_metrics(gdi_t *g, int cell, int *avgw, int *height) {
    gobj *o = gobj_typed(g, ui_font_handle(g, cell), GO_FONT);
    *avgw = o ? o->u.font.avgw : 8;
    *height = o ? o->u.font.height : 16;
}

void gdi_ui_text(gdi_t *g, uint16_t hdc, int x, int y, const char *s, int n, uint32_t fg, uint32_t bg, int cell) {
    gdc *dc = gdc_get(g, hdc);
    uint32_t ofg, obk;
    int obkmode, otalign;
    uint16_t ofont;
    if (!dc || n <= 0) return;
    ofg = dc->fg;
    obk = dc->bk;
    obkmode = dc->bkmode;
    otalign = dc->talign;
    ofont = dc->font;
    dc->fg = fg;
    dc->bk = bg == 0xFFFFFFFFu ? obk : bg;
    dc->bkmode = bg == 0xFFFFFFFFu ? TRANSPARENT : OPAQUE;
    dc->talign = TA_LEFT;
    dc->font = ui_font_handle(g, cell);
    text_core(g, hdc, x, y, 0, NULL, (const uint8_t *)s, n, NULL);
    if ((dc = gdc_get(g, hdc))) {
        dc->fg = ofg;
        dc->bk = obk;
        dc->bkmode = obkmode;
        dc->talign = otalign;
        dc->font = ofont;
    }
}
