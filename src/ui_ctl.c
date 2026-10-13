/* The built-in window classes: dispatch, and BUTTON, STATIC and SCROLLBAR. EDIT lives in ui_edit.c, LISTBOX and
 * COMBOBOX in ui_list.c, the dialog class in ui_dlg.c. Controls draw themselves with the system colours (asking
 * the parent through WM_CTLCOLOR first) in Windows 3.1's look and notify their parent with WM_COMMAND. */
#include <string.h>

#include "ui_priv.h"

#define BM_GETCHECK 0x0400
#define BM_SETCHECK 0x0401
#define BM_GETSTATE 0x0402
#define BM_SETSTATE 0x0403
#define BM_SETSTYLE 0x0404
#define BN_CLICKED 0
#define SBM_SETPOS 0x0400
#define SBM_GETPOS 0x0401
#define SBM_SETRANGE 0x0402
#define SBM_GETRANGE 0x0403
#define SBM_ENABLE_ARROWS 0x0404

#define BS_PUSHBUTTON 0
#define BS_DEFPUSHBUTTON 1
#define BS_CHECKBOX 2
#define BS_AUTOCHECKBOX 3
#define BS_RADIOBUTTON 4
#define BS_3STATE 5
#define BS_AUTO3STATE 6
#define BS_GROUPBOX 7
#define BS_USERBUTTON 8
#define BS_AUTORADIOBUTTON 9
#define BS_PUSHBOX 10
#define BS_LEFTTEXT 0x20


#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031

static ck_kind kind_of(const char *name) {
    static const struct { const char *n; ck_kind k; } t[] = {{"BUTTON", CK_BUTTON}, {"STATIC", CK_STATIC}, {"EDIT", CK_EDIT},
                                                            {"LISTBOX", CK_LISTBOX}, {"COMBOBOX", CK_COMBO}, {"SCROLLBAR", CK_SCROLL}};
    size_t i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        const char *a = name, *b = t[i].n;
        while (*a && *b && (*a & ~0x20) == *b) a++, b++;
        if (!*a && !*b) return t[i].k;
    }
    return CK_NONE;
}

static void repaint(user_t *u, uint16_t hwnd) {
    wm_invalidate(&u->wm, hwnd, NULL, false);
}

/* Immediate repaint, for state changes the program asks for (BM_SETCHECK, SBM_SETPOS...). */
static void repaint_now(user_t *u, uint16_t hwnd) {
    wm_invalidate(&u->wm, hwnd, NULL, false);
    if (wm_is_visible(&u->wm, hwnd)) wm_update_window(&u->wm, hwnd);
}

/* ---- text layout shared by statics ---------------------------------------------------------------- */

/* Splits s (already without mnemonic markers) into lines no wider than width (words break at spaces). */
int ui_wrap_text(user_t *u, int cell, const char *s, int width, bool allow_wrap, tline *out, int max) {
    int n = 0, i = 0, len = (int)strlen(s);
    while (i <= len && n < max) {
        int st = i, last_space = -1, j = i;
        for (; j < len && s[j] != '\n'; j++) {
            if (s[j] == '\r') continue;
            if (s[j] == ' ') last_space = j;
            if (allow_wrap && ui_tw(u, cell, s + st, j - st + 1) > width && j > st) {
                if (last_space > st) j = last_space;      /* break at the last space; else mid-word */
                break;
            }
        }
        out[n].start = st;
        out[n].len = j - st;
        while (out[n].len > 0 && (s[st + out[n].len - 1] == '\r')) out[n].len--;
        n++;
        if (j >= len) break;
        i = j + 1;                                         /* skip the space or newline we broke on */
        if (s[j] != ' ' && s[j] != '\n') i = j;
    }
    return n;
}

