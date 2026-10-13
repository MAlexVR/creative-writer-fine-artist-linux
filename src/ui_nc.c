/* The non-client area of top-level windows: frame, caption bar with system box and minimize/maximize
 * buttons, the menu bar, hit testing and the default handling of the messages that belong to them. */
#include <string.h>

#include "ui_priv.h"

#define CAPH 18
#define BARH 19
#define BTNW 18
#define ITEM_PAD 8

#define C_FRAME 6
#define C_FACE 15
#define C_SHADOW 16
#define C_HILITE 20

static uint32_t col(int idx) { return wm_sys_color(idx); }

static void fill(user_t *u, uint16_t dc, int l, int t, int r, int b, uint32_t c) {
    fb_rect q;
    q.left = l; q.top = t; q.right = r; q.bottom = b;
    if (u->has_gfx && !fb_rect_empty(&q)) u->gfx.fill(u->gfx.ctx, dc, &q, c);
}

static void outline(user_t *u, uint16_t dc, int l, int t, int r, int b, uint32_t c) {
    fill(u, dc, l, t, r, t + 1, c);
    fill(u, dc, l, b - 1, r, b, c);
    fill(u, dc, l, t + 1, l + 1, b - 1, c);
    fill(u, dc, r - 1, t + 1, r, b - 1, c);
}

/* ---- menu item labels and the bar ------------------------------------------------------------ */

void ui_item_label(const ui_item *it, char *label, size_t cap, int *ul, char *accel, size_t acap) {
    const char *s = it->text ? it->text : "";
    size_t n = 0;
    *ul = -1;
    if (accel && acap) accel[0] = 0;
    for (; *s && *s != '\t'; s++) {
        if (*s == '&') {
            if (s[1] == '&') s++;
            else if (*ul < 0 && s[1] && s[1] != '\t') {
                *ul = (int)n;
                continue;
            } else {
                continue;
            }
        }
        if (n + 1 < cap) label[n++] = *s;
    }
    label[n] = 0;
    if (*s == '\t' && accel && acap) {
        strncpy(accel, s + 1, acap - 1);
        accel[acap - 1] = 0;
    }
}

static int text_w(user_t *u, const char *s, int n) { return u->has_gfx ? u->gfx.text_width(u->gfx.ctx, s, n, 0) : 8 * n; }
static int text_h(user_t *u) { return u->has_gfx ? u->gfx.text_height(u->gfx.ctx, 0) : 16; }

void ui_menubar_layout(user_t *u, uint16_t hmenu, const nc_geom *g) {
    ui_menu *m = ui_menu_get(u->ui, hmenu);
    int i, x = 0, right;
    if (!m) return;
    right = g->bar.right - g->bar.left;
    for (i = 0; i < m->n; i++) {
        char label[160];
        int ul;
        ui_item_label(&m->it[i], label, sizeof label, &ul, NULL, 0);
        m->it[i].w = m->it[i].flags & MF_SEPARATOR ? 0 : text_w(u, label, (int)strlen(label)) + 2 * ITEM_PAD;
        if (m->it[i].flags & MF_BITMAP) m->it[i].w = 2 * ITEM_PAD + 16;
    }
    for (i = 0; i < m->n; i++) {
        m->it[i].x = x;
        x += m->it[i].w;
    }
    for (i = m->n - 1; i >= 0 && (m->it[i].flags & MF_HELP); i--) {   /* help items sit at the right end */
        right -= m->it[i].w;
        m->it[i].x = right;
    }
}

/* ---- geometry ----------------------------------------------------------------------------------- */

