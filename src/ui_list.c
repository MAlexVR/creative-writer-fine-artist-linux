/* LISTBOX and COMBOBOX. Both keep their items in a ui_list (strings and one data word each); the list box
 * draws them in rows of one text height with a scroll bar in the non-client area when the style asks for one,
 * the combo box adds a box with the selected text (or an edit part) and a drop-down list that is drawn over the
 * screen and tracked in a short modal loop, like a menu. Owner-draw styles are treated as plain strings. */
#include <string.h>

#include "ui_priv.h"

#define LB_ADDSTRING 0x0401
#define LB_INSERTSTRING 0x0402
#define LB_DELETESTRING 0x0403
#define LB_RESETCONTENT 0x0405
#define LB_SETSEL 0x0406
#define LB_SETCURSEL 0x0407
#define LB_GETSEL 0x0408
#define LB_GETCURSEL 0x0409
#define LB_GETTEXT 0x040A
#define LB_GETTEXTLEN 0x040B
#define LB_GETCOUNT 0x040C
#define LB_SELECTSTRING 0x040D
#define LB_DIR 0x040E
#define LB_GETTOPINDEX 0x040F
#define LB_FINDSTRING 0x0410
#define LB_GETSELCOUNT 0x0411
#define LB_GETSELITEMS 0x0412
#define LB_SETTABSTOPS 0x0413
#define LB_GETHORIZONTALEXTENT 0x0414
#define LB_SETHORIZONTALEXTENT 0x0415
#define LB_SETCOLUMNWIDTH 0x0416
#define LB_ADDFILE 0x0417
#define LB_SETTOPINDEX 0x0418
#define LB_GETITEMRECT 0x0419
#define LB_GETITEMDATA 0x041A
#define LB_SETITEMDATA 0x041B
#define LB_SELITEMRANGE 0x041C
#define LB_SETCARETINDEX 0x041F
#define LB_GETCARETINDEX 0x0420
#define LB_SETITEMHEIGHT 0x0421
#define LB_GETITEMHEIGHT 0x0422
#define LB_FINDSTRINGEXACT 0x0423
#define LB_ERR 0xFFFFFFFFu

#define CB_GETEDITSEL 0x0400
#define CB_LIMITTEXT 0x0401
#define CB_SETEDITSEL 0x0402
#define CB_ADDSTRING 0x0403
#define CB_DELETESTRING 0x0404
#define CB_DIR 0x0405
#define CB_GETCOUNT 0x0406
#define CB_GETCURSEL 0x0407
#define CB_GETLBTEXT 0x0408
#define CB_GETLBTEXTLEN 0x0409
#define CB_INSERTSTRING 0x040A
#define CB_RESETCONTENT 0x040B
#define CB_FINDSTRING 0x040C
#define CB_SELECTSTRING 0x040D
#define CB_SETCURSEL 0x040E
#define CB_SHOWDROPDOWN 0x040F
#define CB_GETITEMDATA 0x0410
#define CB_SETITEMDATA 0x0411
#define CB_GETDROPPEDCONTROLRECT 0x0412
#define CB_SETITEMHEIGHT 0x0413
#define CB_GETITEMHEIGHT 0x0414
#define CB_SETEXTENDEDUI 0x0415
#define CB_GETEXTENDEDUI 0x0416
#define CB_GETDROPPEDSTATE 0x0417
#define CB_FINDSTRINGEXACT 0x0418

#define LBN_SELCHANGE 1
#define LBN_DBLCLK 2
#define CBN_SELCHANGE 1
#define CBN_DBLCLK 2
#define CBN_SETFOCUS 3
#define CBN_KILLFOCUS 4
#define CBN_EDITCHANGE 5
#define CBN_EDITUPDATE 6
#define CBN_DROPDOWN 7
#define CBN_CLOSEUP 8
#define CBN_SELENDOK 9
#define CBN_SELENDCANCEL 10

#define LBS_NOTIFY 0x0001
#define LBS_SORT 0x0002
#define LBS_MULTIPLESEL 0x0008
#define LBS_EXTENDEDSEL 0x0800
#define CBS_SIMPLE 1
#define CBS_DROPDOWN 2
#define CBS_DROPDOWNLIST 3
#define DROP_ROWS 8

/* ---- the item store ------------------------------------------------------------------------------------- */

static int ci_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        unsigned x = kernel_ansi_lower((unsigned char)*a), y = kernel_ansi_lower((unsigned char)*b);
        if (x != y) return x < y ? -1 : 1;
        if (!*a) return 0;
    }
}

static bool ci_prefix(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++)
        if (kernel_ansi_lower((unsigned char)*s) != kernel_ansi_lower((unsigned char)*prefix)) return false;
    return true;
}

