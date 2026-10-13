#ifndef GFIXTURE_H
#define GFIXTURE_H
/* Fixture for the GDI tests: the USER fixture plus gdi, with helpers that run
 * API calls as real guest code (pushed Pascal words, far call, HLT) and check
 * that the callee popped exactly its declared argument bytes. */
#include "gdi.h"
#include "ufixture.h"

static gdi_t *gg;

static inline void gboot_cfg(const gdi_config *in) {
    gdi_config cfg;
    memset(&cfg, 0, sizeof cfg);
    if (in) cfg = *in;
    uboot();
    gg = gdi_create(&cfg, &uh, uu);
    CHECK(gg != NULL);
    if (gg) CHECK(gdi_attach(gg, uf.p));
}

static inline void gboot(void) { gboot_cfg(NULL); }

static inline void gdown(void) {
    gdi_destroy(gg);
    gg = NULL;
    udown();
}

/* One API call; returns DX:AX. A call that stops the run (unimplemented) or
 * leaves the stack unbalanced is a failure. */
static inline uint32_t g_call(const char *mod, unsigned ord, const uint16_t *w, unsigned n) {
    loader_stop s = fx_call_words(&uf, mod, ord, w, n);
    CHECK_EQ(s, LOADER_STOP_HLT);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    return s == LOADER_STOP_HLT ? fx_dxax(&uf) : 0xDEADBEEFu;
}

#define GR(ord, ...) g_call("GDI", (ord), (const uint16_t[]){0, ##__VA_ARGS__} + 1, \
                            sizeof((const uint16_t[]){0, ##__VA_ARGS__}) / sizeof(uint16_t) - 1)
#define UR(ord, ...) g_call("USER", (ord), (const uint16_t[]){0, ##__VA_ARGS__} + 1, \
                            sizeof((const uint16_t[]){0, ##__VA_ARGS__}) / sizeof(uint16_t) - 1)
static inline uint16_t g_lo(uint32_t v) { return (uint16_t)v; }
#define GA(...) g_lo(GR(__VA_ARGS__))
#define UA(...) g_lo(UR(__VA_ARGS__))

static inline uint16_t g_screen_dc(void) { return (uint16_t)UR(66, 0); }   /* GetDC(0) */

static inline const fb_t *g_fb(void) { return user_fb(uu); }
static inline uint8_t g_px(int x, int y) { return g_fb()->p8[y * g_fb()->w + x]; }
static inline void g_clear(uint8_t v) { memset(user_fb(uu)->p8, v, (size_t)g_fb()->w * g_fb()->h); }
/* Guest memory scratch: a fresh data segment of n bytes. */
static inline uint16_t g_seg(uint32_t n) { return useg(n); }

#define RGB(r, g, b) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16)

#endif
