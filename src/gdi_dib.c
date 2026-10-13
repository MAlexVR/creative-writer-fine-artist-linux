/* Device independent bitmaps: SetDIBits, GetDIBits, CreateDIBitmap, StretchDIBits,
 * SetDIBitsToDevice and DIB pattern brushes.
 *
 * Supported: BITMAPINFOHEADER (and bigger headers, read as one) and the old
 * BITMAPCOREHEADER, 1, 4, 8 and 24 bits per pixel, BI_RGB, BI_RLE8 and BI_RLE4,
 * bottom-up and top-down (negative height) rows, RGBQUAD / RGBTRIPLE colour
 * tables and DIB_PAL_COLORS (WORD indices into the DC's logical palette). Not
 * supported: 16 and 32 bits, BI_BITFIELDS. Colours reach the 8-bit framebuffer
 * through the DC's palette (see gctx_pixel): exact matches first, else the
 * nearest system colour; nothing is dithered. */
#include "gdi_priv.h"

#define BI_RGB 0
#define BI_RLE8 1
#define BI_RLE4 2
#define DIB_RGB_COLORS 0
#define DIB_PAL_COLORS 1
#define CBM_INIT 4

typedef struct {
    int w, h;            /* image size, h positive */
    bool topdown;
    int bpp, comp, ncol, usage;
    uint32_t hdr, stride, imgsize;
    uint32_t tab[256];   /* COLORREF per table entry (DIB_RGB_COLORS) */
    uint16_t pal[256];   /* logical palette index per entry (DIB_PAL_COLORS) */
} dibinfo;

static const uint32_t ega16[16] = {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
                                   0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};

static bool guest_read(hle_call *c, uint16_t sel0, uint32_t off0, uint8_t *dst, uint32_t len) {
    uint32_t done = 0;
    while (done < len) {
        uint32_t pos = off0 + done, o = pos & 0xFFFF, chunk = 0x10000 - o;
        uint16_t sel = mem_huge_sel(sel0, pos >> 16);
        const uint8_t *p;
        if (chunk > len - done) chunk = len - done;
        if (!(p = hle_ptr(c, sel, o, chunk))) return false;
        memcpy(dst + done, p, chunk);
        done += chunk;
    }
    return true;
}

static bool guest_write(hle_call *c, uint16_t sel0, uint32_t off0, const uint8_t *src, uint32_t len) {
    uint32_t done = 0;
    while (done < len) {
        uint32_t pos = off0 + done, o = pos & 0xFFFF, chunk = 0x10000 - o;
        uint16_t sel = mem_huge_sel(sel0, pos >> 16);
        uint8_t *p;
        if (chunk > len - done) chunk = len - done;
        if (!(p = hle_ptr(c, sel, o, chunk))) return false;
        memcpy(p, src + done, chunk);
        done += chunk;
    }
    return true;
}

static uint32_t rgbq(const uint8_t *q) { return (uint32_t)q[2] | (uint32_t)q[1] << 8 | (uint32_t)q[0] << 16; }

