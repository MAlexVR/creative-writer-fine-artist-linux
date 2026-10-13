/* USER HLE. See user.h.
 *
 * Ordinals. WRITER.EXE imports 97 USER ordinals; each pairs with the name
 * in the program's own import table, which makes those VERIFIED as
 * a set (tests/test_user.c checks the set against that table). Marked "ARTIST"
 * below: ordinals only ARTIST imports (56, 59, 85, 157, 159, 284, 286, 472,
 * 483), named from the Win 3.1 export list and checked against the arguments
 * seen at their call sites. "UNV" marks ordinals neither binary imports; they
 * come from memory of the export list and are UNVERIFIED. */
#include "user.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kernel_priv.h"
#include "ui.h"
#include "user_priv.h"

/* ---- little helpers --------------------------------------------------------- */

void user_log(user_t *u, const char *fmt, ...) {
    va_list ap;
    if (!u->cfg.log) return;
    va_start(ap, fmt);
    fputs("user: ", u->cfg.log);
    vfprintf(u->cfg.log, fmt, ap);
    fputc('\n', u->cfg.log);
    va_end(ap);
}

static uint32_t host_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static uint32_t raw_now(user_t *u) { return u->cfg.now_ms ? u->cfg.now_ms(u->cfg.now_ctx) : host_ms(); }
uint32_t user_now(user_t *u) { return raw_now(u) - u->t0; }
static uint32_t ops_now(void *ctx) { return user_now(ctx); }

void user_refresh(user_t *u) {
    uint32_t now;
    if (!u->cfg.refresh) return;
    now = user_now(u);
    if (now - u->last_refresh < 16) return;
    u->last_refresh = now;
    u->cfg.refresh(u->cfg.pump_ctx);
}

static void tick(user_t *u) {
    if (u->tick) u->tick(u->tick_ctx);
}

void user_set_tick(user_t *u, void (*fn)(void *ctx), void *ctx) {
    u->tick = fn;
    u->tick_ctx = ctx;
}

wm_t *user_wm(user_t *u) { return &u->wm; }
struct ui *user_ui(user_t *u) { return u->ui; }

void user_set_gfx(user_t *u, const user_gfx *g) {
    if (g) {
        u->gfx = *g;
        u->has_gfx = true;
    } else {
        memset(&u->gfx, 0, sizeof u->gfx);
        u->has_gfx = false;
    }
}

void *user_guest_ptr(user_t *u, uint32_t far, uint32_t len) {
    return (far >> 16) && u->p ? mem_ptr(&u->p->mem, (uint16_t)(far >> 16), far & 0xFFFF, len) : NULL;
}

uint32_t user_scratch_mark(const user_t *u) { return u->scratch_top; }
void user_scratch_release(user_t *u, uint32_t mark) { u->scratch_top = mark; }

uint32_t user_scratch_push(user_t *u, const void *data, uint32_t len) {
    uint32_t off = (u->scratch_top + 3) & ~3u;
    uint8_t *m;
    if (!u->p || off + len > SCRATCH_SIZE || !(m = mem_ptr(&u->p->mem, u->scratch_sel, off, len ? len : 1))) return 0;
    if (data) memcpy(m, data, len);
    else memset(m, 0, len);
    u->scratch_top = off + len;
    return (uint32_t)u->scratch_sel << 16 | off;
}

void user_set_eraser(user_t *u, user_erase_fn fn, void *ctx) {
    u->erase_fn = fn;
    u->erase_ctx = ctx;
}
fb_t *user_fb(user_t *u) { return &u->fb; }
size_t user_stub_count(const user_t *u) { return u->nstubs; }
const user_stub_rec *user_stub_at(const user_t *u, size_t i) { return i < u->nstubs ? &u->stubs[i] : NULL; }

static bool get_rect(hle_call *c, uint16_t sel, uint16_t off, fb_rect *r) {
    const uint8_t *p = hle_ptr(c, sel, off, 8);
    if (!p) return false;
    r->left = (int16_t)ld16(p);
    r->top = (int16_t)ld16(p + 2);
    r->right = (int16_t)ld16(p + 4);
    r->bottom = (int16_t)ld16(p + 6);
    return true;
}

static bool put_rect(hle_call *c, uint16_t sel, uint16_t off, const fb_rect *r) {
    uint8_t *p = hle_ptr(c, sel, off, 8);
    if (!p) return false;
    st16(p, (uint16_t)r->left);
    st16(p + 2, (uint16_t)r->top);
    st16(p + 4, (uint16_t)r->right);
    st16(p + 6, (uint16_t)r->bottom);
    return true;
}

/* ---- calling the guest ------------------------------------------------------- */

static cpu_status trap_hook(cpu_t *c, void *ctx) {
    user_t *u = ctx;
    (void)c;
    u->trap_depth = u->depth;
    return CPU_HALT;
}

/* Runs the far procedure proc with the given Pascal words (leftmost first).
 * On success *ret = DX:AX and every register is as before the call. A fault or
 * a stop requested inside leaves the CPU where it stopped and sets u->abort. */
static bool call_guest(user_t *u, uint32_t proc, const uint16_t *args, unsigned n, uint32_t *ret) {
    cpu_t *c = &u->p->cpu;
    uint32_t r[8], eip, eflags, res;
    uint16_t seg[6];
    unsigned i, mine;
    cpu_status s;
    *ret = 0;
    if (u->abort) return false;
    if (u->depth >= MAX_DEPTH) {
        user_log(u, "callback nesting deeper than %d: stopping", MAX_DEPTH);
        u->abort = CPU_E_GP;
        return false;
    }
    memcpy(r, c->r, sizeof r);
    memcpy(seg, c->seg, sizeof seg);
    eip = c->eip;
    eflags = c->eflags;
    for (i = 0; i < n; i++)
        if ((s = cpu_push16(c, args[i])) != CPU_OK) return u->abort = s, false;
    if ((s = cpu_push16(c, u->trap_sel)) != CPU_OK || (s = cpu_push16(c, 0)) != CPU_OK) return u->abort = s, false;
    c->seg[CPU_CS] = (uint16_t)(proc >> 16);
    c->eip = proc & 0xFFFF;
    c->seg[CPU_DS] = u->p->auto_ds;
    c->r[CPU_EAX] = (c->r[CPU_EAX] & 0xFFFF0000u) | u->p->auto_ds;
    c->eflags &= ~CPU_DF;
    mine = ++u->depth;
    u->trap_depth = 0;
    s = cpu_run(c, 0x7FFFFFFFFFFFFFFFull);
    u->depth--;
    if (s != CPU_HALT || u->trap_depth != mine) {
        u->abort = s == CPU_OK || s == CPU_BUDGET ? CPU_E_UD : s;
        return false;
    }
    res = (c->r[CPU_EDX] & 0xFFFF) << 16 | (c->r[CPU_EAX] & 0xFFFF);
    memcpy(c->r, r, sizeof r);
    memcpy(c->seg, seg, sizeof seg);
    c->eip = eip;
    c->eflags = eflags;
    *ret = res;
    return true;
}


bool user_call_guest(user_t *u, uint32_t proc, const uint16_t *args, unsigned n, uint32_t *ret) {
    return call_guest(u, proc, args, n, ret);
}

cpu_status user_abort(const user_t *u) { return u->abort; }

static uint32_t ops_call(void *ctx, uint32_t proc, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    user_t *u = ctx;
    uint16_t a[5];
    uint32_t r;
    if (proc == 0 || proc == u->builtin_def) return user_defproc(u, hwnd, msg, wp, lp);
    if (u->cfg.log_messages) user_log(u, "msg: hwnd=%04x msg=%04x wp=%04x lp=%08x t=%ums", hwnd, msg, wp, lp, (unsigned)user_now(u));
    a[0] = hwnd;
    a[1] = msg;
    a[2] = wp;
    a[3] = (uint16_t)(lp >> 16);
    a[4] = (uint16_t)lp;
    call_guest(u, proc, a, 5, &r);
    return r;
}

static uint32_t ops_defproc(void *ctx, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    return user_defproc(ctx, hwnd, msg, wp, lp);
}

/* The default procedure as a callable guest address (GetWindowLong(GWL_WNDPROC) of
 * built-in classes, CallWindowProc targets): a hook that runs the Pascal frame. */
