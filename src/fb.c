/* Software framebuffer. See fb.h. */
#include "fb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The 20 static colours of a Windows 256-colour display, as 0x00RRGGBB. */
static const uint32_t static_low[10] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080,
    0x800080, 0x008080, 0xC0C0C0, 0xC0DCC0, 0xA6CAF0};
static const uint32_t static_high[10] = {
    0xFFFBF0, 0xA0A0A4, 0x808080, 0xFF0000, 0x00FF00,
    0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};

bool fb_init(fb_t *f, int w, int h, int bpp) {
    memset(f, 0, sizeof *f);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || (bpp != 8 && bpp != 32)) return false;
    if (bpp == 8) f->p8 = calloc((size_t)w * h, 1);
    else f->p32 = calloc((size_t)w * h, 4);
    if (!f->p8 && !f->p32) return false;
    f->w = w;
    f->h = h;
    f->bpp = bpp;
    fb_default_palette(f);
    return true;
}

void fb_destroy(fb_t *f) {
    free(f->p8);
    free(f->p32);
    memset(f, 0, sizeof *f);
}

void fb_default_palette(fb_t *f) {
    int i;
    memset(f->pal, 0, sizeof f->pal);
    for (i = 0; i < 10; i++) {
        f->pal[i] = static_low[i];
        f->pal[246 + i] = static_high[i];
    }
    f->dirty++;
}

static uint32_t cref_to_rgb(uint32_t c) {
    return (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
}

static uint32_t rgb_to_cref(uint32_t v) {
    return (v & 0xFF) << 16 | (v & 0xFF00) | (v >> 16 & 0xFF);
}

uint32_t fb_pixel_of(const fb_t *f, uint32_t colorref) {
    uint32_t rgb = cref_to_rgb(colorref & 0xFFFFFF), best = 0, bd = 0xFFFFFFFFu;
    int i;
    if (f->bpp == 32) return rgb;
    for (i = 0; i < 256; i++) {
        int dr = (int)(rgb >> 16 & 0xFF) - (int)(f->pal[i] >> 16 & 0xFF);
        int dg = (int)(rgb >> 8 & 0xFF) - (int)(f->pal[i] >> 8 & 0xFF);
        int db = (int)(rgb & 0xFF) - (int)(f->pal[i] & 0xFF);
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < bd) {
            bd = d;
            best = (uint32_t)i;
            if (!d) break;
        }
    }
    return best;
}

uint32_t fb_colorref_at(const fb_t *f, int x, int y) {
    if (x < 0 || y < 0 || x >= f->w || y >= f->h) return 0;
    if (f->bpp == 32) return rgb_to_cref(f->p32[(size_t)y * f->w + x]);
    return rgb_to_cref(f->pal[f->p8[(size_t)y * f->w + x]]);
}

void fb_put(fb_t *f, int x, int y, uint32_t colorref) {
    uint32_t v;
    if (x < 0 || y < 0 || x >= f->w || y >= f->h) return;
    v = fb_pixel_of(f, colorref);
    if (f->bpp == 32) f->p32[(size_t)y * f->w + x] = v;
    else f->p8[(size_t)y * f->w + x] = (uint8_t)v;
    f->dirty++;
}

size_t fb_fill_rect(fb_t *f, const fb_rect *r, uint32_t colorref) {
    fb_rect all = {0, 0, f->w, f->h}, c;
    uint32_t v;
    int x, y;
    if (!fb_rect_intersect(&c, r, &all)) return 0;
    v = fb_pixel_of(f, colorref);
    for (y = c.top; y < c.bottom; y++) {
        if (f->bpp == 32)
            for (x = c.left; x < c.right; x++) f->p32[(size_t)y * f->w + x] = v;
        else
            memset(f->p8 + (size_t)y * f->w + c.left, (int)v, (size_t)(c.right - c.left));
    }
    f->dirty++;
    return (size_t)(c.right - c.left) * (size_t)(c.bottom - c.top);
}

void fb_to_argb(const fb_t *f, uint32_t *out) {
    size_t i, n = (size_t)f->w * f->h;
    for (i = 0; i < n; i++)
        out[i] = 0xFF000000u | (f->bpp == 32 ? f->p32[i] : f->pal[f->p8[i]]);
}

bool fb_rect_empty(const fb_rect *r) { return r->right <= r->left || r->bottom <= r->top; }

bool fb_rect_intersect(fb_rect *out, const fb_rect *a, const fb_rect *b) {
    fb_rect t;
    t.left = a->left > b->left ? a->left : b->left;
    t.top = a->top > b->top ? a->top : b->top;
    t.right = a->right < b->right ? a->right : b->right;
    t.bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (fb_rect_empty(&t)) {
        memset(out, 0, sizeof *out);
        return false;
    }
    *out = t;
    return true;
}

void fb_rect_union(fb_rect *out, const fb_rect *a, const fb_rect *b) {
    fb_rect t;
    if (fb_rect_empty(a)) t = *b;
    else if (fb_rect_empty(b)) t = *a;
    else {
        t.left = a->left < b->left ? a->left : b->left;
        t.top = a->top < b->top ? a->top : b->top;
        t.right = a->right > b->right ? a->right : b->right;
        t.bottom = a->bottom > b->bottom ? a->bottom : b->bottom;
    }
    if (fb_rect_empty(&t)) memset(&t, 0, sizeof t);
    *out = t;
}

bool fb_save_ppm(const fb_t *f, const char *path) {
    FILE *fp = fopen(path, "wb");
    int x, y;
    bool ok;
    if (!fp) return false;
    ok = fprintf(fp, "P6\n%d %d\n255\n", f->w, f->h) > 0;
    for (y = 0; ok && y < f->h; y++)
        for (x = 0; ok && x < f->w; x++) {
            uint32_t c = f->bpp == 8 ? f->pal[f->p8[(size_t)y * f->w + x]] : f->p32[(size_t)y * f->w + x];
            unsigned char rgb[3] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c};
            ok = fwrite(rgb, 1, 3, fp) == 3;
        }
    return fclose(fp) == 0 && ok;
}