bool ui_nc_geom(user_t *u, uint16_t hwnd, nc_geom *g) {
    wm_window *x = wm_get(&u->wm, hwnd);
    fb_rect wr;
    int l, t, r, b, top;
    memset(g, 0, sizeof *g);
    if (!x || (x->style & WS_MINIMIZE) || !wm_window_rect(&u->wm, hwnd, &wr) || !wm_insets(&u->wm, hwnd, &l, &t, &r, &b))
        return false;
    g->w = wr.right - wr.left;
    g->h = wr.bottom - wr.top;
    g->f = l;
    top = g->f;
    if ((x->style & WS_CAPTION) == WS_CAPTION) {
        int right = g->w - g->f;
        g->has_cap = true;
        g->cap = (fb_rect){g->f, g->f, g->w - g->f, g->f + CAPH};
        if (x->style & WS_SYSMENU) {
            g->has_sys = true;
            g->sys = (fb_rect){g->f, g->f, g->f + BTNW, g->f + CAPH};
        }
        if (x->style & WS_MAXIMIZEBOX_) {
            g->has_max = true;
            g->maxb = (fb_rect){right - BTNW, g->f, right, g->f + CAPH};
            right -= BTNW;
        }
        if (x->style & WS_MINIMIZEBOX_) {
            g->has_min = true;
            g->minb = (fb_rect){right - BTNW, g->f, right, g->f + CAPH};
        }
        top += CAPH + 1;
    }
    if (!(x->style & WS_CHILD) && x->id != 0) {
        g->has_bar = true;
        g->bar = (fb_rect){g->f, top, g->w - g->f, top + BARH};
    }
    return true;
}

static bool in_rect(const fb_rect *r, int x, int y) { return x >= r->left && x < r->right && y >= r->top && y < r->bottom; }

int ui_menubar_item_at(user_t *u, uint16_t hwnd, int wx, int wy) {
    nc_geom g;
    wm_window *x = wm_get(&u->wm, hwnd);
    ui_menu *m;
    int i;
    if (!x || !ui_nc_geom(u, hwnd, &g) || !g.has_bar || !in_rect(&g.bar, wx, wy) || !(m = ui_menu_get(u->ui, x->id)))
        return -1;
    ui_menubar_layout(u, x->id, &g);
    for (i = 0; i < m->n; i++)
        if (!(m->it[i].flags & MF_SEPARATOR) && wx >= g.bar.left + m->it[i].x && wx < g.bar.left + m->it[i].x + m->it[i].w)
            return i;
    return -1;
}

int ui_nchittest(user_t *u, uint16_t hwnd, int sx, int sy) {
    wm_window *x = wm_get(&u->wm, hwnd);
    fb_rect wr, cl;
    nc_geom g;
    int ox, oy, wx, wy, cx, cy;
    if (!x || !wm_window_rect(&u->wm, hwnd, &wr)) return HTNOWHERE;
    if (sx < wr.left || sx >= wr.right || sy < wr.top || sy >= wr.bottom) return HTNOWHERE;
    wm_client_origin(&u->wm, hwnd, &ox, &oy);
    wm_client_rect(&u->wm, hwnd, &cl);
    cx = sx - ox;
    cy = sy - oy;
    if (cx >= 0 && cy >= 0 && cx < cl.right && cy < cl.bottom) return HTCLIENT;
    if (x->style & WS_MINIMIZE) return HTCAPTION;
    if (!ui_nc_geom(u, hwnd, &g)) return HTBORDER;
    wx = sx - wr.left;
    wy = sy - wr.top;
    if (g.has_sys && in_rect(&g.sys, wx, wy)) return HTSYSMENU;
    if (g.has_max && in_rect(&g.maxb, wx, wy)) return HTZOOM;
    if (g.has_min && in_rect(&g.minb, wx, wy)) return HTREDUCE;
    if (g.has_cap && in_rect(&g.cap, wx, wy)) return HTCAPTION;
    if (g.has_bar && in_rect(&g.bar, wx, wy)) return HTMENU;
    {
        int l, t, r, b;
        wm_insets(&u->wm, hwnd, &l, &t, &r, &b);
        if ((x->style & WS_VSCROLL) && wx >= g.w - g.f - 16 && wx < g.w - g.f && wy >= t && wy < g.h - b + ((x->style & WS_HSCROLL) ? 0 : 0))
            return HTVSCROLL;
        if ((x->style & WS_HSCROLL) && wy >= g.h - g.f - 16 && wy < g.h - g.f && wx >= l && wx < g.w - r + ((x->style & WS_VSCROLL) ? 16 : 0))
            return HTHSCROLL;
    }
    return HTBORDER;
}

/* ---- painting -------------------------------------------------------------------------------------- */

