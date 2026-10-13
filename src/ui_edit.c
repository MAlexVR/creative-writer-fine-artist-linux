/* EDIT: single-line and multi-line text editing. The text is the window text; the control keeps the selection,
 * the scroll position and the notification state. Lines are separated by "\r\n" as Windows does; multi-line
 * controls without ES_AUTOHSCROLL wrap at the width of the client area. */
#include <string.h>

#include "ui_priv.h"

#define EM_GETSEL 0x0400
#define EM_SETSEL 0x0401
#define EM_GETRECT 0x0402
#define EM_SETRECT 0x0403
#define EM_SETRECTNP 0x0404
#define EM_SCROLL 0x0405
#define EM_LINESCROLL 0x0406
#define EM_GETMODIFY 0x0408
#define EM_SETMODIFY 0x0409
#define EM_GETLINECOUNT 0x040A
#define EM_LINEINDEX 0x040B
#define EM_SETHANDLE 0x040C
#define EM_GETHANDLE 0x040D
#define EM_LINELENGTH 0x0411
#define EM_REPLACESEL 0x0412
#define EM_GETLINE 0x0414
#define EM_LIMITTEXT 0x0415
#define EM_CANUNDO 0x0416
#define EM_UNDO 0x0417
#define EM_FMTLINES 0x0418
#define EM_LINEFROMCHAR 0x0419
#define EM_SETTABSTOPS 0x041B
#define EM_SETPASSWORDCHAR 0x041C
#define EM_EMPTYUNDOBUFFER 0x041D
#define EM_GETFIRSTVISIBLELINE 0x041E
#define EM_SETREADONLY 0x041F
#define EM_GETPASSWORDCHAR 0x0422

#define EN_SETFOCUS 0x0100
#define EN_KILLFOCUS 0x0200
#define EN_CHANGE 0x0300
#define EN_UPDATE 0x0400
#define EN_ERRSPACE 0x0500
#define EN_MAXTEXT 0x0501

#define ES_CENTER 1
#define ES_RIGHT 2
#define ES_MULTILINE 4
#define ES_UPPERCASE 8
#define ES_LOWERCASE 0x10
#define ES_PASSWORD 0x20
#define ES_AUTOVSCROLL 0x40
#define ES_AUTOHSCROLL 0x80
#define ES_NOHIDESEL 0x100
#define ES_READONLY 0x800
#define ES_WANTRETURN 0x1000

#define PADX 2
#define PADY 1
#define DEFAULT_LIMIT 30000
#define MAXLINES 1024

typedef struct {
    user_t *u;
    uint16_t hwnd;
    wm_window *x;
    ui_ctl *c;
    char *disp;          /* the text as shown (password characters) */
    const char *text;
    int len, n, cell, th, cw, ch;
    tline ln[MAXLINES];
} elay;

static bool multi(const wm_window *x) { return (x->style & ES_MULTILINE) != 0; }

static void lay_free(elay *l) { free(l->disp); l->disp = NULL; }

static bool lay_init(elay *l, user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c) {
    fb_rect cl;
    int i;
    memset(l, 0, sizeof *l);
    l->u = u;
    l->hwnd = hwnd;
    l->x = x;
    l->c = c;
    l->text = wm_get_text(&u->wm, hwnd);
    l->len = (int)strlen(l->text);
    if (!(l->disp = malloc((size_t)l->len + 1))) return false;
    memcpy(l->disp, l->text, (size_t)l->len + 1);
    if ((x->style & ES_PASSWORD) && c->pwchar != -1) {
        char pw = c->pwchar > 0 ? (char)c->pwchar : '*';
        for (i = 0; i < l->len; i++)
            if (l->disp[i] != '\r' && l->disp[i] != '\n') l->disp[i] = pw;
    }
    l->cell = ui_cell_of(u, hwnd);
    l->th = ui_th(u, l->cell);
    wm_client_rect(&u->wm, hwnd, &cl);
    l->cw = cl.right;
    l->ch = cl.bottom;
    if (multi(x)) {
        l->n = ui_wrap_text(u, l->cell, l->disp, l->cw - 2 * PADX, (x->style & ES_AUTOHSCROLL) == 0, l->ln, MAXLINES);
        if (l->n == 0) {
            l->ln[0].start = 0;
            l->ln[0].len = 0;
            l->n = 1;
        }
    } else {
        l->ln[0].start = 0;
        l->ln[0].len = l->len;
        l->n = 1;
    }
    return true;
}