/* Text without '&' markers. ul receives the index of the underlined character (-1 none). */
static void strip_prefix(const char *in, char *out, size_t cap, int *ul, bool keep) {
    size_t n = 0;
    *ul = -1;
    for (; *in && n + 1 < cap; in++) {
        if (!keep && *in == '&') {
            if (in[1] == '&') in++;
            else if (*ul < 0) {
                *ul = (int)n;
                continue;
            } else continue;
        }
        out[n++] = *in;
    }
    out[n] = 0;
}

static void draw_lines(user_t *u, uint16_t dc, int cell, const char *s, const tline *ln, int n, int ul, int align, int w, int y,
                       uint32_t fg, uint32_t bg, bool disabled) {
    int i, th = ui_th(u, cell);
    for (i = 0; i < n; i++) {
        int tw = ui_tw(u, cell, s + ln[i].start, ln[i].len), x = align == 1 ? (w - tw) / 2 : align == 2 ? w - tw - 1 : 0;
        if (disabled) ui_text(u, dc, cell, x + 1, y + 1, s + ln[i].start, ln[i].len, wm_sys_color(20), bg);
        ui_text(u, dc, cell, x, y, s + ln[i].start, ln[i].len, disabled ? wm_sys_color(16) : fg, bg);
        if (ul >= ln[i].start && ul < ln[i].start + ln[i].len) {
            int x0 = x + ui_tw(u, cell, s + ln[i].start, ul - ln[i].start), x1 = x0 + ui_tw(u, cell, s + ul, 1);
            ui_fill(u, dc, x0, y + th - 2, x1, y + th - 1, disabled ? wm_sys_color(16) : fg);
        }
        y += th;
    }
}

/* ---- BUTTON ----------------------------------------------------------------------------------------- */

static void button_click(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c) {
    int type = x->style & 0xF;
    switch (type) {
    case BS_AUTOCHECKBOX: c->check = !c->check; break;
    case BS_AUTO3STATE: c->check = (c->check + 1) % 3; break;
    case BS_AUTORADIOBUTTON: {
        uint16_t sib[64], k;
        int n = 0, me = -1, lo, hi, i;
        wm_window *p = wm_get(&u->wm, x->parent);
        for (k = p ? p->first_child : 0; k && n < 64;) {            /* siblings in Z order = tab order */
            wm_window *s = wm_get(&u->wm, k);
            if (!s) break;
            if (k == hwnd) me = n;
            sib[n++] = k;
            k = s->next;
        }
        if (me >= 0) {
            for (lo = me; lo > 0 && !(wm_get(&u->wm, sib[lo])->style & WS_GROUP_); lo--) {}
            for (hi = me + 1; hi < n && !(wm_get(&u->wm, sib[hi])->style & WS_GROUP_); hi++) {}
            for (i = lo; i < hi; i++) {                              /* the group: from a WS_GROUP window to the next one */
                wm_window *s = wm_get(&u->wm, sib[i]);
                ui_win *sw = ui_win_get(u->ui, sib[i], false);
                int st = (int)(s->style & 0xF);
                if (sib[i] != hwnd && sw && sw->ctl && sw->ctl->kind == CK_BUTTON && (st == BS_AUTORADIOBUTTON || st == BS_RADIOBUTTON)) {
                    sw->ctl->check = 0;
                    repaint(u, sib[i]);
                }
            }
        }
        c->check = 1;
        break;
    }
    default: break;
    }
    repaint(u, hwnd);
    ui_notify_parent(u, hwnd, BN_CLICKED);
}

