/* Creation of the resource driven USER. See ui.h. */
#include <stdlib.h>
#include <string.h>

#include "ui_priv.h"

ui_t *ui_create(user_t *u) {
    ui_t *ui = calloc(1, sizeof *ui);
    if (!ui) return NULL;
    ui->u = u;
    u->ui = ui;
    if (!hle_register_table(u->hle, ui_res_defs, ui_res_ndefs, u) ||
        !hle_register_table(u->hle, ui_scroll_defs, ui_scroll_ndefs, u) ||
        !hle_register_table(u->hle, ui_menu_defs, ui_menu_ndefs, u) ||
        !hle_register_table(u->hle, ui_misc_defs, ui_misc_ndefs, u) ||
        !hle_register_table(u->hle, ui_commdlg_defs, ui_commdlg_ndefs, u) ||
        !hle_register_table(u->hle, ui_dlg_defs, ui_dlg_ndefs, u) ||
        !hle_register_table(u->hle, ui_track_defs, ui_track_ndefs, u) ||
        !hle_register_table(u->hle, ui_kbd_defs, ui_kbd_ndefs, u)) {
        free(ui);
        u->ui = NULL;
        return NULL;
    }
    return ui;
}

void ui_destroy(ui_t *ui) {
    size_t i;
    if (!ui) return;
    for (i = 0; i < UI_MAX_IMAGES; i++) {
        free(ui->img[i].cref);
        free(ui->img[i].mask);
    }
    for (i = 0; i < UI_MAX_ACCEL; i++) free(ui->acc[i].e);
    for (i = 0; i < (size_t)ui->clip.n; i++) free(ui->clip.f[i].data);
    for (i = 0; i < UI_MAX_MENUS; i++) {
        int k;
        for (k = 0; k < ui->menu[i].n; k++) free(ui->menu[i].it[k].text);
        free(ui->menu[i].it);
    }
    free(ui);
}

ui_win *ui_win_get(ui_t *ui, uint16_t hwnd, bool create) {
    wm_window *x = wm_get(&ui->u->wm, hwnd);
    if (!x) return NULL;
    if (!x->ui_data && create) x->ui_data = calloc(1, sizeof(ui_win));
    return x->ui_data;
}

/* ---- drawing helpers ---------------------------------------------------------------------------- */

void ui_fill(user_t *u, uint16_t dc, int l, int t, int r, int b, uint32_t c) {
    fb_rect q;
    q.left = l; q.top = t; q.right = r; q.bottom = b;
    if (u->has_gfx && !fb_rect_empty(&q)) u->gfx.fill(u->gfx.ctx, dc, &q, c);
}

void ui_outline(user_t *u, uint16_t dc, int l, int t, int r, int b, uint32_t c) {
    ui_fill(u, dc, l, t, r, t + 1, c);
    ui_fill(u, dc, l, b - 1, r, b, c);
    ui_fill(u, dc, l, t + 1, l + 1, b - 1, c);
    ui_fill(u, dc, r - 1, t + 1, r, b - 1, c);
}

/* A dotted rectangle drawn by inverting every other pixel (the focus rectangle). */
void ui_dotted(user_t *u, uint16_t dc, int l, int t, int r, int b) {
    int x, y;
    fb_rect q;
    if (!u->has_gfx) return;
    for (x = l; x < r; x++)
        for (y = t; y < b; y++) {
            if (!(x == l || x == r - 1 || y == t || y == b - 1) || ((x + y) & 1)) continue;
            q.left = x; q.right = x + 1; q.top = y; q.bottom = y + 1;
            u->gfx.invert(u->gfx.ctx, dc, &q);
        }
}

int ui_cell_of(user_t *u, uint16_t hwnd) {
    wm_window *x = wm_get(&u->wm, hwnd);
    int guard = 0;
    while (x && guard++ < 16) {
        ui_win *uw = x->ui_data;
        if (uw && uw->font) return UI_CELL_FONT | uw->font;          /* the control's own font (WM_SETFONT) */
        if (uw && uw->cell) return uw->cell;
        x = wm_get(&u->wm, x->parent);
    }
    return 0;
}

int ui_tw(user_t *u, int cell, const char *s, int n) { return u->has_gfx ? u->gfx.text_width(u->gfx.ctx, s, n, cell) : 8 * n; }
int ui_th(user_t *u, int cell) { return u->has_gfx ? u->gfx.text_height(u->gfx.ctx, cell) : 16; }

void ui_text(user_t *u, uint16_t dc, int cell, int x, int y, const char *s, int n, uint32_t fg, uint32_t bg) {
    if (u->has_gfx && n > 0) u->gfx.text(u->gfx.ctx, dc, x, y, s, n, fg, bg, cell);
}

void ui_notify_parent(user_t *u, uint16_t hwnd, unsigned code) {
    wm_window *x = wm_get(&u->wm, hwnd);
    if (x && x->parent && x->parent != u->wm.desktop)
        wm_send(&u->wm, x->parent, WM_COMMAND, x->id, (uint32_t)hwnd | (uint32_t)code << 16);
}

void ui_ctlcolors(user_t *u, uint16_t hwnd, uint16_t hdc, int type, uint32_t *fg, uint32_t *bg) {
    wm_window *x = wm_get(&u->wm, hwnd), *p = x ? wm_get(&u->wm, x->parent) : NULL;
    ui_win *pw = p ? p->ui_data : NULL;
    bool in_dialog = pw && pw->dlg;
    uint32_t r;
    switch (type) {
    case 1: case 2: *bg = wm_sys_color(5); *fg = wm_sys_color(8); break;      /* edit, list box */
    case 3: *bg = wm_sys_color(15); *fg = wm_sys_color(18); break;            /* button */
    case 5: *bg = wm_sys_color(0); *fg = wm_sys_color(8); break;              /* scroll bar */
    case 6: *bg = wm_sys_color(in_dialog ? 15 : 5); *fg = wm_sys_color(8); break;   /* static */
    default: *bg = wm_sys_color(15); *fg = wm_sys_color(18); break;           /* dialog, message box */
    }
    if (!p || !p->parent) return;
    r = wm_send(&u->wm, x->parent, WM_CTLCOLOR, hdc, (uint32_t)type << 16 | hwnd) & 0xFFFF;
    if (r && u->has_gfx) {
        uint32_t c, f, b;
        if (u->gfx.brush_color(u->gfx.ctx, (uint16_t)r, &c)) {
            *bg = c;
            if (u->gfx.dc_colors(u->gfx.ctx, hdc, &f, &b)) *fg = f;
        }
    }
}

bool ui_defproc(user_t *u, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp, uint32_t *ret) {
    wm_window *x = wm_get(&u->wm, hwnd);
    if (x && x->cls && x->cls->builtin && ui_ctl_defproc(u, hwnd, msg, wp, lp, ret)) return true;
    return ui_nc_defproc(u, hwnd, msg, wp, lp, ret);
}