static void paint_frame(user_t *u, uint16_t dc, const nc_geom *g) {
    int w = g->w, h = g->h, f = g->f;
    if (f <= 0) return;
    fill(u, dc, 0, 0, w, f, col(C_FACE));
    fill(u, dc, 0, h - f, w, h, col(C_FACE));
    fill(u, dc, 0, f, f, h - f, col(C_FACE));
    fill(u, dc, w - f, f, w, h - f, col(C_FACE));
    if (f >= 3) {
        fill(u, dc, 1, 1, w - 1, 2, col(C_HILITE));
        fill(u, dc, 1, 1, 2, h - 1, col(C_HILITE));
        fill(u, dc, 1, h - 2, w - 1, h - 1, col(C_SHADOW));
        fill(u, dc, w - 2, 1, w - 1, h - 1, col(C_SHADOW));
    }
    outline(u, dc, 0, 0, w, h, col(C_FRAME));
    if (f >= 3) outline(u, dc, f - 1, f - 1, w - f + 1, h - f + 1, col(C_FRAME));
}

static void paint_triangle(user_t *u, uint16_t dc, int cx, int top, int rows, bool up, uint32_t c) {
    int k;
    for (k = 0; k < rows; k++) {
        int half = up ? k : rows - 1 - k;
        fill(u, dc, cx - half, top + k, cx + half + 1, top + k + 1, c);
    }
}

static void paint_button(user_t *u, uint16_t dc, const fb_rect *r, int glyph) {
    int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2;
    fill(u, dc, r->left, r->top, r->right, r->bottom, col(C_FACE));
    fill(u, dc, r->left + 1, r->top, r->right, r->top + 1, col(C_HILITE));
    fill(u, dc, r->left + 1, r->top, r->left + 2, r->bottom - 1, col(C_HILITE));
    fill(u, dc, r->left + 1, r->bottom - 1, r->right, r->bottom, col(C_SHADOW));
    fill(u, dc, r->right - 1, r->top, r->right, r->bottom, col(C_SHADOW));
    fill(u, dc, r->left, r->top, r->left + 1, r->bottom, col(C_FRAME));
    if (glyph == 0) {
        paint_triangle(u, dc, cx, cy - 3, 5, true, col(C_FRAME));
    } else if (glyph == 1) {
        paint_triangle(u, dc, cx, cy - 2, 5, false, col(C_FRAME));
    } else {   /* restore: a small up arrow over a small down arrow */
        paint_triangle(u, dc, cx, cy - 5, 4, true, col(C_FRAME));
        paint_triangle(u, dc, cx, cy, 4, false, col(C_FRAME));
    }
}

static void paint_text_fit(user_t *u, uint16_t dc, int l, int r, int y, const char *s, uint32_t fg) {
    int n = (int)strlen(s), tw;
    while (n > 0 && (tw = text_w(u, s, n)) > r - l - 4) n--;
    if (n <= 0) return;
    tw = text_w(u, s, n);
    u->gfx.text(u->gfx.ctx, dc, l + (r - l - tw) / 2, y, s, n, fg, 0xFFFFFFFFu, 0);
}

void ui_menu_text_at(user_t *u, uint16_t dc, int x, int y, const char *label, int ul, uint32_t fg) {
    int n = (int)strlen(label);
    u->gfx.text(u->gfx.ctx, dc, x, y, label, n, fg, 0xFFFFFFFFu, 0);
    if (ul >= 0 && ul < n) {
        int x0 = x + text_w(u, label, ul), x1 = x0 + text_w(u, label + ul, 1);
        fill(u, dc, x0, y + text_h(u) - 2, x1, y + text_h(u) - 1, fg);
    }
}

static void paint_bar(user_t *u, uint16_t dc, uint16_t hwnd, wm_window *x, const nc_geom *g) {
    ui_menu *m = ui_menu_get(u->ui, x->id);
    int i, th = text_h(u);
    fill(u, dc, g->bar.left, g->bar.top, g->bar.right, g->bar.bottom, col(4));
    fill(u, dc, g->bar.left, g->bar.bottom, g->bar.right, g->bar.bottom + 1, col(C_FRAME));
    if (!m) return;
    ui_menubar_layout(u, x->id, g);
    for (i = 0; i < m->n; i++) {
        const ui_item *it = &m->it[i];
        char label[160];
        int ul, l = g->bar.left + it->x, tx = l + ITEM_PAD, ty = g->bar.top + (BARH - th) / 2;
        bool sel = u->ui->trk_hwnd == hwnd && u->ui->trk_item == i;
        uint32_t fg = it->flags & (MF_GRAYED | MF_DISABLED) ? col(17) : col(7);
        if (it->flags & MF_SEPARATOR) continue;
        if (sel) {
            fill(u, dc, l, g->bar.top, l + it->w, g->bar.bottom, col(13));
            fg = col(14);
        }
        ui_item_label(it, label, sizeof label, &ul, NULL, 0);
        ui_menu_text_at(u, dc, tx, ty, label, ul, fg);
    }
}

