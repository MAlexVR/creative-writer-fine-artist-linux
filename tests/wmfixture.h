#ifndef WMFIXTURE_H
#define WMFIXTURE_H
/* Window-manager fixture: a 640x480 8-bit framebuffer, a manual clock and fake
 * guest window procedures that log every message they receive. */
#include "fixture.h"
#include "wm.h"

#define LOG_MAX 512
typedef struct { uint32_t proc; uint16_t hwnd, msg, wp; uint32_t lp; } wlog_rec;

typedef uint32_t (*wproc_hook)(wm_t *w, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp);

static fb_t wfb;
static wm_t wm;
static uint32_t wclock;
static wlog_rec wlog[LOG_MAX];
static unsigned wnlog;
static wproc_hook whook;       /* optional: runs inside a window procedure, result replaces the default */
static uint32_t wcs_param;     /* lParam handed to WM_NCCREATE / WM_CREATE */
static unsigned wcs_enter, wcs_leave;
static uint32_t wresult[0x600]; /* per-message default result of the fake procedure */
static bool wdef_through;      /* fake procedures forward to wm_defproc instead of returning wresult */

#define PROC_A 0x10010000u
#define PROC_B 0x10020000u

static uint32_t fake_call(void *ctx, uint32_t proc, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    (void)ctx;
    if (wnlog < LOG_MAX) {
        wlog[wnlog].proc = proc;
        wlog[wnlog].hwnd = hwnd;
        wlog[wnlog].msg = msg;
        wlog[wnlog].wp = wp;
        wlog[wnlog].lp = lp;
        wnlog++;
    }
    if (whook) {
        wproc_hook h = whook;
        uint32_t r;
        whook = NULL;               /* one shot, so the hook can send messages itself */
        r = h(&wm, hwnd, msg, wp, lp);
        whook = h;
        return r;
    }
    if (wdef_through) return wm_defproc(&wm, hwnd, msg, wp, lp);
    return msg < 0x600 ? wresult[msg] : 0;
}

static uint32_t fake_now(void *ctx) { (void)ctx; return wclock; }

static uint32_t fake_cs_enter(void *ctx, const wm_create_params *p) {
    (void)ctx; (void)p;
    wcs_enter++;
    return wcs_param;
}
static void fake_cs_leave(void *ctx) { (void)ctx; wcs_leave++; }

static void wboot(void) {
    wm_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.call_wndproc = fake_call;
    ops.now_ms = fake_now;
    ops.createstruct_enter = fake_cs_enter;
    ops.createstruct_leave = fake_cs_leave;
    wclock = 1000;
    wnlog = 0;
    whook = NULL;
    wdef_through = false;
    wcs_param = 0xABCD0000u;
    wcs_enter = wcs_leave = 0;
    memset(wresult, 0, sizeof wresult);
    wresult[WM_NCCREATE] = 1;
    fb_init(&wfb, 640, 480, 8);
    wm_init(&wm, &wfb, &ops);
}

static void wdown(void) {
    wm_destroy(&wm);
    fb_destroy(&wfb);
}

static uint16_t wclass(const char *name, uint32_t proc) {
    wm_class_def d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.wndproc = proc;
    d.hinstance = 0x1111;
    d.hbrbackground = COLOR_WINDOW + 1;
    return wm_register_class(&wm, &d);
}

static wm_create_params wparams(const char *cls, uint32_t style, int x, int y, int cx, int cy, uint16_t parent) {
    wm_create_params p;
    memset(&p, 0, sizeof p);
    p.class_name = cls;
    p.text = "title";
    p.style = style;
    p.x = (int16_t)x; p.y = (int16_t)y; p.cx = (int16_t)cx; p.cy = (int16_t)cy;
    p.parent = parent;
    p.hinstance = 0x1111;
    return p;
}

static uint16_t wmake(const char *cls, uint32_t style, int x, int y, int cx, int cy, uint16_t parent) {
    wm_create_params p = wparams(cls, style, x, y, cx, cy, parent);
    return wm_create_window(&wm, &p);
}

/* Drops every pending message: pending paints are validated first, otherwise
 * WM_PAINT would keep coming back. */
static inline void wdrain(void) {
    wm_msg m;
    unsigned i;
    for (i = 0; i < WM_MAX_WINDOWS; i++)
        if (wm.win[i]) wm_validate(&wm, wm.win[i]->hwnd, NULL);
    while (wm_next_message(&wm, &m, 0, 0, 0, true)) {}
}

/* log helpers */
static inline unsigned wcount(uint16_t msg) {
    unsigned i, n = 0;
    for (i = 0; i < wnlog; i++) n += wlog[i].msg == msg;
    return n;
}
static inline int wfind(uint16_t msg, uint16_t hwnd, unsigned from) {
    unsigned i;
    for (i = from; i < wnlog; i++)
        if (wlog[i].msg == msg && (!hwnd || wlog[i].hwnd == hwnd)) return (int)i;
    return -1;
}
static inline void wclear(void) { wnlog = 0; }

#endif