static int line_of(const elay *l, int idx) {
    int i, r = 0;
    for (i = 0; i < l->n; i++)
        if (l->ln[i].start <= idx) r = i;
    return r;
}

static int x_of(const elay *l, int line, int idx) {
    int s = l->ln[line].start, e = s + l->ln[line].len;
    if (idx > e) idx = e;
    if (idx < s) idx = s;
    return ui_tw(l->u, l->cell, l->disp + s, idx - s);
}

/* The character index nearest to pixel (px, py) of the client area. */
static int pos_at(const elay *l, int px, int py) {
    int line = l->n > 1 || multi(l->x) ? (py - PADY) / l->th + l->c->vscroll_line : 0, k, best, s, len;
    if (line < 0) line = 0;
    if (line >= l->n) line = l->n - 1;
    s = l->ln[line].start;
    len = l->ln[line].len;
    px = px - PADX + l->c->hscroll;
    best = 0;
    for (k = 1; k <= len; k++) {
        int w0 = ui_tw(l->u, l->cell, l->disp + s, k - 1), w1 = ui_tw(l->u, l->cell, l->disp + s, k);
        if (px >= (w0 + w1) / 2) best = k;
        else break;
    }
    return s + best;
}

static void scroll_to_caret(elay *l) {
    ui_ctl *c = l->c;
    int line = line_of(l, c->sel_b), x = x_of(l, line, c->sel_b), vis = (l->ch - 2 * PADY) / l->th;
    if (vis < 1) vis = 1;
    if (multi(l->x)) {
        if (line < c->vscroll_line) c->vscroll_line = line;
        if (line >= c->vscroll_line + vis) c->vscroll_line = line - vis + 1;
    }
    if (l->x->style & ES_AUTOHSCROLL || !multi(l->x)) {
        if (x < c->hscroll) c->hscroll = x > 8 ? x - 8 : 0;
        if (x > c->hscroll + l->cw - 2 * PADX - 2) c->hscroll = x - (l->cw - 2 * PADX) + 10;
    } else {
        c->hscroll = 0;
    }
}

/* ---- text changes ----------------------------------------------------------------------------------- */

static int limit_of(const ui_ctl *c) { return c->maxlen > 0 ? c->maxlen : DEFAULT_LIMIT; }

static void note(user_t *u, uint16_t hwnd, unsigned code) { ui_notify_parent(u, hwnd, code); }

/* Replaces [a, b) with ins; false when nothing was inserted because of the limit. */
static bool replace(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, int a, int b, const char *ins, int n) {
    const char *t = wm_get_text(&u->wm, hwnd);
    int len = (int)strlen(t), room;
    char *nt;
    if (a > b) { int s = a; a = b; b = s; }
    if (a < 0) a = 0;
    if (b > len) b = len;
    if (a > len) a = len;
    room = limit_of(c) - (len - (b - a));
    if (n > room) {
        int keep = room < 0 ? 0 : room;
        bool refused = keep == 0;
        n = keep;
        if (refused && ins) {
            note(u, hwnd, EN_MAXTEXT);
            return false;
        }
    }
    if (!(nt = malloc((size_t)len - (size_t)(b - a) + (size_t)n + 1))) {
        note(u, hwnd, EN_ERRSPACE);
        return false;
    }
    memcpy(nt, t, (size_t)a);
    if (n) memcpy(nt + a, ins, (size_t)n);
    memcpy(nt + a + n, t + b, (size_t)(len - b) + 1);
    wm_set_text(&u->wm, hwnd, nt);
    free(nt);
    c->sel_a = c->sel_b = a + n;
    c->modified = true;
    {
        elay l;
        if (lay_init(&l, u, hwnd, x, c)) {
            scroll_to_caret(&l);
            lay_free(&l);
        }
    }
    note(u, hwnd, EN_UPDATE);
    wm_invalidate(&u->wm, hwnd, NULL, false);
    note(u, hwnd, EN_CHANGE);
    return true;
}

