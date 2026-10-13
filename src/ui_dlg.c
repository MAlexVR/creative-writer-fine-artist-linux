/* Dialogs: templates to windows, the dialog class's default processing (DefDlgProc), modal loops, the
 * keyboard handling of IsDialogMessage, the dialog item API and MessageBox.
 *
 * A dialog is a window of class #32770 (or the class named in the template, whose window procedure calls
 * DefDlgProc) with a ui_dlg state. Every message goes to the guest DLGPROC first (DWL_DLGPROC at window word 4);
 * a TRUE answer is the result (DWL_MSGRESULT, word 0, for most messages), FALSE falls to the defaults here.
 * Template units are converted with the base units of the template's font: DS_SETFONT gives a regular
 * MS Sans Serif cell (8 point: 6x13), otherwise the system font's 8x16. */
#include <string.h>

#include "ui_priv.h"

#define WM_INITDIALOG 0x0110
#define WM_NEXTDLGCTL 0x0028
#define WM_SETFONT 0x0030
#define DM_GETDEFID 0x0400
#define DM_SETDEFID 0x0401
#define DC_HASDEFID 0x534B
#define BM_SETSTYLE 0x0404
#define BM_SETCHECK 0x0401
#define EM_SETSEL 0x0401
#define DWL_MSGRESULT 0
#define DWL_DLGPROC 4
#define DWL_USER 8

#define IDOK 1
#define IDCANCEL 2

#define DS_ABSALIGN 0x0001u
#define DS_MODALFRAME 0x0080u
#define WS_EX_DLGMODALFRAME 0x0001u

struct ui_dlg {
    uint32_t dlgproc;          /* used when the class has no room for DWL_DLGPROC */
    uint16_t def_id;
    uint16_t last_focus;
    bool ended, modal, msgbox, ok_only;
    int result;
};
typedef struct ui_dlg ui_dlg;

static ui_dlg *dlg_of(user_t *u, uint16_t hwnd) {
    ui_win *uw = ui_win_get(u->ui, hwnd, false);
    return uw ? uw->dlg : NULL;
}

/* ---- window words ------------------------------------------------------------------------------------- */

static uint32_t get_dlgproc(user_t *u, uint16_t hwnd, ui_dlg *d) {
    uint32_t v;
    if (wm_get_window_long(&u->wm, hwnd, DWL_DLGPROC, &v, false)) return v;
    return d->dlgproc;
}

static void set_dlgproc(user_t *u, uint16_t hwnd, ui_dlg *d, uint32_t proc) {
    d->dlgproc = proc;
    wm_set_window_long(&u->wm, hwnd, DWL_DLGPROC, proc, false, NULL);
}

static uint32_t msgresult(user_t *u, uint16_t hwnd) {
    uint32_t v = 0;
    wm_get_window_long(&u->wm, hwnd, DWL_MSGRESULT, &v, false);
    return v;
}

/* ---- children ------------------------------------------------------------------------------------------- */

#define MAXKIDS 128

static int kids(user_t *u, uint16_t dlg, uint16_t *out) {
    wm_window *d = wm_get(&u->wm, dlg);
    uint16_t k;
    int n = 0;
    for (k = d ? d->first_child : 0; k && n < MAXKIDS;) {
        wm_window *x = wm_get(&u->wm, k);
        if (!x) break;
        out[n++] = k;
        k = x->next;
    }
    return n;
}

static uint16_t item_by_id(user_t *u, uint16_t dlg, unsigned id) {
    uint16_t k[MAXKIDS];
    int i, n = kids(u, dlg, k);
    for (i = 0; i < n; i++)
        if (wm_get(&u->wm, k[i])->id == id) return k[i];
    return 0;
}

static bool usable(user_t *u, uint16_t h) {
    wm_window *x = wm_get(&u->wm, h);
    return x && (x->style & WS_VISIBLE) && !(x->style & WS_DISABLED);
}

static bool tabstop(user_t *u, uint16_t h) {
    wm_window *x = wm_get(&u->wm, h);
    return usable(u, h) && (x->style & WS_TABSTOP_);
}

/* The next (or previous) tab stop after cur, wrapping; cur 0 starts at the first (last). */
static uint16_t next_tab(user_t *u, uint16_t dlg, uint16_t cur, bool prev) {
    uint16_t k[MAXKIDS];
    int n = kids(u, dlg, k), i, at = -1, step = prev ? -1 : 1;
    for (i = 0; i < n; i++)
        if (k[i] == cur) at = i;
    for (i = 1; i <= n; i++) {
        int j = at < 0 ? (prev ? n - i : i - 1) : ((at + step * i) % n + n) % n;
        if (tabstop(u, k[j])) return k[j];
    }
    return 0;
}

/* Within the group of cur (from a WS_GROUP window up to the next one): the next or previous usable window. */
static uint16_t next_group(user_t *u, uint16_t dlg, uint16_t cur, bool prev) {
    uint16_t k[MAXKIDS];
    int n = kids(u, dlg, k), at = -1, lo, hi, i, span;
    for (i = 0; i < n; i++)
        if (k[i] == cur) at = i;
    if (at < 0) return 0;
    for (lo = at; lo > 0 && !(wm_get(&u->wm, k[lo])->style & WS_GROUP_); lo--) {}
    for (hi = at + 1; hi < n && !(wm_get(&u->wm, k[hi])->style & WS_GROUP_); hi++) {}
    span = hi - lo;
    for (i = 1; i < span; i++) {
        int j = lo + (((at - lo) + (prev ? -i : i)) % span + span) % span;
        if (usable(u, k[j])) return k[j];
    }
    return cur;
}