static int list_insert(ui_list *L, int idx, const char *s, bool sort) {
    char *d = ui_strdup(s);
    if (!d) return -1;
    if (L->n == L->cap) {
        int cap = L->cap ? L->cap * 2 : 16;
        char **ns = realloc(L->s, (size_t)cap * sizeof *ns);
        uint32_t *nd;
        uint8_t *nsel;
        if (!ns) { free(d); return -1; }
        L->s = ns;
        nd = realloc(L->data, (size_t)cap * sizeof *nd);
        if (!nd) { free(d); return -1; }
        L->data = nd;
        nsel = realloc(L->sel, (size_t)cap);
        if (!nsel) { free(d); return -1; }
        L->sel = nsel;
        L->cap = cap;
    }
    if (sort) {
        for (idx = 0; idx < L->n && ci_cmp(L->s[idx], s) <= 0; idx++) {}
    } else if (idx < 0 || idx > L->n) {
        idx = L->n;
    }
    memmove(&L->s[idx + 1], &L->s[idx], (size_t)(L->n - idx) * sizeof *L->s);
    memmove(&L->data[idx + 1], &L->data[idx], (size_t)(L->n - idx) * sizeof *L->data);
    memmove(&L->sel[idx + 1], &L->sel[idx], (size_t)(L->n - idx));
    L->s[idx] = d;
    L->data[idx] = 0;
    L->sel[idx] = 0;
    L->n++;
    if (L->cur >= idx && L->cur >= 0 && L->n > 1) L->cur++;
    return idx;
}

static void list_delete(ui_list *L, int idx) {
    free(L->s[idx]);
    memmove(&L->s[idx], &L->s[idx + 1], (size_t)(L->n - idx - 1) * sizeof *L->s);
    memmove(&L->data[idx], &L->data[idx + 1], (size_t)(L->n - idx - 1) * sizeof *L->data);
    memmove(&L->sel[idx], &L->sel[idx + 1], (size_t)(L->n - idx - 1));
    L->n--;
    if (L->cur == idx) L->cur = -1;
    else if (L->cur > idx) L->cur--;
    if (L->top >= L->n) L->top = L->n > 0 ? L->n - 1 : 0;
}

static void list_clear(ui_list *L) {
    int i;
    for (i = 0; i < L->n; i++) free(L->s[i]);
    L->n = 0;
    L->cur = -1;
    L->top = 0;
    L->caret = 0;
}

static int list_find(const ui_list *L, int start, const char *s, bool exact) {
    int i, k;
    for (i = 1; i <= L->n; i++) {
        k = ((start + i) % L->n + L->n) % L->n;
        if (exact ? ci_cmp(L->s[k], s) == 0 : ci_prefix(L->s[k], s)) return k;
    }
    return -1;
}

/* ---- geometry ----------------------------------------------------------------------------------------------- */

static int item_h(user_t *u, uint16_t hwnd, const ui_ctl *c) { return c->item_h > 0 ? c->item_h : ui_th(u, ui_cell_of(u, hwnd)); }
static bool is_combo(const wm_window *x) { return x->cls && !strcmp(x->cls->name, "COMBOBOX"); }
static int combo_type(const wm_window *x) { return (int)(x->style & 3); }
static bool multi_sel(const wm_window *x) { return !is_combo(x) && (x->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) != 0; }

static int box_h(user_t *u, uint16_t hwnd, const ui_ctl *c) { return item_h(u, hwnd, c) + 8; }

/* The area where the list rows are drawn, in client coordinates. */
static fb_rect rows_rect(user_t *u, uint16_t hwnd, wm_window *x, const ui_ctl *c) {
    fb_rect cl, r;
    wm_client_rect(&u->wm, hwnd, &cl);
    r = cl;
    if (is_combo(x)) {
        r.top = box_h(u, hwnd, c);
        if (combo_type(x) != CBS_SIMPLE) r.bottom = r.top;
    }
    return r;
}

static int visible_rows(user_t *u, uint16_t hwnd, wm_window *x, const ui_ctl *c) {
    fb_rect r = rows_rect(u, hwnd, x, c);
    int n = (r.bottom - r.top) / item_h(u, hwnd, c);
    return n < 1 ? 1 : n;
}

static void fix_top(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, int show) {
    int vis = visible_rows(u, hwnd, x, c);
    ui_list *L = &c->list;
    if (show >= 0) {
        if (show < L->top) L->top = show;
        if (show >= L->top + vis) L->top = show - vis + 1;
    }
    if (L->top > L->n - vis) L->top = L->n - vis;
    if (L->top < 0) L->top = 0;
}

