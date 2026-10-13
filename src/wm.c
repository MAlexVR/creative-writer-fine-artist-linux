/* Window manager core. See wm.h. */
#include "wm.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define HWND_BASE 0x0100u
#define SM_CXSCREEN_DEFAULT 640
#define CXFRAME 4
#define CXBORDER 1
#define CYCAPTION 20
#define CYMENU 19
#define CXVSCROLL 16
#define CYHSCROLL 16
#define WS_EX_DLGMODALFRAME 0x0001u

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    return *a == *b;
}

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

uint32_t wm_now(wm_t *w) { return w->ops.now_ms ? w->ops.now_ms(w->ops.ctx) : 0; }

/* ---- system colours ------------------------------------------------------ */

uint32_t wm_sys_color(int index) {
    static const uint32_t c[COLOR_NCOLORS] = {
        FB_RGB(192, 192, 192), FB_RGB(128, 128, 128), FB_RGB(0, 0, 128), FB_RGB(255, 255, 255),
        FB_RGB(255, 255, 255), FB_RGB(255, 255, 255), FB_RGB(0, 0, 0), FB_RGB(0, 0, 0),
        FB_RGB(0, 0, 0), FB_RGB(255, 255, 255), FB_RGB(192, 192, 192), FB_RGB(192, 192, 192),
        FB_RGB(128, 128, 128), FB_RGB(0, 0, 128), FB_RGB(255, 255, 255), FB_RGB(192, 192, 192),
        FB_RGB(128, 128, 128), FB_RGB(128, 128, 128), FB_RGB(0, 0, 0), FB_RGB(0, 0, 0),
        FB_RGB(255, 255, 255)};
    return index >= 0 && index < COLOR_NCOLORS ? c[index] : 0;
}

/* ---- classes ------------------------------------------------------------- */

static uint16_t add_class(wm_t *w, const wm_class_def *d) {
    unsigned i;
    wm_class *c = NULL;
    if (!d->name || strlen(d->name) >= sizeof c->name) return 0;
    for (i = 0; i < WM_MAX_CLASSES; i++) {
        if (w->classes[i].used && ieq(w->classes[i].name, d->name) &&
            w->classes[i].hinstance == d->hinstance)
            return 0;
        if (!w->classes[i].used && !c) c = &w->classes[i];
    }
    if (!c) return 0;
    memset(c, 0, sizeof *c);
    c->used = true;
    c->atom = (uint16_t)(0xC000u + (unsigned)(c - w->classes));
    strcpy(c->name, d->name);
    c->style = d->style;
    c->wndproc = d->wndproc;
    c->cls_extra = d->cls_extra < 0 ? 0 : d->cls_extra;
    c->wnd_extra = d->wnd_extra < 0 ? 0 : d->wnd_extra;
    c->hinstance = d->hinstance;
    c->hicon = d->hicon;
    c->hcursor = d->hcursor;
    c->hbrbackground = d->hbrbackground;
    c->menu_name = d->menu_name;
    c->builtin = d->builtin;
    if (c->cls_extra) {
        c->extra = calloc(1, (size_t)c->cls_extra);
        if (!c->extra) {
            c->used = false;
            return 0;
        }
    }
    return c->atom;
}

uint16_t wm_register_class(wm_t *w, const wm_class_def *d) { return add_class(w, d); }

wm_class *wm_class_by_atom(wm_t *w, uint16_t atom) {
    unsigned i = (unsigned)atom - 0xC000u;
    return atom >= 0xC000u && i < WM_MAX_CLASSES && w->classes[i].used ? &w->classes[i] : NULL;
}

wm_class *wm_find_class(wm_t *w, const char *name, uint16_t hinstance) {
    unsigned i;
    for (i = 0; i < WM_MAX_CLASSES; i++)
        if (w->classes[i].used && w->classes[i].hinstance == hinstance && ieq(w->classes[i].name, name))
            return &w->classes[i];
    for (i = 0; i < WM_MAX_CLASSES; i++)
        if (w->classes[i].used && (w->classes[i].builtin || (w->classes[i].style & CS_GLOBALCLASS)) &&
            ieq(w->classes[i].name, name))
            return &w->classes[i];
    return NULL;
}

bool wm_unregister_class(wm_t *w, const char *name, uint16_t hinstance) {
    unsigned i;
    wm_class *c = NULL;
    for (i = 0; i < WM_MAX_CLASSES; i++)
        if (w->classes[i].used && w->classes[i].hinstance == hinstance && ieq(w->classes[i].name, name))
            c = &w->classes[i];
    if (!c) return false;
    for (i = 0; i < WM_MAX_WINDOWS; i++)
        if (w->win[i] && w->win[i]->cls == c) return false;
    free(c->extra);
    memset(c, 0, sizeof *c);
    return true;
}

/* ---- window table -------------------------------------------------------- */

wm_window *wm_get(wm_t *w, uint16_t hwnd) {
    unsigned i = (unsigned)hwnd - HWND_BASE;
    return hwnd >= HWND_BASE && i < WM_MAX_WINDOWS ? w->win[i] : NULL;
}

bool wm_is_window(wm_t *w, uint16_t hwnd) { return wm_get(w, hwnd) != NULL; }

static uint16_t alloc_window(wm_t *w, wm_window **out) {
    unsigned n;
    for (n = 0; n < WM_MAX_WINDOWS; n++) {
        unsigned i = (w->next_slot + n) % WM_MAX_WINDOWS;
        if (!w->win[i]) {
            wm_window *x = calloc(1, sizeof *x);
            if (!x) return 0;
            x->hwnd = (uint16_t)(HWND_BASE + i);
            w->win[i] = x;
            w->next_slot = i + 1;
            *out = x;
            return x->hwnd;
        }
    }
    return 0;
}

/* ---- tree ----------------------------------------------------------------- */

static void unlink_win(wm_t *w, wm_window *x) {
    wm_window *p = wm_get(w, x->parent), *a = wm_get(w, x->prev), *b = wm_get(w, x->next);
    if (a) a->next = x->next;
    else if (p) p->first_child = x->next;
    if (b) b->prev = x->prev;
    x->prev = x->next = 0;
}

static void link_top(wm_t *w, wm_window *x, uint16_t parent) {
    wm_window *p = wm_get(w, parent), *f;
    x->parent = parent;
    x->prev = 0;
    x->next = p ? p->first_child : 0;
    if ((f = wm_get(w, x->next))) f->prev = x->hwnd;
    if (p) p->first_child = x->hwnd;
}

static void link_after(wm_t *w, wm_window *x, wm_window *after) {
    wm_window *n = wm_get(w, after->next);
    x->parent = after->parent;
    x->prev = after->hwnd;
    x->next = after->next;
    after->next = x->hwnd;
    if (n) n->prev = x->hwnd;
}

static void link_bottom(wm_t *w, wm_window *x, uint16_t parent) {
    wm_window *p = wm_get(w, parent), *c = p ? wm_get(w, p->first_child) : NULL;
    if (!c) {
        link_top(w, x, parent);
        return;
    }
    while (wm_get(w, c->next)) c = wm_get(w, c->next);
    link_after(w, x, c);
}

uint16_t wm_top_window(wm_t *w, uint16_t parent) {
    wm_window *p = wm_get(w, parent);
    return p ? p->first_child : 0;
}

bool wm_is_child(wm_t *w, uint16_t parent, uint16_t hwnd) {
    wm_window *x = wm_get(w, hwnd);
    while (x && x->parent) {
        if (x->parent == parent) return true;
        x = wm_get(w, x->parent);
    }
    return false;
}

uint16_t wm_get_parent(wm_t *w, uint16_t hwnd) {
    wm_window *x = wm_get(w, hwnd);
    if (!x || hwnd == w->desktop) return 0;
    if (x->style & WS_CHILD) return x->parent;
    return x->owner;
}

uint16_t wm_get_window(wm_t *w, uint16_t hwnd, unsigned cmd) {
    wm_window *x = wm_get(w, hwnd), *p;
    if (!x) return 0;
    switch (cmd) {
    case GW_HWNDFIRST:
        p = wm_get(w, x->parent);
        return p ? p->first_child : hwnd;
    case GW_HWNDLAST: {
        uint16_t h = hwnd;
        while ((x = wm_get(w, h)) && x->next) h = x->next;
        return h;
    }
    case GW_HWNDNEXT: return x->next;
    case GW_HWNDPREV: return x->prev;
    case GW_OWNER: return x->owner;
    case GW_CHILD: return x->first_child;
    default: return 0;
    }
}

static uint16_t toplevel_of(wm_t *w, uint16_t hwnd) {
    wm_window *x = wm_get(w, hwnd);
    while (x && x->parent != w->desktop && x->parent) x = wm_get(w, x->parent);
    return x ? x->hwnd : 0;
}

static bool within(wm_t *w, uint16_t root, uint16_t hwnd) {
    return hwnd && (hwnd == root || wm_is_child(w, root, hwnd));
}

bool wm_is_visible(wm_t *w, uint16_t hwnd) {
    wm_window *x = wm_get(w, hwnd);
    for (; x; x = wm_get(w, x->parent)) {
        if (!(x->style & WS_VISIBLE)) return false;
        if (!x->parent) break;
    }
    return x != NULL;
}

/* ---- geometry ------------------------------------------------------------- */

void wm_nc_insets(uint32_t style, uint32_t exstyle, bool menu, int *l, int *t, int *r, int *b) {
    int f = 0;
    bool dlg = (exstyle & WS_EX_DLGMODALFRAME) || ((style & WS_DLGFRAME) && !(style & WS_BORDER));
    if (dlg) {
        f = CXFRAME;
    } else {
        if (style & WS_THICKFRAME) f = CXFRAME;
        if (style & WS_BORDER) f += CXBORDER;
    }
    *l = *r = *b = *t = f;
    if ((style & WS_CAPTION) == WS_CAPTION) *t += CYCAPTION - CXBORDER;
    if (menu) *t += CYMENU + CXBORDER;
}