static unsigned dlgcode(user_t *u, uint16_t h, const wm_msg *m) {
    uint8_t raw[18];
    uint32_t mark = user_scratch_mark(u), far, r;
    st16(raw, m->hwnd);
    st16(raw + 2, m->message);
    st16(raw + 4, m->wparam);
    st32(raw + 6, m->lparam);
    st32(raw + 10, m->time);
    st16(raw + 14, (uint16_t)m->pt_x);
    st16(raw + 16, (uint16_t)m->pt_y);
    far = user_scratch_push(u, raw, sizeof raw);
    r = wm_send(&u->wm, h, WM_GETDLGCODE, 0, far);
    user_scratch_release(u, mark);
    return r & 0xFFFF;
}

static void set_default_button(user_t *u, uint16_t dlg, uint16_t btn) {
    uint16_t k[MAXKIDS];
    int i, n = kids(u, dlg, k);
    for (i = 0; i < n; i++) {
        wm_window *x = wm_get(&u->wm, k[i]);
        unsigned t = x->style & 0xF;
        bool should = k[i] == btn;
        if (!x->cls || strcmp(x->cls->name, "BUTTON") || (t != 0 && t != 1)) continue;
        if ((t == 1) != should) wm_send(&u->wm, k[i], BM_SETSTYLE, (uint16_t)((x->style & 0xFFF0) | (should ? 1 : 0)), 1);
    }
}

/* A push button took or lost the focus: it is the default button while it has it. */
void ui_dlg_btn_focus(user_t *u, uint16_t btn, bool gained, uint16_t other) {
    wm_window *x = wm_get(&u->wm, btn);
    ui_dlg *d = x ? dlg_of(u, x->parent) : NULL;
    unsigned t = x ? x->style & 0xF : 0;
    wm_window *o;
    if (!d) return;
    if (gained) {
        if (t == 0 || t == 1) set_default_button(u, x->parent, btn);
        return;
    }
    o = wm_get(&u->wm, other);
    if (o && o->parent == x->parent && o->cls && !strcmp(o->cls->name, "BUTTON") && ((o->style & 0xF) == 0 || (o->style & 0xF) == 1))
        return;                                              /* it becomes the default itself */
    set_default_button(u, x->parent, item_by_id(u, x->parent, d->def_id));
}

static void focus_control(user_t *u, uint16_t dlg, uint16_t h) {
    wm_window *x = wm_get(&u->wm, h);
    if (!x) return;
    wm_set_focus(&u->wm, h);
    if (x->cls && !strcmp(x->cls->name, "EDIT") && !(u->abort))
        wm_send(&u->wm, h, EM_SETSEL, 0, 0x7FFF0000u);      /* a tab into an edit control selects its text */
    (void)dlg;
}

/* ---- IsDialogMessage ---------------------------------------------------------------------------------------- */

static void click_button(user_t *u, uint16_t dlg, uint16_t btn) {
    wm_window *x = wm_get(&u->wm, btn);
    if (!x) return;
    wm_send(&u->wm, dlg, WM_COMMAND, x->id, btn);
}

static bool mnemonic_of(const char *t, unsigned *ch) {
    for (; t && *t; t++)
        if (*t == '&') {
            if (t[1] == '&') {
                t++;
                continue;
            }
            if (!t[1]) return false;
            *ch = (unsigned char)t[1] | 0x20;
            return true;
        }
    return false;
}

static bool dialog_mnemonic(user_t *u, uint16_t dlg, uint16_t focus, unsigned c) {
    uint16_t k[MAXKIDS];
    int n = kids(u, dlg, k), at = -1, i;
    for (i = 0; i < n; i++)
        if (k[i] == focus) at = i;
    for (i = 1; i <= n; i++) {
        uint16_t h = k[((at < 0 ? -1 : at) + i + n) % n];
        wm_window *x = wm_get(&u->wm, h);
        unsigned m;
        if (!usable(u, h) || !x->cls || !mnemonic_of(wm_get_text(&u->wm, h), &m) || m != (c | 0x20)) continue;
        if (!strcmp(x->cls->name, "STATIC")) {                /* a label: the next control that can take the focus */
            uint16_t g = next_tab(u, dlg, h, false);
            if (g) focus_control(u, dlg, g);
        } else if (!strcmp(x->cls->name, "BUTTON")) {
            unsigned t = x->style & 0xF;
            if (t == 7) continue;
            if (t != 0 && t != 1) wm_set_focus(&u->wm, h);
            click_button(u, dlg, h);
        } else {
            focus_control(u, dlg, h);
        }
        return true;
    }
    return false;
}