static void button_paint(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, uint16_t dc, int w, int h) {
    int type = x->style & 0xF, cell = ui_cell_of(u, hwnd), th = ui_th(u, cell);
    bool dis = (x->style & WS_DISABLED) != 0, focus = u->wm.focus == hwnd, down = c->pressed && c->inside;
    uint32_t fg, bg;
    char disp[256];
    tline ln[8];
    int n, ul;
    ui_ctlcolors(u, hwnd, dc, 3, &fg, &bg);
    strip_prefix(wm_get_text(&u->wm, hwnd), disp, sizeof disp, &ul, false);
    if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON || type == BS_USERBUTTON || type == BS_PUSHBOX) {
        int l = 0, t = 0, r = w, b = h, tw, ty;   /* tw: set with the text below */
        uint32_t blk = wm_sys_color(6), face = wm_sys_color(15), hi = wm_sys_color(20), sh = wm_sys_color(16);
        if (type == BS_DEFPUSHBUTTON && !dis) {                       /* the default button has a second border */
            ui_fill(u, dc, l + 1, t, r - 1, t + 1, blk);
            ui_fill(u, dc, l + 1, b - 1, r - 1, b, blk);
            ui_fill(u, dc, l, t + 1, l + 1, b - 1, blk);
            ui_fill(u, dc, r - 1, t + 1, r, b - 1, blk);
            l++; t++; r--; b--;
        }
        ui_fill(u, dc, l + 1, t, r - 1, t + 1, blk);                  /* rounded border: the corners stay clear */
        ui_fill(u, dc, l + 1, b - 1, r - 1, b, blk);
        ui_fill(u, dc, l, t + 1, l + 1, b - 1, blk);
        ui_fill(u, dc, r - 1, t + 1, r, b - 1, blk);
        ui_fill(u, dc, l + 1, t + 1, r - 1, b - 1, face);
        if (down) {
            ui_fill(u, dc, l + 1, t + 1, r - 1, t + 2, sh);
            ui_fill(u, dc, l + 1, t + 1, l + 2, b - 1, sh);
        } else {
            ui_fill(u, dc, l + 1, t + 1, r - 2, t + 2, hi);
            ui_fill(u, dc, l + 1, t + 1, l + 2, b - 2, hi);
            ui_fill(u, dc, l + 2, b - 3, r - 1, b - 1, sh);
            ui_fill(u, dc, r - 3, t + 2, r - 1, b - 1, sh);
        }
        n = ui_wrap_text(u, cell, disp, r - l - 4, false, ln, 8);
        if (n) {
            tw = ui_tw(u, cell, disp + ln[0].start, ln[0].len);
            int tx = l + (r - l - tw) / 2 + (down ? 1 : 0);
            ty = t + (b - t - th) / 2 + (down ? 1 : 0);
            if (dis) ui_text(u, dc, cell, tx + 1, ty + 1, disp, ln[0].len, hi, 0xFFFFFFFFu);
            ui_text(u, dc, cell, tx, ty, disp, ln[0].len, dis ? sh : fg, 0xFFFFFFFFu);
            if (ul >= 0 && ul < ln[0].len)
                ui_fill(u, dc, tx + ui_tw(u, cell, disp, ul), ty + th - 2, tx + ui_tw(u, cell, disp, ul) + ui_tw(u, cell, disp + ul, 1),
                        ty + th - 1, dis ? sh : fg);
            if (focus) ui_dotted(u, dc, tx - 3, ty - 1, tx + tw + 3, ty + th + 1);
        }
        return;
    }
    if (type == BS_GROUPBOX) {
        int y0 = th / 2, tw = ui_tw(u, cell, disp, (int)strlen(disp));
        ui_outline(u, dc, 0, y0, w - 1, h - 1, wm_sys_color(16));
        ui_outline(u, dc, 1, y0 + 1, w, h, wm_sys_color(20));
        if (disp[0]) {
            ui_fill(u, dc, 6, y0 - 1, 10 + tw, y0 + 2, bg);
            ui_text(u, dc, cell, 8, 0, disp, (int)strlen(disp), dis ? wm_sys_color(16) : fg, bg);
        }
        return;
    }
    /* check boxes and radio buttons: box 12x12, caption to its right (or left with BS_LEFTTEXT) */
    {
        bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON, left = (x->style & BS_LEFTTEXT) != 0;
        int bx = left ? w - 12 : 0, by = (h - 12) / 2, tx = left ? 0 : 18, tw;
        uint32_t blk = wm_sys_color(6), interior = down || dis ? wm_sys_color(15) : wm_sys_color(5), mark = dis ? wm_sys_color(16) : blk;
        static const int rw[12] = {4, 8, 10, 10, 12, 12, 12, 12, 10, 10, 8, 4};
        int i;
        ui_fill(u, dc, 0, 0, w, h, bg);
        if (!radio) {
            ui_fill(u, dc, bx, by, bx + 12, by + 12, blk);
            ui_fill(u, dc, bx + 1, by + 1, bx + 11, by + 11, c->check == 2 ? wm_sys_color(15) : interior);
            if (c->check) {
                for (i = 0; i < 8; i++) {
                    ui_fill(u, dc, bx + 2 + i, by + 2 + i, bx + 4 + i, by + 3 + i, c->check == 2 ? wm_sys_color(16) : mark);
                    ui_fill(u, dc, bx + 9 - i, by + 2 + i, bx + 11 - i, by + 3 + i, c->check == 2 ? wm_sys_color(16) : mark);
                }
            }
        } else {
            for (i = 0; i < 12; i++) ui_fill(u, dc, bx + (12 - rw[i]) / 2, by + i, bx + (12 + rw[i]) / 2, by + i + 1, blk);
            for (i = 1; i < 11; i++) {
                int ww = rw[i] - 2;
                ui_fill(u, dc, bx + (12 - ww) / 2, by + i, bx + (12 + ww) / 2, by + i + 1, interior);
            }
            if (c->check) {
                static const int dw[4] = {2, 4, 4, 2};
                for (i = 0; i < 4; i++) ui_fill(u, dc, bx + 6 - dw[i] / 2, by + 4 + i, bx + 6 + dw[i] / 2, by + 5 + i, mark);
            }
        }
        n = ui_wrap_text(u, cell, disp, w - 18, false, ln, 8);
        tw = n ? ui_tw(u, cell, disp, ln[0].len) : 0;
        if (n) {
            int ty = (h - th) / 2;
            if (dis) ui_text(u, dc, cell, tx + 1, ty + 1, disp, ln[0].len, wm_sys_color(20), bg);
            ui_text(u, dc, cell, tx, ty, disp, ln[0].len, dis ? wm_sys_color(16) : fg, bg);
            if (ul >= 0 && ul < ln[0].len)
                ui_fill(u, dc, tx + ui_tw(u, cell, disp, ul), ty + th - 2, tx + ui_tw(u, cell, disp, ul) + ui_tw(u, cell, disp + ul, 1),
                        ty + th - 1, dis ? wm_sys_color(16) : fg);
            if (focus) ui_dotted(u, dc, tx - 2, ty - 1, tx + tw + 2, ty + th + 1);
        }
    }
}