static void insets_of(const wm_window *x, int *l, int *t, int *r, int *b) {
    if (x->style & WS_MINIMIZE) {
        *l = *t = *r = *b = 0;
        return;
    }
    wm_nc_insets(x->style, x->exstyle, !(x->style & WS_CHILD) && x->id != 0, l, t, r, b);
    if (x->style & WS_VSCROLL) *r += CXVSCROLL;
    if (x->style & WS_HSCROLL) *b += CYHSCROLL;
}

/* Where the client area starts inside the window: the program's own WM_NCCALCSIZE answer when it gave one. */
static void cinsets_of(const wm_window *x, int *l, int *t, int *r, int *b) {
    if (x->has_client && !(x->style & WS_MINIMIZE)) {
        *l = x->ci_l; *t = x->ci_t; *r = x->ci_r; *b = x->ci_b;
        return;
    }
    insets_of(x, l, t, r, b);
}

static void calc_client(wm_t *w, wm_window *x) {
    fb_rect cl = x->rect;
    uint16_t h = x->hwnd;
    int l, t, r, b;
    x->has_client = false;
    if (!w->ops.nccalcsize || !w->ops.nccalcsize(w->ops.ctx, h, &cl) || !(x = wm_get(w, h))) return;
    insets_of(x, &l, &t, &r, &b);
    x->ci_l = cl.left - x->rect.left;
    x->ci_t = cl.top - x->rect.top;
    x->ci_r = x->rect.right - cl.right;
    x->ci_b = x->rect.bottom - cl.bottom;
    x->has_client = x->ci_l != l || x->ci_t != t || x->ci_r != r || x->ci_b != b;
}

static void client_origin_of(wm_t *w, const wm_window *x, int *ox, int *oy) {
    int l, t, r, b, px = 0, py = 0;
    if (x->hwnd == w->desktop || !x->parent) {
        *ox = *oy = 0;
        return;
    }
    {
        const wm_window *p = wm_get(w, x->parent);
        if (p) client_origin_of(w, p, &px, &py);
    }
    cinsets_of(x, &l, &t, &r, &b);
    *ox = px + x->rect.left + l;
    *oy = py + x->rect.top + t;
}

static void window_screen_rect(wm_t *w, const wm_window *x, fb_rect *out) {
    int px = 0, py = 0;
    const wm_window *p = wm_get(w, x->parent);
    if (p && x->hwnd != w->desktop) client_origin_of(w, p, &px, &py);
    out->left = px + x->rect.left;
    out->top = py + x->rect.top;
    out->right = px + x->rect.right;
    out->bottom = py + x->rect.bottom;
}

static void client_size_of(const wm_window *x, int *cx, int *cy) {
    int l, t, r, b;
    cinsets_of(x, &l, &t, &r, &b);
    *cx = x->rect.right - x->rect.left - l - r;
    *cy = x->rect.bottom - x->rect.top - t - b;
    if (*cx < 0) *cx = 0;
    if (*cy < 0) *cy = 0;
}

bool wm_insets(wm_t *w, uint16_t hwnd, int *l, int *t, int *r, int *b) {
    wm_window *x = wm_get(w, hwnd);
    if (!x) return false;
    insets_of(x, l, t, r, b);
    return true;
}

bool wm_window_rect(wm_t *w, uint16_t hwnd, fb_rect *screen) {
    wm_window *x = wm_get(w, hwnd);
    if (!x) return false;
    window_screen_rect(w, x, screen);
    return true;
}

bool wm_client_rect(wm_t *w, uint16_t hwnd, fb_rect *rc) {
    wm_window *x = wm_get(w, hwnd);
    if (!x) return false;
    rc->left = rc->top = 0;
    client_size_of(x, &rc->right, &rc->bottom);
    return true;
}

bool wm_client_origin(wm_t *w, uint16_t hwnd, int *px, int *py) {
    wm_window *x = wm_get(w, hwnd);
    if (!x) return false;
    client_origin_of(w, x, px, py);
    return true;
}

bool wm_client_to_screen(wm_t *w, uint16_t hwnd, int *px, int *py) {
    int ox, oy;
    if (!wm_client_origin(w, hwnd, &ox, &oy)) return false;
    *px += ox;
    *py += oy;
    return true;
}

bool wm_screen_to_client(wm_t *w, uint16_t hwnd, int *px, int *py) {
    int ox, oy;
    if (!wm_client_origin(w, hwnd, &ox, &oy)) return false;
    *px -= ox;
    *py -= oy;
    return true;
}

/* Screen rectangle of the client area, clipped to every ancestor's client area. */
static void visible_client(wm_t *w, const wm_window *x, fb_rect *out) {
    int ox, oy, cx, cy;
    fb_rect r;
    const wm_window *p;
    client_origin_of(w, x, &ox, &oy);
    client_size_of(x, &cx, &cy);
    r.left = ox; r.top = oy; r.right = ox + cx; r.bottom = oy + cy;
    for (p = wm_get(w, x->parent); p && p->hwnd != w->desktop; p = wm_get(w, p->parent)) {
        fb_rect pc;
        client_origin_of(w, p, &ox, &oy);
        client_size_of(p, &cx, &cy);
        pc.left = ox; pc.top = oy; pc.right = ox + cx; pc.bottom = oy + cy;
        fb_rect_intersect(&r, &r, &pc);
    }
    *out = r;
}

static uint16_t point_in(wm_t *w, wm_window *c, int x, int y) {
    uint16_t h = c->first_child;
    while (h) {
        wm_window *k = wm_get(w, h);
        fb_rect r;
        if (!k) break;
        if ((k->style & WS_VISIBLE) && !k->destroying) {
            window_screen_rect(w, k, &r);
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return point_in(w, k, x, y);
        }
        h = k->next;
    }
    return c->hwnd;
}

uint16_t wm_window_from_point(wm_t *w, int x, int y) {
    wm_window *d = wm_get(w, w->desktop);
    return d ? point_in(w, d, x, y) : 0;
}

/* ---- text ----------------------------------------------------------------- */

void wm_set_text(wm_t *w, uint16_t hwnd, const char *s) {
    wm_window *x = wm_get(w, hwnd);
    char *n;
    if (!x) return;
    n = dup_str(s ? s : "");
    if (!n) return;
    free(x->text);
    x->text = n;
}

const char *wm_get_text(wm_t *w, uint16_t hwnd) {
    wm_window *x = wm_get(w, hwnd);
    return x ? (x->text ? x->text : "") : NULL;
}

/* ---- queues and timers owned by a window ---------------------------------- */

static void queue_remove(wm_queue *q, uint16_t hwnd) {
    unsigned i, n = 0;
    wm_msg tmp[WM_QUEUE_SIZE];
    for (i = 0; i < q->count; i++) {
        wm_msg *m = &q->q[(q->head + i) % WM_QUEUE_SIZE];
        if (m->hwnd != hwnd) tmp[n++] = *m;
    }
    q->head = 0;
    q->count = n;
    memcpy(q->q, tmp, n * sizeof tmp[0]);
}

static void drop_window_state(wm_t *w, uint16_t hwnd) {
    unsigned i;
    queue_remove(&w->posted, hwnd);
    queue_remove(&w->input, hwnd);
    for (i = 0; i < WM_MAX_TIMERS; i++)
        if (w->timers[i].used && w->timers[i].hwnd == hwnd) w->timers[i].used = false;
}

/* ---- messages ------------------------------------------------------------- */

uint32_t wm_send(wm_t *w, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    wm_window *x = wm_get(w, hwnd);
    if (!x) return 0;
    if (x->wndproc && w->ops.call_wndproc) return w->ops.call_wndproc(w->ops.ctx, x->wndproc, hwnd, msg, wp, lp);
    if (w->ops.defproc) return w->ops.defproc(w->ops.ctx, hwnd, msg, wp, lp);
    return wm_defproc(w, hwnd, msg, wp, lp);
}

/* ---- device contexts ------------------------------------------------------- */

wm_dc *wm_dc_get(wm_t *w, uint16_t hdc) {
    unsigned i;
    for (i = 0; i < WM_MAX_DCS; i++)
        if (w->dcs[i].used && w->dcs[i].hdc == hdc) return &w->dcs[i];
    return NULL;
}

static uint16_t new_dc(wm_t *w, uint16_t hwnd, int ox, int oy, const fb_rect *clip) {
    unsigned i;
    for (i = 0; i < WM_MAX_DCS; i++) {
        if (!w->dcs[i].used) {
            uint16_t h;
            do {
                w->next_dc_h = (uint16_t)(w->next_dc_h < 0x0800 || w->next_dc_h >= 0x0BFF ? 0x0800 : w->next_dc_h + 1);
                h = w->next_dc_h;
            } while (wm_dc_get(w, h));
            w->dcs[i].used = true;
            w->dcs[i].serial = ++w->dc_serial;
            w->dcs[i].hdc = h;
            w->dcs[i].hwnd = hwnd;
            w->dcs[i].org_x = ox;
            w->dcs[i].org_y = oy;
            w->dcs[i].clip = *clip;
            return h;
        }
    }
    return 0;
}

/* The private DC of a CS_OWNDC window (it exists from the first GetDC/BeginPaint until the window is destroyed). */
static wm_dc *own_dc_of(wm_t *w, uint16_t hwnd) {
    unsigned i;
    for (i = 0; i < WM_MAX_DCS; i++)
        if (w->dcs[i].used && w->dcs[i].own && w->dcs[i].hwnd == hwnd) return &w->dcs[i];
    return NULL;
}

