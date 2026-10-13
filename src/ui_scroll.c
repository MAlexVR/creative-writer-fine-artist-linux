/* Scroll bars: the shared drawing and hit testing, the modal tracking of arrows, pages and the thumb, the
 * scroll bars of windows (non-client) and the USER API for them. Scroll bar controls (ui_ctl.c) use the same
 * pieces. Auto-repeat of the arrows and pages runs while the button stays down, paced by the host clock. */
#include <string.h>

#include "ui_priv.h"

enum { P_NONE = 0, P_LO, P_PAGE_LO, P_THUMB, P_PAGE_HI, P_HI };

typedef struct { int len, a, b, th0, th1; bool thumb; } sbgeom;

static void geom(const fb_rect *r, bool vert, const ui_sb *s, sbgeom *g) {
    int track;
    g->len = vert ? r->bottom - r->top : r->right - r->left;
    g->a = g->len < 2 * SB_BTN ? g->len / 2 : SB_BTN;
    g->b = g->len - g->a;
    track = g->b - g->a;
    g->thumb = track >= SB_BTN && s->max > s->min && !(s->dis_lo && s->dis_hi);
    if (g->thumb) {
        g->th0 = g->a + (int)((int64_t)(s->pos - s->min) * (track - SB_BTN) / (s->max - s->min));
        g->th1 = g->th0 + SB_BTN;
    } else {
        g->th0 = g->th1 = g->a;
    }
}

/* The part under (x, y), given in the coordinates of r. */
static int hit(const fb_rect *r, bool vert, const ui_sb *s, int x, int y) {
    sbgeom g;
    int v = vert ? y - r->top : x - r->left;
    int across = vert ? x - r->left : y - r->top;
    int total = vert ? r->right - r->left : r->bottom - r->top;
    geom(r, vert, s, &g);
    if (across < 0 || across >= total || v < 0 || v >= g.len) return P_NONE;
    if (v < g.a) return P_LO;
    if (v >= g.b) return P_HI;
    if (!g.thumb) return P_NONE;
    if (v < g.th0) return P_PAGE_LO;
    if (v >= g.th1) return P_PAGE_HI;
    return P_THUMB;
}

static void bevel_button(user_t *u, uint16_t dc, int l, int t, int r, int b, bool down) {
    ui_fill(u, dc, l, t, r, b, wm_sys_color(15));
    if (down) {
        ui_outline(u, dc, l, t, r, b, wm_sys_color(16));
    } else {
        ui_fill(u, dc, l, t, r - 1, t + 1, wm_sys_color(20));
        ui_fill(u, dc, l, t, l + 1, b - 1, wm_sys_color(20));
        ui_fill(u, dc, l + 1, b - 2, r - 1, b - 1, wm_sys_color(16));
        ui_fill(u, dc, r - 2, t + 1, r - 1, b - 1, wm_sys_color(16));
        ui_fill(u, dc, l, b - 1, r, b, wm_sys_color(6));
        ui_fill(u, dc, r - 1, t, r, b, wm_sys_color(6));
    }
}

static void arrow(user_t *u, uint16_t dc, int l, int t, int r, int b, int dir, uint32_t c) {
    int cx = (l + r) / 2, cy = (t + b) / 2, k;
    for (k = 0; k < 4; k++) {
        switch (dir) {
        case 0: ui_fill(u, dc, cx - k, cy - 2 + k, cx + k + 1, cy - 1 + k, c); break;      /* up */
        case 1: ui_fill(u, dc, cx - k, cy + 1 - k, cx + k + 1, cy + 2 - k, c); break;      /* down */
        case 2: ui_fill(u, dc, cx - 2 + k, cy - k, cx - 1 + k, cy + k + 1, c); break;      /* left */
        default: ui_fill(u, dc, cx + 1 - k, cy - k, cx + 2 - k, cy + k + 1, c); break;     /* right */
        }
    }
}