static cpu_status builtin_hook(cpu_t *c, void *ctx) {
    user_t *u = ctx;
    uint16_t sp = (uint16_t)c->r[CPU_ESP], hwnd = 0, msg = 0, wp = 0, lo = 0, hi = 0;
    uint32_t r;
    cpu_read16(c, c->seg[CPU_SS], (uint16_t)(sp + 4), &lo);
    cpu_read16(c, c->seg[CPU_SS], (uint16_t)(sp + 6), &hi);
    cpu_read16(c, c->seg[CPU_SS], (uint16_t)(sp + 8), &wp);
    cpu_read16(c, c->seg[CPU_SS], (uint16_t)(sp + 10), &msg);
    cpu_read16(c, c->seg[CPU_SS], (uint16_t)(sp + 12), &hwnd);
    r = user_defproc(u, hwnd, msg, wp, (uint32_t)hi << 16 | lo);
    c->r[CPU_EAX] = (c->r[CPU_EAX] & 0xFFFF0000u) | (r & 0xFFFF);
    c->r[CPU_EDX] = (c->r[CPU_EDX] & 0xFFFF0000u) | (r >> 16);
    return u->abort ? u->abort : cpu_retf(c, 10);
}

static cpu_status finish(user_t *u) { return u->abort; }

static uint16_t ops_class_menu(void *ctx, uint16_t hinst, uint32_t menu_name) {
    user_t *u = ctx;
    return u->ui ? ui_class_menu(u, hinst, menu_name) : 0;
}

static bool ops_nccalcsize(void *ctx, uint16_t hwnd, fb_rect *rc) {
    user_t *u = ctx;
    uint32_t mark = user_scratch_mark(u), far;
    uint8_t r[8];
    bool ok = false;
    st16(r, (uint16_t)rc->left);
    st16(r + 2, (uint16_t)rc->top);
    st16(r + 4, (uint16_t)rc->right);
    st16(r + 6, (uint16_t)rc->bottom);
    far = user_scratch_push(u, r, sizeof r);
    if (!far) {
        user_scratch_release(u, mark);
        return false;
    }
    wm_send(&u->wm, hwnd, WM_NCCALCSIZE, 0, far);
    {
        const uint8_t *m = user_guest_ptr(u, far, sizeof r);   /* the client rectangle the procedure left there */
        if (m && u->ui) {
            rc->left = (int16_t)ld16(m);
            rc->top = (int16_t)ld16(m + 2);
            rc->right = (int16_t)ld16(m + 4);
            rc->bottom = (int16_t)ld16(m + 6);
            ok = true;
        }
    }
    user_scratch_release(u, mark);
    return ok;
}

static void ops_destroyed(void *ctx, uint16_t hwnd) {
    user_t *u = ctx;
    if (u->ui) ui_window_destroyed(u, hwnd);
}

/* ---- CREATESTRUCT for WM_NCCREATE / WM_CREATE ---------------------------------- */

static uint32_t cs_enter(void *ctx, const wm_create_params *p) {
    user_t *u = ctx;
    uint32_t off = u->scratch_top;
    uint8_t *m;
    if (u->cs_depth >= 8 || !(m = mem_ptr(&u->p->mem, u->scratch_sel, off, 34))) return 0;
    u->cs_marks[u->cs_depth++] = off;
    u->scratch_top = off + 34;
    st32(m, p->param);
    st16(m + 4, p->hinstance);
    st16(m + 6, p->id_or_menu);
    st16(m + 8, p->parent);
    st16(m + 10, (uint16_t)p->cy);
    st16(m + 12, (uint16_t)p->cx);
    st16(m + 14, (uint16_t)p->y);
    st16(m + 16, (uint16_t)p->x);
    st32(m + 18, p->style);
    st32(m + 22, p->text_far);
    st32(m + 26, p->class_name_far);
    st32(m + 30, p->exstyle);
    return (uint32_t)u->scratch_sel << 16 | off;
}

static void cs_leave(void *ctx) {
    user_t *u = ctx;
    if (u->cs_depth > 0) u->scratch_top = u->cs_marks[--u->cs_depth];
}

/* ---- default window procedure: messages that carry guest pointers ---------------- */

uint32_t user_defproc(user_t *u, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    const char *t;
    uint32_t ui_ret;
    if (u->ui && ui_defproc(u, hwnd, msg, wp, lp, &ui_ret)) return ui_ret;
    switch (msg) {
    case WM_ERASEBKGND: {
        wm_window *x = wm_get(&u->wm, hwnd);
        if (!u->erase_fn || !x || !x->cls || !x->cls->hbrbackground) return wm_defproc(&u->wm, hwnd, msg, wp, lp);
        return u->erase_fn(u->erase_ctx, wp, x->cls->hbrbackground) ? 1 : 0;
    }
    case WM_SETTEXT: {
        char buf[512];
        uint16_t sel = (uint16_t)(lp >> 16), off = (uint16_t)lp;
        uint32_t avail = sel ? mem_size(&u->p->mem, sel) : 0;
        const uint8_t *p = avail > off ? mem_ptr(&u->p->mem, sel, off, 1) : NULL;
        size_t n;
        if (!p || !wm_is_window(&u->wm, hwnd)) return 0;
        {
            const uint8_t *z = memchr(p, 0, avail - off);
            n = z ? (size_t)(z - p) : avail - off;
        }
        if (n >= sizeof buf) n = sizeof buf - 1;
        memcpy(buf, p, n);
        buf[n] = 0;
        wm_set_text(&u->wm, hwnd, buf);
        if (u->has_gfx && wm_is_visible(&u->wm, hwnd)) wm_send(&u->wm, hwnd, WM_NCPAINT, 1, 0);
        if (u->ui) ui_text_changed(u, hwnd);
        return 1;
    }
    case WM_GETTEXT: {
        size_t n, cap = wp;
        char *d;
        if (!(t = wm_get_text(&u->wm, hwnd)) || cap == 0) return 0;
        n = strlen(t);
        if (n > cap - 1) n = cap - 1;
        d = mem_ptr(&u->p->mem, (uint16_t)(lp >> 16), (uint16_t)lp, (uint32_t)n + 1);
        if (!d) return 0;
        memcpy(d, t, n);
        d[n] = 0;
        return (uint32_t)n;
    }
    case WM_GETTEXTLENGTH:
        return (t = wm_get_text(&u->wm, hwnd)) ? (uint32_t)strlen(t) : 0;
    default:
        return wm_defproc(&u->wm, hwnd, msg, wp, lp);
    }
}

/* ---- handlers --------------------------------------------------------------------- */

#define BEGIN user_t *u = c->ctx; hle_args a; hle_args_init(&a, c); (void)u
#define CHECK_ARGS do { if (a.st) return a.st; } while (0)

static uint32_t sys_metric(int idx) {
    switch (idx) {
    case 0: case 16: return 640;
    case 1: return 480;
    case 17: return 480 - 20;
    case 2: case 3: case 20: case 21: case 9: case 10: return 16;
    case 4: return 20;
    case 5: case 6: return 1;
    case 7: case 8: case 32: case 33: return 4;
    case 11: case 12: case 13: case 14: return 32;
    case 15: return 19;
    case 19: return 1;
    case 28: case 34: return 100;
    case 29: case 35: return 28;
    case 30: case 31: return 18;
    case 36: case 37: return 4;
    case 38: case 39: return 75;
    case 43: return 3;
    default: return 0;
    }
}

static cpu_status u_initapp(hle_call *c) { hle_ret16(c, 1); return CPU_OK; }

static cpu_status u_getsystemmetrics(hle_call *c) {
    BEGIN;
    int idx = (int16_t)hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, (uint16_t)sys_metric(idx));
    return CPU_OK;
}

static cpu_status u_getsyscolor(hle_call *c) {
    BEGIN;
    int idx = (int16_t)hle_u16(&a);
    CHECK_ARGS;
    hle_ret32(c, wm_sys_color(idx));
    return CPU_OK;
}

static cpu_status u_gettickcount(hle_call *c) {
    user_t *u = c->ctx;
    tick(u);
    user_refresh(u);
    hle_ret32(c, user_now(u));
    return u->abort;
}

/* classes */