/* Reads the header and colour table at sel:off. Returns 0 ok, 1 bad format, 2 guest fault. */
static int dib_parse(hle_call *c, uint16_t sel, uint16_t off, int usage, dibinfo *d) {
    const uint8_t *p = hle_ptr(c, sel, off, 12);
    uint32_t size, entsz, clrused = 0, tabbytes;
    int i;
    memset(d, 0, sizeof *d);
    if (!p) return 2;
    size = gld32(p);
    d->usage = usage;
    if (size == 12) {
        d->w = gld16(p + 4);
        d->h = gld16(p + 6);
        d->bpp = gld16(p + 10);
        entsz = 3;
    } else if (size >= 40 && size <= 256) {
        if (!(p = hle_ptr(c, sel, off, 40))) return 2;
        d->w = (int32_t)gld32(p + 4);
        d->h = (int32_t)gld32(p + 8);
        d->bpp = gld16(p + 14);
        d->comp = (int)gld32(p + 16);
        d->imgsize = gld32(p + 20);
        clrused = gld32(p + 32);
        entsz = 4;
    } else {
        return 1;
    }
    d->hdr = size;
    if (d->h < 0) {
        d->topdown = true;
        d->h = -d->h;
    }
    if (d->w <= 0 || d->h <= 0 || d->w > 8192 || d->h > 8192) return 1;
    if (d->bpp != 1 && d->bpp != 4 && d->bpp != 8 && d->bpp != 24) return 1;
    if (d->comp != BI_RGB && !((d->comp == BI_RLE8 && d->bpp == 8) || (d->comp == BI_RLE4 && d->bpp == 4))) return 1;
    if (d->comp != BI_RGB && d->topdown) return 1;
    d->ncol = d->bpp <= 8 ? (int)(clrused ? clrused : 1u << d->bpp) : 0;
    if (d->ncol > (1 << d->bpp) && d->bpp <= 8) d->ncol = 1 << d->bpp;
    if (d->ncol > 256) d->ncol = 256;
    d->stride = ((uint32_t)d->w * (uint32_t)d->bpp + 31) / 32 * 4;
    tabbytes = (uint32_t)d->ncol * (usage == DIB_PAL_COLORS ? 2 : entsz);
    if (tabbytes) {
        if (!(p = hle_ptr(c, sel, (uint32_t)off + size, tabbytes))) return 2;
        for (i = 0; i < d->ncol; i++) {
            if (usage == DIB_PAL_COLORS) d->pal[i] = gld16(p + 2 * i);
            else if (entsz == 4) d->tab[i] = rgbq(p + 4 * i);
            else d->tab[i] = (uint32_t)p[3 * i + 2] | (uint32_t)p[3 * i + 1] << 8 | (uint32_t)p[3 * i] << 16;
        }
    }
    return 0;
}

static uint32_t dib_bits_offset(const dibinfo *d) {
    return d->hdr + (uint32_t)d->ncol * (d->usage == DIB_PAL_COLORS ? 2u : d->hdr == 12 ? 3u : 4u);
}

/* Decodes the encoded stream into palette indices, bottom row first (file order). */
static bool rle_decode(const dibinfo *d, const uint8_t *s, uint32_t n, uint8_t *idx) {
    uint32_t i = 0;
    int x = 0, y = 0;
    memset(idx, 0, (size_t)d->w * (size_t)d->h);
#define PUT(v) do { if (x < d->w && y < d->h) idx[(size_t)y * d->w + x] = (uint8_t)(v); x++; } while (0)
    while (i + 1 < n) {
        unsigned count = s[i], val = s[i + 1];
        i += 2;
        if (count) {
            unsigned k;
            for (k = 0; k < count; k++) PUT(d->comp == BI_RLE8 ? val : (k & 1 ? val & 15 : val >> 4));
            continue;
        }
        if (val == 0) {
            x = 0;
            y++;
        } else if (val == 1) {
            return true;
        } else if (val == 2) {
            if (i + 1 >= n) return false;
            x += s[i];
            y += s[i + 1];
            i += 2;
        } else {
            unsigned k, bytes = d->comp == BI_RLE8 ? val : (val + 1) / 2;
            if (i + bytes > n) return false;
            for (k = 0; k < val; k++) {
                if (d->comp == BI_RLE8) PUT(s[i + k]);
                else PUT(k & 1 ? s[i + k / 2] & 15 : s[i + k / 2] >> 4);
            }
            i += bytes;
            if (bytes & 1) i++;
        }
    }
#undef PUT
    return true;
}