static void sync_sb(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c) {
    ui_win *uw;
    int vis;
    if (!(x->style & WS_VSCROLL) || !(uw = ui_win_get(u->ui, hwnd, false))) return;
    vis = visible_rows(u, hwnd, x, c);
    uw->sb[SB_VERT].min = 0;
    uw->sb[SB_VERT].max = c->list.n > vis ? c->list.n - vis : 0;
    uw->sb[SB_VERT].pos = c->list.top;
    if (wm_is_visible(&u->wm, hwnd)) ui_ncpaint(u, hwnd);
}

static void changed(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, int show) {
    fix_top(u, hwnd, x, c, show);
    sync_sb(u, hwnd, x, c);
    wm_invalidate(&u->wm, hwnd, NULL, false);
}

/* ---- drawing -------------------------------------------------------------------------------------------------- */

static void draw_rows(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, uint16_t dc, const fb_rect *area, int hover, bool as_popup) {
    int cell = ui_cell_of(u, hwnd), ih = item_h(u, hwnd, c), th = ui_th(u, cell), i, y = area->top;
    uint32_t fg, bg;
    ui_ctlcolors(u, hwnd, dc, 2, &fg, &bg);
    ui_fill(u, dc, area->left, area->top, area->right, area->bottom, bg);
    for (i = c->list.top; i < c->list.n && y < area->bottom; i++, y += ih) {
        bool sel = multi_sel(x) ? c->list.sel[i] != 0 : (as_popup ? i == hover : i == c->list.cur);
        if (sel) ui_fill(u, dc, area->left, y, area->right, y + ih, wm_sys_color(13));
        ui_text(u, dc, cell, area->left + 2, y + (ih - th) / 2, c->list.s[i], (int)strlen(c->list.s[i]), sel ? wm_sys_color(14) : fg, 0xFFFFFFFFu);
        if (multi_sel(x) && i == c->list.caret && u->wm.focus == hwnd) ui_dotted(u, dc, area->left, y, area->right, y + ih);
    }
}

static void paint_list(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, uint16_t dc) {
    fb_rect cl;
    wm_client_rect(&u->wm, hwnd, &cl);
    draw_rows(u, hwnd, x, c, dc, &cl, -1, false);
}

static void paint_combo(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, uint16_t dc) {
    fb_rect cl;
    int bh = box_h(u, hwnd, c), cell = ui_cell_of(u, hwnd), th = ui_th(u, cell), ar;
    uint32_t fg, bg;
    const char *t = wm_get_text(&u->wm, hwnd);
    int type = combo_type(x);
    wm_client_rect(&u->wm, hwnd, &cl);
    ar = cl.right - 16;
    ui_ctlcolors(u, hwnd, dc, 1, &fg, &bg);
    ui_fill(u, dc, 0, 0, cl.right, bh, wm_sys_color(6));
    ui_fill(u, dc, 1, 1, ar, bh - 1, type == CBS_DROPDOWNLIST && u->wm.focus == hwnd ? wm_sys_color(13) : bg);
    if (type == CBS_DROPDOWNLIST && u->wm.focus == hwnd) fg = wm_sys_color(14);
    ui_text(u, dc, cell, 3, (bh - th) / 2, t, (int)strlen(t), fg, 0xFFFFFFFFu);
    /* the arrow button */
    ui_fill(u, dc, ar, 1, cl.right - 1, bh - 1, wm_sys_color(15));
    ui_fill(u, dc, ar, 1, cl.right - 2, 2, wm_sys_color(20));
    ui_fill(u, dc, ar, 1, ar + 1, bh - 2, wm_sys_color(20));
    ui_fill(u, dc, ar + 1, bh - 3, cl.right - 1, bh - 1, wm_sys_color(16));
    ui_fill(u, dc, cl.right - 3, 2, cl.right - 1, bh - 1, wm_sys_color(16));
    {
        int cx = ar + 8, cy = bh / 2, k;
        for (k = 0; k < 4; k++) ui_fill(u, dc, cx - 3 + k, cy - 1 + k, cx + 4 - k, cy + k, wm_sys_color(6));
    }
    if (type == CBS_SIMPLE) {
        fb_rect r = rows_rect(u, hwnd, x, c);
        draw_rows(u, hwnd, x, c, dc, &r, -1, false);
    }
}

/* ---- notifications and selection ---------------------------------------------------------------------------------- */

static void notify(user_t *u, uint16_t hwnd, wm_window *x, unsigned lb, unsigned cb) {
    if (is_combo(x)) ui_notify_parent(u, hwnd, cb);
    else if (x->style & LBS_NOTIFY) ui_notify_parent(u, hwnd, lb);
}