static cpu_status u_registerclass(hle_call *c) {
    BEGIN;
    uint16_t sel, off, nsel;
    const uint8_t *p;
    char name[64];
    wm_class_def d;
    uint16_t atom;
    hle_far(&a, &sel, &off);
    CHECK_ARGS;
    p = hle_ptr(c, sel, off, 26);
    if (!p) return CPU_E_SEL;
    nsel = ld16(p + 24);
    if (!nsel || !hle_read_cstr(c, nsel, ld16(p + 22), name, sizeof name)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    memset(&d, 0, sizeof d);
    d.name = name;
    d.style = ld16(p);
    d.wndproc = ld32(p + 2);
    d.cls_extra = (int16_t)ld16(p + 6);
    d.wnd_extra = (int16_t)ld16(p + 8);
    d.hinstance = ld16(p + 10);
    d.hicon = ld16(p + 12);
    d.hcursor = ld16(p + 14);
    d.hbrbackground = ld16(p + 16);
    d.menu_name = ld32(p + 18);
    atom = wm_register_class(&u->wm, &d);
    user_log(u, "RegisterClass \"%s\" wndproc=%04x:%04x style=%04x -> %s", name, d.wndproc >> 16, d.wndproc & 0xFFFF,
         d.style, atom ? "ok" : "failed");
    hle_ret16(c, atom);
    return CPU_OK;
}

static cpu_status u_unregisterclass(hle_call *c) {
    BEGIN;
    uint16_t sel, off, inst;
    char name[64];
    hle_far(&a, &sel, &off);
    inst = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, sel && hle_read_cstr(c, sel, off, name, sizeof name) && wm_unregister_class(&u->wm, name, inst));
    return CPU_OK;
}

static cpu_status u_getclassinfo(hle_call *c) {
    BEGIN;
    uint16_t inst, nsel, noff, osel, ooff;
    wm_class *cl;
    char name[64];
    uint8_t *o;
    inst = hle_u16(&a);
    hle_far(&a, &nsel, &noff);
    hle_far(&a, &osel, &ooff);
    CHECK_ARGS;
    if (nsel) cl = hle_read_cstr(c, nsel, noff, name, sizeof name) ? wm_find_class(&u->wm, name, inst) : NULL;
    else cl = wm_class_by_atom(&u->wm, noff);
    o = hle_ptr(c, osel, ooff, 26);
    if (!cl || !o) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    st16(o, cl->style);
    st32(o + 2, cl->wndproc ? cl->wndproc : u->builtin_def);
    st16(o + 6, (uint16_t)cl->cls_extra);
    st16(o + 8, (uint16_t)cl->wnd_extra);
    st16(o + 10, cl->hinstance);
    st16(o + 12, cl->hicon);
    st16(o + 14, cl->hcursor);
    st16(o + 16, cl->hbrbackground);
    st32(o + 18, cl->menu_name);
    st32(o + 22, nsel ? (uint32_t)nsel << 16 | noff : 0);
    hle_ret16(c, 1);
    return CPU_OK;
}

/* windows */

static cpu_status create_window(hle_call *c, bool ex) {
    BEGIN;
    uint32_t exstyle = 0, style, param, cls_far, text_far;
    uint16_t csel, coff, tsel, toff, hwnd;
    int16_t x, y, cx, cy;
    uint16_t parent, menu, inst, psel, poff;
    char cname[64], tname[256];
    wm_create_params p;
    if (ex) exstyle = hle_u32(&a);
    hle_far(&a, &csel, &coff);
    hle_far(&a, &tsel, &toff);
    style = hle_u32(&a);
    x = (int16_t)hle_u16(&a);
    y = (int16_t)hle_u16(&a);
    cx = (int16_t)hle_u16(&a);
    cy = (int16_t)hle_u16(&a);
    parent = hle_u16(&a);
    menu = hle_u16(&a);
    inst = hle_u16(&a);
    hle_far(&a, &psel, &poff);
    CHECK_ARGS;
    param = (uint32_t)psel << 16 | poff;
    cls_far = (uint32_t)csel << 16 | coff;
    text_far = (uint32_t)tsel << 16 | toff;
    memset(&p, 0, sizeof p);
    if (csel) {
        if (!hle_read_cstr(c, csel, coff, cname, sizeof cname)) {
            hle_ret16(c, 0);
            return CPU_OK;
        }
        p.class_name = cname;
    } else {
        p.atom = coff;
        strcpy(cname, "#atom");
    }
    tname[0] = 0;
    if (tsel) hle_read_cstr(c, tsel, toff, tname, sizeof tname);
    p.text = tname;
    p.style = style;
    p.exstyle = exstyle;
    p.x = x; p.y = y; p.cx = cx; p.cy = cy;
    p.parent = parent;
    p.id_or_menu = menu;
    p.hinstance = inst;
    p.param = param;
    p.class_name_far = cls_far;
    p.text_far = text_far;
    hwnd = wm_create_window(&u->wm, &p);
    user_log(u, "CreateWindow%s class=\"%s\" title=\"%s\" style=%08x at %d,%d %dx%d parent=%04x -> %04x", ex ? "Ex" : "",
         cname, tname, style, x, y, cx, cy, parent, hwnd);
    hle_ret16(c, u->abort ? 0 : hwnd);
    return finish(u);
}

static cpu_status u_createwindow(hle_call *c) { return create_window(c, false); }
static cpu_status u_createwindowex(hle_call *c) { return create_window(c, true); }

static cpu_status u_destroywindow(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_destroy_window(&u->wm, h));
    return finish(u);
}

static cpu_status u_showwindow(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a);
    int cmd = (int16_t)hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_show_window(&u->wm, h, cmd));
    return finish(u);
}

static cpu_status u_movewindow(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a);
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a), cx = (int16_t)hle_u16(&a), cy = (int16_t)hle_u16(&a);
    uint16_t repaint = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_move_window(&u->wm, h, x, y, cx, cy, repaint != 0));
    return finish(u);
}

static cpu_status u_setwindowpos(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), after = hle_u16(&a);
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a), cx = (int16_t)hle_u16(&a), cy = (int16_t)hle_u16(&a);
    unsigned flags = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_set_window_pos(&u->wm, h, after, x, y, cx, cy, flags));
    return finish(u);
}

static cpu_status rect_query(hle_call *c, bool client) {
    BEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    fb_rect r;
    hle_far(&a, &sel, &off);
    CHECK_ARGS;
    if (!(client ? wm_client_rect(&u->wm, h, &r) : wm_window_rect(&u->wm, h, &r))) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (!put_rect(c, sel, off, &r)) return CPU_E_SEL;
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status u_getwindowrect(hle_call *c) { return rect_query(c, false); }
static cpu_status u_getclientrect(hle_call *c) { return rect_query(c, true); }

static cpu_status convert_point(hle_call *c, bool to_screen) {
    BEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    uint8_t *p;
    int x, y;
    hle_far(&a, &sel, &off);
    CHECK_ARGS;
    if (!(p = hle_ptr(c, sel, off, 4))) return CPU_E_SEL;
    x = (int16_t)ld16(p);
    y = (int16_t)ld16(p + 2);
    if (to_screen ? wm_client_to_screen(&u->wm, h, &x, &y) : wm_screen_to_client(&u->wm, h, &x, &y)) {
        st16(p, (uint16_t)x);
        st16(p + 2, (uint16_t)y);
    }
    return CPU_OK;
}

static cpu_status u_clienttoscreen(hle_call *c) { return convert_point(c, true); }
static cpu_status u_screentoclient(hle_call *c) { return convert_point(c, false); }

static cpu_status u_adjustwindowrect(hle_call *c) {
    BEGIN;
    uint16_t sel, off, menu;
    uint32_t style;
    fb_rect r;
    int l, t, rr, b;
    hle_far(&a, &sel, &off);
    style = hle_u32(&a);
    menu = hle_u16(&a);
    CHECK_ARGS;
    if (!get_rect(c, sel, off, &r)) return CPU_E_SEL;
    wm_nc_insets(style, 0, menu != 0 && !(style & WS_CHILD), &l, &t, &rr, &b);
    r.left -= l; r.top -= t; r.right += rr; r.bottom += b;
    put_rect(c, sel, off, &r);
    hle_ret16(c, 1);
    return CPU_OK;
}

/* words and longs */

static cpu_status window_word(hle_call *c, bool set, bool is_word, bool klass) {
    BEGIN;
    uint16_t h = hle_u16(&a);
    int idx = (int16_t)hle_u16(&a);
    uint32_t v = 0, old = 0;
    bool ok;
    if (set) v = is_word ? hle_u16(&a) : hle_u32(&a);
    CHECK_ARGS;
    if (set) ok = klass ? wm_set_class_long(&u->wm, h, idx, v, is_word, &old)
                        : wm_set_window_long(&u->wm, h, idx, v, is_word, &old);
    else ok = klass ? wm_get_class_long(&u->wm, h, idx, &old, is_word)
                    : wm_get_window_long(&u->wm, h, idx, &old, is_word);
    if (!ok) old = 0;
    else if (!is_word && ((klass && idx == -24) || (!klass && idx == -4)) && old == 0) old = u->builtin_def;
    if (is_word) hle_ret16(c, (uint16_t)old);
    else hle_ret32(c, old);
    return CPU_OK;
}

static cpu_status u_getwindowword(hle_call *c) { return window_word(c, false, true, false); }
static cpu_status u_setwindowword(hle_call *c) { return window_word(c, true, true, false); }
static cpu_status u_getwindowlong(hle_call *c) { return window_word(c, false, false, false); }
static cpu_status u_setwindowlong(hle_call *c) { return window_word(c, true, false, false); }
static cpu_status u_getclassword(hle_call *c) { return window_word(c, false, true, true); }
static cpu_status u_setclassword(hle_call *c) { return window_word(c, true, true, true); }
static cpu_status u_getclasslong(hle_call *c) { return window_word(c, false, false, true); }
static cpu_status u_setclasslong(hle_call *c) { return window_word(c, true, false, true); }

/* text */

static cpu_status u_setwindowtext(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    hle_far(&a, &sel, &off);
    CHECK_ARGS;
    if (!wm_is_window(&u->wm, h)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    wm_send(&u->wm, h, WM_SETTEXT, 0, (uint32_t)sel << 16 | off);
    hle_ret16(c, 1);
    return finish(u);
}

static cpu_status u_getwindowtext(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    int n;
    uint32_t r = 0;
    hle_far(&a, &sel, &off);
    n = (int16_t)hle_u16(&a);
    CHECK_ARGS;
    if (n > 0 && wm_is_window(&u->wm, h)) r = wm_send(&u->wm, h, WM_GETTEXT, (uint16_t)n, (uint32_t)sel << 16 | off);
    hle_ret16(c, (uint16_t)r);
    return finish(u);
}

static cpu_status u_getwindowtextlength(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_is_window(&u->wm, h) ? (uint16_t)wm_send(&u->wm, h, WM_GETTEXTLENGTH, 0, 0) : 0);
    return finish(u);
}

/* tree and predicates */

static cpu_status u_iswindow(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_is_window(&u->wm, h));
    return CPU_OK;
}

