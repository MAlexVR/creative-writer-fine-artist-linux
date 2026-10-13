/* Menu objects: the handle table, items, resources, window menus, the system menu and the
 * USER menu API. Drawing and tracking live in ui_track.c. */
#include <stdlib.h>
#include <string.h>

#include "ui_priv.h"

/* ---- the table ------------------------------------------------------------------------ */

ui_menu *ui_menu_get(ui_t *ui, uint16_t h) {
    unsigned i = (unsigned)h - UI_MENU_BASE;
    return h >= UI_MENU_BASE && i < UI_MAX_MENUS && ui->menu[i].used ? &ui->menu[i] : NULL;
}

uint16_t ui_menu_new(ui_t *ui) {
    unsigned i;
    for (i = 0; i < UI_MAX_MENUS; i++)
        if (!ui->menu[i].used) {
            memset(&ui->menu[i], 0, sizeof ui->menu[i]);
            ui->menu[i].used = true;
            return (uint16_t)(UI_MENU_BASE + i);
        }
    return 0;
}

void ui_menu_destroy(ui_t *ui, uint16_t h) {
    ui_menu *m = ui_menu_get(ui, h);
    int i;
    if (!m) return;
    m->used = false;                       /* first: a menu that contains itself must not loop */
    for (i = 0; i < m->n; i++) {
        if (m->it[i].flags & MF_POPUP) ui_menu_destroy(ui, m->it[i].id);
        free(m->it[i].text);
    }
    free(m->it);
    m->it = NULL;
    m->n = m->cap = 0;
}

/* Inserts at idx (clamped; <0 appends). text is copied. */
static ui_item *item_insert(ui_menu *m, int idx, uint16_t flags, uint16_t id, const char *text, uint32_t data) {
    ui_item *it;
    char *t = NULL;
    if (idx < 0 || idx > m->n) idx = m->n;
    if (text && !(t = ui_strdup(text))) return NULL;
    if (m->n == m->cap) {
        int cap = m->cap ? m->cap * 2 : 8;
        ui_item *v = realloc(m->it, (size_t)cap * sizeof *v);
        if (!v) {
            free(t);
            return NULL;
        }
        m->it = v;
        m->cap = cap;
    }
    memmove(&m->it[idx + 1], &m->it[idx], (size_t)(m->n - idx) * sizeof *m->it);
    m->n++;
    it = &m->it[idx];
    memset(it, 0, sizeof *it);
    it->flags = flags;
    it->id = id;
    it->text = t;
    it->data = data;
    return it;
}

static void item_remove(ui_menu *m, int idx) {
    free(m->it[idx].text);
    memmove(&m->it[idx], &m->it[idx + 1], (size_t)(m->n - idx - 1) * sizeof *m->it);
    m->n--;
}

/* Finds an item. By position: item is the index in h. By command: depth first through the popups. */
bool ui_menu_find(ui_t *ui, uint16_t h, unsigned item, unsigned flags, uint16_t *mh, int *idx) {
    ui_menu *m = ui_menu_get(ui, h);
    int i;
    if (!m) return false;
    if (flags & MF_BYPOSITION) {
        if (item >= (unsigned)m->n) return false;
        *mh = h;
        *idx = (int)item;
        return true;
    }
    for (i = 0; i < m->n; i++) {
        if (m->it[i].flags & MF_POPUP) {
            if (ui_menu_find(ui, m->it[i].id, item, flags, mh, idx)) return true;
        } else if (!(m->it[i].flags & MF_SEPARATOR) && m->it[i].id == item) {
            *mh = h;
            *idx = i;
            return true;
        }
    }
    for (i = 0; i < m->n; i++)         /* a popup is also named by its handle */
        if ((m->it[i].flags & MF_POPUP) && m->it[i].id == item) {
            *mh = h;
            *idx = i;
            return true;
        }
    return false;
}

/* ---- menu templates ---------------------------------------------------------------------- */

