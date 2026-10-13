#ifndef USER_PRIV_H
#define USER_PRIV_H
/* Internal to user.c and the ui_*.c files (the resource driven half of USER). */
#include "kernel_priv.h"
#include "user.h"

#define USER_MODULE "USER"
#define SCRATCH_SIZE 0x2000
#define MAX_DEPTH 48

struct ui;

struct user {
    hle_t *hle;
    user_config cfg;
    loader_proc *p;
    fb_t fb;
    wm_t wm;
    uint32_t t0;
    uint32_t last_refresh;
    uint16_t scratch_sel;
    uint32_t scratch_top, cs_marks[8];
    int cs_depth;
    uint16_t trap_sel;
    unsigned depth, trap_depth;
    cpu_status abort;
    uint32_t builtin_def;
    user_stub_rec stubs[USER_STUB_MAX];
    size_t nstubs;
    char regmsg[32][64];
    unsigned nregmsg;
    unsigned next_handle;
    user_erase_fn erase_fn;
    void *erase_ctx;
    user_gfx gfx;
    bool has_gfx;
    struct ui *ui;
    uint16_t spi_saver_active, spi_beep;   /* SystemParametersInfo state (screen saver, beep): both start on */
    void (*tick)(void *ctx);
    void *tick_ctx;
};

static inline uint16_t ld16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t ld32(const uint8_t *p) { return ld16(p) | (uint32_t)ld16(p + 2) << 16; }
static inline void st16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void st32(uint8_t *p, uint32_t v) { st16(p, (uint16_t)v); st16(p + 2, (uint16_t)(v >> 16)); }

kernel_t *ui_kernel(user_t *u);   /* the KERNEL behind GlobalAlloc, NULL without one */
void user_log(user_t *u, const char *fmt, ...);
/* Lets the host run (input, presentation, clock); false: the run must end. */
bool user_idle(user_t *u, bool wait);
/* Default window procedure of USER (DefWindowProc): UI messages first, then the window manager's. */
uint32_t user_defproc(user_t *u, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp);
/* A host buffer in guest scratch memory (a RECT for WM_NCCALCSIZE, ...); released by user_scratch_release. */
uint32_t user_scratch_push(user_t *u, const void *data, uint32_t len);
uint32_t user_scratch_mark(const user_t *u);
void user_scratch_release(user_t *u, uint32_t mark);
void *user_guest_ptr(user_t *u, uint32_t far, uint32_t len);

#define UBEGIN user_t *u = c->ctx; hle_args a; hle_args_init(&a, c); (void)u
#define UCHECK do { if (a.st) return a.st; } while (0)

#endif