static void select_item(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, int idx) {
    c->list.cur = idx;
    c->list.caret = idx < 0 ? 0 : idx;
    if (is_combo(x) && idx >= 0 && combo_type(x) != CBS_SIMPLE) wm_set_text(&u->wm, hwnd, c->list.s[idx]);
    else if (is_combo(x) && idx >= 0) wm_set_text(&u->wm, hwnd, c->list.s[idx]);
    changed(u, hwnd, x, c, idx);
}

/* ---- the drop-down ----------------------------------------------------------------------------------------------------- */

static void combo_dropdown(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c) {
    ui_list *L = &c->list;
    int ih = item_h(u, hwnd, c), rows = L->n < DROP_ROWS ? (L->n ? L->n : 1) : DROP_ROWS, ox, oy, hover = L->cur, first, orig = L->cur;
    fb_rect cl, rc;
    uint8_t *save = NULL;
    fb_t *fb = &u->fb;
    int w, h, y;
    bool done = false, cancel = false, picked = false;
    static const uint16_t rng[3][2] = {{WM_KEYDOWN, 0x0108}, {WM_MOUSEMOVE, 0x0209}, {0x00A0, 0x00A9}};
    if (c->dropped || !u->has_gfx) return;
    wm_client_origin(&u->wm, hwnd, &ox, &oy);
    wm_client_rect(&u->wm, hwnd, &cl);
    ui_notify_parent(u, hwnd, CBN_DROPDOWN);
    w = cl.right;
    h = rows * ih + 2;
    rc = (fb_rect){ox, oy + box_h(u, hwnd, c), ox + w, oy + box_h(u, hwnd, c) + h};
    if (rc.bottom > fb->h) {
        rc.top = oy - h;
        rc.bottom = oy;
    }
    first = L->cur > rows - 1 ? L->cur - rows + 1 : 0;
    if (fb->bpp == 8 && (save = malloc((size_t)w * (size_t)h))) {
        for (y = 0; y < h; y++)
            if (rc.top + y >= 0 && rc.top + y < fb->h) memcpy(save + (size_t)y * (size_t)w, fb->p8 + (size_t)(rc.top + y) * (size_t)fb->w + rc.left, (size_t)w);
    }
    c->dropped = true;
    {   /* the pointer events go to the combo box while the list is open */
        uint16_t old = wm_set_capture(&u->wm, hwnd);
        while (!done && !u->abort) {
            wm_msg m;
            int r;
            bool got = false;
            uint16_t dc = wm_get_dc(&u->wm, 0);
            if (dc) {
                int i, th = ui_th(u, ui_cell_of(u, hwnd));
                ui_fill(u, dc, rc.left, rc.top, rc.right, rc.bottom, wm_sys_color(5));
                ui_outline(u, dc, rc.left, rc.top, rc.right, rc.bottom, wm_sys_color(6));
                for (i = first; i < L->n && i < first + rows; i++) {
                    int top = rc.top + 1 + (i - first) * ih;
                    bool hv = i == hover;
                    if (hv) ui_fill(u, dc, rc.left + 1, top, rc.right - 1, top + ih, wm_sys_color(13));
                    ui_text(u, dc, ui_cell_of(u, hwnd), rc.left + 3, top + (ih - th) / 2, L->s[i], (int)strlen(L->s[i]), hv ? wm_sys_color(14) : wm_sys_color(8), 0xFFFFFFFFu);
                }
                wm_release_dc(&u->wm, 0, dc);
            }
            for (r = 0; r < 3 && !got; r++) got = wm_next_message(&u->wm, &m, 0, rng[r][0], rng[r][1], true);
            if (!got) {
                if (!user_idle(u, true)) u->abort = loader_request_stop(u->p);
                continue;
            }
            if (m.message >= 0xA0 && m.message <= 0xA9) m.message = (uint16_t)(m.message - 0xA0 + WM_MOUSEMOVE);
            switch (m.message) {
            case WM_MOUSEMOVE:
            case WM_LBUTTONUP:
            case WM_LBUTTONDOWN: {
                int sx = m.pt_x, sy = m.pt_y;
                bool inside = sx >= rc.left && sx < rc.right && sy >= rc.top && sy < rc.bottom;
                bool on_box = sx >= ox && sx < ox + w && sy >= oy && sy < oy + box_h(u, hwnd, c);
                int idx = inside ? first + (sy - rc.top - 1) / ih : -1;
                if (idx >= L->n) idx = -1;
                if (inside && idx >= 0) hover = idx;
                if (m.message == WM_LBUTTONUP && inside && idx >= 0) { picked = true; done = true; }
                else if (m.message == WM_LBUTTONDOWN && !inside) { cancel = true; done = true; (void)on_box; }
                break;
            }
            case WM_KEYDOWN:
                if (m.wparam == VK_DOWN && hover < L->n - 1) hover++;
                else if (m.wparam == VK_UP && hover > 0) hover--;
                else if (m.wparam == VK_RETURN) { picked = hover >= 0; done = true; }
                else if (m.wparam == VK_ESCAPE) { cancel = true; done = true; }
                if (hover >= first + rows) first = hover - rows + 1;
                if (hover >= 0 && hover < first) first = hover;
                break;
            default: break;
            }
        }
        if (old && wm_is_window(&u->wm, old)) wm_set_capture(&u->wm, old);
        else wm_release_capture(&u->wm);
    }
    if (save) {
        for (y = 0; y < h; y++)
            if (rc.top + y >= 0 && rc.top + y < fb->h) memcpy(fb->p8 + (size_t)(rc.top + y) * (size_t)fb->w + rc.left, save + (size_t)y * (size_t)w, (size_t)w);
        free(save);
        fb->dirty++;
    }
    c->dropped = false;
    if (u->abort) return;
    if (picked && hover >= 0) {
        ui_notify_parent(u, hwnd, CBN_SELENDOK);
        select_item(u, hwnd, x, c, hover);
        if (hover != orig) ui_notify_parent(u, hwnd, CBN_SELCHANGE);
    } else if (cancel || !picked) {
        ui_notify_parent(u, hwnd, CBN_SELENDCANCEL);
    }
    ui_notify_parent(u, hwnd, CBN_CLOSEUP);
    wm_invalidate(&u->wm, hwnd, NULL, false);
}