typedef struct {
    ui_t *ui;
    uint16_t stack[16];
    bool ok;
} loader;

static bool load_cb(void *ctx, int depth, uint16_t flags, uint16_t id, const char *text, bool popup) {
    loader *l = ctx;
    ui_menu *cur;
    uint16_t f = (uint16_t)(flags & (MFR_GRAYED | MFR_INACTIVE | MFR_BITMAP | MFR_CHECKED | MFR_POPUP | MFR_MENUBARBREAK |
                                    MFR_MENUBREAK | MFR_HELP));
    if (depth > 15 || !(cur = ui_menu_get(l->ui, l->stack[depth]))) return l->ok = false;
    if (popup) {
        uint16_t sub = ui_menu_new(l->ui);
        if (!sub || !item_insert(cur, -1, f, sub, text, 0)) return l->ok = false;
        l->stack[depth + 1] = sub;
    } else if (flags == 0 && id == 0 && !*text) {
        if (!item_insert(cur, -1, MF_SEPARATOR, 0, NULL, 0)) return l->ok = false;
    } else if (!item_insert(cur, -1, f, id, text, 0)) {
        return l->ok = false;
    }
    return true;
}

uint16_t ui_menu_load(ui_t *ui, const uint8_t *blob, size_t len) {
    loader l;
    uint16_t root = ui_menu_new(ui);
    if (!root) return 0;
    memset(&l, 0, sizeof l);
    l.ui = ui;
    l.ok = true;
    l.stack[0] = root;
    if (res_menu_walk(blob, len, load_cb, &l) != RES_OK || !l.ok) {
        ui_menu_destroy(ui, root);
        return 0;
    }
    return root;
}

uint16_t ui_class_menu(user_t *u, uint16_t hinst, uint32_t menu_name) {
    char name[64];
    const uint8_t *d;
    size_t dl;
    uint16_t sel = (uint16_t)(menu_name >> 16), off = (uint16_t)menu_name;
    bool by_name = sel != 0;
    if (hinst != u->p->auto_ds || !menu_name) return 0;
    if (by_name) {
        const char *p = user_guest_ptr(u, menu_name, 1);
        size_t n = 0;
        if (!p) return 0;
        while (n < sizeof name - 1 && p[n]) {
            name[n] = p[n];
            n++;
        }
        name[n] = 0;
    }
    if (res_find(u->p->ne, RT_MENU, by_name, off, by_name ? name : NULL, &d, &dl) != RES_OK) return 0;
    return ui_menu_load(u->ui, d, dl);
}

/* ---- the system menu --------------------------------------------------------------------- */

static uint16_t sysmenu_build(ui_t *ui) {
    static const struct { uint16_t id; const char *text; } e[] = {
        {SC_RESTORE, "&Restore"}, {SC_MOVE, "&Move"}, {SC_SIZE, "&Size"}, {SC_MINIMIZE, "Mi&nimize"},
        {SC_MAXIMIZE, "Ma&ximize"}, {0, NULL}, {SC_CLOSE, "&Close\tAlt+F4"}};
    uint16_t h = ui_menu_new(ui);
    ui_menu *m = ui_menu_get(ui, h);
    size_t i;
    if (!m) return 0;
    for (i = 0; i < sizeof e / sizeof e[0]; i++)
        if (!item_insert(m, -1, e[i].text ? 0 : MF_SEPARATOR, e[i].id, e[i].text, 0)) {
            ui_menu_destroy(ui, h);
            return 0;
        }
    return h;
}

