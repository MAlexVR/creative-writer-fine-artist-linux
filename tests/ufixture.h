#ifndef UFIXTURE_H
#define UFIXTURE_H
/* Fixture for the USER tests: hle + user + a guest process, a manual clock, a
 * scriptable host pump and real guest window procedures assembled by hand. */
#include "fixture.h"
#include "user.h"

static hle_t uh;
static user_t *uu;
static fx_t uf;
static uint32_t uclock;
static unsigned upump_calls;
static unsigned urefresh_calls;              /* host refreshes (events + display) while the program busy-waits on the clock */
static bool upump_stop;                      /* make the pump return false */
static void (*upump_script)(bool wait);      /* optional work done by the pump */
static bool u_nopump;                        /* boot without a host pump */

static void u_fake_refresh(void *ctx) { (void)ctx; urefresh_calls++; }
static uint32_t u_fake_now(void *ctx) { (void)ctx; return uclock; }
static bool u_fake_pump(void *ctx, bool wait) {
    (void)ctx;
    upump_calls++;
    if (upump_script) upump_script(wait);
    return !upump_stop && upump_calls < 200000;      /* a modal loop that never ends must not hang the tests */
}

#define U_CODE_A 0x400  /* records its arguments, returns lParam + 1 */
#define U_CODE_B 0x500  /* on WM_USER sends WM_USER+1 to the window in DS:[0x50], returns result + 1 */
#define U_CODE_C 0x600  /* invalid instruction */
#define U_CODE_ENUM 0xA00  /* callback with 8 argument words: counts calls at DS:[60h], stores its dword argument at DS:[62h] */
#define U_CODE_D 0x700  /* forwards to the previous procedure at DS:[0x54] through CallWindowProc */

static inline uint16_t u_cs(void) { return uf.p->seg_sel[0]; }
static inline uint16_t u_ds(void) { return uf.p->auto_ds; }
static inline uint32_t u_far(uint16_t off) { return (uint32_t)u_cs() << 16 | off; }

static inline void u_put(uint16_t off, const uint8_t *b, size_t n) {
    memcpy(mem_ptr(&uf.p->mem, u_cs(), off, (uint32_t)n), b, n);
}

static inline void u_install_code(void) {
    static const uint8_t a[] = {
        0x55, 0x89, 0xE5,                       /* push bp; mov bp,sp */
        0x81, 0x7E, 0x0C, 0x83, 0x00,           /* cmp word [bp+12],83h: WM_NCCALCSIZE goes through DefWindowProc, */
        0x75, 0x14,                             /* jne log                            like the programs' procedures do */
        0xFF, 0x76, 0x0E, 0xFF, 0x76, 0x0C, 0xFF, 0x76, 0x0A, 0xFF, 0x76, 0x08, 0xFF, 0x76, 0x06,
        0x9A, 0, 0, 0, 0,                       /* call far DefWindowProc (patched once the thunk exists) */
        0xA3, 0x32, 0x00,                       /* log: mov [32h],ax: AX at entry */
        0x8C, 0xD8, 0xA3, 0x30, 0x00,           /* mov ax,ds; mov [30h],ax: DS at entry */
        0xBE, 0x77, 0x77, 0xBF, 0x66, 0x66,     /* mov si,7777h; mov di,6666h (clobbered on purpose) */
        0xA1, 0x40, 0x00, 0xB9, 0x0A, 0x00, 0xF7, 0xE1, 0x89, 0xC3, /* bx = [40h] * 10 */
        0x8B, 0x46, 0x06, 0x89, 0x87, 0x00, 0x01,   /* log lParam lo */
        0x8B, 0x46, 0x08, 0x89, 0x87, 0x02, 0x01,   /* lParam hi */
        0x8B, 0x46, 0x0A, 0x89, 0x87, 0x04, 0x01,   /* wParam */
        0x8B, 0x46, 0x0C, 0x89, 0x87, 0x06, 0x01,   /* msg */
        0x8B, 0x46, 0x0E, 0x89, 0x87, 0x08, 0x01,   /* hwnd */
        0xFF, 0x06, 0x40, 0x00,                     /* inc word [40h] */
        0x8B, 0x46, 0x06, 0x8B, 0x56, 0x08,         /* ax = lParam lo, dx = hi */
        0x05, 0x01, 0x00, 0x83, 0xD2, 0x00,         /* add ax,1; adc dx,0 */
        0x5D, 0xCA, 0x0A, 0x00};                    /* pop bp; retf 10 */
    static const uint8_t c[] = {0x0F, 0x0B};
    u_put(U_CODE_A, a, sizeof a);
    static const uint8_t e[] = {
        0x55, 0x89, 0xE5,                       /* push bp; mov bp,sp */
        0xFF, 0x06, 0x60, 0x00,                 /* inc word [60h] */
        0x8B, 0x46, 0x06, 0xA3, 0x62, 0x00,     /* mov ax,[bp+6]; mov [62h],ax */
        0xB8, 0x01, 0x00,                       /* mov ax,1: go on */
        0x5D, 0xCA, 0x10, 0x00};                /* pop bp; retf 16 */
    u_put(U_CODE_C, c, sizeof c);
    u_put(U_CODE_ENUM, e, sizeof e);
}

