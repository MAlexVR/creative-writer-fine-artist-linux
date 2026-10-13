/* Menu tracking: popup menus drawn straight on the screen (the pixels under them are saved and put back),
 * a modal loop over the input messages, mouse and keyboard navigation, and the messages programs expect:
 * WM_INITMENU, WM_INITMENUPOPUP, WM_MENUSELECT, then WM_COMMAND (WM_SYSCOMMAND for the system menu).
 *
 * The loop takes keyboard, mouse and non-client mouse messages out of the queues and leaves everything else
 * (paint, timers, posted messages) waiting until the menu closes. Mouse capture goes to the owner window so the
 * events arrive whatever lies under the pointer. */
#include <string.h>

#include "ui_priv.h"

#define MAX_LEVELS 6
#define ROWH 18
#define SEPH 9
#define PAD_L 22
#define PAD_R 10

typedef struct {
    uint16_t menu;
    int sel;
    fb_rect rc;
    uint8_t *save;
} tlevel;

typedef struct {
    user_t *u;
    uint16_t hwnd, root;
    bool bar, sys;
    int bar_sel;
    tlevel lv[MAX_LEVELS];
    int nlv;
    bool done, chosen, quit;
    uint16_t cmd;
    bool mouse;
    int right_ok;
} tracker;

static uint32_t col(int idx) { return wm_sys_color(idx); }
static int text_w(user_t *u, const char *s) { return u->has_gfx ? u->gfx.text_width(u->gfx.ctx, s, (int)strlen(s), 0) : 8 * (int)strlen(s); }
static int text_h(user_t *u) { return u->has_gfx ? u->gfx.text_height(u->gfx.ctx, 0) : 16; }

static void fillr(user_t *u, uint16_t dc, int l, int t, int r, int b, uint32_t c) {
    fb_rect q;
    q.left = l; q.top = t; q.right = r; q.bottom = b;
    if (u->has_gfx && !fb_rect_empty(&q)) u->gfx.fill(u->gfx.ctx, dc, &q, c);
}

/* ---- messages to the owner ----------------------------------------------------------------- */

static bool alive(tracker *t) { return !t->u->abort && wm_is_window(&t->u->wm, t->hwnd); }

static void send_owner(tracker *t, uint16_t msg, uint16_t wp, uint32_t lp) {
    if (alive(t)) wm_send(&t->u->wm, t->hwnd, msg, wp, lp);
    if (!alive(t)) t->done = t->quit = true;
}

static uint16_t item_menu_flags(const tracker *t, const ui_item *it, bool sysmenu) {
    uint16_t f = (uint16_t)(it->flags & (MF_BITMAP | MF_CHECKED | MF_DISABLED | MF_GRAYED | MF_OWNERDRAW | MF_POPUP));
    if (sysmenu) f |= MF_SYSMENU;
    if (t->mouse) f |= MF_MOUSESELECT;
    return f;
}

static void notify_select(tracker *t, uint16_t menu, int idx, bool sysmenu) {
    ui_menu *m = ui_menu_get(t->u->ui, menu);
    const ui_item *it;
    uint16_t wp;
    if (!m || idx < 0 || idx >= m->n) return;
    it = &m->it[idx];
    wp = it->flags & MF_POPUP ? (uint16_t)idx : it->id;
    send_owner(t, WM_MENUSELECT, wp, (uint32_t)menu << 16 | item_menu_flags(t, it, sysmenu));
}

/* ---- popup geometry and drawing ----------------------------------------------------------------- */