static bool has_own_dc(const wm_window *x) { return x->cls && (x->cls->style & CS_OWNDC); }

/* The DC of a window with CS_OWNDC, whose origin and clip are set (clip: NULL = the visible client area). */
static uint16_t own_dc_for(wm_t *w, wm_window *x, const fb_rect *clip_override) {
    fb_rect clip;
    int ox, oy;
    wm_dc *dc = own_dc_of(w, x->hwnd);
    uint16_t h;
    visible_client(w, x, &clip);
    client_origin_of(w, x, &ox, &oy);
    if (clip_override) clip = *clip_override;
    if (dc) {
        dc->org_x = ox;
        dc->org_y = oy;
        dc->clip = clip;
        return dc->hdc;
    }
    h = new_dc(w, x->hwnd, ox, oy, &clip);
    if (h) wm_dc_get(w, h)->own = true;
    return h;
}

uint16_t wm_get_dc(wm_t *w, uint16_t hwnd) {
    wm_window *x;
    fb_rect clip;
    int ox, oy;
    if (hwnd == 0) {
        clip.left = clip.top = 0;
        clip.right = w->fb->w;
        clip.bottom = w->fb->h;
        return new_dc(w, 0, 0, 0, &clip);
    }
    x = wm_get(w, hwnd);
    if (!x) return 0;
    if (has_own_dc(x)) return own_dc_for(w, x, NULL);
    visible_client(w, x, &clip);
    client_origin_of(w, x, &ox, &oy);
    return new_dc(w, hwnd, ox, oy, &clip);
}

uint16_t wm_get_window_dc(wm_t *w, uint16_t hwnd) {
    wm_window *x = wm_get(w, hwnd);
    fb_rect r, clip, vis;
    const wm_window *p;
    if (!x) return 0;
    window_screen_rect(w, x, &r);
    clip = r;
    if (x->has_client && x->ci_l <= 0 && x->ci_t <= 0 && x->ci_r <= 0 && x->ci_b <= 0)
        memset(&clip, 0, sizeof clip);   /* the client area covers the whole frame */
    p = wm_get(w, x->parent);
    if (p && p->hwnd != w->desktop && hwnd != w->desktop) {
        visible_client(w, p, &vis);
        if (!fb_rect_intersect(&clip, &clip, &vis)) memset(&clip, 0, sizeof clip);
    }
    return new_dc(w, hwnd, r.left, r.top, &clip);
}

bool wm_release_dc(wm_t *w, uint16_t hwnd, uint16_t hdc) {
    wm_dc *dc = wm_dc_get(w, hdc);
    (void)hwnd;
    if (!dc) return false;
    if (dc->own) return true;                 /* a private DC stays until its window is destroyed */
    dc->used = false;
    return true;
}

void wm_dc_fill(wm_t *w, uint16_t hdc, const fb_rect *rc, uint32_t colorref) {
    wm_dc *dc = wm_dc_get(w, hdc);
    fb_rect s, c;
    if (!dc) return;
    s.left = rc->left + dc->org_x;
    s.right = rc->right + dc->org_x;
    s.top = rc->top + dc->org_y;
    s.bottom = rc->bottom + dc->org_y;
    if (fb_rect_intersect(&c, &s, &dc->clip)) fb_fill_rect(w->fb, &c, colorref);
}

/* ---- painting ---------------------------------------------------------------- */

void wm_invalidate(wm_t *w, uint16_t hwnd, const fb_rect *rc, bool erase) {
    wm_window *x = wm_get(w, hwnd);
    fb_rect cl, r;
    if (!x || hwnd == w->desktop) return;
    wm_client_rect(w, hwnd, &cl);
    if (rc) {
        if (!fb_rect_intersect(&r, rc, &cl)) return;
    } else {
        r = cl;
        if (fb_rect_empty(&r)) return;
    }
    if (x->has_update) fb_rect_union(&x->update, &x->update, &r);
    else x->update = r;
    x->has_update = true;
    if (erase) x->erase = true;
}

void wm_validate(wm_t *w, uint16_t hwnd, const fb_rect *rc) {
    wm_window *x = wm_get(w, hwnd);
    fb_rect *u;
    if (!x || !x->has_update) return;
    u = &x->update;
    if (!rc || (rc->left <= u->left && rc->top <= u->top && rc->right >= u->right && rc->bottom >= u->bottom)) {
        x->has_update = x->erase = false;
        return;
    }
    if (rc->top <= u->top && rc->bottom >= u->bottom) {   /* full-height strip */
        if (rc->left <= u->left && rc->right > u->left) u->left = rc->right < u->right ? rc->right : u->right;
        else if (rc->right >= u->right && rc->left < u->right) u->right = rc->left > u->left ? rc->left : u->left;
    } else if (rc->left <= u->left && rc->right >= u->right) { /* full-width strip */
        if (rc->top <= u->top && rc->bottom > u->top) u->top = rc->bottom < u->bottom ? rc->bottom : u->bottom;
        else if (rc->bottom >= u->bottom && rc->top < u->bottom) u->bottom = rc->top > u->top ? rc->top : u->top;
    }
    if (fb_rect_empty(u)) x->has_update = x->erase = false;
}

bool wm_get_update_rect(wm_t *w, uint16_t hwnd, fb_rect *rc, bool erase) {
    wm_window *x = wm_get(w, hwnd);
    (void)erase;
    if (!x || !x->has_update) {
        if (rc) memset(rc, 0, sizeof *rc);
        return false;
    }
    if (rc) *rc = x->update;
    return true;
}

uint16_t wm_begin_paint(wm_t *w, uint16_t hwnd, wm_paint *ps) {
    wm_window *x = wm_get(w, hwnd);
    fb_rect vis, upd, clip;
    int ox, oy;
    uint16_t hdc;
    memset(ps, 0, sizeof *ps);
    if (!x) return 0;
    if (x->has_update) upd = x->update;
    else memset(&upd, 0, sizeof upd);
    ps->rc = upd;
    ps->erase = x->erase;
    visible_client(w, x, &vis);
    client_origin_of(w, x, &ox, &oy);
    clip.left = upd.left + ox; clip.right = upd.right + ox;
    clip.top = upd.top + oy; clip.bottom = upd.bottom + oy;
    if (!fb_rect_intersect(&clip, &clip, &vis)) memset(&clip, 0, sizeof clip);
    hdc = has_own_dc(x) ? own_dc_for(w, x, &clip) : new_dc(w, hwnd, ox, oy, &clip);
    ps->hdc = hdc;
    x->has_update = false;
    x->erase = false;
    if (hdc && ps->erase) {
        uint32_t r = wm_send(w, hwnd, WM_ERASEBKGND, hdc, 0);
        ps->erase = (r & 0xFFFF) == 0;
    }
    return hdc;
}

void wm_end_paint(wm_t *w, uint16_t hwnd, const wm_paint *ps) {
    wm_window *x = wm_get(w, hwnd);
    if (x && has_own_dc(x) && wm_dc_get(w, ps->hdc)) own_dc_for(w, x, NULL);   /* the paint clip ends with the paint */
    wm_release_dc(w, hwnd, ps->hdc);
}

void wm_update_window(wm_t *w, uint16_t hwnd) {
    wm_window *x = wm_get(w, hwnd);
    if (!x || hwnd == w->desktop) return;
    if (x->nc_dirty) {
        x->nc_dirty = false;
        wm_send(w, hwnd, WM_NCPAINT, 1, 0);
    }
    if ((x = wm_get(w, hwnd)) && x->has_update) wm_send(w, hwnd, WM_PAINT, 0, 0);
    /* RDW_UPDATENOW | RDW_ALLCHILDREN: the children with an update region of their own are painted too */
    for (x = wm_get(w, hwnd), x = x ? wm_get(w, x->first_child) : NULL; x;) {
        uint16_t child = x->hwnd, next = x->next;
        if (wm_is_visible(w, child)) wm_update_window(w, child);
        x = wm_get(w, next);
    }
}

/* Window area (parent client coordinates) became visible to what lies below. */
static void expose(wm_t *w, uint16_t parent, const fb_rect *area) {
    wm_window *p = wm_get(w, parent);
    uint16_t h;
    if (!p) return;
    if (parent == w->desktop) {
        fb_fill_rect(w->fb, area, wm_sys_color(COLOR_BACKGROUND));
    } else {
        wm_window *pp = p;
        fb_rect pc;
        int l, t, r, b;
        cinsets_of(pp, &l, &t, &r, &b);
        pc.left = area->left - l; pc.right = area->right - l;
        pc.top = area->top - t; pc.bottom = area->bottom - t;
        wm_invalidate(w, parent, &pc, true);
    }
    for (h = p->first_child; h;) {
        wm_window *c = wm_get(w, h);
        fb_rect hit, rel;
        int l, t, r, b;
        if (!c) break;
        if ((c->style & WS_VISIBLE) && fb_rect_intersect(&hit, area, &c->rect)) {
            cinsets_of(c, &l, &t, &r, &b);
            rel.left = hit.left - c->rect.left - l; rel.right = hit.right - c->rect.left - l;
            rel.top = hit.top - c->rect.top - t; rel.bottom = hit.bottom - c->rect.top - t;
            wm_invalidate(w, c->hwnd, &rel, true);
            c->nc_dirty = true;
        }
        h = c->next;
    }
}

static void paint_all(wm_t *w, wm_window *x) {
    x->nc_dirty = true;
    wm_invalidate(w, x->hwnd, NULL, true);
}

/* ---- default window procedure ------------------------------------------------ */