void ui_sb_draw(user_t *u, uint16_t dc, const fb_rect *r, bool vert, const ui_sb *s, int pressed) {
    sbgeom g;
    bool off_lo = s->dis_lo || s->max <= s->min, off_hi = s->dis_hi || s->max <= s->min;
    int x0 = r->left, y0 = r->top, x1 = r->right, y1 = r->bottom;
    geom(r, vert, s, &g);
    if (vert) {
        ui_fill(u, dc, x0, y0 + g.a, x1, y0 + g.b, wm_sys_color(0));
        bevel_button(u, dc, x0, y0, x1, y0 + g.a, pressed == P_LO);
        bevel_button(u, dc, x0, y0 + g.b, x1, y1, pressed == P_HI);
        arrow(u, dc, x0, y0, x1, y0 + g.a, 0, wm_sys_color(off_lo ? 16 : 6));
        arrow(u, dc, x0, y0 + g.b, x1, y1, 1, wm_sys_color(off_hi ? 16 : 6));
        if (g.thumb) bevel_button(u, dc, x0, y0 + g.th0, x1, y0 + g.th1, false);
    } else {
        ui_fill(u, dc, x0 + g.a, y0, x0 + g.b, y1, wm_sys_color(0));
        bevel_button(u, dc, x0, y0, x0 + g.a, y1, pressed == P_LO);
        bevel_button(u, dc, x0 + g.b, y0, x1, y1, pressed == P_HI);
        arrow(u, dc, x0, y0, x0 + g.a, y1, 2, wm_sys_color(off_lo ? 16 : 6));
        arrow(u, dc, x0 + g.b, y0, x1, y1, 3, wm_sys_color(off_hi ? 16 : 6));
        if (g.thumb) bevel_button(u, dc, x0 + g.th0, y0, x0 + g.th1, y1, false);
    }
}

/* ---- tracking ----------------------------------------------------------------------------------- */