static void delete_sel_or(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, bool backward) {
    const char *t = wm_get_text(&u->wm, hwnd);
    int len = (int)strlen(t), a = c->sel_a < c->sel_b ? c->sel_a : c->sel_b, b = c->sel_a < c->sel_b ? c->sel_b : c->sel_a;
    if (a == b) {
        if (backward) {
            if (a == 0) return;
            a -= (a >= 2 && t[a - 1] == '\n' && t[a - 2] == '\r') ? 2 : 1;
        } else {
            if (b >= len) return;
            b += (t[b] == '\r' && t[b + 1] == '\n') ? 2 : 1;
        }
    }
    replace(u, hwnd, x, c, a, b, NULL, 0);
}

static void sel_set(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, int a, int b) {
    elay l;
    int len = (int)strlen(wm_get_text(&u->wm, hwnd));
    if (a < 0) a = 0;
    if (b < 0 || b > len) b = len;
    if (a > len) a = len;
    c->sel_a = a;
    c->sel_b = b;
    if (lay_init(&l, u, hwnd, x, c)) {
        scroll_to_caret(&l);
        lay_free(&l);
    }
    wm_invalidate(&u->wm, hwnd, NULL, false);
}

static void copy_sel(user_t *u, uint16_t hwnd, ui_ctl *c) {
    const char *t = wm_get_text(&u->wm, hwnd);
    int a = c->sel_a < c->sel_b ? c->sel_a : c->sel_b, b = c->sel_a < c->sel_b ? c->sel_b : c->sel_a;
    if (b > a) ui_clip_set_text(u, t + a, b - a);
}

static void paste(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c) {
    char *s = ui_clip_get_text(u);
    if (!s) return;
    if (!multi(x)) {                                 /* a single line takes the first line of the text */
        char *e = strpbrk(s, "\r\n");
        if (e) *e = 0;
    }
    replace(u, hwnd, x, c, c->sel_a, c->sel_b, s, (int)strlen(s));
    free(s);
}

/* ---- keys ------------------------------------------------------------------------------------------------- */

static void move_caret(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, int to, bool extend) {
    c->sel_b = to;
    if (!extend) c->sel_a = to;
    {
        elay l;
        if (lay_init(&l, u, hwnd, x, c)) {
            scroll_to_caret(&l);
            lay_free(&l);
        }
    }
    wm_invalidate(&u->wm, hwnd, NULL, false);
}

static void on_keydown(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, unsigned vk) {
    bool shift = (wm_key_state(&u->wm, VK_SHIFT) & 0x8000) != 0, ctrl = (wm_key_state(&u->wm, VK_CONTROL) & 0x8000) != 0;
    const char *t = wm_get_text(&u->wm, hwnd);
    int len = (int)strlen(t), caret = c->sel_b, lo = c->sel_a < c->sel_b ? c->sel_a : c->sel_b, hi = c->sel_a < c->sel_b ? c->sel_b : c->sel_a;
    elay l;
    int line;
    if (!lay_init(&l, u, hwnd, x, c)) return;
    line = line_of(&l, caret);
    switch (vk) {
    case VK_LEFT:
        if (!shift && lo != hi) move_caret(u, hwnd, x, c, lo, false);
        else if (caret > 0) move_caret(u, hwnd, x, c, caret - ((caret >= 2 && t[caret - 1] == '\n' && t[caret - 2] == '\r') ? 2 : 1), shift);
        break;
    case VK_RIGHT:
        if (!shift && lo != hi) move_caret(u, hwnd, x, c, hi, false);
        else if (caret < len) move_caret(u, hwnd, x, c, caret + ((t[caret] == '\r' && t[caret + 1] == '\n') ? 2 : 1), shift);
        break;
    case VK_HOME: move_caret(u, hwnd, x, c, ctrl ? 0 : l.ln[line].start, shift); break;
    case VK_END: move_caret(u, hwnd, x, c, ctrl ? len : l.ln[line].start + l.ln[line].len, shift); break;
    case VK_UP:
    case VK_DOWN:
    case VK_PRIOR:
    case VK_NEXT:
        if (multi(x)) {
            int step = (vk == VK_PRIOR || vk == VK_NEXT) ? (l.ch - 2 * PADY) / l.th : 1, nl, wx;
            bool up = vk == VK_UP || vk == VK_PRIOR;
            nl = line + (up ? -step : step);
            if (nl < 0) nl = 0;
            if (nl >= l.n) nl = l.n - 1;
            if (c->want_x < 0) c->want_x = x_of(&l, line, caret);
            wx = c->want_x;
            if (nl != line || (!up && caret != l.ln[nl].start + l.ln[nl].len) || (up && caret != 0)) {
                int s = l.ln[nl].start, ll = l.ln[nl].len, k, best = 0;
                for (k = 1; k <= ll; k++)
                    if (ui_tw(u, l.cell, l.disp + s, k) <= wx + 1) best = k;
                    else break;
                move_caret(u, hwnd, x, c, nl == line ? (up ? 0 : s + ll) : s + best, shift);
            }
            c->want_x = wx;
        }
        break;
    case VK_DELETE:
        if (shift) {
            copy_sel(u, hwnd, c);
            if (!c->readonly) replace(u, hwnd, x, c, c->sel_a, c->sel_b, NULL, 0);
        } else if (!c->readonly) {
            delete_sel_or(u, hwnd, x, c, false);
        }
        break;
    case 0x2D:    /* VK_INSERT */
        if (ctrl) copy_sel(u, hwnd, c);
        else if (shift && !c->readonly) paste(u, hwnd, x, c);
        break;
    default: break;
    }
    if (vk != VK_UP && vk != VK_DOWN && vk != VK_PRIOR && vk != VK_NEXT) c->want_x = -1;
    lay_free(&l);
}