bool ui_is_dialog_message(user_t *u, uint16_t dlg, const wm_msg *m) {
    unsigned code;
    wm_window *dw = wm_get(&u->wm, dlg);
    ui_dlg *d = dlg_of(u, dlg);
    uint16_t f = m->hwnd, tgt;
    if (!dw || !d) return false;
    if (f != dlg && !wm_is_child(&u->wm, dlg, f)) return false;
    switch (m->message) {
    case WM_KEYDOWN:
        code = f == dlg ? 0 : dlgcode(u, f, m);
        switch (m->wparam) {
        case VK_TAB:
            if (code & DLGC_WANTTAB) return false;
            tgt = next_tab(u, dlg, f == dlg ? 0 : f, (wm_key_state(&u->wm, VK_SHIFT) & 0x8000) != 0);
            if (tgt) focus_control(u, dlg, tgt);
            return true;
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
            if ((code & (DLGC_WANTARROWS | DLGC_WANTALLKEYS)) || f == dlg) return false;
            tgt = next_group(u, dlg, f, m->wparam == VK_LEFT || m->wparam == VK_UP);
            if (tgt && tgt != f) {
                wm_window *t = wm_get(&u->wm, tgt);
                focus_control(u, dlg, tgt);
                if (t && t->cls && !strcmp(t->cls->name, "BUTTON") && ((t->style & 0xF) == 9 || (t->style & 0xF) == 4))
                    click_button(u, dlg, tgt);               /* arrows select radio buttons */
            }
            return true;
        case VK_RETURN: {
            uint16_t def;
            if (code & DLGC_WANTALLKEYS) return false;
            if (code & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) {
                click_button(u, dlg, f);
                return true;
            }
            def = item_by_id(u, dlg, d->def_id ? d->def_id : IDOK);
            if (def && !usable(u, def)) return true;
            wm_send(&u->wm, dlg, WM_COMMAND, d->def_id ? d->def_id : IDOK, def);
            return true;
        }
        case VK_ESCAPE: {
            uint16_t c;
            if (code & DLGC_WANTALLKEYS) return false;
            c = item_by_id(u, dlg, IDCANCEL);
            if (c && !usable(u, c)) return true;
            wm_send(&u->wm, dlg, WM_COMMAND, IDCANCEL, c);
            return true;
        }
        default: return false;
        }
    case WM_SYSCHAR:
        if (m->wparam > ' ' && m->wparam < 0x7F) return dialog_mnemonic(u, dlg, f, m->wparam);
        return false;
    default: return false;
    }
}

/* ---- default processing ---------------------------------------------------------------------------------------- */

static void end_dialog(user_t *u, uint16_t hwnd, int result) {
    ui_dlg *d = dlg_of(u, hwnd);
    if (!d) return;
    d->result = result;
    d->ended = true;
}

static bool dlg_proc(user_t *u, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp, uint32_t *ret) {
    ui_dlg *d = dlg_of(u, hwnd);
    uint32_t proc, r = 0;
    *ret = 0;
    if (!d) return false;
    proc = get_dlgproc(u, hwnd, d);
    if (proc) {
        uint16_t args[5];
        args[0] = hwnd;
        args[1] = msg;
        args[2] = wp;
        args[3] = (uint16_t)(lp >> 16);
        args[4] = (uint16_t)lp;
        if (!user_call_guest(u, proc, args, 5, &r)) return true;
        if (!dlg_of(u, hwnd)) return true;                     /* the procedure destroyed the dialog */
        if (r & 0xFFFF) {
            switch (msg) {
            case WM_CTLCOLOR: case 0x37: case 0x39: case 0x2E: case 0x2F: case WM_INITDIALOG: *ret = r & 0xFFFF; break;
            default: *ret = msgresult(u, hwnd); break;
            }
            return true;
        }
    }
    switch (msg) {
    case WM_CLOSE: {
        uint16_t c = item_by_id(u, hwnd, IDCANCEL);
        if (!c || usable(u, c)) wm_send(&u->wm, hwnd, WM_COMMAND, IDCANCEL, c);
        return true;
    }
    case WM_COMMAND:
        if (d->msgbox) {
            if (wp == IDCANCEL && !item_by_id(u, hwnd, IDCANCEL)) {
                if (d->ok_only) end_dialog(u, hwnd, IDOK);
            } else if (wp >= IDOK && wp <= 7) {
                end_dialog(u, hwnd, wp);
            }
        }
        return true;
    case WM_ERASEBKGND: {
        uint32_t col = wm_sys_color(15), c;
        fb_rect cl;
        uint16_t br = (uint16_t)wm_send(&u->wm, hwnd, WM_CTLCOLOR, wp, 4u << 16 | hwnd);
        if (br && u->has_gfx && u->gfx.brush_color(u->gfx.ctx, br, &c)) col = c;
        wm_client_rect(&u->wm, hwnd, &cl);
        ui_fill(u, wp, 0, 0, cl.right, cl.bottom, col);
        *ret = 1;
        return true;
    }
    case WM_SETFOCUS: {
        uint16_t t = d->last_focus && wm_is_window(&u->wm, d->last_focus) && usable(u, d->last_focus) ? d->last_focus : next_tab(u, hwnd, 0, false);
        if (t) focus_control(u, hwnd, t);
        return true;
    }
    case WM_ACTIVATE:
        if (wp == 0) {
            if (u->wm.focus && wm_is_child(&u->wm, hwnd, u->wm.focus)) d->last_focus = u->wm.focus;
        } else {
            uint16_t t = d->last_focus && wm_is_window(&u->wm, d->last_focus) && usable(u, d->last_focus) ? d->last_focus : 0;
            if (t) wm_set_focus(&u->wm, t);
            else if (!u->wm.focus || !wm_is_child(&u->wm, hwnd, u->wm.focus)) {
                t = next_tab(u, hwnd, 0, false);
                if (t) focus_control(u, hwnd, t);
            }
        }
        return true;
    case WM_NEXTDLGCTL: {
        uint16_t t;
        if (lp & 0xFFFF) t = wp;
        else t = next_tab(u, hwnd, u->wm.focus, wp != 0);
        if (t && wm_is_window(&u->wm, t)) focus_control(u, hwnd, t);
        return true;
    }
    case DM_GETDEFID:
        *ret = d->def_id ? (uint32_t)DC_HASDEFID << 16 | d->def_id : 0;
        return true;
    case DM_SETDEFID:
        d->def_id = wp;
        set_default_button(u, hwnd, item_by_id(u, hwnd, wp));
        *ret = 1;
        return true;
    default:
        return false;
    }
}

bool ui_dlg_defproc(user_t *u, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp, uint32_t *ret) {
    return dlg_proc(u, hwnd, msg, wp, lp, ret);
}

/* ---- creation from a template --------------------------------------------------------------------------------- */