static void draw_frame(wm_t *w, wm_window *x) {
    fb_rect r, in;
    int l, t, rr, b;
    uint16_t top;
    if (!(x->style & WS_VISIBLE) || x->hwnd == w->desktop) return;
    window_screen_rect(w, x, &r);
    insets_of(x, &l, &t, &rr, &b);
    if (!l && !t && !rr && !b) return;
    fb_fill_rect(w->fb, &r, wm_sys_color(COLOR_WINDOWFRAME));
    in.left = r.left + (l > 1 ? 1 : 0); in.right = r.right - (rr > 1 ? 1 : 0);
    in.top = r.top + (t > 1 && l ? 1 : 0); in.bottom = r.bottom - (b > 1 ? 1 : 0);
    if (l > 1 || t > 1) fb_fill_rect(w->fb, &in, wm_sys_color(COLOR_BTNFACE));
    top = toplevel_of(w, x->hwnd);
    if ((x->style & WS_CAPTION) == WS_CAPTION) {
        fb_rect cap;
        cap.left = r.left + l; cap.right = r.right - rr;
        cap.top = r.top + (l ? l : 0); cap.bottom = cap.top + CYCAPTION - CXBORDER;
        fb_fill_rect(w->fb, &cap, top == w->active ? wm_sys_color(COLOR_ACTIVECAPTION) : wm_sys_color(3));
    }
}

static int hit_test(wm_t *w, wm_window *x, int sx, int sy) {
    fb_rect r, c;
    int ox, oy, cx, cy, l, t, rr, b;
    window_screen_rect(w, x, &r);
    if (sx < r.left || sx >= r.right || sy < r.top || sy >= r.bottom) return HTNOWHERE;
    client_origin_of(w, x, &ox, &oy);
    client_size_of(x, &cx, &cy);
    c.left = ox; c.top = oy; c.right = ox + cx; c.bottom = oy + cy;
    if (sx >= c.left && sx < c.right && sy >= c.top && sy < c.bottom) return HTCLIENT;
    insets_of(x, &l, &t, &rr, &b);
    if ((x->style & WS_CAPTION) == WS_CAPTION && sy >= r.top + l && sy < r.top + l + CYCAPTION - CXBORDER &&
        sx >= r.left + l && sx < r.right - rr)
        return HTCAPTION;
    return HTBORDER;
}

uint32_t wm_defproc(wm_t *w, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    wm_window *x = wm_get(w, hwnd);
    if (!x) return 0;
    switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_PAINT: {
        wm_paint ps;
        wm_begin_paint(w, hwnd, &ps);
        wm_end_paint(w, hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        wm_dc *dc = wm_dc_get(w, wp);
        unsigned b = x->cls ? x->cls->hbrbackground : 0;
        if (!dc || b == 0 || b > COLOR_NCOLORS) return 0;   /* real brushes are GDI objects */
        fb_fill_rect(w->fb, &dc->clip, wm_sys_color((int)b - 1));
        return 1;
    }
    case WM_NCPAINT:
        draw_frame(w, x);
        return 0;
    case WM_CLOSE:
        wm_destroy_window(w, hwnd);
        return 0;
    case WM_NCHITTEST: return (uint32_t)(uint16_t)(int16_t)hit_test(w, x, (int16_t)lp, (int16_t)(lp >> 16));
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == 0xF060) wm_send(w, hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_ACTIVATE:
        if (wp != 0 && !(lp >> 16)) wm_set_focus(w, hwnd);
        return 0;
    case WM_NCACTIVATE:
    case WM_MOUSEACTIVATE:
    case WM_QUERYENDSESSION: return 1;
    case WM_SETCURSOR:
        if ((int16_t)lp == HTCLIENT && x->cls && x->cls->hcursor) {
            w->cursor = x->cls->hcursor;
            return 1;
        }
        return 0;
    default: return 0;
    }
}

/* ---- focus, activation, capture ------------------------------------------------ */

static uint16_t set_focus_to(wm_t *w, uint16_t hwnd) {
    uint16_t old = w->focus;
    if (old == hwnd) return old;
    w->focus = hwnd;
    if (old && wm_is_window(w, old)) wm_send(w, old, WM_KILLFOCUS, hwnd, 0);
    if (hwnd && wm_is_window(w, hwnd) && w->focus == hwnd) wm_send(w, hwnd, WM_SETFOCUS, old, 0);
    return old;
}

static uint16_t activate(wm_t *w, uint16_t top) {
    uint16_t old = w->active;
    if (old == top) return old;
    w->active = top;
    if (old && wm_is_window(w, old)) {
        wm_window *o = wm_get(w, old);
        wm_send(w, old, WM_NCACTIVATE, 0, 0);
        wm_send(w, old, WM_ACTIVATE, 0, (uint32_t)top | (uint32_t)((o->style & WS_MINIMIZE) != 0) << 16);
    }
    if (top && wm_is_window(w, top)) {
        wm_window *n = wm_get(w, top);
        /* no window of the program was active: the application itself is activated (this is the only task) */
        if (!old || !wm_is_window(w, old)) wm_send(w, top, WM_ACTIVATEAPP, 1, 0);
        if (w->active != top || !wm_is_window(w, top)) return old;
        wm_send(w, top, WM_NCACTIVATE, 1, 0);
        wm_send(w, top, WM_ACTIVATE, 1, (uint32_t)old | (uint32_t)((n->style & WS_MINIMIZE) != 0) << 16);
        if (w->active == top && !within(w, top, w->focus)) set_focus_to(w, top);
    } else if (!top && w->focus) {
        set_focus_to(w, 0);
    }
    return old;
}

uint16_t wm_set_active(wm_t *w, uint16_t hwnd) {
    uint16_t top;
    if (hwnd == 0) return w->active;
    top = toplevel_of(w, hwnd);
    if (!top || top == w->desktop) return w->active;
    return activate(w, top);
}

uint16_t wm_set_focus(wm_t *w, uint16_t hwnd) {
    uint16_t old = w->focus, top;
    if (hwnd && !wm_is_window(w, hwnd)) return old;
    if (hwnd) {
        top = toplevel_of(w, hwnd);
        if (top && top != w->desktop && top != w->active) activate(w, top);
    }
    set_focus_to(w, hwnd);
    return old;
}

uint16_t wm_set_capture(wm_t *w, uint16_t hwnd) {
    uint16_t old = w->capture;
    if (wm_is_window(w, hwnd)) w->capture = hwnd;
    return old;
}

void wm_release_capture(wm_t *w) { w->capture = 0; }

/* The topmost visible top-level window, other than skip, or 0. */
static uint16_t next_active(wm_t *w, uint16_t skip) {
    wm_window *d = wm_get(w, w->desktop);
    uint16_t h = d ? d->first_child : 0;
    while (h) {
        wm_window *x = wm_get(w, h);
        if (!x) break;
        if (h != skip && (x->style & WS_VISIBLE) && !x->destroying) return h;
        h = x->next;
    }
    return 0;
}

/* ---- show, enable, move ---------------------------------------------------------- */

static void notify_size_move(wm_t *w, uint16_t hwnd, bool size, bool move, unsigned type) {
    wm_window *x = wm_get(w, hwnd);
    int cx, cy, l, t, r, b;
    if (!x) return;
    cinsets_of(x, &l, &t, &r, &b);
    client_size_of(x, &cx, &cy);
    if (size) wm_send(w, hwnd, WM_SIZE, (uint16_t)type, (uint32_t)(uint16_t)cy << 16 | (uint16_t)cx);
    if (move && (x = wm_get(w, hwnd)))
        wm_send(w, hwnd, WM_MOVE, 0, (uint32_t)(uint16_t)(x->rect.top + t) << 16 | (uint16_t)(x->rect.left + l));
}

static unsigned size_type(const wm_window *x) {
    return x->style & WS_MINIMIZE ? SIZE_MINIMIZED : x->style & WS_MAXIMIZE ? SIZE_MAXIMIZED : SIZE_RESTORED;
}

static void set_rect(wm_t *w, wm_window *x, const fb_rect *n, bool repaint) {
    fb_rect old = x->rect;
    int ocx, ocy, ncx, ncy, ol, ot, nl, nt, r, b;
    uint16_t h = x->hwnd;
    bool visible;
    if (!memcmp(&old, n, sizeof old)) return;
    cinsets_of(x, &ol, &ot, &r, &b);
    client_size_of(x, &ocx, &ocy);
    visible = wm_is_visible(w, h);
    x->rect = *n;
    calc_client(w, x);
    if (!wm_get(w, h)) return;
    cinsets_of(x, &nl, &nt, &r, &b);
    client_size_of(x, &ncx, &ncy);
    if (visible && repaint) {
        expose(w, x->parent, &old);
        paint_all(w, x);
    }
    notify_size_move(w, h, ncx != ocx || ncy != ocy, old.left + ol != n->left + nl || old.top + ot != n->top + nt,
                     size_type(x));
}

void wm_frame_changed(wm_t *w, uint16_t hwnd) {
    wm_window *x = wm_get(w, hwnd);
    if (!x || hwnd == w->desktop) return;
    calc_client(w, x);
    if (!(x = wm_get(w, hwnd))) return;
    if (wm_is_visible(w, hwnd)) paint_all(w, x);
    notify_size_move(w, hwnd, true, false, size_type(x));
}

bool wm_move_window(wm_t *w, uint16_t hwnd, int x, int y, int cx, int cy, bool repaint) {
    wm_window *win = wm_get(w, hwnd);
    fb_rect n;
    if (!win || hwnd == w->desktop) return false;
    n.left = x; n.top = y; n.right = x + cx; n.bottom = y + cy;
    set_rect(w, win, &n, repaint);
    return true;
}

