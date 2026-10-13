/* The drawing services the built-in UI (frames, menus, controls, icons) takes from GDI: see user_gfx in user.h.
 * Coordinates are device pixels of the DC; the DC's clip applies. */
#include "gdi_priv.h"

int gdi_ui_text_width(gdi_t *g, const char *s, int n, int cell);
int gdi_ui_text_height(gdi_t *g, int cell);
void gdi_ui_font_metrics(gdi_t *g, int cell, int *avgw, int *height);
void gdi_ui_text(gdi_t *g, uint16_t hdc, int x, int y, const char *s, int n, uint32_t fg, uint32_t bg, int cell);

static void ui_fill(void *ctx, uint16_t hdc, const fb_rect *r, uint32_t cref) {
    gdi_t *g = ctx;
    gctx x;
    gbrush b;
    bpat bp;
    fb_rect q;
    if (!gctx_open(g, hdc, &x)) {
        gctx_close(&x);
        return;
    }
    memset(&b, 0, sizeof b);
    b.style = BS_SOLID;
    b.color = cref;
    brush_prepare(&x, &b, &bp);
    q.left = r->left + x.ox;
    q.right = r->right + x.ox;
    q.top = r->top + x.oy;
    q.bottom = r->bottom + x.oy;
    if (!fb_rect_empty(&q)) fill_rect_surface(&x, &q, &bp, 13);
    gctx_close(&x);
}

static void ui_invert(void *ctx, uint16_t hdc, const fb_rect *r) {
    gdi_t *g = ctx;
    gctx x;
    fb_rect q;
    if (!gctx_open(g, hdc, &x)) {
        gctx_close(&x);
        return;
    }
    q.left = r->left + x.ox;
    q.right = r->right + x.ox;
    q.top = r->top + x.oy;
    q.bottom = r->bottom + x.oy;
    if (!fb_rect_empty(&q)) invert_rect_surface(&x, &q);
    gctx_close(&x);
}

static void ui_text(void *ctx, uint16_t hdc, int x, int y, const char *s, int n, uint32_t fg, uint32_t bg, int cell) {
    gdi_ui_text(ctx, hdc, x, y, s, n, fg, bg, cell);
}
static int ui_text_width(void *ctx, const char *s, int n, int cell) { return gdi_ui_text_width(ctx, s, n, cell); }
static int ui_text_height(void *ctx, int cell) { return gdi_ui_text_height(ctx, cell); }
static void ui_font_metrics(void *ctx, int cell, int *avgw, int *height) { gdi_ui_font_metrics(ctx, cell, avgw, height); }

static uint16_t ui_bitmap_create(void *ctx, int w, int h, bool mono, const uint32_t *cref) {
    gdi_t *g = ctx;
    gobj *o = NULL;
    uint16_t hb;
    int i;
    if (w <= 0 || h <= 0) return 0;
    hb = gdi_bitmap_create(g, w, h, mono ? 1 : 8, &o);
    if (!hb || !o) return 0;
    for (i = 0; i < w * h; i++) {
        uint32_t c = cref[i] & 0xFFFFFF;
        if (mono) o->u.bmp.bits[i] = ((c & 0xFF) * 30 + (c >> 8 & 0xFF) * 59 + (c >> 16 & 0xFF) * 11) / 100 >= 128;
        else o->u.bmp.bits[i] = gdi_nearest(g, c);
    }
    return hb;
}

static bool ui_brush_color(void *ctx, uint16_t hbrush, uint32_t *cref) {
    gdi_t *g = ctx;
    gobj *o = gobj_typed(g, hbrush, GO_BRUSH);
    if (o) {
        if (o->u.brush.style != BS_SOLID) return false;
        *cref = o->u.brush.color & 0xFFFFFF;
        return true;
    }
    if (hbrush >= 1 && hbrush <= COLOR_NCOLORS) {
        *cref = wm_sys_color(hbrush - 1);
        return true;
    }
    return false;
}

static bool ui_dc_colors(void *ctx, uint16_t hdc, uint32_t *fg, uint32_t *bg) {
    gdc *dc = gdc_get(ctx, hdc);
    if (!dc) return false;
    *fg = dc->fg & 0xFFFFFF;
    *bg = dc->bk & 0xFFFFFF;
    return true;
}

static bool ui_region_box(void *ctx, uint16_t hrgn, fb_rect *box) {
    gobj *o = gobj_typed(ctx, hrgn, GO_REGION);
    if (!o) return false;
    *box = o->u.rgn.box;
    return true;
}

static void ui_to_device(void *ctx, uint16_t hdc, int *x, int *y) {
    gdc *dc = gdc_get(ctx, hdc);
    if (!dc) return;
    *x = gdc_lx(dc, *x);
    *y = gdc_ly(dc, *y);
}

void gdi_ui_install(gdi_t *g) {
    user_gfx gx;
    gx.ctx = g;
    gx.fill = ui_fill;
    gx.invert = ui_invert;
    gx.text = ui_text;
    gx.text_width = ui_text_width;
    gx.text_height = ui_text_height;
    gx.font_metrics = ui_font_metrics;
    gx.bitmap_create = ui_bitmap_create;
    gx.brush_color = ui_brush_color;
    gx.dc_colors = ui_dc_colors;
    gx.region_box = ui_region_box;
    gx.to_device = ui_to_device;
    user_set_gfx(g->user, &gx);
}