/* Colour of every pixel, top-down: COLORREF or GCREF_INDEX | palette index. rows_ok[y] marks rows with data. */
static bool dib_colors(const dibinfo *d, const uint8_t *bits, uint32_t len, uint32_t *out, uint8_t *rows_ok, int first_line,
                       int nlines) {
    int x, y;
    uint8_t *idx = NULL;
    if (d->comp != BI_RGB) {
        idx = malloc((size_t)d->w * (size_t)d->h);
        if (!idx || !rle_decode(d, bits, len, idx)) {
            free(idx);
            return false;
        }
    }
    for (y = 0; y < d->h; y++) {
        int file_row = d->topdown ? y : d->h - 1 - y;      /* scan line numbering: 0 is the bottom unless top-down */
        const uint8_t *row;
        rows_ok[y] = 0;
        if (file_row < first_line || file_row >= first_line + nlines) continue;
        if (!idx && (uint64_t)(file_row - first_line + 1) * d->stride > len) continue;
        rows_ok[y] = 1;
        row = bits + (size_t)(file_row - first_line) * d->stride;
        for (x = 0; x < d->w; x++) {
            uint32_t v;
            if (d->bpp == 24) {
                v = (uint32_t)row[3 * x + 2] | (uint32_t)row[3 * x + 1] << 8 | (uint32_t)row[3 * x] << 16;
            } else {
                unsigned ix;
                if (idx) ix = idx[(size_t)file_row * d->w + x];
                else if (d->bpp == 8) ix = row[x];
                else if (d->bpp == 4) ix = (x & 1) ? row[x / 2] & 15 : row[x / 2] >> 4;
                else ix = (row[x / 8] >> (7 - (x & 7))) & 1;
                if (d->usage == DIB_PAL_COLORS) v = GCREF_INDEX | d->pal[ix < 256 ? ix : 0];
                else v = ix < (unsigned)d->ncol ? d->tab[ix] : 0;
            }
            out[(size_t)y * d->w + x] = v;
        }
    }
    free(idx);
    return true;
}

static bool dib_load(hle_call *c, uint16_t sel, uint32_t off, const dibinfo *d, uint32_t want, uint8_t **buf, uint32_t *len) {
    uint32_t n = want;
    *buf = NULL;
    *len = 0;
    if (d->comp != BI_RGB) n = d->imgsize ? d->imgsize : 0x10000u - (off & 0xFFFF);
    if (n > 64u * 1024 * 1024) return false;
    *buf = malloc(n ? n : 1);
    if (!*buf) return false;
    if (n && !guest_read(c, sel, off, *buf, n)) {
        free(*buf);
        *buf = NULL;
        return false;
    }
    *len = n;
    return true;
}

/* device pixels of the decoded colours; `ref` supplies the palette and the target type */
static uint8_t *to_device(const gctx *ref, bool mono, const uint32_t *colors, size_t n) {
    uint8_t *out = malloc(n ? n : 1);
    gctx tmp = *ref;
    size_t i;
    uint32_t last = 0xFFFFFFFFu;
    uint8_t lastp = 0;
    if (!out) return NULL;
    tmp.mono = mono;
    for (i = 0; i < n; i++) {
        if (colors[i] != last || i == 0) {
            last = colors[i];
            lastp = gctx_pixel(&tmp, last);
        }
        out[i] = lastp;
    }
    return out;
}

/* a reference gctx for conversions that need no surface (bitmaps selected nowhere) */
static bool dc_ref(gdi_t *g, uint16_t hdc, gctx *x) {
    memset(x, 0, sizeof *x);
    grgn_init(&x->clipr);
    x->g = g;
    x->dc = gdc_get(g, hdc);
    return x->dc != NULL;
}

/* ---- SetDIBits / CreateDIBitmap -------------------------------------------------------------------------- */