bool wm_show_window(wm_t *w, uint16_t hwnd, int cmd) {
    wm_window *x = wm_get(w, hwnd);
    bool was, activate_it;
    if (!x || hwnd == w->desktop) return false;
    was = (x->style & WS_VISIBLE) != 0;
    if (cmd == SW_HIDE) {
        if (was) {
            wm_send(w, hwnd, WM_SHOWWINDOW, 0, 0);
            if (!(x = wm_get(w, hwnd))) return was;
            if (wm_is_visible(w, hwnd)) {
                fb_rect old = x->rect;
                x->style &= ~WS_VISIBLE;
                expose(w, x->parent, &old);
            } else {
                x->style &= ~WS_VISIBLE;
            }
            if (!(x->style & WS_CHILD) && w->active == hwnd) activate(w, next_active(w, hwnd));
            if (w->focus && within(w, hwnd, w->focus)) set_focus_to(w, w->active);
        }
        return was;
    }
    if (cmd == SW_SHOWMAXIMIZED && !(x->style & WS_MAXIMIZE)) {
        int l, t, r, b;
        fb_rect m;
        if (!(x->style & (WS_MAXIMIZE | WS_MINIMIZE))) x->restore_rect = x->rect;
        x->style = (x->style & ~WS_MINIMIZE) | WS_MAXIMIZE;
        insets_of(x, &l, &t, &r, &b);
        m.left = -(l > 0 ? CXFRAME : 0); m.top = m.left;
        m.right = w->fb->w - m.left; m.bottom = w->fb->h - m.top;
        set_rect(w, x, &m, true);
        if (!was) x->rect = m;
        if (was) goto shown_done;
    } else if ((cmd == SW_SHOWMINIMIZED || cmd == SW_MINIMIZE || cmd == SW_SHOWMINNOACTIVE) && !(x->style & WS_MINIMIZE)) {
        fb_rect m;
        if (!(x->style & (WS_MAXIMIZE | WS_MINIMIZE))) x->restore_rect = x->rect;
        x->style = (x->style & ~WS_MAXIMIZE) | WS_MINIMIZE;
        m.left = 0; m.top = w->fb->h - 32; m.right = 32; m.bottom = w->fb->h;
        set_rect(w, x, &m, true);
        if (was) goto shown_done;
    } else if ((cmd == SW_SHOWNORMAL || cmd == SW_RESTORE || cmd == SW_SHOWNOACTIVATE) &&
               (x->style & (WS_MAXIMIZE | WS_MINIMIZE))) {
        x->style &= ~(WS_MAXIMIZE | WS_MINIMIZE);
        set_rect(w, x, &x->restore_rect, true);
        if (was) goto shown_done;
    }
    if (!was) {
        x->style |= WS_VISIBLE;
        wm_send(w, hwnd, WM_SHOWWINDOW, 1, 0);
        if ((x = wm_get(w, hwnd))) paint_all(w, x);
    }
shown_done:
    activate_it = (cmd == SW_SHOWNORMAL || cmd == SW_SHOW || cmd == SW_SHOWMAXIMIZED || cmd == SW_RESTORE ||
                   cmd == SW_SHOWMINIMIZED) && wm_is_window(w, hwnd);
    if (activate_it && !(wm_get(w, hwnd)->style & WS_CHILD)) wm_set_active(w, hwnd);
    return was;
}

bool wm_enable_window(wm_t *w, uint16_t hwnd, bool enable) {
    wm_window *x = wm_get(w, hwnd);
    bool was_disabled;
    if (!x) return false;
    was_disabled = (x->style & WS_DISABLED) != 0;
    if (enable == !was_disabled) return was_disabled;
    if (enable) x->style &= ~WS_DISABLED;
    else x->style |= WS_DISABLED;
    if (!enable) {
        if (w->focus == hwnd) set_focus_to(w, 0);
        if (w->capture == hwnd) w->capture = 0;
    }
    wm_send(w, hwnd, WM_ENABLE, enable ? 1 : 0, 0);
    return was_disabled;
}

bool wm_is_enabled(wm_t *w, uint16_t hwnd) {
    wm_window *x = wm_get(w, hwnd);
    return x && !(x->style & WS_DISABLED);
}

bool wm_bring_to_top(wm_t *w, uint16_t hwnd) {
    wm_window *x = wm_get(w, hwnd);
    uint16_t parent;
    if (!x || hwnd == w->desktop) return false;
    parent = x->parent;
    unlink_win(w, x);
    link_top(w, x, parent);
    if (wm_is_visible(w, hwnd)) {
        paint_all(w, x);
    }
    return true;
}

bool wm_set_window_pos(wm_t *w, uint16_t hwnd, uint16_t after, int x, int y, int cx, int cy, unsigned flags) {
    wm_window *win = wm_get(w, hwnd);
    fb_rect n;
    if (!win || hwnd == w->desktop) return false;
    if (!(flags & SWP_NOZORDER)) {
        uint16_t parent = win->parent;
        wm_window *a = wm_get(w, after);
        if (after != hwnd) {
            unlink_win(w, win);
            if (after == 1) link_bottom(w, win, parent);
            else if (a && a->parent == parent) link_after(w, win, a);
            else link_top(w, win, parent);
        }
        if (wm_is_visible(w, hwnd)) paint_all(w, win);
    }
    n = win->rect;
    if (!(flags & SWP_NOMOVE)) {
        n.right = x + (n.right - n.left);
        n.bottom = y + (n.bottom - n.top);
        n.left = x;
        n.top = y;
    }
    if (!(flags & SWP_NOSIZE)) {
        n.right = n.left + cx;
        n.bottom = n.top + cy;
    }
    set_rect(w, win, &n, !(flags & SWP_NOREDRAW));
    if (flags & SWP_SHOWWINDOW) wm_show_window(w, hwnd, (flags & SWP_NOACTIVATE) ? SW_SHOWNA : SW_SHOW);
    if (flags & SWP_HIDEWINDOW) wm_show_window(w, hwnd, SW_HIDE);
    return true;
}

uint16_t wm_set_parent(wm_t *w, uint16_t hwnd, uint16_t parent) {
    wm_window *x = wm_get(w, hwnd);
    uint16_t old;
    if (!x || hwnd == w->desktop) return 0;
    if (parent == 0) parent = w->desktop;
    if (!wm_is_window(w, parent) || parent == hwnd || wm_is_child(w, hwnd, parent)) return 0;
    old = x->parent == w->desktop ? 0 : x->parent;
    unlink_win(w, x);
    link_top(w, x, parent);
    if (parent == w->desktop) x->owner = 0;
    return old;
}

/* ---- create / destroy ---------------------------------------------------------------- */

uint16_t wm_create_window(wm_t *w, const wm_create_params *p) {
    wm_class *cls;
    wm_create_params rp = *p;
    wm_window *x;
    uint16_t hwnd, parent, owner = 0;
    uint32_t cs = 0;
    bool child = (p->style & WS_CHILD) != 0, visible;
    uint32_t r;

    cls = p->class_name ? wm_find_class(w, p->class_name, p->hinstance) : wm_class_by_atom(w, p->atom);
    if (!cls) return 0;
    if (child) {
        if (!wm_is_window(w, p->parent)) return 0;
        parent = p->parent;
    } else {
        parent = w->desktop;
        if (wm_is_window(w, p->parent) && p->parent != w->desktop) owner = p->parent;
    }
    if (!(hwnd = alloc_window(w, &x))) return 0;
    x->parent = parent;
    x->owner = owner;
    x->cls = cls;
    x->style = p->style & ~WS_VISIBLE;
    x->exstyle = p->exstyle;
    x->wndproc = cls->wndproc;
    x->hinstance = p->hinstance;
    x->id = p->id_or_menu;
    if (!child && !x->id && cls->menu_name && w->ops.class_menu) x->id = w->ops.class_menu(w->ops.ctx, p->hinstance, cls->menu_name);
    x->nextra = cls->wnd_extra;
    if (x->nextra) x->extra = calloc(1, (size_t)x->nextra);
    x->text = dup_str(p->text ? p->text : "");
    if ((x->nextra && !x->extra) || !x->text) {
        free(x->extra);
        free(x->text);
        w->win[hwnd - HWND_BASE] = NULL;
        free(x);
        return 0;
    }
    visible = (p->style & WS_VISIBLE) != 0;
    if (child) {
        if (rp.x == WM_CW_USEDEFAULT) rp.x = 0;
        if (rp.y == WM_CW_USEDEFAULT) rp.y = 0;
        if (rp.cx == WM_CW_USEDEFAULT) rp.cx = 0;
        if (rp.cy == WM_CW_USEDEFAULT) rp.cy = 0;
    } else {
        if (rp.x == WM_CW_USEDEFAULT || rp.y == WM_CW_USEDEFAULT) {
            int step = (int)(w->cascade++ % 8) * CYCAPTION;
            if (rp.x == WM_CW_USEDEFAULT) rp.x = (int16_t)step;
            if (rp.y == WM_CW_USEDEFAULT) rp.y = (int16_t)step;
        }
        if (rp.cx == WM_CW_USEDEFAULT) rp.cx = (int16_t)(w->fb->w * 3 / 4);
        if (rp.cy == WM_CW_USEDEFAULT) rp.cy = (int16_t)(w->fb->h * 3 / 4);
    }
    x->rect.left = rp.x;
    x->rect.top = rp.y;
    x->rect.right = rp.x + rp.cx;
    x->rect.bottom = rp.y + rp.cy;
    x->restore_rect = x->rect;
    link_top(w, x, parent);

    if (w->ops.createstruct_enter) cs = w->ops.createstruct_enter(w->ops.ctx, &rp);
    r = wm_send(w, hwnd, WM_NCCREATE, 0, cs);
    if (!wm_is_window(w, hwnd)) {
        if (w->ops.createstruct_leave) w->ops.createstruct_leave(w->ops.ctx);
        return 0;
    }
    if ((r & 0xFFFF) == 0) {
        wm_send(w, hwnd, WM_NCDESTROY, 0, 0);
        if (w->ops.createstruct_leave) w->ops.createstruct_leave(w->ops.ctx);
        x = wm_get(w, hwnd);
        if (x) {
            unlink_win(w, x);
            free(x->text);
            free(x->extra);
            w->win[hwnd - HWND_BASE] = NULL;
            free(x);
        }
        return 0;
    }
    if ((x = wm_get(w, hwnd))) calc_client(w, x);
    if (!wm_is_window(w, hwnd)) {
        if (w->ops.createstruct_leave) w->ops.createstruct_leave(w->ops.ctx);
        return 0;
    }
    r = wm_send(w, hwnd, WM_CREATE, 0, cs);
    if (w->ops.createstruct_leave) w->ops.createstruct_leave(w->ops.ctx);
    if (!wm_is_window(w, hwnd)) return 0;
    if (r == 0xFFFFFFFFu) {
        wm_destroy_window(w, hwnd);
        return 0;
    }
    notify_size_move(w, hwnd, true, true, SIZE_RESTORED);
    if (!wm_is_window(w, hwnd)) return 0;
    if (visible) wm_show_window(w, hwnd, child ? SW_SHOW : SW_SHOWNORMAL);
    return wm_is_window(w, hwnd) ? hwnd : 0;
}