static cpu_status u_iswindowvisible(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_is_visible(&u->wm, h));
    return CPU_OK;
}

static cpu_status style_test(hle_call *c, uint32_t bit) {
    BEGIN;
    uint16_t h = hle_u16(&a);
    wm_window *w;
    CHECK_ARGS;
    w = wm_get(&u->wm, h);
    hle_ret16(c, w && (w->style & bit) != 0);
    return CPU_OK;
}

static cpu_status u_isiconic(hle_call *c) { return style_test(c, WS_MINIMIZE); }
static cpu_status u_iszoomed(hle_call *c) { return style_test(c, WS_MAXIMIZE); }

static cpu_status u_ischild(hle_call *c) {
    BEGIN;
    uint16_t p = hle_u16(&a), h = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_is_child(&u->wm, p, h));
    return CPU_OK;
}

static cpu_status u_getparent(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_get_parent(&u->wm, h));
    return CPU_OK;
}

static cpu_status u_setparent(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), p = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_set_parent(&u->wm, h, p));
    return CPU_OK;
}

static cpu_status u_getwindow(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), cmd = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_get_window(&u->wm, h, cmd));
    return CPU_OK;
}

static cpu_status u_getdesktopwindow(hle_call *c) {
    user_t *u = c->ctx;
    hle_ret16(c, u->wm.desktop);
    return CPU_OK;
}

static cpu_status u_enablewindow(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), en = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_enable_window(&u->wm, h, en != 0));
    return finish(u);
}

static cpu_status u_iswindowenabled(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_is_enabled(&u->wm, h));
    return CPU_OK;
}

static cpu_status u_bringwindowtotop(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a);
    CHECK_ARGS;
    wm_bring_to_top(&u->wm, h);
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status u_flashwindow(hle_call *c) {
    BEGIN;
    hle_u16(&a);
    hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, 0);   /* there is no caption bar to flash */
    return CPU_OK;
}

/* messages that call the guest */

static cpu_status u_defwindowproc(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), msg = hle_u16(&a), wp = hle_u16(&a);
    uint32_t lp = hle_u32(&a);
    CHECK_ARGS;
    hle_ret32(c, user_defproc(u, h, msg, wp, lp));
    return finish(u);
}

static cpu_status u_callwindowproc(hle_call *c) {
    BEGIN;
    uint16_t psel, poff, h, msg, wp;
    uint32_t lp;
    hle_far(&a, &psel, &poff);
    h = hle_u16(&a);
    msg = hle_u16(&a);
    wp = hle_u16(&a);
    lp = hle_u32(&a);
    CHECK_ARGS;
    hle_ret32(c, ops_call(u, (uint32_t)psel << 16 | poff, h, msg, wp, lp));
    return finish(u);
}

static cpu_status u_sendmessage(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), msg = hle_u16(&a), wp = hle_u16(&a);
    uint32_t lp = hle_u32(&a);
    CHECK_ARGS;
    hle_ret32(c, wm_send(&u->wm, h, msg, wp, lp));
    return finish(u);
}

/* message loop */

static void put_msg(hle_call *c, uint16_t sel, uint16_t off, const wm_msg *m) {
    uint8_t *p = hle_ptr(c, sel, off, 18);
    if (!p) return;
    st16(p, m->hwnd);
    st16(p + 2, m->message);
    st16(p + 4, m->wparam);
    st32(p + 6, m->lparam);
    st32(p + 10, m->time);
    st16(p + 14, (uint16_t)m->pt_x);
    st16(p + 16, (uint16_t)m->pt_y);
}

/* Lets the host run (input, presentation, clock); false: the run must end. */
static bool idle(user_t *u, bool wait) {
    return u->cfg.pump && u->cfg.pump(u->cfg.pump_ctx, wait);
}

bool user_idle(user_t *u, bool wait) { return idle(u, wait); }

static cpu_status u_getmessage(hle_call *c) {
    BEGIN;
    uint16_t sel, off, hwnd, min, max;
    wm_msg m;
    hle_far(&a, &sel, &off);
    hwnd = hle_u16(&a);
    min = hle_u16(&a);
    max = hle_u16(&a);
    CHECK_ARGS;
    for (;;) {
        tick(u);
        if (u->abort) return u->abort;
        if (wm_next_message(&u->wm, &m, hwnd, min, max, true)) break;
        if (!idle(u, true)) return loader_request_stop(c->proc);
        if (u->abort) return u->abort;
    }
    if (u->abort) return u->abort;
    put_msg(c, sel, off, &m);
    hle_ret16(c, m.message != WM_QUIT);
    return CPU_OK;
}

static cpu_status u_peekmessage(hle_call *c) {
    BEGIN;
    uint16_t sel, off, hwnd, min, max, flags;
    wm_msg m;
    bool got;
    hle_far(&a, &sel, &off);
    hwnd = hle_u16(&a);
    min = hle_u16(&a);
    max = hle_u16(&a);
    flags = hle_u16(&a);
    CHECK_ARGS;
    tick(u);
    got = !u->abort && wm_next_message(&u->wm, &m, hwnd, min, max, (flags & 1) != 0);
    if (!got && !u->abort) {
        if (!idle(u, false)) return loader_request_stop(c->proc);
        tick(u);
        got = !u->abort && wm_next_message(&u->wm, &m, hwnd, min, max, (flags & 1) != 0);
    }
    if (u->abort) return u->abort;
    if (got) put_msg(c, sel, off, &m);
    hle_ret16(c, got);
    return CPU_OK;
}

static cpu_status u_waitmessage(hle_call *c) {
    user_t *u = c->ctx;
    for (;;) {
        tick(u);
        if (u->abort) return u->abort;
        if (wm_have_message(&u->wm)) break;
        if (!idle(u, true)) return loader_request_stop(c->proc);
    }
    return u->abort;
}

static cpu_status post(hle_call *c, bool app) {
    BEGIN;
    uint16_t h = hle_u16(&a), msg = hle_u16(&a), wp = hle_u16(&a);
    uint32_t lp = hle_u32(&a);
    CHECK_ARGS;
    if (msg == WM_QUIT) {
        wm_post_quit(&u->wm, (int16_t)wp);
        hle_ret16(c, 1);
    } else {
        hle_ret16(c, wm_post(&u->wm, app ? 0 : h, msg, wp, lp));
    }
    return CPU_OK;
}

static cpu_status u_postmessage(hle_call *c) { return post(c, false); }
static cpu_status u_postappmessage(hle_call *c) { return post(c, true); }

static cpu_status u_postquitmessage(hle_call *c) {
    BEGIN;
    int code = (int16_t)hle_u16(&a);
    CHECK_ARGS;
    wm_post_quit(&u->wm, code);
    return CPU_OK;
}

