#ifndef USER_H
#define USER_H

#include <stdbool.h>
#include <stdio.h>

#include "fb.h"
#include "hle.h"
#include "wm.h"

/* Win16 USER.EXE on the HLE dispatcher: window classes, windows, the
 * message loop, timers, input state, a minimal DC surface and a set of logged
 * stubs for the rest of what WRITER and ARTIST import.
 *
 * Callbacks into the guest (window procedures, TIMERPROCs) run on the CPU
 * through a nested cpu_run: the Pascal frame (HWND, WORD, WORD, LONG) is
 * pushed, the return address is a trap slot, and every register is restored
 * afterwards, so SendMessage may be re-entered from inside a window procedure.
 * DS (and AX, for exported-prolog thunks) is set to the program's auto data
 * segment: MakeProcInstance returns the procedure itself (KERNEL), so USER
 * is the one that supplies the instance data segment. The program is a single
 * task; callbacks into DLLs will need their own DS.
 *
 * Idle handling: when GetMessage/WaitMessage find nothing to return, or
 * PeekMessage finds nothing, the host pump runs (input from the presenter,
 * clock). If the pump returns false the guest run stops (a frame limit, the
 * window closed); without a pump an empty GetMessage stops the run instead of
 * blocking forever. */

typedef struct user user_t;

typedef struct {
    int screen_w, screen_h, screen_bpp; /* default 640x480x8 */
    FILE *log;                          /* classes, windows, MessageBox, stub calls; NULL: silent */
    bool log_messages;                  /* also log every message delivered to a guest window procedure */
    uint32_t (*now_ms)(void *ctx);      /* NULL: host monotonic clock */
    void *now_ctx;
    bool (*pump)(void *ctx, bool wait); /* host events and presentation; false stops the run */
    void *pump_ctx;
    void (*refresh)(void *ctx);         /* display + host events only (no frame counting, no scripted input) */
} user_config;

#define USER_STUB_MAX 256

typedef struct {
    uint16_t ordinal;
    const char *name;     /* may be "" */
    const char *task;     /* who implements it (a short tag) */
    uint16_t caller_cs, caller_ip;
} user_stub_rec;

user_t *user_create(const user_config *cfg, hle_t *h);
void user_destroy(user_t *u);
bool user_attach(user_t *u, loader_proc *p);

/* WM_ERASEBKGND of the default window procedure: GDI paints the class background brush (a brush handle, or a system
 * colour index plus one) into the DC. Without it the default procedure only knows system colours. */
typedef bool (*user_erase_fn)(void *ctx, uint16_t hdc, uint16_t hbrush);
void user_set_eraser(user_t *u, user_erase_fn fn, void *ctx);

/* Drawing services for the built-in UI (frames, menus, controls), supplied by GDI. DC coordinates are pixels; colours are
 * COLORREFs. text draws with the system font (cell 0) or a regular MS Sans Serif of the given cell height; bg 0xFFFFFFFF is transparent. Without a gfx the UI keeps its state but
 * paints nothing. */
typedef struct {
    void *ctx;
    void (*fill)(void *ctx, uint16_t hdc, const fb_rect *r, uint32_t cref);
    void (*invert)(void *ctx, uint16_t hdc, const fb_rect *r);
    void (*text)(void *ctx, uint16_t hdc, int x, int y, const char *s, int n, uint32_t fg, uint32_t bg, int cell);
    int (*text_width)(void *ctx, const char *s, int n, int cell);
    int (*text_height)(void *ctx, int cell);
    void (*font_metrics)(void *ctx, int cell, int *avgw, int *height);   /* dialog base units come from these */
    /* a device bitmap from w*h COLORREFs (top row first); mono keeps 1 bit per pixel */
    uint16_t (*bitmap_create)(void *ctx, int w, int h, bool mono, const uint32_t *cref);
    bool (*brush_color)(void *ctx, uint16_t hbrush, uint32_t *cref);
    bool (*dc_colors)(void *ctx, uint16_t hdc, uint32_t *fg, uint32_t *bg);
    bool (*region_box)(void *ctx, uint16_t hrgn, fb_rect *box);   /* bounding box of a GDI region */
    void (*to_device)(void *ctx, uint16_t hdc, int *x, int *y);   /* logical point of the DC (viewport, mapping mode) to its device point; optional */
} user_gfx;
void user_set_gfx(user_t *u, const user_gfx *g);

/* Called every time the program reads the clock (GetTickCount) and every time it asks for a message, before the
 * queue is looked at: MMSYSTEM delivers due timers and finished sounds there. May run guest callbacks. */
void user_set_tick(user_t *u, void (*fn)(void *ctx), void *ctx);

wm_t *user_wm(user_t *u);
struct ui *user_ui(user_t *u);
/* Runs a far guest procedure (a callback given to GDI: EnumFonts, LineDDA...) with the Pascal
 * words args[0..n-1] (leftmost first); *ret = DX:AX. False when the guest faulted or asked to
 * stop; the status to return from the calling handler is then user_abort(). */
bool user_call_guest(user_t *u, uint32_t proc, const uint16_t *args, unsigned n, uint32_t *ret);
cpu_status user_abort(const user_t *u);
fb_t *user_fb(user_t *u);
uint32_t user_now(user_t *u);
/* Called by clock reads (GetTickCount, timeGetTime): lets the host show the screen while a program busy-waits on the
 * clock without a message loop; at most once per 16 ms of program time. */
void user_refresh(user_t *u);
/* Calls to logged stubs so far (bounded list of the first USER_STUB_MAX). */
size_t user_stub_count(const user_t *u);
const user_stub_rec *user_stub_at(const user_t *u, size_t i);

#endif