static uint32_t button_proc(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, uint16_t msg, uint16_t wp, uint32_t lp, bool *handled) {
    int type = x->style & 0xF;
    *handled = true;
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (x->style & WS_DISABLED) return 0;
        if (type != BS_GROUPBOX) {
            wm_set_focus(&u->wm, hwnd);
            wm_set_capture(&u->wm, hwnd);
            c->pressed = c->inside = true;
            repaint_now(u, hwnd);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (c->pressed && u->wm.capture == hwnd) {
            fb_rect cl;
            int px = (int16_t)lp, py = (int16_t)(lp >> 16);
            bool in;
            wm_client_rect(&u->wm, hwnd, &cl);
            in = px >= 0 && py >= 0 && px < cl.right && py < cl.bottom;
            if (in != c->inside) {
                c->inside = in;
                repaint_now(u, hwnd);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (c->pressed) {
            bool click = c->inside;
            c->pressed = false;
            if (u->wm.capture == hwnd) wm_release_capture(&u->wm);
            repaint_now(u, hwnd);
            if (click && !u->abort) button_click(u, hwnd, x, c);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == ' ' && !(x->style & WS_DISABLED) && type != BS_GROUPBOX) {
            c->pressed = c->inside = true;
            repaint_now(u, hwnd);
            return 0;
        }
        *handled = false;
        return 0;
    case WM_KEYUP:
        if (wp == ' ' && c->pressed) {
            c->pressed = false;
            repaint_now(u, hwnd);
            button_click(u, hwnd, x, c);
            return 0;
        }
        *handled = false;
        return 0;
    case WM_SETFOCUS:
        ui_dlg_btn_focus(u, hwnd, true, wp);
        repaint(u, hwnd);
        return 0;
    case WM_ENABLE:
        repaint(u, hwnd);
        return 0;
    case WM_KILLFOCUS:
        c->pressed = false;
        ui_dlg_btn_focus(u, hwnd, false, wp);
        repaint(u, hwnd);
        return 0;
    case WM_GETDLGCODE:
        if (type == BS_DEFPUSHBUTTON) return DLGC_BUTTON | DLGC_DEFPUSHBUTTON;
        if (type == BS_PUSHBUTTON || type == BS_USERBUTTON || type == BS_PUSHBOX) return DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON;
        if (type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON) return DLGC_BUTTON | DLGC_RADIOBUTTON;
        if (type == BS_GROUPBOX) return DLGC_STATIC;
        return DLGC_BUTTON;
    case BM_GETCHECK: return (uint32_t)c->check;
    case BM_SETCHECK:
        c->check = wp > 2 ? 1 : (int)wp;
        if (type != BS_AUTO3STATE && type != BS_3STATE && c->check == 2) c->check = 1;
        repaint_now(u, hwnd);
        return 0;
    case BM_GETSTATE: return (uint32_t)(c->check | (c->pressed ? 4 : 0) | (u->wm.focus == hwnd ? 8 : 0));
    case BM_SETSTATE:
        c->pressed = c->inside = wp != 0;
        repaint_now(u, hwnd);
        return 0;
    case BM_SETSTYLE:
        x->style = (x->style & 0xFFFF0000u) | wp;
        repaint_now(u, hwnd);
        return 0;
    case WM_PAINT: {
        wm_paint ps;
        fb_rect cl;
        uint16_t dc = wm_begin_paint(&u->wm, hwnd, &ps);
        if (dc) {
            wm_client_rect(&u->wm, hwnd, &cl);
            button_paint(u, hwnd, x, c, dc, cl.right, cl.bottom);
            wm_end_paint(&u->wm, hwnd, &ps);
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    default: *handled = false; return 0;
    }
}

/* ---- STATIC ------------------------------------------------------------------------------------------ */

static uint32_t static_proc(user_t *u, uint16_t hwnd, wm_window *x, uint16_t msg, uint16_t wp, bool *handled) {
    *handled = true;
    (void)wp;
    switch (msg) {
    case WM_GETDLGCODE: return DLGC_STATIC;
    case WM_ENABLE: repaint(u, hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        wm_paint ps;
        fb_rect cl;
        uint16_t dc = wm_begin_paint(&u->wm, hwnd, &ps);
        int type = x->style & 0x0F, cell, th, w, h;
        uint32_t fg, bg;
        if (!dc) return 0;
        wm_client_rect(&u->wm, hwnd, &cl);
        w = cl.right;
        h = cl.bottom;
        cell = ui_cell_of(u, hwnd);
        th = ui_th(u, cell);
        ui_ctlcolors(u, hwnd, dc, 6, &fg, &bg);
        switch (type) {
        case 4: ui_fill(u, dc, 0, 0, w, h, wm_sys_color(6)); break;
        case 5: ui_fill(u, dc, 0, 0, w, h, wm_sys_color(1)); break;
        case 6: ui_fill(u, dc, 0, 0, w, h, wm_sys_color(5)); break;
        case 7: ui_outline(u, dc, 0, 0, w, h, wm_sys_color(6)); break;
        case 8: ui_outline(u, dc, 0, 0, w, h, wm_sys_color(1)); break;
        case 9: ui_outline(u, dc, 0, 0, w, h, wm_sys_color(5)); break;
        case 3: {
            uint16_t hi = ui_icon_from_text(u, x->hinstance, wm_get_text(&u->wm, hwnd));
            ui_image *im = ui_image_get(u->ui, hi);
            ui_fill(u, dc, 0, 0, w, h, bg);
            if (im) ui_draw_image(u, dc, 0, 0, im);
            break;
        }
        case 10: break;
        default: {
            char disp[512];
            tline ln[40];
            int n, ul;
            bool noprefix = (x->style & 0x80) != 0;
            ui_fill(u, dc, 0, 0, w, h, bg);
            strip_prefix(wm_get_text(&u->wm, hwnd), disp, sizeof disp, &ul, noprefix);
            n = ui_wrap_text(u, cell, disp, w, type <= 2, ln, 40);
            if (n * th > h && h >= th) n = (h + th - 1) / th;
            draw_lines(u, dc, cell, disp, ln, n, ul, type == 1 ? 1 : type == 2 ? 2 : 0, w, 0, fg, bg, (x->style & WS_DISABLED) != 0);
            break;
        }
        }
        wm_end_paint(&u->wm, hwnd, &ps);
        return 0;
    }
    default: *handled = false; return 0;
    }
}

/* ---- SCROLLBAR ------------------------------------------------------------------------------------------- */

static void sbctl_redraw(void *ctx, int pressed) {
    sbtrack *t = ctx;
    (void)pressed;
    repaint_now(t->u, t->ctl);
}

static uint32_t scroll_proc(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, uint16_t msg, uint16_t wp, uint32_t lp, bool *handled) {
    bool vert = (x->style & 1) != 0;
    *handled = true;
    switch (msg) {
    case SBM_SETPOS: {
        int old = c->sb.pos;
        c->sb.pos = (int16_t)wp < c->sb.min ? c->sb.min : (int16_t)wp > c->sb.max ? c->sb.max : (int16_t)wp;
        if (lp & 0xFFFF) repaint_now(u, hwnd);
        return (uint32_t)(uint16_t)old;
    }
    case SBM_GETPOS: return (uint32_t)(uint16_t)c->sb.pos;
    case SBM_SETRANGE:
        c->sb.min = (int16_t)lp;
        c->sb.max = (int16_t)(lp >> 16);
        if (c->sb.max < c->sb.min) c->sb.max = c->sb.min;
        c->sb.pos = c->sb.pos < c->sb.min ? c->sb.min : c->sb.pos > c->sb.max ? c->sb.max : c->sb.pos;
        if (wp) repaint_now(u, hwnd);
        return 0;
    case SBM_GETRANGE: return (uint32_t)(uint16_t)c->sb.min | (uint32_t)(uint16_t)c->sb.max << 16;
    case SBM_ENABLE_ARROWS:
        c->sb.dis_lo = (wp & 1) != 0;
        c->sb.dis_hi = (wp & 2) != 0;
        repaint_now(u, hwnd);
        return 1;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        sbtrack t;
        fb_rect cl, wr;
        int sx = (int16_t)lp, sy = (int16_t)(lp >> 16), ox, oy;
        if (x->style & WS_DISABLED) return 0;
        wm_client_origin(&u->wm, hwnd, &ox, &oy);
        wm_client_rect(&u->wm, hwnd, &cl);
        (void)wr;
        memset(&t, 0, sizeof t);
        t.u = u;
        t.notify = x->parent;
        t.ctl = hwnd;
        t.vert = vert;
        t.sb = &c->sb;
        t.rect = (fb_rect){ox, oy, ox + cl.right, oy + cl.bottom};
        t.redraw = sbctl_redraw;
        t.rctx = &t;
        ui_sb_track(&t, ox + sx, oy + sy);
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_ENABLE: repaint(u, hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        wm_paint ps;
        fb_rect cl;
        uint16_t dc = wm_begin_paint(&u->wm, hwnd, &ps);
        if (dc) {
            wm_client_rect(&u->wm, hwnd, &cl);
            ui_sb_draw(u, dc, &cl, vert, &c->sb, 0);
            wm_end_paint(&u->wm, hwnd, &ps);
        }
        return 0;
    }
    default: *handled = false; return 0;
    }
}

/* ---- dispatch ------------------------------------------------------------------------------------------------ */

static ui_ctl *ctl_for(user_t *u, uint16_t hwnd, ck_kind k, bool create) {
    ui_win *uw = ui_win_get(u->ui, hwnd, create);
    if (!uw) return NULL;
    if (!uw->ctl && create) {
        uw->ctl = calloc(1, sizeof *uw->ctl);
        if (uw->ctl) {
            uw->ctl->kind = k;
            uw->ctl->maxlen = 0;
            uw->ctl->want_x = -1;
        }
    }
    return uw->ctl;
}

void ui_ctl_free(ui_ctl *c) {
    int i;
    if (!c) return;
    for (i = 0; i < c->list.n; i++) free(c->list.s[i]);
    free(c->list.s);
    free(c->list.data);
    free(c->list.sel);
    free(c);
}

/* The text of a control changed (WM_SETTEXT): redraw it. */
void ui_text_changed(user_t *u, uint16_t hwnd) {
    wm_window *x = wm_get(&u->wm, hwnd);
    ck_kind k;
    if (!x || !x->cls || !x->cls->builtin || (k = kind_of(x->cls->name)) == CK_NONE) return;
    if (k == CK_EDIT) {
        ui_win *uw = ui_win_get(u->ui, hwnd, false);
        if (uw && uw->ctl) ui_edit_text_set(u, hwnd, x, uw->ctl);
    }
    repaint(u, hwnd);
}

bool ui_ctl_defproc(user_t *u, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp, uint32_t *ret) {
    wm_window *x = wm_get(&u->wm, hwnd);
    ck_kind k;
    ui_ctl *c;
    bool handled = false;
    if (!x || !x->cls) return false;
    if (!strcmp(x->cls->name, "#32770")) return ui_dlg_defproc(u, hwnd, msg, wp, lp, ret);
    k = kind_of(x->cls->name);
    if (k == CK_NONE) return false;
    if (msg == WM_NCCREATE) {
        ctl_for(u, hwnd, k, true);
        *ret = 1;
        return true;
    }
    if (!(c = ctl_for(u, hwnd, k, true))) return false;
    *ret = 0;
    switch (k) {
    case CK_BUTTON: *ret = button_proc(u, hwnd, x, c, msg, wp, lp, &handled); break;
    case CK_STATIC: *ret = static_proc(u, hwnd, x, msg, wp, &handled); break;
    case CK_SCROLL: *ret = scroll_proc(u, hwnd, x, c, msg, wp, lp, &handled); break;
    case CK_EDIT: handled = ui_edit_proc(u, hwnd, x, c, msg, wp, lp, ret); break;
    case CK_LISTBOX:
    case CK_COMBO: handled = ui_list_proc(u, hwnd, x, c, msg, wp, lp, ret); break;
    default: break;
    }
    if (!handled && (msg == WM_SETFONT || msg == WM_GETFONT)) {
        ui_win *uw = ui_win_get(u->ui, hwnd, msg == WM_SETFONT);
        if (msg == WM_SETFONT && uw) {
            uw->font = wp;
            if (lp & 1) wm_invalidate(&u->wm, hwnd, NULL, true);
        }
        *ret = msg == WM_GETFONT && uw ? uw->font : 0;
        return true;
    }
    return handled;
}