typedef struct {
    const res_dlg *t;
    uint16_t hinst, parent;
    uint32_t dlgproc, param;
    bool force_visible, msgbox, ok_only;
    int bx, by;                 /* base units; 0: from the template's font */
    int abs_x, abs_y;           /* position in pixels when abs_pos */
    bool abs_pos;
} dlg_params;

static void base_units(user_t *u, const res_dlg *t, int *bx, int *by, int *cell) {
    int pt = t->pointsize;
    *cell = 0;
    if (pt) {
        *cell = (pt * 16 + 6) / 10;
        if (u->has_gfx) u->gfx.font_metrics(u->gfx.ctx, *cell, bx, by);
        else {
            *bx = 6;
            *by = 13;
        }
    } else if (u->has_gfx) {
        u->gfx.font_metrics(u->gfx.ctx, 0, bx, by);
    } else {
        *bx = 8;
        *by = 16;
    }
}

static uint16_t dlg_create(user_t *u, const dlg_params *p) {
    const res_dlg *t = p->t;
    wm_create_params cp;
    uint32_t style = t->style, exstyle = 0;
    int bx, by, cell, l, tt, r, b, cw, ch, x, y, ww, wh, i;
    uint16_t hmenu = 0, hdlg, first = 0;
    ui_win *uw;
    ui_dlg *d;
    char cname[40];
    if (p->bx) {
        bx = p->bx;
        by = p->by;
        cell = 0;
    } else {
        base_units(u, t, &bx, &by, &cell);
    }
    if (t->menu_is_id || t->menu[0]) {
        const uint8_t *md;
        size_t ml;
        if (p->hinst == u->p->auto_ds &&
            res_find(u->p->ne, RT_MENU, !t->menu_is_id, t->menu_id, t->menu, &md, &ml) == RES_OK)
            hmenu = ui_menu_load(u->ui, md, ml);
    }
    if (style & DS_MODALFRAME) exstyle |= WS_EX_DLGMODALFRAME;
    if (!(style & (WS_POPUP | WS_CHILD))) style |= WS_POPUP;
    cw = t->cx * bx / 4;
    ch = t->cy * by / 8;
    wm_nc_insets(style, exstyle, hmenu != 0 && !(style & WS_CHILD), &l, &tt, &r, &b);
    ww = cw + l + r;
    wh = ch + tt + b;
    if (p->abs_pos) {
        x = p->abs_x;
        y = p->abs_y;
    } else {
        int ox = 0, oy = 0;
        x = t->x * bx / 4;
        y = t->y * by / 8;
        if (!(style & DS_ABSALIGN) && p->parent && wm_is_window(&u->wm, p->parent) && !(style & WS_CHILD)) {
            wm_client_origin(&u->wm, p->parent, &ox, &oy);
            x += ox;
            y += oy;
        }
        if (x + ww > u->fb.w) x = u->fb.w - ww;
        if (y + wh > u->fb.h) y = u->fb.h - wh;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
    }
    memset(&cp, 0, sizeof cp);
    snprintf(cname, sizeof cname, "%s", t->cls[0] ? t->cls : "#32770");
    cp.class_name = cname;
    cp.text = t->caption;
    cp.style = style & ~WS_VISIBLE;
    cp.exstyle = exstyle;
    cp.x = (int16_t)x;
    cp.y = (int16_t)y;
    cp.cx = (int16_t)ww;
    cp.cy = (int16_t)wh;
    cp.parent = p->parent;
    cp.id_or_menu = hmenu;
    cp.hinstance = p->hinst;
    cp.param = p->param;
    if (!(hdlg = wm_create_window(&u->wm, &cp))) {
        ui_menu_destroy(u->ui, hmenu);
        return 0;
    }
    uw = ui_win_get(u->ui, hdlg, true);
    d = calloc(1, sizeof *d);
    if (!uw || !d) {
        free(d);
        wm_destroy_window(&u->wm, hdlg);
        return 0;
    }
    uw->dlg = d;
    uw->cell = cell;
    uw->base_x = bx;
    uw->base_y = by;
    d->msgbox = p->msgbox;
    d->ok_only = p->ok_only;
    set_dlgproc(u, hdlg, d, p->dlgproc);
    wm_set_window_long(&u->wm, hdlg, DWL_USER, 0, false, NULL);
    for (i = 0; i < (int)t->nitems; i++) {
        const res_dlg_item *it = &t->items[i];
        wm_create_params ip;
        char text[128], icls[40];
        uint16_t h;
        memset(&ip, 0, sizeof ip);
        if (it->text_is_id) snprintf(text, sizeof text, "#%u", it->text_id);
        else snprintf(text, sizeof text, "%s", it->text);
        snprintf(icls, sizeof icls, "%s", it->cls);
        ip.class_name = icls;
        ip.text = text;
        ip.style = it->style | WS_CHILD;
        ip.x = (int16_t)(it->x * bx / 4);
        ip.y = (int16_t)(it->y * by / 8);
        ip.cx = (int16_t)(it->cx * bx / 4);
        ip.cy = (int16_t)(it->cy * by / 8);
        ip.parent = hdlg;
        ip.id_or_menu = it->id;
        ip.hinstance = p->hinst;
        if (!(h = wm_create_window(&u->wm, &ip))) {
            wm_destroy_window(&u->wm, hdlg);
            return 0;
        }
        /* the child order is the template order, and with it the tab order */
        wm_set_window_pos(&u->wm, h, 1, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (!d->def_id && it->cls[0] && !strcmp(it->cls, "BUTTON") && (it->style & 0xF) == 1) d->def_id = it->id;
    }
    {   /* WM_INITDIALOG: the first tab stop, else the first control that can take the focus */
        uint16_t k[MAXKIDS];
        int n = kids(u, hdlg, k), j;
        uint32_t ok;
        for (j = 0; j < n && !first; j++)
            if (tabstop(u, k[j])) first = k[j];
        for (j = 0; j < n && !first; j++)
            if (usable(u, k[j]) && wm_get(&u->wm, k[j])->cls && strcmp(wm_get(&u->wm, k[j])->cls->name, "STATIC")) first = k[j];
        ok = wm_send(&u->wm, hdlg, WM_INITDIALOG, first, p->param);
        if (u->abort || !wm_is_window(&u->wm, hdlg)) return 0;
        if ((ok & 0xFFFF) && first) focus_control(u, hdlg, first);
    }
    if ((t->style & WS_VISIBLE) || p->force_visible) wm_show_window(&u->wm, hdlg, SW_SHOWNORMAL);
    return wm_is_window(&u->wm, hdlg) ? hdlg : 0;
}

/* ---- modal loop ---------------------------------------------------------------------------------------------------- */

static int dlg_run_modal(user_t *u, uint16_t hdlg, uint16_t owner, bool *failed) {
    ui_dlg *d = dlg_of(u, hdlg);
    int result;
    bool owner_ok = owner && wm_is_window(&u->wm, owner);
    *failed = false;
    if (!d) {
        *failed = true;
        return -1;
    }
    d->modal = true;
    if (owner_ok) wm_enable_window(&u->wm, owner, false);
    while (!d->ended && !u->abort && wm_is_window(&u->wm, hdlg)) {
        wm_msg m;
        if (wm_next_message(&u->wm, &m, 0, 0, 0, true)) {
            if (m.message == WM_QUIT) {
                wm_post_quit(&u->wm, (int16_t)m.wparam);
                d->ended = true;
                d->result = 0;
                break;
            }
            if (!ui_is_dialog_message(u, hdlg, &m)) {
                wm_translate(&u->wm, &m);
                wm_dispatch(&u->wm, &m);
            }
        } else if (!user_idle(u, true)) {
            u->abort = loader_request_stop(u->p);
        }
        d = dlg_of(u, hdlg);
        if (!d) break;
    }
    result = d ? d->result : 0;
    if (owner_ok && wm_is_window(&u->wm, owner)) wm_enable_window(&u->wm, owner, true);
    if (wm_is_window(&u->wm, hdlg)) wm_destroy_window(&u->wm, hdlg);
    if (owner_ok && wm_is_window(&u->wm, owner)) wm_set_active(&u->wm, owner);
    return result;
}

/* ---- the API ---------------------------------------------------------------------------------------------------------- */

static bool load_template(hle_call *c, user_t *u, uint16_t inst, uint16_t sel, uint16_t off, res_dlg *t) {
    char name[64];
    const uint8_t *d;
    size_t dl;
    if (inst != u->p->auto_ds) return false;
    if (sel) {
        if (!hle_read_cstr(c, sel, off, name, sizeof name)) return false;
        if (name[0] == '#') {
            off = (uint16_t)atoi(name + 1);
            sel = 0;
        }
    }
    if (res_find(u->p->ne, RT_DIALOG, sel != 0, off, sel ? name : NULL, &d, &dl) != RES_OK) return false;
    return res_dlg_parse(d, dl, t) == RES_OK;
}

static bool template_at(hle_call *c, uint16_t sel, uint16_t off, res_dlg *t) {
    uint32_t avail = sel ? mem_size(&((user_t *)c->ctx)->p->mem, sel) : 0;
    const uint8_t *d;
    if (avail <= off || !(d = hle_ptr(c, sel, off, avail - off))) return false;
    return res_dlg_parse(d, avail - off, t) == RES_OK;
}

enum { K_DIALOGBOX, K_CREATE };

static cpu_status dialog_api(hle_call *c, int kind, bool indirect, bool with_param) {
    UBEGIN;
    uint16_t inst = hle_u16(&a), tsel = 0, toff = 0, hglobal = 0, parent, psel, poff;
    uint32_t param = 0;
    res_dlg t;
    dlg_params p;
    uint16_t hdlg;
    bool ok;
    if (indirect && kind == K_DIALOGBOX) hglobal = hle_u16(&a);
    else hle_far(&a, &tsel, &toff);
    parent = hle_u16(&a);
    hle_far(&a, &psel, &poff);
    if (with_param) param = hle_u32(&a);
    UCHECK;
    if (indirect && kind == K_DIALOGBOX) ok = template_at(c, hglobal, 0, &t);
    else if (indirect) ok = template_at(c, tsel, toff, &t);
    else ok = load_template(c, u, inst, tsel, toff, &t);
    if (!ok) {
        hle_ret16(c, kind == K_DIALOGBOX ? 0xFFFF : 0);
        return CPU_OK;
    }
    memset(&p, 0, sizeof p);
    p.t = &t;
    p.hinst = inst;
    p.parent = parent;
    p.dlgproc = (uint32_t)psel << 16 | poff;
    p.param = param;
    p.force_visible = kind == K_DIALOGBOX;
    hdlg = dlg_create(u, &p);
    res_dlg_free(&t);
    if (u->abort) return u->abort;
    if (kind == K_CREATE) {
        hle_ret16(c, hdlg);
        return CPU_OK;
    }
    if (!hdlg) {
        hle_ret16(c, 0xFFFF);
        return CPU_OK;
    }
    {
        bool failed;
        int r = dlg_run_modal(u, hdlg, parent, &failed);
        hle_ret16(c, failed ? 0xFFFF : (uint16_t)r);
    }
    return u->abort;
}

static cpu_status u_dialogbox(hle_call *c) { return dialog_api(c, K_DIALOGBOX, false, false); }
static cpu_status u_dialogboxparam(hle_call *c) { return dialog_api(c, K_DIALOGBOX, false, true); }
static cpu_status u_dialogboxindirect(hle_call *c) { return dialog_api(c, K_DIALOGBOX, true, false); }
static cpu_status u_createdialog(hle_call *c) { return dialog_api(c, K_CREATE, false, false); }
static cpu_status u_createdialogparam(hle_call *c) { return dialog_api(c, K_CREATE, false, true); }
static cpu_status u_createdialogindirect(hle_call *c) { return dialog_api(c, K_CREATE, true, false); }

static cpu_status u_enddialog(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), r = hle_u16(&a);
    UCHECK;
    end_dialog(u, h, (int16_t)r);
    hle_ret16(c, dlg_of(u, h) != NULL);
    return CPU_OK;
}