/* ---- messages ------------------------------------------------------------------------------------------------------------- */

/* Maps a CB_* message to the LB_* operation with the same meaning. */
static uint16_t cb_to_lb(uint16_t m) {
    switch (m) {
    case CB_ADDSTRING: return LB_ADDSTRING;
    case CB_DELETESTRING: return LB_DELETESTRING;
    case CB_GETCOUNT: return LB_GETCOUNT;
    case CB_GETCURSEL: return LB_GETCURSEL;
    case CB_GETLBTEXT: return LB_GETTEXT;
    case CB_GETLBTEXTLEN: return LB_GETTEXTLEN;
    case CB_INSERTSTRING: return LB_INSERTSTRING;
    case CB_RESETCONTENT: return LB_RESETCONTENT;
    case CB_FINDSTRING: return LB_FINDSTRING;
    case CB_SELECTSTRING: return LB_SELECTSTRING;
    case CB_SETCURSEL: return LB_SETCURSEL;
    case CB_GETITEMDATA: return LB_GETITEMDATA;
    case CB_SETITEMDATA: return LB_SETITEMDATA;
    case CB_SETITEMHEIGHT: return LB_SETITEMHEIGHT;
    case CB_GETITEMHEIGHT: return LB_GETITEMHEIGHT;
    case CB_FINDSTRINGEXACT: return LB_FINDSTRINGEXACT;
    default: return 0;
    }
}

static const char *str_arg(user_t *u, uint32_t lp) { return user_guest_ptr(u, lp, 1); }