static void on_char(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, unsigned ch) {
    char b[3];
    c->want_x = -1;
    switch (ch) {
    case 3: copy_sel(u, hwnd, c); return;
    case 24:
        copy_sel(u, hwnd, c);
        if (!c->readonly) replace(u, hwnd, x, c, c->sel_a, c->sel_b, NULL, 0);
        return;
    case 22:
        if (!c->readonly) paste(u, hwnd, x, c);
        return;
    case 8:
        if (!c->readonly) delete_sel_or(u, hwnd, x, c, true);
        return;
    case 13:
        if (c->readonly || !multi(x)) return;
        replace(u, hwnd, x, c, c->sel_a, c->sel_b, "\r\n", 2);
        return;
    default: break;
    }
    if (ch < 32 || ch == 127 || c->readonly) return;
    if (x->style & ES_UPPERCASE) ch = kernel_ansi_upper(ch);
    else if (x->style & ES_LOWERCASE) ch = kernel_ansi_lower(ch);
    b[0] = (char)ch;
    b[1] = 0;
    replace(u, hwnd, x, c, c->sel_a, c->sel_b, b, 1);
}

/* ---- painting ------------------------------------------------------------------------------------------------- */

static void paint(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, uint16_t dc) {
    elay l;
    uint32_t fg, bg;
    bool focus = u->wm.focus == hwnd, show_sel = focus || (x->style & ES_NOHIDESEL);
    int a = c->sel_a < c->sel_b ? c->sel_a : c->sel_b, b = c->sel_a < c->sel_b ? c->sel_b : c->sel_a, i, y;
    if (!lay_init(&l, u, hwnd, x, c)) return;
    ui_ctlcolors(u, hwnd, dc, 1, &fg, &bg);
    /* Windows 3.1 draws the text of a disabled edit control like any other (the grey text arrived with later
     * versions); Creative Writer disables the box that has no focus, so greying it would be wrong. */
    ui_fill(u, dc, 0, 0, l.cw, l.ch, bg);
    for (i = c->vscroll_line; i < l.n; i++) {
        int s = l.ln[i].start, e = s + l.ln[i].len, x0 = PADX - c->hscroll, p, q;
        y = multi(x) ? PADY + (i - c->vscroll_line) * l.th : (l.ch - l.th) / 2;
        if (y >= l.ch) break;
        if (x->style & (ES_CENTER | ES_RIGHT)) {
            int w = ui_tw(u, l.cell, l.disp + s, e - s);
            x0 = (x->style & ES_CENTER) ? (l.cw - w) / 2 : l.cw - w - PADX;
        }
        p = show_sel && a < e + 1 && b > s ? (a > s ? a : s) : e;       /* the selected part of this line */
        q = show_sel && a < e + 1 && b > s ? (b < e ? b : e) : e;
        if (p > s) ui_text(u, dc, l.cell, x0, y, l.disp + s, p - s, fg, 0xFFFFFFFFu);
        if (q > p) {
            int xs = x0 + ui_tw(u, l.cell, l.disp + s, p - s), w = ui_tw(u, l.cell, l.disp + p, q - p);
            ui_fill(u, dc, xs, y, xs + w, y + l.th, wm_sys_color(13));
            ui_text(u, dc, l.cell, xs, y, l.disp + p, q - p, wm_sys_color(14), 0xFFFFFFFFu);
        }
        if (e > q) {
            int xs = x0 + ui_tw(u, l.cell, l.disp + s, q - s);
            ui_text(u, dc, l.cell, xs, y, l.disp + q, e - q, fg, 0xFFFFFFFFu);
        }
        if (focus && a == b && line_of(&l, c->sel_b) == i) {
            int cx = x0 + x_of(&l, i, c->sel_b);
            ui_fill(u, dc, cx, y, cx + 1, y + l.th, fg);
        }
    }
    lay_free(&l);
}