static cpu_status u_isdialogmessage(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    wm_msg m;
    const uint8_t *p;
    hle_far(&a, &sel, &off);
    UCHECK;
    if (!(p = hle_ptr(c, sel, off, 18))) return CPU_E_SEL;
    memset(&m, 0, sizeof m);
    m.hwnd = ld16(p);
    m.message = ld16(p + 2);
    m.wparam = ld16(p + 4);
    m.lparam = ld32(p + 6);
    m.time = ld32(p + 10);
    m.pt_x = (int16_t)ld16(p + 14);
    m.pt_y = (int16_t)ld16(p + 16);
    hle_ret16(c, ui_is_dialog_message(u, h, &m));
    return u->abort;
}

static cpu_status u_getdlgitem(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), id = hle_u16(&a);
    UCHECK;
    hle_ret16(c, item_by_id(u, h, id));
    return CPU_OK;
}

static cpu_status u_setdlgitemtext(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), id = hle_u16(&a), sel, off, it;
    hle_far(&a, &sel, &off);
    UCHECK;
    it = item_by_id(u, h, id);
    if (it) wm_send(&u->wm, it, WM_SETTEXT, 0, (uint32_t)sel << 16 | off);
    return u->abort;
}

static cpu_status u_getdlgitemtext(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), id = hle_u16(&a), sel, off, it, max;
    uint8_t *d;
    hle_far(&a, &sel, &off);
    max = hle_u16(&a);
    UCHECK;
    it = item_by_id(u, h, id);
    if (!it) {
        if (max && (d = hle_ptr(c, sel, off, 1))) d[0] = 0;
        hle_ret16(c, 0);
        return CPU_OK;
    }
    hle_ret16(c, (uint16_t)wm_send(&u->wm, it, WM_GETTEXT, max, (uint32_t)sel << 16 | off));
    return u->abort;
}