static void layout(tracker *t, tlevel *l, int x, int y) {
    user_t *u = t->u;
    ui_menu *m = ui_menu_get(u->ui, l->menu);
    int i, yy = 2, maxl = 0, maxa = 0, w, h;
    if (!m) return;
    for (i = 0; i < m->n; i++) {
        char label[160], accel[64];
        int ul;
        ui_item *it = &m->it[i];
        ui_item_label(it, label, sizeof label, &ul, accel, sizeof accel);
        it->x = yy;
        it->w = it->flags & MF_SEPARATOR ? SEPH : ROWH;
        yy += it->w;
        if (!(it->flags & MF_SEPARATOR)) {
            if (text_w(u, label) > maxl) maxl = text_w(u, label);
            if (accel[0] && text_w(u, accel) > maxa) maxa = text_w(u, accel);
        }
    }
    w = PAD_L + maxl + (maxa ? 16 + maxa : 0) + PAD_R + 4;
    if (w < 40) w = 40;
    h = yy + 2;
    if (x + w > u->fb.w) x = u->fb.w - w;
    if (y + h > u->fb.h) y = u->fb.h - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    l->rc = (fb_rect){x, y, x + w, y + h};
}

static void save_bg(tracker *t, tlevel *l) {
    fb_t *fb = &t->u->fb;
    int y, w = l->rc.right - l->rc.left, h = l->rc.bottom - l->rc.top;
    l->save = NULL;
    if (fb->bpp != 8 || w <= 0 || h <= 0 || !(l->save = malloc((size_t)w * (size_t)h))) return;
    for (y = 0; y < h; y++) {
        int sy = l->rc.top + y;
        if (sy < 0 || sy >= fb->h) memset(l->save + (size_t)y * (size_t)w, 0, (size_t)w);
        else memcpy(l->save + (size_t)y * (size_t)w, fb->p8 + (size_t)sy * (size_t)fb->w + l->rc.left, (size_t)w);
    }
}

static void restore_bg(tracker *t, tlevel *l) {
    fb_t *fb = &t->u->fb;
    int y, w = l->rc.right - l->rc.left, h = l->rc.bottom - l->rc.top;
    if (l->save) {
        for (y = 0; y < h; y++) {
            int sy = l->rc.top + y;
            if (sy >= 0 && sy < fb->h) memcpy(fb->p8 + (size_t)sy * (size_t)fb->w + l->rc.left, l->save + (size_t)y * (size_t)w, (size_t)w);
        }
        fb->dirty++;
    }
    free(l->save);
    l->save = NULL;
}

static void draw_check(user_t *u, uint16_t dc, int x, int y, uint32_t c) {
    static const int px[7][2] = {{0, 3}, {1, 4}, {2, 5}, {3, 4}, {4, 3}, {5, 2}, {6, 1}};
    int i;
    for (i = 0; i < 7; i++) fillr(u, dc, x + px[i][0], y + px[i][1], x + px[i][0] + 1, y + px[i][1] + 2, c);
}

static void draw_level(tracker *t, tlevel *l) {
    user_t *u = t->u;
    ui_menu *m = ui_menu_get(u->ui, l->menu);
    uint16_t dc;
    int i, th = text_h(u);
    if (!u->has_gfx || !m || !(dc = wm_get_dc(&u->wm, 0))) return;
    fillr(u, dc, l->rc.left, l->rc.top, l->rc.right, l->rc.bottom, col(4));
    fillr(u, dc, l->rc.left, l->rc.top, l->rc.right, l->rc.top + 1, col(6));
    fillr(u, dc, l->rc.left, l->rc.bottom - 1, l->rc.right, l->rc.bottom, col(6));
    fillr(u, dc, l->rc.left, l->rc.top, l->rc.left + 1, l->rc.bottom, col(6));
    fillr(u, dc, l->rc.right - 1, l->rc.top, l->rc.right, l->rc.bottom, col(6));
    for (i = 0; i < m->n; i++) {
        const ui_item *it = &m->it[i];
        int top = l->rc.top + it->x, bot = top + it->w, left = l->rc.left + 2, right = l->rc.right - 2;
        bool sel = l->sel == i, dis = (it->flags & (MF_GRAYED | MF_DISABLED)) != 0;
        uint32_t fg;
        char label[160], accel[64];
        int ul;
        if (it->flags & MF_SEPARATOR) {
            fillr(u, dc, left, top + SEPH / 2, right, top + SEPH / 2 + 1, col(6));
            continue;
        }
        fg = dis ? col(sel ? 15 : 17) : sel ? col(14) : col(7);
        if (sel) fillr(u, dc, left, top, right, bot, col(13));
        if (it->flags & MF_CHECKED) draw_check(u, dc, left + 4, top + (ROWH - 9) / 2, fg);
        ui_item_label(it, label, sizeof label, &ul, accel, sizeof accel);
        ui_menu_text_at(u, dc, l->rc.left + PAD_L, top + (ROWH - th) / 2, label, ul, fg);
        if (accel[0]) u->gfx.text(u->gfx.ctx, dc, right - 6 - text_w(u, accel), top + (ROWH - th) / 2, accel, (int)strlen(accel), fg, 0xFFFFFFFFu, 0);
        if (it->flags & MF_POPUP) {
            int k, cy = top + ROWH / 2;
            for (k = 0; k < 4; k++) fillr(u, dc, right - 10 + k, cy - (3 - k), right - 9 + k, cy + (3 - k) + 1, fg);
        }
    }
    wm_release_dc(&u->wm, 0, dc);
}