static bool get_msg(hle_call *c, uint16_t sel, uint16_t off, wm_msg *m) {
    const uint8_t *p = hle_ptr(c, sel, off, 18);
    if (!p) return false;
    memset(m, 0, sizeof *m);
    m->hwnd = ld16(p);
    m->message = ld16(p + 2);
    m->wparam = ld16(p + 4);
    m->lparam = ld32(p + 6);
    m->time = ld32(p + 10);
    m->pt_x = (int16_t)ld16(p + 14);
    m->pt_y = (int16_t)ld16(p + 16);
    return true;
}

static cpu_status u_translatemessage(hle_call *c) {
    BEGIN;
    uint16_t sel, off;
    wm_msg m;
    hle_far(&a, &sel, &off);
    CHECK_ARGS;
    if (!get_msg(c, sel, off, &m)) return CPU_E_SEL;
    hle_ret16(c, wm_translate(&u->wm, &m));
    return CPU_OK;
}

static cpu_status u_dispatchmessage(hle_call *c) {
    BEGIN;
    uint16_t sel, off;
    wm_msg m;
    hle_far(&a, &sel, &off);
    CHECK_ARGS;
    if (!get_msg(c, sel, off, &m)) return CPU_E_SEL;
    hle_ret32(c, wm_dispatch(&u->wm, &m));
    return finish(u);
}

static cpu_status u_getmessagetime(hle_call *c) {
    user_t *u = c->ctx;
    hle_ret32(c, u->wm.last_msg.time);
    return CPU_OK;
}

static cpu_status u_getmessagepos(hle_call *c) {
    user_t *u = c->ctx;
    hle_ret32(c, (uint32_t)(uint16_t)u->wm.last_msg.pt_y << 16 | (uint16_t)u->wm.last_msg.pt_x);
    return CPU_OK;
}

static cpu_status u_registerwindowmessage(hle_call *c) {
    BEGIN;
    uint16_t sel, off;
    char name[64];
    unsigned i;
    hle_far(&a, &sel, &off);
    CHECK_ARGS;
    if (!sel || !hle_read_cstr(c, sel, off, name, sizeof name)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    for (i = 0; i < u->nregmsg; i++)
        if (!strcmp(u->regmsg[i], name)) break;
    if (i == u->nregmsg) {
        if (i == sizeof u->regmsg / sizeof u->regmsg[0]) {
            hle_ret16(c, 0);
            return CPU_OK;
        }
        strcpy(u->regmsg[i], name);
        u->nregmsg++;
    }
    hle_ret16(c, (uint16_t)(0xC000 + i));
    return CPU_OK;
}

/* timers */

static cpu_status u_settimer(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), id = hle_u16(&a), ms = hle_u16(&a), sel, off;
    hle_far(&a, &sel, &off);
    CHECK_ARGS;
    hle_ret16(c, wm_set_timer(&u->wm, h, id, ms, (uint32_t)sel << 16 | off));
    return CPU_OK;
}

static cpu_status u_killtimer(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), id = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_kill_timer(&u->wm, h, id));
    return CPU_OK;
}

/* focus, capture, cursor, keyboard */

static cpu_status focus_fn(hle_call *c, int which) {
    BEGIN;
    uint16_t h = 0, r = 0;
    if (which == 22 || which == 59 || which == 18) h = hle_u16(&a);
    CHECK_ARGS;
    switch (which) {
    case 22: r = wm_set_focus(&u->wm, h); break;
    case 23: r = u->wm.focus; break;
    case 59: r = wm_set_active(&u->wm, h); break;
    case 60: r = u->wm.active; break;
    case 18: r = wm_set_capture(&u->wm, h); break;
    case 236: r = u->wm.capture; break;
    }
    hle_ret16(c, r);
    return finish(u);
}

static cpu_status u_setfocus(hle_call *c) { return focus_fn(c, 22); }
static cpu_status u_getfocus(hle_call *c) { return focus_fn(c, 23); }
static cpu_status u_setactivewindow(hle_call *c) { return focus_fn(c, 59); }
static cpu_status u_getactivewindow(hle_call *c) { return focus_fn(c, 60); }
static cpu_status u_setcapture(hle_call *c) { return focus_fn(c, 18); }
static cpu_status u_getcapture(hle_call *c) { return focus_fn(c, 236); }

static cpu_status u_releasecapture(hle_call *c) {
    user_t *u = c->ctx;
    wm_release_capture(&u->wm);
    return CPU_OK;
}

static cpu_status u_getcursorpos(hle_call *c) {
    BEGIN;
    uint16_t sel, off;
    uint8_t *p;
    hle_far(&a, &sel, &off);
    CHECK_ARGS;
    if (!(p = hle_ptr(c, sel, off, 4))) return CPU_E_SEL;
    st16(p, (uint16_t)u->wm.cursor_x);
    st16(p + 2, (uint16_t)u->wm.cursor_y);
    return CPU_OK;
}

static cpu_status u_setcursorpos(hle_call *c) {
    BEGIN;
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a);
    CHECK_ARGS;
    u->wm.cursor_x = x;
    u->wm.cursor_y = y;
    return CPU_OK;
}

static cpu_status u_setcursor(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), old = u->wm.cursor;
    CHECK_ARGS;
    u->wm.cursor = h;
    hle_ret16(c, old);
    return CPU_OK;
}

static cpu_status u_showcursor(hle_call *c) {
    BEGIN;
    uint16_t show = hle_u16(&a);
    CHECK_ARGS;
    u->wm.cursor_show += show ? 1 : -1;
    hle_ret16(c, (uint16_t)u->wm.cursor_show);
    return CPU_OK;
}

static cpu_status u_getcaretblinktime(hle_call *c) { hle_ret16(c, 500); return CPU_OK; }

static cpu_status u_getkeystate(hle_call *c) {
    BEGIN;
    uint16_t vk = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, (uint16_t)wm_key_state(&u->wm, vk));
    return CPU_OK;
}

static cpu_status u_getasynckeystate(hle_call *c) {
    BEGIN;
    uint16_t vk = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, (uint16_t)wm_async_key_state(&u->wm, vk));
    return CPU_OK;
}

/* painting */

static cpu_status u_getdc(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_get_dc(&u->wm, h));
    return CPU_OK;
}

static cpu_status u_releasedc(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), dc = hle_u16(&a);
    CHECK_ARGS;
    hle_ret16(c, wm_release_dc(&u->wm, h, dc));
    return CPU_OK;
}

static cpu_status u_beginpaint(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    wm_paint ps;
    uint8_t *p;
    hle_far(&a, &sel, &off);
    CHECK_ARGS;
    if (!(p = hle_ptr(c, sel, off, 34))) return CPU_E_SEL;
    wm_begin_paint(&u->wm, h, &ps);
    if (u->abort) return u->abort;
    if (!(p = hle_ptr(c, sel, off, 34))) return CPU_E_SEL;
    memset(p, 0, 34);
    st16(p, ps.hdc);
    st16(p + 2, ps.erase);
    st16(p + 4, (uint16_t)ps.rc.left);
    st16(p + 6, (uint16_t)ps.rc.top);
    st16(p + 8, (uint16_t)ps.rc.right);
    st16(p + 10, (uint16_t)ps.rc.bottom);
    hle_ret16(c, ps.hdc);
    return CPU_OK;
}

static cpu_status u_endpaint(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    wm_paint ps;
    const uint8_t *p;
    hle_far(&a, &sel, &off);
    CHECK_ARGS;
    if (!(p = hle_ptr(c, sel, off, 34))) return CPU_E_SEL;
    memset(&ps, 0, sizeof ps);
    ps.hdc = ld16(p);
    wm_end_paint(&u->wm, h, &ps);
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status u_updatewindow(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a);
    CHECK_ARGS;
    wm_update_window(&u->wm, h);
    hle_ret16(c, 1);
    return finish(u);
}

/* An optional RECT pointer: sel 0 is NULL. */
static bool opt_rect(hle_call *c, uint16_t sel, uint16_t off, fb_rect *r, bool *present) {
    *present = sel != 0;
    return !*present || get_rect(c, sel, off, r);
}