bool wm_destroy_window(wm_t *w, uint16_t hwnd) {
    wm_window *x = wm_get(w, hwnd);
    unsigned i;
    if (!x || hwnd == w->desktop || x->destroying) return false;
    x->destroying = true;
    if (w->capture == hwnd) w->capture = 0;
    if (wm_is_visible(w, hwnd)) {
        fb_rect old = x->rect;
        x->style &= ~WS_VISIBLE;
        expose(w, x->parent, &old);
    } else {
        x->style &= ~WS_VISIBLE;
    }
    if (!(x->style & WS_CHILD) && w->active == hwnd) activate(w, next_active(w, hwnd));
    if (within(w, hwnd, w->focus)) set_focus_to(w, w->active != hwnd ? w->active : 0);
    wm_send(w, hwnd, WM_DESTROY, 0, 0);
    /* children (topmost first), then windows this one owns */
    for (;;) {
        wm_window *c = wm_get(w, hwnd);
        uint16_t h;
        if (!c) break;
        h = c->first_child;
        while (h && wm_get(w, h) && wm_get(w, h)->destroying) h = wm_get(w, h)->next;
        if (!h) break;
        wm_destroy_window(w, h);
    }
    for (i = 0; i < WM_MAX_WINDOWS; i++) {
        wm_window *o = w->win[i];
        if (o && o->owner == hwnd && !o->destroying) wm_destroy_window(w, o->hwnd);
    }
    if (wm_is_window(w, hwnd)) wm_send(w, hwnd, WM_NCDESTROY, 0, 0);
    drop_window_state(w, hwnd);
    if (w->ops.destroyed) w->ops.destroyed(w->ops.ctx, hwnd);
    if ((x = wm_get(w, hwnd))) {
        unlink_win(w, x);
        w->win[hwnd - HWND_BASE] = NULL;
        free(x->text);
        free(x->extra);
        free(x);
    }
    for (i = 0; i < WM_MAX_DCS; i++)
        if (w->dcs[i].used && w->dcs[i].hwnd == hwnd) w->dcs[i].used = false;
    return true;
}

/* ---- window and class words ------------------------------------------------------------ */

static bool extra_get(const uint8_t *e, int n, int off, uint32_t *v, bool is_word) {
    int len = is_word ? 2 : 4;
    if (off < 0 || off + len > n || !e) return false;
    *v = e[off] | (uint32_t)e[off + 1] << 8;
    if (!is_word) *v |= (uint32_t)e[off + 2] << 16 | (uint32_t)e[off + 3] << 24;
    return true;
}

static bool extra_set(uint8_t *e, int n, int off, uint32_t v, bool is_word) {
    int len = is_word ? 2 : 4;
    if (off < 0 || off + len > n || !e) return false;
    e[off] = (uint8_t)v;
    e[off + 1] = (uint8_t)(v >> 8);
    if (!is_word) {
        e[off + 2] = (uint8_t)(v >> 16);
        e[off + 3] = (uint8_t)(v >> 24);
    }
    return true;
}

bool wm_get_window_long(wm_t *w, uint16_t hwnd, int idx, uint32_t *v, bool is_word) {
    wm_window *x = wm_get(w, hwnd);
    if (!x) return false;
    if (idx >= 0) return extra_get(x->extra, x->nextra, idx, v, is_word);
    switch (idx) {
    case -4: *v = x->wndproc; return !is_word;
    case -6: *v = x->hinstance; return is_word;
    case -8: *v = x->parent == w->desktop ? 0 : x->parent; return is_word;
    case -12: *v = x->id; return is_word;
    case -16: *v = x->style; return !is_word;
    case -20: *v = x->exstyle; return !is_word;
    default: return false;
    }
}

bool wm_set_window_long(wm_t *w, uint16_t hwnd, int idx, uint32_t v, bool is_word, uint32_t *old) {
    wm_window *x = wm_get(w, hwnd);
    uint32_t o = 0;
    if (!x) return false;
    if (idx >= 0) {
        if (!extra_get(x->extra, x->nextra, idx, &o, is_word)) return false;
        extra_set(x->extra, x->nextra, idx, v, is_word);
    } else {
        switch (idx) {
        case -4: if (is_word) return false; o = x->wndproc; x->wndproc = v; break;
        case -8: if (!is_word) return false; o = x->parent; wm_set_parent(w, hwnd, (uint16_t)v); break;
        case -12: if (!is_word) return false; o = x->id; x->id = (uint16_t)v; break;
        case -16: if (is_word) return false; o = x->style; x->style = v; break;
        case -20: if (is_word) return false; o = x->exstyle; x->exstyle = v; break;
        default: return false;
        }
    }
    if (old) *old = o;
    return true;
}

bool wm_get_class_long(wm_t *w, uint16_t hwnd, int idx, uint32_t *v, bool is_word) {
    wm_window *x = wm_get(w, hwnd);
    wm_class *c = x ? x->cls : NULL;
    if (!c) return false;
    if (idx >= 0) return extra_get(c->extra, c->cls_extra, idx, v, is_word);
    switch (idx) {
    case -8: *v = c->menu_name; return !is_word;
    case -10: *v = c->hbrbackground; return is_word;
    case -12: *v = c->hcursor; return is_word;
    case -14: *v = c->hicon; return is_word;
    case -16: *v = c->hinstance; return is_word;
    case -18: *v = (uint16_t)c->wnd_extra; return is_word;
    case -20: *v = (uint16_t)c->cls_extra; return is_word;
    case -24: *v = c->wndproc; return !is_word;
    case -26: *v = c->style; return is_word;
    default: return false;
    }
}

bool wm_set_class_long(wm_t *w, uint16_t hwnd, int idx, uint32_t v, bool is_word, uint32_t *old) {
    wm_window *x = wm_get(w, hwnd);
    wm_class *c = x ? x->cls : NULL;
    uint32_t o = 0;
    if (!c) return false;
    if (idx >= 0) {
        if (!extra_get(c->extra, c->cls_extra, idx, &o, is_word)) return false;
        extra_set(c->extra, c->cls_extra, idx, v, is_word);
    } else {
        switch (idx) {
        case -8: if (is_word) return false; o = c->menu_name; c->menu_name = v; break;
        case -10: if (!is_word) return false; o = c->hbrbackground; c->hbrbackground = (uint16_t)v; break;
        case -12: if (!is_word) return false; o = c->hcursor; c->hcursor = (uint16_t)v; break;
        case -14: if (!is_word) return false; o = c->hicon; c->hicon = (uint16_t)v; break;
        case -24: if (is_word) return false; o = c->wndproc; c->wndproc = v; break;
        case -26: if (!is_word) return false; o = c->style; c->style = (uint16_t)v; break;
        default: return false;
        }
    }
    if (old) *old = o;
    return true;
}

/* ---- init / destroy ------------------------------------------------------------------------ */

void wm_init(wm_t *w, fb_t *fb, const wm_ops *ops) {
    static const char *const builtins[] = {"BUTTON", "EDIT", "STATIC", "LISTBOX", "COMBOBOX",
                                           "SCROLLBAR", "COMBOLBOX", "MDICLIENT", "#32770"};
    wm_class_def d;
    wm_window *x;
    size_t i;
    memset(w, 0, sizeof *w);
    w->fb = fb;
    if (ops) w->ops = *ops;
    w->cursor_show = 0;
    w->next_timer_id = 0x100;
    memset(&d, 0, sizeof d);
    d.builtin = true;
    for (i = 0; i < sizeof builtins / sizeof builtins[0]; i++) {
        d.name = builtins[i];
        d.hbrbackground = COLOR_BTNFACE + 1;
        d.style = i < 5 ? CS_DBLCLKS : 0;                              /* BUTTON, EDIT, STATIC, LISTBOX, COMBOBOX */
        d.wnd_extra = !strcmp(builtins[i], "#32770") ? 30 : 0;          /* DLGWINDOWEXTRA: DWL_MSGRESULT, DWL_DLGPROC, DWL_USER */
        add_class(w, &d);
    }
    d.style = 0;
    d.wnd_extra = 0;
    d.name = "#32769";
    d.hbrbackground = COLOR_BACKGROUND + 1;
    add_class(w, &d);
    w->desktop = alloc_window(w, &x);
    x->cls = wm_find_class(w, "#32769", 0);
    x->style = WS_POPUP | WS_VISIBLE;
    x->rect.right = fb->w;
    x->rect.bottom = fb->h;
    x->text = dup_str("");
    w->cursor_x = fb->w / 2;
    w->cursor_y = fb->h / 2;
}