/* The state of the stock entries follows the window; entries the program added or removed are left alone. */
void ui_sysmenu_refresh(ui_t *ui, uint16_t hwnd, uint16_t h) {
    wm_window *x = wm_get(&ui->u->wm, hwnd);
    ui_menu *m = ui_menu_get(ui, h);
    bool min, max;
    int i;
    if (!x || !m) return;
    min = (x->style & WS_MINIMIZE) != 0;
    max = (x->style & WS_MAXIMIZE) != 0;
    for (i = 0; i < m->n; i++) {
        bool off = false;
        switch (m->it[i].id) {
        case SC_RESTORE: off = !min && !max; break;
        case SC_MOVE: off = max || min; break;
        case SC_SIZE: off = !(x->style & WS_THICKFRAME) || max || min; break;
        case SC_MINIMIZE: off = !(x->style & WS_MINIMIZEBOX_) || min; break;
        case SC_MAXIMIZE: off = !(x->style & WS_MAXIMIZEBOX_) || max; break;
        default: continue;
        }
        if (m->it[i].flags & MF_SEPARATOR) continue;
        m->it[i].flags = (uint16_t)(off ? m->it[i].flags | MF_GRAYED : m->it[i].flags & ~(MF_GRAYED | MF_DISABLED));
    }
}

uint16_t ui_sysmenu_get(ui_t *ui, uint16_t hwnd, bool revert) {
    wm_window *x = wm_get(&ui->u->wm, hwnd);
    ui_win *uw;
    if (!x || !(x->style & WS_SYSMENU) || !(uw = ui_win_get(ui, hwnd, true))) return 0;
    if (revert) {
        ui_menu_destroy(ui, uw->sysmenu);
        uw->sysmenu = 0;
        return 0;
    }
    if (!ui_menu_get(ui, uw->sysmenu)) uw->sysmenu = sysmenu_build(ui);
    ui_sysmenu_refresh(ui, hwnd, uw->sysmenu);
    return uw->sysmenu;
}

void ui_window_destroyed(user_t *u, uint16_t hwnd) {
    wm_window *x = wm_get(&u->wm, hwnd);
    ui_win *uw;
    if (!x) return;
    if (!(x->style & WS_CHILD) && x->id) ui_menu_destroy(u->ui, x->id);
    if ((uw = x->ui_data)) {
        ui_menu_destroy(u->ui, uw->sysmenu);
        ui_ctl_free(uw->ctl);
        free(uw->dlg);
        free(uw);
        x->ui_data = NULL;
    }
}

/* ---- handlers ------------------------------------------------------------------------------ */

static const char *item_text_arg(hle_call *c, uint16_t flags, uint16_t sel, uint16_t off, char *buf, size_t cap,
                                 uint32_t *data, bool *ok) {
    *data = 0;
    *ok = true;
    if (flags & MF_SEPARATOR) return NULL;
    if (flags & MF_BITMAP) {
        *data = off;
        return NULL;
    }
    if (flags & MF_OWNERDRAW) {
        *data = (uint32_t)sel << 16 | off;
        return NULL;
    }
    if (!sel || !hle_read_cstr(c, sel, off, buf, cap)) {
        *ok = false;
        return NULL;
    }
    return buf;
}

static cpu_status u_createmenu(hle_call *c) {
    UBEGIN;
    hle_ret16(c, ui_menu_new(u->ui));
    return CPU_OK;
}

static cpu_status u_destroymenu(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a);
    UCHECK;
    hle_ret16(c, ui_menu_get(u->ui, h) != NULL);
    ui_menu_destroy(u->ui, h);
    return CPU_OK;
}

enum { M_APPEND, M_INSERT, M_MODIFY };