/* Copies decoded DIB lines into a bitmap. line0: bottom-up number of the first buffer line. */
static int store_lines(gdi_t *g, gobj *bo, const dibinfo *d, const uint8_t *dev, const uint8_t *rows_ok, int first_line, int nlines) {
    int y, copied = 0, w = d->w < bo->u.bmp.w ? d->w : bo->u.bmp.w;
    for (y = 0; y < d->h; y++) {
        int file_row = d->topdown ? y : d->h - 1 - y, by;
        if (!rows_ok[y]) continue;
        if (file_row < first_line || file_row >= first_line + nlines) continue;
        by = d->topdown ? file_row : bo->u.bmp.h - 1 - file_row;
        if (by < 0 || by >= bo->u.bmp.h) continue;
        memcpy(bo->u.bmp.bits + (size_t)by * bo->u.bmp.w, dev + (size_t)y * d->w, (size_t)w);
        copied++;
    }
    (void)g;
    return copied;
}

static int set_dibits(hle_call *c, gdi_t *g, uint16_t hdc, gobj *bo, int start, int count, uint16_t bsel, uint16_t boff,
                      uint16_t isel, uint16_t ioff, int usage, cpu_status *st) {
    dibinfo d;
    gctx ref;
    uint8_t *bits = NULL, *rows_ok = NULL, *dev = NULL;
    uint32_t *colors = NULL, len = 0;
    int r, copied = 0;
    *st = CPU_OK;
    if ((r = dib_parse(c, isel, ioff, usage, &d))) {
        if (r == 2) *st = CPU_E_SEL;
        return 0;
    }
    if (!dc_ref(g, hdc, &ref)) {
        gctx_close(&ref);
        return 0;
    }
    if (count > d.h) count = d.h;
    if (!dib_load(c, bsel, boff, &d, d.stride * (uint32_t)(count > 0 ? count : 0), &bits, &len)) {
        *st = bits ? CPU_OK : CPU_E_SEL;
        gctx_close(&ref);
        return 0;
    }
    colors = malloc((size_t)d.w * (size_t)d.h * sizeof *colors);
    rows_ok = malloc((size_t)d.h);
    if (colors && rows_ok && dib_colors(&d, bits, len, colors, rows_ok, d.comp == BI_RGB ? start : 0, d.comp == BI_RGB ? count : d.h)) {
        dev = to_device(&ref, bo->u.bmp.bpp == 1, colors, (size_t)d.w * (size_t)d.h);
        if (dev) copied = store_lines(g, bo, &d, dev, rows_ok, d.comp == BI_RGB ? start : 0, d.comp == BI_RGB ? count : d.h);
    }
    free(bits); free(colors); free(rows_ok); free(dev);
    gctx_close(&ref);
    return copied;
}

static cpu_status g_setdibits(hle_call *c) {
    GBEGIN;
    uint16_t hdc = hle_u16(&a), hbm = hle_u16(&a), bsel, boff, isel, ioff, usage;
    int start = hle_u16(&a), count = hle_u16(&a), n;
    gobj *bo;
    cpu_status st;
    hle_far(&a, &bsel, &boff);
    hle_far(&a, &isel, &ioff);
    usage = hle_u16(&a);
    GCHECK;
    bo = gobj_typed(g, hbm, GO_BITMAP);
    if (!bo || usage > 1) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    n = set_dibits(c, g, hdc, bo, start, count, bsel, boff, isel, ioff, usage, &st);
    if (st) return st;
    g->fb->dirty++;
    hle_ret16(c, (uint16_t)n);
    return CPU_OK;
}

static cpu_status g_createdibitmap(hle_call *c) {
    GBEGIN;
    uint16_t hdc = hle_u16(&a), hsel, hoff, bsel, boff, isel, ioff, usage, hb;
    uint32_t init;
    dibinfo d;
    gobj *bo;
    int r;
    cpu_status st;
    hle_far(&a, &hsel, &hoff);
    init = hle_u32(&a);
    hle_far(&a, &bsel, &boff);
    hle_far(&a, &isel, &ioff);
    usage = hle_u16(&a);
    GCHECK;
    if ((r = dib_parse(c, hsel, hoff, DIB_RGB_COLORS, &d))) {
        if (r == 2) return CPU_E_SEL;
        hle_ret16(c, 0);
        return CPU_OK;
    }
    hb = gdi_bitmap_create(g, d.w, d.h, d.bpp == 1 ? 1 : 8, &bo);
    if (!hb) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if ((init & CBM_INIT) && (bsel || boff) && (isel || ioff)) {
        set_dibits(c, g, hdc, bo, 0, d.h, bsel, boff, isel, ioff, usage, &st);
        if (st) {
            gobj_release(g, bo);
            return st;
        }
    }
    hle_ret16(c, hb);
    return CPU_OK;
}