static void open_level(tracker *t, uint16_t menu, int x, int y) {
    tlevel *l;
    if (t->nlv >= MAX_LEVELS) return;
    l = &t->lv[t->nlv];
    memset(l, 0, sizeof *l);
    l->menu = menu;
    l->sel = -1;
    layout(t, l, x, y);
    save_bg(t, l);
    t->nlv++;
    draw_level(t, l);
}

static void close_level(tracker *t) {
    if (t->nlv <= 0) return;
    restore_bg(t, &t->lv[--t->nlv]);
}

static void set_sel(tracker *t, int lvl, int idx) {
    tlevel *l = &t->lv[lvl];
    if (l->sel == idx) return;
    l->sel = idx;
    draw_level(t, l);
    notify_select(t, l->menu, idx, t->sys && lvl == 0);
}

/* ---- the bar ------------------------------------------------------------------------------------ */

static bool window_origin(tracker *t, int *x, int *y) {
    fb_rect wr;
    if (!wm_window_rect(&t->u->wm, t->hwnd, &wr)) return false;
    *x = wr.left;
    *y = wr.top;
    return true;
}

static void bar_redraw(tracker *t) {
    t->u->ui->trk_hwnd = t->done ? 0 : t->hwnd;
    t->u->ui->trk_item = t->bar_sel;
    if (t->bar) ui_ncpaint(t->u, t->hwnd);
}

static ui_menu *root_menu(tracker *t) { return ui_menu_get(t->u->ui, t->root); }

/* Opens the popup of item idx of the root (bar) or of the level above; selects its first item if asked. */
static int first_selectable(const ui_menu *m, int from, int dir);

static void open_sub(tracker *t, int lvl, int idx, bool select_first) {
    ui_menu *pm;
    const ui_item *it;
    int x, y, ox, oy;
    uint16_t sub;
    if (lvl < 0) {
        nc_geom g;
        pm = root_menu(t);
        if (!pm || idx < 0 || idx >= pm->n || !(pm->it[idx].flags & MF_POPUP) || !window_origin(t, &ox, &oy) ||
            !ui_nc_geom(t->u, t->hwnd, &g))
            return;
        ui_menubar_layout(t->u, t->root, &g);
        it = &pm->it[idx];
        x = ox + g.bar.left + it->x;
        y = oy + g.bar.bottom;
    } else {
        pm = ui_menu_get(t->u->ui, t->lv[lvl].menu);
        if (!pm || idx < 0 || idx >= pm->n || !(pm->it[idx].flags & MF_POPUP)) return;
        it = &pm->it[idx];
        x = t->lv[lvl].rc.right - 3;
        y = t->lv[lvl].rc.top + it->x - 2;
    }
    sub = it->id;
    if (!ui_menu_get(t->u->ui, sub)) return;
    send_owner(t, WM_INITMENUPOPUP, sub, (uint32_t)(t->sys && lvl < 0 ? 1 : 0) << 16 | (uint16_t)idx);
    if (t->done) return;
    open_level(t, sub, x, y);
    if (select_first) {
        ui_menu *m = ui_menu_get(t->u->ui, sub);
        int f = m ? first_selectable(m, -1, 1) : -1;
        if (f >= 0) set_sel(t, t->nlv - 1, f);
    }
}