/* AppendMenu / InsertMenu / ModifyMenu share the item description. */
static cpu_status menu_add(hle_call *c, int mode) {
    UBEGIN;
    uint16_t h = hle_u16(&a), pos = 0, flags, raw, id, sel, off, mh = h;
    char text[256];
    const char *t;
    uint32_t data;
    bool ok;
    ui_menu *m;
    int idx = -1;
    if (mode != M_APPEND) pos = hle_u16(&a);
    flags = raw = hle_u16(&a);
    id = hle_u16(&a);
    hle_far(&a, &sel, &off);
    UCHECK;
    hle_ret16(c, 0);
    if (!(m = ui_menu_get(u->ui, h))) return CPU_OK;
    t = item_text_arg(c, flags, sel, off, text, sizeof text, &data, &ok);
    if (!ok || ((flags & MF_POPUP) && !ui_menu_get(u->ui, id))) return CPU_OK;
    flags &= (uint16_t)(MF_STATEMASK | MF_BITMAP | MF_POPUP | MF_OWNERDRAW);
    if (mode == M_APPEND) {
        hle_ret16(c, item_insert(m, -1, flags, id, t, data) != NULL);
        return CPU_OK;
    }
    if (mode == M_INSERT && (raw & MF_BYPOSITION) && pos == 0xFFFF) {      /* -1 appends */
        hle_ret16(c, item_insert(m, -1, flags, id, t, data) != NULL);
        return CPU_OK;
    }
    if (!ui_menu_find(u->ui, h, pos, raw, &mh, &idx)) return CPU_OK;
    m = ui_menu_get(u->ui, mh);
    if (mode == M_INSERT) {
        hle_ret16(c, item_insert(m, idx, flags, id, t, data) != NULL);
    } else {
        ui_item *it = &m->it[idx];
        char *nt = t ? ui_strdup(t) : NULL;
        if (t && !nt) return CPU_OK;
        free(it->text);
        it->text = nt;
        it->flags = flags;
        it->id = id;
        it->data = data;
        hle_ret16(c, 1);
    }
    return CPU_OK;
}

static cpu_status u_appendmenu(hle_call *c) { return menu_add(c, M_APPEND); }
static cpu_status u_insertmenu(hle_call *c) { return menu_add(c, M_INSERT); }
static cpu_status u_modifymenu(hle_call *c) { return menu_add(c, M_MODIFY); }

static cpu_status menu_remove(hle_call *c, bool destroy) {
    UBEGIN;
    uint16_t h = hle_u16(&a), item = hle_u16(&a), flags = hle_u16(&a), mh;
    int idx;
    ui_menu *m;
    UCHECK;
    if (!ui_menu_find(u->ui, h, item, flags, &mh, &idx)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    m = ui_menu_get(u->ui, mh);
    if (destroy && (m->it[idx].flags & MF_POPUP)) ui_menu_destroy(u->ui, m->it[idx].id);
    item_remove(m, idx);
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status u_removemenu(hle_call *c) { return menu_remove(c, false); }
static cpu_status u_deletemenu(hle_call *c) { return menu_remove(c, true); }

static cpu_status u_getmenuitemcount(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a);
    ui_menu *m;
    UCHECK;
    m = ui_menu_get(u->ui, h);
    hle_ret16(c, m ? (uint16_t)m->n : 0xFFFF);
    return CPU_OK;
}

static cpu_status u_getmenuitemid(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), pos = hle_u16(&a), r = 0xFFFF;
    ui_menu *m;
    UCHECK;
    if ((m = ui_menu_get(u->ui, h)) && pos < (unsigned)m->n && !(m->it[pos].flags & MF_POPUP))
        r = m->it[pos].flags & MF_SEPARATOR ? 0 : m->it[pos].id;
    hle_ret16(c, r);
    return CPU_OK;
}

static cpu_status u_getsubmenu(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), pos = hle_u16(&a), r = 0;
    ui_menu *m;
    UCHECK;
    if ((m = ui_menu_get(u->ui, h)) && pos < (unsigned)m->n && (m->it[pos].flags & MF_POPUP)) r = m->it[pos].id;
    hle_ret16(c, r);
    return CPU_OK;
}

static cpu_status u_getmenustate(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), item = hle_u16(&a), flags = hle_u16(&a), mh, r = 0xFFFF;
    int idx;
    UCHECK;
    if (ui_menu_find(u->ui, h, item, flags, &mh, &idx)) {
        const ui_item *it = &ui_menu_get(u->ui, mh)->it[idx];
        if (it->flags & MF_POPUP) {
            ui_menu *sub = ui_menu_get(u->ui, it->id);
            r = (uint16_t)((sub ? sub->n : 0) << 8 | (it->flags & 0xFF));
        } else {
            r = (uint16_t)(it->flags & (MF_STATEMASK | MF_BITMAP));
        }
    }
    hle_ret16(c, r);
    return CPU_OK;
}