static cpu_status u_setdlgitemint(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), id = hle_u16(&a), v = hle_u16(&a), sgn = hle_u16(&a), it;
    char buf[16];
    uint32_t mark, far;
    UCHECK;
    it = item_by_id(u, h, id);
    if (!it) return CPU_OK;
    snprintf(buf, sizeof buf, sgn ? "%d" : "%u", sgn ? (int)(int16_t)v : (int)v);
    mark = user_scratch_mark(u);
    far = user_scratch_push(u, buf, (uint32_t)strlen(buf) + 1);
    if (far) wm_send(&u->wm, it, WM_SETTEXT, 0, far);
    user_scratch_release(u, mark);
    return u->abort;
}

static cpu_status u_getdlgitemint(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), id = hle_u16(&a), sel, off, sgn, it;
    char buf[64];
    const char *s;
    long v = 0;
    bool neg = false, ok = false;
    uint8_t *tp;
    hle_far(&a, &sel, &off);
    sgn = hle_u16(&a);
    UCHECK;
    it = item_by_id(u, h, id);
    if (it) {
        strncpy(buf, wm_get_text(&u->wm, it), sizeof buf - 1);
        buf[sizeof buf - 1] = 0;
        s = buf;
        while (*s == ' ' || *s == '\t') s++;
        if (sgn && *s == '-') {
            neg = true;
            s++;
        }
        for (ok = *s >= '0' && *s <= '9'; *s >= '0' && *s <= '9'; s++) {
            v = v * 10 + (*s - '0');
            if (v > 0x10000) {
                ok = false;
                break;
            }
        }
        if (*s) ok = false;
        if (ok && (sgn ? v > (neg ? 32768 : 32767) : v > 65535)) ok = false;
    }
    if (sel && (tp = hle_ptr(c, sel, off, 2))) st16(tp, ok);
    hle_ret16(c, ok ? (uint16_t)(neg ? -v : v) : 0);
    return CPU_OK;
}

static cpu_status u_checkdlgbutton(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), id = hle_u16(&a), chk = hle_u16(&a), it;
    UCHECK;
    if ((it = item_by_id(u, h, id))) wm_send(&u->wm, it, BM_SETCHECK, chk, 0);
    return u->abort;
}

static cpu_status u_isdlgbuttonchecked(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), id = hle_u16(&a), it;
    UCHECK;
    it = item_by_id(u, h, id);
    hle_ret16(c, it ? (uint16_t)wm_send(&u->wm, it, 0x0400, 0, 0) : 0);
    return u->abort;
}

static cpu_status u_checkradiobutton(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), first = hle_u16(&a), last = hle_u16(&a), chk = hle_u16(&a), id, it;
    UCHECK;
    for (id = first; id <= last; id++)
        if ((it = item_by_id(u, h, id))) wm_send(&u->wm, it, BM_SETCHECK, id == chk, 0);
    return u->abort;
}

static cpu_status u_senddlgitemmessage(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), id = hle_u16(&a), msg = hle_u16(&a), wp = hle_u16(&a), it;
    uint32_t lp = hle_u32(&a);
    UCHECK;
    it = item_by_id(u, h, id);
    hle_ret32(c, it ? wm_send(&u->wm, it, msg, wp, lp) : 0);
    return u->abort;
}