static int first_selectable(const ui_menu *m, int from, int dir) {
    int i, n = m->n;
    for (i = 1; i <= n; i++) {
        int k = ((from + dir * i) % n + n) % n;
        if (!(m->it[k].flags & MF_SEPARATOR)) return k;
    }
    return -1;
}

static void select_bar(tracker *t, int idx, bool open_popup, bool select_first) {
    ui_menu *m = root_menu(t);
    while (t->nlv > 0) close_level(t);
    t->bar_sel = idx;
    bar_redraw(t);
    if (!m || idx < 0 || idx >= m->n) return;
    notify_select(t, t->root, idx, false);
    if (open_popup && (m->it[idx].flags & MF_POPUP)) open_sub(t, -1, idx, select_first);
}

static void choose(tracker *t, uint16_t menu, int idx) {
    ui_menu *m = ui_menu_get(t->u->ui, menu);
    if (!m || idx < 0 || idx >= m->n) return;
    if (m->it[idx].flags & (MF_GRAYED | MF_DISABLED | MF_SEPARATOR)) return;
    t->chosen = true;
    t->cmd = m->it[idx].id;
    t->done = true;
}

/* ---- input --------------------------------------------------------------------------------------- */

static int level_item_at(tracker *t, int lvl, int sx, int sy) {
    ui_menu *m = ui_menu_get(t->u->ui, t->lv[lvl].menu);
    int i;
    if (!m) return -1;
    for (i = 0; i < m->n; i++) {
        int top = t->lv[lvl].rc.top + m->it[i].x;
        if (sy >= top && sy < top + m->it[i].w && sx >= t->lv[lvl].rc.left + 2 && sx < t->lv[lvl].rc.right - 2)
            return (m->it[i].flags & MF_SEPARATOR) ? -1 : i;
    }
    return -1;
}

static void on_mouse(tracker *t, uint16_t msg, int sx, int sy) {
    bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN, up = msg == WM_LBUTTONUP || msg == WM_RBUTTONUP;
    int lvl;
    t->mouse = true;
    for (lvl = t->nlv - 1; lvl >= 0; lvl--)
        if (sx >= t->lv[lvl].rc.left && sx < t->lv[lvl].rc.right && sy >= t->lv[lvl].rc.top && sy < t->lv[lvl].rc.bottom) break;
    if (lvl >= 0) {
        int idx = level_item_at(t, lvl, sx, sy);
        ui_menu *m = ui_menu_get(t->u->ui, t->lv[lvl].menu);
        if (idx >= 0 && m) {
            if (idx != t->lv[lvl].sel) {
                while (t->nlv > lvl + 1) close_level(t);
                set_sel(t, lvl, idx);
                if (!t->done && (m->it[idx].flags & MF_POPUP) && !(m->it[idx].flags & (MF_GRAYED | MF_DISABLED)))
                    open_sub(t, lvl, idx, false);
            }
            if (up && !(m->it[idx].flags & MF_POPUP)) choose(t, t->lv[lvl].menu, idx);
        }
        return;
    }
    if (t->bar) {
        int wx, wy, ox, oy, idx;
        if (window_origin(t, &ox, &oy)) {
            wx = sx - ox;
            wy = sy - oy;
            idx = ui_menubar_item_at(t->u, t->hwnd, wx, wy);
            if (idx >= 0) {
                ui_menu *m = root_menu(t);
                if (down && idx == t->bar_sel && t->nlv > 0) {      /* a second click on the title closes the menu */
                    t->done = true;
                    return;
                }
                if (idx != t->bar_sel) select_bar(t, idx, true, false);
                else if (t->nlv == 0 && (m->it[idx].flags & MF_POPUP) && down) select_bar(t, idx, true, false);
                if (up && m && !(m->it[idx].flags & MF_POPUP) && idx == t->bar_sel) choose(t, t->root, idx);
                return;
            }
        }
    }
    if (down || up) t->done = true;                              /* a press or release elsewhere ends it */
}