static uint32_t list_message(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, uint16_t msg, uint16_t wp, uint32_t lp, bool *handled) {
    ui_list *L = &c->list;
    int idx = (int16_t)wp;
    bool multi = multi_sel(x), sort = !is_combo(x) && (x->style & LBS_SORT);
    *handled = true;
    switch (msg) {
    case LB_ADDSTRING: case LB_INSERTSTRING: {
        const char *s = str_arg(u, lp);
        int at;
        if (!s) return LB_ERR;
        at = list_insert(L, msg == LB_ADDSTRING ? -1 : idx, s, sort && msg == LB_ADDSTRING);
        if (at < 0) return LB_ERR;
        changed(u, hwnd, x, c, -1);
        return (uint32_t)at;
    }
    case LB_DELETESTRING:
        if (idx < 0 || idx >= L->n) return LB_ERR;
        list_delete(L, idx);
        changed(u, hwnd, x, c, -1);
        return (uint32_t)L->n;
    case LB_RESETCONTENT:
        list_clear(L);
        changed(u, hwnd, x, c, -1);
        return 0;
    case LB_GETCOUNT: return (uint32_t)L->n;
    case LB_GETTEXT: {
        uint8_t *d;
        size_t n;
        if (idx < 0 || idx >= L->n) return LB_ERR;
        n = strlen(L->s[idx]);
        if (!(d = user_guest_ptr(u, lp, (uint32_t)n + 1))) return LB_ERR;
        memcpy(d, L->s[idx], n + 1);
        return (uint32_t)n;
    }
    case LB_GETTEXTLEN: return idx < 0 || idx >= L->n ? LB_ERR : (uint32_t)strlen(L->s[idx]);
    case LB_SETCURSEL:
        if (multi) return LB_ERR;
        if (idx < 0 || idx >= L->n) {
            select_item(u, hwnd, x, c, -1);
            return LB_ERR;
        }
        select_item(u, hwnd, x, c, idx);
        return (uint32_t)idx;
    case LB_GETCURSEL: return multi ? (L->n ? (uint32_t)L->caret : LB_ERR) : (L->cur < 0 ? LB_ERR : (uint32_t)L->cur);
    case LB_GETSEL:
        if (idx < 0 || idx >= L->n) return LB_ERR;
        return multi ? L->sel[idx] != 0 : idx == L->cur;
    case LB_SETSEL: {
        int i = (int16_t)(lp & 0xFFFF);
        if (!multi) return LB_ERR;
        if (i == -1) memset(L->sel, wp ? 1 : 0, (size_t)L->n);
        else if (i >= 0 && i < L->n) L->sel[i] = wp ? 1 : 0;
        else return LB_ERR;
        changed(u, hwnd, x, c, -1);
        return 0;
    }
    case LB_SELITEMRANGE: {
        int a = (int16_t)(lp & 0xFFFF), b = (int16_t)(lp >> 16), i;
        if (!multi) return LB_ERR;
        for (i = a; i <= b && i < L->n; i++)
            if (i >= 0) L->sel[i] = wp ? 1 : 0;
        changed(u, hwnd, x, c, -1);
        return 0;
    }
    case LB_GETSELCOUNT: {
        int i, n = 0;
        if (!multi) return LB_ERR;
        for (i = 0; i < L->n; i++) n += L->sel[i] != 0;
        return (uint32_t)n;
    }
    case LB_GETSELITEMS: {
        int i, n = 0;
        uint8_t *d = user_guest_ptr(u, lp, 2);
        if (!multi || !d) return LB_ERR;
        for (i = 0; i < L->n && n < (int)wp; i++)
            if (L->sel[i]) {
                if (!(d = user_guest_ptr(u, lp, (uint32_t)(n + 1) * 2))) return LB_ERR;
                st16(d + 2 * n, (uint16_t)i);
                n++;
            }
        return (uint32_t)n;
    }
    case LB_FINDSTRING: case LB_FINDSTRINGEXACT: case LB_SELECTSTRING: {
        const char *s = str_arg(u, lp);
        int at;
        if (!s || L->n == 0) return LB_ERR;
        at = list_find(L, idx, s, msg == LB_FINDSTRINGEXACT);
        if (at < 0) return LB_ERR;
        if (msg == LB_SELECTSTRING && !multi) select_item(u, hwnd, x, c, at);
        return (uint32_t)at;
    }
    case LB_GETTOPINDEX: return (uint32_t)L->top;
    case LB_SETTOPINDEX:
        if (idx < 0 || idx >= L->n) return LB_ERR;
        L->top = idx;
        fix_top(u, hwnd, x, c, -1);
        sync_sb(u, hwnd, x, c);
        wm_invalidate(&u->wm, hwnd, NULL, false);
        return 0;
    case LB_GETITEMRECT: {
        uint8_t *d = user_guest_ptr(u, lp, 8);
        fb_rect r = rows_rect(u, hwnd, x, c);
        int ih = item_h(u, hwnd, c);
        if (!d || idx < 0 || idx >= L->n) return LB_ERR;
        st16(d, (uint16_t)r.left);
        st16(d + 2, (uint16_t)(r.top + (idx - L->top) * ih));
        st16(d + 4, (uint16_t)r.right);
        st16(d + 6, (uint16_t)(r.top + (idx - L->top) * ih + ih));
        return 0;
    }
    case LB_GETITEMDATA: return idx < 0 || idx >= L->n ? LB_ERR : L->data[idx];
    case LB_SETITEMDATA:
        if (idx < 0 || idx >= L->n) return LB_ERR;
        L->data[idx] = lp;
        return 1;
    case LB_SETITEMHEIGHT:
        c->item_h = (int)(lp & 0xFFFF);
        changed(u, hwnd, x, c, -1);
        return 0;
    case LB_GETITEMHEIGHT: return (uint32_t)item_h(u, hwnd, c);
    case LB_SETCARETINDEX:
        if (idx < 0 || idx >= L->n) return LB_ERR;
        L->caret = idx;
        changed(u, hwnd, x, c, idx);
        return 0;
    case LB_GETCARETINDEX: return (uint32_t)L->caret;
    case LB_SETTABSTOPS: case LB_SETHORIZONTALEXTENT: case LB_SETCOLUMNWIDTH: return 1;
    case LB_GETHORIZONTALEXTENT: return 0;
    case LB_DIR: case LB_ADDFILE: return LB_ERR;
    default: *handled = false; return 0;
    }
}