static cpu_status u_invalidaterect(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), sel, off, erase;
    fb_rect r;
    bool present;
    hle_far(&a, &sel, &off);
    erase = hle_u16(&a);
    CHECK_ARGS;
    if (!opt_rect(c, sel, off, &r, &present)) return CPU_E_SEL;
    if (h == 0) {
        wm_window *d = wm_get(&u->wm, u->wm.desktop);
        uint16_t k;
        for (k = d ? d->first_child : 0; k;) {
            wm_window *x = wm_get(&u->wm, k);
            wm_invalidate(&u->wm, k, NULL, erase != 0);
            k = x ? x->next : 0;
        }
    } else {
        wm_invalidate(&u->wm, h, present ? &r : NULL, erase != 0);
    }
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status u_validaterect(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    fb_rect r;
    bool present;
    hle_far(&a, &sel, &off);
    CHECK_ARGS;
    if (!opt_rect(c, sel, off, &r, &present)) return CPU_E_SEL;
    wm_validate(&u->wm, h, present ? &r : NULL);
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status u_getupdaterect(hle_call *c) {
    BEGIN;
    uint16_t h = hle_u16(&a), sel, off, erase;
    fb_rect r;
    bool has;
    hle_far(&a, &sel, &off);
    erase = hle_u16(&a);
    CHECK_ARGS;
    has = wm_get_update_rect(&u->wm, h, &r, erase != 0);
    if (sel && !put_rect(c, sel, off, &r)) return CPU_E_SEL;
    hle_ret16(c, has);
    return CPU_OK;
}

/* rectangles */

static cpu_status u_setrect(hle_call *c) {
    BEGIN;
    uint16_t sel, off;
    fb_rect r;
    hle_far(&a, &sel, &off);
    r.left = (int16_t)hle_u16(&a);
    r.top = (int16_t)hle_u16(&a);
    r.right = (int16_t)hle_u16(&a);
    r.bottom = (int16_t)hle_u16(&a);
    CHECK_ARGS;
    if (!put_rect(c, sel, off, &r)) return CPU_E_SEL;
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status u_setrectempty(hle_call *c) {
    BEGIN;
    uint16_t sel, off;
    fb_rect r = {0, 0, 0, 0};
    hle_far(&a, &sel, &off);
    CHECK_ARGS;
    if (!put_rect(c, sel, off, &r)) return CPU_E_SEL;
    return CPU_OK;
}

static cpu_status u_copyrect(hle_call *c) {
    BEGIN;
    uint16_t ds, doff, ss, soff;
    fb_rect r;
    hle_far(&a, &ds, &doff);
    hle_far(&a, &ss, &soff);
    CHECK_ARGS;
    if (!get_rect(c, ss, soff, &r) || !put_rect(c, ds, doff, &r)) return CPU_E_SEL;
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status u_isrectempty(hle_call *c) {
    BEGIN;
    uint16_t sel, off;
    fb_rect r;
    hle_far(&a, &sel, &off);
    CHECK_ARGS;
    if (!get_rect(c, sel, off, &r)) return CPU_E_SEL;
    hle_ret16(c, fb_rect_empty(&r));
    return CPU_OK;
}

static cpu_status u_equalrect(hle_call *c) {
    BEGIN;
    uint16_t s1, o1, s2, o2;
    fb_rect x, y;
    hle_far(&a, &s1, &o1);
    hle_far(&a, &s2, &o2);
    CHECK_ARGS;
    if (!get_rect(c, s1, o1, &x) || !get_rect(c, s2, o2, &y)) return CPU_E_SEL;
    hle_ret16(c, !memcmp(&x, &y, sizeof x));
    return CPU_OK;
}

static cpu_status u_ptinrect(hle_call *c) {
    BEGIN;
    uint16_t sel, off;
    uint32_t pt;
    fb_rect r;
    int x, y;
    hle_far(&a, &sel, &off);
    pt = hle_u32(&a);
    CHECK_ARGS;
    if (!get_rect(c, sel, off, &r)) return CPU_E_SEL;
    x = (int16_t)pt;
    y = (int16_t)(pt >> 16);
    hle_ret16(c, x >= r.left && x < r.right && y >= r.top && y < r.bottom);
    return CPU_OK;
}

static cpu_status shift_rect(hle_call *c, bool inflate) {
    BEGIN;
    uint16_t sel, off;
    fb_rect r;
    int dx, dy;
    hle_far(&a, &sel, &off);
    dx = (int16_t)hle_u16(&a);
    dy = (int16_t)hle_u16(&a);
    CHECK_ARGS;
    if (!get_rect(c, sel, off, &r)) return CPU_E_SEL;
    if (inflate) {
        r.left -= dx; r.right += dx;
        r.top -= dy; r.bottom += dy;
    } else {
        r.left += dx; r.right += dx;
        r.top += dy; r.bottom += dy;
    }
    put_rect(c, sel, off, &r);
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status u_offsetrect(hle_call *c) { return shift_rect(c, false); }
static cpu_status u_inflaterect(hle_call *c) { return shift_rect(c, true); }

static cpu_status combine_rect(hle_call *c, bool unite) {
    BEGIN;
    uint16_t ds, doff, s1, o1, s2, o2;
    fb_rect x, y, r = {0, 0, 0, 0};
    bool ok;
    hle_far(&a, &ds, &doff);
    hle_far(&a, &s1, &o1);
    hle_far(&a, &s2, &o2);
    CHECK_ARGS;
    if (!get_rect(c, s1, o1, &x) || !get_rect(c, s2, o2, &y)) return CPU_E_SEL;
    if (unite) {
        ok = !fb_rect_empty(&x) || !fb_rect_empty(&y);
        if (ok) {
            if (fb_rect_empty(&x)) r = y;
            else if (fb_rect_empty(&y)) r = x;
            else fb_rect_union(&r, &x, &y);
        }
    } else {
        ok = fb_rect_intersect(&r, &x, &y);
        if (!ok) memset(&r, 0, sizeof r);
    }
    if (!put_rect(c, ds, doff, &r)) return CPU_E_SEL;
    hle_ret16(c, ok);
    return CPU_OK;
}

static cpu_status u_intersectrect(hle_call *c) { return combine_rect(c, false); }
static cpu_status u_unionrect(hle_call *c) { return combine_rect(c, true); }

/* wsprintf (cdecl): lpOut, lpFmt, then the values; the caller pops them. */

#define WS_MAX 1023

typedef struct {
    char *out;
    size_t n;
} wsbuf;

static void ws_put(wsbuf *b, char ch) {
    if (b->n < WS_MAX) b->out[b->n++] = ch;
}

static void ws_pad(wsbuf *b, const char *s, size_t len, int width, bool left, char fill) {
    int pad = width > (int)len ? width - (int)len : 0;
    size_t i;
    if (!left) while (pad-- > 0) ws_put(b, fill);
    for (i = 0; i < len; i++) ws_put(b, s[i]);
    if (left) while (pad-- > 0) ws_put(b, ' ');
}

static cpu_status u_wsprintf(hle_call *c) {
    cpu_t *cpu = c->cpu;
    uint16_t sp = (uint16_t)cpu->r[CPU_ESP], at = (uint16_t)(sp + 4);
    uint32_t v;
    uint16_t osel, ooff, fsel, foff, w;
    char fmt[512], out[WS_MAX + 1], num[16];
    wsbuf b = {out, 0};
    const char *f;
    cpu_status st;
    if ((st = cpu_read32(cpu, cpu->seg[CPU_SS], at, &v)) != CPU_OK) return st;
    ooff = (uint16_t)v; osel = (uint16_t)(v >> 16);
    if ((st = cpu_read32(cpu, cpu->seg[CPU_SS], (uint16_t)(at + 4), &v)) != CPU_OK) return st;
    foff = (uint16_t)v; fsel = (uint16_t)(v >> 16);
    at = (uint16_t)(at + 8);
    if (!hle_read_cstr(c, fsel, foff, fmt, sizeof fmt)) return CPU_E_SEL;
#define NEXT16(var) do { if ((st = cpu_read16(cpu, cpu->seg[CPU_SS], at, &(var))) != CPU_OK) return st; at = (uint16_t)(at + 2); } while (0)
    for (f = fmt; *f; f++) {
        bool left = false, zero = false, lng = false;
        int width = 0;
        if (*f != '%') {
            ws_put(&b, *f);
            continue;
        }
        f++;
        for (;; f++) {
            if (*f == '-') left = true;
            else if (*f == '0') zero = true;
            else break;
        }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == 'l') { lng = true; f++; }
        else if (*f == 'h') f++;
        if (!*f) break;
        switch (*f) {
        case 'd': case 'i': case 'u': case 'x': case 'X': {
            int32_t sv;
            uint32_t uv;
            char spec[8];
            if (lng) {
                uint16_t lo, hi;
                NEXT16(lo);
                NEXT16(hi);
                uv = (uint32_t)hi << 16 | lo;
                sv = (int32_t)uv;
            } else {
                NEXT16(w);
                uv = w;
                sv = (int16_t)w;
            }
            snprintf(spec, sizeof spec, "%%%s", *f == 'd' || *f == 'i' ? "d" : *f == 'u' ? "u" : *f == 'x' ? "x" : "X");
            if (*f == 'd' || *f == 'i') snprintf(num, sizeof num, spec, (int)sv);
            else snprintf(num, sizeof num, spec, (unsigned)uv);
            ws_pad(&b, num, strlen(num), width, left, zero && !left ? '0' : ' ');
            break;
        }
        case 'c':
            NEXT16(w);
            num[0] = (char)w;
            ws_pad(&b, num, 1, width, left, ' ');
            break;
        case 's': {
            uint16_t lo, hi;
            char s[WS_MAX + 2];
            NEXT16(lo);
            NEXT16(hi);
            if (!hi || !hle_read_cstr(c, hi, lo, s, sizeof s)) strcpy(s, hi ? "" : "(null)");
            ws_pad(&b, s, strlen(s), width, left, ' ');
            break;
        }
        case '%':
            ws_put(&b, '%');
            break;
        default:
            ws_put(&b, '%');
            ws_put(&b, *f);
        }
    }
#undef NEXT16
    out[b.n] = 0;
    {
        char *d = hle_ptr(c, osel, ooff, (uint32_t)b.n + 1);
        if (!d) return CPU_E_SEL;
        memcpy(d, out, b.n + 1);
    }
    hle_ret16(c, (uint16_t)b.n);
    return CPU_OK;
}

/* character and string helpers (ANSI Latin-1) */

static bool is_upper(unsigned ch) { return kernel_ansi_lower(ch) != ch; }
static bool is_lower(unsigned ch) { return kernel_ansi_upper(ch) != ch; }
static bool is_alpha(unsigned ch) { return is_upper(ch) || is_lower(ch); }

static cpu_status char_class(hle_call *c, int kind) {
    BEGIN;
    unsigned ch = hle_u16(&a) & 0xFF;
    bool r = false;
    CHECK_ARGS;
    switch (kind) {
    case 0: r = is_upper(ch); break;
    case 1: r = is_lower(ch); break;
    case 2: r = is_alpha(ch); break;
    case 3: r = is_alpha(ch) || (ch >= '0' && ch <= '9'); break;
    }
    hle_ret16(c, r);
    return CPU_OK;
}

static cpu_status u_ischarupper(hle_call *c) { return char_class(c, 0); }
static cpu_status u_ischarlower(hle_call *c) { return char_class(c, 1); }
static cpu_status u_ischaralpha(hle_call *c) { return char_class(c, 2); }
static cpu_status u_ischaralphanumeric(hle_call *c) { return char_class(c, 3); }

static cpu_status case_buff(hle_call *c, bool upper) {
    BEGIN;
    uint16_t sel, off, n;
    uint32_t len;
    uint8_t *p;
    uint32_t i;
    hle_far(&a, &sel, &off);
    n = hle_u16(&a);
    CHECK_ARGS;
    len = n ? n : 0x10000;
    if (!(p = hle_ptr(c, sel, off, len))) return CPU_E_SEL;
    for (i = 0; i < len; i++) p[i] = (uint8_t)(upper ? kernel_ansi_upper(p[i]) : kernel_ansi_lower(p[i]));
    hle_ret16(c, n);
    return CPU_OK;
}

static cpu_status u_ansiupperbuff(hle_call *c) { return case_buff(c, true); }
static cpu_status u_ansilowerbuff(hle_call *c) { return case_buff(c, false); }

static cpu_status u_messagebeep(hle_call *c) { hle_ret16(c, 1); return CPU_OK; }

static cpu_status u_getdialogbaseunits(hle_call *c) { hle_ret32(c, 16u << 16 | 8u); return CPU_OK; }
static cpu_status u_getfreesystemresources(hle_call *c) { hle_ret16(c, 90); return CPU_OK; }

/* GetWindowTask(hwnd): the runtime runs one task, which owns every window. */
static cpu_status u_getwindowtask(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a);
    kernel_t *k = ui_kernel(u);
    UCHECK;
    hle_ret16(c, wm_is_window(&u->wm, h) ? (k ? kernel_task(k) : 1) : 0);
    return CPU_OK;
}

#define SPI_KEYBOARD_SPEED_DEFAULT 31                        /* the fastest repeat rate: what Windows 3.1 answers by default */

/* SystemParametersInfo(uAction, uParam, lpvParam, fuWinIni). Only the Windows 3.1 actions programs read are
 * answered: beep (1/2), mouse (3/4), border (5), keyboard speed (10/11), screen saver time-out/active (14-17),
 * grid granularity (18/19). Settings that a program sets are kept for the session; nothing reaches the host. */
static cpu_status u_systemparametersinfo(hle_call *c) {
    UBEGIN;
    uint16_t act = hle_u16(&a), param = hle_u16(&a), sel, off, ok = 1;
    uint8_t *p;
    hle_far(&a, &sel, &off);
    (void)hle_u16(&a);
    UCHECK;
    switch (act) {
    case 1: case 10: case 14: case 16: case 18: {            /* GET*: one word */
        uint16_t v;
        switch (act) {
        case 1: v = u->spi_beep; break;
        case 10: v = SPI_KEYBOARD_SPEED_DEFAULT; break;
        case 16: v = u->spi_saver_active; break;
        default: v = 0; break;                                /* 14: no screen saver time-out; 18: no grid */
        }
        if (!(p = hle_ptr(c, sel, off, 2))) return CPU_E_SEL;
        st16(p, v);
        break;
    }
    case 3:                                                   /* GETMOUSE: two thresholds and the acceleration */
        if (!(p = hle_ptr(c, sel, off, 6))) return CPU_E_SEL;
        st16(p, 0); st16(p + 2, 0); st16(p + 4, 1);
        break;
    case 5:                                                   /* GETBORDER */
        if (!(p = hle_ptr(c, sel, off, 2))) return CPU_E_SEL;
        st16(p, 1);
        break;
    case 2: u->spi_beep = param ? 1 : 0; break;
    case 17: u->spi_saver_active = param ? 1 : 0; break;
    case 4: case 11: case 15: case 19: break;                 /* SET*: accepted, no effect */
    default: ok = 0; break;
    }
    hle_ret16(c, ok);
    return CPU_OK;
}

/* ---- registration -------------------------------------------------------------------- */

#define U USER_MODULE
static const hle_def user_defs[] = {
    {U, 5, "InitApp", 2, u_initapp},
    {U, 10, "SetTimer", 10, u_settimer},
    {U, 12, "KillTimer", 4, u_killtimer},
    {U, 13, "GetTickCount", 0, u_gettickcount},
    {U, 15, "GetCurrentTime", 0, u_gettickcount},              /* UNV */
    {U, 17, "GetCursorPos", 4, u_getcursorpos},
    {U, 18, "SetCapture", 2, u_setcapture},
    {U, 19, "ReleaseCapture", 0, u_releasecapture},
    {U, 22, "SetFocus", 2, u_setfocus},
    {U, 23, "GetFocus", 0, u_getfocus},
    {U, 28, "ClientToScreen", 6, u_clienttoscreen},
    {U, 29, "ScreenToClient", 6, u_screentoclient},
    {U, 31, "IsIconic", 2, u_isiconic},
    {U, 32, "GetWindowRect", 6, u_getwindowrect},
    {U, 33, "GetClientRect", 6, u_getclientrect},              /* UNV */
    {U, 34, "EnableWindow", 4, u_enablewindow},                /* UNV */
    {U, 35, "IsWindowEnabled", 2, u_iswindowenabled},          /* UNV */
    {U, 36, "GetWindowText", 8, u_getwindowtext},
    {U, 37, "SetWindowText", 6, u_setwindowtext},
    {U, 38, "GetWindowTextLength", 2, u_getwindowtextlength},
    {U, 39, "BeginPaint", 6, u_beginpaint},
    {U, 40, "EndPaint", 6, u_endpaint},
    {U, 41, "CreateWindow", 30, u_createwindow},
    {U, 42, "ShowWindow", 4, u_showwindow},
    {U, 45, "BringWindowToTop", 2, u_bringwindowtotop},        /* UNV */
    {U, 46, "GetParent", 2, u_getparent},
    {U, 47, "IsWindow", 2, u_iswindow},
    {U, 48, "IsChild", 4, u_ischild},                          /* UNV */
    {U, 49, "IsWindowVisible", 2, u_iswindowvisible},          /* UNV */
    {U, 53, "DestroyWindow", 2, u_destroywindow},
    {U, 56, "MoveWindow", 12, u_movewindow},                   /* ARTIST */
    {U, 57, "RegisterClass", 4, u_registerclass},
    {U, 59, "SetActiveWindow", 2, u_setactivewindow},                     /* ARTIST */
    {U, 60, "GetActiveWindow", 0, u_getactivewindow},                     /* UNV */
    {U, 66, "GetDC", 2, u_getdc},
    {U, 68, "ReleaseDC", 4, u_releasedc},
    {U, 69, "SetCursor", 2, u_setcursor},
    {U, 70, "SetCursorPos", 4, u_setcursorpos},                        /* UNV */
    {U, 71, "ShowCursor", 2, u_showcursor},
    {U, 72, "SetRect", 12, u_setrect},
    {U, 73, "SetRectEmpty", 4, u_setrectempty},                        /* UNV */
    {U, 74, "CopyRect", 8, u_copyrect},                            /* UNV */
    {U, 75, "IsRectEmpty", 4, u_isrectempty},
    {U, 76, "PtInRect", 8, u_ptinrect},                            /* UNV */
    {U, 77, "OffsetRect", 8, u_offsetrect},
    {U, 78, "InflateRect", 8, u_inflaterect},                         /* UNV */
    {U, 79, "IntersectRect", 12, u_intersectrect},                      /* UNV */
    {U, 80, "UnionRect", 12, u_unionrect},
    {U, 102, "AdjustWindowRect", 10, u_adjustwindowrect},
    {U, 104, "MessageBeep", 2, u_messagebeep},
    {U, 105, "FlashWindow", 4, u_flashwindow},                  /* UNV */
    {U, 106, "GetKeyState", 2, u_getkeystate},
    {U, 107, "DefWindowProc", 10, u_defwindowproc},
    {U, 108, "GetMessage", 10, u_getmessage},
    {U, 109, "PeekMessage", 12, u_peekmessage},
    {U, 110, "PostMessage", 10, u_postmessage},
    {U, 6, "PostQuitMessage", 2, u_postquitmessage},                 /* UNV */
    {U, 111, "SendMessage", 10, u_sendmessage},
    {U, 112, "WaitMessage", 0, u_waitmessage},                 /* UNV */
    {U, 118, "RegisterWindowMessage", 4, u_registerwindowmessage},                 /* UNV */
    {U, 119, "GetMessagePos", 0, u_getmessagepos},                 /* UNV */
    {U, 120, "GetMessageTime", 0, u_getmessagetime},                 /* UNV */
    {U, 113, "TranslateMessage", 4, u_translatemessage},
    {U, 114, "DispatchMessage", 4, u_dispatchmessage},
    {U, 116, "PostAppMessage", 10, u_postappmessage},
    {U, 122, "CallWindowProc", 14, u_callwindowproc},
    {U, 124, "UpdateWindow", 2, u_updatewindow},
    {U, 125, "InvalidateRect", 8, u_invalidaterect},
    {U, 127, "ValidateRect", 6, u_validaterect},
    {U, 129, "GetClassWord", 4, u_getclassword},
    {U, 130, "SetClassWord", 6, u_setclassword},               /* UNV */
    {U, 131, "GetClassLong", 4, u_getclasslong},               /* UNV */
    {U, 132, "SetClassLong", 8, u_setclasslong},               /* UNV */
    {U, 133, "GetWindowWord", 4, u_getwindowword},
    {U, 134, "SetWindowWord", 6, u_setwindowword},
    {U, 135, "GetWindowLong", 4, u_getwindowlong},
    {U, 136, "SetWindowLong", 8, u_setwindowlong},
    {U, 169, "GetCaretBlinkTime", 0, u_getcaretblinktime},
    {U, 179, "GetSystemMetrics", 2, u_getsystemmetrics},
    {U, 180, "GetSysColor", 2, u_getsyscolor},                 /* UNV */
    {U, 190, "GetUpdateRect", 8, u_getupdaterect},
    {U, 232, "SetWindowPos", 14, u_setwindowpos},
    {U, 233, "SetParent", 4, u_setparent},                     /* UNV */
    {U, 236, "GetCapture", 0, u_getcapture},
    {U, 243, "GetDialogBaseUnits", 0, u_getdialogbaseunits},
    {U, 249, "GetAsyncKeyState", 2, u_getasynckeystate},
    {U, 262, "GetWindow", 4, u_getwindow},                     /* UNV */
    {U, 272, "IsZoomed", 2, u_iszoomed},                       /* UNV */
    {U, 284, "GetFreeSystemResources", 2, u_getfreesystemresources},             /* ARTIST */
    {U, 224, "GetWindowTask", 2, u_getwindowtask},             /* KIDSWTCH */
    {U, 286, "GetDesktopWindow", 0, u_getdesktopwindow},       /* ARTIST */
    {U, 403, "UnregisterClass", 6, u_unregisterclass},         /* UNV */
    {U, 404, "GetClassInfo", 10, u_getclassinfo},
    {U, 420, "_wsprintf", 0, u_wsprintf},
    {U, 244, "EqualRect", 8, u_equalrect},                 /* UNV */
    {U, 433, "IsCharAlpha", 2, u_ischaralpha},       /* UNV */
    {U, 434, "IsCharAlphanumeric", 2, u_ischaralphanumeric}, /* UNV */
    {U, 435, "IsCharUpper", 2, u_ischarupper},
    {U, 436, "IsCharLower", 2, u_ischarlower},
    {U, 437, "AnsiUpperBuff", 6, u_ansiupperbuff},
    {U, 438, "AnsiLowerBuff", 6, u_ansilowerbuff},
    {U, 452, "CreateWindowEx", 34, u_createwindowex},          /* UNV */
    {U, 483, "SystemParametersInfo", 10, u_systemparametersinfo}, /* ARTIST: SPI_GET/SETSCREENSAVEACTIVE (0x10, 0x11) */
};
#undef U

/* lstrcmp, AnsiUpper and friends live with KERNEL's string code; they use no state. */
static bool register_string_helpers(hle_t *h) {
    size_t i;
    for (i = 0; i < kernel_user_ndefs; i++) {
        unsigned o = kernel_user_defs[i].ordinal;
        if (o >= 430 && !hle_register(h, &kernel_user_defs[i], NULL)) return false;
    }
    return true;
}

user_t *user_create(const user_config *cfg, hle_t *h) {
    user_t *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    u->hle = h;
    if (cfg) u->cfg = *cfg;
    if (u->cfg.screen_w <= 0) u->cfg.screen_w = 640;
    if (u->cfg.screen_h <= 0) u->cfg.screen_h = 480;
    if (u->cfg.screen_bpp != 32) u->cfg.screen_bpp = 8;
    u->t0 = raw_now(u);
    u->spi_saver_active = u->spi_beep = 1;
    if (!fb_init(&u->fb, u->cfg.screen_w, u->cfg.screen_h, u->cfg.screen_bpp) ||
        !hle_register_table(h, user_defs, sizeof user_defs / sizeof user_defs[0], u) ||
        !register_string_helpers(h)) {
        fb_destroy(&u->fb);
        free(u);
        return NULL;
    }
    {
        wm_ops ops;
        memset(&ops, 0, sizeof ops);
        ops.ctx = u;
        ops.call_wndproc = ops_call;
        ops.defproc = ops_defproc;
        ops.now_ms = ops_now;
        ops.createstruct_enter = cs_enter;
        ops.createstruct_leave = cs_leave;
        ops.class_menu = ops_class_menu;
        ops.destroyed = ops_destroyed;
        ops.nccalcsize = ops_nccalcsize;
        wm_init(&u->wm, &u->fb, &ops);
    }
    if (!ui_create(u)) {
        wm_destroy(&u->wm);
        fb_destroy(&u->fb);
        free(u);
        return NULL;
    }
    return u;
}

void user_destroy(user_t *u) {
    if (!u) return;
    ui_destroy(u->ui);
    wm_destroy(&u->wm);
    fb_destroy(&u->fb);
    free(u);
}

bool user_attach(user_t *u, loader_proc *p) {
    u->p = p;
    u->scratch_sel = mem_alloc(&p->mem, SCRATCH_SIZE, MEM_DATA);
    u->trap_sel = mem_alloc(&p->mem, 32, MEM_CODE);
    if (!u->scratch_sel || !u->trap_sel) return false;
    memset(mem_ptr(&p->mem, u->trap_sel, 0, 32), 0xF4, 32);   /* HLT; the hooks intercept first */
    u->builtin_def = (uint32_t)u->trap_sel << 16 | 8;
    return cpu_add_hook(&p->cpu, u->trap_sel, 0, trap_hook, u) &&
           cpu_add_hook(&p->cpu, u->trap_sel, 8, builtin_hook, u);
}