static bool mnemonic_match(const ui_item *it, unsigned ch) {
    const char *s = it->text;
    for (; s && *s && *s != '\t'; s++)
        if (*s == '&') {
            if (s[1] == '&') {
                s++;
                continue;
            }
            return s[1] && (unsigned)((unsigned char)s[1] | 0x20) == (ch | 0x20);
        }
    return false;
}

static void activate_item(tracker *t, int lvl, int idx) {     /* Enter, or a mnemonic */
    ui_menu *m = lvl < 0 ? root_menu(t) : ui_menu_get(t->u->ui, t->lv[lvl].menu);
    if (!m || idx < 0 || idx >= m->n) return;
    if (m->it[idx].flags & MF_POPUP) {
        if (lvl < 0) {
            select_bar(t, idx, true, true);
        } else if (!(m->it[idx].flags & (MF_GRAYED | MF_DISABLED))) {
            open_sub(t, lvl, idx, true);
        }
    } else {
        choose(t, lvl < 0 ? t->root : t->lv[lvl].menu, idx);
    }
}

static void bar_step(tracker *t, int dir) {
    ui_menu *m = root_menu(t);
    bool had_popup = t->nlv > 0;
    int n;
    if (!m || m->n == 0) return;
    n = first_selectable(m, t->bar_sel, dir);
    if (n >= 0) select_bar(t, n, had_popup, had_popup);
}

static void on_key(tracker *t, unsigned vk) {
    int top = t->nlv - 1;
    ui_menu *m = top >= 0 ? ui_menu_get(t->u->ui, t->lv[top].menu) : root_menu(t);
    t->mouse = false;
    if (!m) return;
    switch (vk) {
    case VK_ESCAPE:
        if (top >= 1 || (top == 0 && t->bar)) close_level(t);
        else t->done = true;
        if (t->bar && t->nlv == 0 && !t->done) {
            bar_redraw(t);
        }
        return;
    case VK_MENU:
    case VK_F10:
        t->done = true;
        return;
    case VK_DOWN:
    case VK_UP:
        if (top < 0) {
            if (t->bar && t->bar_sel >= 0 && (m->it[t->bar_sel].flags & MF_POPUP)) {
                open_sub(t, -1, t->bar_sel, true);
                if (vk == VK_UP) {
                    ui_menu *pm = ui_menu_get(t->u->ui, t->lv[t->nlv - 1].menu);
                    int last = pm ? first_selectable(pm, 0, -1) : -1;
                    if (last >= 0) set_sel(t, t->nlv - 1, last);
                }
            }
        } else {
            int n = first_selectable(m, t->lv[top].sel, vk == VK_DOWN ? 1 : -1);
            if (n >= 0) set_sel(t, top, n);
        }
        return;
    case VK_RIGHT:
        if (top >= 0 && t->lv[top].sel >= 0 && (m->it[t->lv[top].sel].flags & MF_POPUP)) {
            activate_item(t, top, t->lv[top].sel);
        } else if (t->bar) {
            bar_step(t, 1);
        }
        return;
    case VK_LEFT:
        if (top >= 1) close_level(t);
        else if (t->bar) bar_step(t, -1);
        return;
    case VK_RETURN:
        if (top < 0) activate_item(t, -1, t->bar_sel);
        else if (t->lv[top].sel >= 0) activate_item(t, top, t->lv[top].sel);
        return;
    default:
        if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
            int i;
            for (i = 0; i < m->n; i++)
                if (mnemonic_match(&m->it[i], vk)) {
                    if (top >= 0) set_sel(t, top, i);
                    activate_item(t, top, i);
                    return;
                }
        }
        return;
    }
}