static void select_by_key(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, int to) {
    ui_list *L = &c->list;
    if (L->n == 0) return;
    if (to < 0) to = 0;
    if (to >= L->n) to = L->n - 1;
    if (multi_sel(x)) {
        L->caret = to;
        changed(u, hwnd, x, c, to);
        return;
    }
    if (to == L->cur) return;
    select_item(u, hwnd, x, c, to);
    notify(u, hwnd, x, LBN_SELCHANGE, CBN_SELCHANGE);
}

static void on_key(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, unsigned vk) {
    ui_list *L = &c->list;
    int cur = multi_sel(x) ? L->caret : (L->cur < 0 ? 0 : L->cur), vis = visible_rows(u, hwnd, x, c);
    switch (vk) {
    case VK_DOWN: select_by_key(u, hwnd, x, c, L->cur < 0 && !multi_sel(x) ? 0 : cur + 1); break;
    case VK_UP: select_by_key(u, hwnd, x, c, cur - 1); break;
    case VK_NEXT: select_by_key(u, hwnd, x, c, cur + vis); break;
    case VK_PRIOR: select_by_key(u, hwnd, x, c, cur - vis); break;
    case VK_HOME: select_by_key(u, hwnd, x, c, 0); break;
    case VK_END: select_by_key(u, hwnd, x, c, L->n - 1); break;
    case ' ':
        if (multi_sel(x) && L->n) {
            L->sel[L->caret] = !L->sel[L->caret];
            wm_invalidate(&u->wm, hwnd, NULL, false);
            notify(u, hwnd, x, LBN_SELCHANGE, CBN_SELCHANGE);
        }
        break;
    case 0x73:    /* F4 opens the drop-down */
        if (is_combo(x) && combo_type(x) != CBS_SIMPLE) combo_dropdown(u, hwnd, x, c);
        break;
    default: break;
    }
}

static void on_vscroll(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, unsigned code, int pos) {
    ui_list *L = &c->list;
    int vis = visible_rows(u, hwnd, x, c);
    switch (code) {
    case SB_LINEUP: L->top--; break;
    case SB_LINEDOWN: L->top++; break;
    case SB_PAGEUP: L->top -= vis; break;
    case SB_PAGEDOWN: L->top += vis; break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK: L->top = pos; break;
    case SB_TOP: L->top = 0; break;
    case SB_BOTTOM: L->top = L->n; break;
    default: return;
    }
    fix_top(u, hwnd, x, c, -1);
    sync_sb(u, hwnd, x, c);
    wm_invalidate(&u->wm, hwnd, NULL, false);
}

static bool mouse(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, uint16_t msg, uint32_t lp) {
    int px = (int16_t)lp, py = (int16_t)(lp >> 16), ih = item_h(u, hwnd, c);
    ui_list *L = &c->list;
    if (is_combo(x)) {
        fb_rect cl;
        int bh = box_h(u, hwnd, c), type = combo_type(x);
        wm_client_rect(&u->wm, hwnd, &cl);
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
            if (x->style & WS_DISABLED) return true;
            wm_set_focus(&u->wm, hwnd);
            if (py < bh) {
                if (type != CBS_SIMPLE && (type == CBS_DROPDOWNLIST || px >= cl.right - 16)) combo_dropdown(u, hwnd, x, c);
            } else if (type == CBS_SIMPLE) {
                int idx = L->top + (py - bh) / ih;
                if (idx >= 0 && idx < L->n) {
                    select_item(u, hwnd, x, c, idx);
                    notify(u, hwnd, x, LBN_SELCHANGE, CBN_SELCHANGE);
                }
            }
        }
        return true;
    }
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        int idx = L->top + py / ih;
        if (x->style & WS_DISABLED) return true;
        wm_set_focus(&u->wm, hwnd);
        wm_set_capture(&u->wm, hwnd);
        c->dragging = true;
        if (idx >= 0 && idx < L->n) {
            if (multi_sel(x)) {
                L->sel[idx] = !L->sel[idx];
                L->caret = idx;
                wm_invalidate(&u->wm, hwnd, NULL, false);
                notify(u, hwnd, x, LBN_SELCHANGE, CBN_SELCHANGE);
            } else if (idx != L->cur) {
                select_item(u, hwnd, x, c, idx);
                notify(u, hwnd, x, LBN_SELCHANGE, CBN_SELCHANGE);
            }
            if (msg == WM_LBUTTONDBLCLK) notify(u, hwnd, x, LBN_DBLCLK, CBN_DBLCLK);
        }
        return true;
    }
    if (msg == WM_MOUSEMOVE && c->dragging && u->wm.capture == hwnd && !multi_sel(x)) {
        int idx = L->top + py / ih;
        if (py < 0) idx = L->top - 1;
        if (idx >= L->n) idx = L->n - 1;
        if (idx >= 0 && idx != L->cur) {
            select_item(u, hwnd, x, c, idx);
            notify(u, hwnd, x, LBN_SELCHANGE, CBN_SELCHANGE);
        }
        return true;
    }
    if (msg == WM_LBUTTONUP && c->dragging) {
        c->dragging = false;
        if (u->wm.capture == hwnd) wm_release_capture(&u->wm);
        return true;
    }
    return true;
}