static cpu_status u_getmenustring(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), item = hle_u16(&a), sel, off, flags, mh;
    int max, idx, n = 0;
    uint8_t *d;
    hle_far(&a, &sel, &off);
    max = (int16_t)hle_u16(&a);
    flags = hle_u16(&a);
    UCHECK;
    hle_ret16(c, 0);
    if (max <= 0 || !ui_menu_find(u->ui, h, item, flags, &mh, &idx)) return CPU_OK;
    {
        const ui_item *it = &ui_menu_get(u->ui, mh)->it[idx];
        const char *t = it->text ? it->text : "";
        n = (int)strlen(t);
        if (n > max - 1) n = max - 1;
        if (!(d = hle_ptr(c, sel, off, (uint32_t)n + 1))) return CPU_E_SEL;
        memcpy(d, t, (size_t)n);
        d[n] = 0;
    }
    hle_ret16(c, (uint16_t)n);
    return CPU_OK;
}

/* CheckMenuItem and EnableMenuItem return the previous state, 0xFFFF when there is no such item. */
static cpu_status menu_state_change(hle_call *c, bool check) {
    UBEGIN;
    uint16_t h = hle_u16(&a), item = hle_u16(&a), flags = hle_u16(&a), mh;
    int idx;
    UCHECK;
    if (!ui_menu_find(u->ui, h, item, flags, &mh, &idx)) {
        hle_ret16(c, 0xFFFF);
        return CPU_OK;
    }
    {
        ui_item *it = &ui_menu_get(u->ui, mh)->it[idx];
        uint16_t mask = check ? MF_CHECKED : (MF_GRAYED | MF_DISABLED);
        hle_ret16(c, (uint16_t)(it->flags & mask));
        it->flags = (uint16_t)((it->flags & ~mask) | (flags & mask));
    }
    return CPU_OK;
}

static cpu_status u_checkmenuitem(hle_call *c) { return menu_state_change(c, true); }
static cpu_status u_enablemenuitem(hle_call *c) { return menu_state_change(c, false); }

static cpu_status u_hilitemenuitem(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a), h = hle_u16(&a), item = hle_u16(&a), flags = hle_u16(&a), mh;
    int idx;
    UCHECK;
    (void)hwnd;
    if (!ui_menu_find(u->ui, h, item, flags, &mh, &idx)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    {
        ui_item *it = &ui_menu_get(u->ui, mh)->it[idx];
        it->flags = (uint16_t)(flags & MF_HILITE ? it->flags | MF_HILITE : it->flags & ~MF_HILITE);
    }
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status u_loadmenu(hle_call *c) {
    UBEGIN;
    uint16_t inst = hle_u16(&a), sel, off;
    char name[64];
    const uint8_t *d;
    size_t dl;
    hle_far(&a, &sel, &off);
    UCHECK;
    hle_ret16(c, 0);
    if (inst != u->p->auto_ds) return CPU_OK;
    if (sel) {
        if (!hle_read_cstr(c, sel, off, name, sizeof name)) return CPU_OK;
        if (name[0] == '#') {
            off = (uint16_t)atoi(name + 1);
            sel = 0;
        }
    }
    if (res_find(u->p->ne, RT_MENU, sel != 0, off, sel ? name : NULL, &d, &dl) != RES_OK) return CPU_OK;
    hle_ret16(c, ui_menu_load(u->ui, d, dl));
    return CPU_OK;
}

static cpu_status u_loadmenuindirect(hle_call *c) {
    UBEGIN;
    uint16_t sel, off;
    uint32_t avail;
    const uint8_t *d;
    hle_far(&a, &sel, &off);
    UCHECK;
    hle_ret16(c, 0);
    avail = sel ? mem_size(&u->p->mem, sel) : 0;
    if (avail <= off || !(d = hle_ptr(c, sel, off, avail - off))) return CPU_OK;
    hle_ret16(c, ui_menu_load(u->ui, d, avail - off));
    return CPU_OK;
}

/* ---- menus of windows ---------------------------------------------------------------------------- */

static cpu_status u_getmenu(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a);
    wm_window *x;
    UCHECK;
    x = wm_get(&u->wm, hwnd);
    hle_ret16(c, x && !(x->style & WS_CHILD) && ui_menu_get(u->ui, x->id) ? x->id : 0);
    return CPU_OK;
}