static void run(tracker *t) {
    static const uint16_t ranges[3][2] = {{WM_KEYDOWN, 0x0108}, {WM_MOUSEMOVE, 0x0209}, {0x00A0, 0x00A9}};
    wm_t *w = &t->u->wm;
    while (!t->done) {
        wm_msg m;
        bool got = false;
        int r;
        for (r = 0; r < 3 && !got; r++) got = wm_next_message(w, &m, 0, ranges[r][0], ranges[r][1], true);
        if (!got) {
            if (t->u->abort) {
                t->done = t->quit = true;
                break;
            }
            if (!user_idle(t->u, true)) {
                t->done = t->quit = true;
                t->u->abort = loader_request_stop(t->u->p);
            }
            continue;
        }
        if (m.message >= 0xA0 && m.message <= 0xA9) m.message = (uint16_t)(m.message - 0xA0 + WM_MOUSEMOVE);   /* NC twins */
        switch (m.message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: on_key(t, m.wparam); break;
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: on_mouse(t, m.message, m.pt_x, m.pt_y); break;
        default: break;
        }
        if (t->u->abort) t->done = t->quit = true;
    }
}

static void finish(tracker *t, uint16_t old_capture) {
    user_t *u = t->u;
    while (t->nlv > 0) close_level(t);
    t->done = true;
    if (t->bar) {
        t->bar_sel = -1;
        u->ui->trk_hwnd = 0;
        if (wm_is_window(&u->wm, t->hwnd)) ui_ncpaint(u, t->hwnd);
    }
    if (old_capture && wm_is_window(&u->wm, old_capture)) wm_set_capture(&u->wm, old_capture);
    else wm_release_capture(&u->wm);
    if (!t->quit && wm_is_window(&u->wm, t->hwnd)) {
        wm_send(&u->wm, t->hwnd, WM_MENUSELECT, 0, 0xFFFFu);
        if (t->chosen && wm_is_window(&u->wm, t->hwnd))
            wm_post(&u->wm, t->hwnd, t->sys ? WM_SYSCOMMAND : WM_COMMAND, t->cmd, 0);
    }
}

/* ---- entry points ----------------------------------------------------------------------------------- */

static void begin(tracker *t, user_t *u, uint16_t hwnd, uint16_t root) {
    memset(t, 0, sizeof *t);
    t->u = u;
    t->hwnd = hwnd;
    t->root = root;
    t->bar_sel = -1;
}

/* mode: 0 bar, 1 system menu. start is the bar item (mouse) or -1 (keyboard: the first item). */
void ui_track_window_menu(user_t *u, uint16_t hwnd, bool sys, int start, bool popup_open, bool mouse) {
    tracker t;
    wm_window *x = wm_get(&u->wm, hwnd);
    uint16_t old_capture, root;
    nc_geom g;
    if (!x || u->ui->trk_hwnd) return;
    if (sys) {
        root = ui_sysmenu_get(u->ui, hwnd, false);
        if (!root || !ui_nc_geom(u, hwnd, &g)) return;
    } else {
        root = x->id;
        if (!ui_menu_get(u->ui, root) || !ui_nc_geom(u, hwnd, &g) || !g.has_bar) return;
    }
    begin(&t, u, hwnd, root);
    t.bar = !sys;
    t.sys = sys;
    t.mouse = mouse;
    old_capture = wm_set_capture(&u->wm, hwnd);
    send_owner(&t, WM_INITMENU, root, 0);
    if (!t.done) {
        if (sys) {
            fb_rect wr;
            wm_window_rect(&u->wm, hwnd, &wr);
            send_owner(&t, WM_INITMENUPOPUP, root, 1u << 16);
            if (!t.done) open_level(&t, root, wr.left + g.sys.left, wr.top + g.sys.bottom);
        } else {
            ui_menu *m = ui_menu_get(u->ui, root);
            int idx = start >= 0 ? start : (m && m->n ? first_selectable(m, -1, 1) : -1);
            select_bar(&t, idx, popup_open, false);
        }
    }
    if (!t.done) run(&t);
    finish(&t, old_capture);
}