void wm_destroy(wm_t *w) {
    unsigned i;
    for (i = 0; i < WM_MAX_WINDOWS; i++) {
        if (w->win[i]) {
            free(w->win[i]->text);
            free(w->win[i]->extra);
            free(w->win[i]);
        }
    }
    for (i = 0; i < WM_MAX_CLASSES; i++) free(w->classes[i].extra);
    memset(w, 0, sizeof *w);
}

/* ==== message queue, timers and input ============================================ */

#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_CAPITAL 0x14
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_F10 0x79
#define MK_LBUTTON 0x01
#define MK_RBUTTON 0x02
#define MK_SHIFT 0x04
#define MK_CONTROL 0x08
#define MK_MBUTTON 0x10
#define DBLCLK_MS 500
#define DBLCLK_PX 4

static bool queue_push(wm_queue *q, const wm_msg *m) {
    if (q->count == WM_QUEUE_SIZE) return false;
    q->q[(q->head + q->count++) % WM_QUEUE_SIZE] = *m;
    return true;
}

static bool match(const wm_msg *m, uint16_t hwnd, uint16_t min, uint16_t max) {
    if (hwnd && m->hwnd != hwnd) return false;
    if (min || max) return m->message >= min && m->message <= max;
    return true;
}

static bool in_range(uint16_t msg, uint16_t min, uint16_t max) {
    return (!min && !max) || (msg >= min && msg <= max);
}

static bool queue_take(wm_queue *q, wm_msg *out, uint16_t hwnd, uint16_t min, uint16_t max, bool remove) {
    unsigned i, j;
    for (i = 0; i < q->count; i++) {
        wm_msg *m = &q->q[(q->head + i) % WM_QUEUE_SIZE];
        if (!match(m, hwnd, min, max)) continue;
        *out = *m;
        if (remove) {
            for (j = i; j + 1 < q->count; j++)
                q->q[(q->head + j) % WM_QUEUE_SIZE] = q->q[(q->head + j + 1) % WM_QUEUE_SIZE];
            q->count--;
        }
        return true;
    }
    return false;
}

static void stamp(wm_t *w, wm_msg *m) {
    m->time = wm_now(w);
    m->pt_x = (int16_t)w->cursor_x;
    m->pt_y = (int16_t)w->cursor_y;
}

/* The retrieved key message's modifiers apply while later input is still queued; once it is drained the live state
 * is the same thing (keys_valid is cleared then). */
static bool snapshot(wm_t *w) { return w->keys_valid; }

static uint8_t live_keys(wm_t *w) {
    uint8_t k = 0;
    if (snapshot(w)) return w->keys_at;
    if (w->key_down[0x10]) k |= 1;
    if (w->key_down[0x11]) k |= 2;
    if (w->key_down[0x12]) k |= 4;
    if (w->key_toggle[0x14]) k |= 8;
    return k;
}

static bool post_to(wm_t *w, wm_queue *q, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    wm_msg m;
    memset(&m, 0, sizeof m);
    m.hwnd = hwnd;
    m.message = msg;
    m.wparam = wp;
    m.lparam = lp;
    if (msg >= 0x100 && msg <= 0x108) m.keys = live_keys(w);
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        m.ch = w->next_ch;
        w->next_ch = 0;
    }
    stamp(w, &m);
    return queue_push(q, &m);
}

bool wm_post(wm_t *w, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    if (hwnd == 0xFFFF) {   /* HWND_BROADCAST: every top-level window */
        wm_window *d = wm_get(w, w->desktop);
        uint16_t h = d ? d->first_child : 0;
        bool ok = true;
        while (h) {
            wm_window *x = wm_get(w, h);
            if (!x) break;
            ok = post_to(w, &w->posted, h, msg, wp, lp) && ok;
            h = x->next;
        }
        return ok;
    }
    if (hwnd && !wm_is_window(w, hwnd)) return false;
    return post_to(w, &w->posted, hwnd, msg, wp, lp);
}

void wm_post_quit(wm_t *w, int code) {
    w->quit_pending = true;
    w->quit_code = code;
}

/* Depth-first, topmost sibling first: the first visible window with something to paint. */
static wm_window *find_paint(wm_t *w, wm_window *c, uint16_t hwnd, bool want_nc) {
    uint16_t h;
    for (h = c->first_child; h;) {
        wm_window *k = wm_get(w, h), *r;
        if (!k) break;
        if ((k->style & WS_VISIBLE) && !k->destroying) {
            if (!hwnd || k->hwnd == hwnd) {
                if (want_nc ? k->nc_dirty : k->has_update) return k;
            }
            if ((r = find_paint(w, k, hwnd, want_nc))) return r;
        }
        h = k->next;
    }
    return NULL;
}

static bool next_timer(wm_t *w, wm_msg *out, uint16_t hwnd, bool remove) {
    unsigned i, best = WM_MAX_TIMERS;
    uint32_t now = wm_now(w);
    for (i = 0; i < WM_MAX_TIMERS; i++) {
        wm_timer *t = &w->timers[i];
        if (!t->used || (hwnd && t->hwnd != hwnd) || (int32_t)(now - t->due) < 0) continue;
        if (best == WM_MAX_TIMERS || (int32_t)(t->due - w->timers[best].due) < 0) best = i;
    }
    if (best == WM_MAX_TIMERS) return false;
    memset(out, 0, sizeof *out);
    out->hwnd = w->timers[best].hwnd;
    out->message = WM_TIMER;
    out->wparam = w->timers[best].id;
    out->lparam = w->timers[best].proc;
    stamp(w, out);
    if (remove) w->timers[best].due = now + w->timers[best].interval;
    return true;
}

bool wm_next_message(wm_t *w, wm_msg *out, uint16_t hwnd, uint16_t min, uint16_t max, bool remove) {
    bool got = false;
    wm_window *d = wm_get(w, w->desktop);
    if (queue_take(&w->posted, out, hwnd, min, max, remove) || queue_take(&w->input, out, hwnd, min, max, remove)) {
        got = true;
        if (remove) {
            w->keys_valid = out->message >= 0x100 && out->message <= 0x108 && w->input.count > 0;
            if (w->keys_valid) w->keys_at = out->keys;
            w->last_ch = out->message == WM_KEYDOWN || out->message == WM_SYSKEYDOWN ? out->ch : 0;
            w->last_hwnd = out->hwnd;
            w->last_vk = out->wparam;
        }
    } else if (w->quit_pending && in_range(WM_QUIT, min, max)) {
        memset(out, 0, sizeof *out);
        out->message = WM_QUIT;
        out->wparam = (uint16_t)w->quit_code;
        stamp(w, out);
        if (remove) w->quit_pending = false;
        got = true;
    } else if (d && in_range(WM_PAINT, min, max)) {
        wm_window *p = find_paint(w, d, hwnd, false);
        if (p) {
            uint16_t ph = p->hwnd;
            if (remove && p->nc_dirty) {
                p->nc_dirty = false;
                wm_send(w, ph, WM_NCPAINT, 1, 0);
                p = wm_get(w, ph);
            }
            if (p && p->has_update) {
                memset(out, 0, sizeof *out);
                out->hwnd = ph;
                out->message = WM_PAINT;
                stamp(w, out);
                got = true;
            }
        } else if (remove) {
            wm_window *n;
            while ((n = find_paint(w, d, hwnd, true))) {   /* frames that need painting on their own */
                uint16_t nh = n->hwnd;
                n->nc_dirty = false;
                wm_send(w, nh, WM_NCPAINT, 1, 0);
            }
        }
    }
    if (!got && in_range(WM_TIMER, min, max)) got = next_timer(w, out, hwnd, remove);
    if (got && remove) w->last_msg = *out;
    return got;
}

bool wm_have_message(wm_t *w) {
    wm_msg m;
    return wm_next_message(w, &m, 0, 0, 0, false);
}

uint32_t wm_dispatch(wm_t *w, const wm_msg *m) {
    if (m->message == WM_TIMER && m->lparam && w->ops.call_wndproc) {
        if (m->hwnd && !wm_is_window(w, m->hwnd)) return 0;
        w->ops.call_wndproc(w->ops.ctx, m->lparam, m->hwnd, WM_TIMER, m->wparam, wm_now(w));
        return 0;
    }
    if (!m->hwnd) return 0;
    return wm_send(w, m->hwnd, m->message, m->wparam, m->lparam);
}

/* ---- timers --------------------------------------------------------------------------- */

uint16_t wm_set_timer(wm_t *w, uint16_t hwnd, uint16_t id, uint32_t interval, uint32_t proc) {
    unsigned i, free_slot = WM_MAX_TIMERS;
    wm_timer *t;
    if (hwnd && !wm_is_window(w, hwnd)) return 0;
    for (i = 0; i < WM_MAX_TIMERS; i++) {
        if (w->timers[i].used && hwnd && w->timers[i].hwnd == hwnd && w->timers[i].id == id) {
            free_slot = i;
            break;
        }
        if (!w->timers[i].used && free_slot == WM_MAX_TIMERS) free_slot = i;
    }
    if (free_slot == WM_MAX_TIMERS) return 0;
    if (!hwnd) {
        bool clash;
        do {   /* a fresh id for thread timers */
            id = w->next_timer_id++;
            if (id == 0) id = w->next_timer_id++;
            clash = false;
            for (i = 0; i < WM_MAX_TIMERS; i++)
                if (w->timers[i].used && !w->timers[i].hwnd && w->timers[i].id == id) clash = true;
        } while (clash);
    }
    t = &w->timers[free_slot];
    t->used = true;
    t->hwnd = hwnd;
    t->id = id;
    t->interval = interval;
    t->due = wm_now(w) + interval;
    t->proc = proc;
    return id;
}