static cpu_status u_setmenu(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a), h = hle_u16(&a);
    wm_window *x;
    bool had;
    UCHECK;
    x = wm_get(&u->wm, hwnd);
    if (!x || (x->style & WS_CHILD) || (h && !ui_menu_get(u->ui, h))) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    had = x->id != 0;
    x->id = h;
    if (had != (h != 0)) wm_frame_changed(&u->wm, hwnd);
    else if (wm_is_visible(&u->wm, hwnd)) wm_send(&u->wm, hwnd, WM_NCPAINT, 1, 0);
    hle_ret16(c, 1);
    return u->abort;
}

static cpu_status u_drawmenubar(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a);
    UCHECK;
    if (wm_is_visible(&u->wm, hwnd)) wm_send(&u->wm, hwnd, WM_NCPAINT, 1, 0);
    hle_ret16(c, wm_is_window(&u->wm, hwnd));
    return u->abort;
}

static cpu_status u_getsystemmenu(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a), revert = hle_u16(&a);
    UCHECK;
    hle_ret16(c, ui_sysmenu_get(u->ui, hwnd, revert != 0));
    return CPU_OK;
}

#define U USER_MODULE
const hle_def ui_menu_defs[] = {
    {U, 150, "LoadMenu", 6, u_loadmenu},                       /* UNV */
    {U, 151, "CreateMenu", 0, u_createmenu},                   /* UNV */
    {U, 152, "DestroyMenu", 2, u_destroymenu},                 /* UNV */
    {U, 154, "CheckMenuItem", 6, u_checkmenuitem},             /* UNV */
    {U, 155, "EnableMenuItem", 6, u_enablemenuitem},
    {U, 156, "GetSystemMenu", 4, u_getsystemmenu},
    {U, 157, "GetMenu", 2, u_getmenu},                         /* ARTIST */
    {U, 158, "SetMenu", 4, u_setmenu},                         /* UNV */
    {U, 159, "GetSubMenu", 4, u_getsubmenu},                   /* ARTIST */
    {U, 160, "DrawMenuBar", 2, u_drawmenubar},                 /* UNV */
    {U, 161, "GetMenuString", 12, u_getmenustring},            /* UNV */
    {U, 162, "HiliteMenuItem", 8, u_hilitemenuitem},           /* UNV */
    {U, 220, "LoadMenuIndirect", 4, u_loadmenuindirect},       /* UNV */
    {U, 250, "GetMenuState", 6, u_getmenustate},               /* UNV */
    {U, 263, "GetMenuItemCount", 2, u_getmenuitemcount},       /* UNV */
    {U, 264, "GetMenuItemID", 4, u_getmenuitemid},             /* UNV */
    {U, 410, "InsertMenu", 12, u_insertmenu},                  /* UNV */
    {U, 411, "AppendMenu", 10, u_appendmenu},                  /* UNV */
    {U, 412, "RemoveMenu", 6, u_removemenu},                   /* UNV */
    {U, 413, "DeleteMenu", 6, u_deletemenu},
    {U, 414, "ModifyMenu", 12, u_modifymenu},
    {U, 415, "CreatePopupMenu", 0, u_createmenu},              /* UNV */
};
#undef U
const size_t ui_menu_ndefs = sizeof ui_menu_defs / sizeof ui_menu_defs[0];