void ui_ncpaint(user_t *u, uint16_t hwnd) {
    wm_window *x = wm_get(&u->wm, hwnd);
    nc_geom g;
    uint16_t dc;
    if (!u->has_gfx || !x || hwnd == u->wm.desktop || !(x->style & WS_VISIBLE) || !wm_is_visible(&u->wm, hwnd)) return;
    if (!(dc = wm_get_window_dc(&u->wm, hwnd))) return;
    if (x->style & WS_MINIMIZE) {
        uint16_t hi = x->cls ? x->cls->hicon : 0;
        ui_image *im = ui_image_get(u->ui, hi);
        fill(u, dc, 0, 0, 32, 32, col(1));
        if (im) ui_draw_image(u, dc, 0, 0, im);
    } else if (ui_nc_geom(u, hwnd, &g)) {
        bool active = u->wm.active == hwnd;
        paint_frame(u, dc, &g);
        if (g.has_cap) {
            const char *title = wm_get_text(&u->wm, hwnd);
            int left = g.has_sys ? g.sys.right : g.f, right = g.cap.right;
            if (g.has_min) right = g.minb.left;
            else if (g.has_max) right = g.maxb.left;
            fill(u, dc, g.cap.left, g.cap.top, g.cap.right, g.cap.bottom, col(active ? 2 : 3));
            fill(u, dc, g.cap.left, g.cap.bottom, g.cap.right, g.cap.bottom + 1, col(C_FRAME));
            if (g.has_sys) {
                fill(u, dc, g.sys.left, g.sys.top, g.sys.right, g.sys.bottom, col(C_FACE));
                fill(u, dc, g.sys.right - 1, g.sys.top, g.sys.right, g.sys.bottom, col(C_FRAME));
                fill(u, dc, g.sys.left + 3, g.sys.top + 7, g.sys.left + 15, g.sys.top + 11, col(C_FRAME));
                fill(u, dc, g.sys.left + 4, g.sys.top + 8, g.sys.left + 14, g.sys.top + 10, col(C_HILITE));
            }
            if (g.has_max) paint_button(u, dc, &g.maxb, (x->style & WS_MAXIMIZE) ? 2 : 0);
            if (g.has_min) paint_button(u, dc, &g.minb, 1);
            if (title && *title) paint_text_fit(u, dc, left, right, g.cap.top + (CAPH - text_h(u)) / 2, title, col(active ? 9 : 19));
        }
        if (g.has_bar) paint_bar(u, dc, hwnd, x, &g);
        ui_nc_scrollbars(u, dc, hwnd);
    }
    wm_release_dc(&u->wm, hwnd, dc);
}

/* ---- default processing --------------------------------------------------------------------------- */

static void sys_command(user_t *u, uint16_t hwnd, uint16_t cmd, uint32_t lp) {
    wm_window *x = wm_get(&u->wm, hwnd);
    if (!x) return;
    switch (cmd & 0xFFF0) {
    case SC_CLOSE: wm_send(&u->wm, hwnd, WM_CLOSE, 0, 0); break;
    case SC_MINIMIZE: wm_show_window(&u->wm, hwnd, SW_SHOWMINIMIZED); break;
    case SC_MAXIMIZE: wm_show_window(&u->wm, hwnd, SW_SHOWMAXIMIZED); break;
    case SC_RESTORE:
        if (x->style & (WS_MINIMIZE | WS_MAXIMIZE)) wm_show_window(&u->wm, hwnd, SW_RESTORE);
        break;
    case SC_MOUSEMENU:
        if ((cmd & 0xF) == HTSYSMENU) {
            ui_track_window_menu(u, hwnd, true, -1, true, true);
        } else if ((cmd & 0xF) == HTMENU) {
            fb_rect wr;
            int idx = -1;
            if (wm_window_rect(&u->wm, hwnd, &wr)) idx = ui_menubar_item_at(u, hwnd, (int16_t)lp - wr.left, (int16_t)(lp >> 16) - wr.top);
            if (idx >= 0) ui_track_window_menu(u, hwnd, false, idx, true, true);
        }
        break;
    case SC_KEYMENU: {
        unsigned ch = lp & 0xFFFF;
        if (ch == 0) {
            ui_track_window_menu(u, hwnd, false, -1, false, false);
        } else if (ch == ' ') {
            ui_track_window_menu(u, hwnd, true, -1, true, false);
        } else {
            int idx = ui_menubar_mnemonic(u, hwnd, ch);
            if (idx >= 0) ui_track_window_menu(u, hwnd, false, idx, true, false);
        }
        break;
    }
    default:
        break;      /* SC_MOVE, SC_SIZE, task switching: no window dragging yet */
    }
}