/* A, B and D need the address of an import thunk. */
static inline void u_install_code_calls(uint16_t sendmessage_off, uint16_t sendmessage_sel,
                                        uint16_t callwindowproc_off, uint16_t callwindowproc_sel,
                                        uint16_t defwindowproc_off, uint16_t defwindowproc_sel) {
    uint8_t b[] = {
        0x55, 0x89, 0xE5,
        0x81, 0x7E, 0x0C, 0x00, 0x04,           /* cmp word [bp+12],400h */
        0x75, 0x15,                             /* jne other */
        0xFF, 0x36, 0x50, 0x00,                 /* push word [50h] */
        0x68, 0x01, 0x04, 0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x00,
        0x9A, 0, 0, 0, 0,                       /* call far SendMessage */
        0x40, 0xEB, 0x05,                       /* inc ax; jmp done */
        0xB8, 0x07, 0x00, 0x31, 0xD2,           /* other: mov ax,7; xor dx,dx */
        0x5D, 0xCA, 0x0A, 0x00};                /* done: pop bp; retf 10 */
    uint8_t d[] = {
        0x55, 0x89, 0xE5,
        0xFF, 0x36, 0x56, 0x00, 0xFF, 0x36, 0x54, 0x00,   /* push word [56h]; push word [54h]: previous procedure (far) */
        0xFF, 0x76, 0x0E, 0xFF, 0x76, 0x0C, 0xFF, 0x76, 0x0A, 0xFF, 0x76, 0x08, 0xFF, 0x76, 0x06,
        0x9A, 0, 0, 0, 0,                       /* call far CallWindowProc */
        0x5D, 0xCA, 0x0A, 0x00};
    b[24] = (uint8_t)sendmessage_off; b[25] = (uint8_t)(sendmessage_off >> 8);
    b[26] = (uint8_t)sendmessage_sel; b[27] = (uint8_t)(sendmessage_sel >> 8);
    d[27] = (uint8_t)callwindowproc_off; d[28] = (uint8_t)(callwindowproc_off >> 8);
    d[29] = (uint8_t)callwindowproc_sel; d[30] = (uint8_t)(callwindowproc_sel >> 8);
    {
        const uint8_t far_def[] = {(uint8_t)defwindowproc_off, (uint8_t)(defwindowproc_off >> 8), (uint8_t)defwindowproc_sel,
                                   (uint8_t)(defwindowproc_sel >> 8)};
        u_put(U_CODE_A + 3 + 5 + 2 + 15 + 1, far_def, sizeof far_def);
    }
    u_put(U_CODE_B, b, sizeof b);
    u_put(U_CODE_D, d, sizeof d);
}

static inline void uboot_cfg(const user_config *in) {
    user_config cfg;
    loader_config lc;
    loader_thunk *t1, *t2, *t3;
    memset(&cfg, 0, sizeof cfg);
    if (in) cfg = *in;
    cfg.now_ms = u_fake_now;
    cfg.pump = u_nopump ? NULL : u_fake_pump;
    cfg.refresh = u_fake_refresh;
    urefresh_calls = 0;
    uclock = 1000;
    upump_calls = 0;
    upump_stop = false;
    upump_script = NULL;
    hle_init(&uh);
    uh.stub_unknown = true;
    uh.record = true;
    uu = user_create(&cfg, &uh);
    CHECK(uu != NULL);
    memset(&lc, 0, sizeof lc);
    lc.resolve = hle_resolve;
    lc.resolve_ctx = &uh;
    fx_init(&uf, 0x800, 0x1000);
    if (fx_load(&uf, &lc) != LOADER_OK) return;
    CHECK(user_attach(uu, uf.p));
    u_install_code();
    t1 = fx_thunk(&uf, "USER", 111);
    t2 = fx_thunk(&uf, "USER", 122);
    t3 = fx_thunk(&uf, "USER", 107);
    u_install_code_calls(t1->off, t1->sel, t2->off, t2->sel, t3->off, t3->sel);
}