bool ui_track_popup(user_t *u, uint16_t hmenu, unsigned flags, int x, int y, uint16_t hwnd) {
    tracker t;
    uint16_t old_capture;
    tlevel probe;
    if (!ui_menu_get(u->ui, hmenu) || !wm_is_window(&u->wm, hwnd) || u->ui->trk_hwnd) return false;
    begin(&t, u, hwnd, hmenu);
    t.right_ok = (flags & 2) != 0;
    old_capture = wm_set_capture(&u->wm, hwnd);
    send_owner(&t, WM_INITMENU, hmenu, 0);
    if (!t.done) send_owner(&t, WM_INITMENUPOPUP, hmenu, 0);
    if (!t.done) {
        memset(&probe, 0, sizeof probe);
        probe.menu = hmenu;
        layout(&t, &probe, x, y);
        if (flags & 4) x -= (probe.rc.right - probe.rc.left) / 2;      /* TPM_CENTERALIGN */
        else if (flags & 8) x -= probe.rc.right - probe.rc.left;       /* TPM_RIGHTALIGN */
        open_level(&t, hmenu, x, y);
        run(&t);
    }
    finish(&t, old_capture);
    return true;
}

/* Alt+character: the bar item whose mnemonic is ch; -1 when none. */
int ui_menubar_mnemonic(user_t *u, uint16_t hwnd, unsigned ch) {
    wm_window *x = wm_get(&u->wm, hwnd);
    ui_menu *m = x ? ui_menu_get(u->ui, x->id) : NULL;
    int i;
    for (i = 0; m && i < m->n; i++)
        if (mnemonic_match(&m->it[i], ch)) return i;
    return -1;
}

/* ---- API ----------------------------------------------------------------------------------------------- */

static cpu_status u_trackpopupmenu(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), flags = hle_u16(&a), hwnd, rsel, roff;
    int x, y;
    x = (int16_t)hle_u16(&a);
    y = (int16_t)hle_u16(&a);
    hle_u16(&a);
    hwnd = hle_u16(&a);
    hle_far(&a, &rsel, &roff);
    UCHECK;
    hle_ret16(c, ui_track_popup(u, h, flags, x, y, hwnd));
    return u->abort;
}

static cpu_status u_getmenucheckmarkdimensions(hle_call *c) {
    hle_ret32(c, 9u << 16 | 9u);
    return CPU_OK;
}

static cpu_status u_setmenuitembitmaps(hle_call *c) {
    UBEGIN;
    hle_u16(&a); hle_u16(&a); hle_u16(&a); hle_u16(&a); hle_u16(&a);
    UCHECK;
    hle_ret16(c, 1);       /* check mark bitmaps are accepted and not used: the stock check mark is drawn */
    return CPU_OK;
}

const hle_def ui_track_defs[] = {
    {USER_MODULE, 416, "TrackPopupMenu", 16, u_trackpopupmenu},                     /* UNV */
    {USER_MODULE, 417, "GetMenuCheckMarkDimensions", 0, u_getmenucheckmarkdimensions}, /* UNV */
    {USER_MODULE, 418, "SetMenuItemBitmaps", 10, u_setmenuitembitmaps},             /* UNV */
};
const size_t ui_track_ndefs = sizeof ui_track_defs / sizeof ui_track_defs[0];