static void send_scroll(const sbtrack *t, unsigned code, int pos) {
    wm_send(&t->u->wm, t->notify, t->vert ? WM_VSCROLL : WM_HSCROLL, (uint16_t)code,
            (uint32_t)t->ctl << 16 | (uint16_t)pos);
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void ui_sb_track(sbtrack *t, int sx, int sy) {
    user_t *u = t->u;
    ui_sb *s = t->sb;
    fb_rect r = t->rect;
    int part = hit(&r, t->vert, s, sx, sy), start_pos = s->pos, grab = 0;
    uint32_t next = 0;
    bool done = false, first = true;
    sbgeom g;
    uint16_t old = wm_set_capture(&u->wm, t->notify);
    if (part == P_NONE || s->max <= s->min) {
        if (old) wm_set_capture(&u->wm, old);
        else wm_release_capture(&u->wm);
        return;
    }
    geom(&r, t->vert, s, &g);
    if (part == P_THUMB) grab = (t->vert ? sy - r.top : sx - r.left) - g.th0;
    next = 0;
    while (!done && !u->abort) {
        bool dis = (part == P_LO && s->dis_lo) || (part == P_HI && s->dis_hi);
        int cur = hit(&r, t->vert, s, sx, sy);
        wm_msg m;
        if (part != P_THUMB && !dis && (next == 0 || (int32_t)(user_now(u) - next) >= 0) && cur == part) {
            unsigned code = part == P_LO ? SB_LINEUP : part == P_HI ? SB_LINEDOWN : part == P_PAGE_LO ? SB_PAGEUP : SB_PAGEDOWN;
            send_scroll(t, code, s->pos);
            next = user_now(u) + (first ? 400 : 80);
            first = false;
            t->redraw(t->rctx, part);
        }
        if (!wm_next_message(&u->wm, &m, 0, WM_MOUSEMOVE, 0x0209, true) &&
            !wm_next_message(&u->wm, &m, 0, 0x00A0, 0x00A9, true)) {
            if (!user_idle(u, true)) {
                u->abort = loader_request_stop(u->p);
                break;
            }
            continue;
        }
        sx = m.pt_x;
        sy = m.pt_y;
        if (m.message >= 0xA0 && m.message <= 0xA9) m.message = (uint16_t)(m.message - 0xA0 + WM_MOUSEMOVE);   /* NC twins */
        if (m.message == WM_MOUSEMOVE && part == P_THUMB) {
            int track = g.b - g.a - SB_BTN, v = (t->vert ? sy - r.top : sx - r.left) - grab - g.a, np;
            if (track <= 0) track = 1;
            np = s->min + (int)((int64_t)clamp(v, 0, track) * (s->max - s->min) / track);
            if (np != s->pos) {
                s->pos = np;
                send_scroll(t, SB_THUMBTRACK, np);
                t->redraw(t->rctx, part);
            }
        } else if (m.message == WM_LBUTTONUP) {
            done = true;
        } else if (m.message == WM_MOUSEMOVE) {
            t->redraw(t->rctx, hit(&r, t->vert, s, sx, sy) == part ? part : P_NONE);
        }
    }
    if (part == P_THUMB && !u->abort) send_scroll(t, SB_THUMBPOSITION, s->pos);
    (void)start_pos;
    if (!u->abort) send_scroll(t, SB_ENDSCROLL, s->pos);
    if (old && wm_is_window(&u->wm, old)) wm_set_capture(&u->wm, old);
    else wm_release_capture(&u->wm);
    if (!u->abort) t->redraw(t->rctx, P_NONE);
}

/* ---- scroll bars of windows -------------------------------------------------------------------------- */

bool ui_nc_sb_rect(user_t *u, uint16_t hwnd, bool vert, fb_rect *r) {
    wm_window *x = wm_get(&u->wm, hwnd);
    nc_geom g;
    int l, t, rr, b;
    bool hs, vs;
    if (!x || !ui_nc_geom(u, hwnd, &g) || !wm_insets(&u->wm, hwnd, &l, &t, &rr, &b)) return false;
    hs = (x->style & WS_HSCROLL) != 0;
    vs = (x->style & WS_VSCROLL) != 0;
    if (vert) {
        if (!vs) return false;
        *r = (fb_rect){g.w - g.f - SB_BTN, t, g.w - g.f, g.h - g.f - (hs ? SB_BTN : 0)};
    } else {
        if (!hs) return false;
        *r = (fb_rect){l, g.h - g.f - SB_BTN, g.w - g.f - (vs ? SB_BTN : 0), g.h - g.f};
    }
    return true;
}

void ui_nc_scrollbars(user_t *u, uint16_t dc, uint16_t hwnd) {
    ui_win *uw = ui_win_get(u->ui, hwnd, true);
    fb_rect r;
    nc_geom g;
    if (!uw || !ui_nc_geom(u, hwnd, &g)) return;
    if (ui_nc_sb_rect(u, hwnd, true, &r)) ui_sb_draw(u, dc, &r, true, &uw->sb[SB_VERT], P_NONE);
    if (ui_nc_sb_rect(u, hwnd, false, &r)) ui_sb_draw(u, dc, &r, false, &uw->sb[SB_HORZ], P_NONE);
    {
        wm_window *x = wm_get(&u->wm, hwnd);
        if (x && (x->style & WS_HSCROLL) && (x->style & WS_VSCROLL))
            ui_fill(u, dc, g.w - g.f - SB_BTN, g.h - g.f - SB_BTN, g.w - g.f, g.h - g.f, wm_sys_color(15));
    }
}

static void nc_redraw(void *ctx, int pressed) {
    sbtrack *t = ctx;
    (void)pressed;
    ui_ncpaint(t->u, t->notify);
}

void ui_nc_sb_press(user_t *u, uint16_t hwnd, bool vert, uint32_t lp) {
    ui_win *uw = ui_win_get(u->ui, hwnd, true);
    fb_rect r, wr;
    sbtrack t;
    if (!uw || !ui_nc_sb_rect(u, hwnd, vert, &r) || !wm_window_rect(&u->wm, hwnd, &wr)) return;
    memset(&t, 0, sizeof t);
    t.u = u;
    t.notify = hwnd;
    t.ctl = 0;
    t.vert = vert;
    t.sb = &uw->sb[vert ? SB_VERT : SB_HORZ];
    t.rect = (fb_rect){wr.left + r.left, wr.top + r.top, wr.left + r.right, wr.top + r.bottom};
    t.redraw = nc_redraw;
    t.rctx = &t;
    ui_sb_track(&t, (int16_t)lp, (int16_t)(lp >> 16));
}

/* ---- API ------------------------------------------------------------------------------------------------- */

ui_sb *ui_sb_of(user_t *u, uint16_t hwnd, int bar) {
    ui_win *uw;
    if (!wm_is_window(&u->wm, hwnd)) return NULL;
    uw = ui_win_get(u->ui, hwnd, true);
    if (!uw) return NULL;
    if (bar == SB_HORZ || bar == SB_VERT) return &uw->sb[bar];
    if (bar == SB_CTL) {
        wm_window *x = wm_get(&u->wm, hwnd);
        return uw->ctl && uw->ctl->kind == CK_SCROLL && x ? &uw->ctl->sb : NULL;
    }
    return NULL;
}

static void sb_changed(user_t *u, uint16_t hwnd, int bar) {
    if (bar == SB_CTL) {
        wm_invalidate(&u->wm, hwnd, NULL, false);
        wm_update_window(&u->wm, hwnd);
    } else if (wm_is_visible(&u->wm, hwnd)) {
        ui_ncpaint(u, hwnd);
    }
}

static cpu_status u_setscrollpos(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a);
    int bar = (int16_t)hle_u16(&a), pos = (int16_t)hle_u16(&a);
    uint16_t redraw = hle_u16(&a);
    ui_sb *s;
    UCHECK;
    if (!(s = ui_sb_of(u, hwnd, bar))) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    hle_ret16(c, (uint16_t)s->pos);
    s->pos = pos < s->min ? s->min : pos > s->max ? s->max : pos;
    if (redraw) sb_changed(u, hwnd, bar);
    return u->abort;
}