bool ui_list_proc(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, uint16_t msg, uint16_t wp, uint32_t lp, uint32_t *ret) {
    bool handled;
    bool combo = is_combo(x);
    *ret = 0;
    if (c->list.n == 0 && c->list.cur == 0) c->list.cur = -1;      /* a fresh control has no selection */
    if (combo) {
        uint16_t lbm = cb_to_lb(msg);
        if (lbm) {
            *ret = list_message(u, hwnd, x, c, lbm, wp, lp, &handled);
            (void)handled;
            return true;
        }
        switch (msg) {
        case CB_SHOWDROPDOWN:
            if (wp && combo_type(x) != CBS_SIMPLE) combo_dropdown(u, hwnd, x, c);
            return true;
        case CB_GETDROPPEDSTATE: *ret = c->dropped; return true;
        case CB_GETEDITSEL: *ret = 0; return true;
        case CB_SETEDITSEL: case CB_LIMITTEXT: case CB_SETEXTENDEDUI: case CB_DIR: *ret = 1; return true;
        case CB_GETEXTENDEDUI: *ret = 0; return true;
        case CB_GETDROPPEDCONTROLRECT: return true;
        default: break;
        }
    } else {
        *ret = list_message(u, hwnd, x, c, msg, wp, lp, &handled);
        if (handled) return true;
    }
    switch (msg) {
    case WM_ERASEBKGND: *ret = 1; return true;
    case WM_PAINT: {
        wm_paint ps;
        uint16_t dc = wm_begin_paint(&u->wm, hwnd, &ps);
        if (dc) {
            if (combo) paint_combo(u, hwnd, x, c, dc);
            else paint_list(u, hwnd, x, c, dc);
            wm_end_paint(&u->wm, hwnd, &ps);
        }
        return true;
    }
    case WM_GETDLGCODE: *ret = DLGC_WANTARROWS | DLGC_WANTCHARS; return true;
    case WM_SETFOCUS:
        if (combo) ui_notify_parent(u, hwnd, CBN_SETFOCUS);
        wm_invalidate(&u->wm, hwnd, NULL, false);
        return true;
    case WM_KILLFOCUS:
        if (combo) ui_notify_parent(u, hwnd, CBN_KILLFOCUS);
        wm_invalidate(&u->wm, hwnd, NULL, false);
        return true;
    case WM_ENABLE: wm_invalidate(&u->wm, hwnd, NULL, false); return true;
    case WM_KEYDOWN: on_key(u, hwnd, x, c, wp); return true;
    case WM_CHAR:
        if (wp >= 32 && wp < 127 && c->list.n && !(combo && combo_type(x) == CBS_DROPDOWN)) {
            char s[2];
            int at;
            s[0] = (char)wp;
            s[1] = 0;
            at = list_find(&c->list, multi_sel(x) ? c->list.caret : c->list.cur, s, false);
            if (at >= 0) select_by_key(u, hwnd, x, c, at);
        } else if (combo && combo_type(x) == CBS_DROPDOWN) {
            char *t = ui_strdup(wm_get_text(&u->wm, hwnd));
            size_t n = t ? strlen(t) : 0;
            if (t && wp == 8 && n) t[n - 1] = 0;
            if (t && wp >= 32 && wp < 256 && wp != 127) {
                char *nt = realloc(t, n + 2);
                if (nt) {
                    t = nt;
                    t[n] = (char)wp;
                    t[n + 1] = 0;
                }
            }
            if (t) {
                ui_notify_parent(u, hwnd, CBN_EDITUPDATE);
                wm_set_text(&u->wm, hwnd, t);
                wm_invalidate(&u->wm, hwnd, NULL, false);
                ui_notify_parent(u, hwnd, CBN_EDITCHANGE);
                free(t);
            }
        }
        return true;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP: case WM_MOUSEMOVE:
        return mouse(u, hwnd, x, c, msg, lp);
    case WM_VSCROLL:
        on_vscroll(u, hwnd, x, c, wp, (int16_t)lp);
        return true;
    default: return false;
    }
}