static cpu_status u_getdlgctrlid(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a);
    wm_window *x;
    UCHECK;
    x = wm_get(&u->wm, h);
    hle_ret16(c, x ? x->id : 0);
    return CPU_OK;
}

static cpu_status u_mapdialogrect(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    ui_win *uw;
    uint8_t *p;
    int bx = 8, by = 16;
    hle_far(&a, &sel, &off);
    UCHECK;
    if (!(p = hle_ptr(c, sel, off, 8))) return CPU_E_SEL;
    if ((uw = ui_win_get(u->ui, h, false)) && uw->dlg) {
        bx = uw->base_x;
        by = uw->base_y;
    }
    st16(p, (uint16_t)((int16_t)ld16(p) * bx / 4));
    st16(p + 2, (uint16_t)((int16_t)ld16(p + 2) * by / 8));
    st16(p + 4, (uint16_t)((int16_t)ld16(p + 4) * bx / 4));
    st16(p + 6, (uint16_t)((int16_t)ld16(p + 6) * by / 8));
    return CPU_OK;
}

static cpu_status next_item(hle_call *c, bool group) {
    UBEGIN;
    uint16_t h = hle_u16(&a), cur = hle_u16(&a), prev = hle_u16(&a);
    UCHECK;
    if (group) hle_ret16(c, cur ? next_group(u, h, cur, prev != 0) : next_tab(u, h, 0, prev != 0));
    else hle_ret16(c, next_tab(u, h, cur, prev != 0));
    return CPU_OK;
}

static cpu_status u_nextdlggroupitem(hle_call *c) { return next_item(c, true); }
static cpu_status u_nextdlgtabitem(hle_call *c) { return next_item(c, false); }

static cpu_status u_defdlgproc(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), msg = hle_u16(&a), wp = hle_u16(&a);
    uint32_t lp = hle_u32(&a), r = 0;
    UCHECK;
    if (!dlg_proc(u, h, msg, wp, lp, &r)) r = user_defproc(u, h, msg, wp, lp);
    hle_ret32(c, r);
    return u->abort;
}

/* ---- MessageBox ---------------------------------------------------------------------------------------------------------- */

static const char *const mb_label[8] = {"", "OK", "Cancel", "&Abort", "&Retry", "&Ignore", "&Yes", "&No"};