/* ---- messages -------------------------------------------------------------------------------------------------------- */

static uint32_t lines_msg(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, uint16_t msg, uint16_t wp, uint32_t lp) {
    elay l;
    uint32_t r = 0;
    if (!lay_init(&l, u, hwnd, x, c)) return 0;
    switch (msg) {
    case EM_GETLINECOUNT: r = (uint32_t)l.n; break;
    case EM_LINEINDEX: {
        int line = (int16_t)wp < 0 ? line_of(&l, c->sel_b) : (int16_t)wp;
        r = line >= 0 && line < l.n ? (uint32_t)l.ln[line].start : 0xFFFFFFFFu;
        break;
    }
    case EM_LINELENGTH: {
        int idx = (int16_t)wp;
        if (idx < 0) {
            int a = c->sel_a < c->sel_b ? c->sel_a : c->sel_b, b = c->sel_a < c->sel_b ? c->sel_b : c->sel_a;
            r = (uint32_t)(b - a);
        } else {
            r = (uint32_t)l.ln[line_of(&l, idx)].len;
        }
        break;
    }
    case EM_LINEFROMCHAR: r = (uint32_t)line_of(&l, (int16_t)wp < 0 ? c->sel_b : (int)wp); break;
    case EM_GETLINE: {
        uint8_t *p = user_guest_ptr(u, lp, 2);
        int line = (int16_t)wp, max, n;
        if (!p || line < 0 || line >= l.n) break;
        max = ld16(p);
        n = l.ln[line].len < max ? l.ln[line].len : max;
        if (!(p = user_guest_ptr(u, lp, (uint32_t)(n > 2 ? n : 2)))) break;
        memcpy(p, l.text + l.ln[line].start, (size_t)n);
        r = (uint32_t)n;
        break;
    }
    case EM_GETFIRSTVISIBLELINE: r = (uint32_t)c->vscroll_line; break;
    case EM_LINESCROLL:
        c->vscroll_line += (int16_t)(lp & 0xFFFF);
        if (c->vscroll_line < 0) c->vscroll_line = 0;
        if (c->vscroll_line >= l.n) c->vscroll_line = l.n - 1;
        wm_invalidate(&u->wm, hwnd, NULL, false);
        break;
    default: break;
    }
    lay_free(&l);
    return r;
}