/* ---- GetDIBits ------------------------------------------------------------------------------------------------ */

static uint32_t bitmap_color(const gdi_t *g, const gbitmap *b, uint8_t px) {
    uint32_t v;
    if (b->bpp == 1) return px ? 0xFFFFFF : 0;
    v = g->fb->pal[px];
    return (v & 0xFF) << 16 | (v & 0xFF00) | (v >> 16 & 0xFF);
}

static int nearest_ega(uint32_t cref) {
    int i, best = 0;
    long bd = 1L << 30;
    for (i = 0; i < 16; i++) {
        long dr = (long)(cref & 0xFF) - (long)(ega16[i] & 0xFF), dg = (long)(cref >> 8 & 0xFF) - (long)(ega16[i] >> 8 & 0xFF),
             db = (long)(cref >> 16 & 0xFF) - (long)(ega16[i] >> 16 & 0xFF);
        long d2 = dr * dr + dg * dg + db * db;
        if (d2 < bd) {
            bd = d2;
            best = i;
        }
    }
    return best;
}

static cpu_status g_getdibits(hle_call *c) {
    GBEGIN;
    uint16_t hdc = hle_u16(&a), hbm = hle_u16(&a), bsel, boff, isel, ioff, usage;
    int start = hle_u16(&a), count = hle_u16(&a), bpp, w, h, i, x, y, ncol = 0;
    uint8_t *hd, *out = NULL;
    uint32_t stride, tab[256], hdr, entsz = 4;
    gobj *bo;
    gbitmap *b;
    hle_far(&a, &bsel, &boff);
    hle_far(&a, &isel, &ioff);
    usage = hle_u16(&a);
    (void)hdc;
    GCHECK;
    bo = gobj_typed(g, hbm, GO_BITMAP);
    if (!bo || usage > 1 || !(hd = hle_ptr(c, isel, ioff, 12))) {
        if (!bo || usage > 1) {
            hle_ret16(c, 0);
            return CPU_OK;
        }
        return CPU_E_SEL;
    }
    b = &bo->u.bmp;
    hdr = gld32(hd);
    if (hdr != 12 && (hdr < 40 || hdr > 256)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (!(hd = hle_ptr(c, isel, ioff, hdr == 12 ? 12 : 40))) return CPU_E_SEL;
    bpp = hdr == 12 ? gld16(hd + 10) : gld16(hd + 14);
    if (bpp == 0) bpp = b->bpp == 1 ? 1 : 8;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    w = b->w;
    h = b->h;
    stride = ((uint32_t)w * (uint32_t)bpp + 31) / 32 * 4;
    if (bpp <= 8) {
        ncol = 1 << bpp;
        if (bpp == 8 && b->bpp == 1) ncol = 2;
        if (bpp == 1) ncol = 2;
        if (bpp == 4) {
            for (i = 0; i < 16; i++) tab[i] = ega16[i];
        } else if (b->bpp == 1) {
            tab[0] = 0;
            tab[1] = 0xFFFFFF;
        } else if (bpp == 8) {
            for (i = 0; i < 256; i++) tab[i] = bitmap_color(g, b, (uint8_t)i);
        } else {
            tab[0] = 0;
            tab[1] = 0xFFFFFF;
        }
    }
    entsz = hdr == 12 ? 3 : 4;
    /* header fields */
    if (hdr == 12) {
        gst16(hd + 4, (uint16_t)w);
        gst16(hd + 6, (uint16_t)h);
        gst16(hd + 8, 1);
        gst16(hd + 10, (uint16_t)bpp);
    } else {
        gst32(hd + 4, (uint32_t)w);
        gst32(hd + 8, (uint32_t)h);
        gst16(hd + 12, 1);
        gst16(hd + 14, (uint16_t)bpp);
        gst32(hd + 16, BI_RGB);
        gst32(hd + 20, stride * (uint32_t)h);
        gst32(hd + 32, (uint32_t)ncol);
        gst32(hd + 36, 0);
    }
    if (ncol && usage == DIB_RGB_COLORS) {
        uint8_t *tp = hle_ptr(c, isel, (uint32_t)ioff + hdr, (uint32_t)ncol * entsz);
        if (!tp) return CPU_E_SEL;
        for (i = 0; i < ncol; i++) {
            tp[i * (int)entsz] = (uint8_t)(tab[i] >> 16);
            tp[i * (int)entsz + 1] = (uint8_t)(tab[i] >> 8);
            tp[i * (int)entsz + 2] = (uint8_t)tab[i];
            if (entsz == 4) tp[i * 4 + 3] = 0;
        }
    } else if (ncol && usage == DIB_PAL_COLORS) {
        uint8_t *tp = hle_ptr(c, isel, (uint32_t)ioff + hdr, (uint32_t)ncol * 2);
        if (!tp) return CPU_E_SEL;
        for (i = 0; i < ncol; i++) gst16(tp + 2 * i, (uint16_t)i);
    }
    if (!(bsel || boff)) {                 /* header only */
        hle_ret16(c, (uint16_t)h);
        return CPU_OK;
    }
    if (start >= h) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (start + count > h) count = h - start;
    out = calloc((size_t)stride * (size_t)(count > 0 ? count : 1), 1);
    if (!out) return CPU_E_UD;
    for (i = 0; i < count; i++) {
        int by = h - 1 - (start + i);                 /* scan line 0 is the bottom row */
        uint8_t *row = out + (size_t)i * stride;
        for (x = 0; x < w; x++) {
            uint8_t px = b->bits[(size_t)by * w + x];
            uint32_t col = bitmap_color(g, b, px);
            if (bpp == 24) {
                row[3 * x] = (uint8_t)(col >> 16);
                row[3 * x + 1] = (uint8_t)(col >> 8);
                row[3 * x + 2] = (uint8_t)col;
            } else if (bpp == 8) {
                uint8_t v = b->bpp == 1 ? px : px;
                row[x] = v;
            } else if (bpp == 4) {
                int v = nearest_ega(col);
                row[x / 2] |= (uint8_t)((x & 1) ? v : v << 4);
            } else {
                int lum = (int)((col & 0xFF) * 30 + (col >> 8 & 0xFF) * 59 + (col >> 16 & 0xFF) * 11) / 100;
                if (lum >= 128) row[x / 8] |= (uint8_t)(0x80 >> (x & 7));
            }
        }
    }
    (void)y;
    if (!guest_write(c, bsel, boff, out, stride * (uint32_t)count)) {
        free(out);
        return CPU_E_SEL;
    }
    free(out);
    hle_ret16(c, (uint16_t)count);
    return CPU_OK;
}

/* ---- StretchDIBits / SetDIBitsToDevice ------------------------------------------------------------------------------ */

static void span1(int p0, int p1, int *lo, int *n, bool *neg) {
    int d = p1 - p0;
    *neg = d < 0;
    *lo = d >= 0 ? p0 : p0 + d + 1;
    *n = d >= 0 ? d : -d;
}

static cpu_status dib_to_dc(hle_call *c, gdi_t *g, uint16_t hdc, int xd, int yd, int cxd, int cyd, int xs, int ys, int cxs, int cys,
                            uint16_t bsel, uint16_t boff, uint16_t isel, uint16_t ioff, int usage, uint32_t rop, int start, int nlines,
                            bool stretch, int *result) {
    dibinfo d;
    gctx x;
    uint8_t *bits = NULL, *rows_ok = NULL, *dev = NULL;
    uint32_t *colors = NULL, len = 0;
    int r, dl, dt, dwa, dha, sl, st, swa, sha, lo, n;
    bool nx, ny, fx = false, fy = false, ok = false, snx = false, sny = false;
    bsrc s;
    *result = 0;
    if ((r = dib_parse(c, isel, ioff, usage, &d))) return r == 2 ? CPU_E_SEL : CPU_OK;
    if (!gctx_open(g, hdc, &x)) {
        gctx_close(&x);
        return CPU_OK;
    }
    if (stretch) {
        start = 0;
        nlines = d.h;
    } else if (nlines > d.h) {
        nlines = d.h;
    }
    if (!dib_load(c, bsel, boff, &d, d.stride * (uint32_t)(nlines > 0 ? nlines : 0), &bits, &len)) {
        gctx_close(&x);
        return bits ? CPU_OK : CPU_E_SEL;
    }
    colors = malloc((size_t)d.w * (size_t)d.h * sizeof *colors);
    rows_ok = malloc((size_t)d.h);
    if (!colors || !rows_ok || !dib_colors(&d, bits, len, colors, rows_ok, d.comp == BI_RGB ? start : 0, d.comp == BI_RGB ? nlines : d.h)) goto done;
    dev = to_device(&x, x.mono, colors, (size_t)d.w * (size_t)d.h);
    if (!dev) goto done;
    {
        int x0 = gdc_lx(x.dc, xd), x1 = gdc_lx(x.dc, xd + cxd), y0 = gdc_ly(x.dc, yd), y1 = gdc_ly(x.dc, yd + cyd);
        span1(x0, x1, &dl, &dwa, &nx);
        span1(y0, y1, &dt, &dha, &ny);
        dl += x.ox;
        dt += x.oy;
    }
    span1(xs, xs + cxs, &sl, &swa, &snx);
    span1(ys, ys + cys, &lo, &n, &sny);
    st = d.h - lo - n;                                   /* ys counts lines from the bottom */
    sha = n;
    fx = snx != nx;
    fy = sny != ny;
    s.pix = dev;
    s.w = d.w;
    s.h = d.h;
    s.mono = x.mono;
    s.bkpix = 0;
    s.row_lo = s.row_hi = 0;
    {                                                    /* rows with data form one contiguous range */
        int yy;
        bool in = false;
        for (yy = 0; yy < d.h; yy++) {
            if (rows_ok[yy] && !in) {
                s.row_lo = yy;
                in = true;
            }
            if (rows_ok[yy]) s.row_hi = yy + 1;
        }
    }
    {
        uint8_t r3 = (uint8_t)(rop >> 16);
        ok = blt_core(&x, r3, dl, dt, dwa, dha, rop3_uses_src(r3) ? &s : NULL, sl, st, swa, sha, fx, fy);
    }
    if (ok) *result = stretch ? n : nlines;
done:
    free(bits); free(colors); free(rows_ok); free(dev);
    gctx_close(&x);
    return CPU_OK;
}

static cpu_status g_stretchdibits(hle_call *c) {
    GBEGIN;
    uint16_t hdc = hle_u16(&a), bsel, boff, isel, ioff, usage;
    int xd = (int16_t)hle_u16(&a), yd = (int16_t)hle_u16(&a), cxd = (int16_t)hle_u16(&a), cyd = (int16_t)hle_u16(&a);
    int xs = (int16_t)hle_u16(&a), ys = (int16_t)hle_u16(&a), cxs = (int16_t)hle_u16(&a), cys = (int16_t)hle_u16(&a), res;
    uint32_t rop;
    cpu_status st;
    hle_far(&a, &bsel, &boff);
    hle_far(&a, &isel, &ioff);
    usage = hle_u16(&a);
    rop = hle_u32(&a);
    GCHECK;
    if (usage > 1) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    st = dib_to_dc(c, g, hdc, xd, yd, cxd, cyd, xs, ys, cxs, cys, bsel, boff, isel, ioff, usage, rop, 0, 0, true, &res);
    if (st) return st;
    hle_ret16(c, (uint16_t)res);
    return CPU_OK;
}

static cpu_status g_setdibitstodevice(hle_call *c) {
    GBEGIN;
    uint16_t hdc = hle_u16(&a), bsel, boff, isel, ioff, usage;
    int xd = (int16_t)hle_u16(&a), yd = (int16_t)hle_u16(&a), cx = (int16_t)hle_u16(&a), cy = (int16_t)hle_u16(&a);
    int xs = (int16_t)hle_u16(&a), ys = (int16_t)hle_u16(&a), start = hle_u16(&a), nl = hle_u16(&a), res;
    cpu_status st;
    hle_far(&a, &bsel, &boff);
    hle_far(&a, &isel, &ioff);
    usage = hle_u16(&a);
    GCHECK;
    if (usage > 1) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    st = dib_to_dc(c, g, hdc, xd, yd, cx, cy, xs, ys, cx, cy, bsel, boff, isel, ioff, usage, 0x00CC0020u, start, nl, false, &res);
    if (st) return st;
    hle_ret16(c, (uint16_t)res);
    return CPU_OK;
}

/* ---- DIB pattern brushes ----------------------------------------------------------------------------------------------- */

/* hglobal is a selector in this runtime: the DIB starts at offset 0 (header, colour table, then the bits). */
uint16_t gdi_brush_dib(gdi_t *g, hle_call *c, uint16_t hglobal, int usage) {
    dibinfo d;
    uint8_t *bits = NULL, *rows_ok = NULL;
    uint32_t *colors = NULL, len = 0;
    gobj *o;
    uint16_t h = 0;
    int x, y;
    if (usage > 1 || dib_parse(c, hglobal, 0, usage, &d)) return 0;
    if (!dib_load(c, hglobal, dib_bits_offset(&d), &d, d.stride * (uint32_t)d.h, &bits, &len)) return 0;
    colors = malloc((size_t)d.w * (size_t)d.h * sizeof *colors);
    rows_ok = malloc((size_t)d.h);
    if (colors && rows_ok && dib_colors(&d, bits, len, colors, rows_ok, 0, d.comp == BI_RGB ? d.h : d.h)) {
        h = gobj_new(g, GO_BRUSH, &o);
        if (h) {
            o->u.brush.style = BS_DIBPATTERN;
            o->u.brush.hatch = hglobal;
            o->u.brush.color = (uint32_t)usage;
            for (y = 0; y < 8; y++)
                for (x = 0; x < 8; x++) o->u.brush.pat[y * 8 + x] = colors[(size_t)(y % d.h) * d.w + (size_t)(x % d.w)];
        }
    }
    free(bits); free(colors); free(rows_ok);
    return h;
}

static cpu_status g_createdibpatternbrush(hle_call *c) {
    GBEGIN;
    uint16_t hg = hle_u16(&a), usage = hle_u16(&a);
    GCHECK;
    hle_ret16(c, gdi_brush_dib(g, c, hg, usage));
    return CPU_OK;
}

#define G GDI_MODULE
const hle_def gdi_dib_defs[] = {
    {G, 439, "StretchDIBits", 32, g_stretchdibits},
    {G, 440, "SetDIBits", 18, g_setdibits},
    {G, 441, "GetDIBits", 18, g_getdibits},
    {G, 442, "CreateDIBitmap", 20, g_createdibitmap},
    {G, 443, "SetDIBitsToDevice", 28, g_setdibitstodevice},     /* UNV */
    {G, 445, "CreateDIBPatternBrush", 4, g_createdibpatternbrush},
};
#undef G
const size_t gdi_dib_ndefs = sizeof gdi_dib_defs / sizeof gdi_dib_defs[0];