bool ui_nc_defproc(user_t *u, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp, uint32_t *ret) {
    wm_window *x = wm_get(&u->wm, hwnd);
    if (!x) return false;
    *ret = 0;
    switch (msg) {
    case WM_NCPAINT:
        if (!u->has_gfx) return false;
        ui_ncpaint(u, hwnd);
        return true;
    case WM_NCHITTEST:
        *ret = (uint16_t)(int16_t)ui_nchittest(u, hwnd, (int16_t)lp, (int16_t)(lp >> 16));
        return true;
    case WM_NCCALCSIZE: {
        uint8_t *p;
        int l, t, r, b;
        if (wp != 0 || !(p = user_guest_ptr(u, lp, 8)) || !wm_insets(&u->wm, hwnd, &l, &t, &r, &b)) return true;
        st16(p, (uint16_t)((int16_t)ld16(p) + l));
        st16(p + 2, (uint16_t)((int16_t)ld16(p + 2) + t));
        st16(p + 4, (uint16_t)((int16_t)ld16(p + 4) - r));
        st16(p + 6, (uint16_t)((int16_t)ld16(p + 6) - b));
        return true;
    }
    case WM_NCACTIVATE:
        if (wm_is_visible(&u->wm, hwnd)) wm_send(&u->wm, hwnd, WM_NCPAINT, 1, 0);
        *ret = 1;
        return true;
    case WM_SYSCOMMAND:
        sys_command(u, hwnd, wp, lp);
        return true;
    case WM_KEYDOWN:
        u->ui->alt_only = false;
        return false;
    case WM_SYSKEYDOWN:
        if (wp == VK_MENU) {
            if (!(lp & 0x40000000u)) u->ui->alt_only = true;
        } else if (wp == VK_F10) {
            u->ui->alt_only = false;
            wm_send(&u->wm, hwnd, WM_SYSCOMMAND, SC_KEYMENU, 0);
        } else {
            u->ui->alt_only = false;
        }
        return true;
    case WM_SYSKEYUP:
        if (wp == VK_MENU && u->ui->alt_only) {
            u->ui->alt_only = false;
            wm_send(&u->wm, hwnd, WM_SYSCOMMAND, SC_KEYMENU, 0);
        }
        return true;
    case WM_SYSCHAR:
        u->ui->alt_only = false;
        if ((lp & 0x20000000u) && wp >= ' ') wm_send(&u->wm, hwnd, WM_SYSCOMMAND, SC_KEYMENU, wp);
        return true;
    case WM_NCLBUTTONDOWN:
        switch ((int16_t)wp) {
        case HTSYSMENU:
        case HTMENU: wm_send(&u->wm, hwnd, WM_SYSCOMMAND, (uint16_t)(SC_MOUSEMENU + (int16_t)wp), lp); break;
        case HTVSCROLL: ui_nc_sb_press(u, hwnd, true, lp); break;
        case HTHSCROLL: ui_nc_sb_press(u, hwnd, false, lp); break;
        case HTREDUCE: wm_send(&u->wm, hwnd, WM_SYSCOMMAND, SC_MINIMIZE, lp); break;
        case HTZOOM:
            wm_send(&u->wm, hwnd, WM_SYSCOMMAND, (x->style & WS_MAXIMIZE) ? SC_RESTORE : SC_MAXIMIZE, lp);
            break;
        default: break;
        }
        return true;
    default:
        return false;
    }
}