static cpu_status u_getscrollpos(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a);
    int bar = (int16_t)hle_u16(&a);
    ui_sb *s;
    UCHECK;
    s = ui_sb_of(u, hwnd, bar);
    hle_ret16(c, s ? (uint16_t)s->pos : 0);
    return CPU_OK;
}

static cpu_status u_setscrollrange(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a);
    int bar = (int16_t)hle_u16(&a), lo = (int16_t)hle_u16(&a), hi = (int16_t)hle_u16(&a);
    uint16_t redraw = hle_u16(&a);
    ui_sb *s;
    UCHECK;
    if ((s = ui_sb_of(u, hwnd, bar))) {
        s->min = lo;
        s->max = hi < lo ? lo : hi;
        s->pos = s->pos < s->min ? s->min : s->pos > s->max ? s->max : s->pos;
        if (redraw) sb_changed(u, hwnd, bar);
    }
    hle_ret16(c, s != NULL);
    return u->abort;
}

static cpu_status u_getscrollrange(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a), s1, o1, s2, o2;
    int bar = (int16_t)hle_u16(&a);
    ui_sb *s;
    uint8_t *p;
    hle_far(&a, &s1, &o1);
    hle_far(&a, &s2, &o2);
    UCHECK;
    s = ui_sb_of(u, hwnd, bar);
    if (s) {
        if (s1 && (p = hle_ptr(c, s1, o1, 2))) st16(p, (uint16_t)s->min);
        if (s2 && (p = hle_ptr(c, s2, o2, 2))) st16(p, (uint16_t)s->max);
    }
    hle_ret16(c, s != NULL);
    return CPU_OK;
}

static cpu_status u_showscrollbar(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a), bar = hle_u16(&a), show = hle_u16(&a);
    wm_window *x;
    UCHECK;
    x = wm_get(&u->wm, hwnd);
    hle_ret16(c, 0);
    if (!x) return CPU_OK;
    if (bar == SB_CTL) {
        wm_show_window(&u->wm, hwnd, show ? SW_SHOWNA : SW_HIDE);
    } else {
        uint32_t bits = bar == SB_HORZ ? WS_HSCROLL : bar == SB_VERT ? WS_VSCROLL : (WS_HSCROLL | WS_VSCROLL);
        uint32_t old = x->style;
        x->style = show ? x->style | bits : x->style & ~bits;
        if (x->style != old) wm_frame_changed(&u->wm, hwnd);
    }
    hle_ret16(c, 1);
    return u->abort;
}

static cpu_status u_enablescrollbar(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a), bar = hle_u16(&a), arrows = hle_u16(&a);
    int b;
    bool any = false;
    UCHECK;
    for (b = SB_HORZ; b <= SB_CTL; b++) {
        ui_sb *s;
        if (!(bar == b || (bar == SB_BOTH && b < SB_CTL)) || !(s = ui_sb_of(u, hwnd, b))) continue;
        s->dis_lo = (arrows & 1) != 0;       /* ESB_DISABLE_LTUP */
        s->dis_hi = (arrows & 2) != 0;       /* ESB_DISABLE_RTDN */
        sb_changed(u, hwnd, b);
        any = true;
    }
    hle_ret16(c, any);
    return u->abort;
}

const hle_def ui_scroll_defs[] = {
    {USER_MODULE, 62, "SetScrollPos", 8, u_setscrollpos},              /* UNV */
    {USER_MODULE, 63, "GetScrollPos", 4, u_getscrollpos},              /* UNV */
    {USER_MODULE, 64, "SetScrollRange", 10, u_setscrollrange},         /* UNV */
    {USER_MODULE, 65, "GetScrollRange", 12, u_getscrollrange},         /* UNV */
    {USER_MODULE, 265, "ShowScrollBar", 6, u_showscrollbar},           /* UNV */
    {USER_MODULE, 482, "EnableScrollBar", 6, u_enablescrollbar},       /* UNV */
};
const size_t ui_scroll_ndefs = sizeof ui_scroll_defs / sizeof ui_scroll_defs[0];