static cpu_status u_messagebox(hle_call *c) {
    UBEGIN;
    uint16_t parent, tsel, toff, csel, coff, type;
    char text[512] = "", caption[128] = "Error";
    static const int sets[6][3] = {{1, 0, 0}, {1, 2, 0}, {3, 4, 5}, {6, 7, 2}, {6, 7, 0}, {4, 2, 0}};
    int ids[3], nb = 0, i, defb, icon_id = 0, th, tw = 0, maxw = 300, tx, ty, cw, chh, bw = 70, bh, gap = 12, btotal, content_h;
    tline ln[48];
    int nl, lines_h;
    res_dlg t;
    dlg_params p;
    res_dlg_item items[5];
    uint16_t hdlg;
    int result = 0;
    char wrapped[512];
    parent = hle_u16(&a);
    hle_far(&a, &tsel, &toff);
    hle_far(&a, &csel, &coff);
    type = hle_u16(&a);
    UCHECK;
    if (tsel) hle_read_cstr(c, tsel, toff, text, sizeof text);
    if (csel) hle_read_cstr(c, csel, coff, caption, sizeof caption);
    for (i = 0; i < 3; i++)
        if (sets[(type & 0xF) < 6 ? (type & 0xF) : 0][i]) ids[nb++] = sets[(type & 0xF) < 6 ? (type & 0xF) : 0][i];
    defb = (type >> 8) & 3;
    if (defb >= nb) defb = 0;
    switch (type & 0xF0) {
    case 0x10: icon_id = 32513; break;
    case 0x20: icon_id = 32514; break;
    case 0x30: icon_id = 32515; break;
    case 0x40: icon_id = 32516; break;
    default: break;
    }
    /* layout in pixels with the system font */
    th = ui_th(u, 0);
    strncpy(wrapped, text, sizeof wrapped - 1);
    wrapped[sizeof wrapped - 1] = 0;
    nl = ui_wrap_text(u, 0, wrapped, maxw, true, ln, 48);
    for (i = 0; i < nl; i++) {
        int w = ui_tw(u, 0, wrapped + ln[i].start, ln[i].len);
        if (w > tw) tw = w;
    }
    lines_h = nl * th;
    for (i = 0; i < nb; i++) {
        const char *l = mb_label[ids[i]];
        int w = ui_tw(u, 0, l, (int)strlen(l)) + 24;
        if (w > bw) bw = w;
    }
    bh = th + 10;
    btotal = nb * bw + (nb - 1) * gap;
    tx = 16 + (icon_id ? 32 + 16 : 0);
    content_h = lines_h > (icon_id ? 32 : 0) ? lines_h : 32 * (icon_id != 0);
    ty = 16 + (icon_id && lines_h < 32 ? (32 - lines_h) / 2 : 0);
    cw = tx + tw + 16;
    if (btotal + 32 > cw) cw = btotal + 32;
    if (cw < 130) cw = 130;
    chh = 16 + content_h + 16 + bh + 14;
    memset(&t, 0, sizeof t);
    memset(items, 0, sizeof items);
    t.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    t.cx = (int16_t)cw;
    t.cy = (int16_t)chh;
    snprintf(t.caption, sizeof t.caption, "%s", caption);
    t.nitems = 0;
    t.items = items;
    if (icon_id) {
        res_dlg_item *it = &items[t.nitems++];
        it->x = 16; it->y = 16; it->cx = 32; it->cy = 32;
        it->id = 0xFFFF;
        it->style = WS_CHILD | WS_VISIBLE | 3;
        strcpy(it->cls, "STATIC");
        it->text_is_id = true;
        it->text_id = (uint16_t)icon_id;
    }
    {
        res_dlg_item *it = &items[t.nitems++];
        it->x = (int16_t)tx; it->y = (int16_t)ty; it->cx = (int16_t)(tw + 4); it->cy = (int16_t)(lines_h + 2);
        it->id = 0xFFFF;
        it->style = WS_CHILD | WS_VISIBLE | 0x80;
        strcpy(it->cls, "STATIC");
    }
    for (i = 0; i < nb && t.nitems < 5; i++) {
        res_dlg_item *it = &items[t.nitems++];
        it->x = (int16_t)((cw - btotal) / 2 + i * (bw + gap));
        it->y = (int16_t)(16 + content_h + 16);
        it->cx = (int16_t)bw;
        it->cy = (int16_t)bh;
        it->id = (uint16_t)ids[i];
        it->style = WS_CHILD | WS_VISIBLE | WS_TABSTOP_ | (i == defb ? 1u : 0u) | (i == 0 ? WS_GROUP_ : 0);
        strcpy(it->cls, "BUTTON");
        snprintf(it->text, sizeof it->text, "%s", mb_label[ids[i]]);
    }
    {   /* items hold nb + 2 entries at most: icon, text, three buttons */
        memset(&p, 0, sizeof p);
        p.t = &t;
        p.hinst = u->p->auto_ds;
        p.parent = parent;
        p.force_visible = true;
        p.msgbox = true;
        p.ok_only = (type & 0xF) == 0;
        p.bx = 4;
        p.by = 8;
        p.abs_pos = true;
        {
            int wl, wt, wr, wb, ww, wh;
            wm_nc_insets(t.style, WS_EX_DLGMODALFRAME, false, &wl, &wt, &wr, &wb);
            ww = cw + wl + wr;
            wh = chh + wt + wb;
            p.abs_x = (u->fb.w - ww) / 2;
            p.abs_y = (u->fb.h - wh) / 2;
        }
        hdlg = dlg_create(u, &p);
        if (hdlg) {
            bool failed;
            ui_dlg *d = dlg_of(u, hdlg);
            {   /* the item text buffer of a template is short; the message goes in whole */
                uint16_t k[MAXKIDS];
                int j, n = kids(u, hdlg, k);
                for (j = 0; j < n; j++)
                    if (wm_get(&u->wm, k[j])->id == 0xFFFF && (wm_get(&u->wm, k[j])->style & 0xF) == 0) wm_set_text(&u->wm, k[j], text);
            }
            if (d) {
                uint16_t db = item_by_id(u, hdlg, (unsigned)ids[defb]);
                d->def_id = (uint16_t)ids[defb];
                if (db) {
                    set_default_button(u, hdlg, db);
                    wm_set_focus(&u->wm, db);
                }
            }
            result = dlg_run_modal(u, hdlg, parent, &failed);
            if (failed) result = 0;
        }
    }
    user_log(u, "MessageBox parent=%04x type=%04x caption=\"%s\" text=\"%s\" -> %u", parent, type, caption, text, result);
    hle_ret16(c, (uint16_t)result);
    return u->abort;
}

#define U USER_MODULE
const hle_def ui_dlg_defs[] = {
    {U, 1, "MessageBox", 12, u_messagebox},
    {U, 87, "DialogBox", 12, u_dialogbox},
    {U, 88, "EndDialog", 4, u_enddialog},
    {U, 89, "CreateDialog", 12, u_createdialog},                        /* UNV */
    {U, 90, "IsDialogMessage", 6, u_isdialogmessage},                   /* UNV */
    {U, 91, "GetDlgItem", 4, u_getdlgitem},
    {U, 92, "SetDlgItemText", 8, u_setdlgitemtext},
    {U, 93, "GetDlgItemText", 10, u_getdlgitemtext},                    /* UNV */
    {U, 94, "SetDlgItemInt", 8, u_setdlgitemint},                       /* UNV */
    {U, 95, "GetDlgItemInt", 10, u_getdlgitemint},                      /* UNV */
    {U, 96, "CheckRadioButton", 8, u_checkradiobutton},                 /* UNV */
    {U, 97, "CheckDlgButton", 6, u_checkdlgbutton},                     /* UNV */
    {U, 98, "IsDlgButtonChecked", 4, u_isdlgbuttonchecked},             /* UNV */
    {U, 101, "SendDlgItemMessage", 12, u_senddlgitemmessage},           /* UNV */
    {U, 103, "MapDialogRect", 6, u_mapdialogrect},                      /* UNV */
    {U, 218, "DialogBoxIndirect", 10, u_dialogboxindirect},             /* UNV */
    {U, 219, "CreateDialogIndirect", 12, u_createdialogindirect},       /* UNV */
    {U, 227, "GetNextDlgGroupItem", 6, u_nextdlggroupitem},            /* UNV */
    {U, 228, "GetNextDlgTabItem", 6, u_nextdlgtabitem},                /* UNV */
    {U, 239, "DialogBoxParam", 16, u_dialogboxparam},                   /* UNV */
    {U, 241, "CreateDialogParam", 16, u_createdialogparam},             /* UNV */
    {U, 277, "GetDlgCtrlID", 2, u_getdlgctrlid},                        /* UNV */
    {U, 308, "DefDlgProc", 10, u_defdlgproc},                           /* UNV */
};
#undef U
const size_t ui_dlg_ndefs = sizeof ui_dlg_defs / sizeof ui_dlg_defs[0];