static inline void uboot(void) { uboot_cfg(NULL); }

static inline void udown(void) {
    fx_free(&uf);
    user_destroy(uu);
    hle_destroy(&uh);
    uu = NULL;
}

#define UCALL(ord, ...) fx_call_words(&uf, "USER", (ord), (const uint16_t[]){0, ##__VA_ARGS__} + 1, \
                                      sizeof((const uint16_t[]){0, ##__VA_ARGS__}) / sizeof(uint16_t) - 1)

static inline uint16_t useg(uint32_t size) { return mem_alloc(&uf.p->mem, size, MEM_DATA); }
static inline uint8_t *umem(uint16_t sel, uint32_t off, uint32_t len) { return mem_ptr(&uf.p->mem, sel, off, len); }
static inline uint16_t ustr(const char *s) {
    uint16_t sel = useg(0x400);
    strcpy((char *)umem(sel, 0, 1), s);
    return sel;
}
static inline uint16_t urd16(uint16_t sel, uint32_t off) {
    const uint8_t *p = umem(sel, off, 2);
    return p ? (uint16_t)(p[0] | p[1] << 8) : 0xDEAD;
}
static inline uint32_t urd32(uint16_t sel, uint32_t off) { return urd16(sel, off) | (uint32_t)urd16(sel, off + 2) << 16; }
static inline void uwr16(uint16_t sel, uint32_t off, uint16_t v) {
    uint8_t *p = umem(sel, off, 2);
    if (p) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
}
static inline void uwr32(uint16_t sel, uint32_t off, uint32_t v) { uwr16(sel, off, (uint16_t)v); uwr16(sel, off + 2, (uint16_t)(v >> 16)); }

/* The recording procedure's log in the auto data segment. */
static inline unsigned ulog_n(void) { return urd16(u_ds(), 0x40); }
static inline uint16_t ulog_w(unsigned i, unsigned field) { return urd16(u_ds(), 0x100 + 10 * i + 2 * field); }
#define ULOG_LPLO 0
#define ULOG_LPHI 1
#define ULOG_WP 2
#define ULOG_MSG 3
#define ULOG_HWND 4
static inline uint32_t ulog_lp(unsigned i) { return ulog_w(i, ULOG_LPLO) | (uint32_t)ulog_w(i, ULOG_LPHI) << 16; }
static inline int ulog_find(uint16_t msg, uint16_t hwnd, unsigned from) {
    unsigned i, n = ulog_n();
    for (i = from; i < n; i++)
        if (ulog_w(i, ULOG_MSG) == msg && (!hwnd || ulog_w(i, ULOG_HWND) == hwnd)) return (int)i;
    return -1;
}
static inline void ulog_reset(void) { uwr16(u_ds(), 0x40, 0); }

/* Registers a class through the API. wndproc 0 uses the default procedure. */
static inline uint16_t u_register(const char *name, uint32_t wndproc, uint16_t style, uint16_t cls_extra,
                                  uint16_t wnd_extra, uint16_t hinst, uint16_t brush) {
    uint16_t cs = useg(0x100), ns = ustr(name);
    uwr16(cs, 0, style);
    uwr32(cs, 2, wndproc);
    uwr16(cs, 6, cls_extra);
    uwr16(cs, 8, wnd_extra);
    uwr16(cs, 10, hinst);
    uwr16(cs, 12, 0x0301);   /* hIcon */
    uwr16(cs, 14, 0x0302);   /* hCursor */
    uwr16(cs, 16, brush);
    uwr32(cs, 18, 0);        /* lpszMenuName */
    uwr32(cs, 22, (uint32_t)ns << 16);   /* lpszClassName: far pointer ns:0 */
    if (UCALL(57, FARP(cs, 0)) != LOADER_STOP_HLT) return 0;
    return fx_ax(&uf);
}

/* CreateWindow through the API; the window's rectangle is x, y, cx, cy. */
static inline uint16_t u_create(const char *cls, const char *title, uint32_t style, int x, int y, int cx, int cy,
                                uint16_t parent, uint16_t menu, uint16_t hinst) {
    uint16_t cs = ustr(cls), ts = ustr(title);
    if (UCALL(41, FARP(cs, 0), FARP(ts, 0), LONG(style), (uint16_t)x, (uint16_t)y, (uint16_t)cx, (uint16_t)cy,
              parent, menu, hinst, FARP(0, 0)) != LOADER_STOP_HLT)
        return 0;
    return fx_ax(&uf);
}

#endif