bool ui_edit_proc(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, uint16_t msg, uint16_t wp, uint32_t lp, uint32_t *ret) {
    *ret = 0;
    if (x->style & ES_READONLY) c->readonly = true;
    switch (msg) {
    case WM_NCCREATE: return false;
    case WM_ERASEBKGND: *ret = 1; return true;
    case WM_PAINT: {
        wm_paint ps;
        uint16_t dc = wm_begin_paint(&u->wm, hwnd, &ps);
        if (dc) {
            paint(u, hwnd, x, c, dc);
            wm_end_paint(&u->wm, hwnd, &ps);
        }
        return true;
    }
    case WM_GETDLGCODE:
        *ret = DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS;
        if (multi(x) && (x->style & ES_WANTRETURN)) *ret |= DLGC_WANTALLKEYS;
        return true;
    case WM_SETFOCUS:
        note(u, hwnd, EN_SETFOCUS);
        wm_invalidate(&u->wm, hwnd, NULL, false);
        return true;
    case WM_KILLFOCUS:
        note(u, hwnd, EN_KILLFOCUS);
        wm_invalidate(&u->wm, hwnd, NULL, false);
        return true;
    case WM_ENABLE:
        wm_invalidate(&u->wm, hwnd, NULL, false);
        return true;
    case WM_CHAR:
        on_char(u, hwnd, x, c, wp);
        return true;
    case WM_KEYDOWN:
        on_keydown(u, hwnd, x, c, wp);
        return true;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        elay l;
        if (x->style & WS_DISABLED) return true;
        wm_set_focus(&u->wm, hwnd);
        wm_set_capture(&u->wm, hwnd);
        c->dragging = true;
        if (lay_init(&l, u, hwnd, x, c)) {
            int p = pos_at(&l, (int16_t)lp, (int16_t)(lp >> 16));
            if (msg == WM_LBUTTONDBLCLK) {          /* a double click selects the word */
                int a = p, b = p, len = l.len;
                while (a > 0 && l.text[a - 1] != ' ' && l.text[a - 1] != '\n') a--;
                while (b < len && l.text[b] != ' ' && l.text[b] != '\r') b++;
                c->sel_a = a;
                c->sel_b = b;
            } else {
                c->sel_a = c->sel_b = p;
            }
            c->want_x = -1;
            lay_free(&l);
        }
        wm_invalidate(&u->wm, hwnd, NULL, false);
        return true;
    }
    case WM_MOUSEMOVE:
        if (c->dragging && u->wm.capture == hwnd) {
            elay l;
            if (lay_init(&l, u, hwnd, x, c)) {
                c->sel_b = pos_at(&l, (int16_t)lp, (int16_t)(lp >> 16));
                scroll_to_caret(&l);
                lay_free(&l);
                wm_invalidate(&u->wm, hwnd, NULL, false);
            }
        }
        return true;
    case WM_LBUTTONUP:
        if (c->dragging) {
            c->dragging = false;
            if (u->wm.capture == hwnd) wm_release_capture(&u->wm);
        }
        return true;
    case EM_GETSEL: {
        int a = c->sel_a < c->sel_b ? c->sel_a : c->sel_b, b = c->sel_a < c->sel_b ? c->sel_b : c->sel_a;
        *ret = (uint32_t)(uint16_t)a | (uint32_t)(uint16_t)b << 16;
        return true;
    }
    case EM_SETSEL: {
        int a = (int16_t)(lp & 0xFFFF), b = (int16_t)(lp >> 16);
        sel_set(u, hwnd, x, c, a, b);
        return true;
    }
    case EM_REPLACESEL: {
        const char *s = user_guest_ptr(u, lp, 1);
        if (s) replace(u, hwnd, x, c, c->sel_a, c->sel_b, s, (int)strlen(s));
        return true;
    }
    case EM_LIMITTEXT: c->maxlen = wp; return true;
    case EM_GETMODIFY: *ret = c->modified; return true;
    case EM_SETMODIFY: c->modified = wp != 0; return true;
    case EM_SETREADONLY:
        c->readonly = wp != 0;
        x->style = c->readonly ? x->style | ES_READONLY : x->style & ~(uint32_t)ES_READONLY;
        *ret = 1;
        return true;
    case EM_SETPASSWORDCHAR:
        c->pwchar = wp ? (int)wp : -1;
        wm_invalidate(&u->wm, hwnd, NULL, false);
        return true;
    case EM_GETPASSWORDCHAR: *ret = (x->style & ES_PASSWORD) && c->pwchar != -1 ? (uint32_t)(c->pwchar > 0 ? c->pwchar : '*') : 0; return true;
    case EM_CANUNDO: case EM_UNDO: case EM_FMTLINES: case EM_SETTABSTOPS: case EM_EMPTYUNDOBUFFER: case EM_SETHANDLE: case EM_GETHANDLE:
    case EM_SETRECT: case EM_SETRECTNP: case EM_GETRECT: case EM_SCROLL:
        return true;
    case EM_GETLINECOUNT: case EM_LINEINDEX: case EM_LINELENGTH: case EM_LINEFROMCHAR: case EM_GETLINE:
    case EM_GETFIRSTVISIBLELINE: case EM_LINESCROLL:
        *ret = lines_msg(u, hwnd, x, c, msg, wp, lp);
        return true;
    default: return false;
    }
}

/* WM_SETTEXT replaced the text: the selection goes back to the start, the notifications follow. */
void ui_edit_text_set(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c) {
    c->sel_a = c->sel_b = 0;
    c->hscroll = c->vscroll_line = 0;
    c->want_x = -1;
    (void)x;
    note(u, hwnd, EN_UPDATE);
    wm_invalidate(&u->wm, hwnd, NULL, false);
    note(u, hwnd, EN_CHANGE);
}