bool wm_kill_timer(wm_t *w, uint16_t hwnd, uint16_t id) {
    unsigned i;
    for (i = 0; i < WM_MAX_TIMERS; i++)
        if (w->timers[i].used && w->timers[i].hwnd == hwnd && w->timers[i].id == id) {
            w->timers[i].used = false;
            return true;
        }
    return false;
}

/* ---- keyboard state and TranslateMessage --------------------------------------------------- */

static int key_bit(unsigned vk) { return vk == 0x10 ? 1 : vk == 0x11 ? 2 : vk == 0x12 ? 4 : 0; }

static bool is_down(wm_t *w, unsigned vk) {
    return snapshot(w) && key_bit(vk) ? (w->keys_at & key_bit(vk)) != 0 : w->key_down[vk] != 0;
}

static bool is_toggled(wm_t *w, unsigned vk) {
    return snapshot(w) && vk == 0x14 ? (w->keys_at & 8) != 0 : w->key_toggle[vk] != 0;
}

int wm_key_state(wm_t *w, unsigned vk) {
    int v;
    if (vk > 255) return 0;
    v = is_toggled(w, vk) ? 1 : 0;
    if (is_down(w, vk)) v |= 0x8000;
    return (int16_t)v;
}

int wm_async_key_state(wm_t *w, unsigned vk) {
    if (vk > 255) return 0;
    return w->key_down[vk] ? (int16_t)0x8000 : 0;
}

static unsigned char_for_vk(wm_t *w, unsigned vk) {
    static const char shifted_digits[] = ")!@#$%^&*(";
    bool shift = is_down(w, VK_SHIFT), ctrl = is_down(w, VK_CONTROL), caps = is_toggled(w, VK_CAPITAL);
    if (vk >= 'A' && vk <= 'Z') {
        if (ctrl) return vk - 'A' + 1;
        return (shift != caps) ? vk : vk - 'A' + 'a';
    }
    if (vk >= '0' && vk <= '9') return shift ? (unsigned)shifted_digits[vk - '0'] : vk;
    if (vk >= 0x60 && vk <= 0x69) return '0' + (vk - 0x60);
    switch (vk) {
    case 0x20: return ' ';
    case 0x0D: return '\r';
    case 0x08: return 8;
    case 0x09: return 9;
    case 0x1B: return 27;
    case 0x6A: return '*';
    case 0x6B: return '+';
    case 0x6D: return '-';
    case 0x6E: return '.';
    case 0x6F: return '/';
    case 0xBA: return shift ? ':' : ';';
    case 0xBB: return shift ? '+' : '=';
    case 0xBC: return shift ? '<' : ',';
    case 0xBD: return shift ? '_' : '-';
    case 0xBE: return shift ? '>' : '.';
    case 0xBF: return shift ? '?' : '/';
    case 0xC0: return shift ? '~' : '`';
    case 0xDB: return shift ? '{' : '[';
    case 0xDC: return shift ? '|' : '\\';
    case 0xDD: return shift ? '}' : ']';
    case 0xDE: return shift ? '"' : '\'';
    default: return 0;
    }
}

bool wm_translate(wm_t *w, const wm_msg *m) {
    unsigned ch;
    if (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN) return false;
    ch = m->ch ? m->ch : (w->last_ch && w->last_hwnd == m->hwnd && w->last_vk == m->wparam) ? w->last_ch : char_for_vk(w, m->wparam);
    if (!ch) return false;
    return post_to(w, &w->posted, m->hwnd, m->message == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR, (uint16_t)ch, m->lparam);
}

/* ---- input ------------------------------------------------------------------------------------ */

static unsigned mouse_flags(wm_t *w) {
    unsigned f = w->buttons;
    if (w->key_down[VK_SHIFT]) f |= MK_SHIFT;
    if (w->key_down[VK_CONTROL]) f |= MK_CONTROL;
    return f;
}

static bool window_enabled_chain(wm_t *w, uint16_t hwnd) {
    wm_window *x = wm_get(w, hwnd);
    for (; x && x->hwnd != w->desktop; x = wm_get(w, x->parent))
        if (x->style & WS_DISABLED) return false;
    return true;
}

static void mouse_event(wm_t *w, const wm_input *ev) {
    uint16_t target, msg_client;
    int hit = HTCLIENT;
    bool down = ev->kind == WM_IN_BUTTON_DOWN, up = ev->kind == WM_IN_BUTTON_UP;
    static const unsigned mk[3] = {MK_LBUTTON, MK_RBUTTON, MK_MBUTTON};
    static const unsigned vkb[3] = {1, 2, 4};
    int btn = ev->button >= 0 && ev->button < 3 ? ev->button : 0;
    uint32_t screen_lp;

    w->cursor_x = ev->x;
    w->cursor_y = ev->y;
    if (down) {
        w->buttons |= mk[btn];
        w->key_down[vkb[btn]] = 1;
    } else if (up) {
        w->buttons &= ~mk[btn];
        w->key_down[vkb[btn]] = 0;
    }
    {
        static const uint16_t dn[3] = {WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN};
        static const uint16_t upm[3] = {WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP};
        msg_client = ev->kind == WM_IN_MOVE ? WM_MOUSEMOVE : down ? dn[btn] : upm[btn];
    }

    screen_lp = (uint32_t)(uint16_t)ev->y << 16 | (uint16_t)ev->x;
    if (w->capture && wm_is_window(w, w->capture)) {
        target = w->capture;
    } else {
        target = wm_window_from_point(w, ev->x, ev->y);
        if (target == w->desktop || !target) return;
        if (!window_enabled_chain(w, target)) return;
        hit = (int16_t)wm_send(w, target, WM_NCHITTEST, 0, screen_lp);
        if (!wm_is_window(w, target)) return;
        if (hit == HTNOWHERE || hit == HTERROR) return;
        if (ev->kind != WM_IN_BUTTON_UP) wm_send(w, target, WM_SETCURSOR, target, (uint32_t)(uint16_t)hit | (uint32_t)msg_client << 16);
        if (!wm_is_window(w, target)) return;
    }
    if (down) {
        uint16_t top = toplevel_of(w, target);
        if (top && top != w->desktop && top != w->active && !w->capture) wm_set_active(w, top);
        if (!wm_is_window(w, target)) return;
        {   /* double click: same button, window, close in time and space, class opts in */
            wm_window *x = wm_get(w, target);
            uint32_t now = wm_now(w);
            if (x->cls && (x->cls->style & CS_DBLCLKS) && w->last_click_time &&
                now - w->last_click_time <= DBLCLK_MS && w->last_click_button == btn &&
                abs(ev->x - w->last_click_x) <= DBLCLK_PX && abs(ev->y - w->last_click_y) <= DBLCLK_PX) {
                msg_client = (uint16_t)(msg_client + 2);       /* WM_xBUTTONDBLCLK follows WM_xBUTTONUP */
                w->last_click_time = 0;
            } else {
                w->last_click_time = now ? now : 1;
                w->last_click_x = ev->x;
                w->last_click_y = ev->y;
                w->last_click_button = btn;
            }
        }
    }
    if (hit == HTCLIENT) {
        int cx = ev->x, cy = ev->y;
        wm_screen_to_client(w, target, &cx, &cy);
        if (ev->kind == WM_IN_MOVE && w->input.count) {
            wm_msg *tail = &w->input.q[(w->input.head + w->input.count - 1) % WM_QUEUE_SIZE];
            if (tail->message == WM_MOUSEMOVE && tail->hwnd == target) {
                tail->wparam = (uint16_t)mouse_flags(w);
                tail->lparam = (uint32_t)(uint16_t)cy << 16 | (uint16_t)cx;
                stamp(w, tail);
                return;
            }
        }
        post_to(w, &w->input, target, msg_client, (uint16_t)mouse_flags(w),
                (uint32_t)(uint16_t)cy << 16 | (uint16_t)cx);
    } else {
        post_to(w, &w->input, target, (uint16_t)(WM_NCMOUSEMOVE + (msg_client - WM_MOUSEMOVE)), (uint16_t)hit, screen_lp);
    }
}

static void key_event(wm_t *w, const wm_input *ev) {
    uint16_t target = w->focus && wm_is_window(w, w->focus) ? w->focus : w->active;
    unsigned vk = ev->vk & 0xFF;
    bool down = ev->kind == WM_IN_KEY_DOWN, alt;
    uint32_t lp = 1 | (uint32_t)(ev->scan & 0xFF) << 16;
    uint16_t msg;
    if (down) {
        if (!w->key_down[vk] && (vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL)) w->key_toggle[vk] ^= 1;
        w->key_down[vk] = 1;
    } else {
        w->key_down[vk] = 0;
    }
    alt = w->key_down[VK_MENU] != 0;
    if (!target || !wm_is_window(w, target)) return;
    if (alt) lp |= 0x20000000u;
    if (!down) lp |= 0xC0000000u;
    else if (ev->repeat) lp |= 0x40000000u;
    msg = (alt || vk == VK_F10 || vk == VK_MENU) ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP) : (down ? WM_KEYDOWN : WM_KEYUP);
    w->next_ch = down ? (uint8_t)ev->ch : 0;
    post_to(w, &w->input, target, msg, (uint16_t)vk, lp);
    w->next_ch = 0;
}

void wm_input_event(wm_t *w, const wm_input *ev) {
    switch (ev->kind) {
    case WM_IN_MOVE:
    case WM_IN_BUTTON_DOWN:
    case WM_IN_BUTTON_UP: mouse_event(w, ev); break;
    case WM_IN_KEY_DOWN:
    case WM_IN_KEY_UP: key_event(w, ev); break;
    case WM_IN_QUIT:
        if (w->active && wm_is_window(w, w->active)) post_to(w, &w->input, w->active, WM_SYSCOMMAND, 0xF060, 0);   /* SC_CLOSE, as the close box or Alt+F4 */
        else wm_post_quit(w, 0);
        break;
    }
}
